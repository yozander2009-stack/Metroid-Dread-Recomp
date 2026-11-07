#!/bin/bash
# Linux build: the AppImage, one file that sets up, configures and runs the game like the Windows EXE. Inside: the runtime
# with the Play / setup screens, the recompiler (the Python scripts, patches and RomFS manifest), a relocatable GCC
# (Bootlin), Clang (the official LLVM build, using the GCC's C library and linker) and Python
# (python-build-standalone), so the player needs none of them. No game files inside.
# The game is installed in ~/.local/share/MetroidDreadRecomp (METROID_DREAD_HOME moves it), see appimage/AppRun.
# Downloads are checked against the SHA-256 below and kept in linux/appimage/cache.
# Output: linux/dist/Metroid_Dread-x86_64.AppImage, and linux/dist/update-linux.txt for the in-game updater
# (dread_ui/fe_update_linux.cpp). VERSION=x.y.z names the release (default: 0.0.0-dev); publish both files as release
# assets of tag vVERSION (TAG=... otherwise).
set -e -o pipefail
source "$(dirname "$(readlink -f "$0")")/env.sh"
need curl "curl"; need "$PY" "python3"
need desktop-file-validate "desktop-file-utils (appimagetool makes no AppImage without it, and says nothing)"
C="$W/linux/appimage/cache"; A="$W/linux/appimage/AppDir"; R="$A/usr/share/metroid-dread/recompiler"
OUT="$W/linux/dist/Metroid_Dread-x86_64.AppImage"
GCC_URL=https://toolchains.bootlin.com/downloads/releases/toolchains/x86-64/tarballs/x86-64--glibc--stable-2025.08-1.tar.xz   # GCC 14.3.0, glibc 2.41
GCC_SHA=760acd5c3159448b618e237b61935335baada74fe0cdc0d7611826cb49b41c8c
PY_URL=https://github.com/astral-sh/python-build-standalone/releases/download/20261009/cpython-3.12.15%2B20261009-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz
PY_SHA=ffa8f85f1b56e88b687f08d743d817015524b86a9365fd921243132b80f1a36d
TOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage
LLVM_URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/LLVM-23.1.4-Linux-X64.tar.xz   # 1.9 GB, kept in the cache
LLVM_SHA=25d814fda50d5fb0084b42be09605d8b1595d49d5e2e03a3345d7e112ff6f9f3

fetch() {   # fetch FILE URL [SHA256]
  local f="$C/$1"
  if [ -n "$3" ] && [ -f "$f" ] && ! echo "$3  $f" | sha256sum -c --quiet - 2>/dev/null; then rm -f "$f"; fi
  [ -f "$f" ] || { echo "downloading $1"; curl -fL --retry 3 -o "$f.part" "$2" && mv "$f.part" "$f"; }
  [ -z "$3" ] || echo "$3  $f" | sha256sum -c --quiet - || { echo "error: $1 does not match its SHA-256" >&2; exit 1; }
}
mkdir -p "$C"
fetch gcc.tar.xz "$GCC_URL" "$GCC_SHA"
fetch python.tar.gz "$PY_URL" "$PY_SHA"
fetch appimagetool "$TOOL_URL"
fetch llvm.tar.xz "$LLVM_URL" "$LLVM_SHA"
chmod +x "$C/appimagetool"

# the runtime as released on Windows (SWR_RELEASE: no frame generation, no DLSS notes), with the front end
# renderer=vulkan_native: ScarletNX's renderer (GPL-3.0, its source is in runtime/snxvk), built without DLSS
need "$(command -v glslang || command -v glslangValidator || echo glslang)" "glslang-tools"
"$PY" "$W/work-profiler/build_snxvk2.py" | tail -1
SWR_RELEASE=1 bash "$W/linux/setup_and_build_runtime.sh" > /dev/null
grep -qa "FIRST-TIME SETUP" "$RUNTIME" || { echo "error: the runtime was built without the front end (SWR_NO_LINUX_FRONTEND?)" >&2; exit 1; }

rm -rf "$A"; mkdir -p "$A/usr/bin" "$R/dread" "$R/runtime" "$R/linux" "$A/usr/share/metroid-dread/licenses"
cp "$RUNTIME" "$A/usr/bin/metroid-dread"
cp "$W/dread/pipeline_recipes_shipped.bin" "$A/usr/share/metroid-dread/"   # Native Vulkan's pipeline list (AppRun)
# the recompiler, laid out as dread_setup.py expects it (as in the Windows release's recompiler folder)
cp "$W"/{switchrecomp,codegen,a64,nxloader,metroid_perf}.py "$W/dread/dread_setup.py" "$R/"
cp "$W/dread/dread_v210_patches.json" "$W/dread/romfs_v210.manifest" "$R/dread/"
cp "$W/runtime/cpu.h" "$W/runtime/cpu_helpers.c" "$R/runtime/"
cp "$W/linux/lx_compat.h" "$R/linux/"
LIC="$A/usr/share/metroid-dread/licenses"
cp "$W/LICENSE.txt" "$LIC/LICENSE-SwitchRecomp.txt"; cp "$W/NOTICE.txt" "$W/LICENSE-GPL-3.0.txt" "$LIC/"
cp "$W/third_party/imgui-1.92.5/LICENSE.txt" "$LIC/LICENSE-ImGui.txt"; cp "$W/runtime/vp9/LICENSE-libvpx.txt" "$LIC/"

# GCC: only what compiling and linking C needs (no C++, gdb and its Python, sanitizers, docs)
echo "unpacking GCC"
mkdir -p "$R/gcc"; tar -xf "$C/gcc.tar.xz" -C "$R/gcc" --strip-components=1
( cd "$R/gcc"
  T=x86_64-buildroot-linux-gnu; L=libexec/gcc/$T/14.3.0
  rm -rf share lib/python3* lib/libpython* bin/*gdb* bin/*gcov* bin/*lto-dump* bin/*c++* bin/*g++* bin/*gfortran* relocate-sdk.sh
  rm -rf $L/cc1plus $L/lto1 $L/g++-mapper-server $L/plugin $L/install-tools
  rm -rf $T/sysroot/usr/share $T/sysroot/usr/bin $T/sysroot/usr/sbin $T/sysroot/sbin $T/sysroot/usr/libexec
  rm -f $T/lib64/lib{asan,hwasan,lsan,tsan,ubsan,stdc++,stdc++fs,supc++,gfortran}* $T/sysroot/usr/lib/lib{asan,hwasan,lsan,tsan,ubsan,stdc++,stdc++fs,supc++,gfortran}*
  rm -f $T/sysroot/usr/lib/libc.a $T/sysroot/usr/lib/libm-2.*.a $T/sysroot/usr/lib/libmvec.a
  rm -rf $T/sysroot/usr/include/c++ include/c++ )
# Clang: the compiler binary and its own headers only (the LLVM package is 1.9 GB). It compiles against the bundled GCC's
# C library and startup files and links with its GNU ld, so the game code is built the same way on every distribution.
# Setup uses it when it runs here (it needs libstdc++ from GCC 12 or newer on the system), else GCC (dread_setup.py)
echo "unpacking Clang"
mkdir -p "$R/clang"
tar -xJf "$C/llvm.tar.xz" -C "$R/clang" --strip-components=1 --wildcards '*/bin/clang-23' '*/lib/clang/23/include/*' \
    '*/include/llvm/Support/LICENSE.TXT'
mv "$R/clang/include/llvm/Support/LICENSE.TXT" "$R/clang/LICENSE.TXT"; rm -rf "$R/clang/include"
"$R/gcc/bin/x86_64-buildroot-linux-gnu-strip" --strip-unneeded "$R/clang/bin/clang-23"
cat > "$R/clang/bin/clang" <<'EOF'
#!/bin/sh
# Clang with the bundled GCC's C library, startup files and linker (recompiler/gcc), so the game code doesn't depend on
# the system's development packages
R="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"; T=x86_64-buildroot-linux-gnu
exec "$R/clang/bin/clang-23" --target=$T --gcc-toolchain="$R/gcc" --sysroot="$R/gcc/$T/sysroot" \
    --ld-path="$R/gcc/bin/$T-ld" -Qunused-arguments "$@"
EOF
chmod +x "$R/clang/bin/clang"
cp "$R/clang/LICENSE.TXT" "$LIC/LICENSE-LLVM.txt"

# Python: the standard library dread_setup.py and SwitchRecomp use (no Tk, IDLE, tests, pip)
echo "unpacking Python"
tar -xzf "$C/python.tar.gz" -C "$R"
( cd "$R/python"
  rm -rf include share lib/pkgconfig lib/itcl* lib/tcl* lib/tk* lib/thread* lib/libtcl* lib/libtk*
  cd lib/python3.12 && rm -rf test idlelib tkinter turtledemo ensurepip lib2to3 pydoc_data site-packages/* lib-dynload/_tkinter* config-3.12-* )

# the toolchain must still build and link the game's C, and Python run the recompiler
GCC_BIN="$R/gcc/bin/x86_64-buildroot-linux-gnu-gcc"; T=$(mktemp -d)
printf 'unsigned long GetEnvironmentVariableA(const char*,char*,unsigned long);\nint f(void){ char b[8]; return (int)GetEnvironmentVariableA("X",b,8); }\n' > "$T/t.c"
"$GCC_BIN" -O2 -fPIC -fvisibility=hidden -I "$R/runtime" -include "$R/linux/lx_compat.h" -c "$T/t.c" -o "$T/t.o"
"$GCC_BIN" -shared -o "$T/t.so" "$T/t.o" -lm -ldl
CL_BIN="$R/clang/bin/clang"
"$CL_BIN" -O2 -fPIC -fvisibility=hidden -I "$R/runtime" -include "$R/linux/lx_compat.h" -c "$T/t.c" -o "$T/c.o"
"$CL_BIN" -shared -o "$T/c.so" "$T/c.o" -lm -ldl
"$R/python/bin/python3" -I -c "import hashlib, json, zipfile, concurrent.futures, argparse, struct; import sys; sys.path.insert(0, '$R'); import switchrecomp"
rm -rf "$T" "$R/__pycache__"

# the updater's record of this build: version, release tag, and a hash over the recompiler (not GCC or Python), whose
# change means the game code must be rebuilt after an update
VERSION=${VERSION:-0.0.0-dev}; TAG=${TAG:-v$VERSION}
RHASH=$( cd "$R" && find . -path ./gcc -prune -o -path ./clang -prune -o -path ./python -prune -o -type f -print | LC_ALL=C sort | xargs -d '\n' sha256sum | sha256sum | cut -c1-64 )
printf 'version %s\ntag %s\nrecompiler %s\n' "$VERSION" "$TAG" "$RHASH" > "$A/usr/share/metroid-dread/update.txt"

# desktop entry, icon (the 256 px PNG inside runtime/app.ico) and AppRun
cp "$W/linux/appimage/AppRun" "$A/AppRun"; chmod +x "$A/AppRun"
cp "$W/linux/appimage/metroid-dread.desktop" "$A/"
"$PY" -I - "$W/runtime/app.ico" "$A/metroid-dread.png" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read(); n = struct.unpack_from('<HHH', d)[2]
best = max((struct.unpack_from('<BBBBHHII', d, 6 + 16 * i) for i in range(n)), key=lambda e: e[0] or 256)
open(sys.argv[2], 'wb').write(d[best[7]:best[7] + best[6]])
EOF
ln -sf metroid-dread.png "$A/.DirIcon"

mkdir -p "$(dirname "$OUT")"
du -sh "$R/gcc" "$R/clang" "$R/python" "$A"
ARCH=x86_64 "$C/appimagetool" --appimage-extract-and-run --no-appstream "$A" "$OUT"
ls -la "$OUT"
{ cat "$A/usr/share/metroid-dread/update.txt"
  printf 'package %s %s %s\n' "$(basename "$OUT")" "$(sha256sum "$OUT" | cut -c1-64)" "$(stat -c %s "$OUT")"; } > "$(dirname "$OUT")/update-linux.txt"
cat "$(dirname "$OUT")/update-linux.txt"
