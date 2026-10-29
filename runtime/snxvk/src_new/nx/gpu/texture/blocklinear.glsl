/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The body of blocklinear.comp: one 16 byte run moved between a block linear surface and linear rows, either way.
   GLSL that is also C++ (tests/glsl/glsl.hpp): tests/glsl/blocklinear_glsl.cpp runs it against gpu.c's bl_offset.

   The layout (gpu.c's bl_x and bl_yz): a GOB is 64 bytes by 8 rows, 512 bytes, 32 runs of 16. Run k of a GOB holds
   row (k & 1) | ((k >> 1) & 6) and the row's run ((k >> 1) & 1) | ((k >> 3) & 2):
     bit 0: row & 1        (y & 1) << 4 bytes
     bit 1: run & 1        (x & 16) << 1 bytes
     bits 2, 3: row >> 1   ((y & 7) >> 1) << 6 bytes
     bit 4: run >> 1       (x & 32) << 3 bytes
   Lane x of a workgroup is that k, so a GOB's 32 lanes read or write its 512 bytes in order, and each row's four
   runs (64 bytes) on the linear side are four neighboring lanes' worth. The workgroup's y lanes are two GOBs side by
   side: each linear row gets 128 bytes in a row from a workgroup.

   Everything but k is the same for the GOB's 32 lanes: its block's place (a column of blocks, a row of blocks, a slab
   of 2^bd slices) and its place in the block. The host hands in the shifts and strides (vk/blocklinear.c), and
   the direction picks which side is read with two selects of offsets computed either way, so both directions are
   the same instructions: one shader and one pipeline, not two.

   A swizzle's final partial run merges only valid row bytes with the destination.
   The host computes its four word masks once; full runs retain one load/store.
   Each run has one invocation, so the merge needs no atomics. The C++ byte-wise
   reference checks every tail width and randomized layouts, both directions. */

void main() {
    uint k = gl_LocalInvocationID.x;
    uint run = ((gl_WorkGroupID.x * 2u + gl_LocalInvocationID.y) << 2) | ((k >> 1) & 1u) | ((k >> 3) & 2u);
    uint gy = gob_y0 + gl_WorkGroupID.y;
    /* the row relative to the first one moved: wraps past rows for the rows of the first GOB above y0, so one
       unsigned compare drops those and the ones past the last */
    uint y = ((gy << 3) | ((k >> 1) & 6u) | (k & 1u)) - y0;
    if (run >= runs || y >= rows) return;
    uint z = z0 + gl_WorkGroupID.z;
    uint gob = ((z & dmask) << bh) | (gy & hmask);
    uint bl = bl_bias + (z >> bd) * slab + (gy >> bh) * block_row + ((run >> 2) << bsh) + (gob << 5) + k;
    uint lin = lin_bias + (gl_WorkGroupID.z * rows + y) * lin_runs + run;
    if (to_bl != 0u && run + 1u == runs && tail_mask[3] != 0xffffffffu) {
        uvec4 prior = dst[bl];
        dst[bl] = prior ^ ((prior ^ src[lin]) & tail_mask);
    } else
        dst[to_bl != 0u ? bl : lin] = src[to_bl != 0u ? lin : bl];
}
