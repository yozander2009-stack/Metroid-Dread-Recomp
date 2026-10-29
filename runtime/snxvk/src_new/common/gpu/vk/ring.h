/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_COMMON_VK_RING_H
#define SNX_COMMON_VK_RING_H
#include <stdbool.h>
#include <stdint.h>
#include <volk.h>
/* A ring of submissions on one queue, as the 3DS's and the Vita's devices record them: work goes into the open slot's
   command buffer until it is submitted, a few submissions in flight at once, each with a serial and a fence. A slot
   is free again (its command buffer and descriptor pool reset) once its fence passed, in submission order; nothing
   waits for the GPU but what needs its results or a slot to record in. */
#define SNX_VK_RING_SLOTS 4
typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    VkDescriptorPool descriptors; /* the recording's descriptor sets */
    uint64_t serial;              /* 0: not in flight */
} SnxVkRingSlot;
typedef struct {
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    SnxVkRingSlot slots[SNX_VK_RING_SLOTS];
    unsigned count; /* slots in use */
    unsigned slot;  /* the recording's, or the next one's */
    bool recording;
    uint64_t submitted, completed; /* the last serial submitted, the last one done */
    /* the frontend's, each optional: a recording begun in slot, a recording about to end (a rendering pass closed),
       a submission done and its slot free again */
    void (*begin)(unsigned slot);
    void (*close)(void);
    void (*retire)(unsigned slot, uint64_t serial);
} SnxVkRing;
/* count slots (at most SNX_VK_RING_SLOTS) on queue of family, each with a descriptor pool of descriptors. False when
   any could not be made, with what was made freed */
bool snx_vk_ring_init(SnxVkRing* ring, VkDevice device, VkQueue queue, uint32_t family, unsigned count,
                      const VkDescriptorPoolCreateInfo* descriptors);
/* everything freed, without waiting: the device is idle */
void snx_vk_ring_free(SnxVkRing* ring);
/* the open command buffer, begun if none was (in a slot free again, which may wait for its submission) */
VkCommandBuffer snx_vk_ring_commands(SnxVkRing* ring);
/* The recording submitted, without waiting, after a barrier that makes everything written the host's to read once
   its fence is signaled: its serial (the last one's when nothing was recorded and there is no semaphore). wait and
   signal, when not null, are a swapchain's acquire and present semaphores */
uint64_t snx_vk_ring_submit(SnxVkRing* ring, VkSemaphore wait, VkSemaphore signal);
/* the submissions done so far, without waiting: their slots free again. The last serial done */
uint64_t snx_vk_ring_poll(SnxVkRing* ring);
/* until serial is done (submitting the recording first when serial is its), or timeout_ns passed (UINT64_MAX: no
   bound). True when it is done */
bool snx_vk_ring_wait(SnxVkRing* ring, uint64_t serial, uint64_t timeout_ns);
/* a descriptor set of the layout for the open recording, free again when its submission is done */
VkDescriptorSet snx_vk_ring_set(SnxVkRing* ring, VkDescriptorSetLayout layout);
#endif
