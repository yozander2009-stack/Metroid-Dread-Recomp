#!/bin/bash
# Linux build: a test game folder (linux/test/game). exefs and romfs are links to the installed game (read only); the
# saves are a COPY of SWR_TEST_SAVE (default: backups/eos_verified_20261015_151535/save), never your real saves.
set -e
source "$(dirname "$(readlink -f "$0")")/env.sh"
[ -x "$RUNTIME" ] && [ -f "$CODE_SO" ] || { echo "Build the runtime and install the game first (setup_game.sh)." >&2; exit 1; }
G="$TEST/game"
SAVE=${SWR_TEST_SAVE:-$W/backups/eos_verified_20261015_151535/save}
mkdir -p "$G/${NAME}_data"
cp -f "$RUNTIME" "$G/$NAME"
cp -f "$CODE_SO" "$G/"
ln -sfn "$GAMES/${NAME}_data/exefs" "$G/${NAME}_data/exefs"
ln -sfn "$GAMES/${NAME}_data/romfs" "$G/${NAME}_data/romfs"
rm -rf "$G/${NAME}_data/save"
if [ -d "$SAVE" ]; then cp -r "$SAVE" "$G/${NAME}_data/save"
else mkdir -p "$G/${NAME}_data/save"; echo "note: no test save at $SAVE, starting without saves" >&2; fi
[ -f "$G/$NAME.ini" ] || printf '[Display]\nfullscreen=0\ndocked=1\n[Graphics]\nrenderer=opengl\n' > "$G/$NAME.ini"
ls -la "$G" "$G/${NAME}_data"
