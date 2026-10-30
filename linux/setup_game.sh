#!/bin/bash
# Linux build, step 2: the first-run setup the Windows EXE does, from the command line. Checks your own extracted
# Metroid Dread 2.1.0 (ExeFS + RomFS), installs the RomFS into games/Metroid Dread_data (hard links when on the same
# disk), translates the ExeFS to C, applies the verified patches, compiles "Metroid Dread_code.so" with gcc and puts the
# runtime next to it as "games/Metroid Dread". Stopped or failed runs continue where they were.
# Usage: setup_game.sh GAME_FOLDER [UPDATE_FOLDER]
#   GAME_FOLDER   the extracted game (exefs/ + romfs/), with the 2.1.0 update merged in, or the base game
#   UPDATE_FOLDER the extracted 2.1.0 update, when it is a separate dump
set -e -o pipefail
source "$(dirname "$(readlink -f "$0")")/env.sh"
[ -n "$1" ] || { sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
for d in "$1" "$2"; do [ -z "$d" ] || [ -d "$d" ] || { echo "error: folder not found: $d" >&2; exit 1; }; done
need gcc "build-essential"; need "$PY" "python3"
[ -x "$RUNTIME" ] || bash "$W/linux/setup_and_build_runtime.sh"
mkdir -p "$GAMES"
args=(--out "$GAMES" --name "$NAME" --base "$(readlink -f "$1")")
[ -n "$2" ] && args+=(--update "$(readlink -f "$2")")
# '@@P pct stage|detail' lines become one updating progress line, '@@E' the error, the rest is the log
"$PY" -u "$W/dread/dread_setup.py" "${args[@]}" 2>&1 | tee "$GAMES/setup.log" | while IFS= read -r ln; do
  case "$ln" in
    "@@P "*) p=${ln#@@P }; printf '\r\033[K[%5s%%] %s' "${p%% *}" "$(echo "${p#* }" | tr '|' ' ')";;
    "@@E "*) printf '\n\nerror: %s\n' "${ln#@@E }" >&2;;
    "@@D") printf '\n';;
    "  compiled "*|"  C generated"*) ;;
    *) printf '\r\033[K%s\n' "$ln";;
  esac
done
cp -f "$RUNTIME" "$GAMES/$NAME"
chmod +x "$GAMES/$NAME"
[ -f "$GAMES/$NAME.ini" ] || printf '[Graphics]\nrenderer=opengl\n' > "$GAMES/$NAME.ini"
echo "Installed: $GAMES/$NAME  (start it with: bash linux/run.sh)"
