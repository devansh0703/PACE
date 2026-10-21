// SPDX-License-Identifier: BUSL-1.1
// PACE — end-to-end instrumentation overhead.
//
// Measures the cost of instrumenting a realistic multi-threaded workload:
// a worker pool running a CPU-bound task, with PACE sampling every worker
// on every iteration. The reported number is the overhead a customer's
// application would actually pay.
#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "pace/pace.h"

namespace {

constexpr int kWorkers = 4;
constexpr int kIterations = 64;

// The workload under measurement: a mix of integer and floating-point work
// that does not get optimised away.
uint64_t workload(int iterations) {
  uint64_t acc = 0;
  double d = 1.0;
  for (int i = 0; i < iterations; i++) {
    acc += (uint64_t)i * 2654435761u;
    d = d * 1.0000001 + 1e-9;
  }
  benchmark::DoNotOptimize(acc);
  benchmark::DoNotOptimize(d);
  return acc;
}

void run_workload(bool instrumented) {
  std::vector<std::thread> threads;
  std::atomic<uint64_t> total{0};

  pace_config cfg;
  pace_config_init(&cfg);
  pace_session *session = nullptr;
  if (instrumented && pace_session_create(&cfg, &session) != PACE_OK) {
    session = nullptr;
  }

  for (int w = 0; w < kWorkers; w++) {
    threads.emplace_back([&total, instrumented, session]() {
      pace_thread *handle = nullptr;
      if (instrumented) {
        pace_session_register_thread(session, &handle);
        pace_sample sample{};
        pace_session_sample(session, handle, &sample);
      }
      uint64_t local = 0;
      for (int i = 0; i < kIterations; i++) {
        local += workload(256);
        if (instrumented) {
          pace_sample sample{};
          pace_session_sample(session, handle, &sample);
        }
      }
      total += local;
    });
  }
  for (auto &t : threads) t.join();
  benchmark::DoNotOptimize(total.load());
  pace_session_destroy(session);
}

void BM_WorkloadBaseline(benchmark::State &state) {
  for (auto _ : state) {
    run_workload(false);
  }
}
BENCHMARK(BM_WorkloadBaseline)->Unit(benchmark::kMillisecond);

void BM_WorkloadInstrumented(benchmark::State &state) {
  for (auto _ : state) {
    run_workload(true);
  }
}
BENCHMARK(BM_WorkloadInstrumented)->Unit(benchmark::kMillisecond);

}  // namespace

BENCHMARK_MAIN();
