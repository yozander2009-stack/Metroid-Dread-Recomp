/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_gpu_profile=1: where the GPU's time goes. A scene can be bound by the GPU while every CPU thread idles (one
   game's lookout ran at 6 frames a second with the GPU 91% busy and no thread past 20%), and the CPU's profilers can't
   say which work it is. Each rendering pass of the frame's command buffers gets a timestamp at its start and its end,
   with its first color target (or its depth target), its draws, and what was recorded between it and the pass before
   (copies by kind and bytes, barriers, dispatches: record.c counts them). When the frame's work is done
   (frame_recycle) the times are read, and every 10 s (a slow scene takes minutes for 600 presents) the log gets the
   frame's span, the time in passes, the time in the gaps between them by what they held, and the targets that took the
   most. That is how 955 MB of buffer copies a frame between passes were found. Off, it costs a branch at each pass */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

#define GP_QUERIES 8192 /* timestamps per frame slot: two per pass */
#define GP_KEYS    1024 /* targets summed over 600 presents */

typedef struct {
    u64 gva;
    u32 w, h, fmt, draws;
    u32 xfers, barriers, dispatches; /* recorded since the pass before it ended: what the gap before it holds */
    u32 kind[3];
    u64 bytes;
    u16 q0, q1; /* the Tracy query ids its begin and end went out with, 0 when it emitted none */
    u64 ep; /* the connection the begin went out on: an end or a time from another one is dropped */
} GpPass;
typedef struct {
    VkQueryPool pool;
    u32 used; /* timestamps written in this slot since its reset */
    int reset; /* the reset is recorded for this use of the slot */
    GpPass pass[GP_QUERIES / 2];
} GpSlot;
typedef struct {
    u64 gva;
    u32 w, h, fmt;
    u64 ns, passes, draws;
} GpKey;

static int g_on = -1;
static GpSlot* g_slot[VK_FRAMES];
static GpKey g_key[GP_KEYS];
static u64 g_pass_ns, g_span_ns, g_frames, g_lost;
static u64 g_draws_at_begin;
/* the gaps between passes by what was recorded in them: dispatches, else copies, else barriers, else nothing (the
   GPU waiting for the next submission or a semaphore) */
static const char* const g_gap_name[4] = {"with dispatches", "with copies", "with barriers only", "with nothing"};
static u64 g_gap_ns[4], g_gaps[4];
static u64 g_x_at_end, g_b_at_end, g_d_at_end, g_k_at_end[3], g_bytes_at_end;
/* the copy gaps by their mix: buffer copies only, with buffer-image copies, with image copies or blits */
static const char* const g_mix_name[3] = {"buffer copies only", "with buffer-image copies", "with image copies"};
static u64 g_mix_ns[3], g_mixes[3], g_mix_bytes[3], g_mix_n[3];
static int g_in_pass;
static int g_dead; /* the query pool was refused once: stop asking */

#if defined(SNX_TRACY)
/* Tracy's GPU track: the passes as zones on the GPU's clock, their timestamps read back with the frame's results,
   the emissions themselves in the platform layer (tracy.c). A profiling run asks for them: the setting, or the
   client being up. The calibration baseline is reset at a new connection, whose server re-anchored at the replayed
   context's original pair. */
static i64 g_cal_cpu_prev; /* the client's clock at the track's last calibration pair */
static u64 g_cal_conn;     /* the connection that baseline was taken on */
static int g_cal_ok;       /* the context was announced calibrated, so calibration events are for it */
#endif

int vk_gpuprof_calibrated(void) {
    return g_vk.has_calibrated_host && g_vk.props.limits.timestampComputeAndGraphics;
}

static int gp_on(void) {
    if (g_on < 0) {
        g_on = g_settings.gpu_profile && g_vk.props.limits.timestampComputeAndGraphics;
        if (g_settings.gpu_profile && !g_on) WARN("vulkan: vk_gpu_profile: the device has no timestamps on its graphics queue\n");
    }
    if (g_dead) return 0;
#if defined(SNX_TRACY)
    /* The client's track without the setting. Attached, not merely started: on-demand Tracy drops the GPU events
       of a client no server has connected to, so a --tracy run waiting for a GUI would otherwise pay a query pool
       reset and two timestamps for every pass, a read-back at each recycle, and keep none of it. */
    if (snx_tracy_attached()) return g_vk.props.limits.timestampComputeAndGraphics;
#endif
    return g_on;
}
static GpSlot* gp_slot(void) {
    u32 s = vk_frame_slot();
    if (!g_slot[s]) {
        VkQueryPoolCreateInfo qi = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_QUERIES};
        GpSlot* g = calloc(1, sizeof *g);
        if (!g || !VK_OK(vkCreateQueryPool(g_vk.dev, &qi, NULL, &g->pool))) {
            free(g);
            g_dead = 1;
            return NULL;
        }
        g_slot[s] = g;
    }
    return g_slot[s];
}
/* before vkCmdBeginRendering, outside any pass: the reset of the slot's pool if this is its first pass since recycled */
void vk_gpuprof_pass_begin(VkCommandBuffer cb, const Surf* rt, const Surf* zt) {
    GpSlot* g;
    if (!gp_on() || !(g = gp_slot())) return;
    if (!g->reset) {
        rec_CmdResetQueryPool(cb, g->pool, 0, GP_QUERIES);
        g->reset = 1;
        g->used = 0;
    }
    if (g->used + 2 > GP_QUERIES) {
        g_lost++;
        return;
    }
    const Surf* s = rt ? rt : zt;
    GpPass* p = &g->pass[g->used / 2];
    p->gva = s ? s->gva : 0;
    p->w = s ? s->w : 0;
    p->h = s ? s->h : 0;
    p->fmt = s ? (u32)s->f.vk : 0;
    p->xfers = (u32)(g_rec_xfers - g_x_at_end);
    p->barriers = (u32)(g_rec_barriers - g_b_at_end);
    p->dispatches = (u32)(g_vk_dispatches - g_d_at_end);
    for (int i = 0; i < 3; i++) p->kind[i] = (u32)(g_rec_xfer_kind[i] - g_k_at_end[i]);
    p->bytes = g_rec_buf_bytes - g_bytes_at_end;
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g->pool, g->used);
    g_draws_at_begin = g_vk_drawn;
    g_in_pass = 1;
#if defined(SNX_TRACY)
    p->q0 = p->q1 = 0;
    if (snx_tracy_attached()) {
        char name[48] = "gpu pass";
        if (s) snprintf(name, sizeof name, "gpu pass %ux%u fmt %u", s->w, s->h, (u32)s->f.vk);
        p->q0 = snx_tracy_gpu_zone_begin(name, 0x3d8f4a, VK_GPU_CTX_GRAPHICS, &p->ep);
    }
#endif
}
/* after vkCmdEndRendering */
void vk_gpuprof_pass_end(VkCommandBuffer cb) {
    GpSlot* g;
    GpPass* p;
    if (!g_in_pass) return;
    g_in_pass = 0;
    if (!(g = gp_slot())) return;
    p = &g->pass[g->used / 2];
    p->draws = (u32)(g_vk_drawn - g_draws_at_begin);
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g->pool, g->used + 1);
    g->used += 2;
    g_x_at_end = g_rec_xfers;
    g_b_at_end = g_rec_barriers;
    g_d_at_end = g_vk_dispatches;
    for (int i = 0; i < 3; i++) g_k_at_end[i] = g_rec_xfer_kind[i];
    g_bytes_at_end = g_rec_buf_bytes;
#if defined(SNX_TRACY)
    /* the end goes out whenever the begin did: a dropped pair never reaches the server apart */
    if (p->q0) p->q1 = snx_tracy_gpu_zone_end(p->q0, VK_GPU_CTX_GRAPHICS, p->ep);
#endif
}
static void gp_report(void);
static GpKey* gp_key(const GpPass* p) {
    u64 h = (p->gva >> 8) ^ ((u64)p->w << 40) ^ ((u64)p->h << 20) ^ p->fmt;
    for (u32 i = 0; i < GP_KEYS; i++) {
        GpKey* k = &g_key[(h + i) & (GP_KEYS - 1)];
        if (!k->passes) {
            k->gva = p->gva, k->w = p->w, k->h = p->h, k->fmt = p->fmt;
            return k;
        }
        if (k->gva == p->gva && k->w == p->w && k->h == p->h && k->fmt == p->fmt) return k;
    }
    return NULL;
}
/* the slot's work is done (frame_recycle, before the slot records again): its times into the sums */
void vk_gpuprof_frame_done(u32 slot) {
    GpSlot* g = gp_on() ? g_slot[slot] : NULL;
    if (!g || !g->reset) return;
    g->reset = 0;
    if (!g->used) return;
    static u64 ts[GP_QUERIES];
    if (vkGetQueryPoolResults(g_vk.dev, g->pool, 0, g->used, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
#if defined(SNX_TRACY)
    /* the frame's timestamps onto the GPU track. The context is announced calibrated when the device can put its
       clock next to the client's (a fresh pair of the two every frame, vk_gpuprof_clock), with the anchor then
       that very pair; else the GPU clock is anchored at this moment: the last timestamp was written before
       timeline_wait returned, so this is its CPU time. */
    if (snx_tracy_attached()) {
        u64 conn = snx_tracy_connection_id();
        int cal = vk_gpuprof_calibrated();
        i64 cpu = 0, gpu = 0;
        int have = cal && vk_gpuprof_clock(&cpu, &gpu);
        if (conn != g_cal_conn) { /* a new connection's server re-anchored at the replayed context */
            g_cal_conn = conn;
            g_cal_cpu_prev = 0;
            g_cal_ok = 0; /* its announcement sets this again, as the client replays the context */
        }
        /* A context is announced calibrated only with a pair in hand: without one the server has no anchor and
           applies no calibration to the times sent after it, which shows as times far from the CPU's. The flag
           latches at the announcement, so a context announced without one is never sent calibration events:
           the server would assert on a context it was told is uncalibrated. */
        if (snx_tracy_gpu_context(VK_GPU_CTX_GRAPHICS, have ? gpu : (i64)ts[g->used - 1],
                                  g_vk.props.limits.timestampPeriod, have, "graphics")) {
            g_cal_cpu_prev = have ? cpu : 0; /* the anchor is the announcement's own pair */
            g_cal_ok = have;
        }
        if (have) {
            if (g_cal_ok && g_cal_cpu_prev) {
                i64 delta = cpu - g_cal_cpu_prev;
                if (delta > 0) snx_tracy_gpu_calibrate(gpu, delta, VK_GPU_CTX_GRAPHICS);
            }
            g_cal_cpu_prev = cpu;
        } else
            snx_tracy_gpu_sync((i64)ts[g->used - 1], VK_GPU_CTX_GRAPHICS);
        for (u32 i = 0; i + 1 < g->used; i += 2) {
            const GpPass* p = &g->pass[i / 2];
            if (!p->q0 || !p->q1) continue;
            snx_tracy_gpu_time((i64)ts[i], p->q0, VK_GPU_CTX_GRAPHICS, p->ep);
            snx_tracy_gpu_time((i64)ts[i + 1], p->q1, VK_GPU_CTX_GRAPHICS, p->ep);
        }
    }
#endif
    double period = g_vk.props.limits.timestampPeriod;
    for (u32 i = 0; i + 1 < g->used; i += 2) {
        u64 ns = (u64)((double)(ts[i + 1] - ts[i]) * period);
        GpKey* k = gp_key(&g->pass[i / 2]);
        g_pass_ns += ns;
        if (!k) continue;
        k->ns += ns;
        k->passes++;
        k->draws += g->pass[i / 2].draws;
    }
    for (u32 i = 2; i + 1 < g->used; i += 2) {
        const GpPass* p = &g->pass[i / 2];
        int c = p->dispatches ? 0 : p->xfers ? 1 : p->barriers ? 2 : 3;
        u64 gns = (u64)((double)(ts[i] - ts[i - 1]) * period);
        g_gap_ns[c] += gns;
        g_gaps[c]++;
        if (c == 1) {
            int m = p->kind[2] ? 2 : p->kind[1] ? 1 : 0;
            g_mix_ns[m] += gns;
            g_mixes[m]++;
            g_mix_bytes[m] += p->bytes;
            g_mix_n[m] += p->kind[0] + p->kind[1] + p->kind[2];
        }
    }
    g_span_ns += (u64)((double)(ts[g->used - 1] - ts[0]) * period);
    g_frames++;
    static u64 last;
    u64 now = plat_ns();
    if (!last) last = now;
    if (now - last >= 10000000000ull) {
        last = now;
        gp_report();
    }
}
#if defined(SNX_TRACY)
/* a fresh pair of the GPU's clock and the client's, one driver call: CLOCK_MONOTONIC_RAW is the clock the Tracy
   client keeps its own time in, so the deltas between the pairs map the device's timestamps onto it. 0 without
   the extension or when the driver refused. */
int vk_gpuprof_clock(i64* cpu, i64* gpu) {
    static const VkCalibratedTimestampInfoEXT spec[2] = {
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL, VK_TIME_DOMAIN_DEVICE_EXT},
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL, VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT}};
    u64 ts2[2], deviation;
    if (!vk_gpuprof_calibrated()) return 0;
    if (!VK_OK(vkGetCalibratedTimestampsEXT(g_vk.dev, 2, spec, ts2, &deviation))) return 0;
    *cpu = (i64)ts2[1];
    *gpu = (i64)ts2[0];
    return 1;
}
#endif
static int key_cmp(const void* a, const void* b) {
    u64 x = ((const GpKey*)a)->ns, y = ((const GpKey*)b)->ns;
    return x < y ? 1 : x > y ? -1 : 0;
}
/* the targets that took the most GPU time, then a fresh sum */
static void gp_report(void) {
    if (g_on <= 0 || !g_frames) return;
    static GpKey sorted[GP_KEYS];
    memcpy(sorted, g_key, sizeof sorted);
    qsort(sorted, GP_KEYS, sizeof *sorted, key_cmp);
    LOG("vulkan: GPU over %llu frames: %.2f ms a frame in passes, %.2f ms from the first pass to the last, %llu passes "
        "untimed\n",
        (unsigned long long)g_frames, (double)g_pass_ns / 1e6 / (double)g_frames,
        (double)g_span_ns / 1e6 / (double)g_frames, (unsigned long long)g_lost);
    for (int c = 0; c < 4; c++)
        if (g_gaps[c])
            LOG("vulkan: GPU   gaps %s: %.2f ms a frame, %llu of them\n", g_gap_name[c],
                (double)g_gap_ns[c] / 1e6 / (double)g_frames, (unsigned long long)g_gaps[c]);
    for (int m = 0; m < 3; m++)
        if (g_mixes[m])
            LOG("vulkan: GPU     copy gaps %s: %.2f ms a frame, %llu gaps, %llu copies, %.2f MB of buffer copies a frame\n",
                g_mix_name[m], (double)g_mix_ns[m] / 1e6 / (double)g_frames, (unsigned long long)g_mixes[m],
                (unsigned long long)g_mix_n[m], (double)g_mix_bytes[m] / 1048576.0 / (double)g_frames);
    memset(g_mix_ns, 0, sizeof g_mix_ns);
    memset(g_mixes, 0, sizeof g_mixes);
    memset(g_mix_bytes, 0, sizeof g_mix_bytes);
    memset(g_mix_n, 0, sizeof g_mix_n);
    memset(g_gap_ns, 0, sizeof g_gap_ns);
    memset(g_gaps, 0, sizeof g_gaps);
    for (int i = 0; i < 12 && sorted[i].passes; i++)
        LOG("vulkan: GPU   %.2f ms a frame: %llu passes, %llu draws, target %llx %ux%u format %u\n",
            (double)sorted[i].ns / 1e6 / (double)g_frames, (unsigned long long)sorted[i].passes,
            (unsigned long long)sorted[i].draws, (unsigned long long)sorted[i].gva, sorted[i].w, sorted[i].h,
            sorted[i].fmt);
    memset(g_key, 0, sizeof g_key);
    g_pass_ns = g_span_ns = g_frames = g_lost = 0;
}
