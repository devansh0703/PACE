// SPDX-License-Identifier: BUSL-1.1
// Compare three measurements of the same quantity, for the same threads,
// in the same window:
//
//   1. the kernel's /proc/<tid>/schedstat run_delay counter,
//   2. the eBPF tier's measurement of the scheduler's own events, read
//      from the tier's map, and
//   3. what the SDK REPORTS to a consumer.
//
// The third is the one that matters, and the one that was missing.
//
// The tier's map and the SDK's reported value used to disagree, and this
// program could not see it, because it only read the map. The SDK's
// reported value went through a different path, and on that path the
// tier's measurement was overwritten by the kernel's counter before it
// reached the sample. So the map said one thing and the SDK said
// another, and only the map was being checked.
//
// This program samples through the SDK and reads the map for the same
// thread, so the two are compared directly. A disagreement is a defect
// in one of them, and this program says which.
//
// WHY THE KERNEL'S COUNTER AND THE TIER DIFFER
// --------------------------------------------
// The kernel computes its counter as the time from QUEUEING to
// DEQUEUEING:
//
//   sched_info_queued():  if (!t->sched_info.last_queued)
//                             t->sched_info.last_queued = rq_clock(rq);
//   sched_info_dequeue():  delta = rq_clock(rq) - t->sched_info.last_queued;
//                           t->sched_info.run_delay += delta;
//
// A thread that is woken, queued, and then migrated or requeued loses
// the interval, because the counter is reset when the thread leaves one
// runqueue and only restarts when it enters the next. The tier measures
// the interval directly, from the thread's sched_wakeup or from the
// switch-out that preempted it, to the sched_switch that ran it.
//
// So the two are two different measurements of the same phenomenon, the
// kernel's is a lower bound, and the size of the gap is
// machine-dependent. Measured on a CI runner the kernel's counter
// reported 0.30 s where the scheduler's own events showed 1.81 s.
#include "pace/pace.h"

/* The internal headers, for the session's own state and for the portable
 * thread id helper. */
#include "pace_backend.h"
#include "pace_session.h"
#include "bpf/pace_bpf.h"

#include <dirent.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WORKERS 48

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                          \
  do {                                            \
    checks++;                                     \
    if (!(cond)) {                                \
      failures++;                                 \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                         \
      printf("\n");                               \
    }                                             \
  } while (0)

static pace_session *g_session = NULL;
static volatile int g_stop = 0;

/* A worker that spins and samples itself, so it is certainly the thread
 * that is being preempted. */
static void *worker(void *arg) {
  (void)arg;
  pace_thread *handle = NULL;
  if (pace_session_register_thread(g_session, &handle) != PACE_OK) return NULL;

  pace_sample sample;
  pace_session_sample(g_session, handle, &sample);

  while (!g_stop) {
    volatile unsigned long long acc = 0;
    for (unsigned long long i = 0; i < 500000ull; i++) acc += i;
    (void)acc;
    pace_session_sample(g_session, handle, &sample);
  }
  return NULL;
}

/* The kernel's counter for a thread, from /proc/self/task/<tid>/schedstat. */
static uint64_t kernel_run_delay(uint64_t tid) {
  char path[256];
  snprintf(path, sizeof path, "/proc/self/task/%" PRIu64 "/schedstat", tid);
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  unsigned long long oncpu = 0, rundelay = 0, slices = 0;
  int got = fscanf(f, "%llu %llu %llu", &oncpu, &rundelay, &slices);
  fclose(f);
  if (got != 3) return 0;
  return rundelay;
}

int main(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  cfg.prefer_ebpf_tier = 1;

  if (pace_session_create(&cfg, &g_session) != PACE_OK) {
    printf("the session could not be created\n");
    return 1;
  }

  pace_stats stats;
  memset(&stats, 0, sizeof stats);
  stats.struct_size = sizeof stats;
  pace_session_stats(g_session, &stats);

  if (!stats.ebpf_available) {
    printf("the eBPF tier is not available on this host\n");
    printf("that is a legitimate state: the tier needs privilege, and the\n");
    printf("procfs tier carries the measurement when it is not.\n");
    pace_session_destroy(g_session);
    return 0;
  }
  printf("the eBPF tier is attached\n");

  struct pace_bpf *bpf = (struct pace_bpf *)g_session->bpf;

  pthread_t threads[WORKERS];
  int started = 0;
  for (int i = 0; i < WORKERS; i++) {
    if (pthread_create(&threads[i], NULL, worker, NULL) == 0) started++;
  }
  sleep(2);

  /* The counters are read WHILE the workers are still running.
   *
   * A thread's /proc/<pid>/task/<tid> directory disappears when the
   * thread exits, so reading after the join compares only the main
   * thread -- which was never contended, and whose numbers say nothing
   * about the measurement. That is not a detail: it is how the check
   * came to compare one un-contended thread on a CI runner and report
   * a 20 ms run_delay as though it were the measurement. */
  /* The thread id is the name of the directory under /proc/self/task, so
   * the directory is enumerated rather than indexed. A pthread_t is not a
   * thread id, and the TIDs are not a dense range either. */
  DIR *dir = opendir("/proc/self/task");
  if (!dir) {
    printf("cannot open /proc/self/task\n");
    return 1;
  }

  long compared = 0;
  double kd_total = 0.0, bpf_total = 0.0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;

    uint64_t tid = (uint64_t)strtoull(entry->d_name, NULL, 10);
    uint64_t kernel = kernel_run_delay(tid);
    uint64_t tier = 0;
    if (pace_bpf_read_preempted_ns(bpf, tid, &tier) != PACE_OK) continue;

    if (compared < 8) {
      printf("  thread %-8s the kernel's run_delay %12" PRIu64
             " ns   the eBPF measurement %12" PRIu64 " ns\n",
             entry->d_name, kernel, tier);
    }
    kd_total += (double)kernel;
    bpf_total += (double)tier;
    compared++;
  }
  closedir(dir);

  g_stop = 1;
  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);

  printf("%ld threads compared\n", compared);
  printf("  the kernel's total run_delay: %.0f ns\n", kd_total);
  printf("  the eBPF tier's total:         %.0f ns\n", bpf_total);
  if (kd_total > 0.0) {
    printf("  the eBPF measurement is %+.2f %% of the kernel's counter\n",
           (bpf_total - kd_total) / kd_total * 100.0);
  }
  CHECK(compared > 1, "the workers were compared, not just the main thread");
  CHECK(bpf_total > 0.0, "the eBPF tier measured some preemption");

  /* What the tier observed. The state counts say whether a preempted
   * task reported TASK_RUNNING when it was switched out, and the wait
   * counts say how many switch-ins had a timestamp to measure from.
   * Between them they account for the measurement, so a difference
   * between two machines is attributable rather than mysterious. */
  uint64_t switch_ins = 0, with_timestamp = 0;
  pace_bpf_read_diagnostic(bpf, "wait_counts", 0, &switch_ins);
  pace_bpf_read_diagnostic(bpf, "wait_counts", 1, &with_timestamp);
  printf("  the switch-ins the tier saw: %" PRIu64 ", of which %" PRIu64
         " had a timestamp to measure from\n",
         switch_ins, with_timestamp);

  printf("  the switch-outs the tier saw, by the task's state:\n");
  for (uint64_t state = 0; state < 8; state++) {
    uint64_t count = 0;
    pace_bpf_read_diagnostic(bpf, "state_counts", state, &count);
    if (count == 0) continue;
    const char *name = "another state";
    if (state == 0) name = "TASK_RUNNING, so it was runnable and preempted";
    else if (state == 1) name = "TASK_INTERRUPTIBLE, so it was sleeping";
    else if (state == 2) name = "TASK_UNINTERRUPTIBLE, so it was sleeping";
    printf("    state %" PRIu64 ": %" PRIu64 " -- %s\n", state, count, name);
  }

  /* The check that was missing: what the SDK REPORTS must be the tier's
   * measurement, not the kernel's counter.
   *
   * A fresh session samples one thread for a known window, and the
   * reported preemption for that window is compared against the tier's
   * own cumulative value over the same window. */
  {
    pace_config probe_cfg;
    pace_config_init(&probe_cfg);
    probe_cfg.prefer_ebpf_tier = 1;
    pace_session *probe = NULL;
    if (pace_session_create(&probe_cfg, &probe) == PACE_OK) {
      struct pace_bpf *probe_bpf = (struct pace_bpf *)probe->bpf;
      pace_thread *handle = NULL;
      if (pace_session_register_thread(probe, &handle) == PACE_OK) {
        pace_sample first, second;
        /* The first sample establishes the baseline. */
        pace_session_sample(probe, handle, &first);
        uint64_t before = 0;
        pace_bpf_read_preempted_ns(probe_bpf, pace_current_thread_id(), &before);

        usleep(300 * 1000);

        pace_session_sample(probe, handle, &second);
        uint64_t after = 0;
        pace_bpf_read_preempted_ns(probe_bpf, pace_current_thread_id(), &after);

        uint64_t tier_delta = after - before;
        printf("  the SDK's reported preemption over the window: %" PRIu64 " ns\n",
               second.preempted_ns);
        printf("  the tier's own measurement over the window:     %" PRIu64 " ns\n",
               tier_delta);
        CHECK(second.flags & PACE_SAMPLE_F_HAS_PREEMPTED,
              "the sample claims a preemption measurement");
        /* The SDK's value and the tier's must be the same number. They
         * were not, and the difference was the tier's value being
         * overwritten by the kernel's counter on the way to the
         * sample. */
        if (tier_delta > 0) {
          uint64_t difference = second.preempted_ns > tier_delta
                                    ? second.preempted_ns - tier_delta
                                    : tier_delta - second.preempted_ns;
          CHECK(difference * 100 <= tier_delta * 5,
                "the SDK reported %" PRIu64 " ns where the tier measured %" PRIu64 " ns",
                second.preempted_ns, tier_delta);
        }
      }
      pace_session_destroy(probe);
    }
  }

  printf("%d checks, %d failures\n", checks, failures);
  pace_session_destroy(g_session);
  return failures == 0 ? 0 : 1;
}
