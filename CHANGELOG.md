# Changelog

Full notes for each version are on the [releases page](https://github.com/yozander2009-stack/Metroid-Dread-Recomp/releases).

## 0.6.0 (not released yet)

### New
- **Randovania 2.1.0 single-player seeds** ([#77]): import through Mods & Texture Packs, pick a seed on the Play screen
  or in the in-game menu (restarts the game); each seed has its own saves. Patch definitions from Open Dread Rando.
- **Resolution presets** 720p to 8K, plus SCREEN; internal resolution up to 9.6x, capped to the GPU's memory above 4x.
- **21:9 and 32:9 are no longer experimental**: a Hor+ view that shows more to the sides at the same height as 16:9
  ([#19]).
- **30 and 40 FPS caps** for handhelds ([#39]).
- **Story videos keep their 16:9 shape** at 21:9, 32:9 and 16:10 ([#88]).
- **Cheats page** on the Play screen: built-in cheats (infinite energy and ammo, E.M.M.I. counter always succeeds,
  moon jump), Atmosphere cheat files with what each one can do, import from a file or folder ([#70]).
- Infinite energy and ammo show the infinity sign on the HUD.
- **Infinite Aeion** cheat: Phantom Cloak, Pulse Radar and Flash Shift never drain the gauge.
- **Free camera** cheat: the right stick moves the camera around (hold L3 to move it closer or farther); turning it off
  brings the camera back.
- In-game menu: cheats have their own CHEATS tab, and Gameplay > Startup can skip the Play screen ([#83]).
- **D-pad movement** option ([#79]).
- Setup names the compiler it uses.

### Fixes
- TAA no longer makes textures jitter when shooting; ambient occlusion no longer draws a line at Samus's height or
  copies textures. TAA and AO are no longer experimental ([#5]).
- Energy recharge station crash ([#73]).
- 16:10: the camera keeps following Samus ([#83]).
- Esc reopens the in-game menu reliably ([#78]).
- Above 60 FPS, slow-motion moments (the E.M.M.I. counter and slide) are paced at 60 from their first frame, so they
  no longer fail ([#85], [#80], [#21]).
- Stick snap-back filter catches fast bounces and lets full-tilt turns through ([#54]).
- Loading screens and cutscenes run at the frame rate however the game is started.
- After a rebuild, the Play screen no longer asks to rebuild again.
- Videos decode on several threads on Windows: the Game Over and title videos start without a dropped frame, and the
  boot video's pictures are decoded correctly (some came out wrong before).
- Above 60 FPS, the acid enemy's acid balls no longer stay on the ground and flood the screen with splashes: they burst as
  at 60, and the fight holds 120 / 240 FPS (it dropped to 75-100).
- Native Vulkan on GPUs without dynamic vertex input (AMD's driver): the game's pipeline list now builds the pipelines
  draws use at the start, so far fewer stutters the first time a scene appears.
- Native Vulkan on AMD's Windows driver: dynamic vertex input off by default (black picture on an RX 6700 and AMD
  handhelds), and no crash when TAA starts ([#68]).
- A renamed copy of the game EXE uses the game's own settings, saves and game code ([#68]).
- Native Vulkan: SHFL within a pixel quad runs as a quad operation (ScarletNX 3963d78b), which fixes Samus's plug-in
  animation on AMD's driver; with dynamic vertex input the vertex layout is set before the buffers are bound.
- Controller vibration no longer buzzes forever after the first Omega Blaster, and stops when the game closes or is in
  the background; holding a button fast-forwards videos at 2x at any frame rate ([#93]).
- Title screen: releasing A as the intro ends no longer leaves the game stuck.
- Hologram screens show their own video ([#74]).
- Esc reopens the in-game menu after it was closed with Esc ([#91]).
- Setup works from a folder with accented letters ([#89]).
- Fast loading waits for a loaded save on Windows too (it could stall the title).
- Fewer texture re-uploads and video conversions at high frame rates.
- Linux: crash logs name the runtime thread and list its calls ([#45]).

## 0.5.10 (2026-11-19)
- AMD Radeon cards start on Native Vulkan; Native Vulkan fixes for AMD ([#68], [#69]).
- PLAY rebuilds outdated game code before starting ([#75]).
- Update buttons no longer share an ID ([#72]); offer of the faster Clang compiler for older installs ([#67]).

## 0.5.9 (2026-11-12)
- New: amiibo ([#63]), NVIDIA DLAA, TAA and ambient occlusion ([#5]), ReShade depth, shadow resolution, bloom options,
  stick sensitivity ([#35]), snap-back filter ([#54]), in-game menu on Native Vulkan, Esc opens the menu ([#38]),
  HD rumble, cutscenes and loading screens at the frame-rate limit, mod videos at any size ([#31]), RecompLauncher,
  Native Vulkan on Linux.
- Faster: Clang setup, Native Vulkan pipeline list and yuzu cache import, 240 FPS on OpenGL, heavy scenes on OpenGL.
- Fixes: Z-57 at 16:10 ([#56], [#59]), ultrawide pop-in ([#61]), map panning ([#53]), minimap ([#52]), ADAM's room audio
  ([#34], [#36]), acid enemy at high frame rates, game FXAA off by default, setup compiler errors ([#57]), small windows
  ([#48]), Linux screen dimming ([#50]) and dead zone ([#40]), Steam Deck pacing and audio.

## 0.5.0 (2026-10-30)
- Linux build and AppImage ([#3], [#22]), keyboard remapping ([#25], [#14]), menu button swap ([#13]), 5.1 audio ([#28]),
  cheats ([#11]), 32:9 ([#27]), Brazilian Portuguese fan translation ([#15]), 90 FPS.
- Fixes: no sound on some PCs ([#16]), AMD crash after loading a save ([#12], [#7]), Windows display scaling ([#17]),
  acid enemy crash, keyboard L3/R3 ([#32]), Chinese text ([#30]), rumble ([#20]).

## 0.4.0 (2026-10-24)
- REPORT button, previous log kept, crash logs name the file that crashed, Native Vulkan shaders prepared at start.
- Portuguese setting no longer breaks the menus.

## 0.3.1 (2026-10-24)
- Setup stuck at 22%, cutscene music cut halfway, first-time effect stutter, slow OpenGL fallback.

## 0.3.0 (2026-10-23)
- One-click updates with undo, AUTO-SET, sharpening; setup finds dumps a few folders down.

## 0.2.0 (2026-10-23)
- Skip cutscenes, FPS counter, Add to Steam, program icon, mouse aim; even 30 FPS cutscenes, 4x lighting, reverb.

## 0.1.0 (2026-10-22)
- First preview: setup, Play screen, in-game menu, OpenGL and Native Vulkan.

[#3]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/3
[#5]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/5
[#7]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/7
[#11]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/11
[#12]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/12
[#13]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/13
[#14]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/14
[#15]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/15
[#16]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/16
[#17]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/17
[#19]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/19
[#20]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/20
[#21]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/21
[#22]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/22
[#25]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/25
[#27]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/27
[#28]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/28
[#30]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/30
[#31]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/31
[#32]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/32
[#34]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/34
[#35]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/35
[#36]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/36
[#38]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/38
[#39]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/39
[#40]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/40
[#45]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/45
[#48]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/48
[#50]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/50
[#52]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/52
[#53]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/53
[#54]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/54
[#56]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/56
[#57]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/57
[#59]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/59
[#61]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/61
[#63]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/63
[#67]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/67
[#68]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/68
[#69]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/69
[#70]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/70
[#72]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/72
[#73]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/73
[#74]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/74
[#75]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/75
[#77]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/77
[#78]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/78
[#79]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/79
[#80]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/80
[#83]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/83
[#85]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/85
[#88]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/88
[#89]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/89
[#91]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/91
[#93]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/93
