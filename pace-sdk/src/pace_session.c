/* SPDX-License-Identifier: BUSL-1.1
 * PACE — session lifecycle, thread registry and the sample hot path.
 */
#include "pace_session.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__linux__)
#  include <sys/syscall.h>
#  include <unistd.h>
#elif defined(__APPLE__)
#  include <mach/mach.h>
#  include <pthread.h>
#endif

PACE_TLS pace_thread *pace_tls_thread = NULL;

/**
 * The id of the session that owns pace_tls_thread.
 *
 * The thread handle itself is freed when the session is destroyed, so it
 * cannot be dereferenced to find out whether it is still valid. The id
 * lives here, in thread-local storage that outlives the session, so the
 * fast path can decide whether the handle is still usable without
 * touching freed memory.
 */
PACE_TLS uint64_t pace_tls_session_id = 0;

/**
 * A counter that never repeats, used to give each session a unique id.
 *
 * A thread handle outlives its session when it sits in the thread-local
 * fast path. Comparing the handle's session id against the current
 * session's id recognises a stale handle without dereferencing the freed
 * session, which is what makes the fast path safe after a destroy.
 */
static uint64_t pace_next_session_id = 1;

static PACE_TLS int pace_tls_error = 0;

int pace_last_error(void) { return pace_tls_error; }

static pace_status pace_fail(pace_status status, int err) {
  pace_tls_error = err;
  return status;
}

const char *pace_status_string(pace_status status) {
  switch (status) {
    case PACE_OK: return "ok";
    case PACE_ERR_INVALID_ARG: return "invalid argument";
    case PACE_ERR_UNSUPPORTED: return "unsupported on this platform";
    case PACE_ERR_PERMISSION: return "permission denied";
    case PACE_ERR_NOMEM: return "out of memory";
    case PACE_ERR_IO: return "i/o error";
    case PACE_ERR_STATE: return "invalid state";
    case PACE_ERR_NOT_FOUND: return "not found";
    case PACE_ERR_BUFFER_TOO_SMALL: return "buffer too small";
    default: return "unknown status";
  }
}

const char *pace_status_fix(pace_status status) {
  switch (status) {
    case PACE_OK: return "";
    case PACE_ERR_INVALID_ARG:
      return "check the argument against the header's contract; the handle must "
             "belong to this session and the struct_size must be this build's";
    case PACE_ERR_UNSUPPORTED:
      return "the platform cannot produce this measurement; continue without PACE "
             "and say so in your own UI";
    case PACE_ERR_PERMISSION:
      return "run with CAP_PERFMON, or lower /proc/sys/kernel/perf_event_paranoid; "
             "the tier-0 measurement needs neither";
    case PACE_ERR_NOMEM:
      return "the session's thread table is full; raise PACE_MAX_THREADS or sample "
             "fewer threads";
    case PACE_ERR_IO:
      return "the thread's /proc entry could not be read; check that the thread is "
             "alive and that /proc is mounted";
    case PACE_ERR_STATE:
      return "the session is not in a state that allows this call; check the call's "
             "preconditions in the header";
    case PACE_ERR_NOT_FOUND:
      return "the thread or record does not exist; register the thread before "
             "sampling it";
    case PACE_ERR_BUFFER_TOO_SMALL:
      return "the buffer is too small; size it from the documented return value "
             "and try again";
    default: return "unknown status";
  }
}

const char *pace_status_recovery(pace_status status) {
  switch (status) {
    case PACE_OK: return "";
    case PACE_ERR_INVALID_ARG:
      return "nothing was measured and nothing was written; correct the argument "
             "and call again";
    case PACE_ERR_UNSUPPORTED:
    case PACE_ERR_PERMISSION:
      return "the measurement did not run; no sample was produced; either grant "
             "the permission or run without that tier and call again";
    case PACE_ERR_NOMEM:
      return "the measurement did not run; no sample was produced; free a thread "
             "slot or raise the limit and call again";
    case PACE_ERR_IO:
      return "the measurement did not run; the sample is unchanged and no partial "
             "value was written; check the thread and call again";
    case PACE_ERR_STATE:
    case PACE_ERR_NOT_FOUND:
      return "the measurement did not run; the sample is unchanged; fix the "
             "precondition and call again";
    case PACE_ERR_BUFFER_TOO_SMALL:
      return "nothing was written; resize the buffer and call again";
    default: return "unknown status";
  }
}

const char *pace_backend_name(void) { return PACE_BACKEND_NAME; }

uint64_t pace_current_thread_id(void) {
#if defined(__linux__)
  return (uint64_t)syscall(__NR_gettid);
#elif defined(_WIN32)
  return (uint64_t)GetCurrentThreadId();
#elif defined(__APPLE__)
  /* Darwin has no gettid. The Mach thread port identifies the thread for
   * the lifetime of the process, which is more than the kernel id gives
   * and is what the macOS backend needs. */
  return (uint64_t)mach_thread_self();
#else
  return (uint64_t)pthread_self();
#endif
}

pace_status pace_config_init(pace_config *config) {
  if (!config) return PACE_ERR_INVALID_ARG;
  memset(config, 0, sizeof *config);
  config->struct_size = sizeof *config;
  config->prefer_perf_counters = 1;
  config->keep_perf_fds_open = 1;
  /* Off by default: it costs about 4 microseconds per sample and the
   * run-queue delay already carries the preemption/blocking split. */
  config->collect_switch_counters = 0;
  /* On by default: the eBPF tier is an addition to the procfs tier, and it
   * falls back silently when it is unavailable. */
  config->prefer_ebpf_tier = 1;
  return PACE_OK;
}

pace_status pace_session_create(const pace_config *config, pace_session **out_session) {
  if (!out_session) return PACE_ERR_INVALID_ARG;
  *out_session = NULL;

  pace_config cfg;
  if (config) {
    if (config->struct_size != sizeof(pace_config)) return PACE_ERR_INVALID_ARG;
    cfg = *config;
  } else {
    pace_config_init(&cfg);
  }

  pace_session *session = (pace_session *)calloc(1, sizeof *session);
  if (!session) return PACE_ERR_NOMEM;

  session->config = cfg;
  session->backend = pace_backend_current();
  session->id = __atomic_fetch_add(&pace_next_session_id, 1, __ATOMIC_RELAXED);
  pthread_mutex_init(&session->lock, NULL);

  if (!session->backend->probe) {
    pthread_mutex_destroy(&session->lock);
    free(session);
    return pace_fail(PACE_ERR_UNSUPPORTED, 0);
  }

  pace_status st = session->backend->probe(session);
  if (st != PACE_OK) {
    pthread_mutex_destroy(&session->lock);
    free(session);
    return pace_fail(st, 0);
  }

  *out_session = session;
  return PACE_OK;
}

void pace_session_destroy(pace_session *session) {
  if (!session) return;
  for (int i = 0; i < PACE_MAX_THREADS; i++) {
    pace_thread *t = session->threads[i];
    if (!t) continue;
    if (session->backend && session->backend->thread_free) {
      session->backend->thread_free(session, t);
    }
    free(t);
    session->threads[i] = NULL;
  }
  if (session->backend && session->backend->session_free) {
    session->backend->session_free(session);
  }
  pthread_mutex_destroy(&session->lock);
  free(session);
}

pace_status pace_session_register_thread(pace_session *session, pace_thread **out_thread) {
  if (!session || !out_thread) return PACE_ERR_INVALID_ARG;
  *out_thread = NULL;

  uint64_t tid = pace_current_thread_id();
  pthread_t self = pthread_self();

  pthread_mutex_lock(&session->lock);
  int slot = -1;
  for (int i = 0; i < PACE_MAX_THREADS; i++) {
    pace_thread *existing = session->threads[i];
    if (existing && existing->thread_id == tid) {
      if (pthread_equal(existing->owner, self)) {
        /* The same live thread registering again. */
        pthread_mutex_unlock(&session->lock);
        *out_thread = existing;
        return PACE_OK;
      }
      /* The thread id was recycled: this slot belongs to a thread that has
       * exited and a new thread now carries the same id. The old handle's
       * baselines describe a different thread entirely, so the slot is
       * reclaimed and the handle is rebuilt for the new owner. */
      if (session->backend && session->backend->thread_free) {
        session->backend->thread_free(session, existing);
      }
      free(existing);
      session->threads[i] = NULL;
      session->thread_count--;
      if (slot < 0) slot = i;
      continue;
    }
    if (!existing && slot < 0) slot = i;
  }
  if (slot < 0) {
    pthread_mutex_unlock(&session->lock);
    return pace_fail(PACE_ERR_NOMEM, 0);
  }

  pace_thread *thread = (pace_thread *)calloc(1, sizeof *thread);
  if (!thread) {
    pthread_mutex_unlock(&session->lock);
    return pace_fail(PACE_ERR_NOMEM, 0);
  }
  thread->session = session;
  thread->session_id = session->id;
  thread->thread_id = tid;
  thread->slot = slot;
  thread->owner = self;

  pace_status st = session->backend->thread_init(session, thread);
  if (st != PACE_OK) {
    free(thread);
    pthread_mutex_unlock(&session->lock);
    return pace_fail(st, errno);
  }

  session->threads[slot] = thread;
  session->thread_count++;
  pthread_mutex_unlock(&session->lock);

  pace_tls_thread = thread;
  pace_tls_session_id = session->id;
  *out_thread = thread;
  return PACE_OK;
}

/**
 * Convert a raw reading into a pace_sample by differencing against the
 * thread's previous baseline.
 *
 * The decomposition is:
 *     wall   = now_wall - base_wall
 *     on_cpu = now_on_cpu - base_on_cpu
 *     preempted = run_delay_delta
 *     blocked = wall - on_cpu - preempted
 *
 * The residual is clamped at zero: the three /proc and clock reads are not
 * atomic, so the residual can come out slightly negative (tens of
 * nanoseconds). Clamping keeps the invariant wall == on_cpu + preempted +
 * blocked exact for consumers instead of leaking a negative duration.
 */
static pace_status pace_fill_sample(pace_session *session, pace_thread *thread,
                                    const pace_raw *raw, pace_sample *out_sample) {
  /* The sample is written field by field, and some fields are only
   * written on some paths. A struct the caller reuses across calls would
   * therefore keep the previous call's values in the fields this path
   * does not write: a stale source, a stale cycle count, a stale ratio.
   *
   * So the sample is cleared first, and every field in it is written by
   * this function. */
  memset(out_sample, 0, sizeof *out_sample);
  out_sample->struct_size = (uint32_t)sizeof *out_sample;

  if (!thread->have_baseline) {
    thread->base_wall_ns = raw->wall_ns;
    thread->base_on_cpu_ns = raw->on_cpu_ns;
    thread->base_run_delay_ns = raw->run_delay_ns;
    thread->base_kernel_run_delay_ns = raw->run_delay_ns;
    thread->frozen_preempted_ns = 0;
    thread->base_involuntary = raw->involuntary_switches;
    thread->base_voluntary = raw->voluntary_switches;
    thread->base_core_cycles = raw->core_cycles;
    thread->base_ref_cycles = raw->ref_cycles;
    thread->base_preempted_from_bpf = raw->preempted_from_bpf;
    thread->have_baseline = 1;

    out_sample->source = session->source;
    out_sample->flags |= PACE_SAMPLE_F_DEGRADED;
    __atomic_fetch_add(&session->samples_taken, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&session->degraded_samples, 1, __ATOMIC_RELAXED);
    return PACE_OK;
  }

  /* The preemption time can come from two different counters: the
   * kernel's run-queue delay, or the eBPF tier's measurement of the
   * scheduler's own events. They are different counters with different
   * bases, so a baseline taken from one cannot be subtracted from a
   * sample of the other.
   *
   * The source changes when the eBPF tier is attached mid-session, or
   * when a thread the program has not seen yet is first sampled. When it
   * changes the baseline is re-taken and the sample is reported as
   * degraded, because it spans the change and is therefore not a
   * measurement of either window. */
  if (raw->preempted_from_bpf != thread->base_preempted_from_bpf) {
    /* The source changes when the tier's map recognises the thread, and
     * the two counters measure the same phenomenon over DISJOINT
     * windows: the kernel's counter was the source until now, and the
     * tier's is the source from now on.
     *
     * So the kernel's contribution is FROZEN at its value now, rather
     * than dropped, and the two are added.
     *
     * Without the freeze, the time before the tier's map recognised the
     * thread was lost, and on a CI runner that was 1.16 s of every
     * thread's 1.81 s of preemption: the thread was measured as
     * preempted for 0.65 s when it was really preempted for 1.81 s.
     *
     * The sample itself is still degraded, because it spans the change
     * and is therefore not a measurement of either window alone. */
    if (thread->base_preempted_from_bpf == 0) {
      /* The kernel's counter was the source, so its contribution is the
       * difference from the kernel's baseline. */
      uint64_t kernel_delta = raw->run_delay_ns - thread->base_kernel_run_delay_ns;
      thread->frozen_preempted_ns += kernel_delta;
    } else {
      /* The tier was the source, and now the kernel's is. */
      uint64_t tier_delta = raw->run_delay_ns - thread->base_run_delay_ns;
      thread->frozen_preempted_ns += tier_delta;
    }

    thread->base_run_delay_ns = raw->run_delay_ns;
    thread->base_kernel_run_delay_ns = raw->run_delay_ns;
    thread->base_preempted_from_bpf = raw->preempted_from_bpf;

    /* The sample is NOT emptied. Its window is the whole time since the
     * thread's first sample -- the wall and on-CPU baselines are not
     * reset -- so it can carry the frozen contribution, which belongs to
     * the pre-switch window, together with the tier's difference for the
     * post-switch window.
     *
     * Carrying it in the NEXT sample instead would truncate it: that
     * sample's window is only the post-switch part, and the clamp
     * would cut the frozen contribution down to it.
     *
     * The sample is still degraded, because its window spans a change of
     * source, but its values are the window's. */
    uint64_t switch_wall = raw->wall_ns - thread->base_wall_ns;
    uint64_t switch_on_cpu = raw->on_cpu_ns - thread->base_on_cpu_ns;
    uint64_t switch_run_delay = raw->run_delay_ns - thread->base_run_delay_ns;
    /* The frozen contribution is carried by THIS sample, so it is
     * cleared here rather than by the next one. */
    switch_run_delay += thread->frozen_preempted_ns;
    thread->frozen_preempted_ns = 0;

    int64_t signed_wall = (int64_t)switch_wall;
    int64_t signed_on_cpu = (int64_t)switch_on_cpu;
    int64_t signed_run_delay = (int64_t)switch_run_delay;
    if (signed_on_cpu < 0) signed_on_cpu = 0;
    if (signed_run_delay < 0) signed_run_delay = 0;
    if (signed_on_cpu > signed_wall) signed_on_cpu = signed_wall;
    if (signed_run_delay > signed_wall - signed_on_cpu) {
      signed_run_delay = signed_wall - signed_on_cpu;
    }

    out_sample->wall_ns = switch_wall;
    out_sample->on_cpu_ns = (uint64_t)signed_on_cpu;
    out_sample->preempted_ns = (uint64_t)signed_run_delay;
    out_sample->blocked_ns =
        (uint64_t)(signed_wall - signed_on_cpu - signed_run_delay);
    out_sample->source = session->source;
    out_sample->flags |= PACE_SAMPLE_F_DEGRADED;
    if (raw->preempted_from_bpf) {
      out_sample->flags |= PACE_SAMPLE_F_PREEMPTED_FROM_EVENTS;
    }
    __atomic_fetch_add(&session->samples_taken, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&session->degraded_samples, 1, __ATOMIC_RELAXED);
    return PACE_OK;
  }

  uint64_t wall = raw->wall_ns - thread->base_wall_ns;
  uint64_t on_cpu = raw->on_cpu_ns - thread->base_on_cpu_ns;
  uint64_t run_delay = raw->run_delay_ns - thread->base_run_delay_ns;

  /* The frozen contribution was carried by the sample that observed the
   * change of source, which has the window that belongs to it. It is
   * zero here, because it has already been counted once. */
  run_delay += thread->frozen_preempted_ns;

  /* The three underlying reads (CLOCK_THREAD_CPUTIME_ID, /proc/<tid>/schedstat
   * and the wall clock) are not atomic, so the raw difference can be off by a
   * few microseconds in either direction. Compute in signed arithmetic and
   * clamp, so the consumer always sees a valid non-negative duration and the
   * invariant wall == on_cpu + preempted + blocked holds exactly. */
  int64_t signed_wall = (int64_t)wall;
  int64_t signed_on_cpu = (int64_t)on_cpu;
  int64_t signed_run_delay = (int64_t)run_delay;

  if (signed_on_cpu < 0) signed_on_cpu = 0;
  if (signed_run_delay < 0) signed_run_delay = 0;
  if (signed_on_cpu > signed_wall) signed_on_cpu = signed_wall;
  if (signed_run_delay > signed_wall - signed_on_cpu) {
    signed_run_delay = signed_wall - signed_on_cpu;
  }
  int64_t signed_blocked = signed_wall - signed_on_cpu - signed_run_delay;
  if (signed_blocked < 0) signed_blocked = 0;

  on_cpu = (uint64_t)signed_on_cpu;
  run_delay = (uint64_t)signed_run_delay;
  uint64_t blocked = (uint64_t)signed_blocked;

  out_sample->wall_ns = wall;
  out_sample->on_cpu_ns = on_cpu;
  out_sample->preempted_ns = run_delay;
  out_sample->blocked_ns = blocked;

  /* The source is the session's tier, which is the clock tier unless a
   * hardware counter was read, in which case it is PERF. Without this the
   * sample would carry no source at all, and a consumer could not tell a
   * clock-derived cycle count from a measured one. */
  out_sample->source = session->source;

  out_sample->involuntary_switches = raw->involuntary_switches - thread->base_involuntary;
  out_sample->voluntary_switches = raw->voluntary_switches - thread->base_voluntary;

  uint32_t flags = 0;
  if (raw->degraded) flags |= PACE_SAMPLE_F_DEGRADED;
  if (run_delay > 0) flags |= PACE_SAMPLE_F_WAS_PREEMPTED;
  if (blocked > 0) flags |= PACE_SAMPLE_F_WAS_BLOCKED;
  if (raw->has_split) {
    /* The platform produced a real preemption and blocking measurement, so
     * both quantities are known rather than merely absent. */
    flags |= PACE_SAMPLE_F_HAS_PREEMPTED | PACE_SAMPLE_F_HAS_BLOCKED;
  }
  if (raw->preempted_from_bpf) {
    /* The preemption was measured from the scheduler's own events, not
     * from the kernel's run-queue delay counter. The two differ
     * substantially on some kernels, so the consumer is told which one
     * it got. */
    flags |= PACE_SAMPLE_F_PREEMPTED_FROM_EVENTS;
  }

  double hz = session->reference_hz;

  if (raw->core_cycles > 0 && raw->time_running > 0) {
    uint64_t core_delta = raw->core_cycles - thread->base_core_cycles;
    out_sample->core_cycles = pace_scale_multiplexed(core_delta, raw->time_enabled,
                                                      raw->time_running);
    flags |= PACE_SAMPLE_F_HAS_CORE_CYCLES;
    out_sample->source = PACE_SOURCE_PERF;
  } else {
    out_sample->core_cycles = 0;
  }

  if (raw->ref_cycles > 0) {
    out_sample->ref_cycles = raw->ref_cycles - thread->base_ref_cycles;
    flags |= PACE_SAMPLE_F_HAS_REF_CYCLES;
  } else if (hz > 0.0) {
    /* Derived from the measured reference frequency: exact in the sense that
     * the reference clock is invariant, but not a hardware counter reading. */
    out_sample->ref_cycles = pace_ns_to_ref_cycles(wall, hz);
  }

  if (out_sample->core_cycles > 0 && out_sample->ref_cycles > 0) {
    out_sample->core_per_ref_ratio =
        (double)out_sample->core_cycles / (double)out_sample->ref_cycles;
  }

  out_sample->flags = flags;
  out_sample->_reserved1 = 0;

  thread->base_wall_ns = raw->wall_ns;
  thread->base_on_cpu_ns = raw->on_cpu_ns;
  thread->base_run_delay_ns = raw->run_delay_ns;
  thread->base_involuntary = raw->involuntary_switches;
  thread->base_voluntary = raw->voluntary_switches;
  thread->base_core_cycles = raw->core_cycles;
  thread->base_ref_cycles = raw->ref_cycles;
  thread->base_preempted_from_bpf = raw->preempted_from_bpf;

  /* The session's counters are shared by every thread sampling it, so
   * they are incremented atomically. A plain ++ is a read-modify-write,
   * and two threads doing it at once lose an increment. */
  __atomic_fetch_add(&session->samples_taken, 1, __ATOMIC_RELAXED);
  if (flags & PACE_SAMPLE_F_DEGRADED) {
    __atomic_fetch_add(&session->degraded_samples, 1, __ATOMIC_RELAXED);
  }

  return PACE_OK;
}

pace_status pace_session_sample(pace_session *session, pace_thread *thread,
                                 pace_sample *out_sample) {
  if (!session || !out_sample) return PACE_ERR_INVALID_ARG;
  if (!thread) return pace_fail(PACE_ERR_INVALID_ARG, 0);
  /* A thread handle belongs to exactly one session. Passing one session's
   * handle to another would write the second session's baselines into the
   * first session's thread state, corrupting both. */
  if (thread->session != session) return pace_fail(PACE_ERR_INVALID_ARG, 0);

  out_sample->struct_size = (uint32_t)sizeof(pace_sample);

  pace_raw raw;
  pace_status st = session->backend->thread_read(session, thread, &raw);
  if (st != PACE_OK) {
    __atomic_fetch_add(&session->io_errors, 1, __ATOMIC_RELAXED);
    return pace_fail(st, errno);
  }

  st = pace_fill_sample(session, thread, &raw, out_sample);
  if (st != PACE_OK) return st;

  if (session->observer) session->observer(session->observer_data, out_sample);
  return PACE_OK;
}

pace_status pace_session_sample_self(pace_session *session, pace_sample *out_sample) {
  if (!session) return PACE_ERR_INVALID_ARG;
  /* The thread-local handle is only valid while both the session that
   * created it and the thread that registered it are alive.
   *
   * A thread that has exited leaves its handle behind, and the kernel will
   * eventually recycle its thread id for a new thread, which would then
   * inherit the dead thread's baselines. The owner check catches that.
   *
   * A destroyed session frees the handle and the session. The session id is
   * held in thread-local storage, which outlives both, so a stale handle is
   * recognised without dereferencing freed memory.
   */
  pace_thread *thread = pace_tls_thread;
  if (pace_tls_session_id != session->id || !thread) {
    pace_status st = pace_session_register_thread(session, &thread);
    if (st != PACE_OK) return st;
  } else if (!pthread_equal(thread->owner, pthread_self())) {
    /* The id matches but this is a different thread: the kernel recycled
     * the thread id. Register again, which reclaims the dead thread's
     * slot. */
    pace_status st = pace_session_register_thread(session, &thread);
    if (st != PACE_OK) return st;
  }
  return pace_session_sample(session, thread, out_sample);
}

pace_status pace_session_set_observer(pace_session *session, pace_observer_fn fn,
                                      void *user_data) {
  if (!session) return PACE_ERR_INVALID_ARG;
  session->observer = fn;
  session->observer_data = user_data;
  return PACE_OK;
}

pace_source pace_session_source(const pace_session *session) {
  return session ? session->source : PACE_SOURCE_UNKNOWN;
}

pace_status pace_session_stats(const pace_session *session, pace_stats *out_stats) {
  if (!session || !out_stats) return PACE_ERR_INVALID_ARG;
  /* The struct_size field lets a caller built against an older header ask
   * for a struct this build does not know how to fill. A size that is not
   * the one this build compiled is a caller bug, not something to guess
   * around. */
  if (out_stats->struct_size != 0 && out_stats->struct_size != sizeof(pace_stats)) {
    return PACE_ERR_INVALID_ARG;
  }

  memset(out_stats, 0, sizeof *out_stats);
  out_stats->struct_size = (uint32_t)sizeof(pace_stats);
  out_stats->source = session->source;
  out_stats->threads_registered = (uint32_t)session->thread_count;
  out_stats->samples_taken = __atomic_load_n(&session->samples_taken, __ATOMIC_RELAXED);
  out_stats->degraded_samples = __atomic_load_n(&session->degraded_samples, __ATOMIC_RELAXED);
  out_stats->io_errors = __atomic_load_n(&session->io_errors, __ATOMIC_RELAXED);
  out_stats->ebpf_available = (uint32_t)session->bpf_available;
  return PACE_OK;
}

double pace_session_reference_hz(const pace_session *session) {
  return session ? session->reference_hz : 0.0;
}

int pace_format_cycles(uint64_t cycles, char *buffer, size_t buffer_size) {
  if (!buffer || buffer_size == 0) return 0;
  static const char *units[] = {"cycles", "Kcycles", "Mcycles", "Gcycles", "Tcycles"};
  double value = (double)cycles;
  int unit = 0;
  while (value >= 1000.0 && unit < 4) {
    value /= 1000.0;
    unit++;
  }
  int n = snprintf(buffer, buffer_size, unit == 0 ? "%.0f %s" : "%.2f %s", value, units[unit]);
  if (n < 0 || (size_t)n >= buffer_size) return 0;
  return n;
}
