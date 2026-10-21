/* SPDX-License-Identifier: BUSL-1.1
 * PACE — the platform-neutral clock helpers.
 *
 * These are the helpers that do not depend on the platform. The per-platform
 * clock sources live in the backends: pace_now_monotonic_ns,
 * pace_now_thread_cpu_ns, pace_now_ref_cycles and pace_reference_hz.
 */
#include "pace_backend.h"

uint64_t pace_scale_multiplexed(uint64_t value, uint64_t time_enabled,
                                uint64_t time_running) {
  if (time_running == 0 || time_running >= time_enabled) return value;
  /* value * time_enabled / time_running, in 128-bit to avoid overflow. */
  __uint128_t scaled = (__uint128_t)value * time_enabled;
  scaled /= time_running;
  return (uint64_t)scaled;
}

/**
 * Scale a duration into reference cycles at a measured frequency.
 *
 * The result is a *derived* quantity: the reference clock is invariant, but
 * the frequency is a measurement, so the conversion carries the frequency's
 * own error. A caller that needs a measured cycle count must use the
 * hardware counter, which is what PACE_SOURCE_PERF reports.
 */
uint64_t pace_ns_to_ref_cycles(uint64_t ns, double hz) {
  if (hz <= 0.0) return 0;
  return (uint64_t)((double)ns * hz / 1e9);
}
