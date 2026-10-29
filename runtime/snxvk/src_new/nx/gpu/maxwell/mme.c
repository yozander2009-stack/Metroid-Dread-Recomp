/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/macro.cpp, src/video_core/macro.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project */
/* The macro engine (MME): the 3D engine's macros, interpreted a step at a time. A macro's state is its channel's
   (GpuMme), so that a method it sends that makes the stream wait (a report acquire) stops the macro right after it,
   and the macro goes on from there (mme_resume) when the wait ends, before the stream's next word. A delay slot is a
   step of its own, so such a stop can come between a branch or exit and its slot */
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/tracy.h"

static u32 fetch_param(GpuChannel* ch) {
    GpuMme* m = &ch->mme;
    return m->next < m->np ? ch->mme_params[m->next++] : 0;
}
static void set_reg(GpuMme* m, u32 r, u32 v) {
    if (r) m->regs[r] = v; /* r0 is zero */
}
/* the method address: the method in bits 0-11, the increment after each send in bits 12-17 */
static void set_method(GpuMme* m, u32 v) {
    m->method = v & 0xFFF;
    m->incr = (v >> 12) & 0x3F;
}
/* rec: recorded instead of sent (mme_out) */
static void send(GpuChannel* ch, MmeRec* rec, u32 v) {
    GpuMme* m = &ch->mme;
    u32 at = m->method;
    m->method = (m->method + m->incr) & 0xFFF;
    mme_out(ch, rec, at, v);
}
/* the result operation (bits 4-6) of an ALU, add or bitfield step */
static void result(GpuChannel* ch, MmeRec* rec, u32 op, u32 dst, u32 v) {
    GpuMme* m = &ch->mme;
    switch (op) {
        case 0: set_reg(m, dst, fetch_param(ch)); break; /* fetch, the result unused */
        case 1: set_reg(m, dst, v); break;
        case 2: /* move, and the method address */
            set_reg(m, dst, v);
            set_method(m, v);
            break;
        case 3: /* fetch, and send the result */
            set_reg(m, dst, fetch_param(ch));
            send(ch, rec, v);
            break;
        case 4: /* move, and send it */
            set_reg(m, dst, v);
            send(ch, rec, v);
            break;
        case 5: /* fetch, and the result as the method address */
            set_reg(m, dst, fetch_param(ch));
            set_method(m, v);
            break;
        case 6: /* move, the method address, and send a parameter */
            set_reg(m, dst, v);
            set_method(m, v);
            send(ch, rec, fetch_param(ch));
            break;
        default: /* move, the method address, and send bits 12-17 of the result */
            set_reg(m, dst, v);
            set_method(m, v);
            send(ch, rec, (v >> 12) & 0x3F);
            break;
    }
}
static u32 alu(GpuMme* m, u32 op, u32 a, u32 b) {
    u64 r;
    switch (op) {
        case 0: r = (u64)a + b; break;
        case 1: r = (u64)a + b + m->carry; break;
        case 2:
            r = (u64)a - b;
            m->carry = r < 0x100000000ull; /* no borrow */
            return (u32)r;
        case 3:
            r = (u64)a - b - (m->carry ? 0 : 1);
            m->carry = r < 0x100000000ull;
            return (u32)r;
        case 8: return a ^ b;
        case 9: return a | b;
        case 10: return a & b;
        case 11: return a & ~b;
        case 12: return ~(a & b);
        default: LOG_LIMIT(4, "gpu: macro ALU operation %u is unknown\n", op); return 0;
    }
    m->carry = r >> 32 != 0;
    return (u32)r;
}
/* one instruction */
static void step(GpuChannel* ch, MmeRec* rec) {
    GpuMme* m = &ch->mme;
    u32 insn = ch->mme_code[m->pc & 0x1FFF], at = m->pc;
    u32 op = insn & 7, rop = (insn >> 4) & 7, dst = (insn >> 8) & 7;
    u32 a = m->regs[(insn >> 11) & 7], b = m->regs[(insn >> 14) & 7];
    u32 mask = (1u << ((insn >> 22) & 31)) - 1, src_bit = (insn >> 17) & 31, dst_bit = (insn >> 27) & 31;
    i32 imm = (i32)((insn >> 14) ^ 0x20000u) - 0x20000; /* bits 14-31, signed */
    int in_slot = m->slot, ends = m->exiting, taken = 0;
    m->pc++;
    m->slot = 0;
    m->exiting = 0;
    if (m->branch) { /* this is a taken branch's delay slot: after it, the target */
        m->branch = 0;
        m->pc = m->target;
    }
    switch (op) {
        case 0: result(ch, rec, rop, dst, alu(m, (insn >> 17) & 31, a, b)); break;
        case 1: result(ch, rec, rop, dst, a + (u32)imm); break;
        case 2: /* insert bits of b at src_bit into a at dst_bit */
            result(ch, rec, rop, dst, (a & ~(mask << dst_bit)) | (((b >> src_bit) & mask) << dst_bit));
            break;
        case 3: /* extract at a register's bit, shift left by an immediate */
            result(ch, rec, rop, dst, ((b >> (a & 31)) & mask) << dst_bit);
            break;
        case 4: /* extract at an immediate's bit, shift left by a register */
            result(ch, rec, rop, dst, ((b >> src_bit) & mask) << (a & 31));
            break;
        case 5: set_reg(m, dst, mme_reg(ch, rec, a + (u32)imm)); break; /* read a register */
        case 7: {                                                      /* branch on a zero (bit 4 clear) or not */
            int zero = !a;
            if (((insn >> 4) & 1) ? !zero : zero) {
                taken = 1;
                if ((insn >> 5) & 1) /* annulled: no delay slot */
                    m->pc = at + (u32)imm;
                else {
                    m->branch = 1;
                    m->target = at + (u32)imm;
                    m->slot = 1;
                }
            }
            break;
        }
        default: LOG_LIMIT(4, "gpu: macro operation %u is unknown\n", op); break;
    }
    if (ends) {
        /* this was an exit's delay slot. A taken branch here gets its own slot first */
        if (taken && m->branch)
            m->exiting = 1;
        else
            m->running = 0;
        return;
    }
    /* an exit (bit 7) ends the macro after its delay slot; not from inside a slot, and a taken branch's doesn't
       count. A branch not taken honours it, as Ryujinx's interpreter does */
    if (((insn >> 7) & 1) && !in_slot && !taken) {
        m->exiting = 1;
        m->slot = 1;
    }
}
/* runs the macro until it ends or a method it sent makes the stream wait (rec: recording, which never waits) */
static void mme_go(GpuChannel* ch, MmeRec* rec) {
    GpuMme* m = &ch->mme;
    while (m->running && !ch->park) {
        if (++m->steps > MME_STEPS) {
            WARN_LIMIT(4, "gpu: macro on channel %u ran %u instructions and is cut off\n", ch->id, MME_STEPS);
            m->running = 0;
            return;
        }
        step(ch, rec);
    }
}
void mme_resume(GpuChannel* ch) { mme_go(ch, NULL); }
/* runs macro idx through the interpreter, sending as mme_out does (rec records instead) */
static void mme_interpret(GpuChannel* ch, MmeRec* rec, u32 idx, u32 np) {
    GpuMme* m = &ch->mme;
    memset(m, 0, sizeof *m);
    m->pc = ch->mme_start[idx & 0x7F];
    m->np = np;
    m->regs[1] = np ? ch->mme_params[0] : 0; /* the first parameter is in r1 */
    m->next = 1;
    m->running = 1;
    mme_go(ch, rec);
}

/* ============================================================ native macros */
/* A macro whose code mme_hle.c knows runs as host code: the slot numbers are the SDK's choice, so the code's hash
   names the macro, and an unknown hash keeps the interpreter. The hash covers the code from the macro's start to the
   end of the upload batch that wrote it, as Eden's MacroEngine::Execute (src/video_core/macro.cpp at de7e6e01) hashes
   it, so that hashes Eden's table names mean the same code here: NVN uploads each macro in a batch of its own, which
   ends with the delay slot of its last exit */
void mme_upload_ptr(GpuChannel* ch, u32 v) {
    ch->mme_upload = v;
    ch->mme_batch++;
}
void mme_upload(GpuChannel* ch, u32 v) {
    u32 at = ch->mme_upload++ & 0x1FFF;
    ch->mme_code[at] = v;
    ch->mme_code_batch[at] = ch->mme_batch;
    memset(ch->mme_choice, 0, sizeof ch->mme_choice);
}
void mme_bind(GpuChannel* ch, u32 v) {
    ch->mme_start[ch->mme_start_ptr++ & 0x7F] = v;
    memset(ch->mme_choice, 0, sizeof ch->mme_choice);
}
/* Eden's Common::HashRange over u32 words (src/common/container_hash.h at de7e6e01, from Boost's hash_combine:
   2005-2014 Daniel James, with the 64-bit MurmurHash2 mix of 2016 Austin Appleby): each word is combined as itself */
static u64 mme_hash_word(u64 h, u32 w) {
    const u64 m = 0xC6A4A7935BD1E995ull;
    u64 k = w;
    k *= m;
    k ^= k >> 47;
    k *= m;
    h ^= k;
    h *= m;
    return h + 0xE6546B64ull;
}
void mme_out(GpuChannel* ch, MmeRec* rec, u32 m, u32 v) {
    m &= 0xFFF;
    if (rec) {
        if (rec->n < MME_REC_N) {
            rec->m[rec->n] = m;
            rec->v[rec->n] = v;
            rec->n++;
        }
        rec->total++;
        rec->hash = mme_hash_word(mme_hash_word(rec->hash, m), v);
    } else if (m >= 0xE00) /* a macro method only sets its register, as Ryujinx's interpreter does: starting a call
                              over the parameters the running macro is still reading is nothing the hardware does
                              (Eden would nest one) */
        ch->r3d[m] = v;
    else /* constant buffer data stays held for the words after it (r3d_const_upload), and the next method of any
            other kind writes it: a draw macro's base vertex and base instance went to guest memory as two writes,
            each with its renderer checks and stamps, before the draw and two more after it */
        maxwell3d_method(ch, m, v, !(m >= 0x8E4 && m <= 0x8F3));
}
u32 mme_reg(GpuChannel* ch, MmeRec* rec, u32 a) {
    if (rec && rec->total) rec->read_after_send = 1;
    return ch->r3d[a & 0xFFF];
}
/* the native implementation for macro idx, or -1: looked up once after its code or start changed */
static int mme_choose(GpuChannel* ch, u32 idx) {
    u32 c = ch->mme_choice[idx];
    if (!c) {
        u32 s = ch->mme_start[idx] & 0x1FFF, b = ch->mme_code_batch[s], n = 0;
        u64 h = 0;
        int k = -1;
        if (b) {
            while (n < 0x2000 && ch->mme_code_batch[(s + n) & 0x1FFF] == b) {
                h = mme_hash_word(h, ch->mme_code[(s + n) & 0x1FFF]);
                n++;
            }
            k = mme_native_find(h);
        }
        LOG_LIMIT(256, "mme: macro %u at word %x, %u words uploaded together, hash %016llx: %s\n", idx, s, n,
                  (unsigned long long)h, k >= 0 ? mme_native_name(k) : "interpreted");
        c = k >= 0 ? (u32)k + 2 : 1;
        ch->mme_choice[idx] = (u8)c;
        ch->mme_hash[idx] = h;
    }
    return (int)c - 2;
}
/* mme_hle=0 runs every macro through the interpreter; mme_verify=1 runs a known macro both ways, recorded,
   compares what they send, and sends what the interpreter does. A macro with no implementation has a compiled form
   (mme_jit.c) after the native ones, and mme_verify compares it with the interpreter too */
enum { MME_NATIVE = 1, MME_VERIFY = 2 };
static int mme_mode = -1;
/* calls, counted on the GPU thread (the only one running macros): native, compiled, interpreted, and in verify mode
   those compared, those that differed, those the recording couldn't judge (a register read after a send) and those
   the native or the compiled form left to the interpreter */
static u64 mme_n_native, mme_n_jit, mme_n_interp, mme_n_same, mme_n_differ, mme_n_unjudged, mme_n_declined;
static u64 mme_n_jit_same, mme_n_jit_differ, mme_n_jit_unjudged, mme_n_jit_declined;
/* the interpreted calls by the hash of their macro, in a table with a place for each of the first MME_STATS hashes:
   the log line names the busiest, the macros a native implementation would take next */
#define MME_STATS 64
static struct {
    u64 hash, calls;
} mme_stats[MME_STATS];
static void mme_stat(u64 h) {
    u32 i = (u32)(h ^ h >> 32) % MME_STATS, n;
    for (n = 0; n < MME_STATS && mme_stats[i].calls && mme_stats[i].hash != h; n++) i = (i + 1) % MME_STATS;
    if (n == MME_STATS) return;
    mme_stats[i].hash = h;
    mme_stats[i].calls++;
}
static void mme_stats_log(void) {
    char line[256];
    size_t at = 0;
    u32 shown[4], ns = 0, i, j;
    while (ns < 4) {
        u32 best = MME_STATS;
        for (i = 0; i < MME_STATS; i++) {
            for (j = 0; j < ns && shown[j] != i; j++) {}
            if (j == ns && mme_stats[i].calls && (best == MME_STATS || mme_stats[i].calls > mme_stats[best].calls))
                best = i;
        }
        if (best == MME_STATS) break;
        shown[ns++] = best;
        at += (size_t)snprintf(line + at, sizeof line - at, "%s%016llx %llu", ns > 1 ? ", " : "",
                               (unsigned long long)mme_stats[best].hash, (unsigned long long)mme_stats[best].calls);
        if (at >= sizeof line) break;
    }
    if (ns) LOG("mme: the macros interpreted most, by hash: %s\n", line);
}
static void mme_verify(GpuChannel* ch, u32 idx, int k, u32 np) {
    static MmeRec rec[2]; /* GPU thread only, and too large for a frame of its own */
    MmeRec *nat = &rec[0], *in = &rec[1];
    u32 i, n;
    nat->n = nat->total = in->n = in->total = 0;
    nat->hash = in->hash = 0;
    nat->read_after_send = in->read_after_send = 0;
    if (!mme_native_run(k, ch, nat, ch->mme_params, np)) {
        mme_n_declined++;
        return;
    }
    mme_interpret(ch, in, idx, np);
    if (nat->read_after_send || in->read_after_send) {
        mme_n_unjudged++;
        return;
    }
    if (nat->total == in->total && nat->hash == in->hash) {
        mme_n_same++;
        return;
    }
    mme_n_differ++;
    n = nat->n < in->n ? nat->n : in->n;
    i = 0;
    while (i < n && nat->m[i] == in->m[i] && nat->v[i] == in->v[i]) i++;
    if (i < n)
        WARN_LIMIT(32,
                   "mme: macro %u (%s, hash %016llx) differs from the interpreter at its send %u: native %x = %x, "
                   "interpreter %x = %x (%u and %u sends)\n",
                   idx, mme_native_name(k), (unsigned long long)mme_native_hash(k), i, nat->m[i], nat->v[i], in->m[i],
                   in->v[i], nat->total, in->total);
    else
        WARN_LIMIT(32, "mme: macro %u (%s) differs from the interpreter after send %u (%u and %u sends)\n", idx,
                   mme_native_name(k), i, nat->total, in->total);
}
/* the compiled form of macro idx against the interpreter, recorded: mme_verify's check for the macros mme_hle.c
   has no implementation for. The interpreter's sends are what the call then does */
static void mme_verify_jit(GpuChannel* ch, u32 idx, u32 np) {
    static MmeRec rec[2]; /* GPU thread only */
    MmeRec *jit = &rec[0], *in = &rec[1];
    u32 i, n;
    jit->n = jit->total = in->n = in->total = 0;
    jit->hash = in->hash = 0;
    jit->read_after_send = in->read_after_send = 0;
    if (!mme_jit_run(ch, jit, idx, np)) {
        mme_n_jit_declined++;
        return;
    }
    mme_interpret(ch, in, idx, np);
    if (jit->read_after_send || in->read_after_send) {
        mme_n_jit_unjudged++;
        return;
    }
    if (jit->total == in->total && jit->hash == in->hash) {
        mme_n_jit_same++;
        return;
    }
    mme_n_jit_differ++;
    n = jit->n < in->n ? jit->n : in->n;
    i = 0;
    while (i < n && jit->m[i] == in->m[i] && jit->v[i] == in->v[i]) i++;
    if (i < n)
        WARN_LIMIT(32,
                   "mme: macro %u (compiled, hash %016llx) differs from the interpreter at its send %u: compiled %x = "
                   "%x, interpreter %x = %x (%u and %u sends)\n",
                   idx, (unsigned long long)ch->mme_hash[idx], i, jit->m[i], jit->v[i], in->m[i], in->v[i],
                   jit->total, in->total);
    else
        WARN_LIMIT(32, "mme: macro %u (compiled) differs from the interpreter after send %u (%u and %u sends)\n", idx,
                   i, jit->total, in->total);
}
void mme_run(GpuChannel* ch, u32 idx, u32 np) {
    int k = -1;
    SNX_ZONE_V(z, "mme macro", idx);
    idx &= 0x7F;
    if (mme_mode < 0) {
        mme_mode = (g_settings.native_macros ? MME_NATIVE : 0) | (g_settings.verify_macros ? MME_VERIFY : 0);
    }
    if (mme_mode & MME_NATIVE) k = mme_choose(ch, idx);
    if (k >= 0) {
        if (mme_mode & MME_VERIFY) mme_verify(ch, idx, k, np);
        else if (mme_native_run(k, ch, NULL, ch->mme_params, np)) {
            mme_n_native++;
            goto done;
        }
    }
    /* no native implementation (or none that runs this call): the compiled form, then the interpreter */
    if (mme_mode & MME_NATIVE) {
        if (mme_mode & MME_VERIFY) mme_verify_jit(ch, idx, np);
        else if (mme_jit_run(ch, NULL, idx, np)) {
            mme_n_jit++;
            goto done;
        }
    }
    mme_n_interp++;
    if (mme_mode & MME_NATIVE) mme_stat(ch->mme_hash[idx]);
    mme_interpret(ch, NULL, idx, np);
done:
    if ((mme_n_native + mme_n_jit + mme_n_interp) % 1000000 == 0) {
        LOG("mme: %llu macro calls, %llu native, %llu compiled and %llu interpreted\n",
            (unsigned long long)(mme_n_native + mme_n_jit + mme_n_interp), (unsigned long long)mme_n_native,
            (unsigned long long)mme_n_jit, (unsigned long long)mme_n_interp);
        mme_stats_log();
        if (mme_mode & MME_VERIFY)
            LOG("mme: verified %llu calls the same, %llu different, %llu unjudged (a read after a send), %llu left to the "
                "interpreter\n",
                (unsigned long long)mme_n_same, (unsigned long long)mme_n_differ, (unsigned long long)mme_n_unjudged,
                (unsigned long long)mme_n_declined);
        if (mme_mode & MME_VERIFY)
            LOG("mme: compiled, verified %llu calls the same, %llu different, %llu unjudged, %llu with no compiled form\n",
                (unsigned long long)mme_n_jit_same, (unsigned long long)mme_n_jit_differ,
                (unsigned long long)mme_n_jit_unjudged, (unsigned long long)mme_n_jit_declined);
    }
}
