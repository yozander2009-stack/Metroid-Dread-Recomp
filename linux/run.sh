#!/bin/bash
# Linux build: start the installed game (games/Metroid Dread, from setup_game.sh). Extra arguments go to the game,
# e.g. --fullscreen=0. The log is games/Metroid Dread.log
source "$(dirname "$(readlink -f "$0")")/env.sh"
[ -x "$GAMES/$NAME" ] && [ -f "$CODE_SO" ] || { echo "Not installed yet: run bash linux/setup_game.sh GAME_FOLDER first." >&2; exit 1; }
softgl
export SWR_RECOMPILER="$W"   # the Play screen's setup / rebuild uses this repository's recompiler
cd "$GAMES"
exec "./$NAME" "$@"   # the runtime finds its _code.so, _data and .ini next to argv[0]
