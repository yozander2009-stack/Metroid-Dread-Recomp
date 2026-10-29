/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Frames in flight. A frame runs from one present to the next and may submit many times (a readback, results the
   guest needs): each submission gets its own command buffer and signals the next value of the graphics timeline
   semaphore. A frame has a staging ring for uploads and for data draws read directly (vertices, indices, constant
   buffers), a ring its storage results are copied back into for the CPU, descriptor pools, and the resources to
   destroy once its work is done; none of them is reused before its last submission has finished. Ending a frame at a
   submit would reset them mid-draw, and wait for the GPU at every submit. */
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/platform/log.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/plat.h"
#include "common/utilities/common.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/tracy.h"

typedef struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView v1, v2;
    VkBuffer buf;
    VkBufferView view;
    void (*fn)(void*);
    void* arg;
} Dead;

/* A per-frame ring of one mapped buffer, reused once the frame's previous work is done. */
typedef struct {
    VkBuffer buf;
    VmaAllocation mem;
    u8* ptr;
    u64 cap, used;
    u32 small; /* frames in a row that used a quarter of it or less */
} Ring;

#define SPARE_SETS 32
#define SPARE_LAYOUTS 16
typedef struct {
    VkCommandPool pool;
    VkCommandBuffer* cbs; /* one per submission in this use of the frame */
    u32 ncb, icb;         /* icb: the one recording, or the next to begin */
    u64 last;             /* the timeline value of the frame's last submission, 0 when it has none */
    u64 hold;             /* the next frame's submission that reads buffers in this one's rings (vk_frame_hold), or 0 */
    int recording, fresh; /* fresh: just ended up current, not yet reset for its new use */
    Ring stage;           /* uploads and draw data, written by the CPU and read by the GPU */
    Ring output;          /* GPU-written storage and draw state, near the GPU when its budget allows */
    Ring back;            /* storage results copied back by the GPU for the CPU to read (vk_stage_back) */
    Ring* stage_spare; /* full-size overflow blocks, owned by this frame slot */
    u32 nstage, capstage, istage;
    /* destroyed once this frame's work is done */
    Dead* dead;
    int ndead, capdead;
    u64 dead_bytes; /* device memory evicted cache entries among them release (vk_defer_bytes) */
    /* descriptor pools, reset when the frame is reused and kept */
    VkDescriptorPool pool_d[64];
    int npool, ipool; /* pool_d[ipool] is the one allocating; the ones after it are reset and waiting */
    /* sets allocated ahead, a batch per layout at a time: most draws make a new set, and the driver's allocation cost
       is mostly per call */
    struct {
        VkDescriptorSetLayout layout;
        VkDescriptorSet set[SPARE_SETS];
        u32 n;
    } spare[SPARE_LAYOUTS];
} Frame;

static Frame g_fr[VK_FRAMES];
static u64 g_dead_bytes; /* every frame's dead_bytes */
static u32 g_cur;
static u32 g_cb_begun = 1; /* command buffers begun, so a command buffer's state is told from an earlier one's */
static u64 g_frame_id;
static VkSemaphore g_gsem; /* graphics timeline: submission n signals n */
static u64 g_gval;         /* the last submission */
static u64 g_landed;       /* submissions whose storage results vk_buf_frame_done has landed */
static u64 g_sub_ns;       /* when the last submission was made */
static VkSemaphoreSubmitInfo g_after[2]; /* vk_submit_after's wait and signal, for the next submission (semaphore 0: none) */
u32 g_vk_epoch, g_vk_present;

/* XXH3 (Yann Collet's xxHash 0.8.3, BSD-2-Clause, cmake/dependencies.cmake): the quality XXH64 gave the hash it
   replaced (a change in a word's high bits shows, and two don't cancel), at about three times its speed. On x86-64
   xxHash's dispatcher runs the AVX-512, AVX2 or SSE2 loop the CPU has (60 GB/s on a Zen 4 core, against XXH64's 19),
   from a build for any x86-64. Buffers' checks hashed hundreds of megabytes a frame in one game */
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86) /* as xxh3.c builds it */
#include "xxh_x86dispatch.h"
#define SNX_XXH3(p, n) XXH3_64bits_dispatch(p, n)
#else
#include "xxhash.h"
#define SNX_XXH3(p, n) XXH3_64bits(p, n)
#endif
u64 vk_hash(const void* p, u64 size) { return (u64)SNX_XXH3(p, (size_t)size); }
/* the same hash as vk_hash, which an entry's next check may compute instead, with src copied to dst: the bytes are
   hashed where they are (dst may be write-combined staging, slow to read back) and then copied, from cache */
u64 vk_hash_copy(void* dst, const void* src, u64 size) {
    u64 h = (u64)SNX_XXH3(src, (size_t)size);
    memcpy(dst, src, (size_t)size);
    return h;
}

u64 vk_sample_hash(u64 gva, u64 len) {
    u8 tmp[2048 + 128];
    if (len <= sizeof tmp) {
        vk_guest_peek(gva, tmp, len);
        return vk_hash(tmp, len);
    }
    u32 n = 0;
    vk_guest_peek(gva, tmp, 64);
    n = 64;
    vk_guest_peek(gva + len - 64, tmp + n, 64);
    n += 64;
    u64 step = (len - 32) / 62;
    for (int i = 0; i < 62; i++) {
        vk_guest_peek(gva + (u64)i * step, tmp + n, 32);
        n += 32;
    }
    return vk_hash(tmp, n);
}

u64 vk_stage_align(void) {
    static u64 al;
    if (!al) {
        const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
        al = 16;
        if (l->minUniformBufferOffsetAlignment > al) al = l->minUniformBufferOffsetAlignment;
        if (l->minStorageBufferOffsetAlignment > al) al = l->minStorageBufferOffsetAlignment;
        if (l->minTexelBufferOffsetAlignment > al) al = l->minTexelBufferOffsetAlignment; /* compute buffer images */
    }
    return al;
}

static void dead_push(Frame* f, Dead d) {
    if (f->ndead == f->capdead) {
        f->capdead = f->capdead ? f->capdead * 2 : 64;
        f->dead = realloc(f->dead, sizeof *f->dead * f->capdead);
    }
    f->dead[f->ndead++] = d;
}
static void dead_free(Frame* f) {
    for (int i = 0; i < f->ndead; i++) {
        Dead* d = &f->dead[i];
        if (d->v1) vkDestroyImageView(g_vk.dev, d->v1, NULL);
        if (d->v2) vkDestroyImageView(g_vk.dev, d->v2, NULL);
        if (d->view) vkDestroyBufferView(g_vk.dev, d->view, NULL);
        if (d->img) vk_image_destroy(d->img, d->mem);
        if (d->buf) {
            vk_mem_untrack(d->mem);
            vmaDestroyBuffer(g_vk.vma, d->buf, d->mem);
        }
        if (d->fn) d->fn(d->arg);
    }
    f->ndead = 0;
}

void vk_cmd_init(void) {
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                   .queueFamilyIndex = g_vk.gfx_family};
    VkSemaphoreTypeCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci};
    VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_gsem));
    for (int i = 0; i < VK_FRAMES; i++) {
        VK_OK(vkCreateCommandPool(g_vk.dev, &pci, NULL, &g_fr[i].pool));
        g_fr[i].fresh = 1;
    }
    vk_rec_init();
}

#define STAGE_MIN (16ull << 20)

/* a ring grown for a burst goes once it has been mostly unused for a while (the frame's work is done, nothing reads
   it); the next allocation makes one of the usual size */
/* GPU-written storage goes in device-local memory the CPU maps when there is room for it, else in host memory. Without
   resizable BAR that memory is a window of its own heap, 246 MiB on an RTX 2080, which the whole desktop shares, and
   past its room the driver moves the memory: every CPU write to it then faulted in the kernel, and one game crawled at
   10 frames a second for twenty seconds, its GPU thread half in the kernel. Streamed buffer snapshots use host memory
   instead: their large sequential writes must not cross that window. Stable input buffers get device-local copies.

   The room is the driver's budget (VK_EXT_memory_budget through VMA) less the use it counts, except where the driver
   counts the whole system's use of the window: NVIDIA's (615.78) does, and budgets what is left of it, so that use and
   budget add up to the heap's size. A process that has allocated nothing there (vulkaninfo) is told 155 of 246 MiB
   used and 91 budgeted, as nvidia-smi's BAR1 count. Read as this process's use, that looked past the budget all the
   time, and each frame's ring was freed at its reuse and made again in the window: a 16 MiB vkAllocateMemory and
   vkFreeMemory every frame, about a millisecond (a menu at 760 frames a second ran at 3200 without it). VMA
   (3.4) checks VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT only when it makes a block, and a ring of over half a block (an
   eighth of a small heap) gets memory of its own, so the rings didn't move out either. A ring is made there with room
   for it and a sixteenth of the heap to spare, for the desktop, and stays while the heap is within its budget. Where
   that memory is in the heap the caches use (resizable BAR, a GPU sharing system memory), the caches keep the heap
   within its budget, counting the rings, and a ring goes there while the heap's use stays within it.

   A window of its own is also no more than this process's share of it: a ring there is at most BAR_RING_MAX, and
   this process's memory in the window at most a quarter of it. Within the budget, NVIDIA's driver (615.78, open
   module) still failed to map a 64 MiB ring with 86 MiB budgeted ("NVRM: dmaAllocMapping_GM107: can't alloc VA space
   for mapping" in the kernel's log) and reported success: CPU writes to it then ran at 18 MB/s, against 5 GB/s, and
   every frame of that slot took half a second, in two games, in most runs, at a time that changed from run to run. No
   16 or 32 MiB ring failed. The budget can't see what the desktop and the driver itself keep mapped there, so the
   window is used as a small fast path, as other Vulkan translation layers do, and bigger rings go to host memory */
#define BAR_RING_MAX (32ull << 20)
static int g_bar_heap = -2; /* the heap of the device-local memory the CPU maps, -1 without one; -2 not looked for */
static int bar_heap(void) {
    const VkPhysicalDeviceMemoryProperties* mp;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if (g_bar_heap != -2) return g_bar_heap;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    g_bar_heap = -1;
    for (u32 t = 0; t < mp->memoryTypeCount && g_bar_heap < 0; t++)
        if ((mp->memoryTypes[t].propertyFlags & want) == want) g_bar_heap = (int)mp->memoryTypes[t].heapIndex;
    return g_bar_heap;
}
/* whether that heap has room for a ring of add more bytes (0: keeping the ones there), back: one coming back from
   host memory, after it went there for want of room */
static int bar_room(const VmaBudget* b, u64 add, int back) {
    const VkPhysicalDeviceMemoryProperties* mp;
    int heap = bar_heap();
    u64 size, use, budget, whole;
    if (heap < 0) return 0;
    use = b[heap].usage;
    budget = b[heap].budget;
    if (heap == vk_mem_heap()) return use + add <= (back ? budget / 4 * 3 : budget);
    vmaGetMemoryProperties(g_vk.vma, &mp);
    size = mp->memoryHeaps[heap].size;
    if (add && (add > BAR_RING_MAX || b[heap].statistics.blockBytes + add > size / 4)) return 0;
    whole = use + budget + (1u << 20) >= size && use + budget <= size + (1u << 20); /* NVIDIA's use */
    if (whole) use = 0;                                                           /* the budget is what is left */
    return budget >= use + (add ? add + size / 16 : 0);
}
/* Whether the staging ring's memory should move, while it is idle: it is in that heap, which has no room for it
   anymore, or in host memory while that heap has room for it again. A ring dropped here is made again where
   ring_alloc finds room */
static int ring_misplaced(const Ring* r, const VmaBudget* b) {
    const VkPhysicalDeviceMemoryProperties* mp;
    VmaAllocationInfo ai;
    vmaGetAllocationInfo(g_vk.vma, r->mem, &ai);
    vmaGetMemoryProperties(g_vk.vma, &mp);
    if ((int)mp->memoryTypes[ai.memoryType].heapIndex == bar_heap()) return !bar_room(b, 0, 0);
    return bar_room(b, r->cap, 1);
}
static void ring_reset(Ring* r, const VmaBudget* budget) {
    r->small = r->cap > STAGE_MIN && r->used <= r->cap / 4 ? r->small + 1 : 0;
    if (r->small >= 300 || (budget && r->buf && ring_misplaced(r, budget))) {
        vk_mem_untrack(r->mem);
        vmaDestroyBuffer(g_vk.vma, r->buf, r->mem);
        *r = (Ring){0};
    }
    r->used = 0;
}

static void timeline_wait(u64 value) {
    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_gsem, .pValues = &value};
    SNX_ZONE_C(z_wait, "vk timeline wait", 0xb05252);
    gpu_wait_note("a graphics submission", value);
    SNX_ZONE_V(z, "timeline wait", value);
    VK_OK(vkWaitSemaphores(g_vk.dev, &wi, UINT64_MAX));
    SNX_ZONE_END(z);
    gpu_wait_note(NULL, 0);
}
/* the storage results of every submission up to value, which has finished, land in guest memory, in order */
static void land_through(u64 value) {
    while (g_landed < value) {
        vk_buf_frame_done(++g_landed);
        vk_gfx_image_frame_done(g_landed);
    }
}

/* the frame's previous use is done, its results landed and what it held freed, before it records again */
static void frame_recycle(Frame* f) {
    u64 done = f->last > f->hold ? f->last : f->hold;
    SNX_ZONE(z, "vk frame recycle");
    if (done) {
        timeline_wait(done);
        land_through(done);
        f->last = 0;
        f->hold = 0;
    }
    vk_gpuprof_frame_done((u32)(f - g_fr)); /* its timestamps, read before the slot records again */
    dead_free(f);
    g_dead_bytes -= f->dead_bytes;
    f->dead_bytes = 0;
    vkResetCommandPool(g_vk.dev, f->pool, 0);
    /* the pools stay, reset: a frame's draws fill several, and making them again every frame costs */
    for (int i = 0; i < f->npool; i++) vkResetDescriptorPool(g_vk.dev, f->pool_d[i], 0);
    f->ipool = 0;
    memset(f->spare, 0, sizeof f->spare);
    {
        VmaBudget budget[VK_MAX_MEMORY_HEAPS];
        vmaSetCurrentFrameIndex(g_vk.vma, (u32)g_frame_id); /* VMA asks the driver for its budgets again */
        vmaGetHeapBudgets(g_vk.vma, budget);
        ring_reset(&f->stage, NULL);
        ring_reset(&f->output, budget);
        for (u32 i = 0; i < f->nstage; i++) ring_reset(&f->stage_spare[i], NULL);
        f->istage = 0;
    }
    ring_reset(&f->back, NULL);
    vk_rec_frame_reset((u32)(f - g_fr)); /* the slot's sets made on the recording thread too */
    f->icb = 0;
    f->fresh = 0;
}

VkCommandBuffer vk_cmd(void) {
    Frame* f = &g_fr[g_cur];
    if (!f->recording) {
        if (f->fresh) frame_recycle(f);
        if (f->icb == f->ncb) {
            /* the pool's command buffers may still be recording on the recording thread, and a pool is used by one
               thread at a time. A frame's slot only grows to its most submissions, so this is rare */
            vk_rec_sync();
            VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = f->pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1};
            VkCommandBuffer* cbs = realloc(f->cbs, sizeof *cbs * (f->ncb + 1));
            if (!cbs) fatal("vulkan: out of memory for command buffers");
            f->cbs = cbs;
            VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &f->cbs[f->ncb]));
            f->ncb++;
        }
        vk_rec_begin(f->cbs[f->icb]);
        f->recording = 1;
        g_cb_begun++;
    }
    return f->cbs[f->icb];
}

/* A full block at the allocation limit is needed again by the next use of this
 * frame slot. Keep it until that use has passed the slot's completion wait. */
static int stage_rotate(Frame* f)
{
	Ring previous;

	if (f->istage == f->nstage) {
		if (f->nstage == f->capstage) {
			u32 cap = f->capstage ? f->capstage * 2 : 4;
			Ring* spare;

			if (cap < f->capstage || (u64)cap > SIZE_MAX / sizeof *spare)
				return 0;
			spare = realloc(f->stage_spare, cap * sizeof *spare);
			if (!spare)
				return 0;
			f->stage_spare = spare;
			f->capstage = cap;
		}
		f->stage_spare[f->nstage++] = (Ring){0};
	}
	previous = f->stage;
	f->stage = f->stage_spare[f->istage];
	f->stage_spare[f->istage++] = previous;
	return 1;
}

/* bar: the ring may go in device-local memory the CPU maps, when that has room (bar_room), else it is host memory */
static u8* ring_alloc(Frame* f, Ring* r, u64 size, u64 align, VkBufferUsageFlags usage, VmaAllocationCreateFlags flags,
                      int bar, VkBuffer* buf, u64* off, VmaAllocation* mem) {
    if (align < 16) align = 16;
    u64 at = (r->used + align - 1) & ~(align - 1);
    if (size > g_vk.alloc_cap) return NULL; /* no allocation is bigger (memory.c) */
    if (r == &f->stage && r->buf && at + size > r->cap && r->cap == g_vk.alloc_cap) {
        vmaFlushAllocation(g_vk.vma, r->mem, 0, VK_WHOLE_SIZE);
        if (stage_rotate(f)) at = 0;
    }
    if (!r->buf || at + size > r->cap) {
        /* a bigger buffer; the old one stays alive until this frame's work is done */
        if (r->buf) {
            /* The submit only flushes the current staging ring. Flush a retired one too: earlier copies and ASTC
               dispatches still read its host writes (the readback ring has none) */
            if (r == &f->stage || r == &f->output) vmaFlushAllocation(g_vk.vma, r->mem, 0, VK_WHOLE_SIZE);
            dead_push(f, (Dead){.buf = r->buf, .mem = r->mem});
        }
        u64 cap = r->cap ? r->cap * 2 : STAGE_MIN;
        if (r == &f->stage && f->istage) cap = g_vk.alloc_cap;
        while (cap < size) cap *= 2;
        /* at the cap a full ring is followed by another of the same size: a frame's big uploads take several */
        if (cap > g_vk.alloc_cap) cap = g_vk.alloc_cap;
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = cap,
                                  .usage = usage,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.flags = flags | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                       .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info;
        if (bar) {
            VmaBudget budget[VK_MAX_MEMORY_HEAPS];
            vmaGetHeapBudgets(g_vk.vma, budget);
            if (bar_room(budget, cap, 0)) aci.usage = VMA_MEMORY_USAGE_AUTO;
        }
        if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &aci, &r->buf, &r->mem, &info))) {
            r->buf = VK_NULL_HANDLE;
            r->cap = 0;
            return NULL;
        }
        vk_mem_track(r->mem);
        r->ptr = info.pMappedData;
        r->cap = cap;
        at = 0;
    }
    r->used = at + size;
    *buf = r->buf;
    *off = at;
    if (mem) *mem = r->mem;
    return r->ptr + at;
}

static u8* stage_alloc(u64 size, u64 align, VkBuffer* buf, u64* off, VmaAllocation* mem, int output)
{
	Frame* f;

	vk_cmd(); /* completion of the slot's previous use precedes any reuse */
	f = &g_fr[g_cur];
	return ring_alloc(f, output ? &f->output : &f->stage, size, align,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
		(g_vk.has_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0),
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, output, buf, off, mem);
}
u8* vk_stage_output(u64 size, u64 align, VkBuffer* buf, u64* off, VmaAllocation* mem)
{
	return stage_alloc(size, align, buf, off, mem, 1);
}

u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off)
{
	return stage_alloc(size, align, buf, off, NULL, 1);
}

/* Streamed uploads can be hundreds of megabytes per frame. CPU writes through
 * a mapped device-memory window are slower than writes to host RAM. Keep these
 * snapshots in host memory; stable buffers get their own device-local copies. */
u8* vk_stage_upload(u64 size, u64 align, VkBuffer* buf, u64* off)
{
	return stage_alloc(size, align, buf, off, NULL, 0);
}

/* The staging ring can be device-local memory behind the BAR, where every CPU read crosses PCIe uncached: comparing
   a frame's storage results there cost one game most of its GPU thread. The frame's results are copied
   on the GPU into host-cached memory instead, and read from there. */
u8* vk_stage_back(u64 size, VkBuffer* buf, u64* off, VmaAllocation* mem) {
    vk_cmd();
    /* storage: render targets are swizzled into it (vk/blocklinear.c) */
    return ring_alloc(&g_fr[g_cur], &g_fr[g_cur].back, size, g_vk.props.limits.nonCoherentAtomSize,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, 0, buf, off, mem);
}

/* the frame's commands submitted, waiting for xw and signaling xs besides the graphics timeline (one each at most) */
static void submit(const VkSemaphoreSubmitInfo* xw, const VkSemaphoreSubmitInfo* xs, int land) {
    SNX_ZONE(z_submit, "vk submit");
    vk_render_end();
    Frame* f = &g_fr[g_cur];
    VkCommandBuffer cb = vk_cmd(); /* a submit with nothing recorded still has to wait and signal */
    vk_query_flush(cb);            /* the samples-passed counts of the passes ended (query.c) */
    vk_buf_host_barrier(cb, g_gval + 1);
    /* staging memory may not be host coherent: the host writes are made visible to the GPU here */
    if (f->stage.mem) vmaFlushAllocation(g_vk.vma, f->stage.mem, 0, VK_WHOLE_SIZE);
    if (f->output.mem) vmaFlushAllocation(g_vk.vma, f->output.mem, 0, VK_WHOLE_SIZE);
    VkSemaphoreSubmitInfo w[2];
    u32 nw = 0;
    if (g_after[0].semaphore) { /* at most one of the two: vk_submit_after's comes with no other */
        xw = &g_after[0];
        xs = &g_after[1];
    }
    if (xw) w[nw++] = *xw;
    if (g_vk_cs_wait_value) { /* draws read compute results on the GPU: after that batch */
        w[nw++] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                          .semaphore = g_vk_csem,
                                          .value = g_vk_cs_wait_value,
                                          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        g_vk_cs_wait_value = 0;
    }
    VkSemaphoreSubmitInfo s[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                   .semaphore = g_gsem,
                                   .value = g_gval + 1,
                                   .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    if (xs) s[1] = *xs;
    /* ended and submitted after its commands, on the recording thread (a submission that fails signals the timeline
       from the host there, so waits for it end) */
    vk_rec_submit(cb, w, nw, s, xs ? 2u : 1u);
    memset(g_after, 0, sizeof g_after);
    __atomic_store_n(&g_gval, g_gval + 1, __ATOMIC_RELAXED); /* read by the stall report too */
    g_sub_ns = plat_ns();
    f->recording = 0;
    f->icb++;
    SNX_ZONE_VALUE(z_submit, f->icb);
    f->last = g_gval;
    /* results of submissions the GPU has finished land now, so a later release doesn't find them pending */
    u64 done = 0;
    if (land && VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, g_gsem, &done)))
        land_through(done < g_gval ? done : g_gval);
    SNX_ZONE_END(z_submit);
}

void vk_submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal) {
    VkSemaphoreSubmitInfo w = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                               .stageMask = wait_stage};
    VkSemaphoreSubmitInfo s = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                               .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    submit(wait ? &w : NULL, signal ? &s : NULL, 1);
}
/* Ownership readback runs while its ranges are marked landing. Flush only an
   open command buffer: starting/recycling a frame or landing completed results
   here could acquire those same ranges and wait for this thread itself. */
void vk_submit_for_readback(void) {
    if (g_fr[g_cur].recording) submit(NULL, NULL, 0);
    vk_rec_sync(); /* the immediate copy must follow the recorder's queue calls */
}
void vk_submit_after(VkSemaphore sem, u64 wait, u64 signal) {
    g_after[0] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = sem,
                                         .value = wait, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    g_after[1] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = sem,
                                         .value = signal, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
}

u64 g_vk_finishes, g_vk_finish_ns;
void vk_finish(void) {
    u64 t0 = plat_ns();
    SNX_ZONE(z_finish, "vk finish");
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    timeline_wait(g_gval);
    land_through(g_gval);
    g_vk_finishes++;
    g_vk_finish_ns += plat_ns() - t0;
}

/* A present ends the frame: the next one records in the other slot, once that slot's previous use is done */
void vk_frame_next(void) {
    if (g_fr[g_cur].recording) vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    g_cur = (g_cur + 1) % VK_FRAMES;
    g_fr[g_cur].fresh = 1;
    g_frame_id++;
}
/* the GPU thread's recording time a submission holds at most */
#define SUBMIT_EVERY_NS 1000000
/* A frame that goes long without a present (a loading screen) still has to hand its pools and rings back: one with
   many submissions or descriptor pools ends here, at the start of a draw or dispatch, before anything is set up.
   Otherwise what was recorded goes to the GPU once it holds a millisecond of the GPU thread's work. Submissions came
   only when something needed results, so in one game the GPU sat idle through the first 12 ms of the frame's 21 ms
   on the GPU thread, got all of it in a burst at the end and finished 6 ms after the GPU thread, while the game
   waited for that frame's fence before submitting the next (34.7 fps, 40.3 with these). Earlier submissions also land
   their storage results sooner, so the pending results a draw's buffers are looked up in stay fewer. 2 ms and 0.5 ms
   were both slower. The limit on submissions leaves room for these in a slow frame: one that reaches it records
   the rest in the other slot, and its present then waits for this one's work */
void vk_frame_check(void) {
    const Frame* f = &g_fr[g_cur];
    if (f->icb >= 128 || f->ipool >= 32 || vk_rec_frame_sets(g_cur) >= 32 * 1024)
        vk_frame_next();
    else if (f->recording && plat_ns() - g_sub_ns >= SUBMIT_EVERY_NS)
        submit(NULL, NULL, 1);
}

u64 vk_submit_number(void) { return g_gval + 1; }
u32 vk_cb_begun(void) { return g_cb_begun; }
u64 vk_last_submit(void) { return g_gval; }
void vk_submit_progress(u64* submitted, u64* done) {
    *submitted = __atomic_load_n(&g_gval, __ATOMIC_RELAXED);
    *done = 0;
    if (g_gsem) VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, g_gsem, done));
}
void vk_wait_submit(u64 value) { timeline_wait(value); }
void vk_land_through(u64 value) { land_through(value < g_gval ? value : g_gval); }
u64 vk_frame_id(void) { return g_frame_id; }
void vk_frame_hold(u64 frame_id) {
    if (frame_id + 1 != g_frame_id) return; /* this frame's own rings, or an older frame's, landed when reused */
    g_fr[(g_cur + VK_FRAMES - 1) % VK_FRAMES].hold = vk_submit_number();
}
u32 vk_frame_slot(void) { return g_cur; }

VkDescriptorPool vk_desc_pool_new(void) {
    /* a thousand sets of the stage layout (gpu/vk/pipeline.c), each with all its descriptors: a frame chains more
       pools when it needs them */
    VkDescriptorPoolSize sz[VK_POOL_TYPES];
    VkDescriptorPoolCreateInfo ci = {0};
    vk_pool_sizes(sz, 1024);
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets = 1024;
    ci.poolSizeCount = VK_POOL_TYPES;
    ci.pPoolSizes = sz;
    VkDescriptorPool p = VK_NULL_HANDLE;
    VK_OK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &p));
    return p;
}
/* count sets of layout from the frame's pools into sets: count, or 1 when a pool can't hold that many, 0 on failure */
static u32 desc_alloc_n(Frame* f, VkDescriptorSetLayout layout, VkDescriptorSet* sets, u32 count) {
    VkDescriptorSetLayout layouts[SPARE_SETS];
    for (u32 i = 0; i < count; i++) layouts[i] = layout;
    for (int tries = 0; tries < 3; tries++) {
        if (tries) f->ipool++;
        if (tries == 2) count = 1; /* a fresh pool short of a whole batch */
        if (f->ipool == f->npool) {
            if (f->npool == (int)(sizeof f->pool_d / sizeof f->pool_d[0])) {
                LOG_LIMIT(1, "vulkan: a frame used all its descriptor pools (draws skipped)\n");
                return 0;
            }
            VkDescriptorPool p = vk_desc_pool_new();
            if (!p) return 0;
            f->pool_d[f->npool++] = p;
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = f->pool_d[f->ipool],
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
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout layout) {
    vk_cmd(); /* the pools are reset when the frame starts */
    Frame* f = &g_fr[g_cur];
    int i = 0;
    while (i < SPARE_LAYOUTS && f->spare[i].layout && f->spare[i].layout != layout) i++;
    if (i == SPARE_LAYOUTS) { /* more layouts than kept: one at a time */
        VkDescriptorSet set;
        return desc_alloc_n(f, layout, &set, 1) ? set : VK_NULL_HANDLE;
    }
    f->spare[i].layout = layout;
    if (!f->spare[i].n) f->spare[i].n = desc_alloc_n(f, layout, f->spare[i].set, SPARE_SETS);
    return f->spare[i].n ? f->spare[i].set[--f->spare[i].n] : VK_NULL_HANDLE;
}

/* vk_cmd() first: after a frame advance g_cur is the next slot, still holding an older frame's work. Starting it waits for
   that work and frees its list, so what is pushed now waits for the frame recorded from now on, which is submitted
   after every frame that can still use it */
void vk_defer_image(VkImage img, VmaAllocation a, VkImageView v1, VkImageView v2) {
    vk_cmd();
    dead_push(&g_fr[g_cur], (Dead){.img = img, .mem = a, .v1 = v1, .v2 = v2});
}
void vk_defer_buffer(VkBuffer b, VmaAllocation a) {
    vk_cmd();
    dead_push(&g_fr[g_cur], (Dead){.buf = b, .mem = a});
}
void vk_defer_bytes(u64 bytes) {
    g_fr[g_cur].dead_bytes += bytes;
    g_dead_bytes += bytes;
}
u64 vk_deferred_bytes(void) { return g_dead_bytes; }
void vk_defer_buffer_view(VkBufferView v) {
    vk_cmd();
    dead_push(&g_fr[g_cur], (Dead){.view = v});
}
void vk_defer_call(void (*fn)(void*), void* arg) {
    vk_cmd();
    dead_push(&g_fr[g_cur], (Dead){.fn = fn, .arg = arg});
}

void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout from, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = src_stage,
                               .srcAccessMask = src_access,
                               .dstStageMask = dst_stage,
                               .dstAccessMask = dst_access,
                               .oldLayout = from,
                               .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}

/* one buffer the GPU copies into for the host to read: vk_readback_buffer before recording the copy, vk_finish,
   then vk_readback_ready before reading the pointer */
static VkBuffer g_rb;
static VmaAllocation g_rb_mem;
static u8* g_rb_ptr;
static u64 g_rb_cap;
static u32 g_rb_small; /* readbacks in a row of an eighth of it or less */
u8* vk_readback_buffer(u64 size, VkBuffer* buf) {
    /* it grows to the biggest readback, and shrinks back after many small ones */
    if (size > g_vk.alloc_cap) return NULL; /* read back in parts (memory.c) */
    g_rb_small = g_rb_cap > STAGE_MIN && size <= g_rb_cap / 8 ? g_rb_small + 1 : 0;
    if (size > g_rb_cap || g_rb_small >= 64) {
        if (g_rb) vk_defer_buffer(g_rb, g_rb_mem);
        u64 cap = size > g_rb_cap ? (g_rb_cap ? g_rb_cap : STAGE_MIN) : STAGE_MIN;
        g_rb_small = 0;
        while (cap < size) cap *= 2;
        if (cap > g_vk.alloc_cap) cap = g_vk.alloc_cap;
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = cap,
                                  /* storage: render targets are swizzled into it (vk/blocklinear.c) */
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                       .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info;
        if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &aci, &g_rb, &g_rb_mem, &info))) {
            g_rb = VK_NULL_HANDLE;
            g_rb_cap = 0;
            return NULL;
        }
        vk_mem_track(g_rb_mem);
        g_rb_ptr = info.pMappedData;
        g_rb_cap = cap;
    }
    *buf = g_rb;
    return g_rb_ptr;
}
void vk_readback_ready(void) { vmaInvalidateAllocation(g_vk.vma, g_rb_mem, 0, VK_WHOLE_SIZE); }

void vk_wait_foreground(void) {
#ifdef SNX_IOS
    int said = 0;
    while (!backend_window_shown() && !g_quit) {
        if (!said++) LOG("vulkan: the app is in the background, GPU work waits until it is back\n");
        plat_sleep_ns(20000000);
    }
    if (said) LOG("vulkan: GPU work goes on\n");
#endif
}
