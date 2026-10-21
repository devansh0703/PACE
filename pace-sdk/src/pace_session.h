/* SPDX-License-Identifier: BUSL-1.1
 * PACE — per-thread state, registry and session lifecycle.
 */
#ifndef PACE_INTERNAL_SESSION_H
#define PACE_INTERNAL_SESSION_H

#include "pace/pace.h"
#include "pace_backend.h"

#include <pthread.h>

#define PACE_MAX_THREADS 4096

/** Per-thread state. */
struct pace_thread {
  pace_session *session;
  /**
   * The id of the session that owns this handle.
   *
   * The TLS fast path cannot dereference the session to validate a handle,
   * because the session may have been destroyed and the memory freed. A
   * plain id, drawn from a counter that never repeats, can be compared
   * without touching freed memory.
   */
  uint64_t session_id;
  uint64_t thread_id; /**< OS thread id. */
  int slot;           /**< Index into the session's thread table, or -1. */
  int registered;

  /**
   * The thread that owns this handle.
   *
   * The OS thread id is recycled by the kernel: a new thread can get the id
   * of a thread that has exited. Identifying the owner by its pthread_t
   * instead distinguishes the two, because a pthread_t identifies a thread
   * among the *live* threads of the process.
   */
  pthread_t owner;

  /* Previous sample baseline, per source. */
  uint64_t base_wall_ns;
  uint64_t base_on_cpu_ns;
  uint64_t base_run_delay_ns;
  uint64_t base_involuntary;
  uint64_t base_voluntary;
  uint64_t base_core_cycles;
  uint64_t base_ref_cycles;
  int have_baseline;

  /**
   * Which counter the baseline was taken from: the kernel's run-queue
   * delay, or the eBPF tier's measurement.
   *
   * The two are different counters with different bases, so a baseline
   * from one cannot be subtracted from a sample of the other. When the
   * source changes the baseline is re-taken, and the sample that
   * observes the change is reported as degraded rather than as a
   * measurement.
   */
  int base_preempted_from_bpf;

  /**
   * The preemption time measured by the source that is NOT the current
   * one, over the window during which it was the source.
   *
   * The two counters are different, so a baseline from one cannot be
   * subtracted from a sample of the other. But they measure the same
   * phenomenon over DISJOINT windows: the kernel's counter is the
   * source until the tier's map has an entry for the thread, and the
   * tier's is the source afterwards. So the two windows add.
   *
   * The contribution is frozen at the moment the source changes, and it
   * is then added to the current source's difference.
   *
   * Without this, the time before the tier's map recognised the thread
   * was dropped, and on a CI runner that was 1.16 s of every thread's
   * 1.81 s of preemption.
   */
  uint64_t frozen_preempted_ns;

  /**
   * The kernel's counter's baseline, kept separately from the tier's.
   *
   * The tier's baseline is re-taken when the source changes, and the
   * kernel's is not, because the kernel's contribution is frozen rather
   * than replaced.
   */
  uint64_t base_kernel_run_delay_ns;

  /* Backend-owned state. */
  void *backend_data;
};

/** Session state. */
struct pace_session {
  pace_config config;
  const pace_backend *backend;

  /**
   * A unique id for this session.
   *
   * Drawn from a counter that never repeats, so a stale thread handle can
   * be recognised as stale without dereferencing freed memory.
   */
  uint64_t id;

  pace_source source;
  double reference_hz;

  pace_observer_fn observer;
  void *observer_data;

  /* Counters for pace_session_stats(). */
  uint64_t samples_taken;
  uint64_t degraded_samples;
  uint64_t io_errors;

  /* The eBPF tier, when it was loaded. Owned by the session. */
  void *bpf;
  /** True when the eBPF tier loaded and attached. */
  int bpf_available;

  /**
   * The number of involuntary switches the eBPF tier observed, and the
   * number of times the thread was seen runnable but not running.
   *
   * The eBPF tier counts scheduler events directly, so it is a measurement
   * rather than a difference of cumulative counters. It is used to report
   * the switch counts; the *time* attribution continues to come from the
   * kernel's run-queue delay, which is the only quantity that survives the
   * thread having been off-CPU.
   */
  uint64_t bpf_involuntary_switches;
  uint64_t bpf_voluntary_switches;

  /* Thread table with a mutex. The hot path uses the thread's own slot index
   * to avoid taking this lock on every sample. */
  pthread_mutex_t lock;
  pace_thread *threads[PACE_MAX_THREADS];
  int thread_count;
};

/* Thread-local fast path: the session and thread the current thread belongs to. */
extern PACE_TLS pace_thread *pace_tls_thread;

#endif /* PACE_INTERNAL_SESSION_H */
