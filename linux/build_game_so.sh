#!/bin/bash
# Linux build: recompile "Metroid Dread_code.so" from the installed game (games/Metroid Dread_data), e.g. after a change
# to the patches, codegen.py or runtime/cpu.h. Same steps and flags as setup_game.sh: unchanged C files and their
# objects are reused, so only what changed is compiled. SWR_CFLAGS adds flags (e.g. -march=native).
set -e
source "$(dirname "$(readlink -f "$0")")/env.sh"
[ -d "$GAMES/${NAME}_data/exefs" ] || { echo "Not installed yet: run bash linux/setup_game.sh GAME_FOLDER first." >&2; exit 1; }
bash "$W/linux/setup_game.sh" "$GAMES/${NAME}_data"
ls -la "$CODE_SO"
