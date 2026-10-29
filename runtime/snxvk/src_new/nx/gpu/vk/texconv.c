/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Texture formats the host can't take as they are, converted on the GPU (gpu/texture/texconv.comp) from linear rows
   into rows the copies into the image take. One pipeline for every format, made on first use and kept for the
   device's lifetime, like the other internal pipelines. Uses are on g_vk.gfx, under vk_lock() */
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/texture/texconv_spv.h" /* compiled from texconv.comp by the build (cmake/glsl.cmake) */
#include "nx/gpu/texture/texconv_modes.glsl"
#include "nx/runtime/platform/log.h"

/* texconv.comp's push constants, member for member */
typedef struct {
    u32 mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    u32 remap, const_a, const_b, line_out;
} TcParams;

static VkDescriptorSetLayout g_tc_ds;
static VkPipelineLayout g_tc_layout;
static VkPipeline g_tc_pipe;

static void tc_init(void) {
    if (g_tc_pipe) return;
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_tc_ds)))
        fatal("vulkan: cannot create the texture conversion descriptor layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TcParams)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_tc_ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_tc_layout)))
        fatal("vulkan: cannot create the texture conversion pipeline layout");
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = texconv_spv_size, .pCode = texconv_spv};
    VkShaderModule module;
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &module)))
        fatal("vulkan: cannot load the texture conversion shader");
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = module,
                                                .pName = "main"},
                                      .layout = g_tc_layout};
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &g_tc_pipe)))
        fatal("vulkan: cannot create the texture conversion compute pipeline");
    vkDestroyShaderModule(g_vk.dev, module, NULL);
}

/* the shader's mode for a format's conversion */
static u32 tc_mode(const VkFmt* f) {
    switch (f->conv) {
        case VKC_RGBA4: return TC_RGBA4;
        case VKC_RGB5A1: return TC_RGB5A1;
        case VKC_A5B5G5R1: return TC_A5B5G5R1;
        case VKC_RGB32F: return TC_RGB32F;
        case VKC_D24S8: return TC_D24S8;
        case VKC_S8D24: return TC_S8D24;
        case VKC_D24X8: return TC_D24X8;
        case VKC_D32S8: return TC_D32S8;
        case VKC_NONE: return f->bpp == 8 ? TC_D32S8 : TC_D24S8; /* a depth and stencil format as it is in memory */
    }
    /* the ETC2 and EAC kinds, and the BCn ones, each in the same order */
    return f->conv >= VKC_BC1 ? TC_BC1 + (f->conv - VKC_BC1) : TC_ETC2_RGB + (f->conv - VKC_ETC2);
}

int vk_tc_converts(const VkFmt* f) {
    return f->conv == VKC_NONE ? f->depth && f->stencil : f->conv != VKC_ASTC;
}

static VkTcShape tc_shape(u32 mode, u32 w) {
    VkTcShape s = {.out_bpp = 4, .out_rows = 1, .in_align = 16};
    if (mode <= TC_A5B5G5R1 || (mode >= TC_VI_RGB565 && mode <= TC_VI_RGBA4444)) {
        s.units = (w + 1) / 2;
        s.out_texels = s.units * 2;
    } else if (mode >= TC_BC1 && mode <= TC_BC7) {
        s.units = (w + 3) / 4;
        s.out_texels = s.units * 4;
        s.out_rows = 4;
        if (mode == TC_BC4 || mode == TC_BC4_SNORM) s.out_bpp = 2;
        if (mode == TC_BC6H_UF || mode == TC_BC6H_SF) s.out_bpp = 8;
    } else if (mode >= TC_VI_RGBX8888) {
        s.units = s.out_texels = w;
    } else if (mode == TC_RGB32F) {
        s.units = s.out_texels = w;
        s.out_bpp = 16;
        s.in_align = 48;
    } else if (mode <= TC_D32S8) {
        s.units = (w + 3) / 4;
        s.out_texels = s.units * 4;
        s.stencil = 1;
        if (mode == TC_D32S8) s.in_align = 32;
    } else {
        s.units = (w + 3) / 4;
        s.out_texels = s.units * 4;
        s.out_rows = 4;
        if (mode == TC_EAC_R || mode == TC_EAC_R_SNORM) s.out_bpp = 2;
    }
    return s;
}
VkTcShape vk_tc_shape(const VkFmt* f, u32 w) { return tc_shape(tc_mode(f), w); }

/* vi's pixel formats (present.c's bytes_per_pixel): 2 RGBX8888, 4 RGB565, 5 BGRA8888, 6 RGBA5551, 7 RGBA4444 */
static u32 tc_vi_mode(u32 fmt) {
    switch (fmt) {
        case 2: return TC_VI_RGBX8888;
        case 4: return TC_VI_RGB565;
        case 5: return TC_VI_BGRA8888;
        case 6: return TC_VI_RGBA5551;
        case 7: return TC_VI_RGBA4444;
    }
    return 0;
}
int vk_tc_vi_converts(u32 fmt) { return tc_vi_mode(fmt) != 0; }
VkTcShape vk_tc_vi_shape(u32 fmt, u32 w) { return tc_shape(tc_vi_mode(fmt), w); }

u64 vk_tc_out_size(const VkTcShape* s, u32 rows, u32 slices) {
    u64 texels = (u64)s->out_texels * s->out_rows * rows * slices;
    return texels * s->out_bpp + (s->stencil ? texels : 0);
}

/* the dispatch of gx x rows x slices workgroups with p, over the input range [in_off, in_off + in_size) of in and the
   output's of out (offsets the device's alignment for storage, which p's biases count from) */
static int tc_dispatch(VkCommandBuffer cb, const TcParams* p, u32 gx, u32 rows, u32 slices, VkBuffer in, u64 in_off,
                       u64 in_size, VkBuffer out, u64 out_off, u64 out_size) {
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    if (in_size > lim->maxStorageBufferRange || out_size > lim->maxStorageBufferRange ||
        gx > lim->maxComputeWorkGroupCount[0] || rows > lim->maxComputeWorkGroupCount[1] ||
        slices > lim->maxComputeWorkGroupCount[2]) {
        LOG_LIMIT(1, "vulkan: a %ux%u conversion exceeds the device's limits (left out)\n", p->units, rows);
        return 0;
    }
    tc_init();
    VkDescriptorSet set = vk_desc_alloc(g_tc_ds);
    if (!set) return 0;
    VkDescriptorBufferInfo sides[2] = {{in, in_off, in_size}, {out, out_off, out_size}};
    VkWriteDescriptorSet writes[2];
    for (u32 i = 0; i < 2; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &sides[i]};
    vkUpdateDescriptorSets(g_vk.dev, 2, writes, 0, NULL);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_tc_pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_tc_layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_tc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *p, p);
    rec_CmdDispatch(cb, gx, rows, slices);
    return 1;
}
static u64 storage_align(void) {
    u64 a = g_vk.props.limits.minStorageBufferOffsetAlignment;
    return a < 4 ? 4 : a;
}
static int tc_record(VkCommandBuffer cb, u32 mode, u32 zfloat, u32 w, u32 rows, u32 slices, VkBuffer in, u64 in_at,
                     u32 in_pitch, VkBuffer out, u64 out_at) {
    VkTcShape s = tc_shape(mode, w);
    u64 align = storage_align();
    u64 in_off = in_at & ~(align - 1), out_off = out_at & ~(align - 1);
    u64 depth_bytes = (u64)s.out_texels * s.out_rows * rows * slices * s.out_bpp;
    if ((in_at | out_at | in_pitch) & 3) {
        LOG_LIMIT(1, "vulkan: a %ux%u texture to convert isn't word aligned (left out)\n", w, rows);
        return 0;
    }
    TcParams p = {.mode = mode,
                  .units = s.units,
                  .rows = rows,
                  .in_pitch = in_pitch / 4,
                  .out_pitch = s.out_texels * s.out_bpp / 4,
                  .st_pitch = s.out_texels / 4,
                  .in_bias = (u32)((in_at - in_off) / 4),
                  .out_bias = (u32)((out_at - out_off) / 4),
                  .st_bias = (u32)((out_at - out_off + depth_bytes) / 4),
                  .zfloat = zfloat};
    return tc_dispatch(cb, &p, (s.units + 63) / 64, rows, slices, in, in_off,
                       in_at + (u64)in_pitch * rows * slices - in_off, out, out_off,
                       out_at + vk_tc_out_size(&s, rows, slices) - out_off);
}
int vk_tc_nv12_uv(VkCommandBuffer cb, u32 pairs, u32 rows, VkBuffer in, u64 u_at, u64 v_at, u32 in_pitch,
                  VkBuffer out, u64 out_at, u32 out_pitch) {
    u64 align = storage_align(), lo = u_at < v_at ? u_at : v_at, hi = (u_at > v_at ? u_at : v_at);
    u64 in_off = lo & ~(align - 1), out_off = out_at & ~(align - 1);
    u32 units = (pairs + 3) / 4;
    if ((u_at | v_at | in_pitch | out_at | out_pitch) & 3) return 0;
    TcParams p = {.mode = TC_NV12_UV,
                  .units = units,
                  .rows = rows,
                  .in_pitch = in_pitch / 4,
                  .out_pitch = out_pitch / 4,
                  .st_pitch = in_pitch / 4,
                  .in_bias = (u32)((u_at - in_off) / 4),
                  .out_bias = (u32)((out_at - out_off) / 4),
                  .st_bias = (u32)((v_at - in_off) / 4)};
    return tc_dispatch(cb, &p, (units + 63) / 64, rows, 1, in, in_off, hi + (u64)in_pitch * rows - in_off, out,
                       out_off, out_at + (u64)out_pitch * rows - out_off);
}
int vk_tc_convert(VkCommandBuffer cb, const VkFmt* f, u32 w, u32 rows, u32 slices, VkBuffer in, u64 in_at,
                  u32 in_pitch, VkBuffer out, u64 out_at) {
    u32 mode = tc_mode(f);
    /* 24 bit depth into D32_SFLOAT_S8_UINT, where D24S8 isn't supported, goes out as a float */
    u32 zfloat = f->vk == VK_FORMAT_D32_SFLOAT_S8_UINT && mode != TC_D32S8;
    return tc_record(cb, mode, zfloat, w, rows, slices, in, in_at, in_pitch, out, out_at);
}
int vk_tc_convert_vi(VkCommandBuffer cb, u32 fmt, u32 w, u32 rows, VkBuffer in, u64 in_at, u32 in_pitch, VkBuffer out,
                     u64 out_at) {
    return tc_record(cb, tc_vi_mode(fmt), 0, w, rows, 1, in, in_at, in_pitch, out, out_at);
}
int vk_tc_dma_remap(VkCommandBuffer cb, const GpuDmaOp* o, u32 lines, VkBuffer in, u64 in_at, u32 in_pitch,
                    VkBuffer out, u64 out_at, u32 out_pitch) {
    u64 align = storage_align(), in_off = in_at & ~(align - 1), out_off = out_at & ~(align - 1);
    u32 units = (o->line_out + 3) / 4, k, remap = o->cs | o->ns << 4 | o->nd << 8;
    if ((out_at | out_pitch) & 3) return 0;
    for (k = 0; k < 4; k++) remap |= (o->sel[k] & 7) << (12 + 4 * k);
    TcParams p = {.mode = TC_DMA_REMAP,
                  .units = units,
                  .rows = lines,
                  .in_pitch = in_pitch, /* bytes, for this mode */
                  .out_pitch = out_pitch / 4,
                  .in_bias = (u32)(in_at - in_off),
                  .out_bias = (u32)((out_at - out_off) / 4),
                  .remap = remap,
                  .const_a = o->const_a,
                  .const_b = o->const_b,
                  .line_out = o->line_out};
    /* the input range covers every line's bytes (a fill reads none: a word of it still binds) */
    u64 in_size = in_at + (lines ? (u64)in_pitch * (lines - 1) + o->line_in : 0) + 4 - in_off;
    return tc_dispatch(cb, &p, (units + 63) / 64, lines, 1, in, in_off, (in_size + 3) & ~3ull, out, out_off,
                       out_at + (u64)out_pitch * lines - out_off);
}
int vk_tc_blit(VkCommandBuffer cb, u32 bpp, u32 fmt, u32 nb, u32 rows, VkBuffer in, u64 in_size, u64 src_at,
               u32 in_pitch, u64 col_at, u64 row_at, VkBuffer out, u64 out_at, u32 out_pitch) {
    u64 out_off = out_at & ~(storage_align() - 1);
    u32 units = (nb + 3) / 4;
    if ((col_at | row_at | out_at | out_pitch) & 3) return 0;
    TcParams p = {.mode = fmt ? TC_BLIT_FLOAT : TC_BLIT,
                  .units = units,
                  .rows = rows,
                  .in_pitch = in_pitch, /* bytes, for this mode */
                  .out_pitch = out_pitch / 4,
                  .in_bias = (u32)src_at,
                  .out_bias = (u32)((out_at - out_off) / 4),
                  .remap = fmt ? fmt : bpp,
                  .const_a = (u32)(col_at / 4),
                  .const_b = (u32)(row_at / 4),
                  .line_out = nb};
    return tc_dispatch(cb, &p, (units + 63) / 64, rows, 1, in, 0, (in_size + 3) & ~3ull, out, out_off,
                       out_at + (u64)out_pitch * rows - out_off);
}
