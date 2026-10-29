/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/bits.h"
#include "common/utilities/spirv_emit.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void reserve(SpvWords* s, size_t more) {
    assert(more <= SIZE_MAX - s->count);
    size_t need = s->count + more;
    if (need <= s->capacity) return;
    size_t cap = s->capacity ? s->capacity : 64;
    while (cap < need) {
        assert(cap <= SIZE_MAX / 2);
        cap *= 2;
    }
    assert(cap <= SIZE_MAX / sizeof(uint32_t));
    uint32_t* p = realloc(s->words, cap * sizeof(uint32_t));
    if (!p) abort();
    s->words = p;
    s->capacity = cap;
}

static void instruction(SpvWords* s, uint32_t op, uint32_t words) {
    assert(op <= UINT16_MAX && words > 0 && words <= UINT16_MAX);
    reserve(s, words);
    s->words[s->count++] = (words << 16) | op;
}

static void put(SpvWords* s, uint32_t word) {
    assert(s->count < s->capacity);
    s->words[s->count++] = word;
}

static size_t string_words(const char* str) {
    size_t n = strlen(str);
    assert(n <= SIZE_MAX - 4);
    return (n + 4) / 4;
}

/* word i of a string's words (n its bytes with the terminator): little-endian, padded with zeros */
static uint32_t string_word(const char* str, size_t n, size_t i) {
    uint32_t word = 0;
    for (size_t j = 0; j < 4 && i * 4 + j < n; j++) word |= (uint32_t)(uint8_t)str[i * 4 + j] << (8 * j);
    return word;
}

static void put_str(SpvWords* s, const char* str) {
    size_t n = strlen(str) + 1;
    for (size_t i = 0; i * 4 < n; i++) put(s, string_word(str, n, i));
}

/* a type (the id first, at = 1) or constant (its type first, the id second, at = 2) of these words in types, made
   once */
static uint32_t known(SpvEmitter* e, uint32_t op, size_t at, const uint32_t* operands, size_t count) {
    uint64_t h = SNX_FNV_BASIS ^ op;
    for (size_t i = 0; i < count; i++) h = snx_fnv_step(h, operands[i]);
    for (size_t k = 0; k < e->known_count; k++) {
        if (e->known[k].hash != h) continue;
        const uint32_t* w = e->types.words + e->known[k].at;
        if ((w[0] & 0xffff) != op || w[0] >> 16 != count + 2) continue;
        int same = 1;
        for (size_t i = 0, j = 1; i < count && same; i++, j++) {
            if (j == at) j++;
            same = w[j] == operands[i];
        }
        if (same) return e->known[k].id;
    }
    if (e->known_count == e->known_capacity) {
        size_t capacity = e->known_capacity ? e->known_capacity * 2 : 128;
        SpvKnown* grown = realloc(e->known, capacity * sizeof *grown);
        if (!grown) abort();
        e->known = grown;
        e->known_capacity = capacity;
    }
    uint32_t id = spv_emit_id(e);
    e->known[e->known_count++] = (SpvKnown){h, e->types.count, id};
    instruction(&e->types, op, (uint32_t)(count + 2));
    for (size_t i = 0, j = 1; j < count + 2; j++) put(&e->types, j == at ? id : operands[i++]);
    return id;
}

void spv_emit_init(SpvEmitter* e, size_t estimated_function_words) {
    *e = (SpvEmitter){0};
    e->next_id = 1;
    e->version = 0x00010600; /* SPIR-V 1.6 */
    /* Most instructions are in function bodies. Reserve from the guest program size once. */
    if (estimated_function_words) reserve(&e->functions, estimated_function_words);
}

void spv_emit_destroy(SpvEmitter* e) {
    SpvWords* sections[] = {&e->capabilities, &e->extensions,  &e->imports, &e->entry_points, &e->execution_modes,
                            &e->debug,        &e->decorations, &e->types,   &e->functions, &e->locals};
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) free(sections[i]->words);
    free(e->known);
    free(e->interface);
    free(e->nonuniform);
    free(e->constants);
    free(e->constant_ids);
    *e = (SpvEmitter){0};
}

uint32_t spv_emit_id(SpvEmitter* e) {
    assert(e->next_id && e->next_id < UINT32_MAX);
    return e->next_id++;
}

uint32_t spv_emit_glsl450_import(SpvEmitter* e) {
    const char* name = "GLSL.std.450";
    if (e->glsl) return e->glsl;
    uint32_t id = e->glsl = spv_emit_id(e);
    instruction(&e->imports, SPV_OP_EXT_INST_IMPORT, (uint32_t)(2 + string_words(name)));
    put(&e->imports, id);
    put_str(&e->imports, name);
    return id;
}

void spv_emit_shader_capability(SpvEmitter* e) { spv_emit_capability(e, SPV_CAPABILITY_SHADER); }
void spv_emit_float16_capability(SpvEmitter* e) { spv_emit_capability(e, SPV_CAPABILITY_FLOAT16); }
void spv_emit_demote_extension(SpvEmitter* e) { spv_emit_extension(e, "SPV_EXT_demote_to_helper_invocation"); }
void spv_emit_shader_clock_extension(SpvEmitter* e) {
    spv_emit_extension(e, "SPV_KHR_shader_clock");
    spv_emit_capability(e, SPV_CAPABILITY_SHADER_CLOCK_KHR);
}
void spv_emit_float64_capability(SpvEmitter* e) { spv_emit_capability(e, SPV_CAPABILITY_FLOAT64); }

void spv_emit_draw_parameters_capability(SpvEmitter* e) { spv_emit_capability(e, SPV_CAPABILITY_DRAW_PARAMETERS); }

void spv_emit_layer_capability(SpvEmitter* e) { spv_emit_capability(e, SPV_CAPABILITY_SHADER_LAYER); }

void spv_emit_clip_distance_capability(SpvEmitter* e) { spv_emit_capability(e, SPV_CAPABILITY_CLIP_DISTANCE); }

void spv_emit_image_query_capability(SpvEmitter* e) { spv_emit_capability(e, SPV_CAPABILITY_IMAGE_QUERY); }

void spv_emit_subgroup_capabilities(SpvEmitter* e) {
    spv_emit_capability(e, SPV_CAPABILITY_GROUP_NON_UNIFORM);
    spv_emit_capability(e, SPV_CAPABILITY_GROUP_NON_UNIFORM_BALLOT);
    spv_emit_capability(e, SPV_CAPABILITY_GROUP_NON_UNIFORM_SHUFFLE);
}

void spv_emit_image_gather_extended_capability(SpvEmitter* e) {
    spv_emit_capability(e, SPV_CAPABILITY_IMAGE_GATHER_EXTENDED);
}

void spv_emit_entry_str(SpvEmitter* e, SpvShaderStage stage, uint32_t function, const char* name,
                        const uint32_t* interfaces, size_t interface_count) {
    assert(stage >= SPV_STAGE_VERTEX && stage <= SPV_STAGE_COMPUTE);
    size_t n = string_words(name);
    assert(n <= UINT16_MAX - 3 && interface_count <= UINT16_MAX - 3 - n);
    instruction(&e->entry_points, SPV_OP_ENTRY_POINT, (uint32_t)(3 + n + interface_count));
    put(&e->entry_points, (uint32_t)stage);
    put(&e->entry_points, function);
    put_str(&e->entry_points, name);
    for (size_t i = 0; i < interface_count; i++) put(&e->entry_points, interfaces[i]);
}

void spv_emit_compute_entry_str(SpvEmitter* e, uint32_t function, const char* name, const uint32_t* interfaces,
                                size_t interface_count) {
    spv_emit_entry_str(e, SPV_STAGE_COMPUTE, function, name, interfaces, interface_count);
}

void spv_emit_fragment_origin_upper_left(SpvEmitter* e, uint32_t function) {
    instruction(&e->execution_modes, SPV_OP_EXECUTION_MODE, 3);
    put(&e->execution_modes, function);
    put(&e->execution_modes, 7); /* OriginUpperLeft */
}

void spv_emit_fragment_depth_replacing(SpvEmitter* e, uint32_t function) {
    instruction(&e->execution_modes, SPV_OP_EXECUTION_MODE, 3);
    put(&e->execution_modes, function);
    put(&e->execution_modes, 12); /* DepthReplacing */
}

void spv_emit_float_controls(SpvEmitter* e, uint32_t function, int flush_denorms, int preserve_infnan,
                             int preserve_infnan64) {
    if (!flush_denorms && !preserve_infnan && !preserve_infnan64) return;
    spv_emit_extension(e, "SPV_KHR_float_controls");
    if (flush_denorms) {
        spv_emit_capability(e, SPV_CAPABILITY_DENORM_FLUSH_TO_ZERO);
        spv_emit_execution_mode(e, function, SPV_MODE_DENORM_FLUSH_TO_ZERO, 1, 32);
    }
    if (preserve_infnan) {
        spv_emit_capability(e, SPV_CAPABILITY_SIGNED_ZERO_INF_NAN_PRESERVE);
        spv_emit_execution_mode(e, function, SPV_MODE_SIGNED_ZERO_INF_NAN_PRESERVE, 1, 32);
    }
    if (preserve_infnan64) {
        spv_emit_capability(e, SPV_CAPABILITY_SIGNED_ZERO_INF_NAN_PRESERVE);
        spv_emit_execution_mode(e, function, SPV_MODE_SIGNED_ZERO_INF_NAN_PRESERVE, 1, 64);
    }
}

void spv_emit_local_size(SpvEmitter* e, uint32_t function, uint32_t x, uint32_t y, uint32_t z) {
    assert(x && y && z);
    instruction(&e->execution_modes, SPV_OP_EXECUTION_MODE, 6);
    put(&e->execution_modes, function);
    put(&e->execution_modes, 17); /* LocalSize */
    put(&e->execution_modes, x);
    put(&e->execution_modes, y);
    put(&e->execution_modes, z);
}

void spv_emit_name_str(SpvEmitter* e, uint32_t id, const char* name) {
    size_t n = string_words(name);
    assert(n <= UINT16_MAX - 2);
    instruction(&e->debug, SPV_OP_NAME, (uint32_t)(2 + n));
    put(&e->debug, id);
    put_str(&e->debug, name);
}

uint32_t spv_emit_type_void(SpvEmitter* e) { return known(e, SPV_OP_TYPE_VOID, 1, NULL, 0); }

uint32_t spv_emit_type_bool(SpvEmitter* e) { return known(e, SPV_OP_TYPE_BOOL, 1, NULL, 0); }

uint32_t spv_emit_type_u32(SpvEmitter* e) { return spv_emit_type_int(e, 32, 0); }

uint32_t spv_emit_type_i32(SpvEmitter* e) { return spv_emit_type_int(e, 32, 1); }

uint32_t spv_emit_type_int64(SpvEmitter* e, int is_signed) { return spv_emit_type_int(e, 64, is_signed); }

uint32_t spv_emit_int_convert(SpvEmitter* e, uint32_t type, uint32_t value, int is_signed) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, is_signed ? SPV_OP_S_CONVERT : SPV_OP_U_CONVERT, 4);
    put(&e->functions, type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_fconvert(SpvEmitter* e, uint32_t type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_F_CONVERT, 4);
    put(&e->functions, type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_convert_f_to_s(SpvEmitter* e, uint32_t i32_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_CONVERT_F_TO_S, 4);
    put(&e->functions, i32_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_convert_f_to_u(SpvEmitter* e, uint32_t u32_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_CONVERT_F_TO_U, 4);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_convert_s_to_f(SpvEmitter* e, uint32_t f32_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_CONVERT_S_TO_F, 4);
    put(&e->functions, f32_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_convert_u_to_f(SpvEmitter* e, uint32_t f32_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_CONVERT_U_TO_F, 4);
    put(&e->functions, f32_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_convert_f_to_f(SpvEmitter* e, uint32_t result_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_F_CONVERT, 4);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_type_f16(SpvEmitter* e) { return spv_emit_type_float(e, 16); }
uint32_t spv_emit_type_f32(SpvEmitter* e) { return spv_emit_type_float(e, 32); }

uint32_t spv_emit_type_f64(SpvEmitter* e) { return spv_emit_type_float(e, 64); }

uint32_t spv_emit_type_vec4(SpvEmitter* e, uint32_t component_type) {
    return spv_emit_type_vector(e, component_type, 4);
}

uint32_t spv_emit_type_vec2(SpvEmitter* e, uint32_t component_type) {
    return spv_emit_type_vector(e, component_type, 2);
}

uint32_t spv_emit_type_vec3(SpvEmitter* e, uint32_t component_type) {
    return spv_emit_type_vector(e, component_type, 3);
}

uint32_t spv_emit_type_runtime_array(SpvEmitter* e, uint32_t element_type, uint32_t stride) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, 6); /* ArrayStride */
    put(&e->decorations, stride);
    instruction(&e->types, SPV_OP_TYPE_RUNTIME_ARRAY, 3);
    put(&e->types, id);
    put(&e->types, element_type);
    return id;
}

uint32_t spv_emit_type_struct_one(SpvEmitter* e, uint32_t member_type) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->decorations, SPV_OP_DECORATE, 3);
    put(&e->decorations, id);
    put(&e->decorations, 2); /* Block */
    instruction(&e->decorations, SPV_OP_MEMBER_DECORATE, 5);
    put(&e->decorations, id);
    put(&e->decorations, 0);
    put(&e->decorations, 35); /* Offset */
    put(&e->decorations, 0);
    instruction(&e->types, SPV_OP_TYPE_STRUCT, 3);
    put(&e->types, id);
    put(&e->types, member_type);
    return id;
}


uint32_t spv_emit_type_storage_pointer(SpvEmitter* e, uint32_t pointee_type) {
    return spv_emit_type_pointer(e, 12, pointee_type);
}

uint32_t spv_emit_type_function_pointer(SpvEmitter* e, uint32_t pointee_type) {
    return spv_emit_type_pointer(e, 7, pointee_type);
}

uint32_t spv_emit_type_workgroup_pointer(SpvEmitter* e, uint32_t pointee_type) {
    return spv_emit_type_pointer(e, 4, pointee_type);
}

uint32_t spv_emit_type_array_u32(SpvEmitter* e, uint32_t u32_type, uint32_t length) {
    assert(length);
    uint32_t length_id = spv_emit_constant_u32(e, u32_type, length);
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, SPV_OP_TYPE_ARRAY, 4);
    put(&e->types, id);
    put(&e->types, u32_type);
    put(&e->types, length_id);
    return id;
}

uint32_t spv_emit_workgroup_variable(SpvEmitter* e, uint32_t pointer_type) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, SPV_OP_VARIABLE, 4);
    put(&e->types, pointer_type);
    put(&e->types, id);
    put(&e->types, 4); /* Workgroup */
    return id;
}

uint32_t spv_emit_function_element_pointer(SpvEmitter* e, uint32_t pointer_type, uint32_t array, uint32_t index) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_ACCESS_CHAIN, 5);
    put(&e->functions, pointer_type);
    put(&e->functions, id);
    put(&e->functions, array);
    put(&e->functions, index);
    return id;
}

uint32_t spv_emit_type_function_void(SpvEmitter* e, uint32_t void_type) {
    return spv_emit_type_function(e, void_type, NULL, 0);
}

static size_t constant_slot(uint32_t a, uint32_t b, size_t capacity) {
    uint64_t h = ((uint64_t)a << 32 | b) * 0x9E3779B97F4A7C15ull;
    return (size_t)(h >> 40) & (capacity - 1);
}
static void constant_insert(SpvConstant* table, size_t capacity, const SpvConstant* c, int by_id) {
    size_t i = by_id ? constant_slot(c->id, 0, capacity) : constant_slot(c->type, c->value, capacity);
    while (table[i].id) i = (i + 1) & (capacity - 1);
    table[i] = *c;
}
/* both tables at twice the capacity, the constants made so far in them again */
static void constants_grow(SpvEmitter* e) {
    size_t capacity = e->constant_capacity ? e->constant_capacity * 2 : 256;
    SpvConstant* constants = (SpvConstant*)calloc(capacity, sizeof *constants);
    SpvConstant* ids = (SpvConstant*)calloc(capacity, sizeof *ids);
    if (!constants || !ids) abort();
    for (size_t i = 0; i < e->constant_capacity; i++)
        if (e->constants[i].id) {
            constant_insert(constants, capacity, &e->constants[i], 0);
            constant_insert(ids, capacity, &e->constants[i], 1);
        }
    free(e->constants);
    free(e->constant_ids);
    e->constants = constants;
    e->constant_ids = ids;
    e->constant_capacity = capacity;
}

uint32_t spv_emit_constant_u32(SpvEmitter* e, uint32_t type, uint32_t value) {
    if ((e->constant_count + 1) * 2 > e->constant_capacity) constants_grow(e);
    size_t i = constant_slot(type, value, e->constant_capacity);
    for (; e->constants[i].id; i = (i + 1) & (e->constant_capacity - 1))
        if (e->constants[i].type == type && e->constants[i].value == value) return e->constants[i].id;
    SpvConstant c = {type, value, spv_emit_id(e)};
    instruction(&e->types, SPV_OP_CONSTANT, 4);
    put(&e->types, type);
    put(&e->types, c.id);
    put(&e->types, value);
    e->constants[i] = c;
    constant_insert(e->constant_ids, e->constant_capacity, &c, 1);
    e->constant_count++;
    return c.id;
}

int spv_emit_constant_bits(const SpvEmitter* e, uint32_t id, uint32_t* type, uint32_t* value) {
    if (!e->constant_capacity || !id) return 0;
    for (size_t i = constant_slot(id, 0, e->constant_capacity); e->constant_ids[i].id;
         i = (i + 1) & (e->constant_capacity - 1))
        if (e->constant_ids[i].id == id) {
            *type = e->constant_ids[i].type;
            *value = e->constant_ids[i].value;
            return 1;
        }
    return 0;
}

uint32_t spv_emit_constant64(SpvEmitter* e, uint32_t type, uint64_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, SPV_OP_CONSTANT, 5);
    put(&e->types, type);
    put(&e->types, id);
    put(&e->types, (uint32_t)value); /* the low word first */
    put(&e->types, (uint32_t)(value >> 32));
    return id;
}

uint32_t spv_emit_constant_bool(SpvEmitter* e, uint32_t type, int value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, value ? SPV_OP_CONSTANT_TRUE : SPV_OP_CONSTANT_FALSE, 3);
    put(&e->types, type);
    put(&e->types, id);
    return id;
}

uint32_t spv_emit_constant_f32(SpvEmitter* e, uint32_t type, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return spv_emit_constant_u32(e, type, bits);
}

uint32_t spv_emit_constant_vec4(SpvEmitter* e, uint32_t type, const uint32_t components[4]) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, SPV_OP_CONSTANT_COMPOSITE, 7);
    put(&e->types, type);
    put(&e->types, id);
    for (size_t i = 0; i < 4; i++) put(&e->types, components[i]);
    return id;
}

uint32_t spv_emit_constant_vec2(SpvEmitter* e, uint32_t type, uint32_t x, uint32_t y) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, SPV_OP_CONSTANT_COMPOSITE, 5);
    put(&e->types, type);
    put(&e->types, id);
    put(&e->types, x);
    put(&e->types, y);
    return id;
}

uint32_t spv_emit_constant_vec3(SpvEmitter* e, uint32_t type, uint32_t x, uint32_t y, uint32_t z) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, SPV_OP_CONSTANT_COMPOSITE, 6);
    put(&e->types, type);
    put(&e->types, id);
    put(&e->types, x);
    put(&e->types, y);
    put(&e->types, z);
    return id;
}

uint32_t spv_emit_storage_buffer(SpvEmitter* e, uint32_t struct_type, uint32_t set, uint32_t binding) {
    uint32_t pointer_type = spv_emit_type_storage_pointer(e, struct_type);
    uint32_t id = spv_emit_id(e);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, 34); /* DescriptorSet */
    put(&e->decorations, set);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, 33); /* Binding */
    put(&e->decorations, binding);
    instruction(&e->types, SPV_OP_VARIABLE, 4);
    put(&e->types, pointer_type);
    put(&e->types, id);
    put(&e->types, 12); /* StorageBuffer */
    return id;
}

uint32_t spv_emit_uniform_vec4_array(SpvEmitter* e, uint32_t vec4_type, uint32_t component_type, uint32_t u32_type,
                                     uint32_t set, uint32_t binding, uint32_t elements,
                                     uint32_t* component_pointer_type) {
    assert(elements && component_pointer_type);
    uint32_t length = spv_emit_constant_u32(e, u32_type, elements);
    uint32_t array = spv_emit_id(e);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, array);
    put(&e->decorations, 6); /* ArrayStride */
    put(&e->decorations, 16);
    instruction(&e->types, SPV_OP_TYPE_ARRAY, 4);
    put(&e->types, array);
    put(&e->types, vec4_type);
    put(&e->types, length);
    uint32_t structure = spv_emit_type_struct_one(e, array);
    uint32_t struct_pointer = spv_emit_type_pointer(e, 2, structure); /* Uniform */
    uint32_t variable = spv_emit_id(e);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, variable);
    put(&e->decorations, 34); /* DescriptorSet */
    put(&e->decorations, set);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, variable);
    put(&e->decorations, 33); /* Binding */
    put(&e->decorations, binding);
    instruction(&e->types, SPV_OP_VARIABLE, 4);
    put(&e->types, struct_pointer);
    put(&e->types, variable);
    put(&e->types, 2); /* Uniform */
    *component_pointer_type = spv_emit_type_pointer(e, 2, component_type);
    return variable;
}

uint32_t spv_emit_uniform_component(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member,
                                    uint32_t element, uint32_t component, uint32_t component_type) {
    uint32_t pointer = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_ACCESS_CHAIN, 7);
    put(&e->functions, pointer_type);
    put(&e->functions, pointer);
    put(&e->functions, buffer);
    put(&e->functions, member);
    put(&e->functions, element);
    put(&e->functions, component);
    return spv_emit_load(e, component_type, pointer);
}

uint32_t spv_emit_push_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t f32_type, uint32_t offset,
                            uint32_t second_type, uint32_t* component_pointer_type) {
    assert(component_pointer_type && !(offset & 15));
    uint32_t members = second_type ? 2 : 1;
    uint32_t structure = spv_emit_id(e);
    instruction(&e->decorations, SPV_OP_DECORATE, 3);
    put(&e->decorations, structure);
    put(&e->decorations, 2); /* Block */
    for (uint32_t m = 0; m < members; m++) {
        instruction(&e->decorations, SPV_OP_MEMBER_DECORATE, 5);
        put(&e->decorations, structure);
        put(&e->decorations, m);
        put(&e->decorations, 35); /* Offset */
        put(&e->decorations, offset + 16 * m);
    }
    instruction(&e->types, SPV_OP_TYPE_STRUCT, 2 + members);
    put(&e->types, structure);
    put(&e->types, vec4_type);
    if (second_type) put(&e->types, second_type);
    uint32_t pointer = spv_emit_type_pointer(e, 9, structure); /* PushConstant */
    uint32_t variable = spv_emit_id(e);
    instruction(&e->types, SPV_OP_VARIABLE, 4);
    put(&e->types, pointer);
    put(&e->types, variable);
    put(&e->types, 9);
    *component_pointer_type = spv_emit_type_pointer(e, 9, f32_type);
    return variable;
}

uint32_t spv_emit_push_component_f32(SpvEmitter* e, uint32_t pointer_type, uint32_t block, uint32_t member,
                                     uint32_t component, uint32_t f32_type) {
    uint32_t pointer = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_ACCESS_CHAIN, 6);
    put(&e->functions, pointer_type);
    put(&e->functions, pointer);
    put(&e->functions, block);
    put(&e->functions, member);
    put(&e->functions, component);
    return spv_emit_load(e, f32_type, pointer);
}

static uint32_t output_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t decoration, uint32_t value) {
    uint32_t pointer_type = spv_emit_type_pointer(e, 3, vec4_type); /* Output */
    uint32_t id = spv_emit_id(e);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, decoration);
    put(&e->decorations, value);
    instruction(&e->types, SPV_OP_VARIABLE, 4);
    put(&e->types, pointer_type);
    put(&e->types, id);
    put(&e->types, 3); /* Output */
    return id;
}

uint32_t spv_emit_output_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location) {
    return output_vec4(e, vec4_type, 30, location); /* Location */
}

uint32_t spv_emit_private_variable(SpvEmitter* e, uint32_t type) {
    uint32_t pointer_type = spv_emit_type_pointer(e, 6, type); /* Private */
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, SPV_OP_VARIABLE, 4);
    put(&e->types, pointer_type);
    put(&e->types, id);
    put(&e->types, 6);
    return id;
}

uint32_t spv_emit_output_component(SpvEmitter* e, uint32_t type, uint32_t location, uint32_t component) {
    uint32_t id = output_vec4(e, type, 30, location); /* Location */
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, 31); /* Component */
    put(&e->decorations, component);
    return id;
}

void spv_emit_xfb_capture(SpvEmitter* e, uint32_t variable, uint32_t buffer, uint32_t stride, uint32_t offset) {
    static const uint32_t decorations[3] = {36, 37, 35}; /* XfbBuffer, XfbStride, Offset */
    uint32_t values[3] = {buffer, stride, offset};
    for (uint32_t i = 0; i < 3; i++) {
        instruction(&e->decorations, SPV_OP_DECORATE, 4);
        put(&e->decorations, variable);
        put(&e->decorations, decorations[i]);
        put(&e->decorations, values[i]);
    }
}

uint32_t spv_emit_position_output(SpvEmitter* e, uint32_t vec4_type) {
    uint32_t id = output_vec4(e, vec4_type, 11, 0); /* BuiltIn Position */
    instruction(&e->decorations, SPV_OP_DECORATE, 3);
    put(&e->decorations, id);
    put(&e->decorations, 18); /* Invariant */
    return id;
}

/* SWR (DLSS motion vectors, gpu/vk/dlss.c): Position captured by transform feedback into buffer 0, one vec4 a vertex.
   Inert unless a draw begins transform feedback */
void spv_emit_xfb_position(SpvEmitter* e, uint32_t function, uint32_t position) {
    instruction(&e->capabilities, SPV_OP_CAPABILITY, 2);
    put(&e->capabilities, 53); /* TransformFeedback */
    instruction(&e->execution_modes, SPV_OP_EXECUTION_MODE, 3);
    put(&e->execution_modes, function);
    put(&e->execution_modes, 11); /* Xfb */
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, position);
    put(&e->decorations, 36); /* XfbBuffer */
    put(&e->decorations, 0);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, position);
    put(&e->decorations, 37); /* XfbStride */
    put(&e->decorations, 16);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, position);
    put(&e->decorations, 35); /* Offset */
    put(&e->decorations, 0);
}

uint32_t spv_emit_point_size_output(SpvEmitter* e, uint32_t f32_type) {
    return output_vec4(e, f32_type, 11, 1); /* BuiltIn PointSize */
}

uint32_t spv_emit_layer_output(SpvEmitter* e, uint32_t i32_type) {
    return output_vec4(e, i32_type, 11, 9); /* BuiltIn Layer */
}

uint32_t spv_emit_clip_distance_output(SpvEmitter* e, uint32_t f32_type, uint32_t u32_type, uint32_t length) {
    uint32_t length_id = spv_emit_constant_u32(e, u32_type, length);
    uint32_t array_type = spv_emit_id(e);
    instruction(&e->types, SPV_OP_TYPE_ARRAY, 4);
    put(&e->types, array_type);
    put(&e->types, f32_type);
    put(&e->types, length_id);
    uint32_t pointer_type = spv_emit_type_pointer(e, 3, array_type);
    uint32_t id = spv_emit_id(e);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, 11); /* BuiltIn */
    put(&e->decorations, 3);  /* ClipDistance */
    instruction(&e->types, SPV_OP_VARIABLE, 4);
    put(&e->types, pointer_type);
    put(&e->types, id);
    put(&e->types, 3); /* Output */
    return id;
}

uint32_t spv_emit_output_element_pointer(SpvEmitter* e, uint32_t f32_type, uint32_t array, uint32_t index) {
    return spv_emit_function_element_pointer(e, spv_emit_type_pointer(e, 3, f32_type), array, index);
}

uint32_t spv_emit_frag_depth_output(SpvEmitter* e, uint32_t f32_type) {
    return output_vec4(e, f32_type, 11, 22); /* BuiltIn FragDepth */
}

static uint32_t input_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t decoration, uint32_t value) {
    uint32_t pointer_type = spv_emit_type_pointer(e, 1, vec4_type); /* Input */
    uint32_t id = spv_emit_id(e);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, decoration);
    put(&e->decorations, value);
    instruction(&e->types, SPV_OP_VARIABLE, 4);
    put(&e->types, pointer_type);
    put(&e->types, id);
    put(&e->types, 1); /* Input */
    return id;
}

uint32_t spv_emit_input_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location, int interpolation) {
    assert(interpolation >= 0 && interpolation <= 2);
    uint32_t id = input_vec4(e, vec4_type, 30, location); /* Location */
    if (interpolation) {
        instruction(&e->decorations, SPV_OP_DECORATE, 3);
        put(&e->decorations, id);
        put(&e->decorations, interpolation == 1 ? 14 : 13); /* Flat / NoPerspective */
    }
    return id;
}

uint32_t spv_emit_fragcoord_input(SpvEmitter* e, uint32_t vec4_type) {
    return input_vec4(e, vec4_type, 11, 15); /* BuiltIn FragCoord */
}

uint32_t spv_emit_builtin_input_vec3(SpvEmitter* e, uint32_t vec3_type, uint32_t builtin) {
    return input_vec4(e, vec3_type, 11, builtin);
}

uint32_t spv_emit_builtin_input_u32(SpvEmitter* e, uint32_t u32_type, uint32_t builtin) {
    uint32_t id = input_vec4(e, u32_type, 11, builtin);
    if (builtin == 9 || builtin == 18) spv_emit_flat(e, id); /* Layer and SampleId are read in fragment shaders only */
    return id;
}

/* Integer inputs of a fragment shader need Flat interpolation; Vulkan rejects Flat on vertex inputs. */
void spv_emit_flat(SpvEmitter* e, uint32_t variable) {
    instruction(&e->decorations, SPV_OP_DECORATE, 3);
    put(&e->decorations, variable);
    put(&e->decorations, 14);
}

void spv_emit_capability(SpvEmitter* e, uint32_t capability) {
    for (size_t i = 0; i + 1 < e->capabilities.count; i += 2)
        if (e->capabilities.words[i + 1] == capability) return;
    instruction(&e->capabilities, SPV_OP_CAPABILITY, 2);
    put(&e->capabilities, capability);
}

uint32_t spv_emit_interface_variable(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t u32_type, uint32_t count,
                                     uint32_t builtin, uint32_t location, int patch) {
    uint32_t id = spv_emit_id(e);
    assert(storage == 1 || storage == 3);
    if (count) {
        uint32_t length = spv_emit_constant_u32(e, u32_type, count);
        uint32_t array_type = spv_emit_id(e);
        instruction(&e->types, SPV_OP_TYPE_ARRAY, 4);
        put(&e->types, array_type);
        put(&e->types, type);
        put(&e->types, length);
        type = array_type;
    }
    uint32_t pointer_type = spv_emit_type_pointer(e, storage, type);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, builtin == SPV_NOT_BUILTIN ? 30 : 11); /* Location, BuiltIn */
    put(&e->decorations, builtin == SPV_NOT_BUILTIN ? location : builtin);
    if (patch) {
        instruction(&e->decorations, SPV_OP_DECORATE, 3);
        put(&e->decorations, id);
        put(&e->decorations, 15); /* Patch */
    }
    instruction(&e->types, SPV_OP_VARIABLE, 4);
    put(&e->types, pointer_type);
    put(&e->types, id);
    put(&e->types, storage);
    return id;
}

uint32_t spv_emit_interface_scalar_pointer(SpvEmitter* e, uint32_t storage, uint32_t scalar_type, uint32_t variable,
                                           uint32_t element, uint32_t component) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_ACCESS_CHAIN, element ? 6 : 5);
    put(&e->functions, spv_emit_type_pointer(e, storage, scalar_type));
    put(&e->functions, id);
    put(&e->functions, variable);
    if (element) put(&e->functions, element);
    put(&e->functions, component);
    return id;
}

void spv_emit_execution_mode(SpvEmitter* e, uint32_t function, uint32_t mode, int has_operand, uint32_t operand) {
    instruction(&e->execution_modes, SPV_OP_EXECUTION_MODE, has_operand ? 4 : 3);
    put(&e->execution_modes, function);
    put(&e->execution_modes, mode);
    if (has_operand) put(&e->execution_modes, operand);
}

void spv_emit_emit_vertex(SpvEmitter* e) { instruction(&e->functions, 218, 1); /* OpEmitVertex */ }

void spv_emit_end_primitive(SpvEmitter* e) { instruction(&e->functions, 219, 1); /* OpEndPrimitive */ }

void spv_emit_patch_barrier(SpvEmitter* e, uint32_t u32_type) {
    /* as GLSL's barrier() in a tessellation control shader: Workgroup execution, Invocation memory scope and no
       semantics, which still orders the outputs written before it for the patch's other invocations */
    instruction(&e->functions, 224, 4); /* OpControlBarrier */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 2));
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 4));
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0));
}

/* Ids that are not dynamically uniform: an element of a texture array, and what is loaded or made from it.
   Vulkan wants each decorated NonUniform. */
static void nonuniform(SpvEmitter* e, uint32_t id) {
    for (size_t i = 0; i < e->nonuniform_count; i++)
        if (e->nonuniform[i] == id) return;
    if (e->nonuniform_count == e->nonuniform_capacity) {
        size_t capacity = e->nonuniform_capacity ? e->nonuniform_capacity * 2 : 32;
        uint32_t* grown = realloc(e->nonuniform, capacity * sizeof *grown);
        if (!grown) abort();
        e->nonuniform = grown;
        e->nonuniform_capacity = capacity;
    }
    e->nonuniform[e->nonuniform_count++] = id;
    instruction(&e->decorations, SPV_OP_DECORATE, 3);
    put(&e->decorations, id);
    put(&e->decorations, 5300); /* NonUniform */
}
static int is_nonuniform(const SpvEmitter* e, uint32_t id) {
    for (size_t i = 0; i < e->nonuniform_count; i++)
        if (e->nonuniform[i] == id) return 1;
    return 0;
}

/* the sampled image type of a texture, and its image type, declared once each */
static uint32_t sampled_image_type(SpvEmitter* e, uint32_t scalar_type, uint32_t type, int array, int shadow,
                                   int multisample) {
    assert(type <= 3 && (array == 0 || array == 1) && (shadow == 0 || shadow == 1));
    /* 0 1D, 1 2D, 2 3D, 3 cube; Sampled, of an unknown format */
    return spv_emit_type_sampled_image(e, spv_emit_type_image(e, scalar_type, type, shadow, array, multisample, 1, 0));
}
/* the image type of a sampled image type */
static uint32_t image_of(const SpvEmitter* e, uint32_t sampled_type) {
    for (size_t k = 0; k < e->known_count; k++) {
        const uint32_t* w = e->types.words + e->known[k].at;
        if (e->known[k].id == sampled_type && (w[0] & 0xffff) == SPV_OP_TYPE_SAMPLED_IMAGE) return w[2];
    }
    assert(!"not a sampled image type");
    return 0;
}

uint32_t spv_emit_sampler_image(SpvEmitter* e, uint32_t scalar_type, uint32_t set, uint32_t binding, uint32_t type,
                                int array, int shadow, int multisample, uint32_t* sampled_type) {
    assert(sampled_type);
    *sampled_type = sampled_image_type(e, scalar_type, type, array, shadow, multisample);
    uint32_t pointer = spv_emit_type_pointer(e, 0, *sampled_type); /* UniformConstant */
    uint32_t sampler = spv_emit_id(e);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, sampler);
    put(&e->decorations, 34); /* DescriptorSet */
    put(&e->decorations, set);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, sampler);
    put(&e->decorations, 33); /* Binding */
    put(&e->decorations, binding);
    instruction(&e->types, SPV_OP_VARIABLE, 4);
    put(&e->types, pointer);
    put(&e->types, sampler);
    put(&e->types, 0); /* UniformConstant */
    return sampler;
}

uint32_t spv_emit_storage_image(SpvEmitter* e, uint32_t scalar_type, uint32_t set, uint32_t binding, uint32_t type,
                                int array, uint32_t format, uint32_t* image_type) {
    assert(image_type && (type <= 2 || type == 5) && (array == 0 || array == 1));
    if (!format) spv_emit_capability(e, SPV_CAPABILITY_STORAGE_IMAGE_WRITE_WITHOUT_FORMAT);
    if (type == 5) spv_emit_capability(e, SPV_CAPABILITY_IMAGE_BUFFER); /* a storage texel buffer */
    *image_type = spv_emit_type_image(e, scalar_type, type, 0, array, 0, 2, format);
    uint32_t pointer = spv_emit_type_pointer(e, 0, *image_type);
    uint32_t variable = spv_emit_id(e);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, variable);
    put(&e->decorations, 34); /* DescriptorSet */
    put(&e->decorations, set);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, variable);
    put(&e->decorations, 33); /* Binding */
    put(&e->decorations, binding);
    instruction(&e->types, SPV_OP_VARIABLE, 4);
    put(&e->types, pointer);
    put(&e->types, variable);
    put(&e->types, 0); /* UniformConstant */
    return variable;
}

void spv_emit_image_write(SpvEmitter* e, uint32_t image_type, uint32_t image_variable, uint32_t coordinates,
                          uint32_t texel) {
    uint32_t image = spv_emit_load(e, image_type, image_variable);
    instruction(&e->functions, 99, 4); /* OpImageWrite */
    put(&e->functions, image);
    put(&e->functions, coordinates);
    put(&e->functions, texel);
}

uint32_t spv_emit_image_read(SpvEmitter* e, uint32_t result_type, uint32_t image_type, uint32_t image_variable,
                             uint32_t coordinates) {
    uint32_t image = spv_emit_load(e, image_type, image_variable);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 98, 5); /* OpImageRead */
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    return id;
}

uint32_t spv_emit_image_texel_pointer(SpvEmitter* e, uint32_t scalar_type, uint32_t image_variable,
                                      uint32_t coordinates, uint32_t sample) {
    uint32_t pointer_type = spv_emit_type_pointer(e, 11, scalar_type); /* Image storage class */
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 60, 6); /* OpImageTexelPointer */
    put(&e->functions, pointer_type);
    put(&e->functions, id);
    put(&e->functions, image_variable);
    put(&e->functions, coordinates);
    put(&e->functions, sample);
    return id;
}

/* count textures at one binding, an element picked per invocation with spv_emit_sampler_array_element */
uint32_t spv_emit_sampler_image_array(SpvEmitter* e, uint32_t scalar_type, uint32_t u32_type, uint32_t set,
                                      uint32_t binding, uint32_t type, int array, int shadow, uint32_t count,
                                      uint32_t* sampled_type) {
    *sampled_type = sampled_image_type(e, scalar_type, type, array, shadow, 0);
    spv_emit_capability(e, SPV_CAPABILITY_SHADER_NON_UNIFORM);
    spv_emit_capability(e, SPV_CAPABILITY_SAMPLED_IMAGE_ARRAY_NON_UNIFORM_INDEXING);
    uint32_t length = spv_emit_constant_u32(e, u32_type, count);
    uint32_t array_type = spv_emit_id(e);
    instruction(&e->types, SPV_OP_TYPE_ARRAY, 4);
    put(&e->types, array_type);
    put(&e->types, *sampled_type);
    put(&e->types, length);
    uint32_t pointer = spv_emit_type_pointer(e, 0, array_type); /* UniformConstant */
    uint32_t variable = spv_emit_id(e);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, variable);
    put(&e->decorations, 34); /* DescriptorSet */
    put(&e->decorations, set);
    instruction(&e->decorations, SPV_OP_DECORATE, 4);
    put(&e->decorations, variable);
    put(&e->decorations, 33); /* Binding */
    put(&e->decorations, binding);
    instruction(&e->types, SPV_OP_VARIABLE, 4);
    put(&e->types, pointer);
    put(&e->types, variable);
    put(&e->types, 0); /* UniformConstant */
    return variable;
}

/* the element index of a texture array, a pointer the sampling functions take in place of a texture variable */
uint32_t spv_emit_sampler_array_element(SpvEmitter* e, uint32_t sampled_type, uint32_t array_variable, uint32_t index) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_ACCESS_CHAIN, 5);
    put(&e->functions, spv_emit_type_pointer(e, 0, sampled_type));
    put(&e->functions, id);
    put(&e->functions, array_variable);
    put(&e->functions, index);
    nonuniform(e, index);
    nonuniform(e, id);
    return id;
}

uint32_t spv_emit_sampler2d(SpvEmitter* e, uint32_t f32_type, uint32_t set, uint32_t binding, int shadow,
                            uint32_t* sampled_type) {
    return spv_emit_sampler_image(e, f32_type, set, binding, 1, 0, shadow, 0, sampled_type);
}

void spv_emit_function_void(SpvEmitter* e, uint32_t void_type, uint32_t function, uint32_t function_type) {
    instruction(&e->functions, SPV_OP_FUNCTION, 5);
    put(&e->functions, void_type);
    put(&e->functions, function);
    put(&e->functions, 0); /* FunctionControlMaskNone */
    put(&e->functions, function_type);
}

void spv_emit_label(SpvEmitter* e, uint32_t label) {
    e->block++;
    instruction(&e->functions, SPV_OP_LABEL, 2);
    put(&e->functions, label);
}

uint32_t spv_emit_function_variable(SpvEmitter* e, uint32_t pointer_type) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_VARIABLE, 4);
    put(&e->functions, pointer_type);
    put(&e->functions, id);
    put(&e->functions, 7); /* Function */
    return id;
}

uint32_t spv_emit_function_variable_init(SpvEmitter* e, uint32_t pointer_type, uint32_t initial_value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_VARIABLE, 5);
    put(&e->functions, pointer_type);
    put(&e->functions, id);
    put(&e->functions, 7); /* Function */
    put(&e->functions, initial_value);
    return id;
}

uint32_t spv_emit_load(SpvEmitter* e, uint32_t result_type, uint32_t pointer) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_LOAD, 4);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    if (e->nonuniform_count && is_nonuniform(e, pointer)) nonuniform(e, id);
    return id;
}

uint32_t spv_emit_bitcast(SpvEmitter* e, uint32_t result_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_BITCAST, 4);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

static uint32_t binary(SpvEmitter* e, uint32_t opcode, uint32_t result_type, uint32_t left, uint32_t right) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, opcode, 5);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, left);
    put(&e->functions, right);
    return id;
}

uint32_t spv_emit_fadd(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_F_ADD, f32_type, left, right);
}

uint32_t spv_emit_fmul(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_F_MUL, f32_type, left, right);
}

uint32_t spv_emit_i_equal(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_I_EQUAL, bool_type, left, right);
}

uint32_t spv_emit_i_not_equal(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_I_NOT_EQUAL, bool_type, left, right);
}

uint32_t spv_emit_u_less(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_U_LESS_THAN, bool_type, left, right);
}

uint32_t spv_emit_s_greater(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_S_GREATER_THAN, bool_type, left, right);
}

uint32_t spv_emit_isub_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_I_SUB, u32_type, left, right);
}

uint32_t spv_emit_shift_left_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t shift) {
    return binary(e, SPV_OP_SHIFT_LEFT_LOGICAL, u32_type, value, shift);
}

uint32_t spv_emit_shift_right_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t shift) {
    return binary(e, SPV_OP_SHIFT_RIGHT_LOGICAL, u32_type, value, shift);
}
uint32_t spv_emit_shift_right_s32(SpvEmitter* e, uint32_t i32_type, uint32_t value, uint32_t shift) {
    return binary(e, SPV_OP_SHIFT_RIGHT_ARITHMETIC, i32_type, value, shift);
}
uint32_t spv_emit_sdiv_i32(SpvEmitter* e, uint32_t i32_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_S_DIV, i32_type, left, right);
}

uint32_t spv_emit_bitwise_and_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_BITWISE_AND, u32_type, left, right);
}

uint32_t spv_emit_bitwise_or_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_BITWISE_OR, u32_type, left, right);
}

uint32_t spv_emit_bitwise_xor_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_BITWISE_XOR, u32_type, left, right);
}

uint32_t spv_emit_not_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_NOT, 4);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_bitfield_uextract(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t offset, uint32_t count) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_BIT_FIELD_U_EXTRACT, 6);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, value);
    put(&e->functions, offset);
    put(&e->functions, count);
    return id;
}

uint32_t spv_emit_bit_reverse_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_BIT_REVERSE, 4);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_atomic_u32(SpvEmitter* e, uint32_t u32_type, uint32_t opcode, uint32_t pointer, uint32_t value) {
    assert(opcode == SPV_OP_ATOMIC_EXCHANGE || opcode == SPV_OP_ATOMIC_I_ADD || opcode == 236 || opcode == SPV_OP_ATOMIC_U_MIN ||
           opcode == 238 || opcode == SPV_OP_ATOMIC_U_MAX || opcode == SPV_OP_ATOMIC_AND || opcode == SPV_OP_ATOMIC_OR ||
           opcode == SPV_OP_ATOMIC_XOR);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, opcode, 7);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 1)); /* Device scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0)); /* Relaxed */
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_atomic_shared_u32(SpvEmitter* e, uint32_t u32_type, uint32_t opcode, uint32_t pointer,
                                    uint32_t value) {
    assert(opcode == SPV_OP_ATOMIC_EXCHANGE || opcode == SPV_OP_ATOMIC_I_ADD || opcode == 236 || opcode == SPV_OP_ATOMIC_U_MIN ||
           opcode == 238 || opcode == SPV_OP_ATOMIC_U_MAX || opcode == SPV_OP_ATOMIC_AND || opcode == SPV_OP_ATOMIC_OR ||
           opcode == SPV_OP_ATOMIC_XOR);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, opcode, 7);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 2));     /* Workgroup scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0x108)); /* AcquireRelease | WorkgroupMemory */
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_atomic_load_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 227, 6); /* OpAtomicLoad */
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 1)); /* Device scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0)); /* Relaxed */
    return id;
}

uint32_t spv_emit_atomic_load_shared_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 227, 6); /* OpAtomicLoad */
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 2));     /* Workgroup scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0x102)); /* Acquire | WorkgroupMemory */
    return id;
}

uint32_t spv_emit_atomic_compare_exchange_shared_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer,
                                                     uint32_t desired, uint32_t expected) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 230, 9); /* OpAtomicCompareExchange */
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 2));     /* Workgroup scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0x108)); /* AcquireRelease | WorkgroupMemory */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0x102)); /* Acquire | WorkgroupMemory */
    put(&e->functions, desired);
    put(&e->functions, expected);
    return id;
}

uint32_t spv_emit_atomic_compare_exchange_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer, uint32_t desired,
                                              uint32_t expected) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 230, 9); /* OpAtomicCompareExchange */
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 1)); /* Device scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0)); /* Relaxed success */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0)); /* Relaxed failure */
    put(&e->functions, desired);
    put(&e->functions, expected);
    return id;
}

void spv_emit_control_barrier(SpvEmitter* e, uint32_t u32_type) {
    instruction(&e->functions, 224, 4);                            /* OpControlBarrier */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 2));     /* Workgroup execution scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 2));     /* Workgroup memory scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0x108)); /* AcquireRelease | WorkgroupMemory */
}
void spv_emit_memory_barrier(SpvEmitter* e, uint32_t u32_type, uint32_t scope, uint32_t semantics) {
    instruction(&e->functions, SPV_OP_MEMORY_BARRIER, 3);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, scope));
    put(&e->functions, spv_emit_constant_u32(e, u32_type, semantics));
}

uint32_t spv_emit_bitfield_insert(SpvEmitter* e, uint32_t u32_type, uint32_t base, uint32_t insert, uint32_t offset,
                                  uint32_t count) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_BIT_FIELD_INSERT, 7);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, base);
    put(&e->functions, insert);
    put(&e->functions, offset);
    put(&e->functions, count);
    return id;
}

uint32_t spv_emit_bit_count(SpvEmitter* e, uint32_t u32_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_BIT_COUNT, 4);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_bitfield_sextract(SpvEmitter* e, uint32_t i32_type, uint32_t value, uint32_t offset, uint32_t count) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_BIT_FIELD_S_EXTRACT, 6);
    put(&e->functions, i32_type);
    put(&e->functions, id);
    put(&e->functions, value);
    put(&e->functions, offset);
    put(&e->functions, count);
    return id;
}

uint32_t spv_emit_fcompare(SpvEmitter* e, uint32_t bool_type, uint32_t condition, uint32_t left, uint32_t right) {
    static const uint32_t opcode[16] = {0, 184, 180, 188, 186, 182, 190, 0, 0, 185, 181, 189, 187, 183, 191, 0};
    assert(condition < 16);
    if (condition == 0 || condition == 15) return spv_emit_constant_bool(e, bool_type, condition == 15);
    if (condition == 7 || condition == 8) {
        uint32_t nan = spv_emit_bool_binary(e, bool_type, 1, spv_emit_isnan(e, bool_type, left),
                                            spv_emit_isnan(e, bool_type, right));
        return condition == 7 ? spv_emit_bool_not(e, bool_type, nan) : nan;
    }
    return binary(e, opcode[condition], bool_type, left, right);
}

uint32_t spv_emit_flush_subnormal(SpvEmitter* e, uint32_t f32_type, uint32_t u32_type, uint32_t bool_type, uint32_t glsl,
                                  uint32_t value, int signed_zeros) {
    if (signed_zeros) { /* below the smallest normal, times zero: the zero of its sign; NaN fails the test */
        uint32_t magnitude = spv_emit_glsl_unary(e, f32_type, glsl, SPV_GLSL_F_ABS, value);
        uint32_t tiny = spv_emit_fcompare(e, bool_type, 1, magnitude, spv_emit_constant_u32(e, f32_type, 0x00800000u));
        uint32_t zero = spv_emit_fmul(e, f32_type, value, spv_emit_constant_u32(e, f32_type, 0));
        return spv_emit_select(e, f32_type, tiny, zero, value);
    }
    uint32_t bits = spv_emit_bitcast(e, u32_type, value);
    uint32_t exponent = spv_emit_bitwise_and_u32(e, u32_type, bits, spv_emit_constant_u32(e, u32_type, 0x7f800000u));
    uint32_t sign = spv_emit_bitwise_and_u32(e, u32_type, bits, spv_emit_constant_u32(e, u32_type, 0x80000000u));
    uint32_t flushed =
        spv_emit_select(e, u32_type, spv_emit_i_equal(e, bool_type, exponent, spv_emit_constant_u32(e, u32_type, 0)),
                        sign, bits);
    return spv_emit_bitcast(e, f32_type, flushed);
}

uint32_t spv_emit_bool_not(SpvEmitter* e, uint32_t bool_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_LOGICAL_NOT, 4);
    put(&e->functions, bool_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_bool_binary(SpvEmitter* e, uint32_t bool_type, uint32_t op, uint32_t left, uint32_t right) {
    static const uint32_t opcode[3] = {SPV_OP_LOGICAL_AND, SPV_OP_LOGICAL_OR, SPV_OP_LOGICAL_NOT_EQUAL};
    assert(op < 3);
    return binary(e, opcode[op], bool_type, left, right);
}

uint32_t spv_emit_fdiv(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_F_DIV, f32_type, left, right);
}

uint32_t spv_emit_fnegate(SpvEmitter* e, uint32_t f32_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_F_NEGATE, 4);
    put(&e->functions, f32_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_derivative(SpvEmitter* e, uint32_t f32_type, uint32_t value, int y_axis) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, y_axis ? SPV_OP_DPDY : SPV_OP_DPDX, 4);
    put(&e->functions, f32_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_glsl_unary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_EXT_INST, 6);
    put(&e->functions, f32_type);
    put(&e->functions, id);
    put(&e->functions, import);
    put(&e->functions, (uint32_t)op);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_glsl_binary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t left,
                              uint32_t right) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_EXT_INST, 7);
    put(&e->functions, f32_type);
    put(&e->functions, id);
    put(&e->functions, import);
    put(&e->functions, (uint32_t)op);
    put(&e->functions, left);
    put(&e->functions, right);
    return id;
}

uint32_t spv_emit_isnan(SpvEmitter* e, uint32_t bool_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IS_NAN, 4);
    put(&e->functions, bool_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_select(SpvEmitter* e, uint32_t result_type, uint32_t condition, uint32_t yes, uint32_t no) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_SELECT, 6);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, condition);
    put(&e->functions, yes);
    put(&e->functions, no);
    return id;
}

uint32_t spv_emit_glsl_ternary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t a,
                               uint32_t b, uint32_t c) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_EXT_INST, 8);
    put(&e->functions, f32_type);
    put(&e->functions, id);
    put(&e->functions, import);
    put(&e->functions, (uint32_t)op);
    put(&e->functions, a);
    put(&e->functions, b);
    put(&e->functions, c);
    return id;
}

uint32_t spv_emit_vec4_extract(SpvEmitter* e, uint32_t component_type, uint32_t vector, uint32_t component) {
    assert(component < 4);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_COMPOSITE_EXTRACT, 5);
    put(&e->functions, component_type);
    put(&e->functions, id);
    put(&e->functions, vector);
    put(&e->functions, component);
    return id;
}

uint32_t spv_emit_subgroup_ballot(SpvEmitter* e, uint32_t vec4_type, uint32_t u32_type, uint32_t predicate) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_GROUP_NON_UNIFORM_BALLOT, 5);
    put(&e->functions, vec4_type);
    put(&e->functions, id);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 3)); /* Subgroup scope */
    put(&e->functions, predicate);
    return id;
}

/* the smallest value among the subgroup's active invocations (OpGroupNonUniformUMin, Reduce) */
uint32_t spv_emit_subgroup_umin_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 354, 6); /* OpGroupNonUniformUMin */
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 3)); /* Subgroup scope */
    put(&e->functions, 0);                                     /* Reduce */
    put(&e->functions, value);
    return id;
}

void spv_emit_subgroup_arithmetic_capability(SpvEmitter* e) {
    spv_emit_capability(e, SPV_CAPABILITY_GROUP_NON_UNIFORM_ARITHMETIC);
}

uint32_t spv_emit_subgroup_shuffle_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t lane) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_GROUP_NON_UNIFORM_SHUFFLE, 6);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 3)); /* Subgroup scope */
    put(&e->functions, value);
    put(&e->functions, lane);
    return id;
}

uint32_t spv_emit_vec4_insert(SpvEmitter* e, uint32_t vec4_type, uint32_t vector, uint32_t component_value,
                              uint32_t component) {
    assert(component < 4);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_COMPOSITE_INSERT, 6);
    put(&e->functions, vec4_type);
    put(&e->functions, id);
    put(&e->functions, component_value);
    put(&e->functions, vector);
    put(&e->functions, component);
    return id;
}

uint32_t spv_emit_vec4_construct(SpvEmitter* e, uint32_t vec4_type, const uint32_t components[4]) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_COMPOSITE_CONSTRUCT, 7);
    put(&e->functions, vec4_type);
    put(&e->functions, id);
    for (size_t i = 0; i < 4; i++) put(&e->functions, components[i]);
    return id;
}

uint32_t spv_emit_vec2_construct(SpvEmitter* e, uint32_t vec2_type, uint32_t x, uint32_t y) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_COMPOSITE_CONSTRUCT, 5);
    put(&e->functions, vec2_type);
    put(&e->functions, id);
    put(&e->functions, x);
    put(&e->functions, y);
    return id;
}

uint32_t spv_emit_vec3_construct(SpvEmitter* e, uint32_t vec3_type, uint32_t x, uint32_t y, uint32_t z) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_COMPOSITE_CONSTRUCT, 6);
    put(&e->functions, vec3_type);
    put(&e->functions, id);
    put(&e->functions, x);
    put(&e->functions, y);
    put(&e->functions, z);
    return id;
}

uint32_t spv_emit_sample2d(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                           uint32_t coordinates, uint32_t depth_reference) {
    uint32_t image = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, depth_reference ? SPV_OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD : SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD,
                depth_reference ? 6 : 5);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    if (depth_reference) put(&e->functions, depth_reference);
    return id;
}

uint32_t spv_emit_sample2d_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                               uint32_t coordinates, uint32_t depth_reference, uint32_t lod) {
    uint32_t image = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, depth_reference ? SPV_OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD : SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD,
                depth_reference ? 8 : 7);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    if (depth_reference) put(&e->functions, depth_reference);
    put(&e->functions, 2); /* ImageOperands Lod */
    put(&e->functions, lod);
    return id;
}

uint32_t spv_emit_image_query_size_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                       uint32_t lod) {
    uint32_t image_type = image_of(e, sampled_type);
    uint32_t sampled = spv_emit_load(e, sampled_type, sampler);
    uint32_t image = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IMAGE, 4);
    put(&e->functions, image_type);
    put(&e->functions, image);
    put(&e->functions, sampled);
    if (e->nonuniform_count && is_nonuniform(e, sampled)) nonuniform(e, image);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IMAGE_QUERY_SIZE_LOD, 5);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, lod);
    return id;
}

uint32_t spv_emit_image_query_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                  uint32_t coordinates) {
    uint32_t sampled = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IMAGE_QUERY_LOD, 5);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, sampled);
    put(&e->functions, coordinates);
    return id;
}

uint32_t spv_emit_image_query_levels(SpvEmitter* e, uint32_t u32_type, uint32_t sampled_type, uint32_t sampler) {
    uint32_t image_type = image_of(e, sampled_type);
    uint32_t sampled = spv_emit_load(e, sampled_type, sampler);
    uint32_t image = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IMAGE, 4);
    put(&e->functions, image_type);
    put(&e->functions, image);
    put(&e->functions, sampled);
    if (e->nonuniform_count && is_nonuniform(e, sampled)) nonuniform(e, image);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 106, 4); /* OpImageQueryLevels */
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, image);
    return id;
}

uint32_t spv_emit_image_fetch(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                              uint32_t coordinates, uint32_t lod) {
    uint32_t image_type = image_of(e, sampled_type);
    uint32_t sampled = spv_emit_load(e, sampled_type, sampler);
    uint32_t image = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IMAGE, 4);
    put(&e->functions, image_type);
    put(&e->functions, image);
    put(&e->functions, sampled);
    if (e->nonuniform_count && is_nonuniform(e, sampled)) nonuniform(e, image);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IMAGE_FETCH, 7);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    put(&e->functions, 2); /* ImageOperands Lod */
    put(&e->functions, lod);
    return id;
}

uint32_t spv_emit_image_fetch_sample(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                     uint32_t coordinates, uint32_t sample) {
    uint32_t image_type = image_of(e, sampled_type);
    uint32_t sampled = spv_emit_load(e, sampled_type, sampler);
    uint32_t image = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IMAGE, 4);
    put(&e->functions, image_type);
    put(&e->functions, image);
    put(&e->functions, sampled);
    if (e->nonuniform_count && is_nonuniform(e, sampled)) nonuniform(e, image);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IMAGE_FETCH, 7);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    put(&e->functions, 64); /* ImageOperands Sample */
    put(&e->functions, sample);
    return id;
}

uint32_t spv_emit_image_gather_ex(SpvEmitter* e, uint32_t vec4_type, uint32_t sampled_type, uint32_t sampler,
                                  uint32_t coordinates, uint32_t component, uint32_t dref, uint32_t offset) {
    uint32_t image = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, dref ? SPV_OP_IMAGE_DREF_GATHER : SPV_OP_IMAGE_GATHER, offset ? 8 : 6);
    put(&e->functions, vec4_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    put(&e->functions, dref ? dref : component);
    if (offset) {
        put(&e->functions, 16); /* ImageOperands Offset */
        put(&e->functions, offset);
    }
    return id;
}

uint32_t spv_emit_sample_image(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                               uint32_t coordinates, uint32_t depth_reference, uint32_t mode, uint32_t level,
                               uint32_t offset, int dynamic_offset) {
    uint32_t image = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    uint32_t operands = mode == 1 ? 2 : mode == 2 ? 1 : 0;
    uint32_t opcode;
    if (offset) operands |= dynamic_offset ? 16 : 8;
    opcode = mode == 1         ? depth_reference ? SPV_OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD : SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD
             : depth_reference ? SPV_OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD
                               : SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD;
    instruction(&e->functions, opcode, 5 + !!depth_reference + !!operands + !!level + !!offset);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    if (depth_reference) put(&e->functions, depth_reference);
    if (operands) put(&e->functions, operands);
    if (level) put(&e->functions, level);
    if (offset) put(&e->functions, offset);
    return id;
}

uint32_t spv_emit_sample_grad(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                              uint32_t coordinates, uint32_t dx, uint32_t dy, uint32_t offset) {
    uint32_t image = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD, 8 + !!offset);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    put(&e->functions, 4 | (offset ? 8 : 0)); /* ImageOperands Grad, ConstOffset */
    put(&e->functions, dx);
    put(&e->functions, dy);
    if (offset) put(&e->functions, offset);
    return id;
}

void spv_emit_no_contraction(SpvEmitter* e, uint32_t result) {
    instruction(&e->decorations, SPV_OP_DECORATE, 3);
    put(&e->decorations, result);
    put(&e->decorations, 42); /* NoContraction */
}

uint32_t spv_emit_storage_access_u32(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member_index,
                                     uint32_t element_index) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_ACCESS_CHAIN, 6);
    put(&e->functions, pointer_type);
    put(&e->functions, id);
    put(&e->functions, buffer);
    put(&e->functions, member_index);
    put(&e->functions, element_index);
    return id;
}

uint32_t spv_emit_iadd_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_I_ADD, 5);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, left);
    put(&e->functions, right);
    return id;
}

uint32_t spv_emit_imul_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right) {
    return binary(e, SPV_OP_I_MUL, u32_type, left, right);
}

uint32_t spv_emit_umul_high_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right) {
    uint32_t pair_type = spv_emit_id(e);
    instruction(&e->types, SPV_OP_TYPE_STRUCT, 4);
    put(&e->types, pair_type);
    put(&e->types, u32_type);
    put(&e->types, u32_type);
    uint32_t pair = binary(e, SPV_OP_U_MUL_EXTENDED, pair_type, left, right);
    uint32_t high = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_COMPOSITE_EXTRACT, 5);
    put(&e->functions, u32_type);
    put(&e->functions, high);
    put(&e->functions, pair);
    put(&e->functions, 1);
    return high;
}

void spv_emit_store(SpvEmitter* e, uint32_t pointer, uint32_t value) {
    instruction(&e->functions, SPV_OP_STORE, 3);
    put(&e->functions, pointer);
    put(&e->functions, value);
}

void spv_emit_store_u32(SpvEmitter* e, uint32_t pointer, uint32_t value) { spv_emit_store(e, pointer, value); }

void spv_emit_return(SpvEmitter* e) { instruction(&e->functions, SPV_OP_RETURN, 1); }
void spv_emit_kill(SpvEmitter* e) { instruction(&e->functions, SPV_OP_KILL, 1); }
void spv_emit_demote(SpvEmitter* e) { instruction(&e->functions, SPV_OP_DEMOTE_TO_HELPER_INVOCATION, 1); }
uint32_t spv_emit_is_helper(SpvEmitter* e, uint32_t bool_type) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IS_HELPER_INVOCATION, 3);
    put(&e->functions, bool_type);
    put(&e->functions, id);
    return id;
}
uint32_t spv_emit_read_clock(SpvEmitter* e, uint32_t vec2_u32_type, uint32_t scope) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_READ_CLOCK_KHR, 4);
    put(&e->functions, vec2_u32_type);
    put(&e->functions, id);
    put(&e->functions, scope);
    return id;
}
static void block_ends(SpvEmitter* e) {
    if (e->block_end) e->block_end(e->block_end_user);
}

void spv_emit_selection_merge(SpvEmitter* e, uint32_t merge_label) {
    block_ends(e);
    instruction(&e->functions, SPV_OP_SELECTION_MERGE, 3);
    put(&e->functions, merge_label);
    put(&e->functions, 0); /* SelectionControlMaskNone */
}

void spv_emit_branch_conditional(SpvEmitter* e, uint32_t condition, uint32_t yes_label, uint32_t no_label) {
    block_ends(e);
    instruction(&e->functions, SPV_OP_BRANCH_CONDITIONAL, 4);
    put(&e->functions, condition);
    put(&e->functions, yes_label);
    put(&e->functions, no_label);
}

void spv_emit_branch(SpvEmitter* e, uint32_t label) {
    block_ends(e);
    instruction(&e->functions, SPV_OP_BRANCH, 2);
    put(&e->functions, label);
}

void spv_emit_loop_merge(SpvEmitter* e, uint32_t merge_label, uint32_t continue_label) {
    block_ends(e);
    instruction(&e->functions, SPV_OP_LOOP_MERGE, 4);
    put(&e->functions, merge_label);
    put(&e->functions, continue_label);
    put(&e->functions, 0); /* LoopControlMaskNone */
}

void spv_emit_switch_u32(SpvEmitter* e, uint32_t selector, uint32_t default_label, const uint32_t* literals,
                         const uint32_t* labels, size_t count) {
    assert(count <= (UINT16_MAX - 3) / 2);
    block_ends(e);
    instruction(&e->functions, SPV_OP_SWITCH, (uint32_t)(3 + count * 2));
    put(&e->functions, selector);
    put(&e->functions, default_label);
    for (size_t i = 0; i < count; i++) {
        put(&e->functions, literals[i]);
        put(&e->functions, labels[i]);
    }
}
void spv_emit_function_end(SpvEmitter* e) {
    if (e->locals.count) { /* spv_emit_local's variables, into the first block */
        assert(e->body_start && e->body_start <= e->functions.count);
        reserve(&e->functions, e->locals.count);
        uint32_t* at = e->functions.words + e->body_start;
        memmove(at + e->locals.count, at, (e->functions.count - e->body_start) * sizeof *at);
        memcpy(at, e->locals.words, e->locals.count * sizeof *at);
        e->functions.count += e->locals.count;
        e->locals.count = 0;
    }
    e->body_start = 0;
    instruction(&e->functions, SPV_OP_FUNCTION_END, 1);
}

/* ---- the generic layer ---- */

void spv_emit_extension(SpvEmitter* e, const char* name) {
    size_t n = strlen(name) + 1, words = string_words(name);
    for (size_t i = 0; i < e->extensions.count; i += e->extensions.words[i] >> 16) {
        size_t j = 0;
        if (e->extensions.words[i] >> 16 != 1 + words) continue;
        while (j < words && e->extensions.words[i + 1 + j] == string_word(name, n, j)) j++;
        if (j == words) return;
    }
    assert(words <= UINT16_MAX - 1);
    instruction(&e->extensions, SPV_OP_EXTENSION, (uint32_t)(1 + words));
    put_str(&e->extensions, name);
}

uint32_t spv_emit_type_int(SpvEmitter* e, uint32_t width, int is_signed) {
    uint32_t operands[2] = {width, is_signed ? 1u : 0u};
    return known(e, SPV_OP_TYPE_INT, 1, operands, 2);
}
uint32_t spv_emit_type_float(SpvEmitter* e, uint32_t width) { return known(e, SPV_OP_TYPE_FLOAT, 1, &width, 1); }
uint32_t spv_emit_type_vector(SpvEmitter* e, uint32_t component, uint32_t count) {
    uint32_t operands[2] = {component, count};
    return known(e, SPV_OP_TYPE_VECTOR, 1, operands, 2);
}
uint32_t spv_emit_type_array(SpvEmitter* e, uint32_t element, uint32_t length) {
    assert(length);
    uint32_t operands[2] = {element, spv_emit_const_u32(e, length)};
    return known(e, SPV_OP_TYPE_ARRAY, 1, operands, 2);
}
uint32_t spv_emit_type_pointer(SpvEmitter* e, uint32_t storage, uint32_t type) {
    uint32_t operands[2] = {storage, type};
    return known(e, SPV_OP_TYPE_POINTER, 1, operands, 2);
}
uint32_t spv_emit_type_function(SpvEmitter* e, uint32_t result, const uint32_t* params, size_t count) {
    uint32_t operands[17];
    assert(count <= 16);
    operands[0] = result;
    for (size_t i = 0; i < count; i++) operands[i + 1] = params[i];
    return known(e, SPV_OP_TYPE_FUNCTION, 1, operands, count + 1);
}
uint32_t spv_emit_type_image(SpvEmitter* e, uint32_t sampled_type, uint32_t dim, int depth, int arrayed,
                             int multisampled, uint32_t sampled, uint32_t format) {
    uint32_t operands[7] = {sampled_type, dim, (uint32_t)depth, (uint32_t)arrayed, (uint32_t)multisampled, sampled,
                            format};
    return known(e, SPV_OP_TYPE_IMAGE, 1, operands, 7);
}
uint32_t spv_emit_type_sampled_image(SpvEmitter* e, uint32_t image) {
    return known(e, SPV_OP_TYPE_SAMPLED_IMAGE, 1, &image, 1);
}
uint32_t spv_emit_type_struct(SpvEmitter* e, const uint32_t* members, const uint32_t* offsets, size_t count) {
    uint32_t id = spv_emit_id(e);
    assert(count <= UINT16_MAX - 2);
    instruction(&e->types, SPV_OP_TYPE_STRUCT, (uint32_t)(count + 2));
    put(&e->types, id);
    for (size_t i = 0; i < count; i++) put(&e->types, members[i]);
    for (size_t i = 0; i < count; i++)
        spv_emit_member_decorate(e, id, (uint32_t)i, SPV_DECORATION_OFFSET, &offsets[i], 1);
    return id;
}

uint32_t spv_emit_const_u32(SpvEmitter* e, uint32_t value) {
    uint32_t operands[2] = {spv_emit_type_int(e, 32, 0), value};
    return known(e, SPV_OP_CONSTANT, 2, operands, 2);
}
uint32_t spv_emit_const_i32(SpvEmitter* e, int32_t value) {
    uint32_t operands[2] = {spv_emit_type_int(e, 32, 1), (uint32_t)value};
    return known(e, SPV_OP_CONSTANT, 2, operands, 2);
}
uint32_t spv_emit_const_f32(SpvEmitter* e, float value) {
    uint32_t operands[2] = {spv_emit_type_float(e, 32), 0};
    memcpy(&operands[1], &value, sizeof value);
    return known(e, SPV_OP_CONSTANT, 2, operands, 2);
}
uint32_t spv_emit_const_bool(SpvEmitter* e, int value) {
    uint32_t type = spv_emit_type_bool(e);
    return known(e, value ? SPV_OP_CONSTANT_TRUE : SPV_OP_CONSTANT_FALSE, 2, &type, 1);
}
uint32_t spv_emit_const_composite(SpvEmitter* e, uint32_t type, const uint32_t* parts, size_t count) {
    uint32_t operands[17];
    assert(count <= 16);
    operands[0] = type;
    for (size_t i = 0; i < count; i++) operands[i + 1] = parts[i];
    return known(e, SPV_OP_CONSTANT_COMPOSITE, 2, operands, count + 1);
}

void spv_emit_decorate(SpvEmitter* e, uint32_t id, uint32_t decoration, const uint32_t* operands, size_t count) {
    assert(count <= UINT16_MAX - 3);
    instruction(&e->decorations, SPV_OP_DECORATE, (uint32_t)(count + 3));
    put(&e->decorations, id);
    put(&e->decorations, decoration);
    for (size_t i = 0; i < count; i++) put(&e->decorations, operands[i]);
}
void spv_emit_member_decorate(SpvEmitter* e, uint32_t type, uint32_t member, uint32_t decoration,
                              const uint32_t* operands, size_t count) {
    assert(count <= UINT16_MAX - 4);
    instruction(&e->decorations, SPV_OP_MEMBER_DECORATE, (uint32_t)(count + 4));
    put(&e->decorations, type);
    put(&e->decorations, member);
    put(&e->decorations, decoration);
    for (size_t i = 0; i < count; i++) put(&e->decorations, operands[i]);
}
static uint32_t variable(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t initializer) {
    uint32_t pointer = spv_emit_type_pointer(e, storage, type), id = spv_emit_id(e);
    assert(storage != SPV_STORAGE_FUNCTION);
    instruction(&e->types, SPV_OP_VARIABLE, initializer ? 5 : 4);
    put(&e->types, pointer);
    put(&e->types, id);
    put(&e->types, storage);
    if (initializer) put(&e->types, initializer);
    if (storage == SPV_STORAGE_INPUT || storage == SPV_STORAGE_OUTPUT) {
        if (e->interface_count == e->interface_capacity) {
            size_t capacity = e->interface_capacity ? e->interface_capacity * 2 : 16;
            uint32_t* grown = realloc(e->interface, capacity * sizeof *grown);
            if (!grown) abort();
            e->interface = grown;
            e->interface_capacity = capacity;
        }
        e->interface[e->interface_count++] = id;
    }
    return id;
}
uint32_t spv_emit_variable(SpvEmitter* e, uint32_t storage, uint32_t type) { return variable(e, storage, type, 0); }
uint32_t spv_emit_variable_zeroed(SpvEmitter* e, uint32_t storage, uint32_t type) {
    uint32_t zero = spv_emit_id(e);
    instruction(&e->types, SPV_OP_CONSTANT_NULL, 3);
    put(&e->types, type);
    put(&e->types, zero);
    return variable(e, storage, type, zero);
}
uint32_t spv_emit_resource(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t set, uint32_t binding) {
    uint32_t id = spv_emit_variable(e, storage, type);
    spv_emit_decorate(e, id, SPV_DECORATION_DESCRIPTOR_SET, &set, 1);
    spv_emit_decorate(e, id, SPV_DECORATION_BINDING, &binding, 1);
    return id;
}
uint32_t spv_emit_location(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t location) {
    uint32_t id = spv_emit_variable(e, storage, type);
    spv_emit_decorate(e, id, SPV_DECORATION_LOCATION, &location, 1);
    return id;
}
uint32_t spv_emit_builtin(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t builtin) {
    uint32_t id = spv_emit_variable(e, storage, type);
    spv_emit_decorate(e, id, SPV_DECORATION_BUILT_IN, &builtin, 1);
    return id;
}

uint32_t spv_emit_function(SpvEmitter* e, uint32_t id, uint32_t result, const uint32_t* param_types, size_t count,
                           uint32_t* params) {
    uint32_t type = spv_emit_type_function(e, result, param_types, count);
    assert(!e->body_start && !e->locals.count);
    if (!id) id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_FUNCTION, 5);
    put(&e->functions, result);
    put(&e->functions, id);
    put(&e->functions, 0); /* FunctionControlMaskNone */
    put(&e->functions, type);
    for (size_t i = 0; i < count; i++) {
        params[i] = spv_emit_id(e);
        instruction(&e->functions, SPV_OP_FUNCTION_PARAMETER, 3);
        put(&e->functions, param_types[i]);
        put(&e->functions, params[i]);
    }
    spv_emit_label(e, spv_emit_id(e));
    e->body_start = e->functions.count;
    return id;
}
uint32_t spv_emit_local(SpvEmitter* e, uint32_t type) {
    uint32_t pointer = spv_emit_type_pointer(e, SPV_STORAGE_FUNCTION, type), id = spv_emit_id(e);
    assert(e->body_start);
    instruction(&e->locals, SPV_OP_VARIABLE, 4);
    put(&e->locals, pointer);
    put(&e->locals, id);
    put(&e->locals, SPV_STORAGE_FUNCTION);
    return id;
}
uint32_t spv_emit_op(SpvEmitter* e, uint32_t opcode, uint32_t type, const uint32_t* operands, size_t count) {
    if (!type) {
        spv_emit_void_op(e, opcode, operands, count);
        return 0;
    }
    uint32_t id = spv_emit_id(e);
    assert(count <= UINT16_MAX - 3);
    instruction(&e->functions, opcode, (uint32_t)(count + 3));
    put(&e->functions, type);
    put(&e->functions, id);
    for (size_t i = 0; i < count; i++) put(&e->functions, operands[i]);
    return id;
}
void spv_emit_void_op(SpvEmitter* e, uint32_t opcode, const uint32_t* operands, size_t count) {
    assert(count <= UINT16_MAX - 1);
    if (opcode == SPV_OP_LOOP_MERGE || opcode == SPV_OP_SELECTION_MERGE || opcode == SPV_OP_BRANCH ||
        opcode == SPV_OP_BRANCH_CONDITIONAL || opcode == SPV_OP_SWITCH)
        block_ends(e);
    instruction(&e->functions, opcode, (uint32_t)(count + 1));
    for (size_t i = 0; i < count; i++) put(&e->functions, operands[i]);
}
uint32_t spv_emit_ext(SpvEmitter* e, uint32_t type, uint32_t instruction_number, const uint32_t* operands,
                      size_t count) {
    uint32_t import = spv_emit_glsl450_import(e), id = spv_emit_id(e);
    assert(count <= UINT16_MAX - 5);
    instruction(&e->functions, SPV_OP_EXT_INST, (uint32_t)(count + 5));
    put(&e->functions, type);
    put(&e->functions, id);
    put(&e->functions, import);
    put(&e->functions, instruction_number);
    for (size_t i = 0; i < count; i++) put(&e->functions, operands[i]);
    return id;
}

uint32_t* spv_emit_finish(const SpvEmitter* e, size_t* word_count) {
    const SpvWords* sections[] = {&e->capabilities, &e->extensions,  &e->imports, &e->entry_points, &e->execution_modes,
                                  &e->debug,        &e->decorations, &e->types,   &e->functions};
    size_t n = 8; /* header, memory model */
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) {
        assert(sections[i]->count <= SIZE_MAX - n);
        n += sections[i]->count;
    }
    assert(n <= SIZE_MAX / sizeof(uint32_t));
    uint32_t* out = malloc(n * sizeof(uint32_t));
    if (!out) abort();
    size_t p = 0;
    out[p++] = 0x07230203; /* magic */
    out[p++] = e->version;
    out[p++] = 0;          /* generator */
    out[p++] = e->next_id;
    out[p++] = 0; /* schema */
    if (e->capabilities.count) memcpy(out + p, e->capabilities.words, e->capabilities.count * sizeof(uint32_t));
    p += e->capabilities.count;
    if (e->extensions.count) memcpy(out + p, e->extensions.words, e->extensions.count * sizeof(uint32_t));
    p += e->extensions.count;
    if (e->imports.count) memcpy(out + p, e->imports.words, e->imports.count * sizeof(uint32_t));
    p += e->imports.count;
    out[p++] = (3u << 16) | SPV_OP_MEMORY_MODEL;
    out[p++] = 0; /* Logical */
    out[p++] = 1; /* GLSL450 */
    for (size_t i = 3; i < sizeof sections / sizeof sections[0]; i++) {
        if (sections[i]->count) memcpy(out + p, sections[i]->words, sections[i]->count * sizeof(uint32_t));
        p += sections[i]->count;
    }
    assert(p == n);
    *word_count = n;
    return out;
}
