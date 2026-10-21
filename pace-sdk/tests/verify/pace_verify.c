/* SPDX-License-Identifier: BUSL-1.1
 * PACE — in-process accuracy verifier.
 *
 * Compares PACE's own numbers against the kernel's independent accounting
 * over the *same* thread and the *same* window, and prints one JSON object
 * per measurement so a harness can aggregate them.
 *
 * Three independent kernel sources are compared:
 *
 *   1. /proc/self/stat   utime+stime   - the task's signal-struct accounting
 *   2. /proc/self/schedstat             - the task's cfs_rq->se.sum_exec_runtime
 *                                        and its run-queue delay
 *   3. PACE                             - the library under test
 *
 * Sources 1 and 2 are maintained by different kernel code paths from the
 * schedstat seq_file PACE itself reads, so agreement is a real cross-check
 * rather than a tautology.
 */
#include "pace/pace.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/** utime+stime for a thread, in seconds, from /proc/<tid>/stat. */
static double proc_stat_cpu_seconds(long tid) {
  char path[64];
  snprintf(path, sizeof path, "/proc/%ld/stat", tid);
  FILE *f = fopen(path, "r");
  if (!f) return -1.0;
  char buf[8192];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = 0;
  fclose(f);

  char *open = strchr(buf, '(');
  char *close = strchr(open ? open : buf, ')');
  if (!close) return -1.0;
  char *p = close + 1;
  /* Field 3 is the state character; utime is field 14, stime field 15. */
  int field = 2;
  long utime = 0, stime = 0;
  char *save = NULL;
  for (char *tok = strtok_r(p, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
    field++;
    if (field == 14) utime = atol(tok);
    else if (field == 15) { stime = atol(tok); break; }
  }
  long ticks = sysconf(_SC_CLK_TCK);
  if (ticks <= 0) return -1.0;
  return (double)(utime + stime) / (double)ticks;
}

/** on-CPU ns and run-queue delay ns from /proc/<tid>/schedstat. */
static int proc_schedstat(long tid, uint64_t *on_cpu, uint64_t *run_delay) {
  char path[64];
  snprintf(path, sizeof path, "/proc/%ld/schedstat", tid);
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  unsigned long long a = 0, b = 0, c = 0;
  int n = fscanf(f, "%llu %llu %llu", &a, &b, &c);
  fclose(f);
  if (n < 2) return -1;
  *on_cpu = (uint64_t)a;
  *run_delay = (uint64_t)b;
  return 0;
}

int main(int argc, char **argv) {
  int mode = argc > 1 ? atoi(argv[1]) : 0; /* 0 = spin, 1 = sleep */
  int duration_ms = argc > 2 ? atoi(argv[2]) : 400;

  pace_config cfg;
  pace_config_init(&cfg);
  cfg.prefer_perf_counters = 1;

  pace_session *session = NULL;
  if (pace_session_create(&cfg, &session) != PACE_OK) {
    fprintf(stderr, "pace_session_create failed\n");
    return 1;
  }

  long tid = (long)syscall(__NR_gettid);
  pace_thread *handle = NULL;
  if (pace_session_register_thread(session, &handle) != PACE_OK) {
    fprintf(stderr, "register failed\n");
    return 1;
  }

  pace_sample before;
  pace_session_sample(session, handle, &before);

  /* Snapshot the independent sources at the same moment as the baseline. */
  double stat_before = proc_stat_cpu_seconds(tid);
  uint64_t sched_on_before = 0, sched_delay_before = 0;
  proc_schedstat(tid, &sched_on_before, &sched_delay_before);
  uint64_t t0 = now_ns();

  if (mode == 1) {
    struct timespec ts = {duration_ms / 1000, (long)(duration_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
  } else {
    uint64_t deadline = t0 + (uint64_t)duration_ms * 1000000ull;
    volatile unsigned long long acc = 0;
    while (now_ns() < deadline) {
      for (int i = 0; i < 200000; i++) acc += (unsigned)i;
    }
    (void)acc;
  }

  pace_sample after;
  pace_session_sample(session, handle, &after);

  double stat_after = proc_stat_cpu_seconds(tid);
  uint64_t sched_on_after = 0, sched_delay_after = 0;
  proc_schedstat(tid, &sched_on_after, &sched_delay_after);
  uint64_t t1 = now_ns();

  double pace_on_cpu_s = (double)after.on_cpu_ns / 1e9;
  double stat_on_cpu_s = stat_after - stat_before;
  double sched_on_cpu_s = (double)(sched_on_after - sched_on_before) / 1e9;
  double pace_preempted_s = (double)after.preempted_ns / 1e9;
  double sched_preempted_s = (double)(sched_delay_after - sched_delay_before) / 1e9;
  double pace_wall_s = (double)(t1 - t0) / 1e9;
  double pace_reported_wall_s = (double)after.wall_ns / 1e9;

  printf(
      "{\"mode\":\"%s\",\"tid\":%ld,"
      "\"pace_on_cpu_s\":%.9f,\"stat_on_cpu_s\":%.9f,\"schedstat_on_cpu_s\":%.9f,"
      "\"pace_preempted_s\":%.9f,\"schedstat_preempted_s\":%.9f,"
      "\"pace_wall_s\":%.9f,\"pace_reported_wall_s\":%.9f,"
      "\"pace_blocked_s\":%.9f,\"source\":\"%s\",\"ref_cycles\":%" PRIu64
      ",\"core_cycles\":%" PRIu64 "}\n",
      mode == 1 ? "sleep" : "spin", tid, pace_on_cpu_s, stat_on_cpu_s, sched_on_cpu_s,
      pace_preempted_s, sched_preempted_s, pace_wall_s, pace_reported_wall_s,
      (double)after.blocked_ns / 1e9,
      pace_session_source(session) == PACE_SOURCE_PERF ? "perf" : "clock", after.ref_cycles,
      after.core_cycles);

  pace_session_destroy(session);
  return 0;
}
