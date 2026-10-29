/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The FP32 flush probe's SPIR-V (ftz_probe.comp). The words are in ftz_probe_spv.c, which the build makes from the
   shader with glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_FTZ_PROBE_SPV_H
#define SNX_FTZ_PROBE_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 ftz_probe_spv[];
extern const u64 ftz_probe_spv_size; /* in bytes */

#endif
