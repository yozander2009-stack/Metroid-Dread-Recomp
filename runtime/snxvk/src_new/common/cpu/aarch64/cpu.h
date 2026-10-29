/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Written from the Arm Architecture Reference Manual for A-profile (DDI 0487) pseudocode, with the bitfield,
   division and exclusive-monitor rules cross-checked against QEMU (target/arm/tcg/translate-a64.c,
   helper-a64.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The guest CPU of a game module (a Cortex-A57 at EL0) and the small helpers generated code
   inlines. Every module and its runtime library include it, and the emitters reach the fields by offsetof, so the
   layout below is the binary contract that SNX_ABI versions. */
#ifndef SNX_CPU_H
#define SNX_CPU_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef float f32;
typedef double f64;

#define RESTRICT    __restrict__
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define SNX_INLINE static inline __attribute__((always_inline))

/* The version of the contract between a game module and the runtime: this header (the CPU layout, the SNX_FP_*
   numbers), the helpers in cpu_helpers.c, RtAPI and the module descriptor. The recompiler writes it into each
   module and the runtime refuses any other. Bump it for any change of layout or of a helper's results:
   docs/ARCHITECTURE.md's Module ABI says what changes it, and tests/module_contract.c fails when the layout moves
   while it stays.
   7: CPU.poll and RtAPI.poll, read on the backward branches of generated code.
   8: the FP/SIMD emitters and discovery changes.
   9: RtAPI.svcs, which the NX modules call each SVC through by its number.
  10: RtAPI.miss replaces the module's own snx_interp_entry as snx_lookup's fallback, and CPU.disp's slots are read
      with relaxed atomic loads, since the runtime JIT fills them while guest threads run.
  11: the reworked LLVM backend's modules. Old 10 had been given to two incompatible module formats, and a module
      built for the wrong one must not load.
  12: snx_cas128 takes the CPU, and RtAPI.cas128 is the 16-byte compare-and-swap of the RISC-V and LoongArch
      modules.
  13: the AOT module's snx_variants() table, which the runtime reads for code the recompiler missed instead of
      compiling it at run time.
  14: code the guest patches at run time. IC IVAU calls RtAPI.code_changed, and each AOT function first reads its
      byte in SnxModule.stale, which the runtime sets when a line it was compiled from changed: the function then
      runs its entry through dispatch, where the runtime compiles the code there now.
  15: a store-exclusive whose range is covered by the monitor that armed it. A narrow store-exclusive after a wide
      load-exclusive stores (and returns 0), where every build before made it fail and return 1, so a game's retry
      loop never ended: both the generated code and the interpreter's helper emit the new comparison. */
#define SNX_ABI 15

#if defined(_WIN32)
#define SNX_EXPORT __declspec(dllexport)
#else
#define SNX_EXPORT __attribute__((visibility("default")))
#endif

/* one 128-bit vector register holding the guest's bytes, lane 0 in the lowest-addressed ones: on a big-endian host the
   views wider than a byte are byte-swapped numbers, read and written as numbers through SNX_VS and its kin */
typedef union V128 {
    u8 b[16];
    i8 sb[16];
    u16 h[8];
    i16 sh[8];
    u32 s[4];
    i32 ss[4];
    u64 d[2];
    i64 sd[2];
    f32 f[4];
    f64 df[2];
} V128;

#include "common/runtime/interface.h"
typedef void (*GuestFn)(struct CPU* RESTRICT c, u64 entry);

typedef struct CPU {
    u64 x[32];        /* X0-X30, and x[31] is SP: the zero register never reaches the state */
    u64 pc;
    u32 n, z, cf, vf; /* PSTATE.NZCV, one 0/1 field each */
    V128 v[32];
    u32 fpcr, fpsr;
    u64 tpidr, tpidrro; /* TPIDR_EL0, TPIDRRO_EL0 (the thread's TLS) */
    /* the exclusive monitor: armed by a load-exclusive with its address, size (pairs or 0x10 into it) and the
       value(s) it read */
    u64 excl_addr, excl_val, excl_val2;
    u32 excl_size;
    u32 excl_on;
    u8* mem;       /* the host address of guest address 0 */
    GuestFn* disp; /* the function of each code word: disp[(pc - code_lo) >> 2], 0 for none */
    u64 code_lo, code_hi;
    const RtAPI* rt;
    void* thread; /* the runtime thread or process this CPU belongs to */
    /* written by another host thread when the runtime wants this thread's core back (a thread of higher priority
       waits for it): generated code reads it on each backward branch, the interpreter on each taken branch, and then
       calls rt->poll */
    u32 poll;
} CPU;

/* The emitters bake these offsets into generated code, so modules built before a layout change would read the
   wrong fields: a failed check here means SNX_ABI must change too. The fields after mem move with the pointer size,
   and are checked on 64-bit hosts, the only ones games run on. */
typedef char snx_check_v128[sizeof(V128) == 16 ? 1 : -1];
typedef char snx_check_cpu_x[offsetof(CPU, x) == 0 ? 1 : -1];
typedef char snx_check_cpu_pc[offsetof(CPU, pc) == 256 ? 1 : -1];
typedef char snx_check_cpu_nzcv[offsetof(CPU, n) == 264 && offsetof(CPU, z) == 268 && offsetof(CPU, cf) == 272 &&
                                        offsetof(CPU, vf) == 276
                                    ? 1
                                    : -1];
typedef char snx_check_cpu_v[offsetof(CPU, v) == 280 ? 1 : -1];
typedef char snx_check_cpu_fp[offsetof(CPU, fpcr) == 792 && offsetof(CPU, fpsr) == 796 ? 1 : -1];
typedef char snx_check_cpu_tls[offsetof(CPU, tpidr) == 800 && offsetof(CPU, tpidrro) == 808 ? 1 : -1];
typedef char snx_check_cpu_excl[offsetof(CPU, excl_addr) == 816 && offsetof(CPU, excl_val) == 824 &&
                                        offsetof(CPU, excl_val2) == 832 && offsetof(CPU, excl_size) == 840 &&
                                        offsetof(CPU, excl_on) == 844
                                    ? 1
                                    : -1];
typedef char snx_check_cpu_mem[offsetof(CPU, mem) == 848 ? 1 : -1];
typedef char snx_check_cpu_tail[sizeof(void*) != 8 || (offsetof(CPU, disp) == 856 && offsetof(CPU, code_lo) == 864 &&
                                                       offsetof(CPU, code_hi) == 872 && offsetof(CPU, rt) == 880 &&
                                                       offsetof(CPU, thread) == 888 && offsetof(CPU, poll) == 896 &&
                                                       sizeof(CPU) == 904)
                                    ? 1
                                    : -1];

/* ---- guest memory: a flat mapping at M, no translation, holding the guest's little-endian bytes: a big-endian
   host (ppc64) swaps what it reads and writes as numbers, as the IR emitters' ir_rd and ir_wr do ---- */
#define MP(a) (M + (u64)(a))
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SNX_GUEST16(v) __builtin_bswap16(v)
#define SNX_GUEST32(v) __builtin_bswap32(v)
#define SNX_GUEST64(v) __builtin_bswap64(v)
#else
#define SNX_GUEST16(v) (v)
#define SNX_GUEST32(v) (v)
#define SNX_GUEST64(v) (v)
#endif
/* a vector register's lane as a number: V128 holds the guest's bytes (lane 0 lowest-addressed, as in memory), so on a
   big-endian host each 16, 32 or 64-bit lane read or written as a number is swapped (the IR's ir_vd and ir_ve) */
#define SNX_VB(v, i)         ((v).b[i])
#define SNX_VH(v, i)         ((u16)SNX_GUEST16((v).h[i]))
#define SNX_VS(v, i)         ((u32)SNX_GUEST32((v).s[i]))
#define SNX_VD(v, i)         ((u64)SNX_GUEST64((v).d[i]))
#define SNX_VSET_B(v, i, x)  ((v).b[i] = (u8)(x))
#define SNX_VSET_H(v, i, x)  ((v).h[i] = (u16)SNX_GUEST16((u16)(x)))
#define SNX_VSET_S(v, i, x)  ((v).s[i] = (u32)SNX_GUEST32((u32)(x)))
#define SNX_VSET_D(v, i, x)  ((v).d[i] = (u64)SNX_GUEST64((u64)(x)))

/* ---- indirect calls: the function compiled for a pc, or the runtime JIT for one discovery missed ---- */
/* A slot goes from NULL to a function while other threads read it (the runtime JIT publishes with release ordering,
   gpu/nvn_trace.c installs wrappers): a relaxed load, which costs a plain one, makes that no data race. The code and
   data behind a published address were written before the release store, and reaching them depends on the loaded
   address, which orders them on every host the backends target */
SNX_INLINE GuestFn snx_lookup(CPU* c, u64 pc);
SNX_INLINE GuestFn snx_lookup(CPU* c, u64 pc) {
    u64 offset = pc - c->code_lo;
    GuestFn fn;
    if (UNLIKELY(offset >= c->code_hi - c->code_lo || (pc & 3))) return c->rt->miss;
    fn = __atomic_load_n(&c->disp[offset >> 2], __ATOMIC_RELAXED);
    return LIKELY(fn != 0) ? fn : c->rt->miss;
}
#define CALLDYN(t)                                                                                                     \
    do {                                                                                                               \
        u64 t__ = (t);                                                                                                 \
        snx_lookup(c, t__)(c, t__);                                                                                    \
    } while (0)

/* ---- condition codes (ConditionHolds) ---- */
SNX_INLINE int cond_ok(const CPU* c, u32 cond) {
    int holds;
    switch ((cond >> 1) & 7) {
        case 0: holds = c->z != 0; break;                       /* EQ/NE */
        case 1: holds = c->cf != 0; break;                      /* CS/CC */
        case 2: holds = c->n != 0; break;                       /* MI/PL */
        case 3: holds = c->vf != 0; break;                      /* VS/VC */
        case 4: holds = c->cf != 0 && c->z == 0; break;         /* HI/LS */
        case 5: holds = c->n == c->vf; break;                   /* GE/LT */
        case 6: holds = c->z == 0 && c->n == c->vf; break;      /* GT/LE */
        default: return 1;                                      /* AL, and NV which also always holds */
    }
    return (cond & 1) ? !holds : holds;
}
/* NZCV from a nibble (N in bit 3), and to the register's bits 31:28 */
SNX_INLINE void set_nzcv(CPU* c, u32 nzcv) {
    c->n = (nzcv >> 3) & 1;
    c->z = (nzcv >> 2) & 1;
    c->cf = (nzcv >> 1) & 1;
    c->vf = nzcv & 1;
}
SNX_INLINE u32 get_nzcv(const CPU* c) { return (c->n << 31) | (c->z << 30) | (c->cf << 29) | (c->vf << 28); }

/* ---- AddWithCarry and the flag-setting forms ---- */
SNX_INLINE u64 adds64(CPU* c, u64 a, u64 b, u32 cin) {
    u64 sum = a + b + cin;
    c->n = (u32)(sum >> 63);
    c->z = sum == 0;
    c->cf = cin ? sum <= a : sum < a; /* the unsigned sum overflowed */
    c->vf = (u32)((~(a ^ b) & (a ^ sum)) >> 63); /* same-signed operands, other-signed result */
    return sum;
}
SNX_INLINE u32 adds32(CPU* c, u32 a, u32 b, u32 cin) {
    u32 sum = a + b + cin;
    c->n = sum >> 31;
    c->z = sum == 0;
    c->cf = cin ? sum <= a : sum < a;
    c->vf = (~(a ^ b) & (a ^ sum)) >> 31;
    return sum;
}
/* a - b is AddWithCarry(a, ~b, 1): carry means no borrow */
SNX_INLINE u64 subs64(CPU* c, u64 a, u64 b) {
    u64 diff = a - b;
    c->n = (u32)(diff >> 63);
    c->z = diff == 0;
    c->cf = a >= b;
    c->vf = (u32)(((a ^ b) & (a ^ diff)) >> 63);
    return diff;
}
SNX_INLINE u32 subs32(CPU* c, u32 a, u32 b) {
    u32 diff = a - b;
    c->n = diff >> 31;
    c->z = diff == 0;
    c->cf = a >= b;
    c->vf = ((a ^ b) & (a ^ diff)) >> 31;
    return diff;
}
/* ANDS/BICS: N and Z from the result, C and V cleared */
SNX_INLINE void logic64(CPU* c, u64 r) {
    c->n = (u32)(r >> 63);
    c->z = r == 0;
    c->cf = 0;
    c->vf = 0;
}
SNX_INLINE void logic32(CPU* c, u32 r) {
    c->n = r >> 31;
    c->z = r == 0;
    c->cf = 0;
    c->vf = 0;
}

/* ---- shifts: s below the width, except the rotates, which take it modulo the width ---- */
SNX_INLINE u64 ror64(u64 x, u32 s) {
    s &= 63;
    return s ? (x >> s) | (x << (64 - s)) : x;
}
SNX_INLINE u32 ror32(u32 x, u32 s) {
    s &= 31;
    return s ? (x >> s) | (x << (32 - s)) : x;
}
/* the shifted register operand: t is LSL, LSR, ASR, ROR */
SNX_INLINE u64 shf64(u64 x, u32 t, u32 s) {
    if (t == 0) return x << s;
    if (t == 1) return x >> s;
    if (t == 2) return (u64)((i64)x >> s);
    return ror64(x, s);
}
SNX_INLINE u32 shf32(u32 x, u32 t, u32 s) {
    if (t == 0) return x << s;
    if (t == 1) return x >> s;
    if (t == 2) return (u32)((i32)x >> s);
    return ror32(x, s);
}
/* the extended register operand: opt is UXTB, UXTH, UXTW, UXTX, SXTB, SXTH, SXTW, SXTX, then LSL #sh */
SNX_INLINE u64 ext64(u64 x, u32 opt, u32 sh) {
    switch (opt & 7) {
        case 0: x &= 0xFF; break;
        case 1: x &= 0xFFFF; break;
        case 2: x &= 0xFFFFFFFFu; break;
        case 4: x = (u64)(i64)(i8)(u8)x; break;
        case 5: x = (u64)(i64)(i16)(u16)x; break;
        case 6: x = (u64)(i64)(i32)(u32)x; break;
        default: break; /* UXTX, SXTX: the whole register */
    }
    return x << sh;
}

/* ---- bit counts and reversals ---- */
SNX_INLINE u32 clz64(u64 x) { return x ? (u32)__builtin_clzll(x) : 64; }
SNX_INLINE u32 clz32(u32 x) { return x ? (u32)__builtin_clz(x) : 32; }
/* FreeBSD defines these */
#ifndef bswap32
SNX_INLINE u32 bswap32(u32 x) { return __builtin_bswap32(x); }
#endif
#ifndef bswap64
SNX_INLINE u64 bswap64(u64 x) { return __builtin_bswap64(x); }
#endif
/* CLS: the leading bits equal to the sign bit, the sign bit itself not counted */
SNX_INLINE u32 cls64(u64 x) { return clz64(x ^ (u64)((i64)x >> 1)) - 1; }
SNX_INLINE u32 cls32(u32 x) { return clz32(x ^ (u32)((i32)x >> 1)) - 1; }
SNX_INLINE u64 rbit64(u64 x) {
    const u64 m1 = 0x5555555555555555ull, m2 = 0x3333333333333333ull,
              m4 = 0x0F0F0F0F0F0F0F0Full;
    x = ((x & m1) << 1) | ((x >> 1) & m1); /* swap neighbouring bits, then pairs, then nibbles: bytes reversed */
    x = ((x & m2) << 2) | ((x >> 2) & m2);
    x = ((x & m4) << 4) | ((x >> 4) & m4);
    return bswap64(x);
}
SNX_INLINE u32 rbit32(u32 x) { return (u32)(rbit64(x) >> 32); }
/* REV16: the bytes of each halfword swapped */
SNX_INLINE u64 rev16_64(u64 x) {
    const u64 m = 0x00FF00FF00FF00FFull;
    return ((x & m) << 8) | ((x >> 8) & m);
}
/* REV32: the bytes of each word reversed */
SNX_INLINE u64 rev32_64(u64 x) {
    u64 r = bswap64(x);
    return (r << 32) | (r >> 32);
}

/* ---- division: x/0 is 0 and INT_MIN/-1 is INT_MIN, with no trap ---- */
SNX_INLINE u64 udiv64(u64 a, u64 b) { return b ? a / b : 0; }
SNX_INLINE u32 udiv32(u32 a, u32 b) { return b ? a / b : 0; }
SNX_INLINE i64 sdiv64(i64 a, i64 b) {
    if (b == 0) return 0;
    if (b == -1) return (i64)((u64)0 - (u64)a); /* negation wraps INT_MIN to itself, where C's division traps */
    return a / b;
}
SNX_INLINE i32 sdiv32(i32 a, i32 b) {
    if (b == 0) return 0;
    if (b == -1) return (i32)((u32)0 - (u32)a);
    return a / b;
}

/* ---- the high half of a 64x64 product, from 32-bit partial products ---- */
SNX_INLINE u64 umulh64(u64 a, u64 b) {
    u64 al = (u32)a, ah = a >> 32, bl = (u32)b, bh = b >> 32;
    u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    u64 mid = (ll >> 32) + (u32)lh + (u32)hl; /* below 3 * 2^32: no carry lost */
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}
/* signed: the unsigned product less b for a negative a and less a for a negative b (mod 2^64) */
SNX_INLINE i64 smulh64(i64 a, i64 b) {
    u64 high = umulh64((u64)a, (u64)b);
    if (a < 0) high -= (u64)b;
    if (b < 0) high -= (u64)a;
    return (i64)high;
}

/* ---- UBFM, SBFM, BFM, as ir_helpers.c builds them in IR (DecodeBitMasks with immediate false) ----
   wmask is the rotated field of imms+1 ones, tmask the d+1 low ones, d = (imms - immr) mod datasize */
SNX_INLINE u64 bmask(u32 n) { return n >= 64 ? ~(u64)0 : ((u64)1 << n) - 1; }
SNX_INLINE u64 snx_bf_ror(u64 x, u32 s, u32 ds) { return ds == 32 ? ror32((u32)x, s) : ror64(x, s); }
SNX_INLINE u64 ubfm_(u64 src, u32 immr, u32 imms, u32 ds) {
    u64 wmask = snx_bf_ror(bmask(imms + 1), immr, ds), tmask = bmask(((imms - immr) & (ds - 1)) + 1);
    return snx_bf_ror(src, immr, ds) & wmask & tmask;
}
SNX_INLINE u64 sbfm_(u64 src, u32 immr, u32 imms, u32 ds) {
    u64 wmask = snx_bf_ror(bmask(imms + 1), immr, ds), tmask = bmask(((imms - immr) & (ds - 1)) + 1);
    u64 bot = snx_bf_ror(src, immr, ds) & wmask, top = ((src >> imms) & 1) ? bmask(ds) : 0;
    return (top & ~tmask & bmask(ds)) | (bot & tmask);
}
SNX_INLINE u64 bfm_(u64 dst, u64 src, u32 immr, u32 imms, u32 ds) {
    u64 wmask = snx_bf_ror(bmask(imms + 1), immr, ds), tmask = bmask(((imms - immr) & (ds - 1)) + 1);
    u64 bot = (dst & ~wmask) | (snx_bf_ror(src, immr, ds) & wmask);
    return ((dst & ~tmask) | (bot & tmask)) & bmask(ds);
}

/* ---- host instruction set extensions ----
   The helpers are plain C with no host-specific paths. The runtime library (rtlib.c) is compiled without -march,
   since the recompiler that embeds it runs on other machines than the one that built it; the LLVM backend compiles
   it for the CPU each module is built on (snx_ir_rtlib_object), and the generated code uses the host's extensions
   itself (ir.c's snx_ir_host). */

/* ---- floating point (helpers/fp.c) ----
   Raw IEEE encodings in and out, with integer arithmetic only: no host floating point, libm or host rounding
   state, so results don't depend on the host. Only c->fpcr is read and only c->fpsr written (its cumulative flags,
   never cleared), so exceptions never reach process-global state. Widths: 16 (FEAT_FP16's halves, under FPCR.FZ16
   and never FPCR.AHP, which only snx_fp_convert's halves take), 32 and 64. Rounding modes: 0 ties to even,
   1 toward +inf, 2 toward -inf, 3 toward zero, 4 ties away, 5 to odd (FCVTXN). Integer widths are 32 or 64, with
   0 <= fbits <= the integer width.
   Generated code passes the operation numbers below as literals: they are ABI. */
enum { SNX_FP_ADD, SNX_FP_SUB, SNX_FP_MUL, SNX_FP_DIV, SNX_FP_MULX, SNX_FP_ABD, SNX_FP_RECPS, SNX_FP_RSQRTS };
enum { SNX_FP_SQRT, SNX_FP_RECPE, SNX_FP_RSQRTE, SNX_FP_RECPX };
typedef char snx_check_fp_ops[SNX_FP_MUL == 2 && SNX_FP_RSQRTS == 7 && SNX_FP_SQRT == 0 && SNX_FP_RECPX == 3 ? 1 : -1];
u64 snx_fp_binary(CPU* c, u64 a, u64 b, int width, int op);
u64 snx_fp_fma(CPU* c, u64 add, u64 a, u64 b, int width); /* add + a * b, rounded once */
u64 snx_fp_unary(CPU* c, u64 bits, int width, int op);
u64 snx_fp_convert(CPU* c, u64 bits, int src_width, int dst_width, int rm);
u64 snx_fp_to_int(CPU* c, u64 bits, int fp_width, int int_width, int uns, int fbits, int rm);
u64 snx_fp_from_int(CPU* c, u64 bits, int int_width, int fp_width, int uns, int fbits);
u32 snx_fp_compare(CPU* c, u64 a, u64 b, int width, int signal); /* the NZCV nibble of FCMP (signal: FCMPE) */
u64 snx_fp_minmax(CPU* c, u64 a, u64 b, int width, int minimum, int numeric);
u64 snx_fp_round(CPU* c, u64 bits, int width, int rm, int exact); /* FRINT*; exact: FRINTX's inexact flag */
u64 snx_fp_round_int(CPU* c, u64 bits, int width, int rm, int intsize); /* FRINT32*, FRINT64* (intsize 32, 64) */
u64 snx_fp_jcvt(CPU* c, u64 bits); /* FJCVTZS: the W result, and Z in bit 32 */
u64 snx_fp_bfcvt(CPU* c, u64 bits); /* BFCVT: a single's bfloat16 */
u32 urecpe32(u32 x);
u32 ursqrte32(u32 x);

/* ---- AdvSIMD helpers out of line (helpers/crypto.c, helpers/vector.c) ---- */
void v_aes(V128* d, const V128* k, int dec);         /* AESE (dec 0), AESD (1): d = round(d ^ k) */
void v_aesmc(V128* d, const V128* s, int inv);       /* AESMC (inv 0), AESIMC (1) */
void v_sha1(CPU* c, u32 op, u32 d, u32 n, u32 m);    /* op 0-3: SHA1C/P/M/SU0; 0x10, 0x11: SHA1H, SHA1SU1 */
void v_sha256(CPU* c, u32 op, u32 d, u32 n, u32 m);  /* op 4-6: SHA256H/H2/SU1; 0x12: SHA256SU0 */
void v_pmull(V128* d, const V128* a, const V128* b, int hi); /* PMULL 1Q (hi: PMULL2) */
/* helpers/vector.c: SRSHL, URSHL, [SU]Q[R]SHL by register (desc: esz | lanes << 2 | flags << 8, flags 1 unsigned,
   2 rounding, 4 saturating); SHA512H/H2/SU0/SU1 (op 0-3); SM3PARTW1/W2, SM3TT1A/1B/2A/2B (op 0-5, imm2);
   SM4E, SM4EKEY (op 0, 1); and BFDOT's step */
void snx_vec_shl(CPU* c, u32 d, u32 n, u32 m, u32 desc);
void snx_vec_sha512(CPU* c, u32 op, u32 d, u32 n, u32 m);
void snx_vec_sm3(CPU* c, u32 op, u32 d, u32 n, u32 m, u32 imm2);
void snx_vec_sm4(CPU* c, u32 op, u32 d, u32 n, u32 m);
u32 snx_vec_bfdot(CPU* c, u32 sum, u32 e1, u32 e2); /* BFDOT's step: sum plus the two bfloat16 products */

/* DC ZVA: DCZID_EL0 gives 64-byte blocks */
SNX_INLINE void dc_zva(u8* M, u64 a) { memset(MP(a & ~(u64)63), 0, 64); }

/* ---- exclusives and ordering ----
   Load-exclusive, load-acquire and store-release are sequentially consistent host atomics of the access size
   (LDACQ), so no later access moves above an acquire load and no earlier one below a release store, which
   plain or volatile accesses don't guarantee once the compiler reorders them. A DMB or DSB of the loads type is an
   acquire fence, of the stores type a release fence, and every other one and ISB a seq_cst fence (ir_int.c's
   barriers). Nothing else orders guest memory, so plain guest loads and
   stores stay plain host ones, on x86-64, AArch64 and PowerPC64 hosts alike.
   The monitor is value based: a store-exclusive succeeds when the monitor is armed for the same address and size
   and a compare-and-swap from the value the load read succeeds. It is cleared after each store-exclusive. Status
   0 is success. Unlike hardware it sees no ABA and ordinary stores don't clear it (docs/DIVERGENCES.md). */
/* Guest memory holds whatever the guest stored last, at any width: the helpers' typed accesses to it go through
   these, which may alias every other access as a char's would, so none is assumed apart from another of a different
   type once the helpers are inlined into each other */
typedef u16 __attribute__((may_alias)) snx_guest_u16;
typedef u32 __attribute__((may_alias)) snx_guest_u32;
typedef u64 __attribute__((may_alias)) snx_guest_u64;
SNX_INLINE u32 snx_cas64(snx_guest_u64* p, u64 expected, u64 desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
SNX_INLINE u32 snx_cas32(snx_guest_u32* p, u32 expected, u32 desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
SNX_INLINE u32 snx_cas16(snx_guest_u16* p, u16 expected, u16 desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
SNX_INLINE u32 snx_cas8(u8* p, u8 expected, u8 desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
/* one atomic 16-byte compare-and-swap of both halves (p[0] the low one): cmpxchg16b on x86-64, lqarx/stqcx. on
   POWER8, c->rt->cas128 on RISC-V, LoongArch and PowerPC64 builds for an older baseline */
u32 snx_cas128(CPU* c, u64* p, u64 o0, u64 o1, u64 n0, u64 n1);
/* ABI 3 made these and snx_cas128 atomic, so modules from before are refused */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
/* the T's byte-swapped, as one access (T of 1, 2, 4 or 8 bytes) */
#define SNX_GUEST_T(T, v)                                                                                             \
    ((T)(sizeof(T) == 2   ? __builtin_bswap16((u16)(v))                                                              \
         : sizeof(T) == 4 ? __builtin_bswap32((u32)(v))                                                              \
         : sizeof(T) == 8 ? __builtin_bswap64((u64)(v))                                                              \
                          : (v)))
#define LDACQ(T, a)    SNX_GUEST_T(T, __atomic_load_n((snx_guest_##T*)MP(a), __ATOMIC_SEQ_CST))
#else
#define LDACQ(T, a)    __atomic_load_n((snx_guest_##T*)MP(a), __ATOMIC_SEQ_CST)
#endif
/* LDXR/LDAXR of 1 << sz bytes */
SNX_INLINE u64 ldx(CPU* c, u8* M, u64 a, u32 sz) {
    u64 v;
    if (sz == 0)
        v = __atomic_load_n(MP(a), __ATOMIC_SEQ_CST);
    else if (sz == 1)
        v = LDACQ(u16, a);
    else if (sz == 2)
        v = LDACQ(u32, a);
    else
        v = LDACQ(u64, a);
    c->excl_addr = a;
    c->excl_val = v;
    c->excl_size = sz;
    c->excl_on = 1;
    return v;
}
/* STXR/STLXR: the status, 0 when stored. The monitor's range must cover the store's, as the console allows a
   store-exclusive at a smaller size than the load that armed it: games deliberately pair a wide load-exclusive with a
   narrow store-exclusive to change part of a wider counter in place, and requiring the sizes to be equal made such a
   pair never store, so the game's retry loop spun on it for good. */
SNX_INLINE u32 stx(CPU* c, u8* M, u64 a, u32 sz, u64 v) {
    u32 stored = 0;
    /* the size bits and a pair's flag, which a pair's load sets and its store names too: masked off on both sides,
       so a pair (0x13) covers a single (0x03) and a single inside a wider single succeeds */
    u64 armed = 8ull << (c->excl_size & 3), this = 8ull << (sz & 3);
    if (c->excl_on && c->excl_addr == a && this <= armed) {
        if (sz == 0)
            stored = snx_cas8((u8*)MP(a), (u8)c->excl_val, (u8)v);
        else if (sz == 1)
            stored = snx_cas16((snx_guest_u16*)MP(a), SNX_GUEST16((u16)c->excl_val), SNX_GUEST16((u16)v));
        else if (sz == 2)
            stored = snx_cas32((snx_guest_u32*)MP(a), SNX_GUEST32((u32)c->excl_val), SNX_GUEST32((u32)v));
        else
            stored = snx_cas64((snx_guest_u64*)MP(a), SNX_GUEST64(c->excl_val), SNX_GUEST64(v));
    }
    c->excl_on = 0;
    return !stored;
}
/* LDXP/LDAXP: a pair of words is one 64-bit access (first register in the low word), a pair of doublewords two */
SNX_INLINE void ldxp(CPU* c, u8* M, u64 a, u32 sz, u64* v1, u64* v2) {
    if (sz == 2) {
        u64 both = LDACQ(u64, a);
        *v1 = (u32)both;
        *v2 = both >> 32;
        c->excl_val = both;
        c->excl_val2 = 0;
    } else {
        *v1 = LDACQ(u64, a);
        *v2 = LDACQ(u64, a + 8);
        c->excl_val = *v1;
        c->excl_val2 = *v2;
    }
    c->excl_addr = a;
    c->excl_size = sz | 0x10;
    c->excl_on = 1;
}
/* STXP/STLXP: one 64-bit or one 16-byte compare-and-swap */
SNX_INLINE u32 stxp(CPU* c, u8* M, u64 a, u32 sz, u64 v1, u64 v2) {
    u32 stored = 0;
    if (c->excl_on && c->excl_addr == a && c->excl_size == (sz | 0x10)) {
        if (sz == 2)
            stored = snx_cas64((snx_guest_u64*)MP(a), SNX_GUEST64(c->excl_val),
                               SNX_GUEST64((u64)(u32)v1 | ((u64)v2 << 32)));
        else
            stored = snx_cas128(c, (u64*)MP(a), c->excl_val, c->excl_val2, v1, v2);
    }
    c->excl_on = 0;
    return !stored;
}

/* ---- system registers EL0 reaches, by o0:op1:CRn:CRm:op2 (the MRS/MSR word's bits 19:5) ---- */
u64 snx_mrs(CPU* c, u32 sysreg);
void snx_msr(CPU* c, u32 sysreg, u64 v);
/* IC IVAU Xt: the instruction cache line at Xt invalidated, for code the guest wrote there */
void snx_ic_ivau(CPU* c, u64 a);

/* FMOV (immediate): VFPExpandImm of imm8 = a:b:cdefgh to a single or double */
SNX_INLINE u32 vfp_imm32(u32 i) {
    u32 b = (i >> 6) & 1;
    u32 exponent = ((b ^ 1) << 7) | (b ? 0x7Cu : 0) | ((i >> 4) & 3); /* NOT(b):Replicate(b,5):cd */
    return ((i >> 7) & 1) << 31 | exponent << 23 | (i & 15) << 19;
}
SNX_INLINE u64 vfp_imm64(u32 i) {
    u64 b = (i >> 6) & 1;
    u64 exponent = ((b ^ 1) << 10) | (b ? 0x3FCu : 0) | ((i >> 4) & 3); /* NOT(b):Replicate(b,8):cd */
    return (u64)((i >> 7) & 1) << 63 | exponent << 52 | (u64)(i & 15) << 48;
}
#endif
