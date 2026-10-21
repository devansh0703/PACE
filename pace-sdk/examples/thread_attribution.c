/* SPDX-License-Identifier: BUSL-1.1
 * PACE — standalone demonstration of per-thread time attribution.
 *
 * Spawns worker threads that contend for CPU, samples each one, and prints
 * the on-CPU / preempted / blocked breakdown. This is the "where did the
 * frame time go" question, answered per thread.
 */
#include "pace/pace.h"

/* The internal backend header, for the portable thread id helper. */
#include "pace_backend.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NWORKERS 48 /* Deliberately oversubscribed: 48 threads on 16 CPUs. */

typedef struct {
  pace_session *session;
  uint64_t tid;
  pace_sample sample;
} worker;

static volatile int g_stop = 0;

/* The OS thread id, which is what /proc/<tid> and the analyzer key on.
 * The library's helper is portable: the kernel task id on Linux, the
 * thread id on Windows, the Mach port on macOS. */
static uint64_t current_tid(void) { return pace_current_thread_id(); }

static void *worker_main(void *arg) {
  worker *w = (worker *)arg;
  pace_thread *handle = NULL;
  if (pace_session_register_thread(w->session, &handle) != PACE_OK) return NULL;

  w->tid = current_tid();
  pace_session_sample(w->session, handle, &w->sample); /* baseline */

  while (!g_stop) {
    volatile unsigned long long acc = 0;
    for (unsigned long long i = 0; i < 2000000ull; i++) acc += i;
    (void)acc;
  }

  pace_session_sample(w->session, handle, &w->sample);
  return NULL;
}

int main(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  cfg.prefer_perf_counters = 1;

  pace_session *session = NULL;
  pace_status st = pace_session_create(&cfg, &session);
  if (st != PACE_OK) {
    fprintf(stderr, "pace_session_create failed: %s\n", pace_status_string(st));
    return 1;
  }

  printf("PACE thread attribution demo\n");
  printf("  backend            : %s\n", pace_backend_name());
  printf("  source tier        : %s\n",
         pace_session_source(session) == PACE_SOURCE_PERF ? "perf (measured core cycles)"
                                                         : "clock (derived cycles)");
  printf("  reference frequency: %.0f Hz\n", pace_session_reference_hz(session));
  printf("\n");

  worker workers[NWORKERS];
  pthread_t threads[NWORKERS];
  memset(workers, 0, sizeof workers);

  for (int i = 0; i < NWORKERS; i++) {
    workers[i].session = session;
    pthread_create(&threads[i], NULL, worker_main, &workers[i]);
  }

  /* Contend: the main thread also spins so the workers are preempted.
   * Without an oversubscribed CPU the workers would each keep their core and
   * nothing would be preempted. */
  struct timespec ts = {2, 0};
  nanosleep(&ts, NULL);
  g_stop = 1;

  for (int i = 0; i < NWORKERS; i++) pthread_join(threads[i], NULL);

  /* A figure the platform did not measure is shown as a dash, never as a
   * zero. Windows and macOS cannot separate preemption from blocking
   * without privilege, and a confident 0.0% would be a false claim. */
  int has_split = (workers[0].sample.flags & PACE_SAMPLE_F_HAS_PREEMPTED) != 0;
  if (!has_split) {
    printf("  note: this platform cannot separate preemption from blocking, so the\n"
           "        split columns are unavailable rather than zero.\n\n");
  }

  printf("%-8s %12s %12s %12s %10s\n", "thread", "on_cpu", "preempted", "blocked", "preempt%");
  uint64_t total_wall = 0, total_on_cpu = 0, total_preempted = 0, total_blocked = 0;
  for (int i = 0; i < NWORKERS; i++) {
    const pace_sample *s = &workers[i].sample;
    if (has_split) {
      double pct = s->wall_ns ? 100.0 * (double)s->preempted_ns / (double)s->wall_ns : 0.0;
      printf("%-8" PRIu64 " %9.2f ms %9.2f ms %9.2f ms %9.1f%%\n", workers[i].tid,
             (double)s->on_cpu_ns / 1e6, (double)s->preempted_ns / 1e6,
             (double)s->blocked_ns / 1e6, pct);
    } else {
      printf("%-8" PRIu64 " %9.2f ms %12s %12s %10s\n", workers[i].tid,
             (double)s->on_cpu_ns / 1e6, "-", "-", "-");
    }
    total_wall += s->wall_ns;
    total_on_cpu += s->on_cpu_ns;
    total_preempted += s->preempted_ns;
    total_blocked += s->blocked_ns;
  }
  if (has_split) {
    printf("%-8s %9.2f ms %9.2f ms %9.2f ms %9.1f%%\n", "TOTAL",
           (double)total_on_cpu / 1e6, (double)total_preempted / 1e6,
           (double)total_blocked / 1e6,
           total_wall ? 100.0 * (double)total_preempted / (double)total_wall : 0.0);
  } else {
    printf("%-8s %9.2f ms %12s %12s %10s\n", "TOTAL", (double)total_on_cpu / 1e6, "-", "-",
           "-");
  }

  pace_session_destroy(session);
  return 0;
}
