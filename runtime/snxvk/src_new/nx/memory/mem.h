/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's address space: one host reservation that guest addresses index, its regions, and the memory blocks
   with the states and permissions QueryMemory reports */
#ifndef SNX_MEM_H
#define SNX_MEM_H
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/endian.h"
#include <string.h>

/* The most host address space the guest gets (128 GiB), which tables indexed by guest address are sized for */
#define AS_MAX 0x2000000000ull
/* A 64-bit application of the pre-2.0 address space type: the console gives these 64 GiB, and everything the game
   holds (an IPC buffer descriptor's address among them) has to fit in 36 bits */
#define AS36 (1ull << 36)
/* The host reservation, which a guest address of either width is an offset below: AS_MAX, or less where the system
   allows an app less address space (iOS without the paid extended-virtual-addressing entitlement), when mem_init picks
   a smaller layout to fit it */
extern u64 g_as_size;
#define AS_SIZE (g_as_size)

/* The regions of the address space. The 64-bit layout is g_layout's static value, so it holds before mem_layout32 */
typedef struct {
    u64 base, end; /* what the game may map in */
    u64 heap, heap_size;
    u64 alias, alias_size;
    u64 stack, stack_size;
    u64 kern, kern_end; /* the kernel's own pages (TLS) */
} MemLayout;
#define AS_BASE    (g_layout.base)
#define AS_END     (g_layout.end)
#define HEAP_BASE  (g_layout.heap)
#define HEAP_SIZE  (g_layout.heap_size)
#define ALIAS_BASE (g_layout.alias)
#define ALIAS_SIZE (g_layout.alias_size)
#define STACK_BASE (g_layout.stack)
#define STACK_SIZE (g_layout.stack_size)
#define KERN_BASE  (g_layout.kern)
#define KERN_END   (g_layout.kern_end)
/* where a 32-bit process's first module goes */
#define AS32_CODE_BASE 0x200000ull

/* the host address of guest address a */
#define GP(a) (g_mem + (u64)(a))

/* a run of pages with one state, permission and attribute (QueryMemory's MemoryInfo, less the address width) */
typedef struct {
    u64 addr, size;
    u32 state, perm, attr, ipc, dev; /* ipc and dev: IPC and device reference counts */
    u64 pool; /* the alias pool's offset of addr plus 1 when the pages are the pool's, 0 when the reservation's own */
} MemBlock;

/* ldr:ro's code view at base: size bytes of heap at source, then bss_size bytes at bss, the same pages as the sources
   with the alias pool (a copy without, written back by mem_unmap_code). The sources stay locked until then, and the
   view isn't charged again (it is heap memory) */
typedef struct {
    u64 base, source, size, bss, bss_size;
} MemCodeMap;

/* Memory states (MemoryState with its attribute bits, as QueryMemory returns them to the game) */
enum {
    MS_FREE = 0x00000000,
    MS_IO = 0x00182001,
    MS_STATIC = 0x00042002,
    MS_CODE = 0x04DC7E03,
    MS_CODEDATA = 0x07FEBD04,
    MS_NORMAL = 0x077EBD05,
    MS_SHARED = 0x04402006,
    MS_ALIASCODE = 0x04DD7E08,
    MS_ALIASCODEDATA = 0x07FFBD09,
    MS_IPC = 0x045C3C0A,
    MS_STACK = 0x045C3C0B,
    MS_TLS = 0x0400000C,
    MS_TRANSFERED = 0x055C3C0D,
    MS_SHAREDTRANSFERED = 0x045C380E,
    MS_SHAREDCODE = 0x0440380F,
    MS_INACCESSIBLE = 0x00000010,
    MS_KERNEL = 0x00000013,
    MS_GENCODE = 0x04402214,
    MS_CODEOUT = 0x04402015
};
/* MemoryPermission */
enum { PERM_NONE = 0, PERM_R = 1, PERM_W = 2, PERM_RW = 3, PERM_X = 4, PERM_RX = 5, PERM_DONTCARE = 0x10000000 };
/* MemoryAttribute: the bits svcSetMemoryAttribute names, and the set it may change (Uncached and PermissionLocked) */
enum { MA_LOCKED = 1, MA_IPC_LOCKED = 2, MA_DEVICE_SHARED = 4, MA_UNCACHED = 8, MA_PERMISSION_LOCKED = 0x10 };
#define MA_SET_MASK 0x18u
/* what the calls below return besides 0 */
enum { MEM_BADSTATE = -1, MEM_NOMEM = -2, MEM_LIMIT = -3 };

/* the console's layout below 4 GiB for a 32-bit process, by the NPDM's address space type: 0 with the alias region, 2
   without */
void mem_layout32(unsigned aspace);
/* the console's 64 GiB layout for a 64-bit process of the pre-2.0 address space type (the NPDM's 1), where every
   guest address has to fit in 36 bits */
void mem_layout36(void);
/* The guest's address space reserved now, the largest tier the system gives from the console's 128 GiB down to 5 GiB,
   each refused one said: before the game's code takes a share of a small one. mem_init finds it kept */
void mem_reserve(void);
void mem_init(void);
int mem_map(u64 addr, u64 size, u32 state, u32 perm); /* 0, MEM_BADSTATE when it isn't free, MEM_NOMEM */
int mem_unmap(u64 addr, u64 size);
/* Physical memory in the alias region, and the heap, under the memory lock against one combined budget in bytes.
   0 or a MEM_ value. A heap change that fails leaves the heap as it was */
int mem_map_physical(u64 addr, u64 size, u64 budget);
int mem_unmap_physical(u64 addr, u64 size);
int mem_heap_set(u64 size, u64 budget);
/* maps m's view at a free base it fills in, MEM_LIMIT when no code space is left */
int mem_map_code(MemCodeMap* m);
int mem_unmap_code(const MemCodeMap* m);
void mem_query(u64 addr, MemBlock* out);
/* the bytes of [addr, addr + size) still mapped, up to the first free or inaccessible page: what a GPU mapping older
   than the guest's frees may read (gpu.c's copy_runs). All of them before mem_init, with no block list */
u64 mem_mapped_run(u64 addr, u64 size);
int mem_set_state(u64 addr, u64 size, u32 state, u32 perm);
int mem_set_attr(u64 addr, u64 size, u32 mask, u32 attr);
/* Called with no memory lock held after a mapping change that can make compiled guest code stale: an unmap, a state
   or permission change, an alias written back. The runtime sets it (runtime/guest_jit.c's jit_code_invalidate); NULL
   until then */
void mem_set_code_invalidate(void (*fn)(u64 addr, u64 size));
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align);
u64 kmem_alloc(u64 size, u32 state, u32 perm); /* in the kernel region */

/* Views: a second mapping of mapped memory (MapMemory, code memory's owner and slave). With the alias pool the view
   and its source are the same pages: mem_alias_begin moves the source into the pool, mem_alias_end back into the
   reservation's own memory once no view of it is left. Without the pool (Windows, or a host without memfd) begin and
   end do nothing, a view is a copy, and mem_alias_unmap copies it back into back (0: not at all). 0 or a MEM_ value */
int mem_alias_begin(u64 src, u64 size);
int mem_alias_map(u64 dst, u64 src, u64 size, u32 state, u32 perm);
int mem_alias_unmap(u64 dst, u64 size, u64 back);
void mem_alias_end(u64 src, u64 size);
/* whether a view shares its source's pages: the alias pool is on (a view is a copy without it) */
int mem_pool_active(void);
/* Pool pages of their own (shared memory): size bytes mapped outside the reservation at the pointer returned, with
   their pool offset in off; NULL without a pool or when it is full */
u8* mem_pool_host(u64 size, u64* off);
void mem_pool_host_free(u8* p, u64 off, u64 size);
/* those pages at free guest pages, as mem_map maps; mem_unmap unmaps them */
int mem_map_pool(u64 addr, u64 off, u64 size, u32 state, u32 perm);
/* unmaps the range only when it is in state with the pages from off: 0, or MEM_BADSTATE with nothing changed */
int mem_unmap_pool(u64 addr, u64 off, u64 size, u32 state);
u64 mem_pool_used(void); /* bytes of the pool handed out */
/* [a, a + size), whole pages, made pool pages mapped there only, ready to be trapped (memory/own.c): the parts that
   aren't pool pages moved there, each part frozen meanwhile. 0 with the range's runs of pool pages in runs (*n of
   them, at most max), or -1 (no pool, unmapped or code memory, pages mapped elsewhere too, or more runs than max).
   The caller holds the renderer's lock */
typedef struct {
    u64 addr, size, pool; /* guest pages [addr, addr + size) are pool pages from pool on */
} MemPoolRun;
int mem_own_prepare(u64 a, u64 size, MemPoolRun* runs, u32 max, u32* n);
/* the pool offset of guest address a: 0, or -1 when a isn't pool memory */
int mem_pool_offset(u64 a, u64* off);

/* ---- save states (runtime/state.c), with the game paused and nothing of the GPU's left in guest memory */
/* The pool's pages in use, in offset order: a run a service maps outside the reservation (a shared memory's pages,
   mem_pool_host) has host set, and the rest is cut at its ends, so each span is one or the other */
typedef struct {
    u64 off, size;
    int host;
} MemSpan;
/* the blocks (mem_query's) and the spans into out, max at most: how many there are */
u32 mem_blocks(MemBlock* out, u32 max);
u32 mem_spans(MemSpan* out, u32 max);
int mem_pool_read(u64 off, void* dst, u64 size); /* 0, or -1 */
/* A load's new address space, from a state's blocks and spans. mem_restore_begin takes the renderer's lock and the
   memory lock, lets go of what the GPU holds, and unmaps every block but the modules' code (MS_CODE, which a state
   keeps as the game loaded it) and every span but the services' host views (a load reconciles those with their
   shared memories itself, mem_pool_host and mem_pool_host_free, before it begins). Then mem_restore_alloc places
   each of the state's other spans anew (its offset, or ~0 when the pool is full), mem_restore_write fills them,
   mem_restore_blocks makes the block list the state's (its pool offsets already this run's, its MS_CODE blocks those
   mapped now) and maps and commits it, the caller writes the other blocks' bytes at GP, and mem_restore_end sets the
   heap's and physical memory's sizes, lets go of the locks and tells the JIT and the renderer every block changed.
   Every call but mem_restore_end returns 0, or -1 with nothing more to do but mem_restore_end */
int mem_restore_begin(void);
u64 mem_restore_alloc(u64 size);
int mem_restore_write(u64 off, const void* p, u64 size);
int mem_restore_blocks(const MemBlock* blk, u32 n);
void mem_restore_end(u64 heap_size, u64 phys_mapped);
/* before any of that, while the game runs: whether blk (n of them) is a block list mem_restore_blocks takes, one that
   covers the address space in order with the code mapped now. 0, or -1 */
int mem_restore_check(const MemBlock* blk, u32 n);

extern MemLayout g_layout;
extern u8* g_mem;
/* changed under the memory lock, read with __atomic loads without it */
extern u64 g_heap_size, g_phys_mapped;
extern u64 g_heap_max; /* the heap's cap in bytes, 0 for none */
/* the alias pool's size in bytes, read by mem_init; a test lowers it after to fill the pool */
extern u64 g_pool_size;
/* charged once, before guest threads start */
extern u64 g_code_memory, g_main_stack_size;

/* Guest memory is byte-addressed and little-endian whatever the host's order (common/utilities/endian.h), so these
   go through memcpy and convert: a value read from or written to guest memory as a number goes through them or
   endian.h's snx_ld_le32 and kin, never a plain load or a cast pointer (docs/PPC64.md#big-endian-hosts) */
static inline u16 gr16(u64 a) { return snx_ld_le16(GP(a)); }
static inline u32 gr32(u64 a) { return snx_ld_le32(GP(a)); }
static inline u64 gr64(u64 a) { return snx_ld_le64(GP(a)); }
static inline void gw16(u64 a, u16 v) { snx_st_le16(GP(a), v); }
static inline void gw32(u64 a, u32 v) { snx_st_le32(GP(a), v); }
static inline void gw64(u64 a, u64 v) { snx_st_le64(GP(a), v); }
#endif
