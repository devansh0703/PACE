/* SPDX-License-Identifier: BUSL-1.1
 * PACE — internal platform backend interface.
 *
 * Every platform provides a pace_backend. The generic code in pace_session.c
 * never touches an OS API directly.
 */
#ifndef PACE_INTERNAL_BACKEND_H
#define PACE_INTERNAL_BACKEND_H

#include "pace/pace.h"

#include <stdint.h>

/* Thread-local storage qualifier for the per-thread fast path. */
#if defined(_MSC_VER)
#  define PACE_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define PACE_TLS __thread
#else
#  define PACE_TLS
#endif

#if defined(__linux__)
#  define PACE_BACKEND_NAME "linux"
#elif defined(_WIN32)
#  define PACE_BACKEND_NAME "windows"
#elif defined(__APPLE__)
#  define PACE_BACKEND_NAME "macos"
#else
#  define PACE_BACKEND_NAME "unsupported"
#endif

/** Raw per-thread counters as the OS reports them. */
typedef struct pace_raw {
  uint64_t wall_ns;      /**< Monotonic wall delta since the previous sample. */
  uint64_t on_cpu_ns;    /**< Time the thread spent running. */
  uint64_t run_delay_ns; /**< Time the thread spent runnable but not scheduled. */
  uint64_t involuntary_switches;
  uint64_t voluntary_switches;
  uint64_t core_cycles;  /**< 0 when the platform cannot report measured core cycles. */
  uint64_t ref_cycles;   /**< 0 when the platform cannot report reference cycles. */
  uint64_t time_enabled; /**< perf time the counter was enabled. */
  uint64_t time_running; /**< perf time the counter was actually scheduled. */
  int migrated;          /**< Non-zero when the thread changed CPU during the window. */
  int degraded;          /**< Non-zero when some field is unavailable or partial. */
  /**
   * Non-zero when the platform can separate preemption from blocking.
   *
   * Linux can, because /proc/<tid>/schedstat reports the run-queue delay
   * directly. Windows and macOS cannot without an elevated session, so
   * their samples carry neither PACE_SAMPLE_F_HAS_PREEMPTED nor
   * PACE_SAMPLE_F_HAS_BLOCKED, and a consumer must read the off-CPU time
   * as "not running" rather than as a split.
   */
  int has_split;
} pace_raw;

/**
 * A platform backend.
 *
 * probe()      — capability detection; called once per session.
 * thread_init()— per-thread setup; may allocate.
 * thread_read()— take one measurement for a thread.
 * thread_free()— per-thread teardown.
 * session_free()— backend-specific session teardown.
 */
typedef struct pace_backend {
  const char *name;

  /** Returns PACE_OK when the backend can work at all. */
  pace_status (*probe)(pace_session *session);

  /** Returns PACE_OK, or an error describing why this thread cannot be tracked. */
  pace_status (*thread_init)(pace_session *session, pace_thread *thread);

  /** Take one measurement. */
  pace_status (*thread_read)(pace_session *session, pace_thread *thread, pace_raw *out_raw);

  /** Release per-thread resources. */
  void (*thread_free)(pace_session *session, pace_thread *thread);

  /** Release session resources owned by the backend. */
  void (*session_free)(pace_session *session);
} pace_backend;

/** The backend for the current platform. */
const pace_backend *pace_backend_current(void);

/**
 * The OS thread id of the calling thread.
 *
 * On Linux this is the kernel's task id, on Windows the thread id, and on
 * macOS the Mach thread port. It identifies the calling thread for the
 * lifetime of the process, which is what a `pace_thread` handle is keyed
 * on.
 */
uint64_t pace_current_thread_id(void);

/* Shared helpers used by every backend. */

/** Monotonic clock in nanoseconds. */
uint64_t pace_now_monotonic_ns(void);

/** Per-thread CPU time in nanoseconds (CLOCK_THREAD_CPUTIME_ID equivalent). */
uint64_t pace_now_thread_cpu_ns(void);

/** Reference-cycle count since an arbitrary fixed origin (TSC on x86). */
uint64_t pace_now_ref_cycles(void);

/** Reference frequency in Hz, or 0.0 if unknown. */
double pace_reference_hz(void);

/**
 * Scale a raw counter reading for multiplexed PMUs.
 *
 * When a counter cannot be scheduled continuously the kernel reports
 * time_enabled and time_running. The scaled value is
 * value * time_enabled / time_running. Returns the raw value when the
 * counter was running continuously.
 */
uint64_t pace_scale_multiplexed(uint64_t value, uint64_t time_enabled, uint64_t time_running);

/**
 * Convert a duration into reference cycles at a measured frequency.
 *
 * The result is derived, not measured: the reference clock is invariant but
 * the frequency carries its own error. Returns 0 when the frequency is
 * unknown, so a platform with no reference clock reports nothing rather
 * than a made-up number.
 */
uint64_t pace_ns_to_ref_cycles(uint64_t ns, double hz);

#endif /* PACE_INTERNAL_BACKEND_H */
