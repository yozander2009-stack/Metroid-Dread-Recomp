/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The renderer interface: what the GPU front end (gpu.c, engines.c, maxwell3d.c) and the window backends ask of the
   Vulkan renderer (gpu/vk/renderer.c). It works with a window or offscreen without a swapchain. */
#ifndef SNX_RENDERER_H
#define SNX_RENDERER_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/gpu/gpu.h"

/* What the renderer makes before the game draws, for the screen main.c shows while it does: the shader programs the
   game ships in its RomFS and executables, found and translated, and the pipelines of earlier runs. done once the
   pass over them ended (finished, stopped, or not run) and those pipelines are made */
typedef struct RendererPrepare {
    u32 files, files_seen;       /* the RomFS's files and the executables' segments, and those the pass looked at */
    u32 programs, programs_done; /* the programs found in them (once all were looked at, when they are counted first:
                                    prepare_shaders 2), and those handed on to be translated (the ones translated
                                    before and those left out included) */
    u32 shipped;                 /* the programs it translated, this run */
    u32 shaders;                 /* shaders of earlier runs, theirs included */
    u32 pipes, pipes_made;       /* pipelines of earlier runs, and those made */
    int scanning;                /* the pass runs */
    int loaded;                  /* the shader files of earlier runs are read */
    int done;
#define RENDERER_NOTES     48
#define RENDERER_NOTE_SIZE 256                     /* a line's bytes: as wide as a wide window's row, which cuts it */
    char note[RENDERER_NOTES][RENDERER_NOTE_SIZE]; /* the pass's latest lines, oldest first (the log has every one) */
    int notes;
} RendererPrepare;

typedef struct Renderer {
    const char* name;
    int (*start)(void); /* 0 when it can't run here */
    /* guest memory may have changed since the renderer's caches last checked it: a submission came (on the
       submitting thread), or a stream goes on after a wait (gpu_park, on the GPU thread) that the CPU may have ended
       after writing data. Caches read guest memory again. Any thread */
    void (*mem_may_have_changed)(void);
    /* the GPU thread starts running a job (1) or stops (0): the caches' checks while it runs need the CPU's writes from
       before the submissions and wait ends so far (mem_may_have_changed), and from before those its own waits end,
       but not those a later submission makes meanwhile */
    void (*stream_run)(int on);
    /* the 3D engine (maxwell3d.c) */
    void (*clear)(GpuChannel* ch, u32 v);
    void (*draw)(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
                 u32 instances);
    /* Maxwell ordering methods: 0 texture barrier, 1 tiled barrier, 2 data-cache invalidate,
       3 descriptor-cache invalidate, 4 wait for idle, 5 shader instruction invalidate. */
    void (*method_barrier)(int kind);
    /* DRAW_TEXTURE: a texture's rectangle onto color target 0. NULL: not drawn */
    void (*draw_texture)(GpuChannel* ch);
    /* the 2D engine: 1 when the renderer did the blit on the GPU, 0 to do it in guest memory */
    int (*blit)(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                double sdy);
    /* the VIC composition (gpu/video/vic.c), on the GPU, its picture kept there (docs/GPU_OWNERSHIP.md): 0 when
       the job exceeds the device (it is skipped) */
    int (*vic_compose)(const VicJob* j);
    /* an NVDEC picture laid out in its surfaces on the GPU, kept there as VIC's picture is: 0 when it couldn't be */
    int (*nvdec_write)(const NvdecPic* p);
    /* a new reference to an FFmpeg device (an AVBufferRef of an AVHWDeviceContext) that decodes H.264 with Vulkan
       Video on this renderer's GPU, for NVDEC's decoder, whose pictures then reach nvdec_write as GPU images: NULL when
       the GPU has no decode queue or the vulkan_video setting is off */
    void* (*nvdec_hw_device)(void);
    /* a launch of the DMA copy engine on the GPU, its destination kept there: 0 when it couldn't be */
    int (*dma_copy)(const GpuDmaOp* o);
    /* a 2D blit the GPU's own blit can't do (its source isn't a render target) done on the GPU from memory, its
       destination kept there: 0 when it couldn't be */
    int (*blit_mem)(const GpuBlit* b);
    void (*rt_flush)(u64 gva);  /* a render target at gva is about to be read from guest memory: write it back */
    void (*rt_reload)(u64 gva); /* guest memory under a render target changed: load it again */
    /* a compute launch (q: its QMD's 64 words, converted to host numbers by compute.c) */
    void (*dispatch)(GpuChannel* ch, const u32* q, u64 code);
    /* gmem_read and gmem_ptr land pending compute or graphics storage writes before reading memory */
    void (*cs_mem_need)(u64 a, u64 n);
    /* 0 when no pending result or queued release covers [a, a + n): a read needs none of the mem_need hooks */
    int (*mem_pending)(u64 a, u64 n);
    void (*gfx_mem_need)(u64 a, u64 n);
    void (*before_syncwait)(void); /* the stream is about to wait on a syncpoint: flush everything in flight */
    void (*stream_incr)(u32 id);   /* a syncpoint increment in the stream, after everything before it is done */
    /* a semaphore or report the game can poll: len bytes written at gva once everything before it is done (it may
       happen later, in order with the other releases). data is guest bytes, its numbers little-endian already: the
       front end converted them. NULL: written now, after landing everything */
    void (*release_write)(u64 gva, const void* data, u32 len);
    /* the stream reads or writes [a, a + n): a release still waiting to write there happens first */
    void (*release_need)(u64 a, u64 n);
    /* the stream reads [a, a + n) as it stands after every release before it: 1 when it read guest memory with the
       payloads of the releases queued over it, without waiting for them; 0 when no queued release is there (the caller
       reads as usual, nothing to wait for from them); -1 when one is there that can't be read past now (a samples-passed
       count not known yet, another pending result there, the same memory through another GPU address), and the caller
       waits for it (release_need). dst gets guest bytes, as from gmem_read */
    int (*release_read)(u64 a, void* dst, u64 n);
    u32 (*release_value)(u32 id); /* completed and queued increments, read consistently */
    /* the render enable (3D 0x556 IF_EQUAL when equal is set, else IF_NOT_EQUAL) compares the count at a with the one
       at a + 16, one side a samples-passed count not known yet, the other zero: NVN's occlusion test. -1 when that
       isn't so here (the caller reads both and compares, waiting for the count); 0 or 1, the comparison's result when
       the count is known to be zero or not without the GPU; 2 when the GPU makes it, *pred the predicate the draws and
       clears under it run on (the channel's render_pred), held until render_predicate_drop */
    int (*render_predicate)(u64 a, int equal, u32* pred);
    void (*render_predicate_drop)(u32 pred);
    /* the 3D engine's samples-passed counter (occlusion queries), one per channel: it counts from its reset, while
       SampleCounterEnable is on. A report writes the count at gva as a release, len 4 (the count's low word) or 16 (the
       count, then ticks). samples_report 0: no count to give (no reset yet), and the caller writes a value of its own */
    void (*samples_reset)(GpuChannel* ch);
    void (*samples_enable)(GpuChannel* ch, int on);
    int (*samples_report)(GpuChannel* ch, u64 gva, u32 len, u64 ticks);
    /* a GPFIFO submission ran to its end or waits (gpu_park): what the guest may look at now goes to the GPU */
    void (*stream_end)(void);
    void (*release_retire)(u32 id); /* finish this syncpoint's queued releases before its id is reused */
    void (*report_stall)(void); /* the stall watchdog: what the renderer holds back. Any thread, unlocked */
    /* a game pause (gpu_hold): everything recorded is submitted and done on the GPU, its results landed and every queued
       release performed, so nothing of the renderer's writes guest memory until the next job. The GPU thread, between
       jobs. NULL when the renderer has nothing in flight */
    void (*idle)(void);
    /* a save state's load, the GPU held and guest memory about to be replaced: every surface, buffer and counter kept
       of guest memory goes, so each later use reads the state's memory. NULL when the renderer keeps nothing */
    void (*reset)(void);
    /* a save state's save, the GPU held: every render target's pixels written back to guest memory where they can be,
       so the state holds them. NULL when the renderer keeps none */
    void (*flush_targets)(void);
    /* a save state's render targets whose pixels guest memory doesn't hold (the depth targets), read back into io (0,
       or -1), and after the load's reset those of a state, for the targets the loaded game makes. NULL when the
       renderer keeps none */
    int (*save_targets)(struct StateIo* io);
    int (*load_targets)(struct StateIo* io);
    /* presenting a guest framebuffer */
    /* crop: the part shown (left, top, right, bottom), all 0 for the whole buffer */
    /* transform: Android buffer flags, bit 0 horizontal flip and bit 1 vertical flip */
    void (*present_buffer)(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                           u32 transform);
    u64 (*screenshot)(const char* path); /* the hash of the frame it saved */
    /* the last presented frame scaled to w x h RGBA8 (a save state's thumbnail): 0, or -1 when there is none */
    int (*thumbnail)(u8* rgba, u32 w, u32 h);
    /* the window again, its last frame (or black) under the on-screen controls and the game's menu, for when the game
       has shown nothing for a while: the frontend's loop calls it while either shows. NULL when there is no window */
    void (*refresh)(void);
    /* writes what should outlive the process (the pipeline cache): asked every 5 s from the frontend loop, where it may
       wait for a quiet moment, and before an exit (at_exit), which writes it now. Any thread, while the others still
       run. NULL when the renderer keeps nothing */
    void (*save)(int at_exit);
    /* the window given up for a fatal error's screen (plat.c's fatal, backend/sdl3.c): the swapchain and the surface go,
       as nothing else can draw in a window something presents to. Called on the failing thread, which never returns to
       its callers, so it keeps no lock. 0 when another thread held the renderer too long. NULL without a window */
    int (*release_window)(void);
    /* RendererPrepare's progress, any thread. What isn't done when the game starts goes on in the background. NULL when
       the renderer makes nothing before */
    void (*prepare)(RendererPrepare* p);
} Renderer;

extern const Renderer vk_renderer;
/* the Vulkan renderer, windowed or headless */
extern const Renderer* g_renderer;

/* shared state (gpu/renderer.c) */
extern int g_renderer_up; /* the renderer started */
extern u64 g_draws;
extern _Atomic int g_cs_any;      /* compute results are pending somewhere */
extern _Atomic int g_gfx_any;     /* graphics storage-buffer results are pending somewhere */
extern _Atomic int g_gfx_img_any; /* graphics storage-image results are pending somewhere */

#endif
