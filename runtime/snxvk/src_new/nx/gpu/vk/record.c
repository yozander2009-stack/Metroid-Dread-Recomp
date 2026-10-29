/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The graphics command buffers are recorded on a thread of their own. In one game a fifth of the GPU
   thread's time was the driver encoding commands, which doesn't depend on anything the GPU thread does next: the
   rec_Cmd* functions copy a command's arguments into a ring, and the recording thread makes the Vulkan call. A
   command buffer is begun, ended and submitted from the ring too, in order with its commands.

   Only the frame's command buffer (vk_cmd) goes through the ring. A command for any other one (compute's, VIC's)
   is made at once, as are commands while vk_record=0. Whoever adds to the ring holds vk_lock, so there is one
   producer at a time. A present goes through the ring too, after the submission whose semaphore it waits on. What
   else has to happen after the queued commands are recorded and submitted waits for the ring to drain
   (vk_rec_sync): a new command buffer from the frame's pool, a device wait. Waits on the graphics timeline need
   none, as a timeline value can be waited for before its submission. */
#include <stdlib.h>
#include <string.h>
#include "common/utilities/common.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"

typedef void (*RecFn)(const void* args);
typedef struct {
    RecFn fn; /* NULL: skip to the start of the ring */
    u64 size; /* of the packet, header included, a multiple of 16 */
} RecHdr;

/* The ring holds about a frame of one game's commands (3.8 MB a frame in its underground scene). At 16 MB every packet
   went to memory last written four frames before, long out of the caches, and the stream through the shared cache
   likely pushed out what the GPU thread's draws look up: at 4 MB that thread's load misses dropped a tenth and its
   frames took 5% fewer cycles. The producer waits for room only when the recording thread is a whole frame behind */
#define REC_CAP     (4ull << 20)
#define REC_PUBLISH (32ull << 10)

static u8* g_ring;
static int g_rec = -1; /* recording on the thread: 1, at once: 0, not decided yet: -1 */
static VkCommandBuffer g_rec_cb;
/* written by the producer: the end of what it has added, and the consumer: the end of what it has made */
static u64 g_head;
static char g_pad0[64];
static u64 g_tail;
static char g_pad1[64];
static int g_sleep; /* the consumer waits on g_cv for more */
static PMutex g_m;
static PCond g_cv;

static void rec_thread(void* arg) {
    u64 tail = 0;
    (void)arg;
    SNX_THREAD_NAME("Vulkan recorder");
    (void)g_pad0;
    (void)g_pad1;
    SNX_THREAD_NAME("vk record");
    for (;;) {
        u64 head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
        /* nothing more yet: sleep until the producer publishes (REC_PUBLISH). It spun 4000 pauses first, which with
           a publish every 4 KiB kept it from sleeping at all: a host core spent on waiting (0.8 of one in Tears of
           the Kingdom, 0.26 with this) */
        if (head == tail) {
            pm_lock(&g_m);
            __atomic_store_n(&g_sleep, 1, __ATOMIC_SEQ_CST);
            while ((head = __atomic_load_n(&g_head, __ATOMIC_SEQ_CST)) == tail) pc_wait(&g_cv, &g_m, -1);
            __atomic_store_n(&g_sleep, 0, __ATOMIC_RELAXED);
            pm_unlock(&g_m);
        }
        u64 records __attribute__((unused)) = 0; /* the zone's value; only the profiler reads it */
        SNX_ZONE(z_rec, "vk record");
        while (tail != head) {
            const RecHdr* h = (const RecHdr*)(g_ring + (tail & (REC_CAP - 1)));
            if (h->fn) h->fn(h + 1);
            tail += h->size;
            records++;
        }
        SNX_ZONE_VALUE(z_rec, records);
        SNX_ZONE_END(z_rec);
        __atomic_store_n(&g_tail, tail, __ATOMIC_RELEASE);
    }
}

static int rec_enabled(void) {
    if (g_rec < 0) {
        g_rec = g_settings.vulkan_recording_thread;
        if (g_rec) {
            g_ring = malloc(REC_CAP);
            pm_init(&g_m);
            pc_init(&g_cv);
            if (!g_ring || plat_thread(rec_thread, NULL, (size_t)1 << 20)) {
                char why[256];
                WARN("vulkan: could not start the command recording thread (%s): recording on the GPU thread\n",
                     snx_os_error(why, sizeof why));
                free(g_ring);
                g_ring = NULL;
                g_rec = 0;
            }
        }
    }
    return g_rec;
}

void vk_rec_init(void) { rec_enabled(); }
int vk_rec_threaded(void) { return g_rec > 0; }
u64 vk_rec_behind(void) {
    return g_rec > 0 ? __atomic_load_n(&g_head, __ATOMIC_ACQUIRE) - __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE) : 0;
}

/* a command for cb goes through the ring */
static int rec_on(VkCommandBuffer cb) { return cb == g_rec_cb && g_rec > 0; }

static void wake(void) {
    if (__atomic_load_n(&g_sleep, __ATOMIC_SEQ_CST)) {
        pm_lock(&g_m);
        pc_signal(&g_cv);
        pm_unlock(&g_m);
    }
}

/* What the producer has added (g_pending) is published to the consumer (g_head) REC_PUBLISH bytes at a time, and at
   once for a submission or a drain: until something waits for the commands, it doesn't matter when they are
   recorded. Each publish is a locked store (with the wake-up check it has to be ordered with), and wakes a sleeping
   consumer: a batch of 32 KiB lets it record a while and sleep again, not wake for every few draws */
static u64 g_pending, g_published;
static void publish(void) {
    g_published = g_pending;
    __atomic_store_n(&g_head, g_pending, __ATOMIC_SEQ_CST);
    wake();
}
/* room for a packet of size bytes of arguments, filled in and then made known by rec_commit */
static void* rec_push(RecFn fn, u64 size) {
    u64 need = (sizeof(RecHdr) + size + 15) & ~15ull, at = g_pending & (REC_CAP - 1);
    if (at + need > REC_CAP) { /* the packet doesn't fit before the end: the rest is skipped */
        u64 skip = REC_CAP - at;
        while (g_pending + skip - __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE) > REC_CAP) {
            publish();
            plat_yield();
        }
        RecHdr* s = (RecHdr*)(g_ring + at);
        s->fn = NULL;
        s->size = skip;
        g_pending += skip;
        at = 0;
    }
    while (g_pending + need - __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE) > REC_CAP) {
        publish();
        plat_yield();
    }
    RecHdr* h = (RecHdr*)(g_ring + at);
    h->fn = fn;
    h->size = need;
    g_pending += need;
    return h + 1;
}
static void rec_commit(void) {
    if (g_pending - g_published >= REC_PUBLISH) publish();
}

void vk_rec_sync(void) {
    if (g_rec <= 0) return;
    vk_lock(); /* no command is added meanwhile */
    publish();
    while (__atomic_load_n(&g_tail, __ATOMIC_ACQUIRE) != g_pending) plat_yield();
    vk_unlock();
}

void vk_rec_queue(VkQueue q) {
    if (q == g_vk.gfx) vk_rec_sync();
}

/* ------------------------------------------------------------ beginning and submitting */
static void begin_run(const void* p) {
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_OK(vkBeginCommandBuffer(*(const VkCommandBuffer*)p, &bi));
}
void vk_rec_begin(VkCommandBuffer cb) {
    g_rec_cb = cb;
    if (!rec_enabled()) {
        begin_run(&cb);
        return;
    }
    *(VkCommandBuffer*)rec_push(begin_run, sizeof cb) = cb;
}

typedef struct {
    VkCommandBuffer cb;
    VkSemaphoreSubmitInfo w[2], s[2];
    u32 nw, ns;
    VkSemaphore timeline;
    u64 value;
} Submit;
static void submit_run(const void* p) {
    const Submit* x = p;
    VK_OK(vkEndCommandBuffer(x->cb));
    VkCommandBufferSubmitInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = x->cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .waitSemaphoreInfoCount = x->nw,
                        .pWaitSemaphoreInfos = x->w,
                        .commandBufferInfoCount = 1,
                        .pCommandBufferInfos = &c,
                        .signalSemaphoreInfoCount = x->ns,
                        .pSignalSemaphoreInfos = x->s};
    vk_wait_foreground();
    VkResult r = vkQueueSubmit2(g_vk.gfx, 1, &si, VK_NULL_HANDLE);
    if (r == VK_ERROR_DEVICE_LOST) fatal("vulkan: the GPU device was lost (driver reset or crash)");
    VK_OK(r);
    /* a timeline of FFmpeg's never reaching its value would hang its decoder for good */
    if (r != VK_SUCCESS && x->ns > 1 && x->s[1].value)
        fatal("vulkan: a submission FFmpeg's Vulkan Video decoder waits for failed");
    if (r != VK_SUCCESS) { /* never signaled by the GPU: the host signals it, so waits for it and later ones end */
        VkSemaphoreSignalInfo ss = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = x->timeline, .value = x->value};
        VK_OK(vkSignalSemaphore(g_vk.dev, &ss));
    }
}
void vk_rec_submit(VkCommandBuffer cb, const VkSemaphoreSubmitInfo* w, u32 nw, const VkSemaphoreSubmitInfo* s,
                   u32 ns) {
    Submit x = {.cb = cb, .nw = nw, .ns = ns, .timeline = s[0].semaphore, .value = s[0].value};
    memcpy(x.w, w, nw * sizeof *w);
    memcpy(x.s, s, ns * sizeof *s);
    g_rec_cb = VK_NULL_HANDLE;
    if (!rec_enabled()) {
        submit_run(&x);
        return;
    }
    *(Submit*)rec_push(submit_run, sizeof x) = x;
    publish(); /* the timeline value is waited for by whoever needs the work done */
}

/* A present, after the frame's submission. The GPU thread waited for the ring to drain before presenting, and so
   for the recording of its whole frame: a sixth of its time in one game, where it could have gone on with the next
   frame. Its result comes back at a later present (vk_rec_present) */
typedef struct {
    VkSwapchainKHR sc;
    VkSemaphore wait;
    u32 idx;
} Present;
static VkResult g_present_result; /* atomic: the last present's result that wasn't VK_SUCCESS, until it is taken */
static VkResult present_run(const Present* x) {
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &x->wait,
                           .swapchainCount = 1,
                           .pSwapchains = &x->sc,
                           .pImageIndices = &x->idx};
    SNX_ZONE(z_queue_present, "vkQueuePresentKHR");
    return vkQueuePresentKHR(g_vk.gfx, &pi);
}
static void present_queued(const void* p) {
    VkResult r = present_run(p);
    if (r != VK_SUCCESS) __atomic_store_n(&g_present_result, r, __ATOMIC_RELEASE);
}
VkResult vk_rec_present(VkSwapchainKHR sc, u32 idx, VkSemaphore wait) {
    Present x = {sc, wait, idx};
    if (g_rec <= 0) return present_run(&x);
    *(Present*)rec_push(present_queued, sizeof x) = x;
    publish();
    return vk_rec_present_result();
}
VkResult vk_rec_present_result(void) {
    return g_rec > 0 ? __atomic_exchange_n(&g_present_result, VK_SUCCESS, __ATOMIC_ACQ_REL) : VK_SUCCESS;
}

/* ------------------------------------------------------------ commands of scalar arguments */
#define REC_FN(Name, PARAMS, FIELDS, SET, ARGS, XARGS)                                                                 \
    typedef struct {                                                                                                   \
        VkCommandBuffer cb;                                                                                            \
        FIELDS                                                                                                         \
    } Name##Args;                                                                                                      \
    static void Name##_run(const void* p) {                                                                            \
        const Name##Args* x = p;                                                                                       \
        vk##Name XARGS;                                                                                                \
    }                                                                                                                  \
    void rec_##Name PARAMS {                                                                                           \
        if (!rec_on(cb)) {                                                                                             \
            vk##Name ARGS;                                                                                             \
            return;                                                                                                    \
        }                                                                                                              \
        Name##Args* x = rec_push(Name##_run, sizeof *x);                                                               \
        x->cb = cb;                                                                                                    \
        SET rec_commit();                                                                                              \
    }
#define REC0(Name) REC_FN(Name, (VkCommandBuffer cb), , , (cb), (x->cb))
#define REC1(Name, T1)                                                                                                 \
    REC_FN(Name, (VkCommandBuffer cb, T1 a1), T1 a1;, x->a1 = a1;, (cb, a1), (x->cb, x->a1))
#define REC2(Name, T1, T2)                                                                                             \
    REC_FN(Name, (VkCommandBuffer cb, T1 a1, T2 a2), T1 a1; T2 a2;, x->a1 = a1; x->a2 = a2;, (cb, a1, a2),            \
           (x->cb, x->a1, x->a2))
#define REC3(Name, T1, T2, T3)                                                                                         \
    REC_FN(Name, (VkCommandBuffer cb, T1 a1, T2 a2, T3 a3), T1 a1; T2 a2; T3 a3;, x->a1 = a1; x->a2 = a2;           \
           x->a3 = a3;, (cb, a1, a2, a3), (x->cb, x->a1, x->a2, x->a3))
#define REC4(Name, T1, T2, T3, T4)                                                                                     \
    REC_FN(Name, (VkCommandBuffer cb, T1 a1, T2 a2, T3 a3, T4 a4), T1 a1; T2 a2; T3 a3; T4 a4;, x->a1 = a1;          \
           x->a2 = a2; x->a3 = a3; x->a4 = a4;, (cb, a1, a2, a3, a4), (x->cb, x->a1, x->a2, x->a3, x->a4))
#define REC5(Name, T1, T2, T3, T4, T5)                                                                                 \
    REC_FN(Name, (VkCommandBuffer cb, T1 a1, T2 a2, T3 a3, T4 a4, T5 a5), T1 a1; T2 a2; T3 a3; T4 a4; T5 a5;,        \
           x->a1 = a1; x->a2 = a2; x->a3 = a3; x->a4 = a4; x->a5 = a5;, (cb, a1, a2, a3, a4, a5),                    \
           (x->cb, x->a1, x->a2, x->a3, x->a4, x->a5))
#define REC7(Name, T1, T2, T3, T4, T5, T6, T7)                                                                         \
    REC_FN(Name, (VkCommandBuffer cb, T1 a1, T2 a2, T3 a3, T4 a4, T5 a5, T6 a6, T7 a7),                              \
           T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; T6 a6; T7 a7;,                                                         \
           x->a1 = a1; x->a2 = a2; x->a3 = a3; x->a4 = a4; x->a5 = a5; x->a6 = a6; x->a7 = a7;,                      \
           (cb, a1, a2, a3, a4, a5, a6, a7), (x->cb, x->a1, x->a2, x->a3, x->a4, x->a5, x->a6, x->a7))

/* A graphics pipeline may be a stand-in for one a worker is still making (vk_pipeline_get_deferred): it is waited for
   here, at its bind. One that couldn't be made binds nothing, and the draws after it until the next graphics bind are
   dropped, as the GPU thread drops a draw without its pipeline (the recording thread's own flag: 1). With
   async_pipelines one still being made isn't waited for: its draws are dropped the same way (2) until a later frame
   binds it made. A new shader's compile, 60 to 400 ms on one driver, stalled the frame it first appeared in */
static int g_rec_unbound;
static u64 g_rec_dropped, g_rec_skipped;
typedef struct {
    VkCommandBuffer cb;
    VkPipelineBindPoint bp;
    VkPipeline p;
} BindPipelineArgs;
static void bind_pipeline_run(const void* p) {
    const BindPipelineArgs* x = p;
    int graphics = x->bp == VK_PIPELINE_BIND_POINT_GRAPHICS, pending = 0;
    VkPipeline pipe =
        graphics && g_settings.async_pipelines ? vk_pipeline_now(x->p, &pending) : vk_pipeline_resolve(x->p);
    if (graphics) g_rec_unbound = pipe ? 0 : pending ? 2 : 1;
    if (pipe) vkCmdBindPipeline(x->cb, x->bp, pipe);
}
void rec_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (!rec_on(cb)) {
        VkPipeline pipe = vk_pipeline_resolve(p);
        if (pipe) vkCmdBindPipeline(cb, bp, pipe);
        return;
    }
    BindPipelineArgs* x = rec_push(bind_pipeline_run, sizeof *x);
    *x = (BindPipelineArgs){cb, bp, p};
    rec_commit();
}
static int rec_drop_draw(void) {
    if (!g_rec_unbound) return 0;
    if (g_rec_unbound == 2) {
        if (g_rec_skipped++ % 1000 == 0)
            LOG("vulkan: %llu draws skipped while their pipelines were made (async_pipelines)\n",
                (unsigned long long)g_rec_skipped);
    } else if (!g_rec_dropped++)
        WARN("vulkan: a pipeline couldn't be made: its draws are dropped\n");
    return 1;
}
typedef struct {
    VkCommandBuffer cb;
    u32 a1, a2, a3, a4;
} DrawArgs;
static void draw_run(const void* p) {
    const DrawArgs* x = p;
    if (!rec_drop_draw()) vkCmdDraw(x->cb, x->a1, x->a2, x->a3, x->a4);
}
void rec_CmdDraw(VkCommandBuffer cb, u32 vertices, u32 instances, u32 first_vertex, u32 first_instance) {
    if (!rec_on(cb)) {
        vkCmdDraw(cb, vertices, instances, first_vertex, first_instance);
        return;
    }
    DrawArgs* x = rec_push(draw_run, sizeof *x);
    *x = (DrawArgs){cb, vertices, instances, first_vertex, first_instance};
    rec_commit();
}
typedef struct {
    VkCommandBuffer cb;
    u32 a1, a2, a3;
    i32 a4;
    u32 a5;
} DrawIndexedArgs;
static void draw_indexed_run(const void* p) {
    const DrawIndexedArgs* x = p;
    if (!rec_drop_draw()) vkCmdDrawIndexed(x->cb, x->a1, x->a2, x->a3, x->a4, x->a5);
}
void rec_CmdDrawIndexed(VkCommandBuffer cb, u32 indices, u32 instances, u32 first_index, i32 vertex_offset,
                        u32 first_instance) {
    if (!rec_on(cb)) {
        vkCmdDrawIndexed(cb, indices, instances, first_index, vertex_offset, first_instance);
        return;
    }
    DrawIndexedArgs* x = rec_push(draw_indexed_run, sizeof *x);
    *x = (DrawIndexedArgs){cb, indices, instances, first_index, vertex_offset, first_instance};
    rec_commit();
}
REC3(CmdBindIndexBuffer, VkBuffer, VkDeviceSize, VkIndexType)
REC3(CmdDispatch, u32, u32, u32)
REC0(CmdEndRendering)
REC1(CmdSetCullMode, VkCullModeFlags)
REC1(CmdSetFrontFace, VkFrontFace)
REC1(CmdSetPrimitiveTopology, VkPrimitiveTopology)
REC1(CmdSetPrimitiveRestartEnable, VkBool32)
REC1(CmdSetRasterizerDiscardEnable, VkBool32)
REC1(CmdSetLineWidth, float)
REC1(CmdSetDepthBiasEnable, VkBool32)
REC3(CmdSetDepthBias, float, float, float)
REC1(CmdSetDepthTestEnable, VkBool32)
REC1(CmdSetDepthWriteEnable, VkBool32)
REC1(CmdSetDepthCompareOp, VkCompareOp)
REC1(CmdSetDepthBoundsTestEnable, VkBool32)
REC2(CmdSetDepthBounds, float, float)
REC1(CmdSetStencilTestEnable, VkBool32)
REC5(CmdSetStencilOp, VkStencilFaceFlags, VkStencilOp, VkStencilOp, VkStencilOp, VkCompareOp)
REC2(CmdSetStencilReference, VkStencilFaceFlags, u32)
REC2(CmdSetStencilCompareMask, VkStencilFaceFlags, u32)
REC2(CmdSetStencilWriteMask, VkStencilFaceFlags, u32)
REC1(CmdSetAttachmentFeedbackLoopEnableEXT, VkImageAspectFlags)
REC3(CmdResetQueryPool, VkQueryPool, u32, u32)
REC3(CmdWriteTimestamp2, VkPipelineStageFlags2, VkQueryPool, u32)
REC3(CmdBeginQuery, VkQueryPool, u32, VkQueryControlFlags)
REC2(CmdEndQuery, VkQueryPool, u32)
REC7(CmdCopyQueryPoolResults, VkQueryPool, u32, u32, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags)
REC0(CmdEndConditionalRenderingEXT)

/* conditional rendering on the word at buf + off, its begin info made from the arguments */
typedef struct {
    VkCommandBuffer cb;
    VkBuffer buf;
    VkDeviceSize off;
    VkConditionalRenderingFlagsEXT flags;
} CondArgs;
static void cond_call(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkConditionalRenderingFlagsEXT flags) {
    VkConditionalRenderingBeginInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT,
                                             .buffer = buf,
                                             .offset = off,
                                             .flags = flags};
    vkCmdBeginConditionalRenderingEXT(cb, &ci);
}
static void cond_run(const void* p) {
    const CondArgs* x = p;
    cond_call(x->cb, x->buf, x->off, x->flags);
}
void rec_CmdBeginConditionalRenderingEXT(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                                         VkConditionalRenderingFlagsEXT flags) {
    if (!rec_on(cb)) {
        cond_call(cb, buf, off, flags);
        return;
    }
    CondArgs* x = rec_push(cond_run, sizeof *x);
    x->cb = cb;
    x->buf = buf;
    x->off = off;
    x->flags = flags;
    rec_commit();
}

/* ------------------------------------------------------------ commands with arrays, copied after their arguments */
/* the packet's arrays follow its arguments, each at a multiple of 16 */
#define REC_AL(n) (((n) + 15) & ~(u64)15)

typedef struct {
    VkCommandBuffer cb;
    float c[4];
} BlendArgs;
static void blend_run(const void* p) {
    const BlendArgs* x = p;
    vkCmdSetBlendConstants(x->cb, x->c);
}
void rec_CmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    if (!rec_on(cb)) {
        vkCmdSetBlendConstants(cb, c);
        return;
    }
    BlendArgs* x = rec_push(blend_run, sizeof *x);
    x->cb = cb;
    memcpy(x->c, c, sizeof x->c);
    rec_commit();
}

typedef struct {
    VkCommandBuffer cb;
    VkPipelineLayout layout;
    VkShaderStageFlags stages;
    u32 offset, size;
} PushArgs;
static void push_run(const void* p) {
    const PushArgs* x = p;
    vkCmdPushConstants(x->cb, x->layout, x->stages, x->offset, x->size, (const u8*)p + REC_AL(sizeof *x));
}
void rec_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, u32 offset, u32 size,
                          const void* data) {
    /* the device reads push constants as it reads memory, little-endian: a big-endian host swaps the words */
    if (!rec_on(cb)) {
        u32 le[64];
        if (SNX_HOST_BE && size <= sizeof le) {
            memcpy(le, data, size);
            snx_le32_array(le, size / 4);
            data = le;
        }
        vkCmdPushConstants(cb, layout, stages, offset, size, data);
        return;
    }
    PushArgs* x = rec_push(push_run, REC_AL(sizeof *x) + size);
    *x = (PushArgs){cb, layout, stages, offset, size};
    memcpy((u8*)x + REC_AL(sizeof *x), data, size);
    snx_le32_array((u8*)x + REC_AL(sizeof *x), size / 4);
    rec_commit();
}

typedef struct {
    VkCommandBuffer cb;
    VkPipelineBindPoint bp;
    VkPipelineLayout layout;
    u32 first, n, ndyn;
} SetsArgs;
static VkDescriptorSet rec_set_real(VkDescriptorSet s);
static void sets_run(const void* p) {
    const SetsArgs* x = p;
    const VkDescriptorSet* sets = (const void*)((const u8*)p + REC_AL(sizeof *x));
    const u32* dyn = (const void*)((const u8*)sets + REC_AL(x->n * sizeof *sets));
    VkDescriptorSet real[8];
    for (u32 i = 0; i < x->n && i < 8; i++)
        if (!(real[i] = rec_set_real(sets[i]))) return; /* a set the pools had no room for: not bound */
    vkCmdBindDescriptorSets(x->cb, x->bp, x->layout, x->first, x->n, real, x->ndyn, x->ndyn ? dyn : NULL);
}
void rec_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, u32 first, u32 n,
                               const VkDescriptorSet* sets, u32 ndyn, const u32* dyn) {
    if (!rec_on(cb)) {
        vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, ndyn, dyn);
        return;
    }
    u64 so = REC_AL(sizeof(SetsArgs)), dof = so + REC_AL(n * sizeof *sets);
    SetsArgs* x = rec_push(sets_run, dof + ndyn * sizeof *dyn);
    *x = (SetsArgs){cb, bp, layout, first, n, ndyn};
    memcpy((u8*)x + so, sets, n * sizeof *sets);
    if (ndyn) memcpy((u8*)x + dof, dyn, ndyn * sizeof *dyn);
    rec_commit();
}

typedef struct {
    VkCommandBuffer cb;
    u32 first, n;
    int sizes, strides;
} VbArgs;
static void vb_run(const void* p) {
    const VbArgs* x = p;
    const u8* q = (const u8*)p + REC_AL(sizeof *x);
    u64 bn = REC_AL(x->n * sizeof(VkBuffer)), dn = REC_AL(x->n * sizeof(VkDeviceSize));
    const VkDeviceSize* offs = (const void*)(q + bn);
    const VkDeviceSize* sizes = x->sizes ? (const void*)(q + bn + dn) : NULL;
    const VkDeviceSize* strides = x->strides ? (const void*)(q + bn + dn * (1 + x->sizes)) : NULL;
    vkCmdBindVertexBuffers2(x->cb, x->first, x->n, (const VkBuffer*)q, offs, sizes, strides);
}
void rec_CmdBindVertexBuffers2(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* bufs, const VkDeviceSize* offs,
                               const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    if (!rec_on(cb)) {
        vkCmdBindVertexBuffers2(cb, first, n, bufs, offs, sizes, strides);
        return;
    }
    u64 h = REC_AL(sizeof(VbArgs)), bn = REC_AL(n * sizeof *bufs), dn = REC_AL(n * sizeof *offs);
    VbArgs* x = rec_push(vb_run, h + bn + dn * (1 + !!sizes + !!strides));
    u8* q = (u8*)x + h;
    *x = (VbArgs){cb, first, n, !!sizes, !!strides};
    memcpy(q, bufs, n * sizeof *bufs);
    memcpy(q + bn, offs, n * sizeof *offs);
    if (sizes) memcpy(q + bn + dn, sizes, n * sizeof *sizes);
    if (strides) memcpy(q + bn + dn * (1 + !!sizes), strides, n * sizeof *strides);
    rec_commit();
}
void rec_CmdBindVertexBuffers(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* bufs, const VkDeviceSize* offs) {
    rec_CmdBindVertexBuffers2(cb, first, n, bufs, offs, NULL, NULL);
}

/* transform feedback (VK_EXT_transform_feedback): the buffers with their offsets and sizes (bind), or the counter
   buffers begin resumes from and end writes (NULL: none), each an array of n after the arguments */
typedef struct {
    VkCommandBuffer cb;
    u32 first, n;
    int what; /* 0 bind, 1 begin, 2 end */
    int counters;
} XfbArgs;
static void xfb_run(const void* p) {
    const XfbArgs* x = p;
    const u8* q = (const u8*)p + REC_AL(sizeof *x);
    u64 bn = REC_AL(x->n * sizeof(VkBuffer)), dn = REC_AL(x->n * sizeof(VkDeviceSize));
    const VkBuffer* bufs = (const VkBuffer*)q;
    const VkDeviceSize* offs = (const void*)(q + bn);
    if (x->what == 0)
        vkCmdBindTransformFeedbackBuffersEXT(x->cb, x->first, x->n, bufs, offs, (const void*)(q + bn + dn));
    else if (x->what == 1)
        vkCmdBeginTransformFeedbackEXT(x->cb, x->first, x->n, x->counters ? bufs : NULL, x->counters ? offs : NULL);
    else
        vkCmdEndTransformFeedbackEXT(x->cb, x->first, x->n, x->counters ? bufs : NULL, x->counters ? offs : NULL);
}
static void xfb_rec(VkCommandBuffer cb, int what, u32 first, u32 n, const VkBuffer* bufs, const VkDeviceSize* offs,
                    const VkDeviceSize* sizes) {
    int counters = bufs != NULL;
    u64 h = REC_AL(sizeof(XfbArgs)), bn = REC_AL(n * sizeof(VkBuffer)), dn = REC_AL(n * sizeof(VkDeviceSize));
    XfbArgs* x;
    u8* q;
    if (!rec_on(cb)) {
        if (what == 0)
            vkCmdBindTransformFeedbackBuffersEXT(cb, first, n, bufs, offs, sizes);
        else if (what == 1)
            vkCmdBeginTransformFeedbackEXT(cb, first, n, bufs, offs);
        else
            vkCmdEndTransformFeedbackEXT(cb, first, n, bufs, offs);
        return;
    }
    x = rec_push(xfb_run, h + bn + dn * 2);
    q = (u8*)x + h;
    *x = (XfbArgs){cb, first, n, what, counters};
    if (counters) {
        memcpy(q, bufs, n * sizeof *bufs);
        memcpy(q + bn, offs, n * sizeof *offs);
    }
    if (sizes) memcpy(q + bn + dn, sizes, n * sizeof *sizes);
    rec_commit();
}
void rec_CmdBindTransformFeedbackBuffersEXT(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* bufs,
                                            const VkDeviceSize* offs, const VkDeviceSize* sizes) {
    xfb_rec(cb, 0, first, n, bufs, offs, sizes);
}
void rec_CmdBeginTransformFeedbackEXT(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* counters,
                                      const VkDeviceSize* offs) {
    xfb_rec(cb, 1, first, n, counters, offs, NULL);
}
void rec_CmdEndTransformFeedbackEXT(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* counters,
                                    const VkDeviceSize* offs) {
    xfb_rec(cb, 2, first, n, counters, offs, NULL);
}

typedef struct {
    VkCommandBuffer cb;
    u32 first, n;
} ViewportArgs;
static void viewport_run(const void* p) {
    const ViewportArgs* x = p;
    const void* a = (const u8*)p + REC_AL(sizeof *x);
    if (x->first == ~0u)
        vkCmdSetViewportWithCount(x->cb, x->n, a);
    else
        vkCmdSetViewport(x->cb, x->first, x->n, a);
}
static void scissor_run(const void* p) {
    const ViewportArgs* x = p;
    const void* a = (const u8*)p + REC_AL(sizeof *x);
    if (x->first == ~0u)
        vkCmdSetScissorWithCount(x->cb, x->n, a);
    else
        vkCmdSetScissor(x->cb, x->first, x->n, a);
}
static void rec_array(RecFn fn, VkCommandBuffer cb, u32 first, u32 n, const void* a, u64 size) {
    ViewportArgs* x = rec_push(fn, REC_AL(sizeof *x) + size);
    *x = (ViewportArgs){cb, first, n};
    memcpy((u8*)x + REC_AL(sizeof *x), a, size);
    rec_commit();
}
void rec_CmdSetViewportWithCount(VkCommandBuffer cb, u32 n, const VkViewport* v) {
    if (!rec_on(cb))
        vkCmdSetViewportWithCount(cb, n, v);
    else
        rec_array(viewport_run, cb, ~0u, n, v, n * sizeof *v);
}
void rec_CmdSetViewport(VkCommandBuffer cb, u32 first, u32 n, const VkViewport* v) {
    if (!rec_on(cb))
        vkCmdSetViewport(cb, first, n, v);
    else
        rec_array(viewport_run, cb, first, n, v, n * sizeof *v);
}
void rec_CmdSetScissorWithCount(VkCommandBuffer cb, u32 n, const VkRect2D* r) {
    if (!rec_on(cb))
        vkCmdSetScissorWithCount(cb, n, r);
    else
        rec_array(scissor_run, cb, ~0u, n, r, n * sizeof *r);
}
void rec_CmdSetScissor(VkCommandBuffer cb, u32 first, u32 n, const VkRect2D* r) {
    if (!rec_on(cb))
        vkCmdSetScissor(cb, first, n, r);
    else
        rec_array(scissor_run, cb, first, n, r, n * sizeof *r);
}

/* copies, blits and clears: two handles, two layouts, a count of regions and one more scalar */
typedef struct {
    VkCommandBuffer cb;
    u64 h1, h2; /* handles */
    VkImageLayout l1, l2;
    u32 n;
    u32 e; /* a filter */
    VkClearValue v; /* a clear value */
} XferArgs;
#define XFER_REGIONS(x) ((const void*)((const u8*)(x) + REC_AL(sizeof *(x))))
static void copy_buffer_run(const void* p) {
    const XferArgs* x = p;
    vkCmdCopyBuffer(x->cb, (VkBuffer)x->h1, (VkBuffer)x->h2, x->n, XFER_REGIONS(x));
}
static void copy_image_run(const void* p) {
    const XferArgs* x = p;
    vkCmdCopyImage(x->cb, (VkImage)x->h1, x->l1, (VkImage)x->h2, x->l2, x->n, XFER_REGIONS(x));
}
static void blit_run(const void* p) {
    const XferArgs* x = p;
    vkCmdBlitImage(x->cb, (VkImage)x->h1, x->l1, (VkImage)x->h2, x->l2, x->n, XFER_REGIONS(x), (VkFilter)x->e);
}
static void buffer_to_image_run(const void* p) {
    const XferArgs* x = p;
    vkCmdCopyBufferToImage(x->cb, (VkBuffer)x->h1, (VkImage)x->h2, x->l2, x->n, XFER_REGIONS(x));
}
static void image_to_buffer_run(const void* p) {
    const XferArgs* x = p;
    vkCmdCopyImageToBuffer(x->cb, (VkImage)x->h1, x->l1, (VkBuffer)x->h2, x->n, XFER_REGIONS(x));
}
static void clear_color_run(const void* p) {
    const XferArgs* x = p;
    vkCmdClearColorImage(x->cb, (VkImage)x->h1, x->l1, &x->v.color, x->n, XFER_REGIONS(x));
}
static void clear_depth_run(const void* p) {
    const XferArgs* x = p;
    vkCmdClearDepthStencilImage(x->cb, (VkImage)x->h1, x->l1, &x->v.depthStencil, x->n,
                                XFER_REGIONS(x));
}
u64 g_rec_xfers, g_rec_barriers; /* copies and blits, and barriers, put in the frame's ring (gpuprof.c's gaps) */
u64 g_rec_xfer_kind[3], g_rec_buf_bytes; /* buffer copies, buffer-image copies, image copies and blits; buffer bytes */
static void copy_buffer_run(const void* p);
static void buffer_to_image_run(const void* p);
static void image_to_buffer_run(const void* p);
static XferArgs* rec_xfer(RecFn fn, VkCommandBuffer cb, u64 h1, VkImageLayout l1, u64 h2, VkImageLayout l2, u32 n,
                          const void* regions, u64 size) {
    g_rec_xfers++;
    if (fn == copy_buffer_run) {
        g_rec_xfer_kind[0]++;
        for (u32 i = 0; i < n; i++) g_rec_buf_bytes += ((const VkBufferCopy*)regions)[i].size;
    } else
        g_rec_xfer_kind[fn == buffer_to_image_run || fn == image_to_buffer_run ? 1 : 2]++;
    XferArgs* x = rec_push(fn, REC_AL(sizeof *x) + size);
    memset(x, 0, sizeof *x);
    x->cb = cb;
    x->h1 = h1;
    x->h2 = h2;
    x->l1 = l1;
    x->l2 = l2;
    x->n = n;
    memcpy((u8*)x + REC_AL(sizeof *x), regions, size);
    return x;
}
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, u32 n, const VkBufferCopy* r) {
    if (!rec_on(cb)) {
        vkCmdCopyBuffer(cb, src, dst, n, r);
        return;
    }
    rec_xfer(copy_buffer_run, cb, (u64)src, 0, (u64)dst, 0, n, r, n * sizeof *r);
    rec_commit();
}
void rec_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageCopy* r) {
    if (!rec_on(cb)) {
        vkCmdCopyImage(cb, src, sl, dst, dl, n, r);
        return;
    }
    rec_xfer(copy_image_run, cb, (u64)src, sl, (u64)dst, dl, n, r, n * sizeof *r);
    rec_commit();
}
void rec_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageBlit* r, VkFilter filter) {
    if (!rec_on(cb)) {
        vkCmdBlitImage(cb, src, sl, dst, dl, n, r, filter);
        return;
    }
    rec_xfer(blit_run, cb, (u64)src, sl, (u64)dst, dl, n, r, n * sizeof *r)->e = filter;
    rec_commit();
}
void rec_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, u32 n,
                              const VkBufferImageCopy* r) {
    if (!rec_on(cb)) {
        vkCmdCopyBufferToImage(cb, src, dst, dl, n, r);
        return;
    }
    rec_xfer(buffer_to_image_run, cb, (u64)src, 0, (u64)dst, dl, n, r, n * sizeof *r);
    rec_commit();
}
void rec_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, u32 n,
                              const VkBufferImageCopy* r) {
    if (!rec_on(cb)) {
        vkCmdCopyImageToBuffer(cb, src, sl, dst, n, r);
        return;
    }
    rec_xfer(image_to_buffer_run, cb, (u64)src, sl, (u64)dst, 0, n, r, n * sizeof *r);
    rec_commit();
}
void rec_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue* c, u32 n,
                            const VkImageSubresourceRange* r) {
    if (!rec_on(cb)) {
        vkCmdClearColorImage(cb, img, l, c, n, r);
        return;
    }
    rec_xfer(clear_color_run, cb, (u64)img, l, 0, 0, n, r, n * sizeof *r)->v.color = *c;
    rec_commit();
}
void rec_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue* c,
                                   u32 n, const VkImageSubresourceRange* r) {
    if (!rec_on(cb)) {
        vkCmdClearDepthStencilImage(cb, img, l, c, n, r);
        return;
    }
    rec_xfer(clear_depth_run, cb, (u64)img, l, 0, 0, n, r, n * sizeof *r)->v.depthStencil = *c;
    rec_commit();
}

typedef struct {
    VkCommandBuffer cb;
    u32 na, nr;
} ClearArgs;
static void clear_att_run(const void* p) {
    const ClearArgs* x = p;
    const u8* a = (const u8*)p + REC_AL(sizeof *x);
    vkCmdClearAttachments(x->cb, x->na, (const void*)a, x->nr,
                          (const void*)(a + REC_AL(x->na * sizeof(VkClearAttachment))));
}
void rec_CmdClearAttachments(VkCommandBuffer cb, u32 na, const VkClearAttachment* a, u32 nr, const VkClearRect* r) {
    if (!rec_on(cb)) {
        vkCmdClearAttachments(cb, na, a, nr, r);
        return;
    }
    u64 h = REC_AL(sizeof(ClearArgs)), an = REC_AL(na * sizeof *a);
    ClearArgs* x = rec_push(clear_att_run, h + an + nr * sizeof *r);
    *x = (ClearArgs){cb, na, nr};
    memcpy((u8*)x + h, a, na * sizeof *a);
    memcpy((u8*)x + h + an, r, nr * sizeof *r);
    rec_commit();
}

/* the barriers' structures have no extensions chained (pNext): a dependency or pass with one is recorded at once,
   after the ring drains */
typedef struct {
    VkCommandBuffer cb;
    VkDependencyFlags flags;
    u32 nm, nb, ni;
} BarrierArgs;
static void barrier_run(const void* p) {
    const BarrierArgs* x = p;
    const u8* q = (const u8*)p + REC_AL(sizeof *x);
    u64 mn = REC_AL(x->nm * sizeof(VkMemoryBarrier2)), bn = REC_AL(x->nb * sizeof(VkBufferMemoryBarrier2));
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                          .dependencyFlags = x->flags,
                          .memoryBarrierCount = x->nm,
                          .pMemoryBarriers = (const void*)q,
                          .bufferMemoryBarrierCount = x->nb,
                          .pBufferMemoryBarriers = (const void*)(q + mn),
                          .imageMemoryBarrierCount = x->ni,
                          .pImageMemoryBarriers = (const void*)(q + mn + bn)};
    vkCmdPipelineBarrier2(x->cb, &d);
}
static int barrier_chained(const VkDependencyInfo* d) {
    if (d->pNext) return 1;
    for (u32 i = 0; i < d->memoryBarrierCount; i++)
        if (d->pMemoryBarriers[i].pNext) return 1;
    for (u32 i = 0; i < d->bufferMemoryBarrierCount; i++)
        if (d->pBufferMemoryBarriers[i].pNext) return 1;
    for (u32 i = 0; i < d->imageMemoryBarrierCount; i++)
        if (d->pImageMemoryBarriers[i].pNext) return 1;
    return 0;
}
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) {
    if (rec_on(cb)) g_rec_barriers++;
    if (!rec_on(cb) || barrier_chained(d)) {
        if (rec_on(cb)) vk_rec_sync();
        vkCmdPipelineBarrier2(cb, d);
        return;
    }
    u64 h = REC_AL(sizeof(BarrierArgs)), mn = REC_AL(d->memoryBarrierCount * sizeof(VkMemoryBarrier2)),
        bn = REC_AL(d->bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier2));
    BarrierArgs* x = rec_push(barrier_run, h + mn + bn + d->imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier2));
    u8* q = (u8*)x + h;
    *x = (BarrierArgs){cb, d->dependencyFlags, d->memoryBarrierCount, d->bufferMemoryBarrierCount,
                       d->imageMemoryBarrierCount};
    memcpy(q, d->pMemoryBarriers, d->memoryBarrierCount * sizeof(VkMemoryBarrier2));
    memcpy(q + mn, d->pBufferMemoryBarriers, d->bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier2));
    memcpy(q + mn + bn, d->pImageMemoryBarriers, d->imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier2));
    rec_commit();
}

typedef struct {
    VkCommandBuffer cb;
    VkRenderingFlags flags;
    VkRect2D area;
    u32 layers, view_mask, n;
    int depth, stencil;
} RenderArgs;
static void rendering_run(const void* p) {
    const RenderArgs* x = p;
    const VkRenderingAttachmentInfo* a = (const void*)((const u8*)p + REC_AL(sizeof *x));
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .flags = x->flags,
                          .renderArea = x->area,
                          .layerCount = x->layers,
                          .viewMask = x->view_mask,
                          .colorAttachmentCount = x->n,
                          .pColorAttachments = a,
                          .pDepthAttachment = x->depth ? &a[x->n] : NULL,
                          .pStencilAttachment = x->stencil ? &a[x->n + 1] : NULL};
    vkCmdBeginRendering(x->cb, &ri);
}
void rec_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    int chained = ri->pNext || (ri->pDepthAttachment && ri->pDepthAttachment->pNext) ||
                  (ri->pStencilAttachment && ri->pStencilAttachment->pNext);
    for (u32 i = 0; i < ri->colorAttachmentCount && !chained; i++) chained = ri->pColorAttachments[i].pNext != NULL;
    if (!rec_on(cb) || chained) {
        if (rec_on(cb)) vk_rec_sync();
        vkCmdBeginRendering(cb, ri);
        return;
    }
    u32 n = ri->colorAttachmentCount;
    RenderArgs* x = rec_push(rendering_run, REC_AL(sizeof *x) + (n + 2) * sizeof(VkRenderingAttachmentInfo));
    VkRenderingAttachmentInfo* a = (void*)((u8*)x + REC_AL(sizeof *x));
    *x = (RenderArgs){cb, ri->flags, ri->renderArea, ri->layerCount, ri->viewMask, n, !!ri->pDepthAttachment,
                      !!ri->pStencilAttachment};
    memcpy(a, ri->pColorAttachments, n * sizeof *a);
    if (ri->pDepthAttachment) a[n] = *ri->pDepthAttachment;
    if (ri->pStencilAttachment) a[n + 1] = *ri->pStencilAttachment;
    rec_commit();
}

/* ------------------------------------------------------------ descriptor sets */
/* A draw's new descriptor set is allocated and written on the recording thread too, ahead of the commands that bind
   it: the GPU thread gets a stand-in handle at once (REC_STANDIN in the top bits, which no handle has: a driver's is a
   pointer or a small offset, and the validation layer's are counters), and the recording thread keeps which set it
   stands for until the commands binding it are recorded. The sets come from the recording thread's own pools for
   each frame slot, reset in order with the commands when the slot is reused (vk_rec_frame_reset). Stand-ins count up
   and are reused after REC_SETS, more sets than two frames ever make before vk_frame_check ends one */
#define REC_SETS        (1u << 17)
/* not merely odd: half of the validation layer's handles are odd, and a real set taken for a stand-in went unbound,
   its dispatch reading no descriptors until the GPU hung */
#define REC_STANDIN     0x5EC5000000000000ull
#define REC_SPARE_SETS  32
#define REC_SPARE_KINDS 16
static VkDescriptorSet g_rset[REC_SETS];
static u32 g_rset_next;
typedef struct {
    VkDescriptorPool pool[64];
    int npool, ipool;
    struct {
        VkDescriptorSetLayout layout;
        VkDescriptorSet set[REC_SPARE_SETS];
        u32 n;
    } spare[REC_SPARE_KINDS];
} RecPools;
static RecPools g_rpools[VK_FRAMES];
static u32 g_frame_sets[VK_FRAMES]; /* stand-ins handed out in each slot since its reset (the GPU thread's count) */

static int rec_set_standin(VkDescriptorSet s) { return ((u64)(uintptr_t)s & ~(u64)(REC_SETS - 1)) == REC_STANDIN; }
static VkDescriptorSet rec_set_real(VkDescriptorSet s) {
    return rec_set_standin(s) ? g_rset[(u64)(uintptr_t)s & (REC_SETS - 1)] : s;
}
/* count sets of layout from slot f's pools into sets: count, or 1 when a fresh pool can't hold that many, 0 when none */
static u32 rec_alloc_n(RecPools* f, VkDescriptorSetLayout layout, VkDescriptorSet* sets, u32 count) {
    VkDescriptorSetLayout layouts[REC_SPARE_SETS];
    for (u32 i = 0; i < count; i++) layouts[i] = layout;
    for (int tries = 0; tries < 3; tries++) {
        if (tries) f->ipool++;
        if (tries == 2) count = 1;
        if (f->ipool == f->npool) {
            if (f->npool == (int)(sizeof f->pool / sizeof f->pool[0])) return 0;
            if (!(f->pool[f->npool] = vk_desc_pool_new())) return 0;
            f->npool++;
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = f->pool[f->ipool],
                                          .descriptorSetCount = count,
                                          .pSetLayouts = layouts};
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, sets);
        if (r == VK_SUCCESS) return count;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            VK_OK(r);
            return 0;
        }
    }
    return 0;
}
static VkDescriptorSet rec_alloc(RecPools* f, VkDescriptorSetLayout layout) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    int i = 0;
    while (i < REC_SPARE_KINDS && f->spare[i].layout && f->spare[i].layout != layout) i++;
    if (i == REC_SPARE_KINDS) return rec_alloc_n(f, layout, &set, 1) ? set : VK_NULL_HANDLE;
    f->spare[i].layout = layout;
    if (!f->spare[i].n) f->spare[i].n = rec_alloc_n(f, layout, f->spare[i].set, REC_SPARE_SETS);
    return f->spare[i].n ? f->spare[i].set[--f->spare[i].n] : VK_NULL_HANDLE;
}
static void frame_reset_run(const void* p) {
    RecPools* f = &g_rpools[*(const u32*)p];
    for (int i = 0; i < f->npool; i++) vkResetDescriptorPool(g_vk.dev, f->pool[i], 0);
    f->ipool = 0;
    memset(f->spare, 0, sizeof f->spare);
}
void vk_rec_frame_reset(u32 slot) {
    g_frame_sets[slot] = 0;
    if (g_rec <= 0) return;
    *(u32*)rec_push(frame_reset_run, sizeof slot) = slot;
    rec_commit();
}
u32 vk_rec_frame_sets(u32 slot) { return g_frame_sets[slot]; }

static u64 write_info_size(const VkWriteDescriptorSet* w) {
    switch (w->descriptorType) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return w->descriptorCount * sizeof(VkDescriptorBufferInfo);
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return w->descriptorCount * sizeof(VkBufferView);
        default: return w->descriptorCount * sizeof(VkDescriptorImageInfo);
    }
}
static const void* write_info(const VkWriteDescriptorSet* w) {
    switch (w->descriptorType) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return w->pBufferInfo;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return w->pTexelBufferView;
        default: return w->pImageInfo;
    }
}
typedef struct {
    u32 n, slot, id;
    VkDescriptorSetLayout layout;
} NewSetArgs;
static void new_set_run(const void* p) {
    const NewSetArgs* x = p;
    /* the packet is the recording thread's now: the copied writes are pointed at their copied descriptors in place */
    VkWriteDescriptorSet* w = (VkWriteDescriptorSet*)((u8*)p + REC_AL(sizeof *x));
    u8* info = (u8*)w + REC_AL(x->n * sizeof *w);
    VkDescriptorSet set = rec_alloc(&g_rpools[x->slot], x->layout);
    g_rset[x->id] = set;
    if (!set) {
        LOG_LIMIT(1, "vulkan: a frame used all its descriptor pools (draws are drawn without their resources)\n");
        return;
    }
    for (u32 i = 0; i < x->n; i++) {
        w[i].dstSet = set;
        w[i].pBufferInfo = (const void*)info;
        w[i].pImageInfo = (const void*)info;
        w[i].pTexelBufferView = (const void*)info;
        info += REC_AL(write_info_size(&w[i]));
    }
    if (x->n) vkUpdateDescriptorSets(g_vk.dev, x->n, w, 0, NULL);
}
VkDescriptorSet vk_rec_new_set(VkDescriptorSetLayout layout, u32 n, const VkWriteDescriptorSet* w) {
    u64 size = REC_AL(sizeof(NewSetArgs)) + REC_AL(n * sizeof *w);
    int chained = 0;
    /* a slot's pools are reset when its frame starts, in the first vk_cmd (frame_recycle): before the frame's first
       set is made from them, not after it, which freed the set its draw then bound */
    vk_cmd();
    u32 slot = vk_frame_slot();
    for (u32 i = 0; i < n; i++) {
        size += REC_AL(write_info_size(&w[i]));
        chained |= w[i].pNext != NULL;
    }
    if (g_rec <= 0 || chained) { /* allocated and written here, from the frame's own pools */
        VkDescriptorSet set = vk_desc_alloc(layout);
        VkWriteDescriptorSet c[SNX_VKB_END];
        if (!set || n > SNX_VKB_END) return VK_NULL_HANDLE;
        memcpy(c, w, n * sizeof *w);
        for (u32 i = 0; i < n; i++) c[i].dstSet = set;
        vk_rec_sync();
        if (n) vkUpdateDescriptorSets(g_vk.dev, n, c, 0, NULL);
        return set;
    }
    NewSetArgs* x = rec_push(new_set_run, size);
    VkWriteDescriptorSet* c = (VkWriteDescriptorSet*)((u8*)x + REC_AL(sizeof *x));
    u8* info = (u8*)c + REC_AL(n * sizeof *c);
    u32 id = g_rset_next++ & (REC_SETS - 1);
    *x = (NewSetArgs){n, slot, id, layout};
    memcpy(c, w, n * sizeof *w);
    for (u32 i = 0; i < n; i++) {
        u64 s = write_info_size(&w[i]);
        memcpy(info, write_info(&w[i]), s);
        info += REC_AL(s);
    }
    g_frame_sets[slot]++;
    rec_commit();
    return (VkDescriptorSet)(uintptr_t)(REC_STANDIN | id);
}
