# Metroid Dread — recompiled for PC

> **Work in progress.** This project was shared on Reddit and elsewhere before it was ready: the planned first proper
> release is 1.0.0, and current builds (0.x) are early previews. Expect crashes, setup problems and rough edges on
> hardware it has not been tested on. If something breaks, please open an issue and attach the `.log` file from the
> game folder, plus your CPU, GPU, RAM and Windows version. I'll do the best I can to resolve the errors and make this
> a good experience.

One Windows EXE that sets up, configures and runs a native PC build of **your own** copy of Metroid Dread 2.1.0.
It is the SwitchRecomp runtime (by MZ Labs) with a Metroid Dread front end built in: first-run setup that compiles
the game with a bundled GCC, a Play / settings screen, and an in-game menu (F1 or the Home / Guide button).

![The Play screen](docs/images/play-screen.png)

**No Nintendo files are in this repository**: no game code, no RomFS, no keys, no generated C.

- `dread/dread_v210_patches.json` holds small anchored edits that setup applies to the C it generates from the player's ExeFS.
- `dread/romfs_v210.manifest` lists RomFS file names, sizes and SHA-1 hashes, so setup can verify the player's files.

## Layout

| path | what |
|---|---|
| `switchrecomp.py`, `codegen.py`, `a64.py`, `nxloader.py`, `metroid_perf.py` | SwitchRecomp: ARM64 → C translator (Apache-2.0) |
| `runtime/` | the runtime: kernel, services, GPU → OpenGL, audio, video, input |
| `runtime/dread_ui/` | front end: `fe.cpp` (setup, Play, settings), `ov.cpp` (in-game menu), `ui.cpp` (theme, widgets, settings table) |
| `runtime/snxvk/` | ScarletNX's native Vulkan renderer (GPL-3.0, main b5f4f14) in `src_new/`, vendored, symbols prefixed `snxv_`; our DLSS code in `src_new/nx/gpu/vk/dlss.c` |
| `dread/` | setup driver run by the EXE, patch set, RomFS manifest, release script, design notes |
| `third_party/imgui-1.92.5/` | Dear ImGui (MIT) |
| `linux/` | scripts for the Linux / Steam Deck build |

## Setting up the game (players)

Download the release zip, not this repository. On first start the EXE asks for your game files and compiles the game.
It needs **both** parts of your own dumped copy of Metroid Dread **2.1.0**:

- the **ExeFS** (the game code: the files `main`, `main.npdm`, ...)
- the **RomFS** (the game data: `system`, `packs`, ...), with the 2.1.0 update merged in

A RomFS dump on its own is not enough. Choose the folder that holds both; they can be a few folders down (a dump
folder with one folder per title is fine). If you have the base game and the update as separate dumps, choose the
update as "Update folder". Every RomFS file is checked against the 2.1.0 file list before anything is compiled.

**Updates**: the Play screen checks this repository's releases and offers **UPDATE** when there is a
new version. Only the game's own files that changed are downloaded (a few MB), each one is checked against its
SHA-256 before anything is replaced, and the replaced files are kept so **UNDO** can go back. If the game-code patches
changed, the game is rebuilt from the files setup already installed, without asking for your game folder again.
Saves, settings and mods are never touched. It can be switched off on the Play screen.

**Not sure which settings to pick?** Press **AUTO-SET** on the Play screen. It reads your graphics card, screen and
memory and picks resolution, frame rate and quality to match; a new installation gets these automatically after
setup. Anything can still be changed afterwards.

Found a bug? See [CONTRIBUTING.md](CONTRIBUTING.md): please attach the `.log`.

## Screenshots

| Graphics settings | Controls |
|---|---|
| ![Graphics settings](docs/images/graphics-settings.png) | ![Controls](docs/images/controls.png) |

## Build from source (Windows)

The toolchain is **not** in this repository and never was: the paths below are where you put your own downloads.
The release zip bundles the same GCC and Python so that setup can compile the game on the player's PC.

- `gcc\` in the repository root: [winlibs](https://winlibs.com) GCC 14.2.0, UCRT, POSIX threads, x64
- `python\` in the repository root: Python 3.12 for Windows (the embeddable package is enough)
- glslang 16.6.0 (Vulkan SDK or KhronosGroup release), for the renderer's compute shaders: `GLSLANG=` or on PATH

`build.bat` checks for all three and runs the steps below (`build.bat release X.Y.Z` also packs a release):

```
python\python.exe work-profiler\build_imgui.py
set GLSLANG=<path to glslang.exe 16.6.0> && set SWR_NO_DLSS=1 && python\python.exe work-profiler\build_snxvk2.py
set SWR_RELEASE=1 && python\python.exe switchrecomp.py --build-runtime --cc gcc
python\python.exe dread\make_release.py release <empty folder> --version X.Y.Z --exe bin\runtime_win64_win32.exe
```

The release step also needs `redist\zlib1.dll` (zlib 1.3.1, x64) and `redist\SDL2.dll` (SDL 2.x x64 release), the two
DLLs the game ships beside it. They are not in this repository.

## Build (Android / Retroid)

See [android/README.md](android/README.md) for the ARM64 APK build and deployment scripts,
settings launcher, optional native Vulkan, and custom graphics drivers.
Requires your own extracted Metroid Dread 2.1.0 ExeFS and RomFS.

## What is in this repository

- Source code only, plus three prebuilt third-party static libraries, kept so the build does not need MSYS2:
  `runtime/vp9/win/libvpx.a` (libvpx 1.14.1, VP9 decoder only, build options in `runtime/vp9/README.txt`) and
  `runtime/oh264/win/*.a` (OpenH264 decoder). Rebuild them from the upstream sources if you prefer.
- `.gitignore` keeps out toolchains, build output, EXEs, DLLs, archives and anything from a game folder.
- `dread/dread_v210_patches.json` holds **our** edits only: for each patched file, the SHA-256 of the C that setup
  generates from the player's ExeFS, the line range to replace, and the replacement lines we wrote (frame pacing,
  vector growth, zlib, cutscene skip). It contains no game code, no data from the game and no decryption.
- `dread/romfs_v210.manifest` lists RomFS file names, sizes and SHA-1 hashes, used only to check the player's files.

## Contributors

- **yozander2009-stack** — project lead
- **Claude** (Anthropic) — AI pair programmer: runtime fixes and performance work, the front end and in-game menu,
  setup, the Linux build, testing and documentation

Button-icon packs (Controls > Button icons) are community mods downloaded from GameBanana when a player asks for
them, not included here: Switch to XBOX Layout by Krydar, Switch to PS4 Layout by Nova77, Switch to PS5 Layout by
xSeRosiSx, with PlayStation art from Xelu's FREE Controller Prompts.

The program icon is the [Metroid icon created by Smashicons - Flaticon](https://www.flaticon.com/free-icons/metroid)
(Flaticon licence, free with attribution).

Third-party work this builds on is credited below and on the game's Credits page.

## AI use

Large parts of this project were written with Claude (Anthropic), not only boilerplate: runtime fixes, performance
work, the front end, setup and the build scripts. Every change was built and tested on real hardware before release,
and each fix was measured. The source is all here: read it, build it yourself, and report anything that looks wrong.

## Licences

**The released EXE is distributed under GPL-3.0** (`LICENSE-GPL-3.0.txt`), because it contains the ScarletNX renderer.
Each source file keeps its own licence; Apache-2.0 and MIT code is GPL-3.0-compatible.

- SwitchRecomp: Apache-2.0. The credit "SwitchRecomp by MZ Labs" and NOTICE.txt must be kept.
- ScarletNX renderer: GPL-3.0, which makes the combined EXE GPL-3.0.
- Dear ImGui: MIT.
- libvpx and OpenH264: BSD.
- SDL2 and zlib: zlib licence.

NVIDIA DLAA: the code is here (`dlss.c`), NVIDIA's files are not, and the release is built without it. To add it to
your own build, see [DLSS.md](DLSS.md). A build with DLSS is for your own use only: never share it.

Metroid Dread is © Nintendo. This project is not affiliated with or endorsed by Nintendo.
