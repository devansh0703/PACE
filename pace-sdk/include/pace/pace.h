/* SPDX-License-Identifier: BUSL-1.1
 *
 * PACE — Preemption-Aware Cycle Attribution Engine
 * Copyright (c) 2026 PACE contributors
 *
 * Business Source License 1.1
 * Licensor: PACE
 * Change Date: 2030-11-01
 * Change License: Apache-2.0
 *
 * Additional Use Grant: Production use of the Licensed Work is permitted to any
 * holder of a current, valid PACE commercial license. All other rights are
 * reserved under the Business Source License 1.1.
 *
 * NOTICE: This file is part of PACE. The complete terms of the Business
 * Source License 1.1 are available at:
 *   https://mariadb.com/bsl11/
 */
#ifndef PACE_PACE_H
#define PACE_PACE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
   /* A consumer of the DLL imports the symbols; a consumer of the static
    * library links them plainly. PACE_BUILDING is defined for the library
    * itself and for anything that links the static library, so a symbol
    * resolved at link time never asks for the import stub.
    *
    * PACE_BUILD_SHARED marks the DLL build, where the library itself must
    * export the symbols. */
#  if defined(PACE_BUILDING) && defined(PACE_BUILD_SHARED)
#    define PACE_API __declspec(dllexport)
#  elif defined(PACE_BUILDING)
#    define PACE_API
#  else
#    define PACE_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define PACE_API __attribute__((visibility("default")))
#else
#  define PACE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Library version, encoded as (major << 16) | (minor << 8) | patch. */
#define PACE_VERSION_MAJOR 0
#define PACE_VERSION_MINOR 1
#define PACE_VERSION_PATCH 0
#define PACE_VERSION_NUMBER \
  ((PACE_VERSION_MAJOR << 16) | (PACE_VERSION_MINOR << 8) | PACE_VERSION_PATCH)

/**
 * Status codes. Every fallible entry point returns one of these; no function
 * in this API aborts, and no function returns a partially-initialised object.
 */
typedef enum pace_status {
  PACE_OK = 0,
  PACE_ERR_INVALID_ARG = 1,     /**< A required pointer was NULL or a value out of range. */
  PACE_ERR_UNSUPPORTED = 2,     /**< The platform or kernel lacks the needed primitive. */
  PACE_ERR_PERMISSION = 3,      /**< The operation needs privileges the caller lacks. */
  PACE_ERR_NOMEM = 4,           /**< Allocation failed. */
  PACE_ERR_IO = 5,              /**< An OS call failed; see pace_last_error(). */
  PACE_ERR_STATE = 6,           /**< The object is in the wrong state for this call. */
  PACE_ERR_NOT_FOUND = 7,       /**< A referenced thread or record does not exist. */
  PACE_ERR_BUFFER_TOO_SMALL = 8 /**< A caller-supplied buffer cannot hold the result. */
} pace_status;

/**
 * Human-readable name for a status code. Never NULL.
 *
 * The name is stable and is part of the library's contract: a consumer can
 * branch on it, and it does not change between releases.
 */
PACE_API const char *pace_status_string(pace_status status);

/**
 * A one-line description of what the caller can do about a status code.
 *
 * The fix is always a safe, authorised action: a command to run, a
 * permission to request, or a configuration to change. It never asks the
 * caller to modify the library. Returns an empty string for PACE_OK.
 */
PACE_API const char *pace_status_fix(pace_status status);

/**
 * A one-line description of what to do next after a failure.
 *
 * Every failure is safe to retry once the stated action has been taken, and
 * none of them leaves a partial result that needs reconciling. Returns an
 * empty string for PACE_OK.
 */
PACE_API const char *pace_status_recovery(pace_status status);

/**
 * Where a sample's cycle numbers came from.
 *
 * The distinction is load-bearing: PACE_SOURCE_CLOCK values are derived from
 * clocks and a measured frequency, PACE_SOURCE_PERF values are read from
 * hardware performance counters. Consumers must not present a derived value
 * as a measured one.
 */
typedef enum pace_source {
  PACE_SOURCE_UNKNOWN = 0, /**< No cycle data could be produced. */
  PACE_SOURCE_CLOCK = 1,   /**< Nanosecond clocks; cycles derived from a measured frequency. */
  PACE_SOURCE_PERF = 2     /**< Hardware performance counter; cycles measured directly. */
} pace_source;

/** Bit flags carried in pace_sample::flags. */
enum {
  /** pace_sample::core_cycles holds a measured hardware core-cycle count. */
  PACE_SAMPLE_F_HAS_CORE_CYCLES = 1u << 0,
  /** pace_sample::ref_cycles holds a measured hardware reference-cycle count. */
  PACE_SAMPLE_F_HAS_REF_CYCLES = 1u << 1,
  /** The thread was runnable but not running for part of the window. */
  PACE_SAMPLE_F_WAS_PREEMPTED = 1u << 2,
  /** The thread was sleeping or waiting for I/O for part of the window. */
  PACE_SAMPLE_F_WAS_BLOCKED = 1u << 3,
  /** The sample window was shorter than the platform's reporting granularity. */
  PACE_SAMPLE_F_DEGRADED = 1u << 4,
  /** The window overlapped a thread migration between CPUs. */
  PACE_SAMPLE_F_MIGRATED = 1u << 5,
  /**
   * pace_sample::preempted_ns is a real measurement.
   *
   * When this flag is clear, the platform could not separate preemption from
   * blocking, and preempted_ns carries no information. A consumer must not
   * read a clear flag as "zero preemption".
   */
  PACE_SAMPLE_F_HAS_PREEMPTED = 1u << 6,
  /**
   * pace_sample::blocked_ns is a real measurement.
   *
   * When this flag is clear, the platform could not separate blocking from
   * preemption, and blocked_ns carries no information.
   */
  PACE_SAMPLE_F_HAS_BLOCKED = 1u << 7
};

/**
 * One measurement window for one thread.
 *
 * The four time fields satisfy, to within measurement skew:
 *
 *     wall_ns == on_cpu_ns + preempted_ns + blocked_ns
 *
 * on_cpu_ns is time the thread spent running. preempted_ns is time it was
 * runnable but descheduled (scheduler preemption). blocked_ns is the
 * remainder: sleeping, blocked on a lock, or waiting on I/O.
 */
typedef struct pace_sample {
  /** Size of this struct; must be sizeof(pace_sample). Enables forward compat. */
  uint32_t struct_size;
  uint32_t _reserved0; /**< Reserved for future use; always 0. */

  uint64_t wall_ns;   /**< Monotonic wall-clock duration of the window. */
  uint64_t on_cpu_ns; /**< Duration the thread was actually on a CPU. */
  uint64_t preempted_ns; /**< Duration the thread was runnable but not scheduled. */
  uint64_t blocked_ns;   /**< Duration the thread was neither running nor runnable. */

  uint64_t core_cycles;        /**< Measured core cycles, or 0 when unavailable. */
  uint64_t ref_cycles;         /**< Reference cycles, measured or derived. */
  double core_per_ref_ratio;   /**< Measured on-CPU core cycles per reference cycle. */

  uint64_t involuntary_switches; /**< Times descheduled while runnable. */
  uint64_t voluntary_switches;   /**< Times blocked waiting to be scheduled. */

  pace_source source; /**< Provenance of the cycle fields. */
  uint32_t flags;     /**< Bitwise OR of PACE_SAMPLE_F_* values. */
  uint32_t _reserved1; /**< Reserved for future use; always 0. */
} pace_sample;

/** Configuration for a session. */
typedef struct pace_config {
  /** Size of this struct; must be sizeof(pace_config). Enables forward compat. */
  size_t struct_size;

  /**
   * Request hardware performance counters (tier 1). When 0 the session uses
   * only privilege-free sources (tier 0). When 1 the session uses tier 1 if
   * it is available and silently falls back to tier 0 if it is not.
   */
  int prefer_perf_counters;

  /**
   * Open and keep a perf event file descriptor for every registered thread.
   * Costs one fd per thread. When 0 the fd is opened on demand and closed
   * after each sample, which is slower but avoids fd exhaustion.
   */
  int keep_perf_fds_open;

  /**
   * Read the voluntary/involuntary switch counters from /proc/<tid>/sched on
   * every sample.
   *
   * The switch counters are a refinement, not a requirement: the run-queue
   * delay already separates preemption from blocking. Reading them costs an
   * open + parse of a procfs file per sample, roughly 4 microseconds, which
   * dominates the sample cost. Leave this off unless the consumer needs the
   * raw switch counts.
   */
  int collect_switch_counters;

  /**
   * Use the eBPF scheduler tier when it is available.
   *
   * The eBPF tier reads the scheduler's own sched_wakeup and sched_switch
   * tracepoints, giving a per-interval preemption measurement that does not
   * depend on /proc being readable. It needs a kernel with BTF and needs
   * privilege to attach, so it is an *addition* to the procfs tier rather
   * than a replacement: when it is unavailable the procfs tier carries the
   * measurement unchanged.
   */
  int prefer_ebpf_tier;

  /** Reserved; must be 0. */
  int reserved0;
} pace_config;

/** Opaque session handle. */
typedef struct pace_session pace_session;

/** Opaque per-thread handle. */
typedef struct pace_thread pace_thread;

/**
 * Called for every sample produced by a subscribed session.
 *
 * Invoked on the thread that called pace_session_sample(), so an observer that
 * touches shared state must do its own synchronisation.
 */
typedef void (*pace_observer_fn)(void *user_data, const pace_sample *sample);

/**
 * Fill a pace_config with the library defaults.
 */
PACE_API pace_status pace_config_init(pace_config *config);

/**
 * Create a session.
 *
 * Fails with PACE_ERR_UNSUPPORTED when the platform provides none of the
 * required primitives, and with PACE_ERR_PERMISSION when tier 1 was requested
 * and is unavailable *and* the caller asked for it exclusively.
 *
 * THREADING CONTRACT
 *
 *   - pace_session_create, pace_session_destroy, pace_session_set_observer
 *     and pace_session_stats are NOT safe to call concurrently with sampling.
 *     Call set_observer before sampling starts.
 *   - pace_session_sample and pace_session_sample_self are safe to call
 *     concurrently from different threads, each with its own pace_thread.
 *   - A pace_thread is owned by the thread that registered it. Passing one
 *     thread's handle to another thread to sample is a data race.
 *   - The observer runs on the thread that called pace_session_sample. It
 *     must not call back into the session, must not longjmp, and must not
 *     throw across the C boundary.
 *   - pace_session_destroy invalidates every pace_thread it returned. Using
 *     one afterwards is undefined.
 *   - pace_session_destroy must be called exactly once per session.
 */
PACE_API pace_status pace_session_create(const pace_config *config, pace_session **out_session);

/** Destroy a session. Safe to call with NULL. */
PACE_API void pace_session_destroy(pace_session *session);

/**
 * Register the calling thread and return a handle for it.
 *
 * The handle is owned by the session. Passing the same thread twice returns
 * the same handle.
 */
PACE_API pace_status pace_session_register_thread(pace_session *session, pace_thread **out_thread);

/**
 * Sample a registered thread.
 *
 * The first sample for a thread establishes the baseline and returns a sample
 * with zero durations and PACE_SAMPLE_F_DEGRADED set.
 */
PACE_API pace_status pace_session_sample(pace_session *session, pace_thread *thread,
                                          pace_sample *out_sample);

/** Sample the calling thread without an explicit handle. */
PACE_API pace_status pace_session_sample_self(pace_session *session, pace_sample *out_sample);

/**
 * Install an observer invoked for every sample taken through this session.
 * Passing NULL removes the current observer. Only one observer per session.
 */
PACE_API pace_status pace_session_set_observer(pace_session *session, pace_observer_fn fn,
                                                void *user_data);

/** The pace_source tier the session actually obtained. */
PACE_API pace_source pace_session_source(const pace_session *session);

/**
 * Session counters, for a vendor to surface in their own telemetry.
 *
 * The degraded and error counts are the fastest way to answer "why is my
 * Tier 1 not active" without attaching a debugger.
 */
typedef struct pace_stats {
  /** Size of this struct; must be sizeof(pace_stats). */
  uint32_t struct_size;
  /** The tier the session obtained. */
  pace_source source;
  /** Threads registered with this session. */
  uint32_t threads_registered;
  /** Samples taken through this session. */
  uint64_t samples_taken;
  /** Samples carrying PACE_SAMPLE_F_DEGRADED. */
  uint64_t degraded_samples;
  /** Backend read failures. */
  uint64_t io_errors;
  /** Reserved; always 0. */
  uint64_t _reserved;
  /**
   * True when the eBPF scheduler tier loaded and attached.
   *
   * The eBPF tier counts the scheduler's own events directly. When it is
   * off, the switch counts in the samples come from the kernel's cumulative
   * counters and are only available if the consumer asked for them.
   */
  uint32_t ebpf_available;
  /** Reserved; always 0. */
  uint32_t _reserved1;
} pace_stats;

/** Fill a pace_stats with the session's counters. */
PACE_API pace_status pace_session_stats(const pace_session *session, pace_stats *out_stats);

/**
 * The measured reference frequency in Hz, or 0.0 when it could not be
 * determined. All cycle conversions in this session use this value.
 */
PACE_API double pace_session_reference_hz(const pace_session *session);

/** The OS error code from the most recent failing call on this thread, or 0. */
PACE_API int pace_last_error(void);

/** A static, human-readable description of the active backend. Never NULL. */
PACE_API const char *pace_backend_name(void);

/**
 * Format a cycle count as a human-readable string (e.g. "1.42 Gcycles").
 * Returns the number of characters written, or 0 on error.
 */
PACE_API int pace_format_cycles(uint64_t cycles, char *buffer, size_t buffer_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PACE_PACE_H */
