# PACE eBPF tier build.
#
# The BPF program is compiled with clang's BPF target against a vmlinux.h
# generated from the running kernel's BTF, then a skeleton is generated
# with bpftool. This is the standard CO-RE workflow, and it means the
# program is built against the kernel it will actually run on.
#
# The tier is optional: if bpftool or clang's BPF target is missing, the
# SDK builds without it and the procfs tier carries the measurement.
cmake_minimum_required(VERSION 3.24)

find_program(PACE_BPFTOOL NAMES bpftool)
find_program(PACE_CLANG NAMES clang clang-18 clang-17 clang-16)

# On a distribution where bpftool lives in the linux-tools package, the
# installed /usr/sbin/bpftool is a wrapper that runs the version matching
# the running kernel. On a CI runner the matching package is not
# installed, so the wrapper exits non-zero even though it is present.
#
# A present-but-broken bpftool is worse than a missing one: the build
# fails at the first vmlinux.h generation, long after the tier was
# decided to be enabled. So the tool is exercised, and the tier is
# disabled if it does not work.
if(PACE_BPFTOOL)
  execute_process(
    COMMAND ${PACE_BPFTOOL} version
    RESULT_VARIABLE PACE_BPFTOOL_RC
    OUTPUT_QUIET
    ERROR_QUIET)
  if(NOT PACE_BPFTOOL_RC EQUAL 0)
    message(STATUS "PACE: bpftool at ${PACE_BPFTOOL} does not run; disabling the eBPF tier")
    set(PACE_BPFTOOL "")
  endif()
endif()

# clang's BPF target is what compiles the program, and a clang without it
# is as useless as a missing clang.
if(PACE_CLANG)
  execute_process(
    COMMAND ${PACE_CLANG} -target bpf -x c -c /dev/null -o /dev/null
    RESULT_VARIABLE PACE_CLANG_BPF_RC
    OUTPUT_QUIET
    ERROR_QUIET)
  if(NOT PACE_CLANG_BPF_RC EQUAL 0)
    message(STATUS "PACE: clang at ${PACE_CLANG} has no BPF target; disabling the eBPF tier")
    set(PACE_CLANG "")
  endif()
endif()

if(PACE_BPFTOOL AND PACE_CLANG)
  set(PACE_VMLINUX_H ${CMAKE_CURRENT_BINARY_DIR}/vmlinux.h)
  set(PACE_BPF_OBJ ${CMAKE_CURRENT_BINARY_DIR}/pace_sched.bpf.o)
  set(PACE_BPF_SKEL_H ${CMAKE_CURRENT_BINARY_DIR}/pace_sched.skel.h)

  # Generate vmlinux.h from the running kernel's BTF.
  add_custom_command(
    OUTPUT ${PACE_VMLINUX_H}
    COMMAND ${PACE_BPFTOOL} btf dump file /sys/kernel/btf/vmlinux format c > ${PACE_VMLINUX_H}
    DEPENDS /sys/kernel/btf/vmlinux
    COMMENT "Generating vmlinux.h from the running kernel's BTF"
    VERBATIM)

  # Compile the BPF program and generate its libbpf skeleton.
  add_custom_command(
    OUTPUT ${PACE_BPF_SKEL_H}
    COMMAND ${PACE_CLANG} -g -O2 -target bpf -D__TARGET_ARCH_x86
            -Wno-missing-declarations -Wno-missing-prototypes -Wno-compare-distinct-pointer-types
            -I${CMAKE_CURRENT_SOURCE_DIR}
            -I${CMAKE_CURRENT_BINARY_DIR}
            -c ${CMAKE_CURRENT_SOURCE_DIR}/pace_sched.bpf.c
            -o ${PACE_BPF_OBJ}
    COMMAND ${PACE_BPFTOOL} gen skeleton ${PACE_BPF_OBJ} > ${PACE_BPF_SKEL_H}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pace_sched.bpf.c ${PACE_VMLINUX_H}
    COMMENT "Building the PACE eBPF scheduler program"
    VERBATIM)

  # A custom target that depends on the generated header, so the main
  # library's compile step waits for it.
  add_custom_target(pace_bpf_skeleton DEPENDS ${PACE_BPF_SKEL_H})

  set(PACE_BPF_SKEL_INCLUDE ${CMAKE_CURRENT_BINARY_DIR})
  set(PACE_HAVE_BPF ON)
  message(STATUS "PACE: eBPF tier enabled (clang ${PACE_CLANG}, bpftool ${PACE_BPFTOOL})")
else()
  set(PACE_BPF_SKEL_INCLUDE "")
  set(PACE_HAVE_BPF OFF)
  message(STATUS "PACE: eBPF tier disabled (clang with a BPF target and bpftool are required)")
endif()

# add_subdirectory gives the subdirectory its own scope, so the parent
# cannot see a variable set here. PARENT_SCOPE is how a subdirectory
# reports a decision back to the caller.
set(PACE_HAVE_BPF ${PACE_HAVE_BPF} PARENT_SCOPE)
