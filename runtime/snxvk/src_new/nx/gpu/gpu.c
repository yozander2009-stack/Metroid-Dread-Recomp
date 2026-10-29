/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/dma_pusher.cpp, src/video_core/dma_pusher.h, src/video_core/engines/puller.cpp,
   src/video_core/engines/puller.h, src/video_core/engines/engine_upload.cpp, src/video_core/engines/engine_upload.h,
   src/video_core/textures/decoders.cpp, src/video_core/textures/decoders.h, src/video_core/engines/maxwell_3d.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: 2022 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
   Compute-inline safe reads follow Citron's dma_pusher.cpp, commit
   acfb1dcc1ad9adf25b960c4f8f25c5bf78849406 (Zephyron's Peach fix). */
/* The GPU's host side: guest memory as the GPU sees it, the channels, the pushbuffer and its puller methods, the
   inline-to-memory engine, and the queue of GPFIFO submissions the GPU thread runs.

   A submission (gpu_submit, from nvdrv's ioctl) only queues a job: the ioctl never waits for the GPU. The GPU thread
   runs the jobs of each channel in order. A stream that has to wait (a semaphore acquire, a syncpoint wait, a fence:
   gpu_park) stops after the method that asked, and the thread runs other channels meanwhile, as the console's PFIFO
   switches channels on an acquire that fails. Calls (gpu_call) run on the GPU thread between jobs, in order.

   The stream is decoded as Eden's DmaPusher does (pushbuffer.h has the formats): each word is a command header or
   one of the data words a header owes, and each data word is a method call. Methods below 0x40 go to the puller
   (Eden's Puller: semaphores, syncpoints, binding a class to a subchannel), the others to the engine of the class
   bound to the call's subchannel (engines.c, compute.c, fermi2d.c, dma.c). */
#include "nx/runtime/state_fns.h"
#include "nx/runtime/state_io.h"
#include <stdlib.h>
#include <string.h>
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "nx/gpu/gpu.h"
#include "nx/memory/own.h"
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/maxwell/inline_to_memory_regs.h"
#include "nx/gpu/maxwell/kepler_compute_regs.h"
#include "nx/gpu/maxwell/maxwell_3d_regs.h"
#include "nx/gpu/maxwell/maxwell_dma_regs.h"
#include "nx/gpu/maxwell/fermi2d_regs.h"
#include "nx/gpu/texture/blocklinear.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"

GpuState g_gpu;
volatile u64 g_gpu_submits;
void (*g_gpu_mem_changed)(u64 cpu_addr, u64 len);
void (*g_gpu_before_write)(u64 gva, u64 size);
int g_gpu_releases;
void (*g_gpu_mapping_begin)(u64 gva, u64 size, int full);
void (*g_gpu_mapping_end)(void);

/* ------------------------------------------------------------ guest memory through the GPU's address space */
#define GMEM_PAGE 0x1000u /* the granule unmapped memory and memory states are walked in: a 4 KiB page */
/* a memory state's type, bits 7:0 (Switchbrew's MemoryState): the rest are its attribute flags */
#define MEM_STATE_TYPE_MASK 0xFFu
/* the runs of guest memory behind [gva, gva + size), copied in or out: 1 when every page was mapped. An unmapped
   page, or one the guest freed after mapping it here (gmmu_run), reads as zero and takes no write. What the GPU holds
   of a run lands first (memory/own.c): the caller may hold the renderer's lock, and a trap can't be answered then */
static int copy_runs(u64 gva, u8* in, const u8* out, u64 size) {
    int all = 1;
    while (size) {
        u64 cpu = 0, n = gmmu_run(gva, size, &cpu);
        if (!n) {
            n = GMEM_PAGE - (gva & (GMEM_PAGE - 1));
            if (n > size) n = size;
            if (in) memset(in, 0, (size_t)n);
            all = 0;
            goto next;
        }
        if (__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE)) own_acquire(cpu, n);
        if (in)
            memcpy(in, GP(cpu), (size_t)n);
        else
            memcpy(GP(cpu), out, (size_t)n);
    next:
        if (in)
            in += n;
        else
            out += n;
        gva += n;
        size -= n;
    }
    return all;
}
void gmem_read_raw(u64 gva, void* dst, u64 size) { copy_runs(gva, (u8*)dst, NULL, size); }
void gmem_write_raw(u64 gva, const void* src, u64 size) { copy_runs(gva, NULL, (const u8*)src, size); }
/* whether anything the renderer holds may cover [gva, gva + size): one lookup where each hook below makes its own */
static int mem_pending(u64 gva, u64 size) { return !g_renderer->mem_pending || g_renderer->mem_pending(gva, size); }
void gmem_need(u64 gva, u64 size) {
    if ((__atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE) || g_cs_any || g_gfx_any || g_gfx_img_any) &&
        !mem_pending(gva, size))
        return;
    if (!size) return;
    if (g_cs_any && g_renderer->cs_mem_need) g_renderer->cs_mem_need(gva, size);
    if ((g_gfx_any || g_gfx_img_any) && g_renderer->gfx_mem_need) g_renderer->gfx_mem_need(gva, size);
    if (__atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE) && g_renderer->release_need)
        g_renderer->release_need(gva, size);
}
/* the renderer's hook (vkr_before_write) makes a release queued over the bytes happen first, with everything else
   pending there: dma.c and the inline-to-memory path call it directly */
void gmem_before_write(u64 gva, u64 size) {
    if (size && g_gpu_before_write) g_gpu_before_write(gva, size);
}
int gmem_read(u64 gva, void* dst, u64 size) {
    if ((__atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE) || g_cs_any || g_gfx_any || g_gfx_img_any) &&
        !mem_pending(gva, size))
        return copy_runs(gva, (u8*)dst, NULL, size) ? 0 : -1;
    /* a queued release's payload is known: the stream reads it without waiting for the work before it, as the console's
       GPU, in order, would find it there (one game's conditional rendering read report pairs this way, waiting for the
       GPU 60 times a second) */
    if (__atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE) && g_renderer->release_read) {
        int r = g_renderer->release_read(gva, dst, size);
        if (r > 0) return 0;
        if (r < 0) g_renderer->release_need(gva, size);
        if (g_cs_any) g_renderer->cs_mem_need(gva, size);
        if (g_gfx_any || g_gfx_img_any) g_renderer->gfx_mem_need(gva, size);
        gmem_read_raw(gva, dst, size);
        return 0;
    }
    gmem_need(gva, size);
    return copy_runs(gva, (u8*)dst, NULL, size) ? 0 : -1;
}
int gmem_write(u64 gva, const void* src, u64 size) {
    int all;
    if (!size) return 0;
    gmem_before_write(gva, size);
    all = copy_runs(gva, NULL, (const u8*)src, size);
    gpu_mem_written(gva, size);
    return all ? 0 : -1;
}
/* the host pointer of [gva, gva + size) when contiguous, what the GPU holds of it landed (memory/own.c): the caller
   reads or writes it on the CPU, maybe with the renderer's lock held */
u8* gmem_ptr_untracked(u64 gva, u64 size) {
    u64 cpu = 0;
    if (!size) size = 1;
    if (gmmu_run(gva, size, &cpu) != size) return NULL;
    if (__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE)) own_acquire(cpu, size);
    return GP(cpu);
}
u8* gmem_host(u64 gva, u64 size) {
    u64 cpu = 0;
    if (!size) size = 1;
    return gmmu_run(gva, size, &cpu) == size ? GP(cpu) : NULL;
}
u8* gmem_ptr(u64 gva, u64 size) {
    u8* p = gmem_ptr_untracked(gva, size);
    if (p) gmem_need(gva, size ? size : 1);
    return p;
}
u32 gmem_r32(u64 gva) {
    u8 b[4] = {0};
    gmem_read(gva, b, 4);
    return snx_ld_le32(b);
}
void gmem_w32(u64 gva, u32 v) {
    u8 b[4];
    snx_st_le32(b, v);
    gmem_write(gva, b, 4);
}
/* Hardware fetches a program only up to its end, and TotK has compute programs ending next to an uncommitted page:
   the read stops at the first page that has no GPU mapping or no guest memory behind it */
void gmem_read_code(u64 gva, void* dst, u64 size) {
    MemBlock b;
    int have = 0;
    u64 done = 0;
    memset(&b, 0, sizeof b);
    while (done < size) {
        u64 a = gva + done, n = GMEM_PAGE - (a & (GMEM_PAGE - 1)), cpu = gmmu_translate(a);
        u32 state;
        if (!cpu) break;
        if (!have || cpu < b.addr || cpu - b.addr >= b.size) { /* a block's pages share its state */
            mem_query(cpu, &b);
            have = 1;
        }
        state = b.state & MEM_STATE_TYPE_MASK;
        if (!b.size || state == (MS_FREE & MEM_STATE_TYPE_MASK) || state == (MS_INACCESSIBLE & MEM_STATE_TYPE_MASK))
            break;
        done += n < size - done ? n : size - done;
    }
    if (done) {
        gmem_need(gva, done);
        gmem_read_raw(gva, dst, done);
    }
    memset((u8*)dst + done, 0, (size_t)(size - done));
}

/* ------------------------------------------------------------ block linear layout (Eden's decoders.cpp) */
/* A byte's offset in the layout (texture/blocklinear.h, docs/BLOCKLINEAR.md) is the sum of an x part and a (y, z)
   part, as in Eden's SwizzleImpl (offset_x + offset_y + offset_z), so a row's base and a column's offset can be
   computed apart. The x part: the block column x falls in (x / 64, blocks are 2^(9 + bh + bd) bytes) and the byte's
   place in its GOB's row */
static u64 bl_x(u32 x, u32 bh, u32 bd) {
    return ((u64)(x >> GOB_SIZE_X_SHIFT) << (GOB_SIZE_SHIFT + bh + bd)) + gob_x_offset(x);
}
/* the (y, z) part: z's slab of 2^bd slices (each rows of blocks, gobs_x blocks to a row), the row of blocks y's GOB
   row gy falls in, the GOB's place in its block (slice z mod 2^bd, then GOB row gy mod 2^bh) and the row's place in
   its GOB */
static u64 bl_yz(u32 y, u32 z, u32 wb, u32 h, u32 bh, u32 bd) {
    u64 gobs_x = bl_gobs_x(wb), gy = y >> GOB_SIZE_Y_SHIFT;
    u64 rows = bl_block_rows(h, bh); /* rows of blocks in a slab */
    u64 block = bl_block_size(bh, bd);
    u64 in_block = ((((u64)z & ((1u << bd) - 1)) << bh) + (gy & ((1u << bh) - 1))) << GOB_SIZE_SHIFT;
    return (((u64)(z >> bd) * rows + (gy >> bh)) * gobs_x) * block + in_block + gob_y_offset(y);
}
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2) {
    return bl_x(x_bytes, bh_log2, bd_log2) + bl_yz(y, z, width_bytes, height, bh_log2, bd_log2);
}
/* the guest bytes of a pixel of a color format, for render targets and 2D surfaces alike (nv50's surface formats,
   Eden's RenderTargetFormat values) */
u32 gpu_color_bpp(u32 format) {
    if (format >= 0xC0 && format <= 0xC5) return 16; /* RGBA32 and RGBX32 */
    if (format >= 0xC6 && format <= 0xCE) return 8;  /* RGBA16, RG32, RGBX16 */
    if (format >= 0xE8 && format <= 0xF2) return 2;  /* B5G6R5, BGR5A1, RG8, R16 */
    if (format >= 0xF3 && format <= 0xF7) return 1;  /* R8, A8 */
    if (format == 0xF8 || format == 0xFB || format == 0xFC) return 2;
    return 4;
}

/* ------------------------------------------------------------ what the game can observe */
void gpu_signal_point(void) {
    if (g_gpu_before_write) g_gpu_before_write(0, 0);
}
void gpu_release_write(u64 gva, const void* data, u32 len) {
    if (g_renderer->release_write) {
        g_renderer->release_write(gva, data, len);
        return;
    }
    gpu_signal_point();
    gmem_write(gva, data, len);
}
void gpu_mem_changed(u64 cpu_addr, u64 len) {
    if (g_gpu_mem_changed) g_gpu_mem_changed(cpu_addr, len);
}
static void may_have_changed(void) {
    if (g_renderer->mem_may_have_changed) g_renderer->mem_may_have_changed();
}
/* a syncpoint increment of the stream, after the work before it */
static void stream_incr(u32 id) {
    if (g_renderer->stream_incr) {
        g_renderer->stream_incr(id);
        return;
    }
    gpu_signal_point();
    syncpt_incr(id);
}

/* ------------------------------------------------------------ inline to memory (Eden's engine_upload.cpp) */
#define I2M_MAX (64u << 20) /* the most bytes a launch may want: a longer one is a bad register value, not data */
/* Eden's ProcessExec: the launch latches its destination from the upload registers and owes copy_size bytes,
   line_length_in times line_count, which the data words bring */
void i2m_launch(GpuChannel* ch, u32* regs, int base) {
    GpuI2m* t = &ch->i2m;
    GpuUploadRegs* u = &t->regs;
    const u32* r = regs + base;
    u32 block_size = r[UPLOAD_DEST_BLOCK_SIZE];
    u64 len = (u64)r[UPLOAD_LINE_LENGTH_IN] * r[UPLOAD_LINE_COUNT];
    t->copy_size = t->write_offset = 0;
    if (len > I2M_MAX) {
        WARN_LIMIT(4, "gpu: an inline-to-memory launch of %u lines of %u bytes is over %u MiB: nothing is written\n",
                   r[UPLOAD_LINE_COUNT], r[UPLOAD_LINE_LENGTH_IN], I2M_MAX >> 20);
        return;
    }
    if (len > t->capacity) {
        u8* b = (u8*)realloc(t->inner_buffer, (size_t)len);
        if (!b) {
            WARN_LIMIT(4, "gpu: no memory for an inline-to-memory launch of %llu bytes\n", (unsigned long long)len);
            return;
        }
        t->inner_buffer = b;
        t->capacity = (u32)len;
    }
    u->line_length_in = r[UPLOAD_LINE_LENGTH_IN];
    u->line_count = r[UPLOAD_LINE_COUNT];
    u->dest.address = ((u64)(r[UPLOAD_DEST_ADDRESS_HIGH] & UPLOAD_DEST_ADDRESS_HIGH_MASK) << 32) |
                      r[UPLOAD_DEST_ADDRESS_LOW];
    u->dest.pitch = r[UPLOAD_DEST_PITCH];
    u->dest.block_height = (block_size >> UPLOAD_BLOCK_HEIGHT_SHIFT) & UPLOAD_BLOCK_FIELD_MASK;
    u->dest.block_depth = (block_size >> UPLOAD_BLOCK_DEPTH_SHIFT) & UPLOAD_BLOCK_FIELD_MASK;
    if (u->dest.block_height > GOB_BLOCK_LOG2_MAX || u->dest.block_depth > GOB_BLOCK_LOG2_MAX) {
        WARN_LIMIT(4, "gpu: an inline-to-memory block of 2^%u x 2^%u GOBs is cut to %u\n", u->dest.block_height,
                   u->dest.block_depth, 1u << GOB_BLOCK_LOG2_MAX);
        if (u->dest.block_height > GOB_BLOCK_LOG2_MAX) u->dest.block_height = GOB_BLOCK_LOG2_MAX;
        if (u->dest.block_depth > GOB_BLOCK_LOG2_MAX) u->dest.block_depth = GOB_BLOCK_LOG2_MAX;
    }
    u->dest.width = r[UPLOAD_DEST_WIDTH];
    u->dest.height = r[UPLOAD_DEST_HEIGHT];
    u->dest.layer = r[UPLOAD_DEST_LAYER];
    u->dest.x = r[UPLOAD_DEST_X];
    u->dest.y = r[UPLOAD_DEST_Y];
    t->is_linear = (int)(r[UPLOAD_EXEC] & UPLOAD_EXEC_LINEAR);
    t->copy_size = (u32)len;
}
/* ProcessData's pitch linear destination: the lines pitch bytes apart, in one write when they are back to back */
static void upload_linear(const GpuI2m* t) {
    const GpuUploadRegs* u = &t->regs;
    u32 line;
    if (u->dest.pitch == u->line_length_in || u->line_count == 1) {
        gmem_write(u->dest.address, t->inner_buffer,
                   (u64)u->line_length_in * (u->dest.pitch == u->line_length_in ? u->line_count : 1));
        return;
    }
    for (line = 0; line < u->line_count; line++)
        gmem_write(u->dest.address + (u64)line * u->dest.pitch, t->inner_buffer + (u64)line * u->line_length_in,
                   u->line_length_in);
}
/* ProcessData's block linear destination (Eden's SwizzleSubrect): the lines into the rectangle at (x, y) of slice
   layer. The rows of blocks the lines fall in take the hooks once, then the lines go as 16 byte runs or less (none
   crosses one of a GOB's 16 byte runs) without them, then the write is recorded once */
static void upload_block_linear(const GpuI2m* t) {
    const GpuUploadRegs* u = &t->regs;
    u32 bh = u->dest.block_height, bd = u->dest.block_depth, line, x;
    u64 row_of_blocks = bl_gobs_x(u->dest.width) * bl_block_size(bh, bd); /* bytes */
    u64 slab = (u64)(u->dest.layer >> bd) * bl_block_rows(u->dest.height, bh) * row_of_blocks;
    u64 lo = slab + ((u64)(u->dest.y >> GOB_SIZE_Y_SHIFT) >> bh) * row_of_blocks;
    u64 hi = slab + ((((u64)(u->dest.y + u->line_count - 1) >> GOB_SIZE_Y_SHIFT) >> bh) + 1) * row_of_blocks;
    gmem_before_write(u->dest.address + lo, hi - lo);
    for (line = 0; line < u->line_count; line++) {
        u64 row = bl_yz(u->dest.y + line, u->dest.layer, u->dest.width, u->dest.height, bh, bd);
        for (x = 0; x < u->line_length_in;) {
            u32 at = u->dest.x + x, n = GOB_RUN - (at & (GOB_RUN - 1));
            if (n > u->line_length_in - x) n = u->line_length_in - x;
            gmem_write_raw(u->dest.address + row + bl_x(at, bh, bd),
                           t->inner_buffer + (u64)line * u->line_length_in + x, n);
            x += n;
        }
    }
    gpu_mem_written(u->dest.address + lo, hi - lo);
}
/* Eden's ProcessData: a data word's bytes, low byte first, up to copy_size; the last of them writes the launch */
void i2m_data(GpuChannel* ch, u32 v) {
    GpuI2m* t = &ch->i2m;
    u32 n;
    u8 b[4];
    if (t->write_offset >= t->copy_size) return;
    n = t->copy_size - t->write_offset < 4 ? t->copy_size - t->write_offset : 4;
    snx_st_le32(b, v); /* a data word's bytes go to memory low byte first */
    memcpy(t->inner_buffer + t->write_offset, b, n);
    t->write_offset += n;
    if (t->write_offset < t->copy_size) return;
    if (t->is_linear)
        upload_linear(t);
    else
        upload_block_linear(t);
}

/* ------------------------------------------------------------ channels */
/* the 3D state a channel starts from: zero, but for what games expect without setting it (Eden's
   InitializeRegisterDefaults), and ScarletNX's own: color masks enabled (two games never write them), sRGB writes
   on (framebuffer_srgb, else sRGB targets come out too dark), the render enable TRUE and its override
   USE_RENDER_ENABLE (deko3d never writes it). The registers and their values are Eden's Maxwell3D::Regs fields
   and enums (maxwell_3d_regs.h) */
static void reset_3d(u32* r) {
    static const float one = 1.0f;
    u32 i, f1;
    memcpy(&f1, &one, 4);
    for (i = 0; i < MAXWELL3D_NUM_VIEWPORTS; i++) {
        r[MAXWELL3D_VIEWPORTS + i * MAXWELL3D_VIEWPORT_STRIDE + MAXWELL3D_VIEWPORT_DEPTH_RANGE_FAR] = f1;
        r[MAXWELL3D_VIEWPORT_TRANSFORM + i * MAXWELL3D_VIEWPORT_TRANSFORM_STRIDE + MAXWELL3D_VIEWPORT_TRANSFORM_SWIZZLE] =
            MAXWELL3D_VIEWPORT_SWIZZLE_IDENTITY;
    }
    /* blending off in effect: dst = src * One + dst * Zero, the common state and each target's */
    r[MAXWELL3D_BLEND + MAXWELL3D_BLEND_COLOR_OP] = MAXWELL3D_BLEND_EQUATION_ADD_D3D;
    r[MAXWELL3D_BLEND + MAXWELL3D_BLEND_COLOR_SOURCE] = MAXWELL3D_BLEND_FACTOR_ONE_D3D;
    r[MAXWELL3D_BLEND + MAXWELL3D_BLEND_COLOR_DEST] = MAXWELL3D_BLEND_FACTOR_ZERO_D3D;
    r[MAXWELL3D_BLEND + MAXWELL3D_BLEND_ALPHA_OP] = MAXWELL3D_BLEND_EQUATION_ADD_D3D;
    r[MAXWELL3D_BLEND + MAXWELL3D_BLEND_ALPHA_SOURCE] = MAXWELL3D_BLEND_FACTOR_ONE_D3D;
    r[MAXWELL3D_BLEND + MAXWELL3D_BLEND_ALPHA_DEST] = MAXWELL3D_BLEND_FACTOR_ZERO_D3D;
    for (i = 0; i < MAXWELL3D_NUM_RENDER_TARGETS; i++) {
        u32* b = &r[MAXWELL3D_BLEND_PER_TARGET + i * MAXWELL3D_BLEND_PER_TARGET_STRIDE];
        b[MAXWELL3D_BLEND_COLOR_OP] = MAXWELL3D_BLEND_EQUATION_ADD_D3D;
        b[MAXWELL3D_BLEND_COLOR_SOURCE] = MAXWELL3D_BLEND_FACTOR_ONE_D3D;
        b[MAXWELL3D_BLEND_COLOR_DEST] = MAXWELL3D_BLEND_FACTOR_ZERO_D3D;
        b[MAXWELL3D_BLEND_ALPHA_OP] = MAXWELL3D_BLEND_EQUATION_ADD_D3D;
        b[MAXWELL3D_BLEND_ALPHA_SOURCE] = MAXWELL3D_BLEND_FACTOR_ONE_D3D;
        b[MAXWELL3D_BLEND_PER_TARGET_ALPHA_DEST] = MAXWELL3D_BLEND_FACTOR_ZERO_D3D;
    }
    /* stencil: both faces keep every value and always pass, with full masks */
    for (i = MAXWELL3D_STENCIL_OP_FAIL; i <= MAXWELL3D_STENCIL_OP_ZPASS; i++) {
        r[MAXWELL3D_STENCIL_FRONT_OP + i] = MAXWELL3D_STENCIL_OP_KEEP_D3D;
        r[MAXWELL3D_STENCIL_BACK_OP + i] = MAXWELL3D_STENCIL_OP_KEEP_D3D;
    }
    r[MAXWELL3D_STENCIL_FRONT_OP + MAXWELL3D_STENCIL_OP_FUNC] = MAXWELL3D_COMPARISON_ALWAYS_GL;
    r[MAXWELL3D_STENCIL_BACK_OP + MAXWELL3D_STENCIL_OP_FUNC] = MAXWELL3D_COMPARISON_ALWAYS_GL;
    r[MAXWELL3D_STENCIL_FRONT_FUNC_MASK] = r[MAXWELL3D_STENCIL_FRONT_MASK] = 0xFFFFFFFFu;
    r[MAXWELL3D_STENCIL_BACK_FUNC_MASK] = r[MAXWELL3D_STENCIL_BACK_MASK] = 0xFFFFFFFFu;
    r[MAXWELL3D_STENCIL_TWO_SIDE_ENABLE] = 1;
    r[MAXWELL3D_DEPTH_TEST_FUNC] = MAXWELL3D_COMPARISON_ALWAYS_GL;
    /* clockwise front faces are NVN's default; Eden starts counter-clockwise */
    r[MAXWELL3D_GL_FRONT_FACE] = MAXWELL3D_FRONT_FACE_CLOCKWISE;
    r[MAXWELL3D_GL_CULL_FACE] = MAXWELL3D_CULL_FACE_BACK;
    r[MAXWELL3D_POINT_SIZE] = f1;
    for (i = 0; i < MAXWELL3D_NUM_VERTEX_ATTRIBUTES; i++)
        r[MAXWELL3D_VERTEX_ATTRIB_FORMAT + i] = MAXWELL3D_VERTEX_ATTRIB_CONSTANT; /* constant until set */
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    r[MAXWELL3D_COLOR_TARGET_MRT_ENABLE] = 1;
    r[MAXWELL3D_LINE_WIDTH_SMOOTH] = r[MAXWELL3D_LINE_WIDTH_ALIASED] = f1;
    r[MAXWELL3D_POLYGON_MODE_FRONT] = r[MAXWELL3D_POLYGON_MODE_BACK] = MAXWELL3D_POLYGON_MODE_FILL;
    for (i = 0; i < MAXWELL3D_NUM_RENDER_TARGETS; i++) r[MAXWELL3D_COLOR_MASK + i] = MAXWELL3D_COLOR_MASK_ALL;
    r[MAXWELL3D_FRAMEBUFFER_SRGB] = 1;
    r[MAXWELL3D_RENDER_ENABLE_MODE] = MAXWELL3D_RENDER_ENABLE_TRUE;
    r[MAXWELL3D_RENDER_ENABLE_OVERRIDE] = MAXWELL3D_RENDER_ENABLE_OVERRIDE_USE_RENDER_ENABLE;
}
static void channel_reset(GpuChannel* ch) {
    /* the subchannels as NVN binds them; Eden's channel binds the same classes by the stream's BindObject */
    static const u32 classes[GPU_SUBCHANNELS] = {CLASS_MAXWELL_B,
                                                 CLASS_KEPLER_COMPUTE_B,
                                                 CLASS_KEPLER_INLINE_TO_MEMORY_B,
                                                 CLASS_FERMI_TWOD_A,
                                                 CLASS_MAXWELL_DMA_COPY_A,
                                                 0,
                                                 0,
                                                 0};
    memcpy(ch->sub_class, classes, sizeof classes);
    reset_3d(ch->r3d);
    memcpy(ch->r3d_shadow, ch->r3d, sizeof ch->r3d_shadow); /* the reset values count as tracked */
    ch->r3d_dirty = ~0u; /* nothing the renderer derived from another channel's registers holds for these */
    /* Eden's Fermi2D dst.depth and src.depth: the 2D surfaces' depth, as NVIDIA's driver assumes */
    ch->r2d[FERMI2D_DST + FERMI2D_SURFACE_DEPTH] = ch->r2d[FERMI2D_SRC + FERMI2D_SURFACE_DEPTH] = 1;
}
/* the channel of syncpoint id, made at its first submission. The queue's lock held */
static GpuChannel* channel_get(u32 id) {
    GpuChannel* ch;
    if (id >= GPU_CHANNELS) fatal("gpu: channel %u is past the %u the GPU has\n", id, GPU_CHANNELS);
    if (g_gpu.chan[id]) return g_gpu.chan[id];
    ch = (GpuChannel*)calloc(1, sizeof *ch);
    if (!ch) fatal("gpu: out of memory for GPU channel %u\n", id);
    ch->id = id;
    channel_reset(ch);
    g_gpu.chan[id] = ch;
    g_gpu.nchan++;
    return ch;
}

/* ------------------------------------------------------------ the pushbuffer (Eden's dma_pusher.cpp) */
/* a word of the stream (Eden's ProcessCommands): a data word the state owes is a method call (1, with *sub, *m, *v
   and *last, the header's last word); a header starts owing words (Eden's SetState) or, inline, is a call itself */
static int pb_decode(PbState* b, u32 w, u32* sub, u32* m, u32* v, int* last) {
    u32 mode, count;
    if (b->method_count) {
        *sub = b->subchannel;
        *m = b->method;
        *v = w;
        *last = b->method_count == 1;
        b->method_count--;
        if (b->mode == PB_MODE_INCREASING)
            b->method++;
        else if (b->mode == PB_MODE_INCREASE_ONCE) { /* the first word, then the next method for the rest */
            b->method++;
            b->mode = PB_MODE_NON_INCREASING;
        }
        return 1;
    }
    mode = pb_header_mode(w);
    count = pb_header_count(w);
    *sub = pb_header_subchannel(w);
    *m = pb_header_method(w);
    switch (mode) {
        case PB_MODE_INCREASING:
        case PB_MODE_NON_INCREASING:
        case PB_MODE_INCREASE_ONCE:
            b->mode = mode;
            b->subchannel = *sub;
            b->method = *m;
            b->method_count = count;
            return 0;
        case PB_MODE_INLINE: /* the count is the data */
            *v = count;
            *last = 1;
            return 1;
        default: /* the old modes carry no data words here: a stray one would otherwise eat the methods after it */
            if (w) LOG_LIMIT(8, "gpu: pushbuffer header %08x of mode %u is skipped\n", w, mode);
            return 0;
    }
}
/* the engine a subchannel's class runs on, by the class's low byte (the engine's number across GPU generations:
   0x97 is the 3D engine of MAXWELL_B 0xB197, as of its kin) */
#define CLASS_ENGINE_MASK 0xFFu
enum { ENG_NONE, ENG_3D, ENG_COMPUTE, ENG_I2M, ENG_2D, ENG_DMA };
static int engine_of(u32 cls) {
    switch (cls & CLASS_ENGINE_MASK) {
        case CLASS_MAXWELL_B & CLASS_ENGINE_MASK: return ENG_3D;
        case CLASS_KEPLER_COMPUTE_B & CLASS_ENGINE_MASK: return ENG_COMPUTE;
        case CLASS_KEPLER_INLINE_TO_MEMORY_B & CLASS_ENGINE_MASK: return ENG_I2M;
        case CLASS_FERMI_TWOD_A & CLASS_ENGINE_MASK: return ENG_2D;
        case CLASS_MAXWELL_DMA_COPY_A & CLASS_ENGINE_MASK: return ENG_DMA;
        default: return ENG_NONE;
    }
}
/* Eden's ProcessSemaphoreTriggerMethod: the operation SEMAPHORED names on the semaphore at the address the puller's
   SEMAPHOREA/B hold, with SEMAPHOREC's payload. A release goes through the renderer, after the work before it */
static void process_semaphore_trigger(GpuChannel* ch, u32 v) {
    u64 a = ((u64)(ch->puller[PULLER_SEMAPHORE_ADDRESS_HIGH] & PULLER_SEMAPHORE_ADDRESS_HIGH_MASK) << 32) |
            ch->puller[PULLER_SEMAPHORE_ADDRESS_LOW];
    u32 payload = ch->puller[PULLER_SEMAPHORE_SEQUENCE_PAYLOAD];
    switch (v & PULLER_SEMAPHORE_OP_MASK) {
        case PULLER_SEMAPHORE_OP_ACQUIRE_EQUAL: gpu_park(ch, GPU_PARK_SEMAPHORE, a, payload, 0); break;
        case PULLER_SEMAPHORE_OP_ACQUIRE_GEQUAL: gpu_park(ch, GPU_PARK_SEMAPHORE, a, payload, 1); break;
        case PULLER_SEMAPHORE_OP_RELEASE:
            if (v & PULLER_SEMAPHORE_RELEASE_SIZE_4BYTE) {
                u8 rel[4];
                snx_st_le32(rel, payload);
                gpu_release_write(a, rel, 4);
            } else { /* 16 bytes: the payload, 0, and the timestamp */
                u8 rel[16];
                snx_st_le32(rel, payload);
                snx_st_le32(rel + 4, 0);
                snx_st_le64(rel + 8, gpu_ticks());
                gpu_release_write(a, rel, 16);
            }
            break;
        case PULLER_SEMAPHORE_OP_ACQUIRE_MASK:
        case PULLER_SEMAPHORE_OP_REDUCTION:
            /* ACQ_AND and REDUCTION aren't performed: the stream goes on as if they were */
            WARN_LIMIT(4, "gpu: semaphore operation %x at %llx is not performed\n", v & PULLER_SEMAPHORE_OP_MASK,
                       (unsigned long long)a);
            break;
        default: LOG_LIMIT(4, "gpu: semaphore operation %08x is unknown\n", v); break;
    }
}
/* Eden's ProcessFenceActionMethod: SYNCPOINTB increments its syncpoint after the work before it, or waits for it to
   reach SYNCPOINTA's threshold */
static void process_fence_action(GpuChannel* ch, u32 v) {
    u32 id = puller_syncpoint_id(v);
    if (puller_syncpoint_increment(v)) {
        stream_incr(id);
        return;
    }
    /* results in flight land before the stream waits on something else (the CPU may read them before it increments
       the syncpoint): a wait already met is none, and stalls nothing */
    gpu_park(ch, GPU_PARK_SYNCPOINT, id, ch->puller[PULLER_SYNCPOINT_PAYLOAD], 1);
    if (ch->park && g_renderer->before_syncwait) g_renderer->before_syncwait();
}
/* Eden's CallPullerMethod: a method below NonPullerMethods, kept in the puller's registers */
static void puller_method(GpuChannel* ch, u32 sub, u32 m, u32 v) {
    ch->puller[m] = v;
    switch (m) {
        case PULLER_BIND_OBJECT: ch->sub_class[sub] = v & CLASS_ID_MASK; break; /* Eden's ProcessBindMethod */
        case PULLER_SEMAPHORE_OPERATION: process_semaphore_trigger(ch, v); break;
        case PULLER_SYNCPOINT_OPERATION: process_fence_action(ch, v); break;
        default: break;
    }
}
/* Eden's Puller::CallMethod: the puller's own methods, or the engine bound to the subchannel */
static void method_call(GpuChannel* ch, u32 sub, u32 m, u32 v, int last) {
    int eng = m < PULLER_NON_PULLER_METHODS ? ENG_NONE : engine_of(ch->sub_class[sub]);
    if (eng != ENG_3D) { /* the 3D engine's batched constants and merged draw come before what follows */
        r3d_const_flush(ch);
        r3d_pending_flush();
    }
    if (m < PULLER_NON_PULLER_METHODS) {
        puller_method(ch, sub, m, v);
        return;
    }
    switch (eng) {
        case ENG_3D: maxwell3d_method(ch, m, v, last); break;
        case ENG_COMPUTE: compute_method(ch, m, v, last); break;
        case ENG_I2M: kepler_i2m_method(ch, m, v); break;
        case ENG_2D: fermi2d_method(ch, m, v); break;
        case ENG_DMA: dma_method(ch, m, v); break;
        default:
            LOG_LIMIT(8, "gpu: method %x for class %x on subchannel %u: no engine here runs that class\n", m,
                      ch->sub_class[sub], sub);
            break;
    }
}

/* ------------------------------------------------------------ waits */
#define WAIT_POLL_NS 500000ull /* a release the CPU writes has no wake: semaphore waits look again this often */
/* how long a wait nothing ends lasts before the stream goes on (the console raises an interrupt instead): a report
   semaphore's, and every other kind's. park_over's message names them in seconds */
#define WAIT_LIMIT_REPORT_NS 1000000000ull
#define WAIT_LIMIT_NS 2000000000ull
static int park_met(const GpuChannel* ch) {
    if (ch->park == GPU_PARK_SYNCPOINT || ch->park == GPU_PARK_FENCE)
        return (i32)(syncpt_read((u32)ch->park_addr) - ch->park_val) >= 0;
    {
        u32 cur = gmem_r32(ch->park_addr);
        return ch->park_geq ? (i32)(cur - ch->park_val) >= 0 : cur == ch->park_val;
    }
}
void gpu_park(GpuChannel* ch, u32 kind, u64 addr, u32 val, int geq) {
    if ((kind == GPU_PARK_SYNCPOINT || kind == GPU_PARK_FENCE) && addr >= SYNCPT_IDS) {
        WARN_LIMIT(8, "gpu: a wait for syncpoint %llu, past the %u there are, doesn't wait\n", (unsigned long long)addr,
                   SYNCPT_IDS);
        return;
    }
    ch->park = kind;
    ch->park_addr = addr;
    ch->park_val = val;
    ch->park_geq = geq;
    /* what the CPU wrote before releasing the wait is for the stream to see, met already or not */
    if (park_met(ch))
        ch->park = 0;
    else /* the console raises an interrupt for a wait nothing ends; here the stream goes on after a while */
        ch->park_end = plat_ns() + (kind == GPU_PARK_REPORT ? WAIT_LIMIT_REPORT_NS : WAIT_LIMIT_NS);
    may_have_changed();
}
static const char* park_name(u32 kind) {
    switch (kind) {
        case GPU_PARK_SEMAPHORE: return "semaphore";
        case GPU_PARK_REPORT: return "report semaphore";
        case GPU_PARK_SYNCPOINT: return "syncpoint";
        default: return "fence";
    }
}
/* 1 when the channel's wait is over (met, or timed out); otherwise the ns to look again in, or -1 for a wake */
static int park_over(GpuChannel* ch, i64* again) {
    u64 now;
    if (park_met(ch)) {
        ch->park = 0;
        may_have_changed();
        return 1;
    }
    now = plat_ns();
    if (now >= ch->park_end) {
        WARN_LIMIT(16, "gpu: channel %u waited %u s for %s %llx to %s %u (it is %u), and goes on\n", ch->id,
                   (u32)((ch->park == GPU_PARK_REPORT ? WAIT_LIMIT_REPORT_NS : WAIT_LIMIT_NS) / 1000000000ull),
                   park_name(ch->park), (unsigned long long)ch->park_addr,
                   ch->park_geq ? "reach" : "be", ch->park_val,
                   ch->park == GPU_PARK_SYNCPOINT || ch->park == GPU_PARK_FENCE ? syncpt_read((u32)ch->park_addr)
                                                                               : gmem_r32(ch->park_addr));
        ch->park = 0;
        may_have_changed();
        return 1;
    }
    *again = (i64)(ch->park_end - now);
    if ((ch->park == GPU_PARK_SEMAPHORE || ch->park == GPU_PARK_REPORT) && *again > (i64)WAIT_POLL_NS)
        *again = (i64)WAIT_POLL_NS;
    return 0;
}

/* ------------------------------------------------------------ the queue */
/* An item of the GPU thread's queue: a job (a submission's entries) or a call. Items are linked in queue order; the
   GPU thread alone takes them out, and reads them unlocked while it runs one, so an item never moves. A job's
   entries, their word offsets and the words fetched at submission are one allocation with it */
typedef struct GpuItem GpuItem;
struct GpuItem {
    GpuItem* next;
    void (*fn)(void*); /* set for a call */
    void* arg;
    u32 chans[4], nchans;
    u64 id; /* a job's */
    u32 chan, incs, wait_id, wait_val, expect;
    u32 n, pf;        /* the entries, and those before the first SYNC entry, fetched at submission into words */
    u32 entry, word;  /* where the stream goes on */
    int started;
    u32* late;        /* the entries the GPU thread fetched: [late_first, late_end) */
    u32 late_cap, late_first, late_end;
};
#define QUEUE_ROOM 256 /* runnable jobs at most: jobs of a waiting channel don't count */
#define GPU_THREAD_STACK ((size_t)16 << 20) /* the GPU thread's stack: 16 MiB */
static PMutex g_qlk = PMUTEX_INIT;
static PCond g_qcv;
static int g_qcv_ready, g_qthread;
static GpuItem *g_qhead, *g_qtail;
static u64 g_qgen;           /* bumped by each new item and kick: a pass that saw an older one looks again */
static u32 g_qjobs[GPU_CHANNELS];     /* each channel's queued jobs */
static u32 g_qtotal;
static u8 g_qparked[GPU_CHANNELS];    /* the channel's oldest job waits (gpu_park) */
static u8 g_scan_stale[GPU_CHANNELS]; /* the channel closed: its submission scan starts again from reset */
static u64 g_job_ids;
static int g_hold_want, g_hold_at; /* a game pause asked the GPU thread to hold, and it holds (gpu_hold) */
/* for the stall report: whether this is the GPU thread, the channel whose job runs (its id + 1, 0 between jobs) and
   since when, and the blocking wait it is in (gpu_wait_note) */
static _Thread_local int t_gpu;
static u32 g_run_chan;
static u64 g_run_since;
static const char* g_wait_what;
/* where the running job's stream is, for the stall report: the phase between methods, and the method begun last.
   Read unlocked, each field on its own; the values stay at the last job's when between jobs */
enum { GPU_AT_BETWEEN, GPU_AT_METHOD, GPU_AT_FETCH, GPU_AT_JOB_END, GPU_AT_STREAM_STOP, GPU_AT_SYNCWAIT };
static u32 g_run_at;
static u32 g_run_sub, g_run_method, g_run_class, g_run_entry, g_run_word;
static u64 g_wait_value, g_wait_since;

/* a job's block after the item: its entries, each fetched entry's first word (in the words fetched at submission,
   or in late), then the words */
static u64* job_ent(const GpuItem* j) { return (u64*)(void*)(j + 1); }
static u32* job_off(const GpuItem* j) { return (u32*)(void*)(job_ent(j) + j->n); }
static u32* job_prefetched(const GpuItem* j) { return job_off(j) + j->n; }
static u32 entry_words(u64 e) { return gpfifo_size(e); }
static u64 entry_addr(u64 e) { return gpfifo_addr(e); }
static u32* job_words(GpuItem* j, u32 e) {
    return (e < j->pf ? job_prefetched(j) : j->late) + job_off(j)[e];
}
/* Citron's Peach fix requires safe reads for KeplerCompute's inline data:
   an earlier GPU command may have produced the payload since submission.
   Resolve that result on the GPU thread, in stream order. Only payload words
   are refreshed; command headers and other methods keep their snapshots.
   A non-increasing batch is read once, rather than landing each word. */
static u32 fetch_compute_inline(GpuItem* j, GpuChannel* ch, u32 e, u32* words) {
    u32 n = ch->pb.mode == PB_MODE_NON_INCREASING ? ch->pb.method_count : 1;
    u32 left = entry_words(job_ent(j)[e]) - j->word;
    if (n > left) n = left;
    gmem_read(entry_addr(job_ent(j)[e]) + (u64)j->word * 4, words + j->word, (u64)n * 4);
    snx_le32_array(words + j->word, n);
    return j->word + n;
}
/* the entry e the GPU thread reached was not fetched at submission: it and those after it up to the next SYNC entry
   are fetched now, before any of their methods can hand memory back to the CPU */
static int fetch_late(GpuItem* j, u32 e) {
    u32 end = e + 1, k;
    u64 total = entry_words(job_ent(j)[e]);
    SNX_ZONE_V(z, "gpu fetch late", e);
    while (end < j->n && !gpfifo_sync(job_ent(j)[end])) total += entry_words(job_ent(j)[end++]);
    if (total > j->late_cap) {
        u32* b;
        if (total > SIZE_MAX / 4 || !(b = (u32*)realloc(j->late, (size_t)total * 4))) {
            WARN_LIMIT(4, "gpu: no memory to fetch %llu pushbuffer words\n", (unsigned long long)total);
            return 0;
        }
        j->late = b;
        j->late_cap = (u32)total;
    }
    total = 0;
    for (k = e; k < end; k++) {
        u32 n = entry_words(job_ent(j)[k]);
        job_off(j)[k] = (u32)total;
        gmem_read(entry_addr(job_ent(j)[k]), j->late + total, (u64)n * 4);
        snx_le32_array(j->late + total, n); /* the stream's words, read as numbers from here on */
        total += n;
    }
    j->late_first = e;
    j->late_end = end;
    return 1;
}
/* what the stream leaves for the renderer when it stops, at its end or at a wait */
static void stream_stop(GpuChannel* ch) {
    SNX_ZONE(z, "gpu stream stop");
    r3d_const_flush(ch);
    r3d_pending_flush();
    if (g_renderer->stream_end) g_renderer->stream_end();
}
static void job_end(const GpuItem* j, GpuChannel* ch) {
    u32 i, v;
    SNX_ZONE_V(z, "gpu job end", j->chan);
    r3d_const_flush(ch);
    r3d_pending_flush();
    /* without a renderer that queues the increments, the work's results land before the game can see its fence. A
       renderer's queue lands them itself before each increment (release.c), so landing every pending result here
       was a full GPU wait at the end of each submission, about 2 ms a frame in one game */
    if (!g_renderer->stream_incr) gpu_signal_point();
    dma_snap_finish(j->chan, j->id);
    for (i = 0; i < j->incs; i++) stream_incr(j->chan);
    if (g_renderer->stream_end) g_renderer->stream_end();
    v = g_renderer->release_value ? g_renderer->release_value(j->chan) : syncpt_read(j->chan);
    if ((i32)(v - j->expect) < 0)
        WARN_LIMIT(8, "gpu: channel %u ended a submission at syncpoint value %u, short of the %u it promised\n",
                   j->chan, v, j->expect);
}
/* runs a job on the GPU thread, the queue unlocked: 1 when it ran to its end, 0 when it waits */
static int run_job(GpuItem* j) {
    GpuChannel* ch = g_gpu.chan[j->chan];
    ch->dma_job_id = j->id;
    if (!j->started) {
        j->started = 1;
        ch->dma_launch_index = 0;
        if (j->wait_id != ~0u) { /* the fence is the stream's first wait, as nvgpu puts a syncpoint wait first */
            gpu_park(ch, GPU_PARK_FENCE, j->wait_id, j->wait_val, 1);
            if (ch->park) {
                __atomic_store_n(&g_run_at, GPU_AT_SYNCWAIT, __ATOMIC_RELAXED);
                if (g_renderer->before_syncwait) g_renderer->before_syncwait(); /* as at SYNCPOINTB */
                __atomic_store_n(&g_run_at, GPU_AT_BETWEEN, __ATOMIC_RELAXED);
            }
            if (ch->park) {
                stream_stop(ch);
                return 0;
            }
        }
    }
    if (ch->mme.running) {
        mme_resume(ch);
        if (ch->park) {
            stream_stop(ch);
            return 0;
        }
    }
    while (j->entry < j->n) {
        u32 e = j->entry, len = entry_words(job_ent(j)[e]);
        u32* w;
        u32 inline_end = 0;
        if (e >= j->pf && (e < j->late_first || e >= j->late_end)) {
            __atomic_store_n(&g_run_at, GPU_AT_FETCH, __ATOMIC_RELAXED);
            int got = fetch_late(j, e);
            __atomic_store_n(&g_run_at, GPU_AT_BETWEEN, __ATOMIC_RELAXED);
            if (!got) len = 0;
        }
        w = len ? job_words(j, e) : NULL;
        while (j->word < len) {
            u32 sub, m, v;
            int last;
            if (j->word >= inline_end && ch->pb.method_count && ch->pb.method == KEPLER_COMPUTE_DATA_UPLOAD &&
                engine_of(ch->sub_class[ch->pb.subchannel]) == ENG_COMPUTE)
                inline_end = fetch_compute_inline(j, ch, e, w);
            if (pb_decode(&ch->pb, w[j->word++], &sub, &m, &v, &last)) {
                __atomic_store_n(&g_run_sub, sub, __ATOMIC_RELAXED);
                __atomic_store_n(&g_run_method, m, __ATOMIC_RELAXED);
                __atomic_store_n(&g_run_class, ch->sub_class[sub], __ATOMIC_RELAXED);
                __atomic_store_n(&g_run_entry, e, __ATOMIC_RELAXED);
                __atomic_store_n(&g_run_word, j->word, __ATOMIC_RELAXED);
                __atomic_store_n(&g_run_at, GPU_AT_METHOD, __ATOMIC_RELAXED);
                method_call(ch, sub, m, v, last);
                __atomic_store_n(&g_run_at, ch->park ? GPU_AT_STREAM_STOP : GPU_AT_BETWEEN, __ATOMIC_RELAXED);
                if (ch->park) {
                    stream_stop(ch);
                    return 0;
                }
            }
        }
        j->entry++;
        j->word = 0;
    }
    __atomic_store_n(&g_run_at, GPU_AT_JOB_END, __ATOMIC_RELAXED);
    job_end(j, ch);
    __atomic_store_n(&g_run_at, GPU_AT_BETWEEN, __ATOMIC_RELAXED);
    return 1;
}
static void item_free(GpuItem* it) {
    free(it->late);
    free(it);
}
/* the jobs the GPU thread can run now, for the queue's room. The queue's lock held */
static u32 runnable(void) {
    u32 n = g_qtotal, c;
    for (c = 0; c < GPU_CHANNELS; c++)
        if (g_qparked[c]) n -= g_qjobs[c];
    return n;
}
/* one pass over the queue, its lock held (dropped while an item runs): 1 when something ran. *wake is the time to
   look again at the waits */
static int run_pass(i64* wake) {
    u8 blocked[GPU_CHANNELS]; /* an older job of the channel is still queued */
    int calls_blocked = 0;
    GpuItem *it, *prev = NULL;
    memset(blocked, 0, sizeof blocked);
    for (it = g_qhead; it; prev = it, it = it->next) {
        GpuChannel* ch;
        int done;
        if (it->fn) {
            u32 k;
            int ok = !calls_blocked;
            for (k = 0; k < it->nchans; k++)
                if (it->chans[k] < GPU_CHANNELS && blocked[it->chans[k]]) ok = 0;
            if (!ok) { /* calls keep their order */
                calls_blocked = 1;
                continue;
            }
            if (prev)
                prev->next = it->next;
            else
                g_qhead = it->next;
            if (g_qtail == it) g_qtail = prev;
            pm_unlock(&g_qlk);
            SNX_ZONE(z_call, "gpu call");
            it->fn(it->arg);
            SNX_ZONE_END(z_call);
            item_free(it);
            pm_lock(&g_qlk);
            return 1;
        }
        if (blocked[it->chan]) continue;
        blocked[it->chan] = 1;
        ch = g_gpu.chan[it->chan];
        if (g_qparked[it->chan]) {
            i64 again = -1;
            int over;
            pm_unlock(&g_qlk);
            over = park_over(ch, &again);
            pm_lock(&g_qlk);
            if (!over) {
                if (again >= 0 && (*wake < 0 || again < *wake)) *wake = again;
                continue;
            }
            g_qparked[it->chan] = 0;
        }
        pm_unlock(&g_qlk);
        __atomic_store_n(&g_run_since, plat_ns(), __ATOMIC_RELAXED);
        __atomic_store_n(&g_run_chan, it->chan + 1, __ATOMIC_RELEASE);
        {
            SNX_ZONE_C(z, "gpu job", 0x4a7fb5);
            SNX_ZONE_VALUE(z, it->chan);
            SNX_ZONE_TEXT(z, "channel %u", it->chan);
            if (g_renderer->stream_run) g_renderer->stream_run(1);
            done = run_job(it);
            if (g_renderer->stream_run) g_renderer->stream_run(0);
            SNX_ZONE_END(z);
        }

        __atomic_store_n(&g_run_chan, 0, __ATOMIC_RELEASE);
        pm_lock(&g_qlk);
        if (!done)
            g_qparked[it->chan] = 1; /* its jobs make room for others */
        else {
            if (prev)
                prev->next = it->next;
            else
                g_qhead = it->next;
            if (g_qtail == it) g_qtail = prev;
            g_qjobs[it->chan]--;
            g_qtotal--;
            item_free(it);
        }
        pc_broadcast(&g_qcv);
        return 1;
    }
    return 0;
}
void gpu_wait_note(const char* what, u64 value) {
    if (!t_gpu) return;
    if (what) {
        __atomic_store_n(&g_wait_value, value, __ATOMIC_RELAXED);
        __atomic_store_n(&g_wait_since, plat_ns(), __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_wait_what, what, __ATOMIC_RELEASE);
}
/* read unlocked from the watchdog's thread: each value on its own, which is enough to say where the GPU thread is */
void gpu_report_stall(void) {
    u64 now = plat_ns();
    const char* what = __atomic_load_n(&g_wait_what, __ATOMIC_ACQUIRE);
    u32 run = __atomic_load_n(&g_run_chan, __ATOMIC_ACQUIRE), c;
    if (!__atomic_load_n(&g_qthread, __ATOMIC_RELAXED)) {
        WARN("gpu: nothing was submitted yet\n");
        return;
    }
    if (run)
        WARN("gpu: the GPU thread runs a job of channel %u, for %llu ms\n", run - 1,
             (unsigned long long)((now - __atomic_load_n(&g_run_since, __ATOMIC_RELAXED)) / 1000000u));
    else
        WARN("gpu: the GPU thread is between jobs\n");
    if (run) {
        static const char* const at[] = {"between methods", "in a method", "fetching an entry's words",
                                         "ending the submission", "stopping the stream at a wait",
                                         "before a stream's wait"};
        u32 ati = __atomic_load_n(&g_run_at, __ATOMIC_RELAXED);
        WARN("gpu: the job is at entry %u word %u, %s; its last method %x on subchannel %u (class %x)\n",
             __atomic_load_n(&g_run_entry, __ATOMIC_RELAXED), __atomic_load_n(&g_run_word, __ATOMIC_RELAXED),
             ati < sizeof at / sizeof *at ? at[ati] : "?", __atomic_load_n(&g_run_method, __ATOMIC_RELAXED),
             __atomic_load_n(&g_run_sub, __ATOMIC_RELAXED), __atomic_load_n(&g_run_class, __ATOMIC_RELAXED));
    }
    if (what)
        WARN("gpu: it has waited %llu ms for %s (%llx)\n",
             (unsigned long long)((now - __atomic_load_n(&g_wait_since, __ATOMIC_RELAXED)) / 1000000u), what,
             (unsigned long long)__atomic_load_n(&g_wait_value, __ATOMIC_RELAXED));
    for (c = 0; c < GPU_CHANNELS; c++) {
        u32 jobs = __atomic_load_n(&g_qjobs[c], __ATOMIC_RELAXED), park;
        const GpuChannel* ch = g_gpu.chan[c];
        if (!jobs || !ch) continue;
        park = __atomic_load_n(&ch->park, __ATOMIC_RELAXED);
        if (!park || !__atomic_load_n(&g_qparked[c], __ATOMIC_RELAXED)) {
            WARN("gpu: channel %u has %u jobs queued\n", c, jobs);
            continue;
        }
        {
            u64 addr = __atomic_load_n(&ch->park_addr, __ATOMIC_RELAXED);
            u32 val = __atomic_load_n(&ch->park_val, __ATOMIC_RELAXED);
            int sp = park == GPU_PARK_SYNCPOINT || park == GPU_PARK_FENCE;
            WARN("gpu: channel %u has %u jobs queued, and waits for %s %llx to %s %u (it is %u)\n", c, jobs,
                 park_name(park), (unsigned long long)addr,
                 sp || __atomic_load_n(&ch->park_geq, __ATOMIC_RELAXED) ? "reach" : "be", val,
                 sp ? syncpt_read((u32)addr) : gmem_r32(addr));
        }
    }
    if (g_renderer && g_renderer->report_stall) g_renderer->report_stall();
}
static void gpu_thread(void* arg) {
    (void)arg;
    t_gpu = 1;
    SNX_THREAD_NAME("GPU");
    pm_lock(&g_qlk);
    { extern void plat_prof_mark_gpu(void); plat_prof_mark_gpu(); } /* SwitchRecomp: lets SWR_PROFILE_RING_HOST sample this thread */
    for (;;) {
        u64 gen = g_qgen;
        i64 wake = -1;
        if (run_pass(&wake)) continue;
        /* A whole pass ran nothing: sleep until a new item, a kick, or the next look at a wait. A plot, not a zone:
           this wait can last the rest of the run. */
        SNX_PLOT("gpu idle", 1);
        if (gen == g_qgen) pc_wait(&g_qcv, &g_qlk, wake);
        SNX_PLOT("gpu idle", 0);
    }
}
static void queue_init(void) {
    pm_lock_announce(&g_qlk, "gpu queue"); /* the jobs' handoff, game threads to the GPU thread */
    if (!g_qcv_ready) {
        pc_init(&g_qcv);
        g_qcv_ready = 1;
    }
}
/* the queue's lock held */
static void queue_append(GpuItem* it) {
    it->next = NULL;
    if (g_qtail)
        g_qtail->next = it;
    else
        g_qhead = it;
    g_qtail = it;
    g_qgen++;
    if (!g_qthread) {
        g_qthread = 1;
        if (plat_thread(gpu_thread, NULL, GPU_THREAD_STACK)) {
            char why[256];
            fatal("gpu: could not start the GPU thread: %s\n", snx_os_error(why, sizeof why));
        }
    }
    pc_broadcast(&g_qcv);
}
void gpu_init(void) {
    pm_lock(&g_qlk);
    queue_init();
    pm_unlock(&g_qlk);
}
void gpu_kick(void) {
    pm_lock(&g_qlk);
    g_qgen++;
    if (g_qcv_ready) pc_broadcast(&g_qcv);
    pm_unlock(&g_qlk);
}
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 nchans) {
    GpuItem* it = (GpuItem*)calloc(1, sizeof *it);
    if (!it) fatal("gpu: out of memory queueing a call\n");
    it->fn = fn;
    it->arg = arg;
    if (nchans > 4) nchans = 4;
    if (nchans) memcpy(it->chans, chans, nchans * sizeof *chans);
    it->nchans = nchans;
    pm_lock(&g_qlk);
    queue_init();
    queue_append(it);
    pm_unlock(&g_qlk);
}

/* A game pause: the GPU thread gets here once the jobs queued before it have run or wait (a channel waiting on the CPU
   stays parked, its deadline moved on by the time held, as nothing the CPU does can happen meanwhile), the renderer
   finishes what it has in flight, and the thread waits until gpu_release. One that gpu_hold gave up on goes past */
static void hold_here(void* arg) {
    u64 held;
    u32 c;
    (void)arg;
    if (g_renderer->idle) g_renderer->idle();
    pm_lock(&g_qlk);
    if (g_hold_want) {
        g_hold_at = 1;
        pc_broadcast(&g_qcv);
        held = plat_ns();
        while (g_hold_want) pc_wait(&g_qcv, &g_qlk, -1);
        g_hold_at = 0;
        held = plat_ns() - held;
        for (c = 0; c < 128; c++)
            if (g_qparked[c] && g_gpu.chan[c]) g_gpu.chan[c]->park_end += held;
    }
    pm_unlock(&g_qlk);
}
int gpu_hold(int timeout_ms) {
    u64 end = plat_ns() + (u64)timeout_ms * 1000000u;
    int r = 0;
    pm_lock(&g_qlk);
    if (!g_qthread) { /* nothing was ever submitted: there is no GPU thread to hold */
        pm_unlock(&g_qlk);
        return 0;
    }
    g_hold_want = 1;
    pm_unlock(&g_qlk);
    gpu_call(hold_here, NULL, NULL, 0);
    pm_lock(&g_qlk);
    while (!g_hold_at) {
        u64 now = plat_ns();
        if (now >= end) {
            g_hold_want = 0;
            r = -1;
            break;
        }
        pc_wait(&g_qcv, &g_qlk, (i64)(end - now));
    }
    pm_unlock(&g_qlk);
    if (r) {
        WARN("pause: the GPU thread didn't get to the pause within %d ms\n", timeout_ms);
        gpu_report_stall();
    }
    return r;
}
void gpu_release(void) {
    pm_lock(&g_qlk);
    g_hold_want = 0;
    pc_broadcast(&g_qcv);
    pm_unlock(&g_qlk);
}

/* ------------------------------------------------------------ the submission scan */
/* Each launch of the copy engine with a linear source has its source read at submission (dma.c's dma_snap_launch),
   since games free a copy's source right after submitting it and the GPU thread may run the copy frames later. The
   scan follows each channel's stream only as far as the copy engine needs, with its own subchannel classes and copy
   registers, and stops for good at anything after which the CPU may write the source first: a wait, a macro (which
   can wait), a report operation, a SYNC entry or a fence. A channel the GPU thread has caught up with starts again
   from the channel's own state */
typedef struct {
    u32 sub_class[GPU_SUBCHANNELS];
    PbState pb;
    u32 rdma[MAXWELL_DMA_METHODS];
    int stopped;
} GpuScan;
static GpuScan g_scan[GPU_CHANNELS];
static PMutex g_submit_lk = PMUTEX_INIT; /* the scans in queue order */
/* the engine methods the scan looks for: the 3D engine's report_semaphore operation, at the same method in compute
   (KEPLER_COMPUTE_REPORT_SEMAPHORE_QUERY), and the copy engine's launch */
/* 0 when the scan stops at this method */
static int scan_method(GpuScan* s, u32 chan, u64 job, u32* launches, u32 sub, u32 m, u32 v) {
    int eng;
    if (m < PULLER_NON_PULLER_METHODS) {
        u32 op = v & PULLER_SEMAPHORE_OP_MASK;
        if (m == PULLER_BIND_OBJECT) s->sub_class[sub] = v & CLASS_ID_MASK;
        if (m == PULLER_SEMAPHORE_OPERATION &&
            (op == PULLER_SEMAPHORE_OP_ACQUIRE_EQUAL || op == PULLER_SEMAPHORE_OP_ACQUIRE_GEQUAL ||
             op == PULLER_SEMAPHORE_OP_ACQUIRE_MASK))
            return 0;
        if (m == PULLER_SYNCPOINT_OPERATION && !puller_syncpoint_increment(v)) return 0;
        return 1;
    }
    eng = engine_of(s->sub_class[sub]);
    if (eng == ENG_3D && (m >= MAXWELL3D_NUM_REGS || m == MAXWELL3D_REPORT_SEMAPHORE_QUERY))
        return 0; /* a macro call (it can wait), or a report operation */
    if (eng == ENG_COMPUTE && m == KEPLER_COMPUTE_REPORT_SEMAPHORE_QUERY) return 0;
    if (eng == ENG_DMA && m < MAXWELL_DMA_METHODS) {
        s->rdma[m] = v;
        if (m == MAXWELL_DMA_LAUNCH_DMA) dma_snap_launch(chan, job, (*launches)++, s->rdma, v);
    }
    return 1;
}
static void scan_job(GpuScan* s, const GpuItem* j) {
    u32 e, i, launches = 0;
    if (j->wait_id != ~0u) { /* nothing was fetched: the CPU may write sources before the fence */
        s->stopped = 1;
        return;
    }
    for (e = 0; e < j->pf && !s->stopped; e++) {
        const u32* w = job_prefetched(j) + job_off(j)[e];
        u32 len = entry_words(job_ent(j)[e]);
        for (i = 0; i < len; i++) {
            u32 sub, m, v;
            int last;
            if (pb_decode(&s->pb, w[i], &sub, &m, &v, &last) &&
                !scan_method(s, j->chan, j->id, &launches, sub, m, v)) {
                s->stopped = 1;
                break;
            }
        }
    }
    if (j->pf < j->n) s->stopped = 1; /* a SYNC entry: the rest is fetched when the stream gets there */
}
/* a job for the entries, with those before the first SYNC entry fetched now (none when a fence comes first): the
   guest may reuse its command ring once the ioctl returns */
#define PREFETCH_MAX_WORDS ((64u << 20) / 4) /* 64 MiB of commands fetched at submission at most */
static GpuItem* job_new(const void* entries, u32 n, u32 wait_id) {
    u64 words = 0, size;
    u32 e, pf = 0;
    GpuItem* j;
    const u8* src = (const u8*)entries;
    if (wait_id == ~0u)
        for (pf = 0; pf < n; pf++) {
            u64 en = snx_ld_le64(src + (size_t)pf * 8);
            if (gpfifo_sync(en)) break; /* SYNC: this entry and the rest wait until the stream gets there */
            if (words + entry_words(en) > PREFETCH_MAX_WORDS) break; /* the rest is fetched when reached */
            words += entry_words(en);
        }
    /* the item, the entries (8-byte aligned after it), their offsets, the words */
    size = sizeof(GpuItem) + (u64)n * 8 + (u64)n * 4 + words * 4;
    if (size > SIZE_MAX || !(j = (GpuItem*)calloc(1, (size_t)size)))
        fatal("gpu: out of memory queueing %u entries\n", n);
    j->n = n;
    if (n) memcpy(job_ent(j), entries, (size_t)n * 8);
    snx_le64_array(job_ent(j), n); /* the entries are guest words, the stream's words below too */
    j->pf = pf;
    words = 0;
    for (e = 0; e < pf; e++) {
        u32 k = entry_words(job_ent(j)[e]);
        job_off(j)[e] = (u32)words;
        gmem_read_raw(entry_addr(job_ent(j)[e]), job_prefetched(j) + words, (u64)k * 4);
        snx_le32_array(job_prefetched(j) + words, k);
        words += k;
    }
    return j;
}
void gpu_submit(const void* entries, u32 n, u32 channel, u32 incs, u32 wait_id, u32 wait_val, u32 expect) {
    GpuItem* j;
    GpuScan* s;
    GpuChannel* ch;
    int idle, stale;
    if (channel >= GPU_CHANNELS)
        fatal("gpu: a submission on channel %u, past the %u the GPU has\n", channel, GPU_CHANNELS);
    pm_lock(&g_submit_lk);
    j = job_new(entries, n, wait_id);
    j->chan = channel;
    j->incs = incs;
    j->wait_id = wait_id;
    j->wait_val = wait_val;
    j->expect = expect;
    pm_lock(&g_qlk);
    queue_init();
    stale = !g_gpu.chan[channel] || g_scan_stale[channel];
    ch = channel_get(channel);
    g_scan_stale[channel] = 0;
    idle = !g_qjobs[channel]; /* nothing of the channel is queued or running: its state stands still */
    j->id = ++g_job_ids;
    pm_unlock(&g_qlk);
    s = &g_scan[channel];
    if (stale || idle) {
        memcpy(s->sub_class, ch->sub_class, sizeof s->sub_class);
        s->pb = ch->pb;
        memcpy(s->rdma, ch->rdma, sizeof s->rdma);
        s->stopped = 0;
    }
    if (!s->stopped) scan_job(s, j);
    may_have_changed(); /* before the job can run: what the CPU wrote is for it to see */
    pm_lock(&g_qlk);
    while (runnable() >= QUEUE_ROOM) pc_wait(&g_qcv, &g_qlk, -1);
    g_qjobs[channel]++;
    g_qtotal++;
    queue_append(j);
    pm_unlock(&g_qlk);
    g_gpu_submits++;
    pm_unlock(&g_submit_lk);
}
void gpu_channel_close(u32 channel) {
    GpuChannel* ch;
    if (channel >= GPU_CHANNELS) return;
    r3d_pending_flush(); /* a draw it merged goes first */
    pm_lock(&g_qlk);
    ch = g_gpu.chan[channel];
    g_gpu.chan[channel] = NULL;
    if (ch) g_gpu.nchan--;
    g_qparked[channel] = 0;
    g_scan_stale[channel] = 1;
    pm_unlock(&g_qlk);
    dma_snap_reset(channel);
    if (ch) {
        if (ch->render_pred) g_renderer->render_predicate_drop(ch->render_pred);
        free(ch->inline_indices);
        free(ch->i2m.inner_buffer);
        free(ch);
    }
}
void gpu_release_retire(u32 channel) {
    if (g_renderer->release_retire) g_renderer->release_retire(channel);
}

/* ------------------------------------------------------------ save states */
/* With the GPU thread held (gpu_hold) and the renderer idle. A channel goes as its bytes, with its two allocations'
   contents after it and what the renderer derives from it left out; the queue as its items in order: a job's entries,
   their offsets, the words fetched at submission and late, and where it stopped; a call (vi's present of a queued
   buffer, a closed channel's retire) by its function and argument. The submission scans and the copy snapshots go
   with them, so the jobs run as they would have */
#define GPU_STATE_VERSION 1
static u64 job_words_total(const GpuItem* j) {
    u64 w = 0;
    u32 e;
    for (e = 0; e < j->pf; e++) w += entry_words(job_ent(j)[e]);
    return w;
}
int gpu_state_save(StateIo* io) {
    GpuChannel* copy = (GpuChannel*)malloc(sizeof *copy);
    GpuItem* it;
    u32 c, n = 0;
    u64 now = plat_ns();
    int r = 0;
    if (!copy) return -1;
    pm_lock(&g_qlk);
    r |= io_put32(io, GPU_STATE_VERSION) | io_put64(io, g_job_ids) | io_put64(io, g_draws);
    for (c = 0; c < 128 && !r; c++) {
        const GpuChannel* ch = g_gpu.chan[c];
        if (!ch) continue;
        *copy = *ch;
        copy->i2m.inner_buffer = NULL;
        copy->inline_indices = NULL;
        copy->park_end = ch->park && ch->park_end > now ? ch->park_end - now : 0; /* what is left of the wait */
        r |= io_put32(io, c + 1) | io_put(io, copy, sizeof *copy);
        if (ch->i2m.inner_buffer) r |= io_put(io, ch->i2m.inner_buffer, ch->i2m.write_offset);
        if (ch->inline_indices) r |= io_put(io, ch->inline_indices, (u64)ch->inline_n * 4);
    }
    r |= io_put32(io, 0);
    for (it = g_qhead; it; it = it->next) n++;
    r |= io_put32(io, n);
    for (it = g_qhead; it && !r; it = it->next) {
        r |= io_put32(io, it->fn ? 1 : 0);
        if (it->fn) {
            const char* name = state_fn_name((StateFnPtr)it->fn);
            u32 len = name ? (u32)strlen(name) : 0;
            if (!name) {
                WARN("gpu: a queued call to %p has no name in state_fns.h's tables, and can't be saved\n",
                     (void*)(uintptr_t)it->fn);
                r = -1;
                break;
            }
            r |= io_put32(io, len) | io_put(io, name, len) | io_put64(io, (u64)(uintptr_t)it->arg);
            r |= io_put(io, it->chans, sizeof it->chans) | io_put32(io, it->nchans);
            continue;
        }
        r |= io_put64(io, it->id) | io_put32(io, it->chan) | io_put32(io, it->incs) | io_put32(io, it->wait_id);
        r |= io_put32(io, it->wait_val) | io_put32(io, it->expect) | io_put32(io, it->n) | io_put32(io, it->pf);
        r |= io_put32(io, it->entry) | io_put32(io, it->word) | io_put32(io, (u32)it->started);
        r |= io_put(io, job_ent(it), (u64)it->n * 8) | io_put(io, job_off(it), (u64)it->n * 4);
        r |= io_put(io, job_prefetched(it), job_words_total(it) * 4);
        r |= io_put32(io, it->late_first) | io_put32(io, it->late_end);
        if (it->late_end > it->late_first) {
            u64 w = 0;
            u32 e;
            for (e = it->late_first; e < it->late_end; e++) w += entry_words(job_ent(it)[e]);
            r |= io_put(io, it->late, w * 4);
        }
    }
    r |= io_put(io, g_scan, sizeof g_scan) | io_put(io, g_scan_stale, sizeof g_scan_stale);
    pm_unlock(&g_qlk);
    free(copy);
    return r ? -1 : dma_snap_save(io);
}
/* every channel and queued item goes, as when the channels close, with the GPU thread held */
static void gpu_state_drop(void) {
    GpuItem* it;
    u32 c;
    pm_lock(&g_qlk);
    while ((it = g_qhead) != NULL) {
        g_qhead = it->next;
        item_free(it);
    }
    g_qtail = NULL;
    g_qtotal = 0;
    memset(g_qjobs, 0, sizeof g_qjobs);
    memset(g_qparked, 0, sizeof g_qparked);
    pm_unlock(&g_qlk);
    for (c = 0; c < 128; c++) gpu_channel_close(c);
}
/* a GPU section read, not yet in place */
struct GpuStaged {
    u64 job_ids, draws;
    GpuChannel* chan[128];
    GpuItem *head, *tail;
    GpuScan scan[128];
    u8 scan_stale[128];
    DmaStaged* dma;
};
void gpu_state_staged_free(GpuStaged* s) {
    u32 c;
    if (!s) return;
    for (c = 0; c < 128; c++)
        if (s->chan[c]) {
            free(s->chan[c]->i2m.inner_buffer);
            free(s->chan[c]->inline_indices);
            free(s->chan[c]);
        }
    while (s->head) {
        GpuItem* it = s->head;
        s->head = it->next;
        item_free(it);
    }
    dma_snap_staged_free(s->dma);
    free(s);
}
/* the item at the queue's end */
static void staged_append(GpuStaged* s, GpuItem* it) {
    if (s->tail)
        s->tail->next = it;
    else
        s->head = it;
    s->tail = it;
}
GpuStaged* gpu_state_parse(StateIo* io) {
    GpuStaged* s = (GpuStaged*)calloc(1, sizeof *s);
    u32 version, c, n, i, kind;
    if (!s) return NULL;
    if (io_get32(io, &version) || version != GPU_STATE_VERSION || io_get64(io, &s->job_ids) || io_get64(io, &s->draws))
        goto bad;
    for (;;) {
        GpuChannel* ch;
        if (io_get32(io, &c)) goto bad;
        if (!c) break;
        if (c > 128 || s->chan[c - 1] || !(ch = s->chan[c - 1] = (GpuChannel*)calloc(1, sizeof *ch))) goto bad;
        if (io_get(io, ch, sizeof *ch)) {
            memset(ch, 0, sizeof *ch); /* none of its pointers are this process's */
            goto bad;
        }
        ch->i2m.inner_buffer = NULL;
        ch->inline_indices = NULL;
        if (ch->i2m.capacity) {
            if (ch->i2m.write_offset > ch->i2m.capacity || !(ch->i2m.inner_buffer = (u8*)malloc(ch->i2m.capacity)) ||
                io_get(io, ch->i2m.inner_buffer, ch->i2m.write_offset))
                goto bad;
        }
        if (ch->inline_cap) {
            if (ch->inline_n > ch->inline_cap || !(ch->inline_indices = (u32*)malloc((size_t)ch->inline_cap * 4)) ||
                io_get(io, ch->inline_indices, (u64)ch->inline_n * 4))
                goto bad;
        }
        ch->r3d_dirty = ~0u; /* nothing the renderer derived holds */
        memset(ch->mme_choice, 0, sizeof ch->mme_choice);
        ch->render_pred = 0; /* the predicate was the renderer's: draws run until the next render enable */
        ch->xfb_started = 0; /* the byte counters were the renderer's */
    }
    if (io_get32(io, &n)) goto bad;
    for (i = 0; i < n; i++) {
        GpuItem* it;
        if (io_get32(io, &kind)) goto bad;
        if (kind) {
            char name[256];
            u32 len;
            u64 arg;
            StateFnPtr f;
            if (io_get32(io, &len) || len >= sizeof name || io_get(io, name, len)) goto bad;
            name[len] = 0;
            if (!(f = state_fn_find(name))) {
                WARN("gpu: the state queues a call to %s, which this build doesn't have\n", name);
                goto bad;
            }
            if (!(it = (GpuItem*)calloc(1, sizeof *it))) goto bad;
            staged_append(s, it);
            if (io_get64(io, &arg) || io_get(io, it->chans, sizeof it->chans) || io_get32(io, &it->nchans) ||
                it->nchans > sizeof it->chans / sizeof *it->chans)
                goto bad;
            it->fn = (void (*)(void*))f;
            it->arg = (void*)(uintptr_t)arg;
        } else {
            u32 h[10]; /* chan, incs, wait_id, wait_val, expect, n, pf, entry, word, started */
            u64 id, words, size;
            GpuItem* big;
            if (io_get64(io, &id) || io_get(io, h, sizeof h) || h[5] > (1u << 24) || h[6] > h[5] || h[0] >= 128)
                goto bad;
            /* the entries come first: the words fetched at submission are counted from them */
            size = sizeof(GpuItem) + (u64)h[5] * 12;
            if (!(it = (GpuItem*)calloc(1, (size_t)size))) goto bad;
            it->n = h[5];
            it->pf = h[6];
            if (io_get(io, job_ent(it), (u64)it->n * 8) || io_get(io, job_off(it), (u64)it->n * 4)) {
                free(it);
                goto bad;
            }
            words = job_words_total(it);
            if (words > (1u << 26) || !(big = (GpuItem*)realloc(it, (size_t)(size + words * 4)))) {
                free(it);
                goto bad;
            }
            it = big;
            staged_append(s, it);
            if (io_get(io, job_prefetched(it), words * 4) || io_get32(io, &it->late_first) ||
                io_get32(io, &it->late_end) || it->late_end > it->n || it->late_first > it->late_end)
                goto bad;
            it->id = id;
            it->chan = h[0];
            it->incs = h[1];
            it->wait_id = h[2];
            it->wait_val = h[3];
            it->expect = h[4];
            it->entry = h[7];
            it->word = h[8];
            it->started = (int)h[9];
            if (it->late_end > it->late_first) {
                u64 w = 0;
                u32 e;
                for (e = it->late_first; e < it->late_end; e++) w += entry_words(job_ent(it)[e]);
                if (w > (1u << 26) || !(it->late = (u32*)malloc((size_t)(w ? w : 1) * 4))) goto bad;
                it->late_cap = (u32)w;
                if (io_get(io, it->late, w * 4)) goto bad;
            }
        }
    }
    if (io_get(io, s->scan, sizeof s->scan) || io_get(io, s->scan_stale, sizeof s->scan_stale) ||
        !(s->dma = dma_snap_parse(io)))
        goto bad;
    return s;
bad:
    gpu_state_staged_free(s);
    return NULL;
}
void gpu_state_install(GpuStaged* s) {
    u64 now = plat_ns();
    GpuItem* it;
    u32 c;
    gpu_state_drop();
    pm_lock(&g_qlk);
    g_job_ids = s->job_ids;
    g_draws = s->draws;
    for (c = 0; c < 128; c++)
        if (s->chan[c]) {
            s->chan[c]->park_end += now; /* what was left of the wait, from now */
            g_gpu.chan[c] = s->chan[c];
            g_gpu.nchan++;
            s->chan[c] = NULL;
        }
    for (it = s->head; it; it = it->next)
        if (!it->fn) {
            g_qjobs[it->chan]++;
            g_qtotal++;
            /* the channel's oldest job, stopped at a wait: it waits again */
            if (it->started && g_gpu.chan[it->chan] && g_gpu.chan[it->chan]->park) g_qparked[it->chan] = 1;
        }
    g_qhead = s->head;
    g_qtail = s->tail;
    s->head = s->tail = NULL;
    memcpy(g_scan, s->scan, sizeof g_scan);
    memcpy(g_scan_stale, s->scan_stale, sizeof g_scan_stale);
    g_qgen++;
    pm_unlock(&g_qlk);
    dma_snap_install(s->dma);
    s->dma = NULL;
    free(s);
}
void gpu_state_schema(int (*dump)(u64* h, const char* fmt, ...), u64* h) {
#if defined(__clang__)
    __builtin_dump_struct(&g_scan[0], dump, h);
#else /* SWR: GCC has no struct walk; our runtime has no save states */
    (void)dump, (void)h;
#endif
}
