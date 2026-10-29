/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How big one device memory allocation may be, and images bigger than that; the budget the caches keep within, and
   the clock their ages are measured with.

   Some drivers fail allocations past 256 MiB even where maxMemoryAllocationSize is larger (NVIDIA's on FreeBSD), and
   a game's textures can be bigger: a 2D ASTC texture of 11584x11584 decodes to 512 MiB of RGBA8, a 3D one of 512^3
   too. No allocation is made bigger than g_vk.alloc_cap: the staging and readback rings stop growing there (cmd.c),
   and an image whose memory is bigger is created with sparse binding and bound to several allocations, each at most
   the cap. Shaders and views see one ordinary image: only its memory is in pieces.

   The pieces are dedicated allocations, so the cap is also what the driver is asked for. They are bound on the
   graphics queue, and the bind is waited for on the host before the image is used: a bind is not ordered with
   command buffers submitted after it, on that queue or on the compute queue, without a semaphore each of them would
   have to wait on, and an image this big is made once. */
#include <stdlib.h>
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"

#define ALLOC_CAP_DEFAULT (256ull << 20)

/* the images in pieces, each with its allocations */
typedef struct {
    VkImage img;
    u32 n;
    VmaAllocation pages[];
} Pieces;
static Pieces** g_pieces;
static u32 g_npieces, g_cappieces;

/* VMA allocations as Tracy memory events. */
#if defined(SNX_TRACY)
void vk_mem_track(VmaAllocation a) {
    VmaAllocationInfo info;
    if (!a) return;
    vmaGetAllocationInfo(g_vk.vma, a, &info);
    SNX_MEM_ALLOC((const void*)a, info.size, "gpu");
}
void vk_mem_untrack(VmaAllocation a) {
    if (a) SNX_MEM_FREE((const void*)a, "gpu");
}
#endif

void vk_mem_init(int sparse_queue) {
    VkPhysicalDeviceMaintenance3Properties m3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m3};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    u64 cap = ALLOC_CAP_DEFAULT;
    /* a smaller cap for testing the pieces on a driver that takes anything (lavapipe) */
    if (g_settings.vulkan_allocation_cap) cap = (u64)g_settings.vulkan_allocation_cap << 20;
    if (m3.maxMemoryAllocationSize && cap > m3.maxMemoryAllocationSize) cap = m3.maxMemoryAllocationSize;
    g_vk.alloc_cap = cap;
    g_vk.sparse = sparse_queue && g_vk.feat.sparseBinding;
    LOG("vulkan: allocations of at most %llu MiB, bigger images %s\n", (unsigned long long)(cap >> 20),
        g_vk.sparse ? "in pieces (sparse binding)" : "in one allocation (no sparse binding)");
}

static int pieces_bind(VkImage img, const VkMemoryRequirements* req, Pieces* p, VkResult* res) {
    u64 chunk = g_vk.alloc_cap / req->alignment * req->alignment;
    if (!chunk) return 0;
    u32 n = (u32)((req->size + chunk - 1) / chunk);
    VkSparseMemoryBind* binds = calloc(n, sizeof *binds);
    if (!binds) return 0;
    int ok = 1;
    for (u32 i = 0; i < n && ok; i++) {
        u64 at = (u64)i * chunk, size = req->size - at < chunk ? req->size - at : chunk;
        VkMemoryRequirements piece = {.size = size, .alignment = req->alignment, .memoryTypeBits = req->memoryTypeBits};
        VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                       .preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
        VmaAllocationInfo info;
        *res = vmaAllocateMemory(g_vk.vma, &piece, &aci, &p->pages[i], &info);
        if (!VK_OK(*res)) {
            ok = 0;
            break;
        }
        p->n++;
        vk_mem_track(p->pages[i]);
        binds[i] = (VkSparseMemoryBind){
            .resourceOffset = at, .size = size, .memory = info.deviceMemory, .memoryOffset = info.offset};
    }
    if (ok) {
        VkSparseImageOpaqueMemoryBindInfo opaque = {.image = img, .bindCount = n, .pBinds = binds};
        VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .imageOpaqueBindCount = 1,
                               .pImageOpaqueBinds = &opaque};
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        ok = VK_OK(vkCreateFence(g_vk.dev, &fi, NULL, &fence));
        if (ok) {
            ok = VK_OK(vkQueueBindSparse(g_vk.gfx, 1, &bi, fence)) &&
                 VK_OK(vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX));
            vkDestroyFence(g_vk.dev, fence, NULL);
        }
    }
    free(binds);
    return ok;
}

static void pieces_free(Pieces* p) {
    for (u32 i = 0; i < p->n; i++) {
        vk_mem_untrack(p->pages[i]);
        vmaFreeMemory(g_vk.vma, p->pages[i]);
    }
    free(p);
}

/* an image whose memory is more than one allocation may be: bound to pieces of at most g_vk.alloc_cap */
static int image_in_pieces(const VkImageCreateInfo* ii, VkImage* img, VkResult* res) {
    VkImageCreateInfo si = *ii;
    si.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT;
    *res = vkCreateImage(g_vk.dev, &si, NULL, img);
    if (!VK_OK(*res)) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_vk.dev, *img, &req);
    u64 chunk = req.alignment ? g_vk.alloc_cap / req.alignment * req.alignment : 0;
    u32 n = chunk ? (u32)((req.size + chunk - 1) / chunk) : 0;
    Pieces* p = n ? calloc(1, sizeof *p + n * sizeof p->pages[0]) : NULL;
    if (g_npieces == g_cappieces && p) {
        u32 cap = g_cappieces ? g_cappieces * 2 : 8;
        Pieces** grown = realloc(g_pieces, cap * sizeof *grown);
        if (grown) {
            g_pieces = grown;
            g_cappieces = cap;
        }
    }
    if (!p || g_npieces == g_cappieces || !pieces_bind(*img, &req, p, res)) {
        WARN("vulkan: cannot bind a %llu MiB image in %u pieces of %llu MiB\n", (unsigned long long)(req.size >> 20), n,
             (unsigned long long)(chunk >> 20));
        vkDestroyImage(g_vk.dev, *img, NULL);
        *img = VK_NULL_HANDLE;
        if (p) pieces_free(p);
        return 0;
    }
    p->img = *img;
    g_pieces[g_npieces++] = p;
    LOG_LIMIT(8, "vulkan: a %ux%ux%u image of %llu MiB, bound in %u pieces\n", ii->extent.width, ii->extent.height,
              ii->extent.depth * ii->arrayLayers, (unsigned long long)(req.size >> 20), n);
    return 1;
}

int vk_image_create(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem) {
    VkResult res;
    return vk_image_create_r(ii, img, mem, &res);
}
int vk_image_create_r(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem, VkResult* res) {
    *res = VK_ERROR_UNKNOWN;
    *img = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                         .pCreateInfo = ii};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(g_vk.dev, &q, &req);
    if (req.memoryRequirements.size > g_vk.alloc_cap) {
        if (g_vk.sparse) return image_in_pieces(ii, img, res);
        /* without sparse binding the one allocation is the only way: the driver may still take it */
        LOG_LIMIT(4, "vulkan: a %llu MiB image in one allocation, past the %llu MiB cap\n",
                  (unsigned long long)(req.memoryRequirements.size >> 20), (unsigned long long)(g_vk.alloc_cap >> 20));
    }
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    *res = vmaCreateImage(g_vk.vma, ii, &aci, img, mem, NULL);
    if (VK_OK(*res)) vk_mem_track(*mem);
    return VK_OK(*res);
}

void vk_image_destroy(VkImage img, VmaAllocation mem) {
    if (mem || !img) {
        vk_mem_untrack(mem);
        vmaDestroyImage(g_vk.vma, img, mem);
        return;
    }
    for (u32 i = 0; i < g_npieces; i++)
        if (g_pieces[i]->img == img) {
            vkDestroyImage(g_vk.dev, img, NULL);
            pieces_free(g_pieces[i]);
            g_pieces[i] = g_pieces[--g_npieces];
            return;
        }
    vkDestroyImage(g_vk.dev, img, NULL); /* made without memory */
}

/* ------------------------------------------------------------ the budget */
/* The heap images go to: the biggest device-local one (the only one on a GPU sharing system memory). The BAR window,
   device local too, holds the staging rings, which keep within its budget themselves (cmd.c ring_misplaced) */
static int g_heap = -1;
static u64 g_budget_cap; /* vk_budget_mb, 0 when unset */

static void heap_find(void) {
    const VkPhysicalDeviceMemoryProperties* mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    g_heap = 0;
    for (u32 h = 0; h < mp->memoryHeapCount; h++)
        if ((mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            (!(mp->memoryHeaps[g_heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ||
             mp->memoryHeaps[h].size > mp->memoryHeaps[g_heap].size))
            g_heap = (int)h;
    if (g_settings.vulkan_memory_budget) {
        g_budget_cap = (u64)g_settings.vulkan_memory_budget << 20;
        LOG("vulkan: the caches keep within %llu MB of heap %d (vk_budget_mb)\n",
            (unsigned long long)(g_budget_cap >> 20), g_heap);
    }
}

int vk_mem_heap(void) {
    if (g_heap < 0) heap_find();
    return g_heap;
}

/* the heap's use as the caches see it, and its budget. The use leaves out the free space inside VMA's blocks, which
   the next allocations fill before VMA asks the driver for more, and the memory of evicted entries the frames in
   flight still hold (vk_deferred_bytes): it goes once they are done */
void vk_mem_use(u64* usage, u64* budget) {
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    if (g_heap < 0) heap_find();
    vmaGetHeapBudgets(g_vk.vma, b);
    u64 u = b[g_heap].usage, holes = b[g_heap].statistics.blockBytes - b[g_heap].statistics.allocationBytes,
        gone = vk_deferred_bytes();
    u = u > holes ? u - holes : 0;
    *usage = u > gone ? u - gone : 0;
    *budget = g_budget_cap && g_budget_cap < b[g_heap].budget ? g_budget_cap : b[g_heap].budget;
}

/* the present clock: a present and its time every quarter second at most, seventeen minutes back */
#define CLOCK_N 4096
static struct {
    u32 present;
    u64 ns;
} g_clock[CLOCK_N];
static u32 g_clock_n, g_clock_at; /* samples kept; where the newest is */

void vk_mem_present(void) {
    u64 now = plat_ns();
    if (g_clock_n && now - g_clock[g_clock_at].ns < 250000000ull) return;
    g_clock_at = g_clock_n ? (g_clock_at + 1) % CLOCK_N : 0;
    g_clock[g_clock_at].present = g_vk_present;
    g_clock[g_clock_at].ns = now;
    if (g_clock_n < CLOCK_N) g_clock_n++;
}

u32 vk_present_before(u64 ago_ns, u32 presents) {
    if (g_vk_present <= presents) return 0;
    u32 before = g_vk_present - presents;
    if (!ago_ns) return before;
    /* the newest sample at least ago_ns old: what was last used before its present has been unused since then */
    u64 now = plat_ns();
    for (u32 i = 0; i < g_clock_n; i++) {
        u32 at = (g_clock_at + CLOCK_N - i) % CLOCK_N;
        if (now - g_clock[at].ns < ago_ns) continue;
        return (i32)(g_clock[at].present - before) < 0 ? g_clock[at].present : before;
    }
    return 0; /* the clock doesn't reach back that far: nothing is that old yet */
}
