/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest memory the GPU holds (own.c, docs/GPU_OWNERSHIP.md): what a GPU engine wrote stays in its GPU copy, and the
   guest's own pages under it are trapped, so a CPU access lands the copy first */
#ifndef SNX_MEMORY_OWN_H
#define SNX_MEMORY_OWN_H
#include "common/cpu/aarch64/cpu.h"

/* guest bytes [addr, addr + size) whose contents are in the producer's resource res, from res_off on */
typedef struct {
    u64 addr, size;
    void* res;
    u64 res_off;
} OwnRange;

/* the producer's side (the renderer's): what owning, landing and letting go of a resource does */
typedef struct {
    /* the n ranges' contents from their resources written to dst[i] (host memory that isn't trapped), done when it
       returns: it waits for the GPU. Called with the producer's lock held (lock) and none of own.c's, on any thread */
    void (*land)(const OwnRange* r, u8* const* dst, u32 n);
    void (*retain)(void* res);
    void (*release)(void* res); /* may take the producer's lock */
    /* the thread (plat_tid) holding the producer's lock, or 0: its fault on an owned page can't be answered (the
       landing needs that lock), and ends the process; one on a page no entry is on any more goes on */
    u32 (*busy_tid)(void);
    /* the producer's lock (the renderer's vk_lock), reentrant: a landing takes it before it marks its entries, and
       every own_begin and own_drop caller holds it */
    void (*lock)(void);
    void (*unlock)(void);
    /* the host pages [addr, addr + size) of a landing, whose bytes (size of them) are as it left them, read before
       the pages are let go of: the write tracking, which takes pages it watches again for written, can tell a later
       write from them. Called with the producer's lock held. Optional */
    void (*landed)(u64 addr, u64 size, const u8* bytes);
} OwnHooks;

/* 0 when the host can trap guest memory (plat_trap_init): own_begin can work from then on. -1: producers land their
   results in guest memory at their own fence */
int own_init(const OwnHooks* h);
/* [addr, addr + size) is res's from res_off on, from now on: older owners of those bytes let go of them. 0, or -1
   when the range can't be trapped (no traps, memory that isn't mapped or is mapped somewhere else too): the producer
   then lands it itself. The caller holds the producer's lock (the renderer's, which host memory changes take) */
int own_begin(u64 addr, u64 size, void* res, u64 res_off);
/* the owned parts of [addr, addr + size), in address order and clipped to it, into out: how many there are (more
   than max when they didn't all fit). With the producer's lock held, as the resources live while it is */
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max);
/* the CPU's bytes of [addr, addr + size) copied to dst without touching a trapped page (read through the pool's
   mirror), and its owned parts (clipped, in address order) in out, whose bytes in dst are stale: the caller copies
   them from their resources over dst there. How many owned parts there are; more than max when they didn't fit, and
   nothing was copied then. With the producer's lock held, as own_lookup */
u32 own_stage(u64 addr, u64 size, u8* dst, OwnRange* out, u32 max);
/* change detection over [addr, addr + size) without touching it: 1 when GPU-owned bytes are on its pages, which must
   then not be read (they are trapped), with the owners' identity mixed into *h (FNV-1a steps). A new owner changes
   it, and so does the CPU touching the bytes (which ends the ownership). 0, *h unchanged, when nothing is owned there */
int own_hash(u64 addr, u64 size, u64* h);
/* the CPU is about to touch [addr, addr + size): what the GPU holds of the pages it is on lands now, and they are the
   CPU's again. The runtime's own code calls it before it hands guest memory to the kernel (a system call fails on
   trapped pages) and on any thread that holds the producer's lock */
void own_acquire(u64 addr, u64 size);
/* [addr, addr + size)'s GPU contents don't matter any more (unmapped, or about to be overwritten whole): let go of
   without landing */
void own_drop(u64 addr, u64 size);
/* landings so far (each a wait for the GPU: the statistics count them), and whether own_init found traps */
u64 own_landings(void);
int own_traps(void);
/* whether anything is owned: the common case's test, without a lock */
extern int g_own_any;

#endif
