/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Releases the game can observe: a semaphore or report written for the CPU to poll, a syncpoint incremented. The
   console's GPU makes everything before a release visible in memory first, so storage results recorded before it
   land before it happens. Landing them with a vk_finish at each release left the GPU thread waiting for most of
   one game's frame. A release now waits in a queue instead, gated on the submit that holds the work before it, and the
   GPU thread goes on recording. The release thread waits for that submit on the GPU, lands its results and performs
   the releases it gates, in order. Whoever needs a queued release sooner (a stream that reads the semaphore, or writes
   over it) performs it itself.
   The compute queue's results are waited for the same way (its batches, put aside, land in order), and so are
   fragment programs' storage images (their copies recorded at the release, vk_gfx_image_put_aside).
   A release also tells the CPU that the GPU got there, and the CPU may then write what the commands after it read: on
   the console those run at once, ahead of the CPU. One game rewrites its characters' bone matrices and the bone slots
   in their vertices once a release in the middle of its frame shows. Performed as soon as the GPU thread met it, the
   draws after it read the next frame's slots there, and vertices flew to other characters. So the stream holds its
   releases until it stops, at its end or at a wait (vk_release_submit), when the commands after them have read guest
   memory, and the ones no GPU work holds are performed then. */
#include <string.h>
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h" /* tests/vulkan_release.c stubs vk.h */

typedef struct {
    u64 gate;  /* the graphics submit whose completion it waits for */
    u64 cgate; /* and the compute batch, 0 for none */
    u64 gva;   /* the payload's address, or 0 for a syncpoint increment */
    u32 id, len;
    u8 data[16];
    /* samples: a samples-passed report of query.c's counter qch, whose value is the count of its segments [qfirst,
       qend), known once the gate is done. data holds the timestamp after it. Its gate is the submit being recorded when
       it came, which whoever needs the release submits (submit_through), and the end of the GPFIFO submission */
    int samples;
    u32 qch;
    u64 qfirst, qend;
    /* a samples report's render enable predicate (query.c): 0 none made, slot + 1 while it holds one, or what the count
       is known to be without the GPU, PRED_ZERO or PRED_NONZERO, or PRED_NONE when no predicate can be had */
    int pred;
    u64 cpu; /* the guest address of its payload, translated when it was queued, 0 when it isn't in the word index */
} Release;
enum { PRED_NONE = -3, PRED_ZERO = -2, PRED_NONZERO = -1 };
#define RQ_N   1024 /* queued releases at most: a full queue performs its oldest at once */
#define SYNC_N 128  /* nvdrv's syncpoint ids */

static Release g_rq[RQ_N];
static u64 g_rhead, g_rtail; /* releases queued, performed */
static u64 g_rheld;          /* the first release the stream still holds: those from it on aren't performed yet */
static u32 g_rincr[SYNC_N];  /* increments queued per syncpoint */
static PMutex g_rlock = PMUTEX_INIT;
static PCond g_rcv;
static int g_rthread;
/* the guest pages queued payloads cover, so a read or write that meets none (almost all) skips the queue: added when a
   payload is queued, removed when it is written, both under vk_lock, and a mapping change drains the queue before any
   translation changes (vkr_mapping_begin), so a payload loses the pages it gained */
static PendPages g_rpages = {.also = &g_pend_any};
u64 g_vk_release_forwards;

/* The queued payloads by the guest words they write: for each 4-byte word, the latest release queued over it and how
   many are, so the render enable finds the release that decides a count without looking through the queue
   (count_queued), where one game keeps hundreds of reports a frame. A payload that isn't whole words of
   one mapped page is counted in g_rodd instead, and while there is one the queue is looked through. Under g_rlock */
#define RIX_N 8192 /* twice four words for each release the queue holds: half full at most */
static struct {
    u64 word; /* its guest address, 0 for an empty entry */
    u64 seq;  /* the latest release queued over it, by its queue number */
    u32 n;    /* the releases queued over it */
} g_rix[RIX_N];
static u64 g_rodd;
static u32 rix_home(u64 word) { return (u32)((word >> 2) * 0x9E3779B97F4A7C15ull >> 51) & (RIX_N - 1); }
static u32 rix_find(u64 word) {
    u32 i = rix_home(word);
    while (g_rix[i].word && g_rix[i].word != word) i = (i + 1) & (RIX_N - 1);
    return i;
}
/* the word of the release queued as seq: its latest */
static void rix_add(u64 word, u64 seq) {
    u32 i = rix_find(word);
    g_rix[i].word = word;
    g_rix[i].seq = seq;
    g_rix[i].n++;
}
/* the oldest release over word is performed: an entry left with none goes, the ones after it in its run moving back
   (linear probing's deletion, without tombstones) */
static void rix_remove(u64 word) {
    u32 i = rix_find(word), j, k;
    if (!g_rix[i].word || --g_rix[i].n) return;
    g_rix[i].word = 0;
    for (j = (i + 1) & (RIX_N - 1); g_rix[j].word; j = (j + 1) & (RIX_N - 1)) {
        k = rix_home(g_rix[j].word);
        /* it stays when its home is cyclically within (i, j] */
        if (i <= j ? i < k && k <= j : i < k || k <= j) continue;
        g_rix[i] = g_rix[j];
        /* emptied whole: rix_add counts up from the slot's n, and a count left here made the next word there never
           go, until the full table left rix_find probing forever (the GPU thread hung in release) */
        g_rix[j].word = 0;
        g_rix[j].n = 0;
        i = j;
    }
}
/* a payload release is queued as seq or performed (delta -1) */
static void rix_track(Release* r, u64 seq, int delta) {
    if (delta > 0) {
        u64 c = gmmu_translate(r->gva);
        r->cpu = c && !(c & 3) && !(r->len & 3) && (c & 4095) + r->len <= 4096 ? c : 0;
    }
    if (!r->cpu) {
        g_rodd += delta;
        return;
    }
    for (u32 o = 0; o < r->len; o += 4) {
        if (delta > 0)
            rix_add(r->cpu + o, seq);
        else
            rix_remove(r->cpu + o);
    }
}

static void perform(Release* r) {
    if (r->samples) { /* the count, as the report's structure size says: one word, or the value and the timestamp */
        u64 v = vk_query_value(r->qch, r->qfirst, r->qend);
        if (r->len == 4)
            snx_st_le32(r->data, (u32)v);
        else
            snx_st_le64(r->data, v);
    }
    vk_result_begin();
    if (r->gva)
        gmem_write(r->gva, r->data, r->len);
    else
        syncpt_add_quiet(r->id, 1); /* under vk_lock: the events it reaches are signaled as it is let go */
    vk_result_end();
}
void vk_release_events(void) { syncpt_events(); }
/* lands the results of every submit up to gate and compute batch up to cgate (they have finished) and performs the
   releases they gate, in order */
static void release_through(u64 gate, u64 cgate) {
    vk_lock();
    vk_result_begin();
    vk_compute_land_through(cgate);
    vk_land_through(gate);
    vk_result_end();
    for (;;) {
        Release r;
        pm_lock(&g_rlock);
        if (g_rtail == g_rheld || g_rq[g_rtail % RQ_N].gate > gate || g_rq[g_rtail % RQ_N].cgate > cgate) {
            pm_unlock(&g_rlock);
            break;
        }
        r = g_rq[g_rtail % RQ_N];
        if (r.gva) rix_track(&r, g_rtail, -1);
        g_rtail++;
        if (r.pred > 0) vk_query_pred_drop((u32)r.pred - 1); /* the draws recorded with it may still hold it */
        if (r.gva) pend_add(&g_rpages, r.gva, r.len, -1);
        if (!r.gva) {
            /* release_value sees the transfer from queued to completed atomically. */
            perform(&r);
            if (r.id < SYNC_N) g_rincr[r.id]--;
        }
        if (g_rtail == g_rhead) __atomic_store_n(&g_gpu_releases, 0, __ATOMIC_RELEASE);
        pm_unlock(&g_rlock);
        if (r.gva) perform(&r);
    }
    vk_unlock();
}
static void release_thread(void* p) {
    (void)p;
    SNX_THREAD_NAME("Vulkan release");
    for (;;) {
        u64 gate, cgate;
        pm_lock(&g_rlock);
        while (g_rtail == g_rheld) pc_wait(&g_rcv, &g_rlock, -1);
        gate = g_rq[g_rtail % RQ_N].gate;
        cgate = g_rq[g_rtail % RQ_N].cgate;
        pm_unlock(&g_rlock);
        SNX_ZONE(z_release, "vk release");
        vk_wait_submit(gate);
        vk_compute_wait(cgate);
        {
            SNX_ZONE(z, "land and release");
            release_through(gate, cgate);
            SNX_ZONE_END(z);
        }
    }
}

/* the submit gate names, recorded still: submitted now, so waiting for it ends (vk_lock) */
static void submit_through(u64 gate) {
    if (gate > vk_last_submit()) vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
}
/* r once everything recorded before it has landed and the stream has stopped (g_rheld) */
static void release(Release* r) {
    SNX_ZONE_V(z, "vk release queue", r->samples);
    vk_lock();
    vk_rt_publish_linear();
    vk_gfx_image_put_aside();
    r->cgate = vk_compute_put_aside();
    /* the work before it: none when nothing is pending (a samples-passed report always waits for the GPU, its count
       is the draws'). Else results of earlier submits are older, and the submit being recorded holds the rest (and a
       samples-passed report's counts). The release waits for that submit without submitting it now: whoever needs the
       release submits it (submit_through), and so do the end of the GPFIFO submission and a wait in it (stream_end).
       Submitting at each release made Pokemon Legends Z-A submit about 150 times a frame, with storage results
       pending through most of its releases */
    u64 recording = vk_submit_number();
    if (!r->samples && !vk_buf_pending_any() && !vk_gfx_image_in_flight(0) && !r->cgate)
        r->gate = 0;
    else
        r->gate =
            r->samples || vk_buf_pending_unsubmitted() || vk_gfx_image_in_flight(1) ? recording : vk_last_submit();
    pm_lock(&g_rlock);
    if (g_rhead - g_rtail == RQ_N) { /* full: the oldest goes now, held or not */
        g_rheld = g_rhead;
        u64 oldest = g_rq[g_rtail % RQ_N].gate, coldest = g_rq[g_rtail % RQ_N].cgate;
        pm_unlock(&g_rlock);
        submit_through(oldest);
        vk_wait_submit(oldest);
        vk_compute_wait(coldest);
        release_through(oldest, coldest);
        pm_lock(&g_rlock);
    }
    /* Gates cover the entire queued prefix even if ordinary reads already landed
       the batches belonging to earlier releases. */
    if (g_rhead != g_rtail) {
        const Release* prev = &g_rq[(g_rhead - 1) % RQ_N];
        if (r->gate < prev->gate) r->gate = prev->gate;
        if (r->cgate < prev->cgate) r->cgate = prev->cgate;
    }
    if (r->gva) rix_track(r, g_rhead, 1);
    g_rq[g_rhead % RQ_N] = *r;
    g_rhead++;
    if (r->gva) pend_add(&g_rpages, r->gva, r->len, 1);
    if (!r->gva && r->id < SYNC_N) g_rincr[r->id]++;
    __atomic_store_n(&g_gpu_releases, 1, __ATOMIC_RELEASE);
    if (!g_rthread) {
        pc_init(&g_rcv);
        g_rthread = 1;
        if (plat_thread(release_thread, NULL, (size_t)1 << 20)) {
            char why[256];
            fatal("could not start the GPU release thread: %s", snx_os_error(why, sizeof why));
        }
    }
    pc_broadcast(&g_rcv);
    pm_unlock(&g_rlock);
    vk_unlock();
}

void vk_release_write(u64 gva, const void* data, u32 len) {
    Release r;
    memset(&r, 0, sizeof r);
    r.gva = gva;
    r.len = len < sizeof r.data ? len : (u32)sizeof r.data;
    memcpy(r.data, data, r.len);
    release(&r);
}
/* the GPFIFO submission ended or waits: the stream's releases go, those no GPU work holds at once, and a release still
   gated on the submit being recorded goes to the GPU, as the guest may look at it now */
void vk_release_submit(void) {
    SNX_ZONE(z, "vk release submit");
    vk_lock();
    pm_lock(&g_rlock);
    g_rheld = g_rhead; /* the commands after the releases have read guest memory */
    u64 gate = g_rhead != g_rtail ? g_rq[(g_rhead - 1) % RQ_N].gate : 0; /* the gates only grow */
    if (g_rthread) pc_broadcast(&g_rcv);
    pm_unlock(&g_rlock);
    release_through(0, 0); /* those no GPU work holds */
    submit_through(gate);
    vk_unlock();
}
void vk_release_samples(u64 gva, u32 len, u32 counter, u64 first, u64 end, u64 ticks) {
    Release r;
    memset(&r, 0, sizeof r);
    r.gva = gva;
    r.len = len == 4 ? 4 : 16;
    r.samples = 1;
    r.qch = counter;
    r.qfirst = first;
    r.qend = end;
    snx_st_le64(r.data + 8, ticks); /* guest bytes, as release_write's payloads are */
    release(&r);
}
/* the releases queued, and the submissions the oldest waits for: read unlocked, for the stall report */
void vk_release_queued(u64* n, u64* gate, u64* cgate) {
    u64 tail = __atomic_load_n(&g_rtail, __ATOMIC_RELAXED);
    const Release* r = &g_rq[tail % RQ_N];
    *n = __atomic_load_n(&g_rhead, __ATOMIC_RELAXED) - tail;
    *gate = *n ? __atomic_load_n(&r->gate, __ATOMIC_RELAXED) : 0;
    *cgate = *n ? __atomic_load_n(&r->cgate, __ATOMIC_RELAXED) : 0;
}
void vk_release_incr(u32 id) {
    Release r;
    memset(&r, 0, sizeof r);
    r.id = id;
    release(&r);
}
/* Mapping changes hold vk_lock and drain the queue before changing translations.
   Thus these translations name the backing that was present at enqueue time. */
static int release_overlaps(const Release* r, u64 gva, u64 len) {
    if (!r->gva || !r->len || !len) return 0;
    if (gva >= r->gva ? gva - r->gva < r->len : r->gva - gva < len) return 1;
    for (u32 at = 0; at < r->len;) {
        u64 a = r->gva + at, cpu = gmmu_translate(a);
        u32 n = 4096 - (u32)(a & 4095);
        if (n > r->len - at) n = r->len - at;
        for (u64 off = 0; cpu && off < len;) {
            u64 b = gva + off, other = gmmu_translate(b), span = 4096 - (b & 4095);
            if (span > len - off) span = len - off;
            if (other && (other >= cpu ? other - cpu < n : cpu - other < span)) return 1;
            off += span;
        }
        at += n;
    }
    return 0;
}
/* Drain through the latest matching release. Gates are monotonic prefix maxima. */
static void release_match(u64 gva, u64 len, u32 id, int all) {
    u64 gate = 0, cgate = 0;
    int found = 0;
    if (vk_result_landing()) return;
    vk_lock();
    if (len && !all && !pend_hit(&g_rpages, gva, len)) { /* no queued payload there */
        vk_unlock();
        return;
    }
    pm_lock(&g_rlock);
    g_rheld = g_rhead; /* needed now: the stream holds them no longer */
    if (g_rthread) pc_broadcast(&g_rcv);
    for (u64 i = g_rtail; i < g_rhead; i++) {
        const Release* r = &g_rq[i % RQ_N];
        if (all || (len ? release_overlaps(r, gva, len) : !r->gva && r->id == id)) {
            gate = r->gate;
            cgate = r->cgate;
            found = 1;
        }
    }
    pm_unlock(&g_rlock);
    if (found) {
        submit_through(gate);
        vk_wait_submit(gate);
        vk_compute_wait(cgate);
        release_through(gate, cgate);
    }
    vk_unlock();
}
/* [gva, gva + len) as the stream sees it: memory, then the payloads of the releases queued over it in their order (1),
   or 0 when none is queued there, or -1 when one there can't be read past now. Under vk_lock, which release_through
   holds while it takes releases from the queue and writes them, so a release is either still queued or in memory.
   Storage results still pending there may be newer than the releases (a release lands storage images and puts the
   compute batch aside before it is queued), so they are waited for instead. The queue is looked at first: almost no
   read meets a release, and those go on at once */
int vk_release_read(u64 gva, void* dst, u64 len) {
    int r = 0;
    if (!len || vk_result_landing()) return len ? -1 : 0;
    vk_lock();
    if (!pend_hit(&g_rpages, gva, len)) {
        vk_unlock();
        return 0;
    }
    pm_lock(&g_rlock);
    for (u64 i = g_rtail; i < g_rhead && r >= 0; i++) {
        const Release* q = &g_rq[i % RQ_N];
        if (!release_overlaps(q, gva, len)) continue;
        /* a count the GPU hasn't given yet, or the same memory through another address: not known here */
        r = !q->samples && gva < q->gva + q->len && q->gva < gva + len && len <= 0x10000 ? 1 : -1;
    }
    pm_unlock(&g_rlock);
    if (r > 0 && (vk_compute_pending(gva, len) || vk_buf_pending(gva, len) || vk_gfx_image_pending(gva, len))) r = -1;
    if (r > 0) {
        pm_lock(&g_rlock);
        gmem_read_raw(gva, dst, len);
        for (u64 i = g_rtail; i < g_rhead; i++) {
            const Release* q = &g_rq[i % RQ_N];
            u64 lo, hi;
            if (!q->gva || gva >= q->gva + q->len || q->gva >= gva + len) continue;
            lo = q->gva > gva ? q->gva : gva;
            hi = q->gva + q->len < gva + len ? q->gva + q->len : gva + len;
            memcpy((u8*)dst + (lo - gva), q->data + (lo - q->gva), (size_t)(hi - lo));
        }
        pm_unlock(&g_rlock);
        __atomic_add_fetch(&g_vk_release_forwards, 1, __ATOMIC_RELAXED);
    }
    vk_unlock();
    return r;
}
void vk_release_need(u64 gva, u64 len) {
    if (len && __atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE)) release_match(gva, len, 0, 0);
}
void vk_release_drain(void) { release_match(0, 0, 0, 1); }
void vk_release_retire(u32 id) { release_match(0, 0, id, 0); }
/* The render enable's comparison of a count with zero (renderer.h render_predicate), when the count is a samples report
   still queued: made on the GPU (query.c's predicates), or known without it. The report must be the last release
   queued over the count's 8 bytes, and the other side of the comparison zero, read without waiting for anything */
/* the queued release that is the last over [gva, gva + 8): its queue number, -1 when none is, -2 when the last isn't
   a whole samples count there. With g_rlock held */
static i64 count_queued(u64 gva) {
    i64 last = -1;
    u64 c = gmmu_translate(gva);
    if (!g_rodd && c && !(c & 3) && (c & 4095) <= 4096 - 8) /* by the words' index */
        for (u32 o = 0; o < 8; o += 4) {
            u32 i = rix_find(c + o);
            if (g_rix[i].word && (i64)g_rix[i].seq > last) last = (i64)g_rix[i].seq;
        }
    else
        for (u64 i = g_rtail; i < g_rhead; i++)
            if (release_overlaps(&g_rq[i % RQ_N], gva, 8)) last = (i64)i;
    if (last >= 0) {
        const Release* r = &g_rq[last % RQ_N];
        if (!r->samples || r->gva != gva || r->len != 16) return -2;
    }
    return last;
}
/* the 8 bytes at gva are zero now, as the stream sees them, nothing pending over them. With vk_lock held */
static int reads_zero(u64 gva) {
    u64 v = 0;
    if (vk_compute_pending(gva, 8) || vk_buf_pending(gva, 8) || vk_gfx_image_pending(gva, 8)) return 0;
    gmem_read_raw(gva, &v, 8);
    return v == 0;
}
int vk_render_predicate(u64 a, int equal, u32* pred) {
    i64 ia, ib, at;
    u32 slot = 0;
    int got = -1, k;
    if (!__atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE)) return -1;
    vk_lock();
    if (!vk_query_pred_ok()) { /* outside g_rlock, which the game's threads take: it may make the shader */
        vk_unlock();
        return -1;
    }
    pm_lock(&g_rlock);
    ia = count_queued(a);
    ib = count_queued(a + 16);
    pm_unlock(&g_rlock);
    /* one side a count still queued, the other zero */
    if (ia >= 0 && ib == -1 && reads_zero(a + 16))
        at = ia;
    else if (ib >= 0 && ia == -1 && reads_zero(a))
        at = ib;
    else {
        vk_unlock();
        return -1;
    }
    pm_lock(&g_rlock);
    if (!g_rq[at % RQ_N].pred) {
        /* every samples report queued without a predicate gets one now, in one batch: a frame's occlusion tests
           come before the draws they decide */
        u64 batch[RQ_N];
        i64 last[VK_QUERY_COUNTERS]; /* each counter's report before, in the queue */
        u32 nb = 0, j;
        for (j = 0; j < VK_QUERY_COUNTERS; j++) last[j] = -1;
        for (u64 i = g_rtail; i < g_rhead; i++) {
            Release* r = &g_rq[i % RQ_N];
            const Release* p;
            int prev = VK_PRED_PREV_NONE;
            if (!r->samples || r->qch >= VK_QUERY_COUNTERS) continue;
            p = last[r->qch] >= 0 ? &g_rq[last[r->qch] % RQ_N] : NULL;
            last[r->qch] = (i64)i;
            if (r->pred) continue;
            if (p && p->qfirst == r->qfirst)
                prev = p->pred > 0             ? VK_PRED_PREV_SLOT
                       : p->pred == PRED_ZERO    ? VK_PRED_PREV_ZERO
                       : p->pred == PRED_NONZERO ? VK_PRED_PREV_NONZERO
                                                 : VK_PRED_PREV_NONE;
            u32 pslot = p && p->pred > 0 ? (u32)p->pred - 1 : 0;
            k = vk_query_pred_job(r->qch, r->qfirst, r->qend, p ? p->qend : 0, prev, pslot, &slot);
            r->pred = k == 2 ? (int)slot + 1 : k == 1 ? PRED_NONZERO : k == 0 ? PRED_ZERO : PRED_NONE;
            if (k == 2) batch[nb++] = i;
        }
        pm_unlock(&g_rlock);
        if (!vk_query_pred_flush()) { /* no predicates: the counts are waited for */
            pm_lock(&g_rlock);
            for (j = 0; j < nb; j++) g_rq[batch[j] % RQ_N].pred = PRED_NONE;
            pm_unlock(&g_rlock);
        }
        pm_lock(&g_rlock);
    }
    k = g_rq[at % RQ_N].pred;
    pm_unlock(&g_rlock);
    if (k > 0) {
        vk_query_pred_hold((u32)k - 1);
        *pred = (u32)k | (equal ? VK_PRED_EQUAL : 0);
        got = 2;
    } else if (k == PRED_ZERO || k == PRED_NONZERO)
        got = (k == PRED_ZERO) == !!equal;
    vk_unlock();
    return got;
}
void vk_render_predicate_drop(u32 pred) {
    vk_lock();
    vk_query_pred_drop((pred & ~VK_PRED_EQUAL) - 1);
    vk_unlock();
}
u32 vk_release_value(u32 id) {
    pm_lock(&g_rlock);
    u32 n = syncpt_read(id) + (id < SYNC_N ? g_rincr[id] : 0);
    pm_unlock(&g_rlock);
    return n;
}
