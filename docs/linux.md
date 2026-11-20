# Linux and Steam Deck

## Playing

Download `Metroid_Dread-x86_64.AppImage` from the
[latest release](https://github.com/yozander2009-stack/Metroid-Dread-Recomp/releases/latest), make it executable
(right-click > Properties > Allow executing, or `chmod +x`) and run it. It has everything it needs inside: the
program, the recompiler, a compiler (Clang, with GCC as a fallback) and Python. No game files are inside.

- The game is installed in `~/.local/share/MetroidDreadRecomp` (or `METROID_DREAD_HOME`).
- Needs glibc 2.38 or newer (SteamOS 3.5+), an OpenGL 4.3 driver, and SDL2 for controllers.
- Native Vulkan needs Linux 6.7 or later; OpenGL is used otherwise.
- The AppImage updates itself from the Play screen.
- **Steam Deck:** set it up in Desktop Mode, then **Add to Steam** on the Play screen to play from Game Mode.
- Not on Linux: NVIDIA DLAA, HDR, F7 recording.

## Building from source

Native Linux (X11, or Wayland through XWayland), built from this repository with the system's GCC. The scripts in
`linux/` keep everything inside the repository: the runtime in `bin/`, the installed game in `games/`. You need your
own Metroid Dread 2.1.0 extracted to an ExeFS and a RomFS folder, as for Windows.

```
bash linux/install_deps.sh                    # gcc, X11/OpenGL headers, static libvpx, SDL2 (asks for sudo)
bash linux/setup_and_build_runtime.sh         # bin/runtime_linux_x11, with the in-game menu
bash linux/setup_game.sh GAME_FOLDER [UPDATE_FOLDER]   # checks the files, compiles games/Metroid Dread_code.so
bash linux/run.sh                             # play; settings in games/Metroid Dread.ini, log in games/Metroid Dread.log
```

`linux/run.sh` opens the same Play / setup / settings screens as the Windows program (`--play` skips them,
`--launcher` brings them back). `linux/build_game_so.sh` recompiles only what changed, `linux/package.sh` makes the
Steam Deck package in `linux/dist/`, and `run_test.sh`, `menu_test.sh` and `run_route_linux.sh` run the automated
tests on a copy of the game folder (`SWR_SOFTGL=1` for Mesa's software OpenGL).

### AppImage

`VERSION=x.y.z bash linux/make_appimage.sh` builds `linux/dist/Metroid_Dread-x86_64.AppImage` and
`linux/dist/update-linux.txt` (publish both with a release). It bundles a relocatable GCC 14.3
([Bootlin](https://toolchains.bootlin.com)), Clang / LLVM and Python 3.12
([python-build-standalone](https://github.com/astral-sh/python-build-standalone)). Downloads are checked against their
SHA-256 and cached in `linux/appimage/cache/`.
