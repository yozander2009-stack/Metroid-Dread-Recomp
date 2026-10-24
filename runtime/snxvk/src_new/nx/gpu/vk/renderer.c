/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vulkan renderer: the renderer interface (vk_renderer, the vkr_ functions it points to) and presenting.

   Render targets, clears, draws (gpu/vk/render.c), compute (gpu/vk/compute.c), write-backs to and reloads from
   guest memory, and presenting the game's framebuffer from its render target (or from memory, converted on the CPU,
   when no render target holds it). Everything that records runs under vk_lock(), which is reentrant: submissions and
   presents on the GPU thread (gpu/gpu.c), the menu's refresh on the main thread (vkr_refresh), screenshots on the
   thread asking, and shader results landing on whichever thread reads their memory first. Acquiring a swapchain image
   never waits for the display: with every image still on it or queued for it, that frame isn't shown (counted). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/gpu/renderer.h"

static VkSemaphore g_acquired[VK_FRAMES];
/* the CPU fallback's image, and the last presented frame for screenshots */
static Surf* g_cpu_img;
static Surf* g_last;
static u32 g_last_x, g_last_y, g_last_w, g_last_h;
static u32 g_last_transform;
static void vkr_before_write(u64 gva, u64 size) {
    if (size) vk_release_need(gva, size);
    vk_buf_before_write(gva, size);
    if (size)
        vk_gfx_image_mem_need(gva, size);
    else
        vk_gfx_image_land();
    vk_compute_before_write(gva, size);
    vk_rt_before_write(gva, size);
}
/* pending compute and graphics results and queued releases all count their pages in g_pend_any (pending_pages.h);
   storage images don't, so any of them pending answers yes */
static int vkr_mem_pending(u64 gva, u64 size) {
    int hit;
    if (g_gfx_img_any) return 1;
    vk_lock();
    hit = pend_hit(&g_pend_any, gva, size);
    vk_unlock();
    return hit;
}
static void vkr_gfx_mem_need(u64 gva, u64 size) {
    vk_buf_mem_need(gva, size);
    vk_gfx_image_mem_need(gva, size);
}
/* the GPU range a mapping change is changing, from its begin to its end (size 0 for a change of guest memory) */
static u64 g_mapping_gva, g_mapping_size;
static void vkr_mapping_begin(u64 gva, u64 size, int full) {
    vk_lock();
    g_mapping_gva = gva;
    g_mapping_size = full ? 0 : size;
    vk_release_drain();
    if (full) {
        vk_buf_land_map();
        vk_gfx_image_land();
        vk_compute_land();
    } else {
        vk_buf_mem_need_map(gva, size);
        vk_gfx_image_mem_need(gva, size);
        vk_compute_mem_need_map(gva, size);
    }
}
/* the pages changed count what is pending at the guest pages they show now. A change of guest memory (full) leaves
   the GMMU as it was: pending pages are compared by address, and freed memory is landed first */
static void vkr_mapping_end(void) {
    vk_buf_mapping_changed();
    pend_remap(g_mapping_gva, g_mapping_size);
    g_mapping_size = 0;
    vk_unlock();
}

/* 0 when Vulkan can't run here */
static int vkr_start(void) {
    if (!vk_init()) return 0;
    g_vk_scale = g_settings.resolution_scale;       /* the .ini keeps resolution within 0.25 to 4 (options.c) */
    g_vk_anisotropy = g_settings.anisotropic_filtering; /* the sampler table keeps what it makes: anisotropy applies at start */
    if (g_vk_anisotropy > 1 && !g_vk.feat.samplerAnisotropy)
        LOG("vulkan: this GPU has no anisotropic filtering, so anisotropy=%d does nothing\n", g_vk_anisotropy);
    if (g_vk_scale != 1.0f) LOG("vulkan: render targets at %.2fx\n", g_vk_scale);
    vk_cmd_init();
    vk_pipeline_init();
    vk_own_init(); /* GPU engines' results stay on the GPU where guest memory can be trapped (memory/own.c) */
    g_gpu_before_write = vkr_before_write;
    g_gpu_mapping_begin = vkr_mapping_begin;
    g_gpu_mapping_end = vkr_mapping_end;
    g_gpu_mem_changed = vk_mem_changed;
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!g_opt_headless)
        for (int i = 0; i < VK_FRAMES; i++)
            if (!VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_acquired[i]))) return 0;
    g_renderer_up = 1; /* a renderer is active */
    return 1;
}

/* ------------------------------------------------------------ the front end's calls */
/* a new epoch: the caches that trust what they checked in this one (buffers, constant buffers, shaders) check again */
static void vkr_mem_may_have_changed(void) {
    __atomic_add_fetch(&g_vk_epoch, 1, __ATOMIC_RELAXED);
    vk_ww_new_epoch();
}
static void vkr_draw_texture(GpuChannel* ch) {
    vk_lock();
    vk_render_draw_texture(ch);
    vk_unlock();
}
static void vkr_clear(GpuChannel* ch, u32 v) {
    vk_lock();
    vk_render_clear(ch, v);
    vk_unlock();
}
static void vkr_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 bv, u32 bi, u32 instances) {
    /* a draw predicated on the GPU (gpu.h render_pred) counts too: whether it runs isn't known here without the wait
       the predicate is there to avoid, and the counters made up from this (maxwell/engines.c counter_report) only
       stand in for what the console counts, growing with the work given */
    g_draws++;
    vk_lock();
    vk_render_draw(ch, indexed, topology, first, count, bv, bi, instances);
    vk_unlock();
}
static void vkr_samples_reset(GpuChannel* ch) {
    vk_lock();
    vk_query_reset(ch->id);
    vk_unlock();
}
static void vkr_samples_enable(GpuChannel* ch, int on) {
    vk_lock();
    vk_query_enable(ch->id, on);
    vk_unlock();
}
static int vkr_samples_report(GpuChannel* ch, u64 gva, u32 len, u64 ticks) {
    vk_lock();
    int r = vk_query_report(ch->id, gva, len, ticks);
    vk_unlock();
    return r;
}
static void vkr_method_barrier(int kind) {
    /* Nothing for TILED_CACHE_BARRIER (kind 1) or the sampler and texture header cache invalidations (kind 3), as in
       Eden: a target sampled afterwards gets its barrier when it is bound (vk_surf_use), and headers and samplers are
       read from guest memory at each bind. Nor does any invalidation expire the cached textures: their checks see the
       CPU's writes through the epoch each submission and stream wait starts, and the GPU's through write generations.
       Expiring them at each texture cache invalidation, about a hundred a frame in one game, had every texture bound
       after one check its memory again */
    if (kind == 1 || kind == 3) return;
    vk_lock();
    vk_render_end();
    /* The GPU queue orders work across this barrier, including WAIT_FOR_IDLE. CPU-visible releases are gated on GPU
       completion by the release path instead of stalling this command stream. */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(vk_cmd(), &di);
    vk_unlock();
}
static int vkr_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                    double sdy) {
    vk_lock();
    int r = vk_blit2d(s, d, dx, dy, dw, dh, sx, sy, sdx, sdy);
    vk_unlock();
    return r; /* 0: the front end copies through memory, with the renderer's rt_flush and rt_reload around it */
}
static int vkr_vic_compose(const VicJob* j) { return vk_vic_compose(j); } /* it takes vk_lock */
static int vkr_nvdec_write(const NvdecPic* p) { return vk_nvdec_write(p); }   /* it takes vk_lock */
static void* vkr_nvdec_hw_device(void) { return vk_nvdec_hw_device(); }
static int vkr_dma_copy(const GpuDmaOp* o) { return vk_dma_copy(o); }         /* it takes vk_lock */
static int vkr_blit_mem(const GpuBlit* b) { return vk_blit_mem(b); }          /* it takes vk_lock */
static void vkr_rt_flush(u64 gva) {
    vk_lock();
    vk_gfx_image_mem_need(gva, 1);
    vk_rt_flush(gva);
    vk_unlock();
}
static void vkr_rt_reload(u64 gva) {
    vk_lock();
    vk_rt_reload(gva);
    vk_unlock();
}
static void vkr_dispatch(GpuChannel* ch, const u32* q, u64 code) {
    if (!g_settings.compute_dispatches) return;
    r3d_pending_flush(); /* a merged draw the front end is holding comes first */
    vk_lock();
    vk_dispatch(ch, q, code);
    vk_unlock();
}
/* Pending shader results land before the stream signals or waits. */
static void vkr_before_syncwait(void) {
    vk_buf_land_syncwait();
    vk_gfx_image_land();
    vk_compute_land();
}
static void vkr_stream_incr(u32 id) { vk_release_incr(id); }

/* ------------------------------------------------------------ presenting */
/* the frame from memory when no render target holds it (the game drew it on the CPU, or a video frame): staged as it
   is, deswizzled and converted on the GPU (vk/blocklinear.c, vk/texconv.c) */
static Surf* cpu_frame(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt) {
    if (g_cpu_img && (g_cpu_img->w != w || g_cpu_img->h != h)) {
        vk_defer_image(g_cpu_img->img, g_cpu_img->mem, VK_NULL_HANDLE, g_cpu_img->att);
        if (g_last == g_cpu_img) g_last = NULL;
        free(g_cpu_img);
        g_cpu_img = NULL;
    }
    if (!g_cpu_img) {
        g_cpu_img = calloc(1, sizeof *g_cpu_img);
        if (!g_cpu_img) return NULL;
        g_cpu_img->w = g_cpu_img->sw = w;
        g_cpu_img->h = g_cpu_img->sh = h;
        g_cpu_img->f = vk_rt_format(0xD5); /* RGBA8 */
        g_cpu_img->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .extent = {w, h, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &g_cpu_img->img, &g_cpu_img->mem, NULL))) {
            free(g_cpu_img);
            g_cpu_img = NULL;
            return NULL;
        }
    }
    /* its bytes, staged as they are: a block linear one's whole rows of blocks, a pitch linear one's rows (and
       16 bytes, as a conversion reads whole words and a row of 2 byte pixels may end inside one) */
    u32 bpp = fmt == 4 || fmt == 6 || fmt == 7 ? 2 : 4, row_bytes = w * bpp;
    u32 stride = pitch > row_bytes ? pitch : row_bytes, bl = layout == 3; /* libnx's NvLayout: 3 block linear */
    VkBlMove m = {stride, h, bhl, 0, 0, h, 0, 1, (stride + 15) & ~15u};
    u64 lo = 0, hi = (u64)stride * h, at, cap;
    if (bl) vk_bl_range(&m, &lo, &hi);
    if (cpu_addr >= AS_SIZE || hi > AS_SIZE - cpu_addr) {
        WARN_LIMIT(5, "vi: buffer at %llx runs outside guest memory, not shown\n", (unsigned long long)cpu_addr);
        return NULL;
    }
    /* a pitch linear one whose rows don't start on words is staged a row at a time into rows that do */
    u32 in_pitch = bl ? m.lin_pitch : stride % 4 ? (row_bytes + 3) & ~3u : stride;
    u64 staged_size = bl || in_pitch == stride ? hi : (u64)in_pitch * h;
    VkBuffer staged;
    u8* p = vk_stage(staged_size + 16, vk_stage_align(), &staged, &at);
    if (!p) return NULL;
    /* a CPU address (nvmap's), in the guest's address space: what the GPU holds of it (VIC's picture) is copied on
       the GPU */
    if (bl || in_pitch == stride)
        vk_cpu_read(cpu_addr, hi, p, staged, at);
    else
        for (u32 y = 0; y < h; y++)
            vk_cpu_read(cpu_addr + (u64)y * stride, row_bytes, p + (u64)y * in_pitch, staged, at + (u64)y * in_pitch);
    memset(p + staged_size, 0, 16);
    /* deswizzled and converted to RGBA8 on the GPU, in the scratch buffer: the linear rows, then the converted ones */
    int conv = vk_tc_vi_converts(fmt);
    VkTcShape tc = conv ? vk_tc_vi_shape(fmt, w) : (VkTcShape){.out_texels = in_pitch / 4};
    u64 lin = bl ? (u64)m.lin_pitch * h : 0, out = (lin + 15) & ~15ull;
    u64 need = out + (conv ? (u64)tc.out_texels * 4 * h : 0);
    VkBuffer scratch = need ? vk_bl_scratch(need, &cap) : VK_NULL_HANDLE;
    if (need && !scratch) return NULL;
    VkCommandBuffer cb = vk_cmd();
    VkBuffer from = staged;
    u64 from_at = at;
    if (bl || conv) vk_bl_barrier(cb, 1);
    if (bl) {
        if (!vk_bl_move(cb, &m, 0, staged, at, scratch, 0)) return NULL;
        vk_bl_barrier(cb, 0);
        from = scratch;
        from_at = 0;
    }
    if (conv) {
        if (!vk_tc_convert_vi(cb, fmt, w, h, from, from_at, in_pitch, scratch, out)) return NULL;
        vk_bl_barrier(cb, 0);
        from = scratch;
        from_at = out;
    }
    vk_surf_use(cb, g_cpu_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy c = {.bufferOffset = from_at,
                           .bufferRowLength = tc.out_texels,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {w, h, 1}};
    rec_CmdCopyBufferToImage(cb, from, g_cpu_img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    return g_cpu_img;
}

static int is_srgb(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

static int g_sc_srgb; /* what the swapchain was asked for (the surface may not have it) */
/* the window's surface: lost while the app is in the background (Android), made anew when it comes back */
static u32 g_generation;
static int g_lost;
static u64 g_shown_ns; /* when the game last gave the window a frame (atomic: vkr_refresh reads it unlocked) */
/* a blit's source flipped as the frame's Android transform says (FlipH 1, FlipV 2): the endpoints swapped */
static void flip_src(VkImageBlit* b, u32 transform) {
    if (transform & 1) {
        i32 x = b->srcOffsets[0].x;
        b->srcOffsets[0].x = b->srcOffsets[1].x;
        b->srcOffsets[1].x = x;
    }
    if (transform & 2) {
        i32 y = b->srcOffsets[0].y;
        b->srcOffsets[0].y = b->srcOffsets[1].y;
        b->srcOffsets[1].y = y;
    }
}

/* The window's image: src's g_last_ rectangle (a frame of fw x fh, flipped by transform) letterboxed on black, or
   black alone, with the on-screen controls and the game's menu over it; then the frame's commands are submitted and
   the image presented. A game's frame (game) also moves the per-present bookkeeping, and without src shows nothing.
   Under vk_lock */
/* SWR: the steps of a game present, logged when one takes over 250 ms */
static u64 g_ph[10], g_bk[8];
#define PH(i) (g_ph[i] = plat_ns())
static void show(Surf* src, u32 fw, u32 fh, u32 transform, int game) {
    /* the size the swapchain was made for, not the extent the surface clamped it to: that can stay different */
    int ww, wh, srgb = src ? is_srgb(src->f.vk) : g_sc_srgb, shown = backend_window_shown();
    backend_window_size(&ww, &wh);
    if (shown && (g_lost || backend_window_generation() != g_generation)) {
        vk_finish();
        g_lost = !vk_surface_recreate();
        g_generation = backend_window_generation();
    }
    shown = shown && !g_lost;
    PH(3);
    int tried = 0; /* a create already ran this frame: one that failed waits for the next frame */
    /* a new vsync in the .ini takes a new swapchain, as a new size does */
    if (shown && ww > 0 && wh > 0 &&
        (ww != g_vk.sc_win_w || wh != g_vk.sc_win_h || srgb != g_sc_srgb ||
         __atomic_load_n(&g_settings.vsync, __ATOMIC_RELAXED) != g_vk.sc_vsync)) {
        vk_finish();
        g_sc_srgb = srgb;
        vk_swapchain_create(srgb);
        tried = 1;
    }
    VkSemaphore wait = VK_NULL_HANDLE, signal = VK_NULL_HANDLE;
    u32 idx = 0;
    int recreate = 0, hdr_used = 0;
    if (shown && (src || !game) && (g_vk.swapchain || (!tried && vk_swapchain_create(srgb)))) {
        PH(4);
        VkCommandBuffer cb = vk_cmd(); /* waits for this frame slot, so its acquire semaphore is free */
        PH(5);
        VkSemaphore acq = g_acquired[vk_frame_id() % VK_FRAMES];
        /* no wait: vk_lock is held (guest threads mapping memory take it too), and the game's pace is present_pace's.
           Every image still on the display or queued for it (FIFO behind) leaves this frame unshown */
        VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, 0, acq, VK_NULL_HANDLE, &idx);
        PH(6);
        if (r == VK_NOT_READY || r == VK_TIMEOUT) {
            /* the game faster than the display (vsync=1 with fps_limit over its refresh, or a 59.94 Hz one) */
            static u64 dropped;
            if (game && ++dropped % 600 == 1)
                LOG("vulkan: %llu frames not shown, the display had every swapchain image\n",
                    (unsigned long long)dropped);
        } else if (r == VK_ERROR_OUT_OF_DATE_KHR)
            recreate = 1;
        else if (r == VK_ERROR_SURFACE_LOST_KHR)
            g_lost = 1;
        else if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            wait = acq;
            signal = g_vk.sc_done[idx];
            VkImage sc = g_vk.sc_images[idx];
            VkImage sc_out = sc;
            { /* SWR: HDR output (hdr.c): the frame goes to an SDR image, which the HDR pass then writes to sc_out */
                int vk_present_pass_wanted(void);
                VkImage vk_hdr_mid(int srgb);
                if (vk_present_pass_wanted()) { /* HDR output and/or FXAA */
                    VkImage m = vk_hdr_mid(src ? is_srgb(src->f.vk) : 0);
                    if (m) {
                        sc = m;
                        hdr_used = 1;
                    }
                }
            }
            if (src)
                vk_surf_use(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT);
            /* the submit waits for the acquire at the clear, the image's first use */
            vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue black = {{0, 0, 0, 1}};
            VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            rec_CmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
            vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            /* letterbox: the largest rectangle of the frame's aspect that fits the window's part clear of the system's
               bars, centered there; the clear part scales with the extent the surface may have clamped */
            u32 sw = g_vk.sc_extent.width, sh = g_vk.sc_extent.height, cx = 0, cy = 0, cw = sw, ch = sh, dw, dh;
            int sx, sy, sww, swh;
            backend_window_safe(&sx, &sy, &sww, &swh);
            if (g_vk.sc_win_w > 0 && g_vk.sc_win_h > 0 && sww > 0 && swh > 0 && sx + sww <= g_vk.sc_win_w &&
                sy + swh <= g_vk.sc_win_h) {
                cx = (u32)((u64)sw * (u32)sx / (u32)g_vk.sc_win_w);
                cy = (u32)((u64)sh * (u32)sy / (u32)g_vk.sc_win_h);
                cw = (u32)((u64)sw * (u32)sww / (u32)g_vk.sc_win_w);
                ch = (u32)((u64)sh * (u32)swh / (u32)g_vk.sc_win_h);
            }
            if (src && fw && fh) {
                dw = cw;
                dh = (u32)((u64)cw * fh / fw);
                if (dh > ch) {
                    dh = ch;
                    dw = (u32)((u64)ch * fw / fh);
                }
                i32 dx = (i32)(cx + (cw - dw) / 2), dy = (i32)(cy + (ch - dh) / 2);
                VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .srcOffsets = {{(i32)g_last_x, (i32)g_last_y, 0},
                                                   {(i32)(g_last_x + g_last_w), (i32)(g_last_y + g_last_h), 1}},
                                    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .dstOffsets = {{dx, dy, 0}, {dx + (i32)dw, dy + (i32)dh, 1}}};
                /* SWR: a frame larger than its place goes through the averaged shrink (hdr.c vk_shrink), then 1:1 */
                int vk_shrink_wanted(u32 sw, u32 sh, u32 dw, u32 dh);
                VkImage vk_shrink(VkCommandBuffer cb, Surf* src, u32 sx, u32 sy, u32 sw, u32 sh, u32 dw, u32 dh);
                VkImage shr = vk_shrink_wanted(g_last_w, g_last_h, dw, dh)
                                  ? vk_shrink(cb, src, g_last_x, g_last_y, g_last_w, g_last_h, dw, dh)
                                  : VK_NULL_HANDLE;
                if (shr) {
                    blit.srcOffsets[0] = (VkOffset3D){0, 0, 0};
                    blit.srcOffsets[1] = (VkOffset3D){(i32)dw, (i32)dh, 1};
                }
                flip_src(&blit, transform); /* render targets stay in guest order */
                rec_CmdBlitImage(cb, shr ? shr : src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                                 shr ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
            }
            if (sc == sc_out) {
                int safe[4] = {(int)cx, (int)cy, (int)cw, (int)ch};
                vk_ui_draw(cb, sc, safe);
            }
            if (sc != sc_out) {
                void vk_hdr_resolve(VkCommandBuffer cb, VkImage sc);
                PH(7);
                vk_hdr_resolve(cb, sc_out); /* SWR: the SDR image to the HDR swapchain image, left in PRESENT_SRC */
                PH(8);
            } else
                vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        } else
            VK_OK(r);
    }
    if (game) {
        /* the time from the last present, for the stats line: frames slower than 50 ms (a hitch at any pace a game
           runs at) and the slowest, since the last line */
        static u64 last_present_ns, slow_frames, slowest_ns;
        /* the counters as of the last present: a slow frame says what it did, which names what a hitch waited for */
        static u64 was[10];
        u64 now_ns = plat_ns(), is[10];
        vk_pipeline_stats(&is[0], &is[1], &is[2], &is[3], &is[4], &is[5]);
        vk_tex_stats(&is[6], &is[7]);
        is[8] = vk_rt_made();
        is[9] = g_vk_finish_ns / 1000000;
        if (last_present_ns) {
            u64 d = now_ns - last_present_ns;
            if (d > 50000000) {
                slow_frames++;
                LOG_LIMIT(200,
                          "vulkan: a %llu ms frame: %llu shaders translated (%llu ms), %llu pipelines made (%llu ms waited "
                          "for), %llu render targets made, %llu texture uploads (%llu KB), %llu ms in finishes\n",
                          (unsigned long long)(d / 1000000), (unsigned long long)(is[4] - was[4]),
                          (unsigned long long)(is[5] - was[5]), (unsigned long long)(is[0] - was[0]),
                          (unsigned long long)(is[3] - was[3]), (unsigned long long)(is[8] - was[8]),
                          (unsigned long long)(is[6] - was[6]), (unsigned long long)((is[7] - was[7]) >> 10),
                          (unsigned long long)(is[9] - was[9]));
            }
            if (d > slowest_ns) slowest_ns = d;
        }
        memcpy(was, is, sizeof was);
        last_present_ns = now_ns;
        g_vk_present++;
        g_bk[0] = plat_ns();
        { void vk_dlss_frame(void); vk_dlss_frame(); } /* SWR */
        g_bk[1] = plat_ns();
        vk_mem_present();
        g_bk[2] = plat_ns();
        if (g_last) vk_surf_touch(g_last); /* what screenshots read stays */
        g_bk[3] = plat_ns();
        vk_tex_frame();
        g_bk[4] = plat_ns();
        vk_buf_frame();
        g_bk[5] = plat_ns();
        vk_mem_trim(0, 0);
        g_bk[6] = plat_ns();
        if (g_vk_present % 600 == 0) {
            char cs_why[256];
            u64 cs_skipped = vk_cs_skipped(cs_why, sizeof cs_why);
            LOG("vulkan: %llu draws, %llu skipped; %llu dispatches in %llu batches (%llu ms waiting), %llu skipped%s; "
                "%llu finishes (%llu ms); %llu reads past queued releases; %llu descriptor sets written, %llu reused; "
                "%llu draws kept the last one's shaders and pipeline (%llu checked)\n",
                (unsigned long long)g_vk_drawn, (unsigned long long)g_vk_skipped, (unsigned long long)g_vk_dispatches,
                (unsigned long long)g_vk_cs_batches,
                (unsigned long long)(__atomic_load_n(&g_vk_cs_wait_ns, __ATOMIC_RELAXED) / 1000000),
                (unsigned long long)cs_skipped, cs_why, (unsigned long long)g_vk_finishes,
                (unsigned long long)(g_vk_finish_ns / 1000000),
                (unsigned long long)__atomic_load_n(&g_vk_release_forwards, __ATOMIC_RELAXED),
                (unsigned long long)g_vk_dset_new, (unsigned long long)g_vk_dset_reused,
                (unsigned long long)g_vk_draw_kept, (unsigned long long)g_vk_draw_checked);
            {
                u64 pipes, hits, make_ms, pipe_ms, shaders, shader_ms, uploads, upload_bytes;
                vk_pipeline_stats(&pipes, &hits, &make_ms, &pipe_ms, &shaders, &shader_ms);
                vk_tex_stats(&uploads, &upload_bytes);
                LOG("vulkan: %llu shaders translated (%llu ms), %llu pipelines made (%llu from the cache, %llu ms "
                    "making, %llu ms waited for), %llu render targets made, %llu texture uploads (%llu MB); %llu "
                    "frames over 50 ms in the last 600, the slowest %llu ms\n",
                    (unsigned long long)shaders, (unsigned long long)shader_ms, (unsigned long long)pipes,
                    (unsigned long long)hits, (unsigned long long)make_ms, (unsigned long long)pipe_ms,
                    (unsigned long long)vk_rt_made(), (unsigned long long)uploads,
                    (unsigned long long)(upload_bytes >> 20), (unsigned long long)slow_frames,
                    (unsigned long long)(slowest_ns / 1000000));
                slow_frames = slowest_ns = 0;
                u64 early_made, early_hits, early_left;
                vk_pipeline_prewarm_stats(&early_made, &early_hits, &early_left);
                if (early_made || early_left)
                    LOG("vulkan: %llu pipelines of earlier runs made in the background (%llu from the cache), %llu "
                        "still to make\n",
                        (unsigned long long)early_made, (unsigned long long)early_hits,
                        (unsigned long long)early_left);
            }
            {
                /* each memory heap's use and the driver's budget for it (VK_EXT_memory_budget), and our own memory in
                   it (VMA's blocks): past the budget the driver moves memory, and the heap the CPU maps (the BAR window)
                   is the one to watch. NVIDIA counts the whole system's use of the window as ours, and budgets what
                   is left of it (cmd.c bar_room) */
                VmaBudget b[VK_MAX_MEMORY_HEAPS];
                const VkPhysicalDeviceMemoryProperties* mp;
                char line[256];
                size_t at = 0;
                vmaGetHeapBudgets(g_vk.vma, b);
                vmaGetMemoryProperties(g_vk.vma, &mp);
                for (u32 q = 0; q < mp->memoryHeapCount && at < sizeof line; q++)
                    at += (size_t)snprintf(line + at, sizeof line - at, "%s heap %u %llu of %llu MB (%llu ours)",
                                           q ? "," : "", q, (unsigned long long)(b[q].usage >> 20),
                                           (unsigned long long)(b[q].budget >> 20),
                                           (unsigned long long)(b[q].statistics.blockBytes >> 20));
                LOG("vulkan: memory used and budgeted:%s\n", line);
            }
            {
                /* what the caches hold, which vk_mem_trim keeps within the budget */
                u32 nrt, ntex, nbuf, parked, none;
                u64 brt, btex, bbuf, usage, budget, trimmed;
                vk_surf_stats(VK_LRU_RT, &nrt, &brt, &parked);
                vk_surf_stats(VK_LRU_TEX, &ntex, &btex, &none);
                vk_buf_stats(&nbuf, &bbuf);
                vk_mem_use(&usage, &budget);
                trimmed = vk_mem_trimmed();
                LOG("vulkan: caches: %u render targets (%llu MB, %u parked), %u textures (%llu MB), %u buffers (%llu "
                    "MB); %llu of %llu MB in use as they count it, %llu MB freed for the budget so far\n",
                    nrt, (unsigned long long)(brt >> 20), parked, ntex, (unsigned long long)(btex >> 20), nbuf,
                    (unsigned long long)(bbuf >> 20), (unsigned long long)(usage >> 20),
                    (unsigned long long)(budget >> 20), (unsigned long long)(trimmed >> 20));
            }
            LOG("vulkan: %llu draws sampled a target they render to in place (%llu barriers in passes, %llu passes "
                "begun again), %llu through a copy\n",
                (unsigned long long)g_vk_fb_draws, (unsigned long long)g_vk_fb_barriers,
                (unsigned long long)g_vk_fb_restarts, (unsigned long long)g_vk_rt_copies);
            /* what the GPU engines keep on the GPU (docs/GPU_OWNERSHIP.md), and what the CPU had to wait for */
            LOG("vulkan: GPU-owned guest memory landed for the CPU %llu times, render targets written back for an "
                "engine %llu times\n",
                (unsigned long long)own_landings(), (unsigned long long)g_vk_rt_readbacks);
        }
    }
    /* Offscreen frames still submit and advance the frame ring. g_last remains available for readback. */
    /* the acquired image's first use: the clear, or with the SWR present pass (HDR / FXAA) its colour attachment write */
    PH(9);
    vk_submit(wait, hdr_used ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_CLEAR_BIT, signal);
    if (game) vk_renderdoc_present(g_vk_present);
    vk_frame_next(); /* every show used this frame's slot and acquire semaphore, the menu's own too */
    if (signal) {
        /* after the frame's submission, whose semaphore it waits on, without waiting for its recording */
        VkResult r = vk_rec_present(g_vk.swapchain, idx, signal);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !g_vk.sc_turned))
            recreate = 1;
        else if (r == VK_ERROR_SURFACE_LOST_KHR)
            g_lost = 1;
        else
            VK_OK(r);
    }
    if (recreate && !g_lost && !g_vk.sc_lost) { /* not when this frame's create found the surface lost */
        vk_finish();
        vk_swapchain_create(srgb);
    }
    if (g_vk.sc_lost) { /* a create found the surface lost: the next frame makes it anew */
        g_vk.sc_lost = 0;
        g_lost = 1;
    }
}

/* the game queued a buffer for display: show it, letterboxed to the window. A frame that can't be shown (minimized,
   no image to draw to) is still submitted, with the per-present bookkeeping, so frames don't pile up */
static void vkr_present_buffer(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                               u32 transform) {
    memset(g_ph, 0, sizeof g_ph);
    memset(g_bk, 0, sizeof g_bk);
    u64 ph0 = plat_ns();
    vk_lock();
    PH(0);
    vk_buf_writable_abort(); /* a draw abandoned during setup has no shader writes to land */
    vk_render_end();
    PH(1);
    Surf* src = NULL;
    if (w && h && w <= g_vk.props.limits.maxImageDimension2D && h <= g_vk.props.limits.maxImageDimension2D) {
        src = vk_rt_for_cpu(cpu_addr, w, h);
        if (!src || src->f.is_int) src = cpu_frame(cpu_addr, w, h, pitch, layout, bhl, fmt);
    }
    /* the part of the buffer shown: vi's crop, or all of it */
    u32 x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (crop[2] > crop[0] && crop[3] > crop[1] && crop[0] >= 0 && crop[1] >= 0) {
        x0 = (u32)crop[0] < w ? (u32)crop[0] : 0;
        y0 = (u32)crop[1] < h ? (u32)crop[1] : 0;
        x1 = (u32)crop[2] < w ? (u32)crop[2] : w;
        y1 = (u32)crop[3] < h ? (u32)crop[3] : h;
    }
    if (src) { /* the render target found for the buffer can be smaller than it: a crop outside it shows all of it */
        if (x1 > src->w) x1 = src->w;
        if (y1 > src->h) y1 = src->h;
        if (x1 <= x0 || y1 <= y0) {
            x0 = y0 = 0;
            x1 = w < src->w ? w : src->w;
            y1 = h < src->h ? h : src->h;
        }
    }
    if (src) { /* in its pixels: targets are scaled */
        g_last = src;
        g_last_transform = transform;
        g_last_x = (u32)((u64)src->sw * x0 / src->w);
        g_last_y = (u32)((u64)src->sh * y0 / src->h);
        g_last_w = (u32)((u64)src->sw * x1 / src->w) - g_last_x;
        g_last_h = (u32)((u64)src->sh * y1 / src->h) - g_last_y;
    }
    PH(2);
    show(src, x1 - x0, y1 - y0, transform, 1);
    __atomic_store_n(&g_shown_ns, plat_ns(), __ATOMIC_RELAXED);
    vk_unlock();
    {
        u64 end = plat_ns();
        if (end - ph0 > 250000000ull) {
            static const char* const nm[10] = {"lock", "render end", "find frame", "window checks", "swapchain",
                                               "frame slot", "acquire", "draw", "present pass", "bookkeeping"};
            char line[512];
            size_t at = 0;
            u64 prev = ph0;
            for (int i = 0; i < 10 && at < sizeof line; i++)
                if (g_ph[i]) {
                    at += (size_t)snprintf(line + at, sizeof line - at, " %s %.1f", nm[i], (g_ph[i] - prev) / 1e6);
                    prev = g_ph[i];
                }
            if (at < sizeof line) snprintf(line + at, sizeof line - at, " submit+present %.1f", (end - prev) / 1e6);
            LOG("vulkan: slow present %.0f ms, steps (ms):%s\n", (end - ph0) / 1e6, line);
            if (g_bk[6])
                LOG("vulkan: slow present bookkeeping (ms): stats %.1f dlss %.1f mem_present %.1f surf_touch %.1f "
                    "tex_frame %.1f buf_frame %.1f mem_trim %.1f\n",
                    (g_bk[0] - g_ph[8]) / 1e6, (g_bk[1] - g_bk[0]) / 1e6, (g_bk[2] - g_bk[1]) / 1e6,
                    (g_bk[3] - g_bk[2]) / 1e6, (g_bk[4] - g_bk[3]) / 1e6, (g_bk[5] - g_bk[4]) / 1e6,
                    (g_bk[6] - g_bk[5]) / 1e6);
        }
    }
}

/* The last frame again, under the controls and the menu, while the game shows none: a game loading, stuck, or drawing
   nothing still has them. Not before a quarter second without a frame (a slow game's own come between), at 30 a second
   at most, and only when the GPU thread holds no Vulkan work: then the next call tries again */
static void vkr_refresh(void) {
    static u64 last;
    u64 now = plat_ns();
    if (!backend_window_shown() || now - __atomic_load_n(&g_shown_ns, __ATOMIC_RELAXED) < 250000000ull ||
        now - last < 33000000ull)
        return;
    if (!vk_trylock()) return;
    last = now;
    vk_render_end();
    show(g_last, g_last_w, g_last_h, g_last_transform, 0);
    vk_unlock();
}

/* a part of a surface, blitted to RGBA8 and read back: its hash, and a .ppm at path if given */
static u64 surf_save(Surf* s, u32 x, u32 y, u32 w, u32 h, u32 transform, const char* path) {
    u64 hash = 1469598103934665603ull;
    /* blit to an RGBA8 image, whatever the target's format, then read that back. sRGB to sRGB, so the bytes stay
       encoded as the display shows them */
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = is_srgb(s->f.vk) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage tmp;
    VmaAllocation tmem;
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &tmp, &tmem, NULL))) return hash;
    VkBuffer buf;
    u8* p = vk_readback_buffer((u64)w * h * 4, &buf);
    if (!p) {
        vmaDestroyImage(g_vk.vma, tmp, tmem);
        return hash;
    }
    vk_render_end(); /* the blit, the copy and their barriers can't be recorded inside a rendering pass */
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{(i32)x, (i32)y, 0}, {(i32)(x + w), (i32)(y + h), 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {(i32)w, (i32)h, 1}}};
    flip_src(&blit, transform); /* screenshots show the window's transform, offscreen and headless presents too */
    rec_CmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_NEAREST);
    vk_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    rec_CmdCopyImageToBuffer(cb, tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    vk_finish();
    vk_readback_ready();
    vmaDestroyImage(g_vk.vma, tmp, tmem);
    /* the pixels as the device wrote them, little-endian words: the hash is the same on every host */
    const u8* px = p;
    for (u64 i = 0; i < (u64)w * h; i++) hash = (hash ^ snx_ld_le32(px + i * 4)) * 1099511628211ull;
    if (path) {
        char fn[1100];
        snprintf(fn, sizeof fn, "%s.ppm", path);
        FILE* f = fopen(fn, "wb");
        int ok = f != NULL;
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (u64 i = 0; i < (u64)w * h && ok; i++) {
                u8 rgb[3] = {px[i * 4], px[i * 4 + 1], px[i * 4 + 2]};
                ok = fwrite(rgb, 1, 3, f) == 3;
            }
            ok = fclose(f) == 0 && ok;
        }
        if (!ok) WARN_LIMIT(1, "vulkan: the screenshot %s couldn't be written\n", fn);
    }
    return hash;
}
/* the last presented frame as PPM (path.ppm), and a hash of its pixels */
static u64 vkr_screenshot(const char* path) {
    vk_lock();
    u64 hash = g_last ? surf_save(g_last, g_last_x, g_last_y, g_last_w, g_last_h, g_last_transform, path)
                      : 1469598103934665603ull;
    vk_unlock();
    return hash;
}

/* the stall watchdog: the releases queued and how far the submissions they wait for have got */
static void vk_report_stall(void) {
    u64 n, gate, cgate, gsub, gdone, csub, cdone;
    vk_release_queued(&n, &gate, &cgate);
    vk_submit_progress(&gsub, &gdone);
    vk_compute_progress(&csub, &cdone);
    WARN("vulkan: %llu releases queued (the oldest waits for graphics submission %llu and compute batch %llu), "
         "graphics submissions done %llu of %llu, compute batches %llu of %llu, %llu bytes of commands not recorded\n",
         (unsigned long long)n, (unsigned long long)gate, (unsigned long long)cgate, (unsigned long long)gdone,
         (unsigned long long)gsub, (unsigned long long)cdone, (unsigned long long)csub,
         (unsigned long long)vk_rec_behind());
}
const Renderer vk_renderer = {
    .name = "Vulkan",
    .start = vkr_start,
    .mem_may_have_changed = vkr_mem_may_have_changed,
    .clear = vkr_clear,
    .draw_texture = vkr_draw_texture,
    .draw = vkr_draw,
    .method_barrier = vkr_method_barrier,
    .blit = vkr_blit,
    .vic_compose = vkr_vic_compose,
    .nvdec_write = vkr_nvdec_write,
    .nvdec_hw_device = vkr_nvdec_hw_device,
    .dma_copy = vkr_dma_copy,
    .blit_mem = vkr_blit_mem,
    .rt_flush = vkr_rt_flush,
    .rt_reload = vkr_rt_reload,
    .dispatch = vkr_dispatch,
    .cs_mem_need = vk_compute_mem_need,
    .mem_pending = vkr_mem_pending,
    .gfx_mem_need = vkr_gfx_mem_need,
    .before_syncwait = vkr_before_syncwait,
    .stream_incr = vkr_stream_incr,
    .release_write = vk_release_write,
    .release_need = vk_release_need,
    .release_read = vk_release_read,
    .release_value = vk_release_value,
    .render_predicate = vk_render_predicate,
    .render_predicate_drop = vk_render_predicate_drop,
    .release_retire = vk_release_retire,
    .report_stall = vk_report_stall,
    .samples_reset = vkr_samples_reset,
    .samples_enable = vkr_samples_enable,
    .samples_report = vkr_samples_report,
    .stream_end = vk_release_submit,
    .present_buffer = vkr_present_buffer,
    .screenshot = vkr_screenshot,
    .refresh = vkr_refresh,
    .save = vk_pipeline_save,
    .release_window = vk_window_release,
    .prepare = vk_pipeline_prepare,
};
const Renderer* g_renderer = &vk_renderer;
