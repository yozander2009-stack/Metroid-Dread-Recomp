# Road to 1.0

Current builds (0.x) are previews. **1.0** is the first release meant to be solid everywhere: the whole game playable
start to finish on Windows, Linux, the Steam Deck, Android and macOS, on NVIDIA, AMD and Intel graphics, at a locked
frame rate, with no known crash.

## Milestones

| Version | Goal |
|---|---|
| **0.5 – 0.8** | Windows and Linux perfect: every item below done |
| **0.9** | Android and macOS ports, on the same code |
| **1.0** | Everything polished on every platform |

Legend: ✅ done · 🔧 fixed on `main`, in the next release · ⬜ left to do · 💭 after 1.0, maybe

## Windows and Linux (0.5 – 0.8)

### Performance: locked frame rates
| | |
|---|---|
| ✅ | 60 FPS locked in normal play, 240 FPS on fast PCs (OpenGL and Native Vulkan) |
| ✅ | Faster heavy scenes on OpenGL (constant uploads, vertex ranges) |
| ✅ | Faster loading (native zlib, larger vectors, Clang-compiled game code) |
| ⬜ | Locked 120 in the heaviest effect scenes (the game's own code on the main thread is the limit there) |
| ⬜ | Locked 60 in the Steam Deck's heaviest boss fights |
| ⬜ | Shorter hitches when a video starts or on the first entry into a room |
| ⬜ | No first-time pipeline stutter on Native Vulkan, even on a fresh install |
| ⬜ | 60 FPS on 4-core CPUs and Iris Xe-class integrated graphics at 1x |

### Stability
| | |
|---|---|
| ✅ | Reload / video freeze (game condition variable order) |
| ✅ | Acid enemy and heavy-effect crash at high frame rates |
| ✅ | Outdated game code started after an update ([#75]) |
| 🔧 | Energy recharge station crash ([#73]) |
| ⬜ | AMD OpenGL driver crash: Native Vulkan works around it ([#12], [#39], [#49], [#55], [#45], [#71]) |
| ⬜ | Native Vulkan on some AMD cards ([#68], [#69]) |
| ⬜ | Crash in the E.M.M.I. chase in Ghavoran ([#66]) |
| ⬜ | Memory growth at the Corpius boss ([#65]) |
| ⬜ | Instant death loop ([#80]) |
| ⬜ | Update problems reported on 0.5.10 ([#86], [#83]) |

### High frame rates
| | |
|---|---|
| ✅ | Gameplay at 90 / 120 / 144 / 240 / unlimited |
| ✅ | Cutscenes and loading screens at the frame-rate limit |
| ✅ | Map panning and hit pauses at any frame rate |
| ⬜ | Slow-motion sequences above 60 ([#85]): currently paced at 60, should run at the full rate |
| ⬜ | Cutscene audio sync above 60 ([#81]) |
| ⬜ | Quick-time event timing ([#21]) |
| ⬜ | Slowdown in E.M.M.I. zones and boss rematches ([#58]) |

### Graphics
| | |
|---|---|
| ✅ | OpenGL and Native Vulkan renderers |
| ✅ | Resolution scaling, 16:10, anisotropic filtering, sharpening, shadow resolution, bloom options |
| ✅ | NVIDIA DLAA |
| 🔧 | TAA and ambient occlusion without flicker or lines ([#5]) |
| ⬜ | Flash Shift black block on Intel UHD / GT 1030 ([#46]) |
| ⬜ | Door and background-screen glitches ([#47], [#74], [#33]) |
| ⬜ | 21:9 and 32:9 out of experimental ([#19]) |
| ⬜ | HDR out of experimental ([#8]) |
| ⬜ | TAA and ambient occlusion cheap enough for the Steam Deck |

### Controls
| | |
|---|---|
| ✅ | SDL2 controllers, keyboard remapping, mouse aim, button prompts |
| ✅ | Rumble and HD rumble (Switch Pro, Joy-Con, DualSense) |
| 🔧 | D-pad movement ([#79]) |
| 🔧 | Stick snap-back filter that catches fast bounces ([#54]) |
| ⬜ | Switch 2 Pro Controller on Linux without re-plugging ([#64]) |

### Platforms and setup
| | |
|---|---|
| ✅ | Windows build with one-click updates and undo |
| ✅ | Linux AppImage with updates, Add to Steam |
| ✅ | Bundled Clang compiler (fast setup) |
| ⬜ | Steam Deck Game Mode fully checked (controls, suspend, performance) |

### Player features
| | |
|---|---|
| ✅ | Skip cutscenes, FPS counter, AUTO-SET, REPORT button |
| ✅ | Mods and texture packs, Atmosphere cheats, amiibo, 5.1 audio |
| 🔧 | Built-in cheats that work on the recompiled code, incl. infinite Aeion and a free camera ([#70], [#37]) |
| 🔧 | In-game menu reopening with Esc ([#78]) |
| ⬜ | Portuguese translation together with button-icon packs ([#41]) |

## Android and macOS (0.9)

Started once Windows and Linux are done, so both ports begin from finished code.

| | |
|---|---|
| ⬜ | Android build on the Native Vulkan renderer, with setup on the phone ([#6], [#23]; a port is in progress) |
| ⬜ | Android: touch controls, controller support, Mali / Adreno GPUs |
| ⬜ | macOS build on Apple Silicon and Intel, Vulkan through MoltenVK ([#29]) |
| ⬜ | One-click updates and the Play screen on both |

## 1.0
| | |
|---|---|
| ⬜ | Every item above done, and no known crash on any platform |

## After 1.0, maybe
| | |
|---|---|
| 💭 | Model and higher-resolution texture packs (work started) |
| 💭 | Randovania / randomizer support ([#77]) |
| 💭 | More languages ([#76]) |
| 💭 | Game version 1.0.0 ([#51]) |
| 💭 | More graphics upgrades ([#9]), stereoscopic 3D ([#82]) |

Something missing? [Open an issue](https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/new/choose).

[#5]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/5
[#6]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/6
[#8]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/8
[#9]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/9
[#12]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/12
[#19]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/19
[#21]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/21
[#23]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/23
[#29]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/29
[#33]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/33
[#37]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/37
[#39]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/39
[#41]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/41
[#45]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/45
[#46]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/46
[#47]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/47
[#49]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/49
[#51]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/51
[#54]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/54
[#55]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/55
[#58]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/58
[#64]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/64
[#65]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/65
[#66]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/66
[#68]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/68
[#69]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/69
[#70]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/70
[#71]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/71
[#73]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/73
[#74]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/74
[#75]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/75
[#76]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/76
[#77]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/77
[#78]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/78
[#79]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/79
[#80]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/80
[#81]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/81
[#82]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/82
[#83]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/83
[#85]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/85
[#86]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/86
