# NVIDIA DLAA (DLSS)

The game can use NVIDIA DLSS as **DLAA**: anti-aliasing at the game's own resolution (no upscaling). It works with
the **OpenGL** and **Native Vulkan** renderers on an **NVIDIA RTX** graphics card (20-series or newer) with a current
driver.

## Turning it on (players)

On the Play screen: **Graphics → NVIDIA DLAA anti-aliasing → GET**.

- GET downloads NVIDIA's DLSS file, `nvngx_dlss.dll` (59 MB), from NVIDIA's own GitHub repository
  (https://github.com/NVIDIA/DLSS, version **310.9.16**), checks it against its SHA-256 and puts it next to
  `Metroid Dread.exe`. DLAA then switches on by itself and applies at the next start.
- **Pressing GET means you accept NVIDIA's licence for that file.** READ opens it
  (https://github.com/NVIDIA/DLSS/blob/v310.9.16/LICENSE.txt).
- After that the row is a normal on/off switch. In `Metroid Dread.ini` it is `dlss=1` / `dlss=0`.

NVIDIA's file is not part of the release or this repository: it only comes to your PC from NVIDIA, when you ask for it.
To remove it, delete `nvngx_dlss.dll` from the game folder.

If you prefer to get the file yourself: download https://github.com/NVIDIA/DLSS, take
`lib\Windows_x86_64\rel\nvngx_dlss.dll` (the **`rel`** one; the `dev` one draws debug text over the picture) and copy it
next to `Metroid Dread.exe`.

## Checking that it works

`Metroid Dread.log` in the game folder shows, near the start:

```
dlss: NGX up; DLSS available
```

## Troubleshooting

| what you see | why |
|---|---|
| no DLAA row on the Play screen | not an NVIDIA RTX card, the renderer is "Vulkan output" (use OpenGL or Native Vulkan), or Linux (not available there) |
| "Download failed" | no internet connection, or GitHub could not be reached: try again |
| "does not match NVIDIA's file" | the download was damaged or changed: nothing was installed; try again |
| the log says NGX or DLSS is not available | not an RTX card, or the NVIDIA driver is too old: update it |
| debug text over the picture | a `dev` copy of `nvngx_dlss.dll` was put there by hand: use GET or the `rel` one |
| lower FPS than without DLAA | expected: DLAA costs GPU time |

## How it works (for developers)

Our code (`runtime/dlss_gl.c` for OpenGL, `runtime/snxvk/src_new/nx/gpu/vk/dlss.c` for Native Vulkan) loads NVIDIA's
NGX core from the graphics driver at run time (found through the registry) and calls it through its own declarations;
no NVIDIA SDK code or header is compiled in, and NVIDIA's files are never bundled. The download is in
`runtime/dread_ui/fe_sys.cpp` (`dlss_get_start`: pinned URL + SHA-256). The scene is handed to DLSS before the game's
bloom and post effects; motion vectors come from replaying the game's depth-prepass draws with this and last frame's
positions; the camera jitter is a Halton(2,3) sequence. Without `nvngx_dlss.dll`, or on other GPUs, the game runs
without it.

`SWR_NO_DLSS=1` for `work-profiler/build_snxvk2.py` still builds the renderer without any NGX code (Linux always does).
