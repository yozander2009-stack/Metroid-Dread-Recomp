#!/bin/bash
# Linux build, step 1: build the X11/OpenGL runtime in place (bin/runtime_linux_x11), with the in-game menu (Dear ImGui
# from third_party, compiled into the runtime) and the system's static libvpx for the .webm videos.
# Run as your normal user: bash linux/setup_and_build_runtime.sh
set -e -o pipefail
source "$(dirname "$(readlink -f "$0")")/env.sh"
need gcc "build-essential"; need g++ "build-essential"; need "$PY" "python3"
cd "$W"
# ImGui headers + the sources the Linux runtime compiles itself (what work-profiler/build_imgui.py copies on Windows)
IM="$W/third_party/imgui-1.92.5"; OUT="$W/runtime/imgui"
mkdir -p "$OUT/src"
for f in imgui.h imconfig.h imgui_internal.h imstb_rectpack.h imstb_textedit.h imstb_truetype.h LICENSE.txt \
         backends/imgui_impl_opengl3.h backends/imgui_impl_opengl3_loader.h; do cp -f "$IM/$f" "$OUT/"; done
for f in imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp backends/imgui_impl_opengl3.cpp; do cp -f "$IM/$f" "$OUT/src/"; done
# libvpx.a: same VP9 ABI (image 5, codec 4+5) as the vendored headers. Without it the game runs, minus the videos
mkdir -p runtime/vp9/lin
VPX=$(gcc -print-file-name=libvpx.a)
if [ -f "$VPX" ]; then cp -f "$VPX" runtime/vp9/lin/libvpx.a
else rm -f runtime/vp9/lin/libvpx.a; echo "warning: no static libvpx.a (package libvpx-dev): building without the VP9 videos" >&2; fi
"$PY" switchrecomp.py --build-runtime --cc gcc --target linux --backend x11 2>&1 | tail -60
[ -x "$RUNTIME" ] || { echo "runtime build failed" >&2; exit 1; }
ls -la "$RUNTIME"
