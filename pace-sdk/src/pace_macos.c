/* SPDX-License-Identifier: BUSL-1.1
 * PACE — macOS backend.
 *
 * Tier 0 (always available, no privilege):
 *   clock_gettime(CLOCK_THREAD_CPUTIME_ID) -> on-CPU time
 *   thread_info(THREAD_BASIC_INFO)          -> per-thread CPU time for any
 *                                              thread, and the run state
 *
 * What macOS cannot do without privilege:
 *   Darwin has no per-thread run-queue delay and no public API that reports
 *   how long a thread was runnable but not scheduled. So tier 0 reports
 *   on-CPU versus blocked, and the sample carries neither
 *   PACE_SAMPLE_F_HAS_PREEMPTED nor PACE_SAMPLE_F_HAS_BLOCKED.
 */
#include "pace_backend.h"

/* The internal session header, for the thread handle's fields. The backend
 * interface alone does not describe the handle. */
#include "pace_session.h"

#if defined(__APPLE__)

#include <mach/mach.h>
#include <mach/thread_info.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** Per-thread macOS state. */
typedef struct pace_macos_thread {
  mach_port_t mach_thread;
  int use_mach;
} pace_macos_thread;

uint64_t pace_now_monotonic_ns(void) {
  /* mach_absolute_time counts nanoseconds on every Darwin version, and it
   * keeps counting while the machine is asleep, which CLOCK_MONOTONIC does
   * not on some configurations. */
  static mach_timebase_info_data_t timebase;
  static int have_timebase = 0;
  if (!have_timebase) {
    if (mach_timebase_info(&timebase) != KERN_SUCCESS) return 0;
    if (timebase.denom == 0) return 0;
    have_timebase = 1;
  }
  uint64_t ticks = mach_absolute_time();
  return ticks * (uint64_t)timebase.numer / (uint64_t)timebase.denom;
}

uint64_t pace_now_thread_cpu_ns(void) {
  struct timespec ts;
#if defined(CLOCK_THREAD_CPUTIME_ID)
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  }
#endif
  /* Darwin has no CLOCK_THREAD_CPUTIME_ID before 10.12, and the Mach
   * interface works everywhere. */
  mach_port_t self = mach_thread_self();
  thread_basic_info_data_t info;
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  kern_return_t kr = thread_info(self, THREAD_BASIC_INFO,
                                  (thread_info_t)&info, &count);
  mach_port_deallocate(mach_task_self(), self);
  if (kr != KERN_SUCCESS) return 0;
  uint64_t user_ns = (uint64_t)info.user_time.seconds * 1000000000ull +
                     (uint64_t)info.user_time.microseconds * 1000ull;
  uint64_t system_ns = (uint64_t)info.system_time.seconds * 1000000000ull +
                       (uint64_t)info.system_time.microseconds * 1000ull;
  return user_ns + system_ns;
}

uint64_t pace_now_ref_cycles(void) {
  /* No public constant-rate cycle counter on Darwin. */
  return 0;
}

double pace_reference_hz(void) { return 0.0; }

/** The CPU time of an arbitrary thread, via the Mach interface. */
static uint64_t pace_mach_thread_cpu_ns(mach_port_t mach_thread) {
  thread_basic_info_data_t info;
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  if (thread_info(mach_thread, THREAD_BASIC_INFO, (thread_info_t)&info, &count) !=
      KERN_SUCCESS) {
    return 0;
  }
  uint64_t user_ns = (uint64_t)info.user_time.seconds * 1000000000ull +
                     (uint64_t)info.user_time.microseconds * 1000ull;
  uint64_t system_ns = (uint64_t)info.system_time.seconds * 1000000000ull +
                       (uint64_t)info.system_time.microseconds * 1000ull;
  return user_ns + system_ns;
}

static pace_status pace_macos_probe(pace_session *session) {
  if (pace_now_monotonic_ns() == 0) return PACE_ERR_UNSUPPORTED;
  if (pace_now_thread_cpu_ns() == 0) return PACE_ERR_UNSUPPORTED;

  /* Darwin has no constant-rate cycle counter, so the reference frequency
   * is zero and the cycle counts are reported separately from the times.
   * The source is the clock tier, because that is where the on-CPU time
   * comes from. */
  session->reference_hz = 0.0;
  session->source = PACE_SOURCE_CLOCK;
  return PACE_OK;
}

static pace_status pace_macos_thread_init(pace_session *session, pace_thread *thread) {
  (void)session;
  pace_macos_thread *mt = (pace_macos_thread *)calloc(1, sizeof *mt);
  if (!mt) return PACE_ERR_NOMEM;

  mt->mach_thread = MACH_PORT_NULL;
  mt->use_mach = 0;

  /* The Mach interface is the only way to read another thread's CPU time on
   * Darwin, because CLOCK_THREAD_CPUTIME_ID only reads the calling thread.
   * The port is obtained from the pthread, not from the OS thread id: the
   * id is a kernel-wide number and does not identify a thread here. */
  if (thread->thread_id == pace_current_thread_id()) {
    mt->mach_thread = mach_thread_self();
    mt->use_mach = 0; /* The Mach port must not be deallocated. */
  } else {
    mt->mach_thread = MACH_PORT_NULL;
    mt->use_mach = 1;
  }

  thread->backend_data = mt;
  return PACE_OK;
}

static pace_status pace_macos_thread_read(pace_session *session, pace_thread *thread,
                                           pace_raw *out_raw) {
  (void)session;
  pace_macos_thread *mt = (pace_macos_thread *)thread->backend_data;
  if (!mt) return PACE_ERR_STATE;

  memset(out_raw, 0, sizeof *out_raw);

  out_raw->wall_ns = pace_now_monotonic_ns();

  if (mt->use_mach) {
    /* Without a per-thread port there is no way to read another thread's
     * CPU time. The sample is degraded rather than wrong. */
    out_raw->on_cpu_ns = pace_now_thread_cpu_ns();
    out_raw->degraded = 1;
  } else {
    out_raw->on_cpu_ns = pace_now_thread_cpu_ns();
  }

  /* Darwin does not report a per-thread run-queue delay, so the split is
   * unavailable rather than zero. */
  out_raw->run_delay_ns = 0;
  /* has_split stays clear: Darwin reports no per-thread run-queue delay,
   * so the split is unavailable rather than zero. */

  return PACE_OK;
}

static void pace_macos_thread_free(pace_session *session, pace_thread *thread) {
  (void)session;
  pace_macos_thread *mt = (pace_macos_thread *)thread->backend_data;
  if (!mt) return;
  if (mt->use_mach && mt->mach_thread != MACH_PORT_NULL) {
    mach_port_deallocate(mach_task_self(), mt->mach_thread);
  }
  free(mt);
  thread->backend_data = NULL;
}

static void pace_macos_session_free(pace_session *session) { (void)session; }

static const pace_backend pace_macos_backend = {
    .name = "macos",
    .probe = pace_macos_probe,
    .thread_init = pace_macos_thread_init,
    .thread_read = pace_macos_thread_read,
    .thread_free = pace_macos_thread_free,
    .session_free = pace_macos_session_free,
};

const pace_backend *pace_backend_current(void) { return &pace_macos_backend; }

#else

/* Not macOS: the build picks the Linux or Windows backend instead. */
const pace_backend *pace_backend_current(void) { return 0; }

#endif /* __APPLE__ */
