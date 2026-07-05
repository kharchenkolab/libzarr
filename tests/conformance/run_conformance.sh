#!/usr/bin/env bash
# Cross-implementation conformance, both directions:
#   zarr-python writes -> libzarr reads;  libzarr writes -> zarr-python reads.
set -euo pipefail
PYTHON=$1
TOOL=$2
WORK=$3
HERE=$(cd "$(dirname "$0")" && pwd)

rm -rf "$WORK"
mkdir -p "$WORK"

"$PYTHON" "$HERE/write_fixtures.py" "$WORK/from_python"
"$TOOL" read "$WORK/from_python"

"$TOOL" write "$WORK/from_libzarr"
"$PYTHON" "$HERE/read_back.py" "$WORK/from_libzarr"

echo "conformance OK (both directions)"
