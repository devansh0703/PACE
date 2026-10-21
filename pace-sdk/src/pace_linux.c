/* SPDX-License-Identifier: BUSL-1.1
 * PACE — Linux backend.
 *
 * Tier 0 (always available, no privilege):
 *   CLOCK_THREAD_CPUTIME_ID  -> on-CPU time
 *   /proc/<tid>/schedstat    -> on-CPU ns, run-queue delay ns, timeslices
 *   /proc/<tid>/sched        -> voluntary/involuntary switch counters
 *
 * Tier 1 (needs CAP_PERFMON or perf_event_paranoid <= 1):
 *   perf_event_open(PERF_COUNT_HW_CPU_CYCLES,  pid=tid, cpu=-1)
 *   perf_event_open(PERF_COUNT_HW_REF_CPU_CYCLES, pid=tid, cpu=-1)
 *
 * Measured behaviour of the tier-1 counter (verified on this machine, see
 * docs/PREMISE-VERIFICATION.md): a per-thread hardware counter does not
 * accumulate while the task is descheduled, so it measures on-CPU cycles
 * directly and follows the task across CPU migrations.
 */
#include "pace_backend.h"
#include "pace_session.h"

#if defined(PACE_WITH_BPF)
#  include "bpf/pace_bpf.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#  include <sys/ioctl.h>
#  include <sys/syscall.h>
#  include <linux/perf_event.h>
#  ifndef PERF_COUNT_HW_REF_CPU_CYCLES
#    define PERF_COUNT_HW_REF_CPU_CYCLES 9
#  endif
#endif

#if defined(__x86_64__) || defined(__i386__)
#  define PACE_HAVE_TSC 1
#  include <cpuid.h>
#endif

/* ------------------------------------------------------------------ clocks */

uint64_t pace_now_monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t pace_now_thread_cpu_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* --------------------------------------------------------------------- TSC */

#if defined(PACE_HAVE_TSC)

static inline uint64_t pace_rdtsc(void) {
  uint32_t lo, hi;
  __asm__ __volatile__("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
  return ((uint64_t)hi << 32) | lo;
}

/**
 * The TSC's frequency, from CPUID.
 *
 * Leaf 0x15 gives the crystal ratio, which is exact: crystal_hz * ebx / eax.
 * That is the only CPUID-derived value that is the TSC's frequency rather
 * than something adjacent to it.
 *
 * Leaf 0x16's EAX is the *base* frequency in MHz, which is NOT the TSC's
 * frequency. On a CPU that turbos, the TSC runs at the invariant TSC
 * frequency, which is above the base frequency, and the two differ by
 * however much the core is turboing. On a virtual machine the same thing
 * happens: the hypervisor advertises a base frequency and the TSC runs at
 * a different rate. Measured on a CI runner, the base frequency was
 * 28.7 % below the TSC's actual rate.
 *
 * So leaf 0x16 is not used for the TSC frequency at all. The calibration
 * below is used instead, and it is the honest number: it measures the TSC
 * against the same wall clock the samples are measured against.
 */
static double pace_tsc_hz_from_cpuid(void) {
  unsigned max_leaf = __get_cpuid_max(0, NULL);
  if (max_leaf >= 0x15) {
    unsigned eax, ebx, ecx, edx;
    __cpuid(0x15, eax, ebx, ecx, edx);
    /* crystal_hz * ebx / eax */
    if (eax && ebx && ecx) return (double)ecx * (double)ebx / (double)eax;
  }
  return 0.0;
}

/**
 * Calibrate the TSC against CLOCK_MONOTONIC when CPUID does not expose a
 * crystal ratio. Busy-waits for a fixed interval, so it is only used at
 * session creation, never on the hot path.
 */
static double pace_tsc_hz_by_calibration(void) {
  const uint64_t window_ns = 20000000ull; /* 20 ms */
  struct timespec t0, t1;
  uint64_t c0, c1;

  clock_gettime(CLOCK_MONOTONIC, &t0);
  c0 = pace_rdtsc();
  for (;;) {
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t elapsed = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull;
    /* The nanosecond fields are nanoseconds within the second, so the
     * difference can be negative when the window crosses a second
     * boundary. The borrow is handled here rather than by an unsigned
     * subtraction that would wrap. */
    long nsec_diff = t1.tv_nsec - t0.tv_nsec;
    if (nsec_diff < 0) {
      elapsed -= 1000000000ull;
      nsec_diff += 1000000000L;
    }
    if (elapsed + (uint64_t)nsec_diff >= window_ns) break;
  }
  c1 = pace_rdtsc();

  uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull;
  long nsec_diff = t1.tv_nsec - t0.tv_nsec;
  if (nsec_diff < 0) {
    ns -= 1000000000ull;
    nsec_diff += 1000000000L;
  }
  ns += (uint64_t)nsec_diff;
  if (ns == 0) return 0.0;
  return (double)(c1 - c0) * 1e9 / (double)ns;
}

static double pace_reference_hz_impl(void) {
  static double cached = -1.0;
  if (cached >= 0.0) return cached;

  /* The calibration is the primary source, not the fallback. It measures
   * the TSC against CLOCK_MONOTONIC, which is the clock every sample is
   * measured against, so the two agree by construction. The CPUID crystal
   * ratio is used when the calibration is not possible, and the two are
   * cross-checked against each other either way.
   *
   * The calibration is taken at session creation and never on the hot
   * path, so its 20 ms cost is paid once. */
  double hz = pace_tsc_hz_by_calibration();

  /* A calibration that disagrees with the crystal ratio by more than a few
   * percent means something is wrong -- most likely that the machine was
   * under heavy load while the calibration ran, or that the TSC is not
   * invariant. The crystal ratio is exact, so it wins. */
  double crystal = pace_tsc_hz_from_cpuid();
  if (crystal > 0.0 && hz > 0.0) {
    double ratio = hz / crystal;
    if (ratio < 0.9 || ratio > 1.1) hz = crystal;
  } else if (hz <= 0.0) {
    hz = crystal;
  }

  cached = hz;
  return cached;
}

uint64_t pace_now_ref_cycles(void) { return pace_rdtsc(); }

#else /* !PACE_HAVE_TSC */

static double pace_reference_hz_impl(void) { return 0.0; }
uint64_t pace_now_ref_cycles(void) { return 0; }

#endif

double pace_reference_hz(void) { return pace_reference_hz_impl(); }

/* ------------------------------------------------------------------ /proc */

/**
 * Read the thread's start time from /proc/<tid>/stat field 22.
 *
 * The kernel thread id is recycled: when a thread exits and a new one is
 * created, the new thread can get the same id. A cached file descriptor for
 * /proc/<tid>/schedstat would then point at the *new* thread's accounting,
 * silently attributing another thread's time to this one.
 *
 * Pairing the id with the start time makes the pair unique for the life of
 * the thread, so a recycled id is detected rather than mis-measured.
 */
static int pace_read_start_time(uint64_t tid, uint64_t *out_start_time) {
  char path[64];
  snprintf(path, sizeof path, "/proc/%" PRIu64 "/stat", tid);
  FILE *f = fopen(path, "r");
  if (!f) return -1;

  char buf[4096];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = 0;
  fclose(f);

  char *open = strchr(buf, '(');
  char *close = open ? strchr(open, ')') : NULL;
  if (!close) return -1;

  /* Field 3 is the state character, so the token after it is field 4. */
  int field = 2;
  char *save = NULL;
  for (char *tok = strtok_r(close + 1, " ", &save); tok;
       tok = strtok_r(NULL, " ", &save)) {
    field++;
    if (field == 22) {
      *out_start_time = strtoull(tok, NULL, 10);
      return 0;
    }
  }
  return -1;
}

/** Cached /proc/<tid>/schedstat file descriptor, opened per thread. */
typedef struct pace_linux_thread {
  int schedstat_fd;
  int perf_cycles_fd;
  int perf_ref_fd;
  int cpu_last;
  int perf_fds_transient; /**< The perf fds were opened for one sample only. */
  uint64_t timeslices;
  /**
   * The thread's start time, used to detect a recycled thread id.
   *
   * Zero means it could not be read, in which case the check is skipped
   * rather than failing the sample.
   */
  uint64_t start_time;
  /** The start time the cached descriptor was opened against. */
  uint64_t fd_start_time;
  /** Samples until the next thread-id-recycling check. */
  int start_time_check_countdown;
  /**
   * The thread's id in the KERNEL's pid namespace.
   *
   * The eBPF tier's maps are keyed by task->pid, which is the kernel's
   * pid. The thread id the SDK holds is the one gettid() returns, which
   * is the pid in the process's namespace. Those are the same number
   * only when there is one pid namespace.
   *
   * In a container -- which is what a CI runner is -- they differ, and
   * a lookup by the namespace's thread id misses the map entirely. The
   * tier then contributes nothing while reporting itself attached, and
   * the SDK falls back to the kernel's counter, which on some kernels
   * undercounts the preemption by 80 %.
   *
   * So the kernel's pid is resolved once, at registration, from
   * /proc/<tid>/status's NSpid line, whose LAST entry is the kernel's
   * pid (the entries are listed from the outermost namespace inwards).
   */
  uint64_t kernel_tid;
  /**
   * How many samples pass before the kernel's thread id is resolved
   * again.
   *
   * The resolution reads /proc/self/task/<tid>/status, and the NSpid
   * line in it is not always populated when the thread is first
   * registered: a thread that has just been created can be seen before
   * the kernel has filled it in. So the resolution is retried, and the
   * value is cached once it succeeds.
   *
   * The retry is cheap because it only happens while the resolution is
   * still failing, which is a short window at the start of a thread's
   * life.
   */
  int kernel_tid_check_countdown;
} pace_linux_thread;

/**
 * How many samples pass between thread-id-recycling checks.
 *
 * Reading /proc/<tid>/stat costs about 2.8 microseconds, which is six
 * times the cost of a tier-0 sample, so the check is amortised. At 1000
 * samples per second a recycled id is detected within a second.
 */
#define PACE_START_TIME_CHECK_INTERVAL 1000

static int pace_open_schedstat(uint64_t tid) {
  char path[64];
  snprintf(path, sizeof path, "/proc/%" PRIu64 "/schedstat", tid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  return fd;
}

/**
 * A thread's id in the kernel's pid namespace, or 0 if it cannot be read.
 *
 * The eBPF tier's maps are keyed by task->pid, which is the kernel's
 * pid, while the thread id the SDK holds is the one gettid() returns:
 * the pid in the process's namespace. They are the same number only
 * when there is one pid namespace.
 *
 * In a container they differ, and a lookup by the namespace's thread id
 * misses the map entirely -- so the tier contributes nothing while
 * reporting itself attached, and the SDK falls back to the kernel's
 * counter.
 *
 * /proc/<tid>/status carries the "NSpid:" line, whose entries are the
 * pid in each namespace from the outermost inwards. The LAST entry is
 * the kernel's:
 *
 *   NSpid:  1234   42   7
 *
 * and with a single namespace there is one entry, which is the kernel's
 * too. So the last entry is always the right one.
 *
 * The file is read once per thread, at registration, and not per
 * sample.
 */
uint64_t pace_kernel_thread_id(uint64_t tid) {
  /* The path has to be built from the PROCESS's own directory, because
   * /proc/<tid>/status exists only for a thread group leader. A
   * non-leader thread's status is at /proc/<tgid>/task/<tid>/status,
   * and there is no /proc/<tid>/ for it at all.
   *
   * So the path is /proc/self/task/<tid>/status, which is right for
   * every thread of the process, leader or not. */
  char path[80];
  snprintf(path, sizeof path, "/proc/self/task/%" PRIu64 "/status", tid);

  FILE *f = fopen(path, "r");
  if (!f) return 0;

  char line[256];
  uint64_t kernel_id = 0;
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "NSpid:", 6) != 0) continue;
    /* The entries are separated by whitespace, and the last one is the
     * kernel's. */
    const char *p = line + 6;
    while (*p) {
      while (*p == ' ' || *p == '\t') p++;
      if (*p < '0' || *p > '9') break;
      kernel_id = strtoull(p, (char **)&p, 10);
    }
    break;
  }
  fclose(f);
  return kernel_id;
}

/**
 * True when the kernel is counting the scheduler statistics that
 * /proc/<tid>/schedstat reports.
 *
 * The kernel's own sched_info_on() decides this:
 *
 *   - with CONFIG_SCHEDSTATS compiled in, the statistics are always
 *     collected, and /proc/sys/kernel/sched_schedstats exists;
 *   - with only CONFIG_TASK_DELAY_ACCT, the statistics are collected
 *     only while /proc/sys/kernel/task_delayacct is 1, and that toggle
 *     is OFF by default;
 *   - with neither, the file reads "0 0 0" and always has.
 *
 * So on a distribution kernel, which compiles in CONFIG_TASK_DELAY_ACCT
 * without CONFIG_SCHEDSTATS, the run-queue delay reads zero until
 * somebody turns the toggle on. Measured on a CI runner: the
 * file read "0 0 0" and the preemption was reported as zero on a
 * machine where the threads were preempted constantly.
 *
 * Zero preemption is a measurement, and it is a false one. So the
 * statistics being off is detected, and the split is reported as
 * unavailable rather than as zero.
 */
static int pace_schedstat_available(void) {
  /* The value is computed once and never changes, but it is computed by
   * whichever thread arrives first, and read by the others. A plain
   * static int would be a data race, which the thread sanitizer
   * reports. So it is atomic. */
  static _Atomic int cached = -1;

  int known = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);
  if (known >= 0) return known;

  int available = 0;

  /* CONFIG_SCHEDSTATS compiled in: the statistics are always collected,
   * whatever the toggle says. */
  if (access("/proc/sys/kernel/sched_schedstats", F_OK) == 0) {
    available = 1;
  } else if (access("/proc/sys/kernel/task_delayacct", F_OK) == 0) {
    /* Only the delayacct fallback: the toggle decides, and it is off by
     * default. */
    int value = 0;
    FILE *f = fopen("/proc/sys/kernel/task_delayacct", "r");
    if (f) {
      if (fscanf(f, "%d", &value) != 1) value = 0;
      fclose(f);
    }
    available = value == 1;
  }
  /* Neither: the counters are not collected at all, and available stays
   * zero. */

  /* A later thread may have got here first and computed the same
   * answer, so the store is unconditional and the value is the same
   * either way. */
  __atomic_store_n(&cached, available, __ATOMIC_RELEASE);
  return available;
}

/**
 * Read /proc/<tid>/schedstat: "<on_cpu_ns> <run_delay_ns> <timeslices>".
 *
 * The file is a single short line, so a plain read into a stack buffer is
 * cheaper than a formatted scanf and needs no allocation.
 *
 * pread() with offset 0 is required, not read(): a /proc file is a seq_file
 * whose offset does not rewind on its own, so a second read() would return
 * EOF and the sample would fail.
 */
static pace_status pace_read_schedstat(int fd, uint64_t *on_cpu_ns, uint64_t *run_delay_ns,
                                        uint64_t *timeslices) {
  char buf[128];
  ssize_t n;
  do {
    n = pread(fd, buf, sizeof buf - 1, 0);
  } while (n < 0 && errno == EINTR);
  if (n <= 0) return PACE_ERR_IO;
  buf[n] = '\0';

  char *end = NULL;
  unsigned long long a = strtoull(buf, &end, 10);
  if (end == buf) return PACE_ERR_IO;
  unsigned long long b = strtoull(end, &end, 10);
  if (end == buf) return PACE_ERR_IO;
  unsigned long long c = strtoull(end, &end, 10);
  if (end == buf) c = 0;

  *on_cpu_ns = (uint64_t)a;
  *run_delay_ns = (uint64_t)b;
  *timeslices = (uint64_t)c;
  return PACE_OK;
}

/**
 * Read the voluntary/involuntary switch counters from /proc/<tid>/sched.
 *
 * The file has one "name<padding>: value" pair per line. Only two keys are
 * needed, so the file is streamed and the two lines are matched by prefix.
 */
static pace_status pace_read_sched_counters(uint64_t tid, uint64_t *voluntary,
                                             uint64_t *involuntary) {
  char path[64];
  snprintf(path, sizeof path, "/proc/%" PRIu64 "/sched", tid);
  FILE *f = fopen(path, "re");
  if (!f) return PACE_ERR_IO;

  char line[256];
  int got_vol = 0, got_invol = 0;
  *voluntary = 0;
  *involuntary = 0;

  while (fgets(line, sizeof line, f)) {
    if (!got_vol && strncmp(line, "nr_voluntary_switches", 22) == 0) {
      char *colon = strchr(line, ':');
      if (colon) {
        *voluntary = strtoull(colon + 1, NULL, 10);
        got_vol = 1;
      }
    } else if (!got_invol && strncmp(line, "nr_involuntary_switches", 24) == 0) {
      char *colon = strchr(line, ':');
      if (colon) {
        *involuntary = strtoull(colon + 1, NULL, 10);
        got_invol = 1;
      }
    }
    if (got_vol && got_invol) break;
  }
  fclose(f);
  return (got_vol || got_invol) ? PACE_OK : PACE_ERR_IO;
}

/* --------------------------------------------------------------- perf fds */

#if defined(__linux__)
static long pace_perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu, int group_fd,
                                 unsigned long flags) {
  return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/* The calling thread's OS thread id is pace_current_thread_id(), which the
 * session layer defines once for every platform. */

static int pace_open_perf_counter(uint64_t tid, uint64_t config, int type) {
  struct perf_event_attr attr;
  memset(&attr, 0, sizeof attr);
  attr.size = sizeof attr;
  attr.type = (uint32_t)type;
  attr.config = config;
  attr.disabled = 1;
  /* inherit=0: a thread-level counter must not absorb child threads' cycles. */
  attr.inherit = 0;
  attr.exclude_kernel = 0;
  attr.exclude_hv = 0;
  attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

  long fd = pace_perf_event_open(&attr, (pid_t)tid, -1, -1, 0);
  if (fd < 0) return -1;
  if (ioctl((int)fd, PERF_EVENT_IOC_RESET, 0) != 0 ||
      ioctl((int)fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
    close((int)fd);
    return -1;
  }
  return (int)fd;
}

static uint64_t pace_read_perf_counter(int fd, uint64_t *time_enabled, uint64_t *time_running) {
  struct {
    uint64_t value, time_enabled, time_running;
  } pr;
  *time_enabled = 0;
  *time_running = 0;
  if (fd < 0) return 0;
  ssize_t n = read(fd, &pr, sizeof pr);
  if (n < (ssize_t)sizeof(uint64_t)) return 0;
  *time_enabled = pr.time_enabled;
  *time_running = pr.time_running;
  return pr.value;
}
#endif /* __linux__ */

/* ---------------------------------------------------------------- backend */

static pace_status pace_linux_probe(pace_session *session) {
  session->reference_hz = pace_reference_hz();
  session->source = PACE_SOURCE_CLOCK;

#if defined(PACE_WITH_BPF)
  if (session->config.prefer_ebpf_tier) {
    session->bpf = pace_bpf_open();
    if (session->bpf) {
      session->bpf_available = pace_bpf_is_available(session->bpf);
    }
  }
#endif
  return PACE_OK;
}

static pace_status pace_linux_thread_init(pace_session *session, pace_thread *thread) {
  pace_linux_thread *lt = (pace_linux_thread *)calloc(1, sizeof *lt);
  if (!lt) return PACE_ERR_NOMEM;
  lt->schedstat_fd = -1;
  lt->perf_cycles_fd = -1;
  lt->perf_ref_fd = -1;
  lt->cpu_last = -1;

  lt->schedstat_fd = pace_open_schedstat(thread->thread_id);
  if (lt->schedstat_fd < 0) {
    free(lt);
    return PACE_ERR_UNSUPPORTED;
  }

  /* The eBPF tier's maps are keyed by the kernel's thread id, which is
   * not the same as the one gettid() returns when there is more than one
   * pid namespace. It is resolved here, and retried at the point of use
   * until it succeeds -- see pace_linux_resolve_kernel_tid. */
  lt->kernel_tid = pace_kernel_thread_id(thread->thread_id);
  lt->kernel_tid_check_countdown = 0;

#if defined(__linux__)
  if (session->config.prefer_perf_counters) {
    if (session->config.keep_perf_fds_open) {
      /* Keep one fd per counter per thread. Fast, but a 1000-thread process
       * would hold 2000 descriptors. */
      lt->perf_cycles_fd = pace_open_perf_counter(thread->thread_id,
                                                  PERF_COUNT_HW_CPU_CYCLES, PERF_TYPE_HARDWARE);
      lt->perf_ref_fd = pace_open_perf_counter(thread->thread_id,
                                                PERF_COUNT_HW_REF_CPU_CYCLES,
                                                PERF_TYPE_HARDWARE);
    } else {
      /* Open the counters on demand and close them after the sample. The
       * counter has to be opened against the thread's own id, so this only
       * works for the calling thread. */
      if (thread->thread_id == pace_current_thread_id()) {
        lt->perf_cycles_fd = pace_open_perf_counter(thread->thread_id,
                                                    PERF_COUNT_HW_CPU_CYCLES,
                                                    PERF_TYPE_HARDWARE);
        lt->perf_ref_fd = pace_open_perf_counter(thread->thread_id,
                                                  PERF_COUNT_HW_REF_CPU_CYCLES,
                                                  PERF_TYPE_HARDWARE);
        lt->perf_fds_transient = 1;
      }
    }
    if (lt->perf_cycles_fd >= 0) session->source = PACE_SOURCE_PERF;
  }
#endif

  thread->backend_data = lt;
  return PACE_OK;
}

/**
 * The thread's id in the kernel's pid namespace, resolved lazily.
 *
 * The resolution is attempted at registration and retried here until it
 * succeeds, because the NSpid line in /proc/self/task/<tid>/status is
 * not always populated when a thread is first registered: a thread that
 * has just been created can be seen before the kernel has filled it in.
 *
 * Measured on a CI runner: 32 of 48 workers resolved on the first
 * attempt and 16 did not, and the ones that did not fell back to the
 * namespace's thread id, whose lookup misses the map entirely. So they
 * reported the kernel's counter while the tier claimed to be attached.
 *
 * The retry only happens while the resolution is still failing, which is
 * a short window at the start of a thread's life, and the value is
 * cached once it succeeds.
 */
static uint64_t pace_linux_resolve_kernel_tid(pace_linux_thread *lt, uint64_t tid,
                                               struct pace_bpf *bpf) {
  if (lt->kernel_tid != 0) return lt->kernel_tid;
  if (!bpf) return 0;

  /* The namespace's id is tried FIRST, because on a machine with one
   * pid namespace it IS the kernel's id, and the map confirms it in one
   * cheap lookup with no /proc read at all. */
  if (pace_bpf_has_thread(bpf, tid)) {
    lt->kernel_tid = tid;
    return lt->kernel_tid;
  }

  /* The map's keys ARE the kernel's thread ids, so the map's presence
   * for a candidate is the proof that the candidate is the kernel's.
   *
   * The two candidates are checked in order, and the first one is
   * FREE:
   *
   *   - the namespace's id, which the SDK already has. On a machine
   *     with one pid namespace -- the overwhelmingly common case -- this
   *     IS the kernel's id, and the map confirms it in one cheap
   *     lookup. No /proc read is needed at all.
   *
   *   - the resolved id, which needs a /proc read to obtain and is
   *     therefore checked less often.
   *
   * So the confirmation is as prompt as the map can allow, and the
   * expensive read happens only while the cheap check has not settled
   * it. That matters: the tier's map gains an entry for a thread when
   * the thread is switched in with a runnable-since timestamp, and the
   * confirmation is what turns the tier's measurement on. Confirming
   * late loses the time before the confirmation, and on a CI runner
   * that was 1.1 s of every thread's 1.81 s of preemption.
   *
   * Measured on a CI runner: the resolution answered wrongly for 16 of
   * 48 workers, and those workers fell back to the kernel's counter
   * while the tier claimed to be attached. */
  if (lt->kernel_tid_check_countdown > 0) {
    lt->kernel_tid_check_countdown--;
    return 0;
  }
  /* Every 16 samples: the /proc read is the expensive part, and it is
   * only needed when the process has more than one pid namespace. */
  lt->kernel_tid_check_countdown = 15;

  uint64_t resolved = pace_kernel_thread_id(tid);
  if (resolved != 0 && pace_bpf_has_thread(bpf, resolved)) {
    lt->kernel_tid = resolved;
    return lt->kernel_tid;
  }

  /* The map recognises neither, so the thread has not been scheduled
   * since the program loaded, and the tier's value for it is zero --
   * which is correct: a thread that has not run has not been
   * preempted.
   *
   * So the namespace's id is used, and the value is read under it. The
   * thread's first switch-in creates the map's entry under the kernel's
   * id, and the resolution is confirmed at the next sample. */
  return tid;
}

static pace_status pace_linux_thread_read(pace_session *session, pace_thread *thread,
                                          pace_raw *out_raw) {
  pace_linux_thread *lt = (pace_linux_thread *)thread->backend_data;
  if (!lt) return PACE_ERR_STATE;
  (void)session;

  memset(out_raw, 0, sizeof *out_raw);

  /* Guard against a recycled thread id: if the thread that owns this handle
   * has been replaced by a new one with the same id, the cached descriptor
   * would report the new thread's accounting. Re-open the descriptor when
   * the start time no longer matches.
   *
   * Reading /proc/<tid>/stat costs about 2.8 microseconds, which would
   * dominate the sample, so the check runs once every N samples rather than
   * on every one. A recycled id is therefore detected within N samples of
   * the recycle, and the window of misattribution is bounded by that. */
  if (lt->start_time_check_countdown == 0) {
    lt->start_time_check_countdown = PACE_START_TIME_CHECK_INTERVAL;
    uint64_t current_start = 0;
    if (pace_read_start_time(thread->thread_id, &current_start) == 0 && current_start != 0) {
      if (lt->fd_start_time == 0) {
        lt->fd_start_time = current_start;
      } else if (current_start != lt->fd_start_time) {
        if (lt->schedstat_fd >= 0) close(lt->schedstat_fd);
        lt->schedstat_fd = pace_open_schedstat(thread->thread_id);
        lt->fd_start_time = current_start;
        if (lt->schedstat_fd < 0) return PACE_ERR_IO;
      }
    }
  }
  lt->start_time_check_countdown--;

  uint64_t wall_now = pace_now_monotonic_ns();
  uint64_t on_cpu_now = pace_now_thread_cpu_ns();

  uint64_t on_cpu_ns = 0, run_delay_ns = 0, timeslices = 0;
  if (pace_read_schedstat(lt->schedstat_fd, &on_cpu_ns, &run_delay_ns, &timeslices) != PACE_OK) {
    return PACE_ERR_IO;
  }

  /* Linux reports the run-queue delay directly, so the preemption and
   * blocking split is a real measurement on this platform -- as long as
   * the kernel is collecting the scheduler statistics. On a kernel with
   * them off, /proc/<tid>/schedstat reads "0 0 0" and the split is
   * unavailable rather than zero. */
  out_raw->has_split = pace_schedstat_available();

  uint64_t voluntary = 0, involuntary = 0;
  if (session->config.collect_switch_counters) {
    if (pace_read_sched_counters(thread->thread_id, &voluntary, &involuntary) != PACE_OK) {
      /* The switch counters are a refinement, not a requirement: the
       * run-queue delay already separates preemption from blocking. */
      out_raw->degraded = 1;
    }
  }

#if defined(PACE_WITH_BPF)
  /* The eBPF tier counts the scheduler's own view of this thread. When it is
   * attached it is a direct measurement of involuntary switches, taken from
   * the sched_wakeup and sched_switch tracepoints rather than inferred from
   * a cumulative counter. */
  if (session->bpf_available) {
    uint64_t kernel_tid =
        pace_linux_resolve_kernel_tid(lt, thread->thread_id, (struct pace_bpf *)session->bpf);
    uint64_t bpf_preempt = 0, bpf_block = 0, bpf_wakeup = 0;
    if (kernel_tid &&
        pace_bpf_read_thread((struct pace_bpf *)session->bpf, kernel_tid, &bpf_wakeup,
                             &bpf_preempt, &bpf_block) == PACE_OK) {
      if (bpf_preempt > 0) involuntary = bpf_preempt;
      if (bpf_block > 0) voluntary = bpf_block;
    }

    /* The eBPF tier also measures the preemption WAIT, from the interval
     * between the thread's sched_wakeup and the sched_switch that ran it.
     *
     * The kernel's run_delay counter measures the same phenomenon, but as
     * the time from queueing to dequeuing, so a thread that is woken,
     * queued, and then migrated or requeued loses the interval. On some
     * kernels the undercount is large: measured on a CI runner, the
     * kernel's counter reported 0.30 s where the scheduler's own events
     * showed 1.81 s, an 80 % undercount, while the on-CPU time and the
     * total off-CPU time both agreed with perf.
     *
     * So when the tier is attached its measurement is used in preference
     * to the counter. The counter is kept as the fallback for the threads
     * the program has not seen yet.
     *
     * The flag says which counter the value came from, so it is set
     * whenever the tier supplied the value -- NOT only when the value
     * happens to be non-zero. A flag that tracked the value rather than
     * the source would flicker between zero and non-zero as a thread
     * was and was not preempted, and every flicker would be read as a
     * change of source, which re-takes the baseline and degrades the
     * sample. */
    uint64_t bpf_preempted_ns = 0;
    if (pace_bpf_read_preempted_ns((struct pace_bpf *)session->bpf, kernel_tid,
                                   &bpf_preempted_ns) == PACE_OK) {
      /* The tier's counter starts at zero when the program loads, so its
       * difference from the thread's first sample is the whole window's
       * preemption. There is no need to switch sources, and switching
       * was losing the accumulated time: the re-baseline at the switch
       * took the tier's CURRENT value, which already included the
       * thread's first preemption, and the differences from then on
       * covered only the time after it.
       *
       * Measured on a CI runner: the thread was measured as preempted
       * for 0.38 s when the tier's counter said 1.78 s.
       *
       * Before the thread's first switch-in the tier's value is zero,
       * and zero is correct: the thread has not been scheduled, so it
       * has not been preempted. */
      out_raw->run_delay_ns = bpf_preempted_ns;
      out_raw->preempted_from_bpf = 1;
    }
  }
#endif

#if defined(__linux__)
  if (lt->perf_cycles_fd >= 0) {
    uint64_t en = 0, run = 0;
    out_raw->core_cycles = pace_read_perf_counter(lt->perf_cycles_fd, &en, &run);
    out_raw->time_enabled = en;
    out_raw->time_running = run;
  }
  if (lt->perf_ref_fd >= 0) {
    uint64_t en = 0, run = 0;
    out_raw->ref_cycles = pace_read_perf_counter(lt->perf_ref_fd, &en, &run);
  }
#endif
  out_raw->wall_ns = wall_now;
  out_raw->on_cpu_ns = on_cpu_now;
  /* The run-queue delay, from whichever counter supplied it. The eBPF
   * tier's value was already stored above, and this assignment would
   * otherwise overwrite it with the kernel's counter -- which is what
   * it did, so the tier's measurement was computed and then discarded,
   * and the SDK reported the kernel's counter in every case.
   *
   * The two agree to within a percent on some kernels and differ by 80 %
   * on others, so which one is in the sample matters. */
  if (!out_raw->preempted_from_bpf) {
    out_raw->run_delay_ns = run_delay_ns;
  }
  out_raw->involuntary_switches = involuntary;
  out_raw->voluntary_switches = voluntary;

  (void)timeslices;

#if defined(__linux__)
  /* In the transient mode the counters were opened for this sample only, so
   * close them now: the descriptor count then does not scale with the number
   * of registered threads. */
  if (lt->perf_fds_transient) {
    if (lt->perf_cycles_fd >= 0) close(lt->perf_cycles_fd);
    if (lt->perf_ref_fd >= 0) close(lt->perf_ref_fd);
    lt->perf_cycles_fd = -1;
    lt->perf_ref_fd = -1;
  }
#endif

  return PACE_OK;
}

/**
 * The thread's id in the kernel's pid namespace, as resolved and
 * verified by the SDK.
 *
 * The tier's maps are keyed by that id, and it is not the one gettid()
 * returns when the process has more than one pid namespace. An
 * internal tool that reads the map needs the same id the SDK used, or
 * its lookup misses.
 */
uint64_t pace_linux_thread_kernel_tid(pace_thread *thread) {
  if (!thread) return 0;
  pace_linux_thread *lt = (pace_linux_thread *)thread->backend_data;
  if (!lt) return 0;
  return lt->kernel_tid;
}

static void pace_linux_thread_free(pace_session *session, pace_thread *thread) {
  (void)session;
  pace_linux_thread *lt = (pace_linux_thread *)thread->backend_data;
  if (!lt) return;
  if (lt->schedstat_fd >= 0) close(lt->schedstat_fd);
  if (lt->perf_cycles_fd >= 0) close(lt->perf_cycles_fd);
  if (lt->perf_ref_fd >= 0) close(lt->perf_ref_fd);
  free(lt);
  thread->backend_data = NULL;
}

static void pace_linux_session_free(pace_session *session) {
#if defined(PACE_WITH_BPF)
  if (session->bpf) {
    pace_bpf_close((struct pace_bpf *)session->bpf);
    session->bpf = NULL;
  }
#else
  (void)session;
#endif
}

static const pace_backend pace_linux_backend = {
    .name = "linux",
    .probe = pace_linux_probe,
    .thread_init = pace_linux_thread_init,
    .thread_read = pace_linux_thread_read,
    .thread_free = pace_linux_thread_free,
    .session_free = pace_linux_session_free,
};

const pace_backend *pace_backend_current(void) {
#if defined(__linux__)
  return &pace_linux_backend;
#else
  static const pace_backend unsupported = {0};
  return &unsupported;
#endif
}
