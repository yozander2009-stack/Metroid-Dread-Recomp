/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the parts of the shader translator share: the opcodes and their decoder, the program's layout in words, and
   the control flow helpers of shader.c. */
#ifndef SNX_TRANSLATOR_H
#define SNX_TRANSLATOR_H
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
/* the opcode enum O_NONE, O_<NAME>..., O__COUNT, generated from maxwell_opcodes.def by tools/shaderdec. The
   translator tests ranges of it (O_XMAD_R to O_XMAD_I...), so the spec keeps each family's rows together */
#include "maxwell_opcodes.h"

/* n bits of v from bit lo */
#define B(v, lo, n) ((u32)(((v) >> (lo)) & ((1ull << (n)) - 1)))

/* a decoded opcode: its pattern's fixed bits (mask) and their values (val), how many bits it fixes, and its name for
   messages */
typedef struct {
    int id;
    const char* name;
    u64 mask, val;
    int bits;
} Op;

/* Word i of a program is an instruction: words 0-9 are the 80-byte SPH, and word 10 and every fourth after it
   schedule the three instructions that follow. */
#define IDX_OK(i) ((i) >= 11 && ((i) - 10) % 4 != 0)
/* the token kind a CRS stack instruction pushes or pops: SSY/SYNC 0, PBK/BRK 1, PCNT/CONT 2, PEXIT 3 */
#define CRS_KIND(id)                                                                                                   \
    ((id) == O_SSY || (id) == O_SYNC ? 0 : (id) == O_PBK || (id) == O_BRK ? 1 : (id) == O_PEXIT ? 3 : 2)

/* A program's bytes are guest memory's, so its words are little-endian: the instruction words as host numbers, which
   the decoder and the translator read. code itself on a little-endian host; on a big-endian one a malloc'd copy of
   bytes / 8 words, in *copy for the caller to free (NULL without memory) */
const u64* shader_words(const u8* code, u32 bytes, u64** copy);
/* the opcode of instruction w, or NULL when no encoding matches */
const Op* decode(u64 w);
/* the word a relative branch (BRA, SSY, PBK, PCNT, CAL, PRET, PEXIT) at word idx goes to */
u32 bra_target(u64 w, u32 idx);
/* whether the instruction w (decoded as op) writes general register reg, its whole destination range included
   (spirv_translate.c) */
int shader_writes_reg(u64 w, int op, u32 reg);
/* whether a flow instruction always takes effect: predicate PT and flow condition T */
int flow_always(u64 w);
/* each SYNC, BRK and CONT's target when the CRS stack can be followed at translation time: 1, or 0 when the stack
   has to be kept at run time */
int shader_resolve_crs(const u64* words, u32 count, u32* pop_targets);

/* A program's control flow as structured constructs (structure.c), as a list of tokens in the order the translator
   writes them. Scopes are SPIR-V loop constructs: a block scope runs once and ends before the block it was opened
   for, a loop scope repeats from its header. */
enum {
    SHS_BODY,       /* a block's instructions: the words from a up to b */
    SHS_RETURNED,   /* the body before ended the program: an EXIT that always runs */
    SHS_OPEN_BLOCK, /* a: the scope */
    SHS_OPEN_LOOP,  /* a: the scope */
    SHS_CLOSE,      /* a: the scope. Running off its end leaves it */
    SHS_IF,         /* on the condition of the branch that ends the body before: its target first */
    SHS_ELSE,
    SHS_ENDIF,
    SHS_BREAK,    /* a: the innermost scope, left; b: the scope + 1 to store as the pending exit first, 0 for none */
    SHS_CONTINUE, /* a: the innermost scope, a loop, repeated */
    SHS_END,      /* control runs off the program's last instruction */
    SHS_SWITCH,   /* on the word the BRX that ends the body before goes to: a cases */
    SHS_CASE,     /* a: the case's index, b: its word */
    SHS_DEFAULT,  /* a: the cases (another word ends the program, SHS_END) */
    SHS_ENDSWITCH
};
typedef struct {
    u32 kind, a, b;
} ShsToken;
typedef struct {
    u8 loop;
    /* a pending exit may reach its merge: whether one there goes to the parent scope, or to one further out */
    u8 check, arrive, beyond;
    u32 parent; /* the enclosing scope + 1, 0 for none */
} ShsScope;
typedef struct {
    ShsToken* token;
    ShsScope* scope;
    u32 ntokens, nscopes;
    int uses_flag; /* some break stores a pending exit */
} ShStructure;
/* The structure of a program without calls, unknown jump tables or a CRS stack kept at run time, from the translator's
   block leaders (a byte a word) and its pops' targets: NULL when the graph is irreducible or the structured form
   can't take it. One allocation, freed with free */
ShStructure* shader_structure(const u64* insn, u32 n, const u8* leader, const u32* pop_targets);
#endif
