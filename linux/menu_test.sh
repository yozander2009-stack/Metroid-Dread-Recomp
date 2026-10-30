#!/bin/bash
# Linux: open the in-game menu (Home, replayed from linux/menu_test.csv) on the title screen and screenshot it.
# Runs on the real GPU; SWR_SOFTGL=1 uses Mesa's software OpenGL instead.
set -e
source "$(dirname "$(readlink -f "$0")")/env.sh"
CSV="$W/linux/menu_test.csv"
[ -f "$CSV" ] || { echo "missing $CSV (the frame-indexed input route of the menu test)" >&2; exit 1; }
bash "$W/linux/make_test_game.sh" > /dev/null
G="$TEST/game"
cd "$G"
rm -f menu-m*.ppm
softgl
export SWR_TEST_SCRIPT="$CSV" SWR_TEST_EXIT_FRAME=140 SWR_OV_SHOTS=3,30 SWR_OV_SHOT_PREFIX="$G/menu"
timeout 900 "./$NAME" --fullscreen=0 > run_stdout.txt 2>&1 || echo "exit=$?"
grep -E "menu:|test complete|fatal|FATAL|rror" "$NAME.log" | head -10
ls -la menu-m*.ppm 2>/dev/null && cp menu-m*.ppm "$TEST/"
echo MENUTESTDONE
