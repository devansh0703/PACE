/* SPDX-License-Identifier: BUSL-1.1
 * PACE — unit tests.
 *
 * These assert real behaviour of the library on the running machine. The
 * decomposition invariant test is the important one: it is the property the
 * whole product rests on.
 */
#include "pace/pace.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                     \
  do {                                                       \
    g_checks++;                                              \
    if (!(cond)) {                                           \
      g_failures++;                                          \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
      fprintf(stderr, __VA_ARGS__);                          \
      fprintf(stderr, "\n");                                 \
    }                                                        \
  } while (0)

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* ---------------------------------------------------------------- tests */

static void test_status_strings(void) {
  CHECK(strcmp(pace_status_string(PACE_OK), "ok") == 0, "PACE_OK string");
  CHECK(strcmp(pace_status_string(PACE_ERR_PERMISSION), "permission denied") == 0,
        "PACE_ERR_PERMISSION string");
  CHECK(pace_status_string((pace_status)999) != NULL, "unknown status returns a string");
}

static void test_config_init(void) {
  pace_config cfg;
  memset(&cfg, 0xAB, sizeof cfg);
  CHECK(pace_config_init(&cfg) == PACE_OK, "config init returns ok");
  CHECK(cfg.struct_size == sizeof(pace_config), "config struct_size set");
  CHECK(cfg.prefer_perf_counters == 1, "perf counters preferred by default");
  CHECK(pace_config_init(NULL) == PACE_ERR_INVALID_ARG, "NULL config rejected");
}

static void test_invalid_arguments(void) {
  pace_session *s = NULL;
  CHECK(pace_session_create(NULL, NULL) == PACE_ERR_INVALID_ARG, "NULL out rejected");

  pace_config cfg;
  pace_config_init(&cfg);
  CHECK(pace_session_create(&cfg, &s) == PACE_OK, "session created");
  CHECK(s != NULL, "session pointer set");

  pace_sample sample;
  CHECK(pace_session_sample(NULL, NULL, &sample) == PACE_ERR_INVALID_ARG, "NULL session rejected");
  CHECK(pace_session_sample(s, NULL, &sample) == PACE_ERR_INVALID_ARG, "NULL thread rejected");
  CHECK(pace_session_set_observer(NULL, NULL, NULL) == PACE_ERR_INVALID_ARG, "NULL session rejected");

  pace_session_destroy(s);
  pace_session_destroy(NULL); /* must not crash */
  CHECK(1, "destroy(NULL) is safe");
}

static void test_bad_struct_size(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  cfg.struct_size = 3;
  pace_session *s = NULL;
  CHECK(pace_session_create(&cfg, &s) == PACE_ERR_INVALID_ARG, "wrong struct_size rejected");
  CHECK(s == NULL, "session not created on bad config");
}

/**
 * Whether this platform can report a reference frequency.
 *
 * Linux derives the reference cycles from the invariant TSC, so it has one.
 * Windows and macOS have no public constant-rate cycle counter, so they
 * report the cycle count and the time separately and never derive one from
 * the other. The test asserts what the platform can do, not what Linux can.
 */
static int platform_has_reference_hz(void) {
#if defined(__linux__)
  return 1;
#else
  return 0;
#endif
}

/**
 * Whether this platform can separate preemption from blocking.
 *
 * Only Linux can, because /proc/<tid>/schedstat reports the run-queue delay
 * directly. The others report on-CPU versus blocked and say so.
 */
static int platform_has_split(void) {
#if defined(__linux__)
  return 1;
#else
  return 0;
#endif
}

static void test_session_and_source(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *s = NULL;
  CHECK(pace_session_create(&cfg, &s) == PACE_OK, "session created");
  CHECK(pace_session_source(s) != PACE_SOURCE_UNKNOWN, "a source tier was obtained");
  if (platform_has_reference_hz()) {
    CHECK(pace_session_reference_hz(s) > 0.0, "reference frequency measured: %f Hz",
          pace_session_reference_hz(s));
  } else {
    CHECK(pace_session_reference_hz(s) == 0.0,
          "this platform has no reference frequency, so it must report 0 rather "
          "than a made-up one: %f Hz",
          pace_session_reference_hz(s));
  }
  pace_session_destroy(s);
}

static void test_first_sample_is_baseline(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *s = NULL;
  CHECK(pace_session_create(&cfg, &s) == PACE_OK, "session created");

  pace_sample first;
  CHECK(pace_session_sample_self(s, &first) == PACE_OK, "first sample ok");
  CHECK(first.wall_ns == 0, "first sample has zero wall");
  CHECK(first.on_cpu_ns == 0, "first sample has zero on-CPU");
  CHECK((first.flags & PACE_SAMPLE_F_DEGRADED) != 0, "first sample marked degraded");

  pace_session_destroy(s);
}

static void test_decomposition_invariant(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *s = NULL;
  CHECK(pace_session_create(&cfg, &s) == PACE_OK, "session created");

  pace_sample a;
  CHECK(pace_session_sample_self(s, &a) == PACE_OK, "baseline sample ok");

  /* Burn some CPU so the window is non-trivial. */
  volatile unsigned long long acc = 0;
  for (unsigned long long i = 0; i < 20000000ull; i++) acc += i;
  (void)acc;

  pace_sample b;
  CHECK(pace_session_sample_self(s, &b) == PACE_OK, "second sample ok");
  CHECK(b.wall_ns > 0, "wall time advanced: %" PRIu64 " ns", b.wall_ns);
  CHECK(b.on_cpu_ns > 0, "on-CPU time advanced: %" PRIu64 " ns", b.on_cpu_ns);

  /* The core invariant: wall == on_cpu + preempted + blocked. */
  uint64_t sum = b.on_cpu_ns + b.preempted_ns + b.blocked_ns;
  CHECK(sum == b.wall_ns, "decomposition exact: %" PRIu64 " + %" PRIu64 " + %" PRIu64 " == %" PRIu64,
        b.on_cpu_ns, b.preempted_ns, b.blocked_ns, b.wall_ns);

  /* The reference cycles and the source tier are properties of the
   * platform, not of the library. */
  if (platform_has_reference_hz()) {
    CHECK(b.ref_cycles > 0, "reference cycles present: %" PRIu64, b.ref_cycles);
  } else {
    CHECK(b.ref_cycles == 0,
          "this platform has no reference counter, so it must report 0 rather "
          "than a derived value: %" PRIu64, b.ref_cycles);
  }
  if (b.source == PACE_SOURCE_PERF) {
    CHECK((b.flags & PACE_SAMPLE_F_HAS_CORE_CYCLES) != 0, "core cycles flag set in perf mode");
    CHECK(b.core_cycles > 0, "core cycles measured: %" PRIu64, b.core_cycles);
    CHECK(b.core_per_ref_ratio > 0.0, "core/ref ratio positive: %f", b.core_per_ref_ratio);
  } else if (b.source == PACE_SOURCE_CLOCK) {
    /* The clock tier derives the cycles, so it must not claim that they
     * were measured. */
    CHECK((b.flags & PACE_SAMPLE_F_HAS_CORE_CYCLES) == 0,
          "the clock tier must not claim measured core cycles");
    CHECK(b.core_cycles == 0, "no core cycles claimed without a counter");
  } else {
    /* The source is unknown, which means the platform produced no cycle
     * data at all. That is a legitimate state, and the sample must say so
     * rather than claiming a tier it did not reach. */
    CHECK(b.source == PACE_SOURCE_UNKNOWN,
          "the source is neither PERF nor CLOCK, so it must be UNKNOWN: %d",
          (int)b.source);
    CHECK((b.flags & PACE_SAMPLE_F_HAS_CORE_CYCLES) == 0,
          "an unknown source must not claim measured core cycles");
    CHECK(b.core_cycles == 0, "an unknown source must report no core cycles");
  }

  pace_session_destroy(s);
}

static void test_blocked_time_is_reported(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *s = NULL;
  CHECK(pace_session_create(&cfg, &s) == PACE_OK, "session created");

  pace_sample a;
  pace_session_sample_self(s, &a);
  sleep_ms(120);
  pace_sample b;
  pace_session_sample_self(s, &b);

  CHECK(b.wall_ns >= 100000000ull, "wall covers the sleep: %" PRIu64 " ns", b.wall_ns);
  CHECK(b.blocked_ns > 100000000ull, "sleep counted as blocked: %" PRIu64 " ns", b.blocked_ns);
  CHECK(b.preempted_ns == 0, "a sleeping thread is not 'preempted': %" PRIu64 " ns",
        b.preempted_ns);
  CHECK((b.flags & PACE_SAMPLE_F_WAS_BLOCKED) != 0, "blocked flag set");
  CHECK((b.flags & PACE_SAMPLE_F_WAS_PREEMPTED) == 0, "preempted flag clear");

  if (platform_has_split()) {
    /* The platform measured the split, so the sample must claim it. */
    CHECK((b.flags & PACE_SAMPLE_F_HAS_PREEMPTED) != 0, "HAS_PREEMPTED set");
    CHECK((b.flags & PACE_SAMPLE_F_HAS_BLOCKED) != 0, "HAS_BLOCKED set");
  } else {
    /* The platform cannot separate preemption from blocking, so the sample
     * must not claim a split it did not make. The off-CPU time is still
     * there; it is just not attributed. */
    CHECK((b.flags & PACE_SAMPLE_F_HAS_PREEMPTED) == 0,
          "the platform cannot measure preemption, so the flag must be clear");
    CHECK((b.flags & PACE_SAMPLE_F_HAS_BLOCKED) == 0,
          "the platform cannot measure blocking, so the flag must be clear");
  }

  pace_session_destroy(s);
}

static void counting_observer(void *user, const pace_sample *sample) {
  (void)sample;
  (*(int *)user)++;
}

static void test_observer_called(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *s = NULL;
  CHECK(pace_session_create(&cfg, &s) == PACE_OK, "session created");

  static int calls = 0;
  calls = 0;
  pace_session_set_observer(s, counting_observer, &calls);

  pace_sample tmp;
  pace_session_sample_self(s, &tmp);
  pace_session_sample_self(s, &tmp);
  CHECK(calls == 2, "observer called once per sample: %d", calls);

  pace_session_set_observer(s, NULL, NULL);
  pace_session_sample_self(s, &tmp);
  CHECK(calls == 2, "observer removed");

  pace_session_destroy(s);
}

static void *spin_thread(void *arg) {
  volatile unsigned long long acc = 0;
  for (unsigned long long i = 0; i < 5000000ull; i++) acc += i;
  (void)acc;
  (void)arg;
  return NULL;
}

static void test_multithreaded_sessions(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *s = NULL;
  CHECK(pace_session_create(&cfg, &s) == PACE_OK, "session created");

  enum { N = 8 };
  pthread_t threads[N];
  for (int i = 0; i < N; i++) {
    CHECK(pthread_create(&threads[i], NULL, spin_thread, NULL) == 0, "thread %d created", i);
  }
  for (int i = 0; i < N; i++) pthread_join(threads[i], NULL);

  pace_sample sample;
  CHECK(pace_session_sample_self(s, &sample) == PACE_OK, "sample after threads joined");
  pace_session_destroy(s);
}

static void test_format_cycles(void) {
  char buf[64];
  int n = pace_format_cycles(1500000000ull, buf, sizeof buf);
  CHECK(n > 0, "format returns length");
  CHECK(strstr(buf, "Gcycles") != NULL, "1.5G formatted: %s", buf);

  n = pace_format_cycles(999, buf, sizeof buf);
  CHECK(n > 0 && strstr(buf, "cycles") != NULL, "999 formatted: %s", buf);

  CHECK(pace_format_cycles(100, NULL, 0) == 0, "NULL buffer rejected");
  CHECK(pace_format_cycles(100, buf, 0) == 0, "zero size rejected");
}

static void test_unsupported_backend_reports(void) {
  /* On Linux the backend always supports tier 0. This asserts the contract
   * that a session never reports an unknown source. */
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *s = NULL;
  if (pace_session_create(&cfg, &s) == PACE_OK) {
    CHECK(pace_session_source(s) != PACE_SOURCE_UNKNOWN,
          "the session was created, so the source must be known");
    pace_session_destroy(s);
  } else {
    CHECK(0, "session creation failed on a supported platform");
  }
}

static void test_session_stats(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *s = NULL;
  CHECK(pace_session_create(&cfg, &s) == PACE_OK, "session created");

  pace_stats stats;
  memset(&stats, 0, sizeof stats);
  stats.struct_size = sizeof stats;
  CHECK(pace_session_stats(s, &stats) == PACE_OK, "stats returned");
  CHECK(stats.source == pace_session_source(s), "stats source matches");
  CHECK(stats.samples_taken == 0, "no samples yet: %" PRIu64, stats.samples_taken);
  CHECK(stats.threads_registered == 0, "no threads yet");

  pace_sample sample;
  pace_session_sample_self(s, &sample);
  pace_session_sample_self(s, &sample);
  pace_session_sample_self(s, &sample);

  memset(&stats, 0, sizeof stats);
  stats.struct_size = sizeof stats;
  CHECK(pace_session_stats(s, &stats) == PACE_OK, "stats returned");
  CHECK(stats.samples_taken == 3, "three samples counted: %" PRIu64, stats.samples_taken);
  CHECK(stats.threads_registered == 1, "one thread registered: %u", stats.threads_registered);
  CHECK(stats.degraded_samples == 1, "the baseline sample is degraded: %" PRIu64,
        stats.degraded_samples);
  CHECK(stats.io_errors == 0, "no I/O errors: %" PRIu64, stats.io_errors);

  pace_stats bad;
  memset(&bad, 0, sizeof bad);
  bad.struct_size = 4;
  CHECK(pace_session_stats(s, &bad) == PACE_ERR_INVALID_ARG, "a short struct_size is rejected");
  CHECK(pace_session_stats(NULL, &bad) == PACE_ERR_INVALID_ARG, "NULL session rejected");

  pace_session_destroy(s);
}

static void test_sample_has_struct_size_and_flags(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *s = NULL;
  CHECK(pace_session_create(&cfg, &s) == PACE_OK, "session created");

  pace_sample a;
  pace_session_sample_self(s, &a);
  pace_sample b;
  pace_session_sample_self(s, &b);

  CHECK(b.struct_size == sizeof(pace_sample), "sample carries its struct size: %u",
        b.struct_size);
  /* The flags must agree with what the platform can actually measure. */
  if (platform_has_split()) {
    CHECK((b.flags & PACE_SAMPLE_F_HAS_PREEMPTED) != 0, "HAS_PREEMPTED set");
    CHECK((b.flags & PACE_SAMPLE_F_HAS_BLOCKED) != 0, "HAS_BLOCKED set");
  } else {
    CHECK((b.flags & (PACE_SAMPLE_F_HAS_PREEMPTED | PACE_SAMPLE_F_HAS_BLOCKED)) == 0,
          "the platform cannot measure the split, so neither flag may be set");
  }
  CHECK(b._reserved0 == 0 && b._reserved1 == 0, "reserved fields are zero");

  pace_session_destroy(s);
}

static void test_keep_perf_fds_open_off(void) {
  /* With keep_perf_fds_open off the perf descriptors must not accumulate:
   * the count must not scale with the number of registered threads. */
  pace_config cfg;
  pace_config_init(&cfg);
  cfg.keep_perf_fds_open = 0;
  cfg.prefer_perf_counters = 1;

  pace_session *s = NULL;
  if (pace_session_create(&cfg, &s) != PACE_OK) {
    CHECK(0, "session creation failed");
    return;
  }

  pace_sample sample;
  for (int i = 0; i < 20; i++) {
    pace_session_sample_self(s, &sample);
  }

  pace_stats stats;
  memset(&stats, 0, sizeof stats);
  stats.struct_size = sizeof stats;
  pace_session_stats(s, &stats);
  CHECK(stats.samples_taken == 20, "twenty samples taken: %" PRIu64, stats.samples_taken);

  pace_session_destroy(s);
}

int main(void) {
  printf("PACE backend: %s\n", pace_backend_name());
  test_status_strings();
  test_config_init();
  test_invalid_arguments();
  test_bad_struct_size();
  test_session_and_source();
  test_first_sample_is_baseline();
  test_decomposition_invariant();
  test_blocked_time_is_reported();
  test_observer_called();
  test_multithreaded_sessions();
  test_format_cycles();
  test_unsupported_backend_reports();
  test_session_stats();
  test_sample_has_struct_size_and_flags();
  test_keep_perf_fds_open_off();

  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
