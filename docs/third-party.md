# Credits and licenses

## People
- **yozander2009-stack**: project lead, testing and direction.
- **Claude** (Anthropic): AI pair programmer: runtime fixes and performance work, the launcher and in-game menu,
  setup, the Linux build, testing and documentation.
- **DaniSald**: Linux build, Linux keyboard and mouse, Z-57 at 16:10.
- **ignaciotapie**: ultrawide pop-in fix.
- **theboy181**: 60 FPS cutscenes, 21:9 and other Dread patches
  ([switch-ptchtxt-mods](https://github.com/theboy181/switch-ptchtxt-mods)); 21:9 commissioned by Nikorasu081.
- **JumpmanClub Brasil**: Brazilian Portuguese fan translation: MrVtR (Vitor Ribeiro), translator, romhacker and
  proofreader; CaninosVermelhos (Philipe Rodrigues), Davi Prado and hhgo cb, translators; EsaanAlencar, proofreader.
- **Krydar, Nova77, xSeRosiSx**: Xbox, PS4 and PS5 button-icon mods (downloaded from GameBanana on request), with
  PlayStation art from Xelu's FREE Controller Prompts.
- **Smashicons (Flaticon)**: the program icon, [Metroid icon](https://www.flaticon.com/free-icons/metroid) (Flaticon
  licence, free with attribution).

## Code this builds on
| Project | Use | License |
|---|---|---|
| SwitchRecomp by MZ Labs | the recompiler and runtime | Apache-2.0 (credit and NOTICE.txt kept) |
| ScarletNX | Native Vulkan renderer, Maxwell to SPIR-V shader translation | GPL-3.0 |
| [Open Dread Rando](https://github.com/randovania/open-dread-rando) | Randovania 2.1.0 executable patch definitions and controller-input replacement | GPL-3.0 |
| Dear ImGui | launcher and in-game menu | MIT |
| Intel XeGTAO | ambient occlusion, ported to GLSL | MIT |
| Ryujinx | reverb port (audio) | MIT |
| SDL2 | controllers, Linux audio | zlib |
| zlib | game-data decompression | zlib |
| libvpx | VP9 video | BSD |
| OpenH264 | H.264 video | BSD |
| Clang / LLVM (llvm-mingw on Windows) | bundled compiler | Apache-2.0 with LLVM exception |
| GCC, binutils, MinGW-w64 | fallback compiler | GPL / LGPL with runtime exception |
| Python | runs setup | PSF |

The Ryujinx and yuzu projects' public documentation of the Switch audio renderer and services was used as reference.

## License of the release
The released program is **GPL-3.0** (`LICENSE-GPL-3.0.txt`) because it contains the ScarletNX renderer. Each source
file keeps its own license; Apache-2.0 and MIT code is GPL-3.0-compatible. Full license texts ship in the `licenses`
folder next to the game.

NVIDIA DLAA calls NVIDIA's NGX through the graphics driver; no NVIDIA SDK code or header is compiled in, and NVIDIA's
`nvngx_dlss.dll` is never in this repository or the release. See [DLSS.md](../DLSS.md).

*Metroid Dread* is © Nintendo. This project is not affiliated with or endorsed by Nintendo.
