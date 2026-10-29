/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/host_shaders/astc_decoder.comp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project */
/* ASTC decoding: the body of astc.comp, which declares the push constants (width, height, blocks_x, blocks_y, srgb,
   tile_shift), the footprint (block_dims, specialization constants), the buffers (astc_data, texels) and TILE(field,
   slot), the workgroup's shared memory.

   A workgroup of 64 invocations decodes a tile of 64 blocks, tile_w = 1 << tile_shift wide and 64 / tile_w high, in
   two phases. In the first each invocation decodes one block (DecodeBlock): the decoding is serial by nature, and
   one block per invocation runs it once per block. Its result goes to the block's slot of TILE: the decoded weights,
   the partitions' endpoints and the few numbers that place a texel in the weight grid and the partitions. After a
   barrier the 64 invocations write the tile's texels together (WriteTexels), a row of 64 neighbors at a time: each
   store of the workgroup is contiguous memory, the texel code has no branch, so invocations never diverge, and a
   texture of few blocks still keeps all 64 busy. Invocation per block and texel loops per invocation would give
   none of these: each store of a subgroup lands a footprint apart, every invocation takes its own block's branches,
   and a small texture's few invocations run its texels one after another.

   The file is written in the part of GLSL that is also C++, so the tests compile it unchanged on the CPU
   (tests/glsl/glsl.hpp has the rules): constants that label cases are macros, there are no out or inout
   parameters, arrays are never assigned or passed, and vectors are read one component at a time.

   It is written for what the machine runs, not for reading: some mobile drivers neither inline across functions
   nor fold, unroll or strength-reduce, so the source does that itself. Nothing indexes a private array (drivers
   put such arrays in scratch memory): the per-block tables are constants read with a shift and a mask, the decoded
   values live in shared memory, and the partitions' endpoints in registers chosen with selects. Bit replication
   is a multiplication, loops that only walked a fixed handful of fields are written out, and the texel loop does
   the work of two channels in each 32 bit operation. Every shortcut gives the bytes the CPU reference
   (tests/support/astc.c) gives; docs/ASTC.md lists them with how each was checked. */

/* the integer sequence encodings */
#define JUST_BITS 0u
#define QUINT 1u
#define TRIT 2u

/* the LDR profile's error color, opaque magenta (R in the low byte): illegal blocks, HDR void extents and the
   texels of HDR partitions. Eden's is transparent black */
#define ERROR_COLOR 0xFFFF00FFu

/* the block, and the bit stream being decoded from it, consumed from the low end of x */
uvec4 blk;
uvec4 s;

/* the word of decoded values being filled (Emit) */
uint acc;

/* the unquantization of the sequence being decoded, set per block by ColorParams or WeightParams: bits are
   replicated by a multiplication (jm, js), trits and quints by the specification's formula with its constants
   (c, and B as a multiplication: mb, sb, bmask), A's mask and its kept bit (amask, ahi), and whether values above
   32 go one higher (adj, weights) */
uint q_jm, q_js, q_c, q_mb, q_sb, q_bmask, q_amask, q_ahi, q_adj;

/* A block's slot in TILE, a word per field, each field the 64 slots side by side (so that the first phase's
   invocations keep to their own banks) */
#define F_VALUES 0u    /* 24 words of decoded values, a byte each: color values, then weights */
#define F_E0 24u       /* 4 words: each partition's low endpoint, RGBA with R in the low byte */
#define F_E1 28u       /* 4 words: the high ones */
#define F_INFO 32u     /* the weight grid's width N | height M << 4 | dual plane << 8 | its channel << 9 */
#define F_STEP_AB 33u  /* the partition hash: scores a and b per texel to the right, in 16 bit lanes */
#define F_RSTEP_AB 34u /* per row down */
#define F_ROW_AB 35u   /* at the block's corner */
#define F_STEP_CD 36u  /* and c and d, 0 in the lanes of partitions the block doesn't have */
#define F_RSTEP_CD 37u
#define F_ROW_CD 38u
#define VALUE_WORD(i) TILE(i, gl_LocalInvocationIndex)

uint Bits(uint v, uint off, uint n) {
    return bitfieldExtract(v, int(off), int(n));
}

/* the stream shifted down by n < 32 bits: (y << 1) << (31 - n) is y << (32 - n) without a shift by 32 at n 0 */
void Skip(uint n) {
    s.x = (s.x >> n) | ((s.y << 1u) << (31u - n));
    s.y = (s.y >> n) | ((s.z << 1u) << (31u - n));
    s.z = (s.z >> n) | ((s.w << 1u) << (31u - n));
    s.w >>= n;
}

/* the stream with only its low n bits: the rest read as zero */
void KeepLow(uint n) {
    s.x &= Bits(0xFFFFFFFFu, 0u, min(n, 32u));
    s.y &= Bits(0xFFFFFFFFu, 0u, uint(clamp(int(n) - 32, 0, 32)));
    s.z &= Bits(0xFFFFFFFFu, 0u, uint(clamp(int(n) - 64, 0, 32)));
    s.w &= Bits(0xFFFFFFFFu, 0u, uint(clamp(int(n) - 96, 0, 32)));
}

/* n <= 8 bits of the block at pos */
uint BlockBits(uint pos, uint n) {
    const uint wi = pos >> 5u, sh = pos & 31u;
    const uint lo = wi == 0u ? blk.x : wi == 1u ? blk.y : wi == 2u ? blk.z : blk.w;
    const uint hi = wi == 0u ? blk.y : wi == 1u ? blk.z : wi == 2u ? blk.w : 0u;
    return Bits((lo >> sh) | ((hi << 1u) << (31u - sh)), 0u, n);
}

/* the 4 decoded values of a slot from value at on, from two words of shared memory */
uint Read32At(uint slot, uint at) {
    const uint k = at >> 2u, sh = (at & 3u) * 8u;
    return (TILE(F_VALUES + k, slot) >> sh) | ((TILE(F_VALUES + k + 1u, slot) << 1u) << (31u - sh));
}

/* The encodings in ascending order of their bit length: the kind (2 bits each) and the bit count (4 bits each) of
   index i, from constants. Index 0 to 21: bits 0, bits 1, trits 0, bits 2, quints 0, trits 1, then bits, quints and
   trits of one more bit each */
uint EncKind(uint i) {
    return Bits(i < 16u ? 0x24924920u : 0x249u, (i & 15u) * 2u, 2u);
}
uint EncBits(uint i) {
    return Bits(i < 8u ? 0x13102010u : i < 16u ? 0x64353242u : 0x865754u, (i & 7u) * 4u, 4u);
}
/* the bits that n values of encoding i take: trits are 8 bits for 5, quints 7 for 3, rounded up */
uint BitLength(uint n, uint i) {
    const uint kind = EncKind(i);
    const uint extra = kind == TRIT ? ((n * 8u + 4u) * 0x3334u) >> 16u : kind == QUINT ? ((n * 7u + 2u) * 0x5556u) >> 16u : 0u;
    return EncBits(i) * n + extra;
}

/* color values to 8 bits: b bits replicated as (v * jm) >> js, or the trit or quint formula with constant c and
   B = (((v >> 1) & bmask) * mb) >> sb, from tables by bit count */
void ColorParams(uint kind, uint b) {
    const uint i = b - 1u;
    q_bmask = ((1u << b) - 1u) >> 1u;
    q_amask = 0x1FFu;
    q_ahi = 0x80u;
    q_adj = 0u;
    q_jm = Bits(b <= 4u ? 0x114955FFu : 0x01814121u, (i & 3u) * 8u, 8u);
    q_js = Bits(0x06420100u, (i & 7u) * 4u, 4u);
    if (kind == TRIT) {
        q_c = Bits(b <= 4u ? 0x162C5DCCu : 0x050Bu, (i & 3u) * 8u, 8u);
        q_mb = Bits(b <= 2u ? 0x01160000u : b <= 4u ? 0x00410085u : 0x01010081u, (i & 1u) * 16u, 16u);
        q_sb = Bits(0x420000u, (i & 7u) * 4u, 4u);
    } else {
        q_c = Bits(b <= 4u ? 0x0D1A3671u : 0x06u, (i & 3u) * 8u, 8u);
        q_mb = Bits(b <= 2u ? 0x010C0000u : b <= 4u ? 0x00810105u : 0x0101u, (i & 1u) * 16u, 16u);
        q_sb = Bits(0x31100u, (i & 7u) * 4u, 4u);
    }
}

/* weights to [0, 64]: bits replicated to 6, or the trit or quint formula (0 bits: c is 128 or 64, giving 32 or 16
   steps, which stay as they are), then values above 32 one higher */
void WeightParams(uint kind, uint b) {
    q_bmask = ((1u << b) - 1u) >> 1u;
    q_amask = 0x7Fu;
    q_ahi = 0x20u;
    q_sb = 0u;
    q_jm = Bits(b <= 4u ? 0x1109153Fu : 0x21u, ((b - 1u) & 3u) * 8u, 8u);
    q_js = Bits(0x42000u, ((b - 1u) & 7u) * 4u, 4u);
    q_adj = kind == JUST_BITS || b != 0u ? 1u : 0u;
    q_c = Bits(kind == TRIT ? 0x0B173280u : 0x0D1C40u, (b & 3u) * 8u, 8u);
    q_mb = Bits(kind == TRIT ? 0x21450000u : 0x420000u, (b & 3u) * 8u, 8u);
}

uint UnquantizeBits(uint v) {
    const uint r = (v * q_jm) >> q_js;
    return r + (q_adj & ((r + 31u) >> 6u));
}

uint UnquantizeTQ(uint v, uint d) {
    const uint A = (0u - (v & 1u)) & q_amask;
    const uint B = (((v >> 1u) & q_bmask) * q_mb) >> q_sb;
    const uint r = (A & q_ahi) | (((d * q_c + B) ^ A) >> 2u);
    return r + (q_adj & ((r + 31u) >> 6u));
}

/* value k, one byte each: the word is written whole every time, so nothing is cleared or read back first */
void Emit(uint k, uint r) {
    acc = (k & 3u) == 0u ? r : acc | (r << ((k & 3u) * 8u));
    VALUE_WORD(k >> 2u) = acc;
}

/* count values of b bit kind from the stream, unquantized, into VALUE_WORD. A trit or quint block's values past
   count are stored too (from zero bits): nothing reads them */
void DecodeSequence(uint kind, uint b, uint count) {
    if (kind == JUST_BITS) {
        /* as many values as 31 bits hold, from one word, then one shift */
        const uint group = Bits(b <= 4u ? 0x070A0F1Fu : 0x03040506u, ((b - 1u) & 3u) * 8u, 8u);
        for (uint k = 0u; k < count;) {
            const uint word = s.x;
            const uint end = min(count, k + group);
            for (uint off = 0u; k < end; k++, off += b) {
                Emit(k, UnquantizeBits(Bits(word, off, b)));
            }
            Skip(group * b);
        }
    } else if (kind == QUINT) {
        /* 3 values in 3b + 7 <= 22 bits: m0, Q0-2, m1, Q3-4, m2, Q5-6 */
        for (uint k = 0u; k < count; k += 3u) {
            const uint w = s.x;
            const uint Q = Bits(w, b, 3u) | (Bits(w, 2u * b + 3u, 2u) << 3u) | (Bits(w, 3u * b + 5u, 2u) << 5u);
            const bool high = Bits(Q, 1u, 2u) == 3u;
            const bool both4 = high && Bits(Q, 5u, 2u) == 0u;
            const uint C = high ? (Bits(Q, 3u, 2u) << 3u) | ((~Bits(Q, 5u, 2u) & 3u) << 1u) | Bits(Q, 0u, 1u)
                                : Bits(Q, 0u, 5u);
            const bool five = Bits(C, 0u, 3u) == 5u;
            const uint q0 = both4 ? 4u : five ? Bits(C, 3u, 2u) : Bits(C, 0u, 3u);
            const uint q1 = both4 || five ? 4u : Bits(C, 3u, 2u);
            const uint q2 = both4 ? (Bits(Q, 0u, 1u) << 2u) | ((Bits(Q, 4u, 1u) & ~Bits(Q, 0u, 1u)) << 1u) |
                                        (Bits(Q, 3u, 1u) & ~Bits(Q, 0u, 1u))
                            : high ? 4u
                                   : Bits(Q, 5u, 2u);
            Emit(k, UnquantizeTQ(Bits(w, 0u, b), q0));
            Emit(k + 1u, UnquantizeTQ(Bits(w, b + 3u, b), q1));
            Emit(k + 2u, UnquantizeTQ(Bits(w, 2u * b + 5u, b), q2));
            Skip(3u * b + 7u);
        }
    } else {
        /* 5 values in 5b + 8 <= 38 bits, read in two parts that each fit a word: m0, T0-1, m1, T2-3, m2 (3b + 4
           bits), then T4, m3, T5-6, m4, T7 (2b + 4) */
        for (uint k = 0u; k < count; k += 5u) {
            uint w = s.x;
            const uint m0 = Bits(w, 0u, b), m1 = Bits(w, b + 2u, b), m2 = Bits(w, 2u * b + 4u, b);
            uint T = Bits(w, b, 2u) | (Bits(w, 2u * b + 2u, 2u) << 2u);
            Skip(3u * b + 4u);
            w = s.x;
            T |= (Bits(w, 0u, 1u) << 4u) | (Bits(w, b + 1u, 2u) << 5u) | (Bits(w, 2u * b + 3u, 1u) << 7u);
            const uint m3 = Bits(w, 1u, b), m4 = Bits(w, b + 3u, b);
            Skip(2u * b + 4u);
            const bool c7 = Bits(T, 2u, 3u) == 7u;
            const bool h3 = Bits(T, 5u, 2u) == 3u;
            const uint C = c7 ? (Bits(T, 5u, 3u) << 2u) | Bits(T, 0u, 2u) : Bits(T, 0u, 5u);
            const uint t4 = c7 || h3 ? 2u : Bits(T, 7u, 1u);
            const uint t3 = c7 ? 2u : h3 ? Bits(T, 7u, 1u) : Bits(T, 5u, 2u);
            const bool l3 = Bits(C, 0u, 2u) == 3u;
            const bool m3b = Bits(C, 2u, 2u) == 3u;
            const uint t2 = l3 || m3b ? 2u : Bits(C, 4u, 1u);
            const uint t1 = l3 ? Bits(C, 4u, 1u) : m3b ? 2u : Bits(C, 2u, 2u);
            const uint t0 = l3    ? (Bits(C, 3u, 1u) << 1u) | (Bits(C, 2u, 1u) & ~Bits(C, 3u, 1u))
                            : m3b ? Bits(C, 0u, 2u)
                                  : (Bits(C, 1u, 1u) << 1u) | (Bits(C, 0u, 1u) & ~Bits(C, 1u, 1u));
            Emit(k, UnquantizeTQ(m0, t0));
            Emit(k + 1u, UnquantizeTQ(m1, t1));
            Emit(k + 2u, UnquantizeTQ(m2, t2));
            Emit(k + 3u, UnquantizeTQ(m3, t3));
            Emit(k + 4u, UnquantizeTQ(m4, t4));
        }
    }
}

uint Hash52(uint p) {
    p ^= p >> 15u;
    p -= p << 17u;
    p += p << 7u;
    p += p << 4u;
    p ^= p >> 5u;
    p += p << 16u;
    p ^= p >> 7u;
    p ^= p >> 3u;
    p ^= p << 6u;
    p ^= p >> 17u;
    return p;
}

uint PackRGBA(uint r, uint g, uint b, uint a) {
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}
uint PackClamped(int r, int g, int b, int a) {
    return PackRGBA(uint(clamp(r, 0, 255)), uint(clamp(g, 0, 255)), uint(clamp(b, 0, 255)), uint(clamp(a, 0, 255)));
}

/* the two endpoints of a partition with endpoint mode cem, from its values at off; HDR modes (2, 3, 7, 11, 14 and
   15, the bits of 0xC88C) are the error color, as in the LDR profile, which interpolates to itself */
uvec2 ComputeEndpoints(uint cem, uint off) {
    if (((0xC88Cu >> cem) & 1u) != 0u) {
        return uvec2(ERROR_COLOR, ERROR_COLOR);
    }
    const uint qa = Read32At(gl_LocalInvocationIndex, off), qb = Read32At(gl_LocalInvocationIndex, off + 4u);
    const uint v0 = Bits(qa, 0u, 8u), v1 = Bits(qa, 8u, 8u), v2 = Bits(qa, 16u, 8u), v3 = qa >> 24u;
    const uint v4 = Bits(qb, 0u, 8u), v5 = Bits(qb, 8u, 8u), v6 = Bits(qb, 16u, 8u), v7 = qb >> 24u;
    /* the signed modes' bit transfer: the second value's low 6 bits above its first, signed, and the first value's
       top bit moved into the second */
    const int s1 = bitfieldExtract(int(v1), 1, 6), s3 = bitfieldExtract(int(v3), 1, 6);
    const int s5 = bitfieldExtract(int(v5), 1, 6), s7 = bitfieldExtract(int(v7), 1, 6);
    const int s0 = int((v0 >> 1u) | (v1 & 0x80u)), s2 = int((v2 >> 1u) | (v3 & 0x80u));
    const int s4 = int((v4 >> 1u) | (v5 & 0x80u)), s6 = int((v6 >> 1u) | (v7 & 0x80u));
    switch (cem) {
    case 0u:
        return uvec2(v0 * 0x10101u | 0xFF000000u, v1 * 0x10101u | 0xFF000000u);
    case 1u: {
        const uint L0 = (v0 >> 2u) | (v1 & 0xC0u);
        const uint L1 = min(L0 + (v1 & 0x3Fu), 0xFFu);
        return uvec2(L0 * 0x10101u | 0xFF000000u, L1 * 0x10101u | 0xFF000000u);
    }
    case 4u:
        return uvec2(v0 * 0x10101u | (v2 << 24u), v1 * 0x10101u | (v3 << 24u));
    case 5u:
        return uvec2(PackClamped(s0, s0, s0, s2), PackClamped(s0 + s1, s0 + s1, s0 + s1, s2 + s3));
    case 6u:
        return uvec2(PackRGBA((v0 * v3) >> 8u, (v1 * v3) >> 8u, (v2 * v3) >> 8u, 0xFFu), PackRGBA(v0, v1, v2, 0xFFu));
    case 8u:
        if (v1 + v3 + v5 >= v0 + v2 + v4) {
            return uvec2(PackRGBA(v0, v2, v4, 0xFFu), PackRGBA(v1, v3, v5, 0xFFu));
        }
        return uvec2(PackRGBA((v1 + v5) >> 1u, (v3 + v5) >> 1u, v5, 0xFFu),
                     PackRGBA((v0 + v4) >> 1u, (v2 + v4) >> 1u, v4, 0xFFu));
    case 9u:
        if (s1 + s3 + s5 >= 0) {
            return uvec2(PackClamped(s0, s2, s4, 0xFF), PackClamped(s0 + s1, s2 + s3, s4 + s5, 0xFF));
        }
        return uvec2(PackClamped((s0 + s1 + s4 + s5) >> 1, (s2 + s3 + s4 + s5) >> 1, s4 + s5, 0xFF),
                     PackClamped((s0 + s4) >> 1, (s2 + s4) >> 1, s4, 0xFF));
    case 10u:
        return uvec2(PackRGBA((v0 * v3) >> 8u, (v1 * v3) >> 8u, (v2 * v3) >> 8u, v4), PackRGBA(v0, v1, v2, v5));
    case 12u:
        if (v1 + v3 + v5 >= v0 + v2 + v4) {
            return uvec2(PackRGBA(v0, v2, v4, v6), PackRGBA(v1, v3, v5, v7));
        }
        return uvec2(PackRGBA((v1 + v5) >> 1u, (v3 + v5) >> 1u, v5, v7),
                     PackRGBA((v0 + v4) >> 1u, (v2 + v4) >> 1u, v4, v6));
    default: /* 13 */
        if (s1 + s3 + s5 >= 0) {
            return uvec2(PackClamped(s0, s2, s4, s6), PackClamped(s0 + s1, s2 + s3, s4 + s5, s6 + s7));
        }
        return uvec2(PackClamped((s0 + s1 + s4 + s5) >> 1, (s2 + s3 + s4 + s5) >> 1, s4 + s5, s6 + s7),
                     PackClamped((s0 + s4) >> 1, (s2 + s4) >> 1, s4, s6));
    }
}

/* a UNORM16 value to 8 bits, rounded to nearest, as storing value / 65535 into an RGBA8 image does in Eden:
   (c * 255 + 32767) / 65535 without the division, the same for every c up to 65535 */
uint Unorm16To8(uint c) {
    return (c * 255u + 32895u) >> 16u;
}

/* a block of one color (a void extent, or the error color): one partition whose endpoints are both the color, which
   interpolates to itself whatever the weights read */
void SetSolid(uint rgba) {
    const uint slot = gl_LocalInvocationIndex;
    TILE(F_INFO, slot) = 2u | (2u << 4u);
    TILE(F_E0, slot) = rgba;
    TILE(F_E1, slot) = rgba;
    TILE(F_STEP_AB, slot) = 0u;
    TILE(F_RSTEP_AB, slot) = 0u;
    TILE(F_ROW_AB, slot) = 0u;
    TILE(F_STEP_CD, slot) = 0u;
    TILE(F_RSTEP_CD, slot) = 0u;
    TILE(F_ROW_CD, slot) = 0u;
}

/* the first phase: this invocation's block (blk) into its slot */
void DecodeBlock() {
    const uint mode = Bits(blk.x, 0u, 11u);
    /* void extents: the extent's coordinates are not needed, but unless they are all ones the specification
       requires each minimum below its maximum, which Eden does not check */
    if ((mode & 0x1FFu) == 0x1FCu) {
        const uint s_min = Bits(blk.x, 12u, 13u), s_max = (blk.x >> 25u) | (Bits(blk.y, 0u, 6u) << 7u);
        const uint t_min = Bits(blk.y, 6u, 13u), t_max = blk.y >> 19u;
        const bool hdr_or_reserved = (mode & 0x200u) != 0u || (mode & 0x400u) == 0u || Bits(blk.x, 11u, 1u) == 0u;
        if (hdr_or_reserved || ((s_min & s_max & t_min & t_max) != 0x1FFFu && (s_min >= s_max || t_min >= t_max))) {
            SetSolid(ERROR_COLOR);
            return;
        }
        const uint r = blk.z & 0xFFFFu, g = blk.z >> 16u, b = blk.w & 0xFFFFu;
        /* sRGB decode mode: the top 8 bits of the color */
        const uint rgb = srgb != 0u ? PackRGBA(r >> 8u, g >> 8u, b >> 8u, 0u)
                                    : PackRGBA(Unorm16To8(r), Unorm16To8(g), Unorm16To8(b), 0u);
        SetSolid(rgb | (Unorm16To8(blk.w >> 16u) << 24u));
        return;
    }
    /* reserved modes */
    if ((mode & 0xFu) == 0u || ((mode & 3u) == 0u && (mode & 0x1C0u) == 0x1C0u)) {
        SetSolid(ERROR_COLOR);
        return;
    }
    /* the layout (0 to 9, the specification's table of block modes) from a nibble table on the bits that tell
       them apart, and the weight grid's size from tables of each layout's field shift ((sh - 5) / 2), width in
       bits and half its addend */
    const bool low = (mode & 3u) != 0u;
    const uint mode_layout = low ? Bits(0x42103210u, (Bits(mode, 2u, 2u) | (Bits(mode, 8u, 1u) << 2u)) * 4u, 4u)
                            : Bits(0x87996655u,
                                   (Bits(mode, 5u, 1u) | (Bits(mode, 7u, 1u) << 1u) | (Bits(mode, 8u, 1u) << 2u)) * 4u, 4u);
    const uint N = Bits(mode, 5u + 2u * Bits(0x105u, mode_layout * 2u, 2u), Bits(0x821AAu, mode_layout * 2u, 2u)) +
                   2u * Bits(0x1D671262u, mode_layout * 3u, 3u);
    const uint M = Bits(mode, 5u + 2u * Bits(0x80050u, mode_layout * 2u, 2u), Bits(0x80A6Au, mode_layout * 2u, 2u)) +
                   2u * Bits(0x1BB89709u, mode_layout * 3u, 3u);
    const bool dual_plane = mode_layout != 9u && (mode & 0x400u) != 0u;
    const uint np = Bits(blk.x, 11u, 2u) + 1u;
    const uint num_weights = N * M * (dual_plane ? 2u : 1u);
    const uint max_weight = (Bits(mode, 4u, 1u) | (low ? (mode & 3u) << 1u : (mode & 0xCu) >> 1u)) - 1u +
                            (mode_layout != 9u && (mode & 0x200u) != 0u ? 6u : 0u);
    const uint weight_kind = EncKind(max_weight), weight_b = EncBits(max_weight);
    const uint weight_bits = BitLength(num_weights, max_weight);
    const uint header = np == 1u ? 17u : 29u;
    const uint base_cem = np == 1u ? 0u : Bits(blk.x, 23u, 6u);
    const uint base_mode = base_cem & 3u;
    const uint extra_cem_bits = base_mode > 0u ? np * 3u - 4u : 0u; /* 2, 5 or 8 */
    const uint plane_bits = dual_plane ? 2u : 0u;
    const int remaining = 128 - int(weight_bits + header + extra_cem_bits + plane_bits);
    if (N > block_dims.x || M > block_dims.y || (np == 4u && dual_plane) || num_weights > 64u || weight_bits < 24u ||
        weight_bits > 96u || remaining < 0) {
        SetSolid(ERROR_COLOR);
        return;
    }
    const uint color_bits = uint(remaining);
    const uint plane_c = BlockBits(header + color_bits, plane_bits);
    /* the partitions' endpoint modes, a nibble each */
    uint cems;
    if (np == 1u) {
        cems = Bits(blk.x, 13u, 4u);
    } else if (base_mode == 0u) {
        cems = (base_cem >> 2u) * 0x1111u;
    } else {
        /* a bit per partition (one class higher), then 2 bits each */
        const uint cemx = ((BlockBits(header + color_bits + plane_bits, extra_cem_bits) << 6u) | base_cem) >> 2u;
        const uint m = cemx >> np, cls = base_mode - 1u;
        cems = (((cls + Bits(cemx, 0u, 1u)) << 2u) | Bits(m, 0u, 2u)) |
               ((((cls + Bits(cemx, 1u, 1u)) << 2u) | Bits(m, 2u, 2u)) << 4u) |
               ((((cls + Bits(cemx, 2u, 1u)) << 2u) | Bits(m, 4u, 2u)) << 8u) |
               ((((cls + Bits(cemx, 3u, 1u)) << 2u) | Bits(m, 6u, 2u)) << 12u);
    }
    /* the color values, 2 per class step of each partition: the classes summed in one multiplication */
    const uint classes = (cems >> 2u) & 0x3333u & ((1u << (np * 4u)) - 1u);
    const uint num_values = (((classes * 0x1111u) >> 12u) & 0xFu) * 2u + np * 2u;
    /* the specification's illegal encodings that Eden decodes anyway: more than 18 color values, or too few bits
       for them to have at least 6 levels (trits of 1 bit) */
    if (num_values > 18u || num_values + ((num_values * 8u + 4u) * 0x3334u >> 16u) > color_bits) {
        SetSolid(ERROR_COLOR);
        return;
    }

    /* the largest encoding whose values fit: the lengths only grow with the index, so it is the number of those
       past the first that fit, each a constant times num_values plus the trits' or quints' extra bits */
    {
        const uint n = num_values, C = color_bits;
        const uint et = (n * 8u + 4u) * 0x3334u >> 16u, eq = (n * 7u + 2u) * 0x5556u >> 16u;
#define FITS(b, e) (b * n + e <= C ? 1u : 0u)
        const uint range = FITS(1u, 0u) + FITS(0u, et) + FITS(2u, 0u) + FITS(0u, eq) + FITS(1u, et) + FITS(3u, 0u) +
                           FITS(1u, eq) + FITS(2u, et) + FITS(4u, 0u) + FITS(2u, eq) + FITS(3u, et) + FITS(5u, 0u) +
                           FITS(3u, eq) + FITS(4u, et) + FITS(6u, 0u) + FITS(4u, eq) + FITS(5u, et) + FITS(7u, 0u) +
                           FITS(5u, eq) + FITS(6u, et) + FITS(8u, 0u);
#undef FITS
        /* the color data starts after the header; bits past the sequence's length read as zero (the encoder may
           leave anything there): Eden decodes the last trit or quint block's spare values from them */
        s = blk;
        Skip(header);
        KeepLow(BitLength(n, range));
        const uint kind = EncKind(range), b = EncBits(range);
        ColorParams(kind, b);
        DecodeSequence(kind, b, n);
    }
    {
        const uint c0 = Bits(cems, 0u, 4u), c1 = Bits(cems, 4u, 4u), c2 = Bits(cems, 8u, 4u), c3 = Bits(cems, 12u, 4u);
        const uint o1 = ((c0 >> 2u) + 1u) * 2u, o2 = o1 + ((c1 >> 2u) + 1u) * 2u, o3 = o2 + ((c2 >> 2u) + 1u) * 2u;
        const uint slot = gl_LocalInvocationIndex;
        uvec2 E = ComputeEndpoints(c0, 0u);
        TILE(F_E0, slot) = E.x;
        TILE(F_E1, slot) = E.y;
        if (np > 1u) {
            E = ComputeEndpoints(c1, o1);
            TILE(F_E0 + 1u, slot) = E.x;
            TILE(F_E1 + 1u, slot) = E.y;
        }
        if (np > 2u) {
            E = ComputeEndpoints(c2, o2);
            TILE(F_E0 + 2u, slot) = E.x;
            TILE(F_E1 + 2u, slot) = E.y;
        }
        if (np > 3u) {
            E = ComputeEndpoints(c3, o3);
            TILE(F_E0 + 3u, slot) = E.x;
            TILE(F_E1 + 3u, slot) = E.y;
        }
    }

    /* the weights are stored from the top of the block down, bit-reversed; bits above them read as zero */
    s = uvec4(bitfieldReverse(blk.w), bitfieldReverse(blk.z), bitfieldReverse(blk.y), bitfieldReverse(blk.x));
    KeepLow(weight_bits);
    WeightParams(weight_kind, weight_b);
    DecodeSequence(weight_kind, weight_b, num_weights);

    /* The partition hash's per-block part. For each of the 4 partitions a texel's score is (mx * x + my * y + r) &
       63, the multipliers being the hash's nibbles squared and shifted down: kept in 16 bit lanes, a and b in one
       word and c and d in another, so a texel takes two multiply-adds per pair, x and y doubled in small blocks.
       The lanes of partitions the block doesn't have stay 0, which never wins: a single partition is partition 0
       with no test for it */
    const uint slot = gl_LocalInvocationIndex;
    uint step_ab = 0u, step_cd = 0u, rowstep_ab = 0u, rowstep_cd = 0u, row_ab = 0u, row_cd = 0u;
    if (np > 1u) {
        const uint seed = Bits(blk.x, 13u, 10u) + (np - 1u) * 1024u;
        const uint rnum = Hash52(seed);
        const uint sh_odd = (seed & 1u) != 0u ? ((seed & 2u) != 0u ? 4u : 5u) : (np == 3u ? 6u : 5u);
        const uint sh_even = (seed & 1u) != 0u ? (np == 3u ? 6u : 5u) : ((seed & 2u) != 0u ? 4u : 5u);
        const uint n0 = Bits(rnum, 0u, 4u), n1 = Bits(rnum, 4u, 4u), n2 = Bits(rnum, 8u, 4u), n3 = Bits(rnum, 12u, 4u);
        const uint n4 = Bits(rnum, 16u, 4u), n5 = Bits(rnum, 20u, 4u), n6 = Bits(rnum, 24u, 4u), n7 = rnum >> 28u;
        const uint scale = block_dims.x * block_dims.y < 32u ? 2u : 1u;
        /* c's lane when there are 3 partitions or more, d's when 4 */
        const uint cd_mask = np == 2u ? 0u : np == 3u ? 0xFFFFu : 0xFFFFFFFFu;
        step_ab = (((n0 * n0) >> sh_odd) | (((n2 * n2) >> sh_odd) << 16u)) * scale;
        rowstep_ab = (((n1 * n1) >> sh_even) | (((n3 * n3) >> sh_even) << 16u)) * scale;
        step_cd = ((((n4 * n4) >> sh_odd) | (((n6 * n6) >> sh_odd) << 16u)) * scale) & cd_mask;
        rowstep_cd = ((((n5 * n5) >> sh_even) | (((n7 * n7) >> sh_even) << 16u)) * scale) & cd_mask;
        row_ab = Bits(rnum, 14u, 6u) | (Bits(rnum, 10u, 6u) << 16u);
        row_cd = (Bits(rnum, 6u, 6u) | (Bits(rnum, 2u, 6u) << 16u)) & cd_mask;
    }
    TILE(F_INFO, slot) = N | (M << 4u) | ((dual_plane ? 1u : 0u) << 8u) | (plane_c << 9u);
    TILE(F_STEP_AB, slot) = step_ab;
    TILE(F_RSTEP_AB, slot) = rowstep_ab;
    TILE(F_ROW_AB, slot) = row_ab;
    TILE(F_STEP_CD, slot) = step_cd;
    TILE(F_RSTEP_CD, slot) = rowstep_cd;
    TILE(F_ROW_CD, slot) = row_cd;
}

/* v / block_dims.x and v / block_dims.y for v < 9216 (the texels of a tile of 64 of the largest blocks) as a
   multiplication: exact for every footprint (checked), and no division even where the driver wouldn't fold one */
uint DivW(uint v) {
    return (v * ((131072u + block_dims.x - 1u) / block_dims.x)) >> 17u;
}
uint DivH(uint v) {
    return (v * ((131072u + block_dims.y - 1u) / block_dims.y)) >> 17u;
}

/* The second phase: the tile's texels, invocation i taking texels i, i + 64, ... in rows of the tile, so the
   workgroup's stores are 64 neighbors each. A texel reads its block's slot and has no branch beyond the image's
   edge: one partition and solid blocks need no test (see F_STEP_CD and SetSolid), and a weight grid the block's
   size infills to its own weights exactly (checked for every footprint), so it needs no path of its own */
void WriteTexels() {
    const uint tile_texels_w = block_dims.x << tile_shift;
    const uint base_x = gl_WorkGroupID.x * tile_texels_w;
    const uint base_y = gl_WorkGroupID.y * ((64u >> tile_shift) * block_dims.y);
    const uint out_base = gl_WorkGroupID.z * height * width;
    /* the weight grid infill's steps from texel to grid coordinates, constants of the footprint */
    const uint Ds = (1024u + block_dims.x / 2u) / (block_dims.x - 1u);
    const uint Dt = (1024u + block_dims.y / 2u) / (block_dims.y - 1u);
    for (uint k = 0u; k < block_dims.x * block_dims.y; k++) {
        const uint t = gl_LocalInvocationIndex + k * 64u;
        const uint ty = DivW(t >> tile_shift);
        const uint tx = t - ty * tile_texels_w;
        const uint X = base_x + tx, Y = base_y + ty;
        if (X >= width || Y >= height) {
            continue;
        }
        const uint bxs = DivW(tx), bys = DivH(ty);
        const uint i = tx - bxs * block_dims.x, j = ty - bys * block_dims.y;
        const uint slot = (bys << tile_shift) + bxs;
        const uint info = TILE(F_INFO, slot);
        const uint N = info & 0xFu, M = Bits(info, 4u, 4u), dual = Bits(info, 8u, 1u), pc = Bits(info, 9u, 2u);

        /* the partition: the highest score, the first of equal ones */
        const uint pab = TILE(F_ROW_AB, slot) + TILE(F_RSTEP_AB, slot) * j + TILE(F_STEP_AB, slot) * i;
        const uint pcd = TILE(F_ROW_CD, slot) + TILE(F_RSTEP_CD, slot) * j + TILE(F_STEP_CD, slot) * i;
        const uint a = pab & 0x3Fu, b = (pab >> 16u) & 0x3Fu, c = pcd & 0x3Fu, d = (pcd >> 16u) & 0x3Fu;
        const uint p = a >= b && a >= c && a >= d ? 0u : b >= c && b >= d ? 1u : c >= d ? 2u : 3u;
        const uint E0 = TILE(F_E0 + p, slot), E1 = TILE(F_E1 + p, slot);

        /* the weight grid infilled bilinearly; neighbors past the grid always weigh 0, so they are read without a
           check (the slot has room for them) */
        const uint stride = 1u + dual, row_bytes = N * stride, next = 8u * stride;
        const uint gs = (Ds * (N - 1u) * i + 32u) >> 6u, gt = (Dt * (M - 1u) * j + 32u) >> 6u;
        const uint fs = gs & 0xFu, ft = gt & 0xFu;
        const uint w11 = (fs * ft + 8u) >> 4u;
        const uint w01 = fs - w11, w10 = ft - w11, w00 = 16u - fs - ft + w11;
        const uint at = (gt >> 4u) * row_bytes + (gs >> 4u) * stride;
        const uint qa = Read32At(slot, at), qb = Read32At(slot, at + row_bytes);
        const uint w = (Bits(qa, 0u, 8u) * w00 + Bits(qa, next, 8u) * w01 + Bits(qb, 0u, 8u) * w10 +
                        Bits(qb, next, 8u) * w11 + 8u) >> 4u;
        /* the second plane's weight: the odd bytes, garbage with one plane, where it is multiplied by 0 */
        const uint ws = (Bits(qa, 8u, 8u) * w00 + (qa >> 24u) * w01 + Bits(qb, 8u, 8u) * w10 + (qb >> 24u) * w11 + 8u) >>
                        4u;

        /* Interpolation, two channels a word: R and B (even) and G and A (odd) in 16 bit lanes, each lane
           e0 * 64 + 32 + (e1 - e0) * w = e0 * (64 - w) + e1 * w + 32 in [32, 16352], so the lanes never carry into
           each other and the negative differences are exact modulo 2^32. The second plane's channel adds its own
           difference times the difference of the weights */
        const uint e0e = E0 & 0x00FF00FFu, e1e = E1 & 0x00FF00FFu;
        const uint e0o = (E0 >> 8u) & 0x00FF00FFu, e1o = (E1 >> 8u) & 0x00FF00FFu;
        const uint sdw = ((Bits(E1, pc * 8u, 8u) - Bits(E0, pc * 8u, 8u)) << ((pc >> 1u) * 16u)) * (ws - w) * dual;
        const uint xe = (e0e << 6u) + 0x00200020u + (e1e - e0e) * w + ((pc & 1u) == 0u ? sdw : 0u);
        const uint xo = (e0o << 6u) + 0x00200020u + (e1o - e0o) * w + ((pc & 1u) != 0u ? sdw : 0u);
        texels[out_base + Y * width + X] = ((xe >> 6u) & 0x00FF00FFu) | ((xo << 2u) & 0xFF00FF00u);
    }
}

void main() {
    /* the first phase: this invocation's block of the tile, if the strip has it (no texel reads a slot whose block
       is past the strip's edge: its texels are past the image's) */
    const uint lane = gl_LocalInvocationIndex;
    const uint bx = (gl_WorkGroupID.x << tile_shift) + (lane & ((1u << tile_shift) - 1u));
    const uint by = gl_WorkGroupID.y * (64u >> tile_shift) + (lane >> tile_shift);
    if (bx < blocks_x && by < blocks_y) {
        blk = astc_data[(gl_WorkGroupID.z * blocks_y + by) * blocks_x + bx];
        DecodeBlock();
    }
    memoryBarrierShared();
    barrier();
    WriteTexels();
}
