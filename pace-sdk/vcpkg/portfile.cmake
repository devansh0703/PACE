# The version this port builds. vcpkg_from_github's REF must match it, and
# a mismatch means the port is building something other than what it says.
if(NOT DEFINED VERSION OR NOT VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "the pace port needs a VERSION")
endif()

# The source is the release ASSET, not GitHub's auto-generated archive.
#
# The asset is built with `git archive --prefix=pace-sdk-<version>/`, which
# is reproducible and does not contain this portfile. That is what breaks
# the circular dependency: the port's hash describes a tarball that does
# not change when the portfile changes. GitHub's own archive honours
# .gitattributes but is not byte-reproducible, so a hash of it would break
# as soon as the port is corrected.
set(PACE_SOURCE_URL
  "https://github.com/devansh0703/PACE/releases/download/v${VERSION}/pace-sdk-${VERSION}.tar.gz")

# The SHA512 of that asset.
set(PACE_SHA512 31395e7d2c7e408301d40b340a209346618f9d013996dbfa97af4b00600444f39f2215bb86d64dfd552364e09c7fffd5bc39987be27a8d968f575714493ed5f0)

vcpkg_download_distfile(ARCHIVE
  URLS "${PACE_SOURCE_URL}"
  FILENAME "pace-sdk-${VERSION}.tar.gz"
  SHA512 ${PACE_SHA512})

vcpkg_extract_source_archive(SOURCE_PATH
  ARCHIVE "${CURRENT_BUILDTREES_DIR}/src/pace-sdk-${VERSION}.tar.gz"
)

# The release asset is built with:
#
#   git archive --format=tar --prefix=pace-sdk-<version>/ v<version> \
#     ':!vcpkg' ':!vcpkg.json'
#
# The port's own files are excluded from the asset, so the hash the port
# records describes a tarball that does not change when the portfile
# changes. Without that, publishing a corrected hash would change the
# tarball it describes.

# The eBPF tier is off unless the port is asked for the "ebpf" feature.
# It needs libbpf, whose static link against libelf does not resolve
# reliably in vcpkg, and it is an addition rather than a requirement: the
# tier-0 and tier-1 measurements need none of it.
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
