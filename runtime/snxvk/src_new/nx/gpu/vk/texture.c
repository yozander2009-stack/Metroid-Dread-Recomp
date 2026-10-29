/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Textures from the guest's texture headers (TIC, 32 bytes each), and samplers from its sampler headers (TSC). A
   texture whose address holds a render target is that render target (a copy of its top-left corner when the header
   is smaller). Others are loaded from guest memory, staged as they are and deswizzled and converted on the GPU
   (vk/blocklinear.c, vk/texconv.c, vk/astc.c), and checked for changes now and then: often at first, rarely once
   they have stayed the same. */
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/memory/own.h"
#include "nx/runtime/platform/tracy.h"

/* the block height and depth (log2 GOBs) of a mip level of a block-linear texture. The hardware shrinks them for
   small levels: the height from the base level on, the depth only from level 1 on and by one step a level
   (Ryujinx's SizeCalculator) */
static void bl_level_blocks(u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 level, u32* lbh, u32* lbd) {
    u32 lh = h >> level ? h >> level : 1, gy = ((lh + f->bh - 1) / f->bh + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    for (u32 l = 1; l <= level; l++) {
        u32 ld = d >> l ? d >> l : 1;
        if (bd > 0 && ld <= (1u << (bd - 1))) bd--;
    }
    *lbh = bh;
    *lbd = bd;
}
/* bytes of one mip level of a block-linear texture; lbh and lbd (may be NULL) get its block height and depth */
static u64 bl_level_size(u32 w, u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 level, u32* lbh, u32* lbd) {
    u32 lw = w >> level ? w >> level : 1, lh = h >> level ? h >> level : 1, ld = d >> level ? d >> level : 1;
    u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
    u32 gx = (tw * f->bpp + 63) / 64, gy = (th + 7) / 8, b, bdd;
    bl_level_blocks(h, d, f, bh, bd, level, &b, &bdd);
    if (lbh) *lbh = b;
    if (lbd) *lbd = bdd;
    u64 gys = ((gy + (1u << b) - 1) >> b) << b;
    u64 lds = ((ld + (1u << bdd) - 1) >> bdd) << bdd;
    return (u64)gx * gys * lds * 512;
}
/* bytes from one layer of a block-linear texture to the next: all its levels, aligned to a block of GOBs that is
   shrunk for a small base level (Ryujinx's AlignLayerSize) */
static u64 bl_layer_size(u32 w, u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 levels) {
    /* the sizes asked last, by shape: every lookup of a texture asks this, and it costs divisions by the block size
       over each level, the levels' block sizes each a loop of its own */
    static struct {
        u32 w, h, d, bh, bd, levels, fbw, fbh, fbpp;
        u64 sz;
    } memo[256];
    u32 m = (u32)(((((u64)w * 0x9E3779B1u) ^ h) * 0x85EBCA77u ^ ((u64)d << 20 | (u64)levels << 14 | bh << 8 | bd << 4) ^
                   (u64)f->bpp << 24 ^ (u64)f->bw << 28 ^ (u64)f->bh << 32) *
                      0x9E3779B97F4A7C15ull >>
                  56);
    if (memo[m].w == w && memo[m].h == h && memo[m].d == d && memo[m].bh == bh && memo[m].bd == bd &&
        memo[m].levels == levels && memo[m].fbw == f->bw && memo[m].fbh == f->bh && memo[m].fbpp == f->bpp && w)
        return memo[m].sz;
    u32 w0 = w, h0 = h, d0 = d, bh0 = bh, bd0 = bd;
    u64 sz = 0;
    for (u32 l = 0; l < levels; l++) sz += bl_level_size(w, h, d, f, bh, bd, l, NULL, NULL);
    u32 gy = ((h + f->bh - 1) / f->bh + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    while (bd > 0 && d <= (1u << (bd - 1))) bd--;
    u64 al = 512ull << (bh + bd);
    sz = (sz + al - 1) / al * al;
    memo[m].w = w0, memo[m].h = h0, memo[m].d = d0, memo[m].bh = bh0, memo[m].bd = bd0, memo[m].levels = levels;
    memo[m].fbw = f->bw, memo[m].fbh = f->bh, memo[m].fbpp = f->bpp, memo[m].sz = sz;
    return sz;
}

/* the guest bytes a move of a level reads (lsz bytes at gva) staged as they are (what the GPU holds of them copied on
   the GPU, vk_guest_read), zero past the level's end: buf and off hold the layout's first byte the move reads
   (vk_bl_range) */
static int stage_bl(const VkBlMove* m, u64 gva, u64 lsz, VkBuffer* buf, u64* off) {
    u64 lo, hi;
    vk_bl_range(m, &lo, &hi);
    u8* p = vk_stage(hi - lo, vk_stage_align(), buf, off);
    if (!p) return 0;
    u64 n = lo >= lsz ? 0 : hi <= lsz ? hi - lo : lsz - lo;
    vk_guest_read(gva + lo, n, p, *buf, *off);
    memset(p + n, 0, hi - lo - n);
    return 1;
}

/* one level of one layer in guest memory, for vk_astc_upload to ask for its block rows */
typedef struct {
    Surf* t;
    u64 gva;
    u64 lsz;
    u32 rowb, th, lbh, lbd;
} LevelSource;
/* A guest read while staging lands GPU results, and landing can submit the frame's command buffer: the upload's
   commands then belong to the one recording after it, with the image's transfer use established there again */
static VkCommandBuffer upload_cb(Surf* t, VkCommandBuffer cb) {
    VkCommandBuffer now = vk_cmd();
    if (now == cb) return cb;
    vk_surf_use(now, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    return now;
}
/* block linear blocks staged as they are and deswizzled on the GPU into the scratch buffer; pitch linear ones staged
   a row at a time */
static int astc_rows(void* ctx, VkCommandBuffer* pcb, u32 z, u32 nz, u32 y, u32 n, VkBuffer* buf, u64* off) {
    LevelSource* l = ctx;
    if (l->t->bl) {
        VkBlMove m = {l->rowb, l->th, l->lbh, l->lbd, y, n, z, nz, l->rowb}; /* 16 byte blocks: rows of whole runs */
        VkBuffer staged;
        u64 at;
        u64 cap;
        *buf = vk_bl_scratch((u64)l->rowb * n * nz, &cap);
        *off = 0;
        if (!*buf || !stage_bl(&m, l->gva, l->lsz, &staged, &at)) return 0;
        VkCommandBuffer cb = *pcb = upload_cb(l->t, *pcb);
        vk_bl_barrier(cb, 1);
        int ok = vk_bl_move(cb, &m, 0, staged, at, *buf, 0);
        vk_bl_barrier(cb, 0);
        return ok;
    }
    u8* p = vk_stage((u64)l->rowb * n * nz, vk_stage_align(), buf, off);
    if (!p) return 0;
    for (u32 k = 0; k < nz; k++)
        for (u32 r = 0; r < n; r++) {
            u64 at = ((u64)(z + k) * l->th + y + r) * l->t->pitch;
            u64 to = ((u64)k * n + r) * l->rowb;
            vk_guest_read(l->gva + at, l->rowb, p + to, *buf, *off + to);
        }
    *pcb = upload_cb(l->t, *pcb);
    return 1;
}

/* A texture's upload, recorded in batches that share their barriers: a batch's block linear strips are deswizzled
   into the scratch buffer one after another, then the strips of a format the host can't take as it is are converted
   there too, then every strip is copied into the image. The CPU only copies guest memory into staging, in whole
   ranges: GOBs and rows are never taken apart on it */
typedef struct {
    VkBuffer in;
    u64 in_at, out_at;
    u32 in_pitch, w, rows, slices;
} UpConv;
typedef struct {
    VkCommandBuffer cb;
    Surf* t;
    VkBuffer scratch;
    u64 cap, used;
    int open;        /* the barrier before the batch's dispatches is recorded */
    u32 moves;       /* deswizzles recorded in the batch */
    UpConv conv[32]; /* conversions, recorded once the moves they read are done */
    u32 nconv;
    VkBufferImageCopy reg[64];
    VkBuffer src[64]; /* each copy's buffer: the scratch buffer, or staging */
    u32 nreg;
} Upload;
/* the buffer a batch records into now: staging can submit, which ends the one the batch was opened on */
static VkCommandBuffer up_cb(Upload* u) {
    VkCommandBuffer cb = upload_cb(u->t, u->cb);
    if (cb != u->cb) {
        u->cb = cb;
        u->open = 0; /* the batch's barriers ended with the buffer that held them */
    }
    return cb;
}
static void up_open(Upload* u) {
    up_cb(u);
    if (!u->open) vk_bl_barrier(u->cb, 1);
    u->open = 1;
}
static int up_flush(Upload* u) {
    int ok = 1;
    up_cb(u);
    if (u->moves) vk_bl_barrier(u->cb, 0);
    if (u->nconv) {
        up_open(u);
        for (u32 i = 0; i < u->nconv && ok; i++)
            ok = vk_tc_convert(u->cb, &u->t->f, u->conv[i].w, u->conv[i].rows, u->conv[i].slices, u->conv[i].in,
                               u->conv[i].in_at, u->conv[i].in_pitch, u->scratch, u->conv[i].out_at);
        vk_bl_barrier(u->cb, 0);
    }
    for (u32 i = 0, j; i < u->nreg; i = j) {
        for (j = i + 1; j < u->nreg && u->src[j] == u->src[i];) j++;
        rec_CmdCopyBufferToImage(u->cb, u->src[i], u->t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, j - i, u->reg + i);
    }
    u->open = 0;
    u->moves = u->nconv = u->nreg = 0;
    u->used = 0;
    return ok;
}
/* room in the batch for a strip: size bytes of the scratch buffer (its pieces each aligned to 16 or less), two
   copies and a conversion. The batch is flushed first when they don't fit, and the scratch buffer grown when even an
   empty one wouldn't hold them, so the strip's pieces stay in one batch and one buffer. 0 when memory ran out */
static int up_reserve(Upload* u, u64 size) {
    if ((!size || u->used + 64 + size <= u->cap) && u->nreg + 2 <= sizeof u->reg / sizeof u->reg[0] &&
        u->nconv < sizeof u->conv / sizeof u->conv[0])
        return 1;
    if (!up_flush(u)) return 0;
    return !size || size + 64 <= u->cap || (u->scratch = vk_bl_scratch(size + 64, &u->cap)) != VK_NULL_HANDLE;
}
/* size bytes of the scratch buffer at a multiple of align, within what up_reserve made room for */
static u64 up_space(Upload* u, u64 size, u32 align) {
    u64 at = (u->used + align - 1) / align * align;
    u->used = at + size;
    return at;
}
/* the least common multiple of 16 and a texel's (or block's) bytes */
static u32 bl_lin_align(u32 bpp) {
    u32 a = 16;
    while (a % bpp) a += 16;
    return a;
}

/* one level of one layer, what tex_upload's strips of it share */
typedef struct {
    u32 level, layer, lw, lh, ld, rowb, th;
    u32 conv;     /* converted on the GPU (vk_tc_converts) */
    VkTcShape tc; /* the conversion's shape */
    u32 pitch;    /* the linear rows' pitch: deswizzled, or repacked from a pitch linear level */
} UpLevel;
/* units rows y to y + n (rows of texels, or of blocks) of slices z to z + nz, in linear rows in_pitch bytes apart at
   in_at in in (staging, or the scratch buffer once the batch's moves are done), converted when they need to be and
   copied into the image. The caller has reserved the room (up_reserve) */
static int up_rows(Upload* u, const UpLevel* lv, VkBuffer in, u64 in_at, u32 in_pitch, u32 z, u32 nz, u32 y, u32 n) {
    const VkFmt* f = &u->t->f;
    u32 top = y * f->bh, height = lv->lh - top < n * f->bh ? lv->lh - top : n * f->bh;
    VkBufferImageCopy r = {.bufferRowLength = in_pitch / f->bpp * f->bw,
                           .bufferImageHeight = n * f->bh,
                           .imageSubresource = {f->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                                                lv->level, lv->layer, 1},
                           .imageOffset = {0, (i32)top, (i32)z},
                           .imageExtent = {lv->lw, height, nz}};
    if (!lv->conv) {
        r.bufferOffset = in_at;
        u->src[u->nreg] = in;
        u->reg[u->nreg++] = r;
        return 1;
    }
    u64 out = up_space(u, vk_tc_out_size(&lv->tc, n, nz), 16);
    u->conv[u->nconv++] = (UpConv){in, in_at, out, in_pitch, lv->lw, n, nz};
    r.bufferOffset = out;
    r.bufferRowLength = lv->tc.out_texels;
    u->src[u->nreg] = u->scratch;
    u->reg[u->nreg++] = r;
    if (lv->tc.stencil) {
        r.bufferOffset = out + (u64)lv->tc.out_texels * n * nz * 4;
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        u->src[u->nreg] = u->scratch;
        u->reg[u->nreg++] = r;
    }
    return 1;
}
/* a block linear strip: move m of bytes staged at staged_at deswizzled into the scratch buffer, then up_rows. Its
   rows land at row y, slice z of the level (the move's own rows and slices are a gathered layout's for a strip of
   GOB rows) */
static int up_move(Upload* u, const UpLevel* lv, const VkBlMove* m, VkBuffer staged, u64 staged_at, u32 z, u32 y) {
    u64 lin = (u64)m->slices * m->rows * m->lin_pitch;
    if (!up_reserve(u, lin + (lv->conv ? vk_tc_out_size(&lv->tc, m->rows, m->slices) : 0))) return 0;
    u64 at = up_space(u, lin, bl_lin_align(u->t->f.bpp));
    up_open(u);
    if (!vk_bl_move(u->cb, m, 0, staged, staged_at, u->scratch, at)) return 0;
    u->moves++;
    return up_rows(u, lv, u->scratch, at, m->lin_pitch, z, m->slices, y, m->rows);
}

/* a block linear level: its guest bytes staged as they are and deswizzled on the GPU. The whole level at once when
   it fits in band; else strips of whole rows of blocks of 2^lbd slices, contiguous in memory; else, when one of those
   is too big, strips of 2^k GOB rows of a slice, each block's piece of them (contiguous in it) staged after the last
   one's: the layout of a surface of one row of blocks 2^k GOBs high */
static int level_upload_bl(Upload* u, const UpLevel* lv, u64 gva, u64 lsz, u32 lbh, u32 lbd, u64 band) {
    u32 rowb = lv->rowb, th = lv->th, ld = lv->ld, pitch = lv->pitch;
    u32 gobs_x = (rowb + 63) >> 6, nz = ld < (1u << lbd) ? ld : 1u << lbd;
    u64 block = 512ull << (lbh + lbd), block_row = gobs_x * block;
    /* scratch bytes a row of units takes: deswizzled, and converted */
    u64 per_row = pitch + (lv->conv ? vk_tc_out_size(&lv->tc, 1, 1) : 0);
    VkBuffer staged;
    u64 staged_at;
    if (per_row * th * ld <= band && lsz <= band) {
        VkBlMove m = {rowb, th, lbh, lbd, 0, th, 0, ld, pitch};
        return stage_bl(&m, gva, lsz, &staged, &staged_at) && up_move(u, lv, &m, staged, staged_at, 0, 0);
    }
    u64 rows_cost = per_row * (8u << lbh) * nz;
    if (rows_cost <= band && block_row <= band) {
        u64 k = band / (rows_cost > block_row ? rows_cost : block_row);
        u32 n = (u32)k * (8u << lbh);
        for (u32 z = 0; z < ld; z += nz)
            for (u32 y = 0; y < th; y += n) {
                VkBlMove m = {rowb, th, lbh, lbd, y, th - y < n ? th - y : n, z, ld - z < nz ? ld - z : nz, pitch};
                if (!stage_bl(&m, gva, lsz, &staged, &staged_at) ||
                    !up_move(u, lv, &m, staged, staged_at, z, y))
                    return 0;
            }
        return 1;
    }
    u32 k = lbh;
    while (k && (per_row * (8u << k) > band || (u64)gobs_x * (512u << k) > band)) k--;
    if (per_row * 8 > band || (u64)gobs_x * 512 > band) {
        LOG_LIMIT(1, "vulkan: a %ux%u texture level's rows exceed the staging memory (left out)\n", lv->lw, lv->lh);
        return 0;
    }
    u64 rows_blocks = ((((u64)th + 7) >> 3) + (1u << lbh) - 1) >> lbh, piece = 512ull << k;
    for (u32 z = 0; z < ld; z++)
        for (u32 gy = 0; gy < (th + 7) >> 3; gy += 1u << k) {
            u8* p = vk_stage(gobs_x * piece, vk_stage_align(), &staged, &staged_at);
            if (!p) return 0;
            u64 base = (z >> lbd) * rows_blocks * block_row + (gy >> lbh) * block_row +
                       ((((u64)(z & ((1u << lbd) - 1)) << lbh) + (gy & ((1u << lbh) - 1))) << 9);
            for (u32 gx = 0; gx < gobs_x; gx++) {
                u64 o = base + gx * block, n = o >= lsz ? 0 : o + piece <= lsz ? piece : lsz - o;
                vk_guest_read(gva + o, n, p + gx * piece, staged, staged_at + gx * piece);
                memset(p + gx * piece + n, 0, piece - n);
            }
            u32 y = gy << 3;
            VkBlMove m = {rowb, 8u << k, k, 0, 0, th - y < (8u << k) ? th - y : 8u << k, 0, 1, pitch};
            if (!up_move(u, lv, &m, staged, staged_at, z, y)) return 0;
        }
    return 1;
}

/* a pitch linear level: its rows staged as they are, whole slices or strips of rows of a slice, when the guest's
   pitch suits the copies and the conversion; else each row into rows of lv->pitch */
static int level_upload_pitch(Upload* u, const UpLevel* lv, u64 gva, u64 lsz, u64 band) {
    const Surf* t = u->t;
    u32 th = lv->th, ld = lv->ld, gp = t->pitch;
    int as_is = gp % (lv->conv ? lv->tc.in_align : 4) == 0 && gp % t->f.bpp == 0;
    u32 pitch = as_is ? gp : lv->pitch;
    u64 out_row = lv->conv ? vk_tc_out_size(&lv->tc, 1, 1) : 0, per_row = pitch > out_row ? pitch : out_row;
    u32 n = per_row * th * ld <= band ? th : (u32)(band / per_row ? band / per_row : 1);
    u32 nz = n == th ? ld : 1;
    if (n > th) n = th;
    for (u32 z = 0; z < ld; z += nz)
        for (u32 y = 0; y < th; y += n) {
            u32 rows = th - y < n ? th - y : n, slices = ld - z < nz ? ld - z : nz;
            VkBuffer staged;
            u64 at, first = ((u64)z * th + y) * gp, size = (u64)rows * slices * pitch;
            u8* p = vk_stage(size, vk_stage_align(), &staged, &at);
            if (!p) return 0;
            if (as_is) { /* the rows, slices after another, are one range of guest memory */
                u64 have = first >= lsz ? 0 : first + size <= lsz ? size : lsz - first;
                vk_guest_read(gva + first, have, p, staged, at);
                memset(p + have, 0, size - have);
            } else
                for (u32 k = 0; k < slices; k++)
                    for (u32 r = 0; r < rows; r++) {
                        u64 o = ((u64)(z + k) * th + y + r) * gp;
                        u64 to = ((u64)k * rows + r) * pitch;
                        memset(p + to, 0, pitch);
                        vk_guest_read(gva + o, lv->rowb, p + to, staged, at + to);
                    }
            if (!up_reserve(u, lv->conv ? vk_tc_out_size(&lv->tc, rows, slices) : 0) ||
                !up_rows(u, lv, staged, at, pitch, z, slices, y, rows))
                return 0;
        }
    return 1;
}

/* the most staging one piece of a level takes: a bigger level is staged in strips, as no allocation may be bigger
   than g_vk.alloc_cap (memory.c) */
#define TEX_BAND (64ull << 20)

static u64 g_tex_uploads, g_tex_upload_bytes; /* for the stats line */
void vk_tex_stats(u64* uploads, u64* bytes) {
    *uploads = g_tex_uploads;
    *bytes = g_tex_upload_bytes;
}
/* 1 when every subresource was copied; 0 when memory or staging ran out, and upload_valid keeps it for a retry */
static int tex_upload_(Surf* t);
/* a texture's upload as a profiler zone (SNX_TRACY builds) */
static int tex_upload(Surf* t) {
    SNX_ZONE_V(z, "texture upload", t->size);
    int r = tex_upload_(t);
    SNX_ZONE_END(z);
    return r;
}
static int tex_upload_(Surf* t) {
    const VkFmt* f = &t->f;
    SNX_ZONE_T(z_up, "texture upload", "%ux%u", t->w, t->h);
    SNX_ZONE_VALUE(z_up, t->size);
    g_tex_uploads++;
    g_tex_upload_bytes += t->size;
    t->upload_valid = 0;
    t->wgen = vk_surf_gen();      /* even a new texture at a reused address invalidates an old target's copy */
    t->took_drawn = 0;            /* memory's own texels again, until tex_take_drawn copies targets in */
    vk_surf_relist(t);
    vk_render_end();              /* copies and clears happen outside rendering passes */
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int is3d = t->type == VK_IMAGE_VIEW_TYPE_3D;
    u32 layers = t->layers, d3 = is3d ? t->depth3d : 1;
    u64 band = g_vk.alloc_cap < TEX_BAND ? g_vk.alloc_cap : TEX_BAND;
    int complete = 1;
    Upload u = {.cb = cb, .t = t};
    for (u32 layer = 0; layer < layers && complete; layer++) {
        u64 at = layer * t->layer_size;
        for (u32 l = 0; l < t->levels && complete; l++) {
            u32 lw = t->w >> l, lh = t->h >> l, ld = d3 >> l;
            if (!lw) lw = 1;
            if (!lh) lh = 1;
            if (!ld) ld = 1;
            u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
            u32 rowb = tw * f->bpp, lbh = 0, lbd = 0;
            u64 lsz = t->bl ? bl_level_size(t->w, t->h, d3, f, t->bh, t->bd, l, &lbh, &lbd) : (u64)t->pitch * th * ld;
            if (f->conv == VKC_ASTC) { /* decoded on the GPU from its block rows, deswizzled on the GPU */
                complete = up_flush(&u);
                LevelSource ls = {t, t->gva + at, lsz, rowb, th, lbh, lbd};
                complete = complete && vk_astc_upload(u.cb, t, lw, lh, ld, l, layer, astc_rows, &ls);
                u.scratch = VK_NULL_HANDLE; /* astc_rows may have grown the scratch buffer: asked for again */
                u.cap = 0;
            } else {
                UpLevel lv = {l, layer, lw, lh, ld, rowb, th, (u32)vk_tc_converts(f)};
                u32 align = bl_lin_align(f->bpp);
                if (lv.conv) {
                    lv.tc = vk_tc_shape(f, lw);
                    if (lv.tc.in_align > align) align = lv.tc.in_align;
                }
                lv.pitch = (rowb + align - 1) / align * align;
                complete = t->bl ? level_upload_bl(&u, &lv, t->gva + at, lsz, lbh, lbd, band)
                                 : level_upload_pitch(&u, &lv, t->gva + at, lsz, band);
            }
            if (!complete)
                LOG_LIMIT(1, "vulkan: out of memory or staging uploading a %ux%u texture level (left out)\n", lw, lh);
            at += lsz;
        }
    }
    complete = up_flush(&u) && complete;
    return t->upload_valid = complete;
}

/* change checks: a hash of the whole texture, 4 KiB at a time, the same whichever way its bytes are reached (the
   sampled check for big long-stable textures is vk_sample_hash) */
#define MEM_HASH_PIECE 4096u
static u64 mem_hash(u64 gva, u64 size) {
    u64 piece[MEM_HASH_PIECE / 8];
    u64 h = 1469598103934665603ull;
    /* bytes the GPU holds are trapped: they hash as their owner (vk_guest_peek), so the texture is read in pieces.
       Every byte counts: the CPU rewrites part of a page (a row of a texture, half a GOB's rows) */
    u8* p = __atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE) ? NULL : gmem_ptr(gva, size);
    for (u64 o = 0; o < size; o += MEM_HASH_PIECE) {
        u64 n = size - o < MEM_HASH_PIECE ? size - o : MEM_HASH_PIECE;
        if (!p) vk_guest_peek(gva + o, piece, n);
        h = snx_fnv_step(h, vk_hash(p ? p + o : (const u8*)piece, n));
    }
    return h;
}
/* the texture's memory hashed again, and loaded again when that changed: 1 if it did */
static int tex_rehash(Surf* t) {
    t->gw = gpu_mem_generation();
    u64 hh = mem_hash(t->gva, t->size);
    if (hh == t->hash && t->upload_valid) return 0;
    if (tex_upload(t)) t->hash = hh; /* an incomplete upload keeps the old hash, so the next check retries it */
    return 1;
}

void vk_tex_note_writeback(Surf* t) {
    t->hash = mem_hash(t->gva, t->size);
    t->gw = gpu_mem_generation();
    t->wseen = vk_ww_now();
    t->checked = g_vk_present;
    t->stable = 0;
}

static int format_samples(VkFormat f) {
    static VkFormat ok[64], bad[64];
    static int nok, nbad;
    for (int i = 0; i < nok; i++)
        if (ok[i] == f) return 1;
    for (int i = 0; i < nbad; i++)
        if (bad[i] == f) return 0;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    int r = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    if (r && nok < 64) ok[nok++] = f;
    if (!r && nbad < 64) {
        bad[nbad++] = f;
        WARN("vulkan: the GPU can't sample format %d (textures in it aren't drawn)\n", (int)f);
    }
    return r;
}

static Surf* tex_create(u64 gva, u32 w, u32 h, u32 layers, u32 d3, u32 levels, VkImageViewType type, VkFmt f, u32 fkey,
                        u32 bl, u32 bh, u32 bd, u32 pitch, u64 layer_size) {
    Surf* t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->gva = gva;
    t->w = t->sw = w;
    t->h = t->sh = h;
    t->layers = layers;
    t->depth3d = d3;
    t->levels = levels;
    t->type = type;
    t->f = f;
    t->fkey = fkey;
    t->bl = bl;
    t->bh = bh;
    t->bd = bd;
    t->pitch = pitch;
    t->layer_size = layer_size;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                            .format = f.vk,
                            .extent = {w, h, type == VK_IMAGE_VIEW_TYPE_3D ? d3 : 1},
                            .mipLevels = levels,
                            .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkFormatProperties format_props;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f.vk, &format_props);
    if (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
        ii.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        t->storage_capable = 1;
    }
    if (type != VK_IMAGE_VIEW_TYPE_3D && layers >= 6 && w == h) ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkViewFormats vf;
    vk_view_formats(&ii, &vf);
    /* room is made for it under the budget: the caller holds only surfaces it used this present, which stay */
    if (!vk_cache_image_create(&ii, &t->img, &t->mem, &t->bytes)) {
        free(t);
        return NULL;
    }
    vk_surf_add(t);
    return t;
}

/* a render target read through a smaller header or a feedback draw: a copy refreshed when the target is drawn to. One
   copy per size, in a list through next: one draw can hold copies of a target at two sizes (two headers, or two
   stages), so a copy isn't replaced while a draw may hold it. Copies not read for a second go; their views and image
   when the frame being recorded is done, as descriptors may still name them */
/* the target's top-left w x h copied out, in the target's format, or in as: another color format of the same texel
   size, the target's bytes read as that format (a bit copy, which vkCmdCopyImage makes between such formats) */
static Surf* rt_crop(Surf* rt, u32 w, u32 h, const VkFmt* as) {
    Surf* c = NULL;
    VkFmt f = as ? *as : rt->f;
    for (Surf** pp = &rt->crop; *pp;) {
        Surf* x = *pp;
        if (x->w == w && x->h == h && x->f.vk == f.vk && !x->pre_lv)
            c = x;
        else if ((i32)(g_vk_present - (u32)x->last_used) > 60) {
            *pp = x->next;
            for (int i = 0; i < x->nsv; i++) vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, x->sv[i].v, VK_NULL_HANDLE);
            vk_defer_image(x->img, x->mem, VK_NULL_HANDLE, VK_NULL_HANDLE);
            vk_defer_bytes(x->bytes);
            free(x);
            continue;
        }
        pp = &x->next;
    }
    if (!c) {
        c = calloc(1, sizeof *c);
        if (!c) return rt;
        c->gva = rt->gva;
        c->w = w;
        c->h = h;
        c->sw = vk_sc(w) < rt->sw ? vk_sc(w) : rt->sw; /* the same region of the scaled target */
        c->sh = vk_sc(h) < rt->sh ? vk_sc(h) : rt->sh;
        c->layers = rt->layers;
        c->levels = c->depth3d = 1;
        c->f = f;
        c->fkey = rt->fkey;
        c->type = rt->type;
        c->is_rt = 1; /* sampled like one; not in the table, so never found as one */
        c->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = f.vk,
                                .extent = {c->sw, c->sh, 1},
                                .mipLevels = 1,
                                .arrayLayers = rt->layers,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkViewFormats vf;
        vk_view_formats(&ii, &vf);
        if (!vk_cache_image_create(&ii, &c->img, &c->mem, &c->bytes)) { /* rt was used now: it stays */
            free(c);
            return rt;
        }
        c->crop_gen = ~0u;
        c->next = rt->crop;
        rt->crop = c;
        if (w != rt->w || h != rt->h)
            LOG_LIMIT(10, "vulkan: %ux%u texture reads the %ux%u render target at %llx: only that region is used\n", w,
                      h, rt->w, rt->h, (unsigned long long)rt->gva);
    }
    c->last_used = g_vk_present;
    if (c->crop_gen != rt->wgen) {
        VkCommandBuffer cb = vk_cmd();
        VkImageCopy r[2];
        vk_surf_use(cb, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        vk_surf_use(cb, c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        /* VkImageCopy takes one aspect per region: depth and stencil are two (a view samples one of them) */
        VkImageAspectFlags a = rt->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        memset(r, 0, sizeof r);
        r[0].srcSubresource.aspectMask = a;
        r[0].srcSubresource.layerCount = rt->layers;
        r[0].dstSubresource.aspectMask = a;
        r[0].dstSubresource.layerCount = rt->layers;
        r[0].extent.width = c->sw;
        r[0].extent.height = c->sh;
        r[0].extent.depth = 1;
        r[1] = r[0];
        r[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        r[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        rec_CmdCopyImage(cb, rt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       rt->f.stencil ? 2 : 1, r);
        c->crop_gen = rt->wgen;
    }
    return c;
}

Surf* vk_rt_snapshot(Surf* rt) {
    if (!rt || rt->layout == VK_IMAGE_LAYOUT_UNDEFINED) return rt;
    return rt_crop(rt, rt->w, rt->h, NULL);
}

/* SWR: resolution scale and the game's glow. Dread shrinks the scene into its bloom chain with a few taps per output
   pixel, placed for the guest resolution: at 2x those taps meet 1 rendered pixel in 4, at 4x 1 in 16, so bright small
   things came out as dotted, blocky glow (GitHub issue #17; the OpenGL renderer had the same until gpu_gl.c
   rt_prefiltered). A filtered-only read (ShTex.use 1) of a scaled target by a draw at least 2x smaller, or of a small
   glow buffer (480x270 and under, guest size), gets the target averaged down by 2^lv (lv: 1 at 2x, 2 at 4x), so its
   taps see the averages they see at 1x. Each level is a crop of the target (freed with it, dropped unused like the
   others), made from the level before by a 2:1 linear blit: a 2x2 box. SWR_NO_RT_PREFILTER=1 = off */
static int can_blit(VkFormat f);
static Surf* pre_level(Surf* t, u32 k) {
    if (!k) return t;
    Surf* c = NULL;
    for (Surf* x = t->crop; x && !c; x = x->next)
        if (x->pre_lv == k) c = x;
    if (!c) {
        c = calloc(1, sizeof *c);
        if (!c) return NULL;
        c->gva = t->gva;
        c->sw = t->sw >> k ? t->sw >> k : 1;
        c->sh = t->sh >> k ? t->sh >> k : 1;
        c->w = c->sw;
        c->h = c->sh;
        c->layers = c->levels = c->depth3d = 1;
        c->f = t->f;
        c->fkey = t->fkey;
        c->type = t->type;
        c->is_rt = 1; /* sampled like one; not in the table, so never found as one */
        c->pre_lv = k;
        c->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = t->f.vk,
                                .extent = {c->sw, c->sh, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkViewFormats vf;
        vk_view_formats(&ii, &vf);
        if (!vk_cache_image_create(&ii, &c->img, &c->mem, &c->bytes)) {
            free(c);
            return NULL;
        }
        c->crop_gen = ~0u;
        c->next = t->crop;
        t->crop = c;
        LOG_LIMIT(8, "vulkan: the %ux%u target at %llx gets a copy averaged to %ux%u for the glow passes\n", t->sw, t->sh,
                  (unsigned long long)t->gva, c->sw, c->sh);
    }
    c->last_used = g_vk_present;
    if (c->crop_gen != t->wgen) {
        Surf* from = pre_level(t, k - 1);
        if (!from) return NULL;
        VkCommandBuffer cb = vk_cmd();
        vk_surf_use(cb, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        vk_surf_use(cb, c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .srcOffsets = {{0, 0, 0}, {(i32)from->sw, (i32)from->sh, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .dstOffsets = {{0, 0, 0}, {(i32)c->sw, (i32)c->sh, 1}}};
        rec_CmdBlitImage(cb, from->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &b, VK_FILTER_LINEAR);
        c->crop_gen = t->wgen;
    }
    return c;
}
Surf* vk_rt_prefiltered(Surf* t, const ShTex* st, VkImageViewType want, int num, Surf** rt, u32 n) {
    static int off = -1;
    if (off < 0) off = getenv("SWR_NO_RT_PREFILTER") != NULL;
    if (off || g_vk_scale < 1.99f || !t || !t->is_rt || t->pre_lv || t->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
        want != VK_IMAGE_VIEW_TYPE_2D || num || t->f.depth || t->f.is_int || t->layers != 1 || st->use != 1 ||
        st->type != 1 || st->array || st->shadow || !n || !rt[0] || rt[0] == t)
        return t;
    const Surf* o = rt[0];
    if (!(t->w >= 2 * o->w && t->h >= 2 * o->h) && !(t->w <= 480 && t->h <= 270)) return t;
    u32 lv = 0;
    for (float k = 2.0f; k <= g_vk_scale + 0.01f; k *= 2.0f) lv++;
    if (!lv || !can_blit(t->f.vk)) return t;
    Surf* c = pre_level(t, lv);
    return c ? c : t;
}

/* texture header type -> what the image is created as */
static VkImageViewType tic_view_type(u32 type, u32 d) {
    switch (type) {
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return VK_IMAGE_VIEW_TYPE_CUBE;
        case 8: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case 4:
        case 5: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case 1:
        case 7: return d > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
    return VK_IMAGE_VIEW_TYPE_2D; /* 1D textures are 2D images one texel high */
}

static u32 g_bud_frame;
static i64 g_bud;
/* a texture that stayed the same over its last 40 frames' checks: the schedule below checks it by samples */
static int tex_settled(const Surf* t) { return t->stable >= 40 && t->size >= 16384; }
/* a whole-texture check's outcome (tex_rehash's) in its stability: a change starts it over, an unchanged texture
   counts once a frame (count: its first check in this one). Settling, it takes its samples' hash */
static void tex_settle(Surf* t, u64 gva, int changed, int count) {
    if (changed)
        t->stable = 0;
    else if (count && t->stable < 100000)
        t->stable++;
    if (count && t->stable >= 39 && t->size >= 16384) {
        t->shash = vk_sample_hash(gva, t->size);
        t->tfull = g_vk_present;
    }
}
/* t, a texture found for a header: still as guest memory has it (re-uploaded where it changed), or NULL when its
   upload failed. gva is its address */
static Surf* tex_check(Surf* t, u64 gva) {
    vk_surf_touch(t);
    /* an upload that ran out of memory or staging (here, or a re-upload below) is tried again once a frame. A
       texture is only returned whole */
    if (!t->upload_valid) {
        if (t->checked != g_vk_present) {
            t->checked = g_vk_present;
            tex_rehash(t);
        }
        if (!t->upload_valid) return NULL;
    }
    /* the GPU front end wrote its memory (DMA, I2M, 2D, compute results): checked now, whatever the schedule
       below says, as writes within a frame have to show in the same frame. Not written: it still matches as of
       now, so the next check asks only about later writes */
    int fresh = 0;
    if (gpu_mem_dirty_since(t->gw, gva, t->size)) {
        fresh = 1;
        if (tex_rehash(t)) t->stable = 0;
    } else
        t->gw = gpu_mem_generation();
    /* the CPU's writes, once a frame and again after each submission or stream wait that can make new ones the
       stream's to see (vk_ww_epoch): write tracking proves the memory unchanged without hashing its texels */
    if (t->checked != g_vk_present || t->ww_epoch != vk_ww_epoch()) {
        u32 wnow = vk_ww_now();
        int wr = vk_written_since(gmem_host(gva, t->size), t->size, t->wseen);
        int epoch = t->ww_epoch != vk_ww_epoch();
        t->ww_epoch = vk_ww_epoch();
        if (wr >= 0) {
            t->checked = g_vk_present;
            /* Written: hashed again after the check took in the scans that found the writes, so the new contents hold
               what their marks (the clock as it is now, at most) stand for. A rehash for the front end's writes came
               before those scans, and is done again. Unchanged, the texture keeps its baseline: the clock before the
               check plus one is what the next scans taken in are marked with, and the texture took their writes as
               seen (one game's glyph cache missed the glyphs its CPU drew after a check that marked nothing) */
            if (wr) {
                t->wseen = wnow;
                tex_rehash(t);
            }
            return t->upload_valid ? t : NULL;
        }
        /* without it, hashed whole once in each epoch it is used in until it settles: what the CPU wrote before a
           submission or the end of a stream wait reaches the next sample, as with write tracking. The frame's schedule
           below alone misses that (the write and the sample come between two presents, and a headless run presents
           nothing). Expiring every texture at the cache invalidation methods did it before, and isn't enough either:
           edenprobe rewrites texels with only the sampler and header caches invalidated after, never the texture data
           cache. A settled texture is left to the schedule: epochs start at every submission, and hashing every
           texture in each took half the GPU thread (FreeBSD) */
        if (epoch && !tex_settled(t)) {
            int first = t->checked != g_vk_present;
            t->checked = g_vk_present;
            if (!fresh) tex_settle(t, gva, tex_rehash(t), first);
            return t->upload_valid ? t : NULL;
        }
    }
    /* without it: often at first, backing off while the texture stays the same, within a per-frame budget */
    if (t->checked != g_vk_present && (i32)(g_vk_present - t->next_check) >= 0) {
        if (g_bud_frame != g_vk_present) {
            g_bud_frame = g_vk_present;
            g_bud = 24 << 20;
        }
        if (tex_settled(t) && (i32)(g_vk_present - t->tfull) < 120) {
            t->checked = g_vk_present;
            if (vk_sample_hash(gva, t->size) == t->shash) {
                t->next_check = g_vk_present + 20 + ((u32)(gva >> 9) & 7);
                return t->upload_valid ? t : NULL;
            }
            t->stable = 0;
        }
        if (g_bud > 0 || t->stable < 4) {
            t->checked = g_vk_present;
            g_bud -= (i64)(t->size >> (t->size > 65536 ? 1 : 0));
            tex_settle(t, gva, tex_rehash(t), 1);
            t->next_check = g_vk_present + (t->stable < 4 ? 4 : t->stable < 12 ? 8 : 20) + ((u32)(gva >> 9) & 3);
        }
    }
    return t->upload_valid ? t : NULL;
}
static Surf* tex_from_tic(const u32* tic, int raw, int storage) {
    u64 gva = ((u64)(tic[2] & 0xFFFF) << 32) | tic[1];
    u32 hv = (tic[2] >> 21) & 7;
    int bl = hv == 3 || hv == 4;
    u32 w = (tic[4] & 0xFFFF) + 1, h = (tic[5] & 0xFFFF) + 1, d = ((tic[5] >> 16) & 0x3FFF) + 1;
    u32 type = (tic[4] >> 23) & 0xF;
    int srgb = (tic[4] >> 22) & 1;
    u32 glevels = ((tic[3] >> 28) & 0xF) + 1;
    u32 bh = (tic[3] >> 3) & 7, bd = (tic[3] >> 6) & 7, pitch = bl ? 0 : ((tic[3] & 0xFFFF) << 5);
    if (!gva) return NULL;
    if (hv == 0 || type == 6) {
        /* Buffer TICs describe a linear row of texels, bound as a 2D image the shader's 1D fetch reads at
           (x mod row, x / row): one row, or rows of 2^SH_TEXEL_ROW_SHIFT texels after another in memory for a longer
           buffer (UE4's vertex factories keep a mesh's texture coordinates and colors in such buffers, of tens of
           thousands of texels) */
        if (hv != 0 || type != 6) return NULL;
        w = (((tic[3] & 0xFFFFu) << 16) | (tic[4] & 0xFFFFu)) + 1;
        h = d = 1;
        if (w > (1u << SH_TEXEL_ROW_SHIFT)) {
            h = (w + (1u << SH_TEXEL_ROW_SHIFT) - 1) >> SH_TEXEL_ROW_SHIFT;
            w = 1u << SH_TEXEL_ROW_SHIFT;
        }
        glevels = 1;
        bl = 0;
        bh = bd = 0;
    }
    if ((tic[7] >> 8) & 0xF) LOG_LIMIT(1, "vulkan: multisampled textures are read as single-sampled\n");
    if (type == 0 || type == 4) h = 1;
    if (type == 3)
        d = 6;
    else if (type == 8)
        d *= 6;
    VkFmt f = vk_tic_format(tic[0], srgb);
    if (raw) {
        /* SULD.D.32 takes texel coordinates and reads a texel's 32 bits, whatever the header's components are (one
           game's post-processing reads its A2B10G10R10 targets and R32 masks this way): any color format of 4-byte
           texels stored as they are, viewed as R32_UINT */
        if (f.bpp != 4 || f.bw != 1 || f.conv || f.depth) return NULL;
        f.vk = VK_FORMAT_R32_UINT;
        f.is_int = 1;
        f.sint = 0;
    }
    if (type == 6 && f.bpp && w <= UINT32_MAX / f.bpp) pitch = w * f.bpp;
    if (!f.bpp || !w || w > 16384 || h > 16384 || d > 2048) return NULL;
    if ((u64)w * h * d * (f.bw > 1 ? 1 : f.bpp) > (512ull << 20)) return NULL;
    if (!bl && pitch < (w + f.bw - 1) / f.bw * f.bpp) {
        LOG_LIMIT(1, "vulkan: a pitch-linear texture's pitch (%u bytes) is shorter than its rows (not drawn)\n", pitch);
        return NULL;
    }
    VkImageViewType vt = tic_view_type(type, d);
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE && d != 6) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !g_vk.feat.imageCubeArray) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    u32 layers = vt == VK_IMAGE_VIEW_TYPE_3D ? 1 : d, d3 = vt == VK_IMAGE_VIEW_TYPE_3D ? d : 1;
    if (vt == VK_IMAGE_VIEW_TYPE_3D && !raw) f = vk_tic_format_3d(tic[0], srgb);
    /* bytes from one layer to the next: all the levels the header has, whichever of them it shows */
    u64 stride = bl ? bl_layer_size(w, h, d3, &f, bh, bd, glevels) : (u64)pitch * ((h + f.bh - 1) / f.bh) * d3;
    /* A texture upload reads guest memory. Complete any compute storage writes that overlap it first. */
    if (g_cs_any) vk_compute_mem_need(gva, stride * layers);
    /* the levels the shader sees, base level to max level (word 7), as a texture of their own the way Ryujinx makes
       it: the base level's address, size and blocks. Pitch-linear textures have one level */
    u32 levels = bl ? glevels : 1;
    u32 base = tic[7] & 0xF, maxl = (tic[7] >> 4) & 0xF;
    if (bl && base < glevels && (base || maxl + 1 != glevels)) {
        for (u32 l = 0; l < base; l++) gva += bl_level_size(w, h, d3, &f, bh, bd, l, NULL, NULL);
        bl_level_blocks(h, d3, &f, bh, bd, base, &bh, &bd);
        w = w >> base ? w >> base : 1;
        h = h >> base ? h >> base : 1;
        d3 = d3 >> base ? d3 >> base : 1;
        levels = maxl >= base ? maxl - base + 1 : 1;
        if (levels > glevels - base) levels = glevels - base;
    }
    /* no more levels than the size has (Vulkan requires it) */
    u32 maxdim = w > h ? w : h, maxlev = 1;
    if (d3 > maxdim) maxdim = d3;
    while (maxdim >> maxlev) maxlev++;
    if (levels > maxlev) levels = maxlev;
    /* a render target at that address: read it on the GPU, unless its contents are undefined (never drawn, or memory
       it can't load was written over it): then memory has them */
    Surf* rt = type == 6 ? NULL : vk_rt_find(gva);
    /* a texture of several levels isn't the target (its other levels are other targets' memory, a mip chain drawn level
       by level): loaded from memory */
    if (rt && rt->layout != VK_IMAGE_LAYOUT_UNDEFINED && f.bw == 1 && (!f.conv || f.depth) && type != 2 && rt->w >= w &&
        rt->h >= h && (d == 1 || type == 1 || type == 7) && levels == 1) {
        /* a header of another texel size covers other memory than the target's pixels (depth is read through its
           own aspect, whatever the host format's size) */
        if (f.bpp == rt->f.bpp || f.depth || rt->f.depth) {
            vk_rt_note_read(rt);
            if (rt->layout == VK_IMAGE_LAYOUT_UNDEFINED) goto texture_from_memory;
            /* Crops are sampling snapshots, without storage usage or a guest
               layout for publishing writes. Resolve writable mismatches into
               a surface with the descriptor's own guest layout instead. */
            if (storage && (rt->w != w || rt->h != h || rt->layers != layers ||
                            rt->depth3d != d3 || rt->f.vk != f.vk || !rt->storage_capable))
                goto texture_from_memory;
            /* the same bytes in another color format (an RGBA8 target read as R32_Uint): sampling the target itself
               would convert its texels, so a copy in the texture's format holds them as they are. sRGB and linear
               twins are views of one image and need no copy */
            if (!f.depth && !rt->f.depth && vk_srgb_as(f.vk, 0) != vk_srgb_as(rt->f.vk, 0))
                return rt_crop(rt, w, h, &f);
            return rt->w == w && rt->h == h ? rt : rt_crop(rt, w, h, NULL);
        }
        LOG_LIMIT(1,
                  "vulkan: a texture with %u-byte texels reads the render target at %llx, which has %u-byte pixels: it "
                  "is loaded from memory\n",
                  f.bpp, (unsigned long long)gva, rt->f.bpp);
    }
texture_from_memory:
    /* loaded from memory, with what render targets drew into its layers and levels copied in after (tex_take_drawn) */
    /* what the device can make */
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    int cube = vt != VK_IMAGE_VIEW_TYPE_3D && layers >= 6 && w == h; /* created cube compatible */
    if (vt == VK_IMAGE_VIEW_TYPE_3D
            ? w > lim->maxImageDimension3D || h > lim->maxImageDimension3D || d3 > lim->maxImageDimension3D
            : w > lim->maxImageDimension2D || h > lim->maxImageDimension2D || layers > lim->maxImageArrayLayers ||
                  (cube && w > lim->maxImageDimensionCube)) {
        LOG_LIMIT(1, "vulkan: a %ux%ux%u texture is bigger than the GPU's images can be (not drawn)\n", w, h, d);
        return NULL;
    }
    u32 fkey = (tic[0] & 0x7FFFF) | ((u32)srgb << 31) | ((u32)raw << 30);
    for (Surf* t = vk_surf_first(gva); t; t = t->next) {
        if (t->is_rt || t->gva != gva || t->w != w || t->h != h || t->layers != layers || t->depth3d != d3 ||
            t->fkey != fkey || t->levels != levels || t->type != vt || t->bl != (u32)bl || t->bh != bh || t->bd != bd ||
            t->pitch != pitch || t->layer_size != stride)
            continue;
        return tex_check(t, gva);
    }
    if (!format_samples(f.vk)) return NULL;
    Surf* t = tex_create(gva, w, h, layers, d3, levels, vt, f, fkey, bl, bh, bd, pitch, stride);
    if (!t) return NULL;
    /* this texture's levels in one layer; the layers before the last one take their whole stride */
    u64 sz = 0;
    for (u32 l = 0; l < levels; l++) sz += bl ? bl_level_size(w, h, d3, &f, bh, bd, l, NULL, NULL) : stride;
    t->size = stride * (layers - 1) + sz;
    t->wseen = vk_ww_now();
    vk_written_since(gmem_host(gva, t->size), t->size, 0); /* starts tracking it */
    t->wseen = vk_ww_now();
    t->gw = gpu_mem_generation();
    t->hash = mem_hash(gva, t->size);
    t->checked = g_vk_present;
    t->ww_epoch = vk_ww_epoch(); /* hashed in this epoch */
    t->next_check = g_vk_present + 4;
    return tex_upload(t) ? t : NULL;
}

static VkImageView tex_view(Surf* s, VkImageViewType type, int srgb, const VkComponentMapping* map, int stencil,
                            u32 levels) {
    /* what a view of each image type can be */
    u32 layers;
    switch (type) {
        case VK_IMAGE_VIEW_TYPE_2D:
            if (s->type == VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;
            layers = 1;
            break;
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
            if (s->type == VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;
            layers = s->layers;
            break;
        case VK_IMAGE_VIEW_TYPE_CUBE:
            if (s->layers < 6 || s->w != s->h || s->type == VK_IMAGE_VIEW_TYPE_3D || s->is_rt) return VK_NULL_HANDLE;
            layers = 6;
            break;
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
            if (s->layers < 6 || s->w != s->h || s->type == VK_IMAGE_VIEW_TYPE_3D || s->is_rt ||
                !g_vk.feat.imageCubeArray)
                return VK_NULL_HANDLE;
            layers = s->layers / 6 * 6;
            break;
        case VK_IMAGE_VIEW_TYPE_3D:
            if (s->type != VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;
            layers = 1;
            break;
        default: return VK_NULL_HANDLE;
    }
    VkFormat fmt = s->f.depth ? s->f.vk : vk_srgb_as(s->f.vk, srgb);
    stencil = stencil && s->f.stencil;
    u64 key = (u64)type | (u64)map->r << 4 | (u64)map->g << 8 | (u64)map->b << 12 | (u64)map->a << 16 | (u64)fmt << 20 |
              (u64)stencil << 60 | (u64)(levels != s->levels) << 61;
    for (int i = 0; i < s->nsv; i++)
        if (s->sv[i].key == key) return s->sv[i].v;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = s->img,
                                .viewType = type,
                                .format = fmt,
                                .components = *map,
                                .subresourceRange = {stencil      ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                     : s->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                  : VK_IMAGE_ASPECT_COLOR_BIT,
                                                     0, levels, 0, layers}};
    /* a view in the image's sRGB twin is sampled only: storage, which a render target may have, has no sRGB format */
    VkImageViewUsageCreateInfo vu = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                     .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    if (fmt != s->f.vk) vi.pNext = &vu;
    VkImageView v;
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &v))) return VK_NULL_HANDLE;
    if (s->nsv == (int)(sizeof s->sv / sizeof s->sv[0])) { /* full: drop the oldest */
        vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, s->sv[0].v, VK_NULL_HANDLE);
        memmove(s->sv, s->sv + 1, sizeof s->sv - sizeof s->sv[0]);
        s->nsv--;
    }
    s->sv[s->nsv++] = (SurfView){key, v};
    return v;
}

VkImageView vk_tex_view(Surf* s, VkImageViewType type, int srgb, const VkComponentMapping* map, int stencil) {
    return tex_view(s, type, srgb, map, stencil, s->levels);
}

VkImageView vk_tex_storage_view(Surf* s, VkImageViewType type) {
    const VkComponentMapping identity = {0};
    /* A storage descriptor exposes the first level selected by its TIC, not
       the entire sampled mip chain. Both views share the existing image. */
    return tex_view(s, type, 0, &identity, 0, 1);
}

/* empty textures for slots whose texture is missing or doesn't match the shader: zero, or depth 1.0 for shadow
   samplers, which compare against depth formats only */
VkImageView vk_null_view(VkImageViewType type, int num) {
    static VkImageView views[8][4];
    static Surf* imgs[2][4]; /* [3D][num] */
    if ((unsigned)type >= 8 || num < 0 || num > 3) return VK_NULL_HANDLE;
    if (num == 3 && type == VK_IMAGE_VIEW_TYPE_3D) num = 0; /* no 3D depth images (nor 3D shadow samplers) */
    if (views[type][num]) return views[type][num];
    if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !g_vk.feat.imageCubeArray) return VK_NULL_HANDLE;
    int is3d = type == VK_IMAGE_VIEW_TYPE_3D;
    Surf* s = imgs[is3d][num];
    if (!s) {
        static const VkFormat fm[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
                                       VK_FORMAT_D16_UNORM};
        s = calloc(1, sizeof *s);
        if (!s) return VK_NULL_HANDLE;
        s->w = s->h = s->sw = s->sh = s->levels = s->depth3d = 1;
        s->layers = is3d ? 1 : 6;
        s->type = is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        s->f = num == 3 ? (VkFmt){.vk = fm[3], .bpp = 2, .bw = 1, .bh = 1, .depth = 1}
                        : (VkFmt){fm[num], 4, 1, 1, 0, 0, (u8)(num != 0), 0, (u8)(num == 2)};
        s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .flags = is3d ? 0 : VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                                .format = fm[num],
                                .extent = {1, 1, 1},
                                .mipLevels = 1,
                                .arrayLayers = s->layers,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &s->img, &s->mem, NULL))) {
            free(s);
            return VK_NULL_HANDLE;
        }
        vk_mem_track(s->mem);
        vk_render_end(); /* clears happen outside rendering passes */
        VkCommandBuffer cb = vk_cmd();
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange all = {vk_aspect(&s->f), 0, 1, 0, s->layers};
        if (num == 3) {
            VkClearDepthStencilValue farthest = {1.0f, 0}; /* not far: windef.h defines it away */
            rec_CmdClearDepthStencilImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &farthest, 1, &all);
        } else {
            VkClearColorValue z = {{0, 0, 0, 0}};
            rec_CmdClearColorImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &all);
        }
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        imgs[is3d][num] = s;
    }
    VkComponentMapping id = {0};
    views[type][num] = vk_tex_view(s, type, 0, &id, 0);
    return views[type][num];
}

/* ------------------------------------------------------------ samplers */
static VkSamplerAddressMode wrap(u32 m) {
    switch (m) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 2:
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 3:
        case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 5:
        case 7:
            /* an optional feature: without it mirrored repeat, the same over the first mirror */
            return g_vk.has_mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                                         : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

/* tsc[0] bit 9 compare, bits 10-12 the compare function; tsc[1] filters (gpu/vk/render.c forces nearest for integer
   textures and the compare mode the shader declares) */
int g_vk_anisotropy;
VkSampler vk_sampler(const u32* tsc, int raise, int integer) {
    /* samplers are kept for good, and a device allows at least 4000 (maxSamplerAllocationCount): past 3072, a quarter
       of the table left free for probing, new sampler states get the first sampler made */
    enum { TAB = 4096, MAX = 3072 };
    static struct {
        u32 tsc[8];
        int raise, integer;
        VkSampler s;
    } tab[TAB];
    static u32 n, ncustom;
    static VkSampler first;
    u32 hk = 2166136261u;
    for (int i = 0; i < 8; i++) hk = (hk ^ tsc[i]) * 16777619u;
    hk = (hk ^ (u32)(raise << 1 | integer)) * 16777619u;
    u32 j = hk & (TAB - 1);
    for (u32 probe = 0; probe < TAB && tab[j].s; probe++, j = (j + 1) & (TAB - 1))
        if (!memcmp(tab[j].tsc, tsc, 32) && tab[j].raise == raise && tab[j].integer == integer) return tab[j].s;
    if (n >= MAX || tab[j].s) {
        LOG_LIMIT(1, "vulkan: more than %d sampler states: the others use the first one\n", MAX);
        return first;
    }
    u32 mag = tsc[1] & 3, min = (tsc[1] >> 4) & 3, mip = (tsc[1] >> 6) & 3;
    float minl = (float)(tsc[2] & 0xFFF) / 256.f, maxl = (float)((tsc[2] >> 12) & 0xFFF) / 256.f;
    i32 bias = (i32)((tsc[1] >> 12) & 0x1FFF);
    if (bias & 0x1000) bias |= ~0x1FFF;
    float lb = (float)bias / 256.f, lbmax = g_vk.props.limits.maxSamplerLodBias;
    if (lb > lbmax) lb = lbmax;
    if (lb < -lbmax) lb = -lbmax;
    static const float aniso_lut[8] = {1, 2, 4, 6, 8, 10, 12, 16}; /* Ryujinx's SamplerDescriptor */
    u32 aniso = (tsc[0] >> 20) & 7;
    float af = aniso_lut[aniso];
    /* anisotropy=N raises a mipmapped 2D texture's linear filtering (raise), a 3D scene's surfaces, to N. Nearest
       filtering (2D art, the HUD), samplers without a mip filter and depth compares (shadow maps) keep the game's */
    if (raise && (float)g_vk_anisotropy > af && min == 2 && mag == 2 && mip >= 2 && !((tsc[0] >> 9) & 1))
        af = (float)g_vk_anisotropy;
    if ((tsc[1] >> 10) & 3)
        LOG_LIMIT(1, "vulkan: min/max sampler reduction isn't supported (filtered as an average)\n");
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = mag == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .minFilter = min == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .mipmapMode = mip == 3 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = wrap(tsc[0] & 7),
                              .addressModeV = wrap((tsc[0] >> 3) & 7),
                              .addressModeW = wrap((tsc[0] >> 6) & 7),
                              .mipLodBias = lb,
                              .anisotropyEnable = af > 1 && g_vk.feat.samplerAnisotropy,
                              .maxAnisotropy = af,
                              .compareEnable = (tsc[0] >> 9) & 1,
                              .compareOp = (VkCompareOp)((tsc[0] >> 10) & 7),
                              .minLod = minl,
                              .maxLod = mip <= 1 ? minl + 0.25f : maxl, /* no mip filter: the base level only */
                              .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK}; /* set below */
    if (si.maxAnisotropy > g_vk.props.limits.maxSamplerAnisotropy)
        si.maxAnisotropy = g_vk.props.limits.maxSamplerAnisotropy;
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    /* the border color: the TSC's four words as the texture's texels read, floats for a float view and integers for an
       integer one (nouveau writes the integer ones as their bits), as Vulkan's FLOAT or INT colors. A custom color
       takes one of the device's few custom-color samplers while there are any left, else the nearest built-in one */
    float bc[4];
    i32 bi[4];
    memcpy(bc, &tsc[4], 16);
    memcpy(bi, &tsc[4], 16);
    int black = integer ? !bi[0] && !bi[1] && !bi[2] : bc[0] == 0 && bc[1] == 0 && bc[2] == 0;
    int white = integer ? bi[0] == 1 && bi[1] == 1 && bi[2] == 1 : bc[0] == 1 && bc[1] == 1 && bc[2] == 1;
    int clear = integer ? !bi[3] : bc[3] == 0, opaque = integer ? bi[3] == 1 : bc[3] == 1;
    int custom = !((black && (clear || opaque)) || (white && opaque));
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType =
                                                      VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (custom && (!g_vk.has_border_color || ncustom >= g_vk.border_color_max)) {
        float a = integer ? (float)bi[3] : bc[3];
        float lum = integer ? ((float)bi[0] + (float)bi[1] + (float)bi[2]) / 3 : (bc[0] + bc[1] + bc[2]) / 3;
        if (g_vk.has_border_color)
            LOG_LIMIT(1,
                      "vulkan: more than %u samplers with a border color of their own: the others take the nearest "
                      "of black or white\n",
                      g_vk.border_color_max);
        custom = 0;
        clear = !(a >= 0.5f); /* NaN: transparent, and black */
        black = !(lum >= 0.5f);
    }
    if (custom) {
        if (integer)
            memcpy(cb.customBorderColor.int32, bi, 16);
        else
            memcpy(cb.customBorderColor.float32, bc, 16);
        si.borderColor = integer ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        si.pNext = &cb;
    } else if (clear)
        si.borderColor = integer ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if (black)
        si.borderColor = integer ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else
        si.borderColor = integer ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    VkSampler s;
    if (!VK_OK(vkCreateSampler(g_vk.dev, &si, NULL, &s))) return VK_NULL_HANDLE;
    memcpy(tab[j].tsc, tsc, 32);
    tab[j].raise = raise;
    tab[j].integer = integer;
    tab[j].s = s;
    ncustom += custom;
    if (!n++) first = s;
    return s;
}

void vk_tex_frame(void) {
    /* surfaces whose pixels can be had again go once unused for twenty seconds and in 120 presents (a game at 6 frames
       a second kept 600 presents' worth, 100 seconds, of the targets it made every frame); textures come back from
       memory if they are needed again. Those whose pixels would be lost (vk_surf_reclaim) go before five minutes
       only under the budget's pressure (vk_mem_trim), and a target holding a draw a texture would take stays until
       then. The recency lists hand out the oldest first, so this costs what goes. The lossy pass looks every 600
       presents: it passes over the targets it keeps, and asking whether one holds such a draw can hash its memory.
       Render targets later ones drew all over hold nothing: every 30 presents those unused for a second and eight
       presents go (a game at a dynamic resolution makes its scene's targets again each frame) */
    u32 before = vk_present_before(20000000000ull, 120);
    vk_surf_reclaim(VK_LRU_TEX, before, ~0ull, VK_RECLAIM_LOSSLESS);
    vk_surf_reclaim(VK_LRU_RT, before, ~0ull, VK_RECLAIM_LOSSLESS);
    if (g_vk_present % 30 == 0) vk_rt_reclaim_superseded(vk_present_before(1000000000ull, 8), ~0ull);
    if (g_vk_present % 600 == 0) {
        u32 lossy_before = vk_present_before(300000000000ull, 600);
        vk_surf_reclaim(VK_LRU_TEX, lossy_before, ~0ull, VK_RECLAIM_IDLE);
        vk_surf_reclaim(VK_LRU_RT, lossy_before, ~0ull, VK_RECLAIM_IDLE);
    }
}

/* s copied into t, both unchanged since: kept for t and one other texture, so two textures over one target don't
   copy it on each other's binds (a texture's upload generation is new even at an address reused) */
static int copied_into(const Surf* s, const Surf* t) {
    for (int i = 0; i < 2; i++)
        if (s->copied[i].t == t && s->copied[i].wgen == s->wgen && s->copied[i].tgen == t->wgen) return 1;
    return 0;
}
static void mark_copied(Surf* s, Surf* t) {
    t->took_drawn = 1;
    if (s->copied[0].t != t) s->copied[1] = s->copied[0];
    s->copied[0].t = t;
    s->copied[0].wgen = s->wgen;
    s->copied[0].tgen = t->wgen;
}
/* a scaled target (resolution= other than 1) reaches a texture by a blit, which f's tiling has to take both ways */
static int can_blit(VkFormat f) {
    VkFormatProperties p = {0};
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & need) == need;
}
/* the command buffer with s ready to be read from and t written to, by copies or blits */
static VkCommandBuffer copy_begin(Surf* s, Surf* t) {
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    return cb;
}
/* the first count layers of s into t's level l at layer or Z slice dst, w x h: copied, or blitted from s's scaled
   size, nearest (sampling applies the texture's filter later). A blit into a 3D image takes one slice from layer 0, so
   a scaled s goes into slices one at a time (count 1: layered targets take vk_unscaled_image's way there) */
static void copy_layers(VkCommandBuffer cb, Surf* s, Surf* t, u32 l, u32 dst, u32 count, u32 w, u32 h, int z) {
    /* a region takes one aspect: a depth target's depth, and its stencil in a second region when it has one */
    VkImageSubresourceLayers from = {t->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                     z ? 1 : count};
    VkImageSubresourceLayers to = {from.aspectMask, l, z ? 0 : dst, z ? 1 : count};
    u32 regions = t->f.depth && t->f.stencil ? 2 : 1;
    i32 dz = z ? (i32)dst : 0;
    if (s->sw == s->w && s->sh == s->h) { /* layers into layers, or into as many slices */
        VkImageCopy c[2];
        from.layerCount = count;
        c[0] = (VkImageCopy){
            .srcSubresource = from, .dstSubresource = to, .dstOffset = {0, 0, dz}, .extent = {w, h, z ? count : 1}};
        c[1] = c[0];
        c[1].srcSubresource.aspectMask = c[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        rec_CmdCopyImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         regions, c);
        return;
    }
    VkImageBlit b[2];
    b[0] = (VkImageBlit){.srcSubresource = from,
                         .srcOffsets = {{0, 0, 0}, {(i32)vk_sc(w), (i32)vk_sc(h), 1}},
                         .dstSubresource = to,
                         .dstOffsets = {{0, 0, dz}, {(i32)w, (i32)h, dz + 1}}};
    b[1] = b[0];
    b[1].srcSubresource.aspectMask = b[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    rec_CmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     regions, b, VK_FILTER_NEAREST);
}

/* a texture whose layers, levels or 3D slices were drawn as render targets of their own is copied from those targets
   on the GPU: they are never written back to its memory. Only a target matching one subresource (or, layered, the
   layers from one), whose backing memory has not been overwritten since its draw. Generated textures can be first
   sampled several frames later. Targets are never block compressed, but a compressed texture takes a target whose
   texels are as many bytes as its blocks, a texel for a block: a game compressing on the GPU draws the blocks so (one
   game's text labels, BC5 drawn as RGBA32UI) */
static void tex_take_drawn(Surf* t) {
    /* a converted format's texels differ from its host image's, but a depth texture takes its targets' host images
       as they are (the same format, below): D24S8's guest packing is converted only when it is loaded from memory */
    if (!t || t->is_rt || (t->f.conv && !t->f.depth)) return;
    Surf* small[32];
    Surf** d = small;
    u64 stride = t->layer_size;
    u64 hi = t->gva + t->size;
    int n = vk_rt_drawn_in(t->gva, hi, d, 32);
    if (n > 32) {
        d = malloc((size_t)n * sizeof *d);
        if (d)
            n = vk_rt_drawn_in(t->gva, hi, d, n);
        else {
            d = small;
            n = 32;
        }
    }
    for (int i = 0; i < n; i++) {
        Surf* s = d[i];
        /* depth targets feed depth textures of the same host format, with their stencil when it has one (a depth
           pyramid of a D24S8 target: Pokemon Legends Z-A's lighting samples it with all its levels) */
        if (s->f.depth != t->f.depth || s->f.stencil != t->f.stencil || s->f.bpp != t->f.bpp ||
            (t->f.depth && s->f.vk != t->f.vk) || copied_into(s, t))
            continue;
        /* a blit converts formats where a copy keeps the bits: a scaled target only into a texture of its own format */
        int scaled = s->sw != s->w || s->sh != s->h;
        if (scaled && (s->f.vk != t->f.vk || !can_blit(t->f.vk))) continue;
        u64 off = s->gva - t->gva;
        if (t->type == VK_IMAGE_VIEW_TYPE_3D) {
            if (t->f.depth) continue;
            /* A layered color target can be sampled as a 3D texture at the same address. Its slices only exist in
               the host image: guest memory is not updated by draws. Each array layer goes to the matching Z slice */
            /* Its layers are the texture's slices when they are GOB slices (a volume) or lie inside the texture. A
               target over the texture's first slice with layers a whole texture apart (one game's colour grading
               table, slice 0 drawn through it and the others through targets of their own) holds the first slice
               alone: it is taken as that slice below */
            if (s->layers > 1 && off == 0 && s->w == t->w && s->h == t->h && s->layers >= t->depth3d &&
                (s->volume || s->layer_size * (t->depth3d - 1) < t->size) && s->f.vk == t->f.vk &&
                vk_rt_drawn_fresh(s)) {
                /* a blit can't take array layers into a 3D image's slices, only a copy can: scaled, the layers
                   are blitted down to an array at the guest's size first, and copied into the slices from there */
                VmaAllocation mm = VK_NULL_HANDLE;
                VkImage mid = scaled ? vk_unscaled_image(s, t->depth3d, &mm) : VK_NULL_HANDLE;
                if (scaled && !mid) continue;
                VkCommandBuffer cb = copy_begin(s, t);
                if (scaled) {
                    VkImageSubresourceLayers all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, t->depth3d};
                    VkImageBlit b = {.srcSubresource = all,
                                     .srcOffsets = {{0, 0, 0}, {(i32)vk_sc(t->w), (i32)vk_sc(t->h), 1}},
                                     .dstSubresource = all,
                                     .dstOffsets = {{0, 0, 0}, {(i32)t->w, (i32)t->h, 1}}};
                    vk_barrier(cb, mid, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, 0,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    rec_CmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mid,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
                    vk_barrier(cb, mid, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    /* layer z into slice z, all in one region */
                    VkImageCopy c = {.srcSubresource = all,
                                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .extent = {t->w, t->h, t->depth3d}};
                    rec_CmdCopyImage(cb, mid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                    vk_defer_image(mid, mm, VK_NULL_HANDLE, VK_NULL_HANDLE); /* freed once the frame is done */
                } else
                    copy_layers(cb, s, t, 0, 0, t->depth3d, t->w, t->h, 1);
                mark_copied(s, t);
                continue;
            }
            /* A 3D block-linear texture can be built a Z slice at a time through 2D targets. The target may be
               wider than the texture: its rows include the 64-byte GOB padding of a narrow slice. */
            if (!t->bl || !s->bl || s->f.vk != t->f.vk) continue;
            u64 lo = 0;
            for (u32 l = 0; l < t->levels; l++) {
                u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
                u32 ld = t->depth3d >> l ? t->depth3d >> l : 1, lbh, lbd;
                u64 size = bl_level_size(t->w, t->h, t->depth3d, &t->f, t->bh, t->bd, l, &lbh, &lbd);
                u32 rowb = lw * t->f.bpp, gobs_x = (rowb + 63) / 64;
                u32 blocks_y = ((lh + 7) / 8 + (1u << lbh) - 1) >> lbh;
                /* A block is one GOB wide, 2^lbh high and 2^lbd deep, its GOBs Y first and then Z, and the blocks of
                   a level go X, then Y, then Z. So slice z's first GOB is at block z >> lbd's depth plane, z & (2^lbd
                   - 1) GOB columns of 2^lbh into the first block: slices 512 << lbh bytes apart inside a depth plane,
                   a plane (slab) apart across them. Without block depth that is a slice after another */
                u64 slab = (u64)gobs_x * blocks_y * (512ull << (lbh + lbd)), step = 512ull << lbh;
                u32 sbh;
                u64 source_size = bl_level_size(s->w, s->h, 1, &s->f, s->bh, 0, 0, &sbh, NULL);
                if (off >= lo && off - lo < size && (off - lo) % slab % step == 0 &&
                    (off - lo) % slab / step < (1u << lbd) && s->w >= lw && s->h == lh &&
                    sbh == lbh && (lbd || source_size == slab) && vk_rt_drawn_fresh(s)) {
                    u64 z = ((off - lo) / slab << lbd) + (off - lo) % slab / step;
                    if (z < ld) {
                        copy_layers(copy_begin(s, t), s, t, l, (u32)z, 1, lw, lh, 1);
                        mark_copied(s, t);
                        break;
                    }
                }
                lo += size;
            }
            continue;
        }
        u32 layer = stride ? (u32)(off / stride) : 0;
        u64 rem = off - (u64)layer * stride, lo = 0;
        for (u32 l = 0; l < t->levels && layer < t->layers; l++) {
            u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
            /* a compressed level in blocks, the target's texels. The copy's extent counts the target's texels, and
               the blocks over a level's edge are the level's */
            lw = (lw + t->f.bw - 1) / t->f.bw;
            lh = (lh + t->f.bh - 1) / t->f.bh;
            /* a block-linear level's rows take whole GOBs (64 bytes), so a target drawn as wide as them covers the
               level (a 120-texel depth pyramid level drawn 128 wide, a cube mip narrower than a GOB one GOB wide, a
               230-texel BC5 label's 58 blocks drawn 60 wide): only its left lw texels are the level's */
            int padded_row = t->bl && s->bl && (s->f.vk == t->f.vk || t->f.bw > 1) && s->w > lw &&
                             ((u64)s->w * s->f.bpp + 63) / 64 == ((u64)lw * t->f.bpp + 63) / 64;
            if (rem == lo && (s->w == lw || padded_row) && s->h == lh) {
                if (!vk_rt_drawn_fresh(s)) break; /* newer guest memory at this address takes precedence */
                /* a layered target spaced as the texture's layers fills as many of them as it has, at any level: one
                   game draws each level of its reflection cube through a target of six layers a whole face apart,
                   whose layer k is face k of that level (taken at level 0 alone, faces 1 to 5 of the blurred levels
                   kept guest memory's bytes, and its shiny surfaces reflected noise) */
                u32 count = 1;
                if (s->layers > 1 && !s->volume && s->layer_size == stride) /* a volume's are GOB slices */
                    count = s->layers < t->layers - layer ? s->layers : t->layers - layer;
                copy_layers(copy_begin(s, t), s, t, l, layer, count, lw, lh, 0);
                mark_copied(s, t);
                break;
            }
            if (!t->bl) break;
            lo += bl_level_size(t->w, t->h, 1, &t->f, t->bh, t->bd, l, NULL, NULL);
        }
    }
    if (d != small) free(d);
}
int vk_fmap_on(void);                  /* SwitchRecomp */
void vk_dlss_before_read(u64 gva); /* SwitchRecomp, dlss.c: DLSS runs on the scene target before anything reads it */
Surf* vk_tex_from_tic(const u32* tic) {
    Surf* t = tex_from_tic(tic, 0, 0);
    if (t) vk_dlss_before_read(t->gva);
    tex_take_drawn(t);
    if (t && vk_fmap_on()) { /* SNX_FRAME_MAP: which render targets a pass reads (render.c attributes them) */
        void vk_fmap_read(u64 gva, u32 w, u32 h, u32 fkey);
        Surf* r = vk_rt_find(t->gva);
        if (r) vk_fmap_read(t->gva, t->w, t->h, r->fkey);
    }
    return t;
}
Surf* vk_tex_from_tic_storage(const u32* tic) {
    Surf* t = tex_from_tic(tic, 0, 1);
    if (t) vk_dlss_before_read(t->gva);
    tex_take_drawn(t);
    return t;
}
Surf* vk_tex_recheck(Surf* t) {
    if (g_cs_any) vk_compute_mem_need(t->gva, t->layer_size * t->layers);
    t = tex_check(t, t->gva);
    tex_take_drawn(t);
    return t;
}
Surf* vk_tex_from_tic_raw(const u32* tic) {
    Surf* t = tex_from_tic(tic, 1, 0);
    if (t) vk_dlss_before_read(t->gva);
    tex_take_drawn(t);
    return t;
}
