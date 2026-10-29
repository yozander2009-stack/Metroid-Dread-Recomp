/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/maxwell_3d.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The 3D engine's (MAXWELL_B, class 0xB197) registers that maxwell/engines.c acts on or groups, and the fields of
   the words written to them. Each name is Eden's Maxwell3D::Regs field, upper case. The values are method indices
   (the register's byte offset / 4, Eden's MAXWELL3D_REG_INDEX), which index GpuChannel.r3d; Eden's
   ASSERT_REG_POSITION lists the byte offsets. Registers inside a struct of Eden's are its base plus the member's
   word */
#ifndef SNX_MAXWELL_3D_REGS_H
#define SNX_MAXWELL_3D_REGS_H
#include "common/cpu/aarch64/cpu.h"

enum {
    MAXWELL3D_WAIT_FOR_IDLE = 0x044,
    /* load_mme: the macro code's upload index and a code word, the start table's upload index and a start */
    MAXWELL3D_LOAD_MME_INSTRUCTION_PTR = 0x045,
    MAXWELL3D_LOAD_MME_INSTRUCTION = 0x046,
    MAXWELL3D_LOAD_MME_START_ADDRESS_PTR = 0x047,
    MAXWELL3D_LOAD_MME_START_ADDRESS = 0x048,
    MAXWELL3D_SHADOW_RAM_CONTROL = 0x049,
    MAXWELL3D_UPLOAD = 0x060,      /* the inline-to-memory engine's registers (Eden's Upload::Registers) */
    MAXWELL3D_LAUNCH_DMA = 0x06C,  /* the inline-to-memory launch */
    MAXWELL3D_INLINE_DATA = 0x06D, /* and its data */
    /* the methods below this are the class's own, outside the MME shadow RAM (r3d_shadow) */
    MAXWELL3D_SHADOW_FIRST = 0x080,
    MAXWELL3D_MANDATED_EARLY_Z = 0x084,
    MAXWELL3D_INVALIDATE_SHADER_CACHE = 0x087,
    MAXWELL3D_SYNC_INFO = 0x0B2,
    MAXWELL3D_TESSELLATION = 0x0C8,
    MAXWELL3D_RASTERIZE_ENABLE = 0x0DF,
    /* transform_feedback.buffers[4]: 8 words each, start_offset the fifth (open-gpu-doc's
       SET_STREAM_OUT_BUFFER_LOAD_WRITE_POINTER) */
    MAXWELL3D_TRANSFORM_FEEDBACK_BUFFERS = 0x0E0,
    MAXWELL3D_TRANSFORM_FEEDBACK_CONTROLS = 0x1C0, /* 4 controls of 4 words */
    MAXWELL3D_TRANSFORM_FEEDBACK_ENABLED = 0x1D1,
    MAXWELL3D_RT = 0x200, /* the 8 color targets, 16 words each, format the fifth */
    MAXWELL3D_VIEWPORT_TRANSFORM = 0x280, /* 16 ViewportTransforms of 8 words, the swizzle the seventh */
    MAXWELL3D_VIEWPORTS = 0x300,          /* 16 Viewports of 4 words, depth_range_far the fourth */
    /* vertex_buffer: the non-indexed draw's first vertex and count */
    MAXWELL3D_VERTEX_BUFFER_FIRST = 0x35D,
    MAXWELL3D_VERTEX_BUFFER_COUNT = 0x35E,
    MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI = 0x369,
    MAXWELL3D_POLYGON_MODE_FRONT = 0x36B,
    MAXWELL3D_POLYGON_MODE_BACK = 0x36C,
    MAXWELL3D_PATCH_VERTICES = 0x373,
    MAXWELL3D_FRAGMENT_BARRIER = 0x378,
    MAXWELL3D_STENCIL_BACK_MASK = 0x3D6,
    MAXWELL3D_STENCIL_BACK_FUNC_MASK = 0x3D7,
    MAXWELL3D_INVALIDATE_TEXTURE_DATA_CACHE = 0x3DD,
    MAXWELL3D_TILED_CACHE_BARRIER = 0x3DF,
    MAXWELL3D_COLOR_MASK_COMMON = 0x3E4,
    MAXWELL3D_COLOR_TARGET_MRT_ENABLE = 0x3EB,
    /* draw_texture (12 words, Eden's Regs::DrawTexture): its last, src_y0, starts it */
    MAXWELL3D_DRAW_TEXTURE = 0x420,
    MAXWELL3D_DRAW_TEXTURE_SRC_Y0 = 0x42B,
    MAXWELL3D_VERTEX_ATTRIB_FORMAT = 0x458, /* 32 words */
    MAXWELL3D_VERTEX_ARRAY_INSTANCE_FIRST = 0x485,
    MAXWELL3D_VERTEX_ARRAY_INSTANCE_SUBSEQUENT = 0x486,
    MAXWELL3D_RT_CONTROL = 0x487,
    MAXWELL3D_POST_VTG_SHADER_ATTRIB_SKIP_MASK = 0x490, /* 8 words */
    MAXWELL3D_BLEND_PER_TARGET_ENABLED = 0x4B9,
    MAXWELL3D_INLINE_INDEX_4X8 = 0x4C0, /* count and start, then the word of four 8-bit indices */
    MAXWELL3D_INLINE_INDEX_4X8_INDEX0 = 0x4C1,
    MAXWELL3D_DEPTH_TEST_FUNC = 0x4C3,
    MAXWELL3D_BLEND = 0x4CF, /* the common blend state, 17 words; the renderer reads the 16 after the first */
    MAXWELL3D_STENCIL_FRONT_OP = 0x4E1, /* a StencilOp: fail, zfail, zpass, func */
    MAXWELL3D_STENCIL_FRONT_FUNC_MASK = 0x4E6,
    MAXWELL3D_STENCIL_FRONT_MASK = 0x4E7,
    MAXWELL3D_WINDOW_ORIGIN = 0x4EB,
    MAXWELL3D_LINE_WIDTH_SMOOTH = 0x4EC,
    MAXWELL3D_LINE_WIDTH_ALIASED = 0x4ED,
    MAXWELL3D_INVALIDATE_SAMPLER_CACHE_NO_WFI = 0x509,
    MAXWELL3D_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI = 0x50A,
    MAXWELL3D_GLOBAL_BASE_VERTEX_INDEX = 0x50D,
    MAXWELL3D_GLOBAL_BASE_INSTANCE_INDEX = 0x50E,
    MAXWELL3D_USER_CLIP_ENABLE = 0x544,
    MAXWELL3D_ZPASS_PIXEL_COUNT_ENABLE = 0x545,
    MAXWELL3D_POINT_SIZE = 0x546,
    MAXWELL3D_CLEAR_REPORT_VALUE = 0x54C,
    /* render_enable: the address of the reports it compares, and its mode */
    MAXWELL3D_RENDER_ENABLE_ADDRESS_HIGH = 0x554,
    MAXWELL3D_RENDER_ENABLE_ADDRESS_LOW = 0x555,
    MAXWELL3D_RENDER_ENABLE_MODE = 0x556,
    MAXWELL3D_STENCIL_TWO_SIDE_ENABLE = 0x565,
    MAXWELL3D_STENCIL_BACK_OP = 0x566, /* a StencilOp */
    MAXWELL3D_FRAMEBUFFER_SRGB = 0x56E,
    MAXWELL3D_DRAW_INLINE_INDEX = 0x57A,  /* one 32-bit inline index */
    MAXWELL3D_INLINE_INDEX_2X16 = 0x57B,  /* count and start, then the word of two 16-bit indices */
    MAXWELL3D_INLINE_INDEX_2X16_EVEN = 0x57C,
    MAXWELL3D_PROGRAM_REGION = 0x582, /* the programs' base address, high word then low */
    MAXWELL3D_DRAW_END = 0x585,
    MAXWELL3D_DRAW_BEGIN = 0x586,
    MAXWELL3D_PROVOKING_VERTEX = 0x5A1,
    /* index_buffer (7 words from 0x5F2): the indexed draw's first index and count */
    MAXWELL3D_INDEX_BUFFER_FIRST = 0x5F7,
    MAXWELL3D_INDEX_BUFFER_COUNT = 0x5F8,
    MAXWELL3D_INDEX_BUFFER32_FIRST = 0x5F9,
    MAXWELL3D_INDEX_BUFFER16_FIRST = 0x5FA,
    MAXWELL3D_INDEX_BUFFER8_FIRST = 0x5FB,
    MAXWELL3D_INDEX_BUFFER32_SUBSEQUENT = 0x5FC,
    MAXWELL3D_INDEX_BUFFER16_SUBSEQUENT = 0x5FD,
    MAXWELL3D_INDEX_BUFFER8_SUBSEQUENT = 0x5FE,
    MAXWELL3D_VERTEX_STREAM_INSTANCES = 0x620, /* 32 words */
    MAXWELL3D_GL_FRONT_FACE = 0x647,
    MAXWELL3D_GL_CULL_FACE = 0x648,
    MAXWELL3D_VIEWPORT_CLIP_CONTROL = 0x64F,
    MAXWELL3D_RENDER_ENABLE_OVERRIDE = 0x651,
    MAXWELL3D_LOGIC_OP = 0x671, /* 2 words */
    MAXWELL3D_CLEAR_SURFACE = 0x674,
    MAXWELL3D_COLOR_MASK = 0x680, /* 8 words */
    /* report_semaphore: address, payload, and the query word that starts the operation */
    MAXWELL3D_REPORT_SEMAPHORE = 0x6C0,
    MAXWELL3D_REPORT_SEMAPHORE_QUERY = 0x6C3,
    MAXWELL3D_VERTEX_STREAMS = 0x700, /* 32 streams of 4 words: stride and enable, address high and low, frequency */
    MAXWELL3D_BLEND_PER_TARGET = 0x780, /* 64 words */
    MAXWELL3D_PIPELINES = 0x800,        /* 6 programs of 16 words */
    MAXWELL3D_FALCON = 0x8C0,           /* falcon[32]: falcon[4] runs firmware call 4 */
    MAXWELL3D_FIRMWARE_CALL_4 = 0x8C4,
    /* const_buffer: size, address high and low, offset, then 16 data words */
    MAXWELL3D_CONST_BUFFER = 0x8E0,
    MAXWELL3D_CONST_BUFFER_BUFFER = 0x8E4,
    MAXWELL3D_CONST_BUFFER_BUFFER_LAST = 0x8F3,
    MAXWELL3D_BIND_GROUPS = 0x900, /* 5 stages of 8 words, raw_config the fifth */
    MAXWELL3D_STREAM_OUT_LAYOUT = 0xA00, /* 128 words */
    MAXWELL3D_SHADOW_SCRATCH = 0xD00,
    /* the macro calls: method 0xE00 + 2 * i starts macro i, the odd one after it takes more parameters (Eden's
       MacroRegistersStart) */
    MAXWELL3D_MACRO_REGISTERS_START = 0xE00
};

/* the stride of the register arrays above, in words */
#define MAXWELL3D_TRANSFORM_FEEDBACK_BUFFER_STRIDE 8u
#define MAXWELL3D_TRANSFORM_FEEDBACK_BUFFER_START_OFFSET 4u
#define MAXWELL3D_RT_STRIDE 16u
#define MAXWELL3D_RT_FORMAT 4u
#define MAXWELL3D_VERTEX_STREAM_STRIDE 4u
#define MAXWELL3D_VERTEX_STREAM_FREQUENCY 3u
#define MAXWELL3D_BIND_GROUP_STRIDE 8u
#define MAXWELL3D_BIND_GROUP_RAW_CONFIG 4u
#define MAXWELL3D_TRANSFORM_FEEDBACK_BUFFERS_N 4u
#define MAXWELL3D_VIEWPORT_TRANSFORM_STRIDE 8u
#define MAXWELL3D_VIEWPORT_TRANSFORM_SWIZZLE 6u
#define MAXWELL3D_VIEWPORT_STRIDE 4u
#define MAXWELL3D_VIEWPORT_DEPTH_RANGE_FAR 3u
#define MAXWELL3D_NUM_VIEWPORTS 16u
#define MAXWELL3D_NUM_RENDER_TARGETS 8u
#define MAXWELL3D_NUM_VERTEX_ATTRIBUTES 32u
#define MAXWELL3D_BLEND_PER_TARGET_STRIDE 8u
#define MAXWELL3D_SHADER_STAGES 5u /* the bind groups: vertex, tessellation control and evaluation, geometry,
                                      fragment */

/* SET_MME_SHADOW_RAM_CONTROL (Eden's Regs::ShadowRamControl), its low 2 bits */
enum {
    MAXWELL3D_SHADOW_RAM_TRACK = 0,             /* the value is written to the shadow RAM */
    MAXWELL3D_SHADOW_RAM_TRACK_WITH_FILTER = 1, /* the same */
    MAXWELL3D_SHADOW_RAM_PASSTHROUGH = 2,       /* the register only */
    MAXWELL3D_SHADOW_RAM_REPLAY = 3             /* the shadow RAM's value replaces the one written */
};
#define MAXWELL3D_SHADOW_RAM_CONTROL_MASK 3u

/* SyncInfo (Eden's Regs::SyncInfo): sync_point bits 0-15, condition bit 20 (RopWritesDone when set) */
static inline u32 sync_info_sync_point(u32 v) { return v & 0xFFFFu; }
static inline int sync_info_rop_writes_done(u32 v) { return (v >> 20) & 1u; }

/* InvalidateShaderCache(NoWFI)'s instruction bit (bit 0) */
static inline int invalidate_shader_cache_instruction(u32 v) { return v & 1u; }

/* Draw.begin (Eden's Regs::Draw): topology bits 0-15, instance_id bits 26-27 */
enum {
    MAXWELL3D_DRAW_INSTANCE_FIRST = 0,      /* the instance count starts over */
    MAXWELL3D_DRAW_INSTANCE_SUBSEQUENT = 1, /* the next instance */
    MAXWELL3D_DRAW_INSTANCE_UNCHANGED = 2   /* the same instance */
};
static inline u32 draw_begin_topology(u32 v) { return v & 0xFFFFu; }
static inline u32 draw_begin_instance_id(u32 v) { return (v >> 26) & 3u; }

/* VertexArray and IndexBufferSmall (Eden's Regs), the packed draws of VERTEX_ARRAY_INSTANCE_* and INDEX_BUFFER*_*:
   start or first bits 0-15, count bits 16-27, topology bits 28-31 (Eden gives VertexArray's topology 3 bits, a
   4th set bit is passed on as it is) */
static inline u32 packed_draw_first(u32 v) { return v & 0xFFFFu; }
static inline u32 packed_draw_count(u32 v) { return (v >> 16) & 0xFFFu; }
static inline u32 packed_draw_topology(u32 v) { return v >> 28; }

/* the inline index words: InlineIndex4x8 (index0-3, 8 bits each), InlineIndex2x16 (even then odd, 16 bits each),
   draw_inline_index (one of 32 bits), the low ones first */
#define MAXWELL3D_INLINE_INDEX_BITS_4X8 8u
#define MAXWELL3D_INLINE_INDEX_BITS_2X16 16u
#define MAXWELL3D_INLINE_INDEX_BITS_32 32u

/* RenderEnable::Override (render_enable_override's low 2 bits) and RenderEnable::Mode (the mode's low 3 bits) */
enum {
    MAXWELL3D_RENDER_ENABLE_OVERRIDE_USE_RENDER_ENABLE = 0,
    MAXWELL3D_RENDER_ENABLE_OVERRIDE_ALWAYS_RENDER = 1,
    MAXWELL3D_RENDER_ENABLE_OVERRIDE_NEVER_RENDER = 2
};
#define MAXWELL3D_RENDER_ENABLE_OVERRIDE_MASK 3u
enum {
    MAXWELL3D_RENDER_ENABLE_FALSE = 0,
    MAXWELL3D_RENDER_ENABLE_TRUE = 1,
    MAXWELL3D_RENDER_ENABLE_CONDITIONAL = 2, /* both words of the report's value nonzero */
    MAXWELL3D_RENDER_ENABLE_IF_EQUAL = 3,    /* two reports' values (16 bytes apart) equal */
    MAXWELL3D_RENDER_ENABLE_IF_NOT_EQUAL = 4
};
#define MAXWELL3D_RENDER_ENABLE_MODE_MASK 7u
/* a report as the render enable reads it: the 64-bit value then the 64-bit timestamp */
#define MAXWELL3D_REPORT_BYTES 16u

/* ZPassPixelCountEnable's enable bit (bit 0) */
static inline int zpass_pixel_count_enable(u32 v) { return v & 1u; }

/* ReportSemaphore.query (Eden's Regs::ReportSemaphore): operation bits 0-1, release bit 4 (after preceding reads
   or writes), reduction_enable bit 3, reduction_op bits 9-11, comparison bit 16, format_signed bits 17-18, report
   bits 23-27, short_query bit 28 (one 32-bit word instead of the value and the timestamp) */
enum {
    REPORT_SEMAPHORE_OPERATION_RELEASE = 0,
    REPORT_SEMAPHORE_OPERATION_ACQUIRE = 1,
    REPORT_SEMAPHORE_OPERATION_REPORT_ONLY = 2,
    REPORT_SEMAPHORE_OPERATION_TRAP = 3
};
/* Comparison: Eden names 0 NotEqual and 1 GreaterOrEqual; the acquire waits for the payload itself with 0
   (gpu_park's equal) and reaches it, wrapping, with 1 */
enum { REPORT_SEMAPHORE_COMPARISON_EQUAL = 0, REPORT_SEMAPHORE_COMPARISON_GREATER_OR_EQUAL = 1 };
static inline u32 report_semaphore_operation(u32 v) { return v & 3u; }
static inline int report_semaphore_reduction_enable(u32 v) { return (v >> 3) & 1u; }
static inline u32 report_semaphore_reduction_op(u32 v) { return (v >> 9) & 7u; }
static inline u32 report_semaphore_comparison(u32 v) { return (v >> 16) & 1u; }
static inline u32 report_semaphore_report(u32 v) { return (v >> 23) & 31u; }
static inline int report_semaphore_short_query(u32 v) { return (v >> 28) & 1u; }
/* ReportSemaphore::Report: what a REPORT_ONLY writes */
enum {
    REPORT_PAYLOAD = 0, /* "None" in NVIDIA's documents; the hardware writes the payload (Eden) */
    REPORT_VERTICES_GENERATED = 1,
    REPORT_ZPASS_PIXEL_COUNT = 2,
    REPORT_PRIMITIVES_GENERATED = 3,
    REPORT_ALPHA_BETA_CLOCKS = 4,
    REPORT_VERTEX_SHADER_INVOCATIONS = 5,
    REPORT_STREAMING_PRIMITIVES_NEEDED_MINUS_SUCCEEDED = 6,
    REPORT_GEOMETRY_SHADER_INVOCATIONS = 7,
    REPORT_GEOMETRY_SHADER_PRIMITIVES_GENERATED = 9,
    REPORT_ZCULL_STATS0 = 10,
    REPORT_STREAMING_PRIMITIVES_SUCCEEDED = 11,
    REPORT_ZCULL_STATS1 = 12,
    REPORT_STREAMING_PRIMITIVES_NEEDED = 13,
    REPORT_ZCULL_STATS2 = 14,
    REPORT_CLIPPER_INVOCATIONS = 15,
    REPORT_ZCULL_STATS3 = 16,
    REPORT_CLIPPER_PRIMITIVES_GENERATED = 17,
    REPORT_VTG_PRIMITIVES_OUT = 18,
    REPORT_PIXEL_SHADER_INVOCATIONS = 19,
    REPORT_ZPASS_PIXEL_COUNT64 = 21,
    REPORT_IEEE_CLEAN_COLOR_TARGET = 24,
    REPORT_IEEE_CLEAN_ZETA_TARGET = 25,
    REPORT_STREAMING_BYTE_COUNT = 26,
    REPORT_TESSELLATION_INIT_INVOCATIONS = 27,
    REPORT_BOUNDING_RECTANGLE = 28,
    REPORT_TESSELLATION_SHADER_INVOCATIONS = 29,
    REPORT_TOTAL_STREAMING_PRIMITIVES_NEEDED_MINUS_SUCCEEDED = 30,
    REPORT_TESSELLATION_SHADER_PRIMITIVES_GENERATED = 31,
    REPORT_COUNT = 32
};
/* CLEAR_REPORT_VALUE (Eden's Regs::ClearReport): the counters it resets */
enum {
    CLEAR_REPORT_ZPASS_PIXEL_COUNT = 0x01,
    CLEAR_REPORT_ZCULL_STATS = 0x02,
    CLEAR_REPORT_STREAMING_PRIMITIVES_NEEDED_MINUS_SUCCEEDED = 0x03,
    CLEAR_REPORT_ALPHA_BETA_CLOCKS = 0x04,
    CLEAR_REPORT_STREAMING_PRIMITIVES_SUCCEEDED = 0x10,
    CLEAR_REPORT_STREAMING_PRIMITIVES_NEEDED = 0x11,
    CLEAR_REPORT_VERTICES_GENERATED = 0x12,
    CLEAR_REPORT_PRIMITIVES_GENERATED = 0x13,
    CLEAR_REPORT_VERTEX_SHADER_INVOCATIONS = 0x15,
    CLEAR_REPORT_TESSELLATION_INIT_INVOCATIONS = 0x16,
    CLEAR_REPORT_TESSELLATION_SHADER_INVOCATIONS = 0x17,
    CLEAR_REPORT_TESSELLATION_SHADER_PRIMITIVES_GENERATED = 0x18,
    CLEAR_REPORT_GEOMETRY_SHADER_INVOCATIONS = 0x1A,
    CLEAR_REPORT_GEOMETRY_SHADER_PRIMITIVES_GENERATED = 0x1B,
    CLEAR_REPORT_CLIPPER_INVOCATIONS = 0x1C,
    CLEAR_REPORT_CLIPPER_PRIMITIVES_GENERATED = 0x1D,
    CLEAR_REPORT_PIXEL_SHADER_INVOCATIONS = 0x1E,
    CLEAR_REPORT_VTG_PRIMITIVES_OUT = 0x1F
};

/* the words of the state a channel starts from (reset_3d in gpu.c) */

/* Blend (Eden's Regs::Blend), words from MAXWELL3D_BLEND, and BlendPerTarget, words from MAXWELL3D_BLEND_PER_TARGET
   + i * MAXWELL3D_BLEND_PER_TARGET_STRIDE. The common one has enable_global_color_key before alpha_dest */
enum {
    MAXWELL3D_BLEND_COLOR_OP = 1,
    MAXWELL3D_BLEND_COLOR_SOURCE = 2,
    MAXWELL3D_BLEND_COLOR_DEST = 3,
    MAXWELL3D_BLEND_ALPHA_OP = 4,
    MAXWELL3D_BLEND_ALPHA_SOURCE = 5,
    MAXWELL3D_BLEND_ALPHA_DEST = 7,
    MAXWELL3D_BLEND_PER_TARGET_ALPHA_DEST = 6
};
/* Blend::Equation and Blend::Factor, the D3D values the reset uses */
enum { MAXWELL3D_BLEND_EQUATION_ADD_D3D = 1 };
enum { MAXWELL3D_BLEND_FACTOR_ZERO_D3D = 1, MAXWELL3D_BLEND_FACTOR_ONE_D3D = 2 };

/* StencilOp (Eden's Regs::StencilOp), words from MAXWELL3D_STENCIL_FRONT_OP or _BACK_OP, and its Op's D3D Keep */
enum { MAXWELL3D_STENCIL_OP_FAIL = 0, MAXWELL3D_STENCIL_OP_ZFAIL = 1, MAXWELL3D_STENCIL_OP_ZPASS = 2,
       MAXWELL3D_STENCIL_OP_FUNC = 3 };
enum { MAXWELL3D_STENCIL_OP_KEEP_D3D = 1 };

/* ComparisonOp's GL Always, FrontFace, CullFace and PolygonMode (GL's enums, as Eden's) */
enum { MAXWELL3D_COMPARISON_ALWAYS_GL = 0x207 };
enum { MAXWELL3D_FRONT_FACE_CLOCKWISE = 0x900, MAXWELL3D_FRONT_FACE_COUNTER_CLOCKWISE = 0x901 };
enum { MAXWELL3D_CULL_FACE_BACK = 0x405 };
enum { MAXWELL3D_POLYGON_MODE_FILL = 0x1B02 };

/* ViewportTransform's swizzle: x, y, z, w in 4-bit fields from bit 0, each a ViewportSwizzle (PositiveX 0,
   PositiveY 2, PositiveZ 4, PositiveW 6) */
enum {
    MAXWELL3D_VIEWPORT_SWIZZLE_POSITIVE_X = 0,
    MAXWELL3D_VIEWPORT_SWIZZLE_POSITIVE_Y = 2,
    MAXWELL3D_VIEWPORT_SWIZZLE_POSITIVE_Z = 4,
    MAXWELL3D_VIEWPORT_SWIZZLE_POSITIVE_W = 6
};
#define MAXWELL3D_VIEWPORT_SWIZZLE_BITS 4u
#define MAXWELL3D_VIEWPORT_SWIZZLE_IDENTITY                                                                          \
    (MAXWELL3D_VIEWPORT_SWIZZLE_POSITIVE_X | MAXWELL3D_VIEWPORT_SWIZZLE_POSITIVE_Y << MAXWELL3D_VIEWPORT_SWIZZLE_BITS | \
     MAXWELL3D_VIEWPORT_SWIZZLE_POSITIVE_Z << 2 * MAXWELL3D_VIEWPORT_SWIZZLE_BITS |                                    \
     MAXWELL3D_VIEWPORT_SWIZZLE_POSITIVE_W << 3 * MAXWELL3D_VIEWPORT_SWIZZLE_BITS)

/* VertexAttribute's constant bit (bit 6): the attribute reads a constant, not a buffer */
#define MAXWELL3D_VERTEX_ATTRIB_CONSTANT (1u << 6)
/* ColorMask (Eden's Regs::ColorMask): R, G, B, A in 4-bit fields from bit 0. The reset value sets each of the
   word's eight fields to 1, so R, G, B and A are written */
#define MAXWELL3D_COLOR_MASK_ALL 0x11111111u

#endif
