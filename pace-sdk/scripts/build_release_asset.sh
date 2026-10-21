#!/usr/bin/env bash
# Build the release asset and print the hash the vcpkg port must record.
#
# The port's SHA512 describes this tarball. The port lives in ports/pace/,
# OUTSIDE the asset's tree, so the asset does not contain the hash that
# describes it -- publishing a corrected hash does not change the asset.
#
# The asset is built from the TAG's tree with a fixed modification time
# and a sorted file list, so it is byte-reproducible. `git archive`
# embeds the commit id in the tar header, so an archive of the commit
# would change whenever anything was committed -- including the commit
# that records the hash.
#
# Usage:
#   pace-sdk/scripts/build_release_asset.sh <version>
#
# Prints the SHA512 on the last line, and writes the tarball to
# pace-sdk/dist/pace-sdk-<version>.tar.gz.

set -euo pipefail

VERSION="${1:-}"
if [ -z "$VERSION" ]; then
  echo "usage: $0 <version>" >&2
  exit 2
fi

# The script lives in pace-sdk/scripts, and the project it packages is
# the pace-sdk directory, which is a subdirectory of the single
# repository.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
cd "$REPO_ROOT"

TAG="v${VERSION}"
if ! git rev-parse -q --verify "$TAG" >/dev/null; then
  echo "the tag $TAG does not exist" >&2
  exit 1
fi

DIST="$ROOT/dist"
mkdir -p "$DIST"
ASSET="$DIST/pace-sdk-${VERSION}.tar.gz"

# The build directory holds the tagged tree, and dist/ is excluded
# because it holds the asset itself: including it would make the asset
# contain a copy of itself.
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

git archive --format=tar "$TAG" \
  'pace-sdk' \
  ':!pace-sdk/dist' \
  | tar -x -C "$BUILD_DIR"

# A fixed timestamp for every file, and a sorted list, so the archive is
# byte-identical for identical content.
find "$BUILD_DIR" -exec touch -h -d "@0" {} +
# The prefix is added here, and the project's own "pace-sdk/" directory
# is stripped, so the asset's layout is "pace-sdk-<version>/<paths>"
# with the paths as they are inside the project.
( cd "$BUILD_DIR" && find . -type f -o -type l | LC_ALL=C sort \
    | tar --format=gnu --owner=0 --group=0 --numeric-owner --mtime="@0" \
        --transform "s|^\./pace-sdk/|pace-sdk-${VERSION}/|" \
        --transform "s|^\./|pace-sdk-${VERSION}/|" -T - -cf - ) \
  | gzip -n > "$ASSET"

SIZE=$(stat -c %s "$ASSET")
SHA=$(sha512sum "$ASSET" | cut -d' ' -f1)

echo "asset : $ASSET"
echo "size  : $SIZE bytes"
echo "sha512: $SHA"
echo ""
echo "record this in ports/pace/portfile.cmake as PACE_SHA512:"
echo "  set(PACE_SHA512 $SHA)"
