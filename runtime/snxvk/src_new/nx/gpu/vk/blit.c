/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 2D engine's blits that the GPU's own image blit can't take (a source that isn't a render target), done on the
   GPU from memory, the destination kept there (docs/GPU_OWNERSHIP.md). maxwell/fermi2d.c works out which source pixel
   each destination column and row takes (nearest sampled, scaled); here the source's surface is staged as the GPU
   sees it (guest bytes, what the GPU holds, render targets drawn over it) and taken apart into rows, the destination
   rectangle's rows are gathered through the tables (texconv.comp), a row of the rectangle a word an invocation, the
   components converted between float formats when the blit says so (TC_BLIT_FLOAT), and copied into the
   destination's rows, exactly the rectangle's bytes, which go back into its layout and are published over its span.
   Under vk_lock, in the frame's command buffer */
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"

static u64 up(u64 v, u64 a) { return (v + a - 1) / a * a; }

/* a plane's bytes: pitch linear rows, or a block linear surface up to its slice */
static u64 plane_span(const GpuDmaSide* s) {
    if (!s->bl) return (u64)s->pitch * (s->h ? s->h - 1 : 0) + s->wb;
    u64 gobs_x = ((u64)s->wb + 63) >> 6, rows = ((((u64)s->h + 7) >> 3) + (1u << s->bh) - 1) >> s->bh;
    return gobs_x * rows * ((u64)512 << (s->bh + s->bd)) * ((u64)(s->z >> s->bd) + 1);
}
static u8* stage_range(u64 gva, u64 span, VkBuffer* buf, u64* at) {
    u8* p = vk_stage(span, vk_stage_align(), buf, at);
    if (!p) return NULL;
    vk_guest_read(gva, span, p, *buf, *at);
    vk_rt_overlay(gva, span, *buf, *at);
    return p;
}

static int blit(const GpuBlit* b) {
    u32 n = b->x1 - b->x0, rows = b->y1 - b->y0, bd = GPU_BLIT_DST_BPP(b), nb = n * bd;
    u64 sspan = plane_span(&b->src), dspan = plane_span(&b->dst), cap, sat, dat, tat;
    u32 pa = b->src.bl ? (u32)up(b->src.wb, 16) : b->src.pitch, pb = (u32)up(nb, 4), pc = (u32)up(b->dst.wb, 16);
    /* the scratch: the source's rows, the tables, the gathered rows, the destination's rows (block linear) */
    u64 at_a = 0, at_t = up(b->src.bl ? (u64)pa * b->src.h : sspan, 16);
    u64 at_b = up(at_t + 4ull * (n + rows), 16), at_c = up(at_b + (u64)pb * rows, 16);
    u64 used = at_c + (b->dst.bl ? (u64)pc * rows : 0) + 16;
    VkBuffer sb, db, tb, scr;
    VkCommandBuffer cb;
    VkOwnRes* d;
    int ok = 1;
    if (!n || !rows) return 1;
    vk_render_end();
    if (sspan > g_vk.alloc_cap || dspan > g_vk.alloc_cap || !(scr = vk_bl_scratch(used, &cap))) return 0;
    u32* t = (u32*)vk_stage(4ull * (n + rows), vk_stage_align(), &tb, &tat);
    if (!t || !stage_range(b->src.addr, sspan, &sb, &sat)) return 0;
    memcpy(t, b->scol, 4ull * n);
    memcpy(t + n, b->srow, 4ull * rows);
    snx_le32_array(t, (size_t)n + rows); /* the device reads the tables' words little-endian */
    d = vk_own_res_new(dspan);
    if (!d || !stage_range(b->dst.addr, dspan, &db, &dat)) {
        if (d) vk_own_res_unref(d);
        return 0;
    }
    cb = vk_cmd();
    {
        VkBufferCopy c[3] = {{dat, 0, dspan}, {tat, at_t, 4ull * (n + rows)}, {sat, at_a, sspan}};
        rec_CmdCopyBuffer(cb, db, d->buf, 1, &c[0]);
        rec_CmdCopyBuffer(cb, tb, scr, 1, &c[1]);
        if (!b->src.bl) rec_CmdCopyBuffer(cb, sb, scr, 1, &c[2]); /* pitch linear rows as they are */
    }
    vk_all_barrier(cb);
    VkBlMove ms = {b->src.wb, b->src.h, b->src.bh, b->src.bd, 0, b->src.h, b->src.z, 1, pa};
    VkBlMove md = {b->dst.wb, b->dst.h, b->dst.bh, b->dst.bd, b->y0, rows, b->dst.z, 1, pc};
    u64 slo = 0, shi, dlo = 0, dhi;
    if (b->src.bl) {
        vk_bl_range(&ms, &slo, &shi);
        ok = vk_bl_move(cb, &ms, 0, sb, sat + slo, scr, at_a);
    }
    if (ok && b->dst.bl) {
        vk_bl_range(&md, &dlo, &dhi);
        ok = vk_bl_move(cb, &md, 0, d->buf, dlo, scr, at_c);
    }
    vk_all_barrier(cb);
    ok = ok && vk_tc_blit(cb, b->bpp, b->fmt, nb, rows, scr, used, at_a, pa, at_t, at_t + 4ull * n, scr, at_b, pb);
    vk_all_barrier(cb);
    if (ok) { /* the rectangle's bytes into the destination's rows, which go back into its layout */
        if (b->dst.bl)
            vk_copy_rows(cb, scr, at_b, pb, scr, at_c + (u64)b->x0 * bd, pc, nb, rows);
        else
            vk_copy_rows(cb, scr, at_b, pb, d->buf, (u64)b->y0 * b->dst.pitch + (u64)b->x0 * bd, b->dst.pitch, nb,
                         rows);
        vk_all_barrier(cb);
        if (b->dst.bl) ok = vk_bl_move(cb, &md, 1, d->buf, dlo, scr, at_c);
        vk_all_barrier(cb);
    }
    if (ok) {
        u64 off = 0;
        while (off < dspan) {
            u64 cpu = 0, k = gmmu_run(b->dst.addr + off, dspan - off, &cpu);
            if (!k) {
                k = 0x1000 - ((b->dst.addr + off) & 0xFFF);
                if (k > dspan - off) k = dspan - off;
            } else
                vk_own_publish(cpu, k, d, off);
            off += k;
        }
        gpu_mem_written(b->dst.addr, dspan);
    } else
        LOG_LIMIT(4, "gpu: a 2D blit of %ux%u pixels exceeds the device's limits: skipped\n", n, rows);
    vk_own_res_unref(d);
    return ok;
}
int vk_blit_mem(const GpuBlit* b) {
    int r;
    vk_lock();
    r = blit(b);
    vk_unlock();
    return r;
}
