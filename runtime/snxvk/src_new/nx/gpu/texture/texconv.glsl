/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The body of texconv.comp: texels of a format the host can't take as they are, converted on the GPU from linear rows
   (what blocklinear.comp made of a block linear level, or a pitch linear level's rows staged as they are) into rows
   that copy into the image. GLSL that is also C++ (tests/glsl/glsl.hpp): tests/glsl/texconv_glsl.cpp runs it against
   the CPU's conversions (tests/support/etc2.c for ETC2 and EAC, bcn.c for BCn).

   It also converts the frames vi presents from memory to RGBA8 (vk/renderer.c).

   An invocation owns whole output words: a pair of 2 byte texels, a 4 byte or RGB32F texel, four depth texels
   (whose four stencil bytes are one word) or a 4x4 block. Output rows are padded to that (vk/texconv.c gives the
   copies a bufferRowLength), so no invocation writes part of a word another one writes. Everything is integer arithmetic,
   with the CPU's results to the bit: the one float, a 24 bit depth as a float, is built from its bits. */

uint tc_bswap(uint w) {
    return (w << 24) | ((w & 0xFF00u) << 8) | ((w >> 8) & 0xFF00u) | (w >> 24);
}
/* bits h down to l of the 64 bit word hi:lo (as etc2.c's bits) */
uint tc_bits(uint hi, uint lo, uint h, uint l) {
    uint v = l >= 32u ? hi >> (l - 32u) : l == 0u ? lo : (lo >> l) | (hi << (32u - l));
    return v & ((2u << (h - l)) - 1u);
}
int tc_ext4(uint v) { return int((v << 4) | v); }
int tc_ext5(uint v) { return int((v << 3) | (v >> 2)); }
int tc_ext6(uint v) { return int((v << 2) | (v >> 4)); }
int tc_ext7(uint v) { return int((v << 1) | (v >> 6)); }
int tc_sext3(uint v) { return (v & 4u) != 0u ? int(v) - 8 : int(v); }
/* byte i (0 to 7) of the table in the words w0, w1 */
int tc_byte(uint w0, uint w1, uint i) { return int(((i < 4u ? w0 : w1) >> ((i & 3u) * 8u)) & 255u); }
uint tc_rgba(int r, int g, int b, uint a) {
    return uint(clamp(r, 0, 255)) | (uint(clamp(g, 0, 255)) << 8) | (uint(clamp(b, 0, 255)) << 16) | (a << 24);
}

/* the ETC1 modifiers a and b of each table codeword (indices 0 to 3 are a, b, -a, -b), and the T and H modes'
   distances, a byte each */
#define ETC1_A0 0x0D090502u
#define ETC1_A1 0x2F211812u
#define ETC1_B0 0x2A1D1108u
#define ETC1_B1 0xB76A503Cu
#define ETC2_D0 0x100B0603u
#define ETC2_D1 0x40292017u
/* the EAC modifiers of each table, a nibble each for indices 4 to 7; index k below 4 is -(that of k + 4) - 1. Two
   tables a word, the even one in the low half */
const uint EAC_P[8] = {0xC962E852u, 0xC531C741u, 0xA862B752u, 0xA742A763u,
                       0x97419751u, 0x96419731u, 0x92109632u, 0x86428753u};

/* pixel (x, y) of an ETC2 RGB block hi:lo as RGBA8. punch: punch-through alpha, whose differential bit is the opaque
   bit: a block without it has a transparent index 2, and its differential modifiers for index 0 are 0 */
uint etc2_rgb_pixel(uint hi, uint lo, bool punch, uint x, uint y) {
    uint i = x * 4u + y;
    bool diff = (hi & 2u) != 0u, flip = (hi & 1u) != 0u, see = punch && !diff;
    uint k = (((lo >> (16u + i)) & 1u) << 1) | ((lo >> i) & 1u);
    /* a see-through block's index 2 is transparent, except in the planar mode, which is always opaque */
    bool clear = see && k == 2u;
    uint s = flip ? uint(y >= 2u) : uint(x >= 2u);
    int r0, g0, b0;
    if (!punch && !diff) { /* individual: two 4 bit colors */
        r0 = tc_ext4((hi >> (28u - 4u * s)) & 15u);
        g0 = tc_ext4((hi >> (20u - 4u * s)) & 15u);
        b0 = tc_ext4((hi >> (12u - 4u * s)) & 15u);
    } else { /* differential, unless the second color overflows: then T, H or planar */
        uint r = (hi >> 27) & 31u, g = (hi >> 19) & 31u, b = (hi >> 11) & 31u;
        int dr = tc_sext3((hi >> 24) & 7u), dg = tc_sext3((hi >> 16) & 7u), db = tc_sext3((hi >> 8) & 7u);
        if (int(r) + dr < 0 || int(r) + dr > 31) { /* T */
            if (clear) return 0u;
            int d = tc_byte(ETC2_D0, ETC2_D1, (tc_bits(hi, lo, 35u, 34u) << 1) | (hi & 1u));
            if (k == 0u)
                return tc_rgba(tc_ext4((tc_bits(hi, lo, 60u, 59u) << 2) | tc_bits(hi, lo, 57u, 56u)),
                               tc_ext4(tc_bits(hi, lo, 55u, 52u)), tc_ext4(tc_bits(hi, lo, 51u, 48u)), 255u);
            int m = k == 1u ? d : k == 3u ? -d : 0;
            return tc_rgba(tc_ext4(tc_bits(hi, lo, 47u, 44u)) + m, tc_ext4(tc_bits(hi, lo, 43u, 40u)) + m,
                           tc_ext4(tc_bits(hi, lo, 39u, 36u)) + m, 255u);
        }
        if (int(g) + dg < 0 || int(g) + dg > 31) { /* H */
            if (clear) return 0u;
            uint r1 = tc_bits(hi, lo, 62u, 59u), g1 = (tc_bits(hi, lo, 58u, 56u) << 1) | tc_bits(hi, lo, 52u, 52u);
            uint b1 = (tc_bits(hi, lo, 51u, 51u) << 3) | tc_bits(hi, lo, 49u, 47u);
            uint r2 = tc_bits(hi, lo, 46u, 43u), g2 = tc_bits(hi, lo, 42u, 39u), b2 = tc_bits(hi, lo, 38u, 35u);
            /* the distance index's low bit is which base color is the larger */
            uint da = (tc_bits(hi, lo, 34u, 34u) << 2) | ((hi & 1u) << 1) |
                      uint(((r1 << 8) | (g1 << 4) | b1) >= ((r2 << 8) | (g2 << 4) | b2));
            int d = tc_byte(ETC2_D0, ETC2_D1, da);
            int m = (k & 1u) != 0u ? -d : d;
            if (k < 2u) return tc_rgba(tc_ext4(r1) + m, tc_ext4(g1) + m, tc_ext4(b1) + m, 255u);
            return tc_rgba(tc_ext4(r2) + m, tc_ext4(g2) + m, tc_ext4(b2) + m, 255u);
        }
        if (int(b) + db < 0 || int(b) + db > 31) { /* planar: three colors at the corners, interpolated; opaque */
            int ox = int(x), oy = int(y);
            int o0 = tc_ext6(tc_bits(hi, lo, 62u, 57u));
            int o1 = tc_ext7((tc_bits(hi, lo, 56u, 56u) << 6) | tc_bits(hi, lo, 54u, 49u));
            int o2 = tc_ext6((tc_bits(hi, lo, 48u, 48u) << 5) | (tc_bits(hi, lo, 44u, 43u) << 3) |
                             tc_bits(hi, lo, 41u, 39u));
            int h0 = tc_ext6((tc_bits(hi, lo, 38u, 34u) << 1) | (hi & 1u));
            int h1 = tc_ext7(tc_bits(hi, lo, 31u, 25u)), h2 = tc_ext6(tc_bits(hi, lo, 24u, 19u));
            int v0 = tc_ext6(tc_bits(hi, lo, 18u, 13u)), v1 = tc_ext7(tc_bits(hi, lo, 12u, 6u));
            int v2 = tc_ext6(tc_bits(hi, lo, 5u, 0u));
            return tc_rgba((ox * (h0 - o0) + oy * (v0 - o0) + 4 * o0 + 2) >> 2,
                           (ox * (h1 - o1) + oy * (v1 - o1) + 4 * o1 + 2) >> 2,
                           (ox * (h2 - o2) + oy * (v2 - o2) + 4 * o2 + 2) >> 2, 255u);
        }
        if (clear) return 0u;
        r0 = tc_ext5(uint(int(r) + (s != 0u ? dr : 0)));
        g0 = tc_ext5(uint(int(g) + (s != 0u ? dg : 0)));
        b0 = tc_ext5(uint(int(b) + (s != 0u ? db : 0)));
    }
    uint cw = s != 0u ? tc_bits(hi, lo, 36u, 34u) : tc_bits(hi, lo, 39u, 37u);
    int a = tc_byte(ETC1_A0, ETC1_A1, cw), b = tc_byte(ETC1_B0, ETC1_B1, cw);
    int m = k == 0u ? a : k == 1u ? b : k == 2u ? -a : -b;
    if (see && k == 0u) m = 0;
    return tc_rgba(r0 + m, g0 + m, b0 + m, 255u);
}

/* the modifier of pixel i of an EAC block hi:lo */
int eac_mod(uint hi, uint lo, uint i) {
    uint k = tc_bits(hi, lo, 47u - 3u * i, 45u - 3u * i), t = (hi >> 16) & 15u;
    int p = int((EAC_P[t >> 1] >> ((t & 1u) * 16u + (k & 3u) * 4u)) & 15u);
    return k < 4u ? -p - 1 : p;
}
/* pixel i's 8 bit alpha of an EAC block (ETC2_RGBA) */
uint eac_alpha(uint hi, uint lo, uint i) {
    return uint(clamp(int(hi >> 24) + eac_mod(hi, lo, i) * int((hi >> 20) & 15u), 0, 255));
}
/* pixel i of an 11 bit EAC block as 16 bit unorm or (sn) snorm */
uint eac11(uint hi, uint lo, bool sn, uint i) {
    int m = eac_mod(hi, lo, i), mult = int((hi >> 20) & 15u);
    int step = mult != 0 ? m * mult * 8 : m;
    if (!sn) {
        int v = clamp(int(hi >> 24) * 8 + 4 + step, 0, 2047);
        return (uint(v) << 5) | (uint(v) >> 6);
    }
    int base = int(hi >> 24) - ((hi >> 31) != 0u ? 256 : 0);
    if (base == -128) base = -127;
    int v = clamp(base * 8 + step, -1023, 1023);
    int a = v < 0 ? -v : v;
    int e = (a << 5) | (a >> 5);
    return uint(v < 0 ? -e : e) & 0xFFFFu;
}

/* ---- BC1 to BC7: a block's words little-endian, pixel (x, y) number y * 4 + x ---- */

/* n bits (below 32) of the 128 bit block b from bit at on */
uint bc_bits(uint b[4], uint at, uint n) {
    uint w = at >> 5, s = at & 31u, v = b[w] >> s;
    if (s != 0u && w < 3u) v |= b[w + 1u] << (32u - s);
    return v & ((1u << n) - 1u);
}
/* an endpoint of bits bits (5 to 8) to 8 bits, its top bits repeated below it */
int bc_expand(uint v, uint bits) { return int((v << (8u - bits)) | (v >> (2u * bits - 8u))); }

/* pixel i of a BC1 color block (endpoints e, indices idx) as RGBA8. three: a block whose first endpoint isn't the
   larger has three colors and a transparent black (BC1); BC2 and BC3 take every block as four colors */
uint bc1_pixel(uint e, uint idx, bool three, uint i) {
    uint e0 = e & 0xFFFFu, e1 = e >> 16, k = (idx >> (2u * i)) & 3u;
    int r0 = bc_expand(e0 >> 11, 5u), g0 = bc_expand((e0 >> 5) & 63u, 6u), b0 = bc_expand(e0 & 31u, 5u);
    int r1 = bc_expand(e1 >> 11, 5u), g1 = bc_expand((e1 >> 5) & 63u, 6u), b1 = bc_expand(e1 & 31u, 5u);
    if (k == 0u) return tc_rgba(r0, g0, b0, 255u);
    if (k == 1u) return tc_rgba(r1, g1, b1, 255u);
    if (e0 > e1 || !three) {
        int wa = k == 2u ? 2 : 1, wb = 3 - wa;
        return tc_rgba((wa * r0 + wb * r1 + 1) / 3, (wa * g0 + wb * g1 + 1) / 3, (wa * b0 + wb * b1 + 1) / 3, 255u);
    }
    if (k == 3u) return 0u;
    return tc_rgba((r0 + r1 + 1) / 2, (g0 + g1 + 1) / 2, (b0 + b1 + 1) / 2, 255u);
}
/* pixel i of a BC4 block hi:lo as 16 bit unorm or (sn) snorm: the interpolant's exact value (x / d of the endpoints,
   sevenths or fifths) rounded to nearest */
uint bc4_value(uint hi, uint lo, bool sn, uint i) {
    int a0 = int(lo & 255u), a1 = int((lo >> 8) & 255u);
    if (sn) {
        a0 = max(a0 >= 128 ? a0 - 256 : a0, -127);
        a1 = max(a1 >= 128 ? a1 - 256 : a1, -127);
    }
    uint k = tc_bits(hi, lo, 18u + 3u * i, 16u + 3u * i);
    int x, d = 1;
    if (k < 2u) {
        x = k == 0u ? a0 : a1;
    } else if (a0 > a1) {
        x = int(8u - k) * a0 + int(k - 1u) * a1;
        d = 7;
    } else if (k < 6u) {
        x = int(6u - k) * a0 + int(k - 1u) * a1;
        d = 5;
    } else {
        x = k == 6u ? (sn ? -127 : 0) : (sn ? 127 : 255);
    }
    if (!sn) return uint((x * 257 + d / 2) / d);
    int m = x < 0 ? -x : x, v = (m * 32767 + 127 * d / 2) / (127 * d);
    return uint(x < 0 ? -v : v) & 0xFFFFu;
}
/* pixel i's 8 bit alpha of a BC3 alpha block hi:lo, rounded to nearest */
uint bc3_alpha(uint hi, uint lo, uint i) {
    int a0 = int(lo & 255u), a1 = int((lo >> 8) & 255u);
    uint k = tc_bits(hi, lo, 18u + 3u * i, 16u + 3u * i);
    if (k < 2u) return uint(k == 0u ? a0 : a1);
    if (a0 > a1) return uint((int(8u - k) * a0 + int(k - 1u) * a1 + 3) / 7);
    if (k < 6u) return uint((int(6u - k) * a0 + int(k - 1u) * a1 + 2) / 5);
    return k == 6u ? 0u : 255u;
}

/* BPTC's tables (tests/support/bcn.c's, packed): the subset bit of each pixel of the two subset partitions, 16 bits each; the
   subset of each pixel of the three subset ones, 2 bits each; the anchor pixel of subset 1 of the two subset
   partitions and of subsets 1 (A) and 2 (B) of the three subset ones, a nibble each */
const uint BC_PART2[32] = {
    0x8888CCCCu, 0xECC8EEEEu, 0xFEECC880u, 0xEC80FEC8u, 0xFFECC800u, 0xE800FE80u, 0xFF00FFE8u, 0xF000FFF0u,
    0x008EF710u, 0x08CE7100u, 0x7310008Cu, 0x8CCE3100u, 0x3110088Cu, 0x366C6666u, 0x0FF017E8u, 0x399C718Eu,
    0xF0F0AAAAu, 0x33CC5A5Au, 0x55AA3C3Cu, 0xA55A9696u, 0x13C873CEu, 0x3BDC324Cu, 0xC33C6996u, 0x06609966u,
    0x04E40272u, 0x27204E40u, 0x936CC936u, 0x639C39C6u, 0x9CC69336u, 0xE718817Eu, 0x0FCCCCF0u, 0xEE227744u
};
const uint BC_PART3[64] = {
    0xAA685050u, 0x6A5A5040u, 0x5A5A4200u, 0x5450A0A8u, 0xA5A50000u, 0xA0A05050u, 0x5555A0A0u, 0x5A5A5050u,
    0xAA550000u, 0xAA555500u, 0xAAAA5500u, 0x90909090u, 0x94949494u, 0xA4A4A4A4u, 0xA9A59450u, 0x2A0A4250u,
    0xA5945040u, 0x0A425054u, 0xA5A5A500u, 0x55A0A0A0u, 0xA8A85454u, 0x6A6A4040u, 0xA4A45000u, 0x1A1A0500u,
    0x0050A4A4u, 0xAAA59090u, 0x14696914u, 0x69691400u, 0xA08585A0u, 0xAA821414u, 0x50A4A450u, 0x6A5A0200u,
    0xA9A58000u, 0x5090A0A8u, 0xA8A09050u, 0x24242424u, 0x00AA5500u, 0x24924924u, 0x24499224u, 0x50A50A50u,
    0x500AA550u, 0xAAAA4444u, 0x66660000u, 0xA5A0A5A0u, 0x50A050A0u, 0x69286928u, 0x44AAAA44u, 0x66666600u,
    0xAA444444u, 0x54A854A8u, 0x95809580u, 0x96969600u, 0xA85454A8u, 0x80959580u, 0xAA141414u, 0x96960000u,
    0xAAAA1414u, 0xA05050A0u, 0xA0A5A5A0u, 0x96000000u, 0x40804080u, 0xA9A8A9A8u, 0xAAAAAA44u, 0x2A4A5254u
};
const uint BC_ANCHOR2[8] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xF882282Fu, 0x22882282u, 0xFF8286FFu, 0x6FF22282u, 0x22FF8626u, 0xF22FFFFFu
};
const uint BC_ANCHOR3A[8] = {
    0xFF38FF33u, 0x33566688u, 0xA633F833u, 0xFF586885u, 0xF8A653F8u, 0xFFFF5F3Fu, 0xA58555F3u, 0x33CFD8A5u
};
const uint BC_ANCHOR3B[8] = {
    0x83FF388Fu, 0x8FFFFFFFu, 0x8F8F3F8Fu, 0x8AFFA6F3u, 0xA98AAF3Fu, 0x8663F8F6u, 0xFFFFFF3Fu, 0x8FF3FFFFu
};
const uint BC6_MODE[15] = {
    0x00555560u, 0x136663E1u, 0x2A4455E2u, 0x3C4545E6u, 0x505445EAu,
    0x645554EEu, 0x77556472u, 0x8A565476u, 0x9F65547Au, 0xB466633Eu,
    0xCBAAA503u, 0xD19995C7u, 0xDA88864Bu, 0xE344484Fu, 0xEC000000u
};
const uint BC6_FIELD[118] = {
    0x04480447u, 0x0900044Bu, 0x09020901u, 0x044A0403u, 0x04040307u, 0x030A000Bu, 0x011B0405u, 0x04060308u,
    0x0409022Bu, 0x0557033Bu, 0x055A044Au, 0x000B0600u, 0x0448011Bu, 0x05580601u, 0x0447022Bu, 0x033B0602u,
    0x044B055Bu, 0x03070503u, 0x030A0504u, 0x03080505u, 0x05090506u, 0x09010900u, 0x04030902u, 0x03070AA0u,
    0x0AA10304u, 0x030A000Bu, 0x0AA20305u, 0x0308011Bu, 0x022B0406u, 0x033B0409u, 0x09010900u, 0x03030902u,
    0x044A0AA0u, 0x04040307u, 0x030A0AA1u, 0x0AA20305u, 0x0308011Bu, 0x000B0306u, 0x0309022Bu, 0x033B0447u,
    0x09010900u, 0x03030902u, 0x04480AA0u, 0x03040307u, 0x000B0AA1u, 0x0405030Au, 0x03080AA2u, 0x011B0306u,
    0x0309022Bu, 0x033B044Bu, 0x04480800u, 0x04470801u, 0x044B0802u, 0x044A0403u, 0x04040307u, 0x030A000Bu,
    0x011B0405u, 0x04060308u, 0x0409022Bu, 0x0700033Bu, 0x0448044Au, 0x022B0701u, 0x07020447u, 0x044B033Bu,
    0x03070503u, 0x000B0404u, 0x0405030Au, 0x0308011Bu, 0x05090506u, 0x000B0700u, 0x07010448u, 0x04470557u,
    0x055A0702u, 0x0403044Bu, 0x0307044Au, 0x030A0504u, 0x011B0405u, 0x04060308u, 0x0409022Bu, 0x0700033Bu,
    0x0448011Bu, 0x05580701u, 0x07020447u, 0x044B055Bu, 0x044A0403u, 0x04040307u, 0x030A000Bu, 0x03080505u,
    0x022B0406u, 0x033B0409u, 0x044A0500u, 0x011B000Bu, 0x05010448u, 0x05580557u, 0x0447022Bu, 0x055A0502u,
    0x055B033Bu, 0x0503044Bu, 0x05040307u, 0x0505030Au, 0x05060308u, 0x09000509u, 0x09020901u, 0x09040903u,
    0x09000905u, 0x09020901u, 0x0AA00803u, 0x0AA10804u, 0x0AA20805u, 0x09010900u, 0x07030902u, 0x07040AB0u,
    0x07050AB1u, 0x09000AB2u, 0x09020901u, 0x0AF00303u, 0x0AF10304u, 0x0AF20305u
};
/* the interpolation weights of 2, 3 and 4 bit indices, a byte each: 2 bit ones in word 0, 3 bit in 1 and 2, 4 bit
   in 3 to 6 */
const uint BC_WEIGHT[7] = {0x402B1500u, 0x1B120900u, 0x40372E25u, 0x0D090400u,
                           0x1E1A1511u, 0x2F2B2622u, 0x403C3733u};
int bc_weight(uint bits, uint k) {
    uint base = bits == 2u ? 0u : bits == 3u ? 1u : 3u;
    return int((BC_WEIGHT[base + (k >> 2)] >> ((k & 3u) * 8u)) & 255u);
}
int bc_lerp(int a, int b, int w) { return (a * (64 - w) + b * w + 32) >> 6; }
uint bc_subset(uint ns, uint p, uint i) {
    if (ns == 1u) return 0u;
    if (ns == 2u) return (BC_PART2[p >> 1] >> ((p & 1u) * 16u + i)) & 1u;
    return (BC_PART3[p] >> (2u * i)) & 3u;
}
/* the anchor pixel of subset s (1 or 2) of partition p of ns subsets; subset 0's is pixel 0 */
uint bc_anchor(uint ns, uint p, uint s) {
    uint t = ns == 2u ? BC_ANCHOR2[p >> 3] : s == 1u ? BC_ANCHOR3A[p >> 3] : BC_ANCHOR3B[p >> 3];
    return (t >> ((p & 7u) * 4u)) & 15u;
}
/* pixel i's index of bits bits, in indices from bit at on: an anchor's has one bit less, its top bit 0 */
uint bc_index(uint b[4], uint ns, uint p, uint at, uint bits, uint i) {
    uint a1 = ns >= 2u ? bc_anchor(ns, p, 1u) : 16u, a2 = ns == 3u ? bc_anchor(ns, p, 2u) : 16u;
    uint n = at + i * bits - (i > 0u ? 1u : 0u) - (i > a1 ? 1u : 0u) - (i > a2 ? 1u : 0u);
    bool anchor = i == 0u || i == a1 || i == a2;
    return bc_bits(b, n, anchor ? bits - 1u : bits);
}

int bc_sext(uint v, uint bits) { return int(v << (32u - bits)) >> (32u - bits); }
int bc6_unquantize(int v, uint epb, bool sf) {
    if (!sf) {
        if (epb >= 15u) return v;
        if (v == 0) return 0;
        if (v == (1 << epb) - 1) return 0xFFFF;
        return ((v << 16) + 0x8000) >> epb;
    }
    if (epb >= 16u) return v;
    int m = v < 0 ? -v : v, q;
    if (m == 0)
        q = 0;
    else if (m >= (1 << (epb - 1u)) - 1)
        q = 0x7FFF;
    else
        q = ((m << 15) + 0x4000) >> (epb - 1u);
    return v < 0 ? -q : q;
}
/* an interpolated value to the bits of a half float: 31/64 of the unsigned range, 31/32 of the signed one's
   magnitude */
uint bc6_finish(int v, bool sf) {
    if (!sf) return uint((v * 31) >> 6);
    return v < 0 ? 0x8000u | uint((-v * 31) >> 5) : uint((v * 31) >> 5);
}
/* a BC6H block b into four rows of RGBA16F pixels from word o0, at column u of blocks, alpha 1.0. A reserved mode
   is black. BC6_MODE holds each mode's code (bits 0-4), whether it has two regions (5), whether its endpoints after
   the first are deltas (6), the endpoints' bits (7-11), the deltas' bits of R, G and B (12-23) and its first field
   in BC6_FIELD (24-31; the entry after the last mode ends it). A field, 16 bits, is the endpoint and color it fills
   (endpoint * 3 + color, bits 0-3), the bit its first bit goes to (4-7) and its last's (8-11): a first bit above
   the last is a field read from its high bit down */
void bc6_block(uint b[4], bool sf, uint o0, uint u) {
    uint code = (b[0] & 3u) < 2u ? b[0] & 3u : b[0] & 31u, m = 0u;
    while (m < 14u && ((BC6_MODE[m] & 31u) != code || (m < 2u) != (code < 2u))) m++;
    int e[12];
    for (uint k = 0u; k < 12u; k++) e[k] = 0;
    uint ns = 1u, p = 0u, at = m < 2u ? 2u : 5u;
    if (m < 14u) {
        uint mw = BC6_MODE[m];
        for (uint s = mw >> 24; s < (BC6_MODE[m + 1u] >> 24); s++) {
            uint f = (BC6_FIELD[s >> 1] >> ((s & 1u) * 16u)) & 0xFFFFu, first = (f >> 4) & 15u, last = f >> 8;
            uint n = (last >= first ? last - first : first - last) + 1u, v = bc_bits(b, at, n);
            at += n;
            if (last < first) v = bitfieldReverse(v) >> (32u - n);
            e[f & 15u] |= int(v << min(first, last));
        }
        bool tr = (mw & 64u) != 0u;
        uint epb = (mw >> 7) & 31u;
        if ((mw & 32u) != 0u) {
            ns = 2u;
            p = bc_bits(b, at, 5u);
            at += 5u;
        }
        for (uint c = 0u; c < 3u; c++) {
            uint db = (mw >> (12u + 4u * c)) & 15u;
            if (sf) e[c] = bc_sext(uint(e[c]), epb);
            for (uint k = 1u; k < 2u * ns; k++) {
                int v = e[3u * k + c];
                if (sf || tr) v = bc_sext(uint(v), tr ? db : epb);
                if (tr) {
                    v = (e[c] + v) & ((1 << epb) - 1);
                    if (sf) v = bc_sext(uint(v), epb);
                }
                e[3u * k + c] = v;
            }
            for (uint k = 0u; k < 2u * ns; k++) e[3u * k + c] = bc6_unquantize(e[3u * k + c], epb, sf);
        }
    }
    uint ib = ns == 2u ? 3u : 4u;
    for (uint i = 0u; i < 16u; i++) {
        uint o = o0 + (i >> 2) * out_pitch + 8u * u + 2u * (i & 3u), rgb[3];
        uint s = bc_subset(ns, p, i);
        int w = m < 14u ? bc_weight(ib, bc_index(b, ns, p, at, ib, i)) : 0;
        for (uint c = 0u; c < 3u; c++)
            rgb[c] = m < 14u ? bc6_finish(bc_lerp(e[6u * s + c], e[6u * s + 3u + c], w), sf) : 0u;
        dst[o] = rgb[0] | (rgb[1] << 16);
        dst[o + 1u] = rgb[2] | 0x3C000000u; /* alpha 1.0 */
    }
}

/* BC7's modes, a nibble each: subsets, partition bits, rotation bits, index selection bits, color bits, alpha bits,
   index bits and the second indices' bits. Modes 0, 3, 6 and 7 have a p-bit an endpoint, mode 1 one a subset */
const uint BC7_MODE[8] = {0x03040043u, 0x03060062u, 0x02050063u, 0x02070062u,
                          0x32651201u, 0x22870201u, 0x04770001u, 0x02550062u};
/* a BC7 block b into four rows of RGBA8 pixels from word o0, at column u of blocks. A block without a mode (its
   first byte 0) is transparent black */
void bc7_block(uint b[4], uint o0, uint u) {
    if ((b[0] & 255u) == 0u) {
        for (uint i = 0u; i < 16u; i++) dst[o0 + (i >> 2) * out_pitch + 4u * u + (i & 3u)] = 0u;
        return;
    }
    uint mode = uint(findLSB(b[0] & 255u)), mw = BC7_MODE[mode];
    uint ns = mw & 15u, pb = (mw >> 4) & 15u, rb = (mw >> 8) & 15u, sb = (mw >> 12) & 15u;
    uint cb = (mw >> 16) & 15u, ab = (mw >> 20) & 15u, ib = (mw >> 24) & 15u, ib2 = mw >> 28;
    uint at = mode + 1u, p = bc_bits(b, at, pb);
    uint rot = bc_bits(b, at + pb, rb), isb = bc_bits(b, at + pb + rb, sb);
    at += pb + rb + sb;
    /* endpoint k's channel c in e[4k + c]: R of every endpoint, then G, B and A */
    int e[24];
    uint nend = 2u * ns;
    for (uint c = 0u; c < 4u; c++) {
        uint bits = c < 3u ? cb : ab;
        for (uint k = 0u; k < nend; k++) {
            e[4u * k + c] = bits != 0u ? int(bc_bits(b, at, bits)) : 255;
            at += bits;
        }
    }
    bool epbit = mode == 0u || mode == 3u || mode == 6u || mode == 7u, spbit = mode == 1u;
    uint cprec = cb, aprec = ab;
    if (epbit || spbit) {
        for (uint k = 0u; k < nend; k++) {
            int pbit = int(bc_bits(b, at + (epbit ? k : k >> 1), 1u));
            for (uint c = 0u; c < 4u; c++)
                if (c < 3u || ab != 0u) e[4u * k + c] = (e[4u * k + c] << 1) | pbit;
        }
        at += epbit ? nend : ns;
        cprec++;
        if (ab != 0u) aprec++;
    }
    for (uint k = 0u; k < nend; k++)
        for (uint c = 0u; c < 4u; c++)
            if (c < 3u || ab != 0u) e[4u * k + c] = bc_expand(uint(e[4u * k + c]), c < 3u ? cprec : aprec);
    /* the second indices follow the first, whose anchors (one a subset) have a bit less */
    uint at2 = at + 16u * ib - ns;
    for (uint i = 0u; i < 16u; i++) {
        uint s = bc_subset(ns, p, i), i1 = bc_index(b, ns, p, at, ib, i);
        uint i2 = ib2 != 0u ? bc_index(b, 1u, 0u, at2, ib2, i) : i1;
        /* modes 4 and 5: color from the first indices and alpha from the second, swapped by the index selection */
        uint ci = isb != 0u ? i2 : i1, cbits = isb != 0u ? ib2 : ib;
        uint ai = isb != 0u ? i1 : i2, abits = ib2 == 0u || isb != 0u ? ib : ib2;
        int px[4];
        for (uint c = 0u; c < 4u; c++)
            px[c] = bc_lerp(e[8u * s + c], e[8u * s + 4u + c], c < 3u ? bc_weight(cbits, ci) : bc_weight(abits, ai));
        if (rot != 0u) { /* the alpha channel's values swapped with red's, green's or blue's */
            int t = px[3];
            px[3] = px[rot - 1u];
            px[rot - 1u] = t;
        }
        dst[o0 + (i >> 2) * out_pitch + 4u * u + (i & 3u)] = tc_rgba(px[0], px[1], px[2], uint(px[3]));
    }
}

/* a BC block at word at of the input into four rows of pixels from word o0 of the output, at column u of blocks */
void bc_block(uint at, uint o0, uint u) {
    bool two = mode != TC_BC1 && mode != TC_BC4 && mode != TC_BC4_SNORM;
    uint b[4];
    b[0] = src[at];
    b[1] = src[at + 1u];
    b[2] = 0u;
    b[3] = 0u;
    if (two) {
        b[2] = src[at + 2u];
        b[3] = src[at + 3u];
    }
    if (mode == TC_BC6H_UF || mode == TC_BC6H_SF) {
        bc6_block(b, mode == TC_BC6H_SF, o0, u);
        return;
    }
    if (mode == TC_BC7) {
        bc7_block(b, o0, u);
        return;
    }
    bool sn = mode == TC_BC4_SNORM || mode == TC_BC5_SNORM;
    for (uint y = 0u; y < 4u; y++) {
        uint o = o0 + y * out_pitch;
        if (mode == TC_BC4 || mode == TC_BC4_SNORM) { /* two 16 bit texels a word */
            dst[o + 2u * u] = bc4_value(b[1], b[0], sn, 4u * y) | (bc4_value(b[1], b[0], sn, 4u * y + 1u) << 16);
            dst[o + 2u * u + 1u] =
                bc4_value(b[1], b[0], sn, 4u * y + 2u) | (bc4_value(b[1], b[0], sn, 4u * y + 3u) << 16);
            continue;
        }
        for (uint x = 0u; x < 4u; x++) {
            uint i = y * 4u + x, c;
            if (mode >= TC_BC5) {
                c = bc4_value(b[1], b[0], sn, i) | (bc4_value(b[3], b[2], sn, i) << 16);
            } else if (mode == TC_BC1) {
                c = bc1_pixel(b[0], b[1], true, i);
            } else {
                uint a = mode == TC_BC2 ? (((i < 8u ? b[0] : b[1]) >> (4u * (i & 7u))) & 15u) * 17u
                                        : bc3_alpha(b[1], b[0], i);
                c = (bc1_pixel(b[2], b[3], false, i) & 0xFFFFFFu) | (a << 24);
            }
            dst[o + 4u * u + x] = c;
        }
    }
}

/* the float nearest z / (2^24 - 1), the bits the CPU's (float)z / 16777215.0f gives. In binary the quotient is z's
   24 bits repeated after the point: from z's top bit p on, the significand is z << (23 - p), and the bits after it
   are the zeros above p in the next copy, then that copy's bit p, a 1, with more ones after it (z repeats): the
   remainder is over half an ulp and never a tie, so the nearest float is one ulp up */
uint tc_d24_float(uint z) {
    if (z == 0u) return 0u;
    uint p = uint(findMSB(z));
    uint m = (z << (23u - p)) + 1u;
    if (m == 0x1000000u) { /* rounded up to the next power of two */
        m = 0x800000u;
        p++;
    }
    return ((p + 103u) << 23) | (m & 0x7FFFFFu); /* exponent p - 24, biased by 127 */
}

/* present.c's expansion of a vi pixel's field of 4, 5 or 6 bits to 8 */
uint tc_expand(uint v, uint bits) {
    return bits == 4u ? v * 17u : bits == 5u ? (v << 3) | (v >> 2) : (v << 2) | (v >> 4);
}
uint tc_16to32(uint v) {
    if (mode == TC_VI_RGB565)
        return tc_expand((v >> 11) & 31u, 5u) | (tc_expand((v >> 5) & 63u, 6u) << 8) |
               (tc_expand(v & 31u, 5u) << 16) | 0xFF000000u;
    if (mode == TC_VI_RGBA5551)
        return tc_expand((v >> 11) & 31u, 5u) | (tc_expand((v >> 6) & 31u, 5u) << 8) |
               (tc_expand((v >> 1) & 31u, 5u) << 16) | ((v & 1u) != 0u ? 0xFF000000u : 0u);
    if (mode == TC_VI_RGBA4444)
        return tc_expand((v >> 12) & 15u, 4u) | (tc_expand((v >> 8) & 15u, 4u) << 8) |
               (tc_expand((v >> 4) & 15u, 4u) << 16) | (tc_expand(v & 15u, 4u) << 24);
    if (mode == TC_RGBA4)
        return (v & 15u) * 17u | (((v >> 4) & 15u) * 17u << 8) | (((v >> 8) & 15u) * 17u << 16) |
               ((v >> 12) * 17u << 24);
    if (mode == TC_RGB5A1)
        return (v & 31u) * 255u / 31u | (((v >> 5) & 31u) * 255u / 31u << 8) |
               (((v >> 10) & 31u) * 255u / 31u << 16) | ((v >> 15) * 255u << 24);
    return (v & 1u) * 255u | (((v >> 1) & 31u) * 255u / 31u << 8) | (((v >> 6) & 31u) * 255u / 31u << 16) |
           (((v >> 11) & 31u) * 255u / 31u << 24);
}

/* byte b of the input buffer (byte addressed) */
uint tc_src_byte(uint b) { return (src[b >> 2] >> (8u * (b & 3u))) & 0xFFu; }

/* the bits of the float a half is (tests/support/half.c's half_to_float). A subnormal's top bit p becomes the
   implicit one: m * 2^-24 is 1.f * 2^(p - 24) */
uint tc_half_float(uint h) {
    uint sign = (h & 0x8000u) << 16, e = (h >> 10) & 31u, m = h & 0x3FFu;
    if (e == 31u) return sign | 0x7F800000u | (m << 13);
    if (e != 0u) return sign | ((e + 112u) << 23) | (m << 13);
    if (m == 0u) return sign;
    uint p = uint(findMSB(m));
    return sign | ((p + 103u) << 23) | ((m << (23u - p)) & 0x7FFFFFu);
}
/* the half nearest the float x (its bits), ties to even, too large for one an infinity, a NaN kept quiet (half.c's
   float_to_half). Integer arithmetic: packHalf2x16's rounding is the device's */
uint tc_float_half(uint x) {
    uint sign = (x >> 16) & 0x8000u, e = (x >> 23) & 0xFFu, m = x & 0x7FFFFFu, h, rem, tie;
    if (e == 0xFFu) return sign | 0x7C00u | (m != 0u ? 0x200u | (m >> 13) : 0u);
    if (e >= 143u) return sign | 0x7C00u; /* 2^16 and up */
    if (e <= 112u) {                       /* a subnormal half, or zero below half the smallest */
        if (e < 102u) return sign;
        uint s = 126u - e; /* 14 to 24 */
        m |= 0x800000u;
        h = m >> s;
        rem = m & ((1u << s) - 1u);
        tie = 1u << (s - 1u);
    } else {
        h = ((e - 112u) << 10) | (m >> 13);
        rem = m & 0x1FFFu;
        tie = 0x1000u;
    }
    if (rem > tie || (rem == tie && (h & 1u) != 0u)) h++; /* a carry into the exponent rounds up to it, or infinity */
    return sign | h;
}
/* TC_BLIT_FLOAT: the bits of component k of the source pixel at byte p as a float */
uint tc_blit_component(uint p, uint k) {
    uint sn = remap & 15u, sb = (remap >> 4) & 15u;
    if (k >= sn || (k == 3u && (remap & 0x10000u) != 0u)) return k == 3u ? 0x3F800000u : 0u;
    uint at = p + k * sb, v = tc_src_byte(at) | (tc_src_byte(at + 1u) << 8);
    if (sb == 2u) return tc_half_float(v);
    return v | (tc_src_byte(at + 2u) << 16) | (tc_src_byte(at + 3u) << 24);
}
/* TC_DMA_REMAP: byte ob of a line out, with the line in from byte in_line on and the byte now there in old */
uint tc_remap_byte(uint ob, uint in_line, uint old) {
    uint cs = remap & 15u, ns = (remap >> 4) & 15u, nd = (remap >> 8) & 15u, eo = cs * nd;
    uint e = ob / eo, within = ob - e * eo, k = within / cs, j = within - k * cs, sel = (remap >> (12u + 4u * k)) & 7u;
    if (sel < 4u) return sel < ns ? tc_src_byte(in_line + e * cs * ns + sel * cs + j) : 0u;
    if (sel < 6u) return ((sel == 4u ? const_a : const_b) >> (8u * j)) & 0xFFu;
    return old;
}

void main() {
    uint u = gl_GlobalInvocationID.x;
    if (u >= units) return;
    if (mode == TC_BLIT) {
        uint line = gl_GlobalInvocationID.y, at = out_bias + line * out_pitch + u, w = dst[at], r = 0u;
        uint row = in_bias + src[const_b + line] * in_pitch;
        for (uint b = 0u; b < 4u; b++) {
            uint ob = 4u * u + b, v = (w >> (8u * b)) & 0xFFu;
            if (ob < line_out) { /* a branch, not a select: the tables end at the row's last pixel */
                uint i = ob / remap;
                v = tc_src_byte(row + src[const_a + i] * remap + (ob - i * remap));
            }
            r |= v << (8u * b);
        }
        dst[at] = r;
        return;
    }
    if (mode == TC_BLIT_FLOAT) {
        uint line = gl_GlobalInvocationID.y, at = out_bias + line * out_pitch + u, w = dst[at];
        uint sp = (remap & 15u) * ((remap >> 4) & 15u), db = (remap >> 12) & 15u, dp = ((remap >> 8) & 15u) * db;
        uint row = in_bias + src[const_b + line] * in_pitch;
        /* the word's components (a pixel's are 2 or 4 bytes each, and pixels start on them) */
        for (uint ob = 4u * u; ob < 4u * u + 4u && ob < line_out; ob += db) {
            uint i = ob / dp, v = tc_blit_component(row + src[const_a + i] * sp, (ob - i * dp) / db);
            if (db == 4u) {
                w = v;
            } else {
                uint sh = 8u * (ob & 3u);
                w = (w & ~(0xFFFFu << sh)) | (tc_float_half(v) << sh);
            }
        }
        dst[at] = w;
        return;
    }
    if (mode == TC_DMA_REMAP) {
        uint line = gl_GlobalInvocationID.y, at = out_bias + line * out_pitch + u, w = dst[at], r = 0u;
        for (uint b = 0u; b < 4u; b++) {
            uint ob = 4u * u + b, v = (w >> (8u * b)) & 0xFFu;
            if (ob < line_out) v = tc_remap_byte(ob, in_bias + line * in_pitch, v); /* not past the line's input */
            r |= v << (8u * b);
        }
        dst[at] = r;
        return;
    }
    /* the row of units (texels, or blocks for ETC2, EAC and BCn) of all the dispatch's slices */
    uint row = gl_GlobalInvocationID.z * rows + gl_GlobalInvocationID.y;
    uint in_at = in_bias + row * in_pitch, out_at = out_bias + row * out_pitch;
    if (mode == TC_NV12_UV) {
        uint u4 = src[in_at + u], v4 = src[st_bias + row * st_pitch + u];
        dst[out_at + 2u * u] = (u4 & 0xFFu) | ((v4 & 0xFFu) << 8) | ((u4 & 0xFF00u) << 8) | ((v4 & 0xFF00u) << 16);
        dst[out_at + 2u * u + 1u] =
            ((u4 >> 16) & 0xFFu) | ((v4 >> 8) & 0xFF00u) | ((u4 >> 8) & 0xFF0000u) | (v4 & 0xFF000000u);
    } else if (mode >= TC_BC1) {
        bool small = mode == TC_BC1 || mode == TC_BC4 || mode == TC_BC4_SNORM; /* 8 byte blocks */
        bc_block(in_at + u * (small ? 2u : 4u), out_bias + row * 4u * out_pitch, u);
    } else if (mode >= TC_VI_RGBX8888) {
        uint p = src[in_at + u];
        dst[out_at + u] = mode == TC_VI_RGBX8888 ? p | 0xFF000000u
                                                 : (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
    } else if (mode <= TC_A5B5G5R1 || mode >= TC_VI_RGB565) {
        uint w = src[in_at + u];
        dst[out_at + 2u * u] = tc_16to32(w & 0xFFFFu);
        dst[out_at + 2u * u + 1u] = tc_16to32(w >> 16);
    } else if (mode == TC_RGB32F) {
        dst[out_at + 4u * u] = src[in_at + 3u * u];
        dst[out_at + 4u * u + 1u] = src[in_at + 3u * u + 1u];
        dst[out_at + 4u * u + 2u] = src[in_at + 3u * u + 2u];
        dst[out_at + 4u * u + 3u] = 0x3F800000u; /* 1.0 */
    } else if (mode <= TC_D32S8) {
        uint st = 0u;
        for (uint i = 0u; i < 4u; i++) {
            uint d, s;
            if (mode == TC_D32S8) {
                d = src[in_at + 8u * u + 2u * i];
                s = src[in_at + 8u * u + 2u * i + 1u] & 255u;
            } else {
                uint v = src[in_at + 4u * u + i];
                d = mode == TC_S8D24 ? v >> 8 : v & 0xFFFFFFu;
                s = mode == TC_D24X8 ? 0u : mode == TC_S8D24 ? v & 255u : v >> 24;
                if (zfloat != 0u) d = tc_d24_float(d);
            }
            dst[out_at + 4u * u + i] = d;
            st |= s << (8u * i);
        }
        dst[st_bias + row * st_pitch + u] = st;
    } else {
        /* a block: its 8 byte halves big endian, the first (alpha, or red) in hi:lo, the second in hi2:lo2 */
        bool two = mode == TC_ETC2_RGBA || mode >= TC_EAC_RG, sn = mode == TC_EAC_R_SNORM || mode == TC_EAC_RG_SNORM;
        uint at = in_at + u * (two ? 4u : 2u);
        uint hi = tc_bswap(src[at]), lo = tc_bswap(src[at + 1u]), hi2 = 0u, lo2 = 0u;
        if (two) {
            hi2 = tc_bswap(src[at + 2u]);
            lo2 = tc_bswap(src[at + 3u]);
        }
        /* four rows of pixels a row of blocks */
        uint out_row = out_bias + row * 4u * out_pitch;
        for (uint y = 0u; y < 4u; y++) {
            uint o = out_row + y * out_pitch;
            if (mode == TC_EAC_R || mode == TC_EAC_R_SNORM) {
                dst[o + 2u * u] = eac11(hi, lo, sn, y) | (eac11(hi, lo, sn, 4u + y) << 16);
                dst[o + 2u * u + 1u] = eac11(hi, lo, sn, 8u + y) | (eac11(hi, lo, sn, 12u + y) << 16);
                continue;
            }
            for (uint x = 0u; x < 4u; x++) {
                uint i = x * 4u + y, c;
                if (mode == TC_ETC2_RGBA)
                    c = (etc2_rgb_pixel(hi2, lo2, false, x, y) & 0xFFFFFFu) | (eac_alpha(hi, lo, i) << 24);
                else if (mode <= TC_ETC2_PTA)
                    c = etc2_rgb_pixel(hi, lo, mode == TC_ETC2_PTA, x, y);
                else
                    c = eac11(hi, lo, sn, i) | (eac11(hi2, lo2, sn, i) << 16);
                dst[o + 4u * u + x] = c;
            }
        }
    }
}
