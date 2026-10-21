# The vcpkg port for PACE.
#
# PACE is the SDK: a C library that attributes a thread's elapsed time
# into on-CPU, preempted and blocked time. The analyzer is a separate
# product and is not part of this port.
#
# The source is the release ASSET, not GitHub's auto-generated archive.
#
# The asset is built with `git archive` over the tagged tree, with fixed
# file times and a sorted file list, so it is byte-reproducible. That
# matters because this file records the asset's SHA512: if the asset
# changed whenever anything was committed, then recording a corrected
# hash would change the asset the hash describes.
#
# The port's own files live outside the asset's tree (the asset holds
# only pace-sdk/), so publishing a corrected hash does not change the
# asset.
if(NOT DEFINED VERSION OR NOT VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "the pace port needs a VERSION")
endif()

set(PACE_SOURCE_URL
  "https://github.com/devansh0703/PACE/releases/download/v${VERSION}/pace-sdk-${VERSION}.tar.gz")

# The SHA512 of that asset.
set(PACE_SHA512 bfbd6b7548b8d0e490d976c4338fda7d5c3e137d0275b9a362d47111a6c5f53e287d583dc179701823f98e4a7b17c895b12bb61664834117edf55fa9641dbc7a)

vcpkg_download_distfile(ARCHIVE
  URLS "${PACE_SOURCE_URL}"
  FILENAME "pace-sdk-${VERSION}.tar.gz"
  SHA512 ${PACE_SHA512})

# vcpkg_extract_source_archive strips the archive's top-level directory,
# so the extracted source is already at the root of SOURCE_PATH.
vcpkg_extract_source_archive(SOURCE_PATH
  ARCHIVE "${ARCHIVE}"
)

if(NOT EXISTS "${SOURCE_PATH}/CMakeLists.txt")
  message(FATAL_ERROR "the extracted asset has no CMakeLists.txt at ${SOURCE_PATH}")
endif()

if(NOT EXISTS "${SOURCE_PATH}/LICENSE")
  message(FATAL_ERROR "the extracted asset has no LICENSE at ${SOURCE_PATH}")
endif()

# The eBPF tier is off unless the port is asked for the "ebpf" feature.
# It needs libbpf, whose static link against libelf does not resolve
# reliably in vcpkg, and it is an addition rather than a requirement:
# the tier-0 and tier-1 measurements need none of it.
set(PACE_EBPF_OPTION OFF)
if("ebpf" IN_LIST FEATURES)
  set(PACE_EBPF_OPTION ON)
endif()

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
    -DPACE_BUILD_TESTS=OFF
    -DPACE_BUILD_BENCH=OFF
    -DPACE_BUILD_EXAMPLES=OFF
    -DPACE_ENABLE_BPF=${PACE_EBPF_OPTION}
)

vcpkg_cmake_install()

# The exported CMake config must not name a file that is not there.
#
# paceTargets.cmake names both the release and the debug library. Deleting
# the debug tree's lib directory while the config still references
# debug/lib/libpace.a makes every consumer fail to configure with
# "references the file ... but this file does not exist".
#
# So the debug *headers* and the debug *share* directory are removed, and
# the debug library stays, because the config names it.
vcpkg_cmake_config_fixup(PACKAGE_NAME pace CONFIG_PATH lib/cmake/pace)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
