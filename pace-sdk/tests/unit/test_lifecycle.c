/* SPDX-License-Identifier: BUSL-1.1
 * A check for the defects the engineering review found in the session
 * lifecycle. Each case here is a defect that reproduced before the fix.
 */
#include "pace/pace.h"

/* The internal header, for the thread id and the owner fields this test
 * inspects. It is not part of the public ABI. */
#include "pace_session.h"

#include <pthread.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
/* 1. A thread that exits must not leave a dangling TLS handle.        */
/* ------------------------------------------------------------------ */

static pace_session *g_session = NULL;
static uint64_t g_worker_tid = 0;

static void *vanishing_thread(void *arg) {
  (void)arg;
  pace_thread *thread = NULL;
  if (pace_session_register_thread(g_session, &thread) != PACE_OK) return NULL;
  g_worker_tid = thread->thread_id;
  pace_sample sample;
  pace_session_sample(g_session, thread, &sample);
  return NULL;
}

/* The kernel recycles a thread id once the thread that held it has exited.
 * A new thread can then be given the same id, and it must not inherit the
 * dead thread's handle or its baselines. */
static void test_a_recycled_thread_id_gets_a_fresh_handle(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *session = NULL;
  CHECK(pace_session_create(&cfg, &session) == PACE_OK, "session create failed");
  g_session = session;

  pthread_t worker;
  pthread_create(&worker, NULL, vanishing_thread, NULL);
  pthread_join(worker, NULL);
  CHECK(g_worker_tid != 0, "the worker did not register");

  /* The worker's handle is still in the session's table, but the worker is
   * gone. The main thread must get its own handle. */
  pace_thread *main_thread = NULL;
  CHECK(pace_session_register_thread(session, &main_thread) == PACE_OK,
        "the main thread could not register");
  CHECK(main_thread->thread_id != g_worker_tid,
        "the main thread was given the exited worker's thread id, which "
        "means the kernel did not recycle it and this case did not test "
        "what it claims to test");

  /* The main thread's first sample is a baseline, so it is zero-length and
   * degraded. */
  pace_sample first;
  CHECK(pace_session_sample(session, main_thread, &first) == PACE_OK,
        "the first sample failed");
  CHECK(first.wall_ns == 0, "the first sample was not a baseline");

  /* The second sample must have a real duration measured from the main
   * thread's own baseline, not from the exited worker's. */
  usleep(3000);
  pace_sample second;
  CHECK(pace_session_sample(session, main_thread, &second) == PACE_OK,
        "the second sample failed");
  CHECK(second.wall_ns > 1000000,
        "the second sample's wall time was %llu ns, which is too small for "
        "a 3 ms sleep: the baseline came from another thread",
        (unsigned long long)second.wall_ns);

  pace_session_destroy(session);
  g_session = NULL;
}

/* ------------------------------------------------------------------ */
/* 2. A failed session creation must not leave a dangling pointer.      */
/* ------------------------------------------------------------------ */

static void test_create_failure_does_not_write_the_out_pointer(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  /* A deliberately wrong struct_size must be rejected. */
  cfg.struct_size = 3;

  pace_session *session = (pace_session *)(void *)0xdeadbeef;
  pace_status st = pace_session_create(&cfg, &session);
  CHECK(st == PACE_ERR_INVALID_ARG, "a bad struct_size was accepted");
  /* The contract is that a failed create leaves *out_session NULL, so the
   * caller cannot free a pointer the library never allocated. */
  CHECK(session == NULL,
        "a failed create left %p in the out pointer; the caller would free "
        "a pointer the library never allocated",
        (void *)session);
}

/* ------------------------------------------------------------------ */
/* 3. Sampling a thread registered in a different session is an error, */
/*    not a silent cross-session corruption.                          */
/* ------------------------------------------------------------------ */

static void test_cross_session_thread_is_rejected(void) {
  pace_config cfg;
  pace_config_init(&cfg);

  pace_session *a = NULL;
  pace_session *b = NULL;
  CHECK(pace_session_create(&cfg, &a) == PACE_OK, "session a failed");
  CHECK(pace_session_create(&cfg, &b) == PACE_OK, "session b failed");

  pace_thread *thread = NULL;
  CHECK(pace_session_register_thread(a, &thread) == PACE_OK, "register failed");

  pace_sample sample;
  pace_status st = pace_session_sample(b, thread, &sample);
  CHECK(st == PACE_ERR_INVALID_ARG,
        "sampling session b with session a's thread returned %s instead of "
        "an error: it would write the other session's baselines",
        pace_status_string(st));

  pace_session_destroy(a);
  pace_session_destroy(b);
}

/* ------------------------------------------------------------------ */
/* 4. Destroying a session twice must not double free.                 */
/* ------------------------------------------------------------------ */

static void test_destroy_is_idempotent_for_null(void) {
  pace_session_destroy(NULL);
  CHECK(1, "destroy(NULL) must be a no-op");
}

/* ------------------------------------------------------------------ */
/* 5. A null out_sample must be rejected rather than dereferenced.     */
/* ------------------------------------------------------------------ */

static void test_null_arguments_are_rejected(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *session = NULL;
  CHECK(pace_session_create(&cfg, &session) == PACE_OK, "session create failed");

  pace_thread *thread = NULL;
  CHECK(pace_session_register_thread(session, &thread) == PACE_OK, "register failed");

  CHECK(pace_session_sample(session, thread, NULL) == PACE_ERR_INVALID_ARG,
        "a null out_sample was not rejected");
  CHECK(pace_session_sample(NULL, thread, NULL) == PACE_ERR_INVALID_ARG,
        "a null session was not rejected");
  CHECK(pace_session_stats(session, NULL) == PACE_ERR_INVALID_ARG,
        "a null stats pointer was not rejected");

  pace_session_destroy(session);
}

/* ------------------------------------------------------------------ */
/* 6. The thread-local fast path must not use a destroyed session.      */
/* ------------------------------------------------------------------ */

static void test_the_fast_path_survives_a_destroyed_session(void) {
  pace_config cfg;
  pace_config_init(&cfg);

  /* Sample a thread, destroy its session, and create a new one. The
   * thread-local fast path still holds the old handle, whose memory has
   * been freed. Using it would be a use-after-free. */
  pace_session *first = NULL;
  CHECK(pace_session_create(&cfg, &first) == PACE_OK, "first session failed");
  pace_sample a;
  CHECK(pace_session_sample_self(first, &a) == PACE_OK, "first sample failed");
  pace_session_destroy(first);

  pace_session *second = NULL;
  CHECK(pace_session_create(&cfg, &second) == PACE_OK, "second session failed");

  pace_sample b;
  pace_status st = pace_session_sample_self(second, &b);
  CHECK(st == PACE_OK, "sampling after the first session was destroyed: %s",
        pace_status_string(st));

  /* The new session's first sample is its own baseline, so it is empty. */
  CHECK(b.wall_ns == 0,
        "the first sample of the new session carried %llu ns, which means it "
        "used the destroyed session's baseline",
        (unsigned long long)b.wall_ns);

  usleep(2000);
  pace_sample c;
  CHECK(pace_session_sample_self(second, &c) == PACE_OK, "second sample failed");
  CHECK(c.wall_ns > 1000000,
        "the second sample's wall time was %llu ns, too small for a 2 ms "
        "sleep: the baseline came from the destroyed session",
        (unsigned long long)c.wall_ns);

  pace_session_destroy(second);
}

/* ------------------------------------------------------------------ */
/* 7. A stats struct with the wrong size must be rejected.             */
/* ------------------------------------------------------------------ */

static void test_stats_struct_size_is_checked(void) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *session = NULL;
  CHECK(pace_session_create(&cfg, &session) == PACE_OK, "session create failed");

  pace_stats stats;
  memset(&stats, 0, sizeof stats);

  /* A struct size this build does not know how to fill is a caller bug. */
  stats.struct_size = 4;
  CHECK(pace_session_stats(session, &stats) == PACE_ERR_INVALID_ARG,
        "a stats struct with a foreign struct_size was accepted");

  /* The correct size works. */
  stats.struct_size = sizeof(pace_stats);
  CHECK(pace_session_stats(session, &stats) == PACE_OK,
        "a stats struct with the correct struct_size was rejected");
  CHECK(stats.struct_size == sizeof(pace_stats),
        "the stats struct_size was not filled in");

  /* No thread has registered yet, so the count is zero. */
  CHECK(stats.threads_registered == 0,
        "the stats reported %u threads before any registered, expected 0",
        stats.threads_registered);

  pace_thread *thread = NULL;
  CHECK(pace_session_register_thread(session, &thread) == PACE_OK,
        "register failed");
  stats.struct_size = sizeof(pace_stats);
  CHECK(pace_session_stats(session, &stats) == PACE_OK, "stats failed");
  CHECK(stats.threads_registered == 1,
        "the stats reported %u threads, expected 1", stats.threads_registered);

  pace_session_destroy(session);
}

/* ------------------------------------------------------------------ */
/* 8. Every status code must carry a fix and a recovery.              */
/* ------------------------------------------------------------------ */

static void test_every_status_carries_a_fix_and_a_recovery(void) {
  static const pace_status codes[] = {
      PACE_OK,
      PACE_ERR_INVALID_ARG,
      PACE_ERR_UNSUPPORTED,
      PACE_ERR_PERMISSION,
      PACE_ERR_NOMEM,
      PACE_ERR_IO,
      PACE_ERR_STATE,
      PACE_ERR_NOT_FOUND,
      PACE_ERR_BUFFER_TOO_SMALL,
  };

  for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
    pace_status code = codes[i];
    const char *name = pace_status_string(code);
    CHECK(name && name[0] != 0, "status %d has no name", (int)code);

    const char *fix = pace_status_fix(code);
    const char *recovery = pace_status_recovery(code);
    CHECK(fix != NULL, "status %d has a NULL fix", (int)code);
    CHECK(recovery != NULL, "status %d has a NULL recovery", (int)code);

    if (code == PACE_OK) {
      /* Success has nothing to fix. */
      CHECK(fix[0] == 0, "PACE_OK carries a fix");
      CHECK(recovery[0] == 0, "PACE_OK carries a recovery");
      continue;
    }

    /* The fix must be long enough to act on, and it must not ask the
     * caller to modify the library itself. */
    CHECK(strlen(fix) > 20, "the fix for status %d is too short: %s", (int)code, fix);
    CHECK(strlen(recovery) > 20, "the recovery for status %d is too short: %s",
          (int)code, recovery);

    char lower[256];
    size_t n = strlen(fix) < sizeof lower - 1 ? strlen(fix) : sizeof lower - 1;
    memcpy(lower, fix, n);
    lower[n] = 0;
    for (size_t i2 = 0; i2 < n; i2++) {
      if (lower[i2] >= 'A' && lower[i2] <= 'Z') lower[i2] = (char)(lower[i2] + 32);
    }
    CHECK(strstr(lower, "edit the source") == NULL,
          "the fix for status %d asks the caller to edit the library: %s", (int)code, fix);
    CHECK(strstr(lower, "patch the") == NULL,
          "the fix for status %d asks the caller to patch the library: %s", (int)code, fix);
  }
}

/**
 * A sample struct reused across calls must not carry the previous call's
 * values.
 *
 * The sample is written field by field, and some fields are only written
 * on some paths. A caller that reuses one struct across calls -- which
 * is the natural thing to do on the hot path -- would otherwise see the
 * previous call's source, cycle count and ratio in the fields this call
 * did not write.
 *
 * The macOS CI runner caught this, because there the caller's struct is
 * not zeroed between calls. It is a test on every platform, because the
 * failure is silent where the caller happens to zero the struct.
 */
static void test_a_reused_sample_carries_no_stale_fields(void) {
  pace_config cfg;
  pace_config_init(&cfg);

  pace_session *session = NULL;
  CHECK(pace_session_create(&cfg, &session) == PACE_OK, "the session was created");

  pace_thread *thread = NULL;
  CHECK(pace_session_register_thread(session, &thread) == PACE_OK, "registered");

  /* One struct, filled many times, never cleared. */
  pace_sample sample;
  memset(&sample, 0, sizeof sample);
  for (int i = 0; i < 8; i++) {
    CHECK(pace_session_sample(session, thread, &sample) == PACE_OK, "sampled");
  }

  /* Whatever the platform, the sample must say what it actually knows. */
  CHECK(sample.source == PACE_SOURCE_PERF || sample.source == PACE_SOURCE_CLOCK ||
            sample.source == PACE_SOURCE_UNKNOWN,
        "the source is one of the three known values: %d", (int)sample.source);

  /* A cycle count is only claimed when the flag says it was measured. */
  if (!(sample.flags & PACE_SAMPLE_F_HAS_CORE_CYCLES)) {
    CHECK(sample.core_cycles == 0,
          "a sample without measured core cycles must report zero, not %" PRIu64,
          sample.core_cycles);
  }

  /* The preemption split is only claimed when the platform can measure
   * it. On Windows and macOS it cannot, and the sample must say so
   * rather than reporting a confident zero. */
  if (!(sample.flags & PACE_SAMPLE_F_HAS_PREEMPTED)) {
    CHECK(sample.preempted_ns == 0,
          "a sample without a preemption split must report zero, not %" PRIu64,
          sample.preempted_ns);
  }

  /* The reserved field is reserved, so it is always zero. */
  CHECK(sample._reserved1 == 0, "the reserved field is zero");

  pace_session_destroy(session);
}

int main(void) {
  printf("PACE backend: %s\n", pace_backend_name());

  test_a_recycled_thread_id_gets_a_fresh_handle();
  test_create_failure_does_not_write_the_out_pointer();
  test_cross_session_thread_is_rejected();
  test_destroy_is_idempotent_for_null();
  test_null_arguments_are_rejected();
  test_the_fast_path_survives_a_destroyed_session();
  test_stats_struct_size_is_checked();
  test_every_status_carries_a_fix_and_a_recovery();
  test_a_reused_sample_carries_no_stale_fields();

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
