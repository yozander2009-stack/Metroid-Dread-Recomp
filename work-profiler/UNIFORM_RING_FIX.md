# OpenGL uniform-buffer ring lifetime correction

The allocator could omit fences when an allocation ended exactly at a segment boundary. It also created a retirement fence during buffer setup, before the consuming draw was queued. Cached offsets from a retired segment could then be reused by later draws after that segment's fence. These are independently reproducible GPU/CPU lifetime hazards.

`runtime/gpu_gl.c` now marks crossed segments as retired during setup, publishes their fences at the start of the next draw (after the previous draw is queued), denies cache hits spanning closed segments, and waits before reopening a destination segment. It handles exact boundaries and retries timeouts. Fence creation/wait failure uses a synchronous finish before memory reuse. The normal successful path adds no global finish. Draw setup can allocate at most 36 banks of 64 KB, below an 8 MB segment; merged draws use the same upload path, and compute buffers use a separate path.

```text
python -B work-profiler/test_uniform_ring.py --cc /path/to/gcc
```

The test extracts the production allocator/helpers and models a deliberately lagging GPU. Each cell remembers its last queued consuming draw; a fence completes only the command prefix preceding it. The 41 cases cover exact/nonexact boundaries, 36-bank setup across a boundary, old cached offsets, three wraps, straddling spans, early returns, repeated timeouts, fence/wait failures and disabled ring operation. The previous production source fails 40 of 41 cases. `--source PATH` selects an older `gpu_gl.c` for a negative control.

Two independent source reviews found no blocker in the lifetime rules or GLQ ordering. GLQ waits and failure finishes drain synchronously before CPU overwrite.

This was found while reviewing AMD driver crashes #39/#55. It fixes a demonstrated source defect; it does not prove those reported crashes are resolved. Their older reports lack the newer GL-call/thread-role breadcrumbs, and affected AMD hardware is not available for local confirmation. No vendor, fullscreen, resolution, audio or gameplay timing setting is changed.
