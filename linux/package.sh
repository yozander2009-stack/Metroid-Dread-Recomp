#!/bin/bash
# Linux build: stop any test instance and assemble the Steam Deck package (program + game code + Deck .ini + run.sh +
# README) in linux/dist/MetroidDread-Linux, plus a .tar.gz of it. No game data inside.
set -e
source "$(dirname "$(readlink -f "$0")")/env.sh"
[ -x "$RUNTIME" ] && [ -f "$CODE_SO" ] || { echo "Build the runtime and install the game first (setup_game.sh)." >&2; exit 1; }
pkill -9 -x "$NAME" 2>/dev/null || true
D="$W/linux/dist"; P="$D/MetroidDread-Linux"
rm -rf "$P" "$D/MetroidDread-Linux.tar.gz" && mkdir -p "$P/${NAME}_data"
cp "$RUNTIME" "$P/$NAME"
strip --strip-unneeded "$CODE_SO" -o "$P/${NAME}_code.so"
cp "$D/run.sh" "$D/README-SteamDeck.txt" "$D/$NAME.ini" "$P/"
cp "$GAMES/$NAME.hints" "$P/" 2>/dev/null || true
sed -i 's/\r$//' "$P/run.sh" "$P/README-SteamDeck.txt" "$P/$NAME.ini"
chmod +x "$P/run.sh" "$P/$NAME"
tar -C "$D" -czf "$D/MetroidDread-Linux.tar.gz" MetroidDread-Linux
ls -la "$P" "$D/MetroidDread-Linux.tar.gz"
sha256sum "$P/$NAME" "$P/${NAME}_code.so"
