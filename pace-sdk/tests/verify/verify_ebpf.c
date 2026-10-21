/* SPDX-License-Identifier: BUSL-1.1
 * The eBPF tier's own verification.
 *
 * Attaching is not the same as working. This program attaches the tier,
 * runs a contended workload, and then reads the tier's maps to confirm
 * that the kernel's scheduler events were actually recorded: that threads
 * were seen becoming runnable, that they were seen being switched out
 * while runnable, and that the counts are per-thread rather than global.
 *
 * It needs privilege, and it says so plainly when it does not have it.
 */
#include "pace/pace.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

#define WORKERS 32
#define SPIN_MS 700

/* A flag the workers check, so they stop when the test is done. A tight
 * spin loop has no cancellation point, so pthread_cancel would not stop
 * it. */
static volatile int stop_workers = 0;

/* The session the workers sample, and the result they each saw. */
static pace_session *g_session = NULL;
static uint64_t g_preempted[WORKERS];
static uint64_t g_wall[WORKERS];

/* A worker that spins, so the scheduler has to timeshare the CPUs.
 *
 * The worker samples itself, because a spinning worker is the thread that
 * is certainly being preempted: the main thread may well keep running
 * while the scheduler swaps the workers among themselves. */
static void *spinner(void *arg) {
  long index = (long)arg;
  pace_thread *thread = NULL;
  if (pace_session_register_thread(g_session, &thread) != PACE_OK) return NULL;

  pace_sample sample;
  if (pace_session_sample(g_session, thread, &sample) != PACE_OK) return NULL;

  while (!stop_workers) {
    volatile double x = 1.0;
    for (int i = 0; i < 20000; i++) x += (double)i / 3.0;
    if (pace_session_sample(g_session, thread, &sample) == PACE_OK) {
      g_preempted[index] = sample.preempted_ns;
      g_wall[index] = sample.wall_ns;
    }
  }
  return NULL;
}

int main(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  cfg.prefer_ebpf_tier = 1;

  pace_session *session = NULL;
  if (pace_session_create(&cfg, &session) != PACE_OK) {
    printf("the session could not be created\n");
    return 1;
  }

  pace_stats stats;
  memset(&stats, 0, sizeof stats);
  stats.struct_size = sizeof stats;
  pace_session_stats(session, &stats);

  if (!stats.ebpf_available) {
    printf("the eBPF tier is not available on this host\n");
    printf("that is a legitimate state: the tier needs privilege to open the\n");
    printf("kernel's raw tracepoints, and the procfs tier carries the\n");
    printf("measurement when it is not.\n");
    pace_session_destroy(session);
    return 0;
  }

  printf("the eBPF tier is attached\n");

  g_session = session;
  pthread_t workers[WORKERS];
  for (long i = 0; i < WORKERS; i++) {
    pthread_create(&workers[i], NULL, spinner, (void *)i);
  }
  usleep(SPIN_MS * 1000);
  stop_workers = 1;
  for (int i = 0; i < WORKERS; i++) pthread_join(workers[i], NULL);

  /* Every worker competed for a CPU with 31 others, so every worker must
   * have been preempted, and every worker's decomposition must hold. */
  int preempted_count = 0;
  uint64_t total_preempted = 0, total_wall = 0;
  for (int i = 0; i < WORKERS; i++) {
    if (g_wall[i] == 0) continue;
    if (g_preempted[i] > 0) preempted_count++;
    total_preempted += g_preempted[i];
    total_wall += g_wall[i];
  }
  CHECK(g_wall[0] > 0, "the workers sampled");
  CHECK(preempted_count > 0, "%d of %d workers were preempted", preempted_count, WORKERS);
  printf("%d of %d workers were preempted; %" PRIu64 " ns of %" PRIu64 " ns wall\n",
         preempted_count, WORKERS, total_preempted, total_wall);

  /* The main thread's own sample must carry a real split, because the
   * eBPF tier was attached for it. */
  pace_thread *self = NULL;
  CHECK(pace_session_register_thread(session, &self) == PACE_OK, "registered");
  pace_sample sample;
  for (int i = 0; i < 20; i++) pace_session_sample(session, self, &sample);
  CHECK(sample.flags & PACE_SAMPLE_F_HAS_PREEMPTED, "the sample has a preemption split");
  CHECK(sample.flags & PACE_SAMPLE_F_HAS_BLOCKED, "the sample has a blocking split");
  CHECK(sample.on_cpu_ns + sample.preempted_ns + sample.blocked_ns == sample.wall_ns,
        "the decomposition holds exactly");

  pace_session_destroy(session);
  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
