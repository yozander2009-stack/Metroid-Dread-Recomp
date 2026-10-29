#!/bin/bash
# macOS build (Apple silicon; Intel Macs untested). Native Vulkan through MoltenVK: macOS stops at OpenGL 4.1, the
# OpenGL renderer needs 4.3.
#
#   macos/build.sh                          the runtime: bin/runtime_macos_sdl (and runtime/snxvk/libsnxvk_mac.a)
#   macos/build.sh game DUMP [UPDATE]       setup from your own dump: checks the files, translates and compiles the
#                                           game into games/ (OUT=folder for another place), then installs the runtime
#   macos/build.sh install [FOLDER]         the runtime and its libraries into a set-up game folder (default games/)
#   macos/build.sh app [FOLDER]             "Metroid Dread.app" from a set-up game folder (default games/): one app with
#                                           the program, libraries and game data; settings, saves and the log go to
#                                           ~/Library/Application Support/Metroid Dread. It holds your game: never share it
#
# Needs the Xcode command line tools (xcode-select --install) and Homebrew:
#   brew install sdl2 molten-vk glslang libvpx
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NAME="Metroid Dread"
cd "$ROOT"

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing: $1. $2" >&2; exit 1; }; }

deps() {
  [ "$(uname -s)" = Darwin ] || { echo "This script builds on macOS only." >&2; exit 1; }
  need clang "Install the Xcode command line tools: xcode-select --install"
  need python3 "Install the Xcode command line tools: xcode-select --install"
  need glslang "brew install glslang"
  command -v sdl2-config >/dev/null 2>&1 || pkg-config --exists sdl2 2>/dev/null || { echo "Missing: SDL2. brew install sdl2" >&2; exit 1; }
  mvk_path >/dev/null || { echo "Missing: MoltenVK. brew install molten-vk" >&2; exit 1; }
}

brew_prefix() { command -v brew >/dev/null 2>&1 && brew --prefix "$1" 2>/dev/null; }

mvk_path() {
  for p in "$(brew_prefix molten-vk)/lib/libMoltenVK.dylib" /opt/homebrew/lib/libMoltenVK.dylib /usr/local/lib/libMoltenVK.dylib; do
    [ -f "$p" ] && { echo "$p"; return 0; }; done
  return 1
}

# Match the vendored 2.4.1 API headers. Build only the decoder and common code,
# statically, so installed games need no OpenH264 dylib or Homebrew at launch.
build_h264() (
  [ -f runtime/oh264/mac/libdecoder.a ] && [ -f runtime/oh264/mac/libcommon.a ] && exit 0
  need curl "curl is required to download OpenH264."
  need make "Install the Xcode command line tools: xcode-select --install"
  local T
  T=$(mktemp -d)
  trap 'rm -rf "$T"' EXIT
  curl -fL --retry 3 https://codeload.github.com/cisco/openh264/tar.gz/refs/tags/v2.4.1 -o "$T/openh264.tar.gz"
  echo "8ffbe944e74043d0d3fb53d4a2a14c94de71f58dbea6a06d0dc92369542958ea  $T/openh264.tar.gz" | shasum -a 256 -c -
  tar -xzf "$T/openh264.tar.gz" -C "$T"
  # USE_ASM=No also allows Intel builds without an extra NASM dependency.
  MACOSX_DEPLOYMENT_TARGET=12.0 make -C "$T/openh264-2.4.1" -j "$(sysctl -n hw.ncpu)" \
    CC=clang CXX=clang++ USE_ASM=No V=No libdecoder.a libcommon.a
  mkdir -p runtime/oh264/mac
  cp "$T/openh264-2.4.1/libdecoder.a" "$T/openh264-2.4.1/libcommon.a" runtime/oh264/mac/
)

runtime() {
  deps
  # Both codecs are required: do not silently omit video decoders.
  vpx="$(brew_prefix libvpx)/lib/libvpx.a"
  [ -f "$vpx" ] || { echo "Missing: static libvpx. brew install libvpx" >&2; exit 1; }
  mkdir -p runtime/vp9/mac
  cp -f "$vpx" runtime/vp9/mac/libvpx.a
  echo "== OpenH264 decoder -> runtime/oh264/mac/"
  build_h264
  echo "== native Vulkan renderer (ScarletNX) -> runtime/snxvk/libsnxvk_mac.a"
  python3 work-profiler/build_snxvk2.py
  echo "== runtime -> bin/runtime_macos_sdl"
  python3 switchrecomp.py --build-runtime --cc clang
}

# the runtime as "<dir>/Metroid Dread" with its SDL2 and MoltenVK beside it, re-signed, so it runs without Homebrew
bundle_runtime() {
  local G=$1 EXE sdl sdl3 mvk f
  [ -x bin/runtime_macos_sdl ] || runtime
  EXE="$G/$NAME"
  cp -f bin/runtime_macos_sdl "$EXE"
  # SDL2: the copy the runtime was linked with, referenced from the game folder
  sdl=$(otool -L "$EXE" | awk '/libSDL2/ {print $1; exit}')
  if [ -n "$sdl" ] && [ -f "$sdl" ]; then
    cp -f "$sdl" "$G/libSDL2-2.0.0.dylib"; chmod u+w "$G/libSDL2-2.0.0.dylib"
    install_name_tool -id @executable_path/libSDL2-2.0.0.dylib "$G/libSDL2-2.0.0.dylib" 2>/dev/null
    install_name_tool -change "$sdl" @executable_path/libSDL2-2.0.0.dylib "$EXE" 2>/dev/null
    # Homebrew's sdl2 is sdl2-compat, SDL2's API over SDL3: it opens libSDL3.dylib from its own folder
    if strings "$sdl" | grep -q 'libSDL3.dylib'; then
      sdl3=$(brew_prefix sdl3)/lib/libSDL3.0.dylib
      [ -f "$sdl3" ] || { echo "SDL2 here is sdl2-compat, but SDL3 was not found (brew install sdl3)." >&2; exit 1; }
      cp -f "$sdl3" "$G/libSDL3.dylib"; chmod u+w "$G/libSDL3.dylib"
      install_name_tool -id @loader_path/libSDL3.dylib "$G/libSDL3.dylib" 2>/dev/null
    fi
  fi
  # MoltenVK on its own is a whole Vulkan implementation: backend_sdl.c loads it from the game folder first
  mvk=$(mvk_path); cp -f "$mvk" "$G/libMoltenVK.dylib"; chmod u+w "$G/libMoltenVK.dylib"
  install_name_tool -id @executable_path/libMoltenVK.dylib "$G/libMoltenVK.dylib" 2>/dev/null
  # install_name_tool breaks the signatures; Apple silicon runs nothing unsigned: sign them again (ad hoc)
  for f in "$EXE" "$G/${NAME}_code.dylib" "$G"/libSDL2-2.0.0.dylib "$G"/libSDL3.dylib "$G"/libMoltenVK.dylib; do
    [ -f "$f" ] && codesign -s - -f "$f" 2>/dev/null; done
  chmod +x "$EXE"
}

# the runtime and its libraries into a game folder, with a double-click starter
install_game() {
  G=${1:-$ROOT/games}
  [ -f "$G/${NAME}_code.dylib" ] || { echo "No ${NAME}_code.dylib in $G: run setup first (macos/build.sh game DUMP)." >&2; exit 1; }
  bundle_runtime "$G"
  # a double-click start from Finder (the game finds its files next to itself; captures/ goes here too)
  cat > "$G/Play $NAME.command" <<'EOF'
#!/bin/bash
cd "$(dirname "$0")" && exec "./Metroid Dread" "$@"
EOF
  chmod +x "$G/Play $NAME.command"
  [ -f "$G/$NAME.ini" ] || printf '[Display]\nfullscreen=0\ndocked=1\n[Graphics]\nrenderer=vulkan_native\n' > "$G/$NAME.ini"
  echo "Installed: $G"
  echo "Start it with \"$G/Play $NAME.command\" (or double-click it in Finder)."
}

# "Metroid Dread.app": Contents/MacOS has the program, the game code and the libraries; Contents/Resources the icon and
# the game's exefs and romfs (APFS clones of the game folder's: no extra space on the same disk). The runtime notices
# the .app (main.c app_dirs) and keeps everything it writes in ~/Library/Application Support/Metroid Dread
make_app() {
  G=${1:-$ROOT/games}
  D="$G/${NAME}_data"
  [ -f "$G/${NAME}_code.dylib" ] && [ -d "$D/exefs" ] && [ -d "$D/romfs" ] \
    || { echo "$G is not a set-up game folder (run macos/build.sh game DUMP first)." >&2; exit 1; }
  A="$G/$NAME.app"; C="$A/Contents"
  rm -rf "$A"; mkdir -p "$C/MacOS" "$C/Resources/${NAME}_data"
  cp -f "$G/${NAME}_code.dylib" "$C/MacOS/"
  bundle_runtime "$C/MacOS"
  echo "== copying the game data into the app"
  for d in exefs romfs; do cp -cR "$D/$d" "$C/Resources/${NAME}_data/" 2>/dev/null || cp -R "$D/$d" "$C/Resources/${NAME}_data/"; done
  # the icon: runtime/app.ico (256x256) at every size macOS asks for
  T=$(mktemp -d); sips -s format png runtime/app.ico --out "$T/icon.png" >/dev/null && mkdir "$T/i.iconset"
  for s in 16 32 128 256 512; do sips -z $s $s "$T/icon.png" --out "$T/i.iconset/icon_${s}x${s}.png" >/dev/null
    sips -z $((s*2)) $((s*2)) "$T/icon.png" --out "$T/i.iconset/icon_${s}x${s}@2x.png" >/dev/null; done
  iconutil -c icns "$T/i.iconset" -o "$C/Resources/AppIcon.icns"; rm -rf "$T"
  cat > "$C/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundleDisplayName</key><string>$NAME</string>
  <key>CFBundleExecutable</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>org.switchrecomp.metroid-dread</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>2.1.0</string>
  <key>CFBundleVersion</key><string>2.1.0</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>GCSupportsControllerUserInteraction</key><true/>
</dict>
</plist>
EOF
  echo "== signing (ad hoc)"
  codesign -s - -f "$A"
  # settings, saves and shader caches from the game folder, the first time only (nothing there is overwritten)
  U="$HOME/Library/Application Support/$NAME"; mkdir -p "$U/${NAME}_data"
  [ -f "$U/$NAME.ini" ] || { [ -f "$G/$NAME.ini" ] && cp "$G/$NAME.ini" "$U/"; } || true
  [ -d "$U/${NAME}_data/save" ] || { [ -d "$D/save" ] && cp -R "$D/save" "$U/${NAME}_data/"; } || true
  [ -d "$U/vulkan_native" ] || { [ -d "$G/vulkan_native" ] && cp -R "$G/vulkan_native" "$U/"; } || true
  echo "Made: $A"
  echo "Double-click it, or move it to Applications first. Settings, saves and the log: $U"
}

case "${1:-runtime}" in
  runtime) runtime ;;
  game)
    [ -n "$2" ] || { echo "usage: macos/build.sh game DUMP_FOLDER [UPDATE_FOLDER]" >&2; exit 1; }
    deps; [ -x bin/runtime_macos_sdl ] || runtime
    OUTDIR=${OUT:-$ROOT/games}; mkdir -p "$OUTDIR"
    args=(--out "$OUTDIR" --name "$NAME" --base "$2"); [ -n "$3" ] && args+=(--update "$3")
    python3 dread/dread_setup.py "${args[@]}" | grep -v '^@@P '
    [ "${PIPESTATUS[0]}" = 0 ] || exit 1
    install_game "$OUTDIR" ;;
  install) install_game "$2" ;;
  app) make_app "$2" ;;
  *) sed -n '2,16p' "$0"; exit 1 ;;
esac
