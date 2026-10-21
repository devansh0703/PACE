/* SPDX-License-Identifier: BUSL-1.1
 * PACE — record a real trace to a PACE JSONL file.
 *
 * This is the SDK's own trace writer, exercised against a real contended
 * workload, so the analyzer is fed a genuine trace rather than a fixture.
 *
 * Usage: pace_record <output.jsonl> [duration_ms] [threads]
 */
#include "pace/pace.h"

/* The internal backend header, for the session's own state and for the
 * portable thread id helper. */
#include "pace_backend.h"
#include "pace_session.h"
#include "pace_trace.h"
#if defined(PACE_WITH_BPF)
#include "bpf/pace_bpf.h"
#endif

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
  /** The thread's handle, for the SDK's resolved kernel id. */
  pace_thread *handle;
} worker;

static void *worker_main(void *arg) {
  worker *w = (worker *)arg;
  pace_thread *handle = NULL;
  if (pace_session_register_thread(w->session, &handle) != PACE_OK) return NULL;
  w->handle = handle;
  w->tid = pace_current_thread_id();
  /* The kernel's id is resolved lazily, once the thread has been
   * scheduled and the tier's map has an entry for it, so it is read at
   * the end of the run rather than here. */

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

#if defined(PACE_WITH_BPF)
  /* The tier's cumulative values are read BEFORE the workers are joined.
   *
   * A thread's /proc/<pid>/task/<tid> directory disappears when the
   * thread exits, so reading after the join finds nothing -- and the
   * report would say the tier measured nothing while the trace beside
   * it said otherwise. */
  {
    struct pace_bpf *bpf = (struct pace_bpf *)session->bpf;
    if (bpf && pace_bpf_is_available(bpf)) {
      uint64_t total = 0;
      int resolved = 0, read = 0;
      for (int i = 0; i < nthreads; i++) {
        uint64_t value = 0;
        /* The SDK's own resolution, which is verified against the map.
         * The demo does not resolve again: the two would disagree, and
         * the disagreement is what made the report unreadable. */
        uint64_t kernel_tid = pace_linux_thread_kernel_tid(workers[i].handle);
        if (kernel_tid != 0) resolved++;
        if (kernel_tid != 0 &&
            pace_bpf_read_preempted_ns(bpf, kernel_tid, &value) == PACE_OK) {
          if (value > 0) read++;
          total += value;
        }
      }
      fprintf(stderr,
              "  the tier's cumulative preemption for the workers: %" PRIu64
              " ns; the kernel's thread id was resolved for %d of %d, and %d had a "
              "value\n",
              total, resolved, nthreads, read);
    }
  }
#endif

  for (int i = 0; i < nthreads; i++) pthread_join(threads[i], NULL);

  pace_trace_flush(g_trace);
  pace_trace_close(g_trace);

  /* The tier's state is reported, so a run's log says which measurement
   * it carried. A tier that fell back to the procfs tier is a different
   * measurement from one that used the scheduler's own events, and the
   * difference is large on some kernels, so it is stated rather than
   * left to be inferred. */
  pace_stats stats;
  memset(&stats, 0, sizeof stats);
  stats.struct_size = sizeof stats;
  pace_session_stats(session, &stats);
  fprintf(stderr, "the eBPF tier is %s; %" PRIu64 " samples, %" PRIu64 " degraded\n",
          stats.ebpf_available ? "attached" : "not available", stats.samples_taken,
          stats.degraded_samples);



  pace_session_destroy(session);
  free(workers);
  free(threads);

  fprintf(stderr, "wrote %s\n", out_path);
  return 0;
}
