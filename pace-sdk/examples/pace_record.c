/* SPDX-License-Identifier: BUSL-1.1
 * PACE — record a real trace to a PACE JSONL file.
 *
 * This is the SDK's own trace writer, exercised against a real contended
 * workload, so the analyzer is fed a genuine trace rather than a fixture.
 *
 * Usage: pace_record <output.jsonl> [duration_ms] [threads]
 */
#include "pace/pace.h"

/* The internal backend header, for the portable thread id helper. */
#include "pace_backend.h"
#include "pace_trace.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pace_trace *g_trace = NULL;
static volatile int g_stop = 0;

typedef struct {
  pace_session *session;
  uint64_t tid;
} worker;

static void *worker_main(void *arg) {
  worker *w = (worker *)arg;
  pace_thread *handle = NULL;
  if (pace_session_register_thread(w->session, &handle) != PACE_OK) return NULL;
  w->tid = pace_current_thread_id();

  pace_sample sample;
  pace_session_sample(w->session, handle, &sample);

  while (!g_stop) {
    volatile unsigned long long acc = 0;
    for (unsigned long long i = 0; i < 500000ull; i++) acc += i;
    (void)acc;

    pace_session_sample(w->session, handle, &sample);
    pace_trace_write_sample(g_trace, w->tid, &sample);
  }
  return NULL;
}

int main(int argc, char **argv) {
  const char *out_path = argc > 1 ? argv[1] : "trace.jsonl";
  int duration_ms = argc > 2 ? atoi(argv[2]) : 2000;
  int nthreads = argc > 3 ? atoi(argv[3]) : 48;

  pace_config cfg;
  pace_config_init(&cfg);
  cfg.prefer_perf_counters = 1;

  pace_session *session = NULL;
  pace_status st = pace_session_create(&cfg, &session);
  if (st != PACE_OK) {
    fprintf(stderr, "pace_session_create: %s\n", pace_status_string(st));
    return 1;
  }

  if (pace_trace_open(out_path, &g_trace) != PACE_OK) {
    fprintf(stderr, "cannot open %s for writing\n", out_path);
    return 1;
  }

  worker *workers = (worker *)calloc((size_t)nthreads, sizeof *workers);
  pthread_t *threads = (pthread_t *)calloc((size_t)nthreads, sizeof *threads);
  if (!workers || !threads) return 1;

  for (int i = 0; i < nthreads; i++) {
    workers[i].session = session;
    pthread_create(&threads[i], NULL, worker_main, &workers[i]);
  }

  struct timespec ts = {duration_ms / 1000, (long)(duration_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  g_stop = 1;

  for (int i = 0; i < nthreads; i++) pthread_join(threads[i], NULL);

  pace_trace_flush(g_trace);
  pace_trace_close(g_trace);
  pace_session_destroy(session);
  free(workers);
  free(threads);

  fprintf(stderr, "wrote %s\n", out_path);
  return 0;
}
