/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The render target set, clears and draws. The 3D registers name up to 8 color targets (RT_CONTROL at 0x487 maps
   the slots, each target's address, size, format and tiling at 0x200 + 0x10 * index) and a depth target (enabled
   at 0x54E, address at 0x3F8, size at 0x48A, format and tiling at 0x3FA). A dynamic rendering pass stays open while
   draws and clears keep the same set, and ends before anything that copies or transitions images.

   A draw reads the 3D engine's registers for its state. Signed viewport scales and coordinate swizzles map the
   guest's position. Window-origin bit 4 controls triangle facing independently of that mapping. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/options.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/shader/shader.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/gpu/vk/internal_spv.h"

static struct {
    Surf* rt[8];
    Surf* zt;
    u32 n; /* color slots up to the last bound one */
    VkExtent2D ext;
    int open;
    int srgb_write;
    int zt_ro; /* the depth target is read-only in this pass: draws also sample it */
    /* feedback loops (feedback_order): the color slots in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL, those of them written
       since the pass began or its last barrier, and those sampled since then */
    u32 fb, fb_written, fb_read;
} g_rp;
u64 g_vk_fb_draws, g_vk_fb_barriers, g_vk_fb_restarts, g_vk_rt_copies;

void vk_render_end(void) {
    if (!g_rp.open) return;
    g_rp.open = 0;
    VkCommandBuffer cb = vk_cmd();
    vk_query_pass_end(cb); /* a query begun in the pass ends in it */
    rec_CmdEndRendering(cb);
    vk_gpuprof_pass_end(cb);
}

/* the targets the registers name, without opening anything */
static void read_targets(GpuChannel* ch, Surf** rt, Surf** zt, u32* n) {
    u32* r = ch->r3d;
    u32 ctl = r[0x487], count = ctl & 15;
    if (count > 8) count = 8;
    *n = 0;
    for (u32 i = 0; i < 8; i++) {
        rt[i] = NULL;
        if (i >= count) continue;
        u32* t = &r[0x200 + ((ctl >> (4 + i * 3)) & 7) * 0x10];
        u64 a = ((u64)t[0] << 32) | t[1];
        if (a && t[2] && t[3] && t[4]) {
            /* a pitch-linear target (bit 12 of the memory layout) holds its row pitch in bytes where the width is */
            u32 bpp = vk_rt_format(t[4]).bpp, linear = (t[5] >> 12) & 1, w = linear ? t[2] / bpp : t[2];
            /* COLOR_TARGET_THIRD_DIMENSION (bits 0-27) names the number of renderable array slices, or a volume's
               depth when the memory word's THIRD_DIMENSION_CONTROL (bit 16, NVIDIA's clb197.h) says so: its slices
               are then GOB slices of a 3D surface, and the array pitch says nothing. One game's 64x64x64 light
               volume taken as 64 layers its 1 MiB array pitch apart covered 63 MiB of other memory. Vertex shaders
               may select one with their layer output (the color LUT passes render all slices in one draw). */
            u32 layers = t[6] & 0x0fffffff;
            if (!layers || layers > g_vk.props.limits.maxImageArrayLayers) layers = 1;
            if (w)
                rt[i] = vk_rt_array(a, w, t[3], t[4], t[5], linear ? t[2] : w * bpp, layers, (u64)t[7] * 4,
                                    (t[5] >> 16) & 1);
            if (rt[i]) *n = i + 1;
        }
    }
    *zt = NULL;
    if (r[0x54E]) {
        u64 za = ((u64)r[0x3F8] << 32) | r[0x3F9];
        if (za && r[0x48A] && r[0x48B]) {
            /* ZETA_SIZE's dimension control can force one layer; otherwise its depth is the array size. */
            u32 layers = r[0x48C] & (1u << 16) ? 1 : r[0x48C] & 0xffff;
            if (!layers || layers > g_vk.props.limits.maxImageArrayLayers) layers = 1;
            *zt = vk_zt_array(za, r[0x48A], r[0x48B], r[0x3FA], r[0x3FB], layers, (u64)r[0x3FC] * 4);
        }
    }
}

/* opens a rendering pass on these targets, or keeps the open one if it is the same set. zt_ro: the depth target is
   bound read-only, so draws can sample it too (soft particles test against depth they also read). fb: color slots the
   draw samples, which the pass needs in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL (feedback_order) */
/* SwitchRecomp: DLSS (dlss.c) and SNX_FRAME_MAP render-pass diagnostics */
static void fmap_end(void);
static void fmap_begin(Surf** rt, Surf* zt, u32 n);
void vk_dlss_pass(Surf** rt, u32 n);
void vk_dlss_draw_begin(void);
void vk_dlss_jitter(Surf* zt, int depth_write, int depth_equal, VkViewport* vp);
int vk_dlss_capture_begin(VkCommandBuffer cb, Surf* zt, int depth_write, int plain_tris, u64 key, u32 verts,
                          const VkViewport* vp, const VkViewport* vpj);
void vk_dlss_capture_end(VkCommandBuffer cb);
void vk_dlss_draw(Surf* rt0, Surf* zt, int depth_test);
/* SNX_FRAME_MAP=N logs every render pass of present N (targets, sizes, formats, draws), to find the scene color and
   depth before post-processing and the UI */
static int g_fmap_pass, g_fmap_draws;
int vk_fmap_on(void);
int vk_fmap_on(void) {
    static int at = -2;
    if (at == -2) { const char* e = getenv("SNX_FRAME_MAP"); at = e ? atoi(e) : -1; }
    return at >= 0 && g_vk_present == (u32)at;
}
static void fmap_end(void) {
    if (vk_fmap_on() && g_fmap_pass) LOG("framemap:   pass %d: %d draws\n", g_fmap_pass, g_fmap_draws);
}
static void fmap_begin(Surf** rt, Surf* zt, u32 n) {
    if (!vk_fmap_on()) return;
    g_fmap_pass++; g_fmap_draws = 0;
    for (u32 i = 0; i < n; i++)
        if (rt[i]) LOG("framemap: pass %d color%u gva=%llx %ux%u (image %ux%u) fmt=%x\n", g_fmap_pass, i, (unsigned long long)rt[i]->gva, rt[i]->w, rt[i]->h, rt[i]->sw, rt[i]->sh, rt[i]->fkey);
    if (zt) LOG("framemap: pass %d depth  gva=%llx %ux%u (image %ux%u) fmt=%x\n", g_fmap_pass, (unsigned long long)zt->gva, zt->w, zt->h, zt->sw, zt->sh, zt->fkey);
}
void vk_fmap_draw(void);
void vk_fmap_draw(void) { if (vk_fmap_on()) g_fmap_draws++; }
void vk_fmap_read(u64 gva, u32 w, u32 h, u32 fkey);
void vk_fmap_read(u64 gva, u32 w, u32 h, u32 fkey) {
    if (vk_fmap_on()) LOG("framemap: pass %d reads %llx %ux%u fmt=%x\n", g_fmap_pass, (unsigned long long)gva, w, h, fkey);
}
static int render_begin(Surf** rt, Surf* zt, u32 n, int zt_ro, int srgb_write, u32 fb) {
    fmap_end();
    fmap_begin(rt, zt, n);
    vk_dlss_pass(rt, n); /* SwitchRecomp: DLSS runs on the scene target before the pass after it */
    srgb_write = !!srgb_write;
    if (!n && !zt) return 0;
    if (!zt) zt_ro = 0;
    int same = g_rp.open && g_rp.n == n && g_rp.zt == zt && g_rp.zt_ro == zt_ro && g_rp.srgb_write == srgb_write &&
               !memcmp(g_rp.rt, rt, sizeof g_rp.rt);
    if (same && !(fb & ~g_rp.fb)) return 1;
    if (same) g_vk_fb_restarts++; /* only a target's layout changes */
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    u32 loops = 0;
    VkRenderingAttachmentInfo ca[8];
    VkExtent2D ext = {~0u, ~0u};
    for (u32 i = 0; i < n; i++) {
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                            .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (!rt[i]) continue;
        /* a target left in the feedback-loop layout stays in it: draws keep sampling the same targets pass after pass,
           and moving it back would end the next such pass at its first feedback draw */
        int loop = g_vk.has_feedback &&
                   ((fb >> i & 1) || rt[i]->layout == VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT);
        VkImageLayout layout =
            loop ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        /* the fragment shader's reads are in the barrier's second scope, so the pass begins with every earlier write
           visible to them */
        vk_surf_use(cb, rt[i], layout,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                        (loop ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : 0),
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                        (loop ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0));
        ca[i].imageView = vk_rt_attachment(rt[i], srgb_write);
        if (!ca[i].imageView) return 0;
        ca[i].imageLayout = layout;
        if (loop) loops |= 1u << i;
        if (rt[i]->sw < ext.width) ext.width = rt[i]->sw;
        if (rt[i]->sh < ext.height) ext.height = rt[i]->sh;
    }
    VkRenderingAttachmentInfo za = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkImageLayout zl =
        zt_ro ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    /* a read-only depth target keeps its contents: a store would write it at the end of the pass, which the read
       access vk_surf_use records doesn't cover, and the next barrier wouldn't wait for it */
    if (zt_ro) za.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    if (zt) {
        if (zt_ro)
            vk_surf_use(cb, zt, zl,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                            VK_SAMPLED_STAGES,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        else
            vk_surf_use(cb, zt, zl,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        za.imageView = zt->att;
        za.imageLayout = zl;
        if (zt->sw < ext.width) ext.width = zt->sw;
        if (zt->sh < ext.height) ext.height = zt->sh;
    }
    u32 layers = ~0u;
    for (u32 i = 0; i < n; i++)
        if (rt[i] && rt[i]->layers < layers) layers = rt[i]->layers;
    if (zt && zt->layers < layers) layers = zt->layers;
    if (layers == ~0u) layers = 1;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, ext},
                          .layerCount = layers,
                          .colorAttachmentCount = n,
                          .pColorAttachments = ca,
                          .pDepthAttachment = zt ? &za : NULL,
                          .pStencilAttachment = zt && zt->f.stencil ? &za : NULL};
    vk_gpuprof_pass_begin(cb, n ? rt[0] : NULL, zt);
    rec_CmdBeginRendering(cb, &ri);
    memcpy(g_rp.rt, rt, sizeof g_rp.rt);
    g_rp.zt = zt;
    g_rp.n = n;
    g_rp.ext = ext;
    g_rp.zt_ro = zt_ro;
    g_rp.srgb_write = srgb_write;
    g_rp.fb = loops;
    g_rp.fb_written = g_rp.fb_read = 0;
    g_rp.open = 1;
    return 1;
}
static void target_written(Surf* s) {
    vk_surf_wrote(s);
    vk_rt_drawn(s);
}
/* a draw or clear wrote the depth target, which was undefined (its layout, before the pass, or still) when that is
   pred_undefined. One predicated on the GPU may not have run, and depth is never loaded from memory: until a write
   that surely runs, the target holds nothing a texture may take (tex_take_drawn), as before its first pass. 1 when
   it counts as written */
static int zt_written(Surf* zt, u32 pred, int undefined) {
    if (pred && (undefined || zt->pred_undefined)) {
        zt->pred_undefined = 1;
        return 0;
    }
    zt->pred_undefined = 0;
    return 1;
}

/* A pass with no attachments, for a draw that only captures its vertices (transform feedback with no target): its
   area is as large as a framebuffer may be, so the draw's viewport and scissor fit in it */
static int render_begin_empty(void) {
    if (g_rp.open && !g_rp.n && !g_rp.zt) return 1;
    vk_render_end();
    VkExtent2D ext = {g_vk.props.limits.maxFramebufferWidth, g_vk.props.limits.maxFramebufferHeight};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, ext}, .layerCount = 1};
    vk_gpuprof_pass_begin(vk_cmd(), NULL, NULL);
    rec_CmdBeginRendering(vk_cmd(), &ri);
    memset(g_rp.rt, 0, sizeof g_rp.rt);
    g_rp.zt = NULL;
    g_rp.n = 0;
    g_rp.ext = ext;
    g_rp.zt_ro = 0;
    g_rp.srgb_write = 0;
    g_rp.fb = g_rp.fb_written = g_rp.fb_read = 0;
    g_rp.open = 1;
    return 1;
}

/* Transform feedback's byte counters, a word for each of the 4 buffers: where a capture's end left each buffer, which
   the next draw's capture resumes from (3D 0x1D1; made at the first capture) */
static struct {
    VkBuffer buf;
    VmaAllocation mem;
} g_xfb;
static int xfb_counters(void) {
    if (g_xfb.buf) return 1;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = 16,
                              .usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &aci, &g_xfb.buf, &g_xfb.mem, NULL));
}

/* Feedback loops. A Maxwell draw may sample a color target it renders to: a game may blend in its shaders,
   reading the pixel under the fragment and writing it back, in hundreds of draws a frame. With
   VK_EXT_attachment_feedback_loop_layout the target stays the pass's attachment, in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL,
   which the draw's descriptor names too, and the draw enables the loop for color (dynamic state, or a pipeline flag).
   Each draw still sees the target as all earlier draws left it: before a draw samples a slot written since the last
   barrier, or writes a slot sampled since then, a barrier orders the color attachment writes and fragment shader
   reads before it against those after it, both ways.

   Within a vkCmdBeginRendering pass a barrier is allowed only with dynamicRenderingLocalRead
   (VUID-vkCmdPipelineBarrier2-None-09553), with framebuffer-space stages only (-srcStageMask-09556), by region
   (-dependencyFlags-07891), and with no image barrier on an attachment outside RENDERING_LOCAL_READ or GENERAL
   (-image-09555): so it is a memory barrier, and the layout stays. By region, the specification orders a write at
   (x, y, layer) only before reads by fragments at the same (x, y, layer) ("Framebuffer Region Dependencies"). That is
   all a fragment reading the pixel it covers needs, which is what blending in a shader does. A fragment reading other
   pixels (a blur of the target) isn't ordered by it in the specification: an immediate-mode desktop GPU drains and
   flushes for the barrier as a whole and orders those reads too in practice, while a tiler may order only within its
   tile. Without dynamicRenderingLocalRead, the pass ends and begins again instead, and render_begin's image barrier,
   outside the pass, orders every pixel.

   Eden ends its render pass for each such draw and samples the image in GENERAL (TextureCache::CheckFeedbackLoop,
   BarrierFeedbackLoop). Before this, the target was copied for each draw (vk_rt_snapshot), which stays for devices
   without the extension, for stages before the fragment shader, and for a depth target the draw writes.

   Orders a command of the open pass that samples the color slots in read and writes those in write. 0 when the pass
   could not be begun again */
static int feedback_order(u32 read, u32 write) {
    read &= g_rp.fb;
    write &= g_rp.fb;
    if (!(read & g_rp.fb_written) && !(write & g_rp.fb_read)) return 1;
    if (!g_vk.has_local_read) {
        Surf* rt[8];
        memcpy(rt, g_rp.rt, sizeof rt);
        vk_render_end();
        g_vk_fb_restarts++;
        return render_begin(rt, g_rp.zt, g_rp.n, g_rp.zt_ro, g_rp.srgb_write, g_rp.fb);
    }
    VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    /* FEEDBACK_LOOP says what the barrier is for, to drivers that flush differently for it */
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                           .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT,
                           .memoryBarrierCount = 1,
                           .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(vk_cmd(), &di);
    g_rp.fb_written = g_rp.fb_read = 0;
    g_vk_fb_barriers++;
    return 1;
}
/* a command of the open pass sampled the color slots in read and wrote those in write */
static void feedback_done(u32 read, u32 write) {
    g_rp.fb_read |= read & g_rp.fb;
    g_rp.fb_written |= write & g_rp.fb;
}

/* every dynamic state at a neutral value, for draws of our own */
static void plain_state(VkCommandBuffer cb, VkRect2D sc) {
    vk_render_forget_state(); /* every state set here, differently from a draw */
    VkViewport vp = {0, 0, (float)g_rp.ext.width, (float)g_rp.ext.height, 0, 1};
    rec_CmdSetViewportWithCount(cb, 1, &vp);
    rec_CmdSetScissorWithCount(cb, 1, &sc);
    rec_CmdSetCullMode(cb, VK_CULL_MODE_NONE);
    rec_CmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    rec_CmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    rec_CmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    rec_CmdSetRasterizerDiscardEnable(cb, VK_FALSE);
    rec_CmdSetLineWidth(cb, 1.0f);
    rec_CmdSetDepthBiasEnable(cb, VK_FALSE);
    rec_CmdSetDepthBias(cb, 0, 0, 0);
    float bc[4] = {0, 0, 0, 0};
    rec_CmdSetBlendConstants(cb, bc);
    rec_CmdSetDepthTestEnable(cb, VK_FALSE);
    rec_CmdSetDepthWriteEnable(cb, VK_FALSE);
    rec_CmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    rec_CmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    rec_CmdSetDepthBounds(cb, 0.0f, 1.0f);
    rec_CmdSetStencilTestEnable(cb, VK_FALSE);
    rec_CmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    rec_CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    rec_CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    rec_CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    if (g_vk.has_feedback_dynamic) rec_CmdSetAttachmentFeedbackLoopEnableEXT(cb, VK_IMAGE_ASPECT_NONE);
}

/* a channel's render enable predicate (gpu.h render_pred, 0 none) around one of its draws or clears, inside the
   rendering pass they are in: on the GPU, the comparison it holds decides whether they run */
static void pred_begin(VkCommandBuffer cb, u32 pred) {
    if (pred)
        rec_CmdBeginConditionalRenderingEXT(cb, vk_query_pred_buffer(),
                                            (VkDeviceSize)((pred & ~VK_PRED_EQUAL) - 1) * 4,
                                            (pred & VK_PRED_EQUAL) ? VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT : 0);
}
static void pred_end(VkCommandBuffer cb, u32 pred) {
    if (pred) rec_CmdEndConditionalRenderingEXT(cb);
}

/* a clear of some color components only: a full-screen triangle with that write mask, whose fragment shader writes
   the cleared slot alone, in the target's number type, run on pred */
static int clear_masked(u32 slot, u32 mask, const float* cc, VkRect2D rect, u32 layer, u32 pred) {
    static VkShaderModule vs, fs[8][3];
    const VkFmt* f = &g_rp.rt[slot]->f;
    int num = f->is_int ? (f->sint ? 2 : 1) : 0;
    if (!vs) vs = vk_module_spv(snx_clear_vs_spv, sizeof snx_clear_vs_spv / sizeof *snx_clear_vs_spv);
    if (!fs[slot][num]) {
        static const u32* words[3] = {snx_clear_fs_float_spv, snx_clear_fs_uint_spv, snx_clear_fs_sint_spv};
        static const size_t count[3] = {sizeof snx_clear_fs_float_spv / sizeof *snx_clear_fs_float_spv,
                                        sizeof snx_clear_fs_uint_spv / sizeof *snx_clear_fs_uint_spv,
                                        sizeof snx_clear_fs_sint_spv / sizeof *snx_clear_fs_sint_spv};
        fs[slot][num] = vk_module_spv_location(words[num], count[num], slot);
    }
    if (!vs || !fs[slot][num]) return 0;
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs;
    k.fs = fs[slot][num];
    k.topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    k.polygon = VK_POLYGON_MODE_FILL;
    k.ncolor = g_rp.n;
    for (u32 i = 0; i < g_rp.n; i++)
        k.color[i] =
            g_rp.rt[i] ? (g_rp.srgb_write ? g_rp.rt[i]->f.vk : vk_srgb_as(g_rp.rt[i]->f.vk, 0)) : VK_FORMAT_UNDEFINED;
    k.blend[slot].mask = ((mask & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((mask & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                         ((mask & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((mask & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
    if (g_rp.zt) {
        k.depth = g_rp.zt->f.vk;
        k.stencil = g_rp.zt->f.stencil ? g_rp.zt->f.vk : VK_FORMAT_UNDEFINED;
    }
    VkPipeline p = vk_pipeline_get(&k);
    if (!p) return 0;
    VkCommandBuffer cb = vk_cmd();
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    plain_state(cb, rect);
    u32 pc[8] = {0};
    pc[0] = layer;
    memcpy(&pc[4], cc, 16); /* the register bits, read as the shader's vec4, uvec4 or ivec4 */
    rec_CmdPushConstants(cb, g_vk_playout, VK_PUSH_STAGES, 0, 32, pc);
    vk_query_pass_end(cb); /* a clear passes no samples on the console: its draw here isn't counted */
    pred_begin(cb, pred);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    pred_end(cb, pred);
    return 1;
}

/* gpu_trace_target=address[:bytes]: log the clears and draws that write a render target starting in [address,
   address + bytes) (one byte without a size), to find what produced a wrong target. Read on first use: draws and
   clears run on the GPU thread */
static int trace_target(const Surf* s) {
    static int read;
    static u64 lo, bytes;
    if (!read) {
        read = 1;
        const char* v = g_settings.gpu_trace_target;
        if (*v) {
            char *end, *size_end = NULL;
            lo = strtoull(v, &end, 0);
            bytes = *end == ':' ? strtoull(end + 1, &size_end, 0) : 1;
            if (end == v || (*end && *end != ':') || (size_end && (*size_end || size_end == end + 1)) || !bytes) {
                WARN("gpu_trace_target=%s isn't address[:bytes] with a nonzero size (0x for hex): no trace\n", v);
                bytes = 0;
            }
        }
    }
    return bytes && s && s->gva >= lo && s->gva - lo < bytes;
}

/* v: bit 0 depth, 1 stencil, 2-5 the RGBA mask, 6-9 the color slot, 10-20 the layer */
void vk_render_clear(GpuChannel* ch, u32 v) {
    u32* r = ch->r3d;
    u32 slot = (v >> 6) & 15, mask = (v >> 2) & 15, layer = (v >> 10) & 0x7ff;
    float cc[4], dz;
    memcpy(cc, &r[0x360], 16);
    memcpy(&dz, &r[0x364], 4);
    dz = dz > 1 ? 1 : dz >= 0 ? dz : 0; /* Vulkan takes 0..1 only */
    Surf* rt[8];
    Surf* zt;
    u32 n;
    read_targets(ch, rt, &zt, &n);      /* may record uploads: before anything is opened */
    for (u32 i = 0; mask && i < n; i++) /* the slot the clear takes is resolved below: every bound one is listed */
        if (trace_target(rt[i]))
            LOG_LIMIT(64,
                      "gpu-trace-target: color clear of slot %u, bound slot %u target=%llx %ux%u mask=%x "
                      "value=%08x,%08x,%08x,%08x\n",
                      slot, i, (unsigned long long)rt[i]->gva, rt[i]->w, rt[i]->h, mask, r[0x360], r[0x361], r[0x362],
                      r[0x363]);
    if ((v & 3) && trace_target(zt))
        LOG_LIMIT(64, "gpu-trace-target: clear depth target=%llx %ux%u depth=%d stencil=%d value=%g\n",
                  (unsigned long long)zt->gva, zt->w, zt->h, v & 1, (v >> 1) & 1, dz);
    if (mask) {
        /* the slot is taken through RT_CONTROL's count and permutation, like draws do. Whether the hardware takes the
           target registers of that index directly isn't settled: the first clear where the two differ is logged */
        u32 ctl = r[0x487];
        if (slot >= (ctl & 15) || ((ctl >> (4 + slot * 3)) & 7) != slot)
            LOG_LIMIT(1, "vulkan: a clear of color slot %u under RT_CONTROL %08x\n", slot, ctl);
    }
    int zt_undefined = zt && zt->layout == VK_IMAGE_LAYOUT_UNDEFINED;
    if (!render_begin(rt, zt, n, 0, r[0x56E] & 1, 0)) return;
    u32 layers = ~0u;
    for (u32 i = 0; i < g_rp.n; i++)
        if (g_rp.rt[i] && g_rp.rt[i]->layers < layers) layers = g_rp.rt[i]->layers;
    if (g_rp.zt && g_rp.zt->layers < layers) layers = g_rp.zt->layers;
    if (layer >= layers) return;
    VkClearRect rect = {{{0, 0}, g_rp.ext}, layer, 1};
    if (r3d_clear_scissored(r)) {
        u32 xs = r[0x381], ys = r[0x382];
        i32 x0 = (i32)vk_scf(xs & 0xFFFF), x1 = (i32)vk_scf(xs >> 16), y0 = (i32)vk_scf(ys & 0xFFFF),
            y1 = (i32)vk_scf(ys >> 16);
        if (r[0x4EB] & 1) {
            i32 clip_height = (i32)vk_scf((float)(r[0x3FE] >> 16));
            i32 old_y0 = y0;
            y0 = clip_height - y1;
            y1 = clip_height - old_y0;
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (i32)g_rp.ext.width) x1 = (i32)g_rp.ext.width;
        if (y1 > (i32)g_rp.ext.height) y1 = (i32)g_rp.ext.height;
        if (x1 <= x0 || y1 <= y0) return;
        rect.rect = (VkRect2D){{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
    }
    /* the color slot it writes, after the pass's draws that sampled it */
    u32 written = mask && slot < g_rp.n && g_rp.rt[slot] ? 1u << slot : 0;
    if (!feedback_order(0, written)) return;
    feedback_done(0, written);
    VkClearAttachment at[2];
    u32 na = 0;
    if (mask && slot < g_rp.n && g_rp.rt[slot]) {
        if (mask == 15) {
            at[na] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = slot};
            /* an integer target's clear color registers hold the integers themselves, not floats to convert: an
               R8_Uint clear of 185 arrives as 000000b9 */
            if (g_rp.rt[slot]->f.is_int)
                memcpy(at[na].clearValue.color.uint32, &r[0x360], 16);
            else
                memcpy(at[na].clearValue.color.float32, cc, 16);
            na++;
        } else {
            if (clear_masked(slot, mask, cc, rect.rect, layer, ch->render_pred)) target_written(g_rp.rt[slot]);
        }
    }
    if ((v & 3) && zt) {
        VkImageAspectFlags a = 0;
        if (v & 1) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((v & 2) && zt->f.stencil) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (a) {
            at[na] = (VkClearAttachment){.aspectMask = a};
            at[na].clearValue.depthStencil = (VkClearDepthStencilValue){dz, r[0x368] & 0xFF};
            na++;
        }
    }
    if (na) {
        int color_clear = mask == 15 && slot < g_rp.n && g_rp.rt[slot];
        pred_begin(vk_cmd(), ch->render_pred);
        rec_CmdClearAttachments(vk_cmd(), na, at, 1, &rect);
        pred_end(vk_cmd(), ch->render_pred);
        /* a predicated clear may not run: it covers no target whole */
        if (color_clear) {
            Surf* s = g_rp.rt[slot];
            vk_surf_wrote(s);
            if (!ch->render_pred && s->layers == 1 && rect.rect.offset.x == 0 && rect.rect.offset.y == 0 &&
                rect.rect.extent.width == s->sw && rect.rect.extent.height == s->sh)
                vk_rt_drawn_full(s);
            else
                vk_rt_drawn(s);
        }
        if (na > (u32)color_clear && zt && zt_written(zt, ch->render_pred, zt_undefined)) {
            vk_surf_wrote(zt);
            int all_aspects = (v & 1) && (!zt->f.stencil || (v & 2));
            if (!ch->render_pred && all_aspects && zt->layers == 1 && rect.rect.offset.x == 0 &&
                rect.rect.offset.y == 0 && rect.rect.extent.width == zt->sw && rect.rect.extent.height == zt->sh)
                vk_rt_drawn_full(zt);
            else
                vk_rt_drawn(zt);
        }
    }
}

/* ------------------------------------------------------------ register values -> Vulkan */
static VkBlendFactor blend_factor(u32 v) {
    if (v & 0x4000)
        v &= ~0x4000u; /* a GL enum */
    else
        switch (v) { /* the D3D-style numbering */
            case 1: return VK_BLEND_FACTOR_ZERO;
            case 2: return VK_BLEND_FACTOR_ONE;
            case 3: return VK_BLEND_FACTOR_SRC_COLOR;
            case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
            case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case 7: return VK_BLEND_FACTOR_DST_ALPHA;
            case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case 9: return VK_BLEND_FACTOR_DST_COLOR;
            case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
            case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
            case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
            case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
            default: return VK_BLEND_FACTOR_ONE;
        }
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        /* the dual-source factors as the hardware numbers them (0xC900-0xC903), not by their GL values */
        case 0x8900: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 0x8901: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 0x8902: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 0x8903: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x88F9: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 0x88FA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 0x8589: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 0x88FB: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(u32 v) {
    switch (v) {
        case 2:
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 3:
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 4:
        case 0x8007: return VK_BLEND_OP_MIN;
        case 5:
        case 0x8008: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}
static VkCompareOp cmp_op(u32 v) {
    if (v >= 0x200 && v <= 0x207) return (VkCompareOp)(v - 0x200);
    if (v >= 1 && v <= 8) return (VkCompareOp)(v - 1);
    return VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp stencil_op(u32 v) {
    switch (v) {
        case 1:
        case 0x1E00: return VK_STENCIL_OP_KEEP;
        case 3:
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 4:
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 5:
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 6:
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 7:
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 8:
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_ZERO;
}

/* vertex attribute format from the size code (bits 21-26) and component type (27-29): 1 snorm, 2 unorm, 3 sint,
   4 uint, 5 uscaled, 6 sscaled, 7 float */
static VkFormat attr_format(u32 size, u32 ty, int bgra) {
#define BY(sn, un, si, ui, us, ss, fl)                                                                                 \
    (ty == 1 ? sn : ty == 2 ? un : ty == 3 ? si : ty == 4 ? ui : ty == 5 ? us : ty == 6 ? ss : fl)
    /* Vulkan has no 32-bit scaled formats, and an integer format would hand the float input the integers' bits */
    if ((ty == 5 || ty == 6) && (size == 0x01 || size == 0x02 || size == 0x04 || size == 0x12)) {
        LOG_LIMIT(1, "vulkan: 32-bit scaled vertex attributes aren't done yet (read as 0,0,0,1)\n");
        return VK_FORMAT_UNDEFINED;
    }
    switch (size) {
        case 0x01:
            return BY(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_SINT,
                      VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                      VK_FORMAT_R32G32B32A32_SFLOAT);
        case 0x02:
            return BY(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_UINT,
                      VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32_SFLOAT);
        case 0x04:
            return BY(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_UINT,
                      VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_SFLOAT);
        case 0x12:
            return BY(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SINT, VK_FORMAT_R32_UINT,
                      VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SFLOAT);
        case 0x03:
            return BY(VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SINT,
                      VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED,
                      VK_FORMAT_R16G16B16A16_SFLOAT);
        case 0x05:
            return BY(VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SINT,
                      VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_SSCALED,
                      VK_FORMAT_R16G16B16_SFLOAT);
        case 0x0F:
            return BY(VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_UINT,
                      VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_SFLOAT);
        case 0x1B:
            return BY(VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SINT, VK_FORMAT_R16_UINT,
                      VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED, VK_FORMAT_R16_SFLOAT);
        case 0x0A:
            if (bgra && (ty == 2 || ty == 7)) return VK_FORMAT_B8G8R8A8_UNORM;
            return BY(VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SINT,
                      VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED,
                      VK_FORMAT_R8G8B8A8_UNORM);
        case 0x13:
            return BY(VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8_UINT,
                      VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8_UNORM);
        case 0x18:
            return BY(VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_UINT,
                      VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8_UNORM);
        case 0x1D:
            return BY(VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SINT, VK_FORMAT_R8_UINT,
                      VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED, VK_FORMAT_R8_UNORM);
        case 0x30: /* the type decides the shader's input type too (integer or float): each gets its own format */
            return BY(VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                      VK_FORMAT_A2B10G10R10_SINT_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32,
                      VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                      VK_FORMAT_A2B10G10R10_UNORM_PACK32);
        case 0x31: return ty == 3 || ty == 4 ? VK_FORMAT_UNDEFINED : VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    }
#undef BY
    return VK_FORMAT_UNDEFINED;
}
static int vertex_format_ok(VkFormat f) {
    static VkFormat ok[64], bad[64];
    static int nok, nbad;
    if (f == VK_FORMAT_UNDEFINED) return 0;
    for (int i = 0; i < nok; i++)
        if (ok[i] == f) return 1;
    for (int i = 0; i < nbad; i++)
        if (bad[i] == f) return 0;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    int r = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    if (r && nok < 64) ok[nok++] = f;
    if (!r && nbad < 64) {
        bad[nbad++] = f;
        WARN("vulkan: the GPU can't read vertex format %d (those attributes read as 0,0,0,1)\n", (int)f);
    }
    return r;
}

/* bytes an attribute of a size code (bits 21-26) reads */
static u32 attr_bytes(u32 size) {
    switch (size) {
        case 0x01: return 16;
        case 0x02: return 12;
        case 0x03:
        case 0x04: return 8;
        case 0x05: return 6;
        case 0x13: return 3;
        case 0x18:
        case 0x1B: return 2;
        case 0x1D: return 1;
    }
    return 4; /* 0x0A, 0x0F, 0x12, 0x30, 0x31 */
}

/* ------------------------------------------------------------ resources of a draw */
static u32 tex_handle(const GpuChannel* ch, int stage, u32 tcb, const ShTex* st) {
    u32 ci = (st->bindless ? st->bl_cbuf : tcb) & 31, c2i = st->bl2_cbuf & 31;
    u64 cba = ci < 18 ? ch->cbind[stage][ci] : 0;
    u32 h = cba ? gmem_r32(cba + st->cbuf_off * 4) : 0;
    if (st->bindless && st->bl2) {
        u64 c2 = c2i < 18 ? ch->cbind[stage][c2i] : 0;
        if (c2) h |= gmem_r32(c2 + st->bl2_off * 4);
    }
    return h;
}

/* a texture binding of a draw: the headers its handle names, and the texture */
typedef struct {
    u32 handle, have;
    u32 tic[8], tsc[8];
    Surf* t;
} TexRes;
/* What a texture handle resolved to, kept while nothing it came from can have changed: the pools' addresses, the
   surface table (g_vk_surf_epoch), the CPU's writes the stream can see (vk_ww_epoch), the results pending over guest
   memory (none was added: g_pend_any.adds) and the GPU's writes to the two headers. A draw resolved each of its
   textures again, reading both headers and finding the texture by them: a tenth of the GPU thread in one game, for
   the same few hundred handles every draw. A kept texture is still checked against its memory
   (vk_tex_recheck). A render target read whole as the texture is kept too (rt): its Surf stays the target while the
   surface table doesn't change, and a hit does what tex_from_tic does for it each draw, the read check
   (vk_rt_note_read) and whether its contents are defined. Those were a twentieth of the GPU thread in one game,
   which samples its targets in most draws. A crop of a target, or a texture over a target's memory read from memory,
   isn't kept (their contents change with drawing) */
typedef struct {
    const GpuChannel* ch;
    u64 tic_base, tsc_base, gw, adds;
    u32 handle, surf_epoch, ww_epoch;
    u32 tic[8], tsc[8];
    Surf* t;
    int rt; /* t is the render target itself, read whole */
} TexMemo;
#define TEX_MEMO 1024
static TexMemo g_tex_memo[TEX_MEMO];
static void tex_resolve(GpuChannel* ch, u32 handle, TexRes* x) {
    u32* r = ch->r3d;
    if (r[0x48D]) {
        /* samplers are taken by the handle's sampler index. Ryujinx reads this register as the sampler index mode,
           where one mode takes the texture's index instead: whether a game uses it isn't known yet */
        LOG_LIMIT(1, "vulkan: sampler index mode %u\n", r[0x48D]);
    }
    u64 tic_base = ((u64)r[0x55D] << 32) | r[0x55E], tsc_base = ((u64)r[0x557] << 32) | r[0x558];
    u64 tic_at = tic_base + (handle & 0xFFFFF) * 32ull, tsc_at = tsc_base + ((handle >> 20) & 0xFFF) * 32ull;
    TexMemo* m = &g_tex_memo[(handle * 0x9E3779B1u) >> 22];
    u64 gw = gpu_mem_generation();
    x->handle = handle;
    x->have = 1;
    /* an index past its pool's limit (0x55F the headers', 0x559 the samplers') names no header: no texture, or the
       default sampler, as Eden's VisitImageView and the sampler table have it. One game's movie overlay hands
       texture indices 0x2000 past a 2048-entry pool, where shader code lies */
    if ((handle & 0xFFFFF) > r[0x55F] || ((handle >> 20) & 0xFFF) > r[0x559]) {
        if ((handle & 0xFFFFF) > r[0x55F]) {
            memset(x->tic, 0, sizeof x->tic);
            x->t = NULL;
        } else {
            vk_gmem_words(tic_at, x->tic, 8);
            x->t = vk_tex_from_tic(x->tic);
        }
        if (((handle >> 20) & 0xFFF) > r[0x559])
            memset(x->tsc, 0, sizeof x->tsc);
        else
            vk_gmem_words(tsc_at, x->tsc, 8);
        LOG_LIMIT(1, "vulkan: a texture handle %08x names headers past the pools' limits (%x, %x): no texture there\n",
                  handle, r[0x55F], r[0x559]);
        return;
    }
    if (m->t && m->ch == ch && m->handle == handle && m->tic_base == tic_base && m->tsc_base == tsc_base &&
        m->surf_epoch == g_vk_surf_epoch && m->ww_epoch == vk_ww_epoch() && !g_gfx_img_any &&
        m->adds == __atomic_load_n(&g_pend_any.adds, __ATOMIC_RELAXED) &&
        (m->gw == gw || (!gpu_mem_dirty_since(m->gw, tic_at, 32) && !gpu_mem_dirty_since(m->gw, tsc_at, 32)))) {
        m->gw = gw;
        memcpy(x->tic, m->tic, sizeof x->tic);
        memcpy(x->tsc, m->tsc, sizeof x->tsc);
        if (m->rt) {
            if (m->t->layout != VK_IMAGE_LAYOUT_UNDEFINED) vk_rt_note_read(m->t);
            if (m->t->layout != VK_IMAGE_LAYOUT_UNDEFINED) {
                x->t = m->t;
                return;
            }
            m->t = NULL; /* undefined: memory has its texels, resolved below */
        } else {
            x->t = vk_tex_recheck(m->t);
            if (!x->t) m->t = NULL;
            return;
        }
    }
    vk_gmem_words(tic_at, x->tic, 8);
    vk_gmem_words(tsc_at, x->tsc, 8);
    x->t = vk_tex_from_tic(x->tic);
    m->t = NULL;
    Surf* at = x->t ? vk_rt_find(x->t->gva) : NULL;
    if (x->t && (x->t->is_rt ? x->t == at : !at)) {
        m->rt = x->t->is_rt;
        m->ch = ch;
        m->handle = handle;
        m->tic_base = tic_base;
        m->tsc_base = tsc_base;
        m->gw = gpu_mem_generation();
        m->adds = __atomic_load_n(&g_pend_any.adds, __ATOMIC_RELAXED);
        m->surf_epoch = g_vk_surf_epoch;
        m->ww_epoch = vk_ww_epoch();
        memcpy(m->tic, x->tic, sizeof m->tic);
        memcpy(m->tsc, x->tsc, sizeof m->tsc);
        m->t = x->t;
    }
}

int vk_tex_num(const u32* tic, const Surf* t) {
    VkFmt f;
    if (vk_tic_reads_stencil(tic)) return 1; /* a depth target's stencil is unsigned, whatever the surface holds */
    f = t ? t->f : vk_tic_format(tic[0], 0);
    return f.is_int ? (f.sint ? 2 : 1) : 0;
}

int vk_image_keyable(const ShaderInfo* si, int i) {
    return (si->img[i].type == 3 || si->img[i].type == 4) && si->img[i].kind != 1;
}

int vk_image_key(const ShaderInfo* si, int i, const u32* tic) {
    return !!vk_tic_format(tic[0], 0).is_int != !!si->img[i].bindless;
}

VkImageView vk_storage_image_view(Surf* t, const ShaderInfo* si, int i, u64* bytes) {
    VkComponentMapping identity = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    *bytes = vk_surf_guest_size(t);
    if (!t || !t->storage_capable || !vk_surf_can_writeback(t) || t->levels != 1 || t->depth3d != 1 ||
        (si->img[i].type != 3 && si->img[i].type != 4) || !*bytes || *bytes > UINT32_MAX ||
        (si->img[i].kind ? t->f.vk != VK_FORMAT_R32_UINT : t->f.is_int))
        return VK_NULL_HANDLE;
    /* a 2D array surface sees every layer of the image, a single layer one too */
    return vk_tex_view(t, si->img[i].type == 4 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, 0, &identity, 0);
}

u32 vk_gbuf_desc(int stage, u32 off, const u32* v, u64* addr) {
    u32 size = stage == ST_VS && off == 0xb0 ? 16 : v[2];
    *addr = ((u64)v[1] << 32) | v[0];
    if (!size) size = 16;
    if (size > (16u << 20)) size = 16u << 20;
    return (size + 3) & ~3u;
}

/* What picks the fragment shader's variant: which of its textures and outputs are integer (signed), its color textures
   compared as depth and their functions, and its inputs the stage before doesn't write. 0s when the program itself
   is drawn */
typedef struct {
    u64 tm, ts, dm;
    u32 rm, rs, df, unfed;
} FsKey;
/* the integer color targets (rm, signed rs) the registers name, which key the fragment shader's outputs */
static void rt_int_targets(const u32* r, u32* rm, u32* rs) {
    u32 ctl = r[0x487], count = ctl & 15;
    if (count > 8) count = 8;
    *rm = *rs = 0;
    for (u32 i = 0; i < count; i++) {
        const u32* t = &r[0x200 + ((ctl >> (4 + i * 3)) & 7) * 0x10];
        if (!t[4]) continue;
        VkFmt f = vk_rt_format(t[4]);
        if (f.is_int) {
            *rm |= 1u << i;
            if (f.sint) *rs |= 1u << i;
        }
    }
}
/* resolves the textures of s0, the fragment program, into res (SH_MAXTEX entries, for stage_set), and fills fk with
   the variant they and the targets (rt_int, rt_sint) key. last_out: the generic outputs the stage before rasterization
   writes */
static void fs_key(GpuChannel* ch, const VkSh* s0, TexRes* res, u32 last_out, u32 rt_int, u32 rt_sint, FsKey* fk) {
    u32* r = ch->r3d;
    u32 nd = 0;
    *fk = (FsKey){.rm = rt_int, .rs = rt_sint, .unfed = s0->si.in_mask & ~last_out};
    int nt = s0->si.ntex > SH_MAXTEX ? SH_MAXTEX : s0->si.ntex;
    for (int i = 0; i < nt; i++) {
        const ShTex* st = &s0->si.tex[i];
        tex_resolve(ch, tex_handle(ch, 4, r[0x982], st), &res[i]);
        if (st->shadow) {
            if (st->flags & SNX_TEX_INDEXED) continue; /* the selected entry's function comes from the table */
            /* a depth compare of a color texture compares its first component, which Vulkan's compare samplers
               can't do on a color image: the shader samples it and compares (the TSC's function, LEQUAL when the
               TSC has none, as vk_tex_bind_sampler sets) */
            Surf* t = res[i].t;
            if (t && !t->f.depth && !t->f.is_int) {
                u32 func = (res[i].tsc[0] >> 9) & 1 ? (res[i].tsc[0] >> 10) & 7 : 3;
                fk->dm |= 1ull << i;
                if (nd < SH_DC_COLOR_MAX) fk->df |= func << (3 * nd);
                nd++;
            }
            continue;
        }
        int num = vk_tex_num(res[i].tic, res[i].t);
        if (num) fk->tm |= 1ull << i;
        if (num == 2) fk->ts |= 1ull << i;
    }
    fk->rm &= s0->si.sph[18] ? 0xFF : 0;
    u64 tic_base = ((u64)r[0x55D] << 32) | r[0x55E];
    for (int i = 0; i < s0->si.nimg; i++) {
        if (!vk_image_keyable(&s0->si, i)) continue;
        ShTex handle_info = {
            .cbuf_off = s0->si.img[i].cbuf_off, .bindless = s0->si.img[i].bindless, .bl_cbuf = s0->si.img[i].bl_cbuf};
        u32 handle = tex_handle(ch, 4, r[0x982], &handle_info), tic[8];
        vk_gmem_words(tic_base + (handle & 0xFFFFF) * 32ull, tic, 8);
        if (vk_image_key(&s0->si, i, tic)) fk->rm |= 1u << (8 + i);
    }
}
/* the fragment shader at addr in the variant fk names, s0 when it names none */
static VkSh* fs_variant(GpuChannel* ch, u64 addr, VkSh* s0, const FsKey* fk) {
    if (!fk->tm && !fk->rm && !fk->dm && !fk->unfed) return s0;
    return vk_shader_get(ch, addr, 0, ST_FS, fk->tm, fk->ts, fk->rm, fk->rs, 0, fk->dm, fk->df, fk->unfed, NULL);
}

VkImageViewType vk_tex_view_type(const ShTex* st) {
    return st->type == 2   ? VK_IMAGE_VIEW_TYPE_3D
           : st->type == 3 ? (st->array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE)
           : st->array     ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                           : VK_IMAGE_VIEW_TYPE_2D;
}

VkImageView vk_tex_bind_view(Surf* t, VkImageViewType want, const u32* tic, const u32* tsc, int num) {
    int stencil = t && t->f.stencil && vk_tic_reads_stencil(tic);
    if (!t || (stencil ? 1 : t->f.is_int ? (t->f.sint ? 2 : 1) : 0) != num) return VK_NULL_HANDLE;
    static const VkComponentSwizzle sw[8] = {
        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R,   VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_B,    VK_COMPONENT_SWIZZLE_A,    VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE};
    VkComponentSwizzle m[4] = {sw[(tic[0] >> 19) & 7], sw[(tic[0] >> 22) & 7], sw[(tic[0] >> 25) & 7],
                               sw[(tic[0] >> 28) & 7]};
    /* the header's swizzle is in memory component order: B, G, R, A targets swap R and B */
    if (t->f.bgra)
        for (int k = 0; k < 4; k++)
            m[k] = m[k] == VK_COMPONENT_SWIZZLE_R   ? VK_COMPONENT_SWIZZLE_B
                   : m[k] == VK_COMPONENT_SWIZZLE_B ? VK_COMPONENT_SWIZZLE_R
                                                    : m[k];
    if (t->f.depth) { /* depth in r, g and b, alpha 1 */
        m[0] = m[1] = m[2] = VK_COMPONENT_SWIZZLE_R;
        m[3] = VK_COMPONENT_SWIZZLE_ONE;
    }
    VkComponentMapping map = t->fkey & (1u << 30)
                                 ? (VkComponentMapping){VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO,
                                                        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE}
                                 : (VkComponentMapping){m[0], m[1], m[2], m[3]};
    /* The TIC selects an sRGB-capable format; the TSC controls whether sampling unpacks it to linear. */
    return vk_tex_view(t, want, ((tic[4] >> 22) & 1) && ((tsc[0] >> 13) & 1), &map, stencil);
}

/* the texture (NULL for none) and the view type say whether anisotropy= may raise its filtering: 2D textures with
   mipmaps only, as the yuzu family and Ryujinx do (a cube, a 3D LUT or a copied target keeps the game's) */
VkSampler vk_tex_bind_sampler(const u32* tsc, const ShTex* st, int num, const Surf* tex, VkImageViewType want) {
    u32 tc[8];
    memcpy(tc, tsc, sizeof tc);
    /* the compare mode is what the shader declares */
    if (st->shadow) {
        if (!((tc[0] >> 9) & 1)) tc[0] = (tc[0] & ~(7u << 10)) | 1u << 9 | 3u << 10; /* LEQUAL */
    } else
        tc[0] &= ~(1u << 9);
    /* integer textures can't be filtered: nearest, and a linear mip filter becomes nearest */
    if (num) {
        u32 mip = (tc[1] >> 6) & 3;
        tc[1] = (tc[1] & ~0xF3u) | 0x11u | (mip == 3 ? 2u : mip) << 6;
    }
    /* raised only with anisotropy= set: without it the cache keeps one sampler per TSC */
    int raise = g_vk_anisotropy > 1 && tex && tex->levels > 1 &&
                (want == VK_IMAGE_VIEW_TYPE_2D || want == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    return vk_sampler(tc, raise, num != 0);
}

/* a constant buffer's bytes for this draw, reused while the guest can't have changed them */
static int cb_get(const GpuChannel* ch, int stage, int i, u32 sz, VkBuffer* buf, u64* off) {
    static struct {
        u64 a, gw, frame, cs_adds, gfx_adds;
        u32 sz, epoch;
        VkBuffer buf;
        u64 off;
    } c[5][18];
    u64 a = ch->cbind[stage][i];
    int pending;
    /* checked as it is cached, with no compute or graphics result added since: none can cover it, and only a write of
       its memory leaves the copy stale. The pending checks were the most of cb_get's time in one game */
    if (a && c[stage][i].a == a && c[stage][i].epoch == g_vk_epoch && c[stage][i].frame == vk_submit_number() &&
        c[stage][i].sz >= sz && c[stage][i].cs_adds == vk_compute_adds() && c[stage][i].gfx_adds == vk_buf_adds() &&
        !gpu_mem_dirty_since(c[stage][i].gw, a, sz)) {
        *buf = c[stage][i].buf;
        *off = c[stage][i].off;
        return 1;
    }
    /* Pending compute results there land first. A pending graphics result is newer than the guest memory the checks
       below see: the buffer then comes from vk_buf_get_strict, which reads it on the GPU, and isn't cached */
    if (g_cs_any && a) vk_compute_mem_need(a, sz);
    pending = g_gfx_any && a && vk_buf_pending(a, sz);
    if (!pending && a && c[stage][i].a == a && c[stage][i].epoch == g_vk_epoch &&
        c[stage][i].frame == vk_submit_number() && c[stage][i].sz >= sz &&
        !gpu_mem_dirty_since(c[stage][i].gw, a, sz)) {
        *buf = c[stage][i].buf;
        *off = c[stage][i].off;
        return 1;
    }
    if (!(a ? vk_buf_get_strict(a, sz, buf, off) : vk_buf_copy(a, sz, buf, off))) return 0;
    if (pending) {
        c[stage][i].a = 0;
        return 1;
    }
    c[stage][i].a = a;
    c[stage][i].cs_adds = vk_compute_adds();
    c[stage][i].gfx_adds = vk_buf_adds();
    c[stage][i].gw = gpu_mem_generation();
    c[stage][i].frame = vk_submit_number();
    c[stage][i].sz = sz;
    c[stage][i].epoch = g_vk_epoch;
    c[stage][i].buf = *buf;
    c[stage][i].off = *off;
    return 1;
}

/* small constants in this frame's staging: 1/resolution scale (SNX_RS) at 0, the 0,0,0,1 of attributes without
   data at 16 (float) and 32 (integer) */
static int frame_consts(VkBuffer* buf, u64* off) {
    static u64 frame = ~0ull, o;
    static VkBuffer b;
    if (frame != vk_submit_number()) {
        u8* p = vk_stage(64, vk_stage_align(), &b, &o);
        if (!p) return 0;
        float rs[4] = {1 / g_vk_scale, 1 / g_vk_scale, g_vk_scale, 0}, fa[4] = {0, 0, 0, 1};
        u32 ia[4] = {0, 0, 0, 1};
        memcpy(p, rs, 16);
        memcpy(p + 16, fa, 16);
        memcpy(p + 32, ia, 16);
        snx_le32_array(p, 12); /* the device reads them little-endian */
        frame = vk_submit_number();
    }
    *buf = b;
    *off = o;
    return 1;
}

typedef struct {
    VkWriteDescriptorSet w[SNX_VKB_END];
    VkDescriptorBufferInfo bi[SNX_VKB_END];
    VkDescriptorImageInfo ii[SNX_VKB_SMAP + 1];
    VkDescriptorImageInfo depth_table[SNX_TEX_ARRAY];
    VkDescriptorImageInfo table[SNX_TEX_ARRAY];
    u32 n;
} DescWrites;

/* Fragment programs' storage images, written back to guest memory: each with its bytes from before the draw (original,
   for the merge with the CPU's writes since) and the pages it covers. A release puts their write-back aside: their
   copy is recorded into the frame's host-readable ring (sub, the submit it is in, back, wgen the write generation it
   copies), and they land once that submit is done (vk_gfx_image_frame_done), as storage buffers' results do. Before,
   each release copied and waited for the GPU: half of one game's frame time in vk_finish. Whatever needs their memory
   sooner lands them at once, waiting for that submit when its copy is recorded */
typedef struct {
    Surf* surf;
    u8* original;
    u32 size;
    u64* pages;
    size_t npages;
    u64 sub, back_off, back_size, wgen;
    u8* back;
    VmaAllocation back_mem;
} GfxImageOut;
static GfxImageOut* g_img_out;
static size_t g_img_count, g_img_cap;
static _Thread_local int g_img_landing;
_Atomic int g_gfx_img_any;

static int image_page_cmp(const void* a, const void* b) {
    u64 x = *(const u64*)a, y = *(const u64*)b;
    return (x > y) - (x < y);
}
/* surf has a write-back waiting whose copy isn't recorded yet: a later draw's writes are in it */
static int image_pending(Surf* surf) {
    for (size_t i = 0; i < g_img_count; i++)
        if (g_img_out[i].surf == surf && !g_img_out[i].sub) return 1;
    return 0;
}
static void image_record(Surf* surf, u8* original, u32 size) {
    if (image_pending(surf)) {
        free(original);
        return;
    }
    if (g_img_count == g_img_cap) {
        size_t cap = g_img_cap ? g_img_cap * 2 : 8;
        GfxImageOut* next = realloc(g_img_out, cap * sizeof *next);
        if (!next) fatal("Out of memory recording graphics storage image");
        g_img_out = next;
        g_img_cap = cap;
    }
    size_t max_pages = (size_t)(((surf->gva & 4095) + (u64)size + 4095) >> 12);
    u64* pages = malloc(max_pages * sizeof *pages);
    if (!pages) fatal("Out of memory recording graphics storage image pages");
    size_t npages = 0;
    u64 first = surf->gva & ~4095ull;
    for (size_t i = 0; i < max_pages; i++) {
        u64 cpu = gmmu_translate(first + (u64)i * 4096);
        if (cpu) pages[npages++] = cpu >> 12;
    }
    qsort(pages, npages, sizeof *pages, image_page_cmp);
    g_img_out[g_img_count++] = (GfxImageOut){.surf = surf, .original = original, .size = size, .pages = pages,
                                             .npages = npages};
    g_gfx_img_any = 1;
}
static int image_overlaps(const GfxImageOut* out, u64 gva, u64 size) {
    if (!size) return 0;
    u64 base = out->surf->gva;
    if (gva >= base ? gva - base < out->size : base - gva < size) return 1;
    u64 first = gva & ~4095ull, last = (gva + size - 1) & ~4095ull;
    for (u64 at = first; at <= last; at += 4096) {
        u64 cpu = gmmu_translate(at), page = cpu >> 12;
        if (cpu && bsearch(&page, out->pages, out->npages, sizeof page, image_page_cmp)) return 1;
        if (last - at < 4096) break;
    }
    return 0;
}
static void image_remove(size_t index) {
    free(g_img_out[index].original);
    free(g_img_out[index].pages);
    memmove(g_img_out + index, g_img_out + index + 1, (g_img_count - index - 1) * sizeof *g_img_out);
    g_img_count--;
    g_gfx_img_any = g_img_count != 0;
}
/* an entry whose copy's submit is done, landed from the ring */
static void image_land(size_t index) {
    GfxImageOut* o = &g_img_out[index];
    vmaInvalidateAllocation(g_vk.vma, o->back_mem, o->back_off, o->back_size);
    vk_surf_flush_land(o->surf, o->back, o->original, o->wgen);
    image_remove(index);
}
/* the entry whose bytes from before the draw are original (each its own allocation): waiting for the GPU can land
   others and move the entries, so an entry is found again after a wait */
static size_t image_find(const u8* original) {
    size_t k = 0;
    while (k < g_img_count && g_img_out[k].original != original) k++;
    return k;
}
static void image_flush_at(size_t index) {
    GfxImageOut out = g_img_out[index];
    if (out.sub) { /* its copy is recorded: once its submit is done, from there */
        if (out.sub > vk_last_submit()) vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
        vk_wait_submit(out.sub);
        index = image_find(out.original);
        if (index < g_img_count) image_land(index); /* not landed by a frame's landing meanwhile */
        return;
    }
    vk_surf_flush(out.surf, out.original);
    if (out.surf->mem_wgen != out.surf->wgen) fatal("Cannot write back graphics storage image");
    index = image_find(out.original);
    if (index < g_img_count) image_remove(index);
}
void vk_gfx_image_mem_need(u64 gva, u64 size) {
    if (!g_gfx_img_any || g_img_landing || !size) return;
    vk_lock();
    g_img_landing = 1;
    for (size_t i = 0; i < g_img_count;)
        if (image_overlaps(&g_img_out[i], gva, size))
            image_flush_at(i);
        else
            i++;
    g_img_landing = 0;
    vk_unlock();
}
/* every entry holds its surface until it lands, its copy recorded or not (image_pending leaves those out) */
int vk_gfx_image_holds(const Surf* s) {
    for (size_t i = 0; g_gfx_img_any && i < g_img_count; i++)
        if (g_img_out[i].surf == s) return 1;
    return 0;
}
int vk_gfx_image_pending(u64 gva, u64 size) {
    int hit = 0;
    if (!g_gfx_img_any || !size) return 0;
    vk_lock();
    for (size_t i = 0; i < g_img_count && !hit; i++) hit = image_overlaps(&g_img_out[i], gva, size);
    vk_unlock();
    return hit;
}
void vk_gfx_image_land(void) {
    if (!g_gfx_img_any || g_img_landing) return;
    vk_lock();
    g_img_landing = 1;
    while (g_img_count) image_flush_at(0);
    g_img_landing = 0;
    vk_unlock();
}
void vk_gfx_image_put_aside(void) {
    if (!g_gfx_img_any || g_img_landing) return;
    vk_lock();
    g_img_landing = 1;
    vk_cmd(); /* a new frame, if one begins, lands what it lands before the entries are walked */
    for (size_t i = 0; i < g_img_count;) {
        GfxImageOut* o = &g_img_out[i];
        u64 size, off;
        VkBuffer buf;
        VmaAllocation mem;
        u8* p;
        if (o->sub) {
            i++;
            continue;
        }
        size = vk_surf_flush_size(o->surf);
        p = size ? vk_stage_back(size, &buf, &off, &mem) : NULL;
        if (!p) { /* nothing to copy, or no room in the ring: at once */
            image_flush_at(i);
            continue;
        }
        vk_surf_flush_record(o->surf, buf, off);
        o->sub = vk_submit_number();
        o->back = p;
        o->back_mem = mem;
        o->back_off = off;
        o->back_size = size;
        o->wgen = o->surf->wgen;
        i++;
    }
    g_img_landing = 0;
    vk_unlock();
}
int vk_gfx_image_in_flight(int unsubmitted) {
    for (size_t i = 0; i < g_img_count; i++)
        if (g_img_out[i].sub && (!unsubmitted || g_img_out[i].sub > vk_last_submit())) return 1;
    return 0;
}
void vk_gfx_image_frame_done(u64 sub) {
    int landing = g_img_landing;
    g_img_landing = 1; /* nothing lands the entries over again while they are walked */
    for (size_t i = 0; i < g_img_count;)
        if (g_img_out[i].sub && g_img_out[i].sub <= sub)
            image_land(i);
        else
            i++;
    g_img_landing = landing;
}

typedef struct {
    Surf* surf[8];
    u8* original[8];
    u32 size[8];
    int n;
} DrawImages;
static void draw_images_discard(DrawImages* images) {
    for (int i = 0; i < images->n; i++) free(images->original[i]);
    images->n = 0;
}
static void draw_images_commit(DrawImages* images) {
    for (int i = 0; i < images->n; i++) {
        vk_surf_wrote(images->surf[i]);
        if (images->original[i]) image_record(images->surf[i], images->original[i], images->size[i]);
    }
    images->n = 0;
}
static void dw_buffer(DescWrites* d, VkDescriptorSet set, u32 binding, VkDescriptorType type, VkBuffer b, u64 off,
                      u64 range) {
    d->bi[binding] = (VkDescriptorBufferInfo){b, off, range};
    d->w[d->n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet = set,
                                          .dstBinding = binding,
                                          .descriptorCount = 1,
                                          .descriptorType = type,
                                          .pBufferInfo = &d->bi[binding]};
}

static int is_target(Surf* s, Surf** rt, Surf* zt, u32 n) {
    if (s == zt) return 1;
    for (u32 i = 0; i < n; i++)
        if (rt[i] == s) return 1;
    return 0;
}

/* The elements of SNX_VKB_TEXARRAY, from an indexed texture slot: SNX_TEX_ARRAY handles from the slot's offset on,
   (1 << shift) bytes apart. A handle past the constant buffer's size, or with a header or sampler index past its
   pool's limit (no texture on hardware either), reads as the null texture. So does a texture of another kind than
   the slot's, and a target the draw renders to (the bound path's snapshot, for every element, would copy each
   target the table names on every draw). */
static int tex_tables_set(GpuChannel* ch, int stage, const VkSh* s, const int* first, int count, Surf** rt, Surf* zt,
                          u32 n, VkImageLayout layout, DescWrites* d, VkDescriptorSet set) {
    const ShTex* kind = &s->si.tex[first[0]];
    VkImageViewType want = vk_tex_view_type(kind);
    int num = kind->shadow || !((s->si.tex_int_mask >> first[0]) & 1) ? 0
              : ((s->si.tex_sint_mask >> first[0]) & 1)               ? 2
                                                                      : 1;
    ShTex plain = *kind;
    if (kind->shadow) plain.shadow = 0;
    VkImageView none = vk_null_view(want, num);
    /* not far: windef.h defines it away */
    VkImageView farthest = kind->shadow ? vk_null_view(want, 3) : VK_NULL_HANDLE;
    if (!none || (kind->shadow && !farthest)) return 0;
    u32 handles[SNX_TEX_ARRAY] = {0};
    u32 null_tsc[8] = {0};
    u32 funcs[SNX_TEX_ARRAY] = {0};
    /* each element's entry, NO_ENTRY for one past its constant buffer: the null texture, given an entry only when an
       element needs it, so a table of SNX_TEX_ARRAY real handles fits (edenprobe's bindless suite samples 256) */
    enum { NO_ENTRY = 0xFFFFFFFFu };
    u32 map[SH_INDEXED_TABLES * SNX_TEX_ARRAY] = {0};
    u32 used = 0;
    int need_null = 0;
    VkDescriptorImageInfo null_info, null_depth = {0};
    VkSampler null_sampler = vk_tex_bind_sampler(null_tsc, &plain, num, NULL, want);
    if (!null_sampler) return 0;
    null_info = (VkDescriptorImageInfo){null_sampler, none, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    if (kind->shadow) {
        VkSampler null_compare = vk_tex_bind_sampler(null_tsc, kind, 0, NULL, want);
        if (!null_compare) return 0;
        null_depth = (VkDescriptorImageInfo){null_compare, farthest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    for (int table = 0; table < count; table++) {
        const ShTex* st = &s->si.tex[first[table]];
        u32 shift = (u32)st->flags >> 4, bank = st->bl_cbuf & 31;
        u64 cba = bank < 18 ? ch->cbind[stage][bank] : 0;
        u32 size = cba ? ch->cbind_size[stage][bank] : 0;
        if (size > 65536 || !size) size = 65536;
        for (u32 k = 0; k < SNX_TEX_ARRAY; k++) {
            u32 at = st->cbuf_off * 4 + (k << shift);
            /* past the constant buffer: the null texture. A handle of 0 is header 0 with sampler 0, a texture like any
               other (edenprobe's bindless case samples it) */
            if (!cba || at + 4 > size) {
                map[table * SNX_TEX_ARRAY + k] = NO_ENTRY;
                need_null = 1;
                continue;
            }
            u32 handle = gmem_r32(cba + at);
            u32 id = 0;
            while (id < used && handles[id] != handle) id++;
            if (id == used) {
                if (used == SNX_TEX_ARRAY) {
                    LOG_LIMIT(4, "vulkan: indexed texture tables exceed %u unique handles\n", SNX_TEX_ARRAY);
                    return 0;
                }
                handles[id] = handle;
                used++;
                TexRes x = {0};
                VkImageView view = VK_NULL_HANDLE;
                if ((handle & 0xFFFFF) <= ch->r3d[0x55F] && ((handle >> 20) & 0xFFF) <= ch->r3d[0x559]) {
                    tex_resolve(ch, handle, &x);
                    if (x.t && (!kind->shadow || !x.t->f.is_int) && !is_target(x.t, rt, zt, n))
                        view = vk_tex_bind_view(x.t, want, x.tic, x.tsc, num);
                }
                funcs[id] = x.have && (x.tsc[0] >> 9 & 1) ? (x.tsc[0] >> 10) & 7 : 3;
                int depth = kind->shadow && (!view || x.t->f.depth);
                if (depth) funcs[id] |= 8;
                if (view)
                    vk_surf_use(vk_cmd(), x.t, layout, VK_SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                else {
                    view = none;
                    /* an unused entry holds 0: a handle that names nothing else is the black-texture signal */
                    if (handle)
                        LOG_LIMIT(4, "vulkan: indexed texture reads the null texture (bank %u handle %08x)\n", bank,
                                  handle);
                }
                VkImageView color_view = depth ? none : view;
                VkSampler smp = vk_tex_bind_sampler(x.tsc, &plain, num, color_view != none ? x.t : NULL, want);
                if (!smp) return 0;
                d->table[id] = (VkDescriptorImageInfo){
                    smp, color_view, color_view == none ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : layout};
                if (kind->shadow) {
                    VkImageView depth_view = depth && view != none ? view : farthest;
                    VkSampler compare = vk_tex_bind_sampler(x.tsc, kind, 0, depth_view != farthest ? x.t : NULL, want);
                    if (!compare) return 0;
                    d->depth_table[id] = (VkDescriptorImageInfo){
                        compare, depth_view,
                        depth_view == farthest ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : layout};
                }
            }
            map[table * SNX_TEX_ARRAY + k] = id;
        }
    }
    if (need_null) {
        if (used == SNX_TEX_ARRAY) {
            LOG_LIMIT(4, "vulkan: indexed texture tables exceed %u unique handles\n", SNX_TEX_ARRAY);
            return 0;
        }
        for (u32 e = 0; e < (u32)count * SNX_TEX_ARRAY; e++)
            if (map[e] == NO_ENTRY) map[e] = used;
    }
    /* the null texture in every entry left, its own included: the binding is read whole */
    for (u32 k = used; k < SNX_TEX_ARRAY; k++) {
        d->table[k] = null_info;
        if (kind->shadow) {
            d->depth_table[k] = null_depth;
            funcs[k] = 11; /* LEQUAL and null depth */
        }
    }
    VkWriteDescriptorSet* w = &d->w[d->n++];
    *w = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = set,
                                .dstBinding = SNX_VKB_TEXARRAY,
                                .descriptorCount = SNX_TEX_ARRAY,
                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .pImageInfo = d->table};
    if (kind->shadow) {
        VkWriteDescriptorSet* depth_write = &d->w[d->n++];
        *depth_write = *w;
        depth_write->dstBinding = SNX_VKB_TEXDEPTH;
        depth_write->pImageInfo = d->depth_table;
        VkBuffer buffer;
        u64 offset;
        u8* data = vk_stage(sizeof funcs, vk_stage_align(), &buffer, &offset);
        if (!data) return 0;
        memcpy(data, funcs, sizeof funcs);
        snx_le32_array(data, SNX_TEX_ARRAY);
        dw_buffer(d, set, SNX_VKB_TEXFUNCS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer, offset, sizeof funcs);
    }
    if (count) {
        VkBuffer buffer;
        u64 offset;
        u8* data = vk_stage(sizeof map, vk_stage_align(), &buffer, &offset);
        if (!data) return 0;
        memcpy(data, map, sizeof map);
        snx_le32_array(data, sizeof map / 4);
        dw_buffer(d, set, SNX_VKB_TEXMAP, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer, offset, sizeof map);
    }
    return 1;
}

static void draw_storage_read(const GpuChannel* ch, VkDrawStorage* draw, VkSh* vs, VkSh* fs) {
    VkSh* sh[2] = {vs, fs};
    memset(draw, 0, sizeof *draw);
    for (int s = 0; s < 2; s++) {
        if (!sh[s]) continue;
        int stage = s ? 4 : 0;
        u64 cb0 = ch->cbind[stage][0];
        draw->n[s] = sh[s]->si.ngbuf < 16 ? sh[s]->si.ngbuf : 16;
        if (cb0 && draw->n[s]) vk_buf_mem_need(cb0, 0x610);
        for (int i = 0; i < draw->n[s]; i++) {
            VkDrawGbuf* b = &draw->b[s][i];
            u32 v[3] = {0, 0, 0};
            if (cb0) vk_gmem_words(cb0 + sh[s]->si.gbuf_cbuf_off[i], v, 3);
            b->size = vk_gbuf_desc(stage, sh[s]->si.gbuf_cbuf_off[i], v, &b->addr);
            b->writable = (sh[s]->si.gbuf_wmask >> i) & 1;
        }
    }
}

/* a stage's descriptor set: constant buffers, storage buffers, textures. *zt_ro is set when it samples the depth
   target, which the pass then binds read-only. *fb gathers the color slots it samples in place, which the pass then
   binds in the feedback-loop layout (feedback_order): the fragment stage's, NULL for the others, which sample a copy.
   res: textures fs_key resolved for this stage, or NULL */
/* A stage's descriptor set by its contents: the same writes (buffers, offsets, ranges, views, samplers, layouts) in
   the same layout reuse the set made for them earlier in the frame, which still holds them (its pool is reset when the
   frame slot comes round again, and what it names is destroyed no earlier: vk_defer_*). One game allocated and wrote
   13,400 sets a frame, two a draw, much of it the same textures again. Two independent 64-bit hashes stand for the
   contents */
u64 g_vk_dset_new, g_vk_dset_reused;
static u64 dset_mix(u64 h, u64 v, u64 m) {
    h = (h ^ v) * m;
    return h ^ (h >> 29);
}
static VkDescriptorSet desc_set_cached(VkDescriptorSetLayout layout, DescWrites* d) {
    static struct {
        u64 h1, h2, frame;
        VkDescriptorSet set;
    } cache[1u << 14];
    const u64 m1 = 0x9E3779B97F4A7C15ull, m2 = 0xC2B2AE3D27D4EB4Full;
    u64 h1 = dset_mix(0x243F6A8885A308D3ull, (u64)(uintptr_t)layout, m1),
        h2 = dset_mix(0x13198A2E03707344ull, (u64)(uintptr_t)layout, m2);
    for (u32 i = 0; i < d->n; i++) {
        const VkWriteDescriptorSet* w = &d->w[i];
        u64 head = (u64)w->dstBinding | (u64)w->descriptorType << 20 | (u64)w->descriptorCount << 40 |
                   (u64)w->dstArrayElement << 56;
        h1 = dset_mix(h1, head, m1);
        h2 = dset_mix(h2, head, m2);
        for (u32 j = 0; j < w->descriptorCount; j++) {
            u64 a, b, c;
            if (w->pBufferInfo) {
                a = (u64)(uintptr_t)w->pBufferInfo[j].buffer, b = w->pBufferInfo[j].offset, c = w->pBufferInfo[j].range;
            } else if (w->pImageInfo) {
                a = (u64)(uintptr_t)w->pImageInfo[j].imageView, b = (u64)(uintptr_t)w->pImageInfo[j].sampler;
                c = (u64)w->pImageInfo[j].imageLayout;
            } else {
                a = (u64)(uintptr_t)(w->pTexelBufferView ? w->pTexelBufferView[j] : VK_NULL_HANDLE), b = c = 0;
            }
            h1 = dset_mix(dset_mix(dset_mix(h1, a, m1), b, m1), c, m1);
            h2 = dset_mix(dset_mix(dset_mix(h2, a, m2), b, m2), c, m2);
        }
    }
    u64 frame = vk_frame_id() + 1; /* 0 for an unused slot */
    typeof(cache[0])* e = &cache[h1 >> 50];
    if (e->frame == frame && e->h1 == h1 && e->h2 == h2) {
        g_vk_dset_reused++;
        return e->set;
    }
    VkDescriptorSet set = vk_rec_new_set(layout, d->n, d->w); /* made on the recording thread, before it is bound */
    if (!set) return VK_NULL_HANDLE;
    g_vk_dset_new++;
    *e = (typeof(cache[0])){h1, h2, frame, set};
    return set;
}
static VkDescriptorSet stage_set(GpuChannel* ch, int stage, VkSh* s, Surf** rt, Surf* zt, u32 n, int* zt_ro, u32* fb,
                                 TexRes* res, const VkDrawStorage* draw, DrawImages* images) {
    u32* r = ch->r3d;
    VkDescriptorSetLayout layout = g_vk_dslayout[s->indexed][shader_vk_set(stage)];
    VkDescriptorSet set = VK_NULL_HANDLE; /* the writes take it once their contents are known (desc_set_cached) */
    DescWrites d;
    d.n = 0;
    for (int i = 0; i < 18; i++) {
        if (!(s->si.cbuf_mask & (1u << i))) continue;
        u32 sz = ch->cbind_size[stage][i];
        if (sz > 65536 || !sz) sz = 65536;
        if (s->si.cbuf_max[i] && sz > s->si.cbuf_max[i]) sz = s->si.cbuf_max[i]; /* only the bytes the shader reads */
        sz = (sz + 15) & ~15u;
        if (sz > g_vk.props.limits.maxUniformBufferRange) sz = g_vk.props.limits.maxUniformBufferRange;
        VkBuffer b;
        u64 o;
        if (!cb_get(ch, stage, i, sz, &b, &o)) return VK_NULL_HANDLE;
        dw_buffer(&d, set, SNX_VKB_CB + i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b, o, sz);
    }
    for (int i = 0; i < s->si.ngbuf && i < 16; i++) {
        const VkDrawGbuf* gb = &draw->b[stage ? 1 : 0][i];
        u64 addr = gb->addr;
        u32 size = gb->size;
        VkBuffer b;
        u64 o;
        int ok = gb->aliased            ? (b = gb->buf, o = gb->off, 1)
                 : addr && gb->writable ? vk_buf_copy_writable(addr, size, &b, &o)
                 : addr                 ? vk_buf_get(addr, size, &b, &o)
                                        : vk_buf_copy(0, size, &b, &o);
        if (!ok) return VK_NULL_HANDLE;
        dw_buffer(&d, set, SNX_VKB_SSBO + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, b, o, gb->aliased ? gb->range : size);
    }
    if (s->si.ngbuf)
        dw_buffer(&d, set, SNX_VKB_SMAP, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, draw->map_buf[stage ? 1 : 0],
                  draw->map_off[stage ? 1 : 0], draw->map_size[stage ? 1 : 0]);
    VkBuffer cb;
    u64 co;
    if (stage == 4 && frame_consts(&cb, &co))
        dw_buffer(&d, set, SNX_VKB_RS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cb, co, 16);
    u32 tcb = r[0x982];
    int nt = s->si.ntex > SH_MAXTEX ? SH_MAXTEX : s->si.ntex;
    int table_first[SH_INDEXED_TABLES] = {0}, table_count = 0;
    for (int i = 0; i < nt; i++) {
        ShTex* st = &s->si.tex[i];
        if (st->flags & SNX_TEX_INDEXED) {
            int table = 0;
            while (table < table_count && !shader_same_indexed_table(st, &s->si.tex[table_first[table]])) table++;
            if (table == table_count) {
                if (table_count == SH_INDEXED_TABLES) return VK_NULL_HANDLE;
                table_first[table_count++] = i;
            }
        }
    }
    if (table_count &&
        !tex_tables_set(ch, stage, s, table_first, table_count, rt, zt, n,
                        s->si.nimg ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &d, set))
        return VK_NULL_HANDLE;
    for (int i = 0; i < nt; i++) {
        ShTex* st = &s->si.tex[i];
        if (st->flags & SNX_TEX_INDEXED) continue;
        ShTex plain;
        if (st->shadow && (s->si.tex_dc_color_mask >> i & 1)) { /* compared in the shader: a plain sampler and view */
            plain = *st;
            plain.shadow = 0;
            st = &plain;
        }
        u32 handle = tex_handle(ch, stage, tcb, st);
        /* what fs_key resolved, unless it is a render target or a copy of one: a crop of the same target at
           another size, for another stage, replaces it. Other textures stay until the next present */
        TexRes own, *x = res ? &res[i] : NULL;
        if (!x || !x->have || x->handle != handle || (x->t && x->t->is_rt)) {
            x = &own;
            tex_resolve(ch, handle, x);
        }
        Surf* t = x->t;
        if (st->flags & 2) return VK_NULL_HANDLE;
        if (st->flags & 1) t = vk_tex_from_tic_raw(x->tic);
        /* a raw load's header without 4-byte texels, or no header at all, reads zeros through the null view, as a
           missing texture does: the draw still runs (it was dropped, from one game's movie and gameplay passes) */
        if ((st->flags & 1) && !t)
            LOG_LIMIT(1, "vulkan: a raw surface load's header %08x %08x %08x has no 32-bit view: it reads zeros\n",
                      x->tic[0], x->tic[2], x->tic[4]);
        VkImageViewType want = vk_tex_view_type(st);
        int num = st->flags & 1                                    ? 1
                  : st->shadow || !((s->si.tex_int_mask >> i) & 1) ? 0
                  : ((s->si.tex_sint_mask >> i) & 1)               ? 2
                                                                   : 1;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = s->si.nimg ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        /* sampling the depth target needs it read-only: no depth writes, and no stencil writes on either face */
        int sten_w = zt && zt->f.stencil && r[0x4E0] && ((r[0x4E7] & 0xFF) || (r[0x565] && (r[0x3D6] & 0xFF)));
        int depth_ro = t && t == zt && !r[0x4BA] && !sten_w;
        u32 slots = 0; /* the color slots t is bound to, when the fragment shader may sample it in place */
        if (t && fb && g_vk.has_feedback && t->layout != VK_IMAGE_LAYOUT_UNDEFINED)
            for (u32 j = 0; j < n; j++)
                if (rt[j] == t) slots |= 1u << j;
        if (slots) {
            /* the guest reads the target's previous contents during a draw that writes it: in place, in a feedback
               loop */
            view = vk_tex_bind_view(t, want, x->tic, x->tsc, num);
            if (view) *fb |= slots;
        } else if (t && is_target(t, rt, zt, n) && !depth_ro) {
            /* The guest reads the target's previous contents during a draw that writes it. Without a feedback loop
               Vulkan can't bind one image for both uses here, so copy it before the draw starts and sample that
               copy. */
            Surf* snap = vk_rt_snapshot(t);
            if (snap != t) {
                t = snap;
                view = vk_tex_bind_view(t, want, x->tic, x->tsc, num);
                g_vk_rt_copies++;
            } else
                LOG_LIMIT(1, "vulkan: a draw samples a target it renders to (read as empty)\n");
        } else {
            t = vk_rt_prefiltered(t, st, want, num, rt, n); /* SWR: the glow passes' averaged copy (texture.c) */
            view = vk_tex_bind_view(t, want, x->tic, x->tsc, num);
        }
        const Surf* sampled = view ? t : NULL; /* the texture whose own view is bound, for the sampler */
        if (view && depth_ro) {
            *zt_ro = 1; /* render_begin moves it to the read-only layout */
            layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        } else if (view && slots)
            layout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT; /* render_begin moves it there */
        else if (view)
            vk_surf_use(vk_cmd(), t, layout, VK_SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        else {
            view = vk_null_view(want, st->shadow ? 3 : num);
            layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        if (!view) return VK_NULL_HANDLE;
        VkSampler smp = vk_tex_bind_sampler(x->tsc, st, num, sampled, want);
        if (!smp) return VK_NULL_HANDLE;
        u32 b = SNX_VKB_TEX + i;
        d.ii[b] = (VkDescriptorImageInfo){smp, view, layout};
        d.w[d.n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = set,
                                            .dstBinding = b,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            .pImageInfo = &d.ii[b]};
    }
    for (int i = 0; i < s->si.nimg; i++) {
        const u32 bank = s->si.img[i].bindless ? s->si.img[i].bl_cbuf : tcb;
        u64 cba = bank < 18 ? ch->cbind[stage][bank] : 0;
        if (!cba || !images) return VK_NULL_HANDLE;
        u32 handle = gmem_r32(cba + (u64)s->si.img[i].cbuf_off * 4);
        u64 tic_base = ((u64)r[0x55D] << 32) | r[0x55E];
        u32 tic[8];
        vk_gmem_words(tic_base + (handle & 0xFFFFF) * 32ull, tic, 8);
        Surf* surf = vk_tex_from_tic(tic);
        u64 size = vk_surf_guest_size(surf);
        int kind = s->si.img[i].kind;
        /* a draw can't also write one of its own attachments through a storage image */
        VkImageView view =
            surf && is_target(surf, rt, zt, n) ? VK_NULL_HANDLE : vk_storage_image_view(surf, &s->si, i, &size);
        if (!view) {
            LOG_LIMIT(4,
                      "vulkan: unsupported fragment storage image (draw skipped): %s handle %08x image %p format %u "
                      "raw %d integer %d storage %d levels %u depth %u size %llu target %d\n",
                      kind == 1 ? "atomic" : "store", handle, (void*)surf, surf ? surf->f.vk : 0,
                      surf ? surf->f.raw : 0, surf ? surf->f.is_int : 0, surf ? surf->storage_capable : 0,
                      surf ? surf->levels : 0, surf ? surf->depth3d : 0, (unsigned long long)size,
                      surf ? is_target(surf, rt, zt, n) : 0);
            return VK_NULL_HANDLE;
        }
        int seen = 0;
        for (int j = 0; j < images->n; j++)
            if (images->surf[j] == surf) seen = 1;
        /* an earlier draw's writes to it wait for their copy (image_pending): that copy, recorded later, takes this
           draw's writes too, over the same bytes from before them. Reading guest memory for this draw's own would land
           those writes first, waiting for the GPU (once a frame in Pokemon Legends Z-A). A landing during the setup
           submits, and the setup starts over */
        if (!seen && !image_pending(surf)) {
            u8* original = malloc((size_t)size);
            if (!original) return VK_NULL_HANDLE;
            gmem_read(surf->gva, original, size);
            images->original[images->n] = original;
        } else
            images->original[images->n] = NULL;
        images->surf[images->n] = surf;
        images->size[images->n] = (u32)size;
        images->n++;
        vk_surf_use(vk_cmd(), surf, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_STORAGE_IMAGE_ACCESS);
        u32 binding = SNX_VKB_IMG + (u32)i;
        d.ii[binding] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        d.w[d.n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = set,
                                            .dstBinding = binding,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                            .pImageInfo = &d.ii[binding]};
    }
    return desc_set_cached(layout, &d);
}

/* ------------------------------------------------------------ draws */
/* The dynamic state, pipeline and descriptor sets the previous draw recorded in the command buffer g_dyn.cb names
   (vk_cb_begun): a draw records only what differs, where every draw set about 30 states again (one game: 6,700 draws a
   frame). Every draw pipeline has these states dynamic, so binding another one keeps them. The clear path and the
   on-screen controls set or bind otherwise and forget them (vk_render_forget_state) */
static struct {
    u32 cb;
    VkPipeline pipe;
    VkPipelineLayout layout; /* the one sets were bound through: another may disturb them */
    VkDescriptorSet sets[5];
    u32 fb, cull, front, topo, restart, discard, bias_on, dt, dw, dcmp, dbounds, st;
    VkViewport vp;
    VkRect2D sc;
    float lw, bias[3], bc[4];
    u32 sfront[7], sback[7];
} g_dyn;
#define DYN(field, value, call)                                                                                        \
    do {                                                                                                               \
        if (fresh || g_dyn.field != (value)) {                                                                         \
            g_dyn.field = (value);                                                                                     \
            call;                                                                                                      \
        }                                                                                                              \
    } while (0)
#define DYNV(field, value, call)                                                                                       \
    do {                                                                                                               \
        if (fresh || memcmp(&g_dyn.field, &(value), sizeof g_dyn.field)) {                                             \
            memcpy(&g_dyn.field, &(value), sizeof g_dyn.field);                                                        \
            call;                                                                                                      \
        }                                                                                                              \
    } while (0)
void vk_render_forget_state(void) { g_dyn.cb = 0; }
u64 g_vk_drawn, g_vk_skipped;

/* NVB197_SET_VIEWPORT_COORDINATE_SWIZZLE (0x0A18) can negate Y before the scale and offset. A negative Vulkan
   height implements the same mapping without changing texture coordinates or guest render-target contents.
   Reference: NVIDIA open-gpu-doc/classes/3d/clb197.h. */
VkViewport vk_render_viewport(const u32* r) {
    float vt[6];
    memcpy(vt, &r[0x280], sizeof vt);
    float sx = vt[0], sy = vt[1], tx = vt[3], ty = vt[4], ax = sx < 0 ? -sx : sx, ay = sy < 0 ? -sy : sy;
    VkViewport vp;
    int small = 2 * ax < 1 || 2 * ay < 1;
    if (small != !(r[0x64B] & 1))
        LOG_LIMIT(1, "vulkan: viewport scale %g x %g with transform enable %x\n", sx, sy, r[0x64B]);
    if (small) {
        u32 vx = r[0x300], vy = r[0x301];
        vp = (VkViewport){vk_scf(vx & 0xFFFF), vk_scf(vy & 0xFFFF), vk_scf(vx >> 16), vk_scf(vy >> 16), 0, 1};
    } else
        vp = (VkViewport){vk_scf(tx - ax), vk_scf(ty - sy), vk_scf(2 * ax), vk_scf(2 * sy), 0, 1};
    /* A lower-left window measures y from the surface clip's bottom. Match the 3D blit path's conversion. */
    if (r[0x4EB] & 1) {
        vp.y += vk_scf((float)(r[0x3FE] >> 16));
        vp.height = -vp.height;
    }
    if (((r[0x286] >> 4) & 7) == 3) {
        vp.y += vp.height;
        vp.height = -vp.height;
    }
    float zn, zf;
    memcpy(&zn, &r[0x302], 4);
    memcpy(&zf, &r[0x303], 4);
    if (vt[2] != 0.0f || vt[5] != 0.0f) {
        zn = vt[5] - ((r[0x35F] & 1) ? 0.0f : vt[2]);
        zf = vt[5] + vt[2];
    }
    vp.minDepth = zn < 0 ? 0 : zn > 1 ? 1 : zn;
    vp.maxDepth = zf < 0 ? 0 : zf > 1 ? 1 : zf;
    return vp;
}
/* the vertices of the geometry program's input primitive: the tessellator's output primitive (0xC8: the domain in
   bits 0-1, isolines 0, and the output in bits 8-9, points 0), or the draw's topology */
static u32 gs_input_vertices(u32 topology, int tes_on, u32 params) {
    if (tes_on) return ((params >> 8) & 3) == 0 ? 1 : (params & 3) == 0 ? 2 : 3;
    return topology == 0                      ? 1
           : topology <= 3                    ? 2
           : topology == 10 || topology == 11 ? 4
           : topology == 12 || topology == 13 ? 6
                                              : 3;
}

static VkFrontFace front_face_at_viewport(const u32* r, float viewport_height) {
    int cw = r[0x647] == 0x900;
    /* A negative Vulkan viewport already reverses framebuffer winding. WINDOW_ORIGIN_FLIP_Y also reverses guest
       facing, so only compensate for that bit when the viewport has not performed the same reversal. */
    if ((r[0x4EB] & 0x10) && viewport_height > 0) cw = !cw;
    return cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}
int vk_render_depth_clamp(const u32* r) {
    /* GeometryClip: Passthrough, FrustumXYZ and FrustumZ clip depth; the remaining modes clamp it. */
    u32 mode = (r[0x64F] >> 11) & 7;
    return mode != 1 && mode != 3 && mode != 5;
}

VkFrontFace vk_render_front_face(const u32* r) { return front_face_at_viewport(r, vk_render_viewport(r).height); }

/* the Vulkan topology of a draw's (the 3D engine's numbers, 14 a patch list) */
static VkPrimitiveTopology topology_vk(u32 topology) {
    static const VkPrimitiveTopology adjacent[5] = {
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    if (topology >= 10 && topology <= 14) return adjacent[topology - 10];
    switch (topology) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case 2: /* line loop: drawn open */
        case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 5:
        case 8: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; /* quad strips are triangle strips */
        case 6:
        case 9: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;   /* polygons are fans */
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; /* triangles, and quads made into them */
    }
}
/* ------------------------------------------------------------ a draw's shaders and pipeline, kept between draws */
/* the register groups a draw's shaders and pipeline key are made from */
#define DRAW_GROUPS (R3D_SHADER | R3D_VATTR | R3D_VFORMAT | R3D_RTFORMAT | R3D_BLEND | R3D_PIPEMISC | R3D_XFB)
/* the programs a draw's registers select */
typedef struct {
    VkSh *vs, *tcs, *tes, *gs; /* the stages between NULL when off */
    VkSh* fs0;                 /* the fragment program as it is, NULL when off: its textures pick the variant drawn */
    u32 attr_int, attr_sint;   /* the integer vertex attributes, of all 32, which key the vertex program */
    u32 rt_int, rt_sint;       /* the integer color targets the registers name (rt_int_targets) */
    u32 topology;              /* the draw's, every primitive a patch with tessellation */
    int xfb_on;                /* transform feedback captures */
} DrawShaders;
/* the vertex bindings a pipeline key describes, for finding their buffers each draw: binding b reads guest stream
   key[b] & 31 from its base offset key[b] >> 5 on, with pad bytes past the stream's limit (an attribute reaching past
   the stride of its last vertex), or the constants of the attributes without data when key[b] is ~0u */
typedef struct {
    u32 key[33], pad[33];
    VkDeviceSize stride[33];
    u32 used;  /* the streams whose limits the bindings were planned with */
    u32 empty; /* those of them that were empty, so their attributes read the constants */
} VertexPlan;
/* what a pipeline key takes from the surfaces drawn to: the color slots up to the last bound one, those drawn to,
   which of them are integer, their formats, and the depth target's */
typedef struct {
    u32 n, present, is_int, stencil;
    u32 color[8], depth;
} TargetFormats;
/* What the last draw made from the registers, kept for the next draw while nothing it was made from changed: the
   register groups (pend: those a method changed since, all of them when another channel draws), the draw's topology,
   the shaders' code (trusted within an epoch, as vk_shader_get trusts it) and the constant buffer words their
   translation read (checked each draw, as vk_shader_get checks them on a hit). What else goes into it is found each
   draw and compared: the fragment variant's key (its textures' formats), the targets' formats, which vertex streams
   are empty, and whether the draw samples a target it renders to. Draws made all of it again, a 1 KiB key built and
   compared included, for each of the ~3,400 draws a frame in one game, most of them with the state of the draw
   before. SNX_DIRTY_CHECK makes it again for each draw that keeps it, and warns of any difference: a register missing
   from a group, or an input that isn't compared */
static struct {
    u32 ch_id, pend;
    u32 topology; /* the draw's argument, before tessellation makes it a patch list */
    u32 epoch;    /* g_vk_epoch */
    DrawShaders sh; /* the rest holds when sh.vs is set */
    FsKey fkey;
    VkSh* fs;
    VertexPlan plan;
    TargetFormats tf;
    PipeKey k; /* k and pipe hold together whenever pipe is set */
    VkPipeline pipe;
    /* the vertex registers the state was made from, for a change of R3D_VATTR or R3D_VFORMAT that leaves them as they
       were: attributes and streams the vertex program doesn't read. The attributes changed in a third of one game's
       draws, and comparing what was read kept 66% of its draws where the groups alone kept 60% */
    u32 vattr[32];      /* the attributes the vertex program reads, 0 for the others */
    u32 vstreams;       /* the streams those attributes name */
    u32 vstream[32][3]; /* each one's stride and enable word, divisor, and whether it is per instance */
    int check;          /* SNX_DIRTY_CHECK, -1 until read */
} g_draw = {.pend = ~0u, .check = -1};
u64 g_vk_draw_kept, g_vk_draw_checked;

static void draw_differs(const char* what) {
    WARN_LIMIT(32, "vulkan: SNX_DIRTY_CHECK: a draw that kept the last draw's state has another %s when it is made "
                   "again (%llu draws checked)\n", what, (unsigned long long)g_vk_draw_checked);
}
/* the integer vertex attributes (int, signed sint) of all 32, which key the vertex program */
static void attr_ints(const u32* r, u32* ai, u32* as) {
    *ai = *as = 0;
    for (int i = 0; i < 32; i++) {
        u32 ty = (r[0x458 + i] >> 27) & 7;
        if (ty == 3 || ty == 4) {
            *ai |= 1u << i;
            if (ty == 3) *as |= 1u << i;
        }
    }
}
static void stream_regs(const u32* r, u32 s, u32* w) {
    w[0] = r[0x700 + s * 4];
    w[1] = r[0x703 + s * 4];
    w[2] = r[0x620 + s];
}
/* keeps the vertex registers of the state made, for vertex_regs_hold */
static void vertex_regs_keep(const u32* r, u32 in) {
    g_draw.vstreams = 0;
    for (u32 i = 0; i < 32; i++) {
        g_draw.vattr[i] = in >> i & 1 ? r[0x458 + i] : 0;
        if (in >> i & 1) g_draw.vstreams |= 1u << (r[0x458 + i] & 31);
    }
    for (u32 m = g_draw.vstreams; m; m &= m - 1) stream_regs(r, (u32)__builtin_ctz(m), g_draw.vstream[__builtin_ctz(m)]);
}
/* whether what the kept state read of the vertex registers is as it was: all that the shaders, the key and the
   vertex bindings take from them */
static int vertex_regs_hold(const u32* r, const DrawShaders* s) {
    u32 in = s->vs->si.in_mask, ai, as, w[3];
    attr_ints(r, &ai, &as);
    if (ai != s->attr_int || as != s->attr_sint) return 0;
    for (u32 m = in; m; m &= m - 1)
        if (r[0x458 + __builtin_ctz(m)] != g_draw.vattr[__builtin_ctz(m)]) return 0;
    for (u32 m = g_draw.vstreams; m; m &= m - 1) {
        stream_regs(r, (u32)__builtin_ctz(m), w);
        if (memcmp(w, g_draw.vstream[__builtin_ctz(m)], sizeof w)) return 0;
    }
    return 1;
}
/* whether the state kept in g_draw holds for this draw of ch, as far as what is checked before drawing tells */
static int draw_kept(const GpuChannel* ch, u32 topology, u32 epoch) {
    const DrawShaders* s = &g_draw.sh;
    u32 pend = g_draw.pend & DRAW_GROUPS;
    if (!s->vs || (pend & ~(R3D_VATTR | R3D_VFORMAT)) || g_draw.topology != topology || g_draw.epoch != epoch)
        return 0;
    if (pend) { /* the vertex registers changed: those the state read may not have */
        if (!vertex_regs_hold(ch->r3d, s)) return 0;
        g_draw.pend &= ~pend;
    }
    if (s->xfb_on != (g_vk.has_xfb && ch->r3d[0x1D1] && xfb_counters())) return 0; /* the counters were made since */
    const VkSh* used[6] = {s->vs, s->tcs, s->tes, s->gs, s->fs0, g_draw.fs};
    for (int i = 0; i < 6; i++)
        if (used[i] && used[i]->cbr.n && !vk_shader_cb_hold(ch, used[i])) return 0;
    return 1;
}
static int draw_shaders_same(const DrawShaders* a, const DrawShaders* b) {
    return a->vs == b->vs && a->tcs == b->tcs && a->tes == b->tes && a->gs == b->gs && a->fs0 == b->fs0 &&
           a->attr_int == b->attr_int && a->attr_sint == b->attr_sint && a->rt_int == b->rt_int && a->rt_sint == b->rt_sint && a->topology == b->topology && a->xfb_on == b->xfb_on;
}
/* the shaders of ch's registers for a draw of topology: 0 when the draw is skipped */
static int draw_shaders(GpuChannel* ch, u32 topology, DrawShaders* o) {
    const u32* r = ch->r3d;
    /* shaders: VertexB always runs (after VertexA when that is on), the fragment stage when enabled */
    u64 code = ((u64)r[0x582] << 32) | r[0x583];
    u32 attr_int, attr_sint, clip;
    int tcs_on, tes_on, gs_on;
    attr_ints(r, &attr_int, &attr_sint);
    /* the stages between: tessellation control and evaluation (program slots 2 and 3), geometry (slot 4) */
    tcs_on = r[0x820] & 1;
    tes_on = r[0x830] & 1;
    gs_on = r[0x840] & 1;
    if ((tcs_on || tes_on || gs_on) && g_vk_stage_sets < 5) {
        LOG_LIMIT(1, "vulkan: this GPU binds too few descriptor sets for tessellation and geometry (draw skipped)\n");
        return 0;
    }
    if (tcs_on != tes_on) {
        /* the hardware tessellates with the levels of registers 0xC9-0xCE without a control program, which
           Vulkan can't: no game is known to */
        LOG_LIMIT(1, "vulkan: tessellation with only one of its two programs (draw skipped)\n");
        return 0;
    }
    if ((topology == 14 && !tes_on) || (topology >= 10 && topology <= 13 && !gs_on))
        return 0; /* patches and adjacency need the stages above */
    if (tes_on) topology = 14; /* every primitive is a patch with tessellation, as Vulkan requires */
    /* SetUserClipEnable (0x544): a clip distance the program writes clips only when its bit is set, in the last
       stage before rasterization */
    clip = r[0x544] & 0xFF;
    /* transform feedback (0x1D1): what the last stage before rasterization captures, from each buffer's control
       (0x1C0 + 4 * i: its stream, the words of a vertex, their stride in bytes) and the layout of those words
       (0xA00 + 0x20 * i, a byte each: an attribute word, as Eden's MakeTransformFeedbackVaryings reads them) */
    ShXfb xfb;
    int xfb_on = g_vk.has_xfb && r[0x1D1] && xfb_counters();
    if (xfb_on) {
        memset(&xfb, 0, sizeof xfb);
        for (int i = 0; i < 4; i++) {
            u32 words = r[0x1C1 + 4 * i];
            xfb.count[i] = (u8)(words > 128 ? 128 : words);
            xfb.stride[i] = (u16)r[0x1C2 + 4 * i];
            memcpy(xfb.layout[i], &r[0xA00 + 0x20 * i], sizeof xfb.layout[i]); /* word n / 4's byte n % 4 */
        }
    }
    o->topology = topology;
    o->xfb_on = xfb_on;
    o->attr_int = attr_int;
    o->attr_sint = attr_sint;
    o->vs = vk_shader_get(ch, code + r[0x811], (r[0x800] & 1) ? code + r[0x801] : 0, ST_VS, attr_int, attr_sint, 0, 0,
                          tes_on || gs_on ? SNX_SH_NOT_LAST : clip, 0, 0, 0,
                          xfb_on && !tes_on && !gs_on ? &xfb : NULL);
    o->tcs =
        tcs_on ? vk_shader_get(ch, code + r[0x821], 0, ST_TCS, 0, 0, 0, 0, SNX_SH_NOT_LAST, 0, 0, 0, NULL) : NULL;
    o->tes = tes_on ? vk_shader_get(ch, code + r[0x831], 0, ST_TES, 0, 0, r[0xC8] & 0x333, 0,
                                    gs_on ? SNX_SH_NOT_LAST : clip, 0, 0, 0, xfb_on && !gs_on ? &xfb : NULL)
                    : NULL;
    o->gs = gs_on ? vk_shader_get(ch, code + r[0x841], 0, ST_GS, 0, 0, gs_input_vertices(topology, tes_on, r[0xC8]),
                                  0, clip, 0, 0, 0, xfb_on ? &xfb : NULL)
                  : NULL;
    if ((tcs_on && (!o->tcs || o->tcs->failed)) || (tes_on && (!o->tes || o->tes->failed)) ||
        (gs_on && (!o->gs || o->gs->failed)) || !o->vs || o->vs->failed)
        return 0;
    o->fs0 = (r[0x850] & 1) ? vk_shader_get(ch, code + r[0x851], 0, ST_FS, 0, 0, 0, 0, 0, 0, 0, 0, NULL) : NULL;
    rt_int_targets(r, &o->rt_int, &o->rt_sint);
    return 1;
}
static u64 stream_start(const u32* r, u32 s) { return ((u64)r[0x701 + s * 4] << 32) | r[0x702 + s * 4]; }
/* the bytes of vertex stream s, from its start to its limit (0x7C0 + 2 * s, inclusive): 0 when the limit is below */
static u64 stream_len(const u32* r, u32 s) {
    u64 start = stream_start(r, s), end = ((u64)r[0x7C0 + s * 2] << 32) | r[0x7C1 + s * 2];
    return end >= start ? end - start + 1 : 0;
}
/* those of the streams that are empty */
static u32 streams_empty(const u32* r, u32 streams) {
    u32 e = 0;
    while (streams) {
        u32 s = (u32)__builtin_ctz(streams);
        streams &= streams - 1;
        if (!stream_len(r, s)) e |= 1u << s;
    }
    return e;
}
static void target_formats(Surf* const* rt, const Surf* zt, u32 n, TargetFormats* tf) {
    *tf = (TargetFormats){.n = n};
    for (u32 i = 0; i < n; i++)
        if (rt[i]) {
            tf->present |= 1u << i;
            tf->is_int |= (rt[i]->f.is_int ? 1u : 0u) << i;
            tf->color[i] = rt[i]->f.vk;
        }
    if (zt) {
        tf->depth = zt->f.vk;
        tf->stencil = zt->f.stencil ? 1 : 0;
    }
}
/* the pipeline key of the shaders (fs the fragment variant drawn), ch's registers and the targets drawn to, all but
   the feedback loop, and the vertex bindings it describes: 0 when the draw is skipped */
static int draw_key(const GpuChannel* ch, const DrawShaders* sh, const VkSh* fs, Surf* const* rt, const Surf* zt,
                    u32 n, PipeKey* k, VertexPlan* pl) {
    const u32* r = ch->r3d;
    const VkSh* vs = sh->vs;
    u32 topology = sh->topology;
    memset(k, 0, sizeof *k);
    memset(pl, 0, sizeof *pl);
    k->vs = vs->mod;
    k->fs = fs ? fs->mod : VK_NULL_HANDLE;
    k->depth_clamp = g_vk.feat.depthClamp && vk_render_depth_clamp(r);
    /* PROVOKING_VERTEX (0x5A1): 1 the last vertex, GL's and NVN's default, 0 the first, Vulkan's */
    k->provoking_last = (r[0x5A1] & 1) && g_vk.has_provoking_last;
    if ((r[0x5A1] & 1) && !g_vk.has_provoking_last)
        LOG_LIMIT(1, "vulkan: flat inputs take the first vertex of a primitive, not the last the game asks for\n");
    /* LOGIC_OP (0x671 on, 0x672 the operation as GL numbers it from CLEAR 0x1500 to SET 0x150F, VkLogicOp's order) */
    if (r[0x671] & 1) {
        if (g_vk.feat.logicOp)
            k->logic_op = (r[0x672] & 15) + 1;
        else
            LOG_LIMIT(1, "vulkan: this GPU has no logic operations: targets are blended instead\n");
    }
    VkPrimitiveTopology topo = topology_vk(topology);
    /* adjacency and patches are their own topologies: the geometry program's input primitive is one of them */
    k->topo_class = topo >= VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ? topo
                    : topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST             ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                    : topo <= VK_PRIMITIVE_TOPOLOGY_LINE_STRIP             ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                           : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    k->tcs = sh->tcs ? sh->tcs->mod : VK_NULL_HANDLE;
    k->tes = sh->tes ? sh->tes->mod : VK_NULL_HANDLE;
    k->gs = sh->gs ? sh->gs->mod : VK_NULL_HANDLE;
    {
        const VkSh* stages[5] = {vs, fs, sh->tcs, sh->tes, sh->gs};
        for (int i = 0; i < 5; i++)
            if (stages[i] && stages[i]->indexed) k->arrays |= 1u << shader_vk_set(stages[i]->stage);
    }
    if (sh->tes) { /* 0x373: the control points of a patch */
        k->patch = r[0x373] ? r[0x373] : 1;
        if (k->patch > g_vk.props.limits.maxTessellationPatchSize)
            k->patch = g_vk.props.limits.maxTessellationPatchSize;
    }
    k->polygon = !g_vk.feat.fillModeNonSolid ? VK_POLYGON_MODE_FILL
                 : r[0x36B] == 0x1B01        ? VK_POLYGON_MODE_LINE
                 : r[0x36B] == 0x1B00        ? VK_POLYGON_MODE_POINT
                                             : VK_POLYGON_MODE_FILL;

    /* vertex streams: each used guest stream is a binding, and so is an attribute at an offset past the device's
       limit or reaching past its stream's stride (it is read from its own offset there); one more holds the 0,0,0,1
       of attributes without data (an attribute marked constant, a stream disabled or empty, or a format the device
       can't read) */
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    u32 maxoff = lim->maxVertexInputAttributeOffset;
    u32 nloc = lim->maxVertexInputAttributes < 32 ? lim->maxVertexInputAttributes : 32;
    u32 maxbind = lim->maxVertexInputBindings < 33 ? lim->maxVertexInputBindings : 33;
    if (nloc < 32 && (vs->si.in_mask >> nloc)) {
        LOG_LIMIT(1, "vulkan: a vertex shader reads attributes past the %u this GPU has (draw skipped)\n", nloc);
        return 0;
    }
    int cbind = -1, over = 0;
    for (u32 i = 0; i < nloc && !over; i++) {
        if (!(vs->si.in_mask & (1u << i))) continue;
        u32 a = r[0x458 + i];
        u32 sbuf = a & 31, off = (a >> 7) & 0x3FFF, size = (a >> 21) & 0x3F, ty = (a >> 27) & 7;
        const u32* va = &r[0x700 + sbuf * 4];
        VkFormat fmt = attr_format(size, ty, (a >> 31) & 1);
        int is_int = ty == 3 || ty == 4, data = !((a >> 6) & 1) && (va[0] & 0x1000) && vertex_format_ok(fmt);
        if (data) {
            pl->used |= 1u << sbuf;
            if (!stream_len(r, sbuf)) {
                pl->empty |= 1u << sbuf;
                data = 0;
            }
        }
        if (!data) {
            if (cbind < 0) {
                if (k->nbind == maxbind) {
                    over = 1;
                    continue;
                }
                pl->key[k->nbind] = ~0u;
                cbind = (int)k->nbind;
                k->nbind++;
            }
            k->attr[k->nattr].loc = i;
            k->attr[k->nattr].binding = (u32)cbind;
            k->attr[k->nattr].format = is_int ? (ty == 3 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                                              : VK_FORMAT_R32G32B32A32_SFLOAT;
            k->attr[k->nattr].offset = is_int ? 16 : 0;
            k->nattr++;
            continue;
        }
        u32 stride = va[0] & 0xFFF, asz = attr_bytes(size);
        /* an attribute may be wider than its stream's stride (a game may read 8-byte attributes from
           6-byte strides, its world geometry): Maxwell reads the rest from the next vertex, and so does Vulkan,
           which allows it outside the portability subset (vertexAttributeAccessBeyondStride) */
        if (stride > lim->maxVertexInputBindingStride) {
            LOG_LIMIT(1, "vulkan: a vertex stream with stride %u can't be read (draw skipped)\n", stride);
            return 0;
        }
        /* an attribute past the stride, or past the offsets the GPU allows, gets a binding of its own starting at it */
        u32 base = off > maxoff || (stride && off + asz > stride) ? off : 0;
        u32 key = sbuf | base << 5, b = 0;
        while (b < k->nbind && pl->key[b] != key) b++;
        if (b == k->nbind) {
            if (b == maxbind) {
                over = 1;
                continue;
            }
            /* the stream's limit covers its last vertex's stride: an attribute reaching past the stride reads past it */
            pl->pad[b] = stride && base + asz > stride ? base + asz - stride : 0;
            pl->stride[b] = stride;
            pl->key[b] = key;
            u32 div = r[0x620 + sbuf] ? (va[3] ? va[3] : 1) : 0;
            if (div > 1 && !g_vk.has_divisor) div = 1;
            k->bind[b].divisor = div;
            k->bind[b].stride = g_vk.has_dynamic_stride ? 0 : stride;
            k->nbind++;
        }
        k->attr[k->nattr].loc = i;
        k->attr[k->nattr].binding = b;
        k->attr[k->nattr].format = fmt;
        k->attr[k->nattr].offset = off - base;
        k->nattr++;
    }
    if (over) {
        LOG_LIMIT(1, "vulkan: a draw needs more vertex bindings than the %u this GPU has (draw skipped)\n", maxbind);
        return 0;
    }

    /* attachments and blending */
    k->ncolor = n;
    for (u32 i = 0; i < n; i++) {
        if (!rt[i]) continue; /* format UNDEFINED, nothing written */
        k->color[i] = r[0x56E] & 1 ? rt[i]->f.vk : vk_srgb_as(rt[i]->f.vk, 0);
        u32 cm = r[0x3E4] ? r[0x680] : r[0x680 + i];
        k->blend[i].mask = ((cm & 0xF) ? VK_COLOR_COMPONENT_R_BIT : 0) |
                           ((cm & 0xF0) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                           ((cm & 0xF00) ? VK_COLOR_COMPONENT_B_BIT : 0) |
                           ((cm & 0xF000) ? VK_COLOR_COMPONENT_A_BIT : 0);
        if (r[0x4D8 + i] && !rt[i]->f.is_int) {
            /* the alpha fields are used even without separate alpha, as on hardware */
            const u32* b = &r[0x780 + i * 8];
            u32 eq, sc, dc, eqa, sa, da;
            if (r[0x4B9]) {
                eq = b[1], sc = b[2], dc = b[3];
                eqa = b[4] ? b[4] : b[1], sa = b[5] ? b[5] : b[2], da = b[6] ? b[6] : b[3];
            } else {
                eq = r[0x4D0], sc = r[0x4D1], dc = r[0x4D2];
                eqa = r[0x4D3] ? r[0x4D3] : r[0x4D0], sa = r[0x4D4] ? r[0x4D4] : r[0x4D1];
                da = r[0x4D6] ? r[0x4D6] : r[0x4D2];
            }
            k->blend[i].en = 1;
            k->blend[i].cop = blend_op(eq);
            k->blend[i].sc = blend_factor(sc);
            k->blend[i].dc = blend_factor(dc);
            k->blend[i].aop = blend_op(eqa);
            k->blend[i].sa = blend_factor(sa);
            k->blend[i].da = blend_factor(da);
        }
    }
    if (zt) {
        k->depth = zt->f.vk;
        k->stencil = zt->f.stencil ? zt->f.vk : VK_FORMAT_UNDEFINED;
    }
    return 1;
}

void vk_render_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
                    u32 instances) {
    vk_fmap_draw();
    vk_dlss_draw_begin();
    u32* r = ch->r3d;
    /* the stages between the vertex and fragment programs and their sets */
    int bset_ok, b;
    VkSh *vs, *tcs, *tes, *gs, *between[3];
    VkDescriptorSet bset[3];
    if (!count || !instances) return;
    vk_frame_check();
    if (g_draw.check < 0) {
        const char* e = getenv("SNX_DIRTY_CHECK");
        g_draw.check = e && *e && *e != '0';
        if (g_draw.check)
            LOG("vulkan: SNX_DIRTY_CHECK: draws that keep the last draw's shaders and pipeline make them again to "
                "compare\n");
    }
    /* the register groups changed since the kept state was made: all of them when another channel draws */
    if (g_draw.ch_id != ch->id) {
        g_draw.ch_id = ch->id;
        g_draw.pend = ~0u;
    }
    g_draw.pend |= ch->r3d_dirty;
    ch->r3d_dirty = 0;
    u32 initial_first = first, initial_count = count, draw_topo = topology;
    int retries = 0, initial_indexed = indexed;
retry:
    vk_buf_writable_abort();
    indexed = initial_indexed;
    first = initial_first;
    count = initial_count;
    u64 setup_frame = vk_submit_number();
    u64 code = ((u64)r[0x582] << 32) | r[0x583];
    u32 epoch = __atomic_load_n(&g_vk_epoch, __ATOMIC_RELAXED);
    /* kept: the state the last draw made holds for this one, as far as each check below tells. Each part is made
       here when it doesn't (and when SNX_DIRTY_CHECK compares) */
    int kept = draw_kept(ch, draw_topo, epoch);
    if (kept && g_draw.check) g_vk_draw_checked++;
    DrawShaders sh;
    if (kept) sh = g_draw.sh;
    if (!kept || g_draw.check) {
        DrawShaders made;
        if (!draw_shaders(ch, draw_topo, &made)) {
            if (kept) {
                draw_differs("shaders");
                g_draw.sh.vs = NULL;
            }
            g_vk_skipped++;
            return;
        }
        if (kept && !draw_shaders_same(&made, &sh)) {
            draw_differs("shaders");
            kept = 0;
        }
        sh = made;
    }
    topology = sh.topology;
    vs = sh.vs;
    tcs = sh.tcs;
    tes = sh.tes;
    gs = sh.gs;
    int xfb_on = sh.xfb_on;
    /* the fragment shader's variant, keyed on its textures each draw: they are resolved for stage_set anyway. fres is
       filled for the fragment program's textures (each resolved, have set), and read only for them: a clear of all
       SH_MAXTEX (10 KiB) and a check of all of them against each target were per draw */
    TexRes fres[SH_MAXTEX];
    VkSh* last = gs ? gs : tes ? tes : vs;
    VkSh* fs = NULL;
    FsKey fk = {0};
    if (sh.fs0) {
        fs_key(ch, sh.fs0, fres, last->si.out_mask, sh.rt_int, sh.rt_sint, &fk);
        if (kept && memcmp(&fk, &g_draw.fkey, sizeof fk)) kept = 0;
        if (kept) fs = g_draw.fs;
        if (!kept || g_draw.check) {
            VkSh* made = fs_variant(ch, code + r[0x851], sh.fs0, &fk);
            if (kept && made != fs) {
                draw_differs("fragment shader");
                kept = 0;
            }
            fs = made;
        }
        if (fs && fs->failed) {
            g_vk_skipped++;
            return;
        }
    }
    Surf* rt[8];
    Surf* zt;
    u32 n;
    read_targets(ch, rt, &zt, &n);
    for (u32 i = 0; i < n; i++)
        if (trace_target(rt[i]))
            LOG_LIMIT(64,
                      "gpu-trace-target: draw color slot %u target=%llx %ux%u count=%u indexed=%d vs=%llx fs=%llx\n", i,
                      (unsigned long long)rt[i]->gva, rt[i]->w, rt[i]->h, count, indexed,
                      (unsigned long long)(code + r[0x811]), (unsigned long long)(code + r[0x851]));
    if (trace_target(zt))
        LOG_LIMIT(64, "gpu-trace-target: draw depth target=%llx %ux%u count=%u indexed=%d vs=%llx fs=%llx\n",
                  (unsigned long long)zt->gva, zt->w, zt->h, count, indexed, (unsigned long long)(code + r[0x811]),
                  (unsigned long long)(code + r[0x851]));
    /* a color target the fragment shader samples but doesn't write (no output for it, or a write mask of 0) is drawn
       without: a game's lighting passes may keep the whole G-buffer bound while they read some of it, which
       Maxwell allows, and Vulkan can't sample an attachment of the pass */
    if (fs)
        for (u32 j = 0; j < n; j++) {
            u32 cm = r[0x3E4] ? r[0x680] : r[0x680 + j];
            if (!rt[j] || (((fs->si.sph[18] >> (j * 4)) & 15) && (cm & 0xFFFF))) continue;
            for (int i = 0; i < (fs->si.ntex > SH_MAXTEX ? SH_MAXTEX : fs->si.ntex); i++)
                if (fres[i].have && fres[i].t == rt[j]) {
                    rt[j] = NULL;
                    break;
                }
        }
    if (!n && !zt && !xfb_on) { /* nothing drawn to and nothing captured */
        g_vk_skipped++;
        return;
    }
    static u32 last_cw, last_ch; /* the size of the last color target drawn to */
    if (n && rt[0]) {
        last_cw = rt[0]->w;
        last_ch = rt[0]->h;
    }
    if (!g_settings.shadows && !n && zt && (zt->w != last_cw || zt->h != last_ch)) {
        /* shadows=0: depth-only passes at another size (shadow maps) are skipped, and the map is cleared to far once
           a frame so it reads as fully lit */
        static u32 cleared_frame = ~0u;
        static Surf* cleared;
        if (cleared_frame != g_vk_present || cleared != zt) {
            cleared_frame = g_vk_present;
            cleared = zt;
            if (render_begin(rt, zt, 0, 0, r[0x56E] & 1, 0)) {
                VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                                     (zt->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)};
                a.clearValue.depthStencil = (VkClearDepthStencilValue){1.0f, 0};
                VkClearRect cr = {{{0, 0}, g_rp.ext}, 0, 1};
                rec_CmdClearAttachments(vk_cmd(), 1, &a, 1, &cr);
                target_written(zt);
            }
        }
        g_vk_skipped++;
        return;
    }

    /* the pipeline key and its vertex bindings: the kept ones when the targets' formats and the empty streams are the
       same too */
    TargetFormats tf;
    target_formats(rt, zt, n, &tf);
    if (kept && (memcmp(&tf, &g_draw.tf, sizeof tf) || streams_empty(r, g_draw.plan.used) != g_draw.plan.empty))
        kept = 0;
    PipeKey made_k;
    VertexPlan made_pl;
    const PipeKey* k = &g_draw.k;
    const VertexPlan* pl = &g_draw.plan;
    if (!kept || g_draw.check) {
        if (!draw_key(ch, &sh, fs, rt, zt, n, &made_k, &made_pl)) {
            if (kept) {
                draw_differs("pipeline key");
                g_draw.sh.vs = NULL;
            }
            g_vk_skipped++;
            return;
        }
        if (kept && memcmp(&made_pl, pl, sizeof made_pl)) {
            draw_differs("vertex bindings");
            kept = 0;
        }
        k = &made_k;
        pl = &made_pl;
    }
    /* each binding's buffer: all of them prefetched first (vk_buf_prefetch), then looked up */
    VkBuffer vbuf[33];
    VkDeviceSize voff[33];
    struct {
        u64 gva, len;
        u32 b, base;
    } vq[33];
    u32 nvq = 0;
    for (u32 v = 0; v < k->nbind; v++) {
        if (pl->key[v] == ~0u) {
            /* the constants: staging that fails skips the draw (it once drew without the attributes) */
            if (!frame_consts(&vbuf[v], &voff[v])) {
                g_vk_skipped++;
                return;
            }
            voff[v] += 16;
            continue;
        }
        u32 sbuf = pl->key[v] & 31;
        u64 len = stream_len(r, sbuf);
        if (len > (64u << 20)) len = 64u << 20;
        vq[nvq].gva = stream_start(r, sbuf);
        vq[nvq].len = len + pl->pad[v];
        vq[nvq].b = v;
        vq[nvq].base = pl->key[v] >> 5;
        nvq++;
    }
    for (u32 q = 0; q < nvq; q++) vk_buf_prefetch(vq[q].gva, vq[q].len, 0);
    for (u32 q = 0; q < nvq; q++) vk_buf_prefetch(vq[q].gva, vq[q].len, 1);
    for (u32 q = 0; q < nvq; q++) {
        if (!vk_buf_get(vq[q].gva, vq[q].len, &vbuf[vq[q].b], &voff[vq[q].b])) {
            g_vk_skipped++;
            return;
        }
        voff[vq[q].b] += vq[q].base;
    }
    VkPrimitiveTopology topo = topology_vk(topology);

    /* descriptor sets (may record uploads and barriers, so before the pass opens) */
    VkDrawStorage storage;
    draw_storage_read(ch, &storage, vs, fs);
    int alias = storage.n[0] || storage.n[1] ? vk_buf_draw_has_alias(&storage) : 0;
    if (alias < 0) {
        g_vk_skipped++;
        return;
    }
    if ((storage.n[0] || storage.n[1]) && !vk_buf_draw_prepare(&storage)) {
        g_vk_skipped++;
        return;
    }
    int zt_ro = 0;
    u32 fb = 0;
    DrawImages images = {0};
    VkDescriptorSet vset = stage_set(ch, 0, vs, rt, zt, n, &zt_ro, NULL, NULL, &storage, NULL);
    VkDescriptorSet fset = fs ? stage_set(ch, 4, fs, rt, zt, n, &zt_ro, &fb, fres, &storage, &images) : VK_NULL_HANDLE;
    /* the sets of the stages between, at their stage's index (shader_vk_set) */
    between[0] = tcs;
    between[1] = tes;
    between[2] = gs;
    bset_ok = 1;
    for (b = 0; b < 3; b++) {
        bset[b] = VK_NULL_HANDLE;
        if (!between[b]) continue;
        if (between[b]->si.ngbuf || between[b]->si.nimg) { /* global memory and images are read in VS and FS */
            LOG_LIMIT(1, "vulkan: a tessellation or geometry program uses global memory or images (draw skipped)\n");
            bset_ok = 0;
            break;
        }
        bset[b] = stage_set(ch, b + 1, between[b], rt, zt, n, &zt_ro, NULL, NULL, &storage, NULL);
        if (!bset[b]) bset_ok = 0;
    }
    if (!vset || (fs && !fset) || !bset_ok) {
        draw_images_discard(&images);
        g_vk_skipped++;
        return;
    }
    /* indices, up to the end of the index buffer (0x5F4, inclusive). What is made into other indices on the CPU (quads,
       8-bit indices, another restart index) is capped */
    VkBuffer ibuf = VK_NULL_HANDLE;
    u64 ioff = 0;
    VkIndexType itype = VK_INDEX_TYPE_UINT32;
    u32 ifmt = indexed == 3 ? 2 : r[0x5F6], isz = ifmt == 0 ? 1 : ifmt == 1 ? 2 : 4;
    u64 ia = ((u64)r[0x5F2] << 32) | r[0x5F3], iend = ((u64)r[0x5F4] << 32) | r[0x5F5];
    if (indexed && indexed != 3 && iend >= ia && (u64)first + count > (iend - ia + 1) / isz) {
        u64 avail = (iend - ia + 1) / isz;
        LOG_LIMIT(1, "vulkan: a draw reads indices %u..%u of an index buffer of %llu (cut there)\n", first,
                  first + count - 1, (unsigned long long)avail);
        if (first >= avail) {
            draw_images_discard(&images);
            return;
        }
        count = (u32)(avail - first);
    }
    if ((topology == 7 || (indexed && ifmt == 0)) && count > (1u << 24)) {
        LOG_LIMIT(1, "vulkan: a draw of %u vertices converted on the CPU (cut to %u)\n", count, 1u << 24);
        count = 1u << 24;
    }
    /* Vulkan restarts at all ones of the index type, in strips and fans; the guest's restart index is 0x592 */
    int restart =
        r[0x591] && topo != VK_PRIMITIVE_TOPOLOGY_POINT_LIST && topo != VK_PRIMITIVE_TOPOLOGY_LINE_LIST &&
        topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && topo != VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY &&
        topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY && topo != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    u32 icount = count;
    int quads_indexed = 0;
    if (topology == 7) { /* quads -> two triangles each */
        u32 nq = count / 4;
        const u8* src = NULL;
        if (indexed)
            src = indexed == 3 ? (const u8*)(ch->inline_indices + first)
                               : gmem_ptr(ia + (u64)first * isz, (u64)count * isz);
        u32* idx = (u32*)vk_stage((u64)nq * 24 + 4, vk_stage_align(), &ibuf, &ioff);
        if (!idx) {
            draw_images_discard(&images);
            return;
        }
        /* guest indices are little-endian, the inline ones host words (already widened), and what the device reads
           is little-endian again */
        for (u32 q = 0; q < nq; q++) {
            u32 v[4];
            for (int j = 0; j < 4; j++) {
                u32 e = q * 4 + j;
                u8 g[4] = {0};
                if (!indexed)
                    v[j] = first + e;
                else if (indexed == 3)
                    v[j] = ch->inline_indices[first + e];
                else {
                    if (src)
                        memcpy(g, src + (u64)e * isz, isz);
                    else
                        gmem_read(ia + ((u64)first + e) * isz, g, isz);
                    v[j] = isz == 1 ? g[0] : isz == 2 ? snx_ld_le16(g) : snx_ld_le32(g);
                }
            }
            u32 t6[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
            snx_le32_array(t6, 6);
            memcpy(idx + q * 6, t6, 24);
        }
        icount = nq * 6;
        quads_indexed = indexed;
        indexed = 2; /* drawn indexed, with the first vertex in the indices */
    } else if (indexed) {
        if (indexed == 3) {
            u32* o = (u32*)vk_stage((u64)count * 4, vk_stage_align(), &ibuf, &ioff);
            if (!o) {
                draw_images_discard(&images);
                return;
            }
            memcpy(o, ch->inline_indices + first, (size_t)count * 4);
            if (restart && r[0x592] != 0xFFFFFFFFu)
                for (u32 j = 0; j < count; j++)
                    if (o[j] == r[0x592]) o[j] = 0xFFFFFFFFu;
            snx_le32_array(o, count); /* host words, read by the device little-endian */
            itype = VK_INDEX_TYPE_UINT32;
        } else if (ifmt == 0) { /* 8-bit indices need an extension: widen them */
            const u8* src = gmem_ptr(ia + first, count);
            u16* o = (u16*)vk_stage((u64)count * 2, vk_stage_align(), &ibuf, &ioff);
            if (!o) {
                draw_images_discard(&images);
                return;
            }
            for (u32 j = 0; j < count; j++) {
                u8 v = 0;
                if (src)
                    v = src[j];
                else
                    gmem_read(ia + first + j, &v, 1);
                /* the index type's bits, as below, little-endian for the device */
                o[j] = snx_le16(r[0x591] && v == (r[0x592] & 0xFF) ? 0xFFFF : v);
            }
            itype = VK_INDEX_TYPE_UINT16;
        } else {
            u32 ones = isz == 2 ? 0xFFFFu : 0xFFFFFFFFu, ri = r[0x592] & ones;
            u64 bytes = (u64)count * isz;
            if (restart && ri != ones && count <= (1u << 24)) { /* the guest's restart index becomes Vulkan's */
                u8* o = vk_stage(bytes, vk_stage_align(), &ibuf, &ioff);
                if (!o) {
                    draw_images_discard(&images);
                    return;
                }
                gmem_read(ia + (u64)first * isz, o, bytes);
                for (u32 j = 0; j < count; j++)
                    if (isz == 2 && snx_ld_le16(o + (u64)j * 2) == ri)
                        memset(o + (u64)j * 2, 0xFF, 2);
                    else if (isz == 4 && snx_ld_le32(o + (u64)j * 4) == ri)
                        memset(o + (u64)j * 4, 0xFF, 4);
            } else if (!vk_buf_get(ia + (u64)first * isz, bytes, &ibuf, &ioff)) {
                draw_images_discard(&images);
                return;
            }
            itype = isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        }
    }

    /* the transform feedback buffers (0xE0 + 8 * i: enable, address, size, start offset), from the start offset on as
       Eden's UpdateTransformFeedbackBuffer binds them: a writable copy of guest memory each, which lands there and
       which later draws read on the GPU until then (vk_buf_copy_writable) */
    VkBuffer xbuf[4];
    VkDeviceSize xoff[4], xsize[4];
    u32 xbound = 0;
    for (int i = 0; i < 4 && xfb_on; i++) {
        const u32* t = &r[0xE0 + 8 * i];
        i32 size = (i32)t[3];
        u64 a = (((u64)(t[1] & 0xFF) << 32) | t[2]) + t[4];
        if (!t[0] || size <= 0 || (!r[0x1C1 + 4 * i] && !r[0x1C2 + 4 * i])) continue;
        if (!vk_buf_copy_writable(a, (u32)size, &xbuf[i], &xoff[i])) {
            draw_images_discard(&images);
            g_vk_skipped++;
            return;
        }
        xsize[i] = (u32)size;
        xbound |= 1u << i;
    }
    /* the feedback loop goes into the key last: stage_set found it */
    u32 feedback = fb && !g_vk.has_feedback_dynamic;
    if (k == &made_k)
        made_k.feedback = feedback;
    else if (k->feedback != feedback) {
        made_k = *k;
        made_k.feedback = feedback;
        k = &made_k;
        kept = 0;
    }
    if (kept && g_draw.check && memcmp(k, &g_draw.k, sizeof *k)) {
        draw_differs("pipeline key");
        kept = 0;
    }
    /* the kept pipeline, or the last draw's when its key is the same (draws in a row mostly are): one comparison of
       the key, where the table hashed all of it each draw. Pipelines and modules live as long as the run, so the
       handle stays good, a stand-in for one still being made too (vk_pipeline_get_deferred) */
    VkPipeline pipe;
    if (kept) {
        pipe = g_draw.pipe;
        if (g_draw.check) {
            VkPipeline made = vk_pipeline_get_deferred(k);
            if (vk_pipeline_ready(made) != vk_pipeline_ready(pipe)) {
                draw_differs("pipeline");
                kept = 0;
                pipe = made;
            }
        }
    } else if (g_draw.pipe && !memcmp(k, &g_draw.k, sizeof *k))
        pipe = g_draw.pipe;
    else
        pipe = vk_pipeline_get_deferred(k);
    if (!pipe) {
        draw_images_discard(&images);
        g_vk_skipped++;
        return;
    }
    /* A guest read while preparing descriptors can submit and land an older graphics output into memory these
       descriptors and staged inputs were read from. Rebuild them before recording the draw. */
    if (setup_frame != vk_submit_number()) {
        draw_images_discard(&images);
        if (++retries < 4) goto retry;
        g_vk_skipped++;
        return;
    }
    /* kept for the next draw */
    if (kept)
        g_vk_draw_kept++;
    else if (pipe) {
        g_draw.topology = draw_topo;
        g_draw.epoch = epoch;
        g_draw.sh = sh;
        g_draw.fkey = fk;
        g_draw.fs = fs;
        g_draw.tf = tf;
        if (pl != &g_draw.plan) g_draw.plan = *pl;
        if (k != &g_draw.k) g_draw.k = *k;
        g_draw.pipe = pipe;
        vertex_regs_keep(r, vs->si.in_mask);
        g_draw.pend &= ~DRAW_GROUPS;
    }
    /* the color slots the draw writes */
    u32 writes = 0;
    if (fs)
        for (u32 i = 0; i < n; i++)
            if (rt[i] && (k->blend[i].mask & ((fs->si.sph[18] >> (i * 4)) & 15))) writes |= 1u << i;
    /* a buffer that captured since its write pointer was loaded resumes from its counter, which the last capture's end
       wrote: the end is ordered before the begin outside a pass */
    u32 resume = xbound & ch->xfb_started & ~ch->xfb_reload;
    if (resume) {
        vk_render_end();
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT};
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        rec_CmdPipelineBarrier2(vk_cmd(), &di);
    }
    int zt_undefined = zt && zt->layout == VK_IMAGE_LAYOUT_UNDEFINED;
    if (!n && !zt ? !render_begin_empty()
                  : !render_begin(rt, zt, n, zt_ro, r[0x56E] & 1, fb) || !feedback_order(fb, writes)) {
        draw_images_discard(&images);
        return;
    }
    VkCommandBuffer cb = vk_cmd();
    /* only what differs from the previous draw's in this command buffer (g_dyn) */
    int fresh = g_dyn.cb != vk_cb_begun();
    if (fresh) {
        memset(&g_dyn, 0, sizeof g_dyn);
        g_dyn.cb = vk_cb_begun();
    }
    if (fresh || g_dyn.pipe != pipe) {
        g_dyn.pipe = pipe;
        rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    }
    if (g_vk.has_feedback_dynamic)
        DYN(fb, fb ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_NONE,
            rec_CmdSetAttachmentFeedbackLoopEnableEXT(cb, fb ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_NONE));
    VkDescriptorSet sets[2] = {vset, fset};
    VkPipelineLayout layout = vk_playout(k->arrays);
    if (g_dyn.layout != layout) { /* the sets bound through another layout are bound again */
        memset(g_dyn.sets, 0, sizeof g_dyn.sets);
        g_dyn.layout = layout;
    }
    if (fresh || g_dyn.sets[0] != vset || (fs && g_dyn.sets[1] != fset)) {
        g_dyn.sets[0] = vset;
        g_dyn.sets[1] = fs ? fset : VK_NULL_HANDLE;
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, fs ? 2 : 1, sets, 0, NULL);
    }
    for (b = 0; b < 3; b++)
        if (bset[b] && (fresh || g_dyn.sets[2 + b] != bset[b])) {
            g_dyn.sets[2 + b] = bset[b];
            rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, (u32)b + 2, 1, &bset[b], 0, NULL);
        }
    if (k->nbind)
        rec_CmdBindVertexBuffers2(cb, 0, k->nbind, vbuf, voff, NULL, g_vk.has_dynamic_stride ? pl->stride : NULL);

    VkViewport vp = vk_render_viewport(r);
    if (!(vp.width > 0) || !(vp.height != 0)) {
        draw_images_discard(&images);
        return; /* no area: nothing is drawn (and Vulkan takes none) */
    }
    VkViewport vp_unjittered = vp; /* SwitchRecomp, DLSS: the scene draws move by the frame's jitter (dlss.c) */
    vk_dlss_jitter(zt, zt && r[0x4BA], zt && r[0x4B3] && cmp_op(r[0x4C3]) == VK_COMPARE_OP_EQUAL, &vp);
    DYNV(vp, vp, rec_CmdSetViewportWithCount(cb, 1, &vp));
    VkRect2D sc = {{0, 0}, g_rp.ext};
    if (r[0x380] & 1) {
        u32 xs = r[0x381], ys = r[0x382];
        i32 x0 = (i32)vk_scf(xs & 0xFFFF), x1 = (i32)vk_scf(xs >> 16), y0 = (i32)vk_scf(ys & 0xFFFF),
            y1 = (i32)vk_scf(ys >> 16);
        if (r[0x4EB] & 1) {
            i32 clip_height = (i32)vk_scf((float)(r[0x3FE] >> 16));
            i32 old_y0 = y0;
            y0 = clip_height - y1;
            y1 = clip_height - old_y0;
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (i32)g_rp.ext.width) x1 = (i32)g_rp.ext.width;
        if (y1 > (i32)g_rp.ext.height) y1 = (i32)g_rp.ext.height;
        sc = x1 > x0 && y1 > y0 ? (VkRect2D){{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}} : (VkRect2D){{0, 0}, {0, 0}};
    }
    DYNV(sc, sc, rec_CmdSetScissorWithCount(cb, 1, &sc));
    /* Triangle facing has its own window-origin control, independent of viewport scale and swizzle. */
    {
        VkCullModeFlags cull = !r[0x646]           ? VK_CULL_MODE_NONE
                               : r[0x648] == 0x404 ? VK_CULL_MODE_FRONT_BIT
                               : r[0x648] == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK
                                                   : VK_CULL_MODE_BACK_BIT;
        VkFrontFace front = front_face_at_viewport(r, vp.height);
        u32 prim_restart = restart && (indexed == 1 || indexed == 3);
        DYN(cull, cull, rec_CmdSetCullMode(cb, cull));
        DYN(front, front, rec_CmdSetFrontFace(cb, front));
        DYN(topo, topo, rec_CmdSetPrimitiveTopology(cb, topo));
        DYN(restart, prim_restart, rec_CmdSetPrimitiveRestartEnable(cb, prim_restart));
        /* RASTERIZE_ENABLE (0xDF) off discards the primitives once captured: only a capture draws without them here,
           as the register's reset value isn't known to be on (1 off, 2 on: 0 is a new command buffer's) */
        u32 discard = xfb_on && !r[0xDF] ? 2 : 1;
        DYN(discard, discard, rec_CmdSetRasterizerDiscardEnable(cb, discard == 2));
    }
    {
        float lw; /* smooth lines (0x55C) have a width of their own */
        memcpy(&lw, &r[r[0x55C] ? 0x4EC : 0x4ED], 4);
        lw = g_vk.feat.wideLines && lw > 0 && lw < 64 ? lw : 1.0f;
        DYNV(lw, lw, rec_CmdSetLineWidth(cb, lw));
    }
    /* depth bias: enabled per polygon mode (0x370 points, 0x371 lines, 0x372 filled), clamped by 0x61F. The units
       are passed as they are (Ryujinx halves them: a run decides) */
    {
        u32 bias_on = r[k->polygon == VK_POLYGON_MODE_POINT  ? 0x370
                        : k->polygon == VK_POLYGON_MODE_LINE ? 0x371
                                                            : 0x372] != 0;
        DYN(bias_on, bias_on, rec_CmdSetDepthBiasEnable(cb, bias_on));
    }
    {
        float fac, un, clamp;
        memcpy(&fac, &r[0x55B], 4);
        memcpy(&un, &r[0x56F], 4);
        memcpy(&clamp, &r[0x61F], 4);
        float bias[3] = {un, g_vk.feat.depthBiasClamp ? clamp : 0.0f, fac};
        DYNV(bias, bias, rec_CmdSetDepthBias(cb, bias[0], bias[1], bias[2]));
    }
    if (!g_vk.feat.depthClamp && vk_render_depth_clamp(r))
        LOG_LIMIT(1, "vulkan: depth clamp requested but unavailable on this device\n");
    {
        float bc[4];
        memcpy(bc, &r[0x4C7], 16);
        DYNV(bc, bc, rec_CmdSetBlendConstants(cb, bc));
    }
    /* depth and stencil */
    {
        u32 dt = zt && r[0x4B3], dw = zt && r[0x4BA];
        VkCompareOp dcmp = cmp_op(r[0x4C3]);
        DYN(dt, dt, rec_CmdSetDepthTestEnable(cb, dt));
        DYN(dw, dw, rec_CmdSetDepthWriteEnable(cb, dw));
        DYN(dcmp, dcmp, rec_CmdSetDepthCompareOp(cb, dcmp));
        DYN(dbounds, 1, (rec_CmdSetDepthBoundsTestEnable(cb, VK_FALSE), rec_CmdSetDepthBounds(cb, 0.0f, 1.0f)));
    }
    vk_dlss_draw(n ? rt[0] : NULL, zt, zt && r[0x4B3]); /* SwitchRecomp */
    int stencil = zt && zt->f.stencil && r[0x4E0];
    DYN(st, (u32)stencil, rec_CmdSetStencilTestEnable(cb, stencil));
    /* GL order in the registers: fail, depth fail, pass */
    int two = r[0x565] != 0;
    {
        /* per face: fail, depth fail and pass operations, the compare op, reference, compare and write masks */
        u32 sf[7] = {stencil_op(r[0x4E1]),
                     stencil_op(r[0x4E3]),
                     stencil_op(r[0x4E2]),
                     cmp_op(r[0x4E4]),
                     r[0x4E5],
                     r[0x4E6],
                     r[0x4E7]};
        u32 sb[7] = {stencil_op(r[two ? 0x566 : 0x4E1]),
                     stencil_op(r[two ? 0x568 : 0x4E3]),
                     stencil_op(r[two ? 0x567 : 0x4E2]),
                     cmp_op(r[two ? 0x569 : 0x4E4]),
                     r[two ? 0x3D5 : 0x4E5],
                     r[two ? 0x3D7 : 0x4E6],
                     r[two ? 0x3D6 : 0x4E7]};
        DYNV(sfront, sf,
             (rec_CmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, sf[0], sf[1], sf[2], sf[3]),
              rec_CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, sf[4]),
              rec_CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, sf[5]),
              rec_CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, sf[6])));
        DYNV(sback, sb,
             (rec_CmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, sb[0], sb[1], sb[2], sb[3]),
              rec_CmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, sb[4]),
              rec_CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, sb[5]),
              rec_CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, sb[6])));
    }
    /* push constants: the depth mode (snx_vp.y) for the vertex stage, the alpha test (0x4BB on, 0x4C4 reference,
       0x4C5 function) and the point sprite's T flip for the fragment stage. PointCoord's T is 0 at row 0, which is a
       lower-left window's bottom (0x4EB bit 0): it flips when the sprite's origin (0x581 bit 2, 1 the top) is the
       other side */
    float pc[8] = {0, (r[0x35F] & 1) ? 0.0f : 1.0f, 0, 0};
    memcpy(&pc[0], &r[0x50E], sizeof(u32));
    {
        u32 fn = r[0x4C5];
        int f = fn >= 0x200 && fn <= 0x207 ? (int)(fn - 0x200) : fn >= 1 && fn <= 8 ? (int)fn - 1 : 7;
        pc[4] = (r[0x4BB] & 1) ? 1.0f : 0.0f;
        pc[5] = (float)f;
        memcpy(&pc[6], &r[0x4C4], 4);
        pc[7] = ((r[0x581] >> 2) & 1) == (r[0x4EB] & 1) ? 1.0f : 0.0f;
    }
    rec_CmdPushConstants(cb, g_vk_playout, VK_PUSH_STAGES, 0, 32, pc);

    vk_query_draw(cb, ch->id, r[0x574]); /* counted by the channel's samples-passed counter when it runs */
    VkBuffer counters[4] = {g_xfb.buf, g_xfb.buf, g_xfb.buf, g_xfb.buf};
    VkDeviceSize counter_offs[4] = {0, 4, 8, 12};
    /* the buffers from 0 to the last one bound, as begin and end take the counters of: one between that isn't bound
       gets another's memory for 4 bytes, which nothing captures into (its layout has no words) */
    u32 xcount = xbound & 8 ? 4 : xbound & 4 ? 3 : xbound & 2 ? 2 : xbound & 1 ? 1 : 0;
    if (xcount) {
        VkBuffer from[4];
        int first = xbound & 1 ? 0 : xbound & 2 ? 1 : xbound & 4 ? 2 : 3;
        for (u32 i = 0; i < xcount; i++) {
            if (!(xbound >> i & 1)) {
                xbuf[i] = xbuf[first];
                xoff[i] = xoff[first];
                xsize[i] = 4;
            }
            from[i] = resume >> i & 1 ? g_xfb.buf : VK_NULL_HANDLE; /* none: from the bound offset */
        }
        rec_CmdBindTransformFeedbackBuffersEXT(cb, 0, xcount, xbuf, xoff, xsize);
        rec_CmdBeginTransformFeedbackEXT(cb, 0, xcount, from, counter_offs);
    }
    int dlss_xfb; /* SwitchRecomp, DLSS: a scene prepass draw's positions captured for motion vectors (dlss.c) */
    {
        u64 key = vs ? vs->hash : 0;
        /* the draw by guest addresses: the host buffer and offset move between frames in this renderer (binding 0 can be
           the per-frame constants, and buffers are suballocated), which paired draws with other draws of last frame:
           wrong motion vectors, and DLSS held its old picture (2026-10-22) */
        u64 parts[8] = {nvq ? vq[0].gva : 0, nvq ? vq[0].base : 0, ia, first, count, base_vertex, base_instance, instances};
        for (int i = 0; i < 8; i++) key = (key ^ parts[i]) * 0x100000001B3ull;
        dlss_xfb = !xcount && vk_dlss_capture_begin(cb, zt, zt && r[0x4BA] && r[0x4B3],
                                         topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && !gs && !tes && !tcs, key,
                                         (indexed ? icount : count) * (instances ? instances : 1), &vp_unjittered, &vp);
    }
    pred_begin(cb, ch->render_pred);
    if (indexed) {
        rec_CmdBindIndexBuffer(cb, ibuf, ioff, itype);
        /* quads made from guest indices keep the base vertex, quads made from vertex numbers have it in them */
        rec_CmdDrawIndexed(cb, icount, instances, 0, indexed == 2 && !quads_indexed ? 0 : (i32)base_vertex,
                         base_instance);
    } else
        rec_CmdDraw(cb, count, instances, first, base_instance);
    pred_end(cb, ch->render_pred);
    if (dlss_xfb) vk_dlss_capture_end(cb);
    if (xcount) {
        rec_CmdEndTransformFeedbackEXT(cb, 0, xcount, counters, counter_offs);
        ch->xfb_started = (u8)((ch->xfb_started | xbound) & 15);
        ch->xfb_reload = (u8)(ch->xfb_reload & ~xbound);
        vk_render_end(); /* the next capture's begin waits for this end outside a pass, and readers for its writes */
    }
    vk_buf_writable_commit();
    draw_images_commit(&images);
    feedback_done(fb, writes);
    if (fb) g_vk_fb_draws++;
    for (u32 i = 0; i < n; i++)
        if (writes >> i & 1) target_written(g_rp.rt[i]);
    if (zt && !zt_ro && (r[0x4BA] || (r[0x4E0] && ((r[0x4E7] & 0xFF) || (r[0x565] && (r[0x3D6] & 0xFF))))) &&
        zt_written(zt, ch->render_pred, zt_undefined))
        target_written(zt);
    g_vk_drawn++;
}

/* DRAW_TEXTURE (3D 0x420-0x42B, drawn when 0x42B is written): a texture's rectangle blitted onto color target 0, as
   Eden's DrawManager::DrawTexture reads the registers: the destination in 20.12 fixed point, the source's start the
   same and its steps per destination pixel in 32.32, and y from the bottom in a lower-left window (0x4EB bit 0, the
   height from the surface clip, 0x3FE). The sampler's magnification filter filters it */
static int format_has(VkFormat f, VkFormatFeatureFlags need) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & need) == need;
}
void vk_render_draw_texture(GpuChannel* ch) {
    u32* r = ch->r3d;
    float x0 = (float)(i32)r[0x420] / 4096.f, y0 = (float)(i32)r[0x421] / 4096.f;
    float w = (float)(i32)r[0x422] / 4096.f, h = (float)(i32)r[0x423] / 4096.f;
    double du = (double)(i64)((u64)r[0x425] << 32 | r[0x424]) / 4294967296.0;
    double dv = (double)(i64)((u64)r[0x427] << 32 | r[0x426]) / 4294967296.0;
    float sx0 = (float)(i32)r[0x42A] / 4096.f, sy0 = (float)(i32)r[0x42B] / 4096.f;
    if (r[0x4EB] & 1) y0 = (float)(r[0x3FE] >> 16) - y0;
    float sx1 = sx0 + (float)(du * w), sy1 = sy0 + (float)(dv * h);
    TexRes x;
    tex_resolve(ch, (r[0x428] & 0xFFF) << 20 | (r[0x429] & 0xFFFFF), &x);
    Surf *rt[8], *zt, *t = x.t;
    u32 n;
    read_targets(ch, rt, &zt, &n);
    if (!t || !n || !rt[0]) {
        LOG_LIMIT(1, "vulkan: a draw texture without its texture or a color target (not drawn)\n");
        return;
    }
    Surf* d = rt[0];
    if (t->f.depth || d->f.depth || t->f.is_int != d->f.is_int ||
        !format_has(t->f.vk, VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !format_has(d->f.vk, VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        LOG_LIMIT(1, "vulkan: a draw texture between formats %d and %d can't be blitted (not drawn)\n", (int)t->f.vk,
                  (int)d->f.vk);
        return;
    }
    /* in the images' pixels: a target is scaled, and a texture made from one is too */
    float tsx = (float)t->sw / (float)t->w, tsy = (float)t->sh / (float)t->h;
    VkImageBlit b = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{(i32)(sx0 * tsx), (i32)(sy0 * tsy), 0}, {(i32)(sx1 * tsx), (i32)(sy1 * tsy), 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{(i32)vk_scf(x0), (i32)vk_scf(y0), 0}, {(i32)vk_scf(x0 + w), (i32)vk_scf(y0 + h), 1}}};
    int linear =
        (x.tsc[1] & 3) == 2 && !t->f.is_int && format_has(t->f.vk, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    vk_render_end(); /* blits happen outside rendering passes */
    vk_rt_note_read(t);
    if (t->layout == VK_IMAGE_LAYOUT_UNDEFINED) return;
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    rec_CmdBlitImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    vk_surf_wrote(d);
    vk_rt_drawn(d);
    vk_surf_touch(d);
}
