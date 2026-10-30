#!/bin/bash
# Steam Deck test through the front end (over SSH, Desktop Mode): starts run.sh as a player does, the Play screen comes
# up, the test hook presses Play, then the game replays a route with fresh COPIES of the test saves.
# Usage: deck_fe_route.sh LABEL ROUTE.csv EXIT CAPTURES(comma or "") [KEY=VALUE env ...]
#   EXIT: a frame number (frame routes) or "ms:N" (time routes, SWR_TEST_SCRIPT_MS)
# Optional env: DECK_INI=file (copied as the game .ini), DECK_SAVE=dir (holds save/)
# Layout: ~/dread-test/fe (package; Metroid Dread_data/exefs,romfs linked to the older package's)
LABEL=$1; ROUTE=$2; EXIT=$3; CAPS=$4; shift 4
T=$HOME/dread-test; G=$T/fe; OUT=$T/runs/$LABEL
mkdir -p "$OUT"
rm -rf "$G/Metroid Dread_data/save"
cp -r "${DECK_SAVE:-$T/saves}/save" "$G/Metroid Dread_data/save"
[ -n "$DECK_INI" ] && cp "$DECK_INI" "$G/Metroid Dread.ini"
export DISPLAY=:0 XAUTHORITY=$(ls /run/user/$(id -u)/xauth_* 2>/dev/null | head -1)
cd "$G"
rm -f "Metroid Dread.log"
export SWR_FE_TEST=1 SWR_FE_PLAY_FRAME=90 SWR_TEST_SCRIPT="$ROUTE" SWR_TRACE_PRESENTS="$OUT/presents.csv" SWR_TEST_LAUNCHER=1
case "$EXIT" in ms:*) export SWR_TEST_SCRIPT_MS=1 SWR_TEST_EXIT_MS=${EXIT#ms:};; *) export SWR_TEST_EXIT_FRAME=$EXIT;; esac
[ -n "$CAPS" ] && export SWR_TEST_CAPTURE_FRAMES=$CAPS SWR_TEST_CAPTURE_PREFIX="$OUT/image"
for kv in "$@"; do export "$kv"; done
start=$(date +%s)
timeout 2400 ./run.sh > "$OUT/stdout.txt" 2>&1
code=$?
echo "exit code $code after $(( $(date +%s) - start )) s"
cp "Metroid Dread.log" "Metroid Dread.ini" "$OUT/" 2>/dev/null
grep -h "game continues\|front end\|test complete\|inside the recompiled\|fatal\|particle pool" "$OUT/Metroid Dread.log" | head -6
python3 - "$OUT/presents.csv" <<'EOF'
import csv, sys
iv = sorted(float(r['interval_ms']) for r in csv.DictReader(open(sys.argv[1])))[1:]
n = len(iv)
if n: print('%d frames, mean %.2f ms (%.1f FPS), p99 %.2f ms, max %.1f ms, >16.7 ms %d, >33 ms %d, >50 ms %d' % (
    n, sum(iv) / n, 1000 * n / sum(iv), iv[int(n * .99)], iv[-1], sum(x > 16.9 for x in iv), sum(x > 33.4 for x in iv), sum(x > 50 for x in iv)))
EOF
