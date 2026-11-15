#!/bin/bash
# The Linux AMD test build (issue #68): a small AppImage with only the runtime (front end, Native Vulkan) and a launcher
# (appimage_amdtest/AppRun) that runs it next to the game the Metroid Dread AppImage installed. The program's name puts
# it in AMD test mode (main.c): straight into the game, Native Vulkan / OpenGL on alternate starts, a report at exit.
# Output: linux/dist/Metroid-Dread-AMD-test-x86_64.AppImage. appimagetool comes from linux/appimage/cache (as
# make_appimage.sh fetches it).
set -e -o pipefail
source "$(dirname "$(readlink -f "$0")")/env.sh"
need "$PY" "python3"; need desktop-file-validate "desktop-file-utils"; need file "file"
need "$(command -v glslang || command -v glslangValidator || echo glslang)" "glslang-tools"
C="$W/linux/appimage/cache"; A="$W/linux/appimage/AppDir-amdtest"; OUT="$W/linux/dist/Metroid-Dread-AMD-test-x86_64.AppImage"
[ -x "$C/appimagetool" ] || { mkdir -p "$C"; curl -fL --retry 3 -o "$C/appimagetool" https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage; chmod +x "$C/appimagetool"; }
"$PY" "$W/work-profiler/build_snxvk2.py" | tail -1
SWR_RELEASE=1 bash "$W/linux/setup_and_build_runtime.sh" > /dev/null
grep -qa "FIRST-TIME SETUP" "$RUNTIME" || { echo "error: the runtime was built without the front end" >&2; exit 1; }
rm -rf "$A"; mkdir -p "$A/usr/bin" "$A/usr/share/metroid-dread/licenses"
cp "$RUNTIME" "$A/usr/bin/metroid-dread"
LIC="$A/usr/share/metroid-dread/licenses"
cp "$W/LICENSE.txt" "$LIC/LICENSE-SwitchRecomp.txt"; cp "$W/NOTICE.txt" "$W/LICENSE-GPL-3.0.txt" "$LIC/"
cp "$W/third_party/imgui-1.92.5/LICENSE.txt" "$LIC/LICENSE-ImGui.txt"; cp "$W/runtime/vp9/LICENSE-libvpx.txt" "$LIC/"
cp "$W/linux/appimage_amdtest/AppRun" "$A/AppRun"; chmod +x "$A/AppRun"
sed -e 's/^Name=.*/Name=Metroid Dread AMD test/' -e 's/^Comment=.*/Comment=Debugging build for AMD graphics (issue #68): runs the installed game and saves a report/' \
    "$W/linux/appimage/metroid-dread.desktop" > "$A/metroid-dread.desktop"
"$PY" -I - "$W/runtime/app.ico" "$A/metroid-dread.png" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read(); n = struct.unpack_from('<HHH', d)[2]
best = max((struct.unpack_from('<BBBBHHII', d, 6 + 16 * i) for i in range(n)), key=lambda e: e[0] or 256)
open(sys.argv[2], 'wb').write(d[best[7]:best[7] + best[6]])
EOF
ln -sf metroid-dread.png "$A/.DirIcon"
mkdir -p "$(dirname "$OUT")"
ARCH=x86_64 "$C/appimagetool" --appimage-extract-and-run --no-appstream "$A" "$OUT"
ls -la "$OUT"; sha256sum "$OUT"
