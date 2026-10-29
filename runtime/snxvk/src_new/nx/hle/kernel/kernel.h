/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The emulated Horizon kernel as the rest of the runtime sees it: kernel objects and their handles, threads and the
   guest cores they run on, events, shared memory. The kernel's own files share kernel_int.h */
#ifndef SNX_KERNEL_H
#define SNX_KERNEL_H
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/arm/arm.h"
#include "nx/runtime/platform/plat.h"

/* the one application process's id, which IPC PID descriptors carry too */
#define K_PROCESS_ID 0x51ull
#define NCORES       4 /* guest CPU cores */
#define THREAD_MAX   96 /* the application's ThreadCountMax: creating one more answers R_LIMIT */
#define H_MAX        1024 /* handles: the most the console's handle table holds */

/* what a kernel object is (KObj.type) */
enum {
    KO_NONE,
    KO_EVENT,
    KO_THREAD,
    KO_SESSION,
    KO_PORT,
    KO_SHMEM,
    KO_TMEM,
    KO_PROCESS,
    KO_RESLIMIT,
    KO_CODEMEM,
    KO_SERVER,
    KO_WEVENT /* the writable end of an svcCreateEvent pair */
};

typedef struct KThread KThread;

/* KThread.park and wait_svc */
enum { PARK_RUN, PARK_ENTER, PARK_WAIT, PARK_SERVICE };
enum { WAIT_SVC_SLEEP, WAIT_SVC_SYNC, WAIT_SVC_LOCK, WAIT_SVC_CV, WAIT_SVC_ADDR, WAIT_SVC_SETTLE };

/* Every kernel object starts with a KObj, as its first member: code holding a KObj* casts it to the object its type
   names (KEvent, KThread, KShmem, KTmem, KSession), so the KObj must sit at offset 0 of each. What an object does as
   its last reference goes, and whether it can be waited on and is signaled, follow from its type (kernel.c's
   kobj_destroy and kobj_signaled). The threads waiting on it name it in their waits (KThread.waits) */
typedef struct KObj {
    int type;
    int refs;      /* changed with __atomic operations; the object is destroyed as it reaches 0 */
    int nwait;     /* the waits that name it now, under g_klock: none, and a signal looks for no waiter */
    char name[24]; /* for the logs and the thread dump */
} KObj;

typedef struct KEvent {
    KObj o;
    int signaled; /* stays set until cleared: Horizon's events don't clear themselves */
} KEvent;

/* A guest thread. It holds a guest core while it runs game code and lets go of it only where a console thread would
   be off the CPU (a blocking SVC, a sleep or yield, an IPC call, WFE). It takes the core back on its way out of the
   SVC, never while holding g_klock */
struct KThread {
    KObj o;
    CPU cpu;
    SnxArmCPU arm; /* a 32-bit game's threads run here, their SVCs passing through cpu's registers */
    u64 id;
    int prio;       /* effective: base_prio raised by the waiters on the mutexes it owns */
    int base_prio;  /* as the game set it */
    int core;       /* the core it is scheduled on, 0 to NCORES - 1 */
    int ideal_core; /* as set, -1 for none */
    u32 yields;     /* consecutive yields, of which only some reach the host */
    int on_core;    /* the core it holds, -2 while waiting for one, -1 for none */
    int pinned;     /* the core whose host CPUs its host thread runs on (core_pin), -1 before it first ran */
    u32 core_next;  /* the core's wait list (kt_at) */
    u64 core_ticket;    /* arrival order there, which breaks priority ties */
    u64 affinity;
    u64 tls;
    u64 entry, arg, stack_top;
    int state; /* 0 created, 1 running, 2 exited */
    int woken, cancel;
    /* svcSetThreadActivity: suspended and in_svc (inside an SVC, running no game code) are read with __atomic loads
       without the lock. pause_at is where a paused thread waits: 0 blocked inside the SVC or not paused, 1 before
       the SVC, 2 after it */
    int suspended, in_svc, pause_at;
    /* what a game pause (kernel_hold) counts on, with __atomic stores: 1 blocked in a wait that runs nothing until it
       ends (sleep_nl, kthread_wait, a JIT compile), 2 stopped at a hold point, 0 neither */
    int stopped;
    /* where it stopped, which is how a thread a save state restarts goes on (thread_resume): PARK_RUN at its pc,
       PARK_ENTER the SVC before its pc again (it hadn't begun), PARK_WAIT the rest of its SVC's wait (wait_svc), and
       PARK_SERVICE the SVC again, as its request (a service's wait, kthread_wait) never got its answer */
    int park;
    int wait_svc; /* the SVC whose wait sleep_nl blocks in: WAIT_SVC_* */
    u64 wait_end; /* that wait's deadline in guest time (guest_ns), 0 for none */
    /* what an IPC request it is in holds: its session's reference (kobj_id) and an nvdrv ioctl's use of its fd (the fd
       plus 1), which a request a restored thread makes again lets go of first (svc_service_retry) */
    u32 req_session, req_nvfd;
    int wait_kind; /* 0 none, 1 objects, 2 mutex, 3 condition variable, 4 address, 5 being paused */
    /* kinds 1 and 5: the objects it waits on (kobj_id), as many times as it named each; kind 1's wait_key is the one
       whose signal woke it, 0 before */
    u32 waits[64];
    int nwaits;
    u64 wait_addr, wait_key;
    u32 wait_tag;
    u32 lock_owner; /* the mutex's owner while it waits on one (kind 2), as kt_ix numbers it */
    int wait_result;
    u32 next_waiter; /* its wait list's next thread (kt_at) */
    char name[32];
    char wait_name[24];
    u64 wait_tid; /* the thread a join waits for, for the thread dump */
    i64 wait_timeout;
    u64 sleep_lr, sleep_n, sleep_x[31]; /* where a sleep loop sleeps from, for diagnostics */
};

typedef struct KShmem {
    KObj o;
    u64 size;
    u8* host;
    u64 pool;         /* the alias pool's offset of its pages plus 1 (host maps them), 0 when host is a copy */
    u64 gaddr, gsize; /* without the pool: where the game mapped it and how much; gaddr is 0 when unmapped and read
                         atomically */
    u32 perm;
} KShmem;

typedef struct KTmem {
    KObj o;
    u64 addr, size;
    u32 perm;
} KTmem;

void kernel_init(void);
/* memory_fit's measure, as the game starts: what the system can still give the process (available) and what the process
   holds (footprint), when measured says the system told both. The game is told no more than mem_fit_budget's */
void mem_fit(int measured, u64 available, u64 footprint);
u64 mem_fit_budget(u64 available, u64 footprint);
u32 h_new(KObj* o); /* a new handle to o, which takes a reference */
/* an object as a number, which a save state can hold: its type and its slot in its type's pool, 0 for none. kobj_at
   is the object again, NULL for 0 */
u32 kobj_id(const KObj* o);
KObj* kobj_at(u32 id);
/* a new object's name, cut to fit */
static inline void kobj_set_name(KObj* o, const char* name) {
    size_t i;
    for (i = 0; name && name[i] && i + 1 < sizeof o->name; i++) o->name[i] = name[i];
    o->name[i] = 0;
}
void kobj_ref(KObj* o);
void kobj_unref(KObj* o);
/* whether a mapping of the memory (a MapMemory view, a transfer memory or a code memory) holds [addr, addr + size)
   locked: nvmap's FREE reports no memory for such a range, as the console's page table pins what is mapped */
int svc_mem_range_locked(u64 addr, u64 size);
/* the object h names, of type t (0 for any), with a reference the caller drops with kobj_unref. NULL when none */
KObj* h_get_ref(u32 h, int t);

KEvent* kevent_new(const char* name);
void kevent_signal(KEvent* e);
void kevent_clear(KEvent* e);

KThread* kthread_cur(void);
/* Taking and releasing the thread's core. core_poll is where RtAPI's poll gives it up to a waiting thread, when
   generated code finds CPU.poll set at a backward branch (a loop with no SVC) */
void core_enter(KThread* t);
void core_leave(KThread* t);
void core_poll(KThread* t);
/* A game pause (pause.c): kernel_hold stops every guest thread where it runs nothing (at a hold point, or blocked in a
   wait), 0 once they all are, -1 (after logging the late thread, and letting the others go again) when one isn't within
   timeout_ms. kernel_release lets them go on. The frontend's thread calls both */
int kernel_hold(int timeout_ms);
void kernel_release(void);
/* a guest thread's hold point: off its core while the game is paused, park saying how it would go on (KThread.park).
   1 when it stopped, and the caller takes its core again */
int kthread_hold_point(KThread* t, int park);
/* A save state's load (state.c), with the game held: kernel_retire ends every guest thread's host thread where it
   stopped, 0 once they have, -1 when some are left after timeout_ms. Then, g_hle restored, kthread_resume starts a
   new host thread for each thread that had started, which goes on as it stopped (KThread.park) when the game is let
   go */
int kernel_retire(int timeout_ms);
void kthread_resume(KThread* t);
/* pc_wait for a guest thread blocked in a service on the service's own lock m: it counts as stopped for a game pause,
   and a wake during one waits for the pause to end, m let go of meanwhile (returning 0, as a spurious wake). A save
   state taken then makes the request again on load, which is only right while the service has changed nothing for
   it. Once it has (kthread_commit), the wait is a plain one: a pause waits for the request to finish instead. Another
   thread just waits */
int kthread_wait(PCond* c, PMutex* m, i64 timeout_ns);
/* the request the calling thread serves has changed what a service holds (vi's QueueBuffer queued its slot): it can't
   be made again, so it finishes before a game pause stops the thread. Until the thread's SVC leaves */
void kthread_commit(void);
/* around a guest thread's wait for the host off its core (a JIT compile): stopped as far as a game pause goes, and its
   end a hold point */
void kthread_block(KThread* t);
void kthread_unblock(KThread* t);
void kernel_run_main(u64 entry, u64 stack_size, int prio, int core);
/* SVC #imm of a 64-bit thread (RtAPI.svc) and of a 32-bit one: g_svc64[imm] and svc.c's g_svc32[imm], each SVC's
   own entry, which takes its registers in that ABI (svcgen, hle/svc/hos.svc). The NX modules call g_svc64 by number
   through RtAPI.svcs */
void snx_svc(CPU* c, u32 imm);
void snx_svc32(SnxArmCPU* a, u32 imm);
extern void (*const g_svc64[SNX_SVC_COUNT])(CPU* c, u32 imm);

KShmem* kshmem_new(u64 size, const char* name);
u8* kshmem_ptr(KShmem* s);

/* once the game's modules are loaded, for the dumps and kernel_find_symbol_exact */
void kernel_load_symbols(void);
u64 kernel_find_symbol_exact(const char* name); /* 0 when no loaded module exports this name */
void kernel_dump_regs(CPU* c);
void kernel_dump_threads(void);
void mem_report(char* out, int n);

/* g_klock, the kernel lock, is g_host.kernel.klock (hle.h): every list a waiter or signaller touches is under it */
/* svc_mem.c, for RtAPI.code_changed (runtime/rtapi.c): IC IVAU of the 64-byte line at addr, in a view of code
   (svcMapProcessMemory) or in the code a view shows. Its code address into *code, and 1 when its bytes differ from
   what the view kept of them (which then takes them), 0 when they are the same. -1 for a line no view knows, which may
   have changed */
int svc_code_line(u64 addr, u64* code);

#endif
