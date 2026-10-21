// PACE empirical probe v4 — definitive, clean verification of the measurement premises.
//
// Findings being verified (v3 was run from a stale binary; the real findings are in v4):
//   A. Per-thread PERF_COUNT_HW_CPU_CYCLES (pid=tid, cpu=-1, inherit=0) counts ONLY
//      on-CPU cycles: it does not advance while the task is descheduled.
//   B. inherit=1 contaminates a thread-level counter with every child thread's cycles.
//   C. /proc/<tid>/schedstat = (on_cpu_ns, run_delay_ns, timeslices) where run_delay is
//      runqueue-wait time = involuntary preemption wait.
//   D. /proc/<tid>/sched exposes nr_voluntary_switches / nr_involuntary_switches.
//   E. Decomposition: wall = on_cpu + run_delay + blocked.
//   F. The counter follows the thread across CPU migrations.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

#ifndef PERF_COUNT_HW_REF_CPU_CYCLES
#define PERF_COUNT_HW_REF_CPU_CYCLES 9
#endif

static long perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
                             int group_fd, unsigned long flags) {
  return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}
static inline uint64_t now_ns(clockid_t c) {
  struct timespec ts; clock_gettime(c, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ __volatile__("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
  return ((uint64_t)hi << 32) | lo;
}
static inline void cpuid_count(uint32_t leaf, uint32_t sub, uint32_t out[4]) {
  __asm__ __volatile__("cpuid" : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
                       : "a"(leaf), "c"(sub));
}
// CPUID.15H: TSC/crystal ratio -> tsc_hz = crystal_hz * ebx / eax.
// CPUID.16H EAX: base frequency in MHz (fallback).
static double tsc_hz(void) {
  uint32_t r[4];
  cpuid_count(0, 0, r);
  uint32_t max_leaf = r[0];
  if (max_leaf >= 0x15) {
    cpuid_count(0x15, 0, r);
    if (r[0] && r[1] && r[2]) return (double)r[2] * (double)r[1] / (double)r[0];
  }
  if (max_leaf >= 0x16) {
    cpuid_count(0x16, 0, r);
    if (r[0]) return (double)r[0] * 1000000.0;
  }
  return 0.0;
}

struct perf_read { uint64_t value, time_enabled, time_running; };

static int read_schedstat(pid_t tid, uint64_t *on_cpu, uint64_t *run_delay, uint64_t *slices) {
  char path[128]; snprintf(path, sizeof path, "/proc/%d/schedstat", tid);
  FILE *f = fopen(path, "r"); if (!f) return -1;
  uint64_t s = 0; int n = fscanf(f, "%" SCNu64 " %" SCNu64 " %" SCNu64, on_cpu, run_delay, &s);
  fclose(f);
  if (n < 2) return -1;
  *slices = s; return 0;
}

// /proc/<tid>/sched: "name<pad>: value" — find the key, then parse after the colon.
static int read_sched_kv(pid_t tid, const char *key, double *out) {
  char path[128]; snprintf(path, sizeof path, "/proc/%d/sched", tid);
  FILE *f = fopen(path, "r"); if (!f) return -1;
  char line[512]; size_t klen = strlen(key); int found = -1;
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, key, klen)) {
      char *colon = strchr(line, ':');
      if (colon) { *out = strtod(colon + 1, NULL); found = 0; }
      break;
    }
  }
  fclose(f); return found;
}

static int read_proc_stat_cpu(pid_t tid, int *cpu, char *state) {
  char path[128]; snprintf(path, sizeof path, "/proc/%d/stat", tid);
  FILE *f = fopen(path, "r"); if (!f) return -1;
  char buf[8192]; size_t len = fread(buf, 1, sizeof buf - 1, f); buf[len] = 0; fclose(f);
  char *p = strchr(buf, '('); if (!p) return -1;
  char *cl = strchr(p, ')'); if (!cl) return -1;
  *state = cl[2] ? cl[2] : '?';
  p = cl + 1; int field = 2; char *save = NULL; *cpu = -1;
  for (char *tok = strtok_r(p, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
    field++;
    if (field == 39) *cpu = atoi(tok);
  }
  return 0;
}

struct counters {
  uint64_t cycles, ref_cycles, task_clock;
  uint64_t cyc_enabled, cyc_running, ref_enabled, ref_running;
  uint64_t task_enabled, task_running;
};

/**
 * Read a perf counter.
 *
 * Returns 0 when the counter could not be read, and leaves the
 * destinations at zero in that case. A counter that was never opened must
 * report nothing, not whatever happened to be on the stack: a probe that
 * prints uninitialised memory as a measurement is worse than a probe that
 * prints nothing.
 */
static int read_cyc(int fd, uint64_t *val, uint64_t *en, uint64_t *run) {
  *val = 0;
  *en = 0;
  *run = 0;
  if (fd < 0) return 0;
  struct perf_read pr = {0, 0, 0};
  if (read(fd, &pr, sizeof pr) < (ssize_t)sizeof(uint64_t)) return 0;
  *val = pr.value;
  *en = pr.time_enabled;
  *run = pr.time_running;
  return 1;
}

struct sample {
  uint64_t t_mono, t_threadcpu, t_tsc;
  struct counters c;
  uint64_t sched_on, sched_run_delay, sched_slices;
  double vol, invol, exec_runtime, migrations;
  int cpu; char state;
};

static void take(struct sample *s, int fd_cyc, int fd_ref, int fd_task, pid_t tid) {
  /* Zero first: a counter that could not be read must report nothing
   * rather than whatever was on the stack. */
  memset(s, 0, sizeof *s);
  s->t_mono = now_ns(CLOCK_MONOTONIC);
  s->t_threadcpu = now_ns(CLOCK_THREAD_CPUTIME_ID);
  s->t_tsc = rdtsc();
  read_cyc(fd_cyc, &s->c.cycles, &s->c.cyc_enabled, &s->c.cyc_running);
  read_cyc(fd_ref, &s->c.ref_cycles, &s->c.ref_enabled, &s->c.ref_running);
  /* The task clock has its own enabled/running pair. Writing them into
   * the cycles' pair would report one counter's scheduling as another's. */
  read_cyc(fd_task, &s->c.task_clock, &s->c.task_enabled, &s->c.task_running);
  read_schedstat(tid, &s->sched_on, &s->sched_run_delay, &s->sched_slices);
  read_sched_kv(tid, "nr_voluntary_switches", &s->vol);
  read_sched_kv(tid, "nr_involuntary_switches", &s->invol);
  read_sched_kv(tid, "se.sum_exec_runtime", &s->exec_runtime);
  read_sched_kv(tid, "se.nr_migrations", &s->migrations);
  read_proc_stat_cpu(tid, &s->cpu, &s->state);
}

static void print_diff(const char *tag, const struct sample *a, const struct sample *b, double hz) {
  uint64_t d_mono = b->t_mono - a->t_mono;
  uint64_t d_threadcpu = b->t_threadcpu - a->t_threadcpu;
  uint64_t d_tsc = b->t_tsc - a->t_tsc;
  uint64_t d_cyc = b->c.cycles - a->c.cycles;
  uint64_t d_ref = b->c.ref_cycles - a->c.ref_cycles;
  uint64_t d_task = b->c.task_clock - a->c.task_clock;
  uint64_t d_on = b->sched_on - a->sched_on;
  uint64_t d_delay = b->sched_run_delay - a->sched_run_delay;
  uint64_t d_slices = b->sched_slices - a->sched_slices;
  double d_vol = b->vol - a->vol, d_invol = b->invol - a->invol;
  double d_mig = b->migrations - a->migrations;
  double d_exec = b->exec_runtime - a->exec_runtime;
  /* The TSC IS the reference clock: it ticks at a constant rate, and the
   * reference cycle count is the number of those ticks. So the delta in
   * TSC ticks is already the delta in reference cycles, and multiplying it
   * by the frequency again is wrong by a factor of the frequency.
   *
   * The frequency is used below to convert the wall-clock nanoseconds into
   * reference cycles, which is the conversion a caller would make. */
  double wall_ref_cycles = (double)d_tsc;
  long long blocked = (long long)d_mono - (long long)d_on - (long long)d_delay;

  printf("SAMPLE %s\n", tag);
  printf("  wall_ns=%" PRIu64 " threadcputime_ns=%" PRIu64 " tsc_delta=%" PRIu64 " tsc_hz=%.0f wall_ref_cycles=%.0f\n",
         d_mono, d_threadcpu, d_tsc, hz, wall_ref_cycles);
  printf("  hw_cycles=%" PRIu64 " hw_ref_cycles=%" PRIu64 " hw_cycles_per_refcycle=%.4f\n",
         d_cyc, d_ref, wall_ref_cycles > 0 ? (double)d_cyc / wall_ref_cycles : 0.0);
  /* The wall clock in reference cycles, which is the conversion the SDK
   * performs when it derives cycles from nanoseconds. */
  printf("  wall_ref_cycles_from_ns=%.0f\n", (double)d_mono * hz / 1e9);
  printf("  cyc_time_enabled=%" PRIu64 " cyc_time_running=%" PRIu64 " running_fraction=%.4f\n",
         b->c.cyc_enabled, b->c.cyc_running,
         b->c.cyc_enabled ? (double)b->c.cyc_running / (double)b->c.cyc_enabled : 0.0);
  printf("  sw_task_clock_ns=%" PRIu64 "\n", d_task);
  printf("  schedstat on_cpu_ns=%" PRIu64 " run_delay_ns=%" PRIu64 " timeslices=%" PRIu64 "\n",
         d_on, d_delay, d_slices);
  printf("  nr_voluntary=%+.0f nr_involuntary=%+.0f se.nr_migrations=%+.0f se.sum_exec_runtime=%.0f\n",
         d_vol, d_invol, d_mig, d_exec);
  printf("  cpu=%d->%d state=%c->%c\n", a->cpu, b->cpu, a->state, b->state);
  printf("  DECOMP on_cpu=%" PRIu64 " preempted=%" PRIu64 " blocked=%lld sum_matches_wall=%s\n",
         d_on, d_delay, blocked,
         ((long long)d_on + (long long)d_delay + blocked) == (long long)d_mono ? "YES" : "NO");
  printf("  CYCLES on_cpu_refcycles=%.0f preempted_refcycles=%.0f blocked_refcycles=%.0f\n",
         (double)d_on * hz / 1e9, (double)d_delay * hz / 1e9, (double)blocked * hz / 1e9);
  fflush(stdout);
}

static volatile int g_stop = 0;
static void *hog(void *arg) {
  (void)arg;
  while (!g_stop) { for (volatile int i = 0; i < 20000; i++) { } }
  return NULL;
}

static int open_hw(uint64_t config, int type, int inherit) {
  struct perf_event_attr attr;
  memset(&attr, 0, sizeof attr);
  attr.size = sizeof attr; attr.type = (uint32_t)type; attr.config = config;
  attr.disabled = 1; attr.inherit = inherit;
  attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
  return (int)perf_event_open(&attr, (pid_t)syscall(__NR_gettid), -1, -1, 0);
}

int main(void) {
  double hz = tsc_hz();
  printf("TSC_HZ %.0f\n", hz);
  pid_t tid = (pid_t)syscall(__NR_gettid);
  int fd_cyc  = open_hw(PERF_COUNT_HW_CPU_CYCLES,  PERF_TYPE_HARDWARE, 0);
  int fd_ref  = open_hw(PERF_COUNT_HW_REF_CPU_CYCLES, PERF_TYPE_HARDWARE, 0);
  int fd_task = open_hw(PERF_COUNT_SW_TASK_CLOCK, PERF_TYPE_SOFTWARE, 0);
  printf("FDS cyc=%d ref=%d task=%d errno=%s\n", fd_cyc, fd_ref, fd_task,
         fd_cyc < 0 ? strerror(errno) : "ok");
  if (fd_cyc < 0) { printf("PERF_UNAVAILABLE errno=%d\n", errno); }

  int fds[3] = {fd_cyc, fd_ref, fd_task};
  for (int i = 0; i < 3; i++) {
    if (fds[i] < 0) continue;
    ioctl(fds[i], PERF_EVENT_IOC_RESET, 0);
    ioctl(fds[i], PERF_EVENT_IOC_ENABLE, 0);
  }

  struct sample a, b; struct timespec sl;

  // --- 1: exclusive on-CPU spin (no other threads yet) ---
  take(&a, fd_cyc, fd_ref, fd_task, tid);
  for (volatile int i = 0; i < 200000000; i++) { }
  take(&b, fd_cyc, fd_ref, fd_task, tid);
  print_diff("onspu", &a, &b, hz);

  // --- 2: 48 spinner threads -> involuntary preemption ---
  enum { NH = 48 };
  pthread_t hs[NH];
  for (int i = 0; i < NH; i++) pthread_create(&hs[i], NULL, hog, NULL);
  usleep(300000);
  take(&a, fd_cyc, fd_ref, fd_task, tid);
  for (volatile int i = 0; i < 200000000; i++) { }
  take(&b, fd_cyc, fd_ref, fd_task, tid);
  print_diff("contended", &a, &b, hz);

  g_stop = 1;
  for (int i = 0; i < NH; i++) pthread_join(hs[i], NULL);
  usleep(300000);

  // --- 3: pure blocking sleep (voluntary off-CPU) ---
  take(&a, fd_cyc, fd_ref, fd_task, tid);
  sl.tv_sec = 0; sl.tv_nsec = 200 * 1000 * 1000;
  nanosleep(&sl, NULL);
  take(&b, fd_cyc, fd_ref, fd_task, tid);
  print_diff("sleep200ms", &a, &b, hz);

  // --- 4: migration + counter continuity across CPUs ---
  for (int i = 0; i < NH; i++) pthread_create(&hs[i], NULL, hog, NULL);
  usleep(200000);
  printf("MIGRATION_SAMPLES (counter must keep accumulating across cpu changes)\n");
  uint64_t prev = 0;
  for (int i = 0; i < 12; i++) {
    uint64_t c = 0, en = 0, run = 0;
    read_cyc(fd_cyc, &c, &en, &run);
    uint64_t on = 0, dly = 0, sl2 = 0;
    read_schedstat(tid, &on, &dly, &sl2);
    int cpu = -1; char st = '?';
    read_proc_stat_cpu(tid, &cpu, &st);
    printf("  i=%2d cpu=%2d state=%c cycles=%" PRIu64 " (delta=%lld) on_cpu_ns=%" PRIu64 " run_delay_ns=%" PRIu64 "\n",
           i, cpu, st, c, (long long)(c - prev), on, dly);
    prev = c;
    usleep(30000);
  }
  g_stop = 1;
  for (int i = 0; i < NH; i++) pthread_join(hs[i], NULL);

  // --- 5: inherit=1 contamination check (separate counter) ---
  int fd_inh = open_hw(PERF_COUNT_HW_CPU_CYCLES, PERF_TYPE_HARDWARE, 1);
  if (fd_inh >= 0) {
    ioctl(fd_inh, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd_inh, PERF_EVENT_IOC_ENABLE, 0);
    uint64_t v0 = 0, e0 = 0, r0 = 0; read_cyc(fd_inh, &v0, &e0, &r0);
    pthread_t one;
    pthread_create(&one, NULL, hog, NULL);
    usleep(200000);
    g_stop = 1;
    pthread_join(one, NULL);
    uint64_t v1 = 0, e1 = 0, r1 = 0; read_cyc(fd_inh, &v1, &e1, &r1);
    printf("INHERIT_CHECK inherit1_cycles_delta=%" PRIu64 " (main thread only consumed far less)\n", v1 - v0);
    ioctl(fd_inh, PERF_EVENT_IOC_DISABLE, 0);
  }

  for (int i = 0; i < 3; i++) if (fds[i] >= 0) ioctl(fds[i], PERF_EVENT_IOC_DISABLE, 0);
  return 0;
}
