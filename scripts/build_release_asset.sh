#!/usr/bin/env bash
# Build the release asset and print the hash the vcpkg port must record.
#
# The port's SHA512 describes this tarball, and the portfile lives in the
# source tree, so the port's own files are excluded from the asset. Without
# that exclusion, publishing a corrected hash would change the tarball it
# describes, and the port would be broken for every installer.
#
# Usage:
#   scripts/build_release_asset.sh <version>
#
# Prints the SHA512 on the last line, and writes the tarball to
# dist/pace-sdk-<version>.tar.gz.

set -euo pipefail

VERSION="${1:-}"
if [ -z "$VERSION" ]; then
  echo "usage: $0 <version>" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TAG="v${VERSION}"
if ! git rev-parse -q --verify "$TAG" >/dev/null; then
  echo "the tag $TAG does not exist" >&2
  exit 1
fi

DIST="$ROOT/dist"
mkdir -p "$DIST"
ASSET="$DIST/pace-sdk-${VERSION}.tar.gz"

# The port's own files are excluded, and so is anything the build would
# not need.
# The port's own files are excluded: the portfile holds the hash of this
# tarball, so including it would make the tarball depend on its own
# hash. The build script is excluded for the same reason: it is part of
# the release machinery, not of the library.
#
# The archive is built from the tag's TREE rather than the commit, with
# a fixed modification time and a sorted file list. `git archive` embeds
# the commit id and each file's mtime in the tar header, so an archive of
# the commit changes whenever anything is committed -- including the
# commit that records the hash. Building from the tree with fixed
# metadata makes the asset depend only on the source, so recording the
# hash does not change the asset.
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pace-release-XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT

git archive --format=tar --prefix="pace-sdk-${VERSION}/" "$TAG" \
  ':!vcpkg' ':!vcpkg.json' ':!dist' ':!scripts' | tar -x -C "$BUILD_DIR"

# A fixed timestamp for every file, and a sorted list, so the archive is
# byte-identical for identical content.
find "$BUILD_DIR" -exec touch -h -d "@0" {} +
( cd "$BUILD_DIR" && find . -type f -o -type l | LC_ALL=C sort \
    | tar --format=gnu --owner=0 --group=0 --numeric-owner --mtime="@0" \
        --transform "s|^\./|pace-sdk-${VERSION}/|" -T - -cf - ) \
  | gzip -n > "$ASSET"

SIZE=$(stat -c %s "$ASSET")
SHA=$(sha512sum "$ASSET" | cut -d' ' -f1)

echo "asset : $ASSET"
echo "size  : $SIZE bytes"
echo "sha512: $SHA"
echo ""
echo "record this in vcpkg/portfile.cmake as PACE_SHA512:"
echo "  set(PACE_SHA512 $SHA)"
