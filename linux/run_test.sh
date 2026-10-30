#!/bin/bash
# Linux build: boot the test folder for N seconds (default 150), then a screenshot. Runs on the real GPU;
# SWR_SOFTGL=1 uses Mesa's software OpenGL instead. Log: linux/test/game/Metroid Dread.log
source "$(dirname "$(readlink -f "$0")")/env.sh"
bash "$W/linux/make_test_game.sh" > /dev/null || exit 1
G="$TEST/game"
cd "$G"
softgl
S=${1:-150}
timeout $((S + 60)) "./$NAME" --seconds=$S --shot="$G/shot_end" --fullscreen=0 > run_stdout.txt 2>&1
echo "exit=$?"
tail -3 run_stdout.txt
grep -h "code:\|RomFS\|audio:\|vp9\|VP9\|video\|gpu: frames=\|renderer\|OpenGL\|GL_\|error\|fatal\|WARN" "$NAME.log" | head -40
ls -la shot_end* 2>/dev/null
