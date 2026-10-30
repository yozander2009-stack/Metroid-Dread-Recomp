#!/bin/bash
# Metroid Dread (Linux / Steam Deck): start the game from this folder. Extra arguments go to the game.
cd "$(dirname "$(readlink -f "$0")")"
if [ ! -d "Metroid Dread_data/exefs" ] || [ ! -d "Metroid Dread_data/romfs" ]; then
  echo "Put your own Metroid Dread 2.1.0 files in 'Metroid Dread_data/exefs' and 'Metroid Dread_data/romfs' first (see README-SteamDeck.txt)." >&2
  exit 1
fi
exec "./Metroid Dread" "$@"   # the runtime finds its _code.so, _data and .ini next to argv[0]
