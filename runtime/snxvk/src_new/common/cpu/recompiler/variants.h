/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a game module's snx_variants() returns: the runtime code the AOT build linked in.
   The runtime only reads this table; it never compiles or links code. */
#ifndef SNX_VARIANTS_H
#define SNX_VARIANTS_H
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/common.h"

#define SNX_VARIANTS_VERSION 1u

/* one run of guest instructions, as the cache stores them (address, number of words) */
typedef struct {
    SnxU64 address;
    SnxU32 words;
} SnxVariantRun;

/* one compiled function: its start, the leaders a call may enter, the instructions
   its hash covers, the cache's two hashes, and the native entry */
typedef struct {
    SnxU64 start;
    SnxU32 leader_count;
    const SnxU64* leaders;
    SnxU32 run_count;
    const SnxVariantRun* runs;
    SnxU64 h1, h2;
    GuestFn fn;
} SnxVariantFunction;

typedef struct {
    SnxU32 version;
    SnxU32 count;
    const SnxVariantFunction* functions;
} SnxVariants;

/* reads one guest word; nonzero when the address is not readable code */
typedef int (*SnxVariantWordFn)(void* user, SnxU64 address, SnxU32* word);

/* the two hashes' starts, and one instruction (its address and word) mixed into them: the cache's hash_runs and
   snx_variant_hash both hash this way */
#define SNX_VARIANT_H1_BASIS 0xCBF29CE484222325ULL
#define SNX_VARIANT_H2_BASIS 0x84222325CBF29CE4ULL
static inline void snx_variant_mix(SnxU64* h1, SnxU64* h2, SnxU64 address, SnxU32 word) {
    SnxU64 v = (address << 32) ^ (SnxU64)word;
    *h1 = (*h1 ^ v) * (SnxU64)0x100000001B3ULL;
    *h2 = (*h2 ^ (v * (SnxU64)0x9E3779B97F4A7C15ULL)) * (SnxU64)0xC2B2AE3D27D4EB4FULL;
}

/* the cache's hash_runs, exactly: the runtime recomputes it over the current bytes
   before it trusts a variant. Hashes are zero when a word cannot be read. */
int snx_variant_hash(SnxVariantWordFn word, void* user, const SnxVariantRun* runs, size_t count, SnxU64* h1,
                     SnxU64* h2);

#endif
