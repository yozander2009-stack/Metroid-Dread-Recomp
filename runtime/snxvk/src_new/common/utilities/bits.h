/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_BITS_H
#define SNX_BITS_H

/* Small integer helpers every part of the program had its own copy of: rounding to a multiple, clamping, and the
   FNV-1a hash the caches key their entries with. */

#include "common/utilities/bits.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* value rounded up to a multiple of a (any a above zero, not only powers of two); the caller keeps the sum in range */
static inline uint64_t snx_align_up(uint64_t value, uint64_t a) { return (value + a - 1) / a * a; }
static inline uint64_t snx_align_down(uint64_t value, uint64_t a) { return value / a * a; }
static inline uint64_t snx_div_up(uint64_t value, uint64_t d) { return (value + d - 1) / d; }

/* count bits of v from bit at (count below 32) */
static inline uint32_t snx_bits(uint32_t v, unsigned at, unsigned count) { return (v >> at) & ((1u << count) - 1); }
/* whether length bytes from offset lie inside a range of total bytes, without the sum overflowing */
static inline int snx_in_range(uint64_t offset, uint64_t length, uint64_t total) {
    return offset <= total && length <= total - offset;
}

static inline int snx_clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float snx_clamp_float(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline double snx_clamp_double(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint32_t snx_min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline uint32_t snx_max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }
static inline uint64_t snx_min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static inline uint64_t snx_max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }

/* 64-bit FNV-1a: SNX_FNV_BASIS to start, each call continuing the hash of the bytes before */
#define SNX_FNV_BASIS 0xcbf29ce484222325ull
#define SNX_FNV_PRIME 0x100000001b3ull
/* one FNV-1a step over a whole value (a word, a hash of a part) instead of a byte */
static inline uint64_t snx_fnv_step(uint64_t hash, uint64_t value) { return (hash ^ value) * SNX_FNV_PRIME; }
static inline uint64_t snx_fnv1a(uint64_t hash, const void* bytes, size_t size) {
    const unsigned char* p = (const unsigned char*)bytes;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * SNX_FNV_PRIME;
    return hash;
}

/* qsort comparators over arrays of strings and of 64-bit values */
static inline int snx_compare_strings(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}
static inline int snx_compare_u64(const void* a, const void* b) {
    uint64_t x, y;
    memcpy(&x, a, sizeof x);
    memcpy(&y, b, sizeof y);
    return (x > y) - (x < y);
}

#endif
