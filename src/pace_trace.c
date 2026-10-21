/* SPDX-License-Identifier: BUSL-1.1
 * PACE — JSONL trace writer.
 *
 * The trace format is one JSON object per line. It is deliberately a subset
 * of the Chrome Trace Event format so existing viewers can read it, with the
 * PACE-specific attribution fields carried in the "args" object.
 */
#include "pace_session.h"
#include "pace_trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct pace_trace {
  FILE *out;
  int owns_file;
  uint64_t start_wall_ns;
  uint64_t elapsed_ns; /**< Accumulated trace time, used as the event timestamp. */
};

pace_status pace_trace_open(const char *path, pace_trace **out_trace) {
  if (!out_trace) return PACE_ERR_INVALID_ARG;
  *out_trace = NULL;
  if (!path) return PACE_ERR_INVALID_ARG;

  FILE *f = fopen(path, "w");
  if (!f) return PACE_ERR_IO;

  pace_trace *t = (pace_trace *)calloc(1, sizeof *t);
  if (!t) {
    fclose(f);
    return PACE_ERR_NOMEM;
  }
  t->out = f;
  t->owns_file = 1;
  t->start_wall_ns = pace_now_monotonic_ns();
  *out_trace = t;
  return PACE_OK;
}

void pace_trace_close(pace_trace *trace) {
  if (!trace) return;
  if (trace->out) fclose(trace->out);
  free(trace);
}

/**
 * Write one sample as a Chrome Trace Event "X" (complete) event.
 *
 * The event's "ts" is the accumulated trace time in microseconds and "dur" is
 * the sample's wall duration in microseconds, so a viewer draws the sample as
 * a bar on the thread's track. The attribution breakdown is in "args".
 */
pace_status pace_trace_write_sample(pace_trace *trace, uint64_t thread_id, const pace_sample *s) {
  if (!trace || !s) return PACE_ERR_INVALID_ARG;

  double start_us = (double)trace->elapsed_ns / 1000.0;
  double dur_us = (double)s->wall_ns / 1000.0;
  trace->elapsed_ns += s->wall_ns;

  int n = fprintf(
      trace->out,
      "{\"name\":\"pace.sample\",\"ph\":\"X\",\"pid\":1,\"tid\":%" PRIu64
      ",\"ts\":%.3f,\"dur\":%.3f,\"cat\":\"pace\",\"args\":{"
      "\"wall_ns\":%" PRIu64 ",\"on_cpu_ns\":%" PRIu64 ",\"preempted_ns\":%" PRIu64
      ",\"blocked_ns\":%" PRIu64 ",\"core_cycles\":%" PRIu64 ",\"ref_cycles\":%" PRIu64
      ",\"core_per_ref_ratio\":%.6f"
      ",\"source\":\"%s\",\"involuntary_switches\":%" PRIu64
      ",\"voluntary_switches\":%" PRIu64 ",\"flags\":%u}}\n",
      thread_id, start_us, dur_us, s->wall_ns, s->on_cpu_ns, s->preempted_ns, s->blocked_ns,
      s->core_cycles, s->ref_cycles, s->core_per_ref_ratio,
      s->source == PACE_SOURCE_PERF ? "perf" : (s->source == PACE_SOURCE_CLOCK ? "clock" : "unknown"),
      s->involuntary_switches, s->voluntary_switches, s->flags);

  if (n < 0) return PACE_ERR_IO;
  return PACE_OK;
}

int pace_trace_flush(pace_trace *trace) {
  if (!trace || !trace->out) return -1;
  return fflush(trace->out);
}
