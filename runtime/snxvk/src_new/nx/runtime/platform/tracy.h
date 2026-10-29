/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Tracy zones, plots and memory events; compiled out without SNX_TRACY. */
#ifndef SNX_TRACY_H
#define SNX_TRACY_H

#include <stddef.h>
#include <stdint.h>

#if defined(SNX_TRACY)
#include <tracy/TracyC.h>

/* Every emission passes this gate; shutdown closes it. */
int snx_tracy_enter(void);
void snx_tracy_leave(void);
TracyCZoneCtx snx_tracy_zone_begin(const struct ___tracy_source_location_data* srcloc);
void snx_tracy_zone_end(TracyCZoneCtx ctx);
static inline void snx_tracy_zone_end_at_scope(TracyCZoneCtx* ctx) { snx_tracy_zone_end(*ctx); }

/* The zone ends at the end of the block it opens in, a return included; one that ends earlier calls SNX_ZONE_END. */
#define SNX_ZONE(ctx, name)                                                                                            \
    static const struct ___tracy_source_location_data TracyConcat(snx_tracy_source_, TracyLine) =                     \
        {name, __func__, TracyFile, (uint32_t)TracyLine, 0};                                                           \
    TracyCZoneCtx ctx __attribute__((cleanup(snx_tracy_zone_end_at_scope))) =                                          \
        snx_tracy_zone_begin(&TracyConcat(snx_tracy_source_, TracyLine))
/* a short emission: gated, and skipped when nothing is attached */
#define SNX_TRACY_EMIT(...)                                                                                            \
    do {                                                                                                               \
        if (snx_tracy_enter()) {                                                                                       \
            if (TracyCIsConnected) __VA_ARGS__;                                                                        \
            snx_tracy_leave();                                                                                         \
        }                                                                                                              \
    } while (0)
#define SNX_ZONE_TEXT(ctx, ...) SNX_TRACY_EMIT(TracyCZoneTextF(ctx, __VA_ARGS__))
#define SNX_ZONE_VALUE(ctx, value) SNX_TRACY_EMIT(TracyCZoneValue(ctx, (uint64_t)(value)))
/* a zone whose text or value is known as it opens */
#define SNX_ZONE_T(ctx, name, ...) SNX_ZONE(ctx, name); SNX_ZONE_TEXT(ctx, __VA_ARGS__)
#define SNX_ZONE_V(ctx, name, value) SNX_ZONE(ctx, name); SNX_ZONE_VALUE(ctx, value)
#define SNX_ZONE_TV(ctx, name, value, ...) SNX_ZONE_V(ctx, name, value); SNX_ZONE_TEXT(ctx, __VA_ARGS__)
#define SNX_ZONE_END(ctx) snx_tracy_zone_end(ctx)
/* a zone in a named color, for the timeline's kinds at a glance */
#define SNX_ZONE_C(ctx, name, color)                                                                                   \
    static const struct ___tracy_source_location_data TracyConcat(snx_tracy_source_, TracyLine) =                     \
        {name, __func__, TracyFile, (uint32_t)TracyLine, (uint32_t)(color)};                                           \
    TracyCZoneCtx ctx __attribute__((cleanup(snx_tracy_zone_end_at_scope))) =                                          \
        snx_tracy_zone_begin(&TracyConcat(snx_tracy_source_, TracyLine))
#define SNX_MEM_ALLOC(ptr, size, pool) SNX_TRACY_EMIT(TracyCAllocN((const void*)(ptr), (size_t)(size), (pool)))
#define SNX_MEM_FREE(ptr, pool) SNX_TRACY_EMIT(TracyCFreeN((const void*)(ptr), (pool)))
#define SNX_FRAME_MARK() SNX_TRACY_EMIT(TracyCFrameMark)
/* a discontinuous frame: the work to the end of the block, one of a named frame set (the audio tick's, say). A
   set's starts and ends pair and do not nest, and a set is either all continuous or all discontinuous. */
typedef struct {
    int started;
    const char* name;
} SnxTracyFrame;
#define SNX_FRAME_SCOPED(ctx, name)                                                                                    \
    SnxTracyFrame ctx __attribute__((cleanup(snx_tracy_frame_at_scope))) = {snx_tracy_frame_start(name), (name)}
#define SNX_PLOT(name, value) SNX_TRACY_EMIT(TracyCPlot(name, value))
/* the name goes into the client's table, which the server asks for when a GUI connects; kept per thread, so a
   thread that named itself before the client started is named again when it attaches */
#define SNX_THREAD_NAME(name) snx_tracy_thread_name(name)

/* The GPU track: a context per queue, the zones' query ids paired with their timestamps at the read-back, the two
   clocks' calibration. A query id of 0 means the emission did not go out: the caller keeps the pair and sends only
   what it got. The context is announced at the caller's first read-back (snx_tracy_gpu_context, which returns 1
   then, so the caller can reset its own calibration anchor); a zone goes out only after its context did. Each
   emission carries the connection it belongs to (*ep at the begin): the client's deferred queue replays the
   contexts at a new connection, and an end or a time from another one than the begin's is dropped, where the
   server would fault on a query it never saw. */
int snx_tracy_gpu_context(int ctx, int64_t gpu_time, float period, int calibrated, const char* name);
uint16_t snx_tracy_gpu_zone_begin(const char* name, uint32_t color, int ctx, uint64_t* ep);
uint16_t snx_tracy_gpu_zone_end(uint16_t query, int ctx, uint64_t ep);
void snx_tracy_gpu_time(int64_t gpu_time, uint16_t query, int ctx, uint64_t ep);
void snx_tracy_gpu_sync(int64_t gpu_time, int ctx);
void snx_tracy_gpu_calibrate(int64_t gpu_time, int64_t cpu_delta, int ctx);
/* The connection's id, 0 when nothing is attached: a calibration baseline and an in-flight pair's epoch are keyed
   on it (a new connection re-anchors both). */
uint64_t snx_tracy_connection_id(void);
/* 1 when the client is up and a server is attached. TracyCIsConnected reads the client's own data, which a run
   that never started one (or one that has shut down) has none of, so the code that asks per frame asks this. */
int snx_tracy_attached(void);
/* SNX_FRAME_SCOPED's pair: 1 when the start went out, so the block's end sends none otherwise */
int snx_tracy_frame_start(const char* name);
void snx_tracy_frame_at_scope(SnxTracyFrame* f);
/* The named locks' events for the profiler's lock pane: the context a PMutex keeps once pm_lock_announce named it.
   The events are single items, each gated on its own, so an unpaired one costs nothing but its absence. */
typedef struct __tracy_lockable_context_data SnxTracyLock;
SnxTracyLock* snx_tracy_lock_make(const char* name);
void snx_tracy_lock_free(SnxTracyLock* l);
void snx_tracy_lock_wait(SnxTracyLock* l);
void snx_tracy_lock_lock(SnxTracyLock* l);
void snx_tracy_lock_unlock(SnxTracyLock* l);
void snx_tracy_lock_try(SnxTracyLock* l, int acquired);
#else
#define SNX_ZONE(ctx, name)      int ctx __attribute__((unused)) = 0
#define SNX_ZONE_C(ctx, name, color) int ctx __attribute__((unused)) = 0
#define SNX_ZONE_TEXT(ctx, ...)  ((void)0)
#define SNX_ZONE_VALUE(ctx, value) ((void)0)
#define SNX_ZONE_T(ctx, name, ...) int ctx __attribute__((unused)) = 0
#define SNX_ZONE_V(ctx, name, value) int ctx __attribute__((unused)) = 0
#define SNX_ZONE_TV(ctx, name, value, ...) int ctx __attribute__((unused)) = 0
#define SNX_ZONE_END(ctx)        ((void)0)
#define SNX_MEM_ALLOC(ptr, size, pool) ((void)0)
#define SNX_MEM_FREE(ptr, pool) ((void)0)
#define SNX_FRAME_MARK()        ((void)0)
#define SNX_FRAME_SCOPED(ctx, name) int ctx __attribute__((unused)) = 0
#define SNX_PLOT(name, value)   ((void)0)
#define SNX_THREAD_NAME(name)   ((void)0)
static inline int snx_tracy_gpu_context(int ctx, int64_t gpu_time, float period, int calibrated,
                                        const char* name) {
    (void)ctx, (void)gpu_time, (void)period, (void)calibrated, (void)name;
    return 0;
}
static inline uint16_t snx_tracy_gpu_zone_begin(const char* name, uint32_t color, int ctx, uint64_t* ep) {
    (void)name, (void)color, (void)ctx;
    if (ep) *ep = 0;
    return 0;
}
static inline uint16_t snx_tracy_gpu_zone_end(uint16_t query, int ctx, uint64_t ep) {
    (void)query, (void)ctx, (void)ep;
    return 0;
}
static inline void snx_tracy_gpu_time(int64_t gpu_time, uint16_t query, int ctx, uint64_t ep) {
    (void)gpu_time, (void)query, (void)ctx, (void)ep;
}
static inline void snx_tracy_gpu_sync(int64_t gpu_time, int ctx) { (void)gpu_time, (void)ctx; }
static inline void snx_tracy_gpu_calibrate(int64_t gpu_time, int64_t cpu_delta, int ctx) {
    (void)gpu_time, (void)cpu_delta, (void)ctx;
}
static inline uint64_t snx_tracy_connection_id(void) { return 1; }
static inline int snx_tracy_attached(void) { return 0; }
static inline int snx_tracy_gate_open(void) { return 0; }
typedef struct SnxTracyLock SnxTracyLock;
static inline SnxTracyLock* snx_tracy_lock_make(const char* name) {
    (void)name;
    return NULL;
}
static inline void snx_tracy_lock_free(SnxTracyLock* l) { (void)l; }
static inline void snx_tracy_lock_wait(SnxTracyLock* l) { (void)l; }
static inline void snx_tracy_lock_lock(SnxTracyLock* l) { (void)l; }
static inline void snx_tracy_lock_unlock(SnxTracyLock* l) { (void)l; }
static inline void snx_tracy_lock_try(SnxTracyLock* l, int acquired) { (void)l, (void)acquired; }
#endif

/* Starts the client when --tracy or --tracy-capture asks for it. Returns 1 when the capture it was asked for is not
   running; the run goes on without one. */
#if defined(SNX_TRACY)
/* 1 while emissions are allowed: closed before the client starts and, again, before it shuts down. tracy_conn reads
   it so TracyCIsConnected is never reached while the client has no data to read (an assert). */
int snx_tracy_gate_open(void);
int snx_tracy_init(const char* title);
void snx_tracy_thread_name(const char* name);
/* F7: starts the client if needed, then a 60-second capture. */
void snx_tracy_capture_now(void);
/* 0 warning, 1 log, 2 verbose. */
void snx_tracy_log(int lvl, const char* text);
/* Closes the client cleanly before plat_exit_now. */
void snx_tracy_shutdown(void);
#else
/* Stubs for tests that link plat.c without tracy.c. */
static inline int snx_tracy_init(const char* title) {
    (void)title;
    return 0;
}
static inline void snx_tracy_capture_now(void) {}
static inline void snx_tracy_log(int lvl, const char* text) {
    (void)lvl;
    (void)text;
}
static inline void snx_tracy_shutdown(void) {}
#endif

#endif
