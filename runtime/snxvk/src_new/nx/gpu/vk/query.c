/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's samples-passed counter (3D 0x545 SampleCounterEnable, 0x54C ResetCounter 1, a counter report of
   selector 0x15: NVN's and deko3d's occlusion queries), counted with Vulkan occlusion queries. Each channel has a
   counter of its own, as each channel's 3D context has on the console.

   The count runs from its reset. It is kept in segments: a segment is one query, begun at the first draw counted in a
   rendering pass and ended with the pass, at a reset, at a report, or at a draw of another channel or another sample
   count. A segment belongs to the channel that began it, and holds that draw's guest sample count. At a submit (and
   at a report), the results of the segments ended so far are copied into host-cached memory and their queries reset,
   in the same command buffer: once per submit, as each copy waits for its queries on the GPU. A report is a release
   (release.c) gated on the submit being recorded, which happens when anything needs the release, and at the latest
   when the GPFIFO submission ends (renderer.h stream_end): a game that reports often doesn't submit once for each.
   When it is performed, the GPU has counted, and the channel's segments since its previous report are added to the
   epoch's sum, which is its value: a game that resets once and subtracts reports keeps only the segments since its
   last report live. Whatever reads the report (a semaphore acquire, the CPU, the render enable's comparison but the one
   with zero) waits for that release, as for any other.

   Counting starts at a channel's first reset: deko3d and NVN turn the counter on at startup, and a game that never
   resets it never reads it, so it costs those games nothing. A report before any reset, or in an epoch that outgrew
   the ring, keeps the made-up value the front end writes (maxwell/engines.c), over which a render enable draws.

   Targets are drawn single-sampled, where the console counts each sample of a multisampled one: a segment's count is
   multiplied by its guest sample count (3D 0x574, deko3d's MsaaMode). Render targets scaled by the resolution
   setting pass that many more samples: the sum is divided back, and a count that wasn't zero stays at least 1, as an
   object behind it is visible.
   A render enable that compares a count not known yet with zero is made on the GPU (the predicates at the end).

   Everything here runs with vk_lock held. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/query_predicate_spv.h"
#include "nx/runtime/platform/log.h"

#define QN    4096 /* segments the ring holds: queries in the pool, and result slots */
#define QWAIT 1024 /* a counter's reports queued and not performed yet, at most (the release queue's size) */
#define QCH   VK_QUERY_COUNTERS /* channels that can count at once */

static VkQueryPool g_qpool;
static VkBuffer g_qbuf;
static VmaAllocation g_qmem;
static u64* g_qres;   /* segment s's count at s % QN, once its copy has run */
static int g_qfailed; /* the pool couldn't be made: reports keep the made-up value */
/* each slot's segment: its counter (1 + its index in g_qc, 0 none) and its guest sample count */
static u8 g_qown[QN], g_qmul[QN];
static struct {
    u64 next;   /* segments begun */
    u64 copied; /* segments whose results have their copy recorded */
    int open;   /* the last segment's query is active in the rendering pass */
} g_q;
typedef struct {
    u32 chan;     /* the channel's id */
    int used;     /* the channel has counted */
    int disabled; /* SampleCounterEnable is off: its draws aren't counted */
    int started;  /* a reset came: counting is on from there */
    int bad;      /* the epoch outgrew the ring: its reports keep the made-up value until the next reset */
    u64 first;    /* the epoch's first segment number */
    u64 reported; /* the end of the epoch's last report queued, its first before one */
    /* the oldest segment each queued report not performed yet adds to its epoch's sum, in order: their slots aren't
       used again until then */
    u64 wait[QWAIT];
    u32 whead, wtail;
    /* the reports performed: the sum of epoch sum_first's segments up to sum_end */
    u64 sum, sum_first, sum_end;
} QCounter;
static QCounter g_qc[QCH];
/* the segments copied by each submission of the last QMARKS, for fold_done: where the copies end, and in which
   submission (0: none) */
#define QMARKS 64
static struct {
    u64 sub, copied;
} g_qmark[QMARKS];
static u32 g_qmark_at;

static int pool_make(void) {
    VkQueryPoolCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = QN};
    if (!VK_OK(vkCreateQueryPool(g_vk.dev, &qi, NULL, &g_qpool))) {
        g_qpool = VK_NULL_HANDLE;
        return 0;
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = QN * sizeof(u64),
                             /* read by the predicates' shader too */
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.flags =
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo info;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_qbuf, &g_qmem, &info))) {
        vkDestroyQueryPool(g_vk.dev, g_qpool, NULL);
        g_qpool = VK_NULL_HANDLE;
        return 0;
    }
    vk_mem_track(g_qmem);
    g_qres = info.pMappedData;
    /* a query is reset before its first use, outside a rendering pass */
    vk_render_end();
    rec_CmdResetQueryPool(vk_cmd(), g_qpool, 0, QN);
    if (!g_vk.feat.occlusionQueryPrecise)
        WARN("vulkan: the device's occlusion queries aren't precise: samples-passed counts may be any nonzero value\n");
    return 1;
}

/* the counter of channel chan, made if make is set; NULL when there is none (or no room for one) */
static QCounter* counter(u32 chan, int make) {
    QCounter* free_one = NULL;
    for (int i = 0; i < QCH; i++) {
        if (g_qc[i].used && g_qc[i].chan == chan) return &g_qc[i];
        if (!g_qc[i].used && !free_one) free_one = &g_qc[i];
    }
    if (!make) return NULL;
    if (!free_one) {
        LOG_LIMIT(4, "vulkan: more than %d channels count samples passed, channel %u's reports are made up\n", QCH,
                  chan);
        return NULL;
    }
    memset(free_one, 0, sizeof *free_one);
    free_one->used = 1;
    free_one->chan = chan;
    return free_one;
}
/* the oldest segment a counter's report may still add: its oldest queued report's, or its next report's */
static u64 live_from(const QCounter* c) { return c->whead != c->wtail ? c->wait[c->wtail % QWAIT] : c->reported; }
/* the oldest segment any counter may still add, whose slot can't be used again yet */
static u64 live_oldest(void) {
    u64 oldest = g_q.next;
    for (int i = 0; i < QCH; i++)
        if (g_qc[i].used && g_qc[i].started && live_from(&g_qc[i]) < oldest) oldest = live_from(&g_qc[i]);
    return oldest;
}
/* the guest sample count of an MsaaMode (3D 0x574): 1x1, 2x1, 2x2, 4x2, 4x2 D3D, 2x1 D3D, 4x4, and the 2x2 and 4x2
   virtual coverage modes */
static u32 msaa_samples(u32 mode) {
    static const u8 n[16] = {1, 2, 4, 8, 8, 2, 16, 1, 4, 4, 8, 8, 1, 1, 1, 1};
    return n[mode & 15];
}

/* A counter's live segments run from its oldest report not performed yet, or from its last report: a game that keeps
   counting without reporting (one game draws its loading screens with the counter on, at thousands of passes a
   second with the frame rate free) outgrew the ring, and every report after was made up until the next reset, which
   that game makes rarely: its occlusion tests then flickered. The segments whose copies the GPU has run are added to
   the sum of each counter no report waits on, as vk_query_value adds them, and its live segments start after them.
   A counter with reports queued keeps its segments: a report sums its own range */
static void fold_done(void) {
    u64 submitted, done, upto = 0;
    vk_submit_progress(&submitted, &done);
    for (u32 i = 0; i < QMARKS; i++)
        if (g_qmark[i].sub && g_qmark[i].sub <= done && g_qmark[i].copied > upto) upto = g_qmark[i].copied;
    if (!upto) return;
    vmaInvalidateAllocation(g_vk.vma, g_qmem, 0, VK_WHOLE_SIZE);
    for (int i = 0; i < QCH; i++) {
        QCounter* c = &g_qc[i];
        if (!c->used || !c->started || c->bad || c->whead != c->wtail || upto <= c->reported) continue;
        if (c->sum_first != c->first) { /* the epoch's first sum */
            c->sum_first = c->sum_end = c->first;
            c->sum = 0;
        }
        for (u64 s = c->sum_end; s < upto; s++)
            if (g_qown[s % QN] == i + 1) c->sum += snx_le64(g_qres[s % QN]) * g_qmul[s % QN];
        if (upto > c->sum_end) c->sum_end = upto;
        c->reported = c->sum_end;
    }
}
void vk_query_draw(VkCommandBuffer cb, u32 chan, u32 msaa_mode) {
    QCounter* c = counter(chan, 0);
    u32 mul = msaa_samples(msaa_mode);
    int own = c ? (int)(c - g_qc) + 1 : 0;
    if (g_q.open) {
        u32 at = (u32)((g_q.next - 1) % QN);
        if (g_qown[at] == own && g_qmul[at] == mul) return; /* the segment open now counts this draw too */
        vk_query_pass_end(cb);
    }
    if (!c || !c->started || c->disabled || c->bad) return;
    /* the slot is still needed by a report, or its query not reset yet: this epoch counts no further */
    if (g_q.next - live_oldest() >= QN) fold_done();
    if (g_q.next - live_oldest() >= QN || g_q.next - g_q.copied >= QN) {
        c->bad = 1;
        LOG_LIMIT(4, "vulkan: a samples-passed count spans more than %u passes, its reports are made up\n", QN);
        return;
    }
    u32 at = (u32)(g_q.next % QN);
    g_qown[at] = (u8)own;
    g_qmul[at] = (u8)mul;
    rec_CmdBeginQuery(cb, g_qpool, at, g_vk.feat.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    g_q.next++;
    g_q.open = 1;
}
void vk_query_pass_end(VkCommandBuffer cb) {
    if (!g_q.open) return;
    rec_CmdEndQuery(cb, g_qpool, (u32)((g_q.next - 1) % QN));
    g_q.open = 0;
}
void vk_query_flush(VkCommandBuffer cb) {
    if (g_q.copied == g_q.next) return;
    while (g_q.copied < g_q.next) { /* in runs that don't wrap around the ring */
        u32 at = (u32)(g_q.copied % QN), n = QN - at;
        if (n > g_q.next - g_q.copied) n = (u32)(g_q.next - g_q.copied);
        rec_CmdCopyQueryPoolResults(cb, g_qpool, at, n, g_qbuf, (VkDeviceSize)at * sizeof(u64), sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        rec_CmdResetQueryPool(cb, g_qpool, at, n);
        g_q.copied += n;
    }
    /* for the host's reads, and for a later copy into a slot of a segment no report read */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    g_qmark[g_qmark_at++ % QMARKS] = (typeof(g_qmark[0])){vk_submit_number(), g_q.copied};
}

/* a segment open now of this counter ends: what comes after is another epoch's, or isn't counted */
static void end_own(const QCounter* c) {
    if (g_q.open && g_qown[(g_q.next - 1) % QN] == (u8)(c - g_qc + 1)) vk_query_pass_end(vk_cmd());
}
void vk_query_reset(u32 chan) {
    if (!g_qpool) {
        if (g_qfailed) return;
        if (!pool_make()) {
            g_qfailed = 1;
            return;
        }
    }
    QCounter* c = counter(chan, 1);
    if (!c) return;
    end_own(c);
    if (!c->started) LOG("vulkan: channel %u counts samples passed, with occlusion queries\n", chan);
    c->started = 1;
    c->bad = 0;
    c->first = c->reported = g_q.next;
}
void vk_query_enable(u32 chan, int on) {
    QCounter* c = counter(chan, 1);
    if (!c) return;
    if (!on) end_own(c);
    c->disabled = !on;
}
int vk_query_report(u32 chan, u64 gva, u32 len, u64 ticks) {
    QCounter* c = counter(chan, 0);
    if (!c || !c->started || c->bad || c->whead - c->wtail == QWAIT) return 0;
    end_own(c);
    vk_render_end();
    vk_query_flush(vk_cmd()); /* the segments counted so far, copied in the submit the report waits for */
    c->wait[c->whead++ % QWAIT] = c->reported;
    c->reported = g_q.next;
    vk_release_samples(gva, len, (u32)(c - g_qc), c->first, g_q.next, ticks);
    return 1;
}
/* a report of counter ci being performed, in order: the count of its epoch's segments [first, end), whose copies
   have run */
u64 vk_query_value(u32 ci, u64 first, u64 end) {
    QCounter* c = &g_qc[ci % QCH];
    vmaInvalidateAllocation(g_vk.vma, g_qmem, 0, VK_WHOLE_SIZE);
    if (first != c->sum_first) { /* a new epoch (one with segments starts past the last one's) */
        c->sum_first = c->sum_end = first;
        c->sum = 0;
    }
    for (u64 s = c->sum_end; s < end; s++)
        if (g_qown[s % QN] == ci % QCH + 1) c->sum += snx_le64(g_qres[s % QN]) * g_qmul[s % QN]; /* the device's */
    if (end > c->sum_end) c->sum_end = end;
    if (c->whead != c->wtail) c->wtail++;
    double area = (double)g_vk_scale * (double)g_vk_scale;
    u64 v = area == 1.0 ? c->sum : (u64)((double)c->sum / area + 0.5);
    return c->sum && !v ? 1 : v;
}

/* ------------------------------------------------------------ render enable predicates */
/* NVN's occlusion test is a render enable (3D 0x556) comparing a report's samples-passed count with a zero 16 bytes
   on: IF_NOT_EQUAL draws what passed samples, IF_EQUAL what didn't. When the count isn't known yet (its report is
   queued, release.c), the comparison is made on the GPU instead of waiting for it: a predicate per report, 1 when the
   count isn't zero, written by query_predicate.comp from the counts of the report's segments, on which each draw and
   clear under the render enable is predicated (VK_EXT_conditional_rendering, inverted for IF_EQUAL). Reading the count
   waited for the GPU to run everything recorded until then: one game tests each object's bounding box at
   the start of a frame and draws the object under its result, and its GPU thread stood waiting for the GPU, and the
   GPU for it, for 40% of a frame. Predicates are made in batches, the reports queued when one is first needed, so a
   frame's tests take one pause of the rendering pass.
   A predicate's slot is held by its report until the report is performed and by each channel whose render enable
   uses it (release.c), and isn't given out again until both let go: draws recorded later may still use it. The GPU's
   reads of a slot before it is written again come first, by the barrier before each batch */
#define PN 4096 /* predicate slots: more than the release queue holds, with the channels' */
static VkBuffer g_pbuf;
static VmaAllocation g_pmem;
static VkDescriptorSetLayout g_pds;
static VkPipelineLayout g_playout_pred;
static VkPipeline g_ppipe;
static int g_pfailed;
static u16 g_pref[PN]; /* holders of each slot */
static u32 g_pnext;    /* where the search for a free slot starts */
/* The batch being gathered, by counter: groups of reports of one epoch in their order, each a word for the predicate
   the group starts from (a slot of an earlier batch, or ~0 for none), the number of reports, then for each report its
   slot, its number of segments and their slots: only the segments since the report before it in the group, whose
   predicate the shader carries over. A game that resets once a frame and tests cumulative reports then costs each
   segment once, not once for every report after it */
typedef struct {
    u32* w;
    u32 len, cap;
    i64 open;   /* the word where the counter's group still taking reports starts, -1 for none */
    u64 first;  /* that group's epoch */
    u64 end;    /* and the end of its last report */
} PredBatch;
static PredBatch g_pb[QCH];
static u32 g_pngroups;
static u32 g_pbatch, g_pslot_batch[PN]; /* batches flushed so far, and the batch each slot was last given out in */

static void pred_batch_reset(int drop);
static int pred_init(void) {
    if (g_ppipe) return 1;
    if (g_pfailed || !g_vk.has_cond_render || !g_qpool) return 0;
    g_pfailed = 1; /* until everything below is made */
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = PN * sizeof(u32),
                             .usage =
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_pbuf, &g_pmem, NULL))) return 0;
    vk_mem_track(g_pmem);
    VkDescriptorSetLayoutBinding b[3];
    for (u32 i = 0; i < 3; i++)
        b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                              NULL};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_pds))) return 0;
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_pds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_playout_pred))) return 0;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = query_predicate_spv_size,
                                   .pCode = query_predicate_spv};
    VkShaderModule m;
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m))) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m,
                                                .pName = "main"},
                                      .layout = g_playout_pred};
    int ok = VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &g_ppipe));
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (!ok) {
        g_ppipe = VK_NULL_HANDLE;
        return 0;
    }
    g_pfailed = 0;
    pred_batch_reset(0);
    LOG("vulkan: occlusion-tested draws are predicated on the GPU (conditional rendering)\n");
    return 1;
}
/* n more words of counter ci's groups, at the returned index, or -1 without memory */
static i64 pred_room(PredBatch* b, u32 n) {
    if (b->len + n > b->cap) {
        u32 cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n) cap *= 2;
        u32* p = realloc(b->w, (size_t)cap * sizeof *p);
        if (!p) return -1;
        b->w = p;
        b->cap = cap;
    }
    b->len += n;
    return (i64)(b->len - n);
}
/* the batch's slots go back and its groups with them (after its shader is recorded, or when it can't be) */
static void pred_batch_reset(int drop) {
    for (u32 ci = 0; ci < QCH; ci++) {
        PredBatch* b = &g_pb[ci];
        for (u32 at = 0; drop && at < b->len;) {
            u32 n = b->w[at + 1];
            at += 2;
            for (u32 k = 0; k < n; k++) {
                vk_query_pred_drop(b->w[at]);
                at += 2 + b->w[at + 1];
            }
        }
        b->len = 0;
        b->open = -1;
    }
    g_pngroups = 0;
    g_pbatch++;
}
int vk_query_pred_ok(void) { return pred_init(); }
int vk_query_pred_job(u32 ci, u64 first, u64 end, u64 prev_end, int prev, u32 prev_slot, u32* slot) {
    const QCounter* c = &g_qc[ci % QCH];
    PredBatch* b = &g_pb[ci % QCH];
    u8 own = (u8)(ci % QCH + 1);
    u64 from = first, s;
    u32 n = 0, k, at, init = ~0u;
    i64 r;
    if (!pred_init()) return -1;
    /* the reports performed (vk_query_value) summed the epoch's segments up to sum_end: the count isn't zero when
       they counted something, and the rest of it is in the segments since */
    if (c->sum_first == first) {
        if (c->sum) return 1;
        if (c->sum_end > from) from = c->sum_end;
    }
    if (b->open >= 0 && b->first == first && prev_end == b->end && b->end >= from)
        from = b->end; /* the report before it is the group's last: it goes on from there */
    else {
        /* the report before it, still queued, holds the epoch's count up to its end */
        /* a slot of this batch is written by the same dispatch, unordered: the segments are counted again instead */
        if (prev == VK_PRED_PREV_SLOT && (prev_slot >= PN || g_pslot_batch[prev_slot] == g_pbatch))
            prev = VK_PRED_PREV_NONE;
        if (prev != VK_PRED_PREV_NONE && prev_end > from && prev_end <= end) {
            if (prev == VK_PRED_PREV_NONZERO) return 1;
            if (prev == VK_PRED_PREV_SLOT) init = prev_slot;
            from = prev_end;
        }
        for (s = from; s < end; s++) n += g_qown[s % QN] == own;
        if (!n && init == ~0u) return 0;
        n = 0;
        if ((r = pred_room(b, 2)) < 0) return -1;
        b->w[r] = init;
        b->w[r + 1] = 0;
        b->open = r;
        b->first = first;
        g_pngroups++;
    }
    for (s = from; s < end; s++) n += g_qown[s % QN] == own;
    for (k = 0; k < PN && g_pref[(g_pnext + k) % PN]; k++) {
    }
    if (k == PN) return -1; /* can't be: the holders are fewer */
    at = (g_pnext + k) % PN;
    if ((r = pred_room(b, 2 + n)) < 0) return -1;
    b->w[r] = at;
    b->w[r + 1] = n;
    n = 0;
    for (s = from; s < end; s++)
        if (g_qown[s % QN] == own) b->w[r + 2 + n++] = (u32)(s % QN);
    b->w[b->open + 1]++;
    b->end = end;
    g_pslot_batch[at] = g_pbatch;
    g_pref[at] = 1;
    g_pnext = at + 1;
    *slot = at;
    return 2;
}
void vk_query_pred_hold(u32 slot) {
    if (slot < PN) g_pref[slot]++;
}
void vk_query_pred_drop(u32 slot) {
    if (slot < PN && g_pref[slot]) g_pref[slot]--;
}
VkBuffer vk_query_pred_buffer(void) { return g_pbuf; }
int vk_query_pred_flush(void) {
    u32 n = g_pngroups, len = 0, i, at, ci, g;
    if (!n) return 1;
    for (ci = 0; ci < QCH; ci++) len += g_pb[ci].len;
    VkCommandBuffer cb = vk_cmd();
    u64 size = (u64)(n + len) * sizeof(u32), off;
    VkBuffer jobs;
    u32* words = (u32*)vk_stage(size, vk_stage_align(), &jobs, &off);
    VkDescriptorSet set = words ? vk_desc_alloc(g_pds) : VK_NULL_HANDLE;
    if (!set) { /* out of memory: the batch's slots go back, and their counts are waited for (release.c) */
        WARN_LIMIT(4, "vulkan: no memory for %u groups of occlusion test predicates: their counts are waited for\n",
                   n);
        pred_batch_reset(1);
        return 0;
    }
    /* where each group starts first, then the counters' groups after them */
    for (ci = 0, g = 0, len = n; ci < QCH; ci++) {
        const PredBatch* b = &g_pb[ci];
        for (at = 0; at < b->len; g++) {
            u32 reps = b->w[at + 1];
            words[g] = len + at;
            at += 2;
            for (i = 0; i < reps; i++) at += 2 + b->w[at + 1];
        }
        memcpy(words + len, b->w, (size_t)b->len * sizeof(u32));
        len += b->len;
    }
    snx_le32_array(words, len); /* the shader reads them little-endian */
    pred_batch_reset(0);
    VkDescriptorBufferInfo bufs[3] = {{g_qbuf, 0, VK_WHOLE_SIZE}, {jobs, off, size}, {g_pbuf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[3];
    for (i = 0; i < 3; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set,
                                      .dstBinding = i,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &bufs[i]};
    vkUpdateDescriptorSets(g_vk.dev, 3, w, 0, NULL);
    vk_render_end(); /* a dispatch is outside a rendering pass */
    /* the counts' copies (vk_query_flush) and an earlier batch's predicates a group starts from before the shader
       reads them, and the draws predicated on a slot before it is written again. The batch's words are host writes,
       made visible by the submit */
    VkMemoryBarrier2 before = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                               VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &before};
    rec_CmdPipelineBarrier2(cb, &d);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_ppipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_playout_pred, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_playout_pred, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof n, &n);
    rec_CmdDispatch(cb, (n + 63) / 64, 1, 1);
    /* the predicates for the draws, and the shader's reads of the counts before a copy reuses their slots */
    VkMemoryBarrier2 after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT |
                                              VK_PIPELINE_STAGE_2_COPY_BIT,
                              .dstAccessMask = VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT |
                                               VK_ACCESS_2_TRANSFER_WRITE_BIT};
    d.pMemoryBarriers = &after;
    rec_CmdPipelineBarrier2(cb, &d);
    return 1;
}

/* a save state's load: the counters start over, their reports the made-up value until the game resets them */
void vk_query_forget_all(void) { memset(g_qc, 0, sizeof g_qc); }
