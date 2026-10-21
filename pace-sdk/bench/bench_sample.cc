// SPDX-License-Identifier: BUSL-1.1
// PACE — per-sample overhead benchmark.
//
// Measures the cost of a single pace_session_sample() call, which is the
// number a vendor embedding PACE cares about.
#include <benchmark/benchmark.h>

#include "pace/pace.h"

#include <cstdint>
#include <vector>

namespace {

// The overhead of the call itself, with the session already warm.
void BM_SampleOverhead(benchmark::State &state) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *session = nullptr;
  if (pace_session_create(&cfg, &session) != PACE_OK) {
    state.SkipWithError("pace_session_create failed");
    return;
  }

  pace_sample sample{};
  // Warm up: the first sample only establishes the baseline.
  pace_session_sample_self(session, &sample);

  for (auto _ : state) {
    pace_session_sample_self(session, &sample);
    benchmark::DoNotOptimize(&sample);
  }

  pace_session_destroy(session);
}
BENCHMARK(BM_SampleOverhead)->UseRealTime()->Unit(benchmark::kNanosecond);

// The cost of creating and destroying a session, which a vendor pays once
// per profiled process rather than per sample.
void BM_SessionLifecycle(benchmark::State &state) {
  pace_config cfg;
  pace_config_init(&cfg);
  for (auto _ : state) {
    pace_session *session = nullptr;
    if (pace_session_create(&cfg, &session) != PACE_OK) {
      state.SkipWithError("pace_session_create failed");
      return;
    }
    pace_session_destroy(session);
  }
}
BENCHMARK(BM_SessionLifecycle)->Unit(benchmark::kMicrosecond);

// The cost of registering a thread, which a vendor pays once per thread.
void BM_ThreadRegistration(benchmark::State &state) {
  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *session = nullptr;
  if (pace_session_create(&cfg, &session) != PACE_OK) {
    state.SkipWithError("pace_session_create failed");
    return;
  }
  pace_thread *handle = nullptr;
  pace_session_register_thread(session, &handle);
  for (auto _ : state) {
    pace_session_register_thread(session, &handle);
    benchmark::DoNotOptimize(&handle);
  }
  pace_session_destroy(session);
}
BENCHMARK(BM_ThreadRegistration)->Unit(benchmark::kMicrosecond);

}  // namespace

BENCHMARK_MAIN();
