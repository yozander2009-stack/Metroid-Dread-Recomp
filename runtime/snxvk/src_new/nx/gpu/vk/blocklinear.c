/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Block linear surfaces deswizzled and swizzled on the GPU (gpu/texture/blocklinear.comp). Guest bytes go to the GPU
   as they are, a copy of whole ranges into staging, and come back the same way: the 16 byte pieces of a GOB are
   moved by the shader, between the staged bytes and linear rows in a device-local scratch buffer that copies to
   and from images take. One pipeline serves both directions; it is made on first use and kept for the device's
   lifetime, like the other internal pipelines. Uses are on g_vk.gfx, under vk_lock() */
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/texture/blocklinear_spv.h" /* compiled from blocklinear.comp by the build (cmake/glsl.cmake) */
#include "nx/runtime/platform/log.h"

/* blocklinear.comp's push constants, member for member */
typedef struct {
    u32 runs, rows, lin_runs, y0, gob_y0, z0, bh, hmask, bd, dmask, bsh, block_row, slab, bl_bias, lin_bias, to_bl;
    u32 tail_mask[4];
} BlParams;

static VkDescriptorSetLayout g_bl_ds;
static VkPipelineLayout g_bl_layout;
static VkPipeline g_bl_pipe;
static VkBuffer g_bl_scratch;
static VmaAllocation g_bl_mem;
static u64 g_bl_cap;

static void bl_init(void) {
    if (g_bl_pipe) return;
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_bl_ds)))
        fatal("vulkan: cannot create the block linear descriptor layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BlParams)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_bl_ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_bl_layout)))
        fatal("vulkan: cannot create the block linear pipeline layout");
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = blocklinear_spv_size,
                                   .pCode = blocklinear_spv};
    VkShaderModule module;
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &module))) fatal("vulkan: cannot load the block linear shader");
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = module,
                                                .pName = "main"},
                                      .layout = g_bl_layout};
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &g_bl_pipe)))
        fatal("vulkan: cannot create the block linear compute pipeline");
    vkDestroyShaderModule(g_vk.dev, module, NULL); /* the pipeline doesn't need it once made */
}

VkBuffer vk_bl_scratch(u64 size, u64* cap_out) {
    *cap_out = g_bl_cap;
    if (size <= g_bl_cap) return g_bl_scratch;
    if (size > g_vk.alloc_cap) return VK_NULL_HANDLE;
    /* the old one stays alive until the frame's work that may use it is done */
    if (g_bl_scratch) vk_defer_buffer(g_bl_scratch, g_bl_mem);
    g_bl_scratch = VK_NULL_HANDLE;
    g_bl_cap = 0;
    u64 cap = 4ull << 20;
    while (cap < size) cap *= 2;
    if (cap > g_vk.alloc_cap) cap = g_vk.alloc_cap;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = cap,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_bl_scratch, &g_bl_mem, NULL))) {
        g_bl_scratch = VK_NULL_HANDLE;
        *cap_out = 0;
        return VK_NULL_HANDLE;
    }
    vk_mem_track(g_bl_mem);
    *cap_out = g_bl_cap = cap;
    return g_bl_scratch;
}

/* bytes from a row of blocks to the next, and from 2^bd slices to the next (gpu.c's bl_yz) */
static u64 block_row_bytes(const VkBlMove* m) { return (u64)((m->row_bytes + 63) >> 6) << (9 + m->bh + m->bd); }
static u64 slab_bytes(const VkBlMove* m) {
    return (((((u64)m->height + 7) >> 3) + (1u << m->bh) - 1) >> m->bh) * block_row_bytes(m);
}
void vk_bl_range(const VkBlMove* m, u64* lo, u64* hi) {
    u32 last = m->y0 + m->rows - 1, zl = m->z0 + m->slices - 1;
    *lo = (u64)(m->z0 >> m->bd) * slab_bytes(m) + (u64)((m->y0 >> 3) >> m->bh) * block_row_bytes(m);
    *hi = (u64)(zl >> m->bd) * slab_bytes(m) + (u64)(((last >> 3) >> m->bh) + 1) * block_row_bytes(m);
}

/* global, as the buffers it orders are several and change */
void vk_bl_barrier(VkCommandBuffer cb, int before) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    if (before) {
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    } else {
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_HOST_READ_BIT;
    }
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}

int vk_bl_move(VkCommandBuffer cb, const VkBlMove* m, int to_bl, VkBuffer bl, u64 bl_at, VkBuffer lin, u64 lin_at) {
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    u64 align = lim->minStorageBufferOffsetAlignment < 16 ? 16 : lim->minStorageBufferOffsetAlignment;
    u64 lo, hi;
    if (!m->rows || !m->slices || !m->row_bytes) return 1;
    vk_bl_range(m, &lo, &hi);
    /* the bound ranges start on the device's alignment: the biases say where the data is in them */
    u64 bl_off = bl_at & ~(align - 1), lin_off = lin_at & ~(align - 1);
    u64 bl_size = bl_at + (hi - lo) - bl_off, lin_size = lin_at + (u64)m->slices * m->rows * m->lin_pitch - lin_off;
    u32 runs = (m->row_bytes + 15) >> 4, gx = (runs + 7) >> 3, gy = ((m->y0 + m->rows + 7) >> 3) - (m->y0 >> 3);
    if ((bl_at | lin_at | m->lin_pitch) & 15 || m->lin_pitch < runs * 16 || bl_size > lim->maxStorageBufferRange ||
        lin_size > lim->maxStorageBufferRange || gx > lim->maxComputeWorkGroupCount[0] ||
        gy > lim->maxComputeWorkGroupCount[1] || m->slices > lim->maxComputeWorkGroupCount[2]) {
        LOG_LIMIT(1, "vulkan: a %ux%u block linear surface exceeds the device's limits (left out)\n", m->row_bytes,
                  m->rows);
        return 0;
    }
    bl_init();
    VkDescriptorSet set = vk_desc_alloc(g_bl_ds);
    if (!set) return 0;
    VkDescriptorBufferInfo sides[2] = {{bl, bl_off, bl_size}, {lin, lin_off, lin_size}};
    VkWriteDescriptorSet writes[2];
    for (u32 i = 0; i < 2; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = to_bl ? 1 - i : i, /* binding 0 is read, 1 written */
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &sides[i]};
    vkUpdateDescriptorSets(g_vk.dev, 2, writes, 0, NULL);
    BlParams p = {.runs = runs,
                  .rows = m->rows,
                  .lin_runs = m->lin_pitch >> 4,
                  .y0 = m->y0,
                  .gob_y0 = m->y0 >> 3,
                  .z0 = m->z0,
                  .bh = m->bh,
                  .hmask = (1u << m->bh) - 1,
                  .bd = m->bd,
                  .dmask = (1u << m->bd) - 1,
                  .bsh = 5 + m->bh + m->bd,
                  .block_row = (u32)(block_row_bytes(m) >> 4),
                  .slab = (u32)(slab_bytes(m) >> 4),
                  /* the run at lo is at bl_at: modulo 2^32, as the shader adds it */
                  .bl_bias = (u32)((bl_at - bl_off) >> 4) - (u32)(lo >> 4),
                  .lin_bias = (u32)((lin_at - lin_off) >> 4),
                  .to_bl = (u32)to_bl};
    for (u32 i = 0; i < 4; i++) {
        u32 tail = m->row_bytes & 15;
        u32 bytes = !tail ? 4 : tail > 4 * i ? tail - 4 * i : 0;
        p.tail_mask[i] = bytes >= 4 ? ~0u : (1u << (bytes * 8)) - 1;
    }
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_bl_pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_bl_layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_bl_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    rec_CmdDispatch(cb, gx, gy, m->slices);
    return 1;
}
