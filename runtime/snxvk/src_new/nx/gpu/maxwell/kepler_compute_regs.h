/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/kepler_compute.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The compute engine's (KEPLER_COMPUTE_B, 0xB1C0) registers and its launch description (the QMD, Eden's
   LaunchParams). Register indices are method indices (byte offset / 4), named after Eden's KeplerCompute::Regs and
   their ASSERT_REG_POSITION; the two cache invalidations are NVIDIA's clb1c0.h. QMD indices are words of the 256
   byte description (Eden's ASSERT_LAUNCH_PARAM_POSITION) */
#ifndef SNX_GPU_MAXWELL_KEPLER_COMPUTE_REGS_H
#define SNX_GPU_MAXWELL_KEPLER_COMPUTE_REGS_H
#include "common/cpu/aarch64/cpu.h"

enum {
    KEPLER_COMPUTE_UPLOAD = 0x60,      /* the inline-to-memory block (inline_to_memory_regs.h) */
    KEPLER_COMPUTE_EXEC_UPLOAD = 0x6C, /* its launch */
    KEPLER_COMPUTE_DATA_UPLOAD = 0x6D, /* its data words */
    KEPLER_COMPUTE_INVALIDATE_SHADER_CACHES = 0x87, /* clb1c0.h NVB1C0_INVALIDATE_SHADER_CACHES (0x021C) */
    KEPLER_COMPUTE_LAUNCH_DESC_LOC = 0xAD, /* the launch description's address, in 256 byte units */
    KEPLER_COMPUTE_LAUNCH = 0xAF,          /* run the launch the description describes */
    KEPLER_COMPUTE_TSC = 0x557,            /* the sampler pool: address high, low, limit */
    KEPLER_COMPUTE_TIC = 0x55D,            /* the texture pool: address high, low, limit */
    KEPLER_COMPUTE_CODE_LOC_HIGH = 0x582,  /* code_loc: the address programs start from, bits 39:32 in 7:0 */
    KEPLER_COMPUTE_CODE_LOC_LOW = 0x583,
    KEPLER_COMPUTE_INVALIDATE_SHADER_CACHES_NO_WFI = 0x5A6, /* clb1c0.h (0x1698) */
    /* the report semaphore, laid out as the 3D engine's (Maxwell3D's report_semaphore): address high, low, payload,
       and the operation, whose write runs it */
    KEPLER_COMPUTE_REPORT_SEMAPHORE = 0x6C0,
    KEPLER_COMPUTE_REPORT_SEMAPHORE_QUERY = 0x6C3,
    KEPLER_COMPUTE_TEX_CB_INDEX = 0x982 /* the constant buffer bound textures' handles are read from */
};
#define KEPLER_COMPUTE_LAUNCH_DESC_LOC_SHIFT 8 /* launch_desc_loc.Address(): address << 8 */
#define KEPLER_COMPUTE_CODE_LOC_HIGH_MASK 0xFFu
/* INVALIDATE_SHADER_CACHES' INSTRUCTION, bit 0: the instruction cache, which a new program at old addresses needs */
#define KEPLER_COMPUTE_INVALIDATE_INSTRUCTION 1u

/* the launch description (Eden's LaunchParams): NUM_LAUNCH_PARAMETERS words, and the words of its fields */
#define KEPLER_COMPUTE_QMD_WORDS 0x40
enum {
    QMD_PROGRAM_START = 0x8,     /* the program's offset from code_loc */
    QMD_LINKED_TSC = 0xB,        /* linked_tsc, bit 30 */
    QMD_GRID_DIM_X = 0xC,        /* grid_dim_x, bits 30:0 */
    QMD_GRID_DIM_YZ = 0xD,       /* grid_dim_y 15:0, grid_dim_z 31:16 */
    QMD_SHARED_ALLOC = 0x11,     /* shared_alloc, bits 17:0 */
    QMD_BLOCK_DIM_X = 0x12,      /* block_dim_x, bits 31:16 */
    QMD_BLOCK_DIM_YZ = 0x13,     /* block_dim_y 15:0, block_dim_z 31:16 */
    QMD_CONST_BUFFER_ENABLE = 0x14, /* const_buffer_enable_mask 7:0, cache_layout 30:29 */
    QMD_CONST_BUFFER_CONFIG = 0x1D, /* 8 of them, 2 words each: address low; address high 7:0 and size 31:15 */
    QMD_LOCAL_POS_ALLOC = 0x2D,  /* local_pos_alloc 19:0, barrier_alloc 31:27 */
    QMD_LOCAL_NEG_ALLOC = 0x2E,  /* local_neg_alloc 19:0, gpr_alloc 28:24 */
    QMD_LOCAL_CRS_ALLOC = 0x2F   /* local_crs_alloc 19:0, sass_version 28:24 */
};

/* the QMD's fields (Eden's LaunchParams BitFields), from its words */
static inline u32 qmd_grid_dim_x(const u32* q) { return q[QMD_GRID_DIM_X] & 0x7FFFFFFFu; }
static inline u32 qmd_grid_dim_y(const u32* q) { return q[QMD_GRID_DIM_YZ] & 0xFFFFu; }
static inline u32 qmd_grid_dim_z(const u32* q) { return q[QMD_GRID_DIM_YZ] >> 16; }
static inline u32 qmd_block_dim_x(const u32* q) { return q[QMD_BLOCK_DIM_X] >> 16; }
static inline u32 qmd_block_dim_y(const u32* q) { return q[QMD_BLOCK_DIM_YZ] & 0xFFFFu; }
static inline u32 qmd_block_dim_z(const u32* q) { return q[QMD_BLOCK_DIM_YZ] >> 16; }
static inline u32 qmd_shared_alloc(const u32* q) { return q[QMD_SHARED_ALLOC] & 0x3FFFFu; }
static inline u32 qmd_program_start(const u32* q) { return q[QMD_PROGRAM_START]; }
/* the constant buffers: NumConstBuffers of them, each enabled by its bit of const_buffer_enable_mask, with a
   ConstBufferConfig of two words (address low; address high 7:0 and size 31:15) */
#define QMD_NUM_CONST_BUFFERS 8u
#define QMD_CONST_BUFFER_CONFIG_WORDS 2u
#define MAX_CONST_BUFFER_BYTES 0x10000u /* Eden's Maxwell3D::Regs::MaxConstBufferSize, 64 KiB */
static inline int qmd_const_buffer_enabled(const u32* q, u32 i) { return (q[QMD_CONST_BUFFER_ENABLE] >> i) & 1u; }
static inline u64 qmd_const_buffer_address(const u32* q, u32 i) {
    const u32* c = &q[QMD_CONST_BUFFER_CONFIG + i * QMD_CONST_BUFFER_CONFIG_WORDS];
    return (u64)(c[1] & 0xFFu) << 32 | c[0];
}
static inline u32 qmd_const_buffer_size(const u32* q, u32 i) {
    return q[QMD_CONST_BUFFER_CONFIG + i * QMD_CONST_BUFFER_CONFIG_WORDS + 1] >> 15;
}

/* the register pairs holding a 40-bit address, high word first (Eden's tsc/tic Address()), and tex_cb_index's
   index (bits 2:0) */
static inline u64 kepler_compute_address(const u32* r, u32 high) { return (u64)r[high] << 32 | r[high + 1]; }
static inline u32 kepler_compute_tex_cb_index(const u32* r) { return r[KEPLER_COMPUTE_TEX_CB_INDEX] & 7u; }

/* a texture handle (Eden's Tegra::Texture::TextureHandle): tic_id bits 19:0 and tsc_id bits 31:20, entries of the
   texture and sampler pools, each entry 32 bytes */
static inline u32 texture_handle_tic_id(u32 h) { return h & 0xFFFFFu; }
static inline u32 texture_handle_tsc_id(u32 h) { return h >> 20; }
#define TEXTURE_POOL_ENTRY_BYTES 32u

#endif
