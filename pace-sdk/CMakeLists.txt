cmake_minimum_required(VERSION 3.24)
project(pace
  VERSION 0.1.0
  DESCRIPTION "Preemption-Aware Cycle Attribution Engine"
  # The library itself is C11. C++ is enabled for the Google Benchmark
  # harnesses and the C++ example, which live in this tree.
  LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(PACE_BUILD_TESTS "Build the unit and verification tests" ON)
option(PACE_BUILD_BENCH "Build the Google Benchmark harnesses" ON)
option(PACE_BUILD_EXAMPLES "Build the examples" ON)
option(PACE_ENABLE_PERF_COUNTERS "Allow the tier-1 hardware counter backend" ON)
option(PACE_ENABLE_BPF "Build the eBPF scheduler tier" ON)

# Sanitizers. The concurrency test is the reason the thread sanitizer
# matters: the library promises that pace_session_sample is safe to call
# from several threads at once, and a data race there produces a wrong
# number rather than a crash.
set(PACE_SANITIZER "none" CACHE STRING "Sanitizer to build with: none, address, thread, undefined")
set_property(CACHE PACE_SANITIZER PROPERTY STRINGS none address thread undefined)

if(PACE_SANITIZER STREQUAL "address")
  set(PACE_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
elseif(PACE_SANITIZER STREQUAL "thread")
  set(PACE_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
elseif(PACE_SANITIZER STREQUAL "undefined")
  set(PACE_SANITIZER_FLAGS -fsanitize=undefined -fno-omit-frame-pointer)
else()
  set(PACE_SANITIZER_FLAGS "")
endif()

if(PACE_SANITIZER_FLAGS)
  message(STATUS "PACE: building with the ${PACE_SANITIZER} sanitizer")
endif()

# The eBPF tier is Linux-only.
if(PACE_ENABLE_BPF AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "PACE: the eBPF tier is Linux-only; disabling")
  set(PACE_ENABLE_BPF OFF)
endif()

# The tier-1 backend needs perf_event_open, which is Linux-only.
if(PACE_ENABLE_PERF_COUNTERS AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "PACE: hardware counters are Linux-only; disabling tier 1")
  set(PACE_ENABLE_PERF_COUNTERS OFF)
endif()

# The platform backend. Each backend file is guarded by its own preprocessor
# block, so the build is safe whichever one is listed, but the right one is
# still selected explicitly so the configuration says what it did.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(PACE_BACKEND_SOURCE src/pace_linux.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(PACE_BACKEND_SOURCE src/pace_windows.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(PACE_BACKEND_SOURCE src/pace_macos.c)
else()
  message(WARNING "PACE: no backend for ${CMAKE_SYSTEM_NAME}; the library will "
                  "report PACE_ERR_UNSUPPORTED at run time")
  set(PACE_BACKEND_SOURCE src/pace_linux.c)
endif()
message(STATUS "PACE: backend ${PACE_BACKEND_SOURCE}")

set(PACE_SOURCES
  src/pace_session.c
  ${PACE_BACKEND_SOURCE}
  src/pace_clock.c
  src/pace_trace.c
  src/pace_version.c)

add_library(pace ${PACE_SOURCES})
add_library(pace::pace ALIAS pace)

if(PACE_ENABLE_BPF)
  add_subdirectory(src/bpf)
  target_sources(pace PRIVATE src/bpf/pace_bpf.c)
  target_compile_definitions(pace PRIVATE PACE_WITH_BPF=1)
  # The generated skeleton header lives in the bpf subdirectory's binary
  # directory. Set here because a variable set inside add_subdirectory does
  # not propagate back to the calling scope.
  target_include_directories(pace PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/src/bpf)
  # The generated skeleton header must exist before pace_bpf.c compiles.
  add_dependencies(pace pace_bpf_skeleton)
endif()

target_include_directories(pace
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

# PACE_BUILDING is PUBLIC, not PRIVATE: on Windows a consumer of the
# static library resolves the symbols at link time and must not ask for the
# DLL import stub. A consumer of the DLL does not define it, and gets the
# import attribute.
target_compile_definitions(pace PUBLIC PACE_BUILDING)

# The DLL build exports the symbols instead.
if(BUILD_SHARED_LIBS)
  target_compile_definitions(pace PRIVATE PACE_BUILD_SHARED)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(pace PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -fvisibility=hidden)
endif()

if(PACE_SANITIZER_FLAGS)
  target_compile_options(pace PUBLIC ${PACE_SANITIZER_FLAGS})
  target_link_options(pace PUBLIC ${PACE_SANITIZER_FLAGS})
endif()

find_package(Threads REQUIRED)
target_link_libraries(pace PUBLIC Threads::Threads)

if(PACE_ENABLE_BPF)
  find_library(PACE_LIBBPF NAMES bpf REQUIRED)
  target_link_libraries(pace PRIVATE ${PACE_LIBBPF})
  target_include_directories(pace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/bpf)
endif()

if(PACE_ENABLE_PERF_COUNTERS)
  target_compile_definitions(pace PRIVATE PACE_WITH_PERF=1)
endif()

# Symbol visibility: only PACE_API symbols are exported.
set(PACE_VERSION_SOVERSION 0)
set_target_properties(pace PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PACE_VERSION_SOVERSION}
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON
  PUBLIC_HEADER include/pace/pace.h)

# ---------------------------------------------------------------- install

install(TARGETS pace
  EXPORT paceTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pace)

install(EXPORT paceTargets
  FILE paceTargets.cmake
  NAMESPACE pace::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pace)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/paceConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/paceConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pace)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/paceConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/paceConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/paceConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pace)

# ----------------------------------------------------------------- tests

if(PACE_BUILD_TESTS)
  enable_testing()
  add_executable(pace_unit_tests tests/unit/test_main.c)
  target_link_libraries(pace_unit_tests PRIVATE pace)
  add_test(NAME pace_unit_tests COMMAND pace_unit_tests)

  # The session-lifecycle defects the engineering review found. Kept in a
  # separate binary so the failure names the area, not just a check number.
  # The Linux measurement primitives, tested directly rather than only
  # through a whole sample.
  add_executable(pace_primitive_tests tests/unit/test_primitives.c)
  target_link_libraries(pace_primitive_tests PRIVATE pace)
  target_include_directories(pace_primitive_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME pace_primitive_tests COMMAND pace_primitive_tests)

  # The concurrency test, which is the reason the thread sanitizer option
  # exists: the library promises that sampling is safe from several
  # threads at once, and that promise has never been checked under a
  # thread sanitizer.
  add_executable(pace_concurrency_tests tests/unit/test_concurrency.c)
  target_link_libraries(pace_concurrency_tests PRIVATE pace)
  target_include_directories(pace_concurrency_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME pace_concurrency_tests COMMAND pace_concurrency_tests)

  add_executable(pace_lifecycle_tests tests/unit/test_lifecycle.c)
  target_link_libraries(pace_lifecycle_tests PRIVATE pace)
  # The lifecycle test inspects the internal thread table, so it needs the
  # internal headers on the include path.
  target_include_directories(pace_lifecycle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME pace_lifecycle_tests COMMAND pace_lifecycle_tests)

  # The verification programs read the Linux kernel's own interfaces
  # directly, so they are Linux-only. The unit tests are portable and run
  # on every platform.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(pace_premise_probe tests/verify/premise_probe.c)
    target_link_libraries(pace_premise_probe PRIVATE pace)

    add_executable(pace_verify tests/verify/pace_verify.c)
    target_link_libraries(pace_verify PRIVATE pace)

    # The per-call cost of each measurement primitive, so the published
    # overhead figure can be attributed rather than merely stated.
    add_executable(pace_primitive_costs tests/verify/primitive_costs.c)
    target_link_libraries(pace_primitive_costs PRIVATE pace)
  endif()
endif()

if(PACE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(PACE_BUILD_BENCH)
  add_subdirectory(bench)
endif()
