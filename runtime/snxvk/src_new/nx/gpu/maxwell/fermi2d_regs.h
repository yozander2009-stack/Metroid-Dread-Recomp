/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/fermi_2d.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The 2D engine's (FERMI_TWOD_A, class 0x902D) registers that maxwell/fermi2d.c reads, named after Eden's
   Fermi2D::Regs. The values are method indices (the register's byte offset / 4), which index GpuChannel.r2d; Eden's
   ASSERT_REG_POSITION lists the byte offsets */
#ifndef SNX_FERMI2D_REGS_H
#define SNX_FERMI2D_REGS_H
#include "common/cpu/aarch64/cpu.h"

enum {
    FERMI2D_DST = 0x080, /* the destination Surface (FERMI2D_SURFACE_*) */
    FERMI2D_SRC = 0x08C, /* the source Surface */
    FERMI2D_OPERATION = 0x2AB,
    /* pixels_from_memory: the blit */
    FERMI2D_PIXELS_FROM_MEMORY_SAMPLE_MODE = 0x223,
    FERMI2D_PIXELS_FROM_MEMORY_DST_X0 = 0x22C,
    FERMI2D_PIXELS_FROM_MEMORY_DST_Y0 = 0x22D,
    FERMI2D_PIXELS_FROM_MEMORY_DST_WIDTH = 0x22E,
    FERMI2D_PIXELS_FROM_MEMORY_DST_HEIGHT = 0x22F,
    /* signed 32.32 fixed point, the fraction's word first (FERMI2D_FIXED_ONE) */
    FERMI2D_PIXELS_FROM_MEMORY_DU_DX = 0x230,
    FERMI2D_PIXELS_FROM_MEMORY_DV_DY = 0x232,
    FERMI2D_PIXELS_FROM_MEMORY_SRC_X0 = 0x234,
    FERMI2D_PIXELS_FROM_MEMORY_SRC_Y0 = 0x236,
    /* the integer word of src_y0, the last of the blit's registers: writing it starts the blit (Eden's CallMethod) */
    FERMI2D_PIXELS_FROM_MEMORY_SRC_Y0_INT = 0x237,
    /* the registers GpuChannel.r2d holds, which take in every method the engine uses */
    FERMI2D_NUM_REGS = 0x400
};

/* Surface (Eden's Fermi2D::Surface), words from FERMI2D_DST or FERMI2D_SRC */
enum {
    FERMI2D_SURFACE_FORMAT = 0,     /* RenderTargetFormat */
    FERMI2D_SURFACE_LINEAR = 1,     /* MemoryLayout */
    FERMI2D_SURFACE_BLOCK_SIZE = 2, /* block_width bits 0-3, block_height 4-7, block_depth 8-11 (log2 GOBs) */
    FERMI2D_SURFACE_DEPTH = 3,
    FERMI2D_SURFACE_LAYER = 4,
    FERMI2D_SURFACE_PITCH = 5,
    FERMI2D_SURFACE_WIDTH = 6,
    FERMI2D_SURFACE_HEIGHT = 7,
    FERMI2D_SURFACE_ADDR_UPPER = 8,
    FERMI2D_SURFACE_ADDR_LOWER = 9
};
enum { FERMI2D_MEMORY_LAYOUT_BLOCK_LINEAR = 0, FERMI2D_MEMORY_LAYOUT_PITCH = 1 }; /* MemoryLayout, bit 0 */
static inline u32 fermi2d_block_height(u32 v) { return (v >> 4) & 15u; }
static inline u32 fermi2d_block_depth(u32 v) { return (v >> 8) & 15u; }

/* Operation */
enum {
    FERMI2D_OPERATION_SRC_COPY_AND = 0,
    FERMI2D_OPERATION_ROP_AND = 1,
    FERMI2D_OPERATION_BLEND = 2,
    FERMI2D_OPERATION_SRC_COPY = 3,
    FERMI2D_OPERATION_ROP = 4,
    FERMI2D_OPERATION_SRC_COPY_PREMULT = 5,
    FERMI2D_OPERATION_BLEND_PREMULT = 6
};

/* sample_mode: origin bit 0 (Origin), filter bit 4 (Filter) */
enum { FERMI2D_ORIGIN_CENTER = 0, FERMI2D_ORIGIN_CORNER = 1 };
enum { FERMI2D_FILTER_POINT = 0, FERMI2D_FILTER_BILINEAR = 1 };
static inline u32 fermi2d_sample_mode_origin(u32 v) { return v & 1u; }
static inline u32 fermi2d_sample_mode_filter(u32 v) { return (v >> 4) & 1u; }

/* 1.0 in the blit's signed 32.32 fixed point (Eden's s64 du_dx, dv_dy, src_x0, src_y0; its null_derivative) */
#define FERMI2D_FIXED_ONE 4294967296.0 /* 2^32 */

#endif
