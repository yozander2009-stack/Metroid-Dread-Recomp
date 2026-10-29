/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Render targets by guest GPU address, in a table textures share (gpu/vk/texture.c). A render target lives on the
   GPU; vk_rt_flush writes it back to guest memory when the CPU side needs it (DMA and 2D copies), vk_rt_reload reads
   it again after the CPU side wrote that memory. Games reuse one address for render targets of different sizes and
   formats within a frame: a target whose address is taken by another is parked and reused when its size and format
   come back. Textures and render targets are each in a list from the least recently used to the most, which the
   caches' eviction takes from (vk_surf_reclaim): idle ones, and more when device memory nears its budget
   (vk_mem_trim, with the budget from gpu/vk/memory.c). */
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/gpu/texture/blocklinear.h"
#include "nx/memory/own.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/runtime/state_io.h"

#define SURFH 4096
static Surf* g_surfh[SURFH];
/* The recency lists, from the least recently used to the most: textures (VK_LRU_TEX) and render targets (VK_LRU_RT,
   parked ones too), each with a second list (LRU_AGED + its own) of the old ones whose pixels would be lost if they
   went (lossless below), moved there as an eviction that takes only lossless ones passes them. Each stays in the
   order of last use: what is moved comes from the old end of the first list, older than all left there. A surface
   used again goes back to the recent end of the first. Both render target lists together are every real target, for
   overlapping guest-memory writes (vk_rt_before_write) */
#define LRU_AGED 2
static struct {
    Surf *head, *tail;
} g_lru[5];
static struct {
    u32 n;
    u64 bytes;
} g_held[3];         /* by VK_LRU_TEX or VK_LRU_RT: the surfaces and their memory */
static u32 g_parked; /* render targets parked */
static int lru_kind(const Surf* s) { return s->lru > LRU_AGED ? s->lru - LRU_AGED : s->lru; }
static void lru_link(Surf* s, int list) {
    s->lru = list;
    s->lru_next = NULL;
    s->lru_prev = g_lru[list].tail;
    if (s->lru_prev)
        s->lru_prev->lru_next = s;
    else
        g_lru[list].head = s;
    g_lru[list].tail = s;
}
static void lru_unlink(Surf* s) {
    int list = s->lru;
    if (s->lru_prev)
        s->lru_prev->lru_next = s->lru_next;
    else
        g_lru[list].head = s->lru_next;
    if (s->lru_next)
        s->lru_next->lru_prev = s->lru_prev;
    else
        g_lru[list].tail = s->lru_prev;
    s->lru = VK_LRU_NONE;
}
static u64 g_touched;
void vk_surf_touch(Surf* s) {
    if (s->last_used == g_vk_present) return;
    s->last_used = g_vk_present;
    if (!s->lru) return;
    s->touched = ++g_touched;
    int kind = lru_kind(s);
    lru_unlink(s);
    lru_link(s, kind);
}
static int lossless(const Surf* s);
/* s's pixels can be had again now (written back, loaded, or left undefined) and are taken out of the aged list, else
   a lossless eviction wouldn't find it and the idle one would keep it for five minutes. Into its first list in order
   of last use: near the old end, as everything there was used after everything aged, past only the others relisted */
void vk_surf_relist(Surf* s) {
    if (s->lru <= LRU_AGED || !lossless(s)) return;
    int kind = lru_kind(s);
    Surf* at = g_lru[kind].head; /* the first used after s */
    while (at && (i32)((u32)at->last_used - (u32)s->last_used) <= 0) at = at->lru_next;
    lru_unlink(s);
    s->lru = kind;
    s->touched = ++g_touched; /* past the others of its last_used */
    s->lru_next = at;
    s->lru_prev = at ? at->lru_prev : g_lru[kind].tail;
    if (s->lru_prev)
        s->lru_prev->lru_next = s;
    else
        g_lru[kind].head = s;
    if (at)
        at->lru_prev = s;
    else
        g_lru[kind].tail = s;
}
static void lru_add(Surf* s, int kind) {
    s->last_used = g_vk_present;
    s->touched = ++g_touched;
    lru_link(s, kind);
    g_held[kind].n++;
    g_held[kind].bytes += s->bytes;
}
void vk_surf_stats(int kind, u32* count, u64* bytes, u32* parked) {
    *count = g_held[kind].n;
    *bytes = g_held[kind].bytes;
    *parked = kind == VK_LRU_RT ? g_parked : 0;
}
static _Thread_local int g_rt_flushing;
static u32 hkey(u64 a) { return (u32)((a >> 8) ^ (a >> 20)) & (SURFH - 1); }

/* bytes of a block-linear surface of w_bytes x h, block height 2^bh GOBs (shrunk for small surfaces, as the
   hardware does) */
static u64 bl_size(u32 w_bytes, u32 h, u32 bh) {
    u32 gx = (w_bytes + 63) / 64, gy = (h + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    u64 gys = ((u64)(gy + (1u << bh) - 1) >> bh) << bh;
    return (u64)gx * gys * 512;
}
static u32 bl_height(u32 h, u32 bh) {
    u32 gy = (h + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    return bh;
}
VkImageAspectFlags vk_aspect(const VkFmt* f) {
    if (!f->depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (f->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

void vk_surf_use(VkCommandBuffer cb, Surf* s, VkImageLayout layout, VkPipelineStageFlags2 stage,
                 VkAccessFlags2 access) {
    int writes = (access & (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) != 0;
    int wrote = (s->access & (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) != 0;
    if (s->layout == layout && !writes && !wrote) {
        /* read after read in the same layout needs no barrier, unless the reader is in a stage the barrier that made
           the last write visible didn't name (a copy after a blit read it): a barrier from the earlier readers
           extends that visibility to it */
        if (stage & ~s->stage) {
            vk_render_end();
            vk_barrier(cb, s->img, vk_aspect(&s->f), s->stage ? s->stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       layout, stage, access, layout);
        }
        s->stage |= stage;
        s->access |= access;
        return;
    }
    vk_render_end(); /* no barriers on images inside a rendering pass */
    vk_barrier(cb, s->img, vk_aspect(&s->f), s->stage ? s->stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, s->access,
               s->layout, stage, access, layout);
    s->layout = layout;
    s->stage = stage;
    s->access = access;
}

float g_vk_scale = 1.0f;
u32 vk_sc(u32 v) {
    u32 r = (u32)((float)v * g_vk_scale + 0.5f);
    return r ? r : 1;
}
float vk_scf(float v) { return v * g_vk_scale; }

static u64 g_rt_made; /* for the stats line */
u64 vk_rt_made(void) { return g_rt_made; }
static Surf* surf_create(u64 gva, u32 w, u32 h, VkFmt f, u32 fkey, u32 bl, u32 bh, u32 pitch, int is_rt, u32 layers) {
    Surf* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->gva = gva;
    s->w = w;
    s->h = h;
    s->sw = is_rt ? vk_sc(w) : w;
    s->sh = is_rt ? vk_sc(h) : h;
    s->layers = layers;
    s->levels = 1;
    s->fkey = fkey;
    s->f = f;
    s->bl = bl;
    s->bh = bh;
    s->pitch = pitch;
    s->is_rt = is_rt;
    if (is_rt) g_rt_made++;
    if (is_rt) s->layer_size = bl ? bl_size(w * f.bpp, h, bh) : (u64)pitch * h;
    s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (is_rt) usage |= f.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    /* the draws rendering to a color target can sample it, in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL (gpu/vk/render.c) */
    if (is_rt && !f.depth && g_vk.has_feedback) usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    VkFormatProperties format_props;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f.vk, &format_props);
    if (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        s->storage_capable = 1;
    }
    s->type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = f.vk,
                            .extent = {s->sw, s->sh, 1},
                            .mipLevels = 1,
                            .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkViewFormats vf; /* sRGB and linear views */
    vk_view_formats(&ii, &vf);
    /* a scaled target can pass the allocation cap (4x of 4K is 506 MiB in RGBA8). Room is made for it under the
       budget: the caller holds only surfaces it used this present, which stay */
    if (!vk_cache_image_create(&ii, &s->img, &s->mem, &s->bytes)) {
        free(s);
        return NULL;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = s->img,
                                .viewType = s->type,
                                .format = f.vk,
                                .subresourceRange = {vk_aspect(&f), 0, 1, 0, layers}};
    VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &s->att));
    u32 k = hkey(gva);
    s->next = g_surfh[k];
    g_surfh[k] = s;
    g_vk_surf_epoch++;
    if (is_rt) lru_add(s, VK_LRU_RT);
    return s;
}

/* SET_SRGB_WRITE selects the attachment view, leaving the image's stored bytes and sampling format intact. */
VkImageView vk_rt_attachment(Surf* s, int srgb_write) {
    VkFormat linear = vk_srgb_as(s->f.vk, 0);
    if (srgb_write || linear == s->f.vk) return s->att;
    if (!s->att_linear) {
        /* an attachment in the feedback-loop layout needs the loop's usage on its view, and sampling
           (VUID-VkRenderingAttachmentInfo-imageView-10780) */
        VkImageViewUsageCreateInfo usage = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
            .usage =
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                (g_vk.has_feedback ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0)};
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .pNext = &usage,
                                    .image = s->img,
                                    .viewType = s->type,
                                    .format = linear,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, s->layers}};
        VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &s->att_linear));
    }
    return s->att_linear;
}

Surf* vk_surf_first(u64 gva) { return g_surfh[hkey(gva)]; }
u32 g_vk_surf_epoch;
void vk_surf_add(Surf* s) {
    u32 k = hkey(s->gva);
    s->next = g_surfh[k];
    g_surfh[k] = s;
    g_vk_surf_epoch++;
    lru_add(s, VK_LRU_TEX);
}

/* The live render targets (parked ones aren't) by address, open addressed. vk_rt_find is asked about 4,000 times a
   frame in one game, mostly by texture lookups, and walking the hash chain read two or three Surfs scattered in
   memory each time: a twentieth of its GPU thread. An address has at most one live target (rt_create parks the one
   there before it makes or takes another), and a few hundred live at once, so the table stays in the cache. Without
   it (an allocation failed) the chain is walked */
typedef struct {
    u64 gva;
    Surf* s; /* NULL: a free slot */
} RtSlot;
static RtSlot* g_rtx;
static u32 g_rtx_mask, g_rtx_n;
static int g_rtx_failed;
static u32 rtx_home(u64 gva) { return (u32)((gva * 0x9E3779B97F4A7C15ull) >> 40) & g_rtx_mask; }
static void rtx_put(RtSlot* t, u32 mask, u64 gva, Surf* s) {
    u32 i = (u32)((gva * 0x9E3779B97F4A7C15ull) >> 40) & mask;
    while (t[i].s) i = (i + 1) & mask;
    t[i].gva = gva;
    t[i].s = s;
}
static void rtx_add(Surf* s) {
    if (g_rtx_failed) return;
    if ((g_rtx_n + 1) * 2 > (g_rtx ? g_rtx_mask + 1 : 0)) {
        u32 cap = g_rtx ? (g_rtx_mask + 1) * 2 : 1024;
        RtSlot* t = calloc(cap, sizeof *t);
        if (!t) {
            g_rtx_failed = 1;
            free(g_rtx);
            g_rtx = NULL;
            return;
        }
        for (u32 i = 0; g_rtx && i <= g_rtx_mask; i++)
            if (g_rtx[i].s) rtx_put(t, cap - 1, g_rtx[i].gva, g_rtx[i].s);
        free(g_rtx);
        g_rtx = t;
        g_rtx_mask = cap - 1;
    }
    rtx_put(g_rtx, g_rtx_mask, s->gva, s);
    g_rtx_n++;
}
/* the live target at gva out of the table, the slots after it shifted back so that every probe still finds its own */
static void rtx_del(u64 gva) {
    u32 i, j;
    if (!g_rtx) return;
    for (i = rtx_home(gva); g_rtx[i].s && g_rtx[i].gva != gva; i = (i + 1) & g_rtx_mask) {}
    if (!g_rtx[i].s) return;
    g_rtx_n--;
    for (j = (i + 1) & g_rtx_mask; g_rtx[j].s; j = (j + 1) & g_rtx_mask) {
        u32 h = rtx_home(g_rtx[j].gva);
        /* j's entry stays when its home lies cyclically in (i, j] */
        if (i <= j ? (i < h && h <= j) : (i < h || h <= j)) continue;
        g_rtx[i] = g_rtx[j];
        i = j;
    }
    g_rtx[i].s = NULL;
}
Surf* vk_rt_find(u64 gva) {
    if (g_rtx_failed) {
        for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
            if (s->gva == gva && s->is_rt) return s;
        return NULL;
    }
    if (!g_rtx) return NULL;
    for (u32 i = rtx_home(gva); g_rtx[i].s; i = (i + 1) & g_rtx_mask)
        if (g_rtx[i].gva == gva) return g_rtx[i].s;
    return NULL;
}

VkImage vk_unscaled_image(const Surf* s, u32 layers, VmaAllocation* mem) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = s->f.vk,
                            .extent = {s->w, s->h, 1},
                            .mipLevels = 1,
                            .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage img = VK_NULL_HANDLE;
    if (!vk_image_create(&ii, &img, mem)) return VK_NULL_HANDLE;
    return img;
}
static int blit_ok(VkFormat f, VkFormatFeatureFlags need) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & need) == need;
}
/* a scaled target's contents move between its image and guest memory by blits: 0 when its format can't be blitted
   (said once) */
static int moves_scaled(const Surf* s) {
    if (s->sw == s->w && s->sh == s->h) return 1;
    if (blit_ok(s->f.vk, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) return 1;
    LOG_LIMIT(1, "vulkan: format %d can't be scaled: render targets in it don't move to or from guest memory\n",
              (int)s->f.vk);
    return 0;
}
/* src (TRANSFER_SRC) scaled into dst (TRANSFER_DST), whole images */
static void scale_blit(VkCommandBuffer cb, VkImage src, u32 sw, u32 sh, u32 src_layer, VkImage dst, u32 dw, u32 dh,
                       u32 dst_layer, int is_int) {
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, src_layer, 1},
                     .srcOffsets = {{0, 0, 0}, {(i32)sw, (i32)sh, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, dst_layer, 1},
                     .dstOffsets = {{0, 0, 0}, {(i32)dw, (i32)dh, 1}}};
    rec_CmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   is_int ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
}

/* the guest bytes of a render target: block linear, or pitch linear (its last row ends after the row's bytes, not
   after a whole pitch) */
static u64 rt_layer_bytes(const Surf* s) {
    u64 row = (u64)s->w * s->f.bpp;
    return s->bl ? bl_size((u32)row, s->h, s->bh) : (u64)s->pitch * (s->h - 1) + row;
}
/* whether s's slices are one 3D volume in guest memory (a 3D render target's layers, a 3D texture's depth, written
   to as a storage image), not layers a layer_size apart */
static int surf_volume(const Surf* s) { return s->volume || s->type == VK_IMAGE_VIEW_TYPE_3D; }
static u32 surf_slices(const Surf* s) { return s->type == VK_IMAGE_VIEW_TYPE_3D ? s->depth3d : s->layers; }
/* a target's guest bytes, all layers: kept in rt_size once rt_create has made it, as its shape doesn't change */
static u64 rt_bytes(const Surf* s) {
    if (s->rt_size) return s->rt_size;
    if (surf_volume(s) && s->bl) {
        u64 slice = rt_layer_bytes(s);
        u32 depth = (surf_slices(s) + (1u << s->bd) - 1) & ~((1u << s->bd) - 1);
        return slice * depth;
    }
    return s->layer_size * (s->layers - 1) + rt_layer_bytes(s);
}
u64 vk_surf_guest_size(const Surf* s) { return s ? rt_bytes(s) : 0; }
static u64 g_wseq;
/* every image's write generations, its uploads' and its writes' alike, from one count: a surface counting its own
   writes ran ahead of the uploads' count, and an upload then handed it a generation its image would reach again with
   guest memory still at it, so its next write looked written back already and never landed */
static u32 g_surf_gen;
u32 vk_surf_gen(void) { return ++g_surf_gen; }
void vk_surf_wrote(Surf* s) {
    s->wgen = vk_surf_gen();
    s->wseq = ++g_wseq;
}
/* the GPU pages the render targets not parked cover, so a write that meets none (almost all) skips the walk over every
   target in vk_rt_before_write: a target adds its pages when it is made or taken out of parking, and removes them when it
   is parked or freed */
static PendPages g_rt_pages;
static void rt_pages(const Surf* s, int delta) {
    if (!(s->gva >> 63)) pend_add_va(&g_rt_pages, s->gva, s->rt_size, delta);
}
int vk_surf_can_writeback(const Surf* s) { return s && s->f.raw && !s->f.depth && moves_scaled(s); }

/* s's layers, in rows of pitch bytes from off in buf, a layer slice bytes after the last, copied into its image
   (scaled when it is) */
static void rt_load_copy(VkCommandBuffer cb, Surf* s, VkBuffer buf, u64 off, u32 pitch, u64 slice) {
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (u32 layer = 0; layer < s->layers; layer++) {
        VkBufferImageCopy c = {.bufferOffset = off + (u64)layer * slice,
                               .bufferRowLength = pitch / s->f.bpp,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
                               .imageExtent = {s->w, s->h, 1}};
        VmaAllocation tm;
        VkImage t = s->sw == s->w && s->sh == s->h ? VK_NULL_HANDLE : vk_unscaled_image(s, 1, &tm);
        if (!t)
            rec_CmdCopyBufferToImage(cb, buf, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        else {
            c.imageSubresource.baseArrayLayer = 0;
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            rec_CmdCopyBufferToImage(cb, buf, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            scale_blit(cb, t, s->w, s->h, 0, s->img, s->sw, s->sh, layer, s->f.is_int);
            vk_defer_image(t, tm, VK_NULL_HANDLE, VK_NULL_HANDLE);
        }
    }
}
static void rt_loaded(Surf* s) {
    s->mem_wgen = s->wgen; /* memory and image hold the same now: nothing to write back */
    s->draw_wseen = 0;     /* a successful full load starts a new source epoch for later draws */
    s->gw = gpu_mem_generation();
    vk_surf_relist(s);
}
/* the moves of a block linear target's layers: one of every slice of a volume, one a layer of an array (each layer
   at layer_size from the last, where the layout starts again) */
static VkBlMove rt_bl_move(const Surf* s, u32 pitch) {
    VkBlMove m = {s->w * s->f.bpp, s->h, bl_height(s->h, s->bh), 0, 0, s->h, 0, 1, pitch};
    if (surf_volume(s)) {
        m.bd = s->bd;
        m.slices = surf_slices(s);
    }
    return m;
}
/* a block linear target loaded: its guest bytes staged as they are, from where they are, and deswizzled on the GPU */
static void rt_reload_bl(Surf* s) {
    u64 sz = rt_bytes(s);
    u32 pitch = (s->w * s->f.bpp + 15) & ~15u, moves = s->volume ? 1 : s->layers;
    VkBlMove m = rt_bl_move(s, pitch);
    u64 lin = (u64)pitch * s->h * s->layers, cap;
    VkBuffer staged, scratch;
    u64 off;
    vk_render_end();
    u8* p = vk_stage(sz, vk_stage_align(), &staged, &off);
    if (!p || !(scratch = vk_bl_scratch(lin, &cap))) return;
    vk_guest_read(s->gva, sz, p, staged, off); /* what the GPU holds of it copied on the GPU */
    VkCommandBuffer cb = vk_cmd();
    vk_bl_barrier(cb, 1);
    for (u32 i = 0; i < moves; i++)
        if (!vk_bl_move(cb, &m, 0, staged, off + (u64)i * s->layer_size, scratch, (u64)i * pitch * s->h)) return;
    vk_bl_barrier(cb, 0);
    rt_load_copy(cb, s, scratch, 0, pitch, (u64)pitch * s->h);
    rt_loaded(s);
}

void vk_rt_reload(u64 gva) {
    Surf* s = vk_rt_find(gva);
    if (!s) return;
    vk_surf_wrote(s); /* the copies of it (crops) are out of date */
    if (s->f.depth || !s->f.raw || !moves_scaled(s)) {
        /* new memory over a target we can't load as is: memory is the truth. Its contents are undefined until it is
           drawn to again (textures over it are loaded from memory meanwhile, and it isn't presented); the stage and
           access stay for the next barrier to wait on */
        vk_render_end();
        s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        s->draw_wseen = 0;
        s->gw = gpu_mem_generation();
        vk_surf_relist(s);
        return;
    }
    if (s->bl) {
        rt_reload_bl(s);
        return;
    }
    /* pitch linear: its guest bytes staged as they lie, the copy taking the pitch, when it is whole texels (else a
       row at a time into tight rows); what the GPU holds of them copied on the GPU */
    u64 sz = rt_bytes(s), row = (u64)s->w * s->f.bpp;
    int as_is = s->pitch % s->f.bpp == 0;
    VkBuffer buf;
    u64 off;
    vk_render_end();
    u8* p = vk_stage(as_is ? sz : row * s->h * s->layers, vk_stage_align(), &buf, &off);
    if (!p) return;
    if (as_is)
        vk_guest_read(gva, sz, p, buf, off);
    else
        for (u32 layer = 0; layer < s->layers; layer++)
            for (u32 y = 0; y < s->h; y++) {
                u64 to = ((u64)layer * s->h + y) * row;
                vk_guest_read(gva + (u64)layer * s->layer_size + (u64)y * s->pitch, row, p + to, buf, off + to);
            }
    if (as_is)
        rt_load_copy(vk_cmd(), s, buf, off, s->pitch, s->layer_size);
    else
        rt_load_copy(vk_cmd(), s, buf, off, (u32)row, row * s->h);
    rt_loaded(s);
}

/* a depth target whose Vulkan format holds the guest's bytes (Z32F as D32_SFLOAT, Z16 as D16_UNORM): its depth
   aspect copies out in the guest's own layout. The packed ones (the Z24S8 kinds, Z32F_X24S8) would need converting,
   and a scaled one a depth blit; those stay on the GPU */
static int depth_copies_raw(const Surf* s) {
    return (s->f.vk == VK_FORMAT_D32_SFLOAT || s->f.vk == VK_FORMAT_D16_UNORM) && s->sw == s->w && s->sh == s->h;
}
/* the bytes s's pixels take read back, or 0 when they don't go to memory: a format guest memory can't hold as it is, no
   pixels, or not drawn to since it was last written back or loaded (memory has it already, and a CPU write since then
   is newer than the target). A block linear target comes back in its guest layout, swizzled on the GPU */
static u64 flush_bytes(const Surf* s) {
    if (!s || (s->f.depth ? !depth_copies_raw(s) : !s->f.raw) || s->layout == VK_IMAGE_LAYOUT_UNDEFINED) return 0;
    if (s->mem_wgen == s->wgen || !moves_scaled(s)) return 0;
    return s->bl ? rt_bytes(s) : (u64)s->w * s->f.bpp * s->h * s->layers;
}
/* s's pixels copied into buf at at, in rows of pitch bytes, a layer (or a 3D image's slice) after another, at the
   guest's size */
static void flush_copy(VkCommandBuffer cb, Surf* s, VkBuffer buf, u64 at, u32 pitch) {
    u64 slice = (u64)pitch * s->h;
    u32 depth = s->type == VK_IMAGE_VIEW_TYPE_3D ? s->depth3d : 1; /* a 3D image's slices in one copy */
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    for (u32 layer = 0; layer < s->layers; layer++) {
        VkBufferImageCopy c = {
            .bufferOffset = at + (u64)layer * slice,
            .bufferRowLength = pitch / s->f.bpp,
            .imageSubresource = {s->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
            .imageExtent = {s->w, s->h, depth}};
        VmaAllocation tm;
        VkImage t = s->sw == s->w && s->sh == s->h ? VK_NULL_HANDLE : vk_unscaled_image(s, 1, &tm);
        if (!t)
            rec_CmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        else {
            c.imageSubresource.baseArrayLayer = 0;
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            scale_blit(cb, s->img, s->sw, s->sh, layer, t, s->w, s->h, 0, s->f.is_int);
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            rec_CmdCopyImageToBuffer(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
            vk_defer_image(t, tm, VK_NULL_HANDLE, VK_NULL_HANDLE);
        }
    }
}
/* s's pixels into buf at at as flush_bytes says: tightly packed rows, or the guest's block linear layout, the image
   copied into the scratch buffer and swizzled from there into buf. 0 when that can't be done */
static int flush_record(VkCommandBuffer cb, Surf* s, VkBuffer buf, u64 at) {
    u32 row = s->w * s->f.bpp;
    if (!s->bl) {
        flush_copy(cb, s, buf, at, row);
        return 1;
    }
    u32 pitch = (row + 15) & ~15u, moves = surf_volume(s) ? 1 : s->layers;
    u64 cap;
    VkBlMove m = rt_bl_move(s, pitch);
    VkBuffer scratch = vk_bl_scratch((u64)pitch * s->h * surf_slices(s), &cap);
    if (!scratch) return 0;
    flush_copy(cb, s, scratch, 0, pitch);
    vk_bl_barrier(cb, 1);
    for (u32 i = 0; i < moves; i++)
        if (!vk_bl_move(cb, &m, 1, buf, at + (u64)i * s->layer_size, scratch, (u64)i * pitch * s->h)) return 0;
    vk_bl_barrier(cb, 0);
    return 1;
}
/* the copies recorded so far done, and their bytes readable by the host */
static void flush_wait(VkCommandBuffer cb) {
    /* the copy's writes made visible to the host reads below (the fence wait alone doesn't) */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    vk_finish();
    vk_readback_ready();
}
/* s's pixels, read back to p as of its write generation wgen, into guest memory in its layout (or over original, a
   storage image's merge) */
static void flush_land(Surf* s, const u8* p, const u8* original, u64 wgen) {
    u64 gva = s->gva, row = (u64)s->w * s->f.bpp, slice = row * s->h;
    u64 sz = rt_bytes(s);
    u8* dst = original ? NULL : gmem_ptr(gva, sz);
    u8* tmp = NULL;
    if (!dst) {
        tmp = malloc(sz);
        if (!tmp) return;
        if (original)
            memcpy(tmp, original, sz);
        else
            gmem_read(gva, tmp, sz);
        dst = tmp;
    }
    if (s->bl && surf_volume(s))
        bl_land(dst, p, (u32)row, s->h, surf_slices(s), bl_height(s->h, s->bh), s->bd);
    else
        for (u32 layer = 0; layer < s->layers; layer++) {
            u8* layer_dst = dst + (u64)layer * s->layer_size;
            if (s->bl)
                bl_land(layer_dst, p + (u64)layer * s->layer_size, (u32)row, s->h, 1, bl_height(s->h, s->bh), 0);
            else
                for (u32 y = 0; y < s->h; y++)
                    memcpy(layer_dst + (u64)y * s->pitch, p + (u64)layer * slice + y * row, row);
        }
    int guest_changed = 0, flushing = g_rt_flushing;
    g_rt_flushing = 1; /* s's own write: the targets newer than s over its bytes stay pending (vk_rt_before_write) */
    if (original) {
        const u8* current = gmem_ptr(gva, sz);
        u8* current_copy = NULL;
        if (!current) {
            current_copy = malloc(sz);
            if (!current_copy) fatal("Out of memory comparing storage image writeback");
            gmem_read(gva, current_copy, sz);
            current = current_copy;
        }
        guest_changed = memcmp(current, original, sz) != 0;
        free(current_copy);
        s->mem_wgen = wgen; /* avoid flushing this target again when writeback checks overlapping targets */
        vk_write_changed(gva, dst, original, (u32)sz);
        free(tmp);
    } else if (tmp) {
        gmem_write(gva, tmp, sz);
        free(tmp);
    } else
        gpu_mem_written(gva, sz); /* caches built from this memory check it again */
    g_rt_flushing = flushing;
    s->mem_wgen = wgen;
    s->gw = gpu_mem_generation();
    vk_surf_relist(s);
    if (!s->is_rt) vk_tex_note_writeback(s);
    if (guest_changed) {
        /* The image still holds the shader result where the CPU won the merge. Force the next bind to upload
           the merged guest bytes, even though note_writeback just hashed those same bytes. */
        s->hash ^= 1;
        s->gw = 0;
    }
}
/* The targets over [gva, gva + size) holding pixels guest memory hasn't got, oldest image first: memory freed and
   given to a new target still has the old target's pixels pending, and where two cover the same bytes the newer one's
   go on top. In local while it holds nlocal, else in a list the caller frees */
static u32 rt_pending_over(u64 gva, u64 size, Surf** local, u32 nlocal, Surf*** out) {
    Surf** list = local;
    u32 n = 0, cap = nlocal;
    for (int lru = VK_LRU_RT; lru <= VK_LRU_RT + LRU_AGED; lru += LRU_AGED)
        for (Surf* s = g_lru[lru].head; s; s = s->lru_next) {
            if ((s->gva >> 63) || s->gva >= gva + size || s->gva + rt_bytes(s) <= gva || !flush_bytes(s)) continue;
            if (n == cap) {
                Surf** grown = (Surf**)malloc((size_t)cap * 2 * sizeof *grown);
                if (!grown) fatal("Out of memory listing the render targets over %llx", (unsigned long long)gva);
                memcpy(grown, list, (size_t)n * sizeof *grown);
                if (list != local) free(list);
                list = grown;
                cap *= 2;
            }
            u32 i = n++;
            for (; i && list[i - 1]->wseq > s->wseq; i--) list[i] = list[i - 1];
            list[i] = s;
        }
    *out = list;
    return n;
}
static void surf_flush(Surf* s, const u8* original);
/* the targets over s's bytes whose images are older than s's and not written back yet go to guest memory first,
   oldest first, so s's bytes land over theirs as the console's GPU wrote them; newer ones stay pending, on top */
static void rt_flush_older(const Surf* s) {
    Surf *local[16], **list;
    u64 size = rt_bytes(s);
    u32 n, i;
    if ((s->gva >> 63) || !size || !pend_hit_va(&g_rt_pages, s->gva, size)) return;
    n = rt_pending_over(s->gva, size, local, 16, &list);
    for (i = 0; i < n && list[i]->wseq < s->wseq; i++) surf_flush(list[i], NULL);
    if (list != local) free(list);
}
static void surf_flush(Surf* s, const u8* original) {
    u64 size = flush_bytes(s);
    VkBuffer buf;
    if (!size) return;
    rt_flush_older(s);
    vk_render_end();
    const u8* p = vk_readback_buffer(size, &buf);
    if (!p) return;
    VkCommandBuffer cb = vk_cmd();
    if (!flush_record(cb, s, buf, 0)) return;
    flush_wait(cb);
    flush_land(s, p, original, s->wgen);
}
u64 vk_surf_flush_size(const Surf* s) { return flush_bytes(s); }
void vk_surf_flush_record(Surf* s, VkBuffer buf, u64 at) {
    VkCommandBuffer cb;
    rt_flush_older(s);
    vk_render_end();
    cb = vk_cmd();
    flush_record(cb, s, buf, at);
    /* the copy's writes made visible to the host reads that land it, once its submit is done */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
}
void vk_surf_flush_land(Surf* s, const u8* p, const u8* original, u64 wgen) {
    vk_result_begin();
    flush_land(s, p, original, wgen);
    vk_result_end();
}
void vk_surf_flush(Surf* s, const u8* original) {
    vk_result_begin();
    surf_flush(s, original);
    vk_result_end();
}
void vk_rt_flush(u64 gva) { vk_surf_flush(vk_rt_find(gva), NULL); }
/* A storage image's results kept on the GPU (docs/GPU_OWNERSHIP.md), as the DMA engine's destination is: its span
   staged as the GPU sees it into a buffer of its own (the bytes between its texels keep theirs), its texels laid out
   over that in the guest's layout, and the buffer published over the span run by run of guest memory. Nothing waits:
   the bytes land when the CPU touches them. A 3D volume a game writes twice a frame and only samples took a wait
   each, a fifth of the run. A pitch-linear one's rows go at the guest's pitch, over the bytes between them as the
   guest has them. 0 when it can't be (pitch-linear rows of part texels, or layers not a pitch's rows apart, no room):
   the caller flushes at once */
int vk_surf_publish(Surf* s) {
    u64 size = flush_bytes(s), at, off = 0;
    u32 mem_wgen = s ? s->mem_wgen : 0;
    VkBuffer staged;
    VkOwnRes* r;
    VkCommandBuffer cb;
    if (!size || (s->gva >> 63)) return 0;
    if (!s->bl) {
        if (s->pitch % s->f.bpp || s->type == VK_IMAGE_VIEW_TYPE_3D ||
            (s->layers > 1 && s->layer_size != (u64)s->pitch * s->h))
            return 0;
        size = rt_bytes(s);
    }
    vk_render_end();
    r = vk_own_res_new(size);
    if (!r) return 0;
    u8* p = vk_stage(size, vk_stage_align(), &staged, &at);
    if (!p) {
        vk_own_res_unref(r);
        return 0;
    }
    vk_guest_read(s->gva, size, p, staged, at);
    /* the other targets over the span: s's own pixels go over them below, in the guest's layout, so a target leaves
       itself out (its tight rows would be a pitch-linear one's padding) */
    s->mem_wgen = s->wgen;
    vk_rt_overlay(s->gva, size, staged, at);
    s->mem_wgen = mem_wgen;
    cb = vk_cmd();
    VkBufferCopy c = {at, 0, size};
    rec_CmdCopyBuffer(cb, staged, r->buf, 1, &c);
    vk_all_barrier(cb);
    if (!s->bl)
        flush_copy(cb, s, r->buf, 0, s->pitch);
    else if (!flush_record(cb, s, r->buf, 0)) {
        vk_own_res_unref(r);
        return 0;
    }
    vk_all_barrier(cb);
    /* The buffer now holds this image. Publishing it retires older images over
       the same backing, and must not flush this image a second time. */
    s->mem_wgen = s->wgen;
    while (off < size) {
        u64 cpu = 0, n = gmmu_run(s->gva + off, size - off, &cpu);
        if (!n) { /* an unmapped page takes no write */
            n = 0x1000 - ((s->gva + off) & 0xFFF);
            if (n > size - off) n = size - off;
        } else
            vk_own_publish(cpu, n, r, off);
        off += n;
    }
    gpu_mem_written(s->gva, size);
    vk_own_res_unref(r);
    /* guest memory (its owner) has the image's pixels now: the texture cache finds them current, by their owner */
    s->gw = gpu_mem_generation();
    vk_surf_relist(s);
    if (!s->is_rt) vk_tex_note_writeback(s);
    return 1;
}

u64 g_vk_rt_readbacks; /* render targets written back to guest memory because the GPU's copy couldn't take them */
int vk_rt_overlay(u64 gva, u64 size, VkBuffer buf, u64 at) {
    Surf *local[16], **list;
    int n = 0;
    u32 count, i;
    if (!size || !pend_hit_va(&g_rt_pages, gva, size)) return 0;
    count = rt_pending_over(gva, size, local, 16, &list);
    for (i = 0; i < count; i++) {
        Surf* s = list[i];
        if (s->gva >= gva && s->gva + rt_bytes(s) <= gva + size) {
            vk_render_end();
            if (flush_record(vk_cmd(), s, buf, at + (s->gva - gva))) {
                n++;
                continue;
            }
        }
        /* reaching past the range, or its copy refused: written back to guest memory, the copy keeping the bytes
           the caller read. A target past the range is mostly one whose memory a smaller surface took over since, and
           the program's writes there are newer than its pixels */
        g_vk_rt_readbacks++;
        vk_surf_flush(s, NULL);
    }
    if (list != local) free(list);
    return n;
}

void vk_rt_before_write(u64 gva, u64 size) {
    Surf *local[16], **list;
    u32 count, i;
    if (!size || g_rt_flushing) return;
    vk_lock();
    g_rt_flushing = 1;
    if (pend_hit_va(&g_rt_pages, gva, size)) {
        count = rt_pending_over(gva, size, local, 16, &list);
        for (i = 0; i < count; i++) vk_surf_flush(list[i], NULL);
        if (list != local) free(list);
    }
    g_rt_flushing = 0;
    vk_unlock();
}

typedef struct {
    Surf** surfaces;
    u32 count, capacity;
} RtPendingList;

/* Collect under the GMMU alias lock, then flush after it is released. A flush
   writes guest memory and asks the alias index about those writes itself. */
static void rt_pending_alias(void* ctx, u64 gva, u64 size) {
    RtPendingList* pending = ctx;
    Surf *local[16], **list;
    u32 count;
    if (!pend_hit_va(&g_rt_pages, gva, size)) return;
    count = rt_pending_over(gva, size, local, 16, &list);
    for (u32 i = 0; i < count; i++) {
        Surf* s = list[i];
        u32 at = 0;
        while (at < pending->count && pending->surfaces[at]->wseq < s->wseq) at++;
        if (at < pending->count && pending->surfaces[at] == s) continue;
        if (pending->count == pending->capacity) {
            u32 capacity = pending->capacity * 2;
            if (capacity < pending->capacity || (u64)capacity > SIZE_MAX / sizeof *pending->surfaces)
                fatal("Too many render target aliases");
            Surf** grown = malloc((size_t)capacity * sizeof *grown);
            if (!grown) fatal("Out of memory collecting render target aliases");
            memcpy(grown, pending->surfaces, (size_t)pending->count * sizeof *grown);
            if (pending->capacity > 16) free(pending->surfaces);
            pending->surfaces = grown;
            pending->capacity = capacity;
        }
        memmove(pending->surfaces + at + 1, pending->surfaces + at,
                (size_t)(pending->count - at) * sizeof *pending->surfaces);
        pending->surfaces[at] = s;
        pending->count++;
    }
    if (list != local) free(list);
}

/* A producer can replace image memory through another GPU address. Land every
   older image over that backing before publishing the new owner, so a later
   partial image read cannot write its old pixels over the producer's result. */
void vk_rt_before_write_cpu(u64 cpu, u64 size) {
    Surf* local[16];
    RtPendingList pending = {local, 0, 16};
    if (!size || g_rt_flushing) return;
    vk_lock();
    g_rt_flushing = 1;
    gmmu_aliases(cpu, size, rt_pending_alias, &pending);
    for (u32 i = 0; i < pending.count; i++) vk_surf_flush(pending.surfaces[i], NULL);
    if (pending.surfaces != local) free(pending.surfaces);
    g_rt_flushing = 0;
    vk_unlock();
}

static void zt_restore(Surf* s);
/* Another format over the same bytes: one game draws its scene to an sRGB target and the overlay over it to a UNORM
   one at the same address, and presents that. The target taking the address starts from the pixels of the one it
   parks (fresh: newer than memory) when they are newer than its own and both images hold the guest's bytes as they
   are, in the same shape. Memory, which neither target's draws wrote, doesn't have them. 1 when it took them */
static int rt_take_pixels(Surf* to, Surf* from, int fresh) {
    if (!from || from == to || !fresh || from->layout == VK_IMAGE_LAYOUT_UNDEFINED || from->wseq <= to->wseq ||
        from->f.depth || to->f.depth || !from->f.raw || !to->f.raw || from->f.bpp != to->f.bpp || from->w != to->w ||
        from->h != to->h || from->sw != to->sw || from->sh != to->sh || from->bl != to->bl || from->bh != to->bh ||
        from->bd != to->bd || from->pitch != to->pitch || from->layers != to->layers ||
        from->layer_size != to->layer_size || from->volume || to->volume)
        return 0;
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, to->layers},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, to->layers},
                     .extent = {to->sw, to->sh, 1}};
    rec_CmdCopyImage(cb, from->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &c);
    LOG_LIMIT(10, "vulkan: the %ux%u render target at %llx in format %x takes the pixels of the one in format %x\n",
              to->w, to->h, (unsigned long long)to->gva, to->fkey, from->fkey);
    vk_surf_wrote(to);
    if (from->mem_wgen == from->wgen) to->mem_wgen = to->wgen;
    /* drawn as from was: fresh as long as memory isn't written after from's baseline */
    vk_rt_drawn(to);
    to->draw_wseen = from->draw_wseen;
    to->draw_gw = from->draw_gw;
    to->draw_has_hash = from->draw_has_hash;
    to->draw_hash = from->draw_hash;
    return 1;
}
static Surf* rt_create_(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth, u32 pitch, u32 layers, u64 layer_size,
                        int volume);
/* a render target's lookup or creation as a profiler zone (SNX_TRACY builds) */
static Surf* rt_create(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth, u32 pitch, u32 layers, u64 layer_size,
                       int volume) {
    SNX_ZONE(z, "render target");
    Surf* s = rt_create_(gva, w, h, fmt, tile, depth, pitch, layers, layer_size, volume);
    SNX_ZONE_END(z);
    return s;
}
static Surf* rt_create_(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth, u32 pitch, u32 layers, u64 layer_size,
                        int volume) {
    if (!w || !h) return NULL;
    u32 fkey = fmt | (depth ? 0x10000 : 0);
    /* the layout is part of what a target is: block linear with its block height, or pitch linear with its pitch */
    u32 bl = !((tile >> 12) & 1), bh = bl ? (tile >> 4) & 15 : 0, bd = bl && volume ? (tile >> 8) & 15 : 0;
    while (bd && layers <= (1u << (bd - 1))) bd--;
    if (bl)
        pitch = 0;
    else if (!pitch)
        pitch = w * (depth ? vk_zt_format(fmt) : vk_rt_format(fmt)).bpp;
    VkFmt layout_fmt = depth ? vk_zt_format(fmt) : vk_rt_format(fmt);
    u64 min_layer_size = bl ? bl_size(w * layout_fmt.bpp, h, bh) : (u64)pitch * h;
    if (layer_size < min_layer_size) layer_size = min_layer_size;
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == gva && s->is_rt && s->w == w && s->h == h && s->fkey == fkey && s->bl == bl && s->bh == bh &&
            s->pitch == pitch && s->layers == layers && s->layer_size == layer_size && s->bd == bd &&
            s->volume == volume) {
            vk_rt_note_read(s);
            return s;
        }
    /* another size, format or layout at this address: park the current one, reuse a parked one that fits */
    Surf* parked = NULL;
    /* the one parked here, whose freshness is asked while it holds the address (not when only the size changes, as
       dynamic resolution does every frame in some games) */
    Surf* cur = depth ? NULL : vk_rt_find(gva);
    int cur_fresh = cur && cur->w == w && cur->h == h && cur->layers == layers && !volume && vk_rt_drawn_fresh(cur);
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == (gva | 1ull << 63) && s->is_rt && s->w == w && s->h == h && s->fkey == fkey && s->bl == bl &&
            s->bh == bh && s->pitch == pitch && s->layers == layers && s->layer_size == layer_size && s->bd == bd &&
            s->volume == volume && !parked)
            parked = s;
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == gva && s->is_rt) {
            rt_pages(s, -1);
            rtx_del(gva);
            s->gva = gva | 1ull << 63;
            g_parked++;
            g_vk_surf_epoch++;
        }
    if (parked) {
        parked->gva = gva;
        rtx_add(parked);
        g_parked--;
        g_vk_surf_epoch++;
        rt_pages(parked, 1);
        vk_rt_note_read(parked);
        rt_take_pixels(parked, cur, cur_fresh);
        return parked;
    }
    VkFmt f = depth ? vk_zt_format(fmt) : vk_rt_format(fmt);
    Surf* s = surf_create(gva, w, h, f, fkey, bl, bh, pitch, 1, layers);
    if (!s) return NULL;
    rtx_add(s);
    s->layer_size = layer_size;
    s->bd = bd;
    s->volume = volume;
    s->rt_size = rt_bytes(s);
    rt_pages(s, 1);
    s->gw = gpu_mem_generation();
    /* the CPU may have written the memory already (a cleared or decoded frame): start from it. A depth target never
       comes from memory, but a loaded save state may hold its pixels */
    if (depth)
        zt_restore(s);
    else if (!rt_take_pixels(s, cur, cur_fresh))
        vk_rt_reload(gva);
    return s;
}

Surf* vk_rt(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth, u32 pitch) {
    return rt_create(gva, w, h, fmt, tile, depth, pitch, 1, 0, 0);
}

Surf* vk_rt_array(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, u32 pitch, u32 layers, u64 layer_size, int volume) {
    return rt_create(gva, w, h, fmt, tile, 0, pitch, layers, layer_size, volume);
}

Surf* vk_zt_array(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, u32 layers, u64 layer_size) {
    return rt_create(gva, w, h, fmt, tile, 1, w * vk_zt_format(fmt).bpp, layers, layer_size, 0);
}

void vk_rt_note_read(Surf* s) {
    vk_surf_touch(s);
    if (!s->is_rt) return;
    /* writes the 3D engine's draws don't see: I2M uploads, constant updates and compute results landing there go
       to memory, which DMA and 2D copies would load (vk_rt_reload) but these don't */
    if (s->gw == gpu_mem_generation()) return;
    if (!gpu_mem_dirty_since(s->gw, s->gva, rt_bytes(s))) {
        s->gw = gpu_mem_generation(); /* still unwritten as of now: the next check asks only about later writes */
        return;
    }
    vk_rt_reload(s->gva);
    if (s->gw != gpu_mem_generation())
        LOG_LIMIT(1, "vulkan: could not reload render target at %llx after its guest memory changed\n",
                  (unsigned long long)s->gva);
}

/* the GPU addresses showing a guest address (gmmu_aliases), up to RT_CPU_MAX: one more says there were more */
#define RT_CPU_MAX 32
typedef struct {
    u64 gva[RT_CPU_MAX];
    u32 n;
} RtCpuAt;
static void rt_cpu_at(void* ctx, u64 gva, u64 n) {
    RtCpuAt* q = ctx;
    (void)n;
    if (q->n < RT_CPU_MAX)
        q->gva[q->n++] = gva;
    else
        q->n = RT_CPU_MAX + 1;
}
/* whether s, a target showing the frame, is a better one than best: of the frame's size, else any; the most recently
   used of those, and of two used in the same present the one first used later, as the recency lists order them */
static int rt_cpu_better(const Surf* s, int exact, const Surf* best, int best_exact) {
    if (!best || exact != best_exact) return !best || exact > best_exact;
    i32 d = (i32)((u32)s->last_used - (u32)best->last_used);
    return d > 0 || (d == 0 && s->touched > best->touched);
}
Surf* vk_rt_for_cpu(u64 cpu_addr, u32 w, u32 h) {
    Surf* best = NULL;
    int best_exact = 0;
    RtCpuAt q;
    /* Each present asked this of every render target, translating its address: 3,600 of them in one game, 1.5% of
       its GPU thread. The GPU addresses showing the frame, from the guest side, find the live targets there instead */
    q.n = 0;
    gmmu_aliases(cpu_addr, 1, rt_cpu_at, &q);
    if (q.n <= RT_CPU_MAX) {
        for (u32 i = 0; i < q.n; i++) {
            Surf* s = vk_rt_find(q.gva[i]);
            if (!s || s->f.depth || s->layout == VK_IMAGE_LAYOUT_UNDEFINED || gmmu_translate(s->gva) != cpu_addr)
                continue;
            int exact = s->w == w && s->h == h;
            if (rt_cpu_better(s, exact, best, best_exact)) {
                best = s;
                best_exact = exact;
            }
        }
        if (best) vk_rt_note_read(best);
        return best && best->layout != VK_IMAGE_LAYOUT_UNDEFINED ? best : NULL;
    }
    for (int list = VK_LRU_RT + LRU_AGED; list >= VK_LRU_RT; list -= LRU_AGED)
        for (Surf* s = g_lru[list].head; s; s = s->lru_next) {
            if (s->f.depth || (s->gva >> 63) || s->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
                gmmu_translate(s->gva) != cpu_addr)
                continue;
            int exact = s->w == w && s->h == h;
            /* the lists run to the most recently used: of two used in the same present, the later is the one first
               used later */
            if (!best || exact > best_exact ||
                (exact == best_exact && (i32)((u32)s->last_used - (u32)best->last_used) >= 0)) {
                best = s;
                best_exact = exact;
            }
        }
    if (best) vk_rt_note_read(best);
    return best && best->layout != VK_IMAGE_LAYOUT_UNDEFINED ? best : NULL;
}

/* render targets drawn since they were made, for a texture made of them (gpu/vk/texture.c), in order of the address
   they were drawn at (drawn_from). Every texture bind looks up a range in it: a game keeping 6,500 targets made a walk
   of the whole list three quarters of the GPU thread, where a binary search finds the range's first */
static Surf** g_drawn;
static int g_ndrawn, g_capdrawn;
/* the first drawn target at or past address a */
static int drawn_lower(u64 a) {
    int lo = 0, hi = g_ndrawn;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (g_drawn[mid]->drawn_from < a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
/* the GPU pages they started at when listed, so a texture over none of them (most) skips the list: one game looked at
   each of about 30 for each of 13,000 textures a frame. A target parked since still counts its old page, which only
   lets the walk run */
static PendPages g_drawn_pages;
/* Read backing memory directly: asking gmem_read to land pending output here could submit a draw mid-setup. A run of
   pages contiguous in host memory is hashed at once, eight bytes at a time (vk_hash) */
static u64 rt_mem_hash(const Surf* s) {
    u64 h = 1469598103934665603ull;
    u64 size = rt_bytes(s);
    for (u64 o = 0; o < size;) {
        u64 a = s->gva + o, cpu = gmmu_translate(a), n = 0x1000 - (a & 0xFFF);
        if (n > size - o) n = size - o;
        while (cpu && o + n < size && gmmu_translate(a + n) == cpu + n) /* the next page follows in host memory */
            n += size - o - n < 0x1000 ? size - o - n : 0x1000;
        /* an unmapped page counts as its length, pages the GPU holds as their owners (they are trapped) */
        if (!cpu || !own_hash(cpu, n, &h))
            h = snx_fnv_step(h, cpu ? vk_hash(GP(cpu), n) : n);
        o += n;
    }
    return h;
}
/* Pitch-linear color targets drawn since the last release. A target's pixels reach guest memory only when something
   asks for them, and the CPU can't ask. Reading back on the CPU what the GPU drew is what pitch-linear targets are
   mostly for, and they are few: one game draws its ambient light's spherical harmonics into seven 8x8 RGBA32F targets
   and reads them after a fence, and lit its scenes with the old bytes there, values up to 1e31. A release publishes
   them, GPU-owned (vk_surf_publish): the GPU copies them over their guest bytes, and they land only if the CPU
   touches those. A block linear target the CPU reads still gets no write-back */
static Surf** g_linear;
static u32 g_nlinear, g_caplinear;
static void linear_note(Surf* s) {
    if (s->bl || s->f.depth || s->in_linear) return;
    if (g_nlinear == g_caplinear) {
        u32 cap = g_caplinear ? g_caplinear * 2 : 16;
        Surf** n = realloc(g_linear, (size_t)cap * sizeof *n);
        if (!n) return;
        g_linear = n;
        g_caplinear = cap;
    }
    g_linear[g_nlinear++] = s;
    s->in_linear = 1;
}
static void linear_drop(Surf* s) {
    u32 i = 0;
    if (!s->in_linear) return;
    while (i < g_nlinear && g_linear[i] != s) i++;
    if (i < g_nlinear) g_linear[i] = g_linear[--g_nlinear];
    s->in_linear = 0;
}
int own_traps(void);
void vk_rt_publish_linear(void) {
    /* SWR: without memory traps (our runtime: own_init says none) a publish can't own the span, and the fallback writes
       the target's whole span over guest memory at once, the bytes between its rows as the GPU copy has them: CPU data
       there would be overwritten. Metroid Dread never needed it (the earlier renderer had no such write-back), so only
       with traps, or SWR_VK_PUBLISH_LINEAR=1 (tests). Precaution: the acid-enemy crash was da6776e's clamp (render.c) */
    static int force = -1;
    if (force < 0) force = getenv("SWR_VK_PUBLISH_LINEAR") != NULL;
    if (!force && !own_traps()) {
        while (g_nlinear) g_linear[--g_nlinear]->in_linear = 0;
        return;
    }
    /* publishing can submit and land, which leaves the list alone: it is taken whole first */
    while (g_nlinear) {
        Surf* s = g_linear[--g_nlinear];
        s->in_linear = 0;
        if (!(s->gva >> 63) && flush_bytes(s) && !vk_surf_publish(s)) vk_surf_flush(s, NULL);
    }
}
static u64 g_drawn_max; /* the biggest span a listed target has: how far before an address one over it can start */
/* Whether every byte of s's span was drawn over after s last changed, by targets drawn later, each over its own span:
   memory holds their pixels now (or would on the console), so s's are no one's to read and it goes without loss. A
   game rendering at a dynamic resolution makes its scene's targets again at another size each frame over the same
   memory, and each old one held a draw memory never got: kept as precious, a few thousand took the budget from the
   targets that are (one game's ink maps, drawn once at a level's start, went under that pressure with the budget
   lowered, and its walls took no ink). The drawn list is in order of address, so the targets over s are a sweep from its start */
static int rt_superseded(const Surf* s) {
    u64 lo = s->gva & ~(1ull << 63), hi = lo + s->rt_size, reach = lo;
    if (!s->is_rt || !s->rt_size) return 0;
    for (int i = drawn_lower(lo > g_drawn_max ? lo - g_drawn_max : 0); i < g_ndrawn && g_drawn[i]->drawn_from <= reach;
         i++) {
        const Surf* o = g_drawn[i];
        if (o == s || o->draw_wseq <= s->wseq || o->drawn_from + o->rt_size <= reach) continue;
        reach = o->drawn_from + o->rt_size;
        if (reach >= hi) return 1;
    }
    return 0;
}
void vk_rt_drawn(Surf* s) {
    linear_note(s);
    s->draw_wseq = s->wseq;
    if (s->rt_size > g_drawn_max) g_drawn_max = s->rt_size;
    /* A memory write after the first draw can leave untouched pixels stale even when later draws touch the target.
       Keep that first baseline until the next present instead of scanning the whole target on every draw. */
    if (!s->draw_wseen || s->drawn_at != g_vk_present) {
        u64 size = rt_bytes(s);
        /* without write tracking, a hash of the whole target: only for one a texture takes pixels from (feeds_tex),
           where it is the one answer vk_rt_drawn_fresh has. Hashing every target on its first draw of each present
           took most of the GPU thread (FreeBSD, Linux before 6.7) */
        /* Dirty metadata needs the mapping's address, without acquiring pixels
           that can still be GPU-owned. The hash fallback acquires its own data. */
        s->draw_has_hash = vk_written_since(gmem_host(s->gva, size), size, 0) < 0 && s->feeds_tex;
        if (s->draw_has_hash) s->draw_hash = rt_mem_hash(s);
        s->draw_wseen = vk_ww_now();
        s->draw_gw = gpu_mem_generation();
    }
    s->drawn_at = g_vk_present;
    if (s->in_drawn) return;
    if (g_ndrawn == g_capdrawn) {
        int cap = g_capdrawn ? g_capdrawn * 2 : 64;
        Surf** n = realloc(g_drawn, (size_t)cap * sizeof *n);
        if (!n) return;
        g_drawn = n;
        g_capdrawn = cap;
    }
    s->drawn_from = s->gva;
    int at = drawn_lower(s->drawn_from);
    memmove(&g_drawn[at + 1], &g_drawn[at], (size_t)(g_ndrawn - at) * sizeof *g_drawn);
    g_drawn[at] = s;
    g_ndrawn++;
    s->in_drawn = 1;
    pend_add_va(&g_drawn_pages, s->drawn_from, 1, 1);
}
void vk_rt_drawn_full(Surf* s) {
    s->draw_wseen = 0; /* every pixel was replaced, so earlier memory writes no longer matter */
    vk_rt_drawn(s);
}
int vk_rt_drawn_fresh(Surf* s) {
    u64 size = rt_bytes(s);
    if (!s->draw_wseen || s->layout == VK_IMAGE_LAYOUT_UNDEFINED || gpu_mem_dirty_since(s->draw_gw, s->gva, size))
        return 0;
    if (s->draw_has_hash) return rt_mem_hash(s) == s->draw_hash;
    int written = vk_written_since(gmem_host(s->gva, size), size, s->draw_wseen);
    if (written < 0) {
        /* no write tracking, and no baseline yet: the target's pixels are taken this once, as the GPU's writes to its
           memory are known and the game's own seldom land in a target it draws to. Its next baselines are hashed */
        s->feeds_tex = 1;
        return 1;
    }
    return written == 0;
}
int vk_rt_drawn_in(u64 lo, u64 hi, Surf** out, int max) {
    int n = 0;
    if (hi <= lo || !pend_hit_va(&g_drawn_pages, lo, hi - lo)) return 0;
    for (int i = drawn_lower(lo); i < g_ndrawn && g_drawn[i]->drawn_from < hi; i++) {
        Surf* s = g_drawn[i];
        /* Generated textures may first be sampled several frames after their draw.
           Age does not make guest memory authoritative: tex_take_drawn checks
           vk_rt_drawn_fresh before copying, and surf_free removes evicted targets. A parked target's address (its
           drawn_from with the top bit set) is out of every range */
        if (s->gva == s->drawn_from) {
            if (n < max) out[n] = s;
            n++;
        }
    }
    return n;
}

/* a surface's image and views go when the frame being recorded is done (descriptors in it may name them) */
static void surf_free(Surf* s) {
    g_vk_surf_epoch++;
    if (s->lru) {
        g_held[lru_kind(s)].n--;
        g_held[lru_kind(s)].bytes -= s->bytes;
        lru_unlink(s);
    }
    if (s->is_rt) rt_pages(s, -1);
    if (s->gva >> 63) g_parked--;
    linear_drop(s);
    if (s->in_drawn) {
        int i = drawn_lower(s->drawn_from);
        while (i < g_ndrawn && g_drawn[i] != s) i++;
        if (i < g_ndrawn) {
            memmove(&g_drawn[i], &g_drawn[i + 1], (size_t)(g_ndrawn - i - 1) * sizeof *g_drawn);
            g_ndrawn--;
        }
        pend_add_va(&g_drawn_pages, s->drawn_from, 1, -1);
    }
    for (int i = 0; i < s->nsv; i++) vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, s->sv[i].v, VK_NULL_HANDLE);
    vk_defer_image(s->img, s->mem, s->att, s->att_linear);
    free(s);
}
/* a target holding a draw that memory doesn't have: it stays while its address is its own and memory wasn't written
   over it since, as a texture made there later takes its pixels (tex_take_drawn) and freeing it would lose them. Depth
   draws aren't tracked: a depth target holds one once anything left its layout defined */
static int keeps_drawn(Surf* s) {
    if (!s->is_rt || (s->gva >> 63)) return 0;
    return s->f.depth ? s->layout != VK_IMAGE_LAYOUT_UNDEFINED : vk_rt_drawn_fresh(s);
}
/* a surface whose pixels can be had again once it goes. A texture: loaded from memory, unless it took pixels from
   render targets since. A render target: without defined contents, or with memory holding what it holds (written back
   or loaded since its last draw, in a format that moves between the two; rt_create loads it again), or with every
   byte drawn over by later targets (rt_superseded). A parked one too only then: taken out of parking it gives back
   its own pixels, which memory, written by neither target's draws, doesn't have */
static int lossless(const Surf* s) {
    if (!s->is_rt) return !s->took_drawn;
    if (s->layout == VK_IMAGE_LAYOUT_UNDEFINED) return 1;
    return (!s->f.depth && s->f.raw && s->mem_wgen == s->wgen && moves_scaled(s)) || rt_superseded(s);
}
/* s out of the table and freed, with its copies (gpu/vk/texture.c rt_crop): the device memory that releases */
static u64 surf_evict(Surf* s) {
    if (s->is_rt && !(s->gva >> 63)) rtx_del(s->gva);
    for (Surf** pp = &g_surfh[hkey(s->gva)]; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    u64 bytes = s->bytes;
    for (Surf *c = s->crop, *n; c; c = n) {
        n = c->next;
        bytes += c->bytes;
        surf_free(c);
    }
    surf_free(s);
    vk_defer_bytes(bytes);
    return bytes;
}
u64 vk_surf_reclaim(int kind, u32 before, u64 want, int lossy) {
    /* A render target goes with its contents: guest memory has them as of its last write-back. They aren't written to
       memory as they go: memory is shared by targets the GPU drew over one another, parked and freed ones among them,
       and by textures and buffers that copied from them on the GPU, and the renderer doesn't know them all. Writing an
       evicted target's pixels, as a game's Mii icons wanted (drawn at boot, shown a minute later), put older pixels
       over newer ones in another: a black block over the player character, and red and green blocks over its
       textures. So a target memory doesn't hold goes only under the budget's pressure once nothing lossless is left
       (vk_mem_trim), or after minutes unused (vk_tex_frame), and then not one holding a draw a texture made at its
       address later would take (keeps_drawn), which only the budget's pressure takes.

       From the least recently used on, up to the first used since `before`: with lossy, the aged list first, as it
       holds the oldest of those. A storage image's result still to land holds its surface: those are passed over,
       a few hundred at most a call */
    u64 freed = 0;
    u32 passed = 0;
    if (lossy)
        for (Surf *s = g_lru[kind + LRU_AGED].head, *n; s && freed < want && passed < 512; s = n) {
            n = s->lru_next;
            if ((i32)((u32)s->last_used - before) >= 0) break;
            if (lossy == VK_RECLAIM_IDLE && keeps_drawn(s)) continue;
            if (vk_gfx_image_holds(s))
                passed++;
            else
                freed += surf_evict(s);
        }
    for (Surf *s = g_lru[kind].head, *n; s && freed < want && passed < 512; s = n) {
        n = s->lru_next;
        if ((i32)((u32)s->last_used - before) >= 0) break;
        if (!lossy && !lossless(s)) { /* out of the way of the next lossless eviction, held or not, in order */
            lru_unlink(s);
            lru_link(s, kind + LRU_AGED);
        } else if (lossy == VK_RECLAIM_IDLE && keeps_drawn(s))
            continue;
        else if (vk_gfx_image_holds(s))
            passed++;
        else
            freed += surf_evict(s);
    }
    return freed;
}
/* the render targets last used before `before` that later ones drew all over (rt_superseded), from either list: the
   aged one holds those that went there with pixels memory lacked, which draws since may have made lossless */
u64 vk_rt_reclaim_superseded(u32 before, u64 want) {
    u64 freed = 0;
    for (int lru = VK_LRU_RT + LRU_AGED; lru >= VK_LRU_RT && freed < want; lru -= LRU_AGED)
        for (Surf *s = g_lru[lru].head, *n; s && freed < want; s = n) {
            n = s->lru_next;
            if ((i32)((u32)s->last_used - before) >= 0) break;
            if (s->layout != VK_IMAGE_LAYOUT_UNDEFINED && rt_superseded(s) && !vk_gfx_image_holds(s))
                freed += surf_evict(s);
        }
    return freed;
}

/* ------------------------------------------------------------ the budget */
static u64 g_trimmed;   /* statistics: bytes freed for the budget */
static u32 g_trim_dry;  /* the present a trim last found too little to free (+1: 0 is none) */
static u32 g_forced;    /* the present a failed allocation last forced a trim (+1) */
static u32 g_oom;       /* the present an image allocation last found no memory (+1) */
static u64 g_oom_bytes; /* the smallest size that found none in that present */
u64 vk_mem_trimmed(void) { return g_trimmed; }
void vk_mem_trim(u64 extra, int force) {
    u64 usage, budget, need = 0;
    if (force) {
        /* once a present: what it frees is released when the frame is done, so a second one this present would
           find the same shortfall and take more */
        if (g_forced == g_vk_present + 1) return;
        g_forced = g_vk_present + 1;
    } else if (g_trim_dry == g_vk_present + 1)
        return; /* nothing more to free until the next present */
    vk_mem_use(&usage, &budget);
    usage += extra;
    if (force) { /* the allocation's size, less what evictions before it are releasing already */
        u64 gone = vk_deferred_bytes();
        need = extra > gone ? extra - gone : 0;
    } else if (usage <= budget / 100 * 90)
        return;
    SNX_ZONE_V(z, "vk mem trim", force);
    /* What goes first is what can be had again without loss: textures (loaded again from memory), own buffers (copied
       again), then render targets memory holds. Those unused for a second go down to 80% of the budget, room for what
       comes next. The working set itself (unused for two presents) goes only to stay under 95%: freeing it to 80%
       when it alone is bigger made one game load hundreds of textures and targets again every 600 presents under a
       lowered budget, where staying near the budget costs nothing. Render targets whose pixels exist only here
       (memory has older ones: draws aren't written back, see vk_surf_reclaim) and textures that took such pixels lose
       them when they go, so they go only when the lossless steps leave the heap past 90%: those unused for five
       seconds, then, past 95%, those unused for a second and eight presents, the youngest a lossy entry can be. Every
       step frees the least recently used first. Before them all go the render targets later ones drew all over, unused
       for two presents: what they hold is no one's (rt_superseded) */
    u64 superseded = vk_rt_reclaim_superseded(vk_present_before(0, 2), ~0ull);
    static const struct {
        u32 ms, presents;
        int lossy;
        u32 mark; /* percent of the budget the step frees down to */
    } steps[] = {{1000, 8, 0, 80}, {0, 2, 0, 95}, {5000, 30, 1, 90}, {1000, 8, 1, 95}};
    u64 freed = superseded, by[3] = {0, 0, superseded};
    int short_of = 0;
    for (u32 i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        u64 mark = budget / 100 * steps[i].mark, left = usage > freed ? usage - freed : 0;
        u64 want = left > mark ? left - mark : 0;
        if (freed < need && want < need - freed) want = need - freed;
        if (!want) continue;
        u32 before = vk_present_before((u64)steps[i].ms * 1000000ull, steps[i].presents);
        int level = steps[i].lossy ? VK_RECLAIM_ANY : VK_RECLAIM_LOSSLESS;
        u64 got = vk_surf_reclaim(VK_LRU_TEX, before, want, level);
        by[0] += got;
        if (got < want && !steps[i].lossy) {
            u64 n = vk_buf_reclaim(before, want - got);
            by[1] += n;
            got += n;
        }
        if (got < want) {
            u64 n = vk_surf_reclaim(VK_LRU_RT, before, want - got, level);
            by[2] += n;
            got += n;
        }
        freed += got;
        short_of = got < want;
    }
    g_trimmed += freed;
    if (short_of) g_trim_dry = g_vk_present + 1;
    if (freed || short_of)
        LOG_LIMIT(20,
                  "vulkan: %llu of %llu MB budgeted in use%s: freed %llu MB of textures, %llu of buffers, %llu of "
                  "render targets%s\n",
                  (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20),
                  force ? " (an allocation found no memory)" : "", (unsigned long long)(by[0] >> 20),
                  (unsigned long long)(by[1] >> 20), (unsigned long long)(by[2] >> 20),
                  short_of ? ", all that could go" : "");
}

int vk_cache_image_create(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem, u64* bytes) {
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                         .pCreateInfo = ii};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    VkResult res;
    SNX_ZONE(z, "vk image create");
    vkGetDeviceImageMemoryRequirements(g_vk.dev, &q, &req);
    *bytes = req.memoryRequirements.size;
    SNX_ZONE_VALUE(z, *bytes);
    *img = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
    /* one as big found no memory this present: neither will this one (what was freed meanwhile is released when the
       frame is done). Its draws go without it until the next present */
    if (g_oom == g_vk_present + 1 && *bytes >= g_oom_bytes) return 0;
    vk_mem_trim(*bytes, 0);
    if (vk_image_create_r(ii, img, mem, &res)) return 1;
    if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        if (g_oom != g_vk_present + 1 || *bytes < g_oom_bytes) g_oom_bytes = *bytes;
        g_oom = g_vk_present + 1;
        vk_mem_trim(*bytes, 1); /* room for it by the time it is asked for again, a present or two later */
    }
    return 0;
}

/* A 2D blit whose source is a depth target: the game copies the depth buffer's texels as a color surface of their
   size (Pokemon Legends Z-A blits its D24S8 depth as BGRA8 into another depth buffer, which its lighting tests
   against, and halves it for half-size passes). The texels are copied whole on the console, so here the depth and
   stencil go into a depth target of the source's format at the destination, which the draw that binds it finds.
   Depth targets don't reach guest memory, so a copy through memory would leave it undefined: black lighting. 0 when
   the surfaces don't describe that (another texel size, or a source of another size than the target) */
static int blit_depth(Surf* st, const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy,
                      double sdx, double sdy) {
    u32 fmt = st->fkey & 0xFFFF, tile = (d->linear ? 0x1000u : 0) | (d->bh << 4);
    Surf* dt;
    if (vk_rt_format(s->format).bpp != vk_zt_format(fmt).bpp || s->format != d->format || s->width != st->w ||
        s->height < st->h || !blit_ok(st->f.vk, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return 0;
    vk_surf_touch(st); /* used now: making the destination may evict what wasn't */
    dt = vk_zt_array(d->addr, d->width, d->height, fmt, tile, 1, 0);
    if (!dt || dt == st || dt->f.vk != st->f.vk) return 0;
    float S = g_vk_scale;
    i32 x0 = (i32)(sx * S), y0 = (i32)(sy * S), x1 = (i32)((sx + sdx * dw) * S + 0.5),
        y1 = (i32)((sy + sdy * dh) * S + 0.5);
    i32 X1 = (i32)((dx + dw) * S + 0.5f), Y1 = (i32)((dy + dh) * S + 0.5f);
    dx = (i32)(dx * S);
    dy = (i32)(dy * S);
    if (x1 > (i32)st->sw) x1 = (i32)st->sw;
    if (y1 > (i32)st->sh) y1 = (i32)st->sh;
    if (X1 > (i32)dt->sw) X1 = (i32)dt->sw;
    if (Y1 > (i32)dt->sh) Y1 = (i32)dt->sh;
    if (x1 <= x0 || y1 <= y0 || X1 <= dx || Y1 <= dy || x0 < 0 || y0 < 0 || dx < 0 || dy < 0) return 0;
    vk_rt_note_read(st);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, st, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, dt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    /* one aspect a region; nearest, as filtering packed depth and stencil bits as color would only mix them */
    VkImageBlit b[2] = {{.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                         .srcOffsets = {{x0, y0, 0}, {x1, y1, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                         .dstOffsets = {{dx, dy, 0}, {X1, Y1, 1}}}};
    b[1] = b[0];
    b[1].srcSubresource.aspectMask = b[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    rec_CmdBlitImage(cb, st->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     st->f.stencil ? 2 : 1, b, VK_FILTER_NEAREST);
    vk_surf_wrote(dt);
    if (dx == 0 && dy == 0 && X1 == (i32)dt->sw && Y1 == (i32)dt->sh)
        vk_rt_drawn_full(dt);
    else
        vk_rt_drawn(dt);
    vk_surf_touch(dt);
    return 1;
}

/* the 2D engine's blit between render targets, on the GPU. The surfaces are gpu/gpu.h's
   Surf2D. 0 when it can't be done there: the front end then copies through guest memory */
int vk_blit2d(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
              double sdy) {
    Surf* st = vk_rt_find(s->addr);
    if (st && st->f.depth && st->layout != VK_IMAGE_LAYOUT_UNDEFINED && dw > 0 && dh > 0)
        return blit_depth(st, s, d, dx, dy, dw, dh, sx, sy, sdx, sdy);
    if (!st || st->f.depth || st->layout == VK_IMAGE_LAYOUT_UNDEFINED || dw <= 0 || dh <= 0) return 0;
    vk_surf_touch(st); /* used now: making the destination may evict what wasn't */
    u32 tile = (d->linear ? 0x1000u : 0) | (d->bh << 4);
    Surf* dt = vk_rt(d->addr, d->width, d->height, d->format, tile, 0, d->pitch);
    if (!dt || dt == st || dt->f.depth || st->f.is_int != dt->f.is_int) return 0;
    if (!blit_ok(st->f.vk, VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !blit_ok(dt->f.vk, VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return 0;
    /* in the images' pixels: targets are scaled */
    float S = g_vk_scale;
    i32 x0 = (i32)(sx * S), y0 = (i32)(sy * S), x1 = (i32)((sx + sdx * dw) * S + 0.5),
        y1 = (i32)((sy + sdy * dh) * S + 0.5);
    i32 X1 = (i32)((dx + dw) * S + 0.5f), Y1 = (i32)((dy + dh) * S + 0.5f);
    dx = (i32)(dx * S);
    dy = (i32)(dy * S);
    if (x1 > (i32)st->sw) x1 = (i32)st->sw;
    if (y1 > (i32)st->sh) y1 = (i32)st->sh;
    if (X1 > (i32)dt->sw) X1 = (i32)dt->sw;
    if (Y1 > (i32)dt->sh) Y1 = (i32)dt->sh;
    /* a region starting outside either image (the source's corner can be negative): the front end copies it */
    if (x1 <= x0 || y1 <= y0 || X1 <= dx || Y1 <= dy || x0 < 0 || y0 < 0 || dx < 0 || dy < 0) return 0;
    int scaled = x1 - x0 != X1 - dx || y1 - y0 != Y1 - dy;
    vk_rt_note_read(st);
    if (st->layout == VK_IMAGE_LAYOUT_UNDEFINED) return 0;
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, st, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, dt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{x0, y0, 0}, {x1, y1, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstOffsets = {{dx, dy, 0}, {X1, Y1, 1}}};
    int linear = scaled && !st->f.is_int && blit_ok(st->f.vk, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    rec_CmdBlitImage(cb, st->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    vk_surf_wrote(dt);
    if (dx == 0 && dy == 0 && X1 == (i32)dt->sw && Y1 == (i32)dt->sh)
        vk_rt_drawn_full(dt);
    else
        vk_rt_drawn(dt);
    vk_surf_touch(dt);
    return 1;
}

/* a save state's load: every texture and render target goes, as guest memory under them is the state's now */
void vk_surf_forget_all(void) {
    u32 k;
    for (k = 0; k < SURFH; k++)
        while (g_surfh[k]) surf_evict(g_surfh[k]);
}

/* a save state's save: every render target whose pixels memory doesn't hold yet writes them back, so the state's guest
   memory has them (one a format the CPU can't take, a depth target or a scaled one, can't: it has none to write) */
static int flush_wanted(const Surf* s) {
    return s->is_rt && !(s->gva >> 63) && s->layout != VK_IMAGE_LAYOUT_UNDEFINED && s->mem_wgen != s->wgen &&
           flush_bytes(s);
}
u32 vk_rt_flush_all(void) {
    u32 n = 0, i;
    int list;
    Surf** all;
    /* gathered first: a write-back moves its target in the lists */
    for (list = VK_LRU_RT; list <= VK_LRU_RT + LRU_AGED; list += LRU_AGED)
        for (Surf* s = g_lru[list].head; s; s = s->lru_next) n += flush_wanted(s);
    if (!n || !(all = (Surf**)malloc(sizeof *all * n))) return 0;
    n = 0;
    for (list = VK_LRU_RT; list <= VK_LRU_RT + LRU_AGED; list += LRU_AGED)
        for (Surf* s = g_lru[list].head; s; s = s->lru_next)
            if (flush_wanted(s)) all[n++] = s;
    for (i = 0; i < n; i++) vk_surf_flush(all[i], NULL);
    free(all);
    return n;
}

/* ---- a save state's depth targets. Their pixels never go to guest memory and never come from it (rt_create), so a
   state holds them itself: each one live at the save, its depth and its stencil read back once (the one readback a
   save makes), and put into the target of the same place, format and size the loaded game makes next ---- */
typedef struct {
    u64 gva;
    u32 w, h, sw, sh, fkey, vk, layers;
    u64 dsize, ssize; /* the depth aspect's bytes and the stencil's (0 for none), all layers */
    u8* bytes;        /* the depth's, then the stencil's */
} ZtSaved;
static ZtSaved* g_zt;
static u32 g_nzt;
/* the bytes a texel of each aspect takes in a buffer copy, and whether it has stencil */
static u32 zt_depth_bytes(VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D16_UNORM_S8_UINT ? 2 : 4; }
static int zt_stencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}
static int zt_saved(const Surf* s) {
    return s->is_rt && s->f.depth && !(s->gva >> 63) && s->layout != VK_IMAGE_LAYOUT_UNDEFINED && !s->volume &&
           s->type != VK_IMAGE_VIEW_TYPE_3D;
}
static void zt_drop(void) {
    u32 i;
    for (i = 0; i < g_nzt; i++) free(g_zt[i].bytes);
    free(g_zt);
    g_zt = NULL;
    g_nzt = 0;
}
/* each aspect of every layer of s copied between its image and buf at at, the depth's then the stencil's: to the
   buffer (out) or from it */
static void zt_copy(VkCommandBuffer cb, Surf* s, VkBuffer buf, u64 at, int out) {
    u64 dsize = (u64)s->sw * s->sh * zt_depth_bytes(s->f.vk);
    u32 layer, a, aspects = zt_stencil(s->f.vk) ? 2 : 1;
    for (a = 0; a < aspects; a++)
        for (layer = 0; layer < s->layers; layer++) {
            VkBufferImageCopy c = {.bufferOffset = at,
                                   .imageSubresource = {a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0,
                                                        layer, 1},
                                   .imageExtent = {s->sw, s->sh, 1}};
            if (out)
                rec_CmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
            else
                rec_CmdCopyBufferToImage(cb, buf, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            at += a ? (u64)s->sw * s->sh : dsize;
        }
}
int vk_zt_save(StateIo* io) {
    u64 total = 0, at = 0;
    u32 n = 0, i;
    int list, r = 0;
    Surf** all;
    VkBuffer buf;
    u8* p;
    for (list = VK_LRU_RT; list <= VK_LRU_RT + LRU_AGED; list += LRU_AGED)
        for (Surf* s = g_lru[list].head; s; s = s->lru_next)
            if (zt_saved(s)) {
                n++;
                total += (u64)s->sw * s->sh * s->layers * (zt_depth_bytes(s->f.vk) + (zt_stencil(s->f.vk) ? 1u : 0u));
            }
    if (!n) return io_put32(io, 0);
    if (!(all = (Surf**)malloc(sizeof *all * n))) return -1;
    n = 0;
    for (list = VK_LRU_RT; list <= VK_LRU_RT + LRU_AGED; list += LRU_AGED)
        for (Surf* s = g_lru[list].head; s; s = s->lru_next)
            if (zt_saved(s)) all[n++] = s;
    if (!(p = vk_readback_buffer(total, &buf))) {
        free(all);
        return -1;
    }
    vk_render_end(); /* the copies can't be recorded inside a rendering pass */
    {
        VkCommandBuffer cb = vk_cmd();
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                               .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        for (i = 0; i < n; i++) {
            Surf* s = all[i];
            vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
            zt_copy(cb, s, buf, at, 1);
            at += (u64)s->sw * s->sh * s->layers * (zt_depth_bytes(s->f.vk) + (zt_stencil(s->f.vk) ? 1u : 0u));
        }
        rec_CmdPipelineBarrier2(cb, &di);
    }
    vk_finish();
    vk_readback_ready();
    r |= io_put32(io, n);
    for (i = 0, at = 0; i < n && !r; i++) {
        Surf* s = all[i];
        u64 dsize = (u64)s->sw * s->sh * s->layers * zt_depth_bytes(s->f.vk),
            ssize = zt_stencil(s->f.vk) ? (u64)s->sw * s->sh * s->layers : 0;
        r |= io_put64(io, s->gva) | io_put32(io, s->w) | io_put32(io, s->h) | io_put32(io, s->sw) | io_put32(io, s->sh);
        r |= io_put32(io, s->fkey) | io_put32(io, (u32)s->f.vk) | io_put32(io, s->layers) | io_put64(io, dsize) |
             io_put64(io, ssize) | io_put(io, p + at, dsize + ssize);
        at += dsize + ssize;
    }
    free(all);
    LOG("vulkan: %u depth targets saved (%llu KB)\n", n, (unsigned long long)(total >> 10));
    return r ? -1 : 0;
}
int vk_zt_load(StateIo* io) {
    u32 n, i;
    zt_drop();
    if (io_get32(io, &n) || n > 4096) return -1;
    if (!n) return 0;
    if (!(g_zt = (ZtSaved*)calloc(n, sizeof *g_zt))) return -1;
    for (i = 0; i < n; i++, g_nzt++) {
        ZtSaved* z = &g_zt[i];
        if (io_get64(io, &z->gva) || io_get32(io, &z->w) || io_get32(io, &z->h) || io_get32(io, &z->sw) ||
            io_get32(io, &z->sh) || io_get32(io, &z->fkey) || io_get32(io, &z->vk) || io_get32(io, &z->layers) ||
            io_get64(io, &z->dsize) || io_get64(io, &z->ssize) || z->dsize + z->ssize > (1ull << 32) ||
            !(z->bytes = (u8*)malloc((size_t)(z->dsize + z->ssize + 1))) ||
            io_get(io, z->bytes, z->dsize + z->ssize)) {
            zt_drop();
            return -1;
        }
    }
    return 0;
}
/* s, a depth target just made: the pixels a loaded state holds for one of its place, format and size go into it, once */
static void zt_restore(Surf* s) {
    u32 i;
    for (i = 0; i < g_nzt; i++) {
        ZtSaved* z = &g_zt[i];
        VkCommandBuffer cb;
        VkBuffer buf;
        u64 off;
        u8* to;
        if (z->gva != s->gva || z->w != s->w || z->h != s->h || z->sw != s->sw || z->sh != s->sh ||
            z->fkey != s->fkey || z->vk != (u32)s->f.vk || z->layers != s->layers ||
            z->dsize != (u64)s->sw * s->sh * s->layers * zt_depth_bytes(s->f.vk))
            continue;
        if ((to = vk_stage(z->dsize + z->ssize, 4, &buf, &off)) != NULL) {
            memcpy(to, z->bytes, (size_t)(z->dsize + z->ssize));
            vk_render_end();
            cb = vk_cmd();
            vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
            zt_copy(cb, s, buf, off, 0);
            LOG("vulkan: the depth target at %llx (%ux%u) has its pixels from the save state\n",
                (unsigned long long)s->gva, s->w, s->h);
        }
        free(z->bytes);
        *z = g_zt[--g_nzt];
        return;
    }
}
/* a load's reset: what an earlier state left unmatched goes */
void vk_zt_forget(void) { zt_drop(); }
