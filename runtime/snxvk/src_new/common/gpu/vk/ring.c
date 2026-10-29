/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/ring.h"

bool snx_vk_ring_init(SnxVkRing* ring, VkDevice device, VkQueue queue, uint32_t family, unsigned count,
                      const VkDescriptorPoolCreateInfo* descriptors) {
    *ring = (SnxVkRing){.device = device, .queue = queue, .count = count};
    if (!count || count > SNX_VK_RING_SLOTS) return false;
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = family};
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    bool made = vkCreateCommandPool(device, &pi, NULL, &ring->pool) == VK_SUCCESS;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = ring->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    for (unsigned i = 0; i < count && made; ++i)
        made = vkCreateFence(device, &fi, NULL, &ring->slots[i].fence) == VK_SUCCESS &&
               vkCreateDescriptorPool(device, descriptors, NULL, &ring->slots[i].descriptors) == VK_SUCCESS &&
               vkAllocateCommandBuffers(device, &ai, &ring->slots[i].cb) == VK_SUCCESS;
    if (!made) snx_vk_ring_free(ring);
    return made;
}
void snx_vk_ring_free(SnxVkRing* ring) {
    if (!ring->device) return;
    for (unsigned i = 0; i < SNX_VK_RING_SLOTS; ++i) {
        if (ring->slots[i].descriptors) vkDestroyDescriptorPool(ring->device, ring->slots[i].descriptors, NULL);
        if (ring->slots[i].fence) vkDestroyFence(ring->device, ring->slots[i].fence, NULL);
    }
    if (ring->pool) vkDestroyCommandPool(ring->device, ring->pool, NULL);
    *ring = (SnxVkRing){0};
}

/* a submission done: its slot free again, and what the frontend kept for it */
static void retire(SnxVkRing* ring, unsigned i) {
    SnxVkRingSlot* s = &ring->slots[i];
    vkResetFences(ring->device, 1, &s->fence);
    vkResetCommandBuffer(s->cb, 0);
    vkResetDescriptorPool(ring->device, s->descriptors, 0);
    if (s->serial > ring->completed) ring->completed = s->serial;
    if (ring->retire) ring->retire(i, s->serial);
    s->serial = 0;
}
/* the slots in submission order, the oldest first: the next slot to record is the one submitted longest ago (the
   open recording's slot is not in flight, and is skipped) */
static unsigned oldest(const SnxVkRing* ring, unsigned n) { return (ring->slot + n) % ring->count; }
uint64_t snx_vk_ring_poll(SnxVkRing* ring) {
    if (!ring->pool) return 0;
    for (unsigned n = 0; n < ring->count; ++n) {
        unsigned i = oldest(ring, n);
        if (!ring->slots[i].serial || (ring->recording && i == ring->slot)) continue;
        /* in order: the later ones are not done either */
        if (vkGetFenceStatus(ring->device, ring->slots[i].fence) != VK_SUCCESS) break;
        retire(ring, i);
    }
    return ring->completed;
}
bool snx_vk_ring_wait(SnxVkRing* ring, uint64_t serial, uint64_t timeout_ns) {
    if (!ring->pool || serial <= ring->completed) return true;
    if (serial > ring->submitted) snx_vk_ring_submit(ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
    for (unsigned n = 0; n < ring->count && ring->completed < serial; ++n) {
        unsigned i = oldest(ring, n);
        const SnxVkRingSlot* s = &ring->slots[i];
        if (!s->serial || s->serial <= ring->completed || (ring->recording && i == ring->slot)) continue;
        if (vkWaitForFences(ring->device, 1, &s->fence, VK_TRUE, timeout_ns) != VK_SUCCESS)
            return false; /* the timeout, or a lost device */
        retire(ring, i);
    }
    return ring->completed >= serial;
}
VkCommandBuffer snx_vk_ring_commands(SnxVkRing* ring) {
    if (!ring->recording) {
        /* the slot free for a recording: its last submission waited for when it is still in flight */
        if (ring->slots[ring->slot].serial) (void)snx_vk_ring_wait(ring, ring->slots[ring->slot].serial, UINT64_MAX);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(ring->slots[ring->slot].cb, &bi);
        ring->recording = true;
        if (ring->begin) ring->begin(ring->slot);
    }
    return ring->slots[ring->slot].cb;
}
uint64_t snx_vk_ring_submit(SnxVkRing* ring, VkSemaphore wait, VkSemaphore signal) {
    if (!ring->pool) return 0;
    if (!ring->recording && !wait && !signal) return ring->submitted;
    VkCommandBuffer cb = snx_vk_ring_commands(ring);
    if (ring->close) ring->close();
    /* everything written is the host's to read once the fence is signaled */
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0,
                         NULL);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .waitSemaphoreCount = wait ? 1 : 0,
                       .pWaitSemaphores = &wait,
                       .pWaitDstStageMask = &stage,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &cb,
                       .signalSemaphoreCount = signal ? 1 : 0,
                       .pSignalSemaphores = &signal};
    ring->recording = false;
    SnxVkRingSlot* s = &ring->slots[ring->slot];
    if (vkEndCommandBuffer(cb) != VK_SUCCESS || vkQueueSubmit(ring->queue, 1, &si, s->fence) != VK_SUCCESS) {
        /* a lost device: nothing more runs, and the slot is free as if it had */
        vkResetCommandBuffer(cb, 0);
        return ring->submitted;
    }
    s->serial = ++ring->submitted;
    ring->slot = (ring->slot + 1) % ring->count;
    return ring->submitted;
}
VkDescriptorSet snx_vk_ring_set(SnxVkRing* ring, VkDescriptorSetLayout layout) {
    snx_vk_ring_commands(ring); /* the open recording's slot, free */
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = ring->slots[ring->slot].descriptors,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &layout};
    VkDescriptorSet set = VK_NULL_HANDLE;
    return vkAllocateDescriptorSets(ring->device, &ai, &set) == VK_SUCCESS ? set : VK_NULL_HANDLE;
}
