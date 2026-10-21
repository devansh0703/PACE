/* SPDX-License-Identifier: BUSL-1.1
 * PACE — measure the per-sample overhead of the library.
 *
 * Prints the median and p99 of a single pace_session_sample() call, which is
 * the number a vendor embedding PACE needs. Run it pinned to an isolated
 * core for a stable figure.
 */
#include "pace/pace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int compare_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

int main(int argc, char **argv) {
  int iterations = argc > 1 ? atoi(argv[1]) : 10000;

  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *session = NULL;
  if (pace_session_create(&cfg, &session) != PACE_OK) {
    fprintf(stderr, "pace_session_create failed\n");
    return 1;
  }

  pace_sample sample;
  pace_session_sample_self(session, &sample); /* warm up + baseline */

  uint64_t *deltas = (uint64_t *)malloc((size_t)iterations * sizeof(uint64_t));
  if (!deltas) return 1;

  for (int i = 0; i < iterations; i++) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pace_session_sample_self(session, &sample);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    deltas[i] = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                (uint64_t)(t1.tv_nsec - t0.tv_nsec);
  }

  qsort(deltas, (size_t)iterations, sizeof(uint64_t), compare_u64);
  uint64_t median = deltas[iterations / 2];
  uint64_t p99 = deltas[(int)((double)iterations * 0.99)];
  uint64_t min = deltas[0];
  uint64_t max = deltas[iterations - 1];

  printf("pace_session_sample overhead over %d calls\n", iterations);
  printf("  tier      : %s\n",
         pace_session_source(session) == PACE_SOURCE_PERF ? "perf (measured core cycles)"
                                                         : "clock (derived cycles)");
  printf("  min       : %" PRIu64 " ns\n", min);
  printf("  median    : %" PRIu64 " ns (%.2f us)\n", median, median / 1000.0);
  printf("  p99       : %" PRIu64 " ns (%.2f us)\n", p99, p99 / 1000.0);
  printf("  max       : %" PRIu64 " ns\n", max);

  free(deltas);
  pace_session_destroy(session);
  return 0;
}
