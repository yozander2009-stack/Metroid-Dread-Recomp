# Map correctness fixes (#52 and #53)

Base: a62d0a4d076103ecd23111f0365348bde56c61d3.

## Changes

- #53: `dread/dread_v210_patches.json` scales controller/keyboard map-pan velocity by the incoming menu delta relative to 60 Hz. The existing zoom path already uses delta. Mouse pixel motion, game ticks and physics are unchanged. Setup applies this insertion to the player's locally generated `code_097.c`; a runtime-only update cannot deliver it.
- #52: `runtime/gpu_gl.c` makes the scheduled full texture check cover every byte, including fragmented guest mappings. Unchanged old video revisions fall through normal cache validation. A visible texture overdue for a full check by 120 presents can bypass the routine checking budget, preventing indefinite starvation. Cheap sample checks and normal polling remain in place.

The #52 attachment identifies OpenGL on AMD Mesa even though the issue's renderer selector says Native Vulkan. This change addresses the OpenGL cache path shown in that log. It does not claim an AMD/Linux end-to-end reproduction on the local NVIDIA/Windows machine. The 120-present fallback bounds detection; it is not an every-frame coherence mechanism.

## Focused regression checks

Python 3 and a C compiler are required. Run from the repository root:

```text
python work-profiler/test_texture_hash.py --cc /path/to/gcc
python work-profiler/test_map_pan.py --build-dir /path/to/pristine/Metroid_Dread_build --cc /path/to/gcc
```

The map test reads the player's pristine v2.1.0 generated chunk without modifying it. It validates both descriptor hashes and extracts the arithmetic for 271 checks: the old 480 FPS defect, equal one-second movement at 30 through 480 FPS, and bit-identical 60 FPS results. It stores no generated game code in this repository. `--check-only` checks the insertion and hashes without a compiler.

The texture test extracts the production hash, video revision and polling blocks. Its 26,052 checks cover sparse edits missed by the old code, every short/partial tail, direct versus fragmented mappings, reused video memory, immediate real video revisions, budget exhaustion, and the frame counter wrapping.

Files retain their existing CRLF/LF format. Check whitespace with:

```text
git -c core.whitespace=cr-at-eol diff --check
```

## Implementing this in another checkout

Apply the provided source patch to a clean checkout at the base above, or cherry-pick the supplied commit. If the target repo has newer changes, inspect and merge the small source diffs; do not replace its entire renderer or patch descriptor blindly. Run both focused checks after resolving any overlap.

Build the runtime using the repository's documented `build.bat` / build-from-source workflow. The local validation built ImGui, Native Vulkan and the release runtime with a complete GCC C/C++ toolchain and official glslang. No quality setting changes are required.

For #53, include the updated descriptor in the release's `recompiler/dread/` and rebuild the player's generated game DLL through setup. Setup's `apply_patches` verifies pristine/result hashes. Existing installations need a regenerated/recompiled chunk and relinked DLL, not just a new EXE. Never commit or distribute generated C or the game DLL as source.

Keep saves/settings in a separate game copy while testing. Check title, an existing save, full-screen map panning at 60 and a higher FPS, exploration reveal/new-room transitions, and video transitions under OpenGL and Native Vulkan. AMD/Mesa confirmation of #52 remains necessary before treating the original reporter's exact scenario as verified.
