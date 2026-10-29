/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The operating system under the runtime. plat_sdl.c implements what SDL does the same way everywhere (the clocks,
   sleeping, libraries, path queries), and one of plat_posix.c and plat_win32.c the rest (threads, condition variables,
   the guest's address space, file maps, the crash handler); profiler_win32.c samples threads on Windows, and the host
   audio output lives in the window backend. Paths are UTF-8 everywhere. */
#ifndef SNX_PLAT_H
#define SNX_PLAT_H
#include "common/cpu/aarch64/cpu.h"
#include "common/filesystem/host.h" /* PMutex and the pm_* calls */
#include <stddef.h>
#include <stdio.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX /* or windows.h defines min and max as macros, which any later use of those names trips over */
#endif
#include <windows.h>
#endif

/* ---- condition variables, used with a PMutex */
typedef struct {
#if defined(_WIN32)
    CONDITION_VARIABLE c;
#else
    pthread_cond_t c;
#endif
} PCond;
void pc_init(PCond* c);
void pc_signal(PCond* c);
void pc_broadcast(PCond* c);
/* m held. timeout_ns < 0 waits without limit. Returns 1 when the time ran out, 0 when woken (or spuriously) */
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns);

/* ---- threads */
typedef void (*PThreadFn)(void* arg);
/* A detached thread running fn(arg) on a stack of that many bytes (0: the system's default). 0, or -1 when it
   could not be started */
int plat_thread(PThreadFn fn, void* arg, size_t stack);
void plat_thread_exit(void); /* ends the calling thread (one plat_thread started) */
/* the host CPUs the process may run on, at most max of them, into ids; 0 where the runtime doesn't choose them */
int plat_cpus(int* ids, int max);
/* the calling thread runs only on the n host CPUs in ids */
void plat_thread_cpus(const int* ids, int n);
/* the host for the log's header, so a report needn't describe it: "<OS and version>, <CPU>, <n> threads, <n> GB RAM",
   the CPU left out where the host doesn't name it. Nothing that names the machine or its user */
void plat_system_info(char* out, size_t cap);
void plat_yield(void);
void plat_sleep_ns(i64 ns); /* 0 or less yields */
/* Ends the process with code at once: stdio is flushed, but atexit handlers don't run and other threads are left
   where they are */
void plat_exit_now(int code) __attribute__((noreturn));
/* the guest thread a host thread runs (hle/kernel/thread.c sets it) */
extern __thread void* tls_thread;

/* ---- time */
u64 plat_ns(void);        /* a monotonic clock in nanoseconds */
i64 plat_unix_time(void); /* seconds since 1970 UTC */
i64 plat_tz_offset(void); /* local time minus UTC, in seconds */

/* ---- the guest's address space */
/* The host's page, which the reservation is committed, decommitted and aliased in: 4 KiB, the Switch's own, on x86-64
   and most AArch64 systems, 16 KiB on Apple silicon and some Android kernels, 64 KiB on most PowerPC64 and some
   AArch64 distributions (mem.c). SNX_HOST_PAGE in the environment, a larger power of two up to 1 MiB, makes a host
   with smaller pages work in those, to test the larger pages' paths on any machine */
u64 plat_vm_page(void);
u8* plat_vm_reserve(u64 size); /* aligned to plat_vm_page, inaccessible until committed; NULL, the reason in errno */
/* The process's address space now: what is mapped or reserved in all, and its largest free run between them, for a
   run that fails for want of it (a thread's stack, a mapping). Whether the system told it (Linux, macOS, iOS and
   Windows do) */
int plat_address_space(u64* mapped, u64* largest_free);
int plat_vm_commit(u8* p, u64 size);       /* readable and writable; 0, or -1 */
void plat_vm_decommit(u8* p, u64 size);    /* inaccessible again, and zero when committed next */
/* Calls cb for each run of pages in [p, p + size) written since they were last scanned, and watches them again.
   -1 when the host can't tell (the caller then compares contents). Linux uses userfaultfd's asynchronous write
   protection with PAGEMAP_SCAN, Windows the write watch of the reservation; SNX_NO_WW in the environment turns both
   off */
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size));
/* The alias pool: one host memory object whose pages can be mapped at several addresses of the reservation at once,
   and outside it, so two guest addresses (MapMemory's view and its source, code memory's owner and slave, shared
   memory and its service) are the same pages. Linux and Android (a memfd); elsewhere plat_alias_init says -1 and
   mem.c copies instead. Offsets and sizes are whole 4 KiB pages, and mem.c hands the offsets out. mem.c only uses the
   pool with 4 KiB host pages: a view of other pages needs its address and its pool offset at the same place in a host
   page, and the game picks its addresses in 4 KiB steps */
int plat_alias_init(u64 size); /* 0, or -1 when the host has no pool */
/* the pool's [off, off + size) at p in the reservation, readable and writable, written-page tracking kept: 0, or -1 */
int plat_alias_map(u8* p, u64 off, u64 size);
/* p back to the reservation's own memory, decommitted as plat_vm_decommit leaves it: 0, or -1 */
int plat_alias_unmap(u8* p, u64 size);
/* copies between host memory and the pool: 0, or -1 */
int plat_alias_write(u64 off, const void* src, u64 size);
int plat_alias_read(u64 off, void* dst, u64 size);
void plat_alias_free(u64 off, u64 size); /* the range reads as zero again and uses no memory */
/* the pool's range mapped outside the reservation (a service's side of shared memory): NULL on failure */
u8* plat_alias_host(u64 off, u64 size);
void plat_alias_host_free(u8* p, u64 size);
/* the whole pool mapped once outside the reservation, never trapped: what a trap's handler writes the GPU's data
   through before the trapped pages open (memory/own.c). NULL without a pool */
u8* plat_alias_mirror(void);

/* Traps on pool pages of the reservation (memory/own.c, docs/GPU_OWNERSHIP.md): an access to an armed page, by any
   thread, waits while fault (on a thread of the trap's own, never in a signal handler) gets the page's contents into
   the pool and lets it go on with plat_trap_continue. Linux 5.13 and later (userfaultfd minor faults on shared
   memory); elsewhere plat_trap_init says -1. An access from the kernel (a system call given a pointer into an armed
   page) fails with EFAULT instead of waiting, so the runtime opens a range before it hands it to the kernel. tid is
   the faulting thread's plat_tid */
int plat_trap_init(void (*fault)(u8* p, int write, u32 tid));
u32 plat_tid(void); /* the calling thread's id, as the system numbers threads */
/* whether this process's thread tid (plat_tid's) is waiting in a trap now: 1, 0, or -1 when that can't be told */
int plat_thread_in_fault(u32 tid);
/* Best-effort fatal diagnostics for the blocked thread, not the trap handler. Never resumes it. */
void plat_dump_fault_thread(u32 tid);
/* every page of [p, p + size), whole host pages of pool memory, armed / let go (0, or -1) */
int plat_trap_arm(u8* p, u64 size);
int plat_trap_continue(u8* p, u64 size);
/* [p, p + size) not trapped any more: a page not yet let go reads the pool's contents when it is touched */
void plat_trap_disarm(u8* p, u64 size);
/* [p, p + size) read only while it is copied somewhere else: a write to it, from any thread, waits for plat_thaw and
   is done then (to whatever is mapped there by then). One range at a time */
void plat_freeze(u8* p, u64 size);
void plat_thaw(void);

/* ---- shared libraries */
void* plat_dl_open(const char* path);
void* plat_dl_sym(void* h, const char* name);
const char* plat_dl_error(void); /* the reason the last plat_dl_open or plat_dl_sym returned NULL */
int plat_setenv(const char* name, const char* value); /* for this process and what it loads; 0 on success */

/* ---- files */
int plat_is_dir(const char* path);
/* Reads size bytes at off without moving f's position. The count read, short only at the end of the file, or -1 */
i64 plat_pread(FILE* f, void* buf, u64 size, u64 off);
typedef struct {
    const u8* data; /* NULL for an empty file */
    u64 size;
    int borrowed; /* set by fs_map_file for a view kept for the whole run: plat_file_unmap then only clears this */
    int heap;     /* set by fs_map_file for a file read out of the game's dump into memory: plat_file_unmap frees it */
} PFileMap;
/* the whole file mapped read-only. 0 (an empty file too), or -1 */
int plat_file_map(PFileMap* m, const char* path);
/* the same for a file whose length is already known from an index: size is trusted, the host isn't asked */
int plat_file_map_sized(PFileMap* m, const char* path, u64 size);
void plat_file_unmap(PFileMap* m);
/* Moves tmp over dst atomically, so dst is always either the old or the new file. On failure dst is untouched and
   tmp deleted: 0, or -1 */
int plat_replace_file(const char* tmp, const char* dst);

/* Host audio output of interleaved signed 16-bit samples, little-endian as the guest's are, implemented by the window
   backend. plat_audio_open gives a paused stream, or NULL when there is no device (always when headless).
   plat_audio_queue takes whole frames (SDL refuses a call ending in part of one) and copies them before it returns, so
   the guest may reuse its buffer at once */
typedef struct PAudio PAudio;
PAudio* plat_audio_open(u32 rate, u32 channels);
void plat_audio_close(PAudio* a);
int plat_audio_queue(PAudio* a, const void* samples, u32 bytes);
void plat_audio_start(PAudio* a, int started); /* 1 plays, 0 pauses */
void plat_audio_clear(PAudio* a);
u32 plat_audio_queued(PAudio* a); /* bytes waiting to be played; 0 without a device */
void plat_audio_volume(PAudio* a, float volume);

/* ---- crash reports and profiling */
void plat_install_crash_handler(void);
void plat_prof_start(void); /* starts the sampling profiler (the profile option): Windows only */
#if defined(_WIN32)
void plat_prof_add_thread(HANDLE h); /* a thread to sample; the profiler keeps its own handle */
#endif

#endif
