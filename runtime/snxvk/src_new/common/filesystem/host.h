/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the indexed filesystem needs from the host, shared by the NX and CTR runtimes: a mutex, folders, and files
   kept mapped read-only for the whole run. No guest type enters here, so sizes are <stdint.h>'s */
#ifndef SNX_FS_HOST_H
#define SNX_FS_HOST_H
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
/* A mutex with a static initializer on both hosts; the member is l on both */
typedef struct {
    SRWLOCK l;
    void* tracy; /* the profiler's context for a named lock (pm_lock_announce), null otherwise */
} PMutex;
#define PMUTEX_INIT {SRWLOCK_INIT, NULL}
/* the UTF-8 path as a malloc'd wide string for the W calls; NULL when it isn't valid UTF-8 (or out of memory) */
wchar_t* snx_host_wpath(const char* text);
#else
#include <pthread.h>
typedef struct {
    pthread_mutex_t l;
    void* tracy; /* the profiler's context for a named lock (pm_lock_announce), null otherwise */
} PMutex;
#define PMUTEX_INIT {PTHREAD_MUTEX_INITIALIZER, NULL}
#endif

void pm_init(PMutex* m);
void pm_destroy(PMutex* m);
void pm_lock(PMutex* m);
void pm_unlock(PMutex* m);
int pm_trylock(PMutex* m); /* 1 when it took the lock, 0 when another thread holds it */
/* The profiler's view of the locks, registered while a run has one attached: pm_lock, pm_unlock and pm_trylock
   report the named locks through it, and its pane shows who held which and who waited how long. Nothing registers
   it on a run without the profiler, so a lock unnamed costs a null check. The reporter's context is the raw
   profiler lock; the admission (who reports on a lock at all) is host.c's, which owns the mutex. */
typedef struct {
    void* (*make)(const char* name);
    void (*free)(void* l);
    void (*wait)(void* l);
    void (*lock)(void* l);
    void (*unlock)(void* l);
    void (*try_lock)(void* l, int acquired);
} PmTracy;
void pm_tracy_register(const PmTracy* t);
void pm_lock_announce(PMutex* m, const char* name); /* names a lock; from then on its locks report */
/* A thread blocked in pm_lock on a mutex another thread holds, or waiting on a condition variable with it (cond 1), for
   a hang report: the host thread, the mutex and the caller's address. A lock taken at once notes nothing */
typedef struct {
    uint64_t tid;
    const void* m;
    const void* at;
    int cond;
} PmWait;
void pm_wait_note(const void* m, const void* at, int cond); /* m NULL: the wait is over */
int pm_waits(PmWait* out, int max);                          /* the waits now, at most max */
const void* pm_wait_base(void); /* an address in this module's data, to name a mutex by its offset from */

int plat_mkdir(const char* path); /* 0, or -1 */

/* Opens a read-only file for the rest of the run and maps it shared, so its pages are the host's page cache, read on
   first touch. size is the file's. An empty file keeps *fd with *data NULL. 0, or -1 with *fd = -1 and *data = NULL
   (it can't be opened or mapped, it is too large to map, or keeping it would leave too few descriptors for the files
   opened on demand) */
int plat_file_share(const char* path, uint64_t size, int* fd, const uint8_t** data);
void plat_file_unshare(int fd, const uint8_t* data, uint64_t size);
#endif
