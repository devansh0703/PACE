/* SPDX-License-Identifier: BUSL-1.1
 * PACE — Windows backend.
 *
 * Tier 0 (always available, no privilege):
 *   GetThreadTimes       -> on-CPU time (kernel + user), 100 ns units
 *   QueryThreadCycleTime -> hardware cycle count, when available
 *
 * What Windows cannot do without privilege:
 *   Windows has no per-thread run-queue delay. There is no public API that
 *   reports how long a thread was runnable but not scheduled, and the ETW
 *   context-switch events that could reconstruct it require an elevated
 *   session. So tier 0 reports on-CPU versus blocked, and the sample carries
 *   neither PACE_SAMPLE_F_HAS_PREEMPTED nor PACE_SAMPLE_F_HAS_BLOCKED
 *   unless the ETW tier is enabled.
 */
#include "pace_backend.h"

/* The internal session header, for the thread handle's fields. The backend
 * interface alone does not describe the handle. */
#include "pace_session.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

/** Per-thread Windows state. */
typedef struct pace_windows_thread {
  HANDLE handle;
  uint64_t cycles_base;
  int cycles_supported;
} pace_windows_thread;

uint64_t pace_now_monotonic_ns(void) {
  LARGE_INTEGER frequency;
  static LARGE_INTEGER cached = {0};
  if (cached.QuadPart == 0) {
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0) return 0;
    cached = frequency;
  }
  LARGE_INTEGER counter;
  if (!QueryPerformanceCounter(&counter)) return 0;
  /* Split the division so the multiply cannot overflow: the counter and the
   * nanoseconds are both 64-bit, and the product exceeds 64 bits. */
  uint64_t seconds = (uint64_t)counter.QuadPart / (uint64_t)cached.QuadPart;
  uint64_t remainder = (uint64_t)counter.QuadPart % (uint64_t)cached.QuadPart;
  return seconds * 1000000000ull +
         (remainder * 1000000000ull) / (uint64_t)cached.QuadPart;
}

uint64_t pace_now_thread_cpu_ns(void) {
  /* GetThreadTimes needs a handle to the thread. The pseudo handle for the
   * calling thread is what a sample of the current thread uses. */
  HANDLE handle = GetCurrentThread();
  FILETIME creation, exit, kernel, user;
  if (!GetThreadTimes(handle, &creation, &exit, &kernel, &user)) return 0;

  ULARGE_INTEGER k, u;
  k.LowPart = kernel.dwLowDateTime;
  k.HighPart = kernel.dwHighDateTime;
  u.LowPart = user.dwLowDateTime;
  u.HighPart = user.dwHighDateTime;
  /* FILETIME counts 100 ns units. */
  return (k.QuadPart + u.QuadPart) * 100ull;
}

uint64_t pace_now_ref_cycles(void) {
  /* QueryThreadCycleTime is a per-thread count, not a global one, so it
   * cannot serve as the reference clock here. */
  return 0;
}

double pace_reference_hz(void) {
  /* Windows does not expose a constant reference frequency through a public
   * API. The cycle counts are reported separately from the times, and the
   * two are never derived from one another. */
  return 0.0;
}

/** Open a query handle to a thread by its id. */
static HANDLE pace_open_thread(uint64_t tid) {
  return OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)tid);
}

static pace_status pace_windows_probe(pace_session *session) {
  /* GetThreadTimes on the current thread is the minimum the backend needs.
   * If that fails, the platform cannot support the measurement at all. */
  FILETIME creation, exit, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
    return PACE_ERR_UNSUPPORTED;
  }

  /* Windows has no constant-rate cycle counter, so the reference frequency
   * is zero and the cycle counts are reported separately from the times.
   * The source is the clock tier, because that is where the on-CPU time
   * comes from. */
  session->reference_hz = 0.0;
  session->source = PACE_SOURCE_CLOCK;
  return PACE_OK;
}

static pace_status pace_windows_thread_init(pace_session *session, pace_thread *thread) {
  (void)session;
  pace_windows_thread *wt = (pace_windows_thread *)calloc(1, sizeof *wt);
  if (!wt) return PACE_ERR_NOMEM;

  wt->handle = NULL;
  wt->cycles_supported = 0;

  /* A handle to the calling thread is only valid for that thread, so the
   * handle is opened for a thread id. The current thread's pseudo handle
   * works for the common case of sampling the calling thread. */
  if (thread->thread_id == pace_current_thread_id()) {
    wt->handle = GetCurrentThread();
  } else {
    wt->handle = pace_open_thread(thread->thread_id);
  }
  if (!wt->handle) {
    free(wt);
    return PACE_ERR_NOT_FOUND;
  }

  ULARGE_INTEGER cycles;
  if (QueryThreadCycleTime(wt->handle, &cycles.QuadPart)) {
    wt->cycles_supported = 1;
    wt->cycles_base = 0;
  }

  thread->backend_data = wt;
  return PACE_OK;
}

static pace_status pace_windows_thread_read(pace_session *session, pace_thread *thread,
                                            pace_raw *out_raw) {
  (void)session;
  pace_windows_thread *wt = (pace_windows_thread *)thread->backend_data;
  if (!wt || !wt->handle) return PACE_ERR_STATE;

  memset(out_raw, 0, sizeof *out_raw);

  FILETIME creation, exit_time, kernel, user;
  if (!GetThreadTimes(wt->handle, &creation, &exit_time, &kernel, &user)) {
    return PACE_ERR_IO;
  }

  ULARGE_INTEGER k, u;
  k.LowPart = kernel.dwLowDateTime;
  k.HighPart = kernel.dwHighDateTime;
  u.LowPart = user.dwLowDateTime;
  u.HighPart = user.dwHighDateTime;

  out_raw->wall_ns = pace_now_monotonic_ns();
  out_raw->on_cpu_ns = (k.QuadPart + u.QuadPart) * 100ull;

  /* The cycle count is a cumulative per-thread counter, like the Linux one,
   * so the backend reports the raw cumulative value and the session
   * differences it against the thread's baseline. */
  ULARGE_INTEGER cycles;
  if (wt->cycles_supported && QueryThreadCycleTime(wt->handle, &cycles.QuadPart)) {
    out_raw->core_cycles = cycles.QuadPart;
  }

  /* Windows cannot report the run-queue delay without an elevated ETW
   * session, so the split is unavailable rather than zero. The session
   * layer reads the absence of these fields as "not measured" and the
   * sample does not claim the HAS_PREEMPTED or HAS_BLOCKED flags. */
  out_raw->run_delay_ns = 0;
  out_raw->degraded = 0;
  /* has_split stays clear: Windows cannot report the run-queue delay
   * without an elevated ETW session, so the split is unavailable rather
   * than zero. */

  return PACE_OK;
}

static void pace_windows_thread_free(pace_session *session, pace_thread *thread) {
  (void)session;
  pace_windows_thread *wt = (pace_windows_thread *)thread->backend_data;
  if (!wt) return;
  /* The pseudo handle must not be closed. */
  if (wt->handle && wt->handle != GetCurrentThread()) CloseHandle(wt->handle);
  free(wt);
  thread->backend_data = NULL;
}

static void pace_windows_session_free(pace_session *session) { (void)session; }

static const pace_backend pace_windows_backend = {
    .name = "windows",
    .probe = pace_windows_probe,
    .thread_init = pace_windows_thread_init,
    .thread_read = pace_windows_thread_read,
    .thread_free = pace_windows_thread_free,
    .session_free = pace_windows_session_free,
};

const pace_backend *pace_backend_current(void) { return &pace_windows_backend; }

#else

/* Not Windows: the build picks the Linux or macOS backend instead. */
const pace_backend *pace_backend_current(void) { return 0; }

#endif /* _WIN32 */
