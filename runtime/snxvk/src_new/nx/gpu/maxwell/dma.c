/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/maxwell_dma.cpp, src/video_core/engines/maxwell_dma.h,
   src/video_core/textures/decoders.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The copy engine (MAXWELL_DMA_COPY_A, 0xB0B5; open-gpu-doc's clb0b5.h): one-dimensional copies, and multi-line
   copies between pitch linear and block linear surfaces, with the remap of components and its constant fills, and
   the completion semaphore. gpu.c's puller hands it its methods (dma_method); a LAUNCH_DMA is decoded here into a
   GpuDmaOp (gpu.h) and done on the GPU by the renderer's dma_copy (vk/dma.c), which covers Eden's CopyPitchToPitch,
   CopyBlockLinearToPitch and CopyPitchToBlockLinear by each side's layout. Also the copies of linear sources read at
   submission (dma_snap_*), which gpu.c's submission scan makes. Register names are maxwell_dma_regs.h's */
#include "nx/runtime/state_io.h"
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/maxwell/maxwell_dma_regs.h"

#define DMA_MAX_BYTES (256u << 20) /* a launch past this is a bad register value */

/* ------------------------------------------------------------ a launch's operands */
/* a launch is a GpuDmaOp (gpu.h): the renderer does it on the GPU */
typedef GpuDmaSide DmaSide;
typedef GpuDmaOp DmaOp;
/* an operand (Eden's DMA::ImageOperand or BufferOperand): its address, layout and pitch, and, block linear, its
   DMA::Parameters from params (MAXWELL_DMA_SRC_PARAMS or _DST_PARAMS) with elements elem bytes */
static void dma_operand(DmaSide* s, const u32* r, u64 addr, int bl, u32 pitch, u32 params, u32 elem) {
    const u32* p = r + params;
    s->addr = addr;
    s->bl = bl;
    s->pitch = pitch;
    s->bh = dma_block_size_height(p[MAXWELL_DMA_PARAMS_BLOCK_SIZE]);
    s->bd = dma_block_size_depth(p[MAXWELL_DMA_PARAMS_BLOCK_SIZE]);
    s->wb = p[MAXWELL_DMA_PARAMS_WIDTH] * elem;
    s->h = p[MAXWELL_DMA_PARAMS_HEIGHT];
    s->z = p[MAXWELL_DMA_PARAMS_LAYER];
    s->ox = dma_origin_x(p[MAXWELL_DMA_PARAMS_ORIGIN]) * elem;
    s->oy = dma_origin_y(p[MAXWELL_DMA_PARAMS_ORIGIN]);
}
/* The remap (Eden's RemapConst): each of the destination's components from a source component, constant A or B,
   or left as it is. An element is the components' bytes; a launch whose components all take a constant is a fill,
   which reads nothing */
static void dma_remap(const u32* r, DmaOp* o) {
    u32 k, c = r[MAXWELL_DMA_REMAP_COMPONENTS];
    o->cs = remap_component_size(c);
    o->ns = remap_num_src_components(c);
    o->nd = remap_num_dst_components(c);
    o->fill = 1;
    for (k = 0; k < o->nd; k++) {
        o->sel[k] = remap_dst_component(c, k);
        if (o->sel[k] != REMAP_SWIZZLE_CONST_A && o->sel[k] != REMAP_SWIZZLE_CONST_B) o->fill = 0;
        if (o->sel[k] != k) o->shuffle = 1;
        if (o->sel[k] >= REMAP_SWIZZLE_NO_WRITE) o->keep = 1; /* the destination's own bytes stay */
    }
    if (o->ns != o->nd) o->shuffle = 1;
}
/* A LAUNCH_DMA's operation from its word v and the registers: a line of line_length_in elements, line_count of them
   when multi-line, else one line copied one-dimensionally whatever the layouts (as Eden and Ryujinx do) */
static void dma_decode_launch(const u32* r, u32 v, DmaOp* o) {
    u32 elem_in = 1, elem_out = 1;
    int src_bl, dst_bl;
    memset(o, 0, sizeof *o);
    o->multi = launch_dma_multi_line_enable(v);
    o->remap = launch_dma_remap_enable(v);
    o->width = r[MAXWELL_DMA_LINE_LENGTH_IN];
    o->lines = o->multi ? r[MAXWELL_DMA_LINE_COUNT] : 1;
    if (o->remap) {
        dma_remap(r, o);
        elem_in = o->cs * o->ns;
        elem_out = o->cs * o->nd;
    }
    o->line_in = o->width * elem_in;
    o->line_out = o->width * elem_out;
    o->const_a = r[MAXWELL_DMA_REMAP_CONSTA_VALUE];
    o->const_b = r[MAXWELL_DMA_REMAP_CONSTB_VALUE];
    src_bl = o->multi && launch_dma_src_memory_layout(v) == LAUNCH_DMA_MEMORY_LAYOUT_BLOCKLINEAR;
    dst_bl = o->multi && launch_dma_dst_memory_layout(v) == LAUNCH_DMA_MEMORY_LAYOUT_BLOCKLINEAR;
    dma_operand(&o->src, r, gpu_packed_va(r[MAXWELL_DMA_OFFSET_IN_UPPER], r[MAXWELL_DMA_OFFSET_IN_LOWER]), src_bl,
                r[MAXWELL_DMA_PITCH_IN], MAXWELL_DMA_SRC_PARAMS, elem_in);
    dma_operand(&o->dst, r, gpu_packed_va(r[MAXWELL_DMA_OFFSET_OUT_UPPER], r[MAXWELL_DMA_OFFSET_OUT_LOWER]), dst_bl,
                r[MAXWELL_DMA_PITCH_OUT], MAXWELL_DMA_DST_PARAMS, elem_out);
}
/* the bytes of a pitch linear operand's lines */
static u64 dma_pitch_span(const DmaSide* s, u32 lines, u32 line) { return (u64)s->pitch * (lines - 1) + line; }

/* ------------------------------------------------------------ copies of linear sources made at submission */
/* The console's copy engine reads a source microseconds after the submission, while the GPU thread may run the copy
   frames later, and games free a copy's source right after submitting it (Catherine frees its texture uploads'
   staging at the start of the next frame, and the piano's keys reflected garbage). So a launch with a linear source
   has its source read at submission, in the channel's order; the copy uses it unless the GPU has written the source
   since (guest memory then has the newer data) */
#define SNAP_RING 64
#define SNAP_TOTAL (256u << 20) /* bytes held at most across the channels: past it copies read memory */
#define SNAP_CHANNELS 128u      /* channels are named by syncpoint id, below 128 (nvdrv's syncpt_alloc, gpu.h) */
typedef struct {
    u64 job, src, len, gen;
    u32 launch;
    u8* data;
} DmaSnap;
typedef struct {
    DmaSnap e[SNAP_RING];
    u32 head, tail;
} DmaSnapRing;
/* the submission-time copies, by channel, and the bytes they hold: the GPU thread and the submitting threads share
   them under lk */
static struct {
    DmaSnapRing* rings[SNAP_CHANNELS];
    u64 bytes;
    PMutex lk;
} g_dma = {{NULL}, 0, PMUTEX_INIT};
static void snap_drop(DmaSnapRing* q) { /* the oldest, the lock held */
    DmaSnap* e = &q->e[q->tail++ % SNAP_RING];
    g_dma.bytes -= e->len;
    free(e->data);
    e->data = NULL;
}
static int dma_linear_source(const DmaOp* o) { return !o->fill && !o->src.bl && o->width && o->lines; }
void dma_snap_launch(u32 channel, u64 job_id, u32 launch_index, const u32* r, u32 v) {
    DmaOp o;
    DmaSnapRing* q;
    DmaSnap* e;
    u64 len, gen;
    u8* data;
    if (channel >= SNAP_CHANNELS) return;
    dma_decode_launch(r, v, &o);
    if (!dma_linear_source(&o)) return;
    len = dma_pitch_span(&o.src, o.lines, o.line_in);
    pm_lock(&g_dma.lk);
    if (!g_dma.rings[channel]) g_dma.rings[channel] = (DmaSnapRing*)calloc(1, sizeof(DmaSnapRing));
    q = g_dma.rings[channel];
    if (!q || q->head - q->tail == SNAP_RING || len > SNAP_TOTAL - g_dma.bytes) {
        pm_unlock(&g_dma.lk);
        return;
    }
    g_dma.bytes += len;
    pm_unlock(&g_dma.lk);
    gen = gpu_mem_generation(); /* before the read: a GPU write after it shows */
    data = (u8*)malloc((size_t)len);
    if (data) gmem_read_raw(o.src.addr, data, len);
    pm_lock(&g_dma.lk);
    if (!data) {
        g_dma.bytes -= len;
        pm_unlock(&g_dma.lk);
        return;
    }
    e = &q->e[q->head++ % SNAP_RING];
    e->job = job_id;
    e->launch = launch_index;
    e->src = o.src.addr;
    e->len = len;
    e->gen = gen;
    e->data = data;
    pm_unlock(&g_dma.lk);
}
/* the copy of launch (job, launch) when there is one for this source, taken over by the caller; older ones go */
static u8* snap_take(u32 channel, u64 job, u32 launch, u64 src, u64 len, u64* gen) {
    DmaSnapRing* q;
    u8* data = NULL;
    if (channel >= SNAP_CHANNELS) return NULL;
    pm_lock(&g_dma.lk);
    q = g_dma.rings[channel];
    while (q && q->tail != q->head) {
        DmaSnap* e = &q->e[q->tail % SNAP_RING];
        if (e->job < job || (e->job == job && e->launch < launch)) { /* stale */
            snap_drop(q);
            continue;
        }
        if (e->job == job && e->launch == launch) {
            if (e->src != src || e->len != len) {
                WARN_LIMIT(4, "gpu: the copy of DMA source %llx (%llu bytes) doesn't match its launch (%llx, %llu)\n",
                           (unsigned long long)e->src, (unsigned long long)e->len, (unsigned long long)src,
                           (unsigned long long)len);
                snap_drop(q);
                break;
            }
            data = e->data;
            *gen = e->gen;
            e->data = NULL;
            g_dma.bytes -= e->len;
            q->tail++;
        }
        break;
    }
    pm_unlock(&g_dma.lk);
    return data;
}
void dma_snap_finish(u32 channel, u64 job_id) {
    DmaSnapRing* q;
    if (channel >= SNAP_CHANNELS) return;
    pm_lock(&g_dma.lk);
    q = g_dma.rings[channel];
    while (q && q->tail != q->head && q->e[q->tail % SNAP_RING].job <= job_id) snap_drop(q);
    pm_unlock(&g_dma.lk);
}
void dma_snap_reset(u32 channel) {
    DmaSnapRing* q;
    if (channel >= SNAP_CHANNELS) return;
    pm_lock(&g_dma.lk);
    q = g_dma.rings[channel];
    g_dma.rings[channel] = NULL;
    while (q && q->tail != q->head) snap_drop(q);
    pm_unlock(&g_dma.lk);
    free(q);
}

/* ------------------------------------------------------------ the launch */
/* The copy, on the GPU, its destination kept there (vk/dma.c); a render target over the destination takes the new
   bytes. from: the source's bytes as read at submission, or NULL */
static void dma_copy(const DmaOp* o, const u8* from) {
    DmaOp op = *o;
    u64 in_bytes = o->fill ? 0 : (u64)o->line_in * o->lines, out_bytes = (u64)o->line_out * o->lines;
    if (in_bytes > DMA_MAX_BYTES || out_bytes > DMA_MAX_BYTES) {
        WARN_LIMIT(4, "gpu: a DMA copy of %u lines of %u elements is over %u MiB: nothing is copied\n", o->lines,
                   o->width, DMA_MAX_BYTES >> 20);
        return;
    }
    op.snap = from;
    if (!g_renderer_up || !g_renderer->dma_copy) {
        WARN_LIMIT(4, "gpu: a DMA copy before the renderer started, skipped\n");
        return;
    }
    if (!g_renderer->dma_copy(&op)) WARN_LIMIT(4, "gpu: the GPU could not do a DMA copy, skipped\n");
    if (g_renderer->rt_reload) g_renderer->rt_reload(o->dst.addr);
}
/* The copy of a launch (the first half of Eden's MaxwellDMA::Launch): a linear source's submission-time bytes when
   the GPU hasn't written the source since, else guest memory's */
static void dma_launch_copy(GpuChannel* ch, const DmaOp* o, u32 launch) {
    u8* snap = NULL;
    if (dma_linear_source(o)) {
        u64 gen = 0, len = dma_pitch_span(&o->src, o->lines, o->line_in);
        snap = snap_take(ch->id, ch->dma_job_id, launch, o->src.addr, len, &gen);
        if (snap) {
            gmem_need(o->src.addr, len); /* pending GPU results over the source land, and count as writes */
            if (gpu_mem_dirty_since(gen, o->src.addr, len)) {
                free(snap);
                snap = NULL;
            }
        }
    }
    dma_copy(o, snap);
    free(snap);
}
/* The completion semaphore after the copy (Eden's MaxwellDMA::ReleaseSemaphore): one word, the payload, or four,
   the payload, 0 and the 64-bit timestamp, written through the release path */
static void dma_release_semaphore(const u32* r, u32 type) {
    u64 a = gpu_packed_va(r[MAXWELL_DMA_SEMAPHORE_ADDRESS_UPPER], r[MAXWELL_DMA_SEMAPHORE_ADDRESS_LOWER]);
    u8 w[16]; /* guest memory's bytes, little-endian */
    if (type != LAUNCH_DMA_RELEASE_ONE_WORD_SEMAPHORE && type != LAUNCH_DMA_RELEASE_FOUR_WORD_SEMAPHORE) return;
    snx_st_le32(w, r[MAXWELL_DMA_SEMAPHORE_PAYLOAD]);
    snx_st_le32(w + 4, 0);
    if (type == LAUNCH_DMA_RELEASE_FOUR_WORD_SEMAPHORE) snx_st_le64(w + 8, gpu_ticks());
    gpu_release_write(a, w, type == LAUNCH_DMA_RELEASE_ONE_WORD_SEMAPHORE ? 4 : 16);
}
/* LAUNCH_DMA (Eden's MaxwellDMA::Launch): the copy, when it has elements, then the completion semaphore */
static void dma_launch(GpuChannel* ch, u32 v) {
    const u32* r = ch->rdma;
    u32 launch = ch->dma_launch_index++;
    DmaOp o;
    dma_decode_launch(r, v, &o);
    if (o.width && o.lines) dma_launch_copy(ch, &o, launch);
    dma_release_semaphore(r, launch_dma_semaphore_type(v));
}
/* a copy engine method (Eden's MaxwellDMA::CallMethod): stored, and LAUNCH_DMA started */
void dma_method(GpuChannel* ch, u32 m, u32 v) {
    if (m >= MAXWELL_DMA_NUM_REGS) return;
    ch->rdma[m] = v;
    if (m == MAXWELL_DMA_LAUNCH_DMA) dma_launch(ch, v);
}

/* ------------------------------------------------------------ save states */
/* the snapshots still to be used, ring by ring in order: the jobs a load puts back in the queue take them */
int dma_snap_save(StateIo* io) {
    u32 c, i;
    int r = 0;
    pm_lock(&g_dma.lk);
    for (c = 0; c < 128 && !r; c++) {
        DmaSnapRing* q = g_dma.rings[c];
        if (!q || q->tail == q->head) continue;
        r |= io_put32(io, c + 1) | io_put32(io, q->head - q->tail);
        for (i = q->tail; i != q->head && !r; i++) {
            const DmaSnap* e = &q->e[i % SNAP_RING];
            r |= io_put64(io, e->job) | io_put64(io, e->src) | io_put64(io, e->len) | io_put32(io, e->launch);
            r |= io_put(io, e->data, e->len);
        }
    }
    r |= io_put32(io, 0);
    pm_unlock(&g_dma.lk);
    return r;
}
/* the rings of a state's snapshots, read but not yet in place */
struct DmaStaged {
    DmaSnapRing* q[128];
};
void dma_snap_staged_free(DmaStaged* d) {
    u32 c, i;
    if (!d) return;
    for (c = 0; c < 128; c++)
        if (d->q[c]) {
            for (i = d->q[c]->tail; i != d->q[c]->head; i++) free(d->q[c]->e[i % SNAP_RING].data);
            free(d->q[c]);
        }
    free(d);
}
DmaStaged* dma_snap_parse(StateIo* io) {
    DmaStaged* d = (DmaStaged*)calloc(1, sizeof *d);
    u32 c, n, i;
    if (!d) return NULL;
    for (;;) {
        DmaSnapRing* q;
        if (io_get32(io, &c)) goto bad;
        if (!c) return d;
        if (c > 128 || d->q[c - 1] || io_get32(io, &n) || n > SNAP_RING) goto bad;
        if (!(q = d->q[c - 1] = (DmaSnapRing*)calloc(1, sizeof *q))) goto bad;
        for (i = 0; i < n; i++) {
            DmaSnap* e = &q->e[i];
            if (io_get64(io, &e->job) || io_get64(io, &e->src) || io_get64(io, &e->len) || io_get32(io, &e->launch) ||
                e->len > SNAP_TOTAL || !(e->data = (u8*)malloc(e->len ? (size_t)e->len : 1)))
                goto bad;
            q->head = i + 1; /* freed with the ring from here on */
            if (io_get(io, e->data, e->len)) goto bad;
        }
    }
bad:
    dma_snap_staged_free(d);
    return NULL;
}
void dma_snap_install(DmaStaged* d) {
    u32 c, i;
    u64 gen = gpu_mem_generation(); /* the guest memory they were read from is the state's, as new as any */
    for (c = 0; c < 128; c++) dma_snap_reset(c);
    pm_lock(&g_dma.lk);
    for (c = 0; d && c < 128; c++)
        if (d->q[c]) {
            for (i = d->q[c]->tail; i != d->q[c]->head; i++) {
                d->q[c]->e[i % SNAP_RING].gen = gen;
                g_dma.bytes += d->q[c]->e[i % SNAP_RING].len;
            }
            g_dma.rings[c] = d->q[c];
            d->q[c] = NULL;
        }
    pm_unlock(&g_dma.lk);
    free(d);
}
