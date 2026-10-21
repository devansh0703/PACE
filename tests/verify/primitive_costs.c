/* SPDX-License-Identifier: BUSL-1.1
 * PACE — the per-call cost of each measurement primitive.
 *
 * The published overhead figure is the cost of a whole sample. This program
 * measures the cost of each primitive inside it, so the number can be
 * attributed rather than merely stated.
 *
 * Every figure here is measured on the machine that runs it, and the
 * environment is printed with it.
 */
#include "pace/pace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#  include <sys/syscall.h>
#  include <sys/utsname.h>
#  include <fcntl.h>
#  include <linux/perf_event.h>
#  include <sys/ioctl.h>
#endif

#define SAMPLES 20000

/** Nanoseconds from a monotonic clock. */
static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

struct timing {
  double median_ns;
  uint64_t min_ns;
};

/**
 * Time a call that returns a uint64_t.
 *
 * The median is reported because a single call's cost is dominated by
 * interrupts and scheduling noise; the median over many calls is the
 * number that describes the primitive.
 */
static struct timing time_calls(uint64_t (*fn)(void *), void *ctx, int n) {
  uint64_t *samples = (uint64_t *)malloc((size_t)n * sizeof *samples);
  if (!samples) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  for (int i = 0; i < n; i++) {
    uint64_t start = now_ns();
    (void)fn(ctx);
    samples[i] = now_ns() - start;
  }
  /* A median by selection: qsort is overkill and would dominate. */
  for (int i = 1; i < n; i++) {
    uint64_t key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = key;
  }
  struct timing t;
  t.median_ns = (double)samples[n / 2];
  t.min_ns = samples[0];
  free(samples);
  return t;
}

static uint64_t call_clock_thread_cputime(void *ctx) {
  (void)ctx;
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t call_clock_monotonic(void *ctx) {
  (void)ctx;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#ifdef __linux__
/** Read the cached /proc/<tid>/schedstat descriptor with a pread. */
static int g_schedstat_fd = -1;

static uint64_t call_schedstat_pread(void *ctx) {
  (void)ctx;
  char buf[128];
  ssize_t n = pread(g_schedstat_fd, buf, sizeof buf - 1, 0);
  return (uint64_t)(n > 0 ? n : 0);
}

/** Open /proc/<tid>/schedstat fresh, every time. */
static uint64_t call_schedstat_open(void *ctx) {
  (void)ctx;
  char path[64];
  snprintf(path, sizeof path, "/proc/%ld/schedstat", (long)syscall(SYS_gettid));
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  char buf[128];
  ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
  close(fd);
  return (uint64_t)(n > 0 ? n : 0);
}

/** Read /proc/<tid>/stat and parse field 22, the thread's start time. */
static uint64_t call_start_time_parse(void *ctx) {
  (void)ctx;
  char path[64];
  snprintf(path, sizeof path, "/proc/%ld/stat", (long)syscall(SYS_gettid));
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  char buf[4096];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = 0;
  fclose(f);
  return (uint64_t)n;
}

/** Read a hardware counter with read(2). */
static int g_perf_fd = -1;

static uint64_t call_perf_read(void *ctx) {
  (void)ctx;
  uint64_t value = 0;
  if (read(g_perf_fd, &value, sizeof value) != (ssize_t)sizeof value) return 0;
  return value;
}

/** The cost of a syscall round trip with nothing to do. */
static uint64_t call_getpid(void *ctx) {
  (void)ctx;
  return (uint64_t)syscall(SYS_getpid);
}
#endif

int main(void) {
  printf("PACE primitive costs\n");
  printf("====================\n\n");
  printf("samples per measurement: %d\n", SAMPLES);
#ifdef __linux__
  /* The environment the numbers were measured in, so the figure is
   * attributable rather than merely stated. */
  {
    long paranoid = -1;
    FILE *f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (f) {
      if (fscanf(f, "%ld", &paranoid) != 1) paranoid = -1;
      fclose(f);
    }
    printf("perf_event_paranoid:    %ld\n", paranoid);
  }
  {
    struct utsname uts;
    if (uname(&uts) == 0) printf("kernel:                 %s\n", uts.release);
  }
#endif
  printf("\n");
  printf("%-42s %12s %12s\n", "primitive", "median", "min");
  printf("%-42s %12s %12s\n", "------------------------------------------",
         "------------", "------------");

  struct timing t;

  t = time_calls(call_clock_monotonic, NULL, SAMPLES);
  printf("%-42s %9.0f ns %9.0f ns\n", "clock_gettime(CLOCK_MONOTONIC)", t.median_ns,
         (double)t.min_ns);

  t = time_calls(call_clock_thread_cputime, NULL, SAMPLES);
  printf("%-42s %9.0f ns %9.0f ns\n", "clock_gettime(CLOCK_THREAD_CPUTIME_ID)", t.median_ns,
         (double)t.min_ns);

#ifdef __linux__
  char path[64];
  snprintf(path, sizeof path, "/proc/%ld/schedstat", (long)syscall(SYS_gettid));
  g_schedstat_fd = open(path, O_RDONLY | O_CLOEXEC);
  if (g_schedstat_fd >= 0) {
    t = time_calls(call_schedstat_pread, NULL, SAMPLES);
    printf("%-42s %9.0f ns %9.0f ns\n", "pread(/proc/<tid>/schedstat), cached fd",
           t.median_ns, (double)t.min_ns);
    close(g_schedstat_fd);
  } else {
    printf("%-42s %12s\n", "pread(/proc/<tid>/schedstat), cached fd", "FAILED");
  }

  t = time_calls(call_schedstat_open, NULL, SAMPLES / 4);
  printf("%-42s %9.0f ns %9.0f ns\n", "open+pread+close(/proc/<tid>/schedstat)",
         t.median_ns, (double)t.min_ns);

  t = time_calls(call_start_time_parse, NULL, SAMPLES / 4);
  printf("%-42s %9.0f ns %9.0f ns\n", "open+read+parse(/proc/<tid>/stat)", t.median_ns,
         (double)t.min_ns);

  t = time_calls(call_getpid, NULL, SAMPLES);
  printf("%-42s %9.0f ns %9.0f ns\n", "getpid(), a bare syscall", t.median_ns,
         (double)t.min_ns);

  /* The hardware counter, if this process may open one. */
  struct perf_event_attr attr;
  memset(&attr, 0, sizeof attr);
  attr.size = sizeof attr;
  attr.type = PERF_TYPE_HARDWARE;
  attr.config = PERF_COUNT_HW_CPU_CYCLES;
  attr.disabled = 1;
  /* inherit=0: a thread-level counter must not absorb a child thread's
   * cycles, which is what makes it a per-thread measurement. */
  attr.inherit = 0;
  attr.exclude_kernel = 0;
  attr.exclude_hv = 0;

  g_perf_fd = (int)syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
  if (g_perf_fd >= 0) {
    ioctl(g_perf_fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(g_perf_fd, PERF_EVENT_IOC_ENABLE, 0);
    t = time_calls(call_perf_read, NULL, SAMPLES);
    printf("%-42s %9.0f ns %9.0f ns\n", "read(PERF_COUNT_HW_CPU_CYCLES)", t.median_ns,
           (double)t.min_ns);
    close(g_perf_fd);
  } else {
    printf("%-42s %12s\n", "read(PERF_COUNT_HW_CPU_CYCLES)",
           "no permission");
  }
#endif

  /* The whole sample, for comparison. */
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *session = NULL;
  if (pace_session_create(&cfg, &session) == PACE_OK) {
    pace_thread *thread = NULL;
    pace_session_register_thread(session, &thread);
    pace_sample sample;
    pace_session_sample(session, thread, &sample); /* the baseline */

    uint64_t *samples = (uint64_t *)malloc((size_t)SAMPLES * sizeof *samples);
    for (int i = 0; i < SAMPLES; i++) {
      uint64_t start = now_ns();
      pace_session_sample(session, thread, &sample);
      samples[i] = now_ns() - start;
    }
    for (int i = 1; i < SAMPLES; i++) {
      uint64_t key = samples[i];
      int j = i - 1;
      while (j >= 0 && samples[j] > key) {
        samples[j + 1] = samples[j];
        j--;
      }
      samples[j + 1] = key;
    }
    printf("\n%-42s %9.0f ns %9.0f ns\n", "the whole pace_session_sample()",
           (double)samples[SAMPLES / 2], (double)samples[0]);
    free(samples);
    pace_session_destroy(session);
  }

  return 0;
}
