# How the macOS port works

Notes on what it took to run the project on a Mac, and why, for whoever maintains or extends it. To build and play,
see [README.md](README.md).

## The approach

macOS stops at OpenGL 4.1, and the project's OpenGL renderer needs 4.3 (compute shaders, storage buffers, image
load/store). So the Mac build never draws with OpenGL. It always uses the native Vulkan renderer (ScarletNX,
`runtime/snxvk`), which runs on Metal through [MoltenVK](https://github.com/KhronosGroup/MoltenVK). ScarletNX already
supported MoltenVK: `device.c` asks for portability enumeration and enables `VK_KHR_portability_subset`.

The rest of the stack was already portable:

- The game code is ARM64 translated to C, and the translator already supported ARM64 hosts (the Android build). On
  Apple silicon `LDAR`/`STLR`/`DMB` become real acquire/release atomics, so the weaker ARM memory model is handled.
- The runtime's POSIX layer (`plat.c`) existed for Linux.
- Setup (`dread/dread_setup.py`) is pure Python and runs on the Xcode command line tools' Python 3.9.

What was missing was a window backend, a POSIX build of the Vulkan renderer and its adapter, a few macOS-specific
fixes in the platform layer, and the build plumbing.

Tested on an M5 MacBook with macOS 26.5: full 2.1.0 setup (9774 RomFS files verified, 20 patches applied), the game
boots and plays at about 60 FPS, with the VP9 videos and sound. Intel Macs are untested.

## Pieces

| file | what it does on the Mac |
|---|---|
| `runtime/backend_sdl.c` | **new.** SDL2 window with a Vulkan surface, keyboard, controllers, the test replay hooks. The macOS counterpart of `backend_x11.c` |
| `runtime/snxvk_glue.c` | the native Vulkan adapter, now built on POSIX as well as Windows |
| `work-profiler/build_snxvk2.py` | builds `runtime/snxvk/libsnxvk_mac.a` with clang |
| `runtime/plat.c` | condition variables, 16 KB pages, crash handler, dialogs, thread stacks and QoS |
| `runtime/posix_compat.h` | **new.** force-included into the generated game code |
| `switchrecomp.py` | `--target macos`, `--backend sdl` (the defaults on a Mac), `_code.dylib`, clang and SDL2 flags |
| `dread/dread_setup.py` | clang instead of the bundled GCC, `.dylib` output |
| `macos/build.sh` | **new.** builds the runtime, runs setup, installs a self-contained game folder or makes `Metroid Dread.app` |
| `third_party/khronos/` | **new.** the OpenGL headers macOS lacks |

### Window backend (`backend_sdl.c`)

- **Main thread.** Cocoa only allows windows and events on the main thread. `video_init` and `video_poll` already run
  there (`main.c`'s frontend loop), so the window, its events and the Vulkan surface are made there too.
- **Vulkan library.** SDL loads it, trying in order: `libMoltenVK.dylib` next to the game (the packaged folder), the
  Vulkan loader next to the game, Homebrew's loader, Homebrew's MoltenVK, then SDL's own search.
  `SWR_VULKAN_LIBRARY=<path>` overrides it. MoltenVK on its own is a complete Vulkan implementation, so the packaged
  game needs no loader or ICD file.
- **Retina.** The swapchain size is `SDL_Vulkan_GetDrawableSize` (pixels, not points).
- **OpenGL stubs.** The OpenGL renderer is compiled in but never started, so the backend answers `backend_gl_*` with
  "no context". This keeps `gpu_gl.c`'s shared parts (the skip-cutscene prompt, the FXAA and sharpening settings,
  screenshot dispatch) working unchanged. It is also why the Khronos headers are vendored: the system has no
  `GL/glcorearb.h`, and the runtime loads every GL entry point at run time anyway.
- **Keys and controllers.** The same mapping as Windows and Linux. Cmd+F, Cmd+Enter or F11 for fullscreen, Cmd+Q or
  Esc to quit, F1 shows the controls (there is no in-game menu).

### Native Vulkan renderer

- **Symbol renaming without objcopy.** On Windows, `build_snxvk2.py` renames every global of the ScarletNX objects to
  `snxv_<name>` with `objcopy --redefine-syms`, so its GPU front end can sit next to the runtime's. Apple's toolchain
  has no objcopy, so the Mac build does it with the system linker: `ld -r -alias_list` gives each global an alias
  `snxv_<name>`, and `-exported_symbols_list` keeps only the aliases global (`ld -r` turns the rest local). The one
  redirected undefined symbol (`g_opt_vsync` → `snxvk_opt_vsync`) is done with `-D`. `-fno-common` makes sure every
  global is a real definition with an alias.
- **The adapter on POSIX.** `snxvk_glue.c` was Windows-only. Its platform calls now have POSIX versions (trylock,
  file mapping, atomic rename, `dlerror`, `setenv`, thread id, `strerror`), the `Interlocked*` counters became
  `__atomic`, and the spin-wait uses `yield` on ARM.
- **No write watch.** On Windows the renderer's caches use `GetWriteWatch` to skip memory the game hasn't written.
  macOS has no cheap equivalent. An `mprotect` + fault-handler scheme would break `pread` straight into guest memory
  (`EFAULT`). So `plat_ww_scan` returns -1 and the renderer compares contents instead, which upstream's `buffer.c`
  already supports. It works; it may cost speed in busy scenes.
- **Always on.** `g_snxvk_want` is 1 on macOS whatever the `.ini` says.

### Platform layer (`plat.c`, POSIX part)

- **Condition variables.** macOS has no `pthread_condattr_setclock`, so timed waits use
  `pthread_cond_timedwait_relative_np`.
- **16 KB pages.** Apple silicon has 16 KB pages; the Switch, and so the game's memory map, uses 4 KB. `mprotect` on
  a range that isn't 16 KB-aligned fails. The reservation now keeps a bit per 4 KB guest page:
  - a commit makes every host page that holds a committed guest page accessible;
  - a host page is only given back once none of its guest pages is committed;
  - until then a decommitted 4 KB page is zeroed in place, as it would read after a fresh commit.
  This only switches on when the host page is larger than 4 KB, so x86-64 Linux behaves exactly as before.
- **Zeroing on macOS.** `MADV_DONTNEED` does not zero anonymous memory on macOS, so a released range is replaced
  with fresh pages (`mmap MAP_FIXED`).
- **Thread stacks.** A size that isn't a whole number of pages makes `pthread_attr_setstacksize` fail, and macOS then
  silently gives the thread a 512 KB stack, too small for the recompiled code. Sizes are rounded up.
- **Thread QoS.** Runtime threads get `QOS_CLASS_USER_INTERACTIVE`; at the default class macOS also schedules them on
  the efficiency cores. `plat_thread_low/high_priority` map to QoS classes.
- **Crash handler and dialogs.** Darwin's `ucontext` layout for the faulting PC. Errors are shown with `osascript`,
  which works from any thread, unlike an AppKit alert.

### Game code and setup

- **`posix_compat.h`.** The Dread patches call three Windows functions: `GetEnvironmentVariableA` for the `SWR_*`
  switches, and `LoadLibraryA("zlib1.dll")` with `GetProcAddress`. The header maps them to `getenv` and to `dlopen` of
  the system's `libz.1.dylib`. It is force-included into every generated file, so the verified patches apply
  unchanged. Same idea as `linux/lx_compat.h`.
- **Flags.** The game code is compiled with `-fPIC -fvisibility=hidden` (only `swr_module` is exported),
  `-mmacosx-version-min=12.0`, and linked as a `.dylib` whose install name is `@loader_path`.
- **128-bit CAS.** `cpu_helpers.c` (included into the game code) now uses the compiler's 16-byte compare-and-swap on
  arm64 with clang, a single `caspal`. Before, it took a two-step fallback.
- **Setup.** `dread_setup.py` uses Apple clang with the same flags as `switchrecomp.compile_all`. `same_volume`
  compares device IDs on POSIX; it compared drive letters before, so it was always true. It can run straight from the
  repository's `dread/` folder.
- **Executable path.** `main.c` finds its own folder through `_NSGetExecutablePath` + `realpath`. A relative path
  given to `dlopen` makes dyld search its library folders before the game's.

### Packaging (`macos/build.sh install`)

The game folder gets copies of SDL2 and MoltenVK so it runs without Homebrew:

- Homebrew's SDL2 is sdl2-compat (SDL2's API over SDL3). It opens `libSDL3.dylib` from its own folder, so SDL3 is
  copied too.
- The install names are rewritten to `@executable_path` / `@loader_path`.
- `install_name_tool` invalidates the code signatures, and Apple silicon runs nothing unsigned, so every binary is
  signed again ad hoc (`codesign -s -`).

### The app (`macos/build.sh app`)

`Metroid Dread.app` is the same runtime in a bundle:

- `Contents/MacOS` has the program, the game code and the libraries.
- `Contents/Resources` has the icon (`runtime/app.ico` converted with `sips` and `iconutil`) and the game's `exefs`
  and `romfs`. They are copied with `cp -c`, an APFS clone, so on the same disk they take no extra space.
- The `Info.plist` names it a game (`public.app-category.action-games`, so macOS can use Game Mode) and asks for
  macOS 12.
- The whole bundle is signed ad hoc.

The runtime must never write into the bundle: that would break the signature's seal, and the app may sit in a
read-only place. `main.c` (`app_dirs`) notices it runs from `.app/Contents/MacOS/` and switches folders:

- `~/Library/Application Support/Metroid Dread` becomes the game folder: settings, log, hints, saves and the Vulkan
  caches go there, and it is the working folder (screenshots).
- Its `Metroid Dread_data/exefs` and `romfs` are symbolic links into the bundle's `Resources`, made again at every
  start, so the app can be moved.
- The game code and MoltenVK are still loaded from `Contents/MacOS` (`g_bindir`).

Outside an `.app` nothing changes. The build copies the game folder's settings, saves and shader cache to Application
Support the first time, and never overwrites them.

## Pitfalls hit along the way

- **Black window.** The renderer remembers a window "generation" starting at 0, and makes a new surface whenever the
  backend reports a different one (Android needs this). The backend first reported 1. So on the first frame the
  renderer made a second surface from its GPU thread, and SDL attached a new Metal view to the window off the main
  thread, which never showed. The game ran, and screenshots from the renderer were fine, but the window stayed black.
  `backend_window_generation` now returns 0.
- **Line endings.** The repository is `* -text` with a mix of CRLF and LF files. Rewriting a CRLF file in text mode
  turns it into a whole-file diff. Check with `git ls-files --eol`.
- **Sampler limit warning.** MoltenVK reports 16 samplers per stage while the renderer's layout declares 64 textures,
  so the log says "pipelines may fail" at every start. With Metal argument buffers (MoltenVK's default) the
  update-after-bind limit is 500000, and the game renders correctly. The warning is harmless here.

## What isn't on the Mac

- No Play screen (it is Direct3D 11) and no in-game menu (it needs the OpenGL renderer): settings are in
  `Metroid Dread.ini`.
- No DLSS, HDR output, F7 recording, frame generation, mouse aim or Tab fast forward.
- Setup needs an extracted ExeFS and RomFS, as on other platforms. No extraction tool or keys are included.

## Effect on other platforms

Every change is new macOS code, guarded by `__APPLE__`, or POSIX code that behaves as before on 4 KB pages. The
changed C files cross-compile for Windows with MinGW-w64, and the symbol tables of `plat.o` and `snxvk_glue.o` match
the originals. The POSIX changes were not built or tested on Linux.
