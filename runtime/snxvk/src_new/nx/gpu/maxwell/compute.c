/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/kepler_compute.cpp, src/video_core/engines/kepler_compute.h,
   src/video_core/engines/kepler_memory.cpp, src/video_core/engines/kepler_memory.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The compute engine (KEPLER_COMPUTE_B, 0xB1C0): launches, which the renderer runs from their launch description
   (the QMD, Eden's LaunchParams), its inline-to-memory methods and its report semaphore; and the inline-to-memory
   class (KEPLER_INLINE_TO_MEMORY_B, 0xA140, Eden's KeplerMemory). gpu.c's method_call hands both their methods, by
   the class bound to the call's subchannel. Register names are kepler_compute_regs.h's and
   inline_to_memory_regs.h's */
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/maxwell/inline_to_memory_regs.h"
#include "nx/gpu/maxwell/kepler_compute_regs.h"

#define QMD_BYTES (KEPLER_COMPUTE_QMD_WORDS * 4) /* the launch description, 256 bytes */
#define CAPTURE_CODE_BYTES 0x10000               /* a captured program: up to 64 KiB of it */
#define CAPTURE_PROGRAMS 64                      /* the programs captured at most */
/* renderer.h's method_barrier kind 5: the shader instruction cache is invalidated */
#define BARRIER_SHADER_INSTRUCTION_INVALIDATE 5

/* With diag=1 (the Shader captures setting), each new compute program's launch description and code go to
   captures/compute, for debugging: the first CAPTURE_PROGRAMS programs, the description then the code in one file */
static void capture(const u32* q, u64 code) {
    static u64 seen[CAPTURE_PROGRAMS];
    static u32 nseen;
    u64 pa = code + q[QMD_PROGRAM_START];
    char path[sizeof g_exedir + 64];
    u8* buf;
    FILE* f;
    u32 i;
    for (i = 0; i < nseen; i++)
        if (seen[i] == pa) return;
    if (nseen == CAPTURE_PROGRAMS) return;
    seen[nseen++] = pa;
    buf = (u8*)malloc(QMD_BYTES + CAPTURE_CODE_BYTES);
    if (!buf) return;
    memcpy(buf, q, QMD_BYTES);
    snx_le32_array(buf, KEPLER_COMPUTE_QMD_WORDS); /* the description as guest memory held it */
    gmem_read_code(pa, buf + QMD_BYTES, CAPTURE_CODE_BYTES); /* up to where guest memory ends */
    snprintf(path, sizeof path, "%scaptures", g_exedir);
    plat_mkdir(path);
    snprintf(path, sizeof path, "%scaptures/compute", g_exedir);
    plat_mkdir(path);
    snprintf(path, sizeof path, "%scaptures/compute/cs_%010llx.bin", g_exedir, (unsigned long long)pa);
    f = fopen(path, "wb");
    if (f) {
        if (fwrite(buf, 1, QMD_BYTES + CAPTURE_CODE_BYTES, f) != QMD_BYTES + CAPTURE_CODE_BYTES)
            WARN_LIMIT(1, "gpu: %s wasn't written\n", path);
        fclose(f);
    }
    free(buf);
}
/* Eden's ProcessLaunch: the launch description at launch_desc_loc (in 256 byte units), the program at its
   program_start from code_loc. The renderer reads the description's fields (LaunchParams) */
static void launch(GpuChannel* ch) {
    const u32* r = ch->rcomp;
    u32 q[KEPLER_COMPUTE_QMD_WORDS];
    u64 code = ((u64)(r[KEPLER_COMPUTE_CODE_LOC_HIGH] & KEPLER_COMPUTE_CODE_LOC_HIGH_MASK) << 32) |
               r[KEPLER_COMPUTE_CODE_LOC_LOW];
    gmem_read((u64)r[KEPLER_COMPUTE_LAUNCH_DESC_LOC] << KEPLER_COMPUTE_LAUNCH_DESC_LOC_SHIFT, q, sizeof q);
    snx_le32_array(q, KEPLER_COMPUTE_QMD_WORDS); /* the renderer reads its words and bitfields as numbers */
    if (g_settings.shader_captures) capture(q, code);
    if (g_renderer->dispatch) g_renderer->dispatch(ch, q, code);
}
/* Eden's KeplerCompute::CallMethod: every method is kept in the registers, and these act */
void compute_method(GpuChannel* ch, u32 m, u32 v, int last) {
    (void)last;
    m &= KEPLER_COMPUTE_METHODS - 1;
    ch->rcomp[m] = v;
    switch (m) {
        case KEPLER_COMPUTE_EXEC_UPLOAD: i2m_launch(ch, ch->rcomp, KEPLER_COMPUTE_UPLOAD); break;
        case KEPLER_COMPUTE_DATA_UPLOAD: i2m_data(ch, v); break;
        case KEPLER_COMPUTE_INVALIDATE_SHADER_CACHES:
        case KEPLER_COMPUTE_INVALIDATE_SHADER_CACHES_NO_WFI:
            if ((v & KEPLER_COMPUTE_INVALIDATE_INSTRUCTION) && g_renderer->method_barrier)
                g_renderer->method_barrier(BARRIER_SHADER_INSTRUCTION_INVALIDATE);
            break;
        case KEPLER_COMPUTE_LAUNCH: launch(ch); break;
        case KEPLER_COMPUTE_REPORT_SEMAPHORE_QUERY: /* the report semaphore, the 3D engine's */
            query_get(ch, ch->rcomp, KEPLER_COMPUTE_REPORT_SEMAPHORE, v);
            break;
        default: break;
    }
}
/* Eden's KeplerMemory::CallMethod: the inline-to-memory class is only the upload block */
void kepler_i2m_method(GpuChannel* ch, u32 m, u32 v) {
    if (m >= KEPLER_I2M_METHODS) return;
    ch->ri2m[m] = v;
    if (m == I2M_EXEC_UPLOAD)
        i2m_launch(ch, ch->ri2m, I2M_UPLOAD_REGS);
    else if (m == I2M_DATA_UPLOAD)
        i2m_data(ch, v);
}
