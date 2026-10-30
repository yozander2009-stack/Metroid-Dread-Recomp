#!/bin/bash
# Steam Deck test run (over SSH, Desktop Mode): replays a frame CSV route with fresh COPIES of the test saves, logs
# every present (SWR_TRACE_PRESENTS) and prints a frame-time summary.
# Usage: deck_route.sh LABEL ROUTE.csv EXIT_FRAME CAPTURES(comma or "") [KEY=VALUE env ...]
# Optional env: DECK_INI=file (copied as the game .ini), DECK_SAVE=dir (holds save/; "none" = new game),
#   DECK_LAUNCH=1 (start through run.sh and let the [Launcher] settings apply, as a player starts it)
# Layout on the Deck: ~/dread-test/MetroidDread-Linux (package + Metroid Dread_data/exefs,romfs), ~/dread-test/saves/save
LABEL=$1; ROUTE=$2; EXITF=$3; CAPS=$4; shift 4
T=$HOME/dread-test; G=$T/MetroidDread-Linux; OUT=$T/runs/$LABEL
mkdir -p "$OUT"
rm -rf "$G/Metroid Dread_data/save"
[ "$DECK_SAVE" = none ] || cp -r "${DECK_SAVE:-$T/saves}/save" "$G/Metroid Dread_data/save"
cp "${DECK_INI:-$T/ini/default.ini}" "$G/Metroid Dread.ini"
export DISPLAY=:0 XAUTHORITY=$(ls /run/user/$(id -u)/xauth_* 2>/dev/null | head -1)
cd "$G"
rm -f "Metroid Dread.log"
export SWR_TEST_SCRIPT="$ROUTE" SWR_TEST_EXIT_FRAME=$EXITF SWR_TRACE_PRESENTS="$OUT/presents.csv"
if [ "$DECK_LAUNCH" = 1 ]; then export SWR_TEST_LAUNCHER=1; PROG=./run.sh; else export SWR_SYNC_SHADERS=1; PROG="./Metroid Dread"; fi
[ -n "$CAPS" ] && export SWR_TEST_CAPTURE_FRAMES=$CAPS SWR_TEST_CAPTURE_PREFIX="$OUT/image"
for kv in "$@"; do export "$kv"; done
start=$(date +%s)
timeout 2400 "$PROG" > "$OUT/stdout.txt" 2>&1 &
GPID=$!
# per-thread CPU (the runtime's own thread report is Windows-only): top every 5 s, busiest threads
( sleep 3; P=$(pgrep -f "^./Metroid Dread" | head -1); [ -n "$P" ] && top -H -b -d 5 -p "$P" -w 160 2>/dev/null | grep -A12 "PID USER" > "$OUT/top.txt" ) &
TPID=$!
wait $GPID; code=$?; pkill -P $TPID top 2>/dev/null; kill $TPID 2>/dev/null
echo "exit code $code after $(( $(date +%s) - start )) s"
cp "Metroid Dread.log" "Metroid Dread.ini" "$OUT/"
grep -h "test complete\|inside the recompiled\|fatal\|crash" "$OUT/Metroid Dread.log" | tail -3
python3 - "$OUT/presents.csv" <<'EOF'
import csv, sys
iv = sorted(float(r['interval_ms']) for r in csv.DictReader(open(sys.argv[1])))[1:]
n = len(iv)
if n: print('%d frames, mean %.2f ms (%.1f FPS), p99 %.2f ms, max %.1f ms, >16.7 ms %d, >33 ms %d, >50 ms %d' % (
    n, sum(iv) / n, 1000 * n / sum(iv), iv[int(n * .99)], iv[-1], sum(x > 16.9 for x in iv), sum(x > 33.4 for x in iv), sum(x > 50 for x in iv)))
EOF
