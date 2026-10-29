/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/maxwell_dma.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The copy engine's (MAXWELL_DMA_COPY_A, class 0xB0B5; open-gpu-doc's clb0b5.h) registers and the fields of
   LAUNCH_DMA and the remap, named after Eden's MaxwellDMA::Regs. The values are method indices (the register's byte
   offset / 4), which index GpuChannel.rdma; Eden's ASSERT_REG_POSITION lists the byte offsets */
#ifndef SNX_MAXWELL_DMA_REGS_H
#define SNX_MAXWELL_DMA_REGS_H
#include "common/cpu/aarch64/cpu.h"

enum {
    /* semaphore: the completion semaphore's address (upper, lower) and payload */
    MAXWELL_DMA_SEMAPHORE_ADDRESS_UPPER = 0x090,
    MAXWELL_DMA_SEMAPHORE_ADDRESS_LOWER = 0x091,
    MAXWELL_DMA_SEMAPHORE_PAYLOAD = 0x092,
    MAXWELL_DMA_LAUNCH_DMA = 0x0C0,
    MAXWELL_DMA_OFFSET_IN_UPPER = 0x100,
    MAXWELL_DMA_OFFSET_IN_LOWER = 0x101,
    MAXWELL_DMA_OFFSET_OUT_UPPER = 0x102,
    MAXWELL_DMA_OFFSET_OUT_LOWER = 0x103,
    MAXWELL_DMA_PITCH_IN = 0x104,
    MAXWELL_DMA_PITCH_OUT = 0x105,
    MAXWELL_DMA_LINE_LENGTH_IN = 0x106, /* elements a line */
    MAXWELL_DMA_LINE_COUNT = 0x107,
    /* remap_const: constants A and B and the component word */
    MAXWELL_DMA_REMAP_CONSTA_VALUE = 0x1C0,
    MAXWELL_DMA_REMAP_CONSTB_VALUE = 0x1C1,
    MAXWELL_DMA_REMAP_COMPONENTS = 0x1C2,
    /* dst_params and src_params: a block linear operand's DMA::Parameters (MAXWELL_DMA_PARAMS_*) */
    MAXWELL_DMA_DST_PARAMS = 0x1C3,
    MAXWELL_DMA_SRC_PARAMS = 0x1CA,
    /* the registers GpuChannel.rdma holds, which take in every method the engine uses */
    MAXWELL_DMA_NUM_REGS = 0x200
};

/* DMA::Parameters, words from MAXWELL_DMA_DST_PARAMS or _SRC_PARAMS */
enum {
    MAXWELL_DMA_PARAMS_BLOCK_SIZE = 0, /* BlockSize: width bits 0-3, height 4-7, depth 8-11 (log2 GOBs) */
    MAXWELL_DMA_PARAMS_WIDTH = 1,      /* in elements */
    MAXWELL_DMA_PARAMS_HEIGHT = 2,
    MAXWELL_DMA_PARAMS_DEPTH = 3,
    MAXWELL_DMA_PARAMS_LAYER = 4,      /* the slice copied */
    MAXWELL_DMA_PARAMS_ORIGIN = 5      /* Origin: x bits 0-15 (elements), y bits 16-31 */
};
static inline u32 dma_block_size_height(u32 v) { return (v >> 4) & 15u; }
static inline u32 dma_block_size_depth(u32 v) { return (v >> 8) & 15u; }
static inline u32 dma_origin_x(u32 v) { return v & 0xFFFFu; }
static inline u32 dma_origin_y(u32 v) { return v >> 16; }

/* LaunchDMA's fields (Eden's MaxwellDMA::LaunchDMA) */
enum { /* SemaphoreType, bits 3-4 */
    LAUNCH_DMA_SEMAPHORE_NONE = 0,
    LAUNCH_DMA_RELEASE_ONE_WORD_SEMAPHORE = 1,  /* the payload */
    LAUNCH_DMA_RELEASE_FOUR_WORD_SEMAPHORE = 2  /* the payload, 0, and the 64-bit timestamp */
};
enum { /* MemoryLayout, bits 7 (source) and 8 (destination) */
    LAUNCH_DMA_MEMORY_LAYOUT_BLOCKLINEAR = 0,
    LAUNCH_DMA_MEMORY_LAYOUT_PITCH = 1
};
static inline u32 launch_dma_data_transfer_type(u32 v) { return v & 3u; }
static inline int launch_dma_flush_enable(u32 v) { return (v >> 2) & 1u; }
static inline u32 launch_dma_semaphore_type(u32 v) { return (v >> 3) & 3u; }
static inline u32 launch_dma_interrupt_type(u32 v) { return (v >> 5) & 3u; }
static inline u32 launch_dma_src_memory_layout(u32 v) { return (v >> 7) & 1u; }
static inline u32 launch_dma_dst_memory_layout(u32 v) { return (v >> 8) & 1u; }
static inline int launch_dma_multi_line_enable(u32 v) { return (v >> 9) & 1u; }
static inline int launch_dma_remap_enable(u32 v) { return (v >> 10) & 1u; }

/* RemapConst::Swizzle: what a destination component takes. 7 isn't named; like NO_WRITE it writes nothing */
enum {
    REMAP_SWIZZLE_SRC_X = 0,
    REMAP_SWIZZLE_SRC_Y = 1,
    REMAP_SWIZZLE_SRC_Z = 2,
    REMAP_SWIZZLE_SRC_W = 3,
    REMAP_SWIZZLE_CONST_A = 4,
    REMAP_SWIZZLE_CONST_B = 5,
    REMAP_SWIZZLE_NO_WRITE = 6
};
/* the remap's component word: dst_x, dst_y, dst_z, dst_w at bits 0, 4, 8, 12 (3 bits each; Eden's GetComponent
   steps 3 bits, its BitFields 4), component_size_minus_one bits 16-17, num_src_components_minus_one bits 20-21,
   num_dst_components_minus_one bits 24-25 */
#define REMAP_COMPONENTS_MAX 4u
static inline u32 remap_dst_component(u32 v, u32 k) { return (v >> (4 * k)) & 7u; }
static inline u32 remap_component_size(u32 v) { return ((v >> 16) & 3u) + 1; }
static inline u32 remap_num_src_components(u32 v) { return ((v >> 20) & 3u) + 1; }
static inline u32 remap_num_dst_components(u32 v) { return ((v >> 24) & 3u) + 1; }

#endif
