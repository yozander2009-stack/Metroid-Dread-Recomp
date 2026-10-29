/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/textures/decoders.h, src/video_core/textures/decoders.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The block linear layout of Maxwell surfaces (docs/BLOCKLINEAR.md), and block linear surfaces copied between two
   buffers of the same layout (blocklinear.c). The GPU swizzles a render target written back (blocklinear.comp), and
   bl_land lands its bytes in guest memory. gpu.c's bl_offset gives a byte's offset in the layout with the same
   geometry.

   A GOB (group of bytes) is 64 bytes by 8 rows, 512 bytes. Inside it the offset of byte (x, y) interleaves the bits
   of x and y: x bits 0-3 stay at 0-3 (a 16 byte run), x bit 4 goes to bit 5, x bit 5 to bit 8; y bit 0 goes to bit 4,
   y bits 1-2 to bits 6-7 (Eden's SWIZZLE_X_BITS 0b100101111 and SWIZZLE_Y_BITS 0b011010000). GOBs form blocks
   2^bh GOBs high and 2^bd deep, inside a block GOBs go top to bottom, then slice by slice, blocks go row by row,
   and a slab of 2^bd slices after another */
#ifndef SNX_TEXTURE_BLOCKLINEAR_H
#define SNX_TEXTURE_BLOCKLINEAR_H
#include "common/cpu/aarch64/cpu.h"

/* a GOB's geometry (Eden's GOB_SIZE_X, GOB_SIZE_Y and GOB_SIZE, and their shifts) */
#define GOB_SIZE_X 64u /* bytes in a GOB's row */
#define GOB_SIZE_X_SHIFT 6
#define GOB_SIZE_Y 8u /* rows in a GOB */
#define GOB_SIZE_Y_SHIFT 3
#define GOB_SIZE 512u /* GOB_SIZE_X * GOB_SIZE_Y */
#define GOB_SIZE_SHIFT 9
#define GOB_RUN 16u /* bytes of a row next to each other in a GOB: x bits 0-3 */
/* the largest block the hardware has: 2^5 = 32 GOBs high or deep (Eden's GOBsPerBlock up to THIRTYTWO_GOBS) */
#define GOB_BLOCK_LOG2_MAX 5u

/* where byte x of a GOB's row and row y of it go in the GOB: the two add up to the byte's offset in the GOB */
static inline u32 gob_x_offset(u32 x) { return ((x & 32) << 3) | ((x & 16) << 1) | (x & 15); }
static inline u32 gob_y_offset(u32 y) { return (((y & 7) >> 1) << 6) | ((y & 1) << 4); }
/* GOBs across a row width_bytes wide, and rows of blocks 2^bh GOBs high in a slice height rows high (both rounded
   up: a surface is whole blocks) */
static inline u64 bl_gobs_x(u32 width_bytes) { return ((u64)width_bytes + GOB_SIZE_X - 1) >> GOB_SIZE_X_SHIFT; }
static inline u64 bl_block_rows(u32 height, u32 bh) {
    return ((((u64)height + GOB_SIZE_Y - 1) >> GOB_SIZE_Y_SHIFT) + (1u << bh) - 1) >> bh;
}
/* the bytes of a block 2^bh GOBs high and 2^bd deep */
static inline u64 bl_block_size(u32 bh, u32 bd) { return (u64)GOB_SIZE << (bh + bd); }

/* the bytes of a surface row_bytes wide, h rows high and d slices deep, in blocks of 2^bh GOBs high and 2^bd deep
   (gpu.c's bl_offset), from src to dst, both in that layout. The padding (bytes past a row, rows past h, slices past
   d) is left as dst had it: whole GOBs inside the surface go in runs as long as memory has them, those on its edges
   16 bytes or less at a time */
void bl_land(u8* dst, const u8* src, u32 row_bytes, u32 h, u32 d, u32 bh, u32 bd);

#endif
