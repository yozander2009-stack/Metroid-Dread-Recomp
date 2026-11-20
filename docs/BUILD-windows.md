# Building on Windows

Players don't need this: download the release zip instead. This is for building the program from source.

## Repository layout

| path | what |
|---|---|
| `switchrecomp.py`, `codegen.py`, `a64.py`, `nxloader.py`, `metroid_perf.py` | SwitchRecomp: ARM64 → C translator (Apache-2.0) |
| `runtime/` | the runtime: kernel, services, GPU → OpenGL, audio, video, input |
| `runtime/dread_ui/` | launcher: `fe.cpp` (setup, Play, settings), `ov.cpp` (in-game menu), `ui.cpp` (theme, widgets, settings table) |
| `runtime/snxvk/` | ScarletNX's native Vulkan renderer (GPL-3.0) in `src_new/`, vendored, symbols prefixed `snxv_`; DLSS code in `src_new/nx/gpu/vk/dlss.c` |
| `dread/` | setup driver run by the program, patch set, RomFS manifest, release script, design notes |
| `third_party/imgui-1.92.5/` | Dear ImGui (MIT) |
| `linux/` | Linux and Steam Deck build ([linux.md](linux.md)) |

## What is in this repository

- Source code only, plus three prebuilt third-party static libraries, kept so the build doesn't need MSYS2:
  `runtime/vp9/win/libvpx.a` (libvpx 1.14.1, VP9 decoder only, build options in `runtime/vp9/README.txt`) and
  `runtime/oh264/win/*.a` (OpenH264 decoder). Rebuild them from the upstream sources if you prefer.
- `.gitignore` keeps out toolchains, build output, EXEs, DLLs, archives and anything from a game folder.
- `dread/dread_v210_patches.json` holds only this project's own edits: for each patched file, the SHA-256 of the C
  that setup generates from the player's ExeFS, the line range to replace, and the replacement lines (frame pacing,
  vector growth, zlib, cutscene skip, cheats, ...). It contains no game code, no game data and no decryption.
- `dread/romfs_v210.manifest` lists RomFS file names, sizes and SHA-1 hashes, used only to check the player's files.

## Toolchain

The toolchain is **not** in this repository: put your own downloads at these paths. The release zip bundles a
compiler and Python so that setup can compile the game on the player's PC.

- `gcc\` in the repository root: [winlibs](https://winlibs.com) GCC 14.2.0, UCRT, POSIX threads, x64
- `python\` in the repository root: Python 3.12 for Windows (the embeddable package is enough)
- glslang 16.6.0 (Vulkan SDK or KhronosGroup release) for the renderer's shaders: `GLSLANG=` or on PATH
- optional, for releases: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (UCRT, x86_64), bundled as the
  game compiler setup prefers (`SWR_RELEASE_CLANG=<folder>`)

## Build

`build.bat` checks for the tools and runs the steps below (`build.bat release X.Y.Z` also packs a release):

```
python\python.exe work-profiler\build_imgui.py
set GLSLANG=<path to glslang.exe 16.6.0> && python\python.exe work-profiler\build_snxvk2.py
set SWR_RELEASE=1 && python\python.exe switchrecomp.py --build-runtime --cc gcc
python\python.exe dread\make_release.py release <empty folder> --version X.Y.Z --exe bin\runtime_win64_win32.exe
```

The release step also needs `redist\zlib1.dll` (zlib 1.3.1, x64) and `redist\SDL2.dll` (SDL 2.x x64 release), the two
DLLs the game ships beside it. They are not in this repository.

A release must attach `update.txt`, `update-<version>.zip` and `clang-<version>.zip` (made by `make_release.py`) along
with the zip, or installed copies won't see the update.
