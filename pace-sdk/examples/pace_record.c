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
  /**
   * The worker's own slot in the totals below.
   *
   * The slot is assigned when the worker is created rather than found
   * by a search at sample time: with 48 threads searching for a free
   * slot together, two of them can find the same one and both write to
   * it, and the total comes out short.
   */
  int slot;
} worker;

/* The samples, counted by whether their preemption came from the
 * scheduler's own events or from the kernel's run-queue delay counter.
 *
 * The two are different measurements and they differ substantially on
 * some kernels, so the count says which one the trace carries. */
static volatile int g_samples_from_events = 0;
static volatile int g_samples_from_counter = 0;

/* The trace's preempted total per thread, so it can be compared against
 * the tier's own cumulative for the same thread. */
#define PACE_TRACKED_THREADS 256
static uint64_t g_trace_preempted[PACE_TRACKED_THREADS];
static uint64_t g_trace_wall[PACE_TRACKED_THREADS];
static uint64_t g_trace_on_cpu[PACE_TRACKED_THREADS];
static uint64_t g_trace_tids[PACE_TRACKED_THREADS];

/* The tier's cumulative, captured before the workers are joined because
 * a thread's map entry disappears when it exits. */
static uint64_t g_tier_total = 0;
static int g_tier_resolved = 0;
static int g_tier_read = 0;

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
    if (sample.flags & PACE_SAMPLE_F_PREEMPTED_FROM_EVENTS) {
      __atomic_fetch_add(&g_samples_from_events, 1, __ATOMIC_RELAXED);
    } else {
      __atomic_fetch_add(&g_samples_from_counter, 1, __ATOMIC_RELAXED);
    }
    pace_trace_write_sample(g_trace, w->tid, &sample);

    /* The trace's total for this thread, against the tier's own
     * cumulative for the same thread. The two must agree: the trace
     * holds the differences of the tier's counter, and the counter is
     * the same number. */
    int slot = w->slot;
    g_trace_tids[slot] = w->tid;
    g_trace_preempted[slot] += sample.preempted_ns;
    g_trace_wall[slot] += sample.wall_ns;
    g_trace_on_cpu[slot] += sample.on_cpu_ns;
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
    workers[i].slot = i;
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
      g_tier_total = total;
      g_tier_resolved = resolved;
      g_tier_read = read;
    }
  }
#endif

  for (int i = 0; i < nthreads; i++) pthread_join(threads[i], NULL);

#if defined(PACE_WITH_BPF)
  /* The report is printed AFTER the workers are joined, because the
   * workers' final samples are the ones that carry the bulk of the
   * preemption: a thread's last spin is spent waiting for the CPU, so
   * its last sample carries the whole wait.
   *
   * The tier's cumulative was captured before the join, because a
   * thread's map entry disappears when it exits. So the two are
   * compared across the join, and the comparison is between the whole
   * run's two measurements.
   *
   * Reading both at the same moment made the comparison a partial: on
   * a CI runner the partial was 9.1 s against the trace file's 42.7 s,
   * and the shortfall looked like a defect in the differencing. */
  {
    struct pace_bpf *bpf = (struct pace_bpf *)session->bpf;
    if (bpf && pace_bpf_is_available(bpf)) {
      fprintf(stderr,
              "  the tier's cumulative preemption for the workers: %" PRIu64
              " ns; the kernel's thread id was resolved for %d of %d, and %d had a "
              "value\n",
              g_tier_total, g_tier_resolved, nthreads, g_tier_read);

      /* The trace's total against the tier's total, for the same
       * threads. The two must agree: the trace holds the differences of
       * the tier's counter, so the sum of the differences is the
       * counter's total. Where they do not, the loss is in the
       * differencing, and the ratio says how much. */
      uint64_t trace_total = 0;
      for (int i = 0; i < PACE_TRACKED_THREADS; i++) {
        if (g_trace_tids[i] == 0) continue;
        trace_total += g_trace_preempted[i];
      }
      fprintf(stderr,
              "  the trace's preempted total: %" PRIu64 " ns against the tier's %" PRIu64
              " ns\n",
              trace_total, g_tier_total);

      /* The on-CPU time against the wall time.
       *
       * The ratio is the fraction of the wall time the threads spent
       * running. It is also the test of the two clocks: the on-CPU time
       * comes from the thread CPU clock and the wall time from the
       * monotonic clock, and if they disagree the off-CPU time is
       * misstated -- and the preemption, which is clamped to the
       * off-CPU time, is clamped down with it.
       *
       * With W workers on C CPUs the ratio is C/W, so 48 workers on 4
       * CPUs gives 0.083 and 48 on 16 gives 0.33. */
      uint64_t wall_total = 0, on_cpu_total = 0;
      for (int i = 0; i < PACE_TRACKED_THREADS; i++) {
        if (g_trace_tids[i] == 0) continue;
        wall_total += g_trace_wall[i];
        on_cpu_total += g_trace_on_cpu[i];
      }
      if (wall_total > 0) {
        fprintf(stderr,
                "  the on-CPU time is %.3f of the wall time (%" PRIu64 " ns of %" PRIu64
                " ns)\n",
                (double)on_cpu_total / (double)wall_total, on_cpu_total, wall_total);
      }
    }
  }
#endif

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
  fprintf(stderr,
          "  the samples' preemption came from the scheduler's own events in %d of "
          "them, and from the kernel's run-queue delay counter in %d\n",
          g_samples_from_events, g_samples_from_counter);



  pace_session_destroy(session);
  free(workers);
  free(threads);

  fprintf(stderr, "wrote %s\n", out_path);
  return 0;
}
