/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/maxwell_3d.cpp, src/video_core/engines/draw_manager.cpp,
   src/video_core/engines/maxwell_3d.h, src/common/cpu_features.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The 3D engine's methods (MAXWELL_B, 0xB197), as gpu.c's puller hands them over (maxwell3d_method): the MME shadow
   RAM, draws and their merging into instanced draws (Eden's DrawManager), inline indices, render enable, the report
   semaphores and counters shared with the compute engine (Eden's ProcessQueryGet), macro calls, barriers. What a
   draw or clear does with the state is maxwell3d.c's and the renderer's. Register names are maxwell_3d_regs.h's:
   method indices (byte offset / 4) */
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/maxwell/maxwell_3d_regs.h"

/* ------------------------------------------------------------ the file's state */
/* the counter reports whose made-up values a render enable must not compare (MADEUP_N, madeup_*) */
#define MADEUP_N 64
/* the kinds of draw that merge with their own kind only */
enum { DRAW_ARRAYS, DRAW_INDEXED, DRAW_ARRAY_INSTANCED, DRAW_IB32, DRAW_IB16, DRAW_IB8 };
/* r3d_draw's indexed (gpu.h) */
enum { R3D_DRAW_VERTICES = 0, R3D_DRAW_INDEX_BUFFER = 1, R3D_DRAW_INLINE_INDICES = 3 };
/* the renderer's method_barrier kinds (renderer.h) */
enum {
    BARRIER_TEXTURE = 0,
    BARRIER_TILED = 1,
    BARRIER_DATA_CACHE = 2,
    BARRIER_DESCRIPTOR_CACHE = 3,
    BARRIER_WAIT_FOR_IDLE = 4,
    BARRIER_SHADER_INSTRUCTIONS = 5
};
/* The draw held while the next ones may be more instances of it, as Eden's DrawManager holds an instanced draw
   (DrawMode::Instance) until a method that isn't one of its draws ends it (DrawDeferred). Unlike Eden it is one for
   all channels: another channel's method ends it too */
typedef struct {
    GpuChannel* ch; /* NULL: none */
    u32 kind;
    int indexed;
    u32 topology, first, count, base_vertex, base_instance;
    u32 start, n; /* the instance it started at, and its instances */
} DeferredDraw;
/* the GPU thread's alone */
static struct {
    u64 last_tick_ns; /* the guest time of the last timestamp (gpu_ticks) */
    u64 madeup[MADEUP_N];
    u32 madeup_next;
    DeferredDraw deferred;
} g_engines;

/* ------------------------------------------------------------ timestamps */
/* GM20B's timer counts at 614.4 MHz (Eden's common/cpu_features.h GPUTickFreq, its NsToGPUTickRatio): ticks =
   ns * 614400000 / 10^9 = ns * 384 / 625. With ns = q * 625 + rem, that is q * 384 + rem * 384 / 625 exactly
   (rem * 384 < 625 * 384 can't carry), which never overflows where ns * 384 would */
#define GPU_TICKS_PER_NS_NUM 384u
#define GPU_TICKS_PER_NS_DEN 625u
/* stamps step at least this many ns, more than one tick (625 / 384 ns), so that they strictly increase */
#define GPU_TICK_MIN_STEP_NS 16u
/* The timestamp a report or semaphore carries: the guest clock when the stream reaches it, as GPU ticks, past the
   one before. sead compares them with the CPU's tick. It is not real GPU time */
u64 gpu_ticks(void) {
    u64 ns = guest_ns();
    if (ns < g_engines.last_tick_ns + GPU_TICK_MIN_STEP_NS) ns = g_engines.last_tick_ns + GPU_TICK_MIN_STEP_NS;
    g_engines.last_tick_ns = ns;
    return ns / GPU_TICKS_PER_NS_DEN * GPU_TICKS_PER_NS_NUM +
           ns % GPU_TICKS_PER_NS_DEN * GPU_TICKS_PER_NS_NUM / GPU_TICKS_PER_NS_DEN;
}

/* ------------------------------------------------------------ made-up counter values */
/* Counter reports other than samples passed after its reset carry made-up values, and a condition comparing them
   could skip a draw the game needs: a render enable over an address that last got one draws. The last MADEUP_N such
   addresses; a release or a real count written there takes it out */
static int madeup_has(u64 a) {
    u32 i;
    for (i = 0; i < MADEUP_N; i++)
        if (a && g_engines.madeup[i] == a) return 1;
    return 0;
}
static void madeup_set(u64 a, int made) {
    u32 i;
    for (i = 0; i < MADEUP_N; i++)
        if (a && g_engines.madeup[i] == a) {
            if (!made) g_engines.madeup[i] = 0;
            return;
        }
    if (made && a) g_engines.madeup[g_engines.madeup_next++ % MADEUP_N] = a;
}

/* ------------------------------------------------------------ report semaphores */
/* a report's write: one word (short_query), or the value as 64 bits and the timestamp (Eden's StampQueryResult) */
static void report_write(u64 a, u64 value, int one) {
    u8 d[MAXWELL3D_REPORT_BYTES]; /* guest memory's bytes, little-endian */
    if (one) {
        snx_st_le32(d, (u32)value);
        gpu_release_write(a, d, 4);
    } else {
        snx_st_le64(d, value);
        snx_st_le64(d + 8, gpu_ticks());
        gpu_release_write(a, d, MAXWELL3D_REPORT_BYTES);
    }
}
#define REPORT_BIT(r) (1u << (r))
/* the counters a CLEAR_REPORT_VALUE resets, as REPORT_BIT of their reports */
static u32 reset_selectors(u32 v) {
    switch (v) {
        case CLEAR_REPORT_ZPASS_PIXEL_COUNT:
            return REPORT_BIT(REPORT_ZPASS_PIXEL_COUNT64) | REPORT_BIT(REPORT_ZPASS_PIXEL_COUNT);
        case CLEAR_REPORT_ZCULL_STATS:
            return REPORT_BIT(REPORT_ZCULL_STATS0) | REPORT_BIT(REPORT_ZCULL_STATS1) | REPORT_BIT(REPORT_ZCULL_STATS2) |
                   REPORT_BIT(REPORT_ZCULL_STATS3);
        case CLEAR_REPORT_STREAMING_PRIMITIVES_SUCCEEDED: return REPORT_BIT(REPORT_STREAMING_PRIMITIVES_SUCCEEDED);
        case CLEAR_REPORT_VERTICES_GENERATED: return REPORT_BIT(REPORT_VERTICES_GENERATED);
        case CLEAR_REPORT_PRIMITIVES_GENERATED:
            return REPORT_BIT(REPORT_PRIMITIVES_GENERATED) | REPORT_BIT(REPORT_VTG_PRIMITIVES_OUT);
        case CLEAR_REPORT_VERTEX_SHADER_INVOCATIONS: return REPORT_BIT(REPORT_VERTEX_SHADER_INVOCATIONS);
        case CLEAR_REPORT_TESSELLATION_INIT_INVOCATIONS: return REPORT_BIT(REPORT_TESSELLATION_INIT_INVOCATIONS);
        case CLEAR_REPORT_TESSELLATION_SHADER_INVOCATIONS: return REPORT_BIT(REPORT_TESSELLATION_SHADER_INVOCATIONS);
        case CLEAR_REPORT_TESSELLATION_SHADER_PRIMITIVES_GENERATED:
            return REPORT_BIT(REPORT_TESSELLATION_SHADER_PRIMITIVES_GENERATED);
        case CLEAR_REPORT_GEOMETRY_SHADER_INVOCATIONS: return REPORT_BIT(REPORT_GEOMETRY_SHADER_INVOCATIONS);
        case CLEAR_REPORT_GEOMETRY_SHADER_PRIMITIVES_GENERATED:
            return REPORT_BIT(REPORT_GEOMETRY_SHADER_PRIMITIVES_GENERATED);
        case CLEAR_REPORT_CLIPPER_INVOCATIONS: return REPORT_BIT(REPORT_CLIPPER_INVOCATIONS);
        case CLEAR_REPORT_CLIPPER_PRIMITIVES_GENERATED: return REPORT_BIT(REPORT_CLIPPER_PRIMITIVES_GENERATED);
        case CLEAR_REPORT_PIXEL_SHADER_INVOCATIONS: return REPORT_BIT(REPORT_PIXEL_SHADER_INVOCATIONS);
        /* Ryujinx and Eden disagree on what this one resets */
        case CLEAR_REPORT_VTG_PRIMITIVES_OUT: return REPORT_BIT(REPORT_VTG_PRIMITIVES_OUT);
        default: return 0;
    }
}
/* CLEAR_REPORT_VALUE: the counters reset count from the draws so far; samples passed is counted for real from the
   reset of ZPASS_PIXEL_COUNT on */
static void counter_reset(GpuChannel* ch, u32 v) {
    u32 mask = reset_selectors(v), s;
    for (s = 0; s < REPORT_COUNT; s++)
        if (mask & REPORT_BIT(s)) ch->counter_base[s] = g_draws;
    ch->counter_reset |= mask;
    if (v == CLEAR_REPORT_ZPASS_PIXEL_COUNT && g_renderer->samples_reset) g_renderer->samples_reset(ch);
    if (!mask) LOG_LIMIT(8, "gpu: counter reset %x\n", v);
}
/* made-up counts: three vertices a draw for vertices generated, this many for the others */
#define MADEUP_VERTICES_PER_DRAW 3u
#define MADEUP_COUNT 1000u
/* a REPORT_ONLY of report sel: the payload for REPORT_PAYLOAD (as Eden finds the hardware gives), samples passed
   counted by the renderer once reset, else a made-up count */
static void counter_report(GpuChannel* ch, u64 a, u32 sel, u32 payload, int one) {
    u64 v;
    if (sel == REPORT_PAYLOAD) {
        report_write(a, payload, one);
        madeup_set(a, 0);
        return;
    }
    if ((sel == REPORT_ZPASS_PIXEL_COUNT64 || sel == REPORT_ZPASS_PIXEL_COUNT) &&
        (ch->counter_reset & REPORT_BIT(sel)) && g_renderer->samples_report &&
        g_renderer->samples_report(ch, a, one ? 4 : MAXWELL3D_REPORT_BYTES, gpu_ticks())) {
        madeup_set(a, 0);
        return;
    }
    u64 per_draw = sel == REPORT_VERTICES_GENERATED ? MADEUP_VERTICES_PER_DRAW : MADEUP_COUNT;
    if (ch->counter_reset & REPORT_BIT(sel))
        v = (g_draws - ch->counter_base[sel]) * per_draw;
    else
        v = sel == REPORT_VERTICES_GENERATED ? g_draws * MADEUP_VERTICES_PER_DRAW : MADEUP_COUNT;
    report_write(a, v, one);
    madeup_set(a, 1);
}
void query_get(GpuChannel* ch, u32* r, int base, u32 v) {
    u64 a = gpu_packed_va(r[base], r[base + 1]);
    u32 payload = r[base + 2];
    int one = report_semaphore_short_query(v); /* release, bit 4, is its timing (after reads or writes), not its size */
    if (report_semaphore_reduction_enable(v))
        LOG_LIMIT(4, "gpu: report semaphore reduction %u is not emulated: the payload is written as it is\n",
                  report_semaphore_reduction_op(v));
    switch (report_semaphore_operation(v)) {
        case REPORT_SEMAPHORE_OPERATION_RELEASE:
            report_write(a, payload, one);
            madeup_set(a, 0);
            break;
        case REPORT_SEMAPHORE_OPERATION_ACQUIRE:
            gpu_park(ch, GPU_PARK_REPORT, a, payload,
                     report_semaphore_comparison(v) == REPORT_SEMAPHORE_COMPARISON_GREATER_OR_EQUAL);
            break;
        case REPORT_SEMAPHORE_OPERATION_REPORT_ONLY:
            counter_report(ch, a, report_semaphore_report(v), payload, one);
            break;
        default: LOG_LIMIT(4, "gpu: report semaphore trap %08x is not emulated: nothing is written\n", v); break;
    }
}

/* ------------------------------------------------------------ render enable */
/* Decided when the mode or the override is written, as Eden's ProcessQueryCondition, and kept until one of them is
   written again */
static void render_enable_update(GpuChannel* ch) {
    const u32* r = ch->r3d;
    u64 a = gpu_packed_va(r[MAXWELL3D_RENDER_ENABLE_ADDRESS_HIGH], r[MAXWELL3D_RENDER_ENABLE_ADDRESS_LOW]);
    u32 mode = r[MAXWELL3D_RENDER_ENABLE_MODE] & MAXWELL3D_RENDER_ENABLE_MODE_MASK, pred = 0;
    int on = 1, known;
    if (ch->render_pred) {
        g_renderer->render_predicate_drop(ch->render_pred);
        ch->render_pred = 0;
    }
    switch (r[MAXWELL3D_RENDER_ENABLE_OVERRIDE] & MAXWELL3D_RENDER_ENABLE_OVERRIDE_MASK) {
        case MAXWELL3D_RENDER_ENABLE_OVERRIDE_ALWAYS_RENDER: on = 1; break;
        case MAXWELL3D_RENDER_ENABLE_OVERRIDE_NEVER_RENDER: on = 0; break;
        default:
            switch (mode) {
                case MAXWELL3D_RENDER_ENABLE_FALSE: on = 0; break;
                case MAXWELL3D_RENDER_ENABLE_TRUE: on = 1; break;
                case MAXWELL3D_RENDER_ENABLE_CONDITIONAL: { /* both words of the report's value set */
                    u32 w[2];
                    gmem_read(a, w, 8);
                    on = madeup_has(a) || (w[0] && w[1]);
                    break;
                }
                case MAXWELL3D_RENDER_ENABLE_IF_EQUAL: /* two reports' values (their stamps don't count) */
                case MAXWELL3D_RENDER_ENABLE_IF_NOT_EQUAL: {
                    u64 x, y;
                    int equal = mode == MAXWELL3D_RENDER_ENABLE_IF_EQUAL;
                    if (madeup_has(a) || madeup_has(a + MAXWELL3D_REPORT_BYTES)) {
                        on = 1;
                        break;
                    }
                    /* NVN's occlusion test, a count against zero: while the count isn't known the GPU compares, and
                       the draws wait for it there. Reading it here waited for the GPU to run everything recorded
                       until then, with the GPU idle meanwhile (renderer.h render_predicate) */
                    known = g_renderer->render_predicate ? g_renderer->render_predicate(a, equal, &pred) : -1;
                    if (known >= 0) {
                        on = known != 0;
                        if (known == 2) ch->render_pred = pred;
                        break;
                    }
                    gmem_read(a, &x, 8);
                    gmem_read(a + MAXWELL3D_REPORT_BYTES, &y, 8);
                    on = (x == y) == equal;
                    break;
                }
                default: LOG_LIMIT(4, "gpu: render enable mode %u draws\n", mode); break;
            }
            break;
    }
    ch->render_off = !on;
}

/* ------------------------------------------------------------ draws */
int r3d_pending_on(void) { return g_engines.deferred.ch != NULL; }
/* the held draw, drawn (Eden's DrawDeferred) */
void r3d_pending_flush(void) {
    DeferredDraw* p = &g_engines.deferred;
    GpuChannel* ch = p->ch;
    if (!ch) return;
    p->ch = NULL;
    r3d_draw(ch, p->indexed, p->topology, p->first, p->count, p->base_vertex, p->base_instance + p->start, p->n);
}
/* a draw of instance inst: one more instance of the held draw when everything else is the same, else the held one
   is drawn and this one held */
static void queue_draw(GpuChannel* ch, u32 kind, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex,
                       u32 inst) {
    DeferredDraw* p = &g_engines.deferred;
    u32 bi = ch->r3d[MAXWELL3D_GLOBAL_BASE_INSTANCE_INDEX];
    if (p->ch == ch && p->kind == kind && p->topology == topology && p->first == first && p->count == count &&
        p->base_vertex == base_vertex && p->base_instance == bi && p->start + p->n == inst) {
        p->n++;
        return;
    }
    r3d_pending_flush();
    p->ch = ch;
    p->kind = kind;
    p->indexed = indexed;
    p->topology = topology;
    p->first = first;
    p->count = count;
    p->base_vertex = base_vertex;
    p->base_instance = bi;
    p->start = inst;
    p->n = 1;
}
/* Draw.begin (Eden's DrawManager::DrawBegin): the next instance (SUBSEQUENT, or the undefined 3), the first, or the
   same (UNCHANGED) */
static void draw_begin(GpuChannel* ch, u32 v) {
    u32 id = draw_begin_instance_id(v);
    if (id & MAXWELL3D_DRAW_INSTANCE_SUBSEQUENT)
        ch->instance_count++;
    else if (id == MAXWELL3D_DRAW_INSTANCE_FIRST)
        ch->instance_count = 0;
}
/* Draw.end (Eden's DrawManager::DrawEnd): the inline indices when some came, else the index buffer's range when its
   count is set (and then used up), else the vertex range */
static void draw_end(GpuChannel* ch) {
    u32* r = ch->r3d;
    u32 topology = draw_begin_topology(r[MAXWELL3D_DRAW_BEGIN]);
    if (ch->inline_n) {
        r3d_pending_flush();
        r3d_draw(ch, R3D_DRAW_INLINE_INDICES, topology, 0, ch->inline_n, r[MAXWELL3D_GLOBAL_BASE_VERTEX_INDEX],
                 r[MAXWELL3D_GLOBAL_BASE_INSTANCE_INDEX] + ch->instance_count, 1);
        ch->inline_n = 0;
    } else if (r[MAXWELL3D_INDEX_BUFFER_COUNT]) {
        queue_draw(ch, DRAW_INDEXED, R3D_DRAW_INDEX_BUFFER, topology, r[MAXWELL3D_INDEX_BUFFER_FIRST],
                   r[MAXWELL3D_INDEX_BUFFER_COUNT], r[MAXWELL3D_GLOBAL_BASE_VERTEX_INDEX], ch->instance_count);
        r[MAXWELL3D_INDEX_BUFFER_COUNT] = 0;
    } else
        queue_draw(ch, DRAW_ARRAYS, R3D_DRAW_VERTICES, topology, r[MAXWELL3D_VERTEX_BUFFER_FIRST],
                   r[MAXWELL3D_VERTEX_BUFFER_COUNT], 0, ch->instance_count);
}
/* the inline index methods' indices, widened to u32 (Eden's SetInlineIndexBuffer), INLINE_MAX at most */
#define INLINE_MAX (16u << 20)
#define INLINE_FIRST_CAP 1024u
static void inline_push(GpuChannel* ch, const u32* idx, u32 n) {
    if (ch->inline_n + n > ch->inline_cap) {
        u32 cap = ch->inline_cap ? ch->inline_cap * 2 : INLINE_FIRST_CAP;
        u32* b;
        if (ch->inline_n + n > INLINE_MAX) {
            WARN_LIMIT(4, "gpu: more than %u inline indices: the rest are dropped\n", INLINE_MAX);
            return;
        }
        if (cap > INLINE_MAX) cap = INLINE_MAX;
        b = (u32*)realloc(ch->inline_indices, (size_t)cap * sizeof *b);
        if (!b) {
            WARN_LIMIT(4, "gpu: no memory for %u inline indices\n", cap);
            return;
        }
        ch->inline_indices = b;
        ch->inline_cap = cap;
    }
    memcpy(ch->inline_indices + ch->inline_n, idx, (size_t)n * sizeof *idx);
    ch->inline_n += n;
}
/* ------------------------------------------------------------ the shadow RAM and the merged draw */
/* the methods the shadow RAM leaves alone: the class's own, the window origin, and the data streams */
static int shadowed(u32 m) {
    return m >= MAXWELL3D_SHADOW_FIRST && m != MAXWELL3D_WINDOW_ORIGIN && m != MAXWELL3D_INLINE_INDEX_4X8_INDEX0 &&
           m != MAXWELL3D_DRAW_INLINE_INDEX && m != MAXWELL3D_INLINE_INDEX_2X16_EVEN &&
           !(m >= MAXWELL3D_CONST_BUFFER_BUFFER && m <= MAXWELL3D_CONST_BUFFER_BUFFER_LAST);
}
/* a transform feedback buffer's start_offset, the write pointer its load resets */
#define XFB_START_OFFSET(i) \
    (MAXWELL3D_TRANSFORM_FEEDBACK_BUFFERS + MAXWELL3D_TRANSFORM_FEEDBACK_BUFFER_STRIDE * (i) + \
     MAXWELL3D_TRANSFORM_FEEDBACK_BUFFER_START_OFFSET)
/* a stage's bind group config, which binds a constant buffer */
#define BIND_GROUP_CONFIG(i) \
    (MAXWELL3D_BIND_GROUPS + MAXWELL3D_BIND_GROUP_STRIDE * (i) + MAXWELL3D_BIND_GROUP_RAW_CONFIG)
/* whether a write of v to m leaves the held draw alone: what the next instance of it rewrites, and any other register
   written with the value it has, except the methods that act */
static int keeps_draw(const u32* r, u32 m, u32 v) {
    switch (m) {
        case MAXWELL3D_DRAW_BEGIN: return draw_begin_instance_id(v) & MAXWELL3D_DRAW_INSTANCE_SUBSEQUENT;
        case MAXWELL3D_VERTEX_ARRAY_INSTANCE_SUBSEQUENT:
        case MAXWELL3D_DRAW_END:
        case MAXWELL3D_VERTEX_BUFFER_FIRST:
        case MAXWELL3D_VERTEX_BUFFER_COUNT:
        case MAXWELL3D_INDEX_BUFFER_FIRST:
        case MAXWELL3D_INDEX_BUFFER_COUNT:
        case MAXWELL3D_INDEX_BUFFER32_SUBSEQUENT:
        case MAXWELL3D_INDEX_BUFFER16_SUBSEQUENT:
        case MAXWELL3D_INDEX_BUFFER8_SUBSEQUENT: return 1;
        case MAXWELL3D_WAIT_FOR_IDLE:
        case MAXWELL3D_LOAD_MME_INSTRUCTION_PTR:
        case MAXWELL3D_LOAD_MME_INSTRUCTION:
        case MAXWELL3D_LOAD_MME_START_ADDRESS_PTR:
        case MAXWELL3D_LOAD_MME_START_ADDRESS:
        case MAXWELL3D_LAUNCH_DMA:
        /* a transform feedback buffer's write pointer: draws held before it capture where it was */
        case XFB_START_OFFSET(0):
        case XFB_START_OFFSET(1):
        case XFB_START_OFFSET(2):
        case XFB_START_OFFSET(3):
        case MAXWELL3D_TRANSFORM_FEEDBACK_ENABLED:
        case MAXWELL3D_INLINE_DATA:
        case MAXWELL3D_SYNC_INFO:
        case MAXWELL3D_INVALIDATE_SHADER_CACHE:
        case MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI:
        case MAXWELL3D_FRAGMENT_BARRIER:
        case MAXWELL3D_INVALIDATE_TEXTURE_DATA_CACHE:
        case MAXWELL3D_TILED_CACHE_BARRIER:
        case MAXWELL3D_DRAW_TEXTURE_SRC_Y0:
        case MAXWELL3D_INLINE_INDEX_4X8_INDEX0:
        case MAXWELL3D_INVALIDATE_SAMPLER_CACHE_NO_WFI:
        case MAXWELL3D_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI:
        case MAXWELL3D_ZPASS_PIXEL_COUNT_ENABLE:
        case MAXWELL3D_CLEAR_REPORT_VALUE:
        case MAXWELL3D_RENDER_ENABLE_MODE:
        case MAXWELL3D_DRAW_INLINE_INDEX:
        case MAXWELL3D_INLINE_INDEX_2X16_EVEN:
        case MAXWELL3D_RENDER_ENABLE_OVERRIDE:
        case MAXWELL3D_CLEAR_SURFACE:
        case MAXWELL3D_REPORT_SEMAPHORE_QUERY:
        case MAXWELL3D_FIRMWARE_CALL_4:
        case BIND_GROUP_CONFIG(0):
        case BIND_GROUP_CONFIG(1):
        case BIND_GROUP_CONFIG(2):
        case BIND_GROUP_CONFIG(3):
        case BIND_GROUP_CONFIG(4): return 0;
        default: return r[m] == v;
    }
}
static void barrier(int kind) {
    if (g_renderer->method_barrier) g_renderer->method_barrier(kind);
}
/* a macro call (MAXWELL3D_MACRO_REGISTERS_START up: the even method starts a call, the odd one takes more
   parameters): it runs at the batch's last word */
static void macro_call(GpuChannel* ch, u32 m, u32 v, int last) {
    if (!ch->mme_collecting) {
        ch->mme_collecting = 1;
        ch->mme_index = (m - MAXWELL3D_MACRO_REGISTERS_START) >> 1;
        ch->mme_nparams = 0;
    }
    if (ch->mme_nparams < sizeof ch->mme_params / sizeof ch->mme_params[0])
        ch->mme_params[ch->mme_nparams++] = v;
    else
        WARN_LIMIT(4, "gpu: a macro call's parameters past %u are dropped\n", ch->mme_nparams);
    if (last) {
        ch->mme_collecting = 0;
        mme_run(ch, ch->mme_index, ch->mme_nparams);
    }
}

/* the methods below the macros': G_ACTS for those that do more than store their value (those the switch in
   maxwell3d_method acts on, those keeps_draw names, the constant buffer's data, and those the shadow RAM leaves alone:
   shadowed), and in the low bits the register groups (R3D_*, gpu.h) the method's register is in. Every other method,
   most of a stream, is a plain register: maxwell3d_method stores it without the switch, a call per method word that
   was the GPU thread's biggest single cost. The groups are the bits themselves, not an index into a table of masks,
   as this runs for nearly every method word: more groups than 7 widen the table */
enum { G_ACTS = 0x80 };
_Static_assert(R3D_XFB < G_ACTS, "the 3D register groups fit below G_ACTS");
/* a vertex stream's stride and enable word and its frequency (divisor), around its address */
#define M3D_STREAM(i)                                                                     \
    [MAXWELL3D_VERTEX_STREAMS + MAXWELL3D_VERTEX_STREAM_STRIDE * (i)] = R3D_VFORMAT,      \
    [MAXWELL3D_VERTEX_STREAMS + MAXWELL3D_VERTEX_STREAM_STRIDE * (i) +                    \
     MAXWELL3D_VERTEX_STREAM_FREQUENCY] = R3D_VFORMAT
/* a color target's format */
#define M3D_RT_FORMAT(i) [MAXWELL3D_RT + MAXWELL3D_RT_STRIDE * (i) + MAXWELL3D_RT_FORMAT] = R3D_RTFORMAT
static const u8 g_m3d_group[MAXWELL3D_MACRO_REGISTERS_START] = {
    [MAXWELL3D_INVALIDATE_SHADER_CACHE] = G_ACTS, [MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI] = G_ACTS,
    [MAXWELL3D_WAIT_FOR_IDLE] = G_ACTS, [MAXWELL3D_LOAD_MME_INSTRUCTION_PTR] = G_ACTS,
    [MAXWELL3D_LOAD_MME_INSTRUCTION] = G_ACTS, [MAXWELL3D_LOAD_MME_START_ADDRESS_PTR] = G_ACTS,
    [MAXWELL3D_LOAD_MME_START_ADDRESS] = G_ACTS, [MAXWELL3D_SHADOW_RAM_CONTROL] = G_ACTS,
    [MAXWELL3D_LAUNCH_DMA] = G_ACTS, [MAXWELL3D_INLINE_DATA] = G_ACTS, [MAXWELL3D_SYNC_INFO] = G_ACTS,
    [XFB_START_OFFSET(0)] = G_ACTS, [XFB_START_OFFSET(1)] = G_ACTS, [XFB_START_OFFSET(2)] = G_ACTS,
    [XFB_START_OFFSET(3)] = G_ACTS, [MAXWELL3D_TRANSFORM_FEEDBACK_ENABLED] = G_ACTS | R3D_XFB,
    [MAXWELL3D_VERTEX_BUFFER_FIRST] = G_ACTS, [MAXWELL3D_VERTEX_BUFFER_COUNT] = G_ACTS,
    [MAXWELL3D_FRAGMENT_BARRIER] = G_ACTS, [MAXWELL3D_INVALIDATE_TEXTURE_DATA_CACHE] = G_ACTS,
    [MAXWELL3D_TILED_CACHE_BARRIER] = G_ACTS, [MAXWELL3D_DRAW_TEXTURE_SRC_Y0] = G_ACTS,
    [MAXWELL3D_VERTEX_ARRAY_INSTANCE_FIRST] = G_ACTS, [MAXWELL3D_VERTEX_ARRAY_INSTANCE_SUBSEQUENT] = G_ACTS,
    [MAXWELL3D_INLINE_INDEX_4X8_INDEX0] = G_ACTS, [MAXWELL3D_WINDOW_ORIGIN] = G_ACTS,
    [MAXWELL3D_INVALIDATE_SAMPLER_CACHE_NO_WFI] = G_ACTS, [MAXWELL3D_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI] = G_ACTS,
    [MAXWELL3D_ZPASS_PIXEL_COUNT_ENABLE] = G_ACTS, [MAXWELL3D_CLEAR_REPORT_VALUE] = G_ACTS,
    [MAXWELL3D_RENDER_ENABLE_MODE] = G_ACTS, [MAXWELL3D_DRAW_INLINE_INDEX] = G_ACTS,
    [MAXWELL3D_INLINE_INDEX_2X16_EVEN] = G_ACTS, [MAXWELL3D_DRAW_END] = G_ACTS, [MAXWELL3D_DRAW_BEGIN] = G_ACTS,
    [MAXWELL3D_INDEX_BUFFER_FIRST] = G_ACTS, [MAXWELL3D_INDEX_BUFFER_COUNT] = G_ACTS,
    [MAXWELL3D_INDEX_BUFFER32_FIRST] = G_ACTS, [MAXWELL3D_INDEX_BUFFER16_FIRST] = G_ACTS,
    [MAXWELL3D_INDEX_BUFFER8_FIRST] = G_ACTS, [MAXWELL3D_INDEX_BUFFER32_SUBSEQUENT] = G_ACTS,
    [MAXWELL3D_INDEX_BUFFER16_SUBSEQUENT] = G_ACTS, [MAXWELL3D_INDEX_BUFFER8_SUBSEQUENT] = G_ACTS,
    [MAXWELL3D_RENDER_ENABLE_OVERRIDE] = G_ACTS, [MAXWELL3D_CLEAR_SURFACE] = G_ACTS,
    [MAXWELL3D_REPORT_SEMAPHORE_QUERY] = G_ACTS, [MAXWELL3D_FIRMWARE_CALL_4] = G_ACTS,
    [MAXWELL3D_CONST_BUFFER_BUFFER ... MAXWELL3D_CONST_BUFFER_BUFFER_LAST] = G_ACTS,
    [BIND_GROUP_CONFIG(0)] = G_ACTS, [BIND_GROUP_CONFIG(1)] = G_ACTS, [BIND_GROUP_CONFIG(2)] = G_ACTS,
    [BIND_GROUP_CONFIG(3)] = G_ACTS, [BIND_GROUP_CONFIG(4)] = G_ACTS,
    /* the groups' plain registers */
    [MAXWELL3D_MANDATED_EARLY_Z] = R3D_SHADER, [MAXWELL3D_PROGRAM_REGION ... MAXWELL3D_PROGRAM_REGION + 1] = R3D_SHADER,
    [MAXWELL3D_PIPELINES ... MAXWELL3D_PIPELINES + 0x5F] = R3D_SHADER,
    [MAXWELL3D_VERTEX_ATTRIB_FORMAT ... MAXWELL3D_VERTEX_ATTRIB_FORMAT + 0x1F] = R3D_VATTR,
    [MAXWELL3D_VERTEX_STREAM_INSTANCES ... MAXWELL3D_VERTEX_STREAM_INSTANCES + 0x1F] = R3D_VFORMAT,
    M3D_STREAM(0), M3D_STREAM(1), M3D_STREAM(2), M3D_STREAM(3), M3D_STREAM(4), M3D_STREAM(5), M3D_STREAM(6),
    M3D_STREAM(7), M3D_STREAM(8), M3D_STREAM(9), M3D_STREAM(10), M3D_STREAM(11), M3D_STREAM(12), M3D_STREAM(13),
    M3D_STREAM(14), M3D_STREAM(15), M3D_STREAM(16), M3D_STREAM(17), M3D_STREAM(18), M3D_STREAM(19), M3D_STREAM(20),
    M3D_STREAM(21), M3D_STREAM(22), M3D_STREAM(23), M3D_STREAM(24), M3D_STREAM(25), M3D_STREAM(26), M3D_STREAM(27),
    M3D_STREAM(28), M3D_STREAM(29), M3D_STREAM(30), M3D_STREAM(31),
    [MAXWELL3D_RT_CONTROL] = R3D_RTFORMAT, M3D_RT_FORMAT(0), M3D_RT_FORMAT(1), M3D_RT_FORMAT(2), M3D_RT_FORMAT(3),
    M3D_RT_FORMAT(4), M3D_RT_FORMAT(5), M3D_RT_FORMAT(6), M3D_RT_FORMAT(7),
    [MAXWELL3D_COLOR_MASK_COMMON] = R3D_BLEND, [MAXWELL3D_BLEND_PER_TARGET_ENABLED] = R3D_BLEND,
    [MAXWELL3D_BLEND + 1 ... MAXWELL3D_BLEND + 0x10] = R3D_BLEND, [MAXWELL3D_FRAMEBUFFER_SRGB] = R3D_BLEND,
    [MAXWELL3D_LOGIC_OP ... MAXWELL3D_LOGIC_OP + 1] = R3D_BLEND,
    [MAXWELL3D_COLOR_MASK ... MAXWELL3D_COLOR_MASK + 7] = R3D_BLEND,
    [MAXWELL3D_BLEND_PER_TARGET ... MAXWELL3D_BLEND_PER_TARGET + 0x3F] = R3D_BLEND,
    [MAXWELL3D_TESSELLATION] = R3D_PIPEMISC, [MAXWELL3D_POLYGON_MODE_FRONT] = R3D_PIPEMISC,
    [MAXWELL3D_PATCH_VERTICES] = R3D_PIPEMISC,
    [MAXWELL3D_POST_VTG_SHADER_ATTRIB_SKIP_MASK ... MAXWELL3D_POST_VTG_SHADER_ATTRIB_SKIP_MASK + 7] = R3D_PIPEMISC,
    [MAXWELL3D_USER_CLIP_ENABLE] = R3D_PIPEMISC, [MAXWELL3D_PROVOKING_VERTEX] = R3D_PIPEMISC,
    [MAXWELL3D_VIEWPORT_CLIP_CONTROL] = R3D_PIPEMISC,
    [MAXWELL3D_TRANSFORM_FEEDBACK_CONTROLS ... MAXWELL3D_TRANSFORM_FEEDBACK_CONTROLS + 0xF] = R3D_XFB,
    [MAXWELL3D_STREAM_OUT_LAYOUT ... MAXWELL3D_STREAM_OUT_LAYOUT + 0x7F] = R3D_XFB};
#undef M3D_STREAM
#undef M3D_RT_FORMAT
/* inline indices packed in one word, 32 / bits of them, the low ones first (InlineIndex4x8, InlineIndex2x16,
   draw_inline_index). Out of line: its array, inlined, made maxwell3d_method check a stack canary on every method */
__attribute__((noinline)) static void inline_push_packed(GpuChannel* ch, u32 v, u32 bits) {
    u32 idx[32 / MAXWELL3D_INLINE_INDEX_BITS_4X8], n = 32 / bits, i, mask = bits == 32 ? ~0u : (1u << bits) - 1;
    for (i = 0; i < n; i++) idx[i] = (v >> (i * bits)) & mask;
    inline_push(ch, idx, n);
}

/* a plain register's value v, after the draw held, which v changes or which is another channel's: out of line, so the
   path that calls nothing (maxwell3d_method) saves no registers */
__attribute__((noinline)) static void plain_flushed(GpuChannel* ch, u32 m, u32 v) {
    r3d_pending_flush();
    if (ch->r3d[m] == v) return;
    ch->r3d_dirty |= g_m3d_group[m];
    ch->r3d[m] = v;
}
/* the shadow RAM's part in a write of v to m: the value replayed, or tracked */
static inline u32 shadow_ram(GpuChannel* ch, u32 m, u32 v) {
    if (ch->shadow_mode == MAXWELL3D_SHADOW_RAM_REPLAY) return ch->r3d_shadow[m];
    if (ch->shadow_mode <= MAXWELL3D_SHADOW_RAM_TRACK_WITH_FILTER) ch->r3d_shadow[m] = v;
    return v;
}
/* the methods that act, the macros' and a plain register while constant buffer data is held */
__attribute__((noinline)) static void method_acts(GpuChannel* ch, u32 m, u32 v, int last);
/* A 3D engine method (Eden's Maxwell3D::CallMethod). A plain register (most method words) is stored without the
   switch, and a value as it was needs no store and changes nothing a held draw or the renderer read. The rest goes
   out of line: with every call in this function, it saved and restored five registers or more for each method word */
void maxwell3d_method(GpuChannel* ch, u32 m, u32 v, int last) {
    u32* r = ch->r3d;
    m &= 0xFFF; /* the 3D engine's 0x1000 methods (GpuChannel.r3d) */
    if (m >= MAXWELL3D_MACRO_REGISTERS_START || (g_m3d_group[m] & G_ACTS) || ch->cb_n) {
        method_acts(ch, m, v, last);
        return;
    }
    if (m >= MAXWELL3D_SHADOW_FIRST) v = shadow_ram(ch, m, v);
    if (r[m] != v) {
        if (g_engines.deferred.ch) {
            plain_flushed(ch, m, v);
            return;
        }
        ch->r3d_dirty |= g_m3d_group[m];
        r[m] = v;
    } else if (g_engines.deferred.ch && g_engines.deferred.ch != ch)
        plain_flushed(ch, m, v);
}
static void method_acts(GpuChannel* ch, u32 m, u32 v, int last) {
    u32* r = ch->r3d;
    /* a plain register after the held constant buffer data */
    if (m < MAXWELL3D_MACRO_REGISTERS_START && !(g_m3d_group[m] & G_ACTS)) {
        r3d_const_flush(ch);
        maxwell3d_method(ch, m, v, last);
        return;
    }
    if (m >= MAXWELL3D_MACRO_REGISTERS_START) {
        r3d_const_flush(ch);
        r[m] = v;
        macro_call(ch, m, v, last);
        return;
    }
    if (m >= MAXWELL3D_CONST_BUFFER_BUFFER && m <= MAXWELL3D_CONST_BUFFER_BUFFER_LAST) {
        r3d_pending_flush(); /* constant buffer data: the held draw read the constants before it */
        r[m] = v;
        r3d_const_upload(ch, v, last);
        return;
    }
    r3d_const_flush(ch);
    if (shadowed(m)) v = shadow_ram(ch, m, v);
    if (g_engines.deferred.ch && (g_engines.deferred.ch != ch || !keeps_draw(r, m, v))) r3d_pending_flush();
    if (r[m] != v) ch->r3d_dirty |= g_m3d_group[m] & ~G_ACTS;
    r[m] = v;
    switch (m) { /* a new case, or a new one in keeps_draw, needs G_ACTS in its g_m3d_group too */
        case MAXWELL3D_WAIT_FOR_IDLE:
            /* The idle wait and the texture cache invalidation below order GPU work, which method_barrier gives
               Vulkan. Neither is anything the game sees, so pending shader results don't land here: whatever reads
               guest memory lands the results over the bytes it reads (gmem_need, through gmem_ptr for textures and
               render targets), and a release lands them before the game can see it (release.c). Landing every
               result at each of them cost one game several vk_finish waits a frame */
            barrier(BARRIER_WAIT_FOR_IDLE);
            break;
        case MAXWELL3D_LOAD_MME_INSTRUCTION_PTR: mme_upload_ptr(ch, v); break; /* begins an upload batch */
        case MAXWELL3D_LOAD_MME_INSTRUCTION: mme_upload(ch, v); break;
        case MAXWELL3D_LOAD_MME_START_ADDRESS_PTR: ch->mme_start_ptr = v; break;
        case MAXWELL3D_LOAD_MME_START_ADDRESS: mme_bind(ch, v); break;
        case MAXWELL3D_SHADOW_RAM_CONTROL: ch->shadow_mode = v & MAXWELL3D_SHADOW_RAM_CONTROL_MASK; break;
        case MAXWELL3D_LAUNCH_DMA: i2m_launch(ch, r, MAXWELL3D_UPLOAD); break;
        case MAXWELL3D_INLINE_DATA: i2m_data(ch, v); break;
        case MAXWELL3D_SYNC_INFO: /* the syncpoint once the work before it is done, when the condition is ROP writes */
            if (!sync_info_rop_writes_done(v))
                LOG_LIMIT(4, "gpu: SyncInfo %08x without bit 20 doesn't increment\n", v);
            else if (g_renderer->stream_incr)
                g_renderer->stream_incr(sync_info_sync_point(v));
            break;
        case MAXWELL3D_INVALIDATE_SHADER_CACHE:
        case MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI:
            if (invalidate_shader_cache_instruction(v)) barrier(BARRIER_SHADER_INSTRUCTIONS);
            break;
        case MAXWELL3D_FRAGMENT_BARRIER: barrier(BARRIER_TEXTURE); break;
        case MAXWELL3D_INVALIDATE_TEXTURE_DATA_CACHE: barrier(BARRIER_DATA_CACHE); break;
        case MAXWELL3D_TILED_CACHE_BARRIER: barrier(BARRIER_TILED); break;
        case MAXWELL3D_DRAW_TEXTURE_SRC_Y0: r3d_draw_texture(ch); break;
        case MAXWELL3D_INLINE_INDEX_4X8_INDEX0: inline_push_packed(ch, v, MAXWELL3D_INLINE_INDEX_BITS_4X8); break;
        case MAXWELL3D_VERTEX_ARRAY_INSTANCE_FIRST: /* instance 0 of the packed vertex range */
        case MAXWELL3D_VERTEX_ARRAY_INSTANCE_SUBSEQUENT: /* the next instance */
            if (m == MAXWELL3D_VERTEX_ARRAY_INSTANCE_SUBSEQUENT)
                ch->instance_count++;
            else
                ch->instance_count = 0;
            queue_draw(ch, DRAW_ARRAY_INSTANCED, R3D_DRAW_VERTICES, packed_draw_topology(v), packed_draw_first(v),
                       packed_draw_count(v), 0, ch->instance_count);
            break;
        case MAXWELL3D_INVALIDATE_SAMPLER_CACHE_NO_WFI:
        case MAXWELL3D_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI: barrier(BARRIER_DESCRIPTOR_CACHE); break;
        case MAXWELL3D_ZPASS_PIXEL_COUNT_ENABLE: /* the samples-passed counter counts while it is enabled */
            if (g_renderer->samples_enable) g_renderer->samples_enable(ch, zpass_pixel_count_enable(v));
            break;
        case MAXWELL3D_CLEAR_REPORT_VALUE: counter_reset(ch, v); break;
        case MAXWELL3D_RENDER_ENABLE_MODE:
        case MAXWELL3D_RENDER_ENABLE_OVERRIDE: render_enable_update(ch); break;
        case MAXWELL3D_DRAW_INLINE_INDEX: inline_push_packed(ch, v, MAXWELL3D_INLINE_INDEX_BITS_32); break;
        case MAXWELL3D_INLINE_INDEX_2X16_EVEN: inline_push_packed(ch, v, MAXWELL3D_INLINE_INDEX_BITS_2X16); break;
        /* a transform feedback buffer's start_offset (SET_STREAM_OUT_BUFFER_LOAD_WRITE_POINTER): the buffer's capture
           starts over at its start offset, where draws after an earlier one append to what it captured */
        case XFB_START_OFFSET(0):
        case XFB_START_OFFSET(1):
        case XFB_START_OFFSET(2):
        case XFB_START_OFFSET(3):
            ch->xfb_reload |= (u8)(1u << ((m - XFB_START_OFFSET(0)) / MAXWELL3D_TRANSFORM_FEEDBACK_BUFFER_STRIDE));
            break;
        case MAXWELL3D_DRAW_END: draw_end(ch); break;
        case MAXWELL3D_DRAW_BEGIN: draw_begin(ch, v); break;
        /* INDEX_BUFFER32/16/8_FIRST and _SUBSEQUENT (Eden's DrawIndexSmall): the index buffer's first and count in
           one word */
        case MAXWELL3D_INDEX_BUFFER32_FIRST:
        case MAXWELL3D_INDEX_BUFFER16_FIRST:
        case MAXWELL3D_INDEX_BUFFER8_FIRST:
        case MAXWELL3D_INDEX_BUFFER32_SUBSEQUENT:
        case MAXWELL3D_INDEX_BUFFER16_SUBSEQUENT:
        case MAXWELL3D_INDEX_BUFFER8_SUBSEQUENT:
            if (m >= MAXWELL3D_INDEX_BUFFER32_SUBSEQUENT)
                ch->instance_count++;
            else
                ch->instance_count = 0;
            queue_draw(ch, DRAW_IB32 + (m - MAXWELL3D_INDEX_BUFFER32_FIRST) % 3, R3D_DRAW_INDEX_BUFFER,
                       packed_draw_topology(v), packed_draw_first(v), packed_draw_count(v),
                       r[MAXWELL3D_GLOBAL_BASE_VERTEX_INDEX], ch->instance_count);
            break;
        case MAXWELL3D_CLEAR_SURFACE: r3d_clear(ch, v); break;
        case MAXWELL3D_REPORT_SEMAPHORE_QUERY: query_get(ch, r, MAXWELL3D_REPORT_SEMAPHORE, v); break;
        /* firmware call 4 changes registers that don't matter here, and signals its completion in shadow scratch 0
           (Eden's ProcessFirmwareCall4) */
        case MAXWELL3D_FIRMWARE_CALL_4: r[MAXWELL3D_SHADOW_SCRATCH] = 1; break;
        case BIND_GROUP_CONFIG(0):
        case BIND_GROUP_CONFIG(1):
        case BIND_GROUP_CONFIG(2):
        case BIND_GROUP_CONFIG(3):
        case BIND_GROUP_CONFIG(4):
            r3d_cb_bind(ch, (int)((m - BIND_GROUP_CONFIG(0)) / MAXWELL3D_BIND_GROUP_STRIDE), v);
            break;
        default: break;
    }
}
