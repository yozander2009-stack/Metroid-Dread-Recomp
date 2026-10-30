#!/bin/bash
# Linux build: replay a Windows test route (frame-indexed CSV) with fresh COPIES of the test saves (make_test_game.sh).
# Usage: run_route_linux.sh LABEL ROUTE.csv EXIT_FRAME CAPTURES(comma) [extra env...]
# Output in linux/test/runs/LABEL. Runs on the real GPU; SWR_SOFTGL=1 uses Mesa's software OpenGL instead.
set -e
[ $# -ge 4 ] || { sed -n '3p' "$0" | sed 's/^# //'; exit 1; }
LABEL=$1; ROUTE=$(readlink -f "$2"); EXITF=$3; CAPS=$4; shift 4
source "$(dirname "$(readlink -f "$0")")/env.sh"
bash "$W/linux/make_test_game.sh" > /dev/null
G="$TEST/game"
OUT="$TEST/runs/$LABEL"
mkdir -p "$OUT"
cd "$G"
rm -f "$NAME.log"
softgl
export SWR_TEST_SCRIPT="$ROUTE" SWR_TEST_EXIT_FRAME=$EXITF SWR_TEST_CAPTURE_FRAMES=$CAPS SWR_TEST_CAPTURE_PREFIX="$OUT/image"
for kv in "$@"; do export "$kv"; done
timeout 1500 "./$NAME" --fullscreen=0 > "$OUT/stdout.txt" 2>&1 || echo "exit code $?"
cp "$NAME.log" "$OUT/"
grep -h "test complete\|test capture\|gpu: frames=" "$OUT/$NAME.log" | tail -4
ls "$OUT"
