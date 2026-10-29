/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Whether the device flushes FP32 subnormals by itself, in a module that declares no denormal mode. The console's
   compiler sets FTZ on its FP32 FADD, FMUL and FFMA, and a device without DenormFlushToZero (NVIDIA's 615 driver
   reports none: shaderDenormFlushToZeroFloat32 is false) gets that flush per instruction from the translator, a test
   and a select on every operand and result (spv_emit_flush_subnormal). In one game's largest fragment shader those
   were 43% of the instructions, and NVIDIA's compile of a pipeline follows its size: with empty caches its pipelines
   took 101 to 145 ms each (four runs), against 55 ms without the flushes, and a frame waits for each new one. Yet the
   same device flushes those operands and results itself, signs kept, as this probe shows on an RTX 2080, so the
   flushes changed no bit there. The driver only can't promise it, which is what the feature reports. So it is measured here, on the operations the
   translator would flush, in the kind of module it makes (SignedZeroInfNanPreserve), and only a device that gives the
   console's every bit for them is translated without the per-instruction flush (SH_FTZ_NATIVE) */
#include <string.h>
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ftz_probe_spv.h"

#define PROBE_N 8 /* the shader's invocations (ftz_probe.comp) */
/* each invocation's a, b and c, and what the console's FADD.FTZ (a + c), FMUL.FTZ (a * b) and FFMA.FTZ give for them:
   subnormal operands read as zeros of their sign, subnormal results become zeros of their sign. NAN for a NaN of any
   payload. Without the flush, every row but the normal numbers' differs from what is expected of it */
#define NAN_ANY 0xFFFFFFFFu
static const u32 g_probe[PROBE_N][6] = {
    /* a, b, c, a * b, a + c, fma */
    {0x00000001u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}, /* the smallest, times 1 */
    {0x80000001u, 0x3f800000u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}, /* signs kept */
    {0x00012345u, 0x4b000000u, 0x007fffffu, 0x00000000u, 0x00000000u, 0x00000000u}, /* would be normal kept */
    {0x00800000u, 0x3f000000u, 0x80800000u, 0x00000000u, 0x00000000u, 0x80000000u}, /* subnormal results */
    {0x00800001u, 0x3f800000u, 0x80800000u, 0x00800001u, 0x00000000u, 0x00000000u}, /* the difference of normals */
    {0x007fffffu, 0x40000000u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u}, /* two subnormals' sum */
    {0x3fc00000u, 0x40000000u, 0x3e800000u, 0x40400000u, 0x3fe00000u, 0x40500000u}, /* normal numbers */
    {0x7f800000u, 0x00000001u, 0x00000000u, NAN_ANY, 0x7f800000u, NAN_ANY},         /* infinity times a flushed 0 */
};

static int probe_same(u32 got, u32 want) {
    if (want == NAN_ANY) return (got & 0x7f800000u) == 0x7f800000u && (got & 0x007fffffu);
    return got == want;
}

/* 1 when every result is the console's; 0 when one isn't, or the probe couldn't run */
int vk_ftz_native(void) {
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &pool_size};
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = ftz_probe_spv_size, .pCode = ftz_probe_spv};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = 6 * PROBE_N * sizeof(u32),
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                   .queueFamilyIndex = g_vk.compute_family};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkDescriptorSetLayout dlayout = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation memory = NULL;
    VmaAllocationInfo allocation;
    VkDescriptorSet set;
    VkCommandBuffer cb;
    u32* words;
    int native = 0, submitted = 0, done = 0;
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dli, NULL, &dlayout))) goto out;
    VkPipelineLayoutCreateInfo pli = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dlayout};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pli, NULL, &playout))) goto out;
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &module))) goto out;
    VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                 .module = module,
                                                 .pName = "main"},
                                       .layout = playout};
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &pci, NULL, &pipeline))) goto out;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buffer, &memory, &allocation))) goto out;
    words = allocation.pMappedData;
    for (int i = 0; i < PROBE_N; i++)
        for (int k = 0; k < 3; k++) words[k * PROBE_N + i] = g_probe[i][k];
    memset(words + 3 * PROBE_N, 0, 3 * PROBE_N * sizeof *words);
    if (!VK_OK(vmaFlushAllocation(g_vk.vma, memory, 0, VK_WHOLE_SIZE))) goto out;
    if (!VK_OK(vkCreateDescriptorPool(g_vk.dev, &dpi, NULL, &dpool))) goto out;
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = dpool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &dlayout};
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &dai, &set))) goto out;
    VkDescriptorBufferInfo dbi = {buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                              .dstSet = set,
                              .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    if (!VK_OK(vkCreateCommandPool(g_vk.dev, &cpi, NULL, &cpool))) goto out;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = cpool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
    if (!VK_OK(vkAllocateCommandBuffers(g_vk.dev, &cai, &cb))) goto out;
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (!VK_OK(vkBeginCommandBuffer(cb, &begin))) goto out;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    /* the shader's writes, made available to the host's reads after the fence */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &dep);
    if (!VK_OK(vkEndCommandBuffer(cb))) goto out;
    if (!VK_OK(vkCreateFence(g_vk.dev, &fci, NULL, &fence))) goto out;
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi};
    /* at the renderer's start, before any other submission: nothing else uses the compute queue yet */
    if (!VK_OK(vkQueueSubmit2(g_vk.compute, 1, &si, fence))) goto out;
    submitted = 1;
    done = VK_OK(vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, 5000000000ull));
    if (!done || !VK_OK(vmaInvalidateAllocation(g_vk.vma, memory, 0, VK_WHOLE_SIZE))) goto out;
    native = 1;
    for (int i = 0; i < PROBE_N; i++)
        for (int k = 0; k < 3; k++) {
            u32 got = words[(3 + k) * PROBE_N + i], want = g_probe[i][3 + k];
            if (!probe_same(got, want)) {
                if (native)
                    LOG("vulkan: FP32 subnormals: %s of %08x, %08x and %08x gave %08x where the console gives %08x\n",
                        k == 0 ? "a * b" : k == 1 ? "a + c" : "fma", g_probe[i][0], g_probe[i][1], g_probe[i][2],
                        got, want);
                native = 0;
            }
        }
out:
    /* a submission that never ended leaves everything it uses as it is: a device that slow is lost */
    if (submitted && !done) return 0;
    if (fence) vkDestroyFence(g_vk.dev, fence, NULL);
    if (cpool) vkDestroyCommandPool(g_vk.dev, cpool, NULL);
    if (dpool) vkDestroyDescriptorPool(g_vk.dev, dpool, NULL);
    if (buffer) vmaDestroyBuffer(g_vk.vma, buffer, memory);
    if (pipeline) vkDestroyPipeline(g_vk.dev, pipeline, NULL);
    if (module) vkDestroyShaderModule(g_vk.dev, module, NULL);
    if (playout) vkDestroyPipelineLayout(g_vk.dev, playout, NULL);
    if (dlayout) vkDestroyDescriptorSetLayout(g_vk.dev, dlayout, NULL);
    return native;
}
