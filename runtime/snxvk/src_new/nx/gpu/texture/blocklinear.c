/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/textures/decoders.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* Landing a block linear surface the GPU swizzled (vk/surface.c's write-back) in guest memory, its padding kept */
#include <string.h>
#include "nx/gpu/texture/blocklinear.h"

/* GOBs are walked in memory order, so the whole ones next to each other in memory (a block's, and the blocks of a
   row of blocks when the row's width is a multiple of 64 bytes) are one memcpy. GOB g of the layout is GOB in of
   block g >> (bh + bd); blocks go gobs_x to a row of blocks, block_rows rows to a slab of 2^bd slices, and inside a
   block GOB in is row in & (2^bh - 1) of slice in >> bh */
void bl_land(u8* dst, const u8* src, u32 row_bytes, u32 h, u32 d, u32 bh, u32 bd) {
    u32 gobs_x = (u32)bl_gobs_x(row_bytes), block_gobs_log2 = bh + bd;
    u64 block_rows = bl_block_rows(h, bh);
    u64 slabs = ((u64)d + (1u << bd) - 1) >> bd;
    u64 gobs = (slabs * block_rows * gobs_x) << block_gobs_log2, whole = 0, g;
    for (g = 0; g < gobs; g++) {
        u64 block = g >> block_gobs_log2, brow = block / gobs_x;
        u32 in = (u32)g & ((1u << block_gobs_log2) - 1), gx = (u32)(block % gobs_x);
        u64 gy = ((brow % block_rows) << bh) + (in & ((1u << bh) - 1)); /* the GOB's row of GOBs in its slice */
        u64 z = ((brow / block_rows) << bd) + (in >> bh);                /* its slice */
        if ((u64)gx * GOB_SIZE_X + GOB_SIZE_X <= row_bytes && gy * GOB_SIZE_Y + GOB_SIZE_Y <= h && z < d)
            continue; /* whole: copied with its run */
        if (whole < g) memcpy(dst + whole * GOB_SIZE, src + whole * GOB_SIZE, (size_t)(g - whole) * GOB_SIZE);
        whole = g + 1;
        if (gy * GOB_SIZE_Y >= h || z >= d) continue; /* padding */
        /* a GOB on the surface's edge: the rows and columns inside it, 16 byte runs or less */
        u32 rows = h - (u32)gy * GOB_SIZE_Y < GOB_SIZE_Y ? h - (u32)gy * GOB_SIZE_Y : GOB_SIZE_Y, x, y;
        u32 cols = row_bytes - gx * GOB_SIZE_X < GOB_SIZE_X ? row_bytes - gx * GOB_SIZE_X : GOB_SIZE_X;
        for (y = 0; y < rows; y++)
            for (x = 0; x < cols; x += GOB_RUN) {
                u64 at = g * GOB_SIZE + gob_y_offset(y) + gob_x_offset(x);
                memcpy(dst + at, src + at, cols - x < GOB_RUN ? cols - x : GOB_RUN);
            }
    }
    if (whole < gobs) memcpy(dst + whole * GOB_SIZE, src + whole * GOB_SIZE, (size_t)(gobs - whole) * GOB_SIZE);
}
