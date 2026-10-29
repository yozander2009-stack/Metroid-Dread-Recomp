/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/engine_upload.h, src/video_core/engines/kepler_memory.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The inline-to-memory (upload) registers. The 3D engine (MAXWELL_B), compute (KEPLER_COMPUTE_B) and the
   inline-to-memory class (KEPLER_INLINE_TO_MEMORY_B) each have the same block of them at method 0x60 (Eden's
   Upload::Registers at ASSERT_REG_POSITION(upload, 0x60), then exec_upload at 0x6C and data_upload at 0x6D): a launch
   (EXEC) takes the destination from the block, and the data words that follow (DATA) bring the bytes. Indices are
   method indices (byte offset / 4); the UPLOAD_* ones are relative to the block's start, as gpu.c's i2m_launch reads
   them */
#ifndef SNX_GPU_MAXWELL_INLINE_TO_MEMORY_REGS_H
#define SNX_GPU_MAXWELL_INLINE_TO_MEMORY_REGS_H

/* where the block starts in each class, and its launch and data methods (Eden's upload, exec_upload, data_upload,
   and KeplerMemory's exec and data) */
#define I2M_UPLOAD_REGS 0x60
#define I2M_EXEC_UPLOAD 0x6C
#define I2M_DATA_UPLOAD 0x6D

/* Upload::Registers, from the block's start */
enum {
    UPLOAD_LINE_LENGTH_IN = 0x0,   /* bytes a line */
    UPLOAD_LINE_COUNT = 0x1,       /* lines */
    UPLOAD_DEST_ADDRESS_HIGH = 0x2, /* the destination's address, bits 39:32 in bits 7:0 (OFFSET_OUT_UPPER) */
    UPLOAD_DEST_ADDRESS_LOW = 0x3,
    UPLOAD_DEST_PITCH = 0x4,       /* a pitch linear destination's bytes from a line to the next */
    UPLOAD_DEST_BLOCK_SIZE = 0x5,  /* a block linear destination's block: width 3:0, height 7:4, depth 11:8 (log2) */
    UPLOAD_DEST_WIDTH = 0x6,       /* a block linear destination's width in bytes */
    UPLOAD_DEST_HEIGHT = 0x7,      /* its height in rows */
    UPLOAD_DEST_DEPTH = 0x8,       /* its depth in slices */
    UPLOAD_DEST_LAYER = 0x9,       /* the slice written */
    UPLOAD_DEST_X = 0xA,           /* the origin: x in bytes */
    UPLOAD_DEST_Y = 0xB,           /* and y in rows */
    UPLOAD_EXEC = 0xC,             /* exec_upload: the launch */
    UPLOAD_DATA = 0xD              /* data_upload: a data word */
};
#define UPLOAD_DEST_ADDRESS_HIGH_MASK 0xFFu
/* dest's block size fields (Eden's block_width, block_height and block_depth BitFields) */
#define UPLOAD_BLOCK_HEIGHT_SHIFT 4
#define UPLOAD_BLOCK_DEPTH_SHIFT 8
#define UPLOAD_BLOCK_FIELD_MASK 0xFu
/* exec_upload's linear, bit 0: the destination is pitch linear, else block linear */
#define UPLOAD_EXEC_LINEAR 1u

#endif
