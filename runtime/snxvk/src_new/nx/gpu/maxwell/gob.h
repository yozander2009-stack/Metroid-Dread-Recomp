/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/textures/decoders.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* A GOB (group of bytes), the tile of Maxwell's block linear layout: 64 bytes by 8 rows, 512 bytes, stacked in
   blocks of 1 << block_height GOBs down and 1 << block_depth deep (docs/BLOCKLINEAR.md; Eden's GOB_SIZE_X, _Y and
   GOB_SIZE). The copy and 2D engines size their block linear operands with them */
#ifndef SNX_MAXWELL_GOB_H
#define SNX_MAXWELL_GOB_H

#define MAXWELL_GOB_SIZE_X 64u /* bytes */
#define MAXWELL_GOB_SIZE_Y 8u  /* rows */
#define MAXWELL_GOB_SIZE 512u  /* MAXWELL_GOB_SIZE_X * MAXWELL_GOB_SIZE_Y */
#define MAXWELL_GOB_SIZE_X_SHIFT 6u
#define MAXWELL_GOB_SIZE_Y_SHIFT 3u
#define MAXWELL_GOB_SIZE_SHIFT 9u

#endif
