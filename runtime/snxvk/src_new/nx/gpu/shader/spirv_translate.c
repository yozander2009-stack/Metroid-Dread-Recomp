/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Geometry invocation information follows Citron's emit_spirv_context_get_set.cpp:
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project */
/* Direct Maxwell to SPIR-V translation. Unsupported instructions reject the entire module. */
#include "common/utilities/spirv_emit.h"
#include "nx/gpu/shader/translator.h"
#include <assert.h>
#include <stdlib.h>

enum { VAR_PRED = 255, VAR_FLAG = VAR_PRED + 7, VAR_COUNT = VAR_FLAG + 4 };

typedef struct {
    SpvEmitter emit;
    u32 u32_type, f32_type, f16_type, f64_type, bool_type, zero, true_value, false_value, glsl;
    u32 reg[255];
    /* The registers, predicates and flags (VAR_PRED + p, VAR_FLAG + k) as values in the block being written, so that
       reading one again takes no load and only its last value in the block is stored: the value (0 for a register
       written as reg_alt only, bitcast to its bits when read so), the emitter's block it was read or written in, and
       whether the variable still has to take it. Each is stored before the branch or merge that ends its block
       (var_flush, the emitter's block_end): blocks that follow read the variables. A register's bitcast to another
       32-bit type in the block is reg_alt, of reg_alt_type */
    u32 var_val[VAR_COUNT], var_block[VAR_COUNT], reg_alt[255], reg_alt_type[255];
    u8 var_dirty[VAR_COUNT];
    u8* ftz; /* a bit per id: values with no subnormal (float_ftz's, and reads of registers not tainted) */
    size_t ftz_bytes;
    /* each value's flush in the block it was made in (by id, ftz_of_cap of them): a value read once and used by
       several FTZ instructions is flushed once a block */
    u32 *ftz_of, *ftz_of_block;
    size_t ftz_of_cap;
    /* Registers that may hold a subnormal on the per-instruction FTZ path: written by anything but an FTZ float
       instruction whose result is flushed. A float read of one that isn't is flushed already */
    u8 tainted[256];
    int taint_known;
    int ftz_float; /* the module preserves signed zeros (SignedZeroInfNanPreserve): float_ftz flushes with a float test */
    int soft_ftz_on; /* FTZ instructions flush their operands and results with float_ftz (SH_FTZ_EMULATE's way) */
    /* A pure instruction's guard while it is translated: it runs unguarded and each register, predicate and flag it
       writes keeps its old value where the guard is false, in place of a branch around it. 0 otherwise */
    u32 guard;
    u32 pred[7];
    u32 cc_c, cc_o, cc_z, cc_s;
    u32 cbuf[18], cbuf_pointer[18];
    /* constant buffer words read in the block being written, by bank and offset (a cache: a word that collides is
       read again), and their bitcast to a float, 0 until made. A draw's constant buffers don't change under it */
    struct {
        u32 key, block, word, as_float;
    } cbuf_seen[512];
    u32 scale_buffer, scale_pointer;
    u32 sampler[SH_MAXTEX], sampled_type[SH_MAXTEX];
    u32 texture_array, tex_index[SH_MAXTEX], tex_map_buffer, tex_map_pointer, active_index;
    u8 tex_table[SH_MAXTEX]; /* indexed texture slot to descriptor table */
    u32 tex_func_buffer, tex_func_pointer;
    u32 depth_array, depth_type, depth_sampler;
    u32 swapped; /* 1 + the slot whose sampler is an array element for this instruction */
    u32 image[8], image_type[8];
    u32 gbuf[16], gbuf_pointer, smap, subgroup_lane_input;
    /* A fragment program with flow discards nothing inside its loop: a KIL or a failed alpha test sets this bool,
       an EXIT ends the loop for the lane, and the lane is discarded after it. A demote or a kill in the loop lets a
       driver drop a quad's remaining helpers from execution while they still hold the loop open: AMD's Windows
       compiler then repeats the loop with no lane left to end it, until the GPU is reset. */
    u32 discarded;
    u32 branch_cond; /* with structured flow, the condition of the branch that ends the block, 0 for always */
    u32 branch_target; /* and the word a BRX that ends it goes to */
} DirectCtx;

int g_shader_texture_arrays;
int g_shader_stage_point_size;
int g_shader_output_layer = 1;
int g_shader_viewport_index = 3;
int g_shader_geometry = 1;
int g_shader_float16 = 1;
int g_shader_tessellation = 1;
int g_shader_clock_scopes;
int g_shader_maximal_reconvergence;

/* the Function variable of a register, predicate or flag (z, s, c, o) */
static u32 var_of(const DirectCtx* c, u32 v) {
    if (v < VAR_PRED) return c->reg[v];
    if (v < VAR_FLAG) return c->pred[v - VAR_PRED];
    v -= VAR_FLAG;
    return v == 0 ? c->cc_z : v == 1 ? c->cc_s : v == 2 ? c->cc_c : c->cc_o;
}
static u32 flag_var(const DirectCtx* c, u32 variable) {
    return VAR_FLAG + (variable == c->cc_z ? 0 : variable == c->cc_s ? 1 : variable == c->cc_c ? 2 : 3);
}

/* a predicate or flag's value: its variable's, loaded once a block */
static u32 var_read(DirectCtx* c, u32 v) {
    if (c->var_val[v] && c->var_block[v] == c->emit.block) return c->var_val[v];
    c->var_val[v] = spv_emit_load(&c->emit, c->bool_type, var_of(c, v));
    c->var_block[v] = c->emit.block;
    c->var_dirty[v] = 0;
    return c->var_val[v];
}
static void var_write(DirectCtx* c, u32 v, u32 value) {
    c->var_val[v] = value;
    c->var_block[v] = c->emit.block;
    c->var_dirty[v] = 1;
}

/* Before a merge or a branch: the block's last values into their variables. One written in a block that ended in a
   return or a kill, which store nothing, is dropped */
static void var_flush(void* user) {
    DirectCtx* c = user;
    for (u32 v = 0; v < VAR_COUNT; v++) {
        if (!c->var_dirty[v]) continue;
        c->var_dirty[v] = 0;
        if (c->var_block[v] != c->emit.block) continue;
        if (!c->var_val[v]) c->var_val[v] = spv_emit_bitcast(&c->emit, c->u32_type, c->reg_alt[v]);
        spv_emit_store(&c->emit, var_of(c, v), c->var_val[v]);
    }
}

static u32 reg_read(DirectCtx* c, u32 r) {
    if (r >= 255) return c->zero;
    if (c->var_block[r] == c->emit.block && (c->var_val[r] || c->reg_alt_type[r])) {
        if (!c->var_val[r]) c->var_val[r] = spv_emit_bitcast(&c->emit, c->u32_type, c->reg_alt[r]);
        return c->var_val[r];
    }
    c->var_val[r] = spv_emit_load(&c->emit, c->u32_type, c->reg[r]);
    c->var_block[r] = c->emit.block;
    c->var_dirty[r] = 0;
    c->reg_alt_type[r] = 0;
    return c->var_val[r];
}

static void reg_write(DirectCtx* c, u32 r, u32 value) {
    if (r >= 255) return;
    if (c->guard) value = spv_emit_select(&c->emit, c->u32_type, c->guard, value, reg_read(c, r));
    var_write(c, r, value);
    c->reg_alt_type[r] = 0;
}

/* value, of another 32-bit type, stored as the register's bits: a read of that type in the block takes value back,
   and its bits are a bitcast made when something reads or stores them */
static void reg_write_as(DirectCtx* c, u32 r, u32 value, u32 type) {
    if (r >= 255) return;
    if (c->guard) { /* a guarded write's bits are a select of value's, not value's */
        reg_write(c, r, spv_emit_bitcast(&c->emit, c->u32_type, value));
        return;
    }
    var_write(c, r, 0);
    c->reg_alt[r] = value;
    c->reg_alt_type[r] = type;
}

/* the register's bits as another 32-bit type, bitcast once a block */
static void ftz_mark(DirectCtx* c, u32 value);
static u32 reg_read_as(DirectCtx* c, u32 r, u32 type) {
    if (r >= 255) return spv_emit_bitcast(&c->emit, type, c->zero);
    if (c->var_block[r] == c->emit.block && c->reg_alt_type[r] == type) return c->reg_alt[r];
    u32 bits = reg_read(c, r);
    c->reg_alt[r] = spv_emit_bitcast(&c->emit, type, bits);
    c->reg_alt_type[r] = type;
    if (type == c->f32_type && c->taint_known && !c->tainted[r]) ftz_mark(c, c->reg_alt[r]);
    return c->reg_alt[r];
}

/* How a fragment shader's generic input interpolates (the SPH's imap: 1 constant, 2 perspective, 3 screen
   linear): as its first used component, and perspective when none is marked used, as Eden reads it */
static u32 ipa_interpolation(const u32* sph, u32 input) {
    u32 k, mode;
    for (k = 0; k < 4; k++) {
        mode = (sph[6 + input / 4] >> ((input % 4) * 8 + k * 2)) & 3;
        if (mode) return mode;
    }
    return 2;
}

/* a shared memory word index kept inside the array the shader declares: out of range is undefined on the console,
   and past an OpTypeArray in SPIR-V. A vertex of an input or output array is kept inside it the same way */
static u32 shared_index(DirectCtx* c, u32 index, u32 words) {
    u32 last = spv_emit_constant_u32(&c->emit, c->u32_type, words - 1);
    return spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, last, index), last, index);
}

/* a texture instruction's array layer: the low 16 bits of its register */
static u32 array_layer(DirectCtx* c, u32 r) {
    return spv_emit_convert_u_to_f(&c->emit, c->f32_type,
                                   spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reg_read(c, r),
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff)));
}

static u32 texture_component(DirectCtx* c, u32 sampled, u32 scalar_type, u32 component) {
    u32 value = spv_emit_vec4_extract(&c->emit, scalar_type, sampled, component);
    return scalar_type == c->u32_type ? value : spv_emit_bitcast(&c->emit, c->u32_type, value);
}

/* A depth-compare sample of a color texture (ShaderInfo.tex_dc_color_mask): its compare function (TSC's, 0 NEVER to
   7 ALWAYS), or -1 for a slot that samples a depth texture */
static int dc_color_func(const ShaderInfo* si, int slot) {
    if (!(si->tex_dc_color_mask >> slot & 1)) return -1;
    u32 index = (u32)__builtin_popcountll(si->tex_dc_color_mask & ((1ull << slot) - 1));
    return index < SH_DC_COLOR_MAX ? (int)(si->tex_dc_color_funcs >> (3 * index) & 7) : 3;
}
/* 1.0 when reference <func> texel passes, as a compare sampler returns (GL's order: the reference on the left) */
static u32 dc_compare(DirectCtx* c, int func, u32 reference, u32 texel) {
    static const u32 condition[8] = {0, 1, 2, 3, 4, 5, 6, 15}; /* spv_emit_fcompare's codes for NEVER..ALWAYS */
    u32 pass = spv_emit_fcompare(&c->emit, c->bool_type, condition[func], reference, texel);
    return spv_emit_select(&c->emit, c->f32_type, pass, spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f),
                           spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f));
}
static u32 indexed_metadata(DirectCtx* c, int slot) {
    u32 limit = spv_emit_constant_u32(&c->emit, c->u32_type, SNX_TEX_ARRAY - 1);
    u32 index = c->active_index;
    index = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, limit, index), limit, index);
    u32 element =
        spv_emit_shift_right_u32(&c->emit, c->u32_type, index, spv_emit_constant_u32(&c->emit, c->u32_type, 2));
    u32 component =
        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, index, spv_emit_constant_u32(&c->emit, c->u32_type, 3));
    u32 func = spv_emit_uniform_component(&c->emit, c->tex_func_pointer, c->tex_func_buffer, c->zero, element,
                                          component, c->u32_type);
    return func;
}
static u32 indexed_is_depth(DirectCtx* c, int slot) {
    u32 bit = spv_emit_constant_u32(&c->emit, c->u32_type, 8);
    return spv_emit_i_equal(&c->emit, c->bool_type,
                            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, indexed_metadata(c, slot), bit), bit);
}
static u32 dc_compare_indexed(DirectCtx* c, int slot, u32 reference, u32 texel) {
    u32 func = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, indexed_metadata(c, slot),
                                        spv_emit_constant_u32(&c->emit, c->u32_type, 7));
    u32 result = dc_compare(c, 3, reference, texel);
    for (int i = 0; i < 8; ++i) {
        if (i == 3) continue;
        u32 selected = spv_emit_i_equal(&c->emit, c->bool_type, func, spv_emit_constant_u32(&c->emit, c->u32_type, i));
        result = spv_emit_select(&c->emit, c->f32_type, selected, dc_compare(c, i, reference, texel), result);
    }
    return result;
}
/* a sample with a depth reference (dref 0: none), implicit lod or lod: compared in the shader on a color texture */
static u32 sample_dref(DirectCtx* c, const ShaderInfo* si, int slot, u32 vec4_type, u32 coords, u32 dref, int use_lod,
                       u32 lod) {
    int func = dref ? dc_color_func(si, slot) : -1;
    if (func < 0)
        return use_lod ? spv_emit_sample2d_lod(&c->emit, dref ? c->f32_type : vec4_type, c->sampled_type[slot],
                                               c->sampler[slot], coords, dref, lod)
                       : spv_emit_sample2d(&c->emit, dref ? c->f32_type : vec4_type, c->sampled_type[slot],
                                           c->sampler[slot], coords, dref);
    u32 texel =
        use_lod ? spv_emit_sample2d_lod(&c->emit, vec4_type, c->sampled_type[slot], c->sampler[slot], coords, 0, lod)
                : spv_emit_sample2d(&c->emit, vec4_type, c->sampled_type[slot], c->sampler[slot], coords, 0);
    return dc_compare(c, func, dref, spv_emit_vec4_extract(&c->emit, c->f32_type, texel, 0));
}

static u32 vm_select(DirectCtx* c, u32 i32_type, u32 value, u32 select) {
    u32 lane = select & 7;
    if (lane >= 6) return value;
    u32 width = lane < 4 ? 8 : 16;
    u32 offset = lane < 4 ? lane * 8 : (lane - 4) * 16;
    u32 off_id = spv_emit_constant_u32(&c->emit, c->u32_type, offset);
    u32 width_id = spv_emit_constant_u32(&c->emit, c->u32_type, width);
    return select & 8 ? spv_emit_bitcast(&c->emit, c->u32_type,
                                         spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                    spv_emit_bitcast(&c->emit, i32_type, value), off_id,
                                                                    width_id))
                      : spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, off_id, width_id);
}

/* A video operand as PTX's scalar video instructions take it: its byte, half or word (select & 7), sign-extended
   when select has bit 3, as a 64-bit integer (an s33) */
static u32 video_operand(DirectCtx* c, u32 i32_type, u32 i64_type, u32 u64_type, u32 value, u32 select) {
    u32 narrow = vm_select(c, i32_type, value, select);
    return select & 8 ? spv_emit_int_convert(&c->emit, i64_type, spv_emit_bitcast(&c->emit, i32_type, narrow), 1)
                      : spv_emit_bitcast(&c->emit, i64_type, spv_emit_int_convert(&c->emit, u64_type, narrow, 0));
}

/* The end of a scalar video instruction, as PTX defines it: the 64-bit result clamped to the destination's range
   (signed when dsigned, the width of the merged part) with saturate, then the secondary operation with C (bits
   51-53: 0 and 1 merge into C's high or low half, 2 and 3 into its byte 0 or 2, 4 adds C, 5 and 6 take the
   minimum or maximum, 7 none), and the low 32 bits */
static u32 video_finish(DirectCtx* c, u32 i32_type, u32 i64_type, u32 u64_type, u32 result, u32 third, u32 mode,
                        int saturate, int dsigned, int unsigned_nonnegative) {
    if (saturate) {
        u32 width = mode < 2 ? 16 : mode < 4 ? 8 : 32;
        u64 low = dsigned ? (u64)0 - (1ull << (width - 1)) : 0;
        u64 high = dsigned ? (1ull << (width - 1)) - 1 : (1ull << width) - 1;
        u32 low_id = spv_emit_constant64(&c->emit, i64_type, low),
            high_id = spv_emit_constant64(&c->emit, i64_type, high);
        if (!unsigned_nonnegative)
            result = spv_emit_select(&c->emit, i64_type, spv_emit_s_greater(&c->emit, c->bool_type, low_id, result),
                                     low_id, result);
        u32 above = unsigned_nonnegative
                        ? spv_emit_u_less(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, u64_type, high_id),
                                          spv_emit_bitcast(&c->emit, u64_type, result))
                        : spv_emit_s_greater(&c->emit, c->bool_type, result, high_id);
        result = spv_emit_select(&c->emit, i64_type, above, high_id, result);
    }
    if (mode >= 4 && mode <= 6) {
        u32 wide = dsigned ? spv_emit_int_convert(&c->emit, i64_type, spv_emit_bitcast(&c->emit, i32_type, third), 1)
                           : spv_emit_bitcast(&c->emit, i64_type, spv_emit_int_convert(&c->emit, u64_type, third, 0));
        if (mode == 4)
            result = spv_emit_iadd_u32(&c->emit, i64_type, result, wide);
        else {
            u32 greater = spv_emit_s_greater(&c->emit, c->bool_type, result, wide);
            result = spv_emit_select(&c->emit, i64_type, greater, mode == 5 ? wide : result, mode == 5 ? result : wide);
        }
    }
    u32 value = spv_emit_int_convert(&c->emit, c->u32_type, spv_emit_bitcast(&c->emit, u64_type, result), 0);
    if (mode < 4)
        value = spv_emit_bitfield_insert(&c->emit, c->u32_type, third, value,
                                         spv_emit_constant_u32(&c->emit, c->u32_type, mode == 0 || mode == 3 ? 16 : 0),
                                         spv_emit_constant_u32(&c->emit, c->u32_type, mode < 2 ? 16 : 8));
    return value;
}

static u32 xmad_half(DirectCtx* c, u32 i32_type, u32 value, int high, int is_signed) {
    u32 offset = spv_emit_constant_u32(&c->emit, c->u32_type, high ? 16 : 0);
    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 16);
    if (is_signed) {
        u32 signed_value = spv_emit_bitcast(&c->emit, i32_type, value);
        u32 half = spv_emit_bitfield_sextract(&c->emit, i32_type, signed_value, offset, width);
        return spv_emit_bitcast(&c->emit, c->u32_type, half);
    }
    return spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, offset, width);
}

static u32 global_atomic_opcode(u64 w, int reduction) {
    u32 size = reduction ? B(w, 20, 3) : B(w, 49, 3);
    u32 op = reduction ? B(w, 23, 3) : B(w, 52, 4);
    if (op == 10) op = 0; /* SAFEADD */
    if (size > 1 && size != 3) return 0;
    if (size == 3) return op == 8 ? 229 : op == 0 ? 0xffffffffu : 0;
    switch (op) {
        case 0: return 234;              /* IAdd */
        case 1: return size ? 236 : 237; /* SMin / UMin */
        case 2: return size ? 238 : 239; /* SMax / UMax */
        case 3:
        case 4: return 0xffffffffu; /* Inc / Dec use compare exchange. */
        case 5: return 240;         /* And */
        case 6: return 241;         /* Or */
        case 7: return 242;         /* Xor */
        case 8: return 229;         /* Exchange */
        default: return 0;
    }
}

static u32 shared_atomic_opcode(u64 w) {
    if (B(w, 28, 2) == 2 && B(w, 52, 4) == 8) return 0xfffffffeu; /* U64 exchange */
    if (B(w, 28, 2) > 1) return 0;
    switch (B(w, 52, 4)) {
        case 0: return 234;                     /* IAdd */
        case 1: return B(w, 28, 2) ? 236 : 237; /* SMin / UMin */
        case 2: return B(w, 28, 2) ? 238 : 239; /* SMax / UMax */
        case 3:
        case 4: return 0xffffffffu; /* Inc / Dec use compare exchange. */
        case 5: return 240;         /* And */
        case 6: return 241;         /* Or */
        case 7: return 242;         /* Xor */
        case 8: return 229;         /* Exchange */
        default: return 0;
    }
}

static u32 pred_read(DirectCtx* c, u32 p, int negative) {
    u32 value = p == 7 ? c->true_value : var_read(c, VAR_PRED + p);
    return negative ? spv_emit_bool_not(&c->emit, c->bool_type, value) : value;
}

static void pred_write(DirectCtx* c, u32 p, u32 value) {
    if (p >= 7) return;
    if (c->guard) value = spv_emit_select(&c->emit, c->bool_type, c->guard, value, pred_read(c, p, 0));
    var_write(c, VAR_PRED + p, value);
}

/* a condition code flag (its variable, c->cc_z...): its value, and written, kept as it was where a pure
   instruction's guard is false */
static u32 flag_read(DirectCtx* c, u32 flag) { return var_read(c, flag_var(c, flag)); }
static void cc_store(DirectCtx* c, u32 flag, u32 value) {
    if (c->guard) value = spv_emit_select(&c->emit, c->bool_type, c->guard, value, flag_read(c, flag));
    var_write(c, flag_var(c, flag), value);
}

/* Instructions that only compute registers, predicates and flags from registers, constants and immediates: no memory,
   no texture or attribute, no lanes other than their own, no flow. Run with their guard false they change nothing a
   select of their writes keeps, so a guarded one needs no branch around it */
static int pure_op(int op) {
    switch (op) {
        case O_BFE_C:
        case O_BFE_I:
        case O_BFE_R:
        case O_BFI_CR:
        case O_BFI_I:
        case O_BFI_R:
        case O_BFI_RC:
        case O_CS2R:
        case O_CSET:
        case O_CSETP:
        case O_DADD_C:
        case O_DADD_I:
        case O_DADD_R:
        case O_DFMA_CR:
        case O_DFMA_I:
        case O_DFMA_R:
        case O_DFMA_RC:
        case O_DMNMX_C:
        case O_DMNMX_I:
        case O_DMNMX_R:
        case O_DMUL_C:
        case O_DMUL_I:
        case O_DMUL_R:
        case O_DSET_C:
        case O_DSET_I:
        case O_DSETP_C:
        case O_DSETP_I:
        case O_DSETP_R:
        case O_DSET_R:
        case O_F2F_C:
        case O_F2F_I:
        case O_F2F_R:
        case O_F2I_C:
        case O_F2I_I:
        case O_F2I_R:
        case O_FADD32I:
        case O_FADD_C:
        case O_FADD_I:
        case O_FADD_R:
        case O_FCHK_C:
        case O_FCHK_I:
        case O_FCHK_R:
        case O_FCMP_CR:
        case O_FCMP_I:
        case O_FCMP_R:
        case O_FCMP_RC:
        case O_FFMA32I:
        case O_FFMA_CR:
        case O_FFMA_I:
        case O_FFMA_R:
        case O_FFMA_RC:
        case O_FLO_C:
        case O_FLO_I:
        case O_FLO_R:
        case O_FMNMX_C:
        case O_FMNMX_I:
        case O_FMNMX_R:
        case O_FMUL32I:
        case O_FMUL_C:
        case O_FMUL_I:
        case O_FMUL_R:
        case O_FSET_C:
        case O_FSET_I:
        case O_FSETP_C:
        case O_FSETP_I:
        case O_FSETP_R:
        case O_FSET_R:
        case O_HADD2_32I:
        case O_HADD2_C:
        case O_HADD2_I:
        case O_HADD2_R:
        case O_HFMA2_32I:
        case O_HFMA2_CR:
        case O_HFMA2_I:
        case O_HFMA2_R:
        case O_HFMA2_RC:
        case O_HMUL2_32I:
        case O_HMUL2_C:
        case O_HMUL2_I:
        case O_HMUL2_R:
        case O_HSET2_C:
        case O_HSET2_I:
        case O_HSET2_R:
        case O_HSETP2_C:
        case O_HSETP2_I:
        case O_HSETP2_R:
        case O_I2F_C:
        case O_I2F_I:
        case O_I2F_R:
        case O_I2I_C:
        case O_I2I_I:
        case O_I2I_R:
        case O_IADD32I:
        case O_IADD3_C:
        case O_IADD3_I:
        case O_IADD3_R:
        case O_IADD_C:
        case O_IADD_I:
        case O_IADD_R:
        case O_ICMP_CR:
        case O_ICMP_I:
        case O_ICMP_R:
        case O_ICMP_RC:
        case O_IMAD32I:
        case O_IMAD_CR:
        case O_IMAD_I:
        case O_IMAD_R:
        case O_IMAD_RC:
        case O_IMNMX_C:
        case O_IMNMX_I:
        case O_IMNMX_R:
        case O_IMUL32I:
        case O_IMUL_C:
        case O_IMUL_I:
        case O_IMUL_R:
        case O_ISCADD32I:
        case O_ISCADD_C:
        case O_ISCADD_I:
        case O_ISCADD_R:
        case O_ISET_C:
        case O_ISET_I:
        case O_ISETP_C:
        case O_ISETP_I:
        case O_ISETP_R:
        case O_ISET_R:
        case O_LEA_HC:
        case O_LEA_HR:
        case O_LEA_LC:
        case O_LEA_LI:
        case O_LEA_LR:
        case O_LOP32I:
        case O_LOP3_C:
        case O_LOP3_I:
        case O_LOP3_R:
        case O_LOP_C:
        case O_LOP_I:
        case O_LOP_R:
        case O_MOV32I:
        case O_MOV_C:
        case O_MOV_I:
        case O_MOV_R:
        case O_MUFU:
        case O_P2R_C:
        case O_P2R_I:
        case O_P2R_R:
        case O_POPC_C:
        case O_POPC_I:
        case O_POPC_R:
        case O_PRMT_CR:
        case O_PRMT_I:
        case O_PRMT_R:
        case O_PRMT_RC:
        case O_PSET:
        case O_PSETP:
        case O_RRO_C:
        case O_RRO_I:
        case O_RRO_R:
        case O_SEL_C:
        case O_SEL_I:
        case O_SEL_R:
        case O_SHF_LI:
        case O_SHF_LR:
        case O_SHF_RI:
        case O_SHF_RR:
        case O_SHL_C:
        case O_SHL_I:
        case O_SHL_R:
        case O_SHR_C:
        case O_SHR_I:
        case O_SHR_R:
        case O_VABSDIFF:
        case O_VADD:
        case O_VMAD:
        case O_VMNMX:
        case O_VSET:
        case O_VSETP:
        case O_VSHL:
        case O_VSHR:
        case O_XMAD_CR:
        case O_XMAD_I:
        case O_XMAD_R:
        case O_XMAD_RC:
            return 1;
        default: return 0;
    }
}

/* An FTZ float instruction whose result is flushed as it is written: no scale after the flush (FMUL), no FMZ
   select of an operand. Its destination is d alone */
static int ftz_writer(u64 w) {
    int id = decode(w)->id;
    if (id == O_FADD_R || id == O_FADD_C || id == O_FADD_I) return (int)B(w, 44, 1);
    if (id == O_FADD32I) return (int)B(w, 55, 1);
    if (id == O_FMUL_R || id == O_FMUL_C || id == O_FMUL_I) return (B(w, 44, 2) & 1) && !B(w, 41, 3);
    if (id == O_FMUL32I) return (int)(B(w, 53, 2) & 1);
    if (id == O_FFMA_R || id == O_FFMA_RC || id == O_FFMA_CR || id == O_FFMA_I || id == O_FFMA32I)
        return (int)(B(w, 53, 2) & 1);
    return 0;
}
/* whether w may write register r: its destinations taken wide, d to d+3 and the second field texture instructions
   write (bits 28 to 35) to it plus 3 */
static int may_write(u64 w, u32 r) {
    u32 d = B(w, 0, 8), e = B(w, 28, 8);
    return (d < 255 && r >= d && r - d < 4) || (e < 255 && r >= e && r - e < 4);
}
/* whether register r holds a flushed value when instruction i reads it: the last instruction that may write it in the
   straight-line code before i (no way in between) is an unpredicated FTZ float instruction, or, past where control
   may come in, nothing but FTZ float instructions writes it anywhere (tainted) */
static int reg_flushed_at(const DirectCtx* c, const u64* insn, const u8* block_start, u32 i, u32 r) {
    if (r >= 255) return 1; /* RZ: zero */
    if (!c->taint_known) return 0;
    if (!block_start || !block_start[i])
        for (u32 j = i; j-- > 11;) {
            if (!IDX_OK(j)) continue;
            if (may_write(insn[j], r)) return ftz_writer(insn[j]) && B(insn[j], 16, 3) == 7 && !B(insn[j], 19, 1);
            if (block_start && block_start[j]) break;
        }
    return !c->tainted[r];
}
/* value, a read of register r at instruction i, noted flushed when it is (reg_flushed_at) */
static u32 ftz_read(DirectCtx* c, const u64* insn, const u8* block_start, u32 i, u32 r, u32 value) {
    if (reg_flushed_at(c, insn, block_start, i, r)) ftz_mark(c, value);
    return value;
}

static u32 subgroup_ballot_word(DirectCtx* c, u32 vec4_u32, u32 lane, u32 predicate) {
    u32 mask = spv_emit_subgroup_ballot(&c->emit, vec4_u32, c->u32_type, predicate);
    u32 word = spv_emit_shift_right_u32(&c->emit, c->u32_type, lane, spv_emit_constant_u32(&c->emit, c->u32_type, 5));
    u32 result = spv_emit_vec4_extract(&c->emit, c->u32_type, mask, 0);
    for (u32 i = 1; i < 4; i++)
        result = spv_emit_select(
            &c->emit, c->u32_type,
            spv_emit_i_equal(&c->emit, c->bool_type, word, spv_emit_constant_u32(&c->emit, c->u32_type, i)),
            spv_emit_vec4_extract(&c->emit, c->u32_type, mask, i), result);
    return result;
}

static u32 integer_add(DirectCtx* c, u32 left, u32 right, u32 avg, int extended, int write_cc) {
    if (!extended && !write_cc) {
        if (avg == 2) left = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, left);
        if (avg == 1) right = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, right);
        u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, left, right);
        return avg == 3 ? spv_emit_iadd_u32(&c->emit, c->u32_type, sum, spv_emit_constant_u32(&c->emit, c->u32_type, 1))
                        : sum;
    }
    if (avg == 2) left = spv_emit_not_u32(&c->emit, c->u32_type, left);
    if (avg == 1) right = spv_emit_not_u32(&c->emit, c->u32_type, right);
    u32 sum0 = spv_emit_iadd_u32(&c->emit, c->u32_type, left, right);
    u32 carry0 = spv_emit_u_less(&c->emit, c->bool_type, sum0, left);
    u32 cin = avg ? spv_emit_constant_u32(&c->emit, c->u32_type, 1) : c->zero;
    if (extended) {
        u32 old_carry = flag_read(c, c->cc_c);
        u32 extra =
            spv_emit_select(&c->emit, c->u32_type, old_carry, spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero);
        cin = spv_emit_iadd_u32(&c->emit, c->u32_type, cin, extra);
    }
    u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, sum0, cin);
    if (write_cc) {
        u32 carry1 = spv_emit_u_less(&c->emit, c->bool_type, sum, sum0);
        u32 carry = spv_emit_bool_binary(&c->emit, c->bool_type, 1, carry0, carry1);
        u32 signs = spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, sum, left);
        u32 same =
            spv_emit_not_u32(&c->emit, c->u32_type, spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, left, right));
        u32 bits = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, signs, same);
        u32 top = spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000u);
        u32 overflow = spv_emit_i_not_equal(&c->emit, c->bool_type,
                                            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, bits, top), c->zero);
        u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, sum, c->zero);
        if (extended)
            zero =
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, zero, flag_read(c, c->cc_z));
        u32 sign = spv_emit_i_not_equal(&c->emit, c->bool_type,
                                        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sum, top), c->zero);
        cc_store(c, c->cc_c, carry);
        cc_store(c, c->cc_o, overflow);
        cc_store(c, c->cc_z, zero);
        cc_store(c, c->cc_s, sign);
    }
    return sum;
}

static void flags_logic(DirectCtx* c, u32 value) {
    u32 sign = spv_emit_i_not_equal(&c->emit, c->bool_type,
                                    spv_emit_bitwise_and_u32(&c->emit, c->u32_type, value,
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000u)),
                                    c->zero);
    cc_store(c, c->cc_c, c->false_value);
    cc_store(c, c->cc_o, c->false_value);
    cc_store(c, c->cc_z, spv_emit_i_equal(&c->emit, c->bool_type, value, c->zero));
    cc_store(c, c->cc_s, sign);
}

/* LOP, LOP32I and LOP3 flags: X retains zero only when the previous word was also zero, as IADD.X chains across
   the words of a wider value. */
static void flags_logic_extended(DirectCtx* c, u32 value, int extended) {
    u32 previous = extended ? flag_read(c, c->cc_z) : 0;
    u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, value, c->zero);
    if (extended) zero = spv_emit_bool_binary(&c->emit, c->bool_type, 0, previous, zero);
    u32 sign = spv_emit_i_not_equal(&c->emit, c->bool_type,
                                    spv_emit_bitwise_and_u32(&c->emit, c->u32_type, value,
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000u)),
                                    c->zero);
    cc_store(c, c->cc_z, zero);
    cc_store(c, c->cc_s, sign);
}

static u32 bool_combine(DirectCtx* c, u32 op, u32 left, u32 right) {
    return op < 3 ? spv_emit_bool_binary(&c->emit, c->bool_type, op, left, right) : left;
}

static int cc_valid(u32 code) {
    return code <= 14 || code == 15 || (code >= 16 && code <= 23) || code == 30 || code == 31;
}
static int flow_has_cc(int op) {
    return op == O_BRA || op == O_BRX || op == O_SYNC || op == O_BRK || op == O_CONT || op == O_EXIT || op == O_RET ||
           op == O_KIL;
}
static int helper_side_effect(int op) {
    return op == O_STG || op == O_STL || op == O_STS || op == O_ST || op == O_STP || op == O_RED || op == O_ATOM ||
           op == O_ATOM_CAS || op == O_ATOMS || op == O_ATOMS_CAS || op == O_SUST || op == O_SURED || op == O_SUATOM ||
           op == O_SUATOM_CAS;
}
static u32 cc_not(DirectCtx* c, u32 x) { return spv_emit_bool_not(&c->emit, c->bool_type, x); }
static u32 cc_and(DirectCtx* c, u32 x, u32 y) { return spv_emit_bool_binary(&c->emit, c->bool_type, 0, x, y); }
/* a lane whose writes must not land: a helper, or one a KIL inside the flow loop marked */
static u32 lane_dead(DirectCtx* c) {
    u32 helper = spv_emit_is_helper(&c->emit, c->bool_type);
    if (!c->discarded) return helper;
    return spv_emit_bool_binary(&c->emit, c->bool_type, 1, helper, spv_emit_load(&c->emit, c->bool_type, c->discarded));
}
static u32 cc_or(DirectCtx* c, u32 x, u32 y) { return spv_emit_bool_binary(&c->emit, c->bool_type, 1, x, y); }
static u32 cc_xor(DirectCtx* c, u32 x, u32 y) { return spv_emit_bool_binary(&c->emit, c->bool_type, 2, x, y); }
static u32 cc_test(DirectCtx* c, u32 code) {
    if (code == 0 || code == 15) return code == 15 ? c->true_value : c->false_value;
    u32 s = flag_read(c, c->cc_s);
    u32 z = flag_read(c, c->cc_z);
    u32 carry = flag_read(c, c->cc_c);
    u32 overflow = flag_read(c, c->cc_o);
    switch (code) {
        case 1: return cc_xor(c, cc_and(c, s, cc_not(c, z)), overflow);
        case 2: return cc_and(c, cc_not(c, s), z);
        case 3: return cc_xor(c, s, cc_or(c, z, overflow));
        case 4: return cc_not(c, cc_or(c, cc_xor(c, s, overflow), z));
        case 5: return cc_not(c, z);
        case 6: return cc_not(c, cc_xor(c, s, overflow));
        case 7: return cc_not(c, cc_and(c, s, z));
        case 8: return cc_and(c, s, z);
        case 9: return cc_xor(c, s, overflow);
        case 10: return z;
        case 11: return cc_or(c, cc_xor(c, s, overflow), z);
        case 12: return cc_xor(c, cc_not(c, s), cc_or(c, overflow, z));
        case 13: return cc_or(c, s, cc_not(c, z));
        case 14: return cc_xor(c, cc_or(c, cc_not(c, s), z), overflow);
        case 16: return cc_not(c, overflow);
        case 17: return cc_not(c, carry);
        case 18: return cc_not(c, s);
        case 19: return cc_or(c, z, cc_not(c, carry));
        case 20: return cc_and(c, carry, cc_not(c, z));
        case 21: return s;
        case 22: return carry;
        case 23: return overflow;
        case 30: return cc_or(c, s, z);
        case 31: return cc_not(c, cc_or(c, s, z));
        default: return c->false_value;
    }
}

static u32 integer_compare(DirectCtx* c, u32 i32_type, u64 w, u32 left, u32 right) {
    u32 cmp = B(w, 49, 3);
    if (cmp == 0 || cmp == 7) return cmp == 7 ? c->true_value : c->false_value;
    if (B(w, 43, 1)) {
        /* the high words decide unless equal: LE and GE take LT and GT there, equality goes to the carry */
        u64 plain = w & ~(1ull << 43);
        u64 strict = cmp == 3 ? 1 : cmp == 6 ? 4 : cmp;
        u32 base = integer_compare(c, i32_type, (plain & ~(7ull << 49)) | (strict << 49), left, right);
        u32 equal = integer_compare(c, i32_type, (plain & ~(7ull << 49)) | (2ull << 49), left, right);
        u32 carry = flag_read(c, c->cc_c);
        u32 zero = flag_read(c, c->cc_z);
        u32 no_carry = spv_emit_bool_not(&c->emit, c->bool_type, carry);
        if (cmp == 1 || cmp == 6)
            return spv_emit_bool_binary(
                &c->emit, c->bool_type, 1, base,
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal, cmp == 1 ? no_carry : carry));
        if (cmp == 2) return spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal, zero);
        if (cmp == 3)
            return spv_emit_bool_binary(
                &c->emit, c->bool_type, 1, base,
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal,
                                     spv_emit_bool_binary(&c->emit, c->bool_type, 1, zero, no_carry)));
        if (cmp == 4)
            return spv_emit_bool_binary(
                &c->emit, c->bool_type, 1, base,
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal,
                                     spv_emit_bool_binary(&c->emit, c->bool_type, 0,
                                                          spv_emit_bool_not(&c->emit, c->bool_type, zero), carry)));
        if (cmp == 5)
            return spv_emit_bool_binary(&c->emit, c->bool_type, 1, base,
                                        spv_emit_bool_not(&c->emit, c->bool_type, zero));
    }
    if (cmp == 2) return spv_emit_i_equal(&c->emit, c->bool_type, left, right);
    if (cmp == 5) return spv_emit_i_not_equal(&c->emit, c->bool_type, left, right);
    u32 less, greater;
    if (B(w, 48, 1)) {
        u32 lhs = spv_emit_bitcast(&c->emit, i32_type, left);
        u32 rhs = spv_emit_bitcast(&c->emit, i32_type, right);
        less = spv_emit_s_greater(&c->emit, c->bool_type, rhs, lhs);
        greater = spv_emit_s_greater(&c->emit, c->bool_type, lhs, rhs);
    } else {
        less = spv_emit_u_less(&c->emit, c->bool_type, left, right);
        greater = spv_emit_u_less(&c->emit, c->bool_type, right, left);
    }
    if (cmp == 1) return less;
    if (cmp == 4) return greater;
    return spv_emit_bool_not(&c->emit, c->bool_type, cmp == 3 ? greater : less);
}

/* whether value is one float_ftz made, or one known to have no subnormal */
static int ftz_has(const DirectCtx* c, u32 value) {
    return (size_t)value / 8 < c->ftz_bytes && (c->ftz[value / 8] >> (value % 8) & 1);
}
static void ftz_mark(DirectCtx* c, u32 value) {
    if ((size_t)value / 8 >= c->ftz_bytes) {
        size_t bytes = ((size_t)value / 8 + 1) * 2;
        u8* grown = (u8*)realloc(c->ftz, bytes);
        if (!grown) return; /* not noted: flushed again where it is used */
        memset(grown + c->ftz_bytes, 0, bytes - c->ftz_bytes);
        c->ftz = grown;
        c->ftz_bytes = bytes;
    }
    c->ftz[value / 8] |= (u8)(1u << (value % 8));
}

/* the absolute value and negation of a value with no subnormal have none either */
static u32 float_mod(DirectCtx* c, u32 value, int absolute, int negative) {
    int flushed = ftz_has(c, value);
    if (absolute) value = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_ABS, value);
    if (negative) value = spv_emit_fnegate(&c->emit, c->f32_type, value);
    if (flushed && (absolute || negative)) ftz_mark(c, value);
    return value;
}

/* An FTZ arithmetic instruction consumes and produces signed zero instead of a subnormal. Done here per operand and
   result when the Vulkan device can't request DenormFlushToZero, or when this shader mixes FTZ and non-FTZ
   instructions: with a float test where the module keeps signed zeros, else on the bits (spv_emit_flush_subnormal) */
static u32 float_ftz(DirectCtx* c, u32 value) {
    u32 type, constant;
    /* a value flushed once has no subnormal left, wherever it is used; a constant is flushed here */
    if (ftz_has(c, value)) return value;
    if (spv_emit_constant_bits(&c->emit, value, &type, &constant) && type == c->f32_type)
        return spv_emit_constant_u32(&c->emit, c->f32_type,
                                     constant & 0x7f800000u ? constant : constant & 0x80000000u);
    if (value < c->ftz_of_cap && c->ftz_of[value] && c->ftz_of_block[value] == c->emit.block) return c->ftz_of[value];
    u32 result = spv_emit_flush_subnormal(&c->emit, c->f32_type, c->u32_type, c->bool_type, c->glsl, value, c->ftz_float);
    ftz_mark(c, result);
    if (value >= c->ftz_of_cap) {
        size_t cap = ((size_t)value + 1) * 2;
        u32* of = (u32*)realloc(c->ftz_of, cap * sizeof *of);
        u32* block = of ? (u32*)realloc(c->ftz_of_block, cap * sizeof *block) : NULL;
        if (of) c->ftz_of = of;
        if (!block) return result; /* not noted: flushed again where it is used */
        memset(of + c->ftz_of_cap, 0, (cap - c->ftz_of_cap) * sizeof *of);
        c->ftz_of_block = block;
        c->ftz_of_cap = cap;
    }
    c->ftz_of[value] = result;
    c->ftz_of_block[value] = c->emit.block;
    return result;
}

static u32 double_read(DirectCtx* c, u32 vec2_u32, u32 index) {
    u32 low = reg_read(c, index);
    u32 high = reg_read(c, index < 254 ? index + 1 : 255);
    u32 bits = spv_emit_vec2_construct(&c->emit, vec2_u32, low, high);
    return spv_emit_bitcast(&c->emit, c->f64_type, bits);
}

static u32 double_mod(DirectCtx* c, u32 value, int absolute, int negative) {
    if (absolute) value = spv_emit_glsl_unary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_F_ABS, value);
    if (negative) value = spv_emit_fnegate(&c->emit, c->f64_type, value);
    return value;
}

static void double_write(DirectCtx* c, u32 vec2_u32, u32 index, u32 value) {
    u32 bits = spv_emit_bitcast(&c->emit, vec2_u32, value);
    reg_write(c, index, spv_emit_vec4_extract(&c->emit, c->u32_type, bits, 0));
    if (index < 254) reg_write(c, index + 1, spv_emit_vec4_extract(&c->emit, c->u32_type, bits, 1));
}

static u32 half_pair(DirectCtx* c, u32 vec2, u32 raw, u32 swizzle) {
    if (swizzle == 1) {
        u32 scalar = spv_emit_bitcast(&c->emit, c->f32_type, raw);
        return spv_emit_vec2_construct(&c->emit, vec2, scalar, scalar);
    }
    u32 unpacked = spv_emit_glsl_unary(&c->emit, vec2, c->glsl, SPV_GLSL_UNPACK_HALF2X16, raw);
    if (swizzle == 0) return unpacked;
    u32 scalar = spv_emit_vec4_extract(&c->emit, c->f32_type, unpacked, swizzle == 3);
    return spv_emit_vec2_construct(&c->emit, vec2, scalar, scalar);
}

static u32 half_pair_mod(DirectCtx* c, u32 vec2, u32 pair, int absolute, int negative) {
    if (absolute) pair = spv_emit_glsl_unary(&c->emit, vec2, c->glsl, SPV_GLSL_F_ABS, pair);
    if (negative) pair = spv_emit_fnegate(&c->emit, vec2, pair);
    return pair;
}

/* The two halves of an HADD2, HMUL2, HFMA2, HSET2 or HSETP2 immediate: 9 bits of each half's exponent and mantissa
   (bits 20 and 30, from the half's bit 6) and their signs (bits 29 and 56) */
static u32 half_immediate(DirectCtx* c, u64 w) {
    u32 low = (B(w, 20, 9) << 6) | (B(w, 29, 1) << 15);
    u32 high = (B(w, 30, 9) << 6) | (B(w, 56, 1) << 15);
    return spv_emit_constant_u32(&c->emit, c->u32_type, low | (high << 16));
}

/* HMUL2's and HFMA2's FMZ precision (2): a product with a zero factor is zero, even with an infinity or a NaN, so a
   lane whose A or B is zero gets zero_value's lane */
static u32 half_fmz(DirectCtx* c, u32 vec2, u32 pair, u32 lhs, u32 rhs, u32 zero_value) {
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f), lanes[2];
    for (u32 lane = 0; lane < 2; lane++) {
        u32 factor_a = spv_emit_vec4_extract(&c->emit, c->f32_type, lhs, lane);
        u32 factor_b = spv_emit_vec4_extract(&c->emit, c->f32_type, rhs, lane);
        u32 any = spv_emit_bool_binary(&c->emit, c->bool_type, 1,
                                       spv_emit_fcompare(&c->emit, c->bool_type, 2, factor_a, zero),
                                       spv_emit_fcompare(&c->emit, c->bool_type, 2, factor_b, zero));
        lanes[lane] =
            spv_emit_select(&c->emit, c->f32_type, any,
                            zero_value ? spv_emit_vec4_extract(&c->emit, c->f32_type, zero_value, lane) : zero,
                            spv_emit_vec4_extract(&c->emit, c->f32_type, pair, lane));
    }
    return spv_emit_vec2_construct(&c->emit, vec2, lanes[0], lanes[1]);
}

static u32 half_pair_result(DirectCtx* c, u32 vec2, u32 pair, u32 old, u32 merge, int saturate) {
    if (saturate) {
        u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
        u32 one = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
        pair = spv_emit_glsl_ternary(&c->emit, vec2, c->glsl, SPV_GLSL_F_CLAMP, pair,
                                     spv_emit_vec2_construct(&c->emit, vec2, zero, zero),
                                     spv_emit_vec2_construct(&c->emit, vec2, one, one));
    }
    if (merge == 1)
        return spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_vec4_extract(&c->emit, c->f32_type, pair, 0));
    u32 packed = spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16, pair);
    if (merge == 2 || merge == 3) {
        u32 mask = spv_emit_constant_u32(&c->emit, c->u32_type, merge == 2 ? 0xffffu : 0xffff0000u);
        return spv_emit_bitwise_or_u32(
            &c->emit, c->u32_type,
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, old, spv_emit_not_u32(&c->emit, c->u32_type, mask)),
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, packed, mask));
    }
    return packed;
}

static u32 float_imm20(DirectCtx* c, u64 w) {
    return spv_emit_constant_u32(&c->emit, c->f32_type, (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31));
}

static u32 float_imm32(DirectCtx* c, u64 w) { return spv_emit_constant_u32(&c->emit, c->f32_type, B(w, 20, 32)); }

static u32 float_saturate(DirectCtx* c, u32 value, int saturate) {
    if (!saturate) return value;
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    u32 one = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
    return spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_CLAMP, value, zero, one);
}

static void alpha_test(DirectCtx* c, u32 push, u32 push_pointer, u32 alpha) {
    u32 index[3];
    for (u32 i = 0; i < 3; i++) index[i] = spv_emit_constant_u32(&c->emit, c->u32_type, i);
    u32 enabled = spv_emit_push_component_f32(&c->emit, push_pointer, push, index[0], index[0], c->f32_type);
    u32 function = spv_emit_push_component_f32(&c->emit, push_pointer, push, index[0], index[1], c->f32_type);
    u32 reference = spv_emit_push_component_f32(&c->emit, push_pointer, push, index[0], index[2], c->f32_type);
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    u32 active = spv_emit_fcompare(&c->emit, c->bool_type, 5, enabled, zero);
    u32 pass =
        spv_emit_fcompare(&c->emit, c->bool_type, 2, function, spv_emit_constant_f32(&c->emit, c->f32_type, 7.0f));
    for (u32 mode = 1; mode <= 6; mode++) {
        u32 tag = spv_emit_constant_f32(&c->emit, c->f32_type, (float)mode);
        u32 selected = spv_emit_fcompare(&c->emit, c->bool_type, 2, function, tag);
        u32 comparison = spv_emit_fcompare(&c->emit, c->bool_type, mode, alpha, reference);
        u32 result = spv_emit_bool_binary(&c->emit, c->bool_type, 0, selected, comparison);
        pass = spv_emit_bool_binary(&c->emit, c->bool_type, 1, pass, result);
    }
    u32 fail = spv_emit_bool_not(&c->emit, c->bool_type, pass);
    fail = spv_emit_bool_binary(&c->emit, c->bool_type, 0, active, fail);
    if (c->discarded) { /* in the flow loop: discarded after it */
        spv_emit_store(
            &c->emit, c->discarded,
            spv_emit_bool_binary(&c->emit, c->bool_type, 1, spv_emit_load(&c->emit, c->bool_type, c->discarded), fail));
        return;
    }
    u32 discard = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
    spv_emit_selection_merge(&c->emit, merge);
    spv_emit_branch_conditional(&c->emit, fail, discard, merge);
    spv_emit_label(&c->emit, discard);
    spv_emit_kill(&c->emit);
    spv_emit_label(&c->emit, merge);
}

/* Fragment fetches from a scaled render target (resolution=): texel p is at (p + 0.5) * scale, and TXQ answers the
   size over the scale. The texture's bit is in the push constants' uvec4, x for slots 0-31, y for 32-63 */
_Static_assert(SH_MAXTEX <= 64, "the scaled textures' mask has a bit a slot");
static void scaled_coords(DirectCtx* c, u32 push, int slot, int size, u32 i32_type, u32* x, u32* y) {
    u32 one = spv_emit_constant_u32(&c->emit, c->u32_type, 1);
    u32 bits = spv_emit_push_component_f32(&c->emit, spv_emit_type_pointer(&c->emit, 9, c->u32_type), push, one,
                                           slot >= 32 ? one : c->zero, c->u32_type); /* PushConstant */
    u32 bit = spv_emit_bitwise_and_u32(
        &c->emit, c->u32_type,
        spv_emit_shift_right_u32(&c->emit, c->u32_type, bits, spv_emit_constant_u32(&c->emit, c->u32_type, slot & 31)),
        one);
    /* component 2 is the scale, 0 its reciprocal (frame_consts) */
    u32 factor = spv_emit_uniform_component(&c->emit, c->scale_pointer, c->scale_buffer, c->zero, c->zero,
                                            size ? c->zero : spv_emit_constant_u32(&c->emit, c->u32_type, 2),
                                            c->f32_type);
    factor = spv_emit_select(&c->emit, c->f32_type, spv_emit_i_not_equal(&c->emit, c->bool_type, bit, c->zero), factor,
                             spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f));
    u32 half = spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f);
    u32* v[2] = {x, y};
    for (int k = 0; k < 2; k++) {
        u32 f = size ? spv_emit_convert_u_to_f(&c->emit, c->f32_type, *v[k])
                     : spv_emit_fadd(&c->emit, c->f32_type, spv_emit_convert_s_to_f(&c->emit, c->f32_type, *v[k]), half);
        spv_emit_no_contraction(&c->emit, f);
        f = spv_emit_fmul(&c->emit, c->f32_type, f, factor);
        spv_emit_no_contraction(&c->emit, f);
        if (size) { /* rounded, as vk_sc */
            f = spv_emit_fadd(&c->emit, c->f32_type, f, half);
            spv_emit_no_contraction(&c->emit, f);
            *v[k] = spv_emit_convert_f_to_u(&c->emit, c->u32_type, f);
        } else /* floored: below 0 stays outside */
            *v[k] = spv_emit_convert_f_to_s(&c->emit, i32_type,
                                            spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FLOOR, f));
    }
}

static void vertex_depth_fix(DirectCtx* c, u32 position, u32 push, u32 push_pointer, u32 vec4_type) {
    u32 member = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
    u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, 1);
    u32 enabled = spv_emit_push_component_f32(&c->emit, push_pointer, push, member, component, c->f32_type);
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    u32 test = spv_emit_fcompare(&c->emit, c->bool_type, 5, enabled, zero);
    u32 adjust = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
    spv_emit_selection_merge(&c->emit, merge);
    spv_emit_branch_conditional(&c->emit, test, adjust, merge);
    spv_emit_label(&c->emit, adjust);
    u32 value = spv_emit_load(&c->emit, vec4_type, position);
    u32 z = spv_emit_vec4_extract(&c->emit, c->f32_type, value, 2);
    u32 w = spv_emit_vec4_extract(&c->emit, c->f32_type, value, 3);
    u32 sum = spv_emit_fadd(&c->emit, c->f32_type, z, w);
    spv_emit_no_contraction(&c->emit, sum);
    u32 half = spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f);
    u32 depth = spv_emit_fmul(&c->emit, c->f32_type, sum, half);
    spv_emit_no_contraction(&c->emit, depth);
    value = spv_emit_vec4_insert(&c->emit, vec4_type, value, depth, 2);
    spv_emit_store(&c->emit, position, value);
    spv_emit_branch(&c->emit, merge);
    spv_emit_label(&c->emit, merge);
}

/* Transform feedback: each captured output's private vec4 (ShaderInfo.xfb) copied into its component outputs, the
   variables that carry the capture's offsets, before the program ends or emits a vertex */
static void xfb_store(DirectCtx* c, u32 vec4, const u32* output_attrs, const u8* attr_private, u32 (*components)[4]) {
    for (u32 i = 0; i < 32; i++) {
        if (!attr_private[i]) continue;
        u32 value = spv_emit_load(&c->emit, vec4, output_attrs[i]);
        for (u32 k = 0; k < 4; k++)
            spv_emit_store(&c->emit, components[i][k], spv_emit_vec4_extract(&c->emit, c->f32_type, value, k));
    }
}

/* Hardware passthrough emits once at program exit. Save generated outputs before EmitVertex,
   which makes every output undefined, and merge forwarded components afresh for each vertex. */
static void geometry_passthrough(DirectCtx* c, const ShaderInfo* si, u32 vertices, u32 vec4, u32 position,
                                 u32 position_in, const u32* outputs, const u32* inputs, u32 point_size, u32 layer,
                                 u32 layer_type, u32 clip, u32 clip_count, u32 primitive_id, u32 primitive_id_out,
                                 int depth_fix, u32 push, u32 push_pointer) {
    u32 saved[33] = {0}, saved_clip[8] = {0};
    for (u32 a = 0; a < 33; a++) {
        u32 out = a ? outputs[a - 1] : position;
        if (out) saved[a] = spv_emit_load(&c->emit, vec4, out);
    }
    u32 size = point_size ? spv_emit_load(&c->emit, c->f32_type, point_size) : 0;
    u32 saved_layer = layer ? spv_emit_load(&c->emit, layer_type, layer) : 0;
    for (u32 k = 0; k < clip_count; k++)
        saved_clip[k] = spv_emit_load(&c->emit, c->f32_type,
                                      spv_emit_output_element_pointer(&c->emit, c->f32_type, clip,
                                                                      spv_emit_constant_u32(&c->emit, c->u32_type, k)));
    u32 id = spv_emit_load(&c->emit, c->u32_type, primitive_id);
    u32 count = vertices == 4 ? 2 : vertices == 6 ? 3 : vertices;
    for (u32 v = 0; v < count; v++) {
        u32 index = spv_emit_constant_u32(&c->emit, c->u32_type, vertices == 4 ? v + 1 : vertices == 6 ? v * 2 : v);
        for (u32 a = 0; a < 33; a++) {
            u32 out = a ? outputs[a - 1] : position;
            if (!out) continue;
            u32 in = a ? inputs[a - 1] : position_in;
            u32 attr = a ? 32 + (a - 1) * 4 : 28;
            u32 mask = (~si->gp_skip_mask[attr / 32] >> (attr % 32)) & 15;
            u32 value = saved[a];
            for (u32 k = 0; k < 4; k++) {
                if (!(mask & (1u << k))) continue;
                u32 ptr = spv_emit_interface_scalar_pointer(&c->emit, 1, c->f32_type, in, index,
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, k));
                value = spv_emit_vec4_insert(&c->emit, vec4, value, spv_emit_load(&c->emit, c->f32_type, ptr), k);
            }
            spv_emit_store(&c->emit, out, value);
        }
        if (point_size) spv_emit_store(&c->emit, point_size, size);
        if (layer) spv_emit_store(&c->emit, layer, saved_layer);
        for (u32 k = 0; k < clip_count; k++)
            spv_emit_store(&c->emit,
                           spv_emit_output_element_pointer(&c->emit, c->f32_type, clip,
                                                           spv_emit_constant_u32(&c->emit, c->u32_type, k)),
                           saved_clip[k]);
        spv_emit_store(&c->emit, primitive_id_out, id);
        if (depth_fix) vertex_depth_fix(c, position, push, push_pointer, vec4);
        spv_emit_emit_vertex(&c->emit);
    }
    spv_emit_end_primitive(&c->emit);
}

/* A constant buffer word is loaded as an integer: a float load may flush the bits of an integer that look like a
   denormal (Adreno does, in a shader that declares DenormFlushToZero), and a jump table of small offsets reads 0. */
/* the cache entry of instruction w's constant buffer operand, read in this block if it wasn't */
static u32 cbuf_entry(DirectCtx* c, u64 w) {
    u32 idx = B(w, 34, 5), off = B(w, 20, 14) * 4, key = (idx << 16 | off >> 2) + 1;
    u32 at = (key * 2654435761u) >> 23; /* 9 bits */
    if (c->cbuf_seen[at].key == key && c->cbuf_seen[at].block == c->emit.block) return at;
    u32 member = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
    u32 element = spv_emit_constant_u32(&c->emit, c->u32_type, off >> 4);
    u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, (off >> 2) & 3);
    c->cbuf_seen[at].key = key;
    c->cbuf_seen[at].block = c->emit.block;
    c->cbuf_seen[at].word = spv_emit_uniform_component(&c->emit, c->cbuf_pointer[idx], c->cbuf[idx], member, element,
                                                       component, c->u32_type);
    c->cbuf_seen[at].as_float = 0;
    return at;
}
static u32 cbuf_word(DirectCtx* c, u64 w) { return c->cbuf_seen[cbuf_entry(c, w)].word; }
static u32 cbuf_read(DirectCtx* c, u64 w) {
    u32 at = cbuf_entry(c, w);
    if (!c->cbuf_seen[at].as_float)
        c->cbuf_seen[at].as_float = spv_emit_bitcast(&c->emit, c->f32_type, c->cbuf_seen[at].word);
    return c->cbuf_seen[at].as_float;
}
static u32 integer_right(DirectCtx* c, u64 w, int kind) {
    if (kind == 0) return reg_read(c, B(w, 20, 8));
    if (kind == 1) return cbuf_word(c, w);
    u32 imm = B(w, 20, 19);
    return spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 56, 1) ? imm | 0xfff80000u : imm);
}

static const SpvGlsl450Op conversion_rounds[] = {SPV_GLSL_ROUND_EVEN, SPV_GLSL_FLOOR, SPV_GLSL_CEIL, SPV_GLSL_TRUNC};

/* A conversion's register, constant-buffer or immediate source, unpacked from F16 when requested. */
static u32 conversion_source_f32(DirectCtx* c, u32 vec2, u64 w, int kind, u32 format) {
    u32 raw = kind == 2 ? spv_emit_bitcast(&c->emit, c->u32_type, float_imm20(c, w)) : integer_right(c, w, kind);
    if (format != 1) return spv_emit_bitcast(&c->emit, c->f32_type, raw);
    u32 unpacked = spv_emit_glsl_unary(&c->emit, vec2, c->glsl, SPV_GLSL_UNPACK_HALF2X16, raw);
    return spv_emit_vec4_extract(&c->emit, c->f32_type, unpacked, B(w, 41, 1));
}
/* Two's-complement signed operands correct the unsigned high word by subtracting
   the other operand when their sign bit is set. The low word needs no correction. */
static u32 integer_mul_high(DirectCtx* c, u32 lhs, u32 rhs, int signed_lhs, int signed_rhs) {
    u32 high = spv_emit_umul_high_u32(&c->emit, c->u32_type, lhs, rhs);
    u32 sign = spv_emit_constant_u32(&c->emit, c->u32_type, 31);
    if (signed_lhs)
        high = spv_emit_isub_u32(
            &c->emit, c->u32_type, high,
            spv_emit_imul_u32(&c->emit, c->u32_type, rhs, spv_emit_shift_right_u32(&c->emit, c->u32_type, lhs, sign)));
    if (signed_rhs)
        high = spv_emit_isub_u32(
            &c->emit, c->u32_type, high,
            spv_emit_imul_u32(&c->emit, c->u32_type, lhs, spv_emit_shift_right_u32(&c->emit, c->u32_type, rhs, sign)));
    return high;
}
static int gbuf_tag(int stage, u32 index, u32 offset) {
    /* The vertex system buffer also supplies a bare 64-bit output pointer at 0xb0. */
    if (stage == ST_VS && index == 0 && offset == 0xb0) return 0xb1;
    return index == 0 && shader_gbuf_sized(stage, offset) && !(offset & 15) ? (int)offset + 1 : 0;
}
/* A bare 64-bit address in a constant buffer of the program's own (1 to 17: cbuf 0 is NVN's, its descriptors and
   system values), 16-byte aligned as Eden requires of one (SH_GBUF_*). Taken only where neither an NVN descriptor
   (gbuf_tag) nor a register already holding a base explains the access, and not for compute dispatches, which copy
   each buffer whole and would have no size for one. Without it such a graphics program wasn't translated, and its
   draws were skipped: one game's terrain, all of it */
static int gbuf_tag_any(int stage, u32 index, u32 offset) {
    return stage != ST_CS && index > 0 && index < 18 && offset < 0x10000 && !(offset & 15)
               ? (int)((index << 16) | offset) + 1
               : 0;
}
static int texs_outputs(u64 w, u32 regs[4], u32 comps[4]) {
    u32 da = B(w, 0, 8), db = B(w, 28, 8), sw = B(w, 50, 3);
    int n = 0;
    int i;
    if (db == 255) {
        static const int tab[8][2] = {{0, -1}, {1, -1}, {2, -1}, {3, -1}, {0, 1}, {0, 3}, {1, 3}, {2, 3}};
        for (i = 0; i < 2; i++)
            if (tab[sw][i] >= 0) {
                regs[n] = da == 255 ? 255 : da + (u32)n;
                comps[n++] = (u32)tab[sw][i];
            }
    } else {
        static const int tab[5][4] = {{0, 1, 2, -1}, {0, 1, 3, -1}, {0, 2, 3, -1}, {1, 2, 3, -1}, {0, 1, 2, 3}};
        if (sw > 4) sw = 4;
        for (i = 0; i < 4; i++)
            if (tab[sw][i] >= 0) {
                regs[n] = n < 2 ? da + (u32)n : db + (u32)(n - 2);
                comps[n++] = (u32)tab[sw][i];
            }
    }
    return n;
}
/* The registers TEXS and TLDS write: each component its own register (bit 59), or two halves to a register, the
   first pair in Rd and the second in bits 28-35's register */
static void texs_mark_outputs(u64 w, u8 used[255]) {
    u32 regs[4], comps[4], d = B(w, 0, 8);
    int outputs = texs_outputs(w, regs, comps);
    if (B(w, 59, 1)) {
        for (int k = 0; k < outputs; k++)
            if (regs[k] < 255) used[regs[k]] = 1;
        return;
    }
    if (outputs && d < 255) used[d] = 1;
    if (outputs > 2 && B(w, 28, 8) < 255) used[B(w, 28, 8)] = 1;
}

static u32 direct_mask_count(u32 mask) {
    u32 count = 0;
    for (; mask; mask &= mask - 1) count++;
    return count;
}
/* Whether the hardware writes reg when it runs w. Backward scans for a register's
   definition need the whole range: loads, 64-bit results and masked texture results
   write registers after the one in bits 0-7, and stores and predicate setters keep a
   source there instead. Forms the translator rejects are described too, so a scan
   never skips over a write. */
int shader_writes_reg(u64 w, int op, u32 reg) {
    u32 d = B(w, 0, 8), count = 1;
    if (reg == 255) return 0;
    switch (op) {
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I:
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I:
        case O_HSETP2_R:
        case O_HSETP2_C:
        case O_HSETP2_I:
        case O_DSETP_R:
        case O_DSETP_C:
        case O_DSETP_I:
        case O_PSETP:
        case O_CSETP:
        case O_VSETP:
        case O_FCHK_R:
        case O_FCHK_C:
        case O_FCHK_I:
        case O_R2P_R:
        case O_R2P_C:
        case O_R2P_I:
        case O_R2B:
        case O_STG:
        case O_STS:
        case O_STL:
        case O_ST:
        case O_STP:
        case O_RED:
        case O_AST:
        case O_SUST:
        case O_SURED:
        case O_BRA:
        case O_BRX:
        case O_SSY:
        case O_SYNC:
        case O_PBK:
        case O_BRK:
        case O_PCNT:
        case O_CONT:
        case O_EXIT:
        case O_KIL:
        case O_JMP:
        case O_CAL:
        case O_RET:
        case O_PRET:
        case O_PEXIT:
        case O_NOP:
        case O_BAR:
        case O_MEMBAR:
        case O_DEPBAR:
        case O_VOTE_VTG:
        case O_CCTL:
        case O_CCTLL:
        case O_BPT:
        case O_IDE:
        case O_SETCRSPTR: return 0;
        case O_TLD4S: {
            /* Two destination pairs, the second named in bits 28-35, or one packed register for each. */
            u32 second = B(w, 28, 8);
            if (B(w, 55, 1)) return d == reg || second == reg;
            return (d != 255 && reg - d < 2) || (second != 255 && reg - second < 2);
        }
        case O_TEXS:
        case O_TLDS: {
            u32 regs[4], comps[4];
            int outputs = texs_outputs(w, regs, comps);
            int k;
            if (!B(w, 59, 1)) return outputs && (d == reg || (outputs > 2 && B(w, 28, 8) == reg));
            for (k = 0; k < outputs; k++)
                if (regs[k] == reg) return 1;
            return 0;
        }
        /* A mask of 0 writes nothing, as Eden and Ryujinx read it (NVN never emits one). */
        case O_TEX:
        case O_TEX_B:
        case O_TLD:
        case O_TLD_B:
        case O_TLD4:
        case O_TLD4_B:
        case O_TMML:
        case O_TMML_B:
        case O_TXD:
        case O_TXD_B:
        case O_TXQ:
        case O_TXQ_B: count = direct_mask_count(B(w, 31, 4)); break;
        case O_SULD:
            count = !B(w, 52, 1) ? direct_mask_count(B(w, 20, 4)) : B(w, 20, 3) == 5 ? 2 : B(w, 20, 3) >= 6 ? 4 : 1;
            break;
        case O_LDG:
        case O_LDS:
        case O_LDL: count = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1; break;
        case O_LD: count = B(w, 53, 3) == 5 ? 2 : B(w, 53, 3) >= 6 ? 4 : 1; break;
        case O_LDC: count = B(w, 48, 3) == 5 ? 2 : 1; break;
        case O_ALD: count = B(w, 47, 2) + 1; break;
        case O_ATOM: count = B(w, 49, 3) == 2 || B(w, 49, 3) == 5 ? 2 : 1; break;
        case O_ATOMS: count = B(w, 28, 2) >= 2 ? 2 : 1; break;
        case O_SUATOM: count = B(w, 51, 3) == 2 || B(w, 51, 3) == 3 || B(w, 51, 3) == 7 ? 2 : 1; break;
        case O_ATOM_CAS: count = B(w, 49, 1) ? 2 : 1; break;
        case O_ATOMS_CAS: count = B(w, 52, 4) == 5 ? 2 : 1; break;
        case O_SUATOM_CAS: count = 2; break;
        /* Mesa NAK treats CS2R as a 64-bit sysval load, so it writes both destination words. */
        case O_CS2R: count = 2; break;
        case O_DADD_R:
        case O_DADD_C:
        case O_DADD_I:
        case O_DMUL_R:
        case O_DMUL_C:
        case O_DMUL_I:
        case O_DFMA_R:
        case O_DFMA_RC:
        case O_DFMA_CR:
        case O_DFMA_I:
        case O_DMNMX_R:
        case O_DMNMX_C:
        case O_DMNMX_I: count = 2; break;
        case O_F2F_R:
        case O_F2F_C:
        case O_F2F_I:
        case O_F2I_R:
        case O_F2I_C:
        case O_F2I_I:
        case O_I2F_R:
        case O_I2F_C:
        case O_I2F_I:
        case O_I2I_R:
        case O_I2I_C:
        case O_I2I_I: count = B(w, 8, 2) == 3 ? 2 : 1; break;
        default: break;
    }
    return d != 255 && reg >= d && reg - d < count;
}

/* The predicates w may write, a bit each (p0 to p6). The comparisons write two, in bits 3-5 and 0-2; LOP, LOP3 and
   LEA name theirs in bits 48-50; PT is none. The rest that write any are taken to write them all. */
static u32 sparse_predicate(u64 w, int op);
static u32 direct_pred_writes(u64 w, int op) {
    u32 p;
    switch (op) {
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I:
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I:
        case O_HSETP2_R:
        case O_HSETP2_C:
        case O_HSETP2_I:
        case O_DSETP_R:
        case O_DSETP_C:
        case O_DSETP_I:
        case O_PSETP:
        case O_CSETP:
        case O_VSETP: return (1u << B(w, 3, 3) | 1u << B(w, 0, 3)) & 0x7f;
        case O_FCHK_R:
        case O_FCHK_C:
        case O_FCHK_I:
        case O_R2P_R:
        case O_R2P_C:
        case O_R2P_I:
        case O_SHFL:
        case O_VOTE: return 0x7f;
        case O_LOP_R:
        case O_LOP_C:
        case O_LOP_I:
        case O_LOP3_R:
        case O_LEA_HR:
        case O_LEA_HC:
        case O_LEA_LR:
        case O_LEA_LC:
        case O_LEA_LI: p = B(w, 48, 3); break;
        default: p = sparse_predicate(w, op); break;
    }
    return p < 7 ? 1u << p : 0;
}

/* Where control can arrive from the flow instruction at k, besides the next instruction: a branch, SSY, PBK or PCNT
   target, a jump table's entries, a CAL's callee and the return point after it, a PRET's return point. -1 when that
   can't be known (a JMP, or a jump table that isn't found). elsewhere[t] is set for arrivals that come from other
   instructions than k: a PCNT's target takes CONTs from later in the loop, and a return point takes RETs. */
static int direct_flow_targets(const u64* insn, u32 n, u32 k, u32 targets[64], u8 elsewhere[64]) {
    const Op* flow = decode(insn[k]);
    if (!flow) return 0;
    switch (flow->id) {
        case O_JMP: return -1;
        case O_BRX: {
            int count = shader_brx_targets(insn, k, n, targets, 64);
            int t;
            for (t = 0; t < count; t++) elsewhere[t] = 0;
            return count;
        }
        case O_CAL:
            targets[0] = bra_target(insn[k], k);
            targets[1] = k + 1 + ((k + 1 - 10) % 4 == 0);
            elsewhere[0] = 0;
            elsewhere[1] = 1;
            return 2;
        case O_PRET:
            targets[0] = bra_target(insn[k], k);
            elsewhere[0] = 1;
            return 1;
        case O_BRA:
        case O_SSY:
        case O_PBK:
        case O_PCNT:
            targets[0] = bra_target(insn[k], k);
            elsewhere[0] = flow->id == O_PCNT;
            return 1;
        default: return 0;
    }
}

/* Whether a flow instruction whose targets aren't known at k can be left out of a search that only cares about targets
   at or below limit: every case of a jump table lies after its BRX (shader_brx_targets), so one at or after limit
   cannot reach the search's region. A JMP takes a register's target and can go anywhere, so it never can. */
static int direct_flow_below(const u64* insn, u32 k, u32 limit) {
    const Op* op = decode(insn[k]);
    return op && op->id == O_BRX && k >= limit;
}

/* Whether control goes from the instruction at from straight on to the one at to, with nothing between that leaves */
static int direct_straight(const u64* insn, u32 from, u32 to) {
    u32 k;
    for (k = from + 1; k < to; k++) {
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 0;
        switch (op->id) {
            case O_BRA:
            case O_BRX:
            case O_SSY:
            case O_SYNC:
            case O_PBK:
            case O_BRK:
            case O_PCNT:
            case O_CONT:
            case O_EXIT:
            case O_KIL:
            case O_JMP:
            case O_CAL:
            case O_RET:
            case O_PRET:
            case O_PEXIT: return 0;
            default: break;
        }
    }
    return 1;
}

/* Whether every path to use passes def, and reg keeps def's value on the way: the caller has checked (def, use]
   writes no reg. Paths enter the region only by falling through from def, unless control arrives in it from before
   def (a branch, or the SYNC or BRK of an SSY or PBK set up before def) or from instructions that aren't known
   (CONTs, RETs). An SSY or PBK that goes straight on to def is no way around it: its token's SYNCs and BRKs all come
   after def (the compiler loads a handle first thing in the block whose reconvergence point samples it). A branch
   back into the region from after use is a loop the region grows to take in: a handle loaded before a loop and
   sampled in its body (a blur's taps) reaches every iteration when the rest of the body keeps reg. */
static int direct_handle_definition(const u64* insn, u32 n, u32 j, u32* bank, u32* off, u32* bank2, u32* off2,
                                    int depth);
static int direct_same_handle(const u64* insn, u32 n, u32 a, u32 b, int depth);
static int direct_reaching_def_same(const u64* insn, u32 n, u32 index, u32 reg,
                                    int (*same)(const u64*, u32, u32, u32, int), int depth);

static int direct_def_dominates(const u64* insn, u32 n, u32 def, u32 use, u32 reg,
                                int (*same)(const u64*, u32, u32, u32, int), int depth) {
    u32 end = use;
    for (;;) {
        u32 k, grown = end;
        for (k = 11; k < n; k++) {
            u32 targets[64];
            u8 elsewhere[64];
            int count;
            int t;
            if (!IDX_OK(k)) continue;
            const Op* op = decode(insn[k]);
            if (!op) return 0;
            count = direct_flow_targets(insn, n, k, targets, elsewhere);
            if (count < 0) {
                if (direct_flow_below(insn, k, end)) continue;
                return 0;
            }
            for (t = 0; t < count; t++) {
                if (targets[t] <= def || targets[t] > end) continue;
                if (elsewhere[t]) return 0;
                if (k < def) {
                    if ((op->id == O_SSY || op->id == O_PBK) && direct_straight(insn, k, def)) continue;
                    /* a path that leaves before def carries def's value on if reg already has it there (a loop's
                       exit branch, taken before the loop's last write of reg) */
                    if (same) {
                        int jk = direct_reaching_def_same(insn, n, k, reg, same, depth + 1);
                        if (jk >= 0 && same(insn, n, def, (u32)jk, depth + 1)) continue;
                    }
                    return 0;
                }
                if (k > grown) grown = k;
            }
        }
        if (grown == end) return 1;
        for (k = end + 1; k <= grown; k++) {
            const Op* op;
            if (!IDX_OK(k)) continue;
            op = decode(insn[k]);
            if (!op) return 0;
            if (shader_writes_reg(insn[k], op->id, reg)) {
                /* a loop that rewrites reg with the definition def has doesn't hide def's value */
                if (same && B(insn[k], 0, 8) == reg && same(insn, n, def, k, depth + 1)) continue;
                return 0;
            }
        }
        end = grown;
    }
}

/* A predicate's local definition, following a saved PSET boolean restored by
   ISETP.EQ/NE against RZ. Output predicates of a plain ISETP are complements. */
static int direct_pred_origin(const u64* insn, u32 index, u32 floor, u32 guard, u32* invert, int depth) {
    u32 pred = guard & 7;
    if (pred == 7 || depth > 8) return -1;
    *invert ^= guard >> 3;
    for (u32 j = index; j-- > floor;) {
        const Op* op;
        u64 w;
        if (!IDX_OK(j)) continue;
        w = insn[j];
        op = decode(w);
        if (!op) return -1;
        if (!(direct_pred_writes(w, op->id) >> pred & 1)) continue;
        if (B(w, 16, 4) != 7 ||
            (op->id != O_ISETP_R && op->id != O_ISETP_C && op->id != O_ISETP_I) ||
            B(w, 39, 4) != 7 || B(w, 45, 2) || B(w, 43, 1)) return -1;
        *invert ^= B(w, 0, 3) == pred;
        if (op->id == O_ISETP_R && B(w, 20, 8) == 255 &&
            (B(w, 49, 3) == 2 || B(w, 49, 3) == 5)) {
            u32 reg = B(w, 8, 8);
            /* The saved boolean must have an unconditional local definition.
               A predicated overwrite would leave two possible sources. */
            for (u32 k = j; k-- > floor;) {
                const Op* saved;
                u64 v;
                if (!IDX_OK(k)) continue;
                v = insn[k];
                saved = decode(v);
                if (!saved) return -1;
                if (!shader_writes_reg(v, saved->id, reg)) continue;
                if (B(v, 0, 8) != reg || B(v, 16, 4) != 7) return -1;
                if (saved->id != O_PSET || B(v, 24, 2) || B(v, 45, 2) ||
                    B(v, 29, 4) != 7 || B(v, 39, 4) != 7) return (int)j;
                *invert ^= B(w, 49, 3) == 2;
                return direct_pred_origin(insn, k, floor, B(v, 12, 4), invert, depth + 1);
            }
        }
        return (int)j;
    }
    /* No local write: the predicate entering this block is a symbolic value.
       Its saved/restored copies remain equal even when its producer is outside
       the block. -1 remains unknown, -2 through -8 identify P0 through P6. */
    return -(int)pred - 2;
}

/* Two locally computed guards can have different predicate registers or have
   been saved and restored. Equal compares are equivalent only while their
   register operands retain their values. Constant buffers are read-only. */
static int direct_same_guard(const u64* insn, u32 floor, u32 definition, u32 use) {
    u32 ai = 0, bi = 0;
    int a = direct_pred_origin(insn, definition, floor, B(insn[definition], 16, 4), &ai, 0);
    int b = direct_pred_origin(insn, use, floor, B(insn[use], 16, 4), &bi, 0);
    if (a == -1 || b == -1 || ai != bi) return 0;
    if (a == b) return 1;
    if (a < 0 || b < 0) return 0;
    if ((insn[a] & ~63ull) != (insn[b] & ~63ull)) return 0;
    const Op* op = decode(insn[a]);
    u32 lhs = B(insn[a], 8, 8), rhs = op->id == O_ISETP_R ? B(insn[a], 20, 8) : 255;
    u32 lo = (u32)(a < b ? a : b), hi = (u32)(a > b ? a : b);
    for (u32 k = lo + 1; k < hi; k++) {
        if (!IDX_OK(k)) continue;
        const Op* mid = decode(insn[k]);
        if (!mid || (lhs != 255 && shader_writes_reg(insn[k], mid->id, lhs)) ||
            (rhs != 255 && shader_writes_reg(insn[k], mid->id, rhs))) return 0;
    }
    return 1;
}

/* Whether pred changes between lo and hi: a requirement's value at one end describes the other only while it
   doesn't. */
static int direct_pred_written(const u64* insn, u32 lo, u32 hi, u32 pred) {
    u32 k;
    for (k = lo + 1; k < hi; k++) {
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 1;
        if (direct_pred_writes(insn[k], op->id) >> pred & 1) return 1;
    }
    return 0;
}

/* the words of a program: the 0x20000-byte window vk_shader_get reads it from (SH_CODE), which direct_can_reach's
   reachability bitset holds */
#define DIRECT_WORDS_MAX 16384u

static int direct_mark(u8* seen, u32 use, u32 at) {
    if (at > use || (seen[at >> 3] >> (at & 7) & 1)) return 0;
    seen[at >> 3] |= (u8)(1u << (at & 7));
    return 1;
}

/* Whether control from from can reach use, by the flow instructions as far as they are known: -1 when a jump's target
   isn't (an indirect jump, a token's BRK or CONT), so the caller keeps what it asked about. The bitset is walked to a
   fixed point, which reaches through loops as well as forward edges. */
static int direct_can_reach(const u64* insn, u32 n, u32 from, u32 use) {
    u8 seen[DIRECT_WORDS_MAX / 8];
    u32 k;
    int changed = 1;
    if (from > use) return 0;
    if (use >= DIRECT_WORDS_MAX) return -1;
    memset(seen, 0, sizeof seen);
    seen[from >> 3] |= (u8)(1u << (from & 7));
    while (changed) {
        changed = 0;
        for (k = from; k <= use; k++) {
            const Op* op;
            u32 targets[64];
            u8 elsewhere[64];
            int count;
            int t;
            int fall = 1;
            if (!(seen[k >> 3] >> (k & 7) & 1)) continue;
            if (!IDX_OK(k)) {
                changed |= direct_mark(seen, use, k + 1);
                continue;
            }
            op = decode(insn[k]);
            if (!op) return -1;
            switch (op->id) {
                /* Lanes that do not take a conditional exit keep their register definitions. */
                case O_EXIT: fall = !flow_always(insn[k]); break;
                case O_KIL:
                case O_RET:
                case O_PRET:
                case O_PEXIT: fall = 0; break;
                case O_BRK:
                case O_CONT:
                case O_JMP: return -1;
                case O_BRA: fall = B(insn[k], 16, 4) != 7; break;
                case O_BRX: fall = 0; break;
                default: break;
            }
            count = direct_flow_targets(insn, n, k, targets, elsewhere);
            if (count < 0) return -1;
            if (op->id == O_BRX && !count) return -1;
            for (t = 0; t < count; t++)
                if (!elsewhere[t]) changed |= direct_mark(seen, use, targets[t]);
            if (fall) changed |= direct_mark(seen, use, k + 1);
        }
    }
    return seen[use >> 3] >> (use & 7) & 1;
}

/* Where index's basic block starts: the last place control can arrive at before it, as direct_reaching_def finds it. */
static u32 direct_block_start(const u64* insn, u32 n, u32 index) {
    u32 start = 11;
    u32 k;
    for (k = 11; k < n; k++) {
        u32 targets[64];
        u8 elsewhere[64];
        int count;
        int t;
        if (!IDX_OK(k)) continue;
        count = direct_flow_targets(insn, n, k, targets, elsewhere);
        if (count < 0) return start;
        for (t = 0; t < count; t++)
            if (targets[t] > start && targets[t] <= index) start = targets[t];
    }
    return start;
}

/* Whether control reaches use only through def and only forwards from it: no branch enters (def, use] from before def
   or from after it, and none in the region jumps back. A branch in between then tests a predicate def also saw, with
   no earlier pass through the region to change it. */
static int direct_forward_only(const u64* insn, u32 n, u32 def, u32 use) {
    u32 k;
    for (k = 11; k < n; k++) {
        u32 targets[64];
        u8 elsewhere[64];
        int count;
        int t;
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 0;
        count = direct_flow_targets(insn, n, k, targets, elsewhere);
        if (count < 0) {
            if (direct_flow_below(insn, k, use)) continue;
            return 0;
        }
        for (t = 0; t < count; t++) {
            if (targets[t] <= def || targets[t] > use) continue;
            if (elsewhere[t]) return 0;
            if (k < def) {
                if ((op->id == O_SSY || op->id == O_PBK) && direct_straight(insn, k, def)) continue;
                return 0;
            }
            if (targets[t] <= k) return 0;
        }
    }
    return 1;
}

/* Whether every path to use passes the branch at from: a branch before from that lands past it is a way around. The
   edges in (def, use] all run forwards when direct_forward_only passed, so one scan is enough. */
static int direct_branch_dominates(const u64* insn, u32 n, u32 from, u32 use) {
    u32 k;
    for (k = 11; k < n; k++) {
        u32 targets[64];
        u8 elsewhere[64];
        int count;
        int t;
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 0;
        count = direct_flow_targets(insn, n, k, targets, elsewhere);
        if (count < 0) {
            if (direct_flow_below(insn, k, use)) continue;
            return 0;
        }
        for (t = 0; t < count; t++) {
            if (targets[t] <= from || targets[t] > use) continue;
            if (elsewhere[t]) return 0;
            if (k < from) {
                if ((op->id == O_SSY || op->id == O_PBK) && direct_straight(insn, k, from)) continue;
                return 0;
            }
        }
    }
    return 1;
}

/* A three-valued boolean combine: 1 true, 0 false, -1 unknown. */
static int direct_pred_combine(u32 op, int a, int b) {
    if (op == 0) return a == 0 || b == 0 ? 0 : a == 1 && b == 1 ? 1 : -1;
    if (op == 1) return a == 1 || b == 1 ? 1 : a == 0 && b == 0 ? 0 : -1;
    if (op == 2) return a < 0 || b < 0 ? -1 : a != b;
    return a;
}

/* Whether a guard predicate must hold, given the values the branches to use force at def (known and value, bit p set
   when predicate p's value is known there). The guard resolves through the PSETP instructions that set it, which
   have to run on every path to def (floor starts def's straight run). A requirement is a value at def, so it describes
   a predicate read earlier only while nothing writes the predicate between. 1 when the guard holds, 0 when it doesn't,
   -1 when this can't tell. */
static int direct_guard_under(const u64* insn, u32 index, u32 floor, u32 guard, u32 known, u32 value, int depth) {
    u32 pred = guard & 7;
    int neg = (int)(guard >> 3);
    u32 j;
    if (pred == 7) return 1;
    if (depth > 8) return -1;
    if (known >> pred & 1) {
        int v = (int)(value >> pred & 1);
        return neg ? !v : v;
    }
    for (j = index; j-- > floor;) {
        const Op* op;
        u64 w;
        u32 pa, pb, pc, ka, kb, kc;
        int va, vb, vc, first;
        if (!IDX_OK(j)) continue;
        w = insn[j];
        op = decode(w);
        if (!op) return -1;
        if (!(direct_pred_writes(w, op->id) >> pred & 1)) continue;
        if (B(w, 16, 4) != 7 || op->id != O_PSETP) return -1;
        pa = B(w, 12, 3);
        pb = B(w, 29, 3);
        pc = B(w, 39, 3);
        ka = known;
        kb = known;
        kc = known;
        if ((known >> pa & 1) && direct_pred_written(insn, j, index, pa)) ka &= ~(1u << pa);
        if ((known >> pb & 1) && direct_pred_written(insn, j, index, pb)) kb &= ~(1u << pb);
        if ((known >> pc & 1) && direct_pred_written(insn, j, index, pc)) kc &= ~(1u << pc);
        va = direct_guard_under(insn, j, floor, pa | B(w, 15, 1) << 3, ka, value, depth + 1);
        vb = direct_guard_under(insn, j, floor, pb | B(w, 32, 1) << 3, kb, value, depth + 1);
        vc = direct_guard_under(insn, j, floor, pc | B(w, 42, 1) << 3, kc, value, depth + 1);
        first = direct_pred_combine(B(w, 24, 2), va, vb);
        if (B(w, 0, 3) == pred) /* the second output is written last */
            first = direct_pred_combine(B(w, 24, 2), va < 0 ? -1 : !va, vb);
        else if (B(w, 3, 3) != pred)
            return -1;
        return direct_pred_combine(B(w, 45, 2), first, vc);
    }
    return -1;
}

/* Whether the branches on the way to use make def's predicated write run. A conditional branch that every path to
   use passes, and whose other edge doesn't reach use, forces its predicate's value at def; a guard those values make
   true under PSETP combinators then runs def on every path. The compiler guards a whole block's handles this way: one
   branch tests the predicate it later composes, so the def runs unguarded at the use. */
static int direct_guard_implied(const u64* insn, u32 n, u32 def, u32 use) {
    u32 known = 0, value = 0;
    u32 floor = direct_block_start(insn, n, def);
    u32 b;
    if (!direct_forward_only(insn, n, def, use)) return 0;
    /* the predicate setters resolved below have to run on every path to def */
    if (!direct_straight(insn, floor, def)) return 0;
    for (b = def + 1; b < use; b++) {
        const Op* op;
        u32 g, pred, target;
        int taken, fall, val;
        if (!IDX_OK(b)) continue;
        op = decode(insn[b]);
        if (!op) return 0;
        if (op->id != O_BRA) continue;
        g = B(insn[b], 16, 4);
        if ((g & 7) == 7) continue;
        if (!direct_branch_dominates(insn, n, b, use)) continue;
        target = bra_target(insn[b], b);
        taken = direct_can_reach(insn, n, target, use);
        fall = direct_can_reach(insn, n, b + 1, use);
        if (taken < 0 || fall < 0) return 0;
        if (taken == fall) continue; /* both ways reach use: the branch doesn't force its predicate */
        /* the branch is taken when the predicate is the negation bit's opposite */
        pred = g & 7;
        val = taken ? !(int)(g >> 3) : (int)(g >> 3);
        if (direct_pred_written(insn, def, b, pred)) continue;
        if (known >> pred & 1) {
            if ((int)(value >> pred & 1) != val) return 0;
            continue;
        }
        known |= 1u << pred;
        if (val) value |= 1u << pred;
    }
    if (!known) return 0;
    return direct_guard_under(insn, def, floor, B(insn[def], 16, 4), known, value, 0) == 1;
}

/* The instruction that last wrote reg before index, as its first destination, or -1. In index's basic block, a
   write under a predicate reaches only an instruction under the same one, with that predicate not written between
   them (others may be): the compiler loads a handle and uses it under the guard of one side of a branch it made
   predicated. Across flow, the write has to be unconditional and dominate index. */
static int direct_reaching_def_same(const u64* insn, u32 n, u32 index, u32 reg,
                                    int (*same)(const u64*, u32, u32, u32, int), int depth) {
    u32 guard, pred_written = 0;
    int crossed = 0;
    /* where index's basic block starts: the last place control can arrive at before it */
    u32 block_start = 11;
    u32 k;
    u32 j;
    if (reg == 255) return -1;
    guard = B(insn[index], 16, 4);
    for (k = 11; k < n; k++) {
        u32 targets[64];
        u8 elsewhere[64];
        int count;
        int t;
        if (!IDX_OK(k)) continue;
        count = direct_flow_targets(insn, n, k, targets, elsewhere);
        if (count < 0) {
            if (direct_flow_below(insn, k, index)) continue;
            return -1;
        }
        for (t = 0; t < count; t++)
            if (targets[t] > block_start && targets[t] <= index) block_start = targets[t];
    }
    for (j = index; j-- > 11;) {
        const Op* op;
        if (!IDX_OK(j)) continue;
        op = decode(insn[j]);
        if (!op) return -1;
        if (j < block_start || op->id == O_BRA || op->id == O_BRX || op->id == O_CAL || op->id == O_RET ||
            op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT || op->id == O_EXIT || op->id == O_KIL)
            crossed = 1;
        if (!shader_writes_reg(insn[j], op->id, reg)) {
            pred_written |= direct_pred_writes(insn[j], op->id);
            continue;
        }
        /* A write in an earlier block that control never carries to index isn't the value there: the block's branch
           jumped past the use. */
        if (j < block_start && direct_can_reach(insn, n, j + 1, index) == 0) continue;
        /* A write that reaches reg from a lower destination leaves no single source. */
        if (B(insn[j], 0, 8) != reg) return -1;
        if (B(insn[j], 16, 4) != 7 &&
            (crossed || B(insn[j], 16, 4) != guard || (pred_written >> (guard & 7) & 1)) &&
            !direct_same_guard(insn, block_start, j, index) && !direct_guard_implied(insn, n, j, index))
            return -1;
        if (crossed && !direct_def_dominates(insn, n, j, index, reg, same, depth)) return -1;
        return (int)j;
    }
    return -1;
}

static int direct_reaching_def(const u64* insn, u32 n, u32 index, u32 reg) {
    return direct_reaching_def_same(insn, n, index, reg, NULL, 0);
}

static int direct_track_const(const u64* insn, u32 n, u32 index, u32 reg, u32* value, int depth) {
    int j = depth > 6 ? -1 : direct_reaching_def(insn, n, index, reg);
    u64 w;
    int op;
    if (j < 0) return 0;
    w = insn[j];
    op = decode(w)->id;
    if (op == O_MOV32I) {
        *value = B(w, 20, 32);
        return 1;
    }
    if (op == O_MOV_R) return direct_track_const(insn, n, (u32)j, B(w, 20, 8), value, depth + 1);
    return 0;
}

/* Resolve a register supplied image handle to the constant buffer word read in this basic block. */
static int direct_track_image_handle(const u64* insn, u32 n, u32 index, u32 reg, u32* bank, u32* off, int depth) {
    int j = depth > 6 ? -1 : direct_reaching_def(insn, n, index, reg);
    u64 w;
    int op;
    if (j < 0) return 0;
    w = insn[j];
    op = decode(w)->id;
    if (op == O_MOV_C) {
        *bank = B(w, 34, 5);
        *off = B(w, 20, 14);
        return *bank < 18;
    }
    if (op == O_LDC && B(w, 48, 3) >= 4 && B(w, 48, 3) <= 5) {
        i32 byte_off = (i16)B(w, 20, 16);
        *bank = B(w, 36, 5);
        *off = (u32)byte_off / 4;
        /* Keep the legacy first-descriptor behavior for indexed handles.
           Runtime descriptor selection for arbitrary indices needs a separate path. */
        return *bank < 18 && byte_off >= 0 && !(byte_off & 3);
    }
    if (op == O_MOV_R) return direct_track_image_handle(insn, n, (u32)j, B(w, 20, 8), bank, off, depth + 1);
    return 0;
}

/* Split texture and sampler handles can be ORed or added after separate cbuf
   reads, the second one either through a register or as the instruction's own
   constant buffer operand. Keep both sources so descriptor binding reproduces
   the guest handle. */
static int direct_track_image_handle_pair(const u64* insn, u32 n, u32 index, u32 reg, u32* bank, u32* off, u32* bank2,
                                          u32* off2, int depth);

static int direct_handle_definition(const u64* insn, u32 n, u32 j, u32* bank, u32* off, u32* bank2, u32* off2,
                                     int depth) {
    u64 w = insn[j];
    int op = decode(w)->id;
    int plain_or;
    int plain_add;
    u32 lhs;
    u32 rhs;
    *bank2 = *off2 = 0;
    if (op == O_MOV_C) {
        *bank = B(w, 34, 5);
        *off = B(w, 20, 14);
        return *bank < 18;
    }
    if (op == O_LDC && B(w, 48, 3) >= 4 && B(w, 48, 3) <= 5) {
        i32 byte_off = (i16)B(w, 20, 16);
        *bank = B(w, 36, 5);
        *off = (u32)byte_off / 4;
        return *bank < 18 && byte_off >= 0 && !(byte_off & 3);
    }
    if (op == O_MOV_R)
        return direct_track_image_handle_pair(insn, n, (u32)j, B(w, 20, 8), bank, off, bank2, off2, depth + 1);
    plain_or = (op == O_LOP_R || op == O_LOP_C) && B(w, 41, 2) == 1 && !B(w, 39, 2) && !B(w, 47, 1);
    plain_add = (op == O_IADD_R || op == O_IADD_C) && !B(w, 43, 1) && !B(w, 47, 1) && !B(w, 48, 2);
    if (!plain_or && !plain_add) return 0;
    lhs = B(w, 8, 8);
    if (op == O_LOP_C || op == O_IADD_C) {
        u32 cbank = B(w, 34, 5), coff = B(w, 20, 14);
        if (cbank >= 18) return 0;
        if (lhs == 255) {
            *bank = cbank;
            *off = coff;
            return 1;
        }
        *bank2 = cbank;
        *off2 = coff;
        return direct_track_image_handle(insn, n, (u32)j, lhs, bank, off, depth + 1) ? 2 : 0;
    }
    rhs = B(w, 20, 8);
    if (lhs == 255) return direct_track_image_handle_pair(insn, n, (u32)j, rhs, bank, off, bank2, off2, depth + 1);
    if (rhs == 255) return direct_track_image_handle_pair(insn, n, (u32)j, lhs, bank, off, bank2, off2, depth + 1);
    return direct_track_image_handle(insn, n, (u32)j, lhs, bank, off, depth + 1) &&
                   direct_track_image_handle(insn, n, (u32)j, rhs, bank2, off2, depth + 1)
               ? 2
               : 0;
}

/* Whether the write of reg at b defines the same handle as the one at a: a loop that rewrites reg with the same
   constant buffer reads leaves the handle at a use in the loop unchanged (the second tap of a blur, or its next
   iteration's first). */
static int direct_same_handle(const u64* insn, u32 n, u32 a, u32 b, int depth) {
    u32 ab, ao, ab2, ao2, bb, bo, bb2, bo2;
    int ca, cb;
    if (depth > 6) return 0;
    ca = direct_handle_definition(insn, n, a, &ab, &ao, &ab2, &ao2, depth);
    cb = ca ? direct_handle_definition(insn, n, b, &bb, &bo, &bb2, &bo2, depth) : 0;
    return ca && ca == cb && ab == bb && ao == bo && (ca == 1 || (ab2 == bb2 && ao2 == bo2));
}

/* At a forward branch merge, two different definitions can hold the same
   descriptor. Prove the value on every edge bypassing the nearest definition,
   rather than binding whichever side happens to appear last in the program.
   Indirect flow, reconvergence arrivals and back edges remain conservative. */
static int direct_joined_handle(const u64* insn, u32 n, u32 index, u32 reg, u32* bank, u32* off, u32* bank2,
                                u32* off2, int depth) {
    int j = -1, count, joined = 0;
    for (u32 k = index; k-- > 11;) {
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 0;
        if (!shader_writes_reg(insn[k], op->id, reg)) continue;
        if (B(insn[k], 0, 8) != reg || B(insn[k], 16, 4) != 7) return 0;
        j = (int)k;
        break;
    }
    if (j < 0 || !(count = direct_handle_definition(insn, n, (u32)j, bank, off, bank2, off2, depth + 1))) return 0;
    for (u32 k = 11; k < n; k++) {
        u32 targets[64];
        u8 elsewhere[64];
        const Op* op;
        int edges;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 0;
        if (k > (u32)j && k < index &&
            (op->id == O_CAL || op->id == O_RET || op->id == O_BRX || op->id == O_JMP ||
             op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT)) return 0;
        edges = direct_flow_targets(insn, n, k, targets, elsewhere);
        if (edges < 0) return 0;
        for (int t = 0; t < edges; t++) {
            u32 b, o, b2, o2;
            if (targets[t] <= (u32)j || targets[t] > index) continue;
            if (elsewhere[t] || k > index) return 0;
            if (k >= (u32)j) continue;
            if (op->id != O_BRA ||
                direct_track_image_handle_pair(insn, n, k, reg, &b, &o, &b2, &o2, depth + 1) != count ||
                b != *bank || o != *off || (count == 2 && (b2 != *bank2 || o2 != *off2))) return 0;
            joined = 1;
        }
    }
    return joined ? count : 0;
}

static int direct_track_image_handle_pair(const u64* insn, u32 n, u32 index, u32 reg, u32* bank, u32* off, u32* bank2,
                                          u32* off2, int depth) {
    int j;
    if (depth > 6 || reg == 255) return 0;
    j = direct_reaching_def_same(insn, n, index, reg, direct_same_handle, depth);
    if (j >= 0) return direct_handle_definition(insn, n, (u32)j, bank, off, bank2, off2, depth);
    return direct_joined_handle(insn, n, index, reg, bank, off, bank2, off2, depth);
}
/* x of a 1D texel fetch as the row and column of the 2D image a 1D texture is bound as (SH_TEXEL_ROW_SHIFT). A
   negative x lands in a negative row, out of the image as it is out of the texture */
static void texel_row_coord(DirectCtx* c, u32 i32_type, u32* x, u32* y) {
    u32 shift = spv_emit_constant_u32(&c->emit, c->u32_type, SH_TEXEL_ROW_SHIFT);
    u32 column = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, spv_emit_bitcast(&c->emit, c->u32_type, *x),
                                          spv_emit_constant_u32(&c->emit, c->u32_type,
                                                                (1u << SH_TEXEL_ROW_SHIFT) - 1));
    *y = spv_emit_shift_right_s32(&c->emit, i32_type, *x, shift);
    *x = spv_emit_bitcast(&c->emit, i32_type, column);
}
static u32 cbuf_read_dynamic(DirectCtx* c, u32 bank, u32 address) {
    u32 four = spv_emit_constant_u32(&c->emit, c->u32_type, 4);
    u32 two = spv_emit_constant_u32(&c->emit, c->u32_type, 2);
    u32 three = spv_emit_constant_u32(&c->emit, c->u32_type, 3);
    u32 element = spv_emit_shift_right_u32(&c->emit, c->u32_type, address, four);
    u32 component = spv_emit_bitwise_and_u32(&c->emit, c->u32_type,
                                             spv_emit_shift_right_u32(&c->emit, c->u32_type, address, two), three);
    return spv_emit_uniform_component(&c->emit, c->cbuf_pointer[bank], c->cbuf[bank], c->zero, element, component,
                                      c->u32_type);
}

static u32 double_source(DirectCtx* c, u32 vec2_u32, u64 w, int kind) {
    if (kind == 0) return double_read(c, vec2_u32, B(w, 20, 8));
    u32 low = c->zero, high;
    if (kind == 1) {
        u32 bank = B(w, 34, 5), offset = B(w, 20, 14) * 4;
        if (!(offset & 4)) low = cbuf_read_dynamic(c, bank, spv_emit_constant_u32(&c->emit, c->u32_type, offset));
        high = cbuf_read_dynamic(c, bank, spv_emit_constant_u32(&c->emit, c->u32_type, offset | 4u));
    } else
        high = spv_emit_constant_u32(&c->emit, c->u32_type, (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31));
    return spv_emit_bitcast(&c->emit, c->f64_type, spv_emit_vec2_construct(&c->emit, vec2_u32, low, high));
}

static u32 smap_word(DirectCtx* c, u32 index) {
    u32 pointer = spv_emit_storage_access_u32(&c->emit, c->gbuf_pointer, c->smap, c->zero, index);
    return spv_emit_load(&c->emit, c->u32_type, pointer);
}

static u32 gbuf_word_index(DirectCtx* c, int stage, u32 slot, u32 word) {
    /* Compute dispatches bind each guest buffer as one contiguous storage buffer.
       The graphics page table descriptor is not present in their descriptor set. */
    if (stage == ST_CS) return word;
    u32 h = smap_word(c, spv_emit_constant_u32(&c->emit, c->u32_type, slot));
    u32 no_map = spv_emit_i_equal(&c->emit, c->bool_type, h, spv_emit_constant_u32(&c->emit, c->u32_type, 0xffffffffu));
    u32 offset = smap_word(c, spv_emit_constant_u32(&c->emit, c->u32_type, 16 + slot));
    u32 count = smap_word(c, spv_emit_constant_u32(&c->emit, c->u32_type, 32 + slot));
    u32 valid = spv_emit_u_less(&c->emit, c->bool_type, word, count);
    u32 mapped_word = spv_emit_iadd_u32(&c->emit, c->u32_type, word, offset);
    u32 page =
        spv_emit_shift_right_u32(&c->emit, c->u32_type, mapped_word, spv_emit_constant_u32(&c->emit, c->u32_type, 10));
    u32 safe_h = spv_emit_select(&c->emit, c->u32_type, no_map, c->zero, h);
    u32 safe_page = spv_emit_select(&c->emit, c->u32_type, no_map, c->zero, page);
    u32 page_ix = spv_emit_iadd_u32(&c->emit, c->u32_type, safe_h, safe_page);
    u32 mapped_page = smap_word(c, page_ix);
    u32 high =
        spv_emit_shift_left_u32(&c->emit, c->u32_type, mapped_page, spv_emit_constant_u32(&c->emit, c->u32_type, 10));
    u32 low = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, mapped_word,
                                       spv_emit_constant_u32(&c->emit, c->u32_type, 1023));
    u32 mapped = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, high, low);
    mapped = spv_emit_select(&c->emit, c->u32_type, valid, mapped,
                             spv_emit_constant_u32(&c->emit, c->u32_type, 0xffffffffu));
    return spv_emit_select(&c->emit, c->u32_type, no_map, word, mapped);
}

static u32 global_atomic_pointer(DirectCtx* c, int stage, u32 slot, u32 base_off, u32 a, u64 w) {
    u32 base = cbuf_read_dynamic(c, SH_GBUF_BANK(base_off),
                                 spv_emit_constant_u32(&c->emit, c->u32_type, SH_GBUF_OFFSET(base_off)));
    u32 immediate = (u32)((i32)(B(w, 28, 20) << 12) >> 12);
    u32 address = spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a),
                                    spv_emit_constant_u32(&c->emit, c->u32_type, immediate));
    address = spv_emit_isub_u32(&c->emit, c->u32_type, address, base);
    u32 word =
        spv_emit_shift_right_u32(&c->emit, c->u32_type, address, spv_emit_constant_u32(&c->emit, c->u32_type, 2));
    return spv_emit_storage_access_u32(&c->emit, c->gbuf_pointer, c->gbuf[slot], c->zero,
                                       gbuf_word_index(c, stage, slot, word));
}

static u32 shared_atomic_index(DirectCtx* c, u32 a, u64 w, u32 shared_words) {
    i32 relative = (i32)(B(w, 30, 22) << 10) >> 10;
    u32 immediate = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)(relative * 4));
    u32 address = a == 255 ? immediate : spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), immediate);
    return shared_index(
        c, spv_emit_shift_right_u32(&c->emit, c->u32_type, address, spv_emit_constant_u32(&c->emit, c->u32_type, 2)),
        shared_words);
}

static u32 next_instruction(u32 index, u32 limit) {
    do { index++; } while (index < limit && !IDX_OK(index));
    return index;
}

/* Remove the nearest token of kind, dropping tokens pushed after it as Maxwell's CRS stack does. */
static void dynamic_pop(DirectCtx* c, u32 stack_ptr, const u32 stack[32], u32 kind_value, u32 fallback, u32* found,
                        u32* destination, u32* new_sp) {
    u32 sp = spv_emit_load(&c->emit, c->u32_type, stack_ptr);
    u32 kind = spv_emit_constant_u32(&c->emit, c->u32_type, kind_value);
    u32 shift = spv_emit_constant_u32(&c->emit, c->u32_type, 16);
    u32 mask = spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff);
    *found = c->false_value;
    *destination = fallback;
    *new_sp = sp;
    for (int k = 31; k >= 0; k--) {
        u32 index = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)k);
        u32 in_range = spv_emit_u_less(&c->emit, c->bool_type, index, sp);
        u32 token = spv_emit_load(&c->emit, c->u32_type, stack[k]);
        u32 matches = spv_emit_i_equal(&c->emit, c->bool_type,
                                       spv_emit_shift_right_u32(&c->emit, c->u32_type, token, shift), kind);
        u32 choose = spv_emit_bool_binary(&c->emit, c->bool_type, 0, in_range, matches);
        choose =
            spv_emit_bool_binary(&c->emit, c->bool_type, 0, choose, spv_emit_bool_not(&c->emit, c->bool_type, *found));
        *destination = spv_emit_select(&c->emit, c->u32_type, choose,
                                       spv_emit_bitwise_and_u32(&c->emit, c->u32_type, token, mask), *destination);
        *new_sp = spv_emit_select(&c->emit, c->u32_type, choose, index, *new_sp);
        *found = spv_emit_bool_binary(&c->emit, c->bool_type, 1, *found, choose);
    }
}

static int texture_sample_supported(u64 w, int bindless) {
    u32 type = B(w, 28, 3);
    u32 offset = B(w, bindless ? 36 : 54, 1);
    u32 mode = B(w, bindless ? 37 : 55, 3);
    /* Type 5 denotes a 3D array, which Vulkan cannot sample. LC is a packed 4.8 minimum LOD in the high half of
       the layer register (Mesa nak_nir_lower_tex.c, lower_tex_to_nak); this translator does not decode it, so refuse
       bit 58 (TEX.B bit 40). */
    return type != 5 && mode != 4 && mode != 5 && !(type == 4 && B(w, 50, 1)) && (!offset || type <= 4) &&
           !B(w, bindless ? 40 : 58, 1);
}

/* FSWZADD's lanes (bit L for quad lane L) whose two bits of the swizzle (28-35) are mode: lane L's are bits
   6 - 2L of it, the upper left lane's the top two, as nouveau's QUADOP lays them out (nv50_ir_lowering_gm107.cpp) */
static u32 fswzadd_lanes(u64 w, u32 mode) {
    u32 lanes = 0;
    for (u32 lane = 0; lane < 4; lane++)
        if ((B(w, 28, 8) >> (6 - 2 * lane) & 3) == mode) lanes |= 1u << lane;
    return lanes;
}

/* the comparisons that write two predicates (bits 3-5 and 0-2) and read a third (39-41, PSETP two more) */
static int predicate_comparison(int op) {
    switch (op) {
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I:
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I:
        case O_DSETP_R:
        case O_DSETP_C:
        case O_DSETP_I:
        case O_HSETP2_R:
        case O_HSETP2_C:
        case O_HSETP2_I:
        case O_CSETP:
        case O_VSETP:
        case O_PSETP: return 1;
        default: return 0;
    }
}

/* whether the instruction writes predicate p: the comparisons' two results, AL2P's (bits 44-46), VOTE's (45-47),
   the carry or zero that SHFL, LOP, LOP3 and LEA give (48-50), FCHK's, and any R2P */
static int writes_predicate(u64 w, int op, u32 p) {
    if (predicate_comparison(op)) return B(w, 3, 3) == p || B(w, 0, 3) == p;
    switch (op) {
        case O_AL2P: return B(w, 44, 3) == p;
        case O_VOTE: return B(w, 45, 3) == p;
        case O_SHFL:
        case O_LOP_R:
        case O_LOP_C:
        case O_LOP_I:
        case O_LOP3_R:
        case O_LEA_LR:
        case O_LEA_LC:
        case O_LEA_LI: return B(w, 48, 3) == p;
        case O_FCHK_R:
        case O_FCHK_C:
        case O_FCHK_I: return B(w, 3, 3) == p;
        case O_R2P_R:
        case O_R2P_C:
        case O_R2P_I: return 1;
        default: return 0;
    }
}

/* whether the instruction reads predicate p, as the translator reads them (pred_read's callers): its guard, but the
   stack instructions' it takes no notice of, a guarded write of p (which keeps p where the guard fails), the
   operand at bits 39-41 of the comparisons, selects and min and max, PSET's and PSETP's other two, and P2R's set */
static int reads_predicate(u64 w, int op, u32 p) {
    int guarded = B(w, 16, 3) != 7 || B(w, 19, 1);
    if (B(w, 16, 3) == p && op != O_SSY && op != O_PBK && op != O_PCNT && op != O_PEXIT && op != O_CAL) return 1;
    if (guarded && writes_predicate(w, op, p)) return 1;
    if (op == O_PSET || op == O_PSETP) return B(w, 12, 3) == p || B(w, 29, 3) == p || B(w, 39, 3) == p;
    if (predicate_comparison(op)) return B(w, 39, 3) == p;
    switch (op) {
        case O_CSET:
        case O_VOTE:
        case O_SEL_R:
        case O_SEL_C:
        case O_SEL_I:
        case O_FSET_R:
        case O_FSET_C:
        case O_FSET_I:
        case O_ISET_R:
        case O_ISET_C:
        case O_ISET_I:
        case O_DSET_R:
        case O_DSET_C:
        case O_DSET_I:
        case O_HSET2_R:
        case O_HSET2_C:
        case O_HSET2_I:
        case O_FMNMX_R:
        case O_FMNMX_C:
        case O_FMNMX_I:
        case O_IMNMX_R:
        case O_IMNMX_C:
        case O_IMNMX_I:
        case O_DMNMX_R:
        case O_DMNMX_C:
        case O_DMNMX_I: return B(w, 39, 3) == p;
        case O_P2R_R:
        case O_P2R_C:
        case O_P2R_I: return !B(w, 40, 1);
        default: return 0;
    }
}

/* AL2P's predicate (bits 44-46) is a range check no source describes. The NVN compiler gives every AL2P one, P0 to P6
   in turn, and overwrites or leaves them, so a program is translated without it only when nothing can see it: no
   instruction reads the predicate, or every path from the AL2P meets a comparison or AL2P without a guard that writes
   it whole before anything reads it. Paths follow branches, jump tables and the pops' targets as shader_resolve_crs
   gives them. A pop it can't resolve, a call, a return, a JMP, or an EXIT a PEXIT may take counts as a read */
static int al2p_predicate_unseen(const u64* insn, u32 n, u32 at) {
    u32 p = B(insn[at], 44, 3), i, nwork = 0, *work = NULL, *pops = NULL, targets[64];
    int read = 0, pexit = 0, stack = 0, crs = 0, unseen = 0, count, t;
    u8* seen = NULL;
    if (p == 7) return 1;
    for (i = 11; i < n; i++) {
        const Op* op = IDX_OK(i) && i != at ? decode(insn[i]) : NULL;
        if (!op) continue;
        read |= reads_predicate(insn[i], op->id, p);
        pexit |= op->id == O_PEXIT;
        stack |= op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT;
    }
    if (!read) return 1;
    seen = calloc(n, 1);
    work = malloc(n * sizeof *work);
    if (stack) pops = calloc(n, sizeof *pops);
    if (!seen || !work || (stack && !pops)) goto done;
    crs = stack && shader_resolve_crs(insn, n, pops);
    seen[at] = 1;
    i = at + 1 + ((at + 1 - 10) % 4 == 0);
    if (i < n) {
        seen[i] = 1;
        work[nwork++] = i;
    }
    while (nwork) {
        i = work[--nwork];
        for (;;) {
            u64 w = insn[i];
            const Op* op = decode(w);
            u32 next = i + 1 + ((i + 1 - 10) % 4 == 0);
            count = 0;
            if (!op || reads_predicate(w, op->id, p)) goto done;
            if (B(w, 16, 4) == 7 && (op->id == O_AL2P || predicate_comparison(op->id)) && writes_predicate(w, op->id, p))
                break;
            switch (op->id) {
                case O_BRA:
                    if (B(w, 5, 1)) goto done;
                    targets[count++] = bra_target(w, i);
                    break;
                case O_BRX:
                    count = shader_brx_targets(insn, i, n, targets, 64);
                    if (count < 0) goto done;
                    break;
                case O_SYNC:
                case O_BRK:
                case O_CONT:
                    if (!crs || !pops[i]) goto done;
                    targets[count++] = pops[i];
                    break;
                case O_EXIT:
                    if (pexit) goto done;
                    break;
                case O_JMP:
                case O_CAL:
                case O_RET:
                case O_PRET: goto done;
                default: break;
            }
            for (t = 0; t < count; t++) {
                if (targets[t] >= n || !IDX_OK(targets[t])) goto done;
                if (!seen[targets[t]]) {
                    seen[targets[t]] = 1;
                    work[nwork++] = targets[t];
                }
            }
            if ((count || op->id == O_EXIT) && flow_always(w)) break;
            if (next >= n || seen[next]) break;
            seen[next] = 1;
            i = next;
        }
    }
    unseen = 1;
done:
    free(seen);
    free(work);
    free(pops);
    return unseen;
}

static int supported(u64 w, int op) {
    switch (op) {
        case O_EXIT: return cc_valid(B(w, 0, 5));
        case O_NOP:
        case O_DEPBAR:
        case O_MEMBAR:
        case O_VOTE_VTG: return 1;
        case O_VOTE: return B(w, 48, 2) < 3;
        case O_FLO_R:
        case O_FLO_C:
        case O_FLO_I: return !B(w, 47, 1);
        case O_POPC_R:
        case O_POPC_C:
        case O_POPC_I: return 1;
        case O_SHFL: return 1;
        case O_FSWZADD: return !B(w, 47, 1);
        case O_VMNMX: return !B(w, 47, 1);
        case O_VMAD:
            return (((w >> 48) & ~3ull) == 0x5f04 || ((w >> 48) & ~3ull) == 0x5f00) && !B(w, 47, 1) && !B(w, 51, 5);
        case O_VSETP: return 1;
        case O_RED: return global_atomic_opcode(w, 1) != 0;
        case O_ATOM: return global_atomic_opcode(w, 0) != 0;
        case O_ATOMS: return shared_atomic_opcode(w) != 0;
        /* the scalar video instructions but their CC (bit 47), whose flags no source describes */
        case O_VADD:
        case O_VABSDIFF:
        case O_VSET:
        case O_VSHL:
        case O_VSHR: return !B(w, 47, 1);
        /* PIXLD's MyIndex (mode 5, the sample) as Eden translates it, and CovMask (1, the coverage), with no address
           and no predicate */
        case O_PIXLD:
            return (B(w, 31, 3) == 5 || B(w, 31, 3) == 1) && B(w, 45, 3) == 7 && B(w, 8, 8) == 255 && !B(w, 20, 8);
        /* PRMT modes 0-6; modes 7-15 are not translated */
        case O_PRMT_R:
        case O_PRMT_RC:
        case O_PRMT_CR:
        case O_PRMT_I: return B(w, 48, 4) <= 6;
        /* compare and swap of 32 bits, the comparison in Rb and the new value in Rb + 1, as Mesa's NAK encodes it:
           global (bit 49 clear, data layout 50-51 packed) and shared (sub-op 4) */
        case O_ATOM_CAS: return !B(w, 49, 1) && !B(w, 50, 2);
        case O_ATOMS_CAS: return B(w, 52, 4) == 4 && !B(w, 28, 2);
        case O_LDL:
        case O_STL: return B(w, 48, 3) <= 7;
        case O_LDS:
        case O_STS: return B(w, 48, 3) <= 7;
        /* BAR.SYNC 0 for the whole workgroup: arrivals, reductions and thread counts are other barriers */
        case O_BAR:
            return (w & 0x0000009B00000000ull) == 0x0000008000000000ull && B(w, 43, 1) && !B(w, 8, 8) && B(w, 44, 1) &&
                   !B(w, 20, 12);
        case O_S2R: {
            u32 sr = B(w, 20, 8);
            /* SR_AFFINITY (0x1c) is not modeled; the clock registers require the shader-clock feature below. */
            return sr == 0 || sr == 0x11 || sr == 0x12 || sr == 0x13 || sr == 0x15 || sr == 0x16 || sr == 0x17 ||
                   sr == 0x1c || sr == 0x1d || sr == 0x1e || sr == 0x1f || (sr >= 0x20 && sr <= 0x23) ||
                   (sr >= 0x25 && sr <= 0x27) || sr == 0x38 || sr == 0x39 || sr == 0x3a || sr == 0x3b || sr == 0x3c ||
                   (sr >= 0x50 && sr <= 0x53);
        }
        /* Attribute addresses use the same byte offsets as ALD/AST. The range predicate is not modeled: a program
           whose AL2P predicate something can read is refused (al2p_predicate_unseen) */
        case O_AL2P: return 1;
        /* ALD and AST: a patch's attributes (bit 31) are its tessellation levels and 30 vectors from 0x20. Which stage
           has which is stage_has's. ALD.O (bit 32) reads a control program's per-vertex outputs. With an index
           register (an AL2P's result), an input vector of the header's map, at the register plus addr */
        case O_ALD: {
            u32 addr = B(w, 20, 10), count = B(w, 47, 2) + 1;
            if (B(w, 0, 8) + count > 255) return 0;
            if (B(w, 8, 8) != 255) return !B(w, 31, 1) && !B(w, 32, 1);
            if (B(w, 31, 1)) return addr + count * 4 <= 0x200;
            return (addr >= 0x70 && addr + count * 4 <= 0x280) ||
                   ((addr == 0x60 || addr == 0x2f4 || addr == 0x2f8 || addr == 0x2fc) && count == 1) ||
                   (addr == 0x2f0 && count <= 2);
        }
        case O_AST: {
            u32 addr = B(w, 20, 10), count = B(w, 47, 2) + 1;
            if (B(w, 8, 8) != 255 || !(B(w, 0, 8) + count <= 255 || (B(w, 0, 8) == 255 && count == 1))) return 0;
            if (B(w, 31, 1)) return addr + count * 4 <= 0x200;
            return ((addr == 0x64 || addr == 0x68 || addr == 0x6c) && count == 1) || (addr >= 0x70 && addr + count * 4 <= 0x80) ||
                   (addr >= 0x80 && addr + count * 4 <= 0x280) || (addr >= 0x2c0 && addr + count * 4 <= 0x2e0);
        }
        /* OUT emits and ends geometry primitives on the stream its third operand names */
        case O_OUT_R:
        case O_OUT_C:
        case O_OUT_I: return 1;
        /* ISBE handles are represented by vertex indices. Input/output buffers and allocation modes do not
           change that index; ALD selects the interface to read. Skewed addresses remain unsupported. */
        case O_ISBERD: return B(w, 8, 8) != 255 && !B(w, 24, 8) && !B(w, 31, 1);
        case O_KIL: return cc_valid(B(w, 0, 5));
        /* BRA/CAL/PRET bit 5 selects a constant-buffer target; BRX bit 6 enables LMT. The other flow forms below
           also require bit 5 clear in their supported encoding. */
        case O_BRA: return cc_valid(B(w, 0, 5)) && !B(w, 5, 1);
        case O_CAL: return !B(w, 5, 1);
        case O_PRET: return !B(w, 5, 1);
        /* EXIT consumes a PEXIT token and branches to its target. */
        case O_PEXIT: return !B(w, 5, 1);
        case O_RET: return cc_valid(B(w, 0, 5)) && !B(w, 5, 1);
        case O_BRX: return cc_valid(B(w, 0, 5)) && !B(w, 5, 2);
        case O_SSY:
        case O_PBK:
        case O_PCNT: return !B(w, 5, 1);
        case O_SYNC:
        case O_BRK:
        case O_CONT: return cc_valid(B(w, 0, 5)) && !B(w, 5, 1);
        case O_MOV32I: return B(w, 12, 4) == 1 || B(w, 12, 4) == 15;
        case O_MOV_R:
        case O_MOV_C:
        case O_MOV_I: return 1;
        /* Saturating adds and IMNMX's XLO/XHI aren't translated. */
        case O_IADD32I: return !B(w, 54, 1);
        case O_IMUL_R:
        case O_IMUL_C:
        case O_IMUL_I:
        case O_IMAD_R:
        case O_IMAD_RC:
        case O_IMAD_CR:
        case O_IMAD_I: return !B(w, 47, 1) && !(op >= O_IMAD_R && op <= O_IMAD_I && B(w, 50, 1));
        case O_IMUL32I: return !B(w, 52, 1);
        case O_IMAD32I: return !B(w, 52, 1) && !B(w, 50, 1);
        case O_IADD3_R: return 1;
        case O_IADD3_C:
        case O_IADD3_I: return 1;
        case O_IMNMX_R:
        case O_IMNMX_C:
        case O_IMNMX_I: return !B(w, 43, 2);
        case O_IADD_R:
        case O_IADD_C:
        case O_IADD_I: return !B(w, 50, 1);
        case O_LDG:
        case O_STG: return B(w, 48, 3) <= 7;
        case O_TXQ:
        case O_TXQ_B: return B(w, 22, 6) == 1;
        case O_TLD4S: return 1;
        /* TLD4.B bit 40 and TLD bit 54 request a minimum LOD that this translator does not decode. */
        case O_TLD4:
            return (B(w, 28, 3) <= 3 || B(w, 28, 3) >= 6) &&
                   (!B(w, 54, 2) || (B(w, 54, 2) <= 2 && (B(w, 28, 3) >> 1) <= 1));
        case O_TLD4_B:
            return !B(w, 40, 1) && (B(w, 28, 3) <= 3 || B(w, 28, 3) >= 6) &&
                   (!B(w, 36, 2) || (B(w, 36, 2) <= 2 && (B(w, 28, 3) >> 1) <= 1));
        case O_I2I_R:
        case O_I2I_C:
        case O_I2I_I:
            return B(w, 8, 2) <= 2 && B(w, 10, 2) <= 2 && B(w, 41, 3) <= 3 &&
                   (B(w, 10, 2) != 1 || !(B(w, 41, 3) & 1)) && (B(w, 10, 2) != 2 || !B(w, 41, 3));
        case O_LOP32I: return 1;
        case O_LOP_R:
        case O_LOP_C:
        case O_LOP_I: return 1;
        case O_LOP3_R:
        case O_LOP3_C:
        case O_LOP3_I: return 1;
        case O_SEL_R:
        case O_SEL_C:
        case O_SEL_I: return 1;
        /* every select but CSFU (3), which no source describes, and 5-7, which aren't selects */
        case O_XMAD_R:
        case O_XMAD_I: return B(w, 50, 3) <= 2 || B(w, 50, 3) == 4;
        case O_XMAD_RC:
        case O_XMAD_CR: return B(w, 50, 2) <= 2;
        /* the plain addressing mode: IS, IL and ISL (bits 44-45) take the bank or address from the register */
        case O_LDC:
            return B(w, 48, 3) <= 5 && B(w, 36, 5) < 18 && (B(w, 48, 3) != 5 || B(w, 0, 8) < 254) && !B(w, 44, 2);
        case O_SHF_LR:
        case O_SHF_LI:
        case O_SHF_RR:
        case O_SHF_RI: return B(w, 37, 2) != 1 && !B(w, 47, 1) && !B(w, 48, 2);
        case O_LEA_LR:
        case O_LEA_LC:
        case O_LEA_LI:
        case O_LEA_HR:
        case O_LEA_HC: return 1;
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I: return B(w, 45, 2) < 3;
        case O_ICMP_R:
        case O_ICMP_RC:
        case O_ICMP_CR:
        case O_ICMP_I: return 1;
        case O_FCMP_R:
        case O_FCMP_RC:
        case O_FCMP_CR:
        case O_FCMP_I: return 1;
        case O_BFI_R:
        case O_BFI_RC:
        case O_BFI_CR:
        case O_BFI_I: return 1;
        /* F2F/F2I format 0 is invalid. I2F refuses 16-bit selectors 1 and 3 and every selector on 32/64-bit
           sources. Conversion CC (bit 47) is refused, as are undocumented shift, FLO and FSWZADD flag forms. */
        case O_F2F_R:
        case O_F2F_C:
        case O_F2F_I:
        case O_F2I_R:
        case O_F2I_C:
        case O_F2I_I:
            return !B(w, 47, 1) && B(w, 10, 2) != 0 && B(w, 8, 2) != 0 &&
                   ((op != O_F2F_C && op != O_F2I_C) || B(w, 10, 2) != 3 || !(B(w, 20, 14) & 1));
        case O_I2F_R:
        case O_I2F_C:
        case O_I2F_I:
            return !B(w, 47, 1) && (B(w, 8, 2) == 1 || B(w, 8, 2) == 2 || B(w, 8, 2) == 3) &&
                   (B(w, 10, 2) != 1 || !(B(w, 41, 2) & 1)) && (B(w, 10, 2) < 2 || !B(w, 41, 2)) &&
                   (op != O_I2F_C || B(w, 10, 2) != 3 || !(B(w, 20, 14) & 1));
        case O_ISCADD_R:
        case O_ISCADD_C:
        case O_ISCADD_I:
        case O_ISCADD32I: return 1;
        case O_BFE_R:
        case O_BFE_C:
        case O_BFE_I: return 1;
        case O_SHL_R:
        case O_SHL_C:
        case O_SHL_I: return !B(w, 43, 1) && !B(w, 47, 1);
        case O_SHR_R:
        case O_SHR_C:
        case O_SHR_I: return !B(w, 43, 2) && !B(w, 47, 1);
        case O_ISET_R:
        case O_ISET_C:
        case O_ISET_I: return B(w, 45, 2) < 3;
        case O_FADD_R:
        case O_FADD_C:
        case O_FADD_I: return !B(w, 47, 1);
        case O_FADD32I: return !B(w, 52, 1);
        case O_FMUL_R:
        case O_FMUL_C:
        case O_FMUL_I: return !B(w, 47, 1) && B(w, 41, 3) < 7;
        case O_FMUL32I: return B(w, 53, 2) <= 2 && !B(w, 52, 1);
        case O_FFMA_R:
        case O_FFMA_RC:
        case O_FFMA_CR:
        case O_FFMA_I: return !B(w, 47, 1);
        case O_FFMA32I: return !B(w, 52, 1);
        case O_FMNMX_R:
        case O_FMNMX_C:
        case O_FMNMX_I: return !B(w, 47, 1);
        case O_IPA: {
            u32 addr = B(w, 30, 8) * 4;
            /* IPA.IDX: the address is the index register's alone (an AL2P's result), as Eden and Ryujinx read it */
            if (B(w, 38, 1)) return B(w, 8, 8) != 255;
            return ((addr >= 0x80 && addr < 0x280) || (addr >= 0x70 && addr < 0x80) || addr == 0x60 || addr == 0x64 ||
                    addr == 0x2e0 || addr == 0x2e4 || addr == 0x3fc) &&
                   !B(w, 38, 1);
        }
        case O_PSETP: return B(w, 24, 2) < 3 && B(w, 45, 2) < 3;
        case O_PSET: return B(w, 24, 2) < 3 && B(w, 45, 2) < 3;
        case O_CSETP:
        case O_CSET: return cc_valid(B(w, 8, 5)) && B(w, 45, 2) < 3;
        case O_R2P_R:
        case O_R2P_C:
        case O_R2P_I:
        case O_P2R_R:
        case O_P2R_C:
        case O_P2R_I: return 1;
        /* every function, RCP64H and RSQ64H (6, 7) included */
        case O_MUFU: return B(w, 20, 4) <= 8;
        case O_RRO_R:
        case O_RRO_C:
        case O_RRO_I: return 1;
        /* FCHK.DIVIDE (mode 0) says whether a division needs its slow path: always, which the slow path computes
           exactly. No source documents the condition, and Eden and Ryujinx don't translate it. */
        case O_FCHK_R:
        case O_FCHK_C:
        case O_FCHK_I: return !B(w, 39, 6);
        /* cache control and the interrupt and breakpoint instructions change nothing a shader computes */
        case O_CCTL:
        case O_CCTLL:
        case O_IDE:
        case O_BPT: return 1;
        /* CS2R reads a 64-bit clock or global timer into a pair. SRZ zeroes the pair. */
        case O_CS2R: return B(w, 20, 8) == 0x50 || B(w, 20, 8) == 0x52 || B(w, 20, 8) == 0xff;
        case O_TEXS: return B(w, 53, 4) <= 13 && (B(w, 28, 8) == 255 || B(w, 50, 3) <= 4);
        /* the encodings Eden lists: 3, 9, 10, 11 and 13 aren't TLDS's */
        case O_TLDS:
            return B(w, 53, 4) <= 12 && B(w, 53, 4) != 3 && B(w, 53, 4) != 9 && B(w, 53, 4) != 10 &&
                   B(w, 53, 4) != 11 && (B(w, 28, 8) == 255 || B(w, 50, 3) <= 4);
        /* every type up to 3D (a fetch can't address a cube), multisample only on 2D and 2D arrays */
        case O_TLD:
        case O_TLD_B: return !B(w, 54, 1) && B(w, 28, 3) <= 4 && (!B(w, 50, 1) || B(w, 28, 3) == 2 || B(w, 28, 3) == 3);
        /* Formatted and raw 32-bit loads from 2D, array and volume surfaces;
           buffer loads use storage texel buffers. */
        case O_SULD:
            return ((B(w, 33, 3) == 3 || B(w, 33, 3) == 4 || B(w, 33, 3) == 5) &&
                    (!B(w, 52, 1) || B(w, 20, 4) == 4)) ||
                   (B(w, 33, 3) == 1 && B(w, 52, 1) && B(w, 20, 4) == 4);
        /* stores and atomics also reach 2D array surfaces (4), the layer in the coordinates' third register; stores
           reach 3D surfaces (5) too, z in that register */
        case O_SUST:
            return !B(w, 52, 1) && (B(w, 33, 3) == 3 || B(w, 33, 3) == 4 || B(w, 33, 3) == 5 || B(w, 33, 3) == 1);
        /* 32-bit atomics on buffers, 2D images, arrays and volumes, other than
           INC and DEC, which wrap at the operand. Bindless is bit 54 here. */
        case O_SUATOM:
            return (B(w, 33, 3) == 3 || B(w, 33, 3) == 4 || B(w, 33, 3) == 5 || B(w, 33, 3) == 1) &&
                   (B(w, 51, 3) <= 1 || B(w, 51, 3) == 6) && B(w, 29, 4) <= 8 && B(w, 29, 4) != 3 && B(w, 29, 4) != 4 &&
                   !B(w, 49, 2);
        /* reductions of 32 bits, U32 or S32 (bits 20-22), on 2D, 2D array, 3D and buffer surfaces (a compute program's
           histogram adds into a buffer): ADD is the same for both, MIN and MAX compare S32 signed */
        case O_SURED:
            return (B(w, 33, 3) == 3 || B(w, 33, 3) == 4 || B(w, 33, 3) == 5 || B(w, 33, 3) == 1) && B(w, 20, 3) <= 1 &&
                   B(w, 24, 3) <= 2 && !B(w, 23, 1) && !B(w, 49, 2);
        case O_HADD2_R:
        case O_HADD2_C:
        case O_HADD2_I:
        case O_HMUL2_R:
        case O_HMUL2_C:
        case O_HMUL2_I:
        case O_HFMA2_R:
        case O_HFMA2_RC:
        case O_HFMA2_CR:
        case O_HFMA2_I:
        case O_HADD2_32I:
        case O_HMUL2_32I:
        case O_HFMA2_32I: return 1;
        case O_HSET2_R:
        case O_HSET2_C:
        case O_HSET2_I: return 1;
        case O_HSETP2_R:
        case O_HSETP2_C:
        case O_HSETP2_I: return 1;
        case O_DSET_R:
        case O_DSET_C:
        case O_DSET_I:
        case O_DSETP_R:
        case O_DSETP_C:
        case O_DSETP_I: return 1;
        case O_DADD_R:
        case O_DADD_C:
        case O_DADD_I:
        case O_DMUL_R:
        case O_DMUL_C:
        /* the rounding mode (DADD's and DMUL's 39-40, DFMA's 50-51) is round to nearest, as FADD's and Eden's are:
           SPIR-V sets rounding per conversion, not per instruction */
        case O_DMUL_I: return !B(w, 47, 1);
        case O_DFMA_R:
        case O_DFMA_RC:
        case O_DFMA_CR:
        case O_DFMA_I: return !B(w, 47, 1);
        case O_DMNMX_R:
        case O_DMNMX_C:
        case O_DMNMX_I: return !B(w, 47, 1);
        case O_TEX: return texture_sample_supported(w, 0);
        case O_TEX_B: return texture_sample_supported(w, 1);
        /* every type but a 3D array, which doesn't exist */
        case O_TMML:
        case O_TMML_B: return B(w, 28, 3) != 5;
        /* TXD supports 1D and 2D textures and their arrays, without a LOD clamp (bit 50). */
        case O_TXD:
        case O_TXD_B: return B(w, 28, 3) < 4 && !B(w, 50, 1);
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I: return 1;
        case O_FSET_R:
        case O_FSET_C:
        case O_FSET_I: return 1;
        default: return 0;
    }
}

/* The storage image slot for a surface operation: images of one handle share a slot when they have the same
   dimension (3 a 2D image, 4 a 2D array, 5 a 3D image, 1 a buffer) and are both R32ui (atomics, and loads from buffers) or both
   not. */
static int direct_image_slot(const ShaderInfo* info, u32 off, int bindless, u32 bank, int r32ui, int type) {
    int slot = 0;
    while (slot < info->nimg && (info->img[slot].cbuf_off != off || info->img[slot].bindless != bindless ||
                                 (info->img[slot].kind == 1) != r32ui || info->img[slot].type != type ||
                                 (bindless && info->img[slot].bl_cbuf != bank)))
        slot++;
    return slot;
}

/* The attribute instructions each stage has. Vertex, tessellation and geometry programs read their inputs and store
   their outputs, the stages after the vertex one per vertex of their input primitive (its position too).
   Tessellation control programs store the patch's attributes, which tessellation evaluation programs read with the
   tessellation coordinate. Only geometry programs emit vertices. */
static int stage_has(u64 w, int op, int stage) {
    int between = stage == ST_TCS || stage == ST_TES || stage == ST_GS;
    u32 addr = B(w, 20, 10);
    if (op == O_ALD) {
        if (stage == ST_FS || stage == ST_CS) return 0;
        if (B(w, 8, 8) != 255) return 1; /* indexed: the stage's generic inputs */
        if (B(w, 31, 1)) return stage == ST_TCS || stage == ST_TES;
        if (stage == ST_TCS && B(w, 32, 1) && addr < 0x70) return 0;
        if (addr == 0x2f8 || addr == 0x2fc) return stage == ST_VS;
        if (addr == 0x2f0 || addr == 0x2f4) return stage == ST_TES;
        return addr >= 0x80 || between;
    }
    if (op == O_AST) {
        if (stage == ST_FS || stage == ST_CS || (B(w, 31, 1) && stage != ST_TCS)) return 0;
        if (stage == ST_TCS && !B(w, 31, 1) && (addr == 0x64 || addr == 0x6c || addr >= 0x2c0)) return 0;
        return 1;
    }
    if (op == O_OUT_R || op == O_OUT_C || op == O_OUT_I) return stage == ST_GS;
    if (op == O_ISBERD) return between;
    if (op == O_S2R && ((B(w, 20, 8) >= 0x20 && B(w, 20, 8) <= 0x23) || (B(w, 20, 8) >= 0x25 && B(w, 20, 8) <= 0x27)))
        return stage == ST_CS;
    if (op == O_S2R && B(w, 20, 8) == 0x11) return stage == ST_TCS || stage == ST_GS;
    if (op == O_BAR) return stage == ST_CS || stage == ST_TCS;
    if (op == O_TMML || op == O_TMML_B || op == O_IPA || op == O_KIL || op == O_PIXLD) return stage == ST_FS;
    return 1;
}

/* An indexed handle: the LDC that loads it, through MOVs, takes its address from a register. Its flags
   (SNX_TEX_INDEXED and the stride's shift: the SHL that made the address, or 2 for words) and *ldc, that LDC's
   instruction plus one. 0 when the handle isn't indexed, or can't be: the device has no texture arrays, or the
   stage is compute, which binds none. */
static u32 direct_indexed_handle(const u64* insn, u32 n, u32 index, u32 reg, int stage, u32* ldc) {
    int j = -1, k, depth, op = 0;
    u64 w = 0;
    u32 shift = 2;
    if (!g_shader_texture_arrays || stage == ST_CS) return 0;
    for (depth = 0; depth < 6; depth++) {
        j = direct_reaching_def(insn, n, index, reg);
        if (j < 0) return 0;
        w = insn[j];
        op = decode(w)->id;
        if (op != O_MOV_R) break;
        index = (u32)j;
        reg = B(w, 20, 8);
    }
    if (depth == 6 || op != O_LDC || B(w, 8, 8) == 255 || B(w, 48, 3) != 4) return 0;
    k = direct_reaching_def(insn, n, (u32)j, B(w, 8, 8));
    if (k >= 0 && decode(insn[k])->id == O_SHL_I && B(insn[k], 20, 5) >= 2 && B(insn[k], 20, 5) <= 15)
        shift = B(insn[k], 20, 5);
    *ldc = (u32)j + 1;
    return SNX_TEX_INDEXED | shift << 4;
}

/* Whether a bindless slot reads the handle a tracker found: its bank and second read, or for an indexed handle the
   same LDC (bl2_off holds its instruction plus one, as no second read is kept). */
static int direct_bindless_matches(const ShTex* t, u32 bank, int handle_count, u32 bank2, u32 off2, u32 indexed,
                                   u32 ldc) {
    if ((u32)(t->flags & 0xF4) != indexed || t->bl_cbuf != bank) return 0;
    if (indexed) return t->bl2_off == ldc;
    return t->bl2 == (handle_count == 2) && (handle_count != 2 || (t->bl2_cbuf == bank2 && t->bl2_off == off2));
}

/* What a TXQ's texture is (Maxwell's type field halved, and array), from another instruction of the program reading
   the same texture: a bound one by its handle's offset, a bindless one by the first word of its tracked handle (the
   texture half: the sampler half may differ, as TXQ doesn't use it). TXQ itself doesn't say, and one game queries a 3D
   texture's depth with it, which a 2D slot can't bind. 0 when nothing else reads it: the query is then for a 2D one */
static int direct_query_type(const u64* insn, u32 n, int bindless, u32 bank, u32 off, u32* type, u32* array) {
    for (u32 k = 11; k < n; k++) {
        const Op* op;
        u32 ty, kb, ko, kb2, ko2;
        int b;
        if (!IDX_OK(k) || !(op = decode(insn[k]))) continue;
        switch (op->id) {
            case O_TEX:
            case O_TLD:
            case O_TLD4:
            case O_TMML: b = 0; break;
            case O_TEX_B:
            case O_TLD_B:
            case O_TLD4_B:
            case O_TMML_B: b = 1; break;
            default: continue;
        }
        if (b != bindless) continue;
        ty = B(insn[k], 28, 3);
        if (b) {
            if (!direct_track_image_handle_pair(insn, n, k, B(insn[k], 20, 8), &kb, &ko, &kb2, &ko2, 0) || kb != bank ||
                ko != off)
                continue;
        } else if (B(insn[k], 36, 13) != off)
            continue;
        *type = ty >> 1;
        *array = ty & 1;
        return 1;
    }
    return 0;
}

/* The 2D texture slot a TXQ queries, bound or by a tracked handle, when nothing else says what the texture is
   (direct_query_type). */
static int direct_query_slot(const ShTex* textures, int count, u32 off, int bindless, u32 bank, int handle_count,
                             u32 bank2, u32 off2, u32 indexed, u32 ldc) {
    int slot = 0;
    while (slot < count &&
           (textures[slot].cbuf_off != off || textures[slot].type != 1 || textures[slot].array ||
            textures[slot].shadow || (textures[slot].flags & 3) || textures[slot].bindless != bindless ||
            (bindless && !direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
        slot++;
    return slot;
}

/* The slot of a texture with this type (Maxwell's type field halved), array, shadow and kind (sampled or
   multisample), read through that handle. Raw surfaces have their own slot lookup. */
static int direct_typed_slot(const ShTex* textures, int count, u32 off, u32 type, u32 array, u32 shadow, u32 kind,
                             int bindless, u32 bank, int handle_count, u32 bank2, u32 off2, u32 indexed, u32 ldc) {
    int slot = 0;
    while (slot < count &&
           (textures[slot].cbuf_off != off || textures[slot].type != type || textures[slot].array != array ||
            textures[slot].shadow != shadow || textures[slot].bindless != bindless ||
            (textures[slot].flags & 3) != kind ||
            (bindless && !direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
        slot++;
    return slot;
}

/* direct_typed_slot's slot, added when there is none; -1 when the 32 slots are taken */
static int direct_add_typed_slot(ShTex* textures, int* count, u32 off, u32 type, u32 array, u32 shadow, u32 kind,
                                 int bindless, u32 bank, int handle_count, u32 bank2, u32 off2, u32 indexed, u32 ldc) {
    int slot = direct_typed_slot(textures, *count, off, type, array, shadow, kind, bindless, bank, handle_count, bank2,
                                 off2, indexed, ldc);
    if (slot < *count) return slot;
    if (*count >= SH_MAXTEX) return -1;
    memset(&textures[slot], 0, sizeof textures[slot]);
    textures[slot].cbuf_off = off;
    textures[slot].type = (u8)type;
    textures[slot].array = (u8)array;
    textures[slot].shadow = (u8)shadow;
    textures[slot].flags = (u8)kind;
    textures[slot].bindless = (u8)bindless;
    textures[slot].bl_cbuf = (u8)bank;
    textures[slot].bl2 = handle_count == 2;
    textures[slot].bl2_cbuf = (u8)bank2;
    textures[slot].bl2_off = off2;
    if (indexed) {
        textures[slot].flags |= (u8)indexed;
        textures[slot].bl2_off = ldc;
    }
    (*count)++;
    return slot;
}

/* For this instruction, an indexed texture's sampler is the array element its LDC chose, clamped to the array */
static void direct_swap_element(DirectCtx* c, int slot) {
    int table = c->tex_table[slot];
    u32 limit = spv_emit_constant_u32(&c->emit, c->u32_type, SNX_TEX_ARRAY - 1);
    u32 index = spv_emit_load(&c->emit, c->u32_type, c->tex_index[slot]);
    index = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, limit, index), limit, index);
    if (c->tex_map_buffer) {
        u32 base = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)table * SNX_TEX_ARRAY);
        u32 flat = spv_emit_iadd_u32(&c->emit, c->u32_type, base, index);
        u32 element =
            spv_emit_shift_right_u32(&c->emit, c->u32_type, flat, spv_emit_constant_u32(&c->emit, c->u32_type, 2));
        u32 component =
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, flat, spv_emit_constant_u32(&c->emit, c->u32_type, 3));
        index = spv_emit_uniform_component(&c->emit, c->tex_map_pointer, c->tex_map_buffer, c->zero, element, component,
                                           c->u32_type);
    }
    c->active_index = index;
    c->sampler[slot] = spv_emit_sampler_array_element(&c->emit, c->sampled_type[slot], c->texture_array, index);
    if (c->depth_array)
        c->depth_sampler = spv_emit_sampler_array_element(&c->emit, c->depth_type, c->depth_array, index);
    c->swapped = (u32)slot + 1;
}

/* TEXS's encodings (bits 53-56): 0 1D.LZ, 1 2D, 2 2D.LZ, 3 2D.LL, 4 2D.DC, 5 2D.LL.DC, 6 2D.LZ.DC, 7 ARRAY_2D,
   8 ARRAY_2D.LZ, 9 ARRAY_2D.LZ.DC, 10 3D, 11 3D.LZ, 12 CUBE, 13 CUBE.LL. The registers, as Eden reads them: x in Ra,
   y in Ra + 1 (in Rb for 2D and 2D.LZ, and for an array, whose layer is in Ra and x in Ra + 1), a 3D or cube
   texture's z in Rb, then the level and the depth reference in Rb or Rb + 1. */
static int texs_type(u32 encoding) { return encoding >= 12 ? 3 : encoding >= 10 ? 2 : encoding == 0 ? 0 : 1; }
static int texs_array(u32 encoding) { return encoding >= 7 && encoding <= 9; }
static int texs_shadow(u32 encoding) { return encoding == 4 || encoding == 5 || encoding == 6 || encoding == 9; }
static u32 texs_y(u32 encoding, u32 a, u32 b) {
    return encoding == 1 || encoding == 2 || texs_array(encoding) ? b : a + 1;
}
/* whether the encoding takes an explicit level (LZ, LL), and the register of LL's level (255 for LZ's 0) */
static int texs_explicit(u32 encoding) {
    return encoding == 0 || encoding == 2 || encoding == 3 || encoding == 5 || encoding == 6 || encoding == 8 ||
           encoding == 9 || encoding == 11 || encoding == 13;
}
static u32 texs_level(u32 encoding, u32 b) { return encoding == 3 || encoding == 5 ? b : encoding == 13 ? b + 1 : 255; }
static u32 texs_dref(u32 encoding, u32 b) { return encoding == 5 || encoding == 9 ? b + 1 : b; }

static int half_arithmetic(int op) {
    return op == O_HADD2_R || op == O_HADD2_C || op == O_HADD2_I || op == O_HADD2_32I || op == O_HMUL2_R ||
           op == O_HMUL2_C || op == O_HMUL2_I || op == O_HMUL2_32I || op == O_HFMA2_R || op == O_HFMA2_RC ||
           op == O_HFMA2_CR || op == O_HFMA2_I || op == O_HFMA2_32I;
}

/* The predicate a texture fetch writes when a texel it read isn't resident (bits 51-53), PT (7) for none */
static u32 sparse_predicate(u64 w, int op) {
    return op == O_TEX || op == O_TEX_B || op == O_TLD || op == O_TLD_B || op == O_TLD4 || op == O_TLD4_B ||
                   op == O_TXD || op == O_TXD_B
               ? B(w, 51, 3)
               : 7;
}

static u32 texture_float_component(DirectCtx* c, u32 value, u32 scalar, u32 i32_type) {
    if (scalar == c->f32_type) return value;
    return scalar == i32_type ? spv_emit_convert_s_to_f(&c->emit, c->f32_type, value)
                              : spv_emit_convert_u_to_f(&c->emit, c->f32_type, value);
}

static u32 pack_texture_halves(DirectCtx* c, u32 vec2, u32 first, u32 second) {
    return spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16,
                               spv_emit_vec2_construct(&c->emit, vec2, first, second));
}

/* Sampling with a register offset uses normalized coordinates because SPIR-V requires a constant sample Offset. */
static u32 direct_offset_coords(DirectCtx* c, u32 coords, u32 offset, u32 coord_count, u32 offset_count, int slot,
                                u32 level, u32 vec2, u32 vec3, u32 vec2_i32, u32 vec3_i32, u32 i32_type) {
    u32 size = spv_emit_image_query_size_lod(&c->emit, coord_count == 3 ? vec3_i32 : vec2_i32, c->sampled_type[slot],
                                             c->sampler[slot], level);
    if (c->depth_array && c->swapped == (u32)slot + 1) {
        u32 depth_size = spv_emit_image_query_size_lod(&c->emit, coord_count == 3 ? vec3_i32 : vec2_i32, c->depth_type,
                                                       c->depth_sampler, level);
        size = spv_emit_select(&c->emit, coord_count == 3 ? vec3_i32 : vec2_i32, indexed_is_depth(c, slot), depth_size,
                               size);
    }
    u32 moved[4];
    for (u32 k = 0; k < coord_count; k++) {
        u32 coord = spv_emit_vec4_extract(&c->emit, c->f32_type, coords, k);
        if (k >= offset_count) {
            moved[k] = coord;
            continue;
        }
        u32 delta = spv_emit_vec4_extract(&c->emit, i32_type, offset, k);
        u32 extent = spv_emit_vec4_extract(&c->emit, i32_type, size, k);
        moved[k] =
            spv_emit_fadd(&c->emit, c->f32_type, coord,
                          spv_emit_fdiv(&c->emit, c->f32_type, spv_emit_convert_s_to_f(&c->emit, c->f32_type, delta),
                                        spv_emit_convert_s_to_f(&c->emit, c->f32_type, extent)));
    }
    return coord_count == 3 ? spv_emit_vec3_construct(&c->emit, vec3, moved[0], moved[1], moved[2])
                            : spv_emit_vec2_construct(&c->emit, vec2, moved[0], moved[1]);
}

/* Gather AOFFI stores signed six-bit x/y offsets in the low bits of two consecutive bytes. */
static u32 direct_gather_offset(DirectCtx* c, u32 raw, u32 shift, int zero_y, u32 i32_type, u32 vec2_i32) {
    u32 bits = spv_emit_bitcast(&c->emit, i32_type, raw);
    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 6);
    u32 x = spv_emit_bitfield_sextract(&c->emit, i32_type, bits, spv_emit_constant_u32(&c->emit, c->u32_type, shift),
                                       width);
    u32 y = zero_y ? spv_emit_constant_u32(&c->emit, i32_type, 0)
                   : spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, shift + 8), width);
    return spv_emit_vec2_construct(&c->emit, vec2_i32, x, y);
}

/* TEXS's and TLDS's results: each component to its own register (bit 59), or as halves two to a register, the
   first pair to Rd and the second to bits 28-35's. A depth compare's one result is x, y and z, with 1.0 in alpha. */
static void texs_store(DirectCtx* c, u64 w, u32 sample, u32 scalar, int shadow, u32 i32_type, u32 vec2, u32 zero_f) {
    u32 regs[4], comps[4], d = B(w, 0, 8);
    int outputs = texs_outputs(w, regs, comps);
    if (B(w, 59, 1)) {
        for (int k = 0; k < outputs; k++) {
            if (regs[k] >= 255) continue;
            reg_write(
                c, regs[k],
                shadow ? spv_emit_bitcast(&c->emit, c->u32_type,
                                          comps[k] == 3 ? spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f) : sample)
                       : texture_component(c, sample, scalar, comps[k]));
        }
        return;
    }
    for (int k = 0; k < outputs; k += 2) {
        u32 pair[2];
        for (int lane = 0; lane < 2; lane++) {
            u32 value = k + lane >= outputs ? zero_f
                        : shadow ? (comps[k + lane] == 3 ? spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f) : sample)
                                 : spv_emit_vec4_extract(&c->emit, scalar, sample, comps[k + lane]);
            pair[lane] = shadow || k + lane >= outputs ? value : texture_float_component(c, value, scalar, i32_type);
        }
        u32 packed = pack_texture_halves(c, vec2, pair[0], pair[1]);
        u32 target = k == 0 ? d : B(w, 28, 8);
        if (target < 255) reg_write(c, target, packed);
    }
}

/* PRMT's byte idx (0-7) of the eight in low (0-3) and high (4-7) */
static u32 prmt_byte(DirectCtx* c, u32 low, u32 high, u32 idx) {
    u32 shift = spv_emit_shift_left_u32(
        &c->emit, c->u32_type,
        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, idx, spv_emit_constant_u32(&c->emit, c->u32_type, 3)),
        spv_emit_constant_u32(&c->emit, c->u32_type, 3));
    u32 upper = spv_emit_i_not_equal(
        &c->emit, c->bool_type,
        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, idx, spv_emit_constant_u32(&c->emit, c->u32_type, 4)), c->zero);
    u32 word = spv_emit_select(&c->emit, c->u32_type, upper, high, low);
    return spv_emit_bitwise_and_u32(&c->emit, c->u32_type, spv_emit_shift_right_u32(&c->emit, c->u32_type, word, shift),
                                    spv_emit_constant_u32(&c->emit, c->u32_type, 0xff));
}

static u32* direct_reject(ShaderInfo* si, const char* reason) {
    si->unsupported = 1;
    snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s", reason);
    return NULL;
}

static u32* direct_reject_with_buffers(ShaderInfo* si, const char* reason, u32* pop_targets, u8* gb_at) {
    free(gb_at);
    free(pop_targets);
    return direct_reject(si, reason);
}

/* gb_at's mark for an access whose buffer is found when it runs (global_dynamic) */
#define GB_DYNAMIC 254

/* A global memory access at i whose base register (reg) the tracking lost, and that can't be found among the
   program's buffers when it runs (an atomic, a sub-word access, or no tracked buffer at all). The reason names the
   last instruction before it that wrote the base, so a log shows what to add without the program */
static u32* untracked_reject(ShaderInfo* si, const u64* insn, u32 i, u32 reg, u8* gb_at, u32* pop_targets) {
    const Op* writer = NULL;
    u32 at = i;
    while (!writer && at > 11) {
        const Op* o;
        at--;
        o = IDX_OK(at) ? decode(insn[at]) : NULL;
        if (o && shader_writes_reg(insn[at], o->id, reg)) writer = o;
    }
    free(gb_at);
    free(pop_targets);
    if (writer)
        snprintf(si->first_unsupported, sizeof si->first_unsupported,
                 "global memory base R%u not tracked at %x: %s at %x", reg, i * 8, writer->name, at * 8);
    else
        snprintf(si->first_unsupported, sizeof si->first_unsupported,
                 "global memory base R%u not tracked at %x: no write before", reg, i * 8);
    si->unsupported++;
    return NULL;
}

/* An LDG or STG of whole words whose base register the tracking lost (gb_at's GB_DYNAMIC): as Eden's SPIR-V backend
   does for such an access (DefineGlobalMemoryFunctions), its 64-bit address is compared with each buffer the
   program binds (cbuf 0's descriptor at gbuf_off, address and size), and the buffer that holds it is accessed.
   Without .E (bit 45) the address is the register zero-extended; the 24-bit offset is signed. A load from an address
   in none of them reads 0, and a store there is dropped, where the console would fault. The buffers' sizes are those
   the renderer binds (vk_gbuf_desc: 16 bytes for 0, at most 16 MiB) */
static void global_dynamic(DirectCtx* c, int stage, u64 w, int op, const u32* gbuf_off, int ngbuf) {
    u32 a = B(w, 8, 8), d = B(w, 0, 8), words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1, k;
    /* an absolute address (RZ) has an unsigned offset, as Eden reads it */
    i32 offset = a == 255 ? (i32)B(w, 20, 24) : (i32)(B(w, 20, 24) << 8) >> 8;
    u32 lo = reg_read(c, a), hi = B(w, 45, 1) && a < 255 ? reg_read(c, a + 1) : c->zero;
    u32 address =
        spv_emit_iadd_u32(&c->emit, c->u32_type, lo, spv_emit_constant_u32(&c->emit, c->u32_type, (u32)offset));
    /* the offset's carry into the high word, or its borrow */
    u32 wrapped = offset < 0 ? spv_emit_u_less(&c->emit, c->bool_type, lo, address)
                             : spv_emit_u_less(&c->emit, c->bool_type, address, lo);
    u32 one = spv_emit_constant_u32(&c->emit, c->u32_type, 1);
    u32 carry = spv_emit_select(&c->emit, c->u32_type, wrapped, one, c->zero);
    u32 data[4];
    hi = offset < 0 ? spv_emit_isub_u32(&c->emit, c->u32_type, hi, carry)
                    : spv_emit_iadd_u32(&c->emit, c->u32_type, hi, carry);
    for (k = 0; k < words; k++) data[k] = op == O_STG ? reg_read(c, d + k) : c->zero;
    if (op == O_LDG)
        for (k = 0; k < words; k++) reg_write(c, d + k, c->zero);
    for (int slot = 0; slot < ngbuf; slot++) {
        u32 then_label, merge_label, base_lo, base_hi, size, relative, borrow, inside;
        u32 bank = SH_GBUF_BANK(gbuf_off[slot]), at = SH_GBUF_OFFSET(gbuf_off[slot]);
        if (gbuf_off[slot] == 0xb0) continue; /* the vertex stage's bare pointer, a buffer of its own */
        base_lo = cbuf_read_dynamic(c, bank, spv_emit_constant_u32(&c->emit, c->u32_type, at));
        base_hi = cbuf_read_dynamic(c, bank, spv_emit_constant_u32(&c->emit, c->u32_type, at + 4));
        /* a bare address has no size beside it: the cap below, of which the renderer binds what is mapped */
        if (shader_gbuf_sized(stage, gbuf_off[slot])) {
            size = cbuf_read_dynamic(c, 0, spv_emit_constant_u32(&c->emit, c->u32_type, at + 8));
            size = spv_emit_select(&c->emit, c->u32_type, spv_emit_i_equal(&c->emit, c->bool_type, size, c->zero),
                                   spv_emit_constant_u32(&c->emit, c->u32_type, 16), size);
        } else
            size = spv_emit_constant_u32(&c->emit, c->u32_type, 16u << 20);
        size = spv_emit_select(
            &c->emit, c->u32_type,
            spv_emit_u_less(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, c->u32_type, 16u << 20), size),
            spv_emit_constant_u32(&c->emit, c->u32_type, 16u << 20), size);
        relative = spv_emit_isub_u32(&c->emit, c->u32_type, address, base_lo);
        borrow = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, address, base_lo), one,
                                 c->zero);
        /* inside: the high words' difference, less the low words' borrow, is 0, and the low one is under the size
           (each access's words lie within the binding, as the tracked path's do) */
        inside = spv_emit_select(
            &c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, relative, size),
            spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_isub_u32(&c->emit, c->u32_type, hi, base_hi), borrow),
            one);
        inside =
            spv_emit_i_equal(&c->emit, c->bool_type, inside, c->zero); /* before the merge, which precedes the branch */
        then_label = spv_emit_id(&c->emit);
        merge_label = spv_emit_id(&c->emit);
        spv_emit_selection_merge(&c->emit, merge_label);
        spv_emit_branch_conditional(&c->emit, inside, then_label, merge_label);
        spv_emit_label(&c->emit, then_label);
        for (k = 0; k < words; k++) {
            u32 word = spv_emit_iadd_u32(&c->emit, c->u32_type,
                                         spv_emit_shift_right_u32(&c->emit, c->u32_type, relative,
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 2)),
                                         spv_emit_constant_u32(&c->emit, c->u32_type, k));
            u32 ptr = spv_emit_storage_access_u32(&c->emit, c->gbuf_pointer, c->gbuf[slot], c->zero,
                                                  gbuf_word_index(c, stage, (u32)slot, word));
            if (op == O_STG)
                spv_emit_store(&c->emit, ptr, data[k]);
            else
                reg_write(c, d + k, spv_emit_load(&c->emit, c->u32_type, ptr));
        }
        spv_emit_branch(&c->emit, merge_label);
        spv_emit_label(&c->emit, merge_label);
    }
}

/* insn: code's words as host numbers (shader_words) */
/* Writing a program's structured control flow (shader_structure): the labels of each scope (header, body, continue
   target and merge, made when it opens), and the open selections */
typedef struct {
    const ShStructure* st;
    u32 next;       /* the token to write next */
    u32* label;     /* 4 a scope, then the selections' merge and else labels, 2 each */
    u8* used;       /* a scope's merge or a selection's merge has a branch to it: a byte each */
    u32 selections; /* open */
    u32 flag;       /* the Function variable of the pending exit: the scope + 1 a break goes on to, 0 for none */
    int open;       /* a block is being written: no branch or return has ended it */
    u32 body_start, body_end;
} StructEmit;

enum { SE_DONE, SE_BODY, SE_END };

static void se_branch(DirectCtx* c, StructEmit* se, u32 label) {
    spv_emit_branch(&c->emit, label);
    se->open = 0;
}

/* a scope's merge: from a break, or from running off its end */
static void se_break(DirectCtx* c, StructEmit* se, u32 scope) {
    se->used[scope] = 1;
    se_branch(c, se, se->label[4 * scope + 3]);
}

static void se_ensure_open(DirectCtx* c, StructEmit* se) {
    if (se->open) return;
    spv_emit_label(&c->emit, spv_emit_id(&c->emit)); /* code nothing reaches, which SPIR-V takes in a block */
    se->open = 1;
}

/* At the merge of a scope that a pending exit may leave through: it goes on to the parent scope (which it leaves
   by its merge, or repeats when it is the exit's loop) when the variable is set, and the code after the scope runs
   when it isn't */
static void se_pending(DirectCtx* c, StructEmit* se, u32 scope) {
    const ShsScope* s = &se->st->scope[scope];
    u32 parent = s->parent - 1;
    u32 t = spv_emit_load(&c->emit, c->u32_type, se->flag);
    u32 go = spv_emit_id(&c->emit), pending = spv_emit_id(&c->emit);
    u32 here = spv_emit_constant_u32(&c->emit, c->u32_type, parent + 1);
    u32 set = spv_emit_i_not_equal(&c->emit, c->bool_type, t, c->zero);
    spv_emit_selection_merge(&c->emit, go);
    spv_emit_branch_conditional(&c->emit, set, pending, go);
    spv_emit_label(&c->emit, pending);
    if (!se->st->scope[parent].loop) {
        if (s->arrive && s->beyond)
            spv_emit_store(&c->emit, se->flag,
                           spv_emit_select(&c->emit, c->u32_type, spv_emit_i_equal(&c->emit, c->bool_type, t, here),
                                           c->zero, t));
        else if (s->arrive)
            spv_emit_store(&c->emit, se->flag, c->zero);
        se_break(c, se, parent);
    } else if (s->arrive && s->beyond) {
        u32 again = spv_emit_id(&c->emit), out = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
        u32 arrived = spv_emit_i_equal(&c->emit, c->bool_type, t, here);
        spv_emit_selection_merge(&c->emit, merge);
        spv_emit_branch_conditional(&c->emit, arrived, again, out);
        spv_emit_label(&c->emit, again);
        spv_emit_store(&c->emit, se->flag, c->zero);
        spv_emit_branch(&c->emit, se->label[4 * parent + 2]);
        spv_emit_label(&c->emit, out);
        se_break(c, se, parent);
        spv_emit_label(&c->emit, merge);
        spv_emit_void_op(&c->emit, SPV_OP_UNREACHABLE, NULL, 0);
    } else if (s->arrive) {
        spv_emit_store(&c->emit, se->flag, c->zero);
        spv_emit_branch(&c->emit, se->label[4 * parent + 2]);
    } else
        se_break(c, se, parent);
    spv_emit_label(&c->emit, go);
    se->open = 1;
}

/* Writes the tokens up to the next body (SE_BODY, its words in body_start and body_end), the program's end that
   control runs off (SE_END: the caller writes what EXIT writes, without the outputs, and returns), or the last
   (SE_DONE) */
static int structure_step(DirectCtx* c, StructEmit* se) {
    const ShStructure* st = se->st;
    while (se->next < st->ntokens) {
        const ShsToken* t = &st->token[se->next++];
        u32* sel = se->label + 4 * st->nscopes + 2 * (se->selections ? se->selections - 1 : 0);
        u8* sel_used = se->used + st->nscopes + (se->selections ? se->selections - 1 : 0);
        u32* l = se->label + 4 * t->a;
        switch (t->kind) {
            case SHS_BODY:
                se_ensure_open(c, se);
                se->body_start = t->a;
                se->body_end = t->b;
                return SE_BODY;
            case SHS_END: se_ensure_open(c, se); return SE_END;
            case SHS_RETURNED: se->open = 0; break;
            case SHS_OPEN_BLOCK:
            case SHS_OPEN_LOOP:
                for (u32 k = 0; k < 4; k++) l[k] = spv_emit_id(&c->emit);
                if (se->open) spv_emit_branch(&c->emit, l[0]);
                spv_emit_label(&c->emit, l[0]);
                spv_emit_loop_merge(&c->emit, l[3], l[2]);
                spv_emit_branch(&c->emit, l[1]);
                spv_emit_label(&c->emit, l[1]);
                se->open = 1;
                break;
            case SHS_CLOSE:
                if (se->open) se_break(c, se, t->a);
                /* a block scope's continue target is never reached: the loop it would repeat runs once */
                spv_emit_label(&c->emit, l[2]);
                spv_emit_branch(&c->emit, l[0]);
                spv_emit_label(&c->emit, l[3]);
                if (!se->used[t->a]) {
                    spv_emit_void_op(&c->emit, SPV_OP_UNREACHABLE, NULL, 0);
                    se->open = 0;
                } else {
                    se->open = 1;
                    if (st->scope[t->a].check) se_pending(c, se, t->a);
                }
                break;
            case SHS_IF:
                se_ensure_open(c, se);
                sel += se->selections ? 2 : 0;
                sel_used += se->selections ? 1 : 0;
                se->selections++;
                sel[0] = spv_emit_id(&c->emit);
                sel[1] = spv_emit_id(&c->emit);
                *sel_used = 0;
                {
                    u32 then = spv_emit_id(&c->emit);
                    spv_emit_selection_merge(&c->emit, sel[0]);
                    spv_emit_branch_conditional(&c->emit, c->branch_cond ? c->branch_cond : c->true_value, then,
                                                sel[1]);
                    spv_emit_label(&c->emit, then);
                }
                se->open = 1;
                break;
            case SHS_ELSE:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                spv_emit_label(&c->emit, sel[1]);
                se->open = 1;
                break;
            case SHS_SWITCH: {
                /* the cases' words, from their tokens up to the switch's end */
                u32 literals[64], labels[64], count = 0, depth = 0;
                for (u32 k = se->next; k < st->ntokens && count < 64; k++) {
                    u32 kind = st->token[k].kind;
                    if (kind == SHS_SWITCH) depth++;
                    if (kind == SHS_ENDSWITCH && !depth--) break;
                    if (kind == SHS_CASE && !depth) literals[count++] = st->token[k].b;
                }
                se_ensure_open(c, se);
                sel += se->selections ? 2 : 0;
                sel_used += se->selections ? 1 : 0;
                se->selections++;
                sel[0] = spv_emit_id(&c->emit);
                sel[1] = spv_emit_id(&c->emit); /* the default's label, the cases' from the next id on */
                for (u32 k = 0; k < count; k++) labels[k] = spv_emit_id(&c->emit);
                *sel_used = 0;
                spv_emit_selection_merge(&c->emit, sel[0]);
                spv_emit_switch_u32(&c->emit, c->branch_target, sel[1], literals, labels, count);
                se->open = 0;
                break;
            }
            case SHS_CASE:
            case SHS_DEFAULT:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                spv_emit_label(&c->emit, t->kind == SHS_DEFAULT ? sel[1] : sel[1] + 1 + t->a);
                se->open = 1;
                break;
            case SHS_ENDSWITCH:
            case SHS_ENDIF:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                spv_emit_label(&c->emit, sel[0]);
                se->open = *sel_used;
                if (!se->open) spv_emit_void_op(&c->emit, SPV_OP_UNREACHABLE, NULL, 0);
                se->selections--;
                break;
            case SHS_BREAK:
                se_ensure_open(c, se);
                if (t->b) spv_emit_store(&c->emit, se->flag, spv_emit_constant_u32(&c->emit, c->u32_type, t->b));
                se_break(c, se, t->a);
                break;
            case SHS_CONTINUE:
                se_ensure_open(c, se);
                se_branch(c, se, l[2]);
                break;
            default: break;
        }
    }
    return SE_DONE;
}

static u32* to_spirv(const u8* code, const u64* insn, u32 max_bytes, int stage, ShaderInfo* si, size_t* word_count,
                     int can_flush, int can_preserve, int can_preserve_f64, int can_subgroup) {
    u32 clip;
    if (!code || !si || !word_count || max_bytes < 96 || stage < ST_VS || stage > ST_CS) return NULL;
    if (stage == ST_CS) {
        ShaderInfo original = *si;
        u32* simple = shader_compute_to_spirv(code, max_bytes, si, word_count, can_flush, can_preserve);
        if (simple) return simple;
        *si = original;
        if (!si->cs_local[0] || !si->cs_local[1] || !si->cs_local[2]) return direct_reject(si, "compute local size");
    }
    u32 len;
    shader_hash(code, max_bytes, &len);
    u32 n = len / 8;
    u32 sph[20];
    for (u32 i = 0; i < 20; i++) sph[i] = snx_ld_le32(code + i * 4);
    u32 target = stage == ST_CS ? 0 : sph[18];
    if (stage == ST_FS && (sph[19] & ~3u)) return direct_reject(si, "fragment output beyond depth and mask");
    /* the sample mask (SPH word 19 bit 0) and depth (bit 1) follow the colours, in that order */
    int passthrough = stage == ST_GS && (sph[0] >> 24 & 1);
    u8 used[255] = {0};
    u8 pred_used[7] = {0};
    u8 inputs_used[32] = {0};
    u8 outputs_used[32] = {0};
    int indexed_store = 0;
    int position_used = 0, layer_output_used = 0, layer_input_used = 0, point_size_used = 0;
    int viewport_used = 0;
    /* the stages between the vertex and fragment ones: their input arrays' length (32 for tessellation, the input
       primitive's vertices for geometry), the control program's output vertices, and what they read and write */
    int between = stage == ST_TCS || stage == ST_TES || stage == ST_GS;
    u32 in_vertices = stage == ST_GS ? si->rt_int_mask : 32, out_vertices = stage == ST_TCS ? sph[2] >> 24 : 0;
    u8 patch_used[30] = {0};
    int levels_used = 0, position_in_used = 0, primitive_id_used = 0, tess_coord_used = 0, invocation_used = 0;
    int last = stage != ST_TCS && !(si->clip_enable & SNX_SH_NOT_LAST); /* before rasterization */
    u32 clip_used = 0;
    u8 cbuf_used[18] = {0};
    u32 cbuf_max[18] = {0};
    ShTex textures[SH_MAXTEX] = {0};
    int texture_count = 0;
    ShaderInfo image_info = {0};
    int needs_fragcoord = 0;
    int needs_frontface = 0;
    int needs_point_coord = 0, needs_sample_id = 0, needs_coverage = 0;
    int needs_demote = 0;
    int needs_image_query = 0, needs_derivatives = 0, needs_clock = 0;
    int needs_instance = 0;
    int needs_local_id = 0, needs_group_id = 0;
    int needs_scale = 0, needs_texel_scale = 0;
    int needs_glsl = 0, needs_f16 = 0, needs_f64 = 0, needs_i64 = 0;
    int has_flow = 0, has_calls = 0, has_barrier = 0, converge;
    u32 wait_label = 0;
    int first_indexed[SH_INDEXED_TABLES] = {0}, indexed_count = 0, indexed_table[SH_MAXTEX] = {0}, k_indexed;
    int has_stack = 0, has_pexit = 0, has_conditional_exit = 0;
    int needs_local = 0, needs_shared = 0, needs_subgroup = 0, needs_atomic_cas = 0, lane_used = 0;
    int needs_gather_extended = 0;
    int fp32_flush = 0, fp32_preserve = 0;
    u32 out_reg = 0;
    if (stage == ST_FS)
        for (int rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15) {
                if (out_reg > 251) return direct_reject(si, "fragment output registers");
                for (int k = 0; k < 4; k++)
                    if (target & (1u << (rt * 4 + k))) used[out_reg + k] = 1;
                out_reg += 4;
            }
    if (stage == ST_FS && (sph[19] & 3u)) {
        if (sph[19] & 1u) used[out_reg] = 1;
        if (sph[19] & 2u) used[out_reg + 1] = 1;
    }
    int has_exit = 0;
    for (u32 i = 11; i < n; i++) {
        if (!IDX_OK(i)) continue;
        const Op* op = decode(insn[i]);
        if (!op || !supported(insn[i], op->id) || !stage_has(insn[i], op->id, stage)) {
            if (op && op->id == O_TEXS)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "TEXS encoding %u, fp32 %u",
                         B(insn[i], 53, 4), B(insn[i], 59, 1));
            else if (op && op->id == O_TLDS)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "TLDS encoding %u mask %u fp32 %u",
                         B(insn[i], 53, 4), B(insn[i], 50, 3), B(insn[i], 59, 1));
            else if (op && op->id == O_TXD)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "TXD dim %u offset %u", B(insn[i], 28, 3),
                         B(insn[i], 35, 1));
            else if (op && (op->id == O_HADD2_R || op->id == O_HADD2_C || op->id == O_HADD2_I || op->id == O_HADD2_32I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "HADD2 kind %d merge %u swa %u swb %u sat %u", op->id - O_HADD2_R, B(insn[i], 49, 2),
                         B(insn[i], 47, 2), B(insn[i], 28, 2), B(insn[i], 32, 1));
            else if (op && op->id >= O_XMAD_R && op->id <= O_XMAD_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "XMAD %d m%u c%u x%u s%u%u h%u p%u g%u",
                         op->id - O_XMAD_R, B(insn[i], 50, op->id == O_XMAD_CR || op->id == O_XMAD_RC ? 2 : 3),
                         B(insn[i], 47, 1),
                         op->id == O_XMAD_CR || op->id == O_XMAD_RC ? B(insn[i], 54, 1) : B(insn[i], 38, 1),
                         B(insn[i], 48, 1), B(insn[i], 49, 1), B(insn[i], 53, 1),
                         op->id == O_XMAD_RC ? 0 : B(insn[i], op->id == O_XMAD_CR ? 55 : 36, 1),
                         op->id == O_XMAD_RC ? 0 : B(insn[i], op->id == O_XMAD_CR ? 56 : 37, 1));
            else if (op && op->id == O_LDC)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "LDC size %u index %u addr %u",
                         B(insn[i], 48, 3), B(insn[i], 36, 5), B(insn[i], 8, 8));
            else if (op && op->id >= O_ISETP_R && op->id <= O_ISETP_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "ISETP kind %d cmp %u uns %u x %u cc %u",
                         op->id - O_ISETP_R, B(insn[i], 49, 3), B(insn[i], 48, 1), B(insn[i], 43, 1),
                         B(insn[i], 47, 1));
            else if (op && op->id >= O_F2F_R && op->id <= O_F2F_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "F2F kind %d dst %u src %u rnd %u int %u sat %u cc %u", op->id - O_F2F_R, B(insn[i], 8, 2),
                         B(insn[i], 10, 2), B(insn[i], 39, 2), B(insn[i], 42, 1), B(insn[i], 50, 1), B(insn[i], 47, 1));
            else if (op && op->id >= O_F2I_R && op->id <= O_F2I_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "F2I kind %d dst %u src %u rnd %u signed %u cc %u", op->id - O_F2I_R, B(insn[i], 8, 2),
                         B(insn[i], 10, 2), B(insn[i], 39, 2), B(insn[i], 12, 1), B(insn[i], 47, 1));
            else if (op && op->id >= O_I2F_R && op->id <= O_I2F_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "I2F kind %d dst %u src %u signed %u sel %u cc %u", op->id - O_I2F_R, B(insn[i], 8, 2),
                         B(insn[i], 10, 2), B(insn[i], 13, 1), B(insn[i], 41, 2), B(insn[i], 47, 1));
            else if (op && op->id == O_ALD)
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "ALD addr %u count %u index %u bit32 %u dst %u", B(insn[i], 20, 10), B(insn[i], 47, 2) + 1,
                         B(insn[i], 8, 8), B(insn[i], 32, 1), B(insn[i], 0, 8));
            else if (op && op->id == O_IPA)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "IPA addr %u bit38 %u bit51 %u mode %u",
                         B(insn[i], 30, 8) * 4, B(insn[i], 38, 1), B(insn[i], 51, 1), B(insn[i], 54, 2));
            else if (op && op->id == O_IADD32I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "IADD32I po %u x %u cc %u sat %u",
                         B(insn[i], 55, 2), B(insn[i], 53, 1), B(insn[i], 52, 1), B(insn[i], 54, 1));
            else if (op &&
                     (op->id == O_ISCADD_R || op->id == O_ISCADD_C || op->id == O_ISCADD_I || op->id == O_ISCADD32I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "ISCADD kind %d avg %u shift %u cc %u",
                         op->id - O_ISCADD_R, op->id == O_ISCADD32I ? 0 : B(insn[i], 48, 2),
                         B(insn[i], op->id == O_ISCADD32I ? 53 : 39, 5),
                         B(insn[i], op->id == O_ISCADD32I ? 52 : 47, 1));
            else if (op && (op->id == O_IADD_R || op->id == O_IADD_C || op->id == O_IADD_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "IADD kind %d avg %u x %u cc %u sat %u",
                         op->id - O_IADD_R, B(insn[i], 48, 2), B(insn[i], 43, 1), B(insn[i], 47, 1), B(insn[i], 50, 1));
            else if (op && (op->id == O_I2I_R || op->id == O_I2I_C || op->id == O_I2I_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "I2I kind %d dst %u src %u signed %u%u sel %u neg %u abs %u", op->id - O_I2I_R,
                         B(insn[i], 8, 2), B(insn[i], 10, 2), B(insn[i], 12, 1), B(insn[i], 13, 1), B(insn[i], 41, 2),
                         B(insn[i], 45, 1), B(insn[i], 49, 1));
            else if (op && (op->id == O_SHR_R || op->id == O_SHR_C || op->id == O_SHR_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "SHR kind %d sign %u wrap %u reverse %u xmode %u cc %u", op->id - O_SHR_R, B(insn[i], 48, 1),
                         B(insn[i], 39, 1), B(insn[i], 40, 1), B(insn[i], 43, 2), B(insn[i], 47, 1));
            else if (op && (op->id == O_ISET_R || op->id == O_ISET_C || op->id == O_ISET_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "ISET kind %d cmp %u uns %u x %u cc %u bool %u", op->id - O_ISET_R, B(insn[i], 49, 3),
                         B(insn[i], 48, 1), B(insn[i], 43, 1), B(insn[i], 47, 1), B(insn[i], 45, 2));
            else if (op && (op->id == O_TXQ || op->id == O_TXQ_B))
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s q%u mask%u off%u", op->name,
                         B(insn[i], 22, 6), B(insn[i], 31, 4), B(insn[i], 36, 13));
            else if (op && (op->id == O_TLD || op->id == O_TLD_B))
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s ty%u lod%u lc%u toff%u ms%u mask%u",
                         op->name, B(insn[i], 28, 3), B(insn[i], 55, 1), B(insn[i], 54, 1), B(insn[i], 35, 1),
                         B(insn[i], 50, 1), B(insn[i], 31, 4));
            else if (op && (op->id == O_TLD4 || op->id == O_TLD4_B || op->id == O_TLD4S))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "%s dc%u ao%u comp%u fp16%u da%u db%u mask%u off%u toff%u lc%u", op->name, B(insn[i], 50, 1),
                         B(insn[i],
                           op->id == O_TLD4S    ? 51
                           : op->id == O_TLD4_B ? 36
                                                : 54,
                           op->id == O_TLD4S ? 1 : 2),
                         B(insn[i],
                           op->id == O_TLD4S    ? 52
                           : op->id == O_TLD4_B ? 38
                                                : 56,
                           2),
                         op->id == O_TLD4S ? B(insn[i], 55, 1) : 0, B(insn[i], 0, 8), B(insn[i], 28, 8),
                         B(insn[i], 31, 4), B(insn[i], 36, 13),
                         B(insn[i],
                           op->id == O_TLD4_B  ? 36
                           : op->id == O_TLD4S ? 51
                                               : 54,
                           op->id == O_TLD4S ? 1 : 2),
                         op->id == O_TLD4_B ? B(insn[i], 40, 1) : 0);
            else if (op && op->id == O_AST)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "AST addr %u count %u idxreg %u reg %u",
                         B(insn[i], 20, 10), B(insn[i], 47, 2) + 1, B(insn[i], 8, 8), B(insn[i], 0, 8));
            else if (op && (op->id == O_TEX || op->id == O_TEX_B))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "%s ty%u lod%u dc%u mask%u off%u aoffi%u lc%u", op->name, B(insn[i], 28, 3),
                         B(insn[i], op->id == O_TEX_B ? 37 : 55, 3), B(insn[i], 50, 1), B(insn[i], 31, 4),
                         B(insn[i], 36, 13), B(insn[i], op->id == O_TEX_B ? 36 : 54, 1),
                         B(insn[i], op->id == O_TEX_B ? 40 : 58, 1));
            else /* the word too, to decode which form of it is missing */
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s %016llx",
                         op ? op->name : "unknown opcode", (unsigned long long)insn[i]);
            si->unsupported = 1;
            return NULL;
        }
        if (op->id == O_BRA || op->id == O_CAL || op->id == O_PRET) {
            u32 target_index = bra_target(insn[i], i);
            if (target_index >= n || !IDX_OK(target_index)) return direct_reject(si, "branch target");
            has_flow = 1;
            if (op->id != O_BRA) has_calls = 1;
        }
        if (op->id == O_RET) has_flow = has_calls = 1; /* a RET with no CAL pops an empty stack: it ends the program */
        if ((op->id == O_RED || op->id == O_ATOM) && global_atomic_opcode(insn[i], op->id == O_RED) == 0xffffffffu)
            needs_atomic_cas = 1;
        if (op->id == O_ATOMS && shared_atomic_opcode(insn[i]) == 0xffffffffu) needs_atomic_cas = 1;
        if (op->id == O_BRX) has_flow = 1;
        /* Thread and block ids require compute's invocation and workgroup builtins. */
        if (op->id == O_S2R && stage == ST_CS) {
            u32 sr = B(insn[i], 20, 8);
            needs_local_id |= sr >= 0x20 && sr <= 0x23;
            needs_group_id |= sr >= 0x25 && sr <= 0x27;
        }
        if (op->id == O_S2R && stage == ST_FS && B(insn[i], 20, 8) == 0x13) needs_demote = 1; /* the helper query */
        if (op->id == O_S2R || op->id == O_CS2R) {
            u32 sr = B(insn[i], 20, 8);
            int scope_bit = sr >= 0x50 && sr <= 0x53 ? (sr < 0x52 ? 1 : 2) : 0;
            if (scope_bit && !(g_shader_clock_scopes & scope_bit)) return direct_reject(si, "shader clock unavailable");
            if (scope_bit) needs_clock = 1;
        }
        if (op->id == O_PIXLD) {
            if (B(insn[i], 31, 3) == 5)
                needs_sample_id = 1;
            else
                needs_coverage = 1;
        }
        if (op->id == O_SSY || op->id == O_PBK || op->id == O_PCNT) {
            u32 target_index = bra_target(insn[i], i);
            if (target_index >= n || !IDX_OK(target_index)) return direct_reject(si, "control stack target");
            has_flow = has_stack = 1;
        }
        if (op->id == O_PEXIT) {
            u32 target_index = bra_target(insn[i], i);
            if (target_index >= n || !IDX_OK(target_index)) return direct_reject(si, "PEXIT target");
            has_flow = has_stack = has_pexit = 1;
        }
        if (op->id == O_EXIT && !flow_always(insn[i])) has_conditional_exit = 1;
        if (op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT) has_flow = has_stack = 1;
        if (((op->id >= O_FADD_R && op->id <= O_FFMA_I) || op->id == O_FFMA32I) && op->id != O_FMNMX_R &&
            op->id != O_FMNMX_C && op->id != O_FMNMX_I) {
            u32 mode = op->id == O_FADD32I                        ? B(insn[i], 55, 1)
                       : op->id >= O_FADD_R && op->id <= O_FADD_I ? B(insn[i], 44, 1)
                       : op->id >= O_FMUL_R && op->id <= O_FMUL_I ? B(insn[i], 44, 2)
                                                                  : B(insn[i], 53, 2);
            if (mode & 1)
                fp32_flush = 1;
            else
                fp32_preserve = 1;
        }
        if (op->id == O_FSWZADD) {
            if (B(insn[i], 44, 1))
                fp32_flush = 1;
            else
                fp32_preserve = 1;
        }
        u32 d = B(insn[i], 0, 8), a = B(insn[i], 8, 8), b = B(insn[i], 20, 8);
        u32 guard = B(insn[i], 16, 3);
        if (guard < 7) pred_used[guard] = 1;
        if (d < 255 && op->id != O_EXIT && op->id != O_KIL && op->id != O_BRA && op->id != O_BRX && op->id != O_SSY &&
            op->id != O_PBK && op->id != O_PCNT && op->id != O_SYNC && op->id != O_BRK && op->id != O_CONT &&
            op->id != O_AST && op->id != O_NOP && op->id != O_VOTE_VTG && op->id != O_DEPBAR && op->id != O_CAL &&
            op->id != O_PRET && op->id != O_RET && op->id != O_FSETP_R && op->id != O_FSETP_C && op->id != O_FSETP_I &&
            op->id != O_HSETP2_R && op->id != O_HSETP2_C && op->id != O_HSETP2_I && op->id != O_DSETP_R &&
            op->id != O_DSETP_C && op->id != O_DSETP_I && op->id != O_ISETP_R && op->id != O_ISETP_C &&
            op->id != O_ISETP_I && op->id != O_VSETP && op->id != O_PSETP && op->id != O_CSETP && op->id != O_R2P_R &&
            op->id != O_R2P_C && op->id != O_R2P_I && op->id != O_FCHK_R && op->id != O_FCHK_C && op->id != O_FCHK_I &&
            op->id != O_MEMBAR && op->id != O_CCTL && op->id != O_CCTLL && op->id != O_IDE && op->id != O_BPT &&
            op->id != O_PEXIT)
            used[d] = 1;
        if (op->id == O_AL2P && !al2p_predicate_unseen(insn, n, i))
            return direct_reject(si, "AL2P range predicate read");
        if (op->id == O_ALD && a != 255) {
            /* an indexed read can reach any generic input the header's map lists (bytes 0x18 to 0x27, a bit for
               each of an input's four components) */
            u32 width = B(insn[i], 47, 2) + 1, vertex = B(insn[i], 39, 8);
            used[a] = 1;
            if (between && vertex < 255) used[vertex] = 1;
            for (u32 k = 0; k < width; k++)
                if (d + k < 255) used[d + k] = 1;
            for (u32 v = 0; v < 32; v++)
                if ((sph[6 + v / 8] >> (v % 8 * 4)) & 15) inputs_used[v] = 1;
        } else if (op->id == O_ALD || op->id == O_AST) {
            u32 addr = B(insn[i], 20, 10), width = B(insn[i], 47, 2) + 1, vertex = B(insn[i], 39, 8);
            int input = op->id == O_ALD && !(stage == ST_TCS && B(insn[i], 32, 1));
            if (op->id == O_AST && a != 255) indexed_store = 1;
            if (between && vertex < 255) used[vertex] = 1;
            if (stage == ST_TCS && op->id == O_AST) invocation_used = 1; /* the invocation's output vertex */
            for (u32 k = 0; k < width; k++) {
                if (d + k < 255) used[d + k] = 1;
                u32 location = addr + k * 4;
                if (B(insn[i], 31, 1)) { /* the levels' 6 words and 2 of padding, then the patch's vectors */
                    if (location >= 0x20)
                        patch_used[(location - 0x20) >> 4] = 1;
                    else if (location < 0x18)
                        levels_used |= location < 0x10 ? 1 : 2;
                } else if (input && location >= 0x2f8 && location <= 0x2fc)
                    needs_instance = 1;
                else if (input && (location == 0x2f0 || location == 0x2f4))
                    tess_coord_used = 1;
                else if (input && location == 0x60)
                    primitive_id_used = 1;
                else if (input && location < 0x80)
                    position_in_used = 1;
                else if (input)
                    inputs_used[(location - 0x80) >> 4] = 1;
                else if (location == 0x64)
                    layer_output_used = 1;
                else if (location == 0x68)
                    viewport_used = 1;
                else if (location == 0x6c)
                    point_size_used = 1;
                else if (location >= 0x2c0 && location < 0x2e0) {
                    u32 bit = 1u << ((location - 0x2c0) / 4);
                    if (si->clip_enable & bit) clip_used |= bit;
                } else if (location >= 0x80)
                    outputs_used[(location - 0x80) >> 4] = 1;
                else
                    position_used = 1;
            }
        }
        if (op->id == O_ISBERD) used[a] = 1;
        if (op->id == O_S2R && B(insn[i], 20, 8) == 0x11) invocation_used = 1;
        if (op->id == O_IMUL_R || op->id == O_IMAD_R || op->id == O_IMAD_RC) {
            if (b < 255 && op->id != O_IMAD_RC) used[b] = 1;
            if (op->id == O_IMAD_RC && B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1;
        }
        if (op->id == O_IMAD_R || op->id == O_IMAD_CR || op->id == O_IMAD_I)
            if (B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1;
        if (op->id == O_IMAD32I && d < 255) used[d] = 1;
        if ((op->id >= O_IMUL_R && op->id <= O_IMAD32I) && a < 255) used[a] = 1;
        if (op->id == O_MOV_R || op->id == O_LOP3_R || op->id == O_IMNMX_R || op->id == O_XMAD_R || op->id == O_BFE_R ||
            op->id == O_ICMP_R || op->id == O_IADD3_R || op->id == O_FADD_R || op->id == O_FMUL_R ||
            op->id == O_FFMA_R || op->id == O_FMNMX_R || op->id == O_FSET_R || op->id == O_RRO_R || op->id == O_FLO_R ||
            op->id == O_POPC_R) {
            if (b < 255) used[b] = 1;
        }
        if ((op->id == O_FADD_R || op->id == O_FADD_C || op->id == O_FADD_I || op->id == O_FADD32I ||
             op->id == O_FFMA32I || op->id == O_FMUL_R || op->id == O_FMUL_C || op->id == O_FMUL_I ||
             op->id == O_FMUL32I || op->id == O_FFMA_R || op->id == O_FFMA_RC || op->id == O_FFMA_CR ||
             op->id == O_FFMA_I || op->id == O_FMNMX_R || op->id == O_FMNMX_C || op->id == O_FMNMX_I ||
             op->id == O_FSET_R || op->id == O_FSET_C || op->id == O_FSET_I || op->id == O_MUFU) &&
            a < 255)
            used[a] = 1;
        if ((op->id == O_AL2P || op->id == O_BRX || (op->id >= O_P2R_R && op->id <= O_R2P_I) || op->id == O_LOP3_R ||
             op->id == O_LOP3_C || op->id == O_LOP3_I || op->id == O_IMNMX_R || op->id == O_IMNMX_C ||
             op->id == O_IMNMX_I || op->id == O_XMAD_CR || op->id == O_XMAD_RC || op->id == O_XMAD_R ||
             op->id == O_IADD3_R || op->id == O_IADD3_C || op->id == O_IADD3_I || op->id == O_ICMP_R ||
             op->id == O_ICMP_RC || op->id == O_ICMP_CR || op->id == O_ICMP_I || op->id == O_FCMP_R ||
             op->id == O_FCMP_RC || op->id == O_FCMP_CR || op->id == O_FCMP_I || op->id == O_BFI_R ||
             op->id == O_BFI_RC || op->id == O_BFI_CR || op->id == O_BFI_I || op->id == O_IADD32I ||
             op->id == O_LOP32I || op->id == O_XMAD_I || op->id == O_IADD_R || op->id == O_IADD_C ||
             op->id == O_IADD_I || op->id == O_LDG || op->id == O_STG || op->id == O_RED || op->id == O_ATOM ||
             op->id == O_ATOM_CAS || op->id == O_LOP_R || op->id == O_LOP_C || op->id == O_LOP_I || op->id == O_SEL_R ||
             op->id == O_SEL_C || op->id == O_SEL_I || op->id == O_ISCADD_R || op->id == O_ISCADD_C ||
             op->id == O_ISCADD_I || op->id == O_ISCADD32I || (op->id >= O_BFE_R && op->id <= O_BFE_I) ||
             op->id == O_SHL_R || op->id == O_SHL_C || op->id == O_SHL_I || op->id == O_SHR_R || op->id == O_SHR_C ||
             op->id == O_SHR_I || op->id == O_ISET_R || op->id == O_ISET_C || op->id == O_ISET_I) &&
            a < 255)
            used[a] = 1;
        if ((op->id == O_ISCADD_R || op->id == O_IADD_R || op->id == O_LOP_R || op->id == O_SEL_R ||
             op->id == O_P2R_R || op->id == O_R2P_R) &&
            b < 255)
            used[b] = 1;
        if (op->id == O_SEL_R || op->id == O_SEL_C || op->id == O_SEL_I) {
            u32 predicate = B(insn[i], 39, 3);
            if (predicate < 7) pred_used[predicate] = 1;
        }
        if ((op->id == O_XMAD_I || op->id == O_XMAD_R || op->id == O_XMAD_RC || op->id == O_XMAD_CR) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_F2F_R || op->id == O_F2I_R || op->id == O_I2F_R) && b < 255) used[b] = 1;
        /* the conversions' 64-bit sides (format 3) are register pairs: an F64 (F2F, F2I's source, I2F's result) or
           a 64-bit integer (F2I's result, I2F's source) */
        if ((op->id >= O_F2F_R && op->id <= O_F2F_I) || (op->id >= O_F2I_R && op->id <= O_F2I_I) ||
            (op->id >= O_I2F_R && op->id <= O_I2F_I)) {
            int f2f = op->id <= O_F2F_I && op->id >= O_F2F_R, f2i = op->id >= O_F2I_R && op->id <= O_F2I_I;
            u32 dst_format = B(insn[i], 8, 2), src_format = B(insn[i], 10, 2);
            if ((src_format == 3 && (f2f || f2i)) || (dst_format == 3 && !f2i)) needs_f64 = 1;
            if ((dst_format == 3 && f2i) || (src_format == 3 && !f2f && !f2i)) needs_i64 = 1;
            if (dst_format == 3 && d < 254) used[d + 1] = 1;
            if (src_format == 3 && (op->id == O_F2F_R || op->id == O_F2I_R || op->id == O_I2F_R) && b < 254)
                used[b + 1] = 1;
        }
        if (op->id == O_I2I_R && b < 255) used[b] = 1;
        if ((op->id == O_ICMP_R || op->id == O_ICMP_RC || op->id == O_ICMP_CR || op->id == O_ICMP_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_FCMP_R || op->id == O_FCMP_RC || op->id == O_FCMP_CR || op->id == O_FCMP_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_FCMP_R || op->id == O_FCMP_RC) && b < 255) used[b] = 1;
        if ((op->id == O_BFI_R || op->id == O_BFI_RC || op->id == O_BFI_CR || op->id == O_BFI_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_BFI_R || op->id == O_BFI_RC) && b < 255) used[b] = 1;
        if ((op->id == O_IADD3_R || op->id == O_IADD3_C || op->id == O_IADD3_I) && B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_LOP3_R || op->id == O_LOP3_C || op->id == O_LOP3_I) && B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if (op->id == O_LDC) {
            if (a < 255) used[a] = 1;
            if (B(insn[i], 48, 3) == 5 && d + 1 < 255) used[d + 1] = 1;
            u32 bank = B(insn[i], 36, 5);
            cbuf_used[bank] = 1;
            cbuf_max[bank] = 65536;
        }
        if (op->id == O_LDG || op->id == O_STG) {
            u32 count = B(insn[i], 48, 3) == 5 ? 2 : B(insn[i], 48, 3) >= 6 ? 4 : 1;
            if (d != 255 && d + count > 256) return direct_reject(si, "global memory register range");
            for (u32 k = 0; k < count; k++)
                if (d + k < 255) used[d + k] = 1;
            /* a 64-bit address's high word, which an access whose buffer is found when it runs compares */
            if (B(insn[i], 45, 1) && a + 1 < 255) used[a + 1] = 1;
        }
        if (op->id == O_RED && d < 255) used[d] = 1;
        if (op->id == O_ATOM && b < 255) used[b] = 1;
        if (op->id == O_VADD || op->id == O_VABSDIFF || op->id == O_VSET || op->id == O_VSHL || op->id == O_VSHR) {
            /* Signed intermediate, saturation and secondary operations use 64 bits even for narrow source lanes.
               vk_translate_spirv reports shaderInt64 unavailable if the device cannot run these shaders. */
            needs_i64 = 1;
            if (a < 255) used[a] = 1;
            if (B(insn[i], 50, 1) && b < 255) used[b] = 1;
            if (B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1;
        }
        if (op->id >= O_PRMT_R && op->id <= O_PRMT_I) {
            if (a < 255) used[a] = 1;
            if (op->id == O_PRMT_R && b < 255) used[b] = 1;
            if (B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1; /* C, or the RC form's selector */
        }
        if (op->id == O_ATOM_CAS || op->id == O_ATOMS_CAS) {
            if (op->id == O_ATOMS_CAS) needs_shared = 1;
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
            if (b < 254) used[b + 1] = 1;
        }
        if (op->id == O_LDL || op->id == O_STL) {
            needs_local = 1;
            if (a < 255) used[a] = 1;
            u32 words = B(insn[i], 48, 3) == 5 ? 2 : B(insn[i], 48, 3) >= 6 ? 4 : 1;
            if (d != 255 && d + words > 255) return direct_reject(si, "local memory register range");
            for (u32 k = 0; k < words; k++)
                if (d + k < 255) used[d + k] = 1;
        }
        if (op->id == O_LDS || op->id == O_STS) {
            needs_shared = 1;
            if (a < 255) used[a] = 1;
            u32 words = B(insn[i], 48, 3) == 5 ? 2 : B(insn[i], 48, 3) >= 6 ? 4 : 1;
            if (d != 255 && d + words > 255) return direct_reject(si, "shared memory register range");
            for (u32 k = 0; k < words; k++)
                if (d + k < 255) used[d + k] = 1;
        }
        if (op->id == O_ATOMS) {
            needs_shared = 1;
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
            if (shared_atomic_opcode(insn[i]) == 0xfffffffeu) {
                if (stage != ST_CS || (u64)si->cs_local[0] * si->cs_local[1] * si->cs_local[2] != 1)
                    return direct_reject(si, "64-bit shared exchange requires one invocation");
                if (b + 1 < 255) used[b + 1] = 1;
                if (d + 1 < 255) used[d + 1] = 1;
            }
        }
        if (op->id == O_BAR) has_barrier = 1;
        if (op->id == O_ISETP_R || op->id == O_ISETP_C || op->id == O_ISETP_I) {
            if (a < 255) used[a] = 1;
            if (op->id == O_ISETP_R && b < 255) used[b] = 1;
            u32 pa = B(insn[i], 3, 3), pb = B(insn[i], 0, 3), pc = B(insn[i], 39, 3);
            if (pa < 7) pred_used[pa] = 1;
            if (pb < 7) pred_used[pb] = 1;
            if (pc < 7) pred_used[pc] = 1;
        }
        if (op->id == O_PSETP) {
            u32 pp[] = {B(insn[i], 12, 3), B(insn[i], 29, 3), B(insn[i], 39, 3), B(insn[i], 3, 3), B(insn[i], 0, 3)};
            for (u32 k = 0; k < 5; k++)
                if (pp[k] < 7) pred_used[pp[k]] = 1;
        }
        if (op->id == O_PSET) {
            u32 pred_ids[3] = {B(insn[i], 12, 3), B(insn[i], 29, 3), B(insn[i], 39, 3)};
            for (u32 k = 0; k < 3; k++)
                if (pred_ids[k] < 7) pred_used[pred_ids[k]] = 1;
        }
        if (op->id == O_CSETP || op->id == O_CSET) {
            u32 pp[] = {B(insn[i], 39, 3), B(insn[i], 3, 3), B(insn[i], 0, 3)};
            for (u32 k = 0; k < 3; k++)
                if (pp[k] < 7) pred_used[pp[k]] = 1;
        }
        if (op->id == O_VOTE) {
            if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
            if (B(insn[i], 45, 3) < 7) pred_used[B(insn[i], 45, 3)] = 1;
        }
        if (op->id == O_SHFL) {
            if (a < 255) used[a] = 1;
            if (!B(insn[i], 28, 1) && b < 255) used[b] = 1;
            if (!B(insn[i], 29, 1) && B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1;
            if (B(insn[i], 48, 3) < 7) pred_used[B(insn[i], 48, 3)] = 1;
        }
        if (op->id == O_IMNMX_R || op->id == O_IMNMX_C || op->id == O_IMNMX_I)
            if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
        if (op->id >= O_P2R_R && op->id <= O_R2P_I && !B(insn[i], 40, 1))
            for (u32 p = 0; p < 7; p++) pred_used[p] = 1;
        if ((op->id == O_LOP_R || op->id == O_LOP_C || op->id == O_LOP_I) && B(insn[i], 48, 3) < 7)
            pred_used[B(insn[i], 48, 3)] = 1;
        if (op->id == O_LOP3_R && B(insn[i], 48, 3) < 7) pred_used[B(insn[i], 48, 3)] = 1;
        if (sparse_predicate(insn[i], op->id) < 7) pred_used[sparse_predicate(insn[i], op->id)] = 1;
        if (op->id == O_SHL_R && b < 255) used[b] = 1;
        if (op->id == O_SHF_LR || op->id == O_SHF_LI || op->id == O_SHF_RR || op->id == O_SHF_RI) {
            if (a < 255) used[a] = 1;
            if ((op->id == O_SHF_LR || op->id == O_SHF_RR) && b < 255) used[b] = 1;
            u32 hi = B(insn[i], 39, 8);
            if (hi < 255) used[hi] = 1;
        }
        if (op->id == O_LEA_LR || op->id == O_LEA_LC || op->id == O_LEA_LI || op->id == O_LEA_HR ||
            op->id == O_LEA_HC) {
            if (a < 255) used[a] = 1;
            if ((op->id == O_LEA_LR || op->id == O_LEA_HR) && b < 255) used[b] = 1;
            if (op->id == O_LEA_HR || op->id == O_LEA_HC) {
                u32 hi = B(insn[i], 39, 8);
                if (hi < 255) used[hi] = 1;
            } else if (B(insn[i], 48, 3) < 7)
                pred_used[B(insn[i], 48, 3)] = 1;
        }
        if ((op->id == O_SHR_R || op->id == O_ISET_R) && b < 255) used[b] = 1;
        if (op->id == O_ISET_R || op->id == O_ISET_C || op->id == O_ISET_I) {
            u32 pc = B(insn[i], 39, 3);
            if (pc < 7) pred_used[pc] = 1;
        }
        if ((op->id == O_FFMA_R || op->id == O_FFMA_RC || op->id == O_FFMA_CR || op->id == O_FFMA_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if (op->id == O_MUFU && (B(insn[i], 20, 4) == 6 || B(insn[i], 20, 4) == 7)) needs_f64 = 1;
        if (op->id >= O_FCHK_R && op->id <= O_FCHK_I && B(insn[i], 3, 3) < 7) pred_used[B(insn[i], 3, 3)] = 1;
        if (op->id == O_CS2R && d < 254) used[d] = used[d + 1] = 1;
        if (op->id == O_MUFU || op->id == O_RRO_R || op->id == O_RRO_C || op->id == O_RRO_I || op->id == O_FFMA32I ||
            op->id == O_FFMA_R || op->id == O_FFMA_RC || op->id == O_FFMA_CR || op->id == O_FFMA_I ||
            op->id == O_FMNMX_R || op->id == O_FMNMX_C || op->id == O_FMNMX_I ||
            ((op->id == O_FADD_R || op->id == O_FADD_C || op->id == O_FADD_I) &&
             (B(insn[i], 46, 1) || B(insn[i], 49, 1) || B(insn[i], 50, 1))) ||
            (op->id == O_FADD32I && (B(insn[i], 54, 1) || B(insn[i], 57, 1))) ||
            ((op->id == O_FMUL_R || op->id == O_FMUL_C || op->id == O_FMUL_I) && B(insn[i], 50, 1)) ||
            (op->id == O_FMUL32I && B(insn[i], 55, 1)) || (op->id >= O_F2F_R && op->id <= O_F2F_I) ||
            (op->id >= O_F2I_R && op->id <= O_F2I_I) || (op->id >= O_I2F_R && op->id <= O_I2F_I))
            needs_glsl = 1;
        if (op->id == O_IMUL_C || op->id == O_IMAD_RC || op->id == O_IMAD_CR) {
            u32 idx = B(insn[i], 34, 5), off = B(insn[i], 20, 14) * 4;
            if (idx >= 18) return direct_reject(si, "constant buffer index");
            cbuf_used[idx] = 1;
            if ((off | 15u) + 1 > cbuf_max[idx]) cbuf_max[idx] = (off | 15u) + 1;
        }
        if (op->id == O_LOP3_C || op->id == O_IMNMX_C || op->id == O_XMAD_CR || op->id == O_XMAD_RC ||
            op->id == O_BFE_C || op->id == O_IADD3_C || op->id == O_LEA_LC || op->id == O_LEA_HC || op->id == O_F2I_C ||
            op->id == O_I2F_C || op->id == O_I2I_C || op->id == O_F2F_C || op->id == O_FLO_C || op->id == O_POPC_C ||
            op->id == O_P2R_C || op->id == O_R2P_C || op->id == O_ICMP_RC || op->id == O_ICMP_CR ||
            op->id == O_FCMP_RC || op->id == O_FCMP_CR || op->id == O_BFI_RC || op->id == O_BFI_CR ||
            op->id == O_MOV_C || op->id == O_IADD_C || op->id == O_ISCADD_C || op->id == O_LOP_C || op->id == O_SEL_C ||
            op->id == O_HADD2_C || op->id == O_HMUL2_C || op->id == O_HFMA2_RC || op->id == O_HFMA2_CR ||
            op->id == O_ISETP_C || op->id == O_ISET_C || op->id == O_SHL_C || op->id == O_SHR_C || op->id == O_FADD_C ||
            op->id == O_FMUL_C || op->id == O_FFMA_RC || op->id == O_FFMA_CR || op->id == O_FMNMX_C ||
            op->id == O_RRO_C || op->id == O_FSETP_C || op->id == O_FSET_C || op->id == O_HSET2_C ||
            op->id == O_HSETP2_C || op->id == O_DADD_C || op->id == O_DMUL_C || op->id == O_DFMA_RC ||
            op->id == O_DFMA_CR || op->id == O_DMNMX_C || op->id == O_DSET_C || op->id == O_DSETP_C ||
            op->id == O_OUT_C || op->id == O_PRMT_RC || op->id == O_PRMT_CR) {
            u32 idx = B(insn[i], 34, 5), off = B(insn[i], 20, 14) * 4;
            if (idx >= 18) return direct_reject(si, "constant buffer index");
            cbuf_used[idx] = 1;
            if ((off | 15u) + 1 > cbuf_max[idx]) cbuf_max[idx] = (off | 15u) + 1;
        }
        if (op->id == O_OUT_R && b < 255) used[b] = 1;
        if (op->id == O_FSETP_R || op->id == O_FSETP_C || op->id == O_FSETP_I) {
            if (a < 255) used[a] = 1;
            if (op->id == O_FSETP_R && b < 255) used[b] = 1;
            u32 pc = B(insn[i], 39, 3), pa = B(insn[i], 3, 3), pb = B(insn[i], 0, 3);
            if (pc < 7) pred_used[pc] = 1;
            if (pa < 7) pred_used[pa] = 1;
            if (pb < 7) pred_used[pb] = 1;
            needs_glsl = 1;
        }
        if (op->id == O_FMNMX_R || op->id == O_FMNMX_C || op->id == O_FMNMX_I) {
            u32 predicate = B(insn[i], 39, 3);
            if (predicate < 7) pred_used[predicate] = 1;
        }
        if (op->id == O_FSET_R || op->id == O_FSET_C || op->id == O_FSET_I) {
            u32 predicate = B(insn[i], 39, 3);
            if (predicate < 7) pred_used[predicate] = 1;
            if (B(insn[i], 54, 1) || B(insn[i], 44, 1)) needs_glsl = 1;
        }
        if (op->id == O_TEXS) {
            u32 off = B(insn[i], 36, 13);
            u32 encoding = B(insn[i], 53, 4);
            int shadow = texs_shadow(encoding), type = texs_type(encoding), array = texs_array(encoding);
            int slot = 0;
            for (; slot < texture_count; slot++)
                if (textures[slot].cbuf_off == off && textures[slot].shadow == shadow && textures[slot].type == type &&
                    textures[slot].array == array && !textures[slot].bindless && !textures[slot].flags)
                    break;
            if (slot == texture_count) {
                if (texture_count >= SH_MAXTEX) return direct_reject(si, "texture descriptor count");
                textures[slot].cbuf_off = off;
                textures[slot].type = (u8)type;
                textures[slot].shadow = (u8)shadow;
                textures[slot].array = (u8)array;
                texture_count++;
            }
            textures[slot].use |= 1; /* SWR: a filtered sample */
            if (!B(insn[i], 59, 1)) needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
            if (encoding != 0 && texs_y(encoding, a, b) == a + 1 && a + 1 < 255) used[a + 1] = 1;
            if (array && a + 1 < 255) used[a + 1] = 1;
            if (texs_level(encoding, b) < 255) used[texs_level(encoding, b)] = 1;
            if (shadow && texs_dref(encoding, b) < 255) used[texs_dref(encoding, b)] = 1;
            texs_mark_outputs(insn[i], used);
        }
        /* fragment fetches and size queries may read a scaled target (scaled_coords) */
        if ((op->id == O_TLD || op->id == O_TLD_B || op->id == O_TLDS || op->id == O_TXQ || op->id == O_TXQ_B) &&
            stage == ST_FS)
            needs_texel_scale = needs_glsl = 1;
        if (op->id == O_TLD || op->id == O_TLD_B) {
            u32 indexed = 0, ldc = 0;
            u32 off = B(insn[i], 36, 13), ty = B(insn[i], 28, 3);
            u32 bank = 0, bank2 = 0, off2 = 0;
            int bindless = op->id == O_TLD_B;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            if (bindless && !handle_count) return direct_reject(si, "untracked texture handle");
            int type = ty >> 1, array = ty & 1, multisample = (int)B(insn[i], 50, 1);
            { /* SWR: the slot's reads (ShTex.use) */
                int use_slot = direct_add_typed_slot(textures, &texture_count, off, type, array, 0, multisample ? 2 : 0, bindless,
                                          bank, handle_count, bank2, off2, indexed, ldc);
                if (use_slot < 0)
                    return direct_reject(si, "texture descriptor count");
                textures[use_slot].use |= 2;
            }
            u32 coordinate_count = (type == 0 ? 1u : type == 1 ? 2u : 3u) + (u32)array;
            for (u32 k = 0; k < coordinate_count; k++)
                if (a + k < 255) used[a + k] = 1;
            if (bindless && b < 255) used[b] = 1;
            u32 meta = b + bindless;
            if (B(insn[i], 55, 1) && meta < 255) used[meta++] = 1;
            if (B(insn[i], 35, 1) && meta < 255) used[meta++] = 1;
            if (multisample && meta < 255) used[meta] = 1;
            u32 target = d, mask = B(insn[i], 31, 4);
            for (u32 k = 0; k < 4; k++)
                if (mask & (1u << k)) {
                    if (target < 255) used[target] = 1;
                    target++;
                }
        }
        if (op->id == O_SULD || op->id == O_SUST || op->id == O_SURED || op->id == O_SUATOM) {
            int dim = (int)B(insn[i], 33, 3), buffer = dim == 1;
            u32 bank, off, handle;
            int bindless;
            if (buffer && stage != ST_CS) return direct_reject(si, "buffer surface outside compute");
            /* a fragment program's 2D loads are texel fetches, as a compute program's are: render.c binds the raw
               view their slot asks for. A vertex program stores and reduces through images as a fragment one does
               (one game filters a texture so, a vertex for each texel, with no compute at all) */
            if (stage != ST_CS && stage != ST_FS && (stage != ST_VS || op->id == O_SULD))
                return direct_reject(si, "surface operation outside supported stage");
            bank = 0;
            off = B(insn[i], 36, 13);
            handle = B(insn[i], 39, 8);
            bindless = op->id == O_SUATOM ? (int)B(insn[i], 54, 1) : !B(insn[i], 51, 1);
            if (bindless && !direct_track_image_handle(insn, n, i, handle, &bank, &off, 0))
                return direct_reject(si, "untracked surface handle");
            if (a < 255) used[a] = 1;
            if (!buffer && a + 1 < 255) used[a + 1] = 1;
            if ((dim == 4 || dim == 5) && a + 2 < 255) used[a + 2] = 1;
            if (bindless && handle < 255) used[handle] = 1;
            int raw = op->id == O_SULD && B(insn[i], 52, 1);
            u32 mask = raw ? 1u : B(insn[i], 20, 4), target = d;
            if (op->id == O_SURED) {
                if (target < 255) used[target] = 1;
            } else if (op->id == O_SUATOM) {
                if (d < 255) used[d] = 1;
                if (b < 255) used[b] = 1;
            } else
                for (u32 q = 0; q < 4; q++)
                    if (mask & (1u << q)) {
                        if (target < 255) used[target] = 1;
                        target++;
                    }
            /* buffer loads read the storage texel buffer: a sampled 1D image of a buffer is one row of at most
               16384 texels, and the dispatch's own stores and atomics wouldn't reach it */
            if (op->id != O_SULD || buffer) {
                int r32ui = op->id != O_SUST;
                int type = dim; /* 1, 3, 4 or 5: the forms supported() takes here */
                int slot = direct_image_slot(&image_info, off, bindless, bank, r32ui, type);
                if (slot == image_info.nimg) {
                    if (slot >= 8) return direct_reject(si, "storage image descriptor count");
                    int different_format = !!(si->rt_int_mask & (1u << (8 + slot)));
                    int kind = r32ui ? 1 : (bindless != different_format ? 2 : 0);
                    if (!r32ui && !buffer && (si->rt_sint_mask & (1u << (8 + slot)))) kind = 3;
                    image_info.img[slot].cbuf_off = off;
                    image_info.img[slot].type = (u8)type;
                    image_info.img[slot].bindless = (u8)bindless;
                    image_info.img[slot].bl_cbuf = (u8)bank;
                    image_info.img[slot].kind = (u8)kind;
                    image_info.nimg++;
                }
                if (bindless) {
                    cbuf_used[bank] = 1;
                    if (off * 4 + 16 > cbuf_max[bank]) cbuf_max[bank] = ((off * 4) | 15u) + 1;
                }
            } else {
                int slot = 0;
                int sample_type = dim == 5 ? 2 : 1, arrayed = dim == 4;
                while (slot < texture_count &&
                       (textures[slot].cbuf_off != off || textures[slot].type != sample_type ||
                        textures[slot].array != arrayed ||
                        textures[slot].shadow || textures[slot].bindless != bindless ||
                        (textures[slot].flags & 3) != (raw ? 1 : 0) || (bindless && textures[slot].bl_cbuf != bank)))
                    slot++;
                if (slot == texture_count) {
                    if (slot >= SH_MAXTEX) return direct_reject(si, "texture descriptor count");
                    textures[slot].cbuf_off = off;
                    textures[slot].type = (u8)sample_type;
                    textures[slot].array = (u8)arrayed;
                    textures[slot].bindless = (u8)bindless;
                    textures[slot].flags = raw ? 1 : 0;
                    textures[slot].bl_cbuf = (u8)bank;
                    texture_count++;
                }
            }
        }
        if (half_arithmetic(op->id)) {
            needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if ((op->id == O_HADD2_R || op->id == O_HMUL2_R || op->id == O_HFMA2_R) && b < 255) used[b] = 1;
            if ((op->id == O_HFMA2_R || op->id == O_HFMA2_RC || op->id == O_HFMA2_CR || op->id == O_HFMA2_I) &&
                B(insn[i], 39, 8) < 255)
                used[B(insn[i], 39, 8)] = 1;
        }
        if (op->id == O_HSET2_R || op->id == O_HSET2_C || op->id == O_HSET2_I) {
            needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if (op->id == O_HSET2_R && b < 255) used[b] = 1;
            if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
        }
        if (op->id == O_HSETP2_R || op->id == O_HSETP2_C || op->id == O_HSETP2_I) {
            needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if (op->id == O_HSETP2_R && b < 255) used[b] = 1;
            u32 pp[] = {B(insn[i], 39, 3), B(insn[i], 3, 3), B(insn[i], 0, 3)};
            for (u32 k = 0; k < 3; k++)
                if (pp[k] < 7) pred_used[pp[k]] = 1;
        }
        if ((op->id >= O_DADD_R && op->id <= O_DADD_I) || (op->id >= O_DMUL_R && op->id <= O_DMUL_I) ||
            (op->id >= O_DFMA_R && op->id <= O_DFMA_I) || (op->id >= O_DMNMX_R && op->id <= O_DMNMX_I) ||
            (op->id >= O_DSET_R && op->id <= O_DSET_I) || (op->id >= O_DSETP_R && op->id <= O_DSETP_I)) {
            int right_reg;
            needs_f64 = needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if (a < 254) used[a + 1] = 1;
            right_reg = op->id == O_DADD_R || op->id == O_DMUL_R || op->id == O_DFMA_R || op->id == O_DFMA_RC ||
                        op->id == O_DMNMX_R || op->id == O_DSET_R || op->id == O_DSETP_R;
            if (right_reg && b < 255) used[b] = 1;
            if (right_reg && b < 254) used[b + 1] = 1;
            if ((op->id >= O_DADD_R && op->id <= O_DADD_I) || (op->id >= O_DMUL_R && op->id <= O_DMUL_I) ||
                (op->id >= O_DFMA_R && op->id <= O_DFMA_I) || (op->id >= O_DMNMX_R && op->id <= O_DMNMX_I)) {
                if (d < 254) used[d + 1] = 1;
                if ((op->id >= O_DFMA_R && op->id <= O_DFMA_I) && B(insn[i], 39, 8) < 255) {
                    used[B(insn[i], 39, 8)] = 1;
                    if (B(insn[i], 39, 8) < 254) used[B(insn[i], 39, 8) + 1] = 1;
                }
            }
            if ((op->id >= O_DMNMX_R && op->id <= O_DMNMX_I) || (op->id >= O_DSET_R && op->id <= O_DSET_I) ||
                (op->id >= O_DSETP_R && op->id <= O_DSETP_I))
                if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
            if (op->id >= O_DSETP_R && op->id <= O_DSETP_I) {
                if (B(insn[i], 3, 3) < 7) pred_used[B(insn[i], 3, 3)] = 1;
                if (B(insn[i], 0, 3) < 7) pred_used[B(insn[i], 0, 3)] = 1;
            }
        }
        if (op->id == O_FSWZADD) {
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
        }
        if (op->id == O_VMNMX) {
            if (a < 255) used[a] = 1;
            if (B(insn[i], 50, 1) && b < 255) used[b] = 1;
            u32 third = B(insn[i], 39, 8);
            if (third < 255) used[third] = 1;
        }
        if (op->id == O_VMAD) {
            if (a < 255) used[a] = 1;
            if (B(insn[i], 50, 1) && b < 255) used[b] = 1;
            u32 third = B(insn[i], 39, 8);
            if (third < 255) used[third] = 1;
        }
        if (op->id == O_VSETP) {
            needs_i64 = 1;
            if (a < 255) used[a] = 1;
            if (B(insn[i], 50, 1) && b < 255) used[b] = 1;
            if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
            if (B(insn[i], 3, 3) < 7) pred_used[B(insn[i], 3, 3)] = 1;
            if (B(insn[i], 0, 3) < 7) pred_used[B(insn[i], 0, 3)] = 1;
        }
        if (op->id == O_TLDS) {
            u32 off = B(insn[i], 36, 13);
            u32 enc = B(insn[i], 53, 4);
            /* encoding 6 fetches a sample (Rb) of a multisample texture */
            int array = enc == 8, type = enc == 0 || enc == 1 ? 0 : enc == 7 ? 2 : 1, multisample = enc == 6;
            int slot = 0;
            for (; slot < texture_count; slot++)
                if (textures[slot].cbuf_off == off && textures[slot].type == type && textures[slot].array == array &&
                    !textures[slot].shadow && !textures[slot].bindless && textures[slot].flags == (multisample ? 2 : 0))
                    break;
            if (slot == texture_count) {
                if (texture_count >= SH_MAXTEX) return direct_reject(si, "texture descriptor count");
                textures[slot].cbuf_off = off;
                textures[slot].type = (u8)type;
                textures[slot].array = (u8)array;
                textures[slot].flags = (u8)(multisample ? 2 : 0);
                texture_count++;
            }
            textures[slot].use |= 2; /* SWR: a fetch */
            if (!B(insn[i], 59, 1)) needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if ((enc == 4 || enc == 5 || enc == 6 || enc == 7 || enc == 12) && a + 1 < 255) used[a + 1] = 1;
            if (b < 255) used[b] = 1;
            if ((enc == 12 || array) && b + 1 < 255) used[b + 1] = 1;
            if (!B(insn[i], 59, 1)) {
                u32 regs[4], comps[4];
                int outputs = texs_outputs(insn[i], regs, comps);
                needs_f16 = 1;
                if (d < 255) used[d] = 1;
                if (outputs > 2 && B(insn[i], 28, 8) < 255) used[B(insn[i], 28, 8)] = 1;
            } else {
                texs_mark_outputs(insn[i], used);
            }
        }
        if (op->id == O_TEX || op->id == O_TEX_B) {
            int bindless = op->id == O_TEX_B;
            u32 mode = bindless ? B(insn[i], 37, 3) : B(insn[i], 55, 3);
            u32 offset_enabled = B(insn[i], bindless ? 36 : 54, 1);
            u32 source = b + bindless;
            u32 has_level = mode == 2 || mode == 3 || mode == 6 || mode == 7;
            u32 offset_source = source + has_level;
            u32 off, bank, bank2, off2, indexed = 0, ldc = 0;
            int handle_count;
            if (offset_enabled) {
                u32 packed;
                int known = direct_track_const(insn, n, i, offset_source, &packed, 0);
                if (!known) needs_image_query = 1; /* the offset moves the coordinates by the texture's size */
            }
            off = B(insn[i], 36, 13);
            bank = 0;
            bank2 = 0;
            off2 = 0;
            handle_count = bindless ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            if (bindless && !handle_count) return direct_reject(si, "untracked texture handle");
            int type = B(insn[i], 28, 3) >> 1;
            int array = B(insn[i], 28, 1), shadow = B(insn[i], 50, 1);
            { /* SWR: the slot's reads (ShTex.use) */
                int use_slot = direct_add_typed_slot(textures, &texture_count, off, type, array, shadow, 0, bindless, bank,
                                          handle_count, bank2, off2, indexed, ldc);
                if (use_slot < 0)
                    return direct_reject(si, "texture descriptor count");
                textures[use_slot].use |= 1;
            }
            if (a < 255) used[a] = 1;
            if ((type != 0 || array) && a + 1 < 255) used[a + 1] = 1;
            if ((type >= 2 || array) && a + 2 < 255) used[a + 2] = 1;
            if (type == 3 && array && a + 3 < 255) used[a + 3] = 1;
            if (b < 255) used[b] = 1;
            if (has_level && source < 255) used[source] = 1;
            if (offset_enabled && offset_source < 255) used[offset_source] = 1;
            if (shadow && offset_source + offset_enabled < 255) used[offset_source + offset_enabled] = 1;
            u32 mask = B(insn[i], 31, 4), dst = d;
            for (u32 component = 0; component < 4; component++)
                if (mask & (1u << component)) {
                    if (dst < 255) used[dst] = 1;
                    if (dst < 255) dst++;
                }
        }
        /* Ryujinx InstEmitTexture.EmitTmml: TMML.B takes its handle in Rb; Ra holds an array's layer, then coordinates. */
        if (op->id == O_TMML || op->id == O_TMML_B) {
            int bindless = op->id == O_TMML_B;
            u32 ty = B(insn[i], 28, 3), type = ty >> 1, array = ty & 1;
            u32 off = B(insn[i], 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (bindless && !handle_count) return direct_reject(si, "untracked texture handle");
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            { /* SWR: the slot's reads (ShTex.use) */
                int use_slot = direct_add_typed_slot(textures, &texture_count, off, type, array, 0, 0, bindless, bank, handle_count,
                                          bank2, off2, indexed, ldc);
                if (use_slot < 0)
                    return direct_reject(si, "texture descriptor count");
                textures[use_slot].use |= 2;
            }
            for (u32 k = 0; k < array + (type == 0 ? 1 : type == 1 ? 2 : 3); k++)
                if (a + k < 255) used[a + k] = 1;
            if (bindless && b < 255) used[b] = 1;
            u32 mask = B(insn[i], 31, 4), dst = d;
            for (u32 component = 0; component < 4; component++)
                if (mask & (1u << component) && dst < 255) used[dst++] = 1;
            needs_image_query = 1;
        }
        /* TXD and TXD.B: in Ra TXD.B's handle, the coordinates, then the array layer (bits 0-15) and the offsets (from
           bit 16) in one register; in Rb each coordinate's X derivative then its Y one. Mesa's NAK packs them so for
           Maxwell, and Eden reads them so (Ryujinx takes every X derivative first). */
        if (op->id == O_TXD || op->id == O_TXD_B) {
            int bindless = op->id == O_TXD_B;
            u32 ty = B(insn[i], 28, 3), type = ty >> 1, array = ty & 1, count = type == 0 ? 1 : type == 1 ? 2 : 3;
            u32 off = B(insn[i], 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, a, &bank, &off, &bank2, &off2, 0) : 0;
            if (bindless && !handle_count) return direct_reject(si, "untracked texture handle");
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, a, stage, &ldc);
            { /* SWR: the slot's reads (ShTex.use) */
                int use_slot = direct_add_typed_slot(textures, &texture_count, off, type, array, 0, 0, bindless, bank, handle_count,
                                          bank2, off2, indexed, ldc);
                if (use_slot < 0)
                    return direct_reject(si, "texture descriptor count");
                textures[use_slot].use |= 1;
            }
            u32 coord = a + (u32)bindless, aoffi = B(insn[i], 35, 1);
            for (u32 k = 0; k < count + (array || aoffi); k++)
                if (a + k + (u32)bindless < 255) used[coord + k] = 1;
            if (bindless && a < 255) used[a] = 1;
            for (u32 k = 0; k < 2 * count; k++)
                if (b + k < 255) used[b + k] = 1;
            if (aoffi) {
                u32 packed;
                if (!direct_track_const(insn, n, i, coord + count, &packed, 0))
                    needs_image_query = 1; /* the offset moves the coordinates by the texture's size */
            }
            u32 mask = B(insn[i], 31, 4), dst = d;
            for (u32 component = 0; component < 4; component++)
                if (mask & (1u << component) && dst < 255) used[dst++] = 1;
        }
        if (op->id == O_TXQ || op->id == O_TXQ_B) {
            /* TXQ.B takes the handle in Ra and the level in Ra + 1 */
            int bindless = op->id == O_TXQ_B;
            u32 off = B(insn[i], 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, a, &bank, &off, &bank2, &off2, 0) : 0;
            if (bindless && !handle_count) return direct_reject(si, "untracked texture handle");
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, a, stage, &ldc);
            { /* SWR: the slot's reads (ShTex.use) */
                u32 qtype = 1, qarray = 0;
                if (!indexed) direct_query_type(insn, n, bindless, bank, off, &qtype, &qarray);
                int use_slot = direct_add_typed_slot(textures, &texture_count, off, qtype, qarray, 0, 0, bindless, bank,
                                                     handle_count, bank2, off2, indexed, ldc);
                if (use_slot < 0)
                    return direct_reject(si, "texture descriptor count");
                textures[use_slot].use |= 2;
            }
            needs_image_query = 1;
            if (a < 255) used[a] = 1;
            if (bindless && a + 1 < 255) used[a + 1] = 1;
            u32 target = d;
            for (u32 k = 0; k < 4; k++)
                if (B(insn[i], 31 + k, 1)) {
                    if (target < 255) used[target] = 1;
                    target++;
                }
        }
        if (op->id == O_TLD4S || op->id == O_TLD4 || op->id == O_TLD4_B) {
            u32 indexed = 0, ldc = 0;
            int bindless = op->id == O_TLD4_B;
            u32 off = B(insn[i], 36, 13), bank = 0, bank2 = 0, off2 = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            if (bindless && !handle_count) return direct_reject(si, "untracked texture handle");
            u32 ty = op->id == O_TLD4S ? 2 : B(insn[i], 28, 3);
            u32 type = ty >> 1, array = ty & 1;
            u32 shadow = B(insn[i], 50, 1);
            { /* SWR: the slot's reads (ShTex.use) */
                int use_slot = direct_add_typed_slot(textures, &texture_count, off, type, array, shadow, 0, bindless, bank,
                                          handle_count, bank2, off2, indexed, ldc);
                if (use_slot < 0)
                    return direct_reject(si, "texture descriptor count");
                textures[use_slot].use |= 2;
            }
            if (a < 255) used[a] = 1;
            if (op->id == O_TLD4S) {
                if (b < 255) used[b] = 1;
                if (B(insn[i], 51, 1)) needs_gather_extended = 1;
                if (B(insn[i], 55, 1)) needs_glsl = 1;
                if (B(insn[i], 50, 1) || B(insn[i], 51, 1)) {
                    if (a + 1 < 255) used[a + 1] = 1;
                    if (B(insn[i], 50, 1) && B(insn[i], 51, 1) && b + 1 < 255) used[b + 1] = 1;
                }
                u32 db = B(insn[i], 28, 8);
                if (!B(insn[i], 55, 1) && d < 254) used[d + 1] = 1;
                if (db < 255) used[db] = 1;
                if (!B(insn[i], 55, 1) && db < 254) used[db + 1] = 1;
            } else {
                u32 coords = (type == 0 ? 1u : type == 1 ? 2u : 3u) + array;
                for (u32 k = 1; k < coords; k++)
                    if (a + k < 255) used[a + k] = 1;
                u32 extra = b + bindless;
                if (B(insn[i], bindless ? 36 : 54, 2) == 1) {
                    if (extra < 255) used[extra] = 1;
                    extra++;
                    needs_gather_extended = 1;
                } else if (B(insn[i], bindless ? 36 : 54, 2) == 2) {
                    if (extra < 255) used[extra] = 1;
                    if (extra + 1 < 255) used[extra + 1] = 1;
                    extra += 2;
                    needs_gather_extended = 1;
                }
                if (shadow && extra < 255) used[extra] = 1;
                u32 mask = B(insn[i], 31, 4), dst = d;
                for (u32 component = 0; component < 4; component++)
                    if (mask & (1u << component) && dst < 255) used[dst++] = 1;
            }
        }
        if (op->id == O_IPA && B(insn[i], 38, 1)) {
            /* indexed: any generic input the header's map gives a mode, the perspective ones through FragCoord */
            if (B(insn[i], 51, 1)) needs_glsl = 1;
            used[a] = 1;
            for (u32 v = 0; v < 32; v++)
                if ((sph[6 + v / 4] >> (v % 4 * 8)) & 0xff) {
                    inputs_used[v] = 1;
                    if (ipa_interpolation(sph, v) == 2) needs_fragcoord = 1;
                }
            if (B(insn[i], 54, 2) == 1 && b < 255) used[b] = 1;
        } else if (op->id == O_IPA) {
            if (B(insn[i], 51, 1)) needs_glsl = 1;
            u32 addr = B(insn[i], 30, 8) * 4;
            if (addr == 0x64) {
                layer_input_used = 1;
            } else if (addr == 0x3fc) {
                needs_frontface = 1;
            } else if (addr == 0x2e0 || addr == 0x2e4) {
                needs_point_coord = 1;
            } else if (addr == 0x60) {
                primitive_id_used = 1;
            } else if (addr >= 0x80) {
                u32 input = (addr - 0x80) >> 4;
                u32 interpolation = ipa_interpolation(sph, input);
                inputs_used[input] = 1;
                if (interpolation == 2) needs_fragcoord = 1;
            } else
                needs_fragcoord = 1;
            if (addr == 0x70 || addr == 0x74) needs_scale = 1;
            if (B(insn[i], 54, 2) == 1 && b < 255) used[b] = 1;
        }
        if (op->id == O_EXIT && flow_always(insn[i])) has_exit = 1;
        if (op->id == O_KIL) needs_demote = 1;
        if (stage == ST_FS &&
            (op->id == O_TMML || op->id == O_TMML_B ||
             (op->id == O_TEXS && !texs_explicit(B(insn[i], 53, 4))) ||
             ((op->id == O_TEX || op->id == O_TEX_B) &&
              (B(insn[i], op->id == O_TEX_B ? 37 : 55, 3) == 0 || B(insn[i], op->id == O_TEX_B ? 37 : 55, 3) == 2 ||
               B(insn[i], op->id == O_TEX_B ? 37 : 55, 3) == 6))))
            needs_derivatives = 1;
        if (stage == ST_FS && helper_side_effect(op->id)) needs_demote = 1;
        if (op->id == O_VOTE || op->id == O_SHFL) needs_subgroup = 1;
        /* FSWZADD's lanes in a quad are the subgroup's, as SHFL's are, when they don't all do the same */
        if (op->id == O_FSWZADD && fswzadd_lanes(insn[i], B(insn[i], 28, 2)) != 15) needs_subgroup = 1;
        if (op->id == O_S2R && (B(insn[i], 20, 8) == 0 || (B(insn[i], 20, 8) >= 0x38 && B(insn[i], 20, 8) <= 0x3c)))
            lane_used = 1;
    }
    /* the lane and its masks come from the subgroup, where VOTE and SHFL see the lanes. A vertex pipeline program
       without them has the lane of the batch model the translator gives it (SR_INVOCATION_INFO's primitive index 0:
       each primitive has its batch to itself), whatever the device's subgroups: a control program's invocations are
       lanes 0 up, its invocation id, and any other's is lane 0. NVN's evaluation programs read SR_LANEID (into a
       register SR_INVOCATION_INFO then overwrites), and on a device without subgroups in that stage (lavapipe) the
       program wasn't translated and its draws were lost */
    int lane_model = !needs_subgroup && stage != ST_FS && stage != ST_CS;
    int lane_is_invocation = lane_model && stage == ST_TCS;
    if (lane_used && lane_is_invocation) invocation_used = 1;
    if (lane_used && !lane_model) needs_subgroup = 1;
    if (!has_exit) return direct_reject(si, "missing exit");
    if (has_pexit && has_conditional_exit) return direct_reject(si, "conditional EXIT with PEXIT token");
    if (needs_subgroup && !can_subgroup) return direct_reject(si, "subgroup ballot/shuffle unavailable");
    if (has_flow && needs_derivatives && !can_subgroup)
        return direct_reject(si, "fragment derivative convergence unavailable");
    if (needs_f64 && !can_preserve_f64) return direct_reject(si, "FP64 float controls unavailable");
    /* The flow loop runs one block per iteration, so lanes that took the two sides of a branch reach the join on
       different iterations, and VOTE, SHFL and BAR would see part of the warp. Each iteration runs only the lanes
       at the subgroup's lowest pc, as the hardware reconverges: lanes ahead wait for the others to reach them. */
    converge = has_flow && can_subgroup && (needs_subgroup || has_barrier || needs_derivatives);
    /* FTZ per instruction: without the execution mode, or with it where some FADD, FMUL or FFMA keeps subnormals (the
       mode is the whole module's). Never on a device that flushes by itself, which also flushes those that keep them:
       the flush would change no bit */
    int soft_ftz_on = can_flush != SH_FTZ_NATIVE && (can_flush == SH_FTZ_EMULATE || fp32_preserve);
    if (needs_shared && (stage != ST_CS || si->shared_size <= 0)) return direct_reject(si, "shared memory size");
    for (int i = 0; i < texture_count; i++)
        if (textures[i].shadow && (si->tex_int_mask & (1ull << i))) return direct_reject(si, "integer shadow texture");
    u32* pop_targets = has_stack ? calloc(n, sizeof *pop_targets) : NULL;
    if (has_stack && !pop_targets) return NULL;
    int dynamic_stack = has_pexit || (has_stack && !shader_resolve_crs(insn, n, pop_targets));

    u8* gb_at = calloc(n, 1);
    if (!gb_at) {
        free(pop_targets);
        return NULL;
    }
    int gtags[255] = {0}, ngbuf = 0;
    u32 gbuf_wmask = 0;
    u32 gbuf_off[16] = {0};
    /* the first access whose base the tracking lost (0 for none), its register, and whether one of them stores */
    u32 untracked_at = 0, untracked_reg = 0;
    int untracked_store = 0;
    for (u32 i = 11; i < n; i++) {
        u32 second;
        u32 r;
        if (!IDX_OK(i)) continue;
        u64 w = insn[i];
        int op = decode(w)->id;
        u32 d = B(w, 0, 8), a = B(w, 8, 8), b = B(w, 20, 8);
        int tag = 0;
        /* Direct CBE export is disabled (SR_DIRECTCBEWRITE* are zero). Track its address separately so the
           compiler's unused direct-export stores do not require a guest global-buffer descriptor. */
        if (op == O_S2R && B(w, 20, 8) == 0x15)
            tag = 0xcbe;
        else if (op == O_MOV_C) {
            tag = gbuf_tag(stage, B(w, 34, 5), B(w, 20, 14) * 4);
            if (!tag) tag = gbuf_tag_any(stage, B(w, 34, 5), B(w, 20, 14) * 4);
        } else if (op == O_LDC && a == 255 && (B(w, 48, 3) == 4 || B(w, 48, 3) == 5)) {
            tag = gbuf_tag(stage, B(w, 36, 5), (u32)(i32)(i16)B(w, 20, 16));
            if (!tag) tag = gbuf_tag_any(stage, B(w, 36, 5), (u32)(i32)(i16)B(w, 20, 16));
        } else if (op == O_MOV_R)
            tag = b == 255 ? 0 : gtags[b];
        else if (op == O_IADD32I && a < 255)
            tag = gtags[a];
        else if (op == O_IADD_I && a < 255 && B(w, 48, 2) != 1)
            tag = gtags[a];
        else if (op == O_IADD_C) {
            tag = gbuf_tag(stage, B(w, 34, 5), B(w, 20, 14) * 4);
            if (!tag && B(w, 48, 2) != 2 && a < 255) tag = gtags[a];
            /* an index plus a bare address of the program's own, neither negated. Of two such constants added
               (an offset into the buffer, then its address), the one nearest the access is the base, as Eden's
               breadth-first search back from the access finds it: one game adds c[16][0x10], an offset, and then
               c[6][0x0], the address. An NVN descriptor's address already in Ra stays the base */
            if (!B(w, 48, 2) && (!tag || tag > 0xFFFF)) {
                int bare = gbuf_tag_any(stage, B(w, 34, 5), B(w, 20, 14) * 4);
                if (bare) tag = bare;
            }
            if (B(w, 48, 2) == 1) tag = 0;
        } else if (op == O_IADD_R) {
            tag = B(w, 48, 2) == 1      ? (a < 255 ? gtags[a] : 0)
                  : B(w, 48, 2) == 2    ? (b < 255 ? gtags[b] : 0)
                  : a < 255 && gtags[a] ? gtags[a]
                  : b < 255             ? gtags[b]
                                        : 0;
        } else if ((op == O_IADD3_I || (op == O_IADD3_R && !B(w, 31, 8))) && !B(w, 48, 4)) {
            u32 cr = B(w, 39, 8);
            tag = a < 255 && gtags[a]          ? gtags[a]
                  : cr < 255 && gtags[cr]      ? gtags[cr]
                  : op == O_IADD3_R && b < 255 ? gtags[b]
                                               : 0;
        } else if (op == O_ISCADD_C && B(w, 48, 2) != 1) {
            tag = gbuf_tag(stage, B(w, 34, 5), B(w, 20, 14) * 4);
            if (!tag) tag = gbuf_tag_any(stage, B(w, 34, 5), B(w, 20, 14) * 4);
        } else if (op == O_ISCADD_R && B(w, 48, 2) != 1 && b < 255)
            tag = gtags[b];
        /* LEA's low word adds the base the same way, with its carry out for the high word's LEA.HI.X; negated
           (bit 45) or with a carry in (bit 46) it is no base plus an index */
        else if (op == O_LEA_LC && !B(w, 45, 2)) {
            tag = gbuf_tag(stage, B(w, 34, 5), B(w, 20, 14) * 4);
            if (!tag) tag = gbuf_tag_any(stage, B(w, 34, 5), B(w, 20, 14) * 4);
        } else if (op == O_LEA_LR && !B(w, 45, 2) && b < 255)
            tag = gtags[b];
        if (tag == 0xb1 && op != O_MOV_C && op != O_LDC && op != O_MOV_R) tag = 0;
        if (op == O_LDG || op == O_STG || op == O_RED || op == O_ATOM || op == O_ATOM_CAS) {
            int current = a < 255 ? gtags[a] : 0, slot = 0;
            if (!current && (op == O_LDG || op == O_STG) && B(w, 48, 3) >= 4) {
                /* the address came along a path the tracking doesn't follow (a switch's case, a loop): it is found
                   among the program's tracked buffers when it runs (global_dynamic) */
                gb_at[i] = GB_DYNAMIC;
                untracked_store |= op == O_STG;
                if (!untracked_at) {
                    untracked_at = i;
                    untracked_reg = a;
                }
            } else if (!current) {
                untracked_at = i;
                untracked_reg = a;
                return untracked_reject(si, insn, untracked_at, untracked_reg, gb_at, pop_targets);
            } else if (current == 0xcbe) {
                if (op != O_STG) {
                    free(gb_at);
                    free(pop_targets);
                    return direct_reject(si, "direct CBE address used for a non-export operation");
                }
                gb_at[i] = 255; /* disabled direct CBE export */
            } else {
                if (current == 0xb1) {
                    i32 byte_offset = (i32)(B(w, 20, 24) << 8) >> 8;
                    u32 words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1;
                    if ((op != O_LDG && op != O_STG) || byte_offset < 0 || (u32)byte_offset + words * 4 > 16) {
                        free(gb_at);
                        free(pop_targets);
                        return direct_reject(si, "vertex pointer access exceeds known range");
                    }
                }
                u32 descriptor = (u32)(current - 1);
                for (; slot < ngbuf && gbuf_off[slot] != descriptor; slot++) {}
                if (slot == ngbuf) {
                    if (ngbuf == 16) {
                        free(gb_at);
                        free(pop_targets);
                        return direct_reject(si, "global buffer count");
                    }
                    gbuf_off[ngbuf++] = descriptor;
                }
                gb_at[i] = (u8)(slot + 1);
                if (op != O_LDG) gbuf_wmask |= 1u << slot;
                {
                    u32 bank = SH_GBUF_BANK(descriptor), at = SH_GBUF_OFFSET(descriptor);
                    cbuf_used[bank] = 1;
                    if ((at | 15u) + 1 > cbuf_max[bank]) cbuf_max[bank] = (at | 15u) + 1;
                }
            }
        }
        /* every register the instruction writes stops being a pointer, then its destination takes the new tag */
        second = op == O_TEXS || op == O_TLDS || op == O_TLD4S ? B(w, 28, 8) : 255;
        for (r = d; r < d + 4 && r < 255; r++)
            if (shader_writes_reg(w, op, r)) gtags[r] = 0;
        for (r = second; r < second + 2 && r < 255; r++)
            if (shader_writes_reg(w, op, r)) gtags[r] = 0;
        if (shader_writes_reg(w, op, d)) gtags[d] = tag;
    }
    /* an untracked access needs a tracked buffer to be in; one that stores may write any of them */
    for (int slot = 0; untracked_at && slot < ngbuf; slot++)
        if (gbuf_off[slot] != 0xb0) {
            untracked_at = 0;
            if (untracked_store) gbuf_wmask |= (1u << ngbuf) - 1;
        }
    if (untracked_at) return untracked_reject(si, insn, untracked_at, untracked_reg, gb_at, pop_targets);

    /* A zero skip bit forwards that scalar from the input primitive. */
    if (passthrough) {
        if ((~si->gp_skip_mask[0] >> 28) & 15) position_used = position_in_used = 1;
        for (u32 a = 0; a < 32; a++)
            if ((~si->gp_skip_mask[1 + a / 8] >> ((a % 8) * 4)) & 15) inputs_used[a] = outputs_used[a] = 1;
    }

    /* the primitives of the stages between: the control program's output patch (the SPH's threads per input
       primitive), the evaluation program's domain, spacing and output (ShaderInfo.rt_int_mask), and the geometry
       program's input and output primitives */
    if (stage == ST_TCS && (!out_vertices || out_vertices > 32))
        return direct_reject_with_buffers(si, "patch output vertices", pop_targets, gb_at);
    if (viewport_used && (stage == ST_TCS || !(g_shader_viewport_index & (stage == ST_GS ? 2 : 1))))
        return direct_reject_with_buffers(si, "viewport index output unavailable", pop_targets, gb_at);
    if (stage == ST_TCS && (clip_used || layer_output_used || point_size_used))
        return direct_reject_with_buffers(si, "clip distance, layer or point size in tessellation control", pop_targets,
                                          gb_at);
    if (stage == ST_TES && ((si->rt_int_mask & 3) == 3 || ((si->rt_int_mask >> 4) & 3) == 3))
        return direct_reject_with_buffers(si, "tessellation domain or spacing", pop_targets, gb_at);
    if (stage == ST_GS && in_vertices != 1 && in_vertices != 2 && in_vertices != 3 && in_vertices != 4 &&
        in_vertices != 6)
        return direct_reject_with_buffers(si, "geometry input primitive", pop_targets, gb_at);
    if (stage == ST_GS && !passthrough && ((sph[3] >> 24) & 15) != 1 && ((sph[3] >> 24) & 15) != 6 &&
        ((sph[3] >> 24) & 15) != 7)
        return direct_reject_with_buffers(si, "geometry output topology", pop_targets, gb_at);
    if (stage == ST_GS && ((sph[4] & 0xfff) > 256 || (sph[2] >> 24) > 32))
        return direct_reject_with_buffers(si, "geometry output vertices or invocations", pop_targets, gb_at);
    /* Each distinct handle range and sampled image kind needs its own descriptor table. */
    for (k_indexed = 0; k_indexed < texture_count; k_indexed++) {
        const ShTex* t = &textures[k_indexed];
        if (!(t->flags & SNX_TEX_INDEXED)) continue;
        if (t->flags & 3) return direct_reject_with_buffers(si, "indexed multisample texture", pop_targets, gb_at);
        if (indexed_count &&
            (t->type != textures[first_indexed[0]].type || t->array != textures[first_indexed[0]].array ||
             t->shadow != textures[first_indexed[0]].shadow ||
             !!(si->tex_int_mask & (1ull << k_indexed)) != !!(si->tex_int_mask & (1ull << first_indexed[0])) ||
             !!(si->tex_sint_mask & (1ull << k_indexed)) != !!(si->tex_sint_mask & (1ull << first_indexed[0]))))
            return direct_reject_with_buffers(si, "mixed indexed texture kinds", pop_targets, gb_at);
        int table = 0;
        while (table < indexed_count && !shader_same_indexed_table(t, &textures[first_indexed[table]])) table++;
        if (table == indexed_count) {
            if (indexed_count == SH_INDEXED_TABLES)
                return direct_reject_with_buffers(si, "indexed texture table count", pop_targets, gb_at);
            first_indexed[indexed_count++] = k_indexed;
        }
        indexed_table[k_indexed] = table;
    }
    DirectCtx* c = calloc(1, sizeof *c);
    if (!c) {
        free(gb_at);
        free(pop_targets);
        return NULL;
    }
    c->ftz_float = can_preserve;
    c->soft_ftz_on = soft_ftz_on;
    spv_emit_init(&c->emit, (size_t)n * 20 + 128);
    spv_emit_shader_capability(&c->emit);
    if (stage == ST_TCS || stage == ST_TES) spv_emit_capability(&c->emit, 3); /* Tessellation */
    if (stage == ST_GS) spv_emit_capability(&c->emit, 2);                     /* Geometry */
    if (needs_demote || (converge && stage == ST_FS)) {
        spv_emit_capability(&c->emit, 5379); /* DemoteToHelperInvocation */
        spv_emit_demote_extension(&c->emit);
    }
    if (needs_f16) spv_emit_float16_capability(&c->emit);
    if (needs_f64) spv_emit_float64_capability(&c->emit);
    if (needs_clock) spv_emit_shader_clock_extension(&c->emit);
    if (needs_i64) spv_emit_capability(&c->emit, 11); /* Int64 */
    if (needs_subgroup || converge) spv_emit_subgroup_capabilities(&c->emit);
    if (converge) spv_emit_subgroup_arithmetic_capability(&c->emit);
    if (needs_gather_extended) spv_emit_image_gather_extended_capability(&c->emit);
    if (needs_image_query) spv_emit_image_query_capability(&c->emit);
    /* the per-instruction flush to zero tests the absolute value, GLSL.std.450's FAbs, where the module keeps signed
       zeros (float_ftz): without the import its OpExtInst named id 0, which NVIDIA's SPIR-V front end (615.78)
       crashed on, a shader of FADD and FMUL with nothing else from the set */
    if (fp32_flush && soft_ftz_on && can_preserve) needs_glsl = 1;
    if (needs_glsl) c->glsl = spv_emit_glsl450_import(&c->emit);
    u32 fn = spv_emit_id(&c->emit), label = spv_emit_id(&c->emit);
    u32 void_type = spv_emit_type_void(&c->emit);
    c->u32_type = spv_emit_type_u32(&c->emit);
    u32 i32_type = spv_emit_type_i32(&c->emit);
    c->f32_type = spv_emit_type_f32(&c->emit);
    if (needs_f16) c->f16_type = spv_emit_type_f16(&c->emit);
    if (needs_f64) c->f64_type = spv_emit_type_f64(&c->emit);
    u32 i64_type = needs_i64 ? spv_emit_type_int64(&c->emit, 1) : 0,
        u64_type = needs_i64 ? spv_emit_type_int64(&c->emit, 0) : 0;
    c->bool_type = spv_emit_type_bool(&c->emit);
    c->true_value = spv_emit_constant_bool(&c->emit, c->bool_type, 1);
    c->false_value = spv_emit_constant_bool(&c->emit, c->bool_type, 0);
    u32 vec4 = spv_emit_type_vec4(&c->emit, c->f32_type);
    u32 vec4_u32 = spv_emit_type_vec4(&c->emit, c->u32_type);
    u32 vec4_i32 = spv_emit_type_vec4(&c->emit, i32_type);
    u32 vec2 = spv_emit_type_vec2(&c->emit, c->f32_type);
    u32 vec2f16 = needs_f16 ? spv_emit_type_vec2(&c->emit, c->f16_type) : 0;
    u32 vec2_u32 = spv_emit_type_vec2(&c->emit, c->u32_type);
    u32 vec2_i32 = spv_emit_type_vec2(&c->emit, i32_type);
    u32 vec3 = spv_emit_type_vec3(&c->emit, c->f32_type);
    u32 vec3_u32 = spv_emit_type_vec3(&c->emit, c->u32_type);
    u32 vec3_i32 = spv_emit_type_vec3(&c->emit, i32_type);
    u32 fn_type = spv_emit_type_function_void(&c->emit, void_type);
    u32 reg_ptr = spv_emit_type_function_pointer(&c->emit, c->u32_type);
    c->zero = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
    u32 zero_f = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    /* Maximum VS interface: point size, position, 32 outputs, 32 inputs, 18 buffers, 32 samplers, push constants. */
    u32 outputs[8], inputs[32] = {0}, input_vec_type[32] = {0}, input_component_type[32] = {0};
    u32 output_attrs[32] = {0}, position = 0, point_size = 0, layer_output = 0, layer_input = 0;
    u32 viewport_output = 0;
    /* transform feedback: each output word's buffer (0xFF none) and offset, the outputs written to a private vec4,
       and their component outputs (xfb_store) */
    u8 xfb_buf[160], attr_private[32] = {0};
    u16 xfb_off[160] = {0};
    u32 xfb_components[32][4] = {{0}};
    int xfb_on = 0;
    memset(xfb_buf, 0xFF, sizeof xfb_buf);
    u32 clip_output = 0, clip_count = 0;
    u32 depth_output = 0, interfaces[384], count = 0;
    /* the stages between: the levels and vectors of a patch, and the inputs of each vertex */
    u32 tess_outer = 0, tess_inner = 0, patch_vars[30] = {0}, position_in = 0, primitive_id = 0, primitive_id_out = 0,
        tess_coord = 0;
    u32 invocation_id = 0, patch_vertices = 0;
    int depth_fix = position_used && last && stage != ST_FS && stage != ST_CS;
    /* BuiltIn Layer takes the Geometry capability (a geometry program has it; the device's geometryShader) or
       ShaderLayer (its shaderOutputLayer), which a vertex or tessellation program writing the layer needs. A fragment
       program reads it with either. Without one a write is dropped and a read is 0 */
    if (layer_output_used && stage != ST_GS && !g_shader_output_layer) layer_output_used = 0;
    if (layer_input_used && !g_shader_geometry && !g_shader_output_layer) layer_input_used = 0;
    if ((layer_output_used && stage != ST_GS) || (layer_input_used && !g_shader_geometry))
        spv_emit_layer_capability(&c->emit);
    else if (layer_input_used && stage != ST_GS)
        spv_emit_capability(&c->emit, 2); /* Geometry */
    if (clip_used) spv_emit_clip_distance_capability(&c->emit);
    if (stage == ST_FS)
        for (u32 rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15) {
                u32 color_type =
                    (si->rt_int_mask & (1u << rt)) ? (si->rt_sint_mask & (1u << rt)) ? vec4_i32 : vec4_u32 : vec4;
                outputs[count] = spv_emit_output_vec4(&c->emit, color_type, rt);
                interfaces[count] = outputs[count];
                count++;
            }
    if (stage == ST_FS && (sph[19] & 2u)) {
        depth_output = spv_emit_frag_depth_output(&c->emit, c->f32_type);
        interfaces[count++] = depth_output;
    }
    /* Render targets are single-sampled. Only guest SampleMask bit 0 covers the host sample; masks containing only
       higher bits discard it. PIXLD.COVMASK likewise reports the host's single-sample coverage. */
    u32 sample_mask_output = 0; /* SampleMask, an array of one word */
    if (stage == ST_FS && (sph[19] & 1u)) {
        sample_mask_output = spv_emit_interface_variable(&c->emit, 3, c->u32_type, c->u32_type, 1, 20, 0, 0);
        interfaces[count++] = sample_mask_output;
    }
    if (stage != ST_FS && stage != ST_CS) {
        /* a tessellation control program's outputs are arrays of its output vertices */
        u32 per_vertex = stage == ST_TCS ? out_vertices : 0;
        if (stage == ST_VS || (stage != ST_TCS && g_shader_stage_point_size)) {
            if (stage == ST_TES) spv_emit_capability(&c->emit, 23); /* TessellationPointSize */
            if (stage == ST_GS) spv_emit_capability(&c->emit, 24);  /* GeometryPointSize */
            point_size = spv_emit_point_size_output(&c->emit, c->f32_type);
            interfaces[count++] = point_size;
        }
        if (viewport_used) {
            spv_emit_capability(&c->emit, stage == ST_GS ? 57 : 70); /* MultiViewport, ShaderViewportIndex */
            viewport_output = spv_emit_interface_variable(&c->emit, 3, i32_type, c->u32_type, 0, 10, 0, 0);
            interfaces[count++] = viewport_output;
        }
        if (layer_output_used) {
            layer_output = spv_emit_layer_output(&c->emit, i32_type);
            interfaces[count++] = layer_output;
        }
        if (clip_used) {
            u32 i;
            for (i = 0; i < 8; i++)
                if (clip_used & (1u << i)) clip_count = i + 1;
            clip_output = spv_emit_clip_distance_output(&c->emit, c->f32_type, c->u32_type, clip_count);
            interfaces[count++] = clip_output;
        }
        /* transform feedback, in the last stage before rasterization: the buffer and offset of each output word the
           key captures (a word captured twice goes to its first place) */
        if (!per_vertex && !(si->clip_enable & SNX_SH_NOT_LAST))
            for (u32 b = 0; b < 4; b++)
                for (u32 w = 0; w < si->xfb.count[b] && w < 128; w++) {
                    u32 word = si->xfb.layout[b][w];
                    if (word >= 160 || xfb_buf[word] != 0xFF) continue;
                    xfb_buf[word] = (u8)b;
                    xfb_off[word] = (u16)(w * 4);
                    xfb_on = 1;
                }
        if (position_used) {
            position = per_vertex ? spv_emit_interface_variable(&c->emit, 3, vec4, c->u32_type, per_vertex, 0, 0, 0)
                                  : spv_emit_position_output(&c->emit, vec4);
            interfaces[count++] = position;
            /* the position is a built-in: captured only whole, its four words in order in one buffer */
            if (xfb_buf[28] != 0xFF && xfb_buf[29] == xfb_buf[28] && xfb_buf[30] == xfb_buf[28] &&
                xfb_buf[31] == xfb_buf[28] && xfb_off[29] == xfb_off[28] + 4 && xfb_off[30] == xfb_off[28] + 8 &&
                xfb_off[31] == xfb_off[28] + 12)
                spv_emit_xfb_capture(&c->emit, position, xfb_buf[28], si->xfb.stride[xfb_buf[28]], xfb_off[28]);
        }
        for (u32 i = 0; i < 32; i++)
            if (outputs_used[i]) {
                u32 k;
                int captured = 0;
                for (k = 0; k < 4; k++) captured |= xfb_buf[32 + 4 * i + k] != 0xFF;
                if (captured) {
                    /* a captured output is written to a private vec4 and copied out a component a variable
                       (xfb_store), so that each captured component has an offset of its own: the next stage's vec4
                       input takes the four at its location */
                    output_attrs[i] = spv_emit_private_variable(&c->emit, vec4);
                    interfaces[count++] = output_attrs[i];
                    attr_private[i] = 1;
                    for (k = 0; k < 4; k++) {
                        u32 word = 32 + 4 * i + k;
                        xfb_components[i][k] = spv_emit_output_component(&c->emit, c->f32_type, i, k);
                        interfaces[count++] = xfb_components[i][k];
                        if (xfb_buf[word] != 0xFF)
                            spv_emit_xfb_capture(&c->emit, xfb_components[i][k], xfb_buf[word],
                                                 si->xfb.stride[xfb_buf[word]], xfb_off[word]);
                    }
                    continue;
                }
                output_attrs[i] = per_vertex ? spv_emit_interface_variable(&c->emit, 3, vec4, c->u32_type, per_vertex,
                                                                           SPV_NOT_BUILTIN, i, 0)
                                             : spv_emit_output_vec4(&c->emit, vec4, i);
                interfaces[count++] = output_attrs[i];
            }
    }
    if (stage == ST_TCS || stage == ST_TES) {
        /* a patch's levels and vectors: the control program's outputs, the evaluation program's inputs */
        u32 storage = stage == ST_TCS ? 3 : 1, i;
        if (levels_used & 1) {
            tess_outer = spv_emit_interface_variable(&c->emit, storage, c->f32_type, c->u32_type, 4, 11, 0, 1);
            interfaces[count++] = tess_outer;
        }
        if (levels_used & 2) {
            tess_inner = spv_emit_interface_variable(&c->emit, storage, c->f32_type, c->u32_type, 2, 12, 0, 1);
            interfaces[count++] = tess_inner;
        }
        for (i = 0; i < 30; i++)
            if (patch_used[i]) {
                patch_vars[i] =
                    spv_emit_interface_variable(&c->emit, storage, vec4, c->u32_type, 0, SPV_NOT_BUILTIN, i, 1);
                interfaces[count++] = patch_vars[i];
            }
        patch_vertices = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 0, 14, 0, 0);
        interfaces[count++] = patch_vertices;
    }
    /* PIXLD's sample (SampleId, which shades each sample, with SampleRateShading) and coverage (SampleMask in) */
    u32 sample_id = 0, coverage = 0;
    if (needs_sample_id) {
        spv_emit_capability(&c->emit, 35); /* SampleRateShading */
        sample_id = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 18);
        interfaces[count++] = sample_id;
    }
    if (needs_coverage) {
        coverage = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 1, 20, 0, 0);
        spv_emit_flat(&c->emit, coverage); /* an integer fragment input */
        interfaces[count++] = coverage;
    }
    /* Fragment PrimitiveId can declare Geometry or Tessellation. Without either feature the read is zero. */
    if (stage == ST_FS && primitive_id_used && (g_shader_geometry || g_shader_tessellation)) {
        if (!g_shader_geometry || !layer_input_used) spv_emit_capability(&c->emit, g_shader_geometry ? 2 : 3);
        primitive_id = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 0, 7, 0, 0);
        spv_emit_flat(&c->emit, primitive_id);
        interfaces[count++] = primitive_id;
    }
    if (between) {
        if (position_in_used) {
            position_in = spv_emit_interface_variable(&c->emit, 1, vec4, c->u32_type, in_vertices, 0, 0, 0);
            interfaces[count++] = position_in;
        }
        if (primitive_id_used || stage == ST_GS) {
            primitive_id = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 0, 7, 0, 0);
            interfaces[count++] = primitive_id;
        }
        if (stage == ST_GS) {
            /* Vulkan requires a geometry shader to write PrimitiveId for a later fragment shader to read it. */
            primitive_id_out = spv_emit_interface_variable(&c->emit, 3, c->u32_type, c->u32_type, 0, 7, 0, 0);
            interfaces[count++] = primitive_id_out;
        }
        if (tess_coord_used) {
            tess_coord = spv_emit_interface_variable(&c->emit, 1, vec3, c->u32_type, 0, 13, 0, 0);
            interfaces[count++] = tess_coord;
        }
        if (invocation_used) {
            invocation_id = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 0, 8, 0, 0);
            interfaces[count++] = invocation_id;
        }
    }
    for (u32 i = 0; i < 32; i++) {
        if (!inputs_used[i]) continue;
        if (stage == ST_FS && (si->in_unfed_mask >> i & 1)) continue; /* read as 0, 0, 0, 1 (ShaderInfo) */
        u32 interpolation = 0;
        if (stage == ST_FS) {
            u32 mode_byte = (sph[6 + i / 4] >> (i % 4 * 8)) & 0xFF; /* the header's byte 0x18 + i */
            u32 k = 0;
            while (k < 3 && !((mode_byte >> (k * 2)) & 3)) k++;
            u32 mode = (mode_byte >> (k * 2)) & 3;
            interpolation = mode == 1 ? 1 : mode == 3 ? 2 : 0;
        }
        u32 component_type = c->f32_type;
        u32 vector_type = vec4;
        if (stage == ST_VS && (si->attr_int_mask & (1u << i))) {
            int signed_input = (si->attr_sint_mask & (1u << i)) != 0;
            component_type = signed_input ? i32_type : c->u32_type;
            vector_type = signed_input ? vec4_i32 : vec4_u32;
        }
        input_vec_type[i] = vector_type;
        input_component_type[i] = component_type;
        inputs[i] = between ? spv_emit_interface_variable(&c->emit, 1, vector_type, c->u32_type, in_vertices,
                                                          SPV_NOT_BUILTIN, i, 0)
                            : spv_emit_input_vec4(&c->emit, vector_type, i, interpolation);
        interfaces[count++] = inputs[i];
    }
    u32 fragcoord = 0;
    if (layer_input_used) {
        layer_input = spv_emit_builtin_input_u32(&c->emit, i32_type, 9);
        interfaces[count++] = layer_input;
    }
    u32 frontface = 0;
    if (needs_frontface) {
        frontface = spv_emit_builtin_input_u32(&c->emit, c->bool_type, 17);
        interfaces[count++] = frontface;
    }
    /* a[0x2e0] and a[0x2e4] are the point sprite's S and T */
    u32 point_coord = 0;
    if (needs_point_coord) {
        point_coord = spv_emit_interface_variable(&c->emit, 1, vec2, c->u32_type, 0, 16, 0, 0);
        interfaces[count++] = point_coord;
    }
    u32 instance_index = 0, vertex_index = 0;
    u32 local_input = 0, group_input = 0;
    if (needs_local_id) {
        local_input = spv_emit_builtin_input_vec3(&c->emit, vec3_u32, 27);
        interfaces[count++] = local_input;
    }
    if (needs_group_id) {
        group_input = spv_emit_builtin_input_vec3(&c->emit, vec3_u32, 26);
        interfaces[count++] = group_input;
    }
    if (needs_subgroup) {
        c->subgroup_lane_input = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 41);
        if (stage == ST_FS) spv_emit_flat(&c->emit, c->subgroup_lane_input);
        interfaces[count++] = c->subgroup_lane_input;
    }
    if (needs_instance) {
        instance_index = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 43);
        vertex_index = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 42);
        interfaces[count++] = instance_index;
        interfaces[count++] = vertex_index;
    }
    if (needs_fragcoord) {
        fragcoord = spv_emit_fragcoord_input(&c->emit, vec4);
        interfaces[count++] = fragcoord;
    }
    if (needs_scale || needs_texel_scale) {
        c->scale_buffer =
            spv_emit_uniform_vec4_array(&c->emit, vec4, c->f32_type, c->u32_type, 1, SNX_VKB_RS, 1, &c->scale_pointer);
        interfaces[count++] = c->scale_buffer;
    }
    for (u32 i = 0; i < 18; i++) {
        if (!cbuf_used[i]) continue;
        c->cbuf[i] = spv_emit_uniform_vec4_array(&c->emit, vec4_u32, c->u32_type, c->u32_type, shader_vk_set(stage), i,
                                                 4096, &c->cbuf_pointer[i]);
        interfaces[count++] = c->cbuf[i];
    }
    if (indexed_count && textures[first_indexed[0]].shadow) {
        c->tex_func_buffer =
            spv_emit_uniform_vec4_array(&c->emit, vec4_u32, c->u32_type, c->u32_type, shader_vk_set(stage),
                                        SNX_VKB_TEXFUNCS, SNX_TEX_ARRAY / 4, &c->tex_func_pointer);
        interfaces[count++] = c->tex_func_buffer;
    }
    if (indexed_count) {
        c->tex_map_buffer =
            spv_emit_uniform_vec4_array(&c->emit, vec4_u32, c->u32_type, c->u32_type, shader_vk_set(stage),
                                        SNX_VKB_TEXMAP, SH_INDEXED_TABLES * SNX_TEX_ARRAY / 4, &c->tex_map_pointer);
        interfaces[count++] = c->tex_map_buffer;
    }
    u32 texture_scalar[SH_MAXTEX], texture_vec4[SH_MAXTEX];
    for (int i = 0; i < texture_count; i++) {
        u32 scalar = (textures[i].flags & 1)            ? c->u32_type
                     : (si->tex_int_mask & (1ull << i)) ? (si->tex_sint_mask & (1ull << i)) ? i32_type : c->u32_type
                                                        : c->f32_type;
        texture_scalar[i] = scalar;
        texture_vec4[i] = scalar == i32_type ? vec4_i32 : scalar == c->u32_type ? vec4_u32 : vec4;
        if (textures[i].flags & SNX_TEX_INDEXED) {
            int table = indexed_table[i];
            c->tex_table[i] = (u8)table;
            if (i == first_indexed[0]) {
                c->texture_array =
                    spv_emit_sampler_image_array(&c->emit, scalar, c->u32_type, shader_vk_set(stage), SNX_VKB_TEXARRAY,
                                                 textures[i].type == 0 ? 1 : textures[i].type, textures[i].array, 0,
                                                 SNX_TEX_ARRAY, &c->sampled_type[i]);
                interfaces[count++] = c->texture_array;
                if (textures[i].shadow) {
                    c->depth_array =
                        spv_emit_sampler_image_array(&c->emit, c->f32_type, c->u32_type, shader_vk_set(stage),
                                                     SNX_VKB_TEXDEPTH, textures[i].type == 0 ? 1 : textures[i].type,
                                                     textures[i].array, 1, SNX_TEX_ARRAY, &c->depth_type);
                    interfaces[count++] = c->depth_array;
                }
            } else
                c->sampled_type[i] = c->sampled_type[first_indexed[0]];
            c->sampler[i] = c->texture_array;
            continue;
        }
        c->sampler[i] = spv_emit_sampler_image(&c->emit, scalar, shader_vk_set(stage), SNX_VKB_TEX + (u32)i,
                                               textures[i].type == 0 ? 1 : textures[i].type, textures[i].array,
                                               textures[i].shadow && dc_color_func(si, i) < 0,
                                               !!(textures[i].flags & 2), &c->sampled_type[i]);
        interfaces[count++] = c->sampler[i];
    }
    /* Formatted stores follow the bound view's format. Only loads and atomics
       require R32ui; declaring stores R32ui loses the other components. */
    for (int i = 0; i < image_info.nimg; i++) {
        int integer = image_info.img[i].kind != 0, buffer = image_info.img[i].type == 1;
        c->image[i] = spv_emit_storage_image(
            &c->emit, image_info.img[i].kind == 3 ? i32_type : integer ? c->u32_type : c->f32_type,
            shader_vk_set(stage),
            (buffer ? SNX_VKB_IMGBUF : SNX_VKB_IMG) + (u32)i, buffer ? 5 : image_info.img[i].type == 5 ? 2 : 1,
            image_info.img[i].type == 4,
            image_info.img[i].kind == 1 ? 33 : 0, &c->image_type[i]);
        interfaces[count++] = c->image[i];
    }
    if (ngbuf) {
        u32 array = spv_emit_type_runtime_array(&c->emit, c->u32_type, 4);
        u32 structure = spv_emit_type_struct_one(&c->emit, array);
        c->gbuf_pointer = spv_emit_type_storage_pointer(&c->emit, c->u32_type);
        for (int i = 0; i < ngbuf; i++) {
            c->gbuf[i] = spv_emit_storage_buffer(&c->emit, structure, shader_vk_set(stage), SNX_VKB_SSBO + (u32)i);
            interfaces[count++] = c->gbuf[i];
        }
        if (stage != ST_CS) {
            c->smap = spv_emit_storage_buffer(&c->emit, structure, shader_vk_set(stage), SNX_VKB_SMAP);
            interfaces[count++] = c->smap;
        }
    }
    u32 shared_array = 0, shared_element_pointer = 0, shared_words = 0;
    if (needs_shared) {
        shared_words = ((u32)si->shared_size + 3) / 4;
        u32 array_type = spv_emit_type_array_u32(&c->emit, c->u32_type, shared_words);
        u32 pointer_type = spv_emit_type_workgroup_pointer(&c->emit, array_type);
        shared_element_pointer = spv_emit_type_workgroup_pointer(&c->emit, c->u32_type);
        shared_array = spv_emit_workgroup_variable(&c->emit, pointer_type);
        interfaces[count++] = shared_array;
    }
    u32 push = 0, push_pointer = 0;
    if ((stage == ST_FS && ((target & 15) || needs_point_coord || needs_texel_scale)) || depth_fix || needs_instance) {
        /* the fragment stage's uvec4: its scaled textures (scaled_coords) */
        push = spv_emit_push_vec4(&c->emit, vec4, c->f32_type, stage == ST_FS ? 16 : 0,
                                  stage == ST_FS && needs_texel_scale ? vec4_u32 : 0, &push_pointer);
        interfaces[count++] = push;
    }
    spv_emit_entry_str(&c->emit, (SpvShaderStage)stage, fn, "main", interfaces, count); /* ST_ is SPIR-V's model */
    if (xfb_on) {
        spv_emit_capability(&c->emit, 53);               /* TransformFeedback */
        spv_emit_execution_mode(&c->emit, fn, 11, 0, 0); /* Xfb */
    } else { /* SWR: DLSS captures vertex positions for its motion vectors (gpu/vk/dlss.c), when the game doesn't use
                transform feedback itself */
        int vk_dlss_xfb_wanted(void);
        if (stage == ST_VS && position && vk_dlss_xfb_wanted()) spv_emit_xfb_position(&c->emit, fn, position);
    }
    if (stage == ST_TCS) spv_emit_execution_mode(&c->emit, fn, 26, 1, out_vertices);    /* OutputVertices */
    if (stage == ST_TES) {
        /* 0xC8: the domain (isolines, triangles, quads), the spacing (equal, fractional odd and even) and the output
           (points, lines, triangles clockwise and counterclockwise) */
        u32 domain = si->rt_int_mask & 3, spacing = (si->rt_int_mask >> 4) & 3, output = (si->rt_int_mask >> 8) & 3;
        spv_emit_execution_mode(&c->emit, fn, domain == 0 ? 25 : domain == 1 ? 22 : 24, 0, 0);
        spv_emit_execution_mode(&c->emit, fn, spacing == 0 ? 1 : spacing == 1 ? 3 : 2, 0, 0);
        spv_emit_execution_mode(&c->emit, fn, output == 2 ? 4 : 5, 0, 0); /* VertexOrderCw, VertexOrderCcw */
        if (output == 0) spv_emit_execution_mode(&c->emit, fn, 10, 0, 0); /* PointMode */
    }
    if (stage == ST_GS) {
        /* the input primitive, the SPH's output topology and vertices, and its threads per input primitive */
        u32 topology = passthrough ? (in_vertices == 1                       ? 1
                                      : in_vertices == 2 || in_vertices == 4 ? 6
                                                                             : 7)
                                   : (sph[3] >> 24) & 15;
        u32 invocations = sph[2] >> 24;
        spv_emit_execution_mode(&c->emit, fn,
                                in_vertices == 1   ? 19
                                : in_vertices == 2 ? 20
                                : in_vertices == 4 ? 21
                                : in_vertices == 6 ? 23
                                                   : 22,
                                0, 0);
        spv_emit_execution_mode(&c->emit, fn, topology == 1 ? 27 : topology == 6 ? 28 : 29, 0, 0);
        spv_emit_execution_mode(&c->emit, fn, 26, 1,
                                passthrough        ? (in_vertices == 4   ? 2
                                                      : in_vertices == 6 ? 3
                                                                         : in_vertices)
                                : (sph[4] & 0xfff) ? sph[4] & 0xfff
                                                   : 1);
        spv_emit_execution_mode(&c->emit, fn, 0, 1, invocations ? invocations : 1); /* Invocations */
    }
    if (stage == ST_CS) spv_emit_local_size(&c->emit, fn, si->cs_local[0], si->cs_local[1], si->cs_local[2]);
    if (stage == ST_FS) spv_emit_fragment_origin_upper_left(&c->emit, fn);
    if (stage == ST_FS && si->early_fragment_tests) spv_emit_execution_mode(&c->emit, fn, 9, 0, 0); /* EarlyFragmentTests */
    if (depth_output) spv_emit_fragment_depth_replacing(&c->emit, fn);
    spv_emit_float_controls(&c->emit, fn, fp32_flush && !fp32_preserve && can_flush == SH_FTZ_MODE, can_preserve,
                            needs_f64 && can_preserve_f64);
    spv_emit_function_void(&c->emit, void_type, fn, fn_type);
    spv_emit_label(&c->emit, label);
    c->emit.block_end = var_flush;
    c->emit.block_end_user = c;
    u32 local_array = 0, local_element_pointer = 0, local_words = 0;
    if (needs_local) {
        u32 bytes = (sph[1] & 0xffffffu) + (sph[2] & 0xffffffu);
        local_words = 1024;
        if (bytes) {
            local_words = 1;
            while (local_words < 65536 && local_words * 4 < bytes) local_words <<= 1;
        }
        u32 array_type = spv_emit_type_array_u32(&c->emit, c->u32_type, local_words);
        u32 array_pointer = spv_emit_type_function_pointer(&c->emit, array_type);
        local_element_pointer = spv_emit_type_function_pointer(&c->emit, c->u32_type);
        local_array = spv_emit_function_variable(&c->emit, array_pointer);
    }
    for (u32 r = 0; r < 255; r++)
        if (used[r]) c->reg[r] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    /* On the per-instruction FTZ path, the registers only FTZ float instructions write hold flushed values (from zero,
       their first value): their reads need no flush. Anything else that may write a register taints it, its
       destinations taken wide (d to d+3, and the second field texture instructions write, bits 28 to 35) */
    if (soft_ftz_on) {
        for (u32 i = 11; i < n; i++) {
            if (!IDX_OK(i)) continue;
            if (ftz_writer(insn[i])) continue;
            for (u32 r = 0; r < 255; r++)
                if (may_write(insn[i], r)) c->tainted[r] = 1;
        }
        c->taint_known = 1;
    }
    for (k_indexed = 0; k_indexed < texture_count; k_indexed++)
        if (textures[k_indexed].flags & SNX_TEX_INDEXED)
            c->tex_index[k_indexed] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    u32 pred_ptr = spv_emit_type_function_pointer(&c->emit, c->bool_type);
    for (u32 p = 0; p < 7; p++)
        if (pred_used[p]) c->pred[p] = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    c->cc_c = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    c->cc_o = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    c->cc_z = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    c->cc_s = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    u32 atomic_old_var = needs_atomic_cas ? spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero) : 0;
    u32 stack_ptr = 0, stack[32] = {0};
    if (dynamic_stack) {
        stack_ptr = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
        for (u32 k = 0; k < 32; k++) stack[k] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    }
    u32 call_ptr = 0, call_stack[32] = {0};
    if (has_calls) {
        call_ptr = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
        for (u32 k = 0; k < 32; k++) call_stack[k] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    }
    u32 pc = 0, loop_header = 0, loop_dispatch = 0, dispatch_default = 0, dispatch_merge = 0, loop_continue = 0,
        loop_merge = 0;
    u32 *case_labels = NULL, *case_literals = NULL;
    u8* block_start = NULL;
    size_t case_count = 0;
    ShStructure* structure = NULL;
    StructEmit se = {0};
    if (has_flow) {
        /* one case for each instruction, and the case of lanes waiting to reconverge */
        case_labels = calloc(n + 1, sizeof *case_labels + sizeof *case_literals + 1);
        if (!case_labels) abort();
        case_literals = case_labels + n + 1;
        block_start = (u8*)(case_literals + n + 1);
        block_start[11] = 1;
        int unknown_brx = 0;
        for (u32 i = 11; i < n; i++) {
            if (!IDX_OK(i)) continue;
            int id = decode(insn[i])->id;
            if (id == O_BRX) {
                u32 targets[64];
                int count = shader_brx_targets(insn, i, n, targets, 64);
                if (count < 0)
                    unknown_brx = 1;
                else
                    for (int k = 0; k < count; k++) block_start[targets[k]] = 1;
            }
            if (id == O_BRA || id == O_CAL || id == O_PRET || id == O_PEXIT || id == O_SSY || id == O_PBK ||
                id == O_PCNT)
                block_start[bra_target(insn[i], i)] = 1;
            if (!dynamic_stack && (id == O_SYNC || id == O_BRK || id == O_CONT)) block_start[pop_targets[i]] = 1;
            if (id == O_BRA || id == O_CAL || id == O_RET || id == O_BRX || id == O_SYNC || id == O_BRK ||
                id == O_CONT || id == O_EXIT) {
                u32 next = next_instruction(i, n);
                if (next < n) block_start[next] = 1;
            }
        }
        /* Unknown jump tables retain a case for every possible instruction target. */
        if (unknown_brx)
            for (u32 i = 11; i < n; i++)
                if (IDX_OK(i)) block_start[i] = 1;
        /* The program's own branches and loops as structured constructs, where its control flow allows them: the
           dispatch loop below makes every register live around one big loop, and costs a driver as much to compile
           again as the program's code. Lanes that must reconverge for subgroup operations, barriers or derivatives
           take them where the device reconverges them at each construct's merge (MaximallyReconvergesKHR), as the
           hardware does at its SYNC targets, and keep the loop otherwise, which runs the lowest pc first */
        if (!dynamic_stack && !has_calls && (!converge || g_shader_maximal_reconvergence) && !unknown_brx)
            structure = shader_structure(insn, n, block_start, pop_targets);
        if (structure && converge) { /* the guarantee those lanes need */
            spv_emit_extension(&c->emit, "SPV_KHR_maximal_reconvergence");
            spv_emit_execution_mode(&c->emit, fn, 6023, 0, 0); /* MaximallyReconvergesKHR */
        }
        if (structure) {
            se.st = structure;
            se.open = 1; /* the function's first block */
            se.label = calloc(4 * (size_t)structure->nscopes + 2 * (size_t)structure->ntokens,
                              sizeof *se.label + 1);
            if (!se.label) abort();
            se.used = (u8*)(se.label + 4 * (size_t)structure->nscopes + 2 * (size_t)structure->ntokens);
        }
    }
    /* every OpVariable comes before the first block's other instructions, the clip distances' stores included */
    if (has_flow && !structure)
        pc = spv_emit_function_variable_init(&c->emit, reg_ptr, spv_emit_constant_u32(&c->emit, c->u32_type, 11));
    if (structure && structure->uses_flag) se.flag = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    c->discarded = has_flow && stage == ST_FS ? spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value) : 0;
    for (clip = 0; clip < clip_count; clip++) {
        u32 index = spv_emit_constant_u32(&c->emit, c->u32_type, clip);
        u32 ptr = spv_emit_output_element_pointer(&c->emit, c->f32_type, clip_output, index);
        spv_emit_store(&c->emit, ptr, zero_f);
    }
    if (has_flow && !structure) {
        loop_header = spv_emit_id(&c->emit);
        loop_dispatch = spv_emit_id(&c->emit);
        dispatch_default = spv_emit_id(&c->emit);
        dispatch_merge = spv_emit_id(&c->emit);
        loop_continue = spv_emit_id(&c->emit);
        loop_merge = spv_emit_id(&c->emit);
        for (u32 i = 11; i < n; i++)
            if (IDX_OK(i) && block_start[i]) {
                case_literals[case_count] = i;
                case_labels[case_count++] = spv_emit_id(&c->emit);
            }
        if (converge) {
            wait_label = spv_emit_id(&c->emit);
            case_literals[case_count] = UINT32_MAX - 1;
            case_labels[case_count++] = wait_label;
        }
        if (stage == ST_VS || stage == ST_TES || stage == ST_GS) {
            u32 one_f = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
            if (point_size) spv_emit_store(&c->emit, point_size, one_f);
            u32 components[4] = {zero_f, zero_f, zero_f, one_f};
            u32 empty = spv_emit_constant_vec4(&c->emit, vec4, components);
            if (position) spv_emit_store(&c->emit, position, empty);
            for (u32 i = 0; i < 32; i++)
                if (output_attrs[i]) spv_emit_store(&c->emit, output_attrs[i], empty);
        }
        spv_emit_branch(&c->emit, loop_header);
        spv_emit_label(&c->emit, loop_header);
        spv_emit_loop_merge(&c->emit, loop_merge, loop_continue);
        spv_emit_branch(&c->emit, loop_dispatch);
        spv_emit_label(&c->emit, loop_dispatch);
        u32 selector = spv_emit_load(&c->emit, c->u32_type, pc);
        if (converge) {
            u32 lowest = spv_emit_subgroup_umin_u32(&c->emit, c->u32_type, selector);
            /* Helper lanes may be inactive for subgroup arithmetic. Their minimum may be undefined; they must
               continue at their own pc so they can still provide derivative values to their quad. */
            if (stage == ST_FS)
                lowest = spv_emit_select(&c->emit, c->u32_type, spv_emit_is_helper(&c->emit, c->bool_type), selector,
                                         lowest);
            lowest = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, selector, lowest),
                                     selector, lowest);
            selector =
                spv_emit_select(&c->emit, c->u32_type, spv_emit_i_equal(&c->emit, c->bool_type, selector, lowest),
                                selector, spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX - 1));
        }
        spv_emit_selection_merge(&c->emit, dispatch_merge);
        spv_emit_switch_u32(&c->emit, selector, dispatch_default, case_literals, case_labels, case_count);
    }
    if ((stage == ST_VS || stage == ST_TES || stage == ST_GS) && (!has_flow || structure)) {
        u32 one_f = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
        if (point_size) spv_emit_store(&c->emit, point_size, one_f);
        u32 components[4] = {zero_f, zero_f, zero_f, one_f};
        u32 empty = spv_emit_constant_vec4(&c->emit, vec4, components);
        if (position) spv_emit_store(&c->emit, position, empty);
        for (u32 i = 0; i < 32; i++)
            if (output_attrs[i]) spv_emit_store(&c->emit, output_attrs[i], empty);
    }

    size_t case_index = 0;
    for (u32 i = 11; structure || i < n; i++) {
        /* structured: the blocks in the order their constructs need, the constructs written between them */
        while (structure && i >= se.body_end) {
            int step = structure_step(c, &se);
            if (step == SE_BODY) {
                i = se.body_start;
                break;
            }
            if (step == SE_DONE) {
                i = n;
                break;
            }
            /* control runs off the program's end, which ends the dispatch loop: what follows it */
            if (c->discarded) {
                u32 kill_label = spv_emit_id(&c->emit), kept = spv_emit_id(&c->emit);
                u32 discard = spv_emit_load(&c->emit, c->bool_type, c->discarded);
                spv_emit_selection_merge(&c->emit, kept);
                spv_emit_branch_conditional(&c->emit, discard, kill_label, kept);
                spv_emit_label(&c->emit, kill_label);
                spv_emit_kill(&c->emit);
                spv_emit_label(&c->emit, kept);
            }
            if (passthrough)
                geometry_passthrough(c, si, in_vertices, vec4, position, position_in, output_attrs, inputs, point_size,
                                     layer_output, i32_type, clip_output, clip_count, primitive_id, primitive_id_out,
                                     depth_fix, push, push_pointer);
            if (xfb_on && stage != ST_GS) xfb_store(c, vec4, output_attrs, attr_private, xfb_components);
            if (depth_fix && stage != ST_GS) vertex_depth_fix(c, position, push, push_pointer, vec4);
            spv_emit_return(&c->emit);
            se.open = 0;
        }
        if (i >= n) break;
        if (!IDX_OK(i)) continue;
        if (c->swapped) { /* the array element an indexed texture took for the instruction before */
            c->sampler[c->swapped - 1] = c->texture_array;
            c->swapped = 0;
        }
        if (has_flow && !structure && block_start[i]) { spv_emit_label(&c->emit, case_labels[case_index++]); }
        if (has_flow && !structure && (next_instruction(i, n) >= n || block_start[next_instruction(i, n)])) {
            u32 following = spv_emit_constant_u32(&c->emit, c->u32_type, next_instruction(i, n));
            spv_emit_store(&c->emit, pc, following);
        }
        u64 w = insn[i];
        int op = decode(w)->id;
        u32 d = B(w, 0, 8), a = B(w, 8, 8), b = B(w, 20, 8);
        u32 guard_merge = 0;
        c->guard = 0; /* the instruction before may have ended its translation early */
        if (structure && (op == O_BRA || op == O_SYNC || op == O_BRK || op == O_CONT)) {
            /* the branch that ends the block: the structure's SHS_IF takes its condition */
            c->branch_cond = 0;
            if (!flow_always(w)) {
                c->branch_cond = pred_read(c, B(w, 16, 3), B(w, 19, 1));
                if (B(w, 0, 5) != 15) c->branch_cond = cc_and(c, c->branch_cond, cc_test(c, B(w, 0, 5)));
            }
            continue;
        }
        int skip_helper = stage == ST_FS && helper_side_effect(op);
        if (op != O_SSY && op != O_PBK && op != O_PCNT && op != O_PEXIT && op != O_CAL &&
            (B(w, 16, 3) != 7 || B(w, 19, 1) || (flow_has_cc(op) && B(w, 0, 5) != 15) || skip_helper)) {
            u32 condition = pred_read(c, B(w, 16, 3), B(w, 19, 1));
            if (flow_has_cc(op) && B(w, 0, 5) != 15) condition = cc_and(c, condition, cc_test(c, B(w, 0, 5)));
            if (skip_helper) condition = cc_and(c, condition, cc_not(c, lane_dead(c)));
            if (pure_op(op) && !skip_helper && !(flow_has_cc(op) && B(w, 0, 5) != 15))
                c->guard = condition;
            else {
                u32 execute = spv_emit_id(&c->emit);
                guard_merge = spv_emit_id(&c->emit);
                spv_emit_selection_merge(&c->emit, guard_merge);
                spv_emit_branch_conditional(&c->emit, condition, execute, guard_merge);
                spv_emit_label(&c->emit, execute);
            }
        }
        if (op == O_CAL || op == O_PRET) {
            /* CAL pushes the instruction after it and jumps. PRET only pushes its target, where a later RET returns */
            u32 sp = spv_emit_load(&c->emit, c->u32_type, call_ptr);
            u32 room = spv_emit_u_less(&c->emit, c->bool_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, 32));
            u32 address =
                spv_emit_constant_u32(&c->emit, c->u32_type, op == O_PRET ? bra_target(w, i) : next_instruction(i, n));
            for (u32 k = 0; k < 32; k++) {
                u32 at = spv_emit_i_equal(&c->emit, c->bool_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, k));
                u32 old = spv_emit_load(&c->emit, c->u32_type, call_stack[k]);
                spv_emit_store(&c->emit, call_stack[k], spv_emit_select(&c->emit, c->u32_type, at, address, old));
            }
            spv_emit_store(
                &c->emit, call_ptr,
                spv_emit_iadd_u32(&c->emit, c->u32_type, sp,
                                  spv_emit_select(&c->emit, c->u32_type, room,
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero)));
            if (op == O_CAL)
                spv_emit_store(&c->emit, pc,
                               spv_emit_select(&c->emit, c->u32_type, room,
                                               spv_emit_constant_u32(&c->emit, c->u32_type, bra_target(w, i)),
                                               spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX)));
        } else if (op == O_RET) {
            u32 sp = spv_emit_load(&c->emit, c->u32_type, call_ptr);
            u32 has_entry = spv_emit_u_less(&c->emit, c->bool_type, c->zero, sp);
            u32 index = spv_emit_isub_u32(&c->emit, c->u32_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
            u32 destination = spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX);
            for (u32 k = 0; k < 32; k++) {
                u32 at =
                    spv_emit_i_equal(&c->emit, c->bool_type, index, spv_emit_constant_u32(&c->emit, c->u32_type, k));
                u32 old = spv_emit_load(&c->emit, c->u32_type, call_stack[k]);
                destination = spv_emit_select(&c->emit, c->u32_type, at, old, destination);
            }
            spv_emit_store(&c->emit, call_ptr, spv_emit_select(&c->emit, c->u32_type, has_entry, index, c->zero));
            spv_emit_store(&c->emit, pc, destination);
        } else if (dynamic_stack && (op == O_SSY || op == O_PBK || op == O_PCNT || op == O_PEXIT)) {
            u32 sp = spv_emit_load(&c->emit, c->u32_type, stack_ptr);
            u32 token = spv_emit_constant_u32(&c->emit, c->u32_type, bra_target(w, i) | (u32)CRS_KIND(op) << 16);
            for (u32 k = 0; k < 32; k++) {
                u32 at = spv_emit_i_equal(&c->emit, c->bool_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, k));
                u32 old = spv_emit_load(&c->emit, c->u32_type, stack[k]);
                spv_emit_store(&c->emit, stack[k], spv_emit_select(&c->emit, c->u32_type, at, token, old));
            }
            u32 room = spv_emit_u_less(&c->emit, c->bool_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, 32));
            u32 inc =
                spv_emit_select(&c->emit, c->u32_type, room, spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero);
            spv_emit_store(&c->emit, stack_ptr, spv_emit_iadd_u32(&c->emit, c->u32_type, sp, inc));
        } else if (dynamic_stack && (op == O_SYNC || op == O_BRK || op == O_CONT)) {
            u32 found, destination, new_sp;
            dynamic_pop(c, stack_ptr, stack, CRS_KIND(op), spv_emit_load(&c->emit, c->u32_type, pc), &found,
                        &destination, &new_sp);
            spv_emit_store(&c->emit, pc, destination);
            spv_emit_store(&c->emit, stack_ptr, new_sp);
        } else if (op == O_BRA || op == O_SYNC || op == O_BRK || op == O_CONT) {
            u32 target_index = op == O_BRA ? bra_target(w, i) : pop_targets ? pop_targets[i] : 0;
            u32 destination = spv_emit_constant_u32(&c->emit, c->u32_type, target_index);
            spv_emit_store(&c->emit, pc, destination);
        } else if (op == O_BRX) {
            u32 offset = spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)(B(w, 20, 24) << 8) >> 8));
            u32 signed_index =
                spv_emit_iadd_u32(&c->emit, i32_type, reg_read_as(c, a, i32_type), offset);
            u32 words =
                spv_emit_sdiv_i32(&c->emit, i32_type, signed_index, spv_emit_constant_u32(&c->emit, i32_type, 8));
            u32 target = spv_emit_iadd_u32(&c->emit, c->u32_type, spv_emit_bitcast(&c->emit, c->u32_type, words),
                                           spv_emit_constant_u32(&c->emit, c->u32_type, i + 1));
            u32 after_ten =
                spv_emit_isub_u32(&c->emit, c->u32_type, target, spv_emit_constant_u32(&c->emit, c->u32_type, 10));
            u32 aligned = spv_emit_i_equal(&c->emit, c->bool_type,
                                           spv_emit_bitwise_and_u32(&c->emit, c->u32_type, after_ten,
                                                                    spv_emit_constant_u32(&c->emit, c->u32_type, 3)),
                                           c->zero);
            u32 in_range = spv_emit_bool_not(
                &c->emit, c->bool_type,
                spv_emit_u_less(&c->emit, c->bool_type, target, spv_emit_constant_u32(&c->emit, c->u32_type, 10)));
            u32 needs_skip = spv_emit_bool_binary(&c->emit, c->bool_type, 0, aligned, in_range);
            target = spv_emit_iadd_u32(&c->emit, c->u32_type, target,
                                       spv_emit_select(&c->emit, c->u32_type, needs_skip,
                                                       spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero));
            if (structure)
                c->branch_target = target; /* the structure's SHS_SWITCH */
            else
                spv_emit_store(&c->emit, pc, target);
        } else if (op == O_S2R) {
            u32 sr = B(w, 20, 8);
            u32 value = c->zero;
            if (stage == ST_CS && ((sr >= 0x21 && sr <= 0x23) || (sr >= 0x25 && sr <= 0x27))) {
                u32 local = sr <= 0x23;
                u32 group = spv_emit_load(&c->emit, vec3_u32, local ? local_input : group_input);
                value = spv_emit_vec4_extract(&c->emit, c->u32_type, group, local ? sr - 0x21 : sr - 0x25);
            } else if (sr == 0 || (sr >= 0x38 && sr <= 0x3c)) {
                /* SR_LANEID, and the EQ, LT, LE, GT and GE lane masks of a 32-lane warp */
                u32 lane = lane_is_invocation ? spv_emit_load(&c->emit, c->u32_type, invocation_id)
                           : lane_model       ? c->zero
                                              : spv_emit_load(&c->emit, c->u32_type, c->subgroup_lane_input);
                lane = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, lane,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, 31));
                u32 one = spv_emit_constant_u32(&c->emit, c->u32_type, 1);
                u32 eq = spv_emit_shift_left_u32(&c->emit, c->u32_type, one, lane);
                u32 lt = spv_emit_isub_u32(&c->emit, c->u32_type, eq, one);
                u32 le = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, lt, eq);
                value = sr == 0      ? lane
                        : sr == 0x38 ? eq
                        : sr == 0x39 ? lt
                        : sr == 0x3a ? le
                        : sr == 0x3b ? spv_emit_not_u32(&c->emit, c->u32_type, le)
                                     : spv_emit_not_u32(&c->emit, c->u32_type, lt);
            } else if (stage == ST_CS && sr == 0x20) {
                /* SR_TID packed: x in bits 0-15, y in 16-25 and z in 26-31, the widths of the largest block (Eden
                   gives y 8 bits) */
                u32 tid = spv_emit_load(&c->emit, vec3_u32, local_input);
                value = spv_emit_vec4_extract(&c->emit, c->u32_type, tid, 0);
                value = spv_emit_bitfield_insert(
                    &c->emit, c->u32_type, value, spv_emit_vec4_extract(&c->emit, c->u32_type, tid, 1),
                    spv_emit_constant_u32(&c->emit, c->u32_type, 16), spv_emit_constant_u32(&c->emit, c->u32_type, 10));
                value = spv_emit_bitfield_insert(
                    &c->emit, c->u32_type, value, spv_emit_vec4_extract(&c->emit, c->u32_type, tid, 2),
                    spv_emit_constant_u32(&c->emit, c->u32_type, 26), spv_emit_constant_u32(&c->emit, c->u32_type, 6));
            } else if (stage == ST_FS && sr == 0x13) /* SR_THREAD_KILL: all ones in a lane KIL demoted */
                value = spv_emit_select(&c->emit, c->u32_type, lane_dead(c),
                                        spv_emit_constant_u32(&c->emit, c->u32_type, 0xffffffffu), c->zero);
            else if (sr == 0x11) /* SR_INVOCATION_ID */
                value = spv_emit_load(&c->emit, c->u32_type, invocation_id);
            else if (sr >= 0x50 && sr <= 0x53) {
                u32 scope = spv_emit_constant_u32(&c->emit, c->u32_type, sr < 0x52 ? 3 : 1);
                u32 clock = spv_emit_read_clock(&c->emit, vec2_u32, scope);
                value = spv_emit_vec4_extract(&c->emit, c->u32_type, clock, sr & 1);
            } else if (sr == 0x1d && between) {
                /* SR_INVOCATION_INFO: the vertices of the input primitive in bits 16-23, with the primitive's index
                   in the batch (bits 0-7) 0, which makes the index the program builds for ISBERD the vertex's */
                /* Citron, acfb1dcc1ad9adf25b960c4f8f25c5bf78849406:
                   geometry's input topology is known when translating. Its
                   invocation info is a constant; tessellation remains dynamic. */
                if (stage == ST_GS)
                    value = spv_emit_constant_u32(&c->emit, c->u32_type, in_vertices << 16);
                else
                    value = spv_emit_shift_left_u32(&c->emit, c->u32_type,
                                                    spv_emit_load(&c->emit, c->u32_type, patch_vertices),
                                                    spv_emit_constant_u32(&c->emit, c->u32_type, 16));
            } else
                /* Direct CBE writes are disabled: their enable and address registers (0x15..0x17) are zero. */
                value = sr == 0x12                     ? spv_emit_constant_u32(&c->emit, c->u32_type, 0x3f800000u)
                        : sr == 0x1d && stage != ST_FS ? spv_emit_constant_u32(&c->emit, c->u32_type, 0x00ff0000u)
                                                       : c->zero;
            reg_write(c, d, value);
        } else if (op == O_ALD && a != 255) {
            /* indexed: each word at the register plus addr (and 4 for the next) is a declared input's component,
               the input picked by the address's vector (0x80 the first generic one, 0x70 the position after the
               vertex stage) and the component by its word. An address of no declared input reads 0, as in Eden */
            u32 width = B(w, 47, 2) + 1;
            u32 vertex = between ? shared_index(c, reg_read(c, B(w, 39, 8)), in_vertices) : 0;
            u32 base = spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a),
                                         spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 10)));
            u32 two = spv_emit_constant_u32(&c->emit, c->u32_type, 2),
                three = spv_emit_constant_u32(&c->emit, c->u32_type, 3);
            u32 four = spv_emit_constant_u32(&c->emit, c->u32_type, 4);
            for (u32 k = 0; k < width; k++) {
                u32 offset = k ? spv_emit_iadd_u32(&c->emit, c->u32_type, base,
                                                   spv_emit_constant_u32(&c->emit, c->u32_type, k * 4))
                               : base;
                u32 vector = spv_emit_shift_right_u32(&c->emit, c->u32_type, offset, four);
                u32 component = spv_emit_bitwise_and_u32(
                    &c->emit, c->u32_type, spv_emit_shift_right_u32(&c->emit, c->u32_type, offset, two), three);
                u32 value = c->zero;
                for (u32 v = 0; v <= 32; v++) {
                    u32 variable = v < 32 ? inputs[v] : between ? position_in : 0;
                    if (!variable) continue;
                    u32 type = v < 32 ? input_component_type[v] : c->f32_type;
                    u32 bits = spv_emit_load(
                        &c->emit, type,
                        spv_emit_interface_scalar_pointer(&c->emit, 1, type, variable, vertex, component));
                    if (type != c->u32_type) bits = spv_emit_bitcast(&c->emit, c->u32_type, bits);
                    u32 at = spv_emit_constant_u32(&c->emit, c->u32_type, v < 32 ? 8 + v : 7);
                    value = spv_emit_select(&c->emit, c->u32_type, spv_emit_i_equal(&c->emit, c->bool_type, vector, at),
                                            bits, value);
                }
                reg_write(c, d + k, value);
            }
        } else if (op == O_ALD && (between || B(w, 31, 1))) {
            u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1, vertex = 0, k;
            int patch = B(w, 31, 1);
            int output = stage == ST_TCS && B(w, 32, 1) && !patch;
            if (!patch) vertex = shared_index(c, reg_read(c, B(w, 39, 8)), output ? out_vertices : in_vertices);
            for (k = 0; k < width; k++) {
                u32 location = addr + k * 4, value = c->zero, pointer = 0, storage;
                u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, (location >> 2) & 3);
                if (patch) { /* the control program reads back what it wrote */
                    storage = stage == ST_TCS ? 3 : 1;
                    if (location >= 0x20)
                        pointer = spv_emit_interface_scalar_pointer(&c->emit, storage, c->f32_type,
                                                                    patch_vars[(location - 0x20) >> 4], 0, component);
                    else if (location < 0x18)
                        pointer = spv_emit_interface_scalar_pointer(
                            &c->emit, storage, c->f32_type, location < 0x10 ? tess_outer : tess_inner, 0,
                            spv_emit_constant_u32(&c->emit, c->u32_type, (location & 0xf) / 4));
                } else if (location == 0x2f0 || location == 0x2f4)
                    pointer = spv_emit_interface_scalar_pointer(
                        &c->emit, 1, c->f32_type, tess_coord, 0,
                        spv_emit_constant_u32(&c->emit, c->u32_type, (location - 0x2f0) / 4));
                else if (location == 0x60)
                    value = spv_emit_load(&c->emit, c->u32_type, primitive_id);
                else if (output)
                    pointer = spv_emit_interface_scalar_pointer(
                        &c->emit, location >= 0x80 && attr_private[(location - 0x80) >> 4] ? 6 : 3, c->f32_type,
                        location < 0x80 ? position : output_attrs[(location - 0x80) >> 4], vertex, component);
                else
                    pointer = spv_emit_interface_scalar_pointer(
                        &c->emit, 1, c->f32_type, location < 0x80 ? position_in : inputs[(location - 0x80) >> 4],
                        vertex, component);
                if (pointer)
                    value = spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_load(&c->emit, c->f32_type, pointer));
                reg_write(c, d + k, value);
            }
        } else if (op == O_ALD) {
            u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1;
            for (u32 k = 0; k < width; k++) {
                u32 location = addr + k * 4, index = (location - 0x80) >> 4;
                if (location == 0x2f8 || location == 0x2fc) {
                    u32 instance;
                    if (location == 0x2f8) {
                        instance = spv_emit_load(&c->emit, c->u32_type, instance_index);
                        u32 base_bits =
                            spv_emit_push_component_f32(&c->emit, push_pointer, push, c->zero, c->zero, c->f32_type);
                        u32 base = spv_emit_bitcast(&c->emit, c->u32_type, base_bits);
                        instance = spv_emit_isub_u32(&c->emit, c->u32_type, instance, base);
                    } else
                        instance = spv_emit_load(&c->emit, c->u32_type, vertex_index);
                    reg_write(c, d + k, instance);
                } else {
                    u32 vector = spv_emit_load(&c->emit, input_vec_type[index], inputs[index]);
                    u32 value =
                        spv_emit_vec4_extract(&c->emit, input_component_type[index], vector, (location >> 2) & 3);
                    reg_write(c, d + k,
                              input_component_type[index] == c->u32_type
                                  ? value
                                  : spv_emit_bitcast(&c->emit, c->u32_type, value));
                }
            }
        } else if (op == O_AST && (stage == ST_TCS || B(w, 31, 1))) {
            /* a control program stores its invocation's output vertex, and the patch's levels and vectors */
            u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1, k;
            u32 vertex = B(w, 31, 1) ? 0 : spv_emit_load(&c->emit, c->u32_type, invocation_id);
            for (k = 0; k < width; k++) {
                u32 location = addr + k * 4, pointer = 0;
                u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, (location >> 2) & 3);
                if (!B(w, 31, 1))
                    pointer = spv_emit_interface_scalar_pointer(
                        &c->emit, location >= 0x80 && attr_private[(location - 0x80) >> 4] ? 6 : 3, c->f32_type,
                        location < 0x80 ? position : output_attrs[(location - 0x80) >> 4], vertex, component);
                else if (location >= 0x20)
                    pointer = spv_emit_interface_scalar_pointer(&c->emit, 3, c->f32_type,
                                                                patch_vars[(location - 0x20) >> 4], 0, component);
                else if (location < 0x18)
                    pointer = spv_emit_interface_scalar_pointer(
                        &c->emit, 3, c->f32_type, location < 0x10 ? tess_outer : tess_inner, 0,
                        spv_emit_constant_u32(&c->emit, c->u32_type, (location & 0xf) / 4));
                if (pointer)
                    spv_emit_store(&c->emit, pointer, reg_read_as(c, d + k, c->f32_type));
            }
        } else if (op == O_AST) {
            u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1;
            for (u32 k = 0; k < width; k++) {
                u32 location = addr + k * 4;
                if (location == 0x6c) {
                    if (point_size)
                        spv_emit_store(&c->emit, point_size,
                                       reg_read_as(c, d + k, c->f32_type));
                    continue;
                }
                if (location == 0x68) {
                    spv_emit_store(&c->emit, viewport_output, reg_read_as(c, d + k, i32_type));
                    continue;
                }
                if (location == 0x64) {
                    if (layer_output) /* none: the device can't take this stage's layer */
                        spv_emit_store(&c->emit, layer_output,
                                       reg_read_as(c, d + k, i32_type));
                    continue;
                }
                if (location >= 0x2c0) {
                    u32 index = (location - 0x2c0) / 4;
                    if (clip_used & (1u << index)) {
                        u32 ptr = spv_emit_output_element_pointer(&c->emit, c->f32_type, clip_output,
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, index));
                        spv_emit_store(&c->emit, ptr, reg_read_as(c, d + k, c->f32_type));
                    }
                    continue;
                }
                u32 variable = location < 0x80 ? position : output_attrs[(location - 0x80) >> 4];
                u32 vector = spv_emit_load(&c->emit, vec4, variable);
                u32 value = reg_read_as(c, d + k, c->f32_type);
                vector = spv_emit_vec4_insert(&c->emit, vec4, vector, value, (location >> 2) & 3);
                spv_emit_store(&c->emit, variable, vector);
            }
        } else if (op == O_OUT_R || op == O_OUT_C || op == O_OUT_I) {
            /* The stream is the operand's low two bits. Only stream 0 is rasterized and nothing records the others
               (there is no transform feedback), so a vertex or primitive on streams 1-3 changes nothing: a known
               stream other than 0 emits nothing, and one from a register or constant buffer is tested */
            u32 stream_merge = 0;
            if (op == O_OUT_C || (op == O_OUT_R && b != 255)) {
                u32 stream =
                    spv_emit_bitwise_and_u32(&c->emit, c->u32_type, op == O_OUT_C ? cbuf_word(c, w) : reg_read(c, b),
                                             spv_emit_constant_u32(&c->emit, c->u32_type, 3));
                u32 other = spv_emit_i_not_equal(&c->emit, c->bool_type, stream, c->zero);
                u32 emit_label = spv_emit_id(&c->emit);
                stream_merge = spv_emit_id(&c->emit);
                spv_emit_selection_merge(&c->emit, stream_merge);
                spv_emit_branch_conditional(&c->emit, other, stream_merge, emit_label);
                spv_emit_label(&c->emit, emit_label);
            }
            if (op != O_OUT_I || !B(w, 20, 2)) {
                /* the vertex is complete: the depth mode applies to it, as at the end of the other stages */
                if (B(w, 39, 1)) {
                    if (depth_fix) vertex_depth_fix(c, position, push, push_pointer, vec4);
                    spv_emit_store(&c->emit, primitive_id_out, spv_emit_load(&c->emit, c->u32_type, primitive_id));
                    if (xfb_on) xfb_store(c, vec4, output_attrs, attr_private, xfb_components);
                    spv_emit_emit_vertex(&c->emit);
                    /* outputs are undefined after it: the next vertex gets the default point size, unless the
                       program writes its own */
                    if (point_size && !point_size_used)
                        spv_emit_store(&c->emit, point_size, spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f));
                }
                if (B(w, 40, 1)) spv_emit_end_primitive(&c->emit);
            }
            if (stream_merge) {
                spv_emit_branch(&c->emit, stream_merge);
                spv_emit_label(&c->emit, stream_merge);
            }
            reg_write(c, d, c->zero); /* the output handle, which the host doesn't have */
        } else if (op == O_AL2P) {
            u32 offset = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 11));
            reg_write(c, d, spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), offset));
        } else if (op == O_ISBERD) {
            reg_write(c, d, reg_read(c, a));
        } else if (op == O_MOV32I) {
            u32 value = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
            reg_write(c, d, value);
        } else if (op == O_IMUL_R || op == O_IMUL_C || op == O_IMUL_I || op == O_IMUL32I || op == O_IMAD_R ||
                   op == O_IMAD_RC || op == O_IMAD_CR || op == O_IMAD_I || op == O_IMAD32I) {
            int immediate32 = op == O_IMUL32I || op == O_IMAD32I;
            int mad = op >= O_IMAD_R && op <= O_IMAD32I;
            u32 lhs = reg_read(c, a);
            u32 rhs = immediate32       ? spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32))
                      : op == O_IMAD_RC ? reg_read(c, B(w, 39, 8))
                      : op == O_IMAD_CR ? cbuf_word(c, w)
                                        : integer_right(c, w,
                                                        op == O_IMUL_R || op == O_IMAD_R ? 0
                                                        : op == O_IMUL_C                 ? 1
                                                                                         : 2);
            u32 addend = c->zero;
            int high;
            int signed_lhs;
            int signed_rhs;
            u32 product;
            if (mad) {
                u32 mode;
                addend = op == O_IMAD32I   ? reg_read(c, d)
                         : op == O_IMAD_RC ? cbuf_word(c, w)
                                           : reg_read(c, B(w, 39, 8));
                mode = B(w, immediate32 ? 55 : 51, 2);
                if (mode == 2) rhs = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, rhs);
                if (mode == 1) addend = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, addend);
                if (mode == 3)
                    addend = spv_emit_iadd_u32(&c->emit, c->u32_type, addend,
                                               spv_emit_constant_u32(&c->emit, c->u32_type, 1));
                if (!immediate32 && B(w, 49, 1))
                    addend = spv_emit_iadd_u32(
                        &c->emit, c->u32_type, addend,
                        spv_emit_select(&c->emit, c->u32_type, flag_read(c, c->cc_c),
                                        spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero));
            }
            high = B(w, immediate32 ? 53 : mad ? 54 : 39, 1);
            signed_lhs = B(w, immediate32 ? 54 : mad ? 48 : 40, 1);
            signed_rhs = B(w, immediate32 ? mad ? 57 : 55 : mad ? 53 : 41, 1);
            product = high ? integer_mul_high(c, lhs, rhs, signed_lhs, signed_rhs)
                           : spv_emit_imul_u32(&c->emit, c->u32_type, lhs, rhs);
            reg_write(c, d, mad ? spv_emit_iadd_u32(&c->emit, c->u32_type, product, addend) : product);
        } else if (op == O_VOTE) {
            u32 value = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 lane = spv_emit_load(&c->emit, c->u32_type, c->subgroup_lane_input);
            u32 mask = subgroup_ballot_word(c, vec4_u32, lane, value);
            u32 mode = B(w, 48, 2);
            if (mode < 3) {
                u32 any = spv_emit_i_not_equal(&c->emit, c->bool_type, mask, c->zero);
                u32 result = any;
                if (mode != 1) {
                    u32 active = subgroup_ballot_word(c, vec4_u32, lane, c->true_value);
                    u32 all = spv_emit_i_equal(&c->emit, c->bool_type, mask, active);
                    result = mode == 0 ? all
                                       : spv_emit_bool_binary(&c->emit, c->bool_type, 1, all,
                                                              spv_emit_bool_not(&c->emit, c->bool_type, any));
                }
                pred_write(c, B(w, 45, 3), result);
            }
            reg_write(c, d, mask);
        } else if (op == O_FLO_R || op == O_FLO_C || op == O_FLO_I) {
            u32 value = integer_right(c, w, op - O_FLO_R);
            if (B(w, 40, 1)) value = spv_emit_not_u32(&c->emit, c->u32_type, value);
            if (B(w, 48, 1)) {
                u32 negative = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                  spv_emit_bitcast(&c->emit, i32_type, value));
                value = spv_emit_select(&c->emit, c->u32_type, negative, spv_emit_not_u32(&c->emit, c->u32_type, value),
                                        value);
            }
            u32 spread = value;
            for (u32 shift = 1; shift <= 16; shift <<= 1)
                spread = spv_emit_bitwise_or_u32(
                    &c->emit, c->u32_type, spread,
                    spv_emit_shift_right_u32(&c->emit, c->u32_type, spread,
                                             spv_emit_constant_u32(&c->emit, c->u32_type, shift)));
            u32 index = spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_bit_count(&c->emit, c->u32_type, spread),
                                          spv_emit_constant_u32(&c->emit, c->u32_type, 1));
            if (B(w, 41, 1)) {
                u32 found = spv_emit_i_not_equal(&c->emit, c->bool_type, value, c->zero);
                index = spv_emit_select(
                    &c->emit, c->u32_type, found,
                    spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_constant_u32(&c->emit, c->u32_type, 31), index),
                    index);
            }
            reg_write(c, d, index);
        } else if (op == O_POPC_R || op == O_POPC_C || op == O_POPC_I) {
            u32 value = integer_right(c, w, op - O_POPC_R);
            if (B(w, 40, 1)) value = spv_emit_not_u32(&c->emit, c->u32_type, value);
            reg_write(c, d, spv_emit_bit_count(&c->emit, c->u32_type, value));
        } else if (op == O_SHFL) {
            u32 index = B(w, 28, 1) ? spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 5)) : reg_read(c, b);
            u32 mask =
                B(w, 29, 1) ? spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 34, 13)) : reg_read(c, B(w, 39, 8));
            u32 lane_full = spv_emit_load(&c->emit, c->u32_type, c->subgroup_lane_input);
            u32 lane = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, lane_full,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            u32 seg = spv_emit_bitwise_and_u32(
                &c->emit, c->u32_type,
                spv_emit_shift_right_u32(&c->emit, c->u32_type, mask, spv_emit_constant_u32(&c->emit, c->u32_type, 8)),
                spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            u32 min_lane = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, lane, seg);
            u32 max_lane = spv_emit_bitwise_or_u32(
                &c->emit, c->u32_type, min_lane,
                spv_emit_bitwise_and_u32(&c->emit, c->u32_type, mask,
                                         spv_emit_bitwise_and_u32(&c->emit, c->u32_type,
                                                                  spv_emit_not_u32(&c->emit, c->u32_type, seg),
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 31))));
            u32 mode = B(w, 30, 2), source_lane, valid;
            if (mode == 0) {
                source_lane =
                    spv_emit_bitwise_or_u32(&c->emit, c->u32_type, min_lane,
                                            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, index,
                                                                     spv_emit_not_u32(&c->emit, c->u32_type, seg)));
            } else if (mode == 1) {
                source_lane = spv_emit_isub_u32(&c->emit, c->u32_type, lane, index);
            } else if (mode == 2) {
                source_lane = spv_emit_iadd_u32(&c->emit, c->u32_type, lane, index);
            } else {
                source_lane = spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, lane, index);
            }
            valid = mode == 1 ? spv_emit_bool_not(
                                    &c->emit, c->bool_type,
                                    spv_emit_u_less(&c->emit, c->bool_type,
                                                    spv_emit_isub_u32(&c->emit, c->u32_type, lane, min_lane), index))
                              : spv_emit_bool_not(&c->emit, c->bool_type,
                                                  spv_emit_u_less(&c->emit, c->bool_type, max_lane, source_lane));
            u32 host_lane =
                spv_emit_bitwise_or_u32(&c->emit, c->u32_type,
                                        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, lane_full,
                                                                 spv_emit_constant_u32(&c->emit, c->u32_type, ~31u)),
                                        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, source_lane,
                                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 31)));
            u32 value = reg_read(c, a);
            u32 shuffled = spv_emit_subgroup_shuffle_u32(&c->emit, c->u32_type, value, host_lane);
            pred_write(c, B(w, 48, 3), valid);
            reg_write(c, d, spv_emit_select(&c->emit, c->u32_type, valid, shuffled, value));
        } else if (op == O_MOV_R || op == O_MOV_C || op == O_MOV_I) {
            if (B(w, 39, 4) == 1 || B(w, 39, 4) == 15) reg_write(c, d, integer_right(c, w, op - O_MOV_R));
        } else if (op == O_IADD32I) {
            u32 immediate = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
            reg_write(c, d, integer_add(c, reg_read(c, a), immediate, B(w, 55, 2), B(w, 53, 1), B(w, 52, 1)));
        } else if (op == O_IADD3_R || op == O_IADD3_C || op == O_IADD3_I) {
            u32 first = reg_read(c, a);
            u32 second = integer_right(c, w, op - O_IADD3_R);
            u32 third = reg_read(c, B(w, 39, 8));
            if (op == O_IADD3_R) {
                u32 values[3] = {first, second, third};
                for (u32 k = 0; k < 3; k++) {
                    u32 half = B(w, 35 - 2 * k, 2);
                    if (half == 1)
                        values[k] = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, values[k],
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff));
                    if (half == 2)
                        values[k] = spv_emit_shift_right_u32(&c->emit, c->u32_type, values[k],
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 16));
                }
                first = values[0];
                second = values[1];
                third = values[2];
            }
            if (B(w, 51, 1)) first = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, first);
            if (B(w, 50, 1)) second = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, second);
            if (B(w, 49, 1)) third = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, third);
            u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, first, second);
            u32 carry0 = spv_emit_u_less(&c->emit, c->bool_type, sum, first);
            u32 shift = op == O_IADD3_R ? B(w, 37, 2) : 0;
            if (shift == 1) /* the 33-bit sum shifted: its carry becomes bit 16 */
                sum = spv_emit_bitwise_or_u32(
                    &c->emit, c->u32_type,
                    spv_emit_shift_right_u32(&c->emit, c->u32_type, sum,
                                             spv_emit_constant_u32(&c->emit, c->u32_type, 16)),
                    spv_emit_select(&c->emit, c->u32_type, carry0,
                                    spv_emit_constant_u32(&c->emit, c->u32_type, 0x10000), c->zero));
            if (shift == 2)
                sum = spv_emit_shift_left_u32(&c->emit, c->u32_type, sum,
                                              spv_emit_constant_u32(&c->emit, c->u32_type, 16));
            u32 sum2 = spv_emit_iadd_u32(&c->emit, c->u32_type, sum, third);
            u32 carry1 = spv_emit_u_less(&c->emit, c->bool_type, sum2, sum);
            u32 carry2 = c->false_value;
            if (B(w, 48, 1)) {
                u32 cin = spv_emit_select(&c->emit, c->u32_type, flag_read(c, c->cc_c),
                                          spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero);
                u32 final = spv_emit_iadd_u32(&c->emit, c->u32_type, sum2, cin);
                carry2 = spv_emit_u_less(&c->emit, c->bool_type, final, sum2);
                sum2 = final;
            }
            if (B(w, 47, 1)) {
                u32 carry = spv_emit_bool_binary(&c->emit, c->bool_type, 1, carry1, carry2);
                if (shift != 1 && shift != 2) carry = spv_emit_bool_binary(&c->emit, c->bool_type, 1, carry, carry0);
                u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, sum2, c->zero);
                if (B(w, 48, 1))
                    zero = spv_emit_bool_binary(&c->emit, c->bool_type, 0, zero,
                                                flag_read(c, c->cc_z));
                u32 sign = spv_emit_i_not_equal(
                    &c->emit, c->bool_type,
                    spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sum2,
                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000)),
                    c->zero);
                cc_store(c, c->cc_c, carry);
                cc_store(c, c->cc_z, zero);
                cc_store(c, c->cc_s, sign);
            }
            reg_write(c, d, sum2);
        } else if (op == O_IADD_R || op == O_IADD_C || op == O_IADD_I) {
            u32 left = reg_read(c, a), right = integer_right(c, w, op - O_IADD_R);
            reg_write(c, d, integer_add(c, left, right, B(w, 48, 2), B(w, 43, 1), B(w, 47, 1)));
        } else if (op == O_LOP32I) {
            u32 left = reg_read(c, a);
            u32 right = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
            if (B(w, 55, 1)) left = spv_emit_not_u32(&c->emit, c->u32_type, left);
            if (B(w, 56, 1)) right = spv_emit_not_u32(&c->emit, c->u32_type, right);
            u32 value;
            switch (B(w, 53, 2)) {
                case 0: value = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, left, right); break;
                case 1: value = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, left, right); break;
                case 2: value = spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, left, right); break;
                default: value = right; break;
            }
            reg_write(c, d, value);
            if (B(w, 52, 1)) flags_logic_extended(c, value, (int)B(w, 57, 1));
        } else if (op == O_LOP_R || op == O_LOP_C || op == O_LOP_I) {
            u32 left = reg_read(c, a), right = integer_right(c, w, op - O_LOP_R);
            if (B(w, 39, 1)) left = spv_emit_not_u32(&c->emit, c->u32_type, left);
            if (B(w, 40, 1)) right = spv_emit_not_u32(&c->emit, c->u32_type, right);
            u32 value;
            switch (B(w, 41, 2)) {
                case 0: value = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, left, right); break;
                case 1: value = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, left, right); break;
                case 2: value = spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, left, right); break;
                default: value = right; break;
            }
            reg_write(c, d, value);
            if (B(w, 47, 1)) flags_logic_extended(c, value, (int)B(w, 43, 1));
            if (B(w, 48, 3) < 7) {
                u32 mode = B(w, 44, 2);
                u32 predicate = mode == 0   ? c->false_value
                                : mode == 1 ? c->true_value
                                : mode == 2 ? spv_emit_i_equal(&c->emit, c->bool_type, value, c->zero)
                                            : spv_emit_i_not_equal(&c->emit, c->bool_type, value, c->zero);
                pred_write(c, B(w, 48, 3), predicate);
            }
        } else if (op == O_LOP3_R || op == O_LOP3_C || op == O_LOP3_I) {
            u32 first = reg_read(c, a), second = integer_right(c, w, op - O_LOP3_R);
            u32 third = reg_read(c, B(w, 39, 8));
            u32 lut = op == O_LOP3_R ? B(w, 28, 8) : B(w, 48, 8);
            u32 value = c->zero;
            for (u32 bit = 0; bit < 8; bit++) {
                if (!(lut & (1u << bit))) continue;
                u32 x = bit & 4 ? first : spv_emit_not_u32(&c->emit, c->u32_type, first);
                u32 y = bit & 2 ? second : spv_emit_not_u32(&c->emit, c->u32_type, second);
                u32 z = bit & 1 ? third : spv_emit_not_u32(&c->emit, c->u32_type, third);
                u32 term = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, x, y);
                term = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, term, z);
                value = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, value, term);
            }
            reg_write(c, d, value);
            /* LOP3.CC is bit 47; X is bit 38 for R, 56 for C and 57 for I. */
            if (B(w, 47, 1))
                flags_logic_extended(c, value, (int)B(w, op == O_LOP3_R ? 38 : op == O_LOP3_C ? 56 : 57, 1));
            if (op == O_LOP3_R && B(w, 48, 3) < 7) {
                u32 mode = B(w, 36, 2);
                u32 predicate = mode == 0   ? c->false_value
                                : mode == 1 ? c->true_value
                                : mode == 2 ? spv_emit_i_equal(&c->emit, c->bool_type, value, c->zero)
                                            : spv_emit_i_not_equal(&c->emit, c->bool_type, value, c->zero);
                pred_write(c, B(w, 48, 3), predicate);
            }
        } else if (op == O_SEL_R || op == O_SEL_C || op == O_SEL_I) {
            u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 rhs = integer_right(c, w, op - O_SEL_R);
            reg_write(c, d, spv_emit_select(&c->emit, c->u32_type, condition, reg_read(c, a), rhs));
        } else if (op >= O_XMAD_R && op <= O_XMAD_I) {
            /* Each form's operands and fields, as Eden reads them: B is Rb (R), Rc at bit 39 (RC), a constant buffer
               word (CR) or a 16-bit immediate (I), and C is Rc or the constant buffer word. The product of A's and
               B's halves (signed by bits 48 and 49) is shifted up 16 by PSL; the select picks C, its low or high
               half, or adds B << 16 to it (CBCC); MRG puts B's low half in the result's high half. X adds the carry
               and CC writes the flags, as IADD's do (Eden refuses both). */
            int narrow = op == O_XMAD_RC || op == O_XMAD_CR;
            u32 source_b = op == O_XMAD_R    ? reg_read(c, b)
                           : op == O_XMAD_RC ? reg_read(c, B(w, 39, 8))
                           : op == O_XMAD_CR ? cbuf_word(c, w)
                                             : spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 16));
            u32 source_c = op == O_XMAD_RC ? cbuf_word(c, w) : reg_read(c, B(w, 39, 8));
            u32 select = narrow ? B(w, 50, 2) : B(w, 50, 3);
            int half_b = op == O_XMAD_R ? (int)B(w, 35, 1) : narrow ? (int)B(w, 52, 1) : 0;
            int psl = op == O_XMAD_CR ? (int)B(w, 55, 1) : op == O_XMAD_RC ? 0 : (int)B(w, 36, 1);
            int mrg = op == O_XMAD_CR ? (int)B(w, 56, 1) : op == O_XMAD_RC ? 0 : (int)B(w, 37, 1);
            int extended = narrow ? (int)B(w, 54, 1) : (int)B(w, 38, 1);
            u32 sixteen = spv_emit_constant_u32(&c->emit, c->u32_type, 16);
            u32 left = xmad_half(c, i32_type, reg_read(c, a), B(w, 53, 1), B(w, 48, 1));
            u32 right = xmad_half(c, i32_type, source_b, half_b, B(w, 49, 1));
            u32 product = spv_emit_imul_u32(&c->emit, c->u32_type, left, right);
            if (psl) product = spv_emit_shift_left_u32(&c->emit, c->u32_type, product, sixteen);
            u32 third =
                select == 1   ? xmad_half(c, i32_type, source_c, 0, 0)
                : select == 2 ? xmad_half(c, i32_type, source_c, 1, 0)
                : select == 4
                    ? spv_emit_iadd_u32(&c->emit, c->u32_type,
                                        spv_emit_shift_left_u32(&c->emit, c->u32_type, source_b, sixteen), source_c)
                    : source_c;
            u32 sum = integer_add(c, product, third, 0, extended, (int)B(w, 47, 1));
            if (mrg) {
                u32 low_mask = spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff);
                sum = spv_emit_bitwise_or_u32(
                    &c->emit, c->u32_type, spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sum, low_mask),
                    spv_emit_shift_left_u32(&c->emit, c->u32_type,
                                            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, source_b, low_mask),
                                            sixteen));
            }
            reg_write(c, d, sum);
        } else if (op == O_LDC) {
            u32 off = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)(i32)(i16)B(w, 20, 16));
            u32 address = a == 255 ? off : spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), off);
            u32 bank = B(w, 36, 5);
            u32 value = cbuf_read_dynamic(c, bank, address);
            u32 size = B(w, 48, 3);
            /* the element an indexed texture whose handle this loads picks: the address register over the stride */
            for (k_indexed = 0; k_indexed < texture_count; k_indexed++)
                if ((textures[k_indexed].flags & SNX_TEX_INDEXED) && textures[k_indexed].bl2_off == i + 1)
                    spv_emit_store(&c->emit, c->tex_index[k_indexed],
                                   spv_emit_shift_right_u32(
                                       &c->emit, c->u32_type, reg_read(c, a),
                                       spv_emit_constant_u32(&c->emit, c->u32_type, textures[k_indexed].flags >> 4)));
            if (size < 4) {
                u32 byte_offset = spv_emit_bitwise_and_u32(
                    &c->emit, c->u32_type, address, spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 3 : 2));
                u32 bit_offset = spv_emit_shift_left_u32(&c->emit, c->u32_type, byte_offset,
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 3));
                u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 8 : 16);
                value = size & 1
                            ? spv_emit_bitcast(&c->emit, c->u32_type,
                                               spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                          spv_emit_bitcast(&c->emit, i32_type, value),
                                                                          bit_offset, width))
                            : spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, bit_offset, width);
            }
            reg_write(c, d, value);
            if (size == 5) {
                u32 next =
                    spv_emit_iadd_u32(&c->emit, c->u32_type, address, spv_emit_constant_u32(&c->emit, c->u32_type, 4));
                reg_write(c, d + 1, cbuf_read_dynamic(c, bank, next));
            }
        } else if (op == O_ATOMS) {
            u32 index = shared_atomic_index(c, a, w, shared_words);
            u32 pointer = spv_emit_function_element_pointer(&c->emit, shared_element_pointer, shared_array, index);
            u32 operation = shared_atomic_opcode(w);
            if (operation == 0xfffffffeu) {
                u32 next_index = shared_index(
                    c, spv_emit_iadd_u32(&c->emit, c->u32_type, index, spv_emit_constant_u32(&c->emit, c->u32_type, 1)),
                    shared_words);
                u32 high =
                    spv_emit_function_element_pointer(&c->emit, shared_element_pointer, shared_array, next_index);
                u32 old_low = spv_emit_load(&c->emit, c->u32_type, pointer);
                u32 old_high = spv_emit_load(&c->emit, c->u32_type, high);
                spv_emit_store(&c->emit, pointer, reg_read(c, b));
                spv_emit_store(&c->emit, high, reg_read(c, b + 1));
                reg_write(c, d, old_low);
                reg_write(c, d + 1, old_high);
            } else if (operation != 0xffffffffu) {
                reg_write(c, d, spv_emit_atomic_shared_u32(&c->emit, c->u32_type, operation, pointer, reg_read(c, b)));
            } else {
                u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
                u32 cont = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
                spv_emit_store(&c->emit, atomic_old_var,
                               spv_emit_atomic_load_shared_u32(&c->emit, c->u32_type, pointer));
                spv_emit_branch(&c->emit, header);
                spv_emit_label(&c->emit, header);
                spv_emit_loop_merge(&c->emit, merge, cont);
                spv_emit_branch(&c->emit, body);
                spv_emit_label(&c->emit, body);
                u32 old = spv_emit_load(&c->emit, c->u32_type, atomic_old_var);
                u32 value = reg_read(c, b);
                u32 next;
                if (B(w, 52, 4) == 3) {
                    u32 increment =
                        spv_emit_iadd_u32(&c->emit, c->u32_type, old, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
                    next = spv_emit_select(&c->emit, c->u32_type,
                                           cc_not(c, spv_emit_u_less(&c->emit, c->bool_type, old, value)), c->zero,
                                           increment);
                } else {
                    u32 decrement =
                        spv_emit_isub_u32(&c->emit, c->u32_type, old, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
                    u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, old, c->zero);
                    u32 greater = spv_emit_u_less(&c->emit, c->bool_type, value, old);
                    next = spv_emit_select(&c->emit, c->u32_type, cc_or(c, zero, greater), value, decrement);
                }
                u32 observed = spv_emit_atomic_compare_exchange_shared_u32(&c->emit, c->u32_type, pointer, next, old);
                spv_emit_store(&c->emit, atomic_old_var, observed);
                spv_emit_branch_conditional(&c->emit, spv_emit_i_equal(&c->emit, c->bool_type, observed, old), merge,
                                            cont);
                spv_emit_label(&c->emit, cont);
                spv_emit_branch(&c->emit, header);
                spv_emit_label(&c->emit, merge);
                reg_write(c, d, spv_emit_load(&c->emit, c->u32_type, atomic_old_var));
            }
        } else if (op == O_LDS || op == O_STS) {
            u32 immediate = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)((i32)(B(w, 20, 24) << 8) >> 8));
            u32 address = a == 255 ? immediate : spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), immediate);
            u32 first = spv_emit_shift_right_u32(&c->emit, c->u32_type, address,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 2));
            u32 words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1;
            u32 size = B(w, 48, 3);
            for (u32 k = 0; k < words; k++) {
                u32 index =
                    k ? spv_emit_iadd_u32(&c->emit, c->u32_type, first, spv_emit_constant_u32(&c->emit, c->u32_type, k))
                      : first;
                index = shared_index(c, index, shared_words);
                u32 pointer = spv_emit_function_element_pointer(&c->emit, shared_element_pointer, shared_array, index);
                if (size < 4) {
                    u32 bit_offset = spv_emit_bitwise_and_u32(
                        &c->emit, c->u32_type, address, spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 3 : 2));
                    bit_offset = spv_emit_shift_left_u32(&c->emit, c->u32_type, bit_offset,
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 3));
                    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 8 : 16);
                    if (op == O_STS) {
                        u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
                        u32 retry = spv_emit_id(&c->emit), done = spv_emit_id(&c->emit);
                        spv_emit_branch(&c->emit, header);
                        spv_emit_label(&c->emit, header);
                        spv_emit_loop_merge(&c->emit, done, retry);
                        spv_emit_branch(&c->emit, body);
                        spv_emit_label(&c->emit, body);
                        u32 old = spv_emit_atomic_load_shared_u32(&c->emit, c->u32_type, pointer);
                        u32 desired =
                            spv_emit_bitfield_insert(&c->emit, c->u32_type, old, reg_read(c, d), bit_offset, width);
                        u32 actual =
                            spv_emit_atomic_compare_exchange_shared_u32(&c->emit, c->u32_type, pointer, desired, old);
                        u32 success = spv_emit_i_equal(&c->emit, c->bool_type, actual, old);
                        spv_emit_branch_conditional(&c->emit, success, done, retry);
                        spv_emit_label(&c->emit, retry);
                        spv_emit_branch(&c->emit, header);
                        spv_emit_label(&c->emit, done);
                    } else {
                        u32 old = spv_emit_atomic_load_shared_u32(&c->emit, c->u32_type, pointer);
                        u32 value =
                            size & 1
                                ? spv_emit_bitcast(&c->emit, c->u32_type,
                                                   spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                              spv_emit_bitcast(&c->emit, i32_type, old),
                                                                              bit_offset, width))
                                : spv_emit_bitfield_uextract(&c->emit, c->u32_type, old, bit_offset, width);
                        reg_write(c, d, value);
                    }
                } else if (op == O_STS)
                    spv_emit_store(&c->emit, pointer, reg_read(c, d + k));
                else
                    reg_write(c, d + k, spv_emit_load(&c->emit, c->u32_type, pointer));
            }
        } else if (op == O_BAR) {
            if (stage == ST_TCS)
                spv_emit_patch_barrier(&c->emit, c->u32_type);
            else
                spv_emit_control_barrier(&c->emit, c->u32_type);
        } else if (op == O_LDL || op == O_STL) {
            u32 immediate = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)((i32)(B(w, 20, 24) << 8) >> 8));
            u32 address = a == 255 ? immediate : spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), immediate);
            u32 first = spv_emit_shift_right_u32(&c->emit, c->u32_type, address,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 2));
            u32 words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1;
            u32 size = B(w, 48, 3);
            for (u32 k = 0; k < words; k++) {
                u32 index =
                    k ? spv_emit_iadd_u32(&c->emit, c->u32_type, first, spv_emit_constant_u32(&c->emit, c->u32_type, k))
                      : first;
                index = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, index,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, local_words - 1));
                u32 pointer = spv_emit_function_element_pointer(&c->emit, local_element_pointer, local_array, index);
                if (size < 4) {
                    u32 bit_offset = spv_emit_bitwise_and_u32(
                        &c->emit, c->u32_type, address, spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 3 : 2));
                    bit_offset = spv_emit_shift_left_u32(&c->emit, c->u32_type, bit_offset,
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 3));
                    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 8 : 16);
                    u32 old = spv_emit_load(&c->emit, c->u32_type, pointer);
                    if (op == O_STL)
                        spv_emit_store(
                            &c->emit, pointer,
                            spv_emit_bitfield_insert(&c->emit, c->u32_type, old, reg_read(c, d), bit_offset, width));
                    else {
                        u32 value =
                            size & 1
                                ? spv_emit_bitcast(&c->emit, c->u32_type,
                                                   spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                              spv_emit_bitcast(&c->emit, i32_type, old),
                                                                              bit_offset, width))
                                : spv_emit_bitfield_uextract(&c->emit, c->u32_type, old, bit_offset, width);
                        reg_write(c, d, value);
                    }
                } else if (op == O_STL)
                    spv_emit_store(&c->emit, pointer, reg_read(c, d + k));
                else
                    reg_write(c, d + k, spv_emit_load(&c->emit, c->u32_type, pointer));
            }
        } else if (op == O_STG && gb_at[i] == 255) {
            /* SR_DIRECTCBEWRITEENABLED is zero: use the shader's ordinary attribute-export path. */
        } else if ((op == O_LDG || op == O_STG) && gb_at[i] == GB_DYNAMIC) {
            global_dynamic(c, stage, w, op, gbuf_off, ngbuf);
        } else if (op == O_LDG || op == O_STG) {
            u32 slot = gb_at[i] - 1;
            u32 offset = spv_emit_constant_u32(&c->emit, c->u32_type, SH_GBUF_OFFSET(gbuf_off[slot]));
            u32 base = cbuf_read_dynamic(c, SH_GBUF_BANK(gbuf_off[slot]), offset);
            u32 immediate = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)((i32)(B(w, 20, 24) << 8) >> 8));
            u32 address = spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), immediate);
            address = spv_emit_isub_u32(&c->emit, c->u32_type, address, base);
            u32 index = spv_emit_shift_right_u32(&c->emit, c->u32_type, address,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 2));
            u32 words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1;
            u32 size = B(w, 48, 3);
            for (u32 k = 0; k < words; k++) {
                u32 word =
                    k ? spv_emit_iadd_u32(&c->emit, c->u32_type, index, spv_emit_constant_u32(&c->emit, c->u32_type, k))
                      : index;
                word = gbuf_word_index(c, stage, slot, word);
                u32 ptr = spv_emit_storage_access_u32(&c->emit, c->gbuf_pointer, c->gbuf[slot], c->zero, word);
                if (size < 4) {
                    u32 byte_offset = spv_emit_bitwise_and_u32(
                        &c->emit, c->u32_type, address, spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 3 : 2));
                    u32 bit_offset = spv_emit_shift_left_u32(&c->emit, c->u32_type, byte_offset,
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 3));
                    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 8 : 16);
                    if (op == O_STG) {
                        u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
                        u32 retry = spv_emit_id(&c->emit), done = spv_emit_id(&c->emit);
                        spv_emit_branch(&c->emit, header);
                        spv_emit_label(&c->emit, header);
                        spv_emit_loop_merge(&c->emit, done, retry);
                        spv_emit_branch(&c->emit, body);
                        spv_emit_label(&c->emit, body);
                        u32 old = spv_emit_atomic_load_u32(&c->emit, c->u32_type, ptr);
                        u32 desired =
                            spv_emit_bitfield_insert(&c->emit, c->u32_type, old, reg_read(c, d), bit_offset, width);
                        u32 actual = spv_emit_atomic_compare_exchange_u32(&c->emit, c->u32_type, ptr, desired, old);
                        u32 success = spv_emit_i_equal(&c->emit, c->bool_type, actual, old);
                        spv_emit_branch_conditional(&c->emit, success, done, retry);
                        spv_emit_label(&c->emit, retry);
                        spv_emit_branch(&c->emit, header);
                        spv_emit_label(&c->emit, done);
                    } else {
                        u32 old = spv_emit_atomic_load_u32(&c->emit, c->u32_type, ptr);
                        u32 value =
                            size & 1
                                ? spv_emit_bitcast(&c->emit, c->u32_type,
                                                   spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                              spv_emit_bitcast(&c->emit, i32_type, old),
                                                                              bit_offset, width))
                                : spv_emit_bitfield_uextract(&c->emit, c->u32_type, old, bit_offset, width);
                        reg_write(c, d, value);
                    }
                } else if (op == O_STG)
                    spv_emit_store(&c->emit, ptr, reg_read(c, d + k));
                else
                    reg_write(c, d + k, spv_emit_load(&c->emit, c->u32_type, ptr));
            }
        } else if (op == O_VADD || op == O_VABSDIFF || op == O_VSET || op == O_VSHL || op == O_VSHR) {
            /* Ryujinx's decoding and PTX's semantics: A's and B's selects at 36 and 28 with their signs at 48 and 49
               (the shifts' B is unsigned, and bit 49 wraps their count), B a 16-bit immediate when bit 50 is clear,
               a signed destination at 54 and saturate at 55 (VSET's comparison is there instead), the secondary
               operation at 51-53, and VADD's negation/plus-one mode at 56-57 */
            int shift = op == O_VSHL || op == O_VSHR;
            u32 asel = B(w, 36, 3) | (B(w, 48, 1) << 3);
            u32 bsel = B(w, 28, 3) | (shift ? 0 : B(w, 49, 1) << 3);
            u32 left = video_operand(c, i32_type, i64_type, u64_type, reg_read(c, a), asel);
            u32 right = B(w, 50, 1)
                            ? video_operand(c, i32_type, i64_type, u64_type, reg_read(c, b), bsel)
                            : video_operand(c, i32_type, i64_type, u64_type,
                                            spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 16)), 4 | (bsel & 8));
            u32 zero64 = spv_emit_constant64(&c->emit, i64_type, 0), one64 = spv_emit_constant64(&c->emit, i64_type, 1);
            u32 result;
            if (op == O_VADD) {
                u32 mode = B(w, 56, 2);
                result = mode == 1   ? spv_emit_isub_u32(&c->emit, i64_type, left, right)
                         : mode == 2 ? spv_emit_isub_u32(&c->emit, i64_type, right, left)
                                     : spv_emit_iadd_u32(&c->emit, i64_type, left, right);
                if (mode == 3) result = spv_emit_iadd_u32(&c->emit, i64_type, result, one64);
            } else if (op == O_VABSDIFF) {
                u32 difference = spv_emit_isub_u32(&c->emit, i64_type, left, right);
                result =
                    spv_emit_select(&c->emit, i64_type, spv_emit_s_greater(&c->emit, c->bool_type, zero64, difference),
                                    spv_emit_isub_u32(&c->emit, i64_type, zero64, difference), difference);
            } else if (op == O_VSET) {
                u32 compare = B(w, 54, 3);
                u32 less = spv_emit_s_greater(&c->emit, c->bool_type, right, left);
                u32 greater = spv_emit_s_greater(&c->emit, c->bool_type, left, right);
                u32 equal = spv_emit_i_equal(&c->emit, c->bool_type, left, right);
                u32 test = compare == 0   ? c->false_value
                           : compare == 1 ? less
                           : compare == 2 ? equal
                           : compare == 3 ? cc_not(c, greater)
                           : compare == 4 ? greater
                           : compare == 5 ? cc_not(c, equal)
                           : compare == 6 ? cc_not(c, less)
                                          : c->true_value;
                result = spv_emit_select(&c->emit, i64_type, test, one64, zero64);
            } else {
                /* the count wraps to 5 bits (bit 49) or clamps at 32; VSHR fills with A's sign when A is signed */
                u32 count = B(w, 49, 1)
                                ? spv_emit_bitwise_and_u32(&c->emit, i64_type, right,
                                                           spv_emit_constant64(&c->emit, i64_type, 31))
                                : spv_emit_select(&c->emit, i64_type,
                                                  spv_emit_s_greater(&c->emit, c->bool_type, right,
                                                                     spv_emit_constant64(&c->emit, i64_type, 32)),
                                                  spv_emit_constant64(&c->emit, i64_type, 32), right);
                result = op == O_VSHL  ? spv_emit_shift_left_u32(&c->emit, i64_type, left, count)
                         : B(w, 48, 1) ? spv_emit_shift_right_s32(&c->emit, i64_type, left, count)
                                       : spv_emit_shift_right_u32(&c->emit, i64_type, left, count);
            }
            reg_write(c, d,
                      video_finish(c, i32_type, i64_type, u64_type, result, reg_read(c, B(w, 39, 8)), B(w, 51, 3),
                                   op != O_VSET && B(w, 55, 1), op != O_VSET && B(w, 54, 1),
                                   op == O_VSHL && !B(w, 48, 1)));
        } else if (op >= O_PRMT_R && op <= O_PRMT_I) {
            /* Bytes 0-3 are Ra's and 4-7 C's (Rc at bit 39, or the constant buffer word in the RC form). The selector
               (Rb, Rc in the RC form, the constant buffer word, or the immediate) picks each result byte: in the
               index mode (0) its nibble i, whose bit 3 replicates the byte's sign; in the others by c, its low two
               bits, as PTX's prmt defines F4E (1), B4E (2), RC8 (3), ECL (4), ECR (5) and RC16 (6). */
            u32 low = reg_read(c, a);
            u32 selector = op == O_PRMT_R    ? reg_read(c, b)
                           : op == O_PRMT_RC ? reg_read(c, B(w, 39, 8))
                           : op == O_PRMT_CR ? cbuf_word(c, w)
                                             : spv_emit_constant_u32(&c->emit, c->u32_type,
                                                                     B(w, 20, 19) | (B(w, 56, 1) ? 0xfff80000u : 0));
            u32 high = op == O_PRMT_RC ? cbuf_word(c, w) : reg_read(c, B(w, 39, 8));
            u32 mode = B(w, 48, 3), result = c->zero;
            u32 three = spv_emit_constant_u32(&c->emit, c->u32_type, 3);
            u32 sel = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, selector, three);
            for (u32 k = 0; k < 4; k++) {
                u32 position = spv_emit_constant_u32(&c->emit, c->u32_type, k), idx, byte;
                u32 nibble = 0;
                switch (mode) {
                    case 0:
                        nibble = spv_emit_bitwise_and_u32(
                            &c->emit, c->u32_type,
                            spv_emit_shift_right_u32(&c->emit, c->u32_type, selector,
                                                     spv_emit_constant_u32(&c->emit, c->u32_type, 4 * k)),
                            spv_emit_constant_u32(&c->emit, c->u32_type, 15));
                        idx = nibble;
                        break;
                    case 1: idx = spv_emit_iadd_u32(&c->emit, c->u32_type, sel, position); break;
                    case 2: idx = spv_emit_isub_u32(&c->emit, c->u32_type, sel, position); break;
                    case 3: idx = sel; break;
                    case 4:
                        idx = spv_emit_select(&c->emit, c->u32_type,
                                              spv_emit_u_less(&c->emit, c->bool_type, sel, position), position, sel);
                        break;
                    case 5:
                        idx = spv_emit_select(&c->emit, c->u32_type,
                                              spv_emit_u_less(&c->emit, c->bool_type, sel, position), sel, position);
                        break;
                    default:
                        idx = spv_emit_iadd_u32(
                            &c->emit, c->u32_type,
                            spv_emit_shift_left_u32(
                                &c->emit, c->u32_type,
                                spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sel,
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 1)),
                                spv_emit_constant_u32(&c->emit, c->u32_type, 1)),
                            spv_emit_constant_u32(&c->emit, c->u32_type, k & 1));
                        break;
                }
                byte = prmt_byte(c, low, high, idx);
                if (mode == 0) { /* a nibble's bit 3 replicates the byte's sign across it */
                    u32 replicate =
                        spv_emit_i_not_equal(&c->emit, c->bool_type,
                                             spv_emit_bitwise_and_u32(&c->emit, c->u32_type, nibble,
                                                                      spv_emit_constant_u32(&c->emit, c->u32_type, 8)),
                                             c->zero);
                    u32 negative = spv_emit_i_not_equal(
                        &c->emit, c->bool_type,
                        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, byte,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 0x80)),
                        c->zero);
                    byte = spv_emit_select(&c->emit, c->u32_type, replicate,
                                           spv_emit_select(&c->emit, c->u32_type, negative,
                                                           spv_emit_constant_u32(&c->emit, c->u32_type, 0xff), c->zero),
                                           byte);
                }
                result = spv_emit_bitwise_or_u32(
                    &c->emit, c->u32_type, result,
                    spv_emit_shift_left_u32(&c->emit, c->u32_type, byte,
                                            spv_emit_constant_u32(&c->emit, c->u32_type, 8 * k)));
            }
            reg_write(c, d, result);
        } else if (op == O_ATOM_CAS) {
            u32 slot = gb_at[i] - 1;
            u32 pointer = global_atomic_pointer(c, stage, slot, gbuf_off[slot], a, w);
            reg_write(c, d,
                      spv_emit_atomic_compare_exchange_u32(&c->emit, c->u32_type, pointer, reg_read(c, b + 1),
                                                           reg_read(c, b)));
        } else if (op == O_ATOMS_CAS) {
            u32 index = shared_atomic_index(c, a, w, shared_words);
            u32 pointer = spv_emit_function_element_pointer(&c->emit, shared_element_pointer, shared_array, index);
            reg_write(c, d,
                      spv_emit_atomic_compare_exchange_shared_u32(&c->emit, c->u32_type, pointer, reg_read(c, b + 1),
                                                                  reg_read(c, b)));
        } else if (op == O_RED || op == O_ATOM) {
            u32 slot = gb_at[i] - 1;
            u32 pointer = global_atomic_pointer(c, stage, slot, gbuf_off[slot], a, w);
            u32 value = reg_read(c, op == O_RED ? d : b);
            u32 atomic_opcode = global_atomic_opcode(w, op == O_RED);
            u32 prior;
            if (atomic_opcode != 0xffffffffu)
                prior = spv_emit_atomic_u32(&c->emit, c->u32_type, atomic_opcode, pointer, value);
            else {
                u32 operation = op == O_RED ? B(w, 23, 3) : B(w, 52, 4);
                u32 size = op == O_RED ? B(w, 20, 3) : B(w, 49, 3);
                u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
                u32 cont = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
                spv_emit_store(&c->emit, atomic_old_var, spv_emit_load(&c->emit, c->u32_type, pointer));
                spv_emit_branch(&c->emit, header);
                spv_emit_label(&c->emit, header);
                spv_emit_loop_merge(&c->emit, merge, cont);
                spv_emit_branch(&c->emit, body);
                spv_emit_label(&c->emit, body);
                u32 old = spv_emit_load(&c->emit, c->u32_type, atomic_old_var), desired;
                if (size == 3) {
                    u32 sum = spv_emit_fadd(&c->emit, c->f32_type, spv_emit_bitcast(&c->emit, c->f32_type, old),
                                            spv_emit_bitcast(&c->emit, c->f32_type, value));
                    desired = spv_emit_bitcast(&c->emit, c->u32_type, sum);
                } else if (operation == 3) {
                    u32 increment =
                        spv_emit_iadd_u32(&c->emit, c->u32_type, old, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
                    u32 greater_equal = cc_not(c, spv_emit_u_less(&c->emit, c->bool_type, old, value));
                    desired = spv_emit_select(&c->emit, c->u32_type, greater_equal, c->zero, increment);
                } else {
                    u32 decrement =
                        spv_emit_isub_u32(&c->emit, c->u32_type, old, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
                    u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, old, c->zero);
                    u32 greater = spv_emit_u_less(&c->emit, c->bool_type, value, old);
                    desired = spv_emit_select(&c->emit, c->u32_type, cc_or(c, zero, greater), value, decrement);
                }
                u32 observed = spv_emit_atomic_compare_exchange_u32(&c->emit, c->u32_type, pointer, desired, old);
                spv_emit_store(&c->emit, atomic_old_var, observed);
                u32 done = spv_emit_i_equal(&c->emit, c->bool_type, observed, old);
                spv_emit_branch_conditional(&c->emit, done, merge, cont);
                spv_emit_label(&c->emit, cont);
                spv_emit_branch(&c->emit, header);
                spv_emit_label(&c->emit, merge);
                prior = spv_emit_load(&c->emit, c->u32_type, atomic_old_var);
            }
            if (op == O_ATOM) reg_write(c, d, prior);
        } else if (op == O_PSET) {
            u32 pa = pred_read(c, B(w, 12, 3), B(w, 15, 1));
            u32 pb = pred_read(c, B(w, 29, 3), B(w, 32, 1));
            u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 test = bool_combine(c, B(w, 45, 2), bool_combine(c, B(w, 24, 2), pa, pb), pc);
            u32 value = spv_emit_select(
                &c->emit, c->u32_type, test,
                spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 44, 1) ? 0x3f800000u : 0xffffffffu), c->zero);
            if (B(w, 47, 1)) flags_logic(c, value);
            reg_write(c, d, value);
        } else if (op == O_PSETP) {
            u32 pa = pred_read(c, B(w, 12, 3), B(w, 15, 1));
            u32 pb = pred_read(c, B(w, 29, 3), B(w, 32, 1));
            u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 first = bool_combine(c, B(w, 24, 2), pa, pb);
            u32 second = bool_combine(c, B(w, 24, 2), spv_emit_bool_not(&c->emit, c->bool_type, pa), pb);
            pred_write(c, B(w, 3, 3), bool_combine(c, B(w, 45, 2), first, pc));
            pred_write(c, B(w, 0, 3), bool_combine(c, B(w, 45, 2), second, pc));
        } else if (op == O_CSETP || op == O_CSET) {
            u32 test = cc_test(c, B(w, 8, 5));
            u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 yes = bool_combine(c, B(w, 45, 2), test, condition);
            if (op == O_CSET) {
                u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 44, 1) ? 0x3f800000u : 0xffffffffu);
                u32 result = spv_emit_select(&c->emit, c->u32_type, yes, truth, c->zero);
                if (B(w, 47, 1)) flags_logic(c, result);
                reg_write(c, d, result);
            } else {
                pred_write(c, B(w, 3, 3), yes);
                pred_write(c, B(w, 0, 3), bool_combine(c, B(w, 45, 2), cc_not(c, test), condition));
            }
        } else if (op >= O_P2R_R && op <= O_P2R_I) {
            u32 mask = integer_right(c, w, op - O_P2R_R);
            if (op != O_P2R_I)
                mask = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, mask,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, 255));
            u32 bits = c->zero;
            u32 flags[4] = {VAR_FLAG, VAR_FLAG + 1, VAR_FLAG + 2, VAR_FLAG + 3}; /* z, s, c, o */
            for (u32 p = 0; p < (B(w, 40, 1) ? 4u : 7u); p++) {
                u32 value = B(w, 40, 1) ? var_read(c, flags[p]) : pred_read(c, p, 0);
                u32 bit = spv_emit_select(&c->emit, c->u32_type, value,
                                          spv_emit_constant_u32(&c->emit, c->u32_type, 1u << p), c->zero);
                bits = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, bits, bit);
            }
            u32 shift = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 41, 2) * 8);
            u32 shifted_mask = spv_emit_shift_left_u32(&c->emit, c->u32_type, mask, shift);
            u32 base = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reg_read(c, a),
                                                spv_emit_not_u32(&c->emit, c->u32_type, shifted_mask));
            u32 picked = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, bits, mask);
            u32 shifted = spv_emit_shift_left_u32(&c->emit, c->u32_type, picked, shift);
            reg_write(c, d, spv_emit_bitwise_or_u32(&c->emit, c->u32_type, base, shifted));
        } else if (op >= O_R2P_R && op <= O_R2P_I) {
            u32 source = reg_read(c, a);
            if (B(w, 41, 2))
                source = spv_emit_shift_right_u32(&c->emit, c->u32_type, source,
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 41, 2) * 8));
            u32 mask = integer_right(c, w, op - O_R2P_R);
            u32 flags[4] = {VAR_FLAG, VAR_FLAG + 1, VAR_FLAG + 2, VAR_FLAG + 3}; /* z, s, c, o */
            for (u32 p = 0; p < (B(w, 40, 1) ? 4u : 7u); p++) {
                u32 enabled = spv_emit_i_not_equal(
                    &c->emit, c->bool_type,
                    spv_emit_bitwise_and_u32(&c->emit, c->u32_type, mask,
                                             spv_emit_constant_u32(&c->emit, c->u32_type, 1u << p)),
                    c->zero);
                u32 bit = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, source,
                                                   spv_emit_constant_u32(&c->emit, c->u32_type, 1u << p));
                u32 value = spv_emit_i_not_equal(&c->emit, c->bool_type, bit, c->zero);
                u32 v = B(w, 40, 1) ? flags[p] : VAR_PRED + p;
                var_write(c, v, spv_emit_select(&c->emit, c->bool_type, enabled, value, var_read(c, v)));
            }
        } else if (op == O_ISETP_R || op == O_ISETP_C || op == O_ISETP_I) {
            u32 left = reg_read(c, a), right = integer_right(c, w, op - O_ISETP_R);
            u32 test = integer_compare(c, i32_type, w, left, right);
            u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 yes = bool_combine(c, B(w, 45, 2), test, condition);
            u32 no = bool_combine(c, B(w, 45, 2), spv_emit_bool_not(&c->emit, c->bool_type, test), condition);
            pred_write(c, B(w, 3, 3), yes);
            pred_write(c, B(w, 0, 3), no);
        } else if (op == O_ISET_R || op == O_ISET_C || op == O_ISET_I) {
            u32 left = reg_read(c, a), right = integer_right(c, w, op - O_ISET_R);
            u32 test = integer_compare(c, i32_type, w, left, right);
            u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            test = bool_combine(c, B(w, 45, 2), test, condition);
            u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 44, 1) ? 0x3f800000u : 0xffffffffu);
            u32 result = spv_emit_select(&c->emit, c->u32_type, test, truth, c->zero);
            if (B(w, 47, 1)) flags_logic(c, result);
            reg_write(c, d, result);
        } else if (op == O_IMNMX_R || op == O_IMNMX_C || op == O_IMNMX_I) {
            u32 left = reg_read(c, a), right = integer_right(c, w, op - O_IMNMX_R);
            u32 less = B(w, 48, 1)
                           ? spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, right),
                                                spv_emit_bitcast(&c->emit, i32_type, left))
                           : spv_emit_u_less(&c->emit, c->bool_type, left, right);
            u32 min_value = spv_emit_select(&c->emit, c->u32_type, less, left, right);
            u32 max_value = spv_emit_select(&c->emit, c->u32_type, less, right, left);
            u32 choose_min = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 value = spv_emit_select(&c->emit, c->u32_type, choose_min, min_value, max_value);
            if (B(w, 47, 1)) flags_logic(c, value);
            reg_write(c, d, value);
        } else if (op == O_VSETP) {
            /* Each operand is independently sign- or zero-extended to 33 bits before comparing. */
            u32 left = vm_select(c, i32_type, reg_read(c, a), B(w, 36, 3) | (B(w, 48, 1) << 3));
            u32 right = B(w, 50, 1) ? vm_select(c, i32_type, reg_read(c, b), B(w, 28, 3) | (B(w, 49, 1) << 3))
                                    : vm_select(c, i32_type, spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 16)),
                                                4 | (B(w, 49, 1) << 3));
            u32 left64 = B(w, 48, 1)
                             ? spv_emit_int_convert(&c->emit, i64_type, spv_emit_bitcast(&c->emit, i32_type, left), 1)
                             : spv_emit_bitcast(&c->emit, i64_type, spv_emit_int_convert(&c->emit, u64_type, left, 0));
            u32 right64 =
                B(w, 49, 1) ? spv_emit_int_convert(&c->emit, i64_type, spv_emit_bitcast(&c->emit, i32_type, right), 1)
                            : spv_emit_bitcast(&c->emit, i64_type, spv_emit_int_convert(&c->emit, u64_type, right, 0));
            u32 less = spv_emit_s_greater(&c->emit, c->bool_type, right64, left64);
            u32 greater = spv_emit_s_greater(&c->emit, c->bool_type, left64, right64);
            u32 equal = spv_emit_i_equal(&c->emit, c->bool_type, left64, right64);
            u32 cmp = B(w, 43, 2) | (B(w, 47, 1) << 2);
            u32 result = cmp == 0   ? c->false_value
                         : cmp == 1 ? less
                         : cmp == 2 ? equal
                         : cmp == 3 ? cc_not(c, greater)
                         : cmp == 4 ? greater
                         : cmp == 5 ? cc_not(c, equal)
                         : cmp == 6 ? cc_not(c, less)
                                    : c->true_value;
            u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            pred_write(c, B(w, 3, 3), bool_combine(c, B(w, 45, 2), result, condition));
            pred_write(c, B(w, 0, 3), bool_combine(c, B(w, 45, 2), cc_not(c, result), condition));
        } else if (op == O_VMAD) {
            u32 left = vm_select(c, i32_type, reg_read(c, a), B(w, 36, 3) | (B(w, 48, 1) << 3));
            u32 right = B(w, 50, 1) ? vm_select(c, i32_type, reg_read(c, b), B(w, 28, 3) | (B(w, 49, 1) << 3))
                                    : vm_select(c, i32_type, spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 16)),
                                                4 | (B(w, 49, 1) << 3));
            u32 product = spv_emit_imul_u32(&c->emit, c->u32_type, left, right);
            reg_write(c, d, spv_emit_iadd_u32(&c->emit, c->u32_type, product, reg_read(c, B(w, 39, 8))));
        } else if (op == O_VMNMX) {
            u32 asel = B(w, 36, 3) | (B(w, 48, 1) << 3);
            u32 bsel = B(w, 28, 3) | (B(w, 49, 1) << 3);
            int sa = asel >> 3, sb = bsel >> 3, maximum = B(w, 56, 1);
            u32 va = vm_select(c, i32_type, reg_read(c, a), asel);
            u32 vb = B(w, 50, 1) ? vm_select(c, i32_type, reg_read(c, b), bsel)
                                 : spv_emit_constant_u32(&c->emit, c->u32_type,
                                                         sb ? (u32)(i32)(i16)B(w, 20, 16) : B(w, 20, 16));
            u32 less;
            if (sa == sb) {
                less = sa ? spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, vb),
                                               spv_emit_bitcast(&c->emit, i32_type, va))
                          : spv_emit_u_less(&c->emit, c->bool_type, va, vb);
            } else if (sa) {
                u32 negative = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                  spv_emit_bitcast(&c->emit, i32_type, va));
                less = spv_emit_bool_binary(&c->emit, c->bool_type, 1, negative,
                                            spv_emit_u_less(&c->emit, c->bool_type, va, vb));
            } else {
                u32 nonnegative = spv_emit_bool_not(&c->emit, c->bool_type,
                                                    spv_emit_s_greater(&c->emit, c->bool_type,
                                                                       spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                                       spv_emit_bitcast(&c->emit, i32_type, vb)));
                less = spv_emit_bool_binary(&c->emit, c->bool_type, 0, nonnegative,
                                            spv_emit_u_less(&c->emit, c->bool_type, va, vb));
            }
            u32 result = spv_emit_select(&c->emit, c->u32_type, less, maximum ? vb : va, maximum ? va : vb);
            int result_signed = sa == sb ? sa : !maximum;
            int signed_format = B(w, 54, 1);
            if (B(w, 55, 1)) {
                if (signed_format && !result_signed) {
                    u32 cap = spv_emit_constant_u32(&c->emit, c->u32_type, 0x7fffffffu);
                    u32 above = spv_emit_u_less(&c->emit, c->bool_type, cap, result);
                    result = spv_emit_select(&c->emit, c->u32_type, above, cap, result);
                } else if (!signed_format && result_signed) {
                    u32 negative =
                        spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                           spv_emit_bitcast(&c->emit, i32_type, result));
                    result = spv_emit_select(&c->emit, c->u32_type, negative, c->zero, result);
                }
            }
            u32 third = reg_read(c, B(w, 39, 8));
            u32 mode = B(w, 51, 3);
            if (mode < 4) {
                u32 off = mode == 0 || mode == 3 ? 16 : 0;
                u32 width = mode == 2 || mode == 3 ? 8 : 16;
                result = spv_emit_bitfield_insert(&c->emit, c->u32_type, third, result,
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, off),
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, width));
            } else if (mode == 4) {
                result = spv_emit_iadd_u32(&c->emit, c->u32_type, result, third);
            } else if (mode == 5 || mode == 6) {
                u32 cmp = signed_format
                              ? spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, result),
                                                   spv_emit_bitcast(&c->emit, i32_type, third))
                              : spv_emit_u_less(&c->emit, c->bool_type, third, result);
                result =
                    spv_emit_select(&c->emit, c->u32_type, cmp, mode == 5 ? third : result, mode == 5 ? result : third);
            }
            reg_write(c, d, result);
        } else if (op == O_ICMP_R || op == O_ICMP_RC || op == O_ICMP_CR || op == O_ICMP_I) {
            u32 third = reg_read(c, B(w, 39, 8));
            u32 right = op == O_ICMP_RC || op == O_ICMP_CR ? cbuf_word(c, w)
                        : op == O_ICMP_I                   ? integer_right(c, w, 2)
                                                           : reg_read(c, b);
            u32 comparison = op == O_ICMP_RC ? right : third;
            u32 alternative = op == O_ICMP_RC ? third : right;
            /* ICMP has no .X: bit 43 is part of Rc */
            u32 test = integer_compare(c, i32_type, w & ~(1ull << 43), comparison, c->zero);
            reg_write(c, d, spv_emit_select(&c->emit, c->u32_type, test, reg_read(c, a), alternative));
        } else if (op == O_FCMP_R || op == O_FCMP_RC || op == O_FCMP_CR || op == O_FCMP_I) {
            u32 third = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, B(w, 39, 8)));
            u32 comparison = op == O_FCMP_RC ? cbuf_read(c, w) : third;
            u32 alternative = op == O_FCMP_RC   ? reg_read(c, B(w, 39, 8))
                              : op == O_FCMP_CR ? cbuf_word(c, w)
                              : op == O_FCMP_I  ? spv_emit_bitcast(&c->emit, c->u32_type, float_imm20(c, w))
                                                : reg_read(c, b);
            u32 test = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), comparison, zero_f);
            reg_write(c, d, spv_emit_select(&c->emit, c->u32_type, test, reg_read(c, a), alternative));
        } else if (op == O_BFI_R || op == O_BFI_RC || op == O_BFI_CR || op == O_BFI_I) {
            u32 base = op == O_BFI_RC ? cbuf_word(c, w) : reg_read(c, B(w, 39, 8));
            u32 packed = op == O_BFI_CR   ? cbuf_word(c, w)
                         : op == O_BFI_RC ? reg_read(c, B(w, 39, 8))
                         : op == O_BFI_I  ? integer_right(c, w, 2)
                                          : reg_read(c, b);
            u32 mask = spv_emit_constant_u32(&c->emit, c->u32_type, 255);
            u32 offset = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, packed, mask);
            u32 width =
                spv_emit_bitwise_and_u32(&c->emit, c->u32_type,
                                         spv_emit_shift_right_u32(&c->emit, c->u32_type, packed,
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 8)),
                                         mask);
            u32 thirty_two = spv_emit_constant_u32(&c->emit, c->u32_type, 32);
            u32 valid = spv_emit_u_less(&c->emit, c->bool_type, offset, thirty_two);
            u32 safe_offset = spv_emit_select(&c->emit, c->u32_type, valid, offset, c->zero);
            u32 remaining = spv_emit_isub_u32(&c->emit, c->u32_type, thirty_two, safe_offset);
            u32 fits = spv_emit_u_less(&c->emit, c->bool_type, width, remaining);
            width = spv_emit_select(&c->emit, c->u32_type, fits, width, remaining);
            u32 value = spv_emit_bitfield_insert(&c->emit, c->u32_type, base, reg_read(c, a), safe_offset, width);
            value = spv_emit_select(&c->emit, c->u32_type, valid, value, base);
            if (B(w, 47, 1)) flags_logic(c, value);
            reg_write(c, d, value);
        } else if (op == O_F2F_R || op == O_F2F_C || op == O_F2F_I) {
            /* Formats 1 F16, 2 F32 and 3 F64 (a register pair, or a constant buffer's or the immediate's double). An
               F64 on either side converts with the default rounding and no saturation, as Eden translates it.
               F64 to F16 passes through F32 and can round twice. */
            u32 dst_format = B(w, 8, 2), src_format = B(w, 10, 2);
            if (src_format == 3 || dst_format == 3) {
                u32 value;
                if (src_format == 3)
                    value = double_mod(c, double_source(c, vec2_u32, w, op - O_F2F_R), B(w, 49, 1), B(w, 45, 1));
                else {
                    value = conversion_source_f32(c, vec2, w, op - O_F2F_R, src_format);
                    value = float_mod(c, value, B(w, 49, 1), B(w, 45, 1));
                }
                if (dst_format == 3) {
                    if (src_format != 3)
                        value = spv_emit_fconvert(&c->emit, c->f64_type, value);
                    else if (B(w, 42, 1))
                        value =
                            spv_emit_glsl_unary(&c->emit, c->f64_type, c->glsl, conversion_rounds[B(w, 39, 2)], value);
                    double_write(c, vec2_u32, d, value);
                } else {
                    value = spv_emit_fconvert(&c->emit, c->f32_type, value);
                    reg_write(c, d,
                              dst_format == 1
                                  ? spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16,
                                                        spv_emit_vec2_construct(&c->emit, vec2, value, zero_f))
                                  : spv_emit_bitcast(&c->emit, c->u32_type, value));
                }
            } else {
                u32 src = conversion_source_f32(c, vec2, w, op - O_F2F_R, src_format);
                src = float_mod(c, src, B(w, 49, 1), B(w, 45, 1));
                /* Between two floats of one size, bit 42 makes it an integer rounding (39-40: even, floor, ceil,
                   trunc), F16 as well as F32: Unity's half-precision frac() is an F2F.F16.F16.FLOOR. Between sizes,
                   39-40 is the conversion's rounding, which this translation leaves to the host */
                if (dst_format == src_format && B(w, 42, 1))
                    src = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, conversion_rounds[B(w, 39, 2)], src);
                src = float_saturate(c, src, B(w, 50, 1));
                u32 result;
                if (dst_format == 1) {
                    u32 pair = spv_emit_vec2_construct(&c->emit, vec2, src,
                                                       spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f));
                    result = spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16, pair);
                } else
                    result = spv_emit_bitcast(&c->emit, c->u32_type, src);
                reg_write(c, d, result);
            }
        } else if (op == O_F2I_R || op == O_F2I_C || op == O_F2I_I) {
            /* The source is an F16, F32 or F64 (format 3, a pair), the result 16, 32 or 64 bits (3, a pair). Out of
               range values clamp to the result's range. NaN is 0 from F16 or F32 into 32 bits, and
               1 << (bits - 1) from F64 or into 64 bits, as PTX's cvt gives it. */
            u32 dst_format = B(w, 8, 2), src_format = B(w, 10, 2);
            int wide = src_format == 3, is_signed = (int)B(w, 12, 1);
            u32 ftype = wide ? c->f64_type : c->f32_type, src;
            if (wide)
                src = double_mod(c, double_source(c, vec2_u32, w, op - O_F2I_R), B(w, 49, 1), B(w, 45, 1));
            else {
                src = conversion_source_f32(c, vec2, w, op - O_F2I_R, src_format);
                src = float_mod(c, src, B(w, 49, 1), B(w, 45, 1));
            }
            src = spv_emit_glsl_unary(&c->emit, ftype, c->glsl, conversion_rounds[B(w, 39, 2)], src);
            if (dst_format == 3) {
                /* the clamp's bounds: the range's ends, the top one the largest float below 2^63 or 2^64 */
                u64 low_bits = is_signed ? (wide ? 0xc3e0000000000000ull : 0xdf000000ull) : 0;
                u64 high_bits = is_signed ? (wide ? 0x43dfffffffffffffull : 0x5effffffull)
                                          : (wide ? 0x43efffffffffffffull : 0x5f7fffffull);
                u64 limit_bits = is_signed ? (wide ? 0x43e0000000000000ull : 0x5f000000ull)
                                           : (wide ? 0x43f0000000000000ull : 0x5f800000ull);
                u32 low = wide ? spv_emit_constant64(&c->emit, ftype, low_bits)
                               : spv_emit_constant_u32(&c->emit, ftype, (u32)low_bits);
                u32 high = wide ? spv_emit_constant64(&c->emit, ftype, high_bits)
                                : spv_emit_constant_u32(&c->emit, ftype, (u32)high_bits);
                u32 limit = wide ? spv_emit_constant64(&c->emit, ftype, limit_bits)
                                 : spv_emit_constant_u32(&c->emit, ftype, (u32)limit_bits);
                u32 itype = is_signed ? i64_type : u64_type;
                u32 bounded = spv_emit_glsl_ternary(&c->emit, ftype, c->glsl, SPV_GLSL_F_CLAMP, src, low, high);
                u32 result = is_signed ? spv_emit_convert_f_to_s(&c->emit, itype, bounded)
                                       : spv_emit_convert_f_to_u(&c->emit, itype, bounded);
                result = spv_emit_select(
                    &c->emit, itype, spv_emit_fcompare(&c->emit, c->bool_type, 6, src, limit),
                    spv_emit_constant64(&c->emit, itype, is_signed ? 0x7fffffffffffffffull : ~0ull), result);
                result = spv_emit_select(&c->emit, itype, spv_emit_isnan(&c->emit, c->bool_type, src),
                                         spv_emit_constant64(&c->emit, itype, 1ull << 63), result);
                u32 pair = spv_emit_bitcast(&c->emit, vec2_u32, result);
                reg_write(c, d, spv_emit_vec4_extract(&c->emit, c->u32_type, pair, 0));
                if (d < 254) reg_write(c, d + 1, spv_emit_vec4_extract(&c->emit, c->u32_type, pair, 1));
            } else if (!is_signed) {
                u32 low = wide ? spv_emit_constant64(&c->emit, ftype, 0) : spv_emit_constant_f32(&c->emit, ftype, 0.0f);
                u32 high = wide ? spv_emit_constant64(&c->emit, ftype, 0x41efffffffe00000ull) /* 2^32 - 1 */
                                : spv_emit_constant_f32(&c->emit, ftype, 4294967040.0f);
                u32 bounded = spv_emit_glsl_ternary(&c->emit, ftype, c->glsl, SPV_GLSL_F_CLAMP, src, low, high);
                u32 result = spv_emit_convert_f_to_u(&c->emit, c->u32_type, bounded);
                u32 overflow = spv_emit_fcompare(&c->emit, c->bool_type, 6, src,
                                                 wide ? spv_emit_constant64(&c->emit, ftype, 0x41f0000000000000ull)
                                                      : spv_emit_constant_f32(&c->emit, ftype, 4294967296.0f));
                result = spv_emit_select(&c->emit, c->u32_type, overflow,
                                         spv_emit_constant_u32(&c->emit, c->u32_type, 0xffffffffu), result);
                result =
                    spv_emit_select(&c->emit, c->u32_type, spv_emit_isnan(&c->emit, c->bool_type, src),
                                    wide ? spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000u) : c->zero, result);
                if (dst_format == 1) {
                    u32 bound = spv_emit_constant_u32(&c->emit, c->u32_type, 65535);
                    u32 large = spv_emit_u_less(&c->emit, c->bool_type, bound, result);
                    result = spv_emit_select(&c->emit, c->u32_type, large, bound, result);
                    if (wide)
                        result = spv_emit_select(&c->emit, c->u32_type, spv_emit_isnan(&c->emit, c->bool_type, src),
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 0x8000), result);
                }
                reg_write(c, d, result);
            } else {
                u32 low = wide ? spv_emit_constant64(&c->emit, ftype, 0xc1e0000000000000ull) /* -2^31 */
                               : spv_emit_constant_f32(&c->emit, ftype, -2147483648.0f);
                u32 high = wide ? spv_emit_constant64(&c->emit, ftype, 0x41dfffffffc00000ull) /* 2^31 - 1 */
                                : spv_emit_constant_f32(&c->emit, ftype, 2147483520.0f);
                u32 bounded = spv_emit_glsl_ternary(&c->emit, ftype, c->glsl, SPV_GLSL_F_CLAMP, src, low, high);
                u32 result = spv_emit_convert_f_to_s(&c->emit, i32_type, bounded);
                u32 overflow = spv_emit_fcompare(&c->emit, c->bool_type, 6, src,
                                                 wide ? spv_emit_constant64(&c->emit, ftype, 0x41e0000000000000ull)
                                                      : spv_emit_constant_f32(&c->emit, ftype, 2147483648.0f));
                result = spv_emit_select(&c->emit, i32_type, overflow,
                                         spv_emit_constant_u32(&c->emit, i32_type, 0x7fffffff), result);
                result = spv_emit_select(&c->emit, i32_type, spv_emit_isnan(&c->emit, c->bool_type, src),
                                         spv_emit_constant_u32(&c->emit, i32_type, wide ? 0x80000000u : 0), result);
                if (dst_format == 1) { /* S16 saturates like U16 */
                    u32 top = spv_emit_constant_u32(&c->emit, i32_type, 32767);
                    u32 bottom = spv_emit_constant_u32(&c->emit, i32_type, 0xffff8000u);
                    result = spv_emit_select(&c->emit, i32_type,
                                             spv_emit_s_greater(&c->emit, c->bool_type, result, top), top, result);
                    result = spv_emit_select(
                        &c->emit, i32_type, spv_emit_s_greater(&c->emit, c->bool_type, bottom, result), bottom, result);
                }
                reg_write_as(c, d, result, i32_type);
            }
        } else if (op == O_I2I_R || op == O_I2I_C || op == O_I2I_I) {
            u32 value = integer_right(c, w, op - O_I2I_R);
            u32 src_size = B(w, 10, 2), dst_size = B(w, 8, 2);
            if (src_size < 2) {
                u32 offset = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 41, 3) * 8);
                u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 8u << src_size);
                value = B(w, 13, 1)
                            ? spv_emit_bitcast(&c->emit, c->u32_type,
                                               spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                          spv_emit_bitcast(&c->emit, i32_type, value),
                                                                          offset, width))
                            : spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, offset, width);
            }
            if (B(w, 49, 1)) {
                u32 negative = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                  spv_emit_bitcast(&c->emit, i32_type, value));
                u32 absolute = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, value);
                value = spv_emit_select(&c->emit, c->u32_type, negative, absolute, value);
            }
            if (B(w, 45, 1)) value = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, value);
            if (B(w, 50, 1) && dst_size < 2) {
                u32 high = (B(w, 12, 1) ? 0x7fffffffu : 0xffffffffu) >> (32 - (8u << dst_size));
                u32 hi = spv_emit_constant_u32(&c->emit, c->u32_type, high);
                if (B(w, 13, 1)) {
                    u32 negative =
                        spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                           spv_emit_bitcast(&c->emit, i32_type, value));
                    if (B(w, 12, 1)) {
                        u32 lo = spv_emit_constant_u32(&c->emit, c->u32_type, ~high);
                        u32 below = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, lo),
                                                       spv_emit_bitcast(&c->emit, i32_type, value));
                        value = spv_emit_select(&c->emit, c->u32_type, below, lo, value);
                    } else
                        value = spv_emit_select(&c->emit, c->u32_type, negative, c->zero, value);
                }
                u32 above = B(w, 13, 1) && B(w, 12, 1) ? spv_emit_s_greater(&c->emit, c->bool_type,
                                                                            spv_emit_bitcast(&c->emit, i32_type, value),
                                                                            spv_emit_bitcast(&c->emit, i32_type, hi))
                                                       : spv_emit_u_less(&c->emit, c->bool_type, hi, value);
                value = spv_emit_select(&c->emit, c->u32_type, above, hi, value);
            } else if (B(w, 50, 1) && B(w, 12, 1) != B(w, 13, 1)) {
                /* 32 bits saturate only across a change of signedness */
                if (B(w, 13, 1))
                    value = spv_emit_select(&c->emit, c->u32_type,
                                            spv_emit_s_greater(&c->emit, c->bool_type,
                                                               spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                               spv_emit_bitcast(&c->emit, i32_type, value)),
                                            c->zero, value);
                else {
                    u32 top = spv_emit_constant_u32(&c->emit, c->u32_type, 0x7fffffffu);
                    value = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, top, value),
                                            top, value);
                }
            }
            if (dst_size < 2) {
                u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 8u << dst_size);
                value = spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, c->zero, width);
            }
            if (B(w, 47, 1)) flags_logic(c, value);
            reg_write(c, d, value);
        } else if (op == O_I2F_R || op == O_I2F_C || op == O_I2F_I) {
            /* As Eden converts: abs on the integer at its source width (the most negative value stays), then neg on
               the floating result except for that value. A 64-bit source (format 3) is a register pair, a constant
               buffer's two words or the immediate sign-extended. F16 output passes through F32 and can round twice. */
            u32 size = B(w, 10, 2), dst_format = B(w, 8, 2);
            int is_signed = (int)B(w, 13, 1), absolute = (int)B(w, 49, 1), negate = (int)B(w, 45, 1);
            u32 target_type = dst_format == 3 ? c->f64_type : c->f32_type, result, least = 0;
            if (size == 3) {
                u32 low, high;
                if (op == O_I2F_R) {
                    low = reg_read(c, b);
                    high = reg_read(c, b < 254 ? b + 1 : 255);
                } else if (op == O_I2F_C) {
                    u32 bank = B(w, 34, 5), offset = B(w, 20, 14) * 4;
                    low = cbuf_read_dynamic(c, bank, spv_emit_constant_u32(&c->emit, c->u32_type, offset));
                    high = cbuf_read_dynamic(c, bank, spv_emit_constant_u32(&c->emit, c->u32_type, offset + 4));
                } else {
                    i32 immediate = (i32)(B(w, 20, 19) | (B(w, 56, 1) ? 0xfff80000u : 0));
                    low = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)immediate);
                    high = spv_emit_constant_u32(&c->emit, c->u32_type, immediate < 0 ? 0xffffffffu : 0);
                }
                u32 itype = is_signed ? i64_type : u64_type;
                u32 value = spv_emit_bitcast(&c->emit, itype, spv_emit_vec2_construct(&c->emit, vec2_u32, low, high));
                if (is_signed && negate && !absolute)
                    least = spv_emit_i_equal(&c->emit, c->bool_type, value,
                                             spv_emit_constant64(&c->emit, itype, 1ull << 63));
                if (is_signed && absolute)
                    value = spv_emit_select(
                        &c->emit, itype,
                        spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant64(&c->emit, itype, 0), value),
                        spv_emit_isub_u32(&c->emit, itype, spv_emit_constant64(&c->emit, itype, 0), value), value);
                result = is_signed ? spv_emit_convert_s_to_f(&c->emit, target_type, value)
                                   : spv_emit_convert_u_to_f(&c->emit, target_type, value);
            } else {
                u32 value = integer_right(c, w, op - O_I2F_R);
                u32 width = size == 0 ? 8 : size == 1 ? 16 : 32;
                if (size < 2) {
                    u32 offset = spv_emit_constant_u32(&c->emit, c->u32_type,
                                                       size == 0 ? B(w, 41, 2) * 8 : (B(w, 41, 2) & 2) * 8);
                    u32 bits = spv_emit_constant_u32(&c->emit, c->u32_type, width);
                    value = is_signed ? spv_emit_bitcast(&c->emit, c->u32_type,
                                                         spv_emit_bitfield_sextract(
                                                             &c->emit, i32_type,
                                                             spv_emit_bitcast(&c->emit, i32_type, value), offset, bits))
                                      : spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, offset, bits);
                }
                if (is_signed && negate && !absolute)
                    least = spv_emit_i_equal(&c->emit, c->bool_type, value,
                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0u - (1u << (width - 1))));
                if (is_signed && absolute) {
                    u32 min_value = spv_emit_constant_u32(&c->emit, c->u32_type, 0u - (1u << (width - 1)));
                    u32 negative =
                        spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                           spv_emit_bitcast(&c->emit, i32_type, value));
                    u32 minimum = spv_emit_i_equal(&c->emit, c->bool_type, value, min_value);
                    u32 negate_abs = spv_emit_bool_binary(&c->emit, c->bool_type, 0, negative,
                                                          spv_emit_bool_not(&c->emit, c->bool_type, minimum));
                    value = spv_emit_select(&c->emit, c->u32_type, negate_abs,
                                            spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, value), value);
                }
                result = is_signed ? spv_emit_convert_s_to_f(&c->emit, target_type,
                                                             spv_emit_bitcast(&c->emit, i32_type, value))
                                   : spv_emit_convert_u_to_f(&c->emit, target_type, value);
            }
            if (negate) {
                u32 negated = spv_emit_fnegate(&c->emit, target_type, result);
                result = least ? spv_emit_select(&c->emit, target_type, least, result, negated) : negated;
            }
            if (dst_format == 3)
                double_write(c, vec2_u32, d, result);
            else if (dst_format == 1) {
                u32 pair =
                    spv_emit_vec2_construct(&c->emit, vec2, result, spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f));
                reg_write(c, d, spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16, pair));
            } else
                reg_write_as(c, d, result, c->f32_type);
        } else if (op == O_ISCADD_R || op == O_ISCADD_C || op == O_ISCADD_I || op == O_ISCADD32I) {
            /* ISCADD32I: a 32-bit immediate, the shift in bits 53-57 and CC in bit 52, and no negation */
            int immediate32 = op == O_ISCADD32I;
            u32 left = reg_read(c, a);
            u32 avg = immediate32 ? 0 : B(w, 48, 2);
            u32 shifted =
                spv_emit_shift_left_u32(&c->emit, c->u32_type, left,
                                        spv_emit_constant_u32(&c->emit, c->u32_type, B(w, immediate32 ? 53 : 39, 5)));
            u32 rhs = immediate32 ? spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32))
                                  : integer_right(c, w, op - O_ISCADD_R);
            reg_write(c, d, integer_add(c, shifted, rhs, avg, 0, B(w, immediate32 ? 52 : 47, 1)));
        } else if (op >= O_BFE_R && op <= O_BFE_I) {
            u32 packed = integer_right(c, w, op - O_BFE_R);
            u32 source = reg_read(c, a);
            if (B(w, 40, 1)) source = spv_emit_bit_reverse_u32(&c->emit, c->u32_type, source);
            u32 raw_offset = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, packed,
                                                      spv_emit_constant_u32(&c->emit, c->u32_type, 255));
            u32 thirty_one = spv_emit_constant_u32(&c->emit, c->u32_type, 31);
            u32 offset =
                spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, thirty_one, raw_offset),
                                thirty_one, raw_offset);
            u32 raw_width =
                spv_emit_bitwise_and_u32(&c->emit, c->u32_type,
                                         spv_emit_shift_right_u32(&c->emit, c->u32_type, packed,
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 8)),
                                         spv_emit_constant_u32(&c->emit, c->u32_type, 255));
            u32 remaining =
                spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_constant_u32(&c->emit, c->u32_type, 32), offset);
            u32 width =
                spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, remaining, raw_width),
                                remaining, raw_width);
            u32 nonzero = spv_emit_i_not_equal(&c->emit, c->bool_type, width, c->zero);
            if (!B(w, 48, 1))
                nonzero = cc_and(c, nonzero,
                                 spv_emit_u_less(&c->emit, c->bool_type, raw_offset,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 32)));
            u32 safe_width =
                spv_emit_select(&c->emit, c->u32_type, nonzero, width, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
            u32 extracted =
                B(w, 48, 1) ? spv_emit_bitcast(&c->emit, c->u32_type,
                                               spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                          spv_emit_bitcast(&c->emit, i32_type, source),
                                                                          offset, safe_width))
                            : spv_emit_bitfield_uextract(&c->emit, c->u32_type, source, offset, safe_width);
            u32 value = spv_emit_select(&c->emit, c->u32_type, nonzero, extracted, c->zero);
            if (B(w, 47, 1)) flags_logic(c, value); /* BFE.CC, as Eden writes it */
            reg_write(c, d, value);
        } else if (op == O_SHL_R || op == O_SHL_C || op == O_SHL_I) {
            u32 shift = integer_right(c, w, op - O_SHL_R);
            u32 safe = B(w, 39, 1) ? spv_emit_bitwise_and_u32(&c->emit, c->u32_type, shift,
                                                              spv_emit_constant_u32(&c->emit, c->u32_type, 31))
                                   : shift;
            u32 value = spv_emit_shift_left_u32(&c->emit, c->u32_type, reg_read(c, a), safe);
            if (!B(w, 39, 1)) {
                u32 bound = spv_emit_constant_u32(&c->emit, c->u32_type, 32);
                u32 valid = spv_emit_u_less(&c->emit, c->bool_type, shift, bound);
                value = spv_emit_select(&c->emit, c->u32_type, valid, value, c->zero);
            }
            reg_write(c, d, value);
        } else if (op == O_LEA_LR || op == O_LEA_LC || op == O_LEA_LI || op == O_LEA_HR || op == O_LEA_HC) {
            int high_form = op == O_LEA_HR || op == O_LEA_HC;
            u32 shift = high_form ? B(w, op == O_LEA_HR ? 28 : 51, 5) : B(w, 39, 5);
            int negative = high_form ? B(w, op == O_LEA_HR ? 37 : 56, 1) : B(w, 45, 1);
            u32 source = reg_read(c, a);
            u32 rhs = op == O_LEA_LR || op == O_LEA_HR ? reg_read(c, b)
                      : op == O_LEA_LI                 ? integer_right(c, w, 2)
                                                       : integer_right(c, w, 1);
            u32 value;
            int extended;
            u32 old_carry;
            u32 sum;
            if (high_form) {
                u32 hi = reg_read(c, B(w, 39, 8));
                value = shift ? spv_emit_bitwise_or_u32(
                                    &c->emit, c->u32_type,
                                    spv_emit_shift_right_u32(&c->emit, c->u32_type, source,
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 32 - shift)),
                                    spv_emit_shift_left_u32(&c->emit, c->u32_type, hi,
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, shift)))
                              : hi;
                if (negative) {
                    u32 carry =
                        spv_emit_i_equal(&c->emit, c->bool_type,
                                         spv_emit_shift_left_u32(&c->emit, c->u32_type, source,
                                                                 spv_emit_constant_u32(&c->emit, c->u32_type, shift)),
                                         c->zero);
                    value =
                        spv_emit_iadd_u32(&c->emit, c->u32_type, spv_emit_not_u32(&c->emit, c->u32_type, value),
                                          spv_emit_select(&c->emit, c->u32_type, carry,
                                                          spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero));
                }
            } else {
                if (negative) source = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, source);
                value = spv_emit_shift_left_u32(&c->emit, c->u32_type, source,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, shift));
            }
            /* .X adds the carry in (LO at 46, HI at 38 or 57), .CC writes the flags, and LO's predicate takes the
               add's carry out: LEA.LO ... P0 then LEA.HI.X is how a 64-bit address is formed */
            extended = B(w, op == O_LEA_HR ? 38 : op == O_LEA_HC ? 57 : 46, 1);
            old_carry = extended ? flag_read(c, c->cc_c) : 0;
            sum = integer_add(c, value, rhs, 0, extended, B(w, 47, 1));
            reg_write(c, d, sum);
            if (B(w, 48, 3) < 7) {
                u32 carry = spv_emit_u_less(&c->emit, c->bool_type, sum, value);
                if (extended)
                    carry = spv_emit_bool_binary(
                        &c->emit, c->bool_type, 1, carry,
                        spv_emit_bool_binary(&c->emit, c->bool_type, 0, old_carry,
                                             spv_emit_i_equal(&c->emit, c->bool_type, sum, value)));
                pred_write(c, B(w, 48, 3), carry);
            }
        } else if (op == O_SHF_LR || op == O_SHF_LI || op == O_SHF_RR || op == O_SHF_RI) {
            int left_shift = op == O_SHF_LR || op == O_SHF_LI;
            u32 shift = (op == O_SHF_LR || op == O_SHF_RR) ? reg_read(c, b)
                                                           : spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 19));
            u32 longest = B(w, 37, 2) >= 2;
            u32 limit = spv_emit_constant_u32(&c->emit, c->u32_type, longest ? 63 : 32);
            u32 wrapped = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, shift,
                                                   spv_emit_constant_u32(&c->emit, c->u32_type, longest ? 63 : 31));
            u32 safe = B(w, 50, 1)
                           ? wrapped
                           : spv_emit_select(&c->emit, c->u32_type,
                                             spv_emit_u_less(&c->emit, c->bool_type, limit, shift), limit, shift);
            u32 low = reg_read(c, a), high = reg_read(c, B(w, 39, 8));
            u32 small =
                spv_emit_bitwise_and_u32(&c->emit, c->u32_type, safe, spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            u32 left = spv_emit_shift_left_u32(&c->emit, c->u32_type, high, small);
            u32 reverse =
                spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_constant_u32(&c->emit, c->u32_type, 32), safe);
            reverse = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reverse,
                                               spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            u32 right = spv_emit_shift_right_u32(&c->emit, c->u32_type, low, reverse);
            u32 combined = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, left, right);
            if (left_shift)
                combined = spv_emit_select(&c->emit, c->u32_type,
                                           spv_emit_i_equal(&c->emit, c->bool_type, safe, c->zero), high, combined);
            else {
                u32 low_part = spv_emit_shift_right_u32(&c->emit, c->u32_type, low, small);
                u32 high_part = spv_emit_shift_left_u32(&c->emit, c->u32_type, high, reverse);
                combined = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, low_part, high_part);
                combined = spv_emit_select(&c->emit, c->u32_type,
                                           spv_emit_i_equal(&c->emit, c->bool_type, safe, c->zero), low, combined);
            }
            u32 long_shift =
                spv_emit_isub_u32(&c->emit, c->u32_type, safe, spv_emit_constant_u32(&c->emit, c->u32_type, 32));
            long_shift = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, long_shift,
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            u32 long_value =
                left_shift ? spv_emit_shift_left_u32(&c->emit, c->u32_type, low, long_shift)
                : B(w, 37, 2) == 3
                    ? spv_emit_bitcast(&c->emit, c->u32_type,
                                       spv_emit_shift_right_s32(&c->emit, i32_type,
                                                                spv_emit_bitcast(&c->emit, i32_type, high), long_shift))
                    : spv_emit_shift_right_u32(&c->emit, c->u32_type, high, long_shift);
            u32 result = spv_emit_select(
                &c->emit, c->u32_type,
                spv_emit_u_less(&c->emit, c->bool_type, safe, spv_emit_constant_u32(&c->emit, c->u32_type, 32)),
                combined, long_value);
            reg_write(c, d, result);
        } else if (op == O_SHR_R || op == O_SHR_C || op == O_SHR_I) {
            u32 shift = integer_right(c, w, op - O_SHR_R);
            u32 source = reg_read(c, a);
            if (B(w, 40, 1)) source = spv_emit_bit_reverse_u32(&c->emit, c->u32_type, source);
            u32 safe = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, shift,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            u32 value;
            if (B(w, 48, 1)) {
                if (!B(w, 39, 1)) {
                    u32 below = spv_emit_u_less(&c->emit, c->bool_type, shift,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, 31));
                    safe = spv_emit_select(&c->emit, c->u32_type, below, shift,
                                           spv_emit_constant_u32(&c->emit, c->u32_type, 31));
                }
                value = spv_emit_bitcast(
                    &c->emit, c->u32_type,
                    spv_emit_shift_right_s32(&c->emit, i32_type, spv_emit_bitcast(&c->emit, i32_type, source), safe));
            } else {
                value = spv_emit_shift_right_u32(&c->emit, c->u32_type, source, safe);
            }
            if (!B(w, 48, 1) && !B(w, 39, 1)) {
                u32 valid =
                    spv_emit_u_less(&c->emit, c->bool_type, shift, spv_emit_constant_u32(&c->emit, c->u32_type, 32));
                value = spv_emit_select(&c->emit, c->u32_type, valid, value, c->zero);
            }
            reg_write(c, d, value);
        } else if (op == O_FADD_R || op == O_FADD_C || op == O_FADD_I || op == O_FADD32I || op == O_FMUL_R ||
                   op == O_FMUL_C || op == O_FMUL_I || op == O_FMUL32I) {
            u32 left = ftz_read(c, insn, block_start, i, a, reg_read_as(c, a, c->f32_type));
            u32 right = op == O_FADD_C || op == O_FMUL_C     ? cbuf_read(c, w)
                        : op == O_FADD_I || op == O_FMUL_I   ? float_imm20(c, w)
                        : op == O_FADD32I || op == O_FMUL32I ? float_imm32(c, w)
                                                             : ftz_read(c, insn, block_start, i, b,
                                                                        reg_read_as(c, b, c->f32_type));
            if (op == O_FADD_R || op == O_FADD_C || op == O_FADD_I) {
                left = float_mod(c, left, B(w, 46, 1), B(w, 48, 1));
                right = float_mod(c, right, B(w, 49, 1), B(w, 45, 1));
            } else if (op == O_FADD32I) {
                left = float_mod(c, left, B(w, 54, 1), B(w, 56, 1));
                right = float_mod(c, right, B(w, 57, 1), B(w, 53, 1));
            } else if (op == O_FMUL_R || op == O_FMUL_C || op == O_FMUL_I) {
                right = float_mod(c, right, 0, B(w, 48, 1));
            }
            u32 mode = op == O_FADD32I                                      ? B(w, 55, 1)
                       : op == O_FMUL32I                                    ? B(w, 53, 2)
                       : op == O_FADD_R || op == O_FADD_C || op == O_FADD_I ? B(w, 44, 1)
                                                                            : B(w, 44, 2);
            int soft_ftz = (mode & 1) && c->soft_ftz_on;
            if (soft_ftz) {
                left = float_ftz(c, left);
                right = float_ftz(c, right);
            }
            u32 result = op == O_FADD_R || op == O_FADD_C || op == O_FADD_I || op == O_FADD32I
                             ? spv_emit_fadd(&c->emit, c->f32_type, left, right)
                             : spv_emit_fmul(&c->emit, c->f32_type, left, right);
            spv_emit_no_contraction(&c->emit, result);
            if (soft_ftz) result = float_ftz(c, result);
            /* FMZ (FMUL's 44-45, FMUL32I's 53-54 = 2): a zero factor makes the product 0, even with an infinity */
            if ((op == O_FMUL32I                                      ? B(w, 53, 2)
                 : op == O_FMUL_R || op == O_FMUL_C || op == O_FMUL_I ? B(w, 44, 2)
                                                                      : 0) == 2) {
                u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
                u32 left_zero = spv_emit_fcompare(&c->emit, c->bool_type, 2, left, zero);
                u32 right_zero = spv_emit_fcompare(&c->emit, c->bool_type, 2, right, zero);
                u32 either = spv_emit_bool_binary(&c->emit, c->bool_type, 1, left_zero, right_zero);
                result = spv_emit_select(&c->emit, c->f32_type, either, zero, result);
            }
            if (op == O_FMUL_R || op == O_FMUL_C || op == O_FMUL_I) {
                static const float scale[] = {1.0f, 0.5f, 0.25f, 0.125f, 8.0f, 4.0f, 2.0f};
                u32 sc = B(w, 41, 3);
                if (sc) {
                    u32 factor = spv_emit_constant_f32(&c->emit, c->f32_type, scale[sc]);
                    result = spv_emit_fmul(&c->emit, c->f32_type, result, factor);
                    spv_emit_no_contraction(&c->emit, result);
                }
            }
            int sat = op == O_FADD32I ? 0 : op == O_FMUL32I ? B(w, 55, 1) : B(w, 50, 1);
            result = float_saturate(c, result, sat);
            reg_write_as(c, d, result, c->f32_type);
        } else if (op == O_FFMA_R || op == O_FFMA_RC || op == O_FFMA_CR || op == O_FFMA_I || op == O_FFMA32I) {
            u32 left = ftz_read(c, insn, block_start, i, a, reg_read_as(c, a, c->f32_type));
            u32 right = op == O_FFMA32I   ? float_imm32(c, w)
                        : op == O_FFMA_CR ? cbuf_read(c, w)
                        : op == O_FFMA_I
                            ? float_imm20(c, w)
                            : ftz_read(c, insn, block_start, i, op == O_FFMA_RC ? B(w, 39, 8) : b,
                                       reg_read_as(c, op == O_FFMA_RC ? B(w, 39, 8) : b, c->f32_type));
            u32 addend = op == O_FFMA_RC ? cbuf_read(c, w)
                                         : ftz_read(c, insn, block_start, i, op == O_FFMA32I ? d : B(w, 39, 8),
                                                    reg_read_as(c, op == O_FFMA32I ? d : B(w, 39, 8), c->f32_type));
            if (op == O_FFMA32I) {
                left = float_mod(c, left, 0, B(w, 56, 1));
                addend = float_mod(c, addend, 0, B(w, 57, 1));
            } else {
                right = float_mod(c, right, 0, B(w, 48, 1));
                addend = float_mod(c, addend, 0, B(w, 49, 1));
            }
            int soft_ftz = (B(w, 53, 2) & 1) && c->soft_ftz_on;
            if (soft_ftz) {
                left = float_ftz(c, left);
                right = float_ftz(c, right);
                addend = float_ftz(c, addend);
            }
            u32 result = spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FMA, left, right, addend);
            if (soft_ftz) result = float_ftz(c, result);
            if (B(w, 53, 2) == 2) {
                u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
                u32 left_zero = spv_emit_fcompare(&c->emit, c->bool_type, 2, left, zero);
                u32 right_zero = spv_emit_fcompare(&c->emit, c->bool_type, 2, right, zero);
                result = spv_emit_select(&c->emit, c->f32_type, cc_or(c, left_zero, right_zero), addend, result);
            }
            result = float_saturate(c, result, op == O_FFMA32I ? B(w, 55, 1) : B(w, 50, 1));
            reg_write_as(c, d, result, c->f32_type);
        } else if (op == O_FMNMX_R || op == O_FMNMX_C || op == O_FMNMX_I) {
            u32 left = reg_read_as(c, a, c->f32_type);
            u32 right = op == O_FMNMX_C   ? cbuf_read(c, w)
                        : op == O_FMNMX_I ? float_imm20(c, w)
                                          : reg_read_as(c, b, c->f32_type);
            left = float_mod(c, left, B(w, 46, 1), B(w, 48, 1));
            right = float_mod(c, right, B(w, 49, 1), B(w, 45, 1));
            u32 is_left_nan = spv_emit_isnan(&c->emit, c->bool_type, left);
            u32 is_right_nan = spv_emit_isnan(&c->emit, c->bool_type, right);
            u32 minimum = spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_MIN, left, right);
            u32 maximum = spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_MAX, left, right);
            u32 choose_min = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 value = spv_emit_select(&c->emit, c->f32_type, choose_min, minimum, maximum);
            value = spv_emit_select(&c->emit, c->f32_type, is_right_nan, left, value);
            value = spv_emit_select(&c->emit, c->f32_type, is_left_nan, right, value);
            reg_write_as(c, d, value, c->f32_type);
        } else if (op == O_FSETP_R || op == O_FSETP_C || op == O_FSETP_I) {
            u32 left = reg_read_as(c, a, c->f32_type);
            left = float_mod(c, left, B(w, 7, 1), B(w, 43, 1));
            u32 right;
            if (op == O_FSETP_C)
                right = cbuf_read(c, w);
            else if (op == O_FSETP_I)
                right = spv_emit_constant_u32(&c->emit, c->f32_type, (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31));
            else
                right = reg_read_as(c, b, c->f32_type);
            right = float_mod(c, right, B(w, 44, 1), B(w, 6, 1));
            u32 test = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), left, right);
            u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 op_bool = B(w, 45, 2);
            u32 yes = bool_combine(c, op_bool, test, pc);
            u32 inverse = spv_emit_bool_not(&c->emit, c->bool_type, test);
            u32 no = bool_combine(c, op_bool, inverse, pc);
            pred_write(c, B(w, 3, 3), yes);
            pred_write(c, B(w, 0, 3), no);
        } else if ((op >= O_DADD_R && op <= O_DADD_I) || (op >= O_DMUL_R && op <= O_DMUL_I) ||
                   (op >= O_DFMA_R && op <= O_DFMA_I) || (op >= O_DMNMX_R && op <= O_DMNMX_I)) {
            u32 left = double_read(c, vec2_u32, a);
            u32 right = op == O_DFMA_RC                      ? double_read(c, vec2_u32, B(w, 39, 8))
                        : op == O_DFMA_CR                    ? double_source(c, vec2_u32, w, 1)
                        : op >= O_DFMA_R && op <= O_DFMA_I   ? double_source(c, vec2_u32, w, op == O_DFMA_I ? 2 : 0)
                        : (op >= O_DADD_R && op <= O_DADD_I) ? double_source(c, vec2_u32, w, op - O_DADD_R)
                        : (op >= O_DMUL_R && op <= O_DMUL_I) ? double_source(c, vec2_u32, w, op - O_DMUL_R)
                                                             : double_source(c, vec2_u32, w, op - O_DMNMX_R);
            u32 value;
            if (op >= O_DADD_R && op <= O_DADD_I) {
                left = double_mod(c, left, B(w, 46, 1), B(w, 48, 1));
                right = double_mod(c, right, B(w, 49, 1), B(w, 45, 1));
                value = spv_emit_fadd(&c->emit, c->f64_type, left, right);
                spv_emit_no_contraction(&c->emit, value);
            } else if (op >= O_DMUL_R && op <= O_DMUL_I) {
                left = double_mod(c, left, 0, B(w, 48, 1));
                value = spv_emit_fmul(&c->emit, c->f64_type, left, right);
                spv_emit_no_contraction(&c->emit, value);
            } else if (op >= O_DFMA_R && op <= O_DFMA_I) {
                u32 third;
                right = double_mod(c, right, 0, B(w, 48, 1));
                third = op == O_DFMA_RC ? double_source(c, vec2_u32, w, 1) : double_read(c, vec2_u32, B(w, 39, 8));
                u32 addend = double_mod(c, third, 0, B(w, 49, 1));
                value = spv_emit_glsl_ternary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_FMA, left, right, addend);
            } else {
                left = double_mod(c, left, B(w, 46, 1), B(w, 48, 1));
                right = double_mod(c, right, B(w, 49, 1), B(w, 45, 1));
                u32 min = spv_emit_glsl_binary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_F_MIN, left, right);
                u32 max = spv_emit_glsl_binary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_F_MAX, left, right);
                u32 left_nan = spv_emit_isnan(&c->emit, c->bool_type, left);
                u32 right_nan = spv_emit_isnan(&c->emit, c->bool_type, right);
                min = spv_emit_select(&c->emit, c->f64_type, left_nan, right, min);
                max = spv_emit_select(&c->emit, c->f64_type, left_nan, right, max);
                min = spv_emit_select(&c->emit, c->f64_type, right_nan, left, min);
                max = spv_emit_select(&c->emit, c->f64_type, right_nan, left, max);
                value = spv_emit_select(&c->emit, c->f64_type, pred_read(c, B(w, 39, 3), B(w, 42, 1)), min, max);
            }
            double_write(c, vec2_u32, d, value);
        } else if ((op >= O_DSET_R && op <= O_DSET_I) || (op >= O_DSETP_R && op <= O_DSETP_I)) {
            int is_set = op >= O_DSET_R && op <= O_DSET_I;
            u32 left = double_mod(c, double_read(c, vec2_u32, a), is_set ? B(w, 54, 1) : B(w, 7, 1), B(w, 43, 1));
            u32 right = double_mod(c, double_source(c, vec2_u32, w, is_set ? op - O_DSET_R : op - O_DSETP_R),
                                   B(w, 44, 1), is_set ? B(w, 53, 1) : B(w, 6, 1));
            u32 comparison = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), left, right);
            u32 predicate = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            if (is_set) {
                u32 test = bool_combine(c, B(w, 45, 2), comparison, predicate);
                u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 52, 1) ? 0x3f800000u : 0xffffffffu);
                u32 value = spv_emit_select(&c->emit, c->u32_type, test, truth, c->zero);
                if (B(w, 47, 1)) flags_logic(c, value);
                reg_write(c, d, value);
            } else {
                u32 inverse = spv_emit_bool_not(&c->emit, c->bool_type, comparison);
                pred_write(c, B(w, 3, 3), bool_combine(c, B(w, 45, 2), comparison, predicate));
                pred_write(c, B(w, 0, 3), bool_combine(c, B(w, 45, 2), inverse, predicate));
            }
        } else if (op == O_FSET_R || op == O_FSET_C || op == O_FSET_I) {
            u32 left = reg_read_as(c, a, c->f32_type);
            u32 right = op == O_FSET_C   ? cbuf_read(c, w)
                        : op == O_FSET_I ? float_imm20(c, w)
                                         : reg_read_as(c, b, c->f32_type);
            left = float_mod(c, left, B(w, 54, 1), B(w, 43, 1));
            right = float_mod(c, right, B(w, 44, 1), B(w, 53, 1));
            u32 test = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), left, right);
            u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            test = bool_combine(c, B(w, 45, 2), test, pc);
            u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 52, 1) ? 0x3f800000u : 0xffffffffu);
            u32 value = spv_emit_select(&c->emit, c->u32_type, test, truth, c->zero);
            if (B(w, 47, 1)) flags_logic(c, value);
            reg_write(c, d, value);
        } else if (op == O_IPA && B(w, 38, 1)) {
            /* indexed: the generic input at the index register's address, picked among those the header gives a
               mode by its vector (one nothing feeds reads 0, 0, 0, 1) and interpolated in its own mode, then the
               instruction's mode and saturation, as below */
            u32 offset = reg_read(c, a);
            u32 vector = spv_emit_shift_right_u32(&c->emit, c->u32_type, offset,
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 4));
            u32 component =
                spv_emit_bitwise_and_u32(&c->emit, c->u32_type,
                                         spv_emit_shift_right_u32(&c->emit, c->u32_type, offset,
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 2)),
                                         spv_emit_constant_u32(&c->emit, c->u32_type, 3));
            u32 last =
                spv_emit_i_equal(&c->emit, c->bool_type, component, spv_emit_constant_u32(&c->emit, c->u32_type, 3));
            u32 wcoord = fragcoord
                             ? spv_emit_vec4_extract(&c->emit, c->f32_type, spv_emit_load(&c->emit, vec4, fragcoord), 3)
                             : 0;
            u32 value = zero_f;
            for (u32 v = 0; v < 32; v++) {
                u32 bits;
                if (!((sph[6 + v / 4] >> (v % 4 * 8)) & 0xff)) continue;
                if (inputs[v]) {
                    bits = spv_emit_load(
                        &c->emit, c->f32_type,
                        spv_emit_interface_scalar_pointer(&c->emit, 1, c->f32_type, inputs[v], 0, component));
                    if (ipa_interpolation(sph, v) == 2 && wcoord) {
                        bits = spv_emit_fmul(&c->emit, c->f32_type, bits, wcoord);
                        spv_emit_no_contraction(&c->emit, bits);
                    }
                } else /* no output feeds it (in_unfed_mask) */
                    bits = spv_emit_select(&c->emit, c->f32_type, last,
                                           spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f), zero_f);
                value = spv_emit_select(&c->emit, c->f32_type,
                                        spv_emit_i_equal(&c->emit, c->bool_type, vector,
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 8 + v)),
                                        bits, value);
            }
            if (B(w, 54, 2) == 1) {
                value = spv_emit_fmul(&c->emit, c->f32_type, value, reg_read_as(c, b, c->f32_type));
                spv_emit_no_contraction(&c->emit, value);
            }
            value = float_saturate(c, value, B(w, 51, 1));
            reg_write_as(c, d, value, c->f32_type);
        } else if (op == O_IPA) {
            u32 addr = B(w, 30, 8) * 4;
            u32 component = (addr >> 2) & 3;
            u32 value;
            u32 interpolation = 0;
            if (addr == 0x64) {
                value = layer_input ? spv_emit_bitcast(&c->emit, c->f32_type,
                                                       spv_emit_bitcast(&c->emit, c->u32_type,
                                                                        spv_emit_load(&c->emit, i32_type, layer_input)))
                                    : spv_emit_bitcast(&c->emit, c->f32_type, c->zero); /* no capability: layer 0 */
            } else if (addr == 0x3fc) {
                u32 facing = spv_emit_load(&c->emit, c->bool_type, frontface);
                u32 bits = spv_emit_select(&c->emit, c->u32_type, facing,
                                           spv_emit_constant_u32(&c->emit, c->u32_type, 0xffffffffu), c->zero);
                value = spv_emit_bitcast(&c->emit, c->f32_type, bits);
            } else if (addr == 0x60) {
                value = spv_emit_bitcast(&c->emit, c->f32_type,
                                         primitive_id ? spv_emit_load(&c->emit, c->u32_type, primitive_id) : c->zero);
            } else if (addr == 0x2e0 || addr == 0x2e4) {
                value =
                    spv_emit_vec4_extract(&c->emit, c->f32_type, spv_emit_load(&c->emit, vec2, point_coord), component);
                if (addr == 0x2e4) { /* T flips with the sprite's origin, a push constant (see vk/render.c) */
                    u32 flip =
                        spv_emit_push_component_f32(&c->emit, push_pointer, push, c->zero,
                                                    spv_emit_constant_u32(&c->emit, c->u32_type, 3), c->f32_type);
                    u32 flipped =
                        spv_emit_fadd(&c->emit, c->f32_type, spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f),
                                      spv_emit_fnegate(&c->emit, c->f32_type, value));
                    value = spv_emit_select(&c->emit, c->f32_type,
                                            spv_emit_fcompare(&c->emit, c->bool_type, 5, flip, zero_f), flipped, value);
                }
            } else if (addr >= 0x80) {
                u32 input = (addr - 0x80) >> 4;
                if (inputs[input]) {
                    value = spv_emit_load(&c->emit, vec4, inputs[input]);
                    value = spv_emit_vec4_extract(&c->emit, c->f32_type, value, component);
                } else /* no output feeds it (in_unfed_mask) */
                    value = spv_emit_constant_f32(&c->emit, c->f32_type, component == 3 ? 1.0f : 0.0f);
                interpolation = ipa_interpolation(sph, input);
            }
            if (addr >= 0x70 && addr < 0x80) {
                u32 coord = spv_emit_load(&c->emit, vec4, fragcoord);
                value = spv_emit_vec4_extract(&c->emit, c->f32_type, coord, component);
                /* a[0x7c] is the interpolated 1/w, FragCoord.w as it is: shaders take its reciprocal for
                   IPA.MUL */
                if (addr != 0x7c && needs_scale && component < 2) {
                    u32 zero_index = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
                    u32 scale = spv_emit_uniform_component(&c->emit, c->scale_pointer, c->scale_buffer, zero_index,
                                                           zero_index, zero_index, c->f32_type);
                    value = spv_emit_fmul(&c->emit, c->f32_type, value, scale);
                    spv_emit_no_contraction(&c->emit, value);
                }
            } else if (interpolation == 2) {
                /* The hardware interpolates attr/w for a perspective input, and the shader multiplies by w
                   (IPA.MUL with the reciprocal of a[0x7c]). Vulkan's input is attr itself: times the
                   interpolated 1/w, FragCoord.w, it is what the hardware gives. */
                u32 coord = spv_emit_load(&c->emit, vec4, fragcoord);
                u32 wcoord = spv_emit_vec4_extract(&c->emit, c->f32_type, coord, 3);
                value = spv_emit_fmul(&c->emit, c->f32_type, value, wcoord);
                spv_emit_no_contraction(&c->emit, value);
            }
            if (B(w, 54, 2) == 1) {
                u32 rhs = reg_read_as(c, b, c->f32_type);
                value = spv_emit_fmul(&c->emit, c->f32_type, value, rhs);
                spv_emit_no_contraction(&c->emit, value);
            }
            value = float_saturate(c, value, B(w, 51, 1));
            reg_write_as(c, d, value, c->f32_type);
        } else if (op == O_MUFU && (B(w, 20, 4) == 6 || B(w, 20, 4) == 7)) {
            /* RCP64H and RSQ64H: Ra is a double's high word (the low one 0), the result the high word of its
               reciprocal or reciprocal square root, as Mesa's nouveau uses them */
            u32 high = spv_emit_vec2_construct(&c->emit, vec2_u32, c->zero, reg_read(c, a));
            u32 value = double_mod(c, spv_emit_bitcast(&c->emit, c->f64_type, high), B(w, 46, 1), B(w, 48, 1));
            value = B(w, 20, 4) == 6
                        ? spv_emit_fdiv(&c->emit, c->f64_type,
                                        spv_emit_constant64(&c->emit, c->f64_type, 0x3ff0000000000000ull), value)
                        : spv_emit_glsl_unary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_INVERSE_SQRT, value);
            reg_write(c, d,
                      spv_emit_vec4_extract(&c->emit, c->u32_type, spv_emit_bitcast(&c->emit, vec2_u32, value), 1));
        } else if (op == O_MUFU || op == O_RRO_R || op == O_RRO_C || op == O_RRO_I) {
            u32 source = op == O_MUFU ? a : b;
            u32 value = op == O_RRO_C   ? cbuf_read(c, w)
                        : op == O_RRO_I ? float_imm20(c, w)
                                        : reg_read_as(c, source, c->f32_type);
            u32 absolute = op == O_MUFU ? B(w, 46, 1) : B(w, 49, 1);
            u32 negative = op == O_MUFU ? B(w, 48, 1) : B(w, 45, 1);
            if (absolute) value = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_ABS, value);
            if (negative) value = spv_emit_fnegate(&c->emit, c->f32_type, value);
            if (op == O_MUFU) {
                SpvGlsl450Op fn = 0;
                switch (B(w, 20, 4)) {
                    case 0: fn = SPV_GLSL_COS; break;
                    case 1: fn = SPV_GLSL_SIN; break;
                    case 2: fn = SPV_GLSL_EXP2; break;
                    case 3: fn = SPV_GLSL_LOG2; break;
                    case 5: fn = SPV_GLSL_INVERSE_SQRT; break;
                    case 8: fn = SPV_GLSL_SQRT; break;
                    case 4: {
                        u32 one = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
                        value = spv_emit_fdiv(&c->emit, c->f32_type, one, value);
                        break;
                    }
                    default: break;
                }
                if (fn) value = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, fn, value);
                if (B(w, 50, 1)) {
                    u32 fzero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
                    u32 fone = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
                    value = spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_CLAMP, value, fzero, fone);
                }
            }
            reg_write_as(c, d, value, c->f32_type);
        } else if (op == O_FSWZADD) {
            /* Each lane of a quad (0 the upper left, 1 the upper right, 2 the lower left, 3 the lower right) does the
               mode in its bits of the swizzle (fswzadd_lanes): a + b (0), b - a (1), a - b (2) or b (3). Nothing
               crosses lanes: compilers read the neighbour with SHFL.BFLY first, and dFdx is FSWZADD of it and x with
               0x99 (nouveau's handleDFDX, NAK's ddx). Nouveau's manual TXD fixes the order of the lanes and the
               modes: 0xcc adds the gradient in a to b on the right lanes and keeps b on the left ones. Eden and
               Ryujinx count the lanes from the low bits, and Eden negates every mode. The rounding mode (39-40) is
               round to nearest, as FADD's is, and NDV (38) changes nothing computed */
            u32 left = ftz_read(c, insn, block_start, i, a, reg_read_as(c, a, c->f32_type));
            u32 right = ftz_read(c, insn, block_start, i, b, reg_read_as(c, b, c->f32_type));
            int soft_ftz = B(w, 44, 1) && c->soft_ftz_on;
            if (soft_ftz) {
                left = float_ftz(c, left);
                right = float_ftz(c, right);
            }
            u32 lane = 0;
            if (fswzadd_lanes(w, B(w, 28, 2)) != 15)
                lane = spv_emit_bitwise_and_u32(&c->emit, c->u32_type,
                                                spv_emit_load(&c->emit, c->u32_type, c->subgroup_lane_input),
                                                spv_emit_constant_u32(&c->emit, c->u32_type, 3));
            /* pick[k]: whether this lane's mode is k + 1, where some lanes' are and others' aren't */
            u32 masks[3] = {fswzadd_lanes(w, 1), fswzadd_lanes(w, 2), fswzadd_lanes(w, 3)};
            u32 pick[3] = {0, 0, 0};
            for (int k = 0; k < 3; k++) {
                if (!masks[k] || masks[k] == 15) continue;
                u32 bit = spv_emit_bitwise_and_u32(
                    &c->emit, c->u32_type,
                    spv_emit_shift_right_u32(&c->emit, c->u32_type,
                                             spv_emit_constant_u32(&c->emit, c->u32_type, masks[k]), lane),
                    spv_emit_constant_u32(&c->emit, c->u32_type, 1));
                pick[k] = spv_emit_i_not_equal(&c->emit, c->bool_type, bit,
                                               spv_emit_constant_u32(&c->emit, c->u32_type, 0));
            }
            u32 result = right;
            if (masks[2] != 15) {
                u32 lhs = left, rhs = right;
                if (masks[0] == 15) lhs = spv_emit_fnegate(&c->emit, c->f32_type, left);
                if (pick[0])
                    lhs = spv_emit_select(&c->emit, c->f32_type, pick[0], spv_emit_fnegate(&c->emit, c->f32_type, left),
                                          left);
                if (masks[1] == 15) rhs = spv_emit_fnegate(&c->emit, c->f32_type, right);
                if (pick[1])
                    rhs = spv_emit_select(&c->emit, c->f32_type, pick[1],
                                          spv_emit_fnegate(&c->emit, c->f32_type, right), right);
                result = spv_emit_fadd(&c->emit, c->f32_type, lhs, rhs);
                spv_emit_no_contraction(&c->emit, result);
                if (soft_ftz) result = float_ftz(c, result);
                if (pick[2]) result = spv_emit_select(&c->emit, c->f32_type, pick[2], right, result);
            }
            reg_write_as(c, d, result, c->f32_type);
        } else if (half_arithmetic(op)) {
            /* Each form's operands and fields, as Eden's half_floating_point_*.cpp decode them. A constant buffer
               operand is one F32 for both halves (swizzle 1). The 32I forms write both halves (no merge), take A's
               swizzle at 53, and HFMA2_32I adds Rd. FTZ isn't modelled: the halves are computed as F32. */
            int fma = op == O_HFMA2_R || op == O_HFMA2_RC || op == O_HFMA2_CR || op == O_HFMA2_I || op == O_HFMA2_32I;
            int multiply = op == O_HMUL2_R || op == O_HMUL2_C || op == O_HMUL2_I || op == O_HMUL2_32I;
            int imm32 = op == O_HADD2_32I || op == O_HMUL2_32I || op == O_HFMA2_32I;
            u32 raw_b = 0, raw_c = 0, swizzle_b = 0, swizzle_c = 0, precision = 0;
            int abs_a = 0, neg_a = 0, abs_b = 0, neg_b = 0, neg_c = 0, saturate = 0;
            switch (op) {
                case O_HADD2_R:
                case O_HMUL2_R:
                    raw_b = reg_read(c, b);
                    swizzle_b = B(w, 28, 2);
                    abs_b = (int)B(w, 30, 1);
                    neg_b = (int)B(w, 31, 1);
                    saturate = (int)B(w, 32, 1);
                    break;
                case O_HADD2_C:
                case O_HMUL2_C:
                    raw_b = cbuf_word(c, w);
                    swizzle_b = 1;
                    abs_b = (int)B(w, 54, 1);
                    neg_b = op == O_HADD2_C ? (int)B(w, 56, 1) : 0;
                    saturate = (int)B(w, 52, 1);
                    break;
                case O_HADD2_I:
                case O_HMUL2_I:
                    raw_b = half_immediate(c, w);
                    saturate = (int)B(w, 52, 1);
                    break;
                case O_HADD2_32I:
                case O_HMUL2_32I:
                    raw_b = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
                    saturate = (int)B(w, 52, 1);
                    break;
                case O_HFMA2_R:
                    raw_b = reg_read(c, b);
                    swizzle_b = B(w, 28, 2);
                    neg_b = (int)B(w, 31, 1);
                    raw_c = reg_read(c, B(w, 39, 8));
                    swizzle_c = B(w, 35, 2);
                    neg_c = (int)B(w, 30, 1);
                    saturate = (int)B(w, 32, 1);
                    precision = B(w, 37, 2);
                    break;
                case O_HFMA2_RC:
                case O_HFMA2_CR:
                    raw_b = op == O_HFMA2_RC ? reg_read(c, B(w, 39, 8)) : cbuf_word(c, w);
                    swizzle_b = op == O_HFMA2_RC ? B(w, 53, 2) : 1;
                    neg_b = (int)B(w, 56, 1);
                    raw_c = op == O_HFMA2_RC ? cbuf_word(c, w) : reg_read(c, B(w, 39, 8));
                    swizzle_c = op == O_HFMA2_RC ? 1 : B(w, 53, 2);
                    neg_c = (int)B(w, 51, 1);
                    saturate = (int)B(w, 52, 1);
                    precision = B(w, 57, 2);
                    break;
                case O_HFMA2_I:
                    raw_b = half_immediate(c, w);
                    raw_c = reg_read(c, B(w, 39, 8));
                    swizzle_c = B(w, 53, 2);
                    neg_c = (int)B(w, 51, 1);
                    saturate = (int)B(w, 52, 1);
                    precision = B(w, 57, 2);
                    break;
                default: /* O_HFMA2_32I */
                    raw_b = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
                    raw_c = reg_read(c, d);
                    neg_c = (int)B(w, 52, 1);
                    precision = B(w, 55, 2);
                    break;
            }
            if (op == O_HADD2_32I)
                neg_a = (int)B(w, 56, 1);
            else if (!fma && !imm32) {
                abs_a = (int)B(w, 44, 1);
                neg_a = op == O_HMUL2_R ? 0 : (int)B(w, 43, 1);
            }
            if (multiply) precision = B(w, imm32 ? 55 : 39, 2);
            u32 lhs =
                half_pair_mod(c, vec2, half_pair(c, vec2, reg_read(c, a), B(w, imm32 ? 53 : 47, 2)), abs_a, neg_a);
            u32 rhs = half_pair_mod(c, vec2, half_pair(c, vec2, raw_b, swizzle_b), abs_b, neg_b);
            u32 addend = fma ? half_pair_mod(c, vec2, half_pair(c, vec2, raw_c, swizzle_c), 0, neg_c) : 0;
            u32 pair = fma        ? spv_emit_glsl_ternary(&c->emit, vec2, c->glsl, SPV_GLSL_FMA, lhs, rhs, addend)
                       : multiply ? spv_emit_fmul(&c->emit, vec2, lhs, rhs)
                                  : spv_emit_fadd(&c->emit, vec2, lhs, rhs);
            spv_emit_no_contraction(&c->emit, pair);
            if ((fma || multiply) && precision == 2) pair = half_fmz(c, vec2, pair, lhs, rhs, addend);
            reg_write(c, d, half_pair_result(c, vec2, pair, reg_read(c, d), imm32 ? 0 : B(w, 49, 2), saturate));
        } else if (op == O_HSET2_R || op == O_HSET2_C || op == O_HSET2_I) {
            u32 raw_b;
            u32 lhs;
            u32 rhs;
            if (op == O_HSET2_R)
                raw_b = reg_read(c, b);
            else if (op == O_HSET2_C)
                raw_b = cbuf_word(c, w);
            else
                raw_b = half_immediate(c, w);
            lhs = half_pair_mod(c, vec2, half_pair(c, vec2, reg_read(c, a), B(w, 47, 2)), B(w, 44, 1), B(w, 43, 1));
            rhs = half_pair_mod(c, vec2,
                                half_pair(c, vec2, raw_b,
                                          op == O_HSET2_R   ? B(w, 28, 2)
                                          : op == O_HSET2_C ? 1
                                                            : 0),
                                op == O_HSET2_R && B(w, 30, 1),
                                op == O_HSET2_R ? B(w, 31, 1) : op == O_HSET2_C && B(w, 56, 1));
            u32 predicate = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 compare = B(w, op == O_HSET2_R ? 35 : 49, 4);
            u32 result = c->zero;
            for (u32 lane = 0; lane < 2; lane++) {
                u32 left = spv_emit_vec4_extract(&c->emit, c->f32_type, lhs, lane);
                u32 right = spv_emit_vec4_extract(&c->emit, c->f32_type, rhs, lane);
                u32 test = spv_emit_fcompare(&c->emit, c->bool_type, compare, left, right);
                test = bool_combine(c, B(w, 45, 2), test, predicate);
                u32 truth = B(w, op == O_HSET2_R ? 49 : 53, 1) ? 0x3c00u : 0xffffu;
                u32 bits = spv_emit_select(&c->emit, c->u32_type, test,
                                           spv_emit_constant_u32(&c->emit, c->u32_type, truth << (lane * 16)), c->zero);
                result = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, result, bits);
            }
            reg_write(c, d, result);
        } else if (op == O_HSETP2_R || op == O_HSETP2_C || op == O_HSETP2_I) {
            u32 raw_b;
            u32 lhs;
            u32 rhs;
            if (op == O_HSETP2_R)
                raw_b = reg_read(c, b);
            else if (op == O_HSETP2_C)
                raw_b = cbuf_word(c, w);
            else
                raw_b = half_immediate(c, w);
            lhs = half_pair_mod(c, vec2, half_pair(c, vec2, reg_read(c, a), B(w, 47, 2)), B(w, 44, 1), B(w, 43, 1));
            rhs = half_pair_mod(c, vec2,
                                half_pair(c, vec2, raw_b,
                                          op == O_HSETP2_R   ? B(w, 28, 2)
                                          : op == O_HSETP2_C ? 1
                                                             : 0),
                                (op == O_HSETP2_C && B(w, 54, 1)) || (op == O_HSETP2_R && B(w, 30, 1)),
                                op == O_HSETP2_R ? B(w, 31, 1) : op == O_HSETP2_C && B(w, 56, 1));
            u32 predicate = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 compare = B(w, op == O_HSETP2_R ? 35 : 49, 4);
            u32 outcomes[2];
            for (u32 lane = 0; lane < 2; lane++) {
                u32 left = spv_emit_vec4_extract(&c->emit, c->f32_type, lhs, lane);
                u32 right = spv_emit_vec4_extract(&c->emit, c->f32_type, rhs, lane);
                u32 test = spv_emit_fcompare(&c->emit, c->bool_type, compare, left, right);
                outcomes[lane] = bool_combine(c, B(w, 45, 2), test, predicate);
            }
            if (B(w, op == O_HSETP2_R ? 49 : 53, 1)) {
                u32 both = spv_emit_bool_binary(&c->emit, c->bool_type, 0, outcomes[0], outcomes[1]);
                pred_write(c, B(w, 3, 3), both);
                pred_write(c, B(w, 0, 3), spv_emit_bool_not(&c->emit, c->bool_type, both));
            } else {
                pred_write(c, B(w, 3, 3), outcomes[0]);
                pred_write(c, B(w, 0, 3), outcomes[1]);
            }
        } else if (op == O_TLD || op == O_TLD_B) {
            u32 indexed = 0, ldc = 0;
            u32 off = B(w, 36, 13), ty = B(w, 28, 3);
            u32 bank = 0, bank2 = 0, off2 = 0;
            int handle_count =
                op == O_TLD_B ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            int type = ty >> 1, array = ty & 1, multisample = (int)B(w, 50, 1), slot = 0;
            while (slot < texture_count &&
                   (textures[slot].cbuf_off != off || textures[slot].type != type || textures[slot].array != array ||
                    textures[slot].shadow || textures[slot].bindless != (op == O_TLD_B) ||
                    !!(textures[slot].flags & 2) != multisample ||
                    (op == O_TLD_B &&
                     !direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
                slot++;
            if (textures[slot].flags & SNX_TEX_INDEXED) direct_swap_element(c, slot);
            u32 x = reg_read_as(c, a + array, i32_type);
            u32 y = type == 0 ? spv_emit_constant_u32(&c->emit, i32_type, 0)
                              : reg_read_as(c, a + array + 1, i32_type);
            /* Rb's operands, in order: TLD.B's handle, the level (bit 55), the offsets (bit 35) and the sample (bit 50) */
            u32 meta = b + (op == O_TLD_B), level = B(w, 55, 1) ? meta++ : 255, packed_reg = B(w, 35, 1) ? meta++ : 255;
            u32 z =
                type <= 1 && !array ? 0
                : type == 0 || array
                    ? spv_emit_bitcast(&c->emit, i32_type,
                                       spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reg_read(c, a),
                                                                spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff)))
                    : reg_read_as(c, a + 2, i32_type);
            if (packed_reg < 255) { /* 4-bit signed offsets: x, then y but for 1D, then z for 3D */
                u32 packed = reg_read_as(c, packed_reg, i32_type);
                u32 four = spv_emit_constant_u32(&c->emit, c->u32_type, 4);
                x = spv_emit_iadd_u32(&c->emit, i32_type, x,
                                      spv_emit_bitfield_sextract(&c->emit, i32_type, packed, c->zero, four));
                if (type >= 1)
                    y = spv_emit_iadd_u32(&c->emit, i32_type, y,
                                          spv_emit_bitfield_sextract(&c->emit, i32_type, packed, four, four));
                if (type == 2)
                    z = spv_emit_iadd_u32(&c->emit, i32_type, z,
                                          spv_emit_bitfield_sextract(&c->emit, i32_type, packed,
                                                                     spv_emit_constant_u32(&c->emit, c->u32_type, 8),
                                                                     four));
            }
            if (type == 0 && !array) texel_row_coord(c, i32_type, &x, &y);
            if (type == 1 && needs_texel_scale) scaled_coords(c, push, slot, 0, i32_type, &x, &y);
            u32 coord = z ? spv_emit_vec3_construct(&c->emit, vec3_i32, x, y, z)
                          : spv_emit_vec2_construct(&c->emit, vec2_i32, x, y);
            u32 sample =
                multisample
                    ? spv_emit_image_fetch_sample(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot],
                                                  coord, reg_read_as(c, meta, i32_type))
                    : spv_emit_image_fetch(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot], coord,
                                           level < 255 ? reg_read_as(c, level, i32_type)
                                                       : spv_emit_constant_u32(&c->emit, i32_type, 0));
            u32 mask = B(w, 31, 4), target = d;
            for (u32 k = 0; k < 4; k++)
                if (mask & (1u << k)) {
                    reg_write(c, target, texture_component(c, sample, texture_scalar[slot], k));
                    if (target < 255) target++;
                }
        } else if (op == O_SULD || op == O_SUST || op == O_SURED || op == O_SUATOM) {
            int dim = (int)B(w, 33, 3), buffer = dim == 1;
            u32 bank = 0, off = B(w, 36, 13);
            int bindless = op == O_SUATOM ? (int)B(w, 54, 1) : !B(w, 51, 1);
            if (bindless) direct_track_image_handle(insn, n, i, B(w, 39, 8), &bank, &off, 0);
            u32 x = reg_read_as(c, a, i32_type), coord = x;
            if (!buffer) {
                u32 y = reg_read_as(c, a + 1, i32_type);
                /* a 2D array's layer is the low 16 bits of the third register, as TLD's is, and a 3D surface's z the
                   whole register (Eden's MakeCoords in surface_load_store.cpp reads them the same way) */
                coord = dim == 4 ? spv_emit_vec3_construct(
                                       &c->emit, vec3_i32, x, y,
                                       spv_emit_bitcast(&c->emit, i32_type,
                                                        spv_emit_bitwise_and_u32(
                                                            &c->emit, c->u32_type, reg_read(c, a + 2),
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff))))
                        : dim == 5 ? spv_emit_vec3_construct(&c->emit, vec3_i32, x, y,
                                                             reg_read_as(c, a + 2, i32_type))
                                   : spv_emit_vec2_construct(&c->emit, vec2_i32, x, y);
            }
            u32 mask = op == O_SULD && B(w, 52, 1) ? 1u : B(w, 20, 4), source = d;
            if (op != O_SULD || buffer) {
                int slot = direct_image_slot(&image_info, off, bindless, bank, op != O_SUST, dim);
                int kind = image_info.img[slot].kind;
                if (op == O_SULD) {
                    u32 texel = spv_emit_image_read(&c->emit, vec4_u32, c->image_type[slot], c->image[slot], coord);
                    reg_write(c, d, spv_emit_vec4_extract(&c->emit, c->u32_type, texel, 0));
                } else if (kind == 1) {
                    u32 opcode;
                    if (op == O_SURED) {
                        u32 operation = B(w, 24, 3);
                        opcode = operation == 0   ? 234
                                 : operation == 1 ? (B(w, 20, 3) ? 236 : 237)
                                                  : (B(w, 20, 3) ? 238 : 239);
                    } else {
                        /* ADD, MIN, MAX, -, -, AND, OR, XOR, EXCH; only S32 compares signed */
                        static const u32 opcodes[9] = {234, 237, 239, 0, 0, 240, 241, 242, 229};
                        u32 operation = B(w, 29, 4);
                        opcode = opcodes[operation];
                        if (B(w, 51, 3) == 1 && (operation == 1 || operation == 2)) opcode--;
                    }
                    u32 pointer = spv_emit_image_texel_pointer(&c->emit, c->u32_type, c->image[slot], coord, c->zero);
                    u32 prior = spv_emit_atomic_u32(&c->emit, c->u32_type, opcode, pointer,
                                                    reg_read(c, op == O_SUATOM ? b : source));
                    if (op == O_SUATOM) reg_write(c, d, prior);
                } else {
                    u32 components[4];
                    u32 scalar = kind == 3 ? i32_type : kind == 2 ? c->u32_type : c->f32_type;
                    for (u32 q = 0; q < 4; q++) {
                        components[q] = kind ? spv_emit_constant_u32(&c->emit, scalar, q == 3 ? 1 : 0)
                                              : spv_emit_constant_f32(&c->emit, c->f32_type, q == 3 ? 1.0f : 0.0f);
                        if (mask & (1u << q)) {
                            components[q] = reg_read_as(c, source, scalar);
                            if (source < 255) source++;
                        }
                    }
                    u32 texel = spv_emit_vec4_construct(&c->emit, kind == 3 ? vec4_i32 : kind == 2 ? vec4_u32 : vec4,
                                                       components);
                    spv_emit_image_write(&c->emit, c->image_type[slot], c->image[slot], coord, texel);
                }
            } else {
                int slot = 0;
                int sample_type = dim == 5 ? 2 : 1, arrayed = dim == 4;
                while (slot < texture_count &&
                       (textures[slot].cbuf_off != off || textures[slot].type != sample_type ||
                        textures[slot].array != arrayed ||
                        textures[slot].shadow || textures[slot].bindless != bindless ||
                        (textures[slot].flags & 3) != (B(w, 52, 1) ? 1 : 0) ||
                        (bindless && textures[slot].bl_cbuf != bank)))
                    slot++;
                u32 sample = spv_emit_image_fetch(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot],
                                                  coord, spv_emit_constant_u32(&c->emit, i32_type, 0));
                for (u32 q = 0; q < 4; q++)
                    if (mask & (1u << q)) {
                        reg_write(c, source, texture_component(c, sample, texture_scalar[slot], q));
                        if (source < 255) source++;
                    }
            }
        } else if (op == O_TLDS) {
            u32 off = B(w, 36, 13);
            u32 encoding = B(w, 53, 4);
            int array = encoding == 8, type = encoding == 0 || encoding == 1 ? 0 : encoding == 7 ? 2 : 1;
            int multisample = encoding == 6, slot = 0;
            u32 x;
            u32 y;
            while (slot < texture_count &&
                   (textures[slot].cbuf_off != off || textures[slot].type != type || textures[slot].array != array ||
                    textures[slot].shadow || textures[slot].bindless || textures[slot].flags != (multisample ? 2 : 0)))
                slot++;
            /* the 2D array form takes x and y from Rb and Rb + 1, the layer from Ra's low 16 bits */
            x = reg_read_as(c, array ? b : a, i32_type);
            y = type == 0 ? spv_emit_constant_u32(&c->emit, i32_type, 0)
                          : spv_emit_bitcast(&c->emit, i32_type,
                                             reg_read(c, array ? b + 1
                                                         : encoding == 4 || encoding == 5 || encoding == 6 ||
                                                                 encoding == 7 || encoding == 12
                                                             ? a + 1
                                                             : b));
            if (encoding == 4 || encoding == 12) {
                u32 packed = spv_emit_bitcast(&c->emit, i32_type, reg_read(c, b + (encoding == 12)));
                u32 four = spv_emit_constant_u32(&c->emit, c->u32_type, 4);
                u32 ox = spv_emit_bitfield_sextract(&c->emit, i32_type, packed, c->zero, four);
                u32 oy = spv_emit_bitfield_sextract(&c->emit, i32_type, packed, four, four);
                x = spv_emit_iadd_u32(&c->emit, i32_type, x, ox);
                y = spv_emit_iadd_u32(&c->emit, i32_type, y, oy);
            }
            if (type == 0) texel_row_coord(c, i32_type, &x, &y);
            if (type == 1 && needs_texel_scale) scaled_coords(c, push, slot, 0, i32_type, &x, &y);
            u32 coord = type == 2 ? spv_emit_vec3_construct(&c->emit, vec3_i32, x, y,
                                                            reg_read_as(c, b, i32_type))
                        : array   ? spv_emit_vec3_construct(
                                        &c->emit, vec3_i32, x, y,
                                        spv_emit_bitcast(&c->emit, i32_type,
                                                         spv_emit_bitwise_and_u32(
                                                             &c->emit, c->u32_type, reg_read(c, a),
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff))))
                                  : spv_emit_vec2_construct(&c->emit, vec2_i32, x, y);
            u32 lod = encoding == 1 || encoding == 5 || encoding == 12 ? reg_read(c, b) : c->zero;
            u32 sample = multisample ? spv_emit_image_fetch_sample(&c->emit, texture_vec4[slot], c->sampled_type[slot],
                                                                   c->sampler[slot], coord, reg_read(c, b))
                                     : spv_emit_image_fetch(&c->emit, texture_vec4[slot], c->sampled_type[slot],
                                                            c->sampler[slot], coord, lod);
            if (texture_scalar[slot] == c->f32_type && !B(w, 59, 1)) {
                u32 regs[4], comps[4], halves[4];
                int outputs = texs_outputs(w, regs, comps);
                for (int k = 0; k < outputs; k++)
                    halves[k] = spv_emit_vec4_extract(&c->emit, c->f32_type, sample, comps[k]);
                u32 lo = spv_emit_vec2_construct(&c->emit, vec2, outputs > 0 ? halves[0] : zero_f,
                                                 outputs > 1 ? halves[1] : zero_f);
                lo = spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_convert_f_to_f(&c->emit, vec2f16, lo));
                if (d < 255) reg_write(c, d, lo);
                if (outputs > 2) {
                    u32 db = B(w, 28, 8);
                    u32 hi = spv_emit_vec2_construct(&c->emit, vec2, halves[2], outputs > 3 ? halves[3] : zero_f);
                    hi = spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_convert_f_to_f(&c->emit, vec2f16, hi));
                    if (db < 255) reg_write(c, db, hi);
                }
            } else {
                texs_store(c, w, sample, texture_scalar[slot], 0, i32_type, vec2, zero_f);
            }
        } else if (op == O_TEXS) {
            u32 off = B(w, 36, 13);
            u32 encoding = B(w, 53, 4);
            int shadow = texs_shadow(encoding), type = texs_type(encoding), array = texs_array(encoding);
            int slot = 0;
            while (slot < texture_count &&
                   (textures[slot].cbuf_off != off || textures[slot].shadow != shadow || textures[slot].type != type ||
                    textures[slot].array != array || textures[slot].bindless || textures[slot].flags))
                slot++;
            u32 x = reg_read_as(c, array ? a + 1 : a, c->f32_type);
            u32 y = encoding == 0 ? spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f)
                                  : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, texs_y(encoding, a, b)));
            u32 uv = array       ? spv_emit_vec3_construct(&c->emit, vec3, x, y, array_layer(c, a))
                     : type >= 2 ? spv_emit_vec3_construct(&c->emit, vec3, x, y,
                                                           reg_read_as(c, b, c->f32_type))
                                 : spv_emit_vec2_construct(&c->emit, vec2, x, y);
            u32 dref = shadow ? spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, texs_dref(encoding, b))) : 0;
            u32 sample;
            int explicit_lod = texs_explicit(encoding) || stage != ST_FS;
            u32 level = texs_explicit(encoding) ? texs_level(encoding, b) : 255;
            u32 lod = level < 255 ? reg_read_as(c, level, c->f32_type) : zero_f;
            sample = sample_dref(c, si, slot, texture_vec4[slot], uv, dref, explicit_lod, lod);
            texs_store(c, w, sample, texture_scalar[slot], shadow, i32_type, vec2, zero_f);
        } else if (op == O_TEX || op == O_TEX_B) {
            int bindless = op == O_TEX_B;
            u32 mode = bindless ? B(w, 37, 3) : B(w, 55, 3);
            u32 offset_enabled = B(w, bindless ? 36 : 54, 1);
            u32 source = b + bindless;
            u32 has_level = mode == 2 || mode == 3 || mode == 6 || mode == 7;
            u32 offset_source = source + has_level;
            u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            int type = B(w, 28, 3) >> 1;
            int array = B(w, 28, 1), shadow = B(w, 50, 1);
            int slot = 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            while (slot < texture_count && (textures[slot].cbuf_off != off || textures[slot].shadow != shadow ||
                                            textures[slot].type != type || textures[slot].array != array ||
                                            textures[slot].bindless != bindless || (textures[slot].flags & 3) ||
                                            (bindless && !direct_bindless_matches(&textures[slot], bank, handle_count,
                                                                                  bank2, off2, indexed, ldc))))
                slot++;
            if (textures[slot].flags & SNX_TEX_INDEXED) direct_swap_element(c, slot);
            u32 x = reg_read_as(c, a + array, c->f32_type);
            u32 y = type == 0 ? spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f)
                              : reg_read_as(c, a + array + 1, c->f32_type);
            u32 coords =
                type == 3 && array ? spv_emit_vec4_construct(
                                         &c->emit, vec4,
                                         (u32[4]){x, y, reg_read_as(c, a + 3, c->f32_type),
                                                  array_layer(c, a)})
                : array            ? spv_emit_vec3_construct(&c->emit, vec3, x, y, array_layer(c, a))
                : type >= 2 ? spv_emit_vec3_construct(&c->emit, vec3, x, y,
                                                      reg_read_as(c, a + 2, c->f32_type))
                            : spv_emit_vec2_construct(&c->emit, vec2, x, y);
            u32 level = mode == 1   ? zero_f
                        : has_level ? reg_read_as(c, source, c->f32_type)
                                    : 0;
            u32 dref =
                shadow ? reg_read_as(c, offset_source + offset_enabled, c->f32_type) : 0;
            u32 offset = 0;
            int dynamic_offset = 0;
            if (offset_enabled) {
                u32 packed = 0;
                int known = direct_track_const(insn, n, i, offset_source, &packed, 0);
                u32 bits;
                dynamic_offset = !known;
                bits = known ? 0 : reg_read(c, offset_source);
                if (!known) bits = spv_emit_bitcast(&c->emit, i32_type, bits);
                u32 ox = known ? spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)(packed << 28) >> 28))
                               : spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 0),
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 4));
                u32 oy = known ? spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)(packed << 24) >> 28))
                               : spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 4),
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 4));
                if (type == 2) {
                    u32 oz = known ? spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)(packed << 20) >> 28))
                                   : spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                                spv_emit_constant_u32(&c->emit, c->u32_type, 8),
                                                                spv_emit_constant_u32(&c->emit, c->u32_type, 4));
                    offset = known ? spv_emit_constant_vec3(&c->emit, vec3_i32, ox, oy, oz)
                                   : spv_emit_vec3_construct(&c->emit, vec3_i32, ox, oy, oz);
                } else {
                    if (type == 0) oy = spv_emit_constant_u32(&c->emit, i32_type, 0);
                    offset = known ? spv_emit_constant_vec2(&c->emit, vec2_i32, ox, oy)
                                   : spv_emit_vec2_construct(&c->emit, vec2_i32, ox, oy);
                }
            }
            /* Vulkan takes a non-constant Offset on gathers only: a sample moves its coordinates instead, by the
               offset over the level's size (1D textures are 2D images one texel high, their y offset 0) */
            if (dynamic_offset) {
                u32 moved = type == 2 ? 3 : 2, components = moved + (u32)array;
                /* Bias is not a mip level. Explicit negative levels query the base level. */
                u32 size_level = mode == 3 || mode == 7 ? level : zero_f;
                size_level = spv_emit_select(&c->emit, c->f32_type,
                                             spv_emit_fcompare(&c->emit, c->bool_type, 6, size_level, zero_f),
                                             size_level, zero_f);
                u32 lod_index = spv_emit_convert_f_to_s(&c->emit, i32_type, size_level);
                coords = direct_offset_coords(c, coords, offset, components, moved, slot, lod_index, vec2, vec3,
                                              vec2_i32, vec3_i32, i32_type);
                offset = 0;
                dynamic_offset = 0;
            }
            /* outside the fragment stage there are no derivatives: LOD 0, or the bias as the level */
            u32 sample_mode = mode == 1 || mode == 3 || mode == 7 || stage != ST_FS ? 1
                              : mode == 2 || mode == 6                              ? 2
                                                                                    : 0;
            if (sample_mode == 1 && !level) level = zero_f;
            /* a depth compare of a color texture samples it plainly and compares its first component here */
            int indexed_dc = dref && shadow && (textures[slot].flags & SNX_TEX_INDEXED);
            int dc_func = indexed_dc ? 3 : dref ? dc_color_func(si, slot) : -1;
            u32 sample =
                dc_func < 0
                    ? spv_emit_sample_image(&c->emit, shadow ? c->f32_type : texture_vec4[slot], c->sampled_type[slot],
                                            c->sampler[slot], coords, dref, sample_mode, level, offset, dynamic_offset)
                    : (indexed_dc ? dc_compare_indexed : dc_compare)(
                          c, indexed_dc ? slot : dc_func, dref,
                          spv_emit_vec4_extract(&c->emit, c->f32_type,
                                                spv_emit_sample_image(&c->emit, texture_vec4[slot],
                                                                      c->sampled_type[slot], c->sampler[slot], coords,
                                                                      0, sample_mode, level, offset, dynamic_offset),
                                                0));
            if (indexed_dc) {
                u32 depth = spv_emit_sample_image(&c->emit, c->f32_type, c->depth_type, c->depth_sampler, coords, dref,
                                                  sample_mode, level, offset, dynamic_offset);
                sample = spv_emit_select(&c->emit, c->f32_type, indexed_is_depth(c, slot), depth, sample);
            }
            u32 mask = B(w, 31, 4), dst = d;
            for (u32 component = 0; component < 4; component++)
                if (mask & (1u << component)) {
                    u32 value =
                        shadow ? spv_emit_bitcast(&c->emit, c->u32_type,
                                                  component == 3 ? spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f)
                                                                 : sample)
                               : texture_component(c, sample, texture_scalar[slot], component);
                    reg_write(c, dst, value);
                    if (dst < 255) dst++;
                }
        } else if (op == O_TXD || op == O_TXD_B) {
            int bindless = op == O_TXD_B;
            u32 ty = B(w, 28, 3), type = ty >> 1, array = ty & 1, count = type == 0 ? 1 : type == 1 ? 2 : 3;
            u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, a, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, a, stage, &ldc);
            int slot = direct_typed_slot(textures, texture_count, off, type, array, 0, 0, bindless, bank, handle_count,
                                         bank2, off2, indexed, ldc);
            if (textures[slot].flags & SNX_TEX_INDEXED) direct_swap_element(c, slot);
            u32 coord = a + (u32)bindless, packed_reg = coord + count, values[4], dx[3], dy[3], k;
            /* a 1D texture is bound as a 2D one of height 1: y 0.5, and no change along it */
            u32 dims = type == 0 ? 2 : count;
            for (k = 0; k < count; k++) {
                values[k] = reg_read_as(c, coord + k, c->f32_type);
                dx[k] = reg_read_as(c, b + 2 * k, c->f32_type);
                dy[k] = reg_read_as(c, b + 2 * k + 1, c->f32_type);
            }
            if (type == 0) {
                values[1] = spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f);
                dx[1] = dy[1] = zero_f;
            }
            if (array) values[dims] = array_layer(c, packed_reg);
            u32 coords = dims + array == 4   ? spv_emit_vec4_construct(&c->emit, vec4, values)
                         : dims + array == 3 ? spv_emit_vec3_construct(&c->emit, vec3, values[0], values[1], values[2])
                                             : spv_emit_vec2_construct(&c->emit, vec2, values[0], values[1]);
            u32 offset = 0;
            if (B(w, 35, 1)) { /* signed 4-bit offsets from bit 16 of the packed register */
                u32 packed = 0, o[3];
                if (direct_track_const(insn, n, i, packed_reg, &packed, 0)) {
                    for (k = 0; k < 3; k++)
                        o[k] = spv_emit_constant_u32(&c->emit, i32_type,
                                                     k < count ? (u32)((i32)(packed << (12 - 4 * k)) >> 28) : 0);
                    offset = dims == 3 ? spv_emit_constant_vec3(&c->emit, vec3_i32, o[0], o[1], o[2])
                                       : spv_emit_constant_vec2(&c->emit, vec2_i32, o[0], o[1]);
                } else {
                    /* Vulkan takes a non-constant offset on gathers only. TXD moves coordinates by the offset over
                       the base level's size as an approximation. */
                    u32 bits = reg_read_as(c, packed_reg, i32_type), o[3];
                    for (k = 0; k < dims; k++)
                        o[k] =
                            k < count
                                ? spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 16 + 4 * k),
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 4))
                                : spv_emit_constant_u32(&c->emit, i32_type, 0);
                    u32 dynamic_offset = dims == 3 ? spv_emit_vec3_construct(&c->emit, vec3_i32, o[0], o[1], o[2])
                                                   : spv_emit_vec2_construct(&c->emit, vec2_i32, o[0], o[1]);
                    coords = direct_offset_coords(c, coords, dynamic_offset, dims + array, count, slot,
                                                  spv_emit_constant_u32(&c->emit, i32_type, 0), vec2, vec3, vec2_i32,
                                                  vec3_i32, i32_type);
                }
            }
            u32 gradient_x = dims == 3 ? spv_emit_vec3_construct(&c->emit, vec3, dx[0], dx[1], dx[2])
                                       : spv_emit_vec2_construct(&c->emit, vec2, dx[0], dx[1]);
            u32 gradient_y = dims == 3 ? spv_emit_vec3_construct(&c->emit, vec3, dy[0], dy[1], dy[2])
                                       : spv_emit_vec2_construct(&c->emit, vec2, dy[0], dy[1]);
            u32 sample = spv_emit_sample_grad(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot],
                                              coords, gradient_x, gradient_y, offset);
            u32 mask = B(w, 31, 4), dst = d;
            for (u32 component = 0; component < 4; component++)
                if (mask & (1u << component)) {
                    reg_write(c, dst, texture_component(c, sample, texture_scalar[slot], component));
                    if (dst < 255) dst++;
                }
        } else if (op == O_TXQ || op == O_TXQ_B) {
            int bindless = op == O_TXQ_B;
            u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, a, &bank, &off, &bank2, &off2, 0) : 0;
            int slot;
            u32 qtype = 1, qarray = 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, a, stage, &ldc);
            if (!indexed && direct_query_type(insn, n, bindless, bank, off, &qtype, &qarray))
                slot = direct_typed_slot(textures, texture_count, off, qtype, qarray, 0, 0, bindless, bank, handle_count,
                                         bank2, off2, indexed, ldc);
            else
                slot = direct_query_slot(textures, texture_count, off, bindless, bank, handle_count, bank2, off2,
                                         indexed, ldc);
            if (textures[slot].flags & SNX_TEX_INDEXED) direct_swap_element(c, slot);
            /* a 3D texture's depth, or an array's layers, is the third component; a 1D texture is bound as a 2D one of
               height 1, a cube's faces are no layers */
            int third = qtype == 2 || qarray;
            u32 dimensions = spv_emit_image_query_size_lod(&c->emit, third ? vec3_u32 : vec2_u32, c->sampled_type[slot],
                                                           c->sampler[slot], reg_read(c, a + (u32)bindless));
            u32 values[4] = {
                spv_emit_vec4_extract(&c->emit, c->u32_type, dimensions, 0),
                spv_emit_vec4_extract(&c->emit, c->u32_type, dimensions, 1),
                third ? spv_emit_vec4_extract(&c->emit, c->u32_type, dimensions, 2)
                      : spv_emit_constant_u32(&c->emit, c->u32_type, 1),
                spv_emit_image_query_levels(&c->emit, c->u32_type, c->sampled_type[slot], c->sampler[slot])};
            if (qtype == 1 && needs_texel_scale) scaled_coords(c, push, slot, 1, i32_type, &values[0], &values[1]);
            u32 dst = d;
            for (u32 k = 0; k < 4; k++)
                if (B(w, 31 + k, 1)) {
                    reg_write(c, dst, values[k]);
                    if (dst < 255) dst++;
                }
        } else if (op == O_TMML || op == O_TMML_B) {
            int bindless = op == O_TMML_B;
            u32 ty = B(w, 28, 3), type = ty >> 1, array = ty & 1;
            u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            int slot = direct_typed_slot(textures, texture_count, off, type, array, 0, 0, bindless, bank, handle_count,
                                         bank2, off2, indexed, ldc);
            if (textures[slot].flags & SNX_TEX_INDEXED) direct_swap_element(c, slot);
            /* a 1D texture is bound as a 2D one of height 1 */
            u32 x = reg_read_as(c, a + array, c->f32_type);
            u32 y = type == 0 ? spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f)
                              : reg_read_as(c, a + array + 1, c->f32_type);
            u32 coords =
                type < 2 ? spv_emit_vec2_construct(&c->emit, vec2, x, y)
                         : spv_emit_vec3_construct(&c->emit, vec3, x, y,
                                                   reg_read_as(c, a + array + 2, c->f32_type));
            u32 lod = spv_emit_image_query_lod(&c->emit, vec2, c->sampled_type[slot], c->sampler[slot], coords);
            u32 mask = B(w, 31, 4), dst = d;
            for (u32 component = 0; component < 4; component++) {
                if (!(mask & (1u << component))) continue;
                u32 value = c->zero;
                if (component < 2) {
                    u32 f = spv_emit_vec4_extract(&c->emit, c->f32_type, lod, 1 - component);
                    f = spv_emit_fmul(&c->emit, c->f32_type, f, spv_emit_constant_f32(&c->emit, c->f32_type, 256.0f));
                    value = spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_convert_f_to_s(&c->emit, i32_type, f));
                }
                reg_write(c, dst, value);
                if (dst < 255) dst++;
            }
        } else if (op == O_TLD4S || op == O_TLD4 || op == O_TLD4_B) {
            u32 indexed = 0, ldc = 0;
            int bindless = op == O_TLD4_B;
            u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            u32 ty = op == O_TLD4S ? 2 : B(w, 28, 3);
            u32 type = ty >> 1, array = ty & 1;
            u32 shadow = B(w, 50, 1);
            int slot = 0;
            while (slot < texture_count &&
                   (textures[slot].cbuf_off != off || textures[slot].type != type || textures[slot].array != array ||
                    textures[slot].shadow != shadow || textures[slot].bindless != bindless ||
                    (bindless &&
                     !direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
                slot++;
            if (textures[slot].flags & SNX_TEX_INDEXED) direct_swap_element(c, slot);
            u32 x = reg_read_as(c, a + array, c->f32_type);
            u32 y = type == 0
                        ? spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f)
                        : spv_emit_bitcast(&c->emit, c->f32_type,
                                           reg_read(c, op == O_TLD4S && !shadow && !B(w, 51, 1) ? b : a + array + 1));
            u32 coords;
            if (!array && type != 3)
                coords = spv_emit_vec2_construct(&c->emit, vec2, x, y);
            else {
                u32 z =
                    type == 3 ? reg_read_as(c, a + array + 2, c->f32_type) : array_layer(c, a);
                if (type == 3 && array) {
                    u32 four[4];
                    four[0] = x;
                    four[1] = y;
                    four[2] = z;
                    four[3] = array_layer(c, a);
                    coords = spv_emit_vec4_construct(&c->emit, vec4, four);
                } else
                    coords = spv_emit_vec3_construct(&c->emit, vec3, x, y, z);
            }
            u32 component = spv_emit_constant_u32(&c->emit, c->u32_type,
                                                  B(w,
                                                    op == O_TLD4S ? 52
                                                    : bindless    ? 38
                                                                  : 56,
                                                    2));
            u32 offset = 0, dref = 0;
            int per_tap_offset = 0;
            u32 ptp_source = 0;
            if (op != O_TLD4S) {
                u32 source = b + bindless;
                u32 offset_mode = B(w, bindless ? 36 : 54, 2);
                if (offset_mode == 1) {
                    /* a 1D texture is a 2D one a texel high, which doesn't move in y */
                    offset = direct_gather_offset(c, reg_read(c, source++), 0, type == 0, i32_type, vec2_i32);
                } else if (offset_mode == 2) {
                    per_tap_offset = 1;
                    ptp_source = source;
                    source += 2;
                }
                if (shadow) dref = reg_read_as(c, source, c->f32_type);
            } else {
                if (B(w, 51, 1)) { offset = direct_gather_offset(c, reg_read(c, b), 0, 0, i32_type, vec2_i32); }
                if (shadow) dref = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b + B(w, 51, 1)));
            }
            /* a compare gather of a color texture gathers its first components and compares them in the shader */
            int indexed_dc = dref && shadow && (textures[slot].flags & SNX_TEX_INDEXED);
            int dc_func = indexed_dc ? 3 : dref ? dc_color_func(si, slot) : -1;
            if (dc_func >= 0) component = c->zero;
            u32 gathered;
            if (per_tap_offset) {
                u32 taps[4];
                /* tap k's offset is (x, y) in bytes 2(k % 2) and 2(k % 2) + 1 of register k / 2, and its texel is the
                   i0j0 one of a gather at that offset, its w */
                for (u32 tap = 0; tap < 4; tap++) {
                    u32 tap_offset = direct_gather_offset(c, reg_read(c, ptp_source + tap / 2), (tap & 1) * 16,
                                                          type == 0, i32_type, vec2_i32);
                    u32 sampled =
                        spv_emit_image_gather_ex(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot],
                                                 coords, component, dc_func < 0 ? dref : 0, tap_offset);
                    taps[tap] = spv_emit_vec4_extract(&c->emit, texture_scalar[slot], sampled, 3);
                    if (dc_func >= 0)
                        taps[tap] = indexed_dc ? dc_compare_indexed(c, slot, dref, taps[tap])
                                               : dc_compare(c, dc_func, dref, taps[tap]);
                    if (indexed_dc) {
                        u32 depth = spv_emit_image_gather_ex(&c->emit, texture_vec4[slot], c->depth_type,
                                                             c->depth_sampler, coords, component, dref, tap_offset);
                        taps[tap] = spv_emit_select(&c->emit, c->f32_type, indexed_is_depth(c, slot),
                                                    spv_emit_vec4_extract(&c->emit, c->f32_type, depth, 3), taps[tap]);
                    }
                }
                gathered = spv_emit_vec4_construct(&c->emit, texture_vec4[slot], taps);
            } else {
                gathered =
                    spv_emit_image_gather_ex(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot],
                                             coords, component, dc_func < 0 ? dref : 0, offset);
                if (dc_func >= 0) { /* each gathered first component against the reference */
                    u32 lanes[4];
                    for (u32 lane = 0; lane < 4; lane++)
                        lanes[lane] =
                            indexed_dc
                                ? dc_compare_indexed(c, slot, dref,
                                                     spv_emit_vec4_extract(&c->emit, c->f32_type, gathered, lane))
                                : dc_compare(c, dc_func, dref,
                                             spv_emit_vec4_extract(&c->emit, c->f32_type, gathered, lane));
                    gathered = spv_emit_vec4_construct(&c->emit, texture_vec4[slot], lanes);
                }
            }
            if (indexed_dc && !per_tap_offset) {
                u32 depth = spv_emit_image_gather_ex(&c->emit, texture_vec4[slot], c->depth_type, c->depth_sampler,
                                                     coords, component, dref, offset);
                gathered = spv_emit_select(&c->emit, texture_vec4[slot], indexed_is_depth(c, slot), depth, gathered);
            }
            if (op == O_TLD4S) {
                u32 db = B(w, 28, 8);
                if (B(w, 55, 1)) {
                    u32 components[4];
                    for (u32 k = 0; k < 4; k++)
                        components[k] = texture_float_component(
                            c, spv_emit_vec4_extract(&c->emit, texture_scalar[slot], gathered, k), texture_scalar[slot],
                            i32_type);
                    reg_write(c, d, pack_texture_halves(c, vec2, components[0], components[1]));
                    reg_write(c, db, pack_texture_halves(c, vec2, components[2], components[3]));
                } else {
                    u32 regs[4] = {d, d < 254 ? d + 1 : 255, db, db < 254 ? db + 1 : 255};
                    for (u32 k = 0; k < 4; k++)
                        if (regs[k] < 255)
                            reg_write(c, regs[k], texture_component(c, gathered, texture_scalar[slot], k));
                }
            } else {
                u32 mask = B(w, 31, 4), dst = d;
                for (u32 k = 0; k < 4; k++)
                    if (mask & (1u << k)) {
                        reg_write(c, dst, texture_component(c, gathered, texture_scalar[slot], k));
                        if (dst < 255) dst++;
                    }
            }
        } else if (op == O_PIXLD) {
            reg_write(c, d,
                      B(w, 31, 3) == 5 ? spv_emit_load(&c->emit, c->u32_type, sample_id)
                                       : spv_emit_load(&c->emit, c->u32_type,
                                                       spv_emit_interface_scalar_pointer(&c->emit, 1, c->u32_type,
                                                                                         coverage, 0, c->zero)));
        } else if (op == O_CCTL || op == O_CCTLL || op == O_IDE || op == O_BPT) {
            /* These cache and breakpoint hints have no shader-visible result. */
        } else if (op >= O_FCHK_R && op <= O_FCHK_I) {
            pred_write(c, B(w, 3, 3), c->true_value);
        } else if (op == O_CS2R) {
            u32 sr = B(w, 20, 8);
            u32 clock = sr == 0xff
                            ? 0
                            : spv_emit_read_clock(&c->emit, vec2_u32,
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, sr == 0x50 ? 3 : 1));
            reg_write(c, d, clock ? spv_emit_vec4_extract(&c->emit, c->u32_type, clock, 0) : c->zero);
            if (d < 254) reg_write(c, d + 1, clock ? spv_emit_vec4_extract(&c->emit, c->u32_type, clock, 1) : c->zero);
        } else if (op == O_MEMBAR) {
            /* Only compute has workgroup memory. Graphics memory barriers use Device scope even for MEMBAR.CTA;
               Vulkan does not permit Workgroup scope in those stages. */
            u32 scope = stage == ST_CS && !B(w, 8, 2) ? 2 : 1;
            spv_emit_memory_barrier(&c->emit, c->u32_type, scope, 0x848u | (stage == ST_CS ? 0x100u : 0));
        } else if (op == O_KIL) {
            /* the lane goes on as a helper, so that its quad's derivatives stay defined, and nothing it writes
               lands: what the hardware gives, and what Eden does. Inside the flow loop it goes on as a lane that
               only remembers it, and is discarded after the loop (DirectCtx.discarded) */
            if (c->discarded)
                spv_emit_store(&c->emit, c->discarded, c->true_value);
            else
                spv_emit_demote(&c->emit);
        } else if (op == O_EXIT) {
            u32 pexit_merge = 0;
            if (has_pexit) {
                u32 found, destination, new_sp;
                u32 branch_label = spv_emit_id(&c->emit), exit_label = spv_emit_id(&c->emit);
                pexit_merge = spv_emit_id(&c->emit);
                dynamic_pop(c, stack_ptr, stack, CRS_KIND(O_PEXIT), c->zero, &found, &destination, &new_sp);
                spv_emit_selection_merge(&c->emit, pexit_merge);
                spv_emit_branch_conditional(&c->emit, found, branch_label, exit_label);
                spv_emit_label(&c->emit, branch_label);
                spv_emit_store(&c->emit, pc, destination);
                spv_emit_store(&c->emit, stack_ptr, new_sp);
                spv_emit_branch(&c->emit, pexit_merge);
                spv_emit_label(&c->emit, exit_label);
            }
            if (passthrough)
                geometry_passthrough(c, si, in_vertices, vec4, position, position_in, output_attrs, inputs, point_size,
                                     layer_output, i32_type, clip_output, clip_count, primitive_id, primitive_id_out,
                                     depth_fix, push, push_pointer);
            if (xfb_on && stage != ST_GS) xfb_store(c, vec4, output_attrs, attr_private, xfb_components);
            if (depth_fix && stage != ST_GS) { /* a geometry program's vertices are fixed as it emits them */
                vertex_depth_fix(c, position, push, push_pointer, vec4);
            } else if (stage == ST_FS) {
                u32 reg = 0, output = 0, alpha = zero_f;
                for (u32 rt = 0; rt < 8; rt++) {
                    u32 mask = (target >> (rt * 4)) & 15;
                    if (!mask) continue;
                    u32 components[4];
                    int integer = !!(si->rt_int_mask & (1u << rt));
                    int signed_color = !!(si->rt_sint_mask & (1u << rt));
                    u32 color_type = integer ? signed_color ? vec4_i32 : vec4_u32 : vec4;
                    for (u32 k = 0; k < 4; k++, reg++) {
                        u32 raw = mask & (1u << k) ? reg_read(c, reg) : c->zero;
                        components[k] = integer ? signed_color ? spv_emit_bitcast(&c->emit, i32_type, raw) : raw
                                        : mask & (1u << k) ? spv_emit_bitcast(&c->emit, c->f32_type, raw)
                                                           : zero_f;
                        if (rt == 0 && k == 3) alpha = spv_emit_bitcast(&c->emit, c->f32_type, raw);
                    }
                    u32 color = spv_emit_vec4_construct(&c->emit, color_type, components);
                    spv_emit_store(&c->emit, outputs[output++], color);
                }
                if (sample_mask_output)
                    spv_emit_store(
                        &c->emit,
                        spv_emit_interface_scalar_pointer(&c->emit, 3, c->u32_type, sample_mask_output, 0, c->zero),
                        reg_read(c, reg));
                if (depth_output)
                    spv_emit_store(&c->emit, depth_output,
                                   reg_read_as(c, reg + 1, c->f32_type));
                if (push && (target & 15)) alpha_test(c, push, push_pointer, alpha);
            }
            if (c->discarded && !structure) { /* the lane leaves the flow loop, and a discarded one is discarded after it */
                spv_emit_store(&c->emit, pc, spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX));
                if (has_pexit) spv_emit_branch(&c->emit, pexit_merge);
            } else {
                if (c->discarded) { /* structured: a discarded lane is discarded as it ends */
                    u32 kill_label = spv_emit_id(&c->emit), kept = spv_emit_id(&c->emit);
                    u32 discard = spv_emit_load(&c->emit, c->bool_type, c->discarded);
                    spv_emit_selection_merge(&c->emit, kept);
                    spv_emit_branch_conditional(&c->emit, discard, kill_label, kept);
                    spv_emit_label(&c->emit, kill_label);
                    spv_emit_kill(&c->emit);
                    spv_emit_label(&c->emit, kept);
                }
                spv_emit_return(&c->emit);
            }
            if (has_pexit) spv_emit_label(&c->emit, pexit_merge);
        }
        /* no texture is sparse, so every texel a fetch reads is resident */
        pred_write(c, sparse_predicate(w, op), c->false_value);
        c->guard = 0;
        if (guard_merge) {
            if (op != O_EXIT || (c->discarded && !structure)) spv_emit_branch(&c->emit, guard_merge);
            spv_emit_label(&c->emit, guard_merge);
        } else if (op == O_EXIT && !has_flow)
            break;
        u32 next = next_instruction(i, n);
        if (has_flow && !structure && (next >= n || block_start[next]) &&
            (guard_merge || op != O_EXIT || has_pexit || c->discarded))
            spv_emit_branch(&c->emit, dispatch_merge);
    }
    if (structure && se.open) spv_emit_void_op(&c->emit, SPV_OP_UNREACHABLE, NULL, 0); /* every path ended before */
    if (has_flow && !structure) {
        /* A branch outside the decoded instruction range must terminate. A switch
           default that simply loops would leave a GPU invocation spinning forever. */
        if (converge) { /* lanes ahead of the subgroup's lowest pc wait for this iteration */
            spv_emit_label(&c->emit, wait_label);
            spv_emit_branch(&c->emit, dispatch_merge);
        }
        spv_emit_label(&c->emit, dispatch_default);
        spv_emit_store(&c->emit, pc, spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX));
        spv_emit_branch(&c->emit, dispatch_merge);
        spv_emit_label(&c->emit, dispatch_merge);
        u32 next_pc = spv_emit_load(&c->emit, c->u32_type, pc);
        u32 finished =
            spv_emit_i_equal(&c->emit, c->bool_type, next_pc, spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX));
        spv_emit_branch_conditional(&c->emit, finished, loop_merge, loop_continue);
        spv_emit_label(&c->emit, loop_continue);
        spv_emit_branch(&c->emit, loop_header);
        spv_emit_label(&c->emit, loop_merge);
        if (c->discarded) {
            u32 kill_label = spv_emit_id(&c->emit), kept = spv_emit_id(&c->emit);
            u32 discard = spv_emit_load(&c->emit, c->bool_type, c->discarded);
            spv_emit_selection_merge(&c->emit, kept);
            spv_emit_branch_conditional(&c->emit, discard, kill_label, kept);
            spv_emit_label(&c->emit, kill_label);
            spv_emit_kill(&c->emit);
            spv_emit_label(&c->emit, kept);
        }
        if (passthrough)
            geometry_passthrough(c, si, in_vertices, vec4, position, position_in, output_attrs, inputs, point_size,
                                 layer_output, i32_type, clip_output, clip_count, primitive_id, primitive_id_out,
                                 depth_fix, push, push_pointer);
        if (xfb_on && stage != ST_GS) xfb_store(c, vec4, output_attrs, attr_private, xfb_components);
        if (depth_fix && stage != ST_GS) vertex_depth_fix(c, position, push, push_pointer, vec4);
        spv_emit_return(&c->emit);
    }
    spv_emit_function_end(&c->emit);
    u32* result = spv_emit_finish(&c->emit, word_count);
    spv_emit_destroy(&c->emit);
    free(c->ftz);
    free(c->ftz_of);
    free(c->ftz_of_block);
    free(c);
    free(case_labels); /* the literals and block starts too */
    free(structure);
    free(se.label); /* and what was used */
    free(pop_targets);
    free(gb_at);
    ShaderInfo next = {0};
    next.stage = stage;
    memcpy(next.cs_local, si->cs_local, sizeof next.cs_local);
    next.shared_size = si->shared_size;
    next.attr_int_mask = si->attr_int_mask;
    next.attr_sint_mask = si->attr_sint_mask;
    next.clip_enable = si->clip_enable;
    memcpy(next.gp_skip_mask, si->gp_skip_mask, sizeof next.gp_skip_mask);
    next.uses_clip_mask = clip_used;
    next.writes_viewport = viewport_used;
    next.rt_int_mask = si->rt_int_mask;
    next.rt_sint_mask = si->rt_sint_mask;
    next.tex_int_mask = si->tex_int_mask;
    next.tex_sint_mask = si->tex_sint_mask;
    next.tex_dc_color_mask = si->tex_dc_color_mask;
    next.tex_dc_color_funcs = si->tex_dc_color_funcs;
    next.in_unfed_mask = si->in_unfed_mask;
    next.ntex = texture_count;
    next.nimg = image_info.nimg;
    memcpy(next.img, image_info.img, (size_t)image_info.nimg * sizeof image_info.img[0]);
    next.ngbuf = ngbuf;
    next.gbuf_wmask = gbuf_wmask;
    memcpy(next.gbuf_cbuf_off, gbuf_off, (size_t)ngbuf * sizeof gbuf_off[0]);
    next.fp32_flush = fp32_flush;
    next.fp32_preserve = fp32_preserve;
    next.uses_position_in = needs_fragcoord;
    memcpy(next.tex, textures, (size_t)texture_count * sizeof textures[0]);
    for (u32 i = 0; i < 18; i++)
        if (cbuf_used[i]) {
            next.cbuf_mask |= 1u << i;
            next.cbuf_max[i] = cbuf_max[i];
        }
    for (u32 i = 0; i < 32; i++) {
        if (inputs_used[i]) next.in_mask |= 1u << i;
        if (outputs_used[i]) next.out_mask |= 1u << i;
    }
    if (indexed_store) next.out_mask = ~0u;
    memcpy(next.sph, sph, sizeof sph);
    *si = next;
    return result;
}
u32* shader_to_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* word_count, int can_flush,
                     int can_preserve, int can_preserve_f64, int can_subgroup) {
    u64* copy;
    const u64* insn;
    u32* r;
    if (!code) return NULL;
    insn = shader_words(code, max_bytes, &copy);
    if (!insn) return NULL;
    r = to_spirv(code, insn, max_bytes, stage, si, word_count, can_flush, can_preserve, can_preserve_f64,
                 can_subgroup);
    free(copy);
    return r;
}
