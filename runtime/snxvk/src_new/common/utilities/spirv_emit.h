/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A C SPIR-V writer, for the shaders the runtimes make while a game runs: the Maxwell programs NX translates
   (nx/gpu/shader/spirv_translate.c and spirv_compute.c) and the 3DS's PICA vertex programs and fragment pipeline
   (3ds/gpu/vk/vsgen.c and fsgen.c). Instructions append words directly to their logical sections; spv_emit_finish
   orders the sections into a Vulkan shader module of the Logical GLSL450 memory model, SPIR-V 1.6 unless version says
   otherwise.

   Two layers share the sections. The instruction functions (spv_emit_fadd, spv_emit_sample_image...) each write one
   instruction or declaration of their kind, with the types and ids the caller passes. The generic layer writes any
   instruction from its opcode and operands (spv_emit_op, and the SPV, SPV_VOID and SPV_EXT shorthands), and declares
   any variable, decoration or type. Scalar, vector, pointer, function and image types are made once each, by either
   layer, as SPIR-V requires: asking again gives the same id. So are the generic layer's arrays and its constants
   (spv_emit_const_*). The specialized layer also reuses 32-bit scalar constants by type and bits. A function written with spv_emit_function
   gathers the variables spv_emit_local declares into its first block, wherever they are declared. Running out of
   memory aborts. */
#ifndef SNX_SPIRV_EMIT_H
#define SNX_SPIRV_EMIT_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t* words;
    size_t count, capacity;
} SpvWords;

/* a type or constant made once: its instruction's place in types, and its id */
typedef struct {
    uint64_t hash;
    size_t at;
    uint32_t id;
} SpvKnown;

/* a 32-bit OpConstant: one per type and value */
typedef struct {
    uint32_t type, value, id;
} SpvConstant;

typedef struct {
    SpvWords capabilities, extensions, imports, entry_points, execution_modes, debug, decorations, types, functions;
    SpvWords locals;   /* the spv_emit_local variables of the function being written */
    size_t body_start; /* where they go in functions: after its first label (0 outside spv_emit_function) */
    uint32_t next_id;
    uint32_t version; /* the module's SPIR-V version word, 0x00010600 (1.6) from spv_emit_init */
    uint32_t glsl;    /* the GLSL.std.450 import, 0 until it is made */
    SpvKnown* known;
    size_t known_count, known_capacity;
    uint32_t* interface; /* the Input and Output variables spv_emit_variable made, for the entry point */
    size_t interface_count, interface_capacity;
    uint32_t* nonuniform; /* ids decorated NonUniform: texture array elements and what is made from them */
    size_t nonuniform_count, nonuniform_capacity;
    uint32_t block; /* the OpLabels emitted so far: values an emitter's user keeps are good within one block */
    /* the 32-bit constants made, open addressing on a power of two: by type and value, and by id */
    SpvConstant *constants, *constant_ids;
    size_t constant_count, constant_capacity;
    /* called before each merge instruction and each branch or switch (not a return, kill or OpUnreachable) with
       block_end_user, so that a user keeping values of its own Function variables in a block stores them there */
    void (*block_end)(void* user);
    void* block_end_user;
} SpvEmitter;

typedef enum {
    SPV_STAGE_VERTEX = 0,
    SPV_STAGE_TESS_CONTROL = 1,
    SPV_STAGE_TESS_EVALUATION = 2,
    SPV_STAGE_GEOMETRY = 3,
    SPV_STAGE_FRAGMENT = 4,
    SPV_STAGE_COMPUTE = 5
} SpvShaderStage;

/* the opcodes, storage classes, decorations, built-ins, execution modes and capabilities, by the SPIR-V
   specification's numbers */
enum {
    SPV_OP_NAME = 5,
    SPV_OP_EXTENSION = 10,
    SPV_OP_EXT_INST_IMPORT = 11,
    SPV_OP_EXT_INST = 12,
    SPV_OP_MEMORY_MODEL = 14,
    SPV_OP_ENTRY_POINT = 15,
    SPV_OP_EXECUTION_MODE = 16,
    SPV_OP_CAPABILITY = 17,
    SPV_OP_TYPE_VOID = 19,
    SPV_OP_TYPE_BOOL = 20,
    SPV_OP_TYPE_INT = 21,
    SPV_OP_TYPE_FLOAT = 22,
    SPV_OP_TYPE_VECTOR = 23,
    SPV_OP_TYPE_IMAGE = 25,
    SPV_OP_TYPE_SAMPLED_IMAGE = 27,
    SPV_OP_TYPE_ARRAY = 28,
    SPV_OP_TYPE_RUNTIME_ARRAY = 29,
    SPV_OP_TYPE_STRUCT = 30,
    SPV_OP_TYPE_POINTER = 32,
    SPV_OP_TYPE_FUNCTION = 33,
    SPV_OP_CONSTANT_TRUE = 41,
    SPV_OP_CONSTANT_FALSE = 42,
    SPV_OP_CONSTANT = 43,
    SPV_OP_CONSTANT_COMPOSITE = 44,
    SPV_OP_CONSTANT_NULL = 46,
    SPV_OP_FUNCTION = 54,
    SPV_OP_FUNCTION_PARAMETER = 55,
    SPV_OP_FUNCTION_END = 56,
    SPV_OP_FUNCTION_CALL = 57,
    SPV_OP_VARIABLE = 59,
    SPV_OP_IMAGE_TEXEL_POINTER = 60,
    SPV_OP_LOAD = 61,
    SPV_OP_STORE = 62,
    SPV_OP_ACCESS_CHAIN = 65,
    SPV_OP_DECORATE = 71,
    SPV_OP_MEMBER_DECORATE = 72,
    SPV_OP_VECTOR_SHUFFLE = 79,
    SPV_OP_COMPOSITE_CONSTRUCT = 80,
    SPV_OP_COMPOSITE_EXTRACT = 81,
    SPV_OP_COMPOSITE_INSERT = 82,
    SPV_OP_SAMPLED_IMAGE = 86,
    SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87,
    SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD = 88,
    SPV_OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD = 89,
    SPV_OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD = 90,
    SPV_OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD = 91,
    SPV_OP_IMAGE_FETCH = 95,
    SPV_OP_IMAGE_GATHER = 96,
    SPV_OP_IMAGE_DREF_GATHER = 97,
    SPV_OP_IMAGE_READ = 98,
    SPV_OP_IMAGE_WRITE = 99,
    SPV_OP_IMAGE = 100,
    SPV_OP_IMAGE_QUERY_SIZE_LOD = 103,
    SPV_OP_IMAGE_QUERY_SIZE = 104,
    SPV_OP_IMAGE_QUERY_LOD = 105,
    SPV_OP_IMAGE_QUERY_LEVELS = 106,
    SPV_OP_CONVERT_F_TO_U = 109,
    SPV_OP_CONVERT_F_TO_S = 110,
    SPV_OP_CONVERT_S_TO_F = 111,
    SPV_OP_CONVERT_U_TO_F = 112,
    SPV_OP_U_CONVERT = 113,
    SPV_OP_S_CONVERT = 114,
    SPV_OP_F_CONVERT = 115,
    SPV_OP_BITCAST = 124,
    SPV_OP_S_NEGATE = 126,
    SPV_OP_F_NEGATE = 127,
    SPV_OP_I_ADD = 128,
    SPV_OP_F_ADD = 129,
    SPV_OP_I_SUB = 130,
    SPV_OP_F_SUB = 131,
    SPV_OP_I_MUL = 132,
    SPV_OP_F_MUL = 133,
    SPV_OP_U_DIV = 134,
    SPV_OP_S_DIV = 135,
    SPV_OP_F_DIV = 136,
    SPV_OP_U_MOD = 137,
    SPV_OP_S_MOD = 139,
    SPV_OP_F_MOD = 141,
    SPV_OP_VECTOR_TIMES_SCALAR = 142,
    SPV_OP_DOT = 148,
    SPV_OP_U_MUL_EXTENDED = 151,
    SPV_OP_ANY = 154,
    SPV_OP_ALL = 155,
    SPV_OP_IS_NAN = 156,
    SPV_OP_IS_INF = 157,
    SPV_OP_LOGICAL_EQUAL = 164,
    SPV_OP_LOGICAL_NOT_EQUAL = 165,
    SPV_OP_LOGICAL_OR = 166,
    SPV_OP_LOGICAL_AND = 167,
    SPV_OP_LOGICAL_NOT = 168,
    SPV_OP_SELECT = 169,
    SPV_OP_I_EQUAL = 170,
    SPV_OP_I_NOT_EQUAL = 171,
    SPV_OP_U_GREATER_THAN = 172,
    SPV_OP_S_GREATER_THAN = 173,
    SPV_OP_U_GREATER_THAN_EQUAL = 174,
    SPV_OP_S_GREATER_THAN_EQUAL = 175,
    SPV_OP_U_LESS_THAN = 176,
    SPV_OP_S_LESS_THAN = 177,
    SPV_OP_U_LESS_THAN_EQUAL = 178,
    SPV_OP_S_LESS_THAN_EQUAL = 179,
    SPV_OP_F_ORD_EQUAL = 180,
    SPV_OP_F_UNORD_NOT_EQUAL = 183,
    SPV_OP_F_ORD_LESS_THAN = 184,
    SPV_OP_F_ORD_GREATER_THAN = 186,
    SPV_OP_F_ORD_LESS_THAN_EQUAL = 188,
    SPV_OP_F_ORD_GREATER_THAN_EQUAL = 190,
    SPV_OP_SHIFT_RIGHT_LOGICAL = 194,
    SPV_OP_SHIFT_RIGHT_ARITHMETIC = 195,
    SPV_OP_SHIFT_LEFT_LOGICAL = 196,
    SPV_OP_BITWISE_OR = 197,
    SPV_OP_BITWISE_XOR = 198,
    SPV_OP_BITWISE_AND = 199,
    SPV_OP_NOT = 200,
    SPV_OP_BIT_FIELD_INSERT = 201,
    SPV_OP_BIT_FIELD_S_EXTRACT = 202,
    SPV_OP_BIT_FIELD_U_EXTRACT = 203,
    SPV_OP_BIT_REVERSE = 204,
    SPV_OP_BIT_COUNT = 205,
    SPV_OP_DPDX = 207,
    SPV_OP_DPDY = 208,
    SPV_OP_FWIDTH = 209,
    SPV_OP_EMIT_VERTEX = 218,
    SPV_OP_END_PRIMITIVE = 219,
    SPV_OP_CONTROL_BARRIER = 224,
    SPV_OP_MEMORY_BARRIER = 225,
    SPV_OP_ATOMIC_LOAD = 227,
    SPV_OP_ATOMIC_EXCHANGE = 229,
    SPV_OP_ATOMIC_COMPARE_EXCHANGE = 230,
    SPV_OP_ATOMIC_I_ADD = 234,
    SPV_OP_ATOMIC_S_MIN = 236,
    SPV_OP_ATOMIC_U_MIN = 237,
    SPV_OP_ATOMIC_S_MAX = 238,
    SPV_OP_ATOMIC_U_MAX = 239,
    SPV_OP_ATOMIC_AND = 240,
    SPV_OP_ATOMIC_OR = 241,
    SPV_OP_ATOMIC_XOR = 242,
    SPV_OP_PHI = 245,
    SPV_OP_LOOP_MERGE = 246,
    SPV_OP_SELECTION_MERGE = 247,
    SPV_OP_LABEL = 248,
    SPV_OP_BRANCH = 249,
    SPV_OP_BRANCH_CONDITIONAL = 250,
    SPV_OP_SWITCH = 251,
    SPV_OP_KILL = 252,
    SPV_OP_RETURN = 253,
    SPV_OP_RETURN_VALUE = 254,
    SPV_OP_UNREACHABLE = 255,
    SPV_OP_GROUP_NON_UNIFORM_BALLOT = 339,
    SPV_OP_GROUP_NON_UNIFORM_SHUFFLE = 345,
    SPV_OP_GROUP_NON_UNIFORM_U_MIN = 354,
    SPV_OP_READ_CLOCK_KHR = 5056,
    SPV_OP_DEMOTE_TO_HELPER_INVOCATION = 5380,
    SPV_OP_IS_HELPER_INVOCATION = 5381
};
enum {
    SPV_STORAGE_UNIFORM_CONSTANT = 0,
    SPV_STORAGE_INPUT = 1,
    SPV_STORAGE_UNIFORM = 2,
    SPV_STORAGE_OUTPUT = 3,
    SPV_STORAGE_WORKGROUP = 4,
    SPV_STORAGE_PRIVATE = 6,
    SPV_STORAGE_FUNCTION = 7,
    SPV_STORAGE_PUSH_CONSTANT = 9,
    SPV_STORAGE_IMAGE = 11,
    SPV_STORAGE_STORAGE_BUFFER = 12
};
enum {
    SPV_DECORATION_BLOCK = 2,
    SPV_DECORATION_ARRAY_STRIDE = 6,
    SPV_DECORATION_BUILT_IN = 11,
    SPV_DECORATION_NO_PERSPECTIVE = 13,
    SPV_DECORATION_FLAT = 14,
    SPV_DECORATION_PATCH = 15,
    SPV_DECORATION_INVARIANT = 18,
    SPV_DECORATION_NON_WRITABLE = 24,
    SPV_DECORATION_LOCATION = 30,
    SPV_DECORATION_COMPONENT = 31,
    SPV_DECORATION_BINDING = 33,
    SPV_DECORATION_DESCRIPTOR_SET = 34,
    SPV_DECORATION_OFFSET = 35,
    SPV_DECORATION_XFB_BUFFER = 36,
    SPV_DECORATION_XFB_STRIDE = 37,
    SPV_DECORATION_NO_CONTRACTION = 42,
    SPV_DECORATION_NON_UNIFORM = 5300
};
enum {
    SPV_BUILT_IN_POSITION = 0,
    SPV_BUILT_IN_POINT_SIZE = 1,
    SPV_BUILT_IN_CLIP_DISTANCE = 3,
    SPV_BUILT_IN_LAYER = 9,
    SPV_BUILT_IN_FRAG_COORD = 15,
    SPV_BUILT_IN_FRONT_FACING = 17,
    SPV_BUILT_IN_SAMPLE_ID = 18,
    SPV_BUILT_IN_FRAG_DEPTH = 22
};
enum {
    SPV_MODE_ORIGIN_UPPER_LEFT = 7,
    SPV_MODE_XFB = 11,
    SPV_MODE_DEPTH_REPLACING = 12,
    SPV_MODE_LOCAL_SIZE = 17,
    SPV_MODE_DENORM_FLUSH_TO_ZERO = 4460,     /* SPV_KHR_float_controls, its operand the width */
    SPV_MODE_SIGNED_ZERO_INF_NAN_PRESERVE = 4461 /* SPV_KHR_float_controls, its operand the width */
};
enum {
    SPV_CAPABILITY_SHADER = 1,
    SPV_CAPABILITY_FLOAT16 = 9,
    SPV_CAPABILITY_FLOAT64 = 10,
    SPV_CAPABILITY_INT64 = 11,
    SPV_CAPABILITY_IMAGE_GATHER_EXTENDED = 25,
    SPV_CAPABILITY_CLIP_DISTANCE = 32,
    SPV_CAPABILITY_SAMPLED_BUFFER = 46,
    SPV_CAPABILITY_IMAGE_BUFFER = 47,
    SPV_CAPABILITY_IMAGE_QUERY = 50,
    SPV_CAPABILITY_TRANSFORM_FEEDBACK = 53,
    SPV_CAPABILITY_STORAGE_IMAGE_WRITE_WITHOUT_FORMAT = 56,
    SPV_CAPABILITY_GROUP_NON_UNIFORM = 61,
    SPV_CAPABILITY_GROUP_NON_UNIFORM_ARITHMETIC = 63,
    SPV_CAPABILITY_GROUP_NON_UNIFORM_BALLOT = 64,
    SPV_CAPABILITY_GROUP_NON_UNIFORM_SHUFFLE = 65,
    SPV_CAPABILITY_SHADER_LAYER = 69,
    SPV_CAPABILITY_DRAW_PARAMETERS = 4427,
    SPV_CAPABILITY_DENORM_FLUSH_TO_ZERO = 4465,
    SPV_CAPABILITY_SIGNED_ZERO_INF_NAN_PRESERVE = 4466,
    SPV_CAPABILITY_SHADER_CLOCK_KHR = 5055,
    SPV_CAPABILITY_SHADER_NON_UNIFORM = 5301,
    SPV_CAPABILITY_SAMPLED_IMAGE_ARRAY_NON_UNIFORM_INDEXING = 5307,
    SPV_CAPABILITY_DEMOTE_TO_HELPER_INVOCATION = 5379
};
/* image dimensions: OpTypeImage's Dim */
enum { SPV_DIM_1D = 0, SPV_DIM_2D = 1, SPV_DIM_3D = 2, SPV_DIM_CUBE = 3, SPV_DIM_BUFFER = 5 };
/* the GLSL.std.450 instructions */
typedef enum {
    SPV_GLSL_ROUND = 1,
    SPV_GLSL_ROUND_EVEN = 2,
    SPV_GLSL_TRUNC = 3,
    SPV_GLSL_F_ABS = 4,
    SPV_GLSL_S_ABS = 5,
    SPV_GLSL_F_SIGN = 6,
    SPV_GLSL_FLOOR = 8,
    SPV_GLSL_CEIL = 9,
    SPV_GLSL_FRACT = 10,
    SPV_GLSL_SIN = 13,
    SPV_GLSL_COS = 14,
    SPV_GLSL_POW = 26,
    SPV_GLSL_EXP = 27,
    SPV_GLSL_LOG = 28,
    SPV_GLSL_EXP2 = 29,
    SPV_GLSL_LOG2 = 30,
    SPV_GLSL_SQRT = 31,
    SPV_GLSL_INVERSE_SQRT = 32,
    SPV_GLSL_F_MIN = 37,
    SPV_GLSL_U_MIN = 38,
    SPV_GLSL_S_MIN = 39,
    SPV_GLSL_F_MAX = 40,
    SPV_GLSL_U_MAX = 41,
    SPV_GLSL_S_MAX = 42,
    SPV_GLSL_F_CLAMP = 43,
    SPV_GLSL_U_CLAMP = 44,
    SPV_GLSL_S_CLAMP = 45,
    SPV_GLSL_F_MIX = 46,
    SPV_GLSL_STEP = 48,
    SPV_GLSL_SMOOTH_STEP = 49,
    SPV_GLSL_FMA = 50,
    SPV_GLSL_PACK_HALF2X16 = 58,
    SPV_GLSL_UNPACK_HALF2X16 = 62,
    SPV_GLSL_LENGTH = 66,
    SPV_GLSL_DISTANCE = 67,
    SPV_GLSL_CROSS = 68,
    SPV_GLSL_NORMALIZE = 69,
    SPV_GLSL_REFLECT = 71,
    SPV_GLSL_N_MIN = 79,
    SPV_GLSL_N_MAX = 80,
    SPV_GLSL_N_CLAMP = 81
} SpvGlsl450Op;

void spv_emit_init(SpvEmitter* e, size_t estimated_function_words);
void spv_emit_destroy(SpvEmitter* e);
uint32_t spv_emit_id(SpvEmitter* e);
/* the GLSL.std.450 import, made on the first call */
uint32_t spv_emit_glsl450_import(SpvEmitter* e);
void spv_emit_shader_capability(SpvEmitter* e);
void spv_emit_float16_capability(SpvEmitter* e);
void spv_emit_demote_extension(SpvEmitter* e);
void spv_emit_shader_clock_extension(SpvEmitter* e);
void spv_emit_float64_capability(SpvEmitter* e);
void spv_emit_draw_parameters_capability(SpvEmitter* e);
void spv_emit_layer_capability(SpvEmitter* e);
void spv_emit_clip_distance_capability(SpvEmitter* e);
uint32_t spv_emit_layer_output(SpvEmitter* e, uint32_t i32_type);
uint32_t spv_emit_clip_distance_output(SpvEmitter* e, uint32_t f32_type, uint32_t u32_type, uint32_t length);
uint32_t spv_emit_output_element_pointer(SpvEmitter* e, uint32_t f32_type, uint32_t array, uint32_t index);
void spv_emit_image_query_capability(SpvEmitter* e);
void spv_emit_subgroup_capabilities(SpvEmitter* e);
void spv_emit_image_gather_extended_capability(SpvEmitter* e);
void spv_emit_entry_str(SpvEmitter* e, SpvShaderStage stage, uint32_t function, const char* name,
                        const uint32_t* interfaces, size_t interface_count);
void spv_emit_compute_entry_str(SpvEmitter* e, uint32_t function, const char* name, const uint32_t* interfaces,
                                size_t interface_count);
void spv_emit_fragment_origin_upper_left(SpvEmitter* e, uint32_t function);
void spv_emit_fragment_depth_replacing(SpvEmitter* e, uint32_t function);
void spv_emit_float_controls(SpvEmitter* e, uint32_t function, int flush_denorms, int preserve_infnan,
                             int preserve_infnan64);
void spv_emit_local_size(SpvEmitter* e, uint32_t function, uint32_t x, uint32_t y, uint32_t z);
void spv_emit_name_str(SpvEmitter* e, uint32_t id, const char* name);
uint32_t spv_emit_type_void(SpvEmitter* e);
uint32_t spv_emit_type_bool(SpvEmitter* e);
uint32_t spv_emit_type_u32(SpvEmitter* e);
uint32_t spv_emit_type_i32(SpvEmitter* e);
/* a 64-bit integer type: the module declares the Int64 capability (11) */
uint32_t spv_emit_type_int64(SpvEmitter* e, int is_signed);
/* OpSConvert requires a signed result type; OpUConvert requires an unsigned result type. Both truncate if narrowing. */
uint32_t spv_emit_int_convert(SpvEmitter* e, uint32_t type, uint32_t value, int is_signed);
/* OpFConvert: a float of one width to another (F32 and F64) */
uint32_t spv_emit_fconvert(SpvEmitter* e, uint32_t type, uint32_t value);
uint32_t spv_emit_convert_f_to_s(SpvEmitter* e, uint32_t i32_type, uint32_t value);
uint32_t spv_emit_convert_f_to_u(SpvEmitter* e, uint32_t u32_type, uint32_t value);
uint32_t spv_emit_convert_s_to_f(SpvEmitter* e, uint32_t f32_type, uint32_t value);
uint32_t spv_emit_convert_u_to_f(SpvEmitter* e, uint32_t f32_type, uint32_t value);
uint32_t spv_emit_convert_f_to_f(SpvEmitter* e, uint32_t result_type, uint32_t value);
uint32_t spv_emit_type_f16(SpvEmitter* e);
uint32_t spv_emit_type_f32(SpvEmitter* e);
uint32_t spv_emit_type_f64(SpvEmitter* e);
uint32_t spv_emit_type_vec4(SpvEmitter* e, uint32_t component_type);
uint32_t spv_emit_type_vec2(SpvEmitter* e, uint32_t component_type);
uint32_t spv_emit_type_vec3(SpvEmitter* e, uint32_t component_type);
uint32_t spv_emit_type_runtime_array(SpvEmitter* e, uint32_t element_type, uint32_t stride);
uint32_t spv_emit_type_struct_one(SpvEmitter* e, uint32_t member_type);
uint32_t spv_emit_type_storage_pointer(SpvEmitter* e, uint32_t pointee_type);
uint32_t spv_emit_type_function_pointer(SpvEmitter* e, uint32_t pointee_type);
uint32_t spv_emit_type_workgroup_pointer(SpvEmitter* e, uint32_t pointee_type);
uint32_t spv_emit_type_array_u32(SpvEmitter* e, uint32_t u32_type, uint32_t length);
uint32_t spv_emit_workgroup_variable(SpvEmitter* e, uint32_t pointer_type);
uint32_t spv_emit_function_element_pointer(SpvEmitter* e, uint32_t pointer_type, uint32_t array, uint32_t index);
uint32_t spv_emit_type_function_void(SpvEmitter* e, uint32_t void_type);
uint32_t spv_emit_constant_u32(SpvEmitter* e, uint32_t type, uint32_t value);
/* whether id is a 32-bit OpConstant this emitter made, and its type and bits */
int spv_emit_constant_bits(const SpvEmitter* e, uint32_t id, uint32_t* type, uint32_t* value);
uint32_t spv_emit_constant_bool(SpvEmitter* e, uint32_t type, int value);
/* a constant of a 64-bit type (F64, or a 64-bit integer) from its bits */
uint32_t spv_emit_constant64(SpvEmitter* e, uint32_t type, uint64_t value);
uint32_t spv_emit_constant_f32(SpvEmitter* e, uint32_t type, float value);
uint32_t spv_emit_constant_vec4(SpvEmitter* e, uint32_t type, const uint32_t components[4]);
uint32_t spv_emit_constant_vec2(SpvEmitter* e, uint32_t type, uint32_t x, uint32_t y);
uint32_t spv_emit_constant_vec3(SpvEmitter* e, uint32_t type, uint32_t x, uint32_t y, uint32_t z);
uint32_t spv_emit_storage_buffer(SpvEmitter* e, uint32_t struct_type, uint32_t set, uint32_t binding);
uint32_t spv_emit_uniform_vec4_array(SpvEmitter* e, uint32_t vec4_type, uint32_t component_type, uint32_t u32_type,
                                     uint32_t set, uint32_t binding, uint32_t elements,
                                     uint32_t* component_pointer_type);
uint32_t spv_emit_uniform_component(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member,
                                    uint32_t element, uint32_t component, uint32_t component_type);
/* a push constant block: a vec4 at offset, then a 16-byte member of second_type unless it is 0 */
uint32_t spv_emit_push_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t f32_type, uint32_t offset,
                            uint32_t second_type, uint32_t* component_pointer_type);
uint32_t spv_emit_push_component_f32(SpvEmitter* e, uint32_t pointer_type, uint32_t block, uint32_t member,
                                     uint32_t component, uint32_t f32_type);
uint32_t spv_emit_output_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location);
/* a Private variable of type, the stage's own (SPIR-V 1.4 on lists it in the entry point's interface) */
uint32_t spv_emit_private_variable(SpvEmitter* e, uint32_t type);
/* an Output of a scalar type at location and component: one component of a vec4 output split into its four */
uint32_t spv_emit_output_component(SpvEmitter* e, uint32_t type, uint32_t location, uint32_t component);
/* variable captured by transform feedback into buffer at offset bytes, its vertices stride bytes apart there. The
   module needs the TransformFeedback capability (53) and its entry point the Xfb execution mode (11), once */
void spv_emit_xfb_capture(SpvEmitter* e, uint32_t variable, uint32_t buffer, uint32_t stride, uint32_t offset);
uint32_t spv_emit_position_output(SpvEmitter* e, uint32_t vec4_type);
uint32_t spv_emit_point_size_output(SpvEmitter* e, uint32_t f32_type);
uint32_t spv_emit_frag_depth_output(SpvEmitter* e, uint32_t f32_type);
/* interpolation: 0 perspective, 1 flat, 2 no perspective */
uint32_t spv_emit_input_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location, int interpolation);
uint32_t spv_emit_fragcoord_input(SpvEmitter* e, uint32_t vec4_type);
uint32_t spv_emit_builtin_input_vec3(SpvEmitter* e, uint32_t vec3_type, uint32_t builtin);
uint32_t spv_emit_builtin_input_u32(SpvEmitter* e, uint32_t u32_type, uint32_t builtin);
void spv_emit_flat(SpvEmitter* e, uint32_t variable);
/* a capability, declared once however often it is asked for */
void spv_emit_capability(SpvEmitter* e, uint32_t capability);
void spv_emit_xfb_position(SpvEmitter* e, uint32_t function, uint32_t position); /* SWR: DLSS motion vectors */
/* An Input (1) or Output (3) variable of type, or of an array of count of it when count isn't 0, decorated BuiltIn
   builtin, or Location location when builtin is SPV_NOT_BUILTIN, and Patch when patch is set */
#define SPV_NOT_BUILTIN 0xffffffffu
uint32_t spv_emit_interface_variable(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t u32_type, uint32_t count,
                                     uint32_t builtin, uint32_t location, int patch);
/* a pointer to one scalar of an interface variable: its element (an id, or 0 when the variable isn't an array), then
   its component (an id) */
uint32_t spv_emit_interface_scalar_pointer(SpvEmitter* e, uint32_t storage, uint32_t scalar_type, uint32_t variable,
                                           uint32_t element, uint32_t component);
void spv_emit_execution_mode(SpvEmitter* e, uint32_t function, uint32_t mode, int has_operand, uint32_t operand);
void spv_emit_emit_vertex(SpvEmitter* e);
void spv_emit_end_primitive(SpvEmitter* e);
/* a tessellation control barrier: the patch's invocations, and its outputs written before it */
void spv_emit_patch_barrier(SpvEmitter* e, uint32_t u32_type);
uint32_t spv_emit_sampler_image_array(SpvEmitter* e, uint32_t scalar_type, uint32_t u32_type, uint32_t set,
                                      uint32_t binding, uint32_t type, int array, int shadow, uint32_t count,
                                      uint32_t* sampled_type);
uint32_t spv_emit_sampler_array_element(SpvEmitter* e, uint32_t sampled_type, uint32_t array_variable, uint32_t index);
uint32_t spv_emit_sampler2d(SpvEmitter* e, uint32_t f32_type, uint32_t set, uint32_t binding, int shadow,
                            uint32_t* sampled_type);
uint32_t spv_emit_sampler_image(SpvEmitter* e, uint32_t f32_type, uint32_t set, uint32_t binding, uint32_t type,
                                int array, int shadow, int multisample, uint32_t* sampled_type);
uint32_t spv_emit_storage_image(SpvEmitter* e, uint32_t scalar_type, uint32_t set, uint32_t binding, uint32_t type,
                                int array, uint32_t format, uint32_t* image_type);
void spv_emit_image_write(SpvEmitter* e, uint32_t image_type, uint32_t image_variable, uint32_t coordinates,
                          uint32_t texel);
uint32_t spv_emit_image_read(SpvEmitter* e, uint32_t result_type, uint32_t image_type, uint32_t image_variable,
                             uint32_t coordinates);
uint32_t spv_emit_image_texel_pointer(SpvEmitter* e, uint32_t scalar_type, uint32_t image_variable,
                                      uint32_t coordinates, uint32_t sample);
uint32_t spv_emit_vec3_construct(SpvEmitter* e, uint32_t vec3_type, uint32_t x, uint32_t y, uint32_t z);
void spv_emit_function_void(SpvEmitter* e, uint32_t void_type, uint32_t function, uint32_t function_type);
void spv_emit_label(SpvEmitter* e, uint32_t label);
uint32_t spv_emit_function_variable(SpvEmitter* e, uint32_t pointer_type);
uint32_t spv_emit_function_variable_init(SpvEmitter* e, uint32_t pointer_type, uint32_t initial_value);
uint32_t spv_emit_load(SpvEmitter* e, uint32_t result_type, uint32_t pointer);
uint32_t spv_emit_bitcast(SpvEmitter* e, uint32_t result_type, uint32_t value);
uint32_t spv_emit_fadd(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_fmul(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_i_equal(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right);
uint32_t spv_emit_i_not_equal(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right);
uint32_t spv_emit_u_less(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right);
uint32_t spv_emit_s_greater(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right);
uint32_t spv_emit_isub_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_shift_left_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t shift);
uint32_t spv_emit_shift_right_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t shift);
uint32_t spv_emit_shift_right_s32(SpvEmitter* e, uint32_t i32_type, uint32_t value, uint32_t shift);
uint32_t spv_emit_sdiv_i32(SpvEmitter* e, uint32_t i32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_bitwise_and_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_bitwise_or_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_bitwise_xor_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_not_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value);
uint32_t spv_emit_bitfield_uextract(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t offset, uint32_t count);
uint32_t spv_emit_bit_reverse_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value);
uint32_t spv_emit_atomic_u32(SpvEmitter* e, uint32_t u32_type, uint32_t opcode, uint32_t pointer, uint32_t value);
uint32_t spv_emit_atomic_shared_u32(SpvEmitter* e, uint32_t u32_type, uint32_t opcode, uint32_t pointer,
                                    uint32_t value);
uint32_t spv_emit_atomic_load_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer);
uint32_t spv_emit_atomic_load_shared_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer);
uint32_t spv_emit_atomic_compare_exchange_shared_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer,
                                                     uint32_t desired, uint32_t expected);
uint32_t spv_emit_atomic_compare_exchange_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer, uint32_t desired,
                                              uint32_t expected);
void spv_emit_control_barrier(SpvEmitter* e, uint32_t u32_type);
/* OpMemoryBarrier in that memory scope (1 Device, 2 Workgroup), with those memory semantics */
void spv_emit_memory_barrier(SpvEmitter* e, uint32_t u32_type, uint32_t scope, uint32_t semantics);
uint32_t spv_emit_bitfield_insert(SpvEmitter* e, uint32_t u32_type, uint32_t base, uint32_t insert, uint32_t offset,
                                  uint32_t count);
uint32_t spv_emit_bit_count(SpvEmitter* e, uint32_t u32_type, uint32_t value);
uint32_t spv_emit_bitfield_sextract(SpvEmitter* e, uint32_t i32_type, uint32_t value, uint32_t offset, uint32_t count);
uint32_t spv_emit_fcompare(SpvEmitter* e, uint32_t bool_type, uint32_t condition, uint32_t left, uint32_t right);
uint32_t spv_emit_bool_not(SpvEmitter* e, uint32_t bool_type, uint32_t value);
uint32_t spv_emit_bool_binary(SpvEmitter* e, uint32_t bool_type, uint32_t op, uint32_t left, uint32_t right);
uint32_t spv_emit_fdiv(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_fnegate(SpvEmitter* e, uint32_t f32_type, uint32_t value);
uint32_t spv_emit_derivative(SpvEmitter* e, uint32_t f32_type, uint32_t value, int y_axis);
uint32_t spv_emit_glsl_unary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t value);
uint32_t spv_emit_glsl_binary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t left,
                              uint32_t right);
uint32_t spv_emit_glsl_ternary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t a,
                               uint32_t b, uint32_t c);
uint32_t spv_emit_isnan(SpvEmitter* e, uint32_t bool_type, uint32_t value);
uint32_t spv_emit_select(SpvEmitter* e, uint32_t result_type, uint32_t condition, uint32_t yes, uint32_t no);
/* a 32-bit float with a subnormal flushed to the zero of its sign, NaN and everything else as it is: with a float test
   when the module preserves signed zeros (SignedZeroInfNanPreserve), else on its bits */
uint32_t spv_emit_flush_subnormal(SpvEmitter* e, uint32_t f32_type, uint32_t u32_type, uint32_t bool_type, uint32_t glsl,
                                  uint32_t value, int signed_zeros);
uint32_t spv_emit_vec4_extract(SpvEmitter* e, uint32_t component_type, uint32_t vector, uint32_t component);
uint32_t spv_emit_subgroup_ballot(SpvEmitter* e, uint32_t vec4_type, uint32_t u32_type, uint32_t predicate);
uint32_t spv_emit_subgroup_umin_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value);
void spv_emit_subgroup_arithmetic_capability(SpvEmitter* e);
uint32_t spv_emit_subgroup_shuffle_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t lane);
uint32_t spv_emit_vec4_insert(SpvEmitter* e, uint32_t vec4_type, uint32_t vector, uint32_t component_value,
                              uint32_t component);
uint32_t spv_emit_vec4_construct(SpvEmitter* e, uint32_t vec4_type, const uint32_t components[4]);
uint32_t spv_emit_vec2_construct(SpvEmitter* e, uint32_t vec2_type, uint32_t x, uint32_t y);
uint32_t spv_emit_sample2d(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                           uint32_t coordinates, uint32_t depth_reference);
uint32_t spv_emit_sample2d_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                               uint32_t coordinates, uint32_t depth_reference, uint32_t lod);
uint32_t spv_emit_image_query_size_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                       uint32_t lod);
uint32_t spv_emit_image_query_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                  uint32_t coordinates);
uint32_t spv_emit_image_query_levels(SpvEmitter* e, uint32_t u32_type, uint32_t sampled_type, uint32_t sampler);
uint32_t spv_emit_image_fetch(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                              uint32_t coordinates, uint32_t lod);
uint32_t spv_emit_image_fetch_sample(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                     uint32_t coordinates, uint32_t sample);
uint32_t spv_emit_image_gather_ex(SpvEmitter* e, uint32_t vec4_type, uint32_t sampled_type, uint32_t sampler,
                                  uint32_t coordinates, uint32_t component, uint32_t dref, uint32_t offset);
/* mode: 0 implicit LOD, 1 explicit LOD, 2 bias. Optional depth and texel offset. */
uint32_t spv_emit_sample_image(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                               uint32_t coordinates, uint32_t depth_reference, uint32_t mode, uint32_t level,
                               uint32_t offset, int dynamic_offset);
/* OpImageSampleExplicitLod with the Grad operand (dx and dy) and an optional constant offset */
uint32_t spv_emit_sample_grad(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                              uint32_t coordinates, uint32_t dx, uint32_t dy, uint32_t offset);
void spv_emit_no_contraction(SpvEmitter* e, uint32_t result);
uint32_t spv_emit_storage_access_u32(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member_index,
                                     uint32_t element_index);
uint32_t spv_emit_iadd_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_imul_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_umul_high_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
void spv_emit_store_u32(SpvEmitter* e, uint32_t pointer, uint32_t value);
void spv_emit_store(SpvEmitter* e, uint32_t pointer, uint32_t value);
void spv_emit_return(SpvEmitter* e);
void spv_emit_kill(SpvEmitter* e);
/* OpDemoteToHelperInvocation: the module declares the DemoteToHelperInvocation capability (5379) */
void spv_emit_demote(SpvEmitter* e);
/* OpIsHelperInvocationEXT: whether the lane is a helper, including one demoted by KIL */
uint32_t spv_emit_is_helper(SpvEmitter* e, uint32_t bool_type);
uint32_t spv_emit_read_clock(SpvEmitter* e, uint32_t vec2_u32_type, uint32_t scope);
void spv_emit_selection_merge(SpvEmitter* e, uint32_t merge_label);
void spv_emit_branch_conditional(SpvEmitter* e, uint32_t condition, uint32_t yes_label, uint32_t no_label);
void spv_emit_branch(SpvEmitter* e, uint32_t label);
void spv_emit_loop_merge(SpvEmitter* e, uint32_t merge_label, uint32_t continue_label);
void spv_emit_switch_u32(SpvEmitter* e, uint32_t selector, uint32_t default_label, const uint32_t* literals,
                         const uint32_t* labels, size_t count);
void spv_emit_function_end(SpvEmitter* e);
/* The caller owns the returned words. A module is assembled once after all functions are emitted. */
uint32_t* spv_emit_finish(const SpvEmitter* e, size_t* word_count);

/* ---- the generic layer ---- */

/* an extension, declared once however often it is asked for */
void spv_emit_extension(SpvEmitter* e, const char* name);

/* types, made once each */
uint32_t spv_emit_type_int(SpvEmitter* e, uint32_t width, int is_signed);
uint32_t spv_emit_type_float(SpvEmitter* e, uint32_t width);
uint32_t spv_emit_type_vector(SpvEmitter* e, uint32_t component, uint32_t count);
/* an array of length (a number, made a constant here) */
uint32_t spv_emit_type_array(SpvEmitter* e, uint32_t element, uint32_t length);
uint32_t spv_emit_type_pointer(SpvEmitter* e, uint32_t storage, uint32_t type);
uint32_t spv_emit_type_function(SpvEmitter* e, uint32_t result, const uint32_t* params, size_t count);
/* OpTypeImage: dim an SPV_DIM_*, sampled 1 for an image sampled through a sampler, 2 for a storage image (with its
   format) */
uint32_t spv_emit_type_image(SpvEmitter* e, uint32_t sampled_type, uint32_t dim, int depth, int arrayed,
                             int multisampled, uint32_t sampled, uint32_t format);
uint32_t spv_emit_type_sampled_image(SpvEmitter* e, uint32_t image);
/* a struct of its own each time (structs are decorated, as blocks): members with their byte offsets */
uint32_t spv_emit_type_struct(SpvEmitter* e, const uint32_t* members, const uint32_t* offsets, size_t count);

/* constants, made once each: 32-bit unsigned and signed integers, 32-bit floats, booleans, composites */
uint32_t spv_emit_const_u32(SpvEmitter* e, uint32_t value);
uint32_t spv_emit_const_i32(SpvEmitter* e, int32_t value);
uint32_t spv_emit_const_f32(SpvEmitter* e, float value);
uint32_t spv_emit_const_bool(SpvEmitter* e, int value);
uint32_t spv_emit_const_composite(SpvEmitter* e, uint32_t type, const uint32_t* parts, size_t count);

void spv_emit_decorate(SpvEmitter* e, uint32_t id, uint32_t decoration, const uint32_t* operands, size_t count);
void spv_emit_member_decorate(SpvEmitter* e, uint32_t type, uint32_t member, uint32_t decoration,
                              const uint32_t* operands, size_t count);
/* a module-scope variable of type (the pointer is made here); an Input or Output one is kept in interface, the
   entry point's */
uint32_t spv_emit_variable(SpvEmitter* e, uint32_t storage, uint32_t type);
/* a Private (or Output) one that starts as zeros: a variable without an initializer holds undefined values until
   written, which drivers give differently */
uint32_t spv_emit_variable_zeroed(SpvEmitter* e, uint32_t storage, uint32_t type);
/* a decorated resource: a Uniform block or a UniformConstant image or sampler at set and binding */
uint32_t spv_emit_resource(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t set, uint32_t binding);
/* an Input or Output at a location, or a built-in */
uint32_t spv_emit_location(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t location);
uint32_t spv_emit_builtin(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t builtin);

/* a function: its id (made beforehand to call it before it is written, or 0 for a new one), its result and parameter
   types. Parameters come back in params; the first block's label is placed. spv_emit_function_end ends it */
uint32_t spv_emit_function(SpvEmitter* e, uint32_t id, uint32_t result, const uint32_t* param_types, size_t count,
                           uint32_t* params);
/* a Function variable of type (the pointer is made here) in the first block of the spv_emit_function being written */
uint32_t spv_emit_local(SpvEmitter* e, uint32_t type);
/* an instruction of the function's: with a result of type (its id returned), or without one (type 0, nothing
   returned) */
uint32_t spv_emit_op(SpvEmitter* e, uint32_t opcode, uint32_t type, const uint32_t* operands, size_t count);
void spv_emit_void_op(SpvEmitter* e, uint32_t opcode, const uint32_t* operands, size_t count);
/* a GLSL.std.450 instruction (the import is made on the first) */
uint32_t spv_emit_ext(SpvEmitter* e, uint32_t type, uint32_t instruction, const uint32_t* operands, size_t count);

/* the shorthands: SPV(e, opcode, type, operands...) and SPV_VOID(e, opcode, operands...), SPV_EXT(e, type,
   instruction, operands...), each with one operand at least (C99 has no empty variadic arguments): an instruction
   without any is spv_emit_void_op(e, opcode, NULL, 0) */
#define SPV_ARGS(...) ((const uint32_t[]){__VA_ARGS__}), (sizeof((const uint32_t[]){__VA_ARGS__}) / 4)
#define SPV(e, opcode, type, ...) spv_emit_op((e), (opcode), (type), SPV_ARGS(__VA_ARGS__))
#define SPV_VOID(e, opcode, ...) spv_emit_void_op((e), (opcode), SPV_ARGS(__VA_ARGS__))
#define SPV_EXT(e, type, instruction, ...) spv_emit_ext((e), (type), (instruction), SPV_ARGS(__VA_ARGS__))
#endif
