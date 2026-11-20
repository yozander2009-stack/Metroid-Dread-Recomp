## What this changes

<!-- One or two sentences. Link the issue: Fixes #123 -->

## How it was tested

- [ ] Built on Windows (and Linux, if it touches shared or Linux code)
- [ ] Played the affected part of the game, renderer(s): OpenGL / Native Vulkan
- [ ] Frame pacing unchanged at 60 (and at a higher limit, if it touches pacing or rendering)

## Checklist

- [ ] No game files, keys, generated C or Nintendo assets in this PR
- [ ] Game-code changes go through `dread/dread_v210_patches.json`, not generated files
- [ ] Guest timing (60 Hz tick, the game's own sleeps and readiness checks) left alone, or explained
