#!/usr/bin/env bash
# Proves the install/export/config plumbing: install libzarr into a throwaway
# prefix, then configure + build + run a standalone consumer that finds it only
# through find_package(libzarr CONFIG). This is exactly what the vcpkg port and
# any downstream user rely on.
#
#   run_packaging_test.sh <source_dir> <work_dir> [extra libzarr cmake args...]
#
# e.g.  run_packaging_test.sh . /tmp/pkg -DLIBZARR_WITH_ZLIB=ON
set -euo pipefail

SRC=$(cd "$1" && pwd)
WORK=$2
shift 2
PREFIX=$WORK/prefix

rm -rf "$WORK"
mkdir -p "$WORK"

# 1. Configure + install libzarr (header-only; the dev harness is irrelevant here).
cmake -S "$SRC" -B "$WORK/build" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DLIBZARR_BUILD_TESTS=OFF \
  "$@"
cmake --install "$WORK/build"

# 2. Configure + build the consumer against the installed package only. The
#    install prefix goes first on CMAKE_PREFIX_PATH; any inherited prefix (where
#    zlib/zstd/blosc live) stays available so find_dependency() resolves them.
cmake -S "$SRC/tests/packaging" -B "$WORK/consumer" \
  -DCMAKE_PREFIX_PATH="$PREFIX;${CMAKE_PREFIX_PATH:-}"
cmake --build "$WORK/consumer"

# 3. Run it.
"$WORK/consumer/consumer"
echo "packaging test OK"
