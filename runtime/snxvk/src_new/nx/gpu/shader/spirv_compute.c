/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Compute shaders are decoded directly to SPIR-V. Unsupported encodings reject the whole program. */
#include "common/utilities/spirv_emit.h"
#include "nx/gpu/shader/translator.h"

typedef struct {
    SpvEmitter e;
    u32 u, f, b, zero, yes, no, glsl;
    u32 reg[255], pred[7], carry;
    u32 cb[18], cbptr[18], gb[16], gbptr;
    u32 group, local;
} CsDirect;

static u32 cr(CsDirect* c, u32 r) { return r == 255 ? c->zero : spv_emit_load(&c->e, c->u, c->reg[r]); }
static void cw(CsDirect* c, u32 r, u32 v) {
    if (r != 255) spv_emit_store(&c->e, c->reg[r], v);
}
static u32 cp(CsDirect* c, u32 p, int neg) {
    u32 v = p == 7 ? c->yes : spv_emit_load(&c->e, c->b, c->pred[p]);
    return neg ? spv_emit_bool_not(&c->e, c->b, v) : v;
}
static void pw(CsDirect* c, u32 p, u32 v) {
    if (p != 7) spv_emit_store(&c->e, c->pred[p], v);
}
static u32 cu(CsDirect* c, u32 v) { return spv_emit_constant_u32(&c->e, c->u, v); }
static u32 cbread(CsDirect* c, u32 idx, u32 byte_off) {
    u32 member = c->zero;
    u32 element = cu(c, byte_off >> 4);
    u32 component = cu(c, (byte_off >> 2) & 3);
    /* an integer load: a float one may flush words whose bits look like a denormal */
    return spv_emit_uniform_component(&c->e, c->cbptr[idx], c->cb[idx], member, element, component, c->u);
}
static u32 right(CsDirect* c, u64 w, int kind) {
    if (kind == 0) return cr(c, B(w, 20, 8));
    if (kind == 1) return cbread(c, B(w, 34, 5), B(w, 20, 14) * 4);
    u32 imm = B(w, 20, 19);
    return cu(c, B(w, 56, 1) ? imm | 0xfff80000u : imm);
}
static int tag_cbuf(u32 idx, u32 off) {
    return idx == 0 && off >= 0x110 && off < 0x410 && !(off & 15) ? (int)off + 1 : 0;
}
static int buffer_index(ShaderInfo* si, int tag) {
    if (!tag) return -1;
    u32 off = (u32)(tag - 1);
    for (int i = 0; i < si->ngbuf; i++)
        if (si->gbuf_cbuf_off[i] == off) return i;
    if (si->ngbuf == 16) return -1;
    si->gbuf_cbuf_off[si->ngbuf] = off;
    return si->ngbuf++;
}
static int supported(u64 w, int op) {
    switch (op) {
        case O_DEPBAR:
        case O_MEMBAR: return 1;
        case O_EXIT: return B(w, 0, 5) == 15;
        case O_S2R: return (B(w, 20, 8) >= 0x21 && B(w, 20, 8) <= 0x23) || (B(w, 20, 8) >= 0x25 && B(w, 20, 8) <= 0x27);
        case O_MOV_R:
        case O_MOV_C: return B(w, 39, 4) == 1 || B(w, 39, 4) == 15;
        case O_MOV32I: return B(w, 12, 4) == 1 || B(w, 12, 4) == 15;
        /* saturation, SHL's carry and rounding other than RN aren't translated here: the full translator gets them */
        case O_IADD_R:
        case O_IADD_C:
        case O_IADD_I: return !B(w, 50, 1);
        case O_IADD32I: return !B(w, 54, 1);
        case O_ISCADD_R:
        case O_ISCADD_C:
        case O_ISCADD_I: return 1;
        case O_ISET_R:
        case O_ISET_C:
        case O_ISET_I:
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I: return !B(w, 43, 1) && !B(w, 47, 1);
        case O_SHL_R:
        case O_SHL_C:
        case O_SHL_I: return !B(w, 43, 1) && !B(w, 47, 1);
        case O_FMUL_R:
        case O_FMUL_C:
        case O_FMUL_I: return B(w, 41, 3) == 0 && !B(w, 50, 1) && !B(w, 47, 1) && B(w, 44, 2) <= 1 && !B(w, 39, 2);
        case O_FFMA_R:
        case O_FFMA_RC:
        case O_FFMA_CR:
        case O_FFMA_I: return !B(w, 50, 1) && !B(w, 47, 1) && B(w, 53, 2) <= 1 && !B(w, 51, 2);
        case O_LDG:
        case O_STG: return B(w, 48, 3) >= 4 && B(w, 48, 3) <= 6;
        default: return 0;
    }
}
static void unsupported(ShaderInfo* si, const char* name) {
    si->unsupported = 1;
    snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s", name);
}

/* words: code's as host numbers (shader_words) */
static u32* compute_to_spirv(const u8* code, const u64* words, u32 max_bytes, ShaderInfo* si, size_t* word_count,
                             int can_flush, int can_preserve) {
    if (!code || !si || !word_count || max_bytes < 96 || !si->cs_local[0] || !si->cs_local[1] || !si->cs_local[2])
        return NULL;
    u32 len;
    shader_hash(code, max_bytes, &len);
    u32 n = len / 8;
    u8 used[255] = {0}, pred_used[7] = {0}, cb_used[18] = {0};
    u32 cb_max[18] = {0};
    int tags[255] = {0}, gb_at[(0x10000 + 80) / 8] = {0};
    if (n > sizeof gb_at / sizeof *gb_at) {
        unsupported(si, "compute program length");
        return NULL;
    }
    int flush = 0, preserve = 0, needs_carry = 0, needs_group = 0, needs_local = 0;
    ShaderInfo meta = {0};
    meta.stage = ST_CS;
    memcpy(meta.cs_local, si->cs_local, sizeof meta.cs_local);
    meta.shared_size = si->shared_size;
    for (u32 i = 11; i < n; i++) {
        if (!IDX_OK(i)) continue;
        u64 w = words[i];
        const Op* op = decode(w);
        if (!op || !supported(w, op->id)) {
            unsupported(si, op ? op->name : "unknown opcode");
            return NULL;
        }
        u32 d = B(w, 0, 8), a = B(w, 8, 8), b = B(w, 20, 8), id = op->id;
        u32 guard = B(w, 16, 3);
        if (guard < 7) pred_used[guard] = 1;
        if (id == O_EXIT) { continue; }
        if (id == O_DEPBAR || id == O_MEMBAR) continue;
        if (d < 255 && id != O_STG && id != O_ISETP_R && id != O_ISETP_C && id != O_ISETP_I) used[d] = 1;
        if (a < 255 && id != O_MOV_R && id != O_MOV_C && id != O_MOV32I && id != O_S2R && id != O_ISETP_R &&
            id != O_ISETP_C && id != O_ISETP_I)
            used[a] = 1;
        if ((id == O_ISETP_R || id == O_ISETP_C || id == O_ISETP_I) && a < 255) used[a] = 1;
        if ((id == O_MOV_R || id == O_IADD_R || id == O_ISCADD_R || id == O_ISET_R || id == O_ISETP_R ||
             id == O_SHL_R || id == O_FMUL_R || id == O_FFMA_R || id == O_FFMA_RC) &&
            b < 255)
            used[b] = 1;
        if (id == O_FFMA_R || id == O_FFMA_RC || id == O_FFMA_CR || id == O_FFMA_I) {
            u32 third = B(w, 39, 8);
            if (third < 255) used[third] = 1;
            if (B(w, 53, 2))
                flush = 1;
            else
                preserve = 1;
        }
        if (id == O_FMUL_R || id == O_FMUL_C || id == O_FMUL_I) {
            if (B(w, 44, 2))
                flush = 1;
            else
                preserve = 1;
        }
        if (id == O_S2R) {
            needs_local |= b >= 0x21 && b <= 0x23;
            needs_group |= b >= 0x25 && b <= 0x27;
        }
        if (id == O_ISETP_R || id == O_ISETP_C || id == O_ISETP_I) {
            u32 pa = B(w, 3, 3), pb = B(w, 0, 3), pc = B(w, 39, 3);
            if (pa < 7) pred_used[pa] = 1;
            if (pb < 7) pred_used[pb] = 1;
            if (pc < 7) pred_used[pc] = 1;
        }
        if (id == O_ISET_R || id == O_ISET_C || id == O_ISET_I) {
            u32 pc = B(w, 39, 3);
            if (pc < 7) pred_used[pc] = 1;
        }
        if (id == O_IADD32I) needs_carry |= B(w, 53, 1) || B(w, 52, 1);
        if (id == O_IADD_R || id == O_IADD_C || id == O_IADD_I) needs_carry |= B(w, 43, 1) || B(w, 47, 1);
        if (id == O_ISCADD_R || id == O_ISCADD_C || id == O_ISCADD_I) needs_carry |= B(w, 47, 1);
        if (id == O_MOV_C || id == O_IADD_C || id == O_ISCADD_C || id == O_ISET_C || id == O_ISETP_C ||
            id == O_FMUL_C || id == O_FFMA_RC || id == O_FFMA_CR || id == O_SHL_C) {
            u32 idx = B(w, 34, 5), off = B(w, 20, 14) * 4;
            if (idx >= 18) {
                unsupported(si, "constant buffer index");
                return NULL;
            }
            cb_used[idx] = 1;
            if ((off | 15u) + 1 > cb_max[idx]) cb_max[idx] = (off | 15u) + 1;
        }
        if (id == O_MOV_C && d < 255)
            tags[d] = tag_cbuf(B(w, 34, 5), B(w, 20, 14) * 4);
        else if (id == O_MOV_R && d < 255)
            tags[d] = b == 255 ? 0 : tags[b];
        else if (id == O_IADD_C && d < 255) {
            int tag = tag_cbuf(B(w, 34, 5), B(w, 20, 14) * 4);
            tags[d] = B(w, 48, 2) == 1 ? 0 : tag ? tag : B(w, 48, 2) != 2 && a != 255 ? tags[a] : 0;
        } else if (id == O_IADD_R && d < 255)
            tags[d] = B(w, 48, 2) == 1      ? (a == 255 ? 0 : tags[a])
                      : B(w, 48, 2) == 2    ? (b == 255 ? 0 : tags[b])
                      : a != 255 && tags[a] ? tags[a]
                      : b == 255            ? 0
                                            : tags[b];
        else if (id == O_ISCADD_C && d < 255)
            tags[d] = B(w, 48, 2) == 1 ? 0 : tag_cbuf(B(w, 34, 5), B(w, 20, 14) * 4);
        else if (id == O_ISCADD_R && d < 255)
            tags[d] = B(w, 48, 2) == 1 ? 0 : b == 255 ? 0 : tags[b];
        else if (id == O_IADD32I && d < 255) /* an offset from a pointer stays in its buffer */
            tags[d] = a == 255 || B(w, 55, 2) == 2 ? 0 : tags[a];
        else if (d < 255 && id != O_LDG && id != O_STG && id != O_ISETP_R && id != O_ISETP_C && id != O_ISETP_I)
            tags[d] = 0;
        if (id == O_LDG || id == O_STG) {
            int gi = buffer_index(&meta, a == 255 ? 0 : tags[a]);
            if (gi < 0) {
                si->unsupported = 1;
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s at %u R%u tag %d", op->name, i, a,
                         a == 255 ? 0 : tags[a]);
                return NULL;
            }
            gb_at[i] = gi + 1;
            cb_used[0] = 1;
            u32 off = meta.gbuf_cbuf_off[gi];
            if ((off | 15u) + 1 > cb_max[0]) cb_max[0] = (off | 15u) + 1;
            if (id == O_STG) meta.gbuf_wmask |= 1u << gi;
            u32 count = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) == 6 ? 4 : 1;
            if (d != 255 && d + count > 255) {
                unsupported(si, "global memory register range");
                return NULL;
            }
            for (u32 k = 0; k < count; k++)
                if (d + k < 255) used[d + k] = 1;
            if (id == O_LDG)
                for (u32 k = 0; k < count; k++)
                    if (d + k < 255) tags[d + k] = 0;
        }
    }
    if (flush && preserve) {
        unsupported(si, "compute control flow or FP32 mode");
        return NULL;
    }
    CsDirect* c = calloc(1, sizeof *c);
    if (!c) return NULL;
    spv_emit_init(&c->e, (size_t)n * 30 + 256);
    spv_emit_shader_capability(&c->e);
    c->glsl = spv_emit_glsl450_import(&c->e);
    u32 fn = spv_emit_id(&c->e), label = spv_emit_id(&c->e);
    u32 vt = spv_emit_type_void(&c->e);
    c->u = spv_emit_type_u32(&c->e);
    c->f = spv_emit_type_f32(&c->e);
    c->b = spv_emit_type_bool(&c->e);
    u32 v3 = spv_emit_type_vec3(&c->e, c->u), v4 = spv_emit_type_vec4(&c->e, c->u);
    u32 fnt = spv_emit_type_function_void(&c->e, vt);
    u32 rp = spv_emit_type_function_pointer(&c->e, c->u), pp = spv_emit_type_function_pointer(&c->e, c->b);
    c->zero = cu(c, 0);
    c->yes = spv_emit_constant_bool(&c->e, c->b, 1);
    c->no = spv_emit_constant_bool(&c->e, c->b, 0);
    u32 interfaces[36], ninterface = 0;
    if (needs_group) interfaces[ninterface++] = c->group = spv_emit_builtin_input_vec3(&c->e, v3, 26);
    if (needs_local) interfaces[ninterface++] = c->local = spv_emit_builtin_input_vec3(&c->e, v3, 27);
    for (u32 i = 0; i < 18; i++)
        if (cb_used[i]) {
            c->cb[i] = spv_emit_uniform_vec4_array(&c->e, v4, c->u, c->u, 0, i, 4096, &c->cbptr[i]);
            interfaces[ninterface++] = c->cb[i];
            meta.cbuf_mask |= 1u << i;
            meta.cbuf_max[i] = cb_max[i];
        }
    if (meta.ngbuf) {
        u32 arr = spv_emit_type_runtime_array(&c->e, c->u, 4);
        u32 structure = spv_emit_type_struct_one(&c->e, arr);
        c->gbptr = spv_emit_type_storage_pointer(&c->e, c->u);
        for (int i = 0; i < meta.ngbuf; i++) {
            c->gb[i] = spv_emit_storage_buffer(&c->e, structure, 0, 18 + (u32)i);
            interfaces[ninterface++] = c->gb[i];
        }
    }
    spv_emit_compute_entry_str(&c->e, fn, "main", interfaces, ninterface);
    spv_emit_local_size(&c->e, fn, meta.cs_local[0], meta.cs_local[1], meta.cs_local[2]);
    spv_emit_float_controls(&c->e, fn, flush && can_flush == SH_FTZ_MODE, can_preserve, 0);
    spv_emit_function_void(&c->e, vt, fn, fnt);
    spv_emit_label(&c->e, label);
    for (u32 i = 0; i < 255; i++)
        if (used[i]) c->reg[i] = spv_emit_function_variable_init(&c->e, rp, c->zero);
    for (u32 i = 0; i < 7; i++)
        if (pred_used[i]) c->pred[i] = spv_emit_function_variable_init(&c->e, pp, c->no);
    if (needs_carry) c->carry = spv_emit_function_variable_init(&c->e, pp, c->no);
    int terminated = 0;
    for (u32 i = 11; i < n; i++) {
        if (!IDX_OK(i)) continue;
        u64 w = words[i];
        u32 id = decode(w)->id, d = B(w, 0, 8), a = B(w, 8, 8), b = B(w, 20, 8);
        u32 merge = 0;
        if (B(w, 16, 3) != 7 || B(w, 19, 1)) {
            u32 exec = spv_emit_id(&c->e);
            merge = spv_emit_id(&c->e);
            u32 condition = cp(c, B(w, 16, 3), B(w, 19, 1));
            spv_emit_selection_merge(&c->e, merge);
            spv_emit_branch_conditional(&c->e, condition, exec, merge);
            spv_emit_label(&c->e, exec);
        }
        if (id == O_EXIT)
            spv_emit_return(&c->e);
        else if (id == O_MEMBAR) {
            /* CTA orders workgroup memory, anything else device memory. Image | AtomicCounter | Workgroup |
               Uniform | AcquireRelease, like vektor's barrier. A memory fence only: never execution. */
            spv_emit_memory_barrier(&c->e, c->u, B(w, 8, 2) == 0 ? 2 : 1, 0xD48);
        } else if (id == O_S2R) {
            u32 vec = spv_emit_load(&c->e, v3, b <= 0x23 ? c->local : c->group);
            cw(c, d, spv_emit_vec4_extract(&c->e, c->u, vec, (b <= 0x23 ? b - 0x21 : b - 0x25)));
        } else if (id == O_MOV32I)
            cw(c, d, cu(c, B(w, 20, 32)));
        else if (id == O_MOV_R || id == O_MOV_C)
            cw(c, d, id == O_MOV_R ? cr(c, b) : right(c, w, 1));
        else if (id == O_IADD_R || id == O_IADD_C || id == O_IADD_I || id == O_IADD32I || id == O_ISCADD_R ||
                 id == O_ISCADD_C || id == O_ISCADD_I) {
            int imm32 = id == O_IADD32I;
            int scaled = id == O_ISCADD_R || id == O_ISCADD_C || id == O_ISCADD_I;
            int kind = imm32 ? 3 : id == O_IADD_R || id == O_ISCADD_R ? 0 : id == O_IADD_C || id == O_ISCADD_C ? 1 : 2;
            u32 av = cr(c, a), bv = kind == 3 ? cu(c, B(w, 20, 32)) : right(c, w, kind);
            u32 avg = imm32 ? B(w, 55, 2) : B(w, 48, 2);
            int x = imm32 ? B(w, 53, 1) : !scaled && B(w, 43, 1);
            int cc = imm32 ? B(w, 52, 1) : B(w, 47, 1);
            if (scaled) av = spv_emit_shift_left_u32(&c->e, c->u, av, cu(c, B(w, 39, 5)));
            if (avg == 2)
                av = cc || x ? spv_emit_not_u32(&c->e, c->u, av) : spv_emit_isub_u32(&c->e, c->u, c->zero, av);
            if (avg == 1)
                bv = cc || x ? spv_emit_not_u32(&c->e, c->u, bv) : spv_emit_isub_u32(&c->e, c->u, c->zero, bv);
            u32 sum = spv_emit_iadd_u32(&c->e, c->u, av, bv);
            u32 carry = spv_emit_u_less(&c->e, c->b, sum, av);
            u32 cin = cc || x ? (avg ? cu(c, 1) : c->zero) : avg == 3 ? cu(c, 1) : c->zero;
            if (x) {
                u32 old = spv_emit_select(&c->e, c->u, spv_emit_load(&c->e, c->b, c->carry), cu(c, 1), c->zero);
                cin = spv_emit_iadd_u32(&c->e, c->u, cin, old);
            }
            if (cc || x || avg) {
                u32 next = spv_emit_iadd_u32(&c->e, c->u, sum, cin);
                if (cc) {
                    u32 extra = spv_emit_u_less(&c->e, c->b, next, sum);
                    spv_emit_store(&c->e, c->carry, spv_emit_bool_binary(&c->e, c->b, 1, carry, extra));
                }
                sum = next;
            }
            cw(c, d, sum);
        } else if (id == O_SHL_R || id == O_SHL_C || id == O_SHL_I) {
            u32 shift = right(c, w, id - O_SHL_R);
            u32 safe = B(w, 39, 1) ? spv_emit_bitwise_and_u32(&c->e, c->u, shift, cu(c, 31)) : shift;
            u32 result = spv_emit_shift_left_u32(&c->e, c->u, cr(c, a), safe);
            if (!B(w, 39, 1))
                result = spv_emit_select(&c->e, c->u, spv_emit_u_less(&c->e, c->b, shift, cu(c, 32)), result, c->zero);
            cw(c, d, result);
        } else if (id == O_ISET_R || id == O_ISET_C || id == O_ISET_I || id == O_ISETP_R || id == O_ISETP_C ||
                   id == O_ISETP_I) {
            int ispred = id == O_ISETP_R || id == O_ISETP_C || id == O_ISETP_I;
            u32 lhs = cr(c, a), rhs = right(c, w, id - (ispred ? O_ISETP_R : O_ISET_R));
            u32 op = B(w, 49, 3);
            int sign = B(w, 48, 1);
            u32 less = sign ? spv_emit_s_greater(&c->e, c->b, rhs, lhs) : spv_emit_u_less(&c->e, c->b, lhs, rhs);
            u32 greater = sign ? spv_emit_s_greater(&c->e, c->b, lhs, rhs) : spv_emit_u_less(&c->e, c->b, rhs, lhs);
            u32 equal = spv_emit_i_equal(&c->e, c->b, lhs, rhs);
            u32 test = op == 0   ? c->no
                       : op == 1 ? less
                       : op == 2 ? equal
                       : op == 3 ? spv_emit_bool_not(&c->e, c->b, greater)
                       : op == 4 ? greater
                       : op == 5 ? spv_emit_bool_not(&c->e, c->b, equal)
                       : op == 6 ? spv_emit_bool_not(&c->e, c->b, less)
                                 : c->yes;
            u32 pc = cp(c, B(w, 39, 3), B(w, 42, 1));
            u32 bop = B(w, 45, 2);
            u32 inverse = spv_emit_bool_not(&c->e, c->b, test);
            if (bop < 3) {
                test = spv_emit_bool_binary(&c->e, c->b, bop, test, pc);
                inverse = spv_emit_bool_binary(&c->e, c->b, bop, inverse, pc);
            }
            if (ispred) {
                pw(c, B(w, 3, 3), test);
                pw(c, B(w, 0, 3), inverse);
            } else
                cw(c, d, spv_emit_select(&c->e, c->u, test, cu(c, B(w, 44, 1) ? 0x3f800000u : 0xffffffffu), c->zero));
        } else if (id == O_FMUL_R || id == O_FMUL_C || id == O_FMUL_I || id == O_FFMA_R || id == O_FFMA_RC ||
                   id == O_FFMA_CR || id == O_FFMA_I) {
            int fma = id >= O_FFMA_R && id <= O_FFMA_I;
            u32 lhs = spv_emit_bitcast(&c->e, c->f, cr(c, a));
            u32 rhs = fma && id == O_FFMA_CR             ? right(c, w, 1)
                      : fma && id == O_FFMA_RC           ? cr(c, B(w, 39, 8))
                      : id == O_FFMA_I || id == O_FMUL_I ? cu(c, (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31))
                                                         : right(c, w, fma ? 0 : id - O_FMUL_R);
            rhs = spv_emit_bitcast(&c->e, c->f, rhs);
            if (fma && B(w, 48, 1)) rhs = spv_emit_fnegate(&c->e, c->f, rhs);
            if (!fma && B(w, 48, 1)) rhs = spv_emit_fnegate(&c->e, c->f, rhs);
            u32 result;
            if (fma) {
                u32 add = id == O_FFMA_RC ? right(c, w, 1) : cr(c, B(w, 39, 8));
                add = spv_emit_bitcast(&c->e, c->f, add);
                if (B(w, 49, 1)) add = spv_emit_fnegate(&c->e, c->f, add);
                result = spv_emit_glsl_ternary(&c->e, c->f, c->glsl, SPV_GLSL_FMA, lhs, rhs, add);
            } else {
                result = spv_emit_fmul(&c->e, c->f, lhs, rhs);
                spv_emit_no_contraction(&c->e, result);
            }
            cw(c, d, spv_emit_bitcast(&c->e, c->u, result));
        } else if (id == O_LDG || id == O_STG) {
            u32 gi = (u32)gb_at[i] - 1;
            i32 imm = (i32)(B(w, 20, 24) << 8) >> 8;
            u32 addr = spv_emit_iadd_u32(&c->e, c->u, cr(c, a), cu(c, (u32)imm));
            addr = spv_emit_isub_u32(&c->e, c->u, addr, cbread(c, 0, meta.gbuf_cbuf_off[gi]));
            u32 index = spv_emit_shift_right_u32(&c->e, c->u, addr, cu(c, 2));
            u32 count = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) == 6 ? 4 : 1;
            for (u32 k = 0; k < count; k++) {
                u32 ix = k ? spv_emit_iadd_u32(&c->e, c->u, index, cu(c, k)) : index;
                u32 ptr = spv_emit_storage_access_u32(&c->e, c->gbptr, c->gb[gi], c->zero, ix);
                u32 r = d == 255 ? 255 : d + k; /* RZ stands for every register of a wide access */
                if (id == O_LDG)
                    cw(c, r, spv_emit_load(&c->e, c->u, ptr));
                else
                    spv_emit_store_u32(&c->e, ptr, cr(c, r));
            }
        }
        if (merge) {
            if (id != O_EXIT) spv_emit_branch(&c->e, merge);
            spv_emit_label(&c->e, merge);
        } else if (id == O_EXIT) {
            terminated = 1;
            break;
        }
    }
    if (!terminated) spv_emit_return(&c->e);
    spv_emit_function_end(&c->e);
    u32* result = spv_emit_finish(&c->e, word_count);
    spv_emit_destroy(&c->e);
    free(c);
    *si = meta;
    return result;
}
u32* shader_compute_to_spirv(const u8* code, u32 max_bytes, ShaderInfo* si, size_t* word_count, int can_flush,
                             int can_preserve) {
    u64* copy;
    const u64* words;
    u32* r;
    if (!code) return NULL;
    words = shader_words(code, max_bytes, &copy);
    if (!words) return NULL;
    r = compute_to_spirv(code, words, max_bytes, si, word_count, can_flush, can_preserve);
    free(copy);
    return r;
}
