# Google Benchmark is fetched rather than vendored: it is a test-only
# dependency, and FetchContent keeps the repository free of a third-party
# source tree while pinning the exact version used for the published numbers.
include(FetchContent)

set(PACE_BENCHMARK_VERSION v1.10.0 CACHE STRING "Google Benchmark version to build against")
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG ${PACE_BENCHMARK_VERSION}
  GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(googlebenchmark)

add_executable(pace_bench_sample bench_sample.cc)
target_link_libraries(pace_bench_sample PRIVATE pace benchmark::benchmark)

add_executable(pace_bench_instrumented bench_instrumented.cc)
target_link_libraries(pace_bench_instrumented PRIVATE pace benchmark::benchmark)
