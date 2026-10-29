/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Compute: the compute class's launches (QMD), on a compute queue of their own when the device has one (a queue of a
   compute-only family, or a second queue of the graphics family), else on the graphics queue. Each dispatch gets
   copies of its constant and storage buffers in a host-visible arena and is recorded into a batch. The batch is
   submitted and its results land in guest memory before anything could observe them: a read of that memory
   (gmem_read and gmem_ptr call cs_mem_need), anything after it in the stream the game can observe (semaphore releases
   and reports: gpu_signal_point), a GPU-side write of that memory, a syncpoint increment or a wait. A byte is written
   only where the shader changed it and guest memory still holds what the shader read, since on the console the GPU
   stores while it runs and a later CPU write wins.

   A queue of its own only waits for compute, so holding signals until results land costs a dispatch's run time,
   not the rendering backlog's (which made that ordering too slow under the old OpenGL renderer, ROADMAP.md Phase 2).
   Dispatches that read textures run on the graphics queue, after everything recorded before them. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/maxwell/kepler_compute_regs.h"
#include "nx/gpu/shader/shader.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/backing.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/memory/own.h"

typedef struct CsPipe {
    u64 key;
    VkPipeline p;
    ShaderInfo si;
    VkCbReads cbr; /* the constant buffer words translation read */
    int failed;
    struct CsPipe* next;
    struct CsPipe* keyed; /* the program translated for the numeric types of the images last bound to it */
} CsPipe;
static CsPipe* g_csp[256];

/* the arena: inputs and outputs of the batch's dispatches, and what the outputs held before (org, host only) */
static VkBuffer g_ar;
static VmaAllocation g_ar_mem;
static u8* g_ar_ptr;
static u8* g_org;
static u64 g_ar_cap, g_ar_used;
typedef struct {
    u64 addr;
    u64 off;
    u32 size;
} CsOut;
static CsOut g_out[256];
static int g_nout;
/* the guest pages every pending result covers, the current batch's and those put aside: a read or write of memory no
   result covers (most of them) is answered without checking each result, through the GMMU page by page */
static PendPages g_pages = {.also = &g_pend_any};
static void outs_pages(const CsOut* out, int n, int delta) {
    for (int i = 0; i < n; i++) pend_add(&g_pages, out[i].addr, out[i].size, delta);
}

static VkCommandPool g_cpool;
static VkCommandBuffer g_ccb;
static VkDescriptorPool g_dpool;
static int g_recording, g_landing;
static int g_inflight; /* submitted and waited for in cs_land, results not landed yet */
/* Submitted batches whose results haven't landed, oldest first. A batch is submitted when it is full, or when a
   release, a draw or a reader needs its results, and lands when something needs them: the release thread once the
   batch is done, or a reader now. Landing each at its submit (with a fence wait) was 4 waits a frame in one game. Each
   keeps its command buffer, descriptor pool, arena and original bytes until it lands; the current batch (the globals
   above) records into others meanwhile. */
typedef struct {
    VkCommandBuffer cb;
    VkDescriptorPool dpool;
    VkBuffer ar;
    VmaAllocation ar_mem;
    u8* ar_ptr;
    u8* org;
    u64 ar_cap, org_cap, cval;
    CsOut out[256];
    int nout, gfx;
#if defined(SNX_TRACY)
    VkQueryPool qpool; /* the batch's span on the GPU track, its two timestamps read back when it lands */
    u16 q0, q1;
    u64 ep; /* the connection the begin went out on: an end or a time from another one is dropped */
#endif
} CsBatch;
#define CS_PENDING 3
static CsBatch g_pend[CS_PENDING];
static u32 g_phead, g_ptail; /* batches put aside, landed */
/* draws can read results on the GPU before they land (vk_compute_gpu_src): the batch signals g_vk_csem, the next
   graphics submit waits for g_vk_cs_wait_value, and the arena they read stays alive until that frame is done */
VkSemaphore g_vk_csem;
u64 g_vk_cs_wait_value;
static u64 g_cval;
static int g_ar_gfx;
static u64 g_org_cap;
u64 g_vk_dispatches, g_vk_cs_batches, g_vk_cs_wait_ns;
#if defined(SNX_TRACY)
u64 g_vk_cs_cpu_reads; /* dispatches kept on the compute queue because the CPU reads what they write */
#endif
/* the dispatches skipped, by why: most reasons are logged once, so the counts show which one a game's dispatches keep
   hitting (a program that wasn't translated is skipped every time it is dispatched) */
enum {
    CS_SKIP_LIMITS,
    CS_SKIP_UNTRANSLATED,
    CS_SKIP_FEATURE,
    CS_SKIP_TEXTURE,
    CS_SKIP_STORAGE_IMAGE,
    CS_SKIP_BUFFER_IMAGE,
    CS_SKIP_STORAGE_BUFFER,
    CS_SKIP_RESOURCES,
    CS_SKIP_INPUTS_MOVED,
    CS_SKIP_COUNT
};
static u64 g_cs_skips[CS_SKIP_COUNT];
static const char* const g_cs_skip_names[CS_SKIP_COUNT] = {
    "past the GPU's limits",       "not translated",
    "missing a device feature",    "a texture not bound",
    "a storage image not bound",   "a buffer image not bound",
    "a storage buffer not read",   "out of descriptors or staging",
    "inputs changed while binding"};
u64 vk_cs_skipped(char* why, size_t size) {
    u64 total = 0;
    size_t at = 0;
    int i;
    if (size) why[0] = 0;
    for (i = 0; i < CS_SKIP_COUNT; i++) {
        int n;
        if (!g_cs_skips[i]) continue;
        total += g_cs_skips[i];
        if (at >= size) continue;
        n = snprintf(why + at, size - at, "%s%llu %s", at ? ", " : " (", (unsigned long long)g_cs_skips[i],
                     g_cs_skip_names[i]);
        if (n > 0) at += (size_t)n;
    }
    if (at && at + 1 < size) {
        why[at] = ')';
        why[at + 1] = 0;
    }
    return total;
}

static u8 g_cb[QMD_NUM_CONST_BUFFERS][MAX_CONST_BUFFER_BYTES];
static u32 g_cbn[QMD_NUM_CONST_BUFFERS];
static int cs_cb_read(u32 idx, u32 off, u32* out) {
    if (idx < QMD_NUM_CONST_BUFFERS && off + 4 <= g_cbn[idx]) {
        *out = snx_ld_le32(g_cb[idx] + off); /* g_cb keeps the guest's bytes, for the uniform buffers */
        return 1;
    }
    return 0;
}

static void cs_init(void) {
    VkDescriptorPoolSize sz[VK_POOL_TYPES];
    VkDescriptorPoolCreateInfo dci = {0};
    if (g_cpool) return;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g_vk.compute_family};
    VK_OK(vkCreateCommandPool(g_vk.dev, &pci, NULL, &g_cpool));
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = g_cpool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &g_ccb));
    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_vk_csem));
    /* the dispatches of a batch: a batch that runs out lands, which resets the pool */
    vk_pool_sizes(sz, 1024);
    dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dci.maxSets = 1024;
    dci.poolSizeCount = VK_POOL_TYPES;
    dci.pPoolSizes = sz;
    VK_OK(vkCreateDescriptorPool(g_vk.dev, &dci, NULL, &g_dpool));
}

/* a bigger arena; only between batches, nothing points into it then */
static u64 g_ar_want = 8ull << 20; /* doubles when a batch fills the arena, up to 64 MB */
/* arenas draws read from: reused once the frames that read them are done */
static struct {
    VkBuffer b;
    VmaAllocation m;
    u8* p;
    u64 cap, frame;
} g_retired[8];
static int g_nretired;
static void arena_retire(void) {
    if (!g_ar) return;
    if (g_nretired == 8) { /* full: the oldest goes for good */
        vk_defer_buffer(g_retired[0].b, g_retired[0].m);
        memmove(g_retired, g_retired + 1, sizeof g_retired[0] * 7);
        g_nretired--;
    }
    g_retired[g_nretired++] = (typeof(g_retired[0])){g_ar, g_ar_mem, g_ar_ptr, g_ar_cap, vk_frame_id()};
    g_ar = VK_NULL_HANDLE;
}
/* the original bytes beside the arena, as big as it: batches put aside swap theirs with the current one's */
static int org_fit(void) {
    if (g_org_cap >= g_ar_cap) return 1;
    free(g_org);
    g_org = malloc(g_ar_cap);
    g_org_cap = g_org ? g_ar_cap : 0;
    return g_org != NULL;
}
static int arena_fit(u64 need) {
    if (g_ar && g_ar_used <= g_ar_cap && need <= g_ar_cap - g_ar_used && g_ar_cap >= g_ar_want) return org_fit();
    vk_cmd(); /* frames older than VK_FRAMES are done once the current one has started */
    for (int i = 0; i < g_nretired; i++)
        if (g_retired[i].frame + VK_FRAMES <= vk_frame_id() && g_retired[i].cap >= need &&
            g_retired[i].cap >= g_ar_want) {
            if (g_ar) vk_defer_buffer(g_ar, g_ar_mem);
            g_ar = g_retired[i].b;
            g_ar_mem = g_retired[i].m;
            g_ar_ptr = g_retired[i].p;
            g_ar_cap = g_retired[i].cap;
            g_retired[i] = g_retired[--g_nretired];
            return org_fit();
        }
    if (g_ar) vk_defer_buffer(g_ar, g_ar_mem); /* draws may still read it */
    u64 cap = g_ar_want;
    while (cap < need) cap *= 2;
    u32 fam[2] = {g_vk.gfx_family, g_vk.compute_family};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = cap,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
                              /* both queues use it, one at a time, without ownership transfers */
                              .sharingMode = fam[0] != fam[1] ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
                              .queueFamilyIndexCount = fam[0] != fam[1] ? 2 : 0,
                              .pQueueFamilyIndices = fam};
    VmaAllocationCreateInfo aci = {.flags =
                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo info;
    g_ar = VK_NULL_HANDLE;
    g_ar_cap = 0;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &aci, &g_ar, &g_ar_mem, &info))) return 0;
    vk_mem_track(g_ar_mem);
    g_ar_ptr = info.pMappedData;
    g_ar_cap = cap;
    return org_fit();
}
static u64 arena_alloc(u64 size) {
    u64 al = vk_stage_align(), at = (g_ar_used + al - 1) & ~(al - 1);
    g_ar_used = at + size;
    return at;
}

/* submits the batch without waiting; it signals g_vk_csem with the next value */
#if defined(SNX_TRACY)
/* the batch's span on the GPU track (VK_GPU_CTX_COMPUTE): a timestamp at each end of its command buffer, with its
   own two-query pool, so a landed batch's results survive the batches recorded after it */
static VkQueryPool g_cqpool;
static u16 g_cq0, g_cq1;
static u64 g_cq_ep;        /* the connection the batch's begin went out on */
static i64 g_cs_cal_prev;  /* the client's clock at the track's last calibration pair */
static u64 g_cs_cal_conn;  /* the connection that baseline was taken on */
static int g_cs_cal_ok;    /* the context was announced calibrated, so calibration events are for it */
/* the landed batch's timestamps onto the track: the context announced at its first read-back (with the calibration
   clock's pair for its anchor when it has one), then calibrated or anchored, then the pair of timestamps its zone
   went out with */
static void cs_times(VkQueryPool pool, u16 q0, u16 q1, u64 ep) {
    i64 ts2[2];
    if (!pool) return;
    if (!VK_OK(vkGetQueryPoolResults(g_vk.dev, pool, 0, 2, sizeof ts2, ts2, 8, VK_QUERY_RESULT_64_BIT))) return;
    if (!snx_tracy_attached()) return;
    int cal = vk_gpuprof_calibrated();
    i64 cpu = 0, gpu = 0;
    int have = cal && vk_gpuprof_clock(&cpu, &gpu);
    u64 conn = snx_tracy_connection_id();
    if (conn != g_cs_cal_conn) { /* a new connection's server re-anchored at the replayed context */
        g_cs_cal_conn = conn;
        g_cs_cal_prev = 0;
        g_cs_cal_ok = 0; /* its announcement sets this again, as the client replays the context */
    }
    /* A context is announced calibrated only with a pair in hand: without one the server has no anchor and
       applies no calibration to the times sent after it, which shows as times far from the CPU's. The flag
       latches at the announcement, so a context announced without one is never sent calibration events: the
       server would assert on a context it was told is uncalibrated. */
    if (snx_tracy_gpu_context(VK_GPU_CTX_COMPUTE, have ? gpu : ts2[1], g_vk.props.limits.timestampPeriod, have,
                              "compute")) {
        g_cs_cal_prev = have ? cpu : 0; /* the anchor is the announcement's own pair */
        g_cs_cal_ok = have;
    }
    if (have) {
        if (g_cs_cal_ok && g_cs_cal_prev) {
            i64 delta = cpu - g_cs_cal_prev;
            if (delta > 0) snx_tracy_gpu_calibrate(gpu, delta, VK_GPU_CTX_COMPUTE);
        }
        g_cs_cal_prev = cpu;
    } else
        snx_tracy_gpu_sync(ts2[1], VK_GPU_CTX_COMPUTE);
    if (q0 && q1) {
        snx_tracy_gpu_time(ts2[0], q0, VK_GPU_CTX_COMPUTE, ep);
        snx_tracy_gpu_time(ts2[1], q1, VK_GPU_CTX_COMPUTE, ep);
    }
}
#endif

static void cs_submit(void) {
    if (!g_recording) return;
    vmaFlushAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE); /* the arena may not be host coherent */
#if defined(SNX_TRACY)
    if (g_cqpool && g_cq0) { /* a begin recorded at the buffer's begin: its end, and the timestamp to pair */
        rec_CmdWriteTimestamp2(g_ccb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g_cqpool, 1);
        /* the end goes out whenever the begin did: a dropped pair never reaches the server apart */
        g_cq1 = snx_tracy_gpu_zone_end(g_cq0, VK_GPU_CTX_COMPUTE, g_cq_ep);
    }
#endif
    VK_OK(vkEndCommandBuffer(g_ccb));
    VkCommandBufferSubmitInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = g_ccb};
    VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                 .semaphore = g_vk_csem,
                                 .value = ++g_cval,
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .commandBufferInfoCount = 1,
                        .pCommandBufferInfos = &c,
                        .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &sig};
    vk_rec_queue(g_vk.compute); /* after the graphics submissions before it, when the queue is theirs too */
    vk_wait_foreground();
    VkResult r = vkQueueSubmit2(g_vk.compute, 1, &si, VK_NULL_HANDLE);
    if (r == VK_ERROR_DEVICE_LOST) fatal("vulkan: the GPU device was lost (driver reset or crash)");
    g_recording = 0;
    g_inflight = 1;
    if (!VK_OK(r)) { /* never signaled by the GPU: the host signals it, so waits for it end */
        VkSemaphoreSignalInfo ss = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = g_vk_csem, .value = g_cval};
        VK_OK(vkSignalSemaphore(g_vk.dev, &ss));
    }
}
static void cs_wait(u64 value) {
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                              .semaphoreCount = 1,
                              .pSemaphores = &g_vk_csem,
                              .pValues = &value};
    u64 t0 = plat_ns();
    SNX_ZONE_C(z, "vk compute wait", 0xb05252);
    SNX_ZONE_VALUE(z, value);
    gpu_wait_note("a compute batch", value);
    VK_OK(vkWaitSemaphores(g_vk.dev, &wi, UINT64_MAX));
    gpu_wait_note(NULL, 0);
    __atomic_add_fetch(&g_vk_cs_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
}
/* the arena's results that changed, into guest memory, after its batch is done */
static void cs_write_back(VmaAllocation mem, const u8* ar, const u8* org, const CsOut* out, int nout) {
    vmaInvalidateAllocation(g_vk.vma, mem, 0, VK_WHOLE_SIZE);
    for (int i = 0; i < nout; i++) vk_write_changed(out[i].addr, ar + out[i].off, org + out[i].off, out[i].size);
}
/* an arena whose batch has landed: draws that read it may still run (gfx), so it waits for their frame like the
   arenas draws read; one they didn't read is free now */
static void cs_arena_done(VkBuffer b, VmaAllocation m, u8* p, u64 cap, int gfx) {
    if (g_nretired == 8) { /* full: the oldest goes for good */
        vk_defer_buffer(g_retired[0].b, g_retired[0].m);
        memmove(g_retired, g_retired + 1, sizeof g_retired[0] * 7);
        g_nretired--;
    }
    g_retired[g_nretired++] = (typeof(g_retired[0])){
        b, m, p, cap, gfx || vk_frame_id() < VK_FRAMES ? vk_frame_id() : vk_frame_id() - VK_FRAMES};
}
static void cs_any_update(void) {
    int any = g_nout > 0;
    for (u32 i = g_ptail; i != g_phead && !any; i++) any = g_pend[i % CS_PENDING].nout > 0;
    g_cs_any = any;
    if (!any && !pend_empty(&g_pages)) /* a result lost pages through other mappings than it gained them through */
        WARN_LIMIT(1, "vulkan: compute results landed, but their guest pages are still counted\n");
}
/* the oldest batch put aside lands: it is done once g_vk_csem reaches its value */
static void cs_land_oldest(void) {
    CsBatch* b = &g_pend[g_ptail % CS_PENDING];
    int was = g_landing;
    cs_wait(b->cval);
    g_landing = 1;
#if defined(SNX_TRACY)
    cs_times(b->qpool, b->q0, b->q1, b->ep);
#endif
    cs_write_back(b->ar_mem, b->ar_ptr, b->org, b->out, b->nout);
    g_landing = was;
    outs_pages(b->out, b->nout, -1);
    vkResetDescriptorPool(g_vk.dev, b->dpool, 0);
    cs_arena_done(b->ar, b->ar_mem, b->ar_ptr, b->ar_cap, b->gfx);
    b->ar = VK_NULL_HANDLE;
    b->nout = 0;
    g_ptail++;
    g_vk_cs_batches++;
    cs_any_update();
}
/* submits the current batch and puts it aside to land later; the next batch records into the slot's command buffer
   and pool and a fresh arena */
static void cs_put_aside(void) {
    CsBatch* b;
    VkCommandBuffer cb;
    VkDescriptorPool dp;
    u8* org;
    u64 org_cap;
#if defined(SNX_TRACY)
    VkQueryPool qp;
#endif
    if (!g_recording) return;
    SNX_ZONE(z, "vk compute put aside");
    cs_submit();
    if (g_phead - g_ptail == CS_PENDING) cs_land_oldest();
    b = &g_pend[g_phead % CS_PENDING];
    if (!b->cb) { /* first use of the slot: its own command buffer and pool for the batch that follows */
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = g_cpool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        VkDescriptorPoolSize sz[VK_POOL_TYPES];
        VkDescriptorPoolCreateInfo dci = {0};
        VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &b->cb));
#if defined(SNX_TRACY)
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
        VK_OK(vkCreateQueryPool(g_vk.dev, &qi, NULL, &b->qpool));
#endif
        vk_pool_sizes(sz, 1024);
        dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dci.maxSets = 1024;
        dci.poolSizeCount = VK_POOL_TYPES;
        dci.pPoolSizes = sz;
        VK_OK(vkCreateDescriptorPool(g_vk.dev, &dci, NULL, &b->dpool));
    }
    cb = b->cb;
    dp = b->dpool;
    org = b->org;
    org_cap = b->org_cap;
#if defined(SNX_TRACY)
    qp = b->qpool;
#endif
    b->cb = g_ccb;
    b->dpool = g_dpool;
    b->ar = g_ar;
    b->ar_mem = g_ar_mem;
    b->ar_ptr = g_ar_ptr;
    b->ar_cap = g_ar_cap;
    b->org = g_org;
    b->org_cap = g_org_cap;
    b->cval = g_cval;
    b->gfx = g_ar_gfx;
#if defined(SNX_TRACY)
    /* the batch's track pool and zone ids swap with its command buffer: the next batch records into this slot's */
    b->qpool = g_cqpool;
    b->q0 = g_cq0;
    b->q1 = g_cq1;
    b->ep = g_cq0 ? g_cq_ep : 0;
    g_cq0 = g_cq1 = 0;
    g_cqpool = qp;
#endif
    memcpy(b->out, g_out, (size_t)g_nout * sizeof g_out[0]);
    b->nout = g_nout;
    g_phead++;
    g_ccb = cb;
    g_dpool = dp;
    g_org = org;
    g_org_cap = org_cap;
    g_ar = VK_NULL_HANDLE; /* arena_fit gives the next batch one */
    g_ar_cap = g_ar_used = 0;
    g_nout = 0;
    g_ar_gfx = 0;
    g_inflight = 0;
    cs_any_update();
}
/* every batch lands, the current one submitted and waited for */
static void cs_land(void) {
    if (g_landing) return;
    SNX_ZONE(z, "vk compute land");
    while (g_ptail != g_phead) cs_land_oldest();
    if (!g_recording && !g_inflight) return;
    g_landing = 1;
    cs_submit();
    cs_wait(g_cval);
    g_inflight = 0;
#if defined(SNX_TRACY)
    cs_times(g_cqpool, g_cq0, g_cq1, g_cq0 ? g_cq_ep : 0);
    g_cq0 = g_cq1 = 0;
#endif
    cs_write_back(g_ar_mem, g_ar_ptr, g_org, g_out, g_nout);
    outs_pages(g_out, g_nout, -1);
    /* only now: until the results are in memory, readers on other threads wait for this landing in
       vk_compute_mem_need */
    g_nout = 0;
    g_ar_used = 0;
    vkResetDescriptorPool(g_vk.dev, g_dpool, 0);
    if (g_ar_gfx) { /* draws read this arena: it waits until their frame is done, the next batch gets another */
        arena_retire();
        g_ar_gfx = 0;
    }
    g_vk_cs_batches++;
    g_landing = 0;
    cs_any_update();
}

/* A descriptor can fail after its inputs were copied into the arena. No command references those inputs yet. */
static void cs_abandon_binding(u8* originals[8]) {
    vk_buf_writable_abort(); /* a deferred dispatch's outputs, not recorded */
    for (int i = 0; i < 8; i++) free(originals[i]);
    if (g_recording || g_inflight) return;
    g_ar_used = 0;
    vkResetDescriptorPool(g_vk.dev, g_dpool, 0);
}
/* a draw reads [gva, gva + len), which one pending result covers: it reads it from the batch's arena, after the
   batch. Any other overlap lands the results now, so the caller's caches see them in memory */
int vk_compute_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    u32 k;
    if (!g_cs_any || g_landing || !pend_hit(&g_pages, gva, len)) return 0;
    for (int i = 0; i < g_nout; i++) {
        CsOut* o = &g_out[i];
        if (gva >= o->addr + o->size || o->addr >= gva + len) {
            if (vk_backing_overlap(gva, len, o->addr, o->size)) cs_land();
            else continue;
            return 0;
        }
        /* partly covered, or at an offset that suits no use of it (index, storage and uniform buffers): it lands */
        if (gva < o->addr || gva + len > o->addr + o->size || ((o->off + (gva - o->addr)) & (vk_stage_align() - 1))) {
            cs_land();
            return 0;
        }
        g_ar_gfx = 1;
        cs_put_aside();
        o = &g_pend[(g_phead - 1) % CS_PENDING].out[i];
        g_vk_cs_wait_value = g_cval;
        *buf = g_pend[(g_phead - 1) % CS_PENDING].ar;
        *off = o->off + (gva - o->addr);
        return 1;
    }
    for (k = g_ptail; k != g_phead; k++) {
        CsBatch* b = &g_pend[k % CS_PENDING];
        for (int i = 0; i < b->nout; i++) {
            CsOut* o = &b->out[i];
            if (gva >= o->addr + o->size || o->addr >= gva + len) {
                if (vk_backing_overlap(gva, len, o->addr, o->size)) cs_land();
                else continue;
                return 0;
            }
            if (gva < o->addr || gva + len > o->addr + o->size ||
                ((o->off + (gva - o->addr)) & (vk_stage_align() - 1))) {
                cs_land();
                return 0;
            }
            b->gfx = 1;
            if (g_vk_cs_wait_value < b->cval) g_vk_cs_wait_value = b->cval;
            *buf = b->ar;
            *off = o->off + (gva - o->addr);
            return 1;
        }
    }
    return 0;
}
void vk_compute_land(void) {
    vk_lock();
    cs_land();
    vk_unlock();
}
u64 vk_compute_put_aside(void) {
    u64 gate = 0;
    vk_lock();
    if (g_nout) cs_put_aside();
    if (g_ptail != g_phead) gate = g_pend[(g_phead - 1) % CS_PENDING].cval;
    vk_unlock();
    return gate;
}
void vk_compute_wait(u64 value) {
    if (value) cs_wait(value);
}
void vk_compute_progress(u64* submitted, u64* done) {
    *submitted = __atomic_load_n(&g_cval, __ATOMIC_RELAXED);
    *done = 0;
    if (g_vk_csem) VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, g_vk_csem, done));
}
void vk_compute_land_through(u64 value) {
    vk_lock();
    while (g_ptail != g_phead && g_pend[g_ptail % CS_PENDING].cval <= value) cs_land_oldest();
    vk_unlock();
}
/* the front end writes guest memory (size > 0) or reaches a point the game can observe (size 0) */
void vk_compute_before_write(u64 gva, u64 size) {
    if (!size)
        vk_compute_land();
    else if (g_cs_any)
        vk_compute_mem_need(gva, size);
}
/* a read of guest memory: results for it have to be there first. Called from any thread that reads guest memory while
   g_cs_any is set: the pending results are looked at under vk_lock, which recording and landing them hold, so a
   reader waits for a landing in progress on another thread. The landing thread itself passes (g_landing). A result of
   a batch put aside lands that batch and those before it */
static void cs_mem_need(u64 a, u64 n) {
    u32 k, last = g_ptail;
    int found = 0;
    if (!g_landing) {
        for (k = g_ptail; k != g_phead; k++) {
            const CsBatch* b = &g_pend[k % CS_PENDING];
            for (int i = 0; i < b->nout && !(found && last == k + 1); i++)
                if (vk_backing_overlap(a, n, b->out[i].addr, b->out[i].size)) {
                    found = 1;
                    last = k + 1;
                }
        }
        while (found && g_ptail != last) cs_land_oldest();
        for (int i = 0; i < g_nout; i++)
                if (vk_backing_overlap(a, n, g_out[i].addr, g_out[i].size)) {
                    cs_land();
                    break;
                }
    }
}
void vk_compute_mem_need(u64 a, u64 n) {
    vk_lock();
    if (pend_hit(&g_pages, a, n)) cs_mem_need(a, n);
    vk_unlock();
}
u64 vk_compute_adds(void) { return g_pages.adds; }
int vk_compute_pending(u64 a, u64 n) {
    int hit;
    vk_lock();
    hit = pend_hit(&g_pages, a, n);
    vk_unlock();
    return hit;
}
/* the GMMU is about to change [a, a + n): every result there lands with the mappings it covered its pages through,
   including one that overlaps it only on GPU pages that weren't mapped, which covered no page */
void vk_compute_mem_need_map(u64 a, u64 n) {
    vk_lock();
    cs_mem_need(a, n);
    vk_unlock();
}

/* the pipeline of a program (80 zero bytes, then the code), by its code and launch parameters, and by the constant
   buffer words its translation read */
/* A program is translated for the numeric types of its sampled and storage images. Storage stores use
   rt_int_mask bits 8 and up to switch the bound/bindless default between float and integer, and rt_sint_mask's
   corresponding bits to select signed stores. Atomics retain their R32ui declaration. */
typedef struct {
    u64 tex_int, tex_sint;
    u32 image, image_sint;
} CsKey;
static CsPipe g_cs_alloc_failed = {.failed = 1}; /* out of memory: not cached, retried */
static CsPipe* cs_pipe(const u8* code, u32 lx, u32 ly, u32 lz, u32 shm, CsKey k) {
    u32 blen;
    u64 key = vk_shader_hash(code, 0x10000 + 80, &blen, cs_cb_read) ^ ((u64)lx << 40) ^ ((u64)ly << 48) ^
              ((u64)lz << 56) ^ ((u64)shm << 20) ^ ((u64)k.image << 32) ^ (u64)k.tex_int * 0x9E3779B97F4A7C15ull ^
              (u64)k.tex_sint * 0xC2B2AE3D27D4EB4Full ^ (u64)k.image_sint * 0xA24BAED4963EE407ull;
    CsPipe* p = g_csp[key & 255];
    while (p && (p->key != key || !vk_cb_reads_hold(&p->cbr, cs_cb_read))) p = p->next;
    if (p) return p;
    p = calloc(1, sizeof *p);
    if (!p) return &g_cs_alloc_failed;
    p->key = key;
    p->next = g_csp[key & 255];
    g_csp[key & 255] = p;
    p->si.cs_local[0] = lx;
    p->si.cs_local[1] = ly;
    p->si.cs_local[2] = lz;
    p->si.shared_size = (int)shm;
    p->si.rt_int_mask = k.image << 8;
    p->si.rt_sint_mask = k.image_sint << 8;
    p->si.tex_int_mask = k.tex_int;
    p->si.tex_sint_mask = k.tex_sint;
    const char* corpus = vk_corpus_dir();
    if (corpus) shader_corpus_save(corpus, code, blen, 0x10000 + 80, ST_CS, &p->si, key);
    size_t nw = 0;
    u32* spv = vk_translate_spirv(code, 0x10000 + 80, ST_CS, &p->si, &nw, cs_cb_read, &p->cbr);
    if (!spv || p->si.unsupported) {
        p->failed = 1;
        LOG("vulkan: compute %016llx not translated: %s\n", (unsigned long long)key,
            p->si.first_unsupported[0] ? p->si.first_unsupported : "unsupported feature or shader");
        return p;
    }
    u32 image_slots = g_vk.props.limits.maxPerStageDescriptorStorageImages;
    if (image_slots > g_vk.props.limits.maxDescriptorSetStorageImages)
        image_slots = g_vk.props.limits.maxDescriptorSetStorageImages;
    /* compared unsigned: a driver with no limit reports 0xFFFFFFFF, which as an int is -1 and refused every program */
    if ((u32)p->si.nimg > image_slots) {
        LOG_LIMIT(4, "vulkan: compute image count exceeds this GPU's descriptor limit (skipped)\n");
        free(spv);
        p->failed = 1;
        return p;
    }
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
    VkShaderModule m;
    int ok = VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m));
    free(spv);
    if (!ok) {
        p->failed = 1;
        return p;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m,
                                                .pName = "main"},
                                      .layout = g_vk_playout};
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &p->p))) p->failed = 1;
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (!p->failed)
        LOG_LIMIT(12, "vulkan: compute %016llx ready: block %ux%ux%u, %d global buffers, %d textures, %d images\n",
                  (unsigned long long)key, lx, ly, lz, p->si.ngbuf, p->si.ntex, p->si.nimg);
    return p;
}

/* A buffer image: the texels a buffer header describes, bound as a storage texel buffer over their copy in the
   arena. Raw loads and atomics view it as R32_UINT, as the console reads it for them whatever its format. Formatted
   stores are typeless in the shader, unsigned for bindless images: an integer format is viewed as itself, a 32-bit
   float one as the integer format of the same bits, which the shader's stores of register bits match. */
typedef struct {
    u64 gva;
    u32 bytes;
    VkFormat fmt;
} CsBufImg;
static int cs_buffer_image(const u32* tic, int kind, CsBufImg* out) {
    u32 hv = (tic[2] >> 21) & 7, type = (tic[4] >> 23) & 0xF;
    u64 gva;
    u64 texels;
    VkFmt f;
    VkFormat fmt;
    u64 bytes;
    u32 element;
    VkFormatProperties fp;
    VkFormatFeatureFlags want;
    if (hv != 0 || type != 6) return 0;
    gva = ((u64)(tic[2] & 0xFFFF) << 32) | tic[1];
    texels = (((u64)(tic[3] & 0xFFFF) << 16) | (tic[4] & 0xFFFF)) + 1;
    f = vk_tic_format(tic[0], 0);
    if (!gva || !f.bpp || f.bw != 1 || !f.raw || f.depth) return 0;
    fmt = f.vk;
    if (kind == 1)
        fmt = VK_FORMAT_R32_UINT;
    else if (kind == 2 && !f.is_int)
        fmt = fmt == VK_FORMAT_R32_SFLOAT            ? VK_FORMAT_R32_UINT
              : fmt == VK_FORMAT_R32G32_SFLOAT       ? VK_FORMAT_R32G32_UINT
              : fmt == VK_FORMAT_R32G32B32A32_SFLOAT ? VK_FORMAT_R32G32B32A32_UINT
                                                     : VK_FORMAT_UNDEFINED;
    else if (kind == 2 && f.sint) /* the shader's image is unsigned: the same bits viewed as such */
        fmt = fmt == VK_FORMAT_R32_SINT            ? VK_FORMAT_R32_UINT
              : fmt == VK_FORMAT_R32G32_SINT       ? VK_FORMAT_R32G32_UINT
              : fmt == VK_FORMAT_R32G32B32A32_SINT ? VK_FORMAT_R32G32B32A32_UINT
              : fmt == VK_FORMAT_R16G16B16A16_SINT ? VK_FORMAT_R16G16B16A16_UINT
              : fmt == VK_FORMAT_R16G16_SINT       ? VK_FORMAT_R16G16_UINT
              : fmt == VK_FORMAT_R16_SINT          ? VK_FORMAT_R16_UINT
              : fmt == VK_FORMAT_R8G8B8A8_SINT     ? VK_FORMAT_R8G8B8A8_UINT
              : fmt == VK_FORMAT_R8G8_SINT         ? VK_FORMAT_R8G8_UINT
              : fmt == VK_FORMAT_R8_SINT           ? VK_FORMAT_R8_UINT
                                                   : VK_FORMAT_UNDEFINED;
    else if (kind == 0 && f.is_int)
        fmt = VK_FORMAT_UNDEFINED;
    if (fmt == VK_FORMAT_UNDEFINED) return 0;
    bytes = texels * f.bpp;
    element = kind == 1 ? 4 : f.bpp;
    if (bytes > (64ull << 20) || bytes / element > g_vk.props.limits.maxTexelBufferElements) return 0;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, fmt, &fp);
    want = VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT |
           (kind == 1 ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT : 0);
    if ((fp.bufferFeatures & want) != want) return 0;
    out->gva = gva;
    out->bytes = (u32)(bytes / element * element);
    out->fmt = fmt;
    return 1;
}

/* Both the initial translation and format specialization use the same bounded program. */
static void cs_read_code(u64 pa, u8* dst) {
    u32 code_size = 0x10000;
    /* NVN puts a header with its program size 0x100 bytes before compute code. Bounded by it, a BRX whose
       targets aren't resolved can't scan into the unrelated programs that follow in the same window. Other
       clients keep the whole window. */
    if (pa >= 0x100) {
        const u8* header = gmem_ptr_untracked(pa - 0x100, 8);
        if (header) {
            u32 magic = snx_ld_le32(header), declared = snx_ld_le32(header + 4);
            if (magic == 0x12345679 && declared >= 0x80 && declared <= 0x10000 && !(declared & 7) &&
                gmem_ptr_untracked(pa, declared))
                code_size = declared;
        }
    }
    memset(dst, 0, 0x10000 + 80);
    gmem_read_code(pa, dst + 80, code_size);
}

/* Storage buffers kept on the GPU (the model of Citron Neo's buffer cache, buffer_cache.h's
   BindHostComputeStorageBuffers and MarkWrittenBuffer, Copyright 2022 yuzu Emulator Project and 2025 citron Emulator
   Project, GPL-3.0-or-later), on the ownership ScarletNX's engines already use (docs/GPU_OWNERSHIP.md), so that the
   guest's own pages are trapped and land only when the CPU touches them, not at a sync point. A range one resource
   holds whole is bound where it is. Any other is copied, the CPU's bytes by the CPU and the GPU's by the GPU
   (vk_guest_read): into staging when it is only read, into a resource of its own when it is written, which then
   holds those bytes (res_publish). Nothing is waited for, compared or written back */
/* how recently the CPU must have made a written range land for a dispatch writing it to run on the compute queue */
#define CS_CPU_READ_PRESENTS 1200
static int res_bind(u64 gva, u64 len, int written, VkBuffer* buf, u64* off, VkOwnRes** made) {
    OwnRange o[2];
    VkBuffer sb;
    u64 cpu = 0, so;
    u8* p;
    *made = NULL;
    if (!gva) return vk_buf_copy(0, len, buf, off);
    if (gmmu_run(gva, len, &cpu) == len && own_lookup(cpu, len, o, 2) == 1 && o[0].addr == cpu && o[0].size == len &&
        !(o[0].res_off & (vk_stage_align() - 1))) {
        *buf = ((const VkOwnRes*)o[0].res)->buf;
        *off = o[0].res_off;
        return 1;
    }
    /* one only read takes the buffer cache's copy, which is copied again only when its bytes change (written, by the
       CPU through the write tracking or by the GPU): Citron Neo's SynchronizeBuffer uploads only the pages the CPU
       changed. Copied whole at every dispatch, the inputs were nearly all of one game's GPU thread (its passes read
       four buffers for every one they write) */
    if (!written) return vk_buf_get_strict(gva, len, buf, off);
    vk_render_end(); /* the copies happen outside rendering passes */
    if (!(p = vk_stage_upload(len, vk_stage_align(), &sb, &so))) return 0;
    vk_guest_read(gva, len, p, sb, so);
    VkOwnRes* r = vk_own_res_new(len);
    if (!r) return 0;
    VkBufferCopy c = {so, 0, len};
    rec_CmdCopyBuffer(vk_cmd(), sb, r->buf, 1, &c);
    *buf = r->buf;
    *off = 0;
    *made = r;
    return 1;
}
/* r's bytes are [gva, gva + len)'s from now on, GPU-owned run by run of guest memory, as a DMA copy's destination */
static void res_publish(u64 gva, u64 len, VkOwnRes* r) {
    for (u64 at = 0; at < len;) {
        u64 cpu = 0, n = gmmu_run(gva + at, len - at, &cpu);
        if (!n) { /* an unmapped page takes no write */
            n = 0x1000 - ((gva + at) & 0xFFF);
            if (n > len - at) n = len - at;
        } else
            vk_own_publish(cpu, n, r, at);
        at += n;
    }
}
static void res_unref_all(VkOwnRes** made, int n) {
    for (int i = 0; i < n; i++)
        if (made[i]) {
            vk_own_res_unref(made[i]);
            made[i] = NULL;
        }
}

void vk_dispatch(GpuChannel* ch, const u32* q, u64 code) {
    u32* rc = ch->rcomp;
    u32 gx = qmd_grid_dim_x(q), gy = qmd_grid_dim_y(q), gz = qmd_grid_dim_z(q), lx = qmd_block_dim_x(q),
        ly = qmd_block_dim_y(q), lz = qmd_block_dim_z(q), shm = qmd_shared_alloc(q);
    CsKey ck = {0};
    u32 tcb_key;
    u64 tic_key_base;
    CsBufImg bimg[8] = {0};
    int i;
    vk_frame_check();
    if (!gx || !gy || !gz || !lx || !ly || !lz) return;
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    if (gx > lim->maxComputeWorkGroupCount[0] || gy > lim->maxComputeWorkGroupCount[1] ||
        gz > lim->maxComputeWorkGroupCount[2] || lx > lim->maxComputeWorkGroupSize[0] ||
        ly > lim->maxComputeWorkGroupSize[1] || lz > lim->maxComputeWorkGroupSize[2] ||
        (u64)lx * ly * lz > lim->maxComputeWorkGroupInvocations || shm > lim->maxComputeSharedMemorySize) {
        LOG_LIMIT(1,
                  "vulkan: a dispatch of %ux%ux%u blocks of %ux%ux%u with %u bytes shared is past this GPU's limits "
                  "(skipped)\n",
                  gx, gy, gz, lx, ly, lz, shm);
        g_cs_skips[CS_SKIP_LIMITS]++;
        return;
    }
    cs_init();
    /* constant buffers from the QMD (reading them lands results they overlap) */
    u64 cba[QMD_NUM_CONST_BUFFERS] = {0};
    for (u32 i = 0; i < QMD_NUM_CONST_BUFFERS; i++) {
        g_cbn[i] = 0;
        if (!qmd_const_buffer_enabled(q, i)) continue;
        cba[i] = qmd_const_buffer_address(q, i);
        u32 sz = qmd_const_buffer_size(q, i);
        if (!sz || sz > sizeof g_cb[i]) sz = sizeof g_cb[i];
        g_cbn[i] = sz;
        gmem_read(cba[i], g_cb[i], sz);
    }
    /* Recheck code at a new submit or instruction invalidation, as vk_shader_get does. */
    struct cs_fast {
        u64 addr;
        u64 shader_generation;
        u64 code_hash; /* of the code window cs_pipe translated */
        u32 epoch, lx, ly, lz, shm;
        CsPipe* p;
    };
    static struct cs_fast fc[64];
    u64 pa = code + qmd_program_start(q);
    struct cs_fast* fe = &fc[((pa >> 8) ^ (pa >> 16)) & 63];
    CsPipe* p = fe->p;
    int same = p && fe->addr == pa && fe->lx == lx && fe->ly == ly && fe->lz == lz && fe->shm == shm &&
               vk_cb_reads_hold(&p->cbr, cs_cb_read);
    if (!same || fe->shader_generation != g_vk.shader_generation || fe->epoch != g_vk_epoch) {
        static u8 code_buf[0x10000 + 80]; /* compute programs have no header: 80 zero bytes stand in for one */
        CsKey unkeyed = {0};
        cs_read_code(pa, code_buf);
        /* same window and cbuf reads: same program, so skip vk_shader_hash's walk (5% of one game's GPU thread) */
        u64 h = vk_hash(code_buf, sizeof code_buf);
        if (!same || h != fe->code_hash) {
            p = cs_pipe(code_buf, lx, ly, lz, shm, unkeyed);
            fe->code_hash = h;
        }
        fe->addr = pa;
        fe->shader_generation = g_vk.shader_generation;
        fe->epoch = g_vk_epoch;
        fe->lx = lx;
        fe->ly = ly;
        fe->lz = lz;
        fe->shm = shm;
        fe->p = p == &g_cs_alloc_failed ? NULL : p;
    }
    /* Stores default to unsigned for bindless images and float for bound ones. Descriptors of another class,
       signed stores, and integer sampled textures select a variant keyed on their numeric types. */
    tcb_key = kepler_compute_tex_cb_index(rc);
    tic_key_base = kepler_compute_address(rc, KEPLER_COMPUTE_TIC);
    if (!p->failed)
        for (i = 0; i < p->si.ntex && i < SH_MAXTEX; i++) {
            ShTex* st = &p->si.tex[i];
            u32 h = 0, h2 = 0, tic[8];
            int num;
            if (st->shadow || st->flags) continue; /* raw and multisample textures are declared on their own */
            if (!cs_cb_read(st->bindless ? (st->bl_cbuf & 7) : tcb_key, st->cbuf_off * 4, &h)) continue;
            if (st->bindless && st->bl2 && cs_cb_read(st->bl2_cbuf & 7, st->bl2_off * 4, &h2)) h |= h2;
            vk_gmem_words(tic_key_base + (h & 0xFFFFF) * 32ull, tic, 8);
            /* by the header alone: textures are resolved only when bound, a color texture's surface has its header's
               number type, and depth isn't sampled here */
            num = vk_tex_num(tic, NULL);
            if (num) ck.tex_int |= 1ull << i;
            if (num == 2) ck.tex_sint |= 1ull << i;
        }
    if (!p->failed)
        for (i = 0; i < p->si.nimg && i < 8; i++) {
            u32 h = 0, tic[8];
            if (!vk_image_keyable(&p->si, i)) continue;
            if (!cs_cb_read(p->si.img[i].bindless ? (p->si.img[i].bl_cbuf & 7) : tcb_key, p->si.img[i].cbuf_off * 4,
                            &h))
                continue;
            vk_gmem_words(tic_key_base + (h & 0xFFFFF) * 32ull, tic, 8);
            if (vk_image_key(&p->si, i, tic)) ck.image |= 1u << i;
            if (vk_tex_num(tic, NULL) == 2) ck.image_sint |= 1u << i;
        }
    if (ck.tex_int || ck.image || ck.image_sint) {
        if (!p->keyed || (p->keyed->si.rt_int_mask >> 8) != ck.image || p->keyed->si.tex_int_mask != ck.tex_int ||
            p->keyed->si.tex_sint_mask != ck.tex_sint || (p->keyed->si.rt_sint_mask >> 8) != ck.image_sint) {
            static u8 keyed_code[0x10000 + 80];
            cs_read_code(pa, keyed_code);
            p->keyed = cs_pipe(keyed_code, lx, ly, lz, shm, ck);
        }
        p = p->keyed;
    }
    if (p->failed) {
        g_cs_skips[CS_SKIP_UNTRANSLATED]++;
        return;
    }
    int needs_unformatted_image = 0;
    for (i = 0; i < p->si.nimg; i++)
        needs_unformatted_image |= p->si.img[i].kind != 1;
    if (needs_unformatted_image && !g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        LOG_LIMIT(1, "vulkan: storage image writes need shaderStorageImageWriteWithoutFormat (skipped)\n");
        g_cs_skips[CS_SKIP_FEATURE]++;
        return;
    }
    /* storage buffers: {address, size} in constant buffer 0 */
    u64 sa[16];
    u32 ss[16];
    int ng = p->si.ngbuf > 16 ? 16 : p->si.ngbuf;
    u64 need = 0;
    for (int i = 0; i < ng; i++) {
        u32 v[3] = {0, 0, 0};
        cs_cb_read(0, p->si.gbuf_cbuf_off[i], &v[0]);
        cs_cb_read(0, p->si.gbuf_cbuf_off[i] + 4, &v[1]);
        cs_cb_read(0, p->si.gbuf_cbuf_off[i] + 8, &v[2]);
        ss[i] = vk_gbuf_desc(ST_CS, p->si.gbuf_cbuf_off[i], v, &sa[i]);
        need += ss[i] + vk_stage_align();
    }
    for (int i = 0; i < 8; i++)
        if (p->si.cbuf_mask & (1u << i)) need += g_cbn[i] + vk_stage_align();
    /* buffer images: their headers from the compute engine's texture pool, handles from its texture bank */
    for (i = 0; i < p->si.nimg && i < 8; i++) {
        u64 tic_base;
        u32 h = 0, tic[8];
        if (p->si.img[i].type != 1) continue;
        tic_base = kepler_compute_address(rc, KEPLER_COMPUTE_TIC);
        if ((u32)i >= g_vk_buffer_image_slots ||
            !cs_cb_read(p->si.img[i].bindless ? (p->si.img[i].bl_cbuf & 7) : kepler_compute_tex_cb_index(rc), p->si.img[i].cbuf_off * 4,
                        &h)) {
            g_cs_skips[CS_SKIP_BUFFER_IMAGE]++;
            return;
        }
        vk_gmem_words(tic_base + (h & 0xFFFFF) * 32ull, tic, 8);
        if (!cs_buffer_image(tic, p->si.img[i].kind, &bimg[i])) {
            LOG_LIMIT(4, "vulkan: unsupported compute buffer image (header word 0 %08x, skipped)\n", tic[0]);
            g_cs_skips[CS_SKIP_BUFFER_IMAGE]++;
            return;
        }
        need += bimg[i].bytes + vk_stage_align();
    }
    need += sizeof(i32) * 320 + vk_stage_align(); /* optional format data */
    /* dispatches that read textures run on the graphics queue, after what was recorded before them. Inputs that
       pending compute results overlap land those first, before anything is put in the arena */
    int gfx = p->si.ntex > 0 || p->si.nimg > 0, overlap = 0, retries = 0;
    /* so does one whose storage buffers a draw's pending result covers: on the graphics queue, after that draw, it
       reads the result on the GPU instead of the compute queue waiting for it to land (one game: three a frame) */
    for (i = 0; i < ng && !gfx; i++) gfx = sa[i] && vk_buf_pending(sa[i], ss[i]);
    /* A graphics-queue dispatch binds like a draw: staged inputs, results pending in staging (buffer.c's GfxOut),
       landed when the guest needs them or once their submit is done. Landing each one at once took a vk_finish per
       dispatch, about 9 a frame in one game. Its storage images are published over their guest bytes, GPU-owned
       (vk_surf_publish), and land at once only where they can't be */
    int deferred = gfx;
    /* one of storage buffers alone, where the host traps guest memory, keeps them on the GPU (res_bind). On the compute
       queue each landed at the next dispatch that read it: one game's passes update the same buffers dispatch after
       dispatch, and waited for the GPU at nearly every one, about 14 frames a second on any machine */
    int resident = !gfx && own_traps();
    /* but not one that writes a range the CPU has made land lately: kept on the GPU, each read of it by the CPU trapped
       and waited for the GPU, and one game reads its compute results every frame (34,000 landings in two minutes, its
       frame rate down from 54 to 39). Such a dispatch runs on the compute queue as before, alongside the draws, its
       results landing where they are read. One whose results only later dispatches and draws read stays resident */
    for (int i = 0; i < ng && resident; i++) {
        u64 cpu = 0, run;
        if (!((p->si.gbuf_wmask >> i) & 1) || !sa[i]) continue;
        if ((run = gmmu_run(sa[i], ss[i], &cpu)) && vk_own_cpu_read(cpu, run, CS_CPU_READ_PRESENTS)) {
            resident = 0;
#if defined(SNX_TRACY)
            g_vk_cs_cpu_reads++; /* the loop exits with it: one a dispatch */
#endif
        }
    }
    VkOwnRes* made[16] = {0};
    deferred = deferred || resident;
    /* a read-only input inside a pending result of this batch is read from the arena on the GPU, after a barrier
       (multi-pass skinning reads what the pass before wrote); any other overlap lands the results first */
    i64 chain[16];
    for (int i = 0; i < ng; i++) {
        chain[i] = -1;
        if (!pend_hit(&g_pages, sa[i], ss[i])) continue;
        for (int k = 0; k < g_nout && !overlap; k++) {
            CsOut* o = &g_out[k];
            if (sa[i] >= o->addr + o->size || o->addr >= sa[i] + ss[i]) {
                if (vk_backing_overlap(sa[i], ss[i], o->addr, o->size)) overlap = 1;
                continue;
            }
            if (!((p->si.gbuf_wmask >> i) & 1) && g_recording && sa[i] >= o->addr && sa[i] + ss[i] <= o->addr + o->size)
                chain[i] = (i64)(o->off + (sa[i] - o->addr));
            else
                overlap = 1;
        }
    }
    if (deferred)
        for (int i = 0; i < ng; i++) chain[i] = -1; /* inputs come through vk_buf_get, which finds pending results */
    else {
        if (g_recording && g_ar_used + need > g_ar_cap && g_ar_want < (64ull << 20))
            g_ar_want *= 2; /* fewer, bigger batches */
        if (gfx || overlap || g_inflight) {
            cs_land();
            for (int i = 0; i < ng; i++) chain[i] = -1; /* the results are in memory now */
        } else if (g_nout + ng > 256 || g_ar_used + need > g_ar_cap) {
            cs_put_aside(); /* full: it runs while the next one records, and lands when its results are needed */
            for (int i = 0; i < ng; i++) chain[i] = -1; /* a new arena */
        }
        if (!g_recording && !arena_fit(need)) {
            g_cs_skips[CS_SKIP_RESOURCES]++;
            return;
        }
    }

    VkDescriptorSet set;
    u64 setup_sub;
rebind:
    setup_sub = vk_submit_number();
    if (deferred) {
        vk_buf_writable_abort();
        set = vk_desc_alloc(g_vk_dslayout[0][shader_vk_set(ST_CS)]);
        if (!set) {
            g_cs_skips[CS_SKIP_RESOURCES]++;
            return;
        }
    } else {
        VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                           .descriptorPool = g_dpool,
                                           .descriptorSetCount = 1,
                                           .pSetLayouts = &g_vk_dslayout[0][shader_vk_set(ST_CS)]};
        if (vkAllocateDescriptorSets(g_vk.dev, &dai, &set) != VK_SUCCESS) {
            cs_land();
            if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &dai, &set))) {
                g_cs_skips[CS_SKIP_RESOURCES]++;
                return;
            }
        }
    }
    VkWriteDescriptorSet w[SNX_VKB_END];
    VkDescriptorBufferInfo bi[80];
    VkDescriptorImageInfo ii[SH_MAXTEX + 8];
    Surf* images[8] = {0};
    u8* originals[8] = {0};
    VkBufferView bufimg_view[8] = {0};
    VkBuffer bufimg_buf[8] = {0};
    u64 bufimg_off[8] = {0};
    u32 nw = 0;
    for (int i = 0; i < 8; i++)
        if (p->si.cbuf_mask & (1u << i)) {
            /* only the bytes the shader reads (robust buffer access keeps reads past them in bounds, of undefined
               value) */
            u32 range = g_cbn[i];
            if (p->si.cbuf_max[i] && range > p->si.cbuf_max[i]) range = p->si.cbuf_max[i];
            range = (range + 15) & ~15u;
            if (!range) range = 16;
            if (range > g_vk.props.limits.maxUniformBufferRange) range = g_vk.props.limits.maxUniformBufferRange;
            u32 n = g_cbn[i] < range ? g_cbn[i] : range;
            VkBuffer cbuf = g_ar;
            u64 o;
            u8* dst;
            if (deferred) {
                dst = vk_stage(range, vk_stage_align(), &cbuf, &o);
                if (!dst) {
                    g_cs_skips[CS_SKIP_RESOURCES]++;
                    return;
                }
            } else {
                o = arena_alloc(range);
                dst = g_ar_ptr + o;
            }
            memcpy(dst, g_cb[i], n);
            memset(dst + n, 0, range - n);
            bi[nw] = (VkDescriptorBufferInfo){cbuf, o, range};
            w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = SNX_VKB_CB + i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .pBufferInfo = &bi[nw]};
            nw++;
        }
    u64 so[16];
    VkBuffer bi_buf[16];
    int chained = 0;
    for (int i = 0; i < ng; i++) {
        VkBuffer sbuf = g_ar;
        u64 o;
        int alias = -1;
        for (int k = 0; k < i && resident && alias < 0; k++)
            if (sa[k] && sa[k] == sa[i] && ss[k] == ss[i]) alias = k; /* one range bound twice names one buffer */
        if (resident && alias >= 0) {
            sbuf = bi_buf[alias];
            o = so[alias];
            so[i] = o;
        } else if (resident) {
            if (!res_bind(sa[i], ss[i], (p->si.gbuf_wmask >> i) & 1, &sbuf, &o, &made[i])) {
                res_unref_all(made, ng);
                g_cs_skips[CS_SKIP_STORAGE_BUFFER]++;
                return;
            }
            so[i] = o;
        } else if (deferred) {
            /* written: a pending result of its own, from its original bytes; read: the bytes draws would read,
               which may be an earlier draw's or dispatch's result on the GPU */
            int ok = (p->si.gbuf_wmask >> i) & 1 ? vk_buf_copy_writable(sa[i], ss[i], &sbuf, &o)
                     : sa[i]                     ? vk_buf_get_strict(sa[i], ss[i], &sbuf, &o)
                                                 : vk_buf_copy(0, ss[i], &sbuf, &o);
            if (!ok) {
                vk_buf_writable_abort();
                g_cs_skips[CS_SKIP_STORAGE_BUFFER]++;
                return;
            }
            so[i] = o;
        } else {
            if (chain[i] >= 0) chained = 1;
            o = so[i] = chain[i] >= 0 ? (u64)chain[i] : arena_alloc(ss[i]);
            if (chain[i] >= 0)
                ; /* written by an earlier dispatch of the batch, read in place */
            else if (sa[i])
                gmem_read(sa[i], g_ar_ptr + o, ss[i]);
            else
                memset(g_ar_ptr + o, 0, ss[i]);
        }
        bi_buf[i] = sbuf;
        bi[nw] = (VkDescriptorBufferInfo){sbuf, o, ss[i]};
        w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = SNX_VKB_SSBO + i,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &bi[nw]};
        nw++;
    }
    VkCommandBuffer cb;
    if (gfx) {
        /* textures from the compute engine's headers; texture handles from its texture constant buffer bank. Their
           number types are what the program was keyed on (vk_tex_num), and depth textures aren't sampled here */
        u64 tic_base = kepler_compute_address(rc, KEPLER_COMPUTE_TIC), tsc_base = kepler_compute_address(rc, KEPLER_COMPUTE_TSC);
        u32 tcb = kepler_compute_tex_cb_index(rc);
        int fmt_used = 0;
        i32 fmt[320] = {0}; /* SNX_FMT, std140: snx_ic[8] snx_tc[32] snx_ib[8] snx_tb[32], 16 bytes each */
        for (int i = 0; i < p->si.ntex && i < SH_MAXTEX; i++) {
            ShTex* st = &p->si.tex[i];
            u32 h = 0, h2 = 0;
            cs_cb_read(st->bindless ? (st->bl_cbuf & 7) : tcb, st->cbuf_off * 4, &h);
            if (st->bindless && st->bl2 && cs_cb_read(st->bl2_cbuf & 7, st->bl2_off * 4, &h2)) h |= h2;
            u32 tic[8], tsc[8];
            vk_gmem_words(tic_base + (u64)texture_handle_tic_id(h) * TEXTURE_POOL_ENTRY_BYTES, tic, 8);
            vk_gmem_words(tsc_base + (u64)texture_handle_tsc_id(h) * TEXTURE_POOL_ENTRY_BYTES, tsc, 8);
            VkImageViewType want = vk_tex_view_type(st);
            if (st->flags & 2) {
                LOG_LIMIT(4, "vulkan: multisample texture upload is unavailable (skipped)\n");
                g_cs_skips[CS_SKIP_TEXTURE]++;
                cs_abandon_binding(originals);
                return;
            }
            Surf* t = st->flags & 1 ? vk_tex_from_tic_raw(tic) : vk_tex_from_tic(tic);
            if ((st->flags & 1) && !t) {
                LOG_LIMIT(4, "vulkan: raw surface load requires a 32-bit color format (skipped)\n");
                g_cs_skips[CS_SKIP_TEXTURE]++;
                cs_abandon_binding(originals);
                return;
            }
            int num = st->flags & 1                      ? 1
                      : !((p->si.tex_int_mask >> i) & 1) ? 0
                      : ((p->si.tex_sint_mask >> i) & 1) ? 2
                                                         : 1;
            VkImageView v =
                t && (!t->f.depth || vk_tic_reads_stencil(tic)) ? vk_tex_bind_view(t, want, tic, tsc, num)
                                                                : VK_NULL_HANDLE;
            VkImageLayout sampled_layout =
                p->si.nimg ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            const Surf* sampled = v ? t : NULL; /* the texture whose own view is bound, for the sampler */
            if (v)
                vk_surf_use(vk_cmd(), t, sampled_layout, VK_SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            else {
                v = vk_null_view(want, num);
                sampled_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            VkSampler smp = vk_tex_bind_sampler(tsc, st, num, sampled, want);
            if (!v || !smp) { /* before any output is recorded: nothing of this dispatch lands */
                g_cs_skips[CS_SKIP_TEXTURE]++;
                cs_abandon_binding(originals);
                return;
            }
            ii[i] = (VkDescriptorImageInfo){smp, v, sampled_layout};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = SNX_VKB_TEX + i,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &ii[i]};
            fmt[(8 + i) * 4] = t ? (t->f.vk == VK_FORMAT_R16G16B16A16_SFLOAT || t->f.vk == VK_FORMAT_R16G16_SFLOAT ||
                                            t->f.vk == VK_FORMAT_R16_SFLOAT
                                        ? 1
                                    : t->f.bpp == 4 && !t->f.is_int ? 2
                                                                    : 0)
                                 : 0;
            fmt[(48 + i) * 4] = t && t->f.bpp ? t->f.bpp : 4;
            fmt_used = 1;
        }
        for (int i = 0; i < p->si.nimg; i++) {
            if (p->si.img[i].type == 1) continue; /* buffer images are bound below */
            const u32 off = p->si.img[i].cbuf_off;
            u32 h = 0, tic[8];
            if (!cs_cb_read(p->si.img[i].bindless ? (p->si.img[i].bl_cbuf & 7) : tcb, off * 4, &h)) {
                g_cs_skips[CS_SKIP_STORAGE_IMAGE]++;
                cs_abandon_binding(originals);
                return;
            }
            vk_gmem_words(tic_base + (h & 0xFFFFF) * 32ull, tic, 8);
            Surf* t = vk_tex_from_tic_storage(tic);
            u64 image_bytes;
            VkImageView view = vk_storage_image_view(t, &p->si, i, &image_bytes);
            if (!view) {
                LOG_LIMIT(4, "vulkan: unsupported compute storage image (header word 0 %08x, kind %d, skipped)\n",
                          tic[0], p->si.img[i].kind);
                g_cs_skips[CS_SKIP_STORAGE_IMAGE]++;
                cs_abandon_binding(originals);
                return;
            }
            int seen = 0;
            for (int k = 0; k < i; k++)
                if (images[k] == t) {
                    seen = 1;
                    break;
                }
            if (!seen && !deferred) { /* a published image keeps no merge: its owner takes the span whole */
                originals[i] = malloc(image_bytes);
                if (!originals[i]) {
                    g_cs_skips[CS_SKIP_RESOURCES]++;
                    cs_abandon_binding(originals);
                    return;
                }
                gmem_read(t->gva, originals[i], image_bytes);
            }
            vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_STORAGE_IMAGE_ACCESS);
            images[i] = t;
            ii[p->si.ntex + i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = SNX_VKB_IMG + i,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                             .pImageInfo = &ii[p->si.ntex + i]};
        }
        for (i = 0; i < p->si.nimg && i < 8; i++) {
            int alias;
            int k;
            VkBufferViewCreateInfo vci;
            if (p->si.img[i].type != 1) continue;
            /* the slots of one buffer (its loads and atomics, and its stores) view one copy, so each sees the
               others' writes as the console's would */
            alias = -1;
            for (k = 0; k < i && alias < 0; k++)
                if (bufimg_view[k] && bimg[k].gva == bimg[i].gva) alias = k;
            if (alias >= 0) {
                bufimg_off[i] = bufimg_off[alias];
                bufimg_buf[i] = bufimg_buf[alias];
                if (bimg[i].bytes > bimg[alias].bytes) bimg[i].bytes = bimg[alias].bytes;
            } else if (deferred) { /* a pending result, like a written storage buffer */
                if (!vk_buf_copy_writable(bimg[i].gva, (u32)bimg[i].bytes, &bufimg_buf[i], &bufimg_off[i])) {
                    for (k = 0; k < i; k++)
                        if (bufimg_view[k]) vk_defer_buffer_view(bufimg_view[k]);
                    g_cs_skips[CS_SKIP_BUFFER_IMAGE]++;
                    cs_abandon_binding(originals);
                    return;
                }
            } else {
                bufimg_off[i] = arena_alloc(bimg[i].bytes);
                bufimg_buf[i] = g_ar;
                gmem_read(bimg[i].gva, g_ar_ptr + bufimg_off[i], bimg[i].bytes);
            }
            memset(&vci, 0, sizeof vci);
            vci.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
            vci.buffer = bufimg_buf[i];
            vci.format = bimg[i].fmt;
            vci.offset = bufimg_off[i];
            vci.range = bimg[i].bytes;
            if (!VK_OK(vkCreateBufferView(g_vk.dev, &vci, NULL, &bufimg_view[i]))) {
                for (k = 0; k < i; k++)
                    if (bufimg_view[k]) vk_defer_buffer_view(bufimg_view[k]); /* a draw may still use the set */
                g_cs_skips[CS_SKIP_BUFFER_IMAGE]++;
                cs_abandon_binding(originals);
                return;
            }
            memset(&w[nw], 0, sizeof w[nw]);
            w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[nw].dstSet = set;
            w[nw].dstBinding = SNX_VKB_IMGBUF + i;
            w[nw].descriptorCount = 1;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            w[nw].pTexelBufferView = &bufimg_view[i];
            nw++;
        }
        if (fmt_used) {
            VkBuffer fbuf = g_ar;
            u64 o;
            u8* dst;
            if (deferred) {
                dst = vk_stage(sizeof fmt, vk_stage_align(), &fbuf, &o);
                if (!dst) {
                    for (i = 0; i < p->si.nimg && i < 8; i++)
                        if (bufimg_view[i]) vk_defer_buffer_view(bufimg_view[i]);
                    g_cs_skips[CS_SKIP_RESOURCES]++;
                    cs_abandon_binding(originals);
                    return;
                }
            } else {
                o = arena_alloc(sizeof fmt);
                dst = g_ar_ptr + o;
            }
            memcpy(dst, fmt, sizeof fmt);
            snx_le32_array(dst, sizeof fmt / 4); /* the shader reads the words little-endian */
            bi[nw] = (VkDescriptorBufferInfo){fbuf, o, sizeof fmt};
            w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = SNX_VKB_FMT,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .pBufferInfo = &bi[nw]};
            nw++;
        }
    }
    /* reading an input landed results and submitted: what was read before may be stale, as in a draw's setup */
    if (deferred && setup_sub != vk_submit_number()) {
        for (i = 0; i < p->si.nimg && i < 8; i++)
            if (bufimg_view[i]) {
                vk_defer_buffer_view(bufimg_view[i]);
                bufimg_view[i] = VK_NULL_HANDLE;
            }
        vk_buf_writable_abort();
        res_unref_all(made, ng);
        if (++retries < 4) goto rebind;
        g_cs_skips[CS_SKIP_INPUTS_MOVED]++;
        return;
    }
    /* the outputs, once every input is read (an input can alias another storage buffer's output) */
    for (int i = 0; i < ng && !deferred; i++)
        if (((p->si.gbuf_wmask >> i) & 1) && sa[i]) {
            memcpy(g_org + so[i], g_ar_ptr + so[i], ss[i]);
            g_out[g_nout++] = (CsOut){sa[i], so[i], ss[i]};
            pend_add(&g_pages, sa[i], ss[i], 1);
        }
    /* buffer images land like storage buffers: only the bytes the dispatch changed are written back */
    for (i = 0; i < p->si.nimg && i < 8 && !deferred; i++) {
        int alias = 0;
        int k;
        for (k = 0; k < i; k++) alias |= bufimg_view[k] && bufimg_off[k] == bufimg_off[i];
        if (bufimg_view[i] && !alias && g_nout < 256) {
            memcpy(g_org + bufimg_off[i], g_ar_ptr + bufimg_off[i], bimg[i].bytes);
            g_out[g_nout].addr = bimg[i].gva;
            g_out[g_nout].off = bufimg_off[i];
            g_out[g_nout].size = bimg[i].bytes;
            g_nout++;
            pend_add(&g_pages, bimg[i].gva, bimg[i].bytes, 1);
        }
    }
    if (gfx || resident) {
        vk_render_end();
        cb = vk_cmd();
    } else {
        if (!g_recording) {
            VkCommandBufferBeginInfo b = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            VK_OK(vkBeginCommandBuffer(g_ccb, &b));
            g_recording = 1;
#if defined(SNX_TRACY)
            /* the batch's span on the GPU track: the pool's reset, a timestamp at the top, and the zone's begin,
               whose id its end and the read-back pair with */
            if (snx_tracy_attached() && g_vk.props.limits.timestampComputeAndGraphics) {
                if (!g_cqpool) {
                    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                                .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
                    vkCreateQueryPool(g_vk.dev, &qi, NULL, &g_cqpool);
                }
                if (g_cqpool) {
                    rec_CmdResetQueryPool(g_ccb, g_cqpool, 0, 2);
                    rec_CmdWriteTimestamp2(g_ccb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_cqpool, 0);
                    g_cq0 = snx_tracy_gpu_zone_begin("gpu compute batch", 0x2f9e8f, VK_GPU_CTX_COMPUTE, &g_cq_ep);
                }
            }
#endif
        }
        cb = g_ccb;
    }
    vkUpdateDescriptorSets(g_vk.dev, nw, w, 0, NULL);
    if (resident) vk_all_barrier(cb); /* the copies into its buffers, and earlier work on them, before it */
    if (chained) {
        VkMemoryBarrier2 cmb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        VkDependencyInfo cdi = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &cmb};
        rec_CmdPipelineBarrier2(cb, &cdi);
    }
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->p);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_vk_playout, 0, 1, &set, 0, NULL);
    rec_CmdDispatch(cb, gx, gy, gz);
    if (resident) {
        vk_all_barrier(cb); /* its writes before what reads them next */
        for (int i = 0; i < ng; i++) {
            if (!(((p->si.gbuf_wmask >> i) & 1) && sa[i])) continue;
            if (made[i]) res_publish(sa[i], ss[i], made[i]);
            gpu_mem_written(sa[i], ss[i]); /* what is built from these bytes (buffers, textures) takes the new ones */
        }
        res_unref_all(made, ng);
        g_vk_dispatches++;
        return;
    }
    if (deferred) {
        /* pending from here: the submit copies them where the host reads them (vk_buf_host_barrier). Submitted at
           once, the GPU runs it while the stream goes on, and a reader of its results waits for that submit alone,
           not for the draws recorded after it: Pokemon Legends Z-A writes command lists with a dispatch and fetches
           them a little later (a GPFIFO entry with SYNC), once a frame */
        vk_buf_writable_commit();
        for (i = 0; i < p->si.nimg && i < 8; i++)
            if (bufimg_view[i]) vk_defer_buffer_view(bufimg_view[i]);
        /* the storage images' texels over their guest bytes, after the dispatch on the GPU */
        for (i = 0; i < p->si.nimg && i < 8; i++) {
            int seen = 0;
            for (int k = 0; k < i && !seen; k++) seen = images[k] == images[i];
            if (!images[i] || seen) continue;
            vk_surf_wrote(images[i]);
            if (!vk_surf_publish(images[i])) vk_surf_flush(images[i], NULL);
        }
        g_vk_dispatches++;
        vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
        return;
    }
    /* results visible to the host */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    g_vk_dispatches++;
    if (gfx) {
        /* everything before it on the graphics queue runs first; then its results land at once */
        vmaFlushAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE); /* the arena may not be host coherent */
        vk_finish();
        vmaInvalidateAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE);
        g_landing = 1;
        for (int i = 0; i < g_nout; i++)
            vk_write_changed(g_out[i].addr, g_ar_ptr + g_out[i].off, g_org + g_out[i].off, g_out[i].size);
        outs_pages(g_out, g_nout, -1);
        for (i = 0; i < p->si.nimg && i < 8; i++)
            if (bufimg_view[i]) vkDestroyBufferView(g_vk.dev, bufimg_view[i], NULL);
        for (int i = 0; i < p->si.nimg; i++) {
            if (!images[i]) continue; /* a buffer image, landed with the outputs */
            int seen = 0;
            for (int k = 0; k < i; k++)
                if (images[k] == images[i]) {
                    seen = 1;
                    break;
                }
            if (seen) continue;
            vk_surf_wrote(images[i]);
            vk_surf_flush(images[i], originals[i]);
        }
        for (int i = 0; i < p->si.nimg; i++) free(originals[i]);
        g_nout = 0;
        g_ar_used = 0;
        vkResetDescriptorPool(g_vk.dev, g_dpool, 0);
        g_landing = 0;
        return;
    }
    cs_any_update();
}
