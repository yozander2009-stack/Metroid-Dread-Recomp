/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What nvdrv shares with the GPU, host1x and vi: nvmap handles, the GPU's address space, and the syncpoints */
#ifndef SNX_NVDRV_H
#define SNX_NVDRV_H
#include "nx/runtime/state_fns.h"
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/platform/plat.h" /* PMutex, PCond */

/* The syncpoint ids the table holds. An id at or past it would alias a smaller one: it reads 0, never counts as
   reached, and the ioctls refuse it. syncpt_alloc hands out fewer (NVIDIA's stream library has room for 128). Wider
   than both the 128 below and Eden's MaxSyncPoints (192), so an id a game names that syncpt_alloc didn't hand out
   still reads, increments and waits as itself */
#define SYNCPT_IDS 256
/* The ids syncpt_alloc hands out, [SYNCPT_FIRST, SYNCPT_N), 108 of them: GPU channels and host1x's NVDEC and VIC
   channels alike. SYNCPT_N is the 128 NVIDIA's stream library (NvRmStream) sizes its per-syncpoint tables at, since
   NvRmChannelNumSyncPoints in the game's library is a stub that fails: an id of 128 or more crashed the first VIC
   job in NvRmStreamBegin and wrote past the table into the game's heap (host1x once took ids from 200). The first 20
   are never handed out, as the allocator has always started there; nothing in its history says why. Eden's fixed ids
   (12 VIC, 26 and 27 the vblanks, 0x36 NVDEC, 0x37 NVJPG) aren't kept out: nothing here uses a fixed id */
#define SYNCPT_FIRST 20u
#define SYNCPT_N     128u

void nv_init(void);
/* an ioctl on fd that a restored thread makes again (svc_service_retry): the use of the fd the old one counted goes */
void nv_ioctl_abandon(u32 fd);

/* A live, allocated handle's address and size, read together under nvdrv's lock from any thread. 1, or 0 (both
   outputs 0) for a dead handle */
int nvmap_get(u32 h, u64* addr, u32* size);
u64 nvmap_addr_by_id(u32 id); /* 0 when no live handle has that id */

/* The guest address a GPU address maps to, 0 when its page isn't mapped */
u64 gmmu_translate(u64 gva);
/* The bytes from gva on, at most max, that stay contiguous in guest memory, with gva's guest address in *cpu. 0 when
   gva's page isn't mapped */
u64 gmmu_run(u64 gva, u64 max, u64* cpu);
/* Points the pages of [gva, gva + size) at guest memory from cpu on, the renderer told (its mapping bracket): what
   nvhost-as-gpu's MapBufferEx does, and a test without the ioctls */
void gmmu_map(u64 gva, u64 cpu, u64 size);
/* Each GPU range that shows guest bytes of [cpu, cpu + size), clipped to them: fn(ctx, gva, n) for the n bytes at
   gva, each mapped page once. The same guest memory is mapped at several GPU addresses, so what is known of it by one
   address holds at the others, and this finds them all from the guest side. From any thread. fn runs under the
   index's lock: it takes no lock and doesn't call back into nvdrv. The index compares addresses only, as the page
   table holds them: guest memory freed under a mapping still counts here, where gmmu_translate reads it as unmapped */
void gmmu_aliases(u64 cpu, u64 size, void (*fn)(void* ctx, u64 gva, u64 n), void* ctx);
/* The other way: each run of the page table over [gva, gva + size), a range below 2^40, in GPU order and clipped to it:
   fn(ctx, gva, cpu, n) for n bytes at gva showing guest memory from cpu on, whole pages. Addresses only, and under the
   same lock, as gmmu_aliases */
void gmmu_pieces(u64 gva, u64 size, void (*fn)(void* ctx, u64 gva, u64 cpu, u64 n), void* ctx);

/* Every syncpoint user allocates its ids here, so they stay below the 128 the stream library sizes its tables at.
   Running out is fatal. A freed id can be handed out again */
u32 syncpt_alloc(void);
void syncpt_free(u32 id);
u32 syncpt_read(u32 id);
void syncpt_incr(u32 id);
void syncpt_add(u32 id, u32 n); /* n increments, one wakeup */
/* syncpt_add for a thread holding the renderer's lock (vk_lock), and only inside such a hold: the event slots waiting
   for the value are signaled by the same thread's syncpt_events, which its outermost vk_unlock calls as it lets the
   lock go (vk_release_events). Signaling takes the kernel's lock, and a thread holding that may wait in a trap on
   GPU-owned memory, which is answered under the renderer's lock (memory/own.c) */
void syncpt_add_quiet(u32 id, u32 n);
/* the event slots this thread's syncpt_add_quiet increments reached, signaled: with no renderer's lock held. Nothing
   when there were none since the last call */
void syncpt_events(void);
/* A channel promises the next n increments it will make, in order. Returns the value they reach: the fence the ioctl
   hands back, and what nvhost-ctrl reads as the maximum */
u32 syncpt_promise(u32 id, u32 n);
/* 1 once id reaches thresh (across the 32-bit wrap), 0 when the time runs out. 0 looks once, negative waits for good */
int syncpt_wait(u32 id, u32 thresh, i64 timeout_ns);
/* Runs fn(arg) on the GPU thread after the submissions that promised the fences (id[i], val[i]), i < n <= 4. A fence
   nothing queued has promised is waited for here instead, for at most 1 s, and its id[i] is set to ~0 */
void nv_gpu_call_after(u32 n, u32* id, const u32* val, void (*fn)(void*), void* arg);
/* ---- HLE state in g_hle (hle.h) */
#define NVMAP_HANDLES 8192 /* nvmap's handles, numbered 1..8191 */
#define NV_FDS        256  /* nvdrv's fds, numbered 1..255 */

/* The GPU's page table (gmmu_*), Eden's Tegra::MemoryManager geometry: a 40-bit GPU address space
   (address_space_bits, the characteristics' gpu_va_bit_count 0x28) in 4 KiB pages (page_bits), looked up in two
   levels. A second-level table holds 2^GMMU_L2_BITS page entries (16 MiB of GPU addresses), and the first level the
   2^16 tables the remaining bits name */
#define GMMU_ADDRESS_SPACE_BITS 40
#define GMMU_PAGE_BITS          12
#define GMMU_PAGE_SIZE          (1ull << GMMU_PAGE_BITS)
#define GMMU_PAGE_MASK          (GMMU_PAGE_SIZE - 1)
#define GMMU_L2_BITS            12
#define GMMU_L2_ENTRIES         (1u << GMMU_L2_BITS)
#define GMMU_L1_BITS            (GMMU_ADDRESS_SPACE_BITS - GMMU_PAGE_BITS - GMMU_L2_BITS)
#define NV_GMMU_L1              (1u << GMMU_L1_BITS) /* the first level's entries */
#define GMMU_PAGES              (1ull << (GMMU_ADDRESS_SPACE_BITS - GMMU_PAGE_BITS)) /* pages in the space */

/* ctrl fds with event slots in use at once. Each NvRm user (NVN, a movie player, the video decoder) opens one: games
   hold a handful, one game two */
#define NV_CTRL_TABLES 64
/* a ctrl fd's event slots (Eden's MaxNvEvents) */
#define NV_CTRL_EVENT_SLOTS 64u
/* a channel fd's QueryEvent events, by id: 1 SM exception, 2 SM pause, 3 error notifier (Ryujinx's numbering), 0
   unused */
#define NV_CHANNEL_EVENTS 4u

/* an nvmap handle (nvh, by handle number). CREATE and each FROM_ID take a reference, FREE gives one back; a GPU
   mapping pins it, as the console's nvmap does, and the last reference and pin ends it. allocated: ALLOC gave it guest
   memory at addr */
typedef struct {
    u64 addr;
    u32 size, align, heap, flags;
    u32 id;
    u32 refs, pins;
    int alive, allocated;
    u8 kind;
} NvMapH;
/* what MAP_BUFFER_EX mapped, for UNMAP_BUFFER: [gva, gva+size) maps cpu of nvmap handle h. base is the address
   UNMAP_BUFFER names; a modify (NVHOST_AS_MAP_REMAP) or FREE_SPACE splits a mapping into pieces that keep it. va is the
   VA the mapping took from the allocator (0 when the game placed it), given back when it is unmapped */
typedef struct {
    u64 gva, size, cpu;
    u64 base, va;
    u32 h;
} GMap;
typedef struct {
    u64 a, e;
} VaRange;
/* a run of the GPU page table: its pages [gpage, gpage + pages) show guest pages from cpage on */
typedef struct {
    u64 gpage, pages, cpage;
} GRun;
/* gmmu_aliases' index of the runs by guest page, built from them on its first query after a change. The guest pages
   where a run starts or ends cut guest memory into nbound - 1 intervals, interval i [bound[i], bound[i + 1]), and
   cover[first[i]] to cover[first[i + 1]] are the runs over all of it, so a query walks the intervals its range
   meets. seen[r] is the query that last reported run r, so a run over several intervals is reported once. One block
   from bound, cap bytes */
typedef struct {
    u64* bound;
    u32* first; /* nbound + 1 entries */
    u32* cover;
    u32* seen;  /* one per run */
    size_t cap;
    u32 nbound, query;
    int stale; /* the runs changed since the index was built */
} GAlias;
/* an nvhost-ctrl event slot. ev is made on first use and nvdrv keeps a reference. An armed slot is signaled once
   syncpoint id reaches thresh; armed changes under nvev_lk */
typedef struct {
    u32 ev; /* its kernel object id */
    u32 id, thresh;
    int armed;
} NvEvent;
/* a ctrl fd's event slots (ctrl_evt, a pool under nvev_lk) */
typedef struct {
    u32 fd; /* 1 + the fd whose slots these are, set while it's taken */
    NvEvent slot[NV_CTRL_EVENT_SLOTS];
} NvEventTable;
/* an fd (nvf, by number) */
typedef struct {
    int dev;       /* a DEV_* value, DEV_NONE while the entry is free */
    u32 syncpt;    /* DEV_GPU: the channel's syncpoint, 0 until its first use */
    u64 user_data; /* DEV_GPU: SET_USER_DATA's value, for GET_USER_DATA */
    /* QueryEvent's events of a channel (NV_CHANNEL_EVENTS), made on first query and never signaled: nothing here
       raises them */
    u32 ev[NV_CHANNEL_EVENTS];
    struct H1Ch* h1x; /* DEV_NVDEC, DEV_VIC: the fd's host1x channel (host1x_open) */
    /* the ioctls running on the fd. Close during one only marks it closed, and the last ioctl to leave releases it:
       the entry and its channel stay while an ioctl uses them, and Open doesn't hand the fd out again meanwhile */
    u32 users;
    int closed;
} NvFile;
/* a (device, ioctl) pair logged already */
typedef struct {
    int dev;
    u32 cmd;
} NvSeen;
/* nv_init sets the two fields that don't start at 0 (nvh_next, big_page_bits) and the locks */
typedef struct {
    NvMapH nvh[NVMAP_HANDLES];
    u32 nvh_next; /* handles below this have been handed out */
    /* freed handles, handed out again before nvh_next grows: movies create and free buffers for every clip, and a
       handle past the table would be written outside it */
    u32 nvh_free[NVMAP_HANDLES];
    u32 nvh_nfree;
    u64* gmmu[NV_GMMU_L1]; /* gmmu[i] -> GMMU_L2_ENTRIES page entries (nvdrv.c's GMMU_PTE_*) */
    GMap* gmaps;
    u32 ngmaps, capgmaps;
    /* the GPU page table again as runs, sorted by GPU page and disjoint, changed with it (gmmu_map, gmmu_unmap: every
       mapping, MAP_BUFFER_EX's, REMAP's and FREE_SPACE's, goes through them), and their index by guest page. Both
       under alias_lock, which is taken last: inside aslock and the renderer's mapping bracket */
    GRun* gruns;
    u32 ngruns, capgruns;
    GAlias alias;
    u32 big_page_bits; /* 64K (AS_BIG_PAGE_BITS_64K) until ALLOC_AS_EX asks otherwise */
    VaRange* vafree;
    u32 nvafree, capvafree;
    int va_ready; /* the free ranges hold the whole region at first (va_init) */
    u32 syncpt[SYNCPT_IDS];
    u32 syncpt_max[SYNCPT_IDS];
    u8 syncpt_used[SYNCPT_N];
    u8 syncpt_gpu[SYNCPT_N]; /* a GPU channel's: submissions promise its increments (syncpt_max), the GPU thread makes
                                them (sync_lk) */
    /* each ctrl fd's event slots, a table of ctrl_evt taken on first use and given back with the fd, under
       nvev_lk: 1 + its index, 0 for none. As on the console, the slots are the fd's (Ryujinx's NvHostCtrlDeviceFile
       keeps them per open). A game's GPU fence waits (one fd) and its movie player (another) both use slot 0,
       and with one table between them a thread resetting its event took the other's wake, which then waited for
       good */
    u32 ctrl_ev[NV_FDS];
    NvEventTable ctrl_evt[NV_CTRL_TABLES];
    u32 ctrl_evt_next;
    /* a game thread arms a slot while GPU and video threads run nv_events_check after their increments: syncpt_add's
       at once, syncpt_add_quiet's at the outermost vk_unlock, after vk_lock goes (syncpt_events). Arming clears the
       event first and does both in one hold: an increment between a separate arm and clear would signal and then
       lose its wakeup, leaving the game waiting for good. A mutex, not a spin: kevent_signal runs under it */
    NvFile nvf[NV_FDS];
    NvSeen seen[256];
    u32 nseen;
} HleNvdrv;
/* nvdrv's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex lock;
    PMutex aslock; /* held across ioctl_as: the mappings below and the VA allocator */
    PMutex alias_lock;
    PCond sync_cv;
    PMutex sync_lk;
    PMutex submit_lk; /* a submission's promise and its place in the GPU queue, in the same order */
    PMutex nvev_lk;
    /* nv_first_use's (device, ioctl) pairs, under seen_lock */
    PMutex seen_lock;
} HostNvdrv;
extern const StateFnTable g_nvdrv_state_fns; /* the functions a save state may hold of nvdrv.c */

#endif
