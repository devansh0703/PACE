# The examples use the internal backend header for the portable thread id
# helper, so the internal include path is set once here.
add_executable(pace_thread_attribution thread_attribution.c)
target_link_libraries(pace_thread_attribution PRIVATE pace)
target_include_directories(pace_thread_attribution PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(pace_record pace_record.c)
target_link_libraries(pace_record PRIVATE pace)
target_include_directories(pace_record PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The recording demo reports the tier's own cumulative value for its
# workers, which it reads from the tier's map. That needs the internal
# eBPF header and the PACE_WITH_BPF define.
#
# The define describes the BUILD, not the ABI: it says the tier was
# compiled in. It is PRIVATE on the library, so the examples that want
# it are given it here, and only when the tier is actually available.
if(PACE_ENABLE_BPF AND PACE_HAVE_BPF)
  target_compile_definitions(pace_record PRIVATE PACE_WITH_BPF=1)
endif()

add_executable(pace_overhead pace_overhead.c)
target_link_libraries(pace_overhead PRIVATE pace)

# The eBPF probe needs the BPF object and the scheduler tracepoints, which
# exist only on Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(pace_bpf_probe pace_bpf_probe.c)
  target_link_libraries(pace_bpf_probe PRIVATE pace)
endif()
