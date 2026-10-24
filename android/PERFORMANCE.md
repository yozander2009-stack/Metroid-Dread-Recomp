# Retroid Pocket Nova observations

Tested on October 23, 2026 with Metroid Dread 2.1.0, based on upstream commit
`578f6ec` plus this Android port. The APK was cross-compiled on an Apple Silicon
Mac with NDK 27.1.12297006 and Clang 18, using `-O2`.

Device: Retroid Pocket Nova, Android 13/API 33, ARM64, Adreno 740, 1280x960 display
at 120 Hz. Renderer: Qualcomm OpenGL ES 3.2 driver V@0676.53, dated December 27,
2023. The table below describes the initial OpenGL ES port; it does not measure the
subsequent experimental Vulkan build. Frame generation is not included.

| Workload | Observed performance | Evidence |
| --- | --- | --- |
| Active gameplay | Averages in the 40s FPS | Device owner's hands-on report; representative of their experience, not a computed benchmark mean |
| Recorded Omega Blaster sequence | About 25–26 FPS in sampled frames | 20-second device screen recording with visible FPS counter; recording overhead was not isolated |
| Title screen | Up to 120 FPS | Runtime log: `performance: 120.0 FPS` |
| Mostly static gameplay scene | Roughly 100 FPS in some samples | Runtime log samples of 103.5, 103.9, and 104.5 FPS; not representative of active gameplay |
| Original cinematics | Around 30 FPS | Original cinematic pacing retained; `cutscenes_60fps=0` |

The 4:3 run used `docked=1`, `aspect=4:3` (1200x900 base), `resolution=1`,
`fps_limit=120`, `forced_vsync=1`, and `show_fps=1`. Shadows were tested both on
and off (`shadows=0` applies live); the roughly 100 FPS samples occurred with
shadows off in a mostly static scene. A 120 FPS cap is a ceiling, not a claim
that the device sustains that rate.

These observations are from exploratory play, not a fixed-route benchmark or a
controlled comparison between shadow settings. Scene complexity, movement,
shader warm-up, power mode, and thermals were not controlled. Short idle-scene
samples must not be described as a 100 FPS gameplay average. Sustained 120 FPS
gameplay has not been achieved. Game-speed correctness above 60 FPS also has
not been systematically validated.

The locally captured sample video is a 20-second, 1280x960 H.264 recording captured with
Android `screenrecord` at 2.5 Mbps, without audio. Its sampled frames show the
counter around 25–26 FPS during an Omega Blaster sequence. The recording's
nominal display capture rate must not be interpreted as the game's rendering
rate. Use the visible counter and the owner's active-play report instead.

The runtime's `GPU thread CPU busy` percentage describes host rendering-thread
CPU time, not hardware GPU utilization. It does not establish a hardware GPU
bottleneck on its own.

Functional checks passed: signed APK installation and update over ADB, all
9,774 RomFS files verified by SHA-1 on-device, 4:3 rendering, physical controller
input, AAudio initialization, VP9 playback, saving, and loading the saved game
after redeployment. Only this device has been tested; long-term stability,
other Android GPUs, audio route changes, and desktop regressions remain untested.

## Launcher and native Vulkan follow-up

The optional `--vulkan` build starts the native ScarletNX renderer on the Nova:
Adreno 740, Vulkan 1.3.128, system driver `802a4035`, 1280x960 FIFO swapchain
with six images. Title rendering, save-selection UI and the loaded save room
were checked. One native-resolution save-room screenshot showed 37 FPS; that is
a single static-scene sample, not a gameplay average. These checks establish
functional rendering, not an active-gameplay speedup. No
controlled GLES/Vulkan performance comparison has been completed.

Background lifecycle check: pressing Home produced `Android: game paused`,
and two consecutive Android `top` samples showed 0.0% CPU for the game process.
Returning resumed the same process and recreated the Vulkan swapchain. Controller
detection reported the built-in physical controller before a button was pressed.
With touch enabled in GLES, auto-hide suppressed the overlay; turning auto-hide
off made it visible again. Both cases were checked with device screenshots.

The launcher exposes 50–200% internal resolution, 60/90/120 FPS or unlimited,
shadows, FXAA, anisotropy and VSync. Its Performance preset chooses 75% resolution
and disables shadows/FXAA; Quality chooses native resolution, shadows/FXAA and
4× filtering. Neither preset changes the selected FPS limit or promises a rate.

## Custom-driver validation

The Vulkan build now imports standard AdrenoTools driver ZIPs and offers System
or an installed custom package in the launcher. No vendor/name allowlist is used.
The Nova's existing `Turnip Adreno Driver T23 (@Mr_Purple_666)` package imported
through Android's document picker and actually initialized as
`PurpleVK-public Adreno (TM) 740`, Vulkan 1.4.335, driver `06463063`. The selected
`vulkan.purple.so` was verified in the game process's memory maps. The title and
loaded save room rendered. A package with generic name/vendor metadata also
imported and initialized through the same path.

The custom-driver save-room screenshot showed 78 FPS at 75% resolution with
shadows and FXAA off. This is a static-scene sample, not active-gameplay average
or a controlled comparison: the earlier system-Vulkan 37 FPS screenshot used
native resolution. These numbers must not be presented as a driver speedup.

On-device negative import checks rejected directory traversal and a non-ARM64
ELF payload, left no partial staging installation, and retained the previous
selection. Successful-package import and persisted selection were checked.
Driver-start failure recovery and other driver binaries remain untested. Driver
packages remain user-supplied; none are included in Git or in the APK.
