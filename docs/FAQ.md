# FAQ

Read this before opening an issue: questions it already answers are closed. Known problems are listed at the end and
in the [Road to 1.0](ROADMAP.md).

## Game files

**Which version of the game do I need?**
Metroid Dread **2.1.0**, the last update. Other versions are refused by setup ([#51] tracks 1.0.0 support).

**Setup says it can't find the ExeFS, or that a RomFS on its own is not enough.**
The game is made of two parts and setup needs both, dumped from your own Switch with the 2.1.0 update applied:
- the **ExeFS**: the game code, files `main`, `main.npdm`, `rtld`, `sdk`, `subsdk0`...
- the **RomFS**: the game data, folders `system`, `packs`, ...

Choose the folder that holds both; they can be a few folders down. If the base game and the update are separate dumps,
choose the update as the update folder. This project doesn't explain how to dump or decrypt a game.

**Setup says some RomFS files don't match.**
Every file is checked against the 2.1.0 list. A mismatch means a different version, a dump without the update merged,
or a damaged file: dump again.

**How long does setup take, and how much space does it need?**
A few minutes with the bundled Clang compiler (longer on older CPUs), and about 7 GB. It runs once; updates only
recompile what changed.

**My antivirus stops setup / the compiler.**
Setup runs a bundled compiler on your PC. If Windows or an antivirus blocks it, setup says so. Allow the game folder
and try again.

## Performance and graphics

**OpenGL or Native Vulkan?**
**AUTO-SET** on the Play screen picks for your PC. Native Vulkan is the default on AMD cards; OpenGL is the default
elsewhere. Both are complete renderers; if one misbehaves on your PC, try the other.

**The game crashes on my AMD Radeon card.**
AMD's OpenGL driver crashes for some players (`atio6axx.dll` in the log; [#12], [#39], [#49]). Use **Native Vulkan**,
which AMD cards start on by default since 0.5.10. If Native Vulkan crashes too, use **REPORT** on the Play screen and
attach the zip to [#68].

**Can it run above 60 FPS?**
Yes: 60, 90, 120, 144, 240 or unlimited, with cutscenes and loading screens at your limit. The game uses real frame
times, so gameplay speed doesn't change. A few scripted moments still need work above 60 ([#85], [#81], [#21]): set
the limit to 60 there, or press **F4** to switch between 60 and your limit.

**Short stutters the first time I see an effect.**
Native Vulkan builds each graphics pipeline the first time it's needed. A built-in list prepares most of them before
you play; the rest are saved and reused. On OpenGL keep **Build all shaders before playing** on (default).

**Which anti-aliasing should I use?**
- **TAA:** steadiest edges; works on every GPU. Costs a lot on the Steam Deck.
- **NVIDIA DLAA:** best on RTX cards (downloaded from NVIDIA when you press GET).
- **FXAA:** cheapest; not needed above 1x resolution.

**Is the game sharp enough? It looks soft.**
Raise **Resolution** and turn on **Sharpening** (Graphics). TAA also adds light sharpening.

**Ultrawide?**
16:10 is fully supported (Steam Deck). 21:9 and 32:9 are experimental: the game shows more to the sides, but
some rooms end at the edge of the screen ([#19]).

**HDR?**
Experimental, Native Vulkan only (Graphics > HDR).

## Controls

**My controller doesn't work.**
Any controller SDL2 knows works (Xbox, Switch Pro, Joy-Con, DualShock 4, DualSense, 8BitDo and more). Check
**Controls** on the Play screen: pick player 1 there and try the buttons in the live tester.

**Keyboard keys and mouse aim?**
Every key can be rebound in **Controls > Keyboard**. Mouse aim is in Controls too.

**Xbox or PlayStation button prompts?**
**Controls > Button icons:** Automatic matches your controller, or pick one. The icon packs are community mods,
downloaded from GameBanana when you choose them.

## Saves, mods and cheats

**Where are my saves?**
- Windows: `Metroid Dread_data\save` next to `Metroid Dread.exe`
- Linux / Steam Deck: `~/.local/share/MetroidDreadRecomp/Metroid Dread_data/save`

Close the game and copy the whole folder to back up.

**Mods and texture packs?**
**Mods & Texture Packs** on the Play screen imports a zip or a folder (Atmosphere / LayeredFS layout), and sets order
and on/off. RomFS mods work; ExeFS (code) mods don't, because the game code is recompiled.

**Cheats?**
The **Cheats** page has built-in cheats (infinite energy and ammo, E.M.M.I. counter, moon jump) and takes Atmosphere
cheat files. Cheats that change values work; cheats that patch game code mostly can't, because the code is
recompiled: the page shows which ones won't have an effect.

**amiibo?**
Put your amiibo dumps (`.bin`) in `Metroid Dread_data\amiibo\`, pick amiibo in the game's options, then
**F1 > Gameplay > AMIIBO > SCAN**.

**Portuguese?**
Metroid Dread has no Portuguese of its own. The Brazilian Portuguese fan translation by JumpmanClub Brasil can be
added under **Display > Language**.

## Steam Deck and Linux

**How do I install it on the Steam Deck?**
In Desktop Mode, download `Metroid_Dread-x86_64.AppImage`, make it executable (right-click > Properties > Allow
executing) and run it. Then use **Add to Steam** on the Play screen to play from Game Mode.

**Recommended Deck settings?**
AUTO-SET picks them. Keep TAA and ambient occlusion off on the Deck for 60 FPS.

**Native Vulkan on Linux?**
Needs Linux 6.7 or later; OpenGL is used otherwise.

## Reporting a bug

Use the latest release. Press **REPORT** on the Play screen: it zips your logs, settings and system info (no saves,
no game files) and opens a prefilled issue. Drag the zip into the issue. For a crash in a specific place, attach a
zipped copy of your `save` folder too.

The log is `Metroid Dread.log` next to the game (Linux: in `~/.local/share/MetroidDreadRecomp`); the run before is
kept as `Metroid Dread.previous.log`.

## Known issues

- AMD OpenGL driver crash: use Native Vulkan ([#12], [#39], [#49]).
- Some scripted moments above 60 FPS ([#85], [#81], [#21]).
- Flash Shift shows a black block on some Intel UHD and GT 1030 GPUs ([#46]).
- 21:9 and 32:9 are experimental ([#19]).
- TAA and ambient occlusion are expensive on the Steam Deck.

[#12]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/12
[#19]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/19
[#21]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/21
[#39]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/39
[#46]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/46
[#49]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/49
[#51]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/51
[#68]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/68
[#81]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/81
[#85]: https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/85
