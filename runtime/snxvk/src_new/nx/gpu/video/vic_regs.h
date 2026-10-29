/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/host1x/vic.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project */
/* VIC's method registers and the ConfigStruct its jobs read, as vic.c resolves them.

   Methods are word indices into a channel's registers (Eden's VicRegisters byte offset / 4). A register that names
   memory holds an IOVA >> 8 (Eden's Offset::Address). The config's fields are given by byte offset in ConfigStruct
   and its parts, and their BitFields as accessors on the u64 (or u32) that holds them, named as Eden names them */
#ifndef SNX_VIC_REGS_H
#define SNX_VIC_REGS_H
#include "common/cpu/aarch64/cpu.h"

/* VicRegisters (Vic::Method) */
enum {
    VIC_EXECUTE = 0x300 / 4,
    VIC_SURFACES = 0x400 / 4, /* surfaces[8 slots][8 SurfaceIndex] of PlaneOffsets {luma, chroma_u, chroma_v} */
    VIC_SET_CONFIG_STRUCT_OFFSET = 0x708 / 4,
    VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET = 0x720 / 4,
    VIC_SET_OUTPUT_SURFACE_CHROMA_OFFSET = 0x724 / 4
};
#define VIC_SLOTS 8u
#define VIC_PLANE_OFFSETS_WORDS 3u /* PlaneOffsets: luma, chroma_u, chroma_v */
#define VIC_SURFACE_INDICES 8u     /* SurfaceIndex: Current, Previous, Next, ... */
/* slot s's Current surface: its luma word, then chroma_u */
static inline u32 vic_slot_surface(u32 s) { return VIC_SURFACES + s * VIC_SURFACE_INDICES * VIC_PLANE_OFFSETS_WORDS; }
#define VIC_OFFSET_SHIFT 8u /* an Offset register holds the IOVA >> 8 */
static inline u64 vic_offset_address(u32 reg) { return (u64)reg << VIC_OFFSET_SHIFT; }

/* VideoPixelFormat, the formats composed here: Eden's names, and NVIDIA's for the YUV ones. NVIDIA names the bytes of
   a chroma pair from the high one down, so Y8__U8V8_N420 keeps V in the even byte */
enum {
    VIC_PIXEL_A8B8G8R8 = 31,
    VIC_PIXEL_A8R8G8B8 = 32,
    VIC_PIXEL_X8B8G8R8 = 35,
    VIC_PIXEL_Y8__U8V8_N420 = 67, /* NV21 */
    VIC_PIXEL_Y8__V8U8_N420 = 68  /* NV12 */
};
/* BlkKind: pitch linear, every other value block linear (Generic_16Bx2) */
enum { VIC_BLK_KIND_PITCH = 0 };

/* the field of n bits from bit lo of v */
static inline u32 vic_field(u64 v, u32 lo, u32 n) { return (u32)(v >> lo) & ((1u << n) - 1); }

/* ConfigStruct: pipe_config 0x00, output_config 0x10, output_surface_config 0x20, out_color_matrix 0x30,
   clear_rects 0x50, slot_structs 0x90 */
#define VIC_CONFIG_SIZE 0x610u
enum {
    VIC_CONFIG_OUTPUT_CONFIG = 0x10,         /* u64: alpha fill and background */
    VIC_CONFIG_OUTPUT_TARGET_RECT_LR = 0x18, /* u32: target_rect_left, target_rect_right */
    VIC_CONFIG_OUTPUT_TARGET_RECT_TB = 0x1C, /* u32: target_rect_top, target_rect_bottom */
    VIC_CONFIG_OUTPUT_SURFACE_CONFIG = 0x20, /* OutputSurfaceConfig: a SurfaceConfig */
    VIC_CONFIG_SLOT_STRUCTS = 0x90
};
#define VIC_SLOT_STRUCT_SIZE 0xB0u
/* OutputConfig's first u64: the background colour, 10 bits a channel */
static inline u32 vic_background_a(u64 v) { return vic_field(v, 6, 10); }
static inline u32 vic_background_r(u64 v) { return vic_field(v, 16, 10); }
static inline u32 vic_background_g(u64 v) { return vic_field(v, 26, 10); }
static inline u32 vic_background_b(u64 v) { return vic_field(v, 36, 10); }
/* the target rectangle's u32s: left (top) in bits 0-13, right (bottom) in 16-29, inclusive */
static inline u32 vic_rect_lo(u32 v) { return vic_field(v, 0, 14); }
static inline u32 vic_rect_hi(u32 v) { return vic_field(v, 16, 14); }

/* OutputSurfaceConfig and SlotSurfaceConfig, the same layout: a u64 at +0 with the format and the surface size, and
   a u64 at +8 with the padded plane sizes (luma width - 1 and height - 1, then chroma's at bit 32). Sizes are - 1 */
enum { VIC_SURFACE_CONFIG_FORMAT = 0x0, VIC_SURFACE_CONFIG_PLANE_SIZES = 0x8 };
static inline u32 vic_surface_pixel_format(u64 v) { return vic_field(v, 0, 7); }
static inline u32 vic_surface_block_kind(u64 v) { return vic_field(v, 11, 4); }
static inline u32 vic_surface_block_height(u64 v) { return vic_field(v, 15, 4); } /* in GOBs, log2 */
static inline u32 vic_surface_width(u64 v) { return vic_field(v, 32, 14) + 1; }
static inline u32 vic_surface_height(u64 v) { return vic_field(v, 46, 14) + 1; }
static inline u32 vic_luma_width(u64 v) { return vic_field(v, 0, 14) + 1; }
static inline u32 vic_chroma_width(u64 v) { return vic_field(v, 32, 14) + 1; } /* in U,V pairs */

/* SlotStruct: config (SlotConfig) 0x00, surface_config 0x40, luma_key 0x50, color_matrix 0x60, gamut_matrix 0x80,
   blending 0xA0. SlotConfig's u64s by offset */
enum {
    VIC_SLOT_CONFIG = 0x00,        /* slot_enable bit 0 */
    VIC_SLOT_CLAMP_ALPHA = 0x10,   /* soft_clamp_low, soft_clamp_high, planar_alpha */
    VIC_SLOT_SOURCE_RECT_LR = 0x20, /* source_rect_left bits 0-29, right 32-61: 16.16 fixed point, inclusive */
    VIC_SLOT_SOURCE_RECT_TB = 0x28, /* top, bottom the same */
    VIC_SLOT_DEST_RECT = 0x30,      /* dest_rect_left 0-13, right 16-29, top 32-45, bottom 48-61, inclusive */
    VIC_SLOT_SURFACE_CONFIG = 0x40,
    VIC_SLOT_COLOR_MATRIX = 0x60 /* MatrixStruct: four u64 */
};
static inline u32 vic_slot_enable(u64 v) { return vic_field(v, 0, 1); }
static inline u32 vic_soft_clamp_low(u64 v) { return vic_field(v, 0, 10); }
static inline u32 vic_soft_clamp_high(u64 v) { return vic_field(v, 10, 10); }
static inline u32 vic_planar_alpha(u64 v) { return vic_field(v, 32, 10); }
#define VIC_SOURCE_RECT_FRAC_BITS 16u /* the source rectangle is 16.16 fixed point: the pixel is its integer part */
static inline u32 vic_source_rect_lo(u64 v) { return vic_field(v, 0, 30) >> VIC_SOURCE_RECT_FRAC_BITS; }
static inline u32 vic_source_rect_hi(u64 v) { return vic_field(v, 32, 30) >> VIC_SOURCE_RECT_FRAC_BITS; }
static inline u32 vic_dest_rect_left(u64 v) { return vic_field(v, 0, 14); }
static inline u32 vic_dest_rect_right(u64 v) { return vic_field(v, 16, 14); }
static inline u32 vic_dest_rect_top(u64 v) { return vic_field(v, 32, 14); }
static inline u32 vic_dest_rect_bottom(u64 v) { return vic_field(v, 48, 14); }

/* MatrixStruct: u64 k holds column k of the 3x4 matrix, matrix_coeff0k, 1k, 2k (rows R, G, B) as signed 20-bit
   fields at bits 0, 20 and 40. matrix_r_shift is bits 60-63 of the first, matrix_enable bit 63 of the second */
#define VIC_MATRIX_COLUMNS 4u
#define VIC_MATRIX_ROWS 3u
#define VIC_MATRIX_COEFF_BITS 20u
static inline int vic_matrix_coeff(u64 column, u32 row) {
    u32 v = vic_field(column, VIC_MATRIX_COEFF_BITS * row, VIC_MATRIX_COEFF_BITS);
    return (int)v - (int)((v & 0x80000u) << 1); /* sign extended from bit 19 */
}
static inline u32 vic_matrix_r_shift(u64 column0) { return vic_field(column0, 60, 4); }
static inline u32 vic_matrix_enable(u64 column1) { return vic_field(column1, 63, 1); }

#endif
