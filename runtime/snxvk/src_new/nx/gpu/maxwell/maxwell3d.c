/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/maxwell_3d.cpp, src/video_core/engines/draw_manager.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The 3D engine's side of the renderer: draws and clears (gated by the render enable), DRAW_TEXTURE, the constant
   buffers' data and bindings, the 2D engine's blit, a clear in memory before the renderer runs, and the record of
   GPU-side writes the renderer's caches check */
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/maxwell/write_pages.h"
#include "nx/hle/services/nvdrv.h"

/* ------------------------------------------------------------ the record of GPU-side writes */
/* Each write stamps a new generation on every 4 KiB page it covers, by GPU address: the pages written and every other
   GPU page showing the same guest memory, found through the GMMU's index of mappings by guest address (gmmu_aliases)
   when the write is recorded. Writes are recorded a few thousand times a frame and asked about tens of thousands of
   times, so the aliasing is resolved on the side asked less, and a question reads one table with no translation. A
   mapping change stamps the pages it changes (nvdrv.c's gmmu_change_end), so a GPU page newly showing guest memory
   reads as written after anything asked before. Exact per page: a texture is dirty only when a write touched one of
   its own pages. Writers take the lock, readers don't (gpu_mem_dirty_since); the generation is published with
   release order for those reads.
   SNX_ALIAS_CHECK keeps the guest-address table as well and has gpu_mem_dirty_since answer the old way too,
   translating the range, warning when only that way finds a write */
static PMutex g_wlk = PMUTEX_INIT;
static WritePages g_wgpu, g_wcpu;
static u64 g_wgen;
static int g_wcheck = -1; /* SNX_ALIAS_CHECK, -1 until read */
static int wcheck_on(void) {
    int c = __atomic_load_n(&g_wcheck, __ATOMIC_RELAXED);
    if (c < 0) { /* threads reading it at once all read the same */
        const char* e = getenv("SNX_ALIAS_CHECK");
        c = e && *e && *e != '0';
        if (__atomic_exchange_n(&g_wcheck, c, __ATOMIC_RELAXED) < 0 && c)
            LOG("gpu: SNX_ALIAS_CHECK: GPU-side writes are checked through the GMMU too\n");
    }
    return c;
}
/* calls fn on each run of contiguous guest memory behind [gva, gva + size), from gmmu_run */
static void guest_runs(u64 gva, u64 size, int (*fn)(u64 cpu, u64 n, void* ctx), void* ctx) {
    u64 left = gva + size < gva ? ~(u64)0 - gva : size;
    while (left) {
        u64 cpu = 0, n = gmmu_run(gva, left, &cpu);
        if (n) {
            if (fn(cpu, n, ctx)) return;
        } else if ((n = 0x1000 - (gva & 0xFFF)) >= left)
            break;
        gva += n;
        left -= n;
    }
}
static void stamp_alias(void* ctx, u64 gva, u64 n) { write_pages_stamp(&g_wgpu, gva, n, *(const u64*)ctx); }
/* guest memory written: the GPU pages showing it */
static void stamp_cpu(u64 cpu, u64 n, u64 gen) {
    gmmu_aliases(cpu, n, stamp_alias, &gen);
    if (wcheck_on()) write_pages_stamp(&g_wcpu, cpu, n, gen);
}
static int stamp_run(u64 cpu, u64 n, void* ctx) {
    stamp_cpu(cpu, n, *(const u64*)ctx);
    return 0;
}
void gpu_mem_written(u64 gva, u64 size) {
    u64 gen;
    if (!size) return;
    pm_lock(&g_wlk);
    gen = g_wgen + 1;
    write_pages_stamp(&g_wgpu, gva, size, gen);
    guest_runs(gva, size, stamp_run, &gen);
    __atomic_store_n(&g_wgen, gen, __ATOMIC_RELEASE);
    pm_unlock(&g_wlk);
}
void gpu_mem_written_shader(u64 gva, u64 size) { gpu_mem_written(gva, size); }
void gpu_mem_written_cpu(u64 cpu, u64 size) {
    u64 gen;
    if (!size) return;
    pm_lock(&g_wlk);
    gen = g_wgen + 1;
    stamp_cpu(cpu, size, gen);
    __atomic_store_n(&g_wgen, gen, __ATOMIC_RELEASE);
    pm_unlock(&g_wlk);
}
u64 gpu_mem_generation(void) { return __atomic_load_n(&g_wgen, __ATOMIC_ACQUIRE); }
/* a save state's load: every page reads as written after anything asked before, through the tables' overflow, which
   covers every page */
void gpu_mem_written_all(void) {
    u64 gen;
    pm_lock(&g_wlk);
    gen = g_wgen + 1;
    __atomic_store_n(&g_wgpu.overflow, gen, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wcpu.overflow, gen, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wgen, gen, __ATOMIC_RELEASE);
    pm_unlock(&g_wlk);
}
typedef struct {
    u64 since;
    int dirty;
} DirtyAsk;
static int dirty_run(u64 cpu, u64 n, void* ctx) {
    DirtyAsk* q = (DirtyAsk*)ctx;
    q->dirty = write_pages_since(&g_wcpu, q->since, cpu, n);
    return q->dirty;
}
/* Without g_wlk, which only orders the writers: a stamp is stored after the memory it is for is written, so a reader
   holding the lock could already see new bytes under an old stamp, and reading the tables lock-free (write_pages.c)
   tells no less. Every draw asks this of each buffer, texture and target, and the lock was 2% of the GPU thread in
   one game */
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) {
    DirtyAsk q;
    if (!sz || since >= __atomic_load_n(&g_wgen, __ATOMIC_ACQUIRE)) return 0;
    if (write_pages_since(&g_wgpu, since, a, sz)) return 1;
    if (!wcheck_on()) return 0;
    q.since = since;
    q.dirty = 0;
    guest_runs(a, sz, dirty_run, &q);
    if (q.dirty)
        WARN_LIMIT(16, "gpu: guest memory behind GPU range %llx+%llx was written, but not stamped at its GPU pages\n",
                   (unsigned long long)a, (unsigned long long)sz);
    return q.dirty;
}

/* ------------------------------------------------------------ draws and clears */
/* the separate topology (0x652 bit 0, PrimitiveTopologyControl UseSeparateState) and its override enum (0x65C) as
   the draw's topology: points and lines are numbered one past the draw's enum, the legacy values by name */
static u32 topology_of(const u32* r, u32 topology) {
    u32 o = r[0x65C];
    if (!(r[0x652] & 1) || !o) return topology;
    switch (o) {
        case 0x1: return 0; /* points */
        case 0x2: return 1; /* lines */
        case 0x1001: return 0;
        case 0x1002:
        case 0x100F: return 1;
        case 0x1003:
        case 0x1012: return 4;
        case 0x1010:
        case 0x1011: return 3;
        case 0x1013:
        case 0x1014: return 5;
        case 0x1015: return 6;
        default: return o & 15;
    }
}

void r3d_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
              u32 instances) {
    if (ch->render_off || !g_renderer->draw) return;
    g_renderer->draw(ch, indexed, topology_of(ch->r3d, topology), first, count, base_vertex, base_instance, instances);
}
/* guest memory without the renderer's hooks: only for the clear below, made while no renderer runs */
static void raw_write(u64 gva, const u8* src, u32 n) {
    while (n) {
        u32 k = 0x1000 - (u32)(gva & 0xFFF);
        u64 cpu = gmmu_translate(gva);
        if (k > n) k = n;
        if (cpu) memcpy(GP(cpu), src, k);
        gva += k;
        src += k;
        n -= k;
    }
}
static u8 unorm8(const u32* r, int c) {
    float f;
    memcpy(&f, &r[0x360 + c], 4);
    if (!(f > 0)) return 0;
    return f >= 1 ? 255 : (u8)(f * 255.0f + 0.5f);
}
/* a full color clear of one target in guest memory, for a clear before the renderer runs. A pitch linear target
   holds its row pitch in bytes in the width register, as the renderer reads it */
static void clear_in_memory(GpuChannel* ch, u32 v) {
    const u32* r = ch->r3d;
    u32 t = (v >> 6) & 15, b = 0x200 + t * 16, fmt, bpp, linear, w, h, pitch, x, y, bh;
    u8 px[16], row[256];
    u64 a, span;
    if (t >= 8 || ((v >> 2) & 15) != 15 || !(fmt = r[b + 4])) return;
    a = ((u64)(r[b] & 0xFF) << 32) | r[b + 1];
    bpp = gpu_color_bpp(fmt);
    linear = (r[b + 5] >> 12) & 1;
    bh = (r[b + 5] >> 4) & 15;
    pitch = linear ? r[b + 2] : r[b + 2] * bpp;
    w = linear ? r[b + 2] / bpp : r[b + 2];
    h = r[b + 3];
    memset(px, 0, sizeof px);
    if (fmt == 0xCF || fmt == 0xD0) /* A8R8G8B8: B, G, R, A in memory */
        px[0] = unorm8(r, 2), px[1] = unorm8(r, 1), px[2] = unorm8(r, 0), px[3] = unorm8(r, 3);
    else if (bpp == 4) /* A8B8G8R8 and the other 4-byte formats taken as it */
        px[0] = unorm8(r, 0), px[1] = unorm8(r, 1), px[2] = unorm8(r, 2), px[3] = unorm8(r, 3);
    else if (bpp == 16) /* the four floats as they are, low byte first */
        for (x = 0; x < 4; x++) snx_st_le32(px + x * 4, r[0x360 + x]);
    else
        LOG_LIMIT(2, "gpu: a clear of format %x before the renderer runs writes zeros\n", fmt);
    for (x = 0; x < sizeof row; x += bpp) memcpy(row + x, px, bpp);
    if (!a || !w || !h) return;
    if (linear) {
        for (y = 0; y < h; y++)
            for (x = 0; x < w * bpp; x += sizeof row)
                raw_write(a + (u64)y * pitch + x, row, w * bpp - x < sizeof row ? w * bpp - x : (u32)sizeof row);
        gpu_mem_written(a, (u64)pitch * (h - 1) + (u64)w * bpp);
        return;
    }
    for (y = 0; y < h; y++) /* 16 byte runs in the layout, recorded once */
        for (x = 0; x < w * bpp; x += 16)
            raw_write(a + bl_offset(x, y, 0, w * bpp, h, bh, 0), row, w * bpp - x < 16 ? w * bpp - x : 16);
    span = (u64)((w * bpp + 63) / 64) * 64 * ((((u64)h + 7) / 8 + (1u << bh) - 1) >> bh << bh) * 8;
    gpu_mem_written(a, span);
}
void r3d_clear(GpuChannel* ch, u32 v) {
    if (ch->render_off) return;
    if (!g_renderer_up)
        clear_in_memory(ch, v);
    else if (g_renderer->clear)
        g_renderer->clear(ch, v);
}
/* DRAW_TEXTURE: not gated by the render enable here, the renderer reads its registers as Eden does */
void r3d_draw_texture(GpuChannel* ch) {
    if (g_renderer->draw_texture) g_renderer->draw_texture(ch);
}
int r3d_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
             double sdy) {
    return g_renderer->blit ? g_renderer->blit(s, d, dx, dy, dw, dh, sx, sy, sdx, sdy) : 0;
}

/* ------------------------------------------------------------ constant buffers */
static u64 cb_address(const u32* r) { return ((u64)(r[0x8E1] & 0xFF) << 32) | r[0x8E2]; }
/* BIND_GROUP (0x904 + 8 * stage): bit 0 valid, bits 4-8 the slot, of the buffer at 0x8E1-0x8E2 of size 0x8E0 */
void r3d_cb_bind(GpuChannel* ch, int stage, u32 v) {
    u32 slot = (v >> 4) & 31;
    if (stage < 0 || stage >= 5 || slot >= 18) {
        LOG_LIMIT(4, "gpu: constant buffer bind %08x of stage %d is past the slots\n", v, stage);
        return;
    }
    ch->cbind[stage][slot] = (v & 1) ? cb_address(ch->r3d) : 0;
    ch->cbind_size[stage][slot] = (v & 1) ? ch->r3d[0x8E0] : 0;
}
/* LOAD_CONSTANT_BUFFER (0x8E4-0x8F3): a word at the buffer's address plus its offset (0x8E3), which moves on by 4.
   A batch's words are held and written in one go at its last, or before anything else */
void r3d_const_upload(GpuChannel* ch, u32 v, int last) {
    u32* r = ch->r3d;
    u64 a = cb_address(r) + r[0x8E3];
    if (ch->cb_n && (a != ch->cb_addr + (u64)ch->cb_n * 4 || ch->cb_n == sizeof ch->cb_words / 4))
        r3d_const_flush(ch);
    if (!ch->cb_n) ch->cb_addr = a;
    ch->cb_words[ch->cb_n++] = v;
    r[0x8E3] += 4;
    if (last) r3d_const_flush(ch);
}
void r3d_const_flush(GpuChannel* ch) {
    if (!ch->cb_n) return;
    snx_le32_array(ch->cb_words, ch->cb_n); /* guest memory's order: the words aren't read again */
    gmem_write(ch->cb_addr, ch->cb_words, (u64)ch->cb_n * 4);
    ch->cb_n = 0;
}
