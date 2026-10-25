#!/bin/sh
# Host unit test for the Bluepad32 -> NES_PAD_* mapping (no ESP-IDF, no board).
# Usage: ./run_host_test.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-gcc}
"$CC" -std=c11 -Wall -Wextra -Werror \
    -I "$REPO/components/gamepad" -I "$REPO/components/common/include" \
    "$HERE/map_test.c" "$REPO/components/gamepad/gamepad_map.c" \
    -o "$OUT/map_test"
"$OUT/map_test"
