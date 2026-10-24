/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 SwitchRecomp (this file is part of the GPL-3.0 ScarletNX fork) */
/* HDR output (ini hdr=1, Windows HDR on): the swapchain is FP16 scRGB (else HDR10 PQ). The frame is letterboxed into
   an SDR image as before, then one fullscreen pass (shaders/hdr_out.frag) writes it to the swapchain: normal content
   at paper white (ini hdr_paper_white), the brightest tones expanded smoothly toward the peak (hdr_peak, default the
   display's own). The game's tone mapping stays as it is: this is an SDR-to-HDR presentation, like Windows Auto HDR */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/hdr_spv.h"
#include "nx/runtime/platform/log.h"

int g_vk_hdr_ext;   /* VK_EXT_swapchain_colorspace enabled on the instance (device.c) */
int g_vk_sc_hdr;    /* the swapchain: 0 SDR, 1 scRGB FP16, 2 HDR10 PQ (device.c) */
extern int g_opt_hdr, g_opt_hdr_paper, g_opt_hdr_peak, g_opt_aa, g_opt_sharp;   /* the runtime's .ini (g_opt_aa: FXAA, gpu_gl.c) */
extern int backend_hdr_peak_nits(void);                  /* the display's peak, 0 if unknown */

int vk_hdr_wanted(void) { return g_opt_hdr && g_vk_hdr_ext; }
extern int g_opt_show_fps;                               /* FPS counter (main.c fps_overlay_rects) */
extern int fps_overlay_rects(int W, int H, int (*r)[5], int max);
extern volatile int g_skip_prompt;   /* SWR: "Hold (-) to skip" (main.c skip_hold_filter) */
extern volatile float g_skip_hold;
extern u64 plat_ns(void);
extern int skip_prompt_rgba(u8** px, int* w, int* h, int* tw);   /* gpu_gl.c: the prompt in the game's font */
static float g_ovl_alpha;
/* the present pass runs for HDR output, anti-aliasing, the FPS counter or the skip prompt (renderer.c show) */
int vk_present_pass_wanted(void) { return g_vk_sc_hdr || g_opt_aa || g_opt_sharp > 0 || g_opt_show_fps || g_skip_prompt || g_ovl_alpha > 0; }

static VkImage g_mid;
static VmaAllocation g_mid_mem;
static VkImageView g_mid_view;
static u32 g_mid_w, g_mid_h;
static VkFormat g_mid_fmt;
static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_layout;
static VkDescriptorPool g_pool;
static VkDescriptorSet g_set;
static VkSampler g_samp;
static VkPipeline g_pipe;
static VkFormat g_pipe_fmt;
static VkImage g_ovl; /* SWR: the skip prompt image (1x1 clear until the font is ready) */
static VmaAllocation g_ovl_mem;
static VkImageView g_ovl_view;
static int g_ovl_w, g_ovl_h, g_ovl_tw, g_ovl_state; /* state: 0 none, 1 created, 2 uploaded */
static u8* g_ovl_px;
static VkImage g_sc_img[VK_SC_MAX];
static VkImageView g_sc_view[VK_SC_MAX];

static int layout_make(void) {
    if (g_layout) return 1;
    VkDescriptorSetLayoutBinding b[2] = {{.binding = 0,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                          .descriptorCount = 1,
                                          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                         {.binding = 1, /* SWR: skip prompt */
                                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                          .descriptorCount = 1,
                                          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 68};
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &g_dsl,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &pc};
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .magFilter = VK_FILTER_LINEAR, /* FXAA samples between texels */
                               .minFilter = VK_FILTER_LINEAR,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dci, NULL, &g_dsl))) return 0;
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &lci, NULL, &g_layout))) return 0;
    if (!VK_OK(vkCreateDescriptorPool(g_vk.dev, &pci, NULL, &g_pool))) return 0;
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = g_pool,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &g_dsl};
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, &g_set))) return 0;
    return VK_OK(vkCreateSampler(g_vk.dev, &sci, NULL, &g_samp));
}

static int pipeline_make(VkFormat format) {
    if (g_pipe && g_pipe_fmt == format) return 1;
    if (g_pipe) vkDestroyPipeline(g_vk.dev, g_pipe, NULL); /* the swapchain changed format: vk_finish ran */
    g_pipe = VK_NULL_HANDLE;
    VkShaderModule vs = vk_module_spv(g_hdr_out_vert, sizeof g_hdr_out_vert / 4),
                   fs = vk_module_spv(g_hdr_out_frag, sizeof g_hdr_out_frag / 4);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL,
                                                 .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo gci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        .pNext = &ri,
                                        .stageCount = 2,
                                        .pStages = st,
                                        .pVertexInputState = &vi,
                                        .pInputAssemblyState = &ia,
                                        .pViewportState = &vp,
                                        .pRasterizationState = &rs,
                                        .pMultisampleState = &ms,
                                        .pColorBlendState = &cb,
                                        .pDynamicState = &ds,
                                        .layout = g_layout};
    int ok = vs && fs && VK_OK(vkCreateGraphicsPipelines(g_vk.dev, vk_pipeline_cache(), 1, &gci, NULL, &g_pipe));
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    g_pipe_fmt = format;
    if (!ok) g_pipe = VK_NULL_HANDLE;
    return ok;
}

static VkImageView sc_view(VkImage sc) {
    for (u32 i = 0; i < VK_SC_MAX; ++i)
        if (g_sc_img[i] == sc) return g_sc_view[i];
    for (u32 i = 0; i < VK_SC_MAX; ++i)
        if (!g_sc_img[i]) {
            VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = sc,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = g_vk.sc_format,
                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            if (!VK_OK(vkCreateImageView(g_vk.dev, &vci, NULL, &g_sc_view[i]))) return VK_NULL_HANDLE;
            g_sc_img[i] = sc;
            return g_sc_view[i];
        }
    return VK_NULL_HANDLE;
}
void vk_hdr_swapchain_gone(void) {
    for (u32 i = 0; i < VK_SC_MAX; ++i) {
        if (g_sc_view[i]) vkDestroyImageView(g_vk.dev, g_sc_view[i], NULL);
        g_sc_view[i] = VK_NULL_HANDLE;
        g_sc_img[i] = VK_NULL_HANDLE;
    }
}

/* the SDR image the frame is letterboxed into while the swapchain is HDR: the swapchain's size, sRGB when the frame
   is (a blit between sRGB formats keeps the bytes, as the SDR path's blit to an sRGB swapchain does). NULL = can't */
VkImage vk_hdr_mid(int srgb) {
    VkFormat f = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    u32 w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    if (g_mid && g_mid_w == w && g_mid_h == h && g_mid_fmt == f) return g_mid;
    if (!layout_make()) return VK_NULL_HANDLE;
    if (g_mid) {
        /* SWR: the present pass is queued on the recording thread now: the set it binds is rewritten below, so let
           every queued and submitted frame finish first (a size or format change only) */
        if (vk_rec_threaded()) vk_rec_sync();
        vk_finish();
        vk_defer_image(g_mid, g_mid_mem, g_mid_view, VK_NULL_HANDLE);
    }
    g_mid = VK_NULL_HANDLE;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = f,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, /* read through a UNORM view: the bytes */
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &ai, &g_mid, &g_mid_mem, NULL))) {
        g_mid = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .image = g_mid,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, /* display-encoded bytes, sRGB image or not */
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vci, NULL, &g_mid_view))) {
        vmaDestroyImage(g_vk.vma, g_mid, g_mid_mem);
        g_mid = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    if (!g_ovl) { /* SWR: the skip prompt, composed once on the CPU; uploaded by vk_hdr_resolve before its first use */
        if (!skip_prompt_rgba(&g_ovl_px, &g_ovl_w, &g_ovl_h, &g_ovl_tw)) {
            g_ovl_w = g_ovl_h = 1; g_ovl_tw = 0; g_ovl_px = calloc(1, 4);
        }
        VkImageCreateInfo oci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {(u32)g_ovl_w, (u32)g_ovl_h, 1},
                                 .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo oai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (g_ovl_px && VK_OK(vmaCreateImage(g_vk.vma, &oci, &oai, &g_ovl, &g_ovl_mem, NULL))) {
            VkImageViewCreateInfo ovci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_ovl,
                                          .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            if (VK_OK(vkCreateImageView(g_vk.dev, &ovci, NULL, &g_ovl_view))) g_ovl_state = 1;
        }
    }
    VkDescriptorImageInfo di[2] = {{g_samp, g_mid_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {g_samp, g_ovl_view ? g_ovl_view : g_mid_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet wr[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = g_set,
                                   .dstBinding = 0,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &di[0]},
                                  {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = g_set,
                                   .dstBinding = 1,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &di[1]}};
    vkUpdateDescriptorSets(g_vk.dev, 2, wr, 0, NULL);
    g_mid_w = w;
    g_mid_h = h;
    g_mid_fmt = f;
    if (g_vk_sc_hdr)
        LOG("vulkan: HDR output %s, paper white %d nits, peak %d nits%s\n", g_vk_sc_hdr == 1 ? "scRGB" : "HDR10 (PQ)",
            g_opt_hdr_paper, g_opt_hdr_peak > 0 ? g_opt_hdr_peak : backend_hdr_peak_nits(), g_opt_aa ? ", FXAA" : "");
    else
        LOG("vulkan: present pass:%s%s%s\n", g_opt_aa ? " FXAA" : "", g_opt_sharp > 0 ? " sharpening" : "",
            g_opt_aa || g_opt_sharp > 0 ? "" : " overlays only");
    return g_mid;
}

/* the SDR image (left in TRANSFER_DST, drawn) onto the swapchain image, which ends in PRESENT_SRC */
void vk_hdr_resolve(VkCommandBuffer cb, VkImage sc) {
    /* SWR: these go through the recording thread's ring (rec_Cmd*, record.c) like the rest of the frame, in order
       after it. They used to be made directly after draining the ring (vk_rec_sync) every frame: with FXAA that wait
       sometimes lasted 0.8-2.6 s (MEASURED 2026-10-22, stall-fxaa-1/2/5/7: all of it in this pass) */
    VkImageView view = sc_view(sc);
    if (!view || !pipeline_make(g_vk.sc_format)) return;
    vk_barrier(cb, g_mid, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    int peak = g_opt_hdr_peak > 0 ? g_opt_hdr_peak : backend_hdr_peak_nits();
    if (peak <= 0) peak = 400;
    int paper = g_opt_hdr_paper > 0 ? g_opt_hdr_paper : 200;
    struct {
        float paper, peak, mode, aa, out_srgb, knee;
        float ox, oy, ow, oh, oa, hold, bx, by, bw, bh;
        float sharp; /* SWR: 0 off, else 0..1 (ini sharpening / 100) */
    } pc = {(float)paper, (float)peak, (float)g_vk_sc_hdr, g_opt_aa ? 1.0f : 0.0f, g_vk.sc_srgb ? 1.0f : 0.0f, 0.75f};
    pc.sharp = g_opt_sharp > 0 ? (float)g_opt_sharp / 100.0f : 0.0f;
    if (g_ovl_state == 1) { /* SWR: first use: the prompt image to the GPU, in this frame's commands */
        VkBuffer sb; u64 so; u64 n = (u64)g_ovl_w * g_ovl_h * 4;
        u8* dst = vk_stage(n, 16, &sb, &so);
        if (dst) {
            memcpy(dst, g_ovl_px, n);
            vk_barrier(cb, g_ovl, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy cp = {.bufferOffset = so, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {(u32)g_ovl_w, (u32)g_ovl_h, 1}};
            rec_CmdCopyBufferToImage(cb, sb, g_ovl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            vk_barrier(cb, g_ovl, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            g_ovl_state = 2;
        }
    }
    { /* SWR: the prompt fades in and out like the OpenGL one (gpu_gl.c gpu_gl_skip_prompt) */
        static u64 last; u64 now = plat_ns(); float dt = last ? (float)(now - last) / 1e9f : 0; last = now;
        if (dt > 0.1f) dt = 0.1f;
        float want = g_skip_prompt ? 1.0f : 0.0f, step = dt * 8.0f > 1 ? 1 : dt * 8.0f;
        g_ovl_alpha += (want - g_ovl_alpha) * step; if (g_ovl_alpha < 0.01f && !g_skip_prompt) g_ovl_alpha = 0;
        if (g_ovl_state == 2 && g_ovl_tw > 0 && g_ovl_alpha > 0) {
            float k = (float)g_vk.sc_extent.height / 1080.0f; if (k < 0.5f) k = 0.5f;
            float x = 48 * k, y = (g_opt_show_fps ? 96 : 44) * k;
            pc.ox = x - 2 * k; pc.oy = y - 4 * k; pc.ow = g_ovl_w * k; pc.oh = g_ovl_h * k; pc.oa = g_ovl_alpha;
            pc.hold = g_skip_hold; pc.bx = x; pc.by = y + 42 * k; pc.bw = g_ovl_tw * k; pc.bh = 4 * k;
        }
    }
    VkRenderingAttachmentInfo at = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, g_vk.sc_extent},
                          .layerCount = 1,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = &at};
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sci = {{0, 0}, g_vk.sc_extent};
    rec_CmdBeginRendering(cb, &ri);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sci);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &g_set, 0, NULL);
    rec_CmdPushConstants(cb, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    { /* SWR: the FPS counter as cleared rectangles over the picture, in the swapchain's encoding */
        int r[64][5];
        int n = fps_overlay_rects((int)g_vk.sc_extent.width, (int)g_vk.sc_extent.height, r, 64);
        for (int i = 0; i < n; i++) {
            VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {{{0, 0, 0, 1}}}};
            if (r[i][4]) {
                float y[3] = {1.0f, 0.92f, 0.2f};
                if (g_vk_sc_hdr == 1) { y[0] = 2.5f; y[1] = 2.2f; y[2] = 0.3f; }        /* scRGB: 1.0 = 80 nits */
                else if (g_vk_sc_hdr == 2) { y[0] = 0.58f; y[1] = 0.56f; y[2] = 0.35f; } /* PQ code values, ~200 nits */
                else if (g_vk.sc_srgb) { y[1] = 0.83f; y[2] = 0.033f; }                  /* linear for an sRGB view */
                ca.clearValue.color.float32[0] = y[0]; ca.clearValue.color.float32[1] = y[1]; ca.clearValue.color.float32[2] = y[2];
            }
            int x0 = r[i][0] < 0 ? 0 : r[i][0], y0 = r[i][1] < 0 ? 0 : r[i][1];
            int x1 = r[i][0] + r[i][2], y1 = r[i][1] + r[i][3];
            if (x1 > (int)g_vk.sc_extent.width) x1 = (int)g_vk.sc_extent.width;
            if (y1 > (int)g_vk.sc_extent.height) y1 = (int)g_vk.sc_extent.height;
            if (x1 <= x0 || y1 <= y0) continue;
            VkClearRect cr = {{{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}}, 0, 1};
            rec_CmdClearAttachments(cb, 1, &ca, 1, &cr);
        }
    }
    rec_CmdEndRendering(cb);
    vk_render_forget_state(); /* its pipeline and state, not a draw's: the next draw sets all again */
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

/* ------------------------------------------------------------ SWR: the frame shrunk to its on-screen size
   A frame larger than its place in the window (resolution 2 and up) went there through one linear blit, which reads
   2x2 texels per window pixel: at 2x and more most of the frame was skipped rather than averaged, and edges aliased
   where the OpenGL renderer's were smooth (GitHub issue #17). vk_shrink draws the frame's rectangle into an image of
   exactly the on-screen size with shaders/shrink.frag (4x4 filtered taps per pixel, as gpu_gl.c's window pass does);
   renderer.c show then copies that 1:1, flip included. SWR_NO_SHRINK=1 = the single blit (tests) */
#include "nx/gpu/vk/shrink_spv.h"
static VkDescriptorSetLayout g_shr_dsl;
static VkPipelineLayout g_shr_layout;
static VkPipeline g_shr_pipe;
static VkFormat g_shr_pipe_fmt;
static VkImage g_shr_img;
static VmaAllocation g_shr_mem;
static VkImageView g_shr_view;
static u32 g_shr_w, g_shr_h;
static VkFormat g_shr_fmt;

/* whether a frame of sw x sh going to a dw x dh place in the window is shrunk (more than 5% either way) */
int vk_shrink_wanted(u32 sw, u32 sh, u32 dw, u32 dh) {
    static int off = -1;
    if (off < 0) off = getenv("SWR_NO_SHRINK") != NULL;
    return !off && dw && dh && (sw * 20 > dw * 21 || sh * 20 > dh * 21);
}
static int shrink_pipeline(VkFormat format) {
    if (g_shr_pipe && g_shr_pipe_fmt == format) return 1;
    if (!g_shr_layout) {
        VkDescriptorSetLayoutBinding b = {.binding = 0,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                          .descriptorCount = 1,
                                          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
        VkDescriptorSetLayoutCreateInfo dci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount = 1,
                                          .pSetLayouts = &g_shr_dsl,
                                          .pushConstantRangeCount = 1,
                                          .pPushConstantRanges = &pc};
        if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dci, NULL, &g_shr_dsl))) return 0;
        if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &lci, NULL, &g_shr_layout))) return 0;
    }
    if (g_shr_pipe) { /* another format: frames in flight may still use the old pipeline */
        if (vk_rec_threaded()) vk_rec_sync();
        vk_finish();
        vkDestroyPipeline(g_vk.dev, g_shr_pipe, NULL);
        g_shr_pipe = VK_NULL_HANDLE;
    }
    VkShaderModule vs = vk_module_spv(g_hdr_out_vert, sizeof g_hdr_out_vert / 4),
                   fs = vk_module_spv(g_shrink_frag, sizeof g_shrink_frag / 4);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL,
                                                 .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo gci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        .pNext = &ri,
                                        .stageCount = 2,
                                        .pStages = st,
                                        .pVertexInputState = &vi,
                                        .pInputAssemblyState = &ia,
                                        .pViewportState = &vp,
                                        .pRasterizationState = &rs,
                                        .pMultisampleState = &ms,
                                        .pColorBlendState = &cb,
                                        .pDynamicState = &ds,
                                        .layout = g_shr_layout};
    int ok = vs && fs && VK_OK(vkCreateGraphicsPipelines(g_vk.dev, vk_pipeline_cache(), 1, &gci, NULL, &g_shr_pipe));
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    g_shr_pipe_fmt = format;
    if (!ok) g_shr_pipe = VK_NULL_HANDLE;
    return ok;
}
/* the image of the on-screen size (format f), made or remade */
static int shrink_image(u32 w, u32 h, VkFormat f) {
    if (g_shr_img && g_shr_w == w && g_shr_h == h && g_shr_fmt == f) return 1;
    if (g_shr_img) vk_defer_image(g_shr_img, g_shr_mem, g_shr_view, VK_NULL_HANDLE); /* frames in flight may read it */
    g_shr_img = VK_NULL_HANDLE;
    g_shr_view = VK_NULL_HANDLE;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = f,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &ai, &g_shr_img, &g_shr_mem, NULL))) {
        g_shr_img = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .image = g_shr_img,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = f,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vci, NULL, &g_shr_view))) {
        vmaDestroyImage(g_vk.vma, g_shr_img, g_shr_mem);
        g_shr_img = VK_NULL_HANDLE;
        return 0;
    }
    g_shr_w = w;
    g_shr_h = h;
    g_shr_fmt = f;
    LOG("vulkan: the frame is shrunk to %ux%u on screen (averaged, not one blit)\n", w, h);
    return 1;
}
/* src's rectangle (sx, sy, sw, sh) drawn averaged into a dw x dh image, left in TRANSFER_SRC_OPTIMAL for the caller's
   1:1 blit: that image, or VK_NULL_HANDLE (then the caller blits as before) */
VkImage vk_shrink(VkCommandBuffer cb, Surf* src, u32 sx, u32 sy, u32 sw, u32 sh, u32 dw, u32 dh) {
    if (!src || !src->att || src->f.depth || src->f.is_int || !layout_make() || !shrink_pipeline(src->f.vk) ||
        !shrink_image(dw, dh, src->f.vk))
        return VK_NULL_HANDLE;
    vk_surf_use(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkDescriptorImageInfo di = {g_samp, src->att, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &di};
    VkDescriptorSet set = vk_rec_new_set(g_shr_dsl, 1, &wr);
    if (!set) return VK_NULL_HANDLE;
    vk_barrier(cb, g_shr_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    struct {
        float ox, oy, kx, ky;
    } pc = {(float)sx, (float)sy, (float)sw / (float)dw, (float)sh / (float)dh};
    VkExtent2D ext = {dw, dh};
    VkRenderingAttachmentInfo at = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = g_shr_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, ext},
                          .layerCount = 1,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = &at};
    VkViewport vp = {0, 0, (float)dw, (float)dh, 0, 1};
    VkRect2D sci = {{0, 0}, ext};
    rec_CmdBeginRendering(cb, &ri);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_shr_pipe);
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sci);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_shr_layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_shr_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_render_forget_state(); /* its pipeline and state, not a draw's: the next draw sets all again */
    vk_barrier(cb, g_shr_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return g_shr_img;
}
