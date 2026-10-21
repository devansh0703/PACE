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
set(PACE_SHA512 9952a131dbc57e56fd84de0877953a99456df953c03ea8255da228e64926ceec234846041e381f7448641e0659ee82c77a674cdb7fea3f5e95a1df9958a25693)

vcpkg_download_distfile(ARCHIVE
  URLS "${PACE_SOURCE_URL}"
  FILENAME "pace-sdk-${VERSION}.tar.gz"
  SHA512 ${PACE_SHA512})

# vcpkg stores a downloaded file under a mangled name when the filename
# is long: "pace-sdk-0.1.0.tar.gz" became
# "pace-sdk-0-31395e7d.1.0.tar.gz", with the hash inserted and the name
# truncated. The file is in the downloads directory, and vcpkg_download_
# distfile sets ARCHIVE to the path it used, so that is the path to
# extract from.
# vcpkg_extract_source_archive strips the archive's top-level directory,
# so the extracted source is already at the root.
vcpkg_extract_source_archive(SOURCE_PATH
  ARCHIVE "${ARCHIVE}"
)

if(NOT EXISTS "${SOURCE_PATH}/CMakeLists.txt")
  message(FATAL_ERROR "the extracted asset has no CMakeLists.txt at ${SOURCE_PATH}")
endif()

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
