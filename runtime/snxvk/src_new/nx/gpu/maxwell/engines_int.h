/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the GPU engines' files (engines.c, mme.c, mme_hle.c, compute.c, fermi2d.c, dma.c, maxwell3d.c) share: the
   front end's headers, a GPU address from its register pair, and the entry points one engine's file gives another */
#ifndef SNX_ENGINES_INT_H
#define SNX_ENGINES_INT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/memory/mem.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* Maxwell's GPU virtual addresses are 40 bits: a register pair holds bits 32-39 in its upper word's low byte and
   bits 0-31 in its lower word (Eden's PackedGPUVAddr) */
#define GPU_VA_UPPER_MASK 0xFFu
static inline u64 gpu_packed_va(u32 upper, u32 lower) { return ((u64)(upper & GPU_VA_UPPER_MASK) << 32) | lower; }

/* engines.c: a report semaphore operation (Eden's Maxwell3D::ProcessQueryGet; the query word v written to base + 3)
   of the 3D or compute engine, whose registers r hold the address (upper, lower) at base and base + 1 and the payload
   at base + 2 */
void query_get(GpuChannel* ch, u32* r, int base, u32 v);

/* mme.c and mme_jit.c: instructions one macro call may run, past which it never ends */
#define MME_STEPS 5000000u
/* mme.c: runs macro idx over the np parameters in ch->mme_params (it may stop at a wait, gpu.h's mme_resume),
   natively when its code is one mme_hle.c knows */
void mme_run(GpuChannel* ch, u32 idx, u32 np);
/* the macro upload methods: 0x45 (where the next code word goes, which begins an upload batch), 0x46 (a code word)
   and 0x48 (a macro's start, at the slot 0x47 set) */
void mme_upload_ptr(GpuChannel* ch, u32 v);
void mme_upload(GpuChannel* ch, u32 v);
void mme_bind(GpuChannel* ch, u32 v);
/* the methods a macro sends, recorded instead of sent: mme_verify records a native implementation and the
   interpreter, and compares them. The first MME_REC_N are kept, and the hash covers them all */
#define MME_REC_N 1024
typedef struct MmeRec {
    u32 n, total; /* methods kept, and sent */
    u64 hash;
    int read_after_send; /* a register read came after a send, which recording didn't apply: what it read can differ */
    u32 m[MME_REC_N], v[MME_REC_N];
} MmeRec;
/* what a macro does, natively or interpreted: sends method m (to the 3D engine, or a macro method's register only,
   as from the pushbuffer a macro method would start a macro inside this one), or reads register a. With rec, the
   send is recorded instead */
void mme_out(GpuChannel* ch, MmeRec* rec, u32 m, u32 v);
u32 mme_reg(GpuChannel* ch, MmeRec* rec, u32 a);

/* mme_jit.c: the compiled form of a macro's code, chosen by its hash after the native ones: 1 when it ran the macro
   (a wait may have stopped it, ch->mme.running), 0 when it has none and the interpreter runs it. mme_jit=0 turns
   the compiled forms off */
int mme_jit_run(GpuChannel* ch, MmeRec* rec, u32 idx, u32 np);
void mme_jit_free(void);
/* the compiled code's 5,000,000-instruction cutoff: the warning the interpreter writes */
void mme_jit_cutoff(GpuChannel* ch);

/* mme_hle.c: native implementations of known macros, by the hash of their code (mme.c's mme_choose) */
/* the implementation for a hash, or -1 */
int mme_native_find(u64 hash);
const char* mme_native_name(int k);
u64 mme_native_hash(int k);
/* runs implementation k over the np parameters p, as mme_out sends: 0 when it leaves this call to the interpreter,
   which it says before sending anything */
int mme_native_run(int k, GpuChannel* ch, MmeRec* rec, const u32* p, u32 np);
/* the iterations a native implementation loops over at most, about 9 of the macro's instructions each, under the
   interpreter's limit (mme.c's MME_STEPS): a call that could loop more is the interpreter's */
#define MME_LOOP_MAX 500000u

#endif
