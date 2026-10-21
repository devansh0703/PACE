/* SPDX-License-Identifier: BUSL-1.1
 * PACE — C++17 RAII wrapper.
 */
#ifndef PACE_PACE_HPP
#define PACE_PACE_HPP

#include "pace/pace.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

namespace pace {

/** Thrown by the C++ wrapper when a C API call fails. */
class error : public std::runtime_error {
 public:
  error(pace_status status, const std::string &what)
      : std::runtime_error(what), status_(status) {}
  pace_status status() const noexcept { return status_; }

 private:
  pace_status status_;
};

namespace detail {
inline void check(pace_status status, const char *what) {
  if (status != PACE_OK) {
    throw error(status, std::string(what) + ": " + pace_status_string(status));
  }
}
}  // namespace detail

/**
 * A measurement session.
 *
 * Owns the underlying pace_session and destroys it on scope exit.
 */
class Session {
 public:
  /**
   * Create a session.
   *
   * A zero-initialised pace_config is filled in with the library defaults,
   * so `Session session{};` is the normal construction.
   *
   * @throws error when the platform cannot support a session.
   */
  explicit Session(const pace_config &config = pace_config{}) {
    pace_config cfg = config;
    if (cfg.struct_size == 0) {
      detail::check(pace_config_init(&cfg), "pace_config_init");
    }
    detail::check(pace_session_create(&cfg, &session_), "pace_session_create");
  }

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  Session(Session &&other) noexcept : session_(other.session_) { other.session_ = nullptr; }
  Session &operator=(Session &&other) noexcept {
    if (this != &other) {
      pace_session_destroy(session_);
      session_ = other.session_;
      other.session_ = nullptr;
    }
    return *this;
  }

  ~Session() { pace_session_destroy(session_); }

  pace_session *get() const noexcept { return session_; }

  /** The tier this session obtained. */
  pace_source source() const noexcept { return pace_session_source(session_); }

  /** The measured reference frequency in Hz, or 0.0. */
  double reference_hz() const noexcept { return pace_session_reference_hz(session_); }

  /**
   * Take one sample of the calling thread.
   *
   * The first call establishes the baseline and returns a zero-duration
   * sample flagged PACE_SAMPLE_F_DEGRADED.
   */
  pace_sample sample() {
    pace_sample s{};
    detail::check(pace_session_sample_self(session_, &s), "pace_session_sample_self");
    return s;
  }

  /** Install an observer invoked for every sample. */
  template <typename Fn>
  void on_sample(Fn fn) {
    detail::check(pace_session_set_observer(session_, +[](void *user, const pace_sample *s) {
                  (*static_cast<Fn *>(user))(*s);
                }, &fn),
                  "pace_session_set_observer");
  }

 private:
  pace_session *session_;
};

namespace detail {
inline double pct(double part, double whole) { return whole > 0.0 ? part / whole : 0.0; }
}  // namespace detail

/**
 * A human-readable one-line summary of a sample.
 *
 * Format: "on_cpu=12.3ms preempted=210.0ms blocked=0.0ms (67.8% preempted)".
 */
inline std::string describe(const pace_sample &s) {
  char buffer[256];
  double wall = (double)s.wall_ns / 1e6;
  double on_cpu = (double)s.on_cpu_ns / 1e6;
  double preempted = (double)s.preempted_ns / 1e6;
  double blocked = (double)s.blocked_ns / 1e6;
  snprintf(buffer, sizeof buffer, "on_cpu=%.1fms preempted=%.1fms blocked=%.1fms (%.1f%% preempted)",
           on_cpu, preempted, blocked, detail::pct((double)s.preempted_ns, (double)s.wall_ns) * 100.0);
  return std::string(buffer);
}

}  // namespace pace

#endif /* PACE_PACE_HPP */
