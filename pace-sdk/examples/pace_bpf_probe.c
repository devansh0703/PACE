/* SPDX-License-Identifier: BUSL-1.1
 * PACE — eBPF tier probe.
 *
 * Reports whether the eBPF scheduler tier loaded and attached, and if so,
 * what it observed for a contended workload.
 */
#include "pace/pace.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile int g_stop = 0;

static void *spinner(void *arg) {
  (void)arg;
  while (!g_stop) {
    volatile unsigned long long acc = 0;
    for (unsigned long long i = 0; i < 200000; i++) acc += i;
    (void)acc;
  }
  return NULL;
}

int main(int argc, char **argv) {
  int nthreads = argc > 1 ? atoi(argv[1]) : 48;
  int duration_ms = argc > 2 ? atoi(argv[2]) : 1500;

  pace_config cfg;
  pace_config_init(&cfg);
  cfg.prefer_ebpf_tier = 1;

  pace_session *session = NULL;
  pace_status st = pace_session_create(&cfg, &session);
  if (st != PACE_OK) {
    fprintf(stderr, "pace_session_create failed: %s\n", pace_status_string(st));
    return 1;
  }

  pace_stats stats;
  memset(&stats, 0, sizeof stats);
  stats.struct_size = sizeof stats;
  pace_session_stats(session, &stats);
  printf("session source tier : %s\n",
         stats.source == PACE_SOURCE_PERF ? "perf (measured core cycles)"
                                         : "clock (derived cycles)");

  /* A contended workload: more spinners than hardware threads. */
  if (nthreads > 128) nthreads = 128;
  pthread_t threads[128];
  int started = 0;
  for (int i = 0; i < nthreads; i++) {
    if (pthread_create(&threads[i], NULL, spinner, NULL) == 0) started++;
  }
  printf("worker threads      : %d\n", started);

  /* Sample the calling thread while the workers contend. */
  pace_sample sample;
  pace_session_sample_self(session, &sample);
  struct timespec ts = {duration_ms / 1000, (long)(duration_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  pace_session_sample_self(session, &sample);

  g_stop = 1;
  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);

  memset(&stats, 0, sizeof stats);
  stats.struct_size = sizeof stats;
  pace_session_stats(session, &stats);
  printf("samples taken       : %" PRIu64 "\n", stats.samples_taken);
  printf("degraded samples     : %" PRIu64 "\n", stats.degraded_samples);
  printf("io errors            : %" PRIu64 "\n", stats.io_errors);
  printf("flags                : 0x%x\n", sample.flags);
  printf("has preempted        : %s\n",
         (sample.flags & PACE_SAMPLE_F_HAS_PREEMPTED) ? "yes" : "no");
  printf("has blocked          : %s\n",
         (sample.flags & PACE_SAMPLE_F_HAS_BLOCKED) ? "yes" : "no");
  printf("on-CPU               : %.3f ms\n", (double)sample.on_cpu_ns / 1e6);
  printf("preempted            : %.3f ms\n", (double)sample.preempted_ns / 1e6);
  printf("blocked              : %.3f ms\n", (double)sample.blocked_ns / 1e6);
  printf("wall                 : %.3f ms\n", (double)sample.wall_ns / 1e6);

  pace_session_destroy(session);
  return 0;
}
