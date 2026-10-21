/* SPDX-License-Identifier: BUSL-1.1
 * Concurrency, under a sanitizer.
 *
 * The public header promises that pace_session_sample is safe to call
 * concurrently from different threads, each with its own pace_thread. That
 * promise has never been tested under a thread sanitizer, and a data race
 * in a profiler's hot path is the kind of bug that produces a wrong number
 * rather than a crash.
 *
 * Built with -fsanitize=thread this test is the check. Built without it,
 * it is still a stress test: many threads, many samples, and the
 * invariant checked on every one.
 */
#include "pace/pace.h"

#include "pace_backend.h"

#include <inttypes.h>
#include <pthread.h>
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

#define WORKERS 16
#define SAMPLES_PER_WORKER 4000

typedef struct {
  pace_session *session;
  pace_thread *thread;
  int violations;    /**< Samples that broke the decomposition invariant. */
  int errors;        /**< Samples that could not be taken. */
  uint64_t samples;  /**< Samples taken. */
  int registered;    /**< Whether registration succeeded. */
} worker;

/**
 * Register this thread and sample it repeatedly.
 *
 * The worker checks the invariant on every sample, because a race in the
 * differencing would show up as a broken sum long before it showed up as
 * a crash.
 */
static void *worker_main(void *arg) {
  worker *w = (worker *)arg;

  if (pace_session_register_thread(w->session, &w->thread) != PACE_OK) {
    w->registered = 0;
    return NULL;
  }
  w->registered = 1;

  pace_sample sample;
  /* The first sample establishes the baseline. */
  if (pace_session_sample(w->session, w->thread, &sample) != PACE_OK) {
    w->errors++;
    return NULL;
  }

  for (int i = 0; i < SAMPLES_PER_WORKER; i++) {
    if (pace_session_sample(w->session, w->thread, &sample) != PACE_OK) {
      w->errors++;
      continue;
    }
    w->samples++;

    uint64_t sum = sample.on_cpu_ns + sample.preempted_ns + sample.blocked_ns;
    if (sum != sample.wall_ns) {
      w->violations++;
      if (w->violations == 1) {
        printf("  first violation: wall=%" PRIu64 " on_cpu=%" PRIu64
               " preempted=%" PRIu64 " blocked=%" PRIu64 "\n",
               sample.wall_ns, sample.on_cpu_ns, sample.preempted_ns,
               sample.blocked_ns);
      }
    }
  }
  return NULL;
}

/**
 * Many threads sampling the same session at once.
 */
static void test_concurrent_sampling(void) {
  pace_config cfg;
  pace_config_init(&cfg);

  pace_session *session = NULL;
  CHECK(pace_session_create(&cfg, &session) == PACE_OK, "session created");

  worker workers[WORKERS];
  memset(workers, 0, sizeof workers);
  pthread_t threads[WORKERS];

  for (int i = 0; i < WORKERS; i++) {
    workers[i].session = session;
    CHECK(pthread_create(&threads[i], NULL, worker_main, &workers[i]) == 0,
          "worker %d created", i);
  }
  for (int i = 0; i < WORKERS; i++) pthread_join(threads[i], NULL);

  int registered = 0, violations = 0, errors = 0;
  uint64_t samples = 0;
  for (int i = 0; i < WORKERS; i++) {
    registered += workers[i].registered;
    violations += workers[i].violations;
    errors += workers[i].errors;
    samples += workers[i].samples;
  }

  CHECK(registered == WORKERS, "%d of %d workers registered", registered, WORKERS);
  CHECK(errors == 0, "%d samples could not be taken", errors);
  CHECK(violations == 0, "%d samples broke the decomposition invariant", violations);
  CHECK(samples == (uint64_t)WORKERS * SAMPLES_PER_WORKER,
        "took %" PRIu64 " samples, expected %d", samples,
        WORKERS * SAMPLES_PER_WORKER);

  pace_session_destroy(session);
}

/**
 * Many threads registering at once.
 *
 * Registration takes the session's lock, so this is the path where a
 * mistake in the lock handling would show.
 */
static void test_concurrent_registration(void) {
  pace_config cfg;
  pace_config_init(&cfg);

  pace_session *session = NULL;
  CHECK(pace_session_create(&cfg, &session) == PACE_OK, "session created");

  worker workers[WORKERS];
  memset(workers, 0, sizeof workers);
  pthread_t threads[WORKERS];

  for (int i = 0; i < WORKERS; i++) {
    workers[i].session = session;
    pthread_create(&threads[i], NULL, worker_main, &workers[i]);
  }
  for (int i = 0; i < WORKERS; i++) pthread_join(threads[i], NULL);

  int registered = 0;
  for (int i = 0; i < WORKERS; i++) registered += workers[i].registered;
  CHECK(registered == WORKERS, "%d of %d workers registered", registered, WORKERS);

  pace_stats stats;
  memset(&stats, 0, sizeof stats);
  stats.struct_size = sizeof stats;
  CHECK(pace_session_stats(session, &stats) == PACE_OK, "stats read");
  CHECK(stats.threads_registered == WORKERS,
        "the session counted %u threads, expected %d", stats.threads_registered,
        WORKERS);

  pace_session_destroy(session);
}

/**
 * A thread that exits while others keep sampling.
 *
 * The session's thread table is shared, and a departing thread's handle
 * has to be reclaimed without disturbing the others. This is the case
 * where a lock held too long, or a freed handle still referenced, would
 * show up.
 */
static void test_threads_leaving_while_others_sample(void) {
  pace_config cfg;
  pace_config_init(&cfg);

  pace_session *session = NULL;
  CHECK(pace_session_create(&cfg, &session) == PACE_OK, "session created");

  /* A wave of short-lived threads, each registering, sampling and
   * leaving, while a long-lived thread samples throughout. */
  pthread_t resident;
  worker resident_worker;
  memset(&resident_worker, 0, sizeof resident_worker);
  resident_worker.session = session;

  CHECK(pthread_create(&resident, NULL, worker_main, &resident_worker) == 0,
        "the resident worker was created");

  for (int wave = 0; wave < 8; wave++) {
    worker transient[8];
    memset(transient, 0, sizeof transient);
    pthread_t threads[8];
    for (int i = 0; i < 8; i++) {
      transient[i].session = session;
      pthread_create(&threads[i], NULL, worker_main, &transient[i]);
    }
    for (int i = 0; i < 8; i++) pthread_join(threads[i], NULL);
  }

  pthread_join(resident, NULL);

  CHECK(resident_worker.violations == 0,
        "the resident worker saw %d broken samples", resident_worker.violations);
  CHECK(resident_worker.errors == 0, "the resident worker had %d errors",
        resident_worker.errors);

  pace_session_destroy(session);
}

int main(void) {
  printf("PACE backend: %s\n", pace_backend_name());
#ifdef __SANITIZE_THREAD__
  printf("sanitizer: thread\n");
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
  printf("sanitizer: thread\n");
#  else
  printf("sanitizer: none\n");
#  endif
#else
  printf("sanitizer: none\n");
#endif

  test_concurrent_sampling();
  test_concurrent_registration();
  test_threads_leaving_while_others_sample();

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
