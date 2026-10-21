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
