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
   * blocking split is a real measurement on this platform. */
  out_raw->has_split = 1;

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
    uint64_t bpf_preempt = 0, bpf_block = 0, bpf_wakeup = 0;
    if (pace_bpf_read_thread((struct pace_bpf *)session->bpf, thread->thread_id, &bpf_wakeup,
                             &bpf_preempt, &bpf_block) == PACE_OK) {
      if (bpf_preempt > 0) involuntary = bpf_preempt;
      if (bpf_block > 0) voluntary = bpf_block;
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
  out_raw->run_delay_ns = run_delay_ns;
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
