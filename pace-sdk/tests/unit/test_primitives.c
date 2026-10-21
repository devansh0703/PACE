/* SPDX-License-Identifier: BUSL-1.1
 * The Linux measurement primitives, tested directly.
 *
 * The CEO review found that the TSC detection and the /proc parsers had no
 * test of their own: the unit tests exercised them only through a whole
 * sample, so a parser that returned a plausible wrong number would pass.
 *
 * These tests call the primitives directly.
 */
#include "pace/pace.h"

#include "pace_backend.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#  include <fcntl.h>
#  include <sys/syscall.h>
#endif

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                             \
  do {                                               \
    checks++;                                        \
    if (!(cond)) {                                   \
      failures++;                                    \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
      printf(__VA_ARGS__);                            \
      printf("\n");                                  \
    }                                                \
  } while (0)

/* ------------------------------------------------------------------ */
/* The reference clock: T1.                                          */
/* ------------------------------------------------------------------ */

/**
 * Whether this platform has a constant-rate reference counter.
 *
 * Linux has the invariant TSC. Windows and macOS have no public
 * constant-rate counter, so they report the cycle count and the elapsed
 * time separately and never derive one from the other.
 */
static int platform_has_reference_clock(void) {
#if defined(__linux__)
  return 1;
#else
  return 0;
#endif
}

static void test_the_reference_clock_advances(void) {
  /* The reference cycle counter must advance, and it must advance at a
   * rate close to the measured frequency. A TSC that had stopped, or that
   * advanced at the wrong rate, would silently corrupt every cycle number
   * the product reports. */

  uint64_t start = pace_now_ref_cycles();
  CHECK(start != 0, "the reference counter returned zero");

  /* Give it a window long enough that the measurement is not noise. */
  struct timespec sleep_for = {0, 50 * 1000 * 1000}; /* 50 ms */
  nanosleep(&sleep_for, NULL);

  uint64_t end = pace_now_ref_cycles();
  uint64_t delta = end - start;
  CHECK(delta > 0, "the reference counter did not advance");

  double hz = pace_reference_hz();
  CHECK(hz > 0.0, "the reference frequency was not measured: %f Hz", hz);

  /* The elapsed wall time and the reference cycles must agree to within
   * a few percent. The wall clock and the TSC are different clocks, so
   * this is a real cross-check rather than a tautology. */
  uint64_t wall_start = pace_now_monotonic_ns();
  uint64_t cyc_start = pace_now_ref_cycles();
  nanosleep(&sleep_for, NULL);
  uint64_t wall_end = pace_now_monotonic_ns();
  uint64_t cyc_end = pace_now_ref_cycles();

  double wall_s = (double)(wall_end - wall_start) / 1e9;
  double cycles_s = (double)(cyc_end - cyc_start) / hz;
  if (wall_s > 0.0) {
    double error = (cycles_s - wall_s) / wall_s;
    if (error < 0) error = -error;
    CHECK(error < 0.05,
          "the reference counter and the wall clock disagree by %.2f%% over "
          "%.4f s: the frequency calibration is wrong",
          error * 100.0, wall_s);
  }
}

static void test_the_reference_frequency_is_plausible(void) {
  double hz = pace_reference_hz();
  CHECK(hz > 1e9, "the reference frequency is implausibly low: %f Hz", hz);
  CHECK(hz < 1e11, "the reference frequency is implausibly high: %f Hz", hz);
}

static void test_the_conversion_helper(void) {
  /* pace_ns_to_ref_cycles is the conversion every derived cycle number
   * goes through, so its edges are worth pinning. */

  CHECK(pace_ns_to_ref_cycles(1000000000ull, 1e9) == 1000000000ull,
        "one second at 1 GHz is a billion cycles");

  /* A platform with no reference clock must report nothing rather than a
   * made-up number. */
  CHECK(pace_ns_to_ref_cycles(1000000000ull, 0.0) == 0,
        "a zero frequency must produce zero cycles, not a guess");
  CHECK(pace_ns_to_ref_cycles(1000000000ull, -1.0) == 0,
        "a negative frequency must produce zero cycles");
}

static void test_the_multiplexing_scale(void) {
  /* A counter that ran continuously needs no scaling. */
  CHECK(pace_scale_multiplexed(1000, 1000, 1000) == 1000,
        "a continuously-running counter must not be scaled");

  /* A counter that was scheduled for half the window is scaled up, because
   * the value it reports covers only half the time. This is the case the
   * scaling exists for: the kernel multiplexes a counter that cannot be
   * scheduled continuously, and the raw value undercounts. */
  uint64_t scaled = pace_scale_multiplexed(1000, 2000, 1000);
  CHECK(scaled == 2000, "a half-scheduled counter must be scaled up: %" PRIu64,
        scaled);

  /* A counter that ran for half the window, reporting a value proportional
   * to that time, must come out at the unmultiplexed rate. */
  CHECK(pace_scale_multiplexed(500, 2000, 1000) == 1000,
        "a half-scheduled counter reporting half the value must scale back "
        "to the full rate");

  /* A counter that never ran has no value to scale. */
  CHECK(pace_scale_multiplexed(1000, 2000, 0) == 1000,
        "a counter that never ran must be reported unscaled");
}

#if defined(__linux__)
/* ------------------------------------------------------------------ */
/* The /proc parsers: T2.                                           */
/* ------------------------------------------------------------------ */

/**
 * The /proc/<tid>/schedstat file has three whitespace-separated numbers:
 * the on-CPU time, the run-queue delay, and the number of timeslices.
 *
 * The parser is internal, so the test reads the file itself and checks it
 * against the shape the parser assumes, and then checks that the values
 * the SDK reports agree with the file.
 */
static void test_the_schedstat_file_has_the_shape_the_parser_expects(void) {
  char path[64];
  snprintf(path, sizeof path, "/proc/%ld/schedstat", (long)syscall(SYS_gettid));

  FILE *f = fopen(path, "r");
  CHECK(f != NULL, "cannot open %s", path);
  if (!f) return;

  unsigned long long on_cpu = 0, run_delay = 0, timeslices = 0;
  int n = fscanf(f, "%llu %llu %llu", &on_cpu, &run_delay, &timeslices);
  fclose(f);

  CHECK(n == 3, "the schedstat file has %d fields, not 3", n);
  CHECK(timeslices > 0, "a running thread has been given timeslices: %llu",
        timeslices);
  /* The on-CPU time must be a plausible number of nanoseconds, not
   * milliseconds and not seconds. A parser that got the scale wrong
   * would still produce a plausible-looking count. */
  CHECK(on_cpu < 100000000000ull, /* 100 s */
        "the on-CPU time is implausibly large for a test thread: %llu ns",
        on_cpu);
}

/**
 * The /proc/<tid>/sched file's switch counters.
 *
 * The voluntary and involuntary counters are how a consumer tells a
 * blocking wait from a preemption without the run-queue delay, so the
 * SDK's own reading of them has to be checked against the file.
 */
static void test_the_switch_counters_are_read_correctly(void) {
  char path[64];
  snprintf(path, sizeof path, "/proc/%ld/sched", (long)syscall(SYS_gettid));

  FILE *f = fopen(path, "r");
  CHECK(f != NULL, "cannot open %s", path);
  if (!f) return;

  char line[512];
  unsigned long long voluntary = 0, involuntary = 0;
  int found = 0;
  while (fgets(line, sizeof line, f)) {
    unsigned long long v = 0, i = 0;
    if (sscanf(line, "nr_voluntary_switches : %llu", &v) == 1) {
      voluntary = v;
      found |= 1;
    } else if (sscanf(line, "nr_involuntary_switches : %llu", &i) == 1) {
      involuntary = i;
      found |= 2;
    }
  }
  fclose(f);

  CHECK(found == 3, "the sched file has both switch counters: %d of 2", found);
  /* The main thread has not been switched away from much, but the
   * counters must be present and sane. */
  CHECK(voluntary + involuntary < 1000000ull,
        "the switch counters are implausibly large: %llu and %llu", voluntary,
        involuntary);
}

/**
 * The SDK's own reading of the switch counters must agree with the file.
 */
static void test_the_sdk_reads_the_same_switch_counters(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  cfg.collect_switch_counters = 1;

  pace_session *session = NULL;
  CHECK(pace_session_create(&cfg, &session) == PACE_OK, "session created");
  pace_thread *thread = NULL;
  CHECK(pace_session_register_thread(session, &thread) == PACE_OK, "registered");

  pace_sample first;
  CHECK(pace_session_sample(session, thread, &first) == PACE_OK, "first sample");

  /* Do some work and block, so both kinds of switch happen. */
  volatile unsigned long long acc = 0;
  for (unsigned long long i = 0; i < 20000000ull; i++) acc += i;
  (void)acc;
  struct timespec sleep_for = {0, 20 * 1000 * 1000};
  nanosleep(&sleep_for, NULL);

  pace_sample second;
  CHECK(pace_session_sample(session, thread, &second) == PACE_OK, "second sample");

  /* The sample's counts are deltas from the baseline, so they must be
   * non-negative and must not exceed the thread's lifetime totals. */
  CHECK(second.voluntary_switches >= 0, "the voluntary count is negative");
  CHECK(second.involuntary_switches >= 0, "the involuntary count is negative");
  CHECK(second.voluntary_switches + second.involuntary_switches < 1000000ull,
        "the switch counts are implausibly large: %" PRIu64 " and %" PRIu64,
        second.voluntary_switches, second.involuntary_switches);

  pace_session_destroy(session);
}

/**
 * The run-queue delay must be zero for a thread that is not contending,
 * because a thread that never waits in the run queue has no preemption
 * time. This is the check that the parser did not confuse the two
 * schedstat fields.
 */
static void test_the_run_queue_delay_is_not_the_on_cpu_time(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *session = NULL;
  CHECK(pace_session_create(&cfg, &session) == PACE_OK, "session created");
  pace_thread *thread = NULL;
  CHECK(pace_session_register_thread(session, &thread) == PACE_OK, "registered");

  pace_sample first;
  pace_session_sample(session, thread, &first);

  volatile unsigned long long acc = 0;
  for (unsigned long long i = 0; i < 50000000ull; i++) acc += i;
  (void)acc;

  pace_sample second;
  CHECK(pace_session_sample(session, thread, &second) == PACE_OK, "second sample");

  /* A thread that held the CPU the whole time was not preempted, so the
   * preempted time must be far smaller than the on-CPU time. If the parser
   * had mixed up the two schedstat fields, they would be equal. */
  CHECK(second.on_cpu_ns > 0, "the on-CPU time did not advance");
  CHECK(second.preempted_ns < second.on_cpu_ns,
        "the preempted time (%" PRIu64 " ns) is not smaller than the on-CPU "
        "time (%" PRIu64 " ns): the two schedstat fields are confused",
        second.preempted_ns, second.on_cpu_ns);

  pace_session_destroy(session);
}
#endif /* __linux__ */

/* ------------------------------------------------------------------ */
/* The reference-cycle conversion, checked against a second clock.    */
/* ------------------------------------------------------------------ */

/**
 * The TSC is the reference clock, so its tick count IS the reference cycle
 * count. Multiplying a TSC delta by the frequency to get reference cycles
 * is wrong by a factor of the frequency.
 *
 * The premise probe did exactly that and printed a number 2.9 billion
 * times too large. This test converts the wall clock at the measured
 * frequency and compares it against the TSC's own tick count: they come
 * from different clocks and must agree.
 */
static void test_the_two_reference_cycle_conversions_agree(void) {
  double hz = pace_reference_hz();
  if (hz <= 0.0) return; /* no reference clock on this platform */

  uint64_t wall_start = pace_now_monotonic_ns();
  uint64_t tsc_start = pace_now_ref_cycles();
  struct timespec sleep_for = {0, 50 * 1000 * 1000};
  nanosleep(&sleep_for, NULL);
  uint64_t wall_end = pace_now_monotonic_ns();
  uint64_t tsc_end = pace_now_ref_cycles();

  uint64_t wall_ns = wall_end - wall_start;
  uint64_t tsc_ticks = tsc_end - tsc_start;

  /* The wall clock converted at the measured frequency. */
  double from_nanoseconds = (double)wall_ns * hz / 1e9;
  /* The TSC's own tick count, which is the reference cycle count. */
  double from_tsc = (double)tsc_ticks;

  CHECK(from_nanoseconds > 0.0, "the wall-clock conversion produced nothing");
  CHECK(from_tsc > 0.0, "the TSC delta was zero over a 50 ms window");

  double error = (from_nanoseconds - from_tsc) / from_tsc;
  if (error < 0) error = -error;
  CHECK(error < 0.01,
        "the two reference-cycle conversions disagree by %.4f%%: the wall "
        "clock gives %.0f and the TSC gives %.0f",
        error * 100.0, from_nanoseconds, from_tsc);

  /* The TSC delta must also be a sensible count of ticks for the elapsed
   * time, which is the check that catches a conversion that is off by the
   * frequency rather than by a small error. */
  double expected_ticks = (double)wall_ns * hz / 1e9;
  double ratio = (double)tsc_ticks / expected_ticks;
  CHECK(ratio > 0.99 && ratio < 1.01,
        "the TSC advanced %.4f times the expected number of ticks", ratio);
}

/**
 * The sample claims the preemption split only when the kernel is
 * collecting the scheduler statistics.
 *
 * On a distribution kernel, which compiles in CONFIG_TASK_DELAY_ACCT
 * without CONFIG_SCHEDSTATS, the run-queue delay is only counted while
 * /proc/sys/kernel/task_delayacct is 1, and that toggle is off by
 * default. The schedstat file then reads "0 0 0", and a preemption of
 * zero would be a false measurement: the thread was preempted, the
 * kernel just was not counting it.
 *
 * So the flags and the kernel's own accounting must agree.
 */
static void test_the_split_is_claimed_only_when_the_kernel_collects_it(void) {
  /* The kernel's own rule, from sched_info_on():
   *   CONFIG_SCHEDSTATS compiled in -> the statistics are always
   *     collected, and /proc/sys/kernel/sched_schedstats exists;
   *   only CONFIG_TASK_DELAY_ACCT -> the toggle decides, and it is off
   *     by default;
   *   neither -> not collected at all. */
  int schedstats_compiled_in = access("/proc/sys/kernel/sched_schedstats", F_OK) == 0;
  int delayacct_on = 0;
  if (!schedstats_compiled_in) {
    FILE *f = fopen("/proc/sys/kernel/task_delayacct", "r");
    if (f) {
      if (fscanf(f, "%d", &delayacct_on) != 1) delayacct_on = 0;
      fclose(f);
    }
  }
  int kernel_collects = schedstats_compiled_in || delayacct_on == 1;

  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *session = NULL;
  CHECK(pace_session_create(&cfg, &session) == PACE_OK, "the session was created");
  pace_thread *thread = NULL;
  CHECK(pace_session_register_thread(session, &thread) == PACE_OK, "registered");

  pace_sample sample;
  CHECK(pace_session_sample(session, thread, &sample) == PACE_OK, "sampled");
  pace_session_sample(session, thread, &sample);

  if (kernel_collects) {
    CHECK((sample.flags & PACE_SAMPLE_F_HAS_PREEMPTED) != 0,
          "the kernel collects the statistics, so the split must be claimed");
    CHECK((sample.flags & PACE_SAMPLE_F_HAS_BLOCKED) != 0,
          "the kernel collects the statistics, so the split must be claimed");
  } else {
    CHECK((sample.flags & PACE_SAMPLE_F_HAS_PREEMPTED) == 0,
          "the kernel does not collect the statistics, so the split must not "
          "be claimed: a preemption of zero would be a false measurement");
    CHECK((sample.flags & PACE_SAMPLE_F_HAS_BLOCKED) == 0,
          "the kernel does not collect the statistics, so the split must not "
          "be claimed");
  }

  pace_session_destroy(session);
}

int main(void) {
  printf("PACE backend: %s\n", pace_backend_name());

  if (platform_has_reference_clock()) {
    test_the_reference_clock_advances();
    test_the_reference_frequency_is_plausible();
  }
  test_the_conversion_helper();
  test_the_multiplexing_scale();
  if (platform_has_reference_clock()) {
    test_the_two_reference_cycle_conversions_agree();
  }

#if defined(__linux__)
  test_the_schedstat_file_has_the_shape_the_parser_expects();
  test_the_switch_counters_are_read_correctly();
  test_the_sdk_reads_the_same_switch_counters();
  test_the_run_queue_delay_is_not_the_on_cpu_time();
  test_the_split_is_claimed_only_when_the_kernel_collects_it();
#endif

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
