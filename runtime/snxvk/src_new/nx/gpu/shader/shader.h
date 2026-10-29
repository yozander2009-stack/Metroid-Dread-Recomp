/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Maxwell shader programs: what translation learns about one (ShaderInfo), the descriptor layout the SPIR-V uses, and
   the translator's entry points. */
#ifndef SNX_SHADER_H
#define SNX_SHADER_H
#include "common/cpu/aarch64/cpu.h"
#ifdef __cplusplus
extern "C" {
#endif
/* program stages; the values index tables and are stored in the corpus */
enum { ST_VS = 0, ST_TCS = 1, ST_TES = 2, ST_GS = 3, ST_FS = 4, ST_CS = 5 };
#define SH_MAXTEX         64 /* texture slots per program */
#define SH_INDEXED_TABLES 10 /* indexed texture tables per program */
/* A 1D texture is bound as a 2D image, and a buffer texture longer than a row of one as rows of 2^SH_TEXEL_ROW_SHIFT
   texels (texture.c): a 1D texel fetch at x reads (x mod row, x / row) of it, which is (x, 0) for every texture that
   fits one row */
#define SH_TEXEL_ROW_SHIFT 14
/* a texture slot */
typedef struct {
    u32 cbuf_off; /* where its handle is in the texture constant buffer, in words */
    u8 type;      /* 0 1D, 1 2D, 2 3D, 3 cube */
    u8 array, shadow;
    u8 bindless;
    u8 bl_cbuf;  /* bindless: the constant buffer holding the handle */
    u8 bl2;      /* the handle is two reads ORed: the texture's and the sampler's */
    u8 bl2_cbuf; /* the second read's constant buffer */
    u8 flags;    /* bit 0 raw integer surface load, bit 1 multisample fetch, bit 2 SNX_TEX_INDEXED; bits 4-7 the
                    indexed table's stride shift */
    u32 bl2_off; /* the second read's offset, in words */
    u8 use;      /* SWR: how the program reads it: bit 0 filtered samples (TEX, TEXS, TXD), bit 1 anything else
                    (TLD, TLDS fetches, TLD4 gathers, TMML queries). Only a slot of filtered samples alone may be
                    given a smaller averaged copy (vk/texture.c vk_rt_prefiltered) */
} ShTex;
/* whether two indexed slots read the same handle table the same way */
static inline int shader_same_indexed_table(const ShTex* a, const ShTex* b) {
    return a->bl_cbuf == b->bl_cbuf && a->cbuf_off == b->cbuf_off && a->flags == b->flags && a->type == b->type &&
           a->array == b->array && a->shadow == b->shadow;
}
/* A global buffer (ShaderInfo.gbuf_cbuf_off) is named by where its 64-bit address is: the constant buffer in bits 16-20,
   the byte offset in it in bits 0-15. NVN's storage buffer descriptors, in cbuf 0 from 0x110 to 0x410 (compute) or
   0x830, follow the address with the buffer's size (shader_gbuf_sized). A graphics program may also keep a bare
   address in a constant buffer of its own, with nothing after it: one game's terrain vertex programs read their patch
   data through such an address in cbuf 6, as Eden tracks them when no NVN descriptor holds the base
   (global_memory_to_storage_buffer_pass.cpp's Track without the NVN bias). The renderer sizes those by the memory
   mapped from the address on. The vertex stage's cbuf 0 at 0xb0 is a bare pointer of 16 bytes. */
#define SH_GBUF_BANK(d)   ((d) >> 16)
#define SH_GBUF_OFFSET(d) ((d) & 0xFFFFu)
static inline int shader_gbuf_sized(int stage, u32 d) { return d >= 0x110 && d < (stage == ST_CS ? 0x410u : 0x830u); }
/* Transform feedback (key of the last program before rasterization): for each of the 4 buffers, the attribute word
   each 4-byte word of a vertex there holds (its ISA attribute address / 4: the position's x is 28, generic i's
   component k 32 + 4 * i + k), of count words, the vertices stride bytes apart. All counts 0: nothing is captured */
typedef struct {
    u8 layout[4][128];
    u8 count[4];
    u16 stride[4];
} ShXfb;
/* A program's key state (set by the renderer before translating) and what translation found. The corpus stores it
   whole, so a corpus file only loads into a build where this struct has the same size. */
typedef struct {
    int stage;
    u32 sph[20];      /* the shader program header, 80 bytes */
    u32 cbuf_mask;    /* constant buffers read */
    u32 cbuf_max[18]; /* bytes read of each, so only those are bound */
    ShTex tex[SH_MAXTEX];
    int ntex;
    u32 in_mask;       /* generic attributes 0-31 read */
    u32 attr_int_mask; /* key: vertex attributes of integer formats, and of those the signed ones */
    u32 attr_sint_mask;
    u32 clip_enable; /* key: the clip distances the 3D engine enables (SetUserClipEnable), the only ones written, and
                        SNX_SH_NOT_LAST */
    int uses_position_in, uses_clip_mask, writes_layer, writes_point_size;
    /* FP32 arithmetic asking to flush denormals (FTZ/FMZ), and FP32 arithmetic that doesn't. Only flush in every
       FADD, FMUL and FFMA lets the module declare flushing, as Eden declares DenormFlushToZero */
    int fp32_flush, fp32_preserve;
    u32 gbuf_cbuf_off[16]; /* the global buffers found, by where a constant buffer holds their address (SH_GBUF_*) */
    int ngbuf;
    int unsupported; /* instructions or forms translation refused */
    char first_unsupported[96]; /* the longest: an untracked global memory base with its writer */
    u32 cs_local[3];
    int shared_size;
    u32 gbuf_wmask; /* global buffers the program writes */
    struct {
        u32 cbuf_off;
        u8 type, bindless, bl_cbuf, kind; /* kind: 0 float store, 1 R32ui load/atomic, 2 unsigned store, 3 signed store */
    } img[8];
    int nimg; /* storage images stored to or used atomically */
    /* FS key: textures of integer formats and their signed ones. rt_int_mask and rt_sint_mask are FS's integer
       render targets; for TES rt_int_mask holds the tessellation parameters (register 0xC8), for GS the input
       primitive's vertex count */
    u64 tex_int_mask, tex_sint_mask;
    u32 rt_int_mask, rt_sint_mask; /* FS/CS bits 8-15 also key storage image stores' integer and signed types */
    u32 gp_skip_mask[8]; /* GS key: POST_VTG_SHADER_ATTRIB_SKIP_MASK, a bit per scalar attribute */
    /* FS key: color textures sampled with a depth compare (TEX.DC compares their first component, which a Vulkan
       compare sampler can't do on a color format): the slots, and their compare functions, 3 bits each in slot
       order */
    u64 tex_dc_color_mask;
    u32 tex_dc_color_funcs;
    u32 out_mask; /* generic outputs 0-31 written (all of them after an attribute store at a register's offset) */
    /* FS key: generic inputs the last stage before rasterization doesn't write. They read 0, 0, 0, 1 as yuzu reads
       them (GPL-2.0-or-later, shader_recompiler emit_spirv_context_get_set.cpp): Vulkan leaves an unfed input
       undefined */
    u32 in_unfed_mask;
    /* FS key: MANDATED_EARLY_Z (0x084) on, so the depth and stencil tests run before the program, as Eden's
       force_early_z has them: a program that stores to images or buffers then leaves no trace for fragments they
       reject */
    u32 early_fragment_tests;
    ShXfb xfb; /* key: transform feedback's capture of this program's outputs */
    int writes_viewport; /* the last vertex stage selects one of the 16 viewport/scissor pairs */
} ShaderInfo;
#define SH_DC_COLOR_MAX  10 /* slots tex_dc_color_funcs has room for */
/* Each stage has a descriptor set (shader_vk_set), its bindings grouped by kind. */
#define SNX_VKB_CB       0                         /* 18 constant buffers */
#define SNX_VKB_SSBO     18                        /* 16 global memory buffers */
#define SNX_VKB_TEX      34                        /* SH_MAXTEX combined image samplers */
#define SNX_VKB_IMG      (SNX_VKB_TEX + SH_MAXTEX) /* 8 storage images (compute, vertex, fragment) */
#define SNX_VKB_RS       (SNX_VKB_IMG + 8)         /* fragment: 1 / the resolution scale (SNX_RS) */
#define SNX_VKB_FMT      (SNX_VKB_RS + 1)          /* compute: the images' format classes (SNX_FMT) */
#define SNX_VKB_SMAP     (SNX_VKB_FMT + 1)         /* graphics: storage buffer page indices for aliased bindings */
#define SNX_VKB_IMGBUF   (SNX_VKB_SMAP + 1)        /* 8 storage texel buffers, surface access to buffer images (compute) */
#define SNX_VKB_TEXARRAY (SNX_VKB_IMGBUF + 8)      /* up to SNX_TEX_ARRAY distinct indexed handles */
#define SNX_VKB_TEXFUNCS (SNX_VKB_TEXARRAY + 1)    /* their depth compare functions */
#define SNX_VKB_TEXDEPTH (SNX_VKB_TEXFUNCS + 1)    /* their depth views */
#define SNX_VKB_TEXMAP   (SNX_VKB_TEXDEPTH + 1)    /* table and element to compact descriptor index */
#define SNX_VKB_END      (SNX_VKB_TEXMAP + 1)      /* one past the last binding */
/* An indexed texture loads its handle with an LDC whose address has a register in it: one of SNX_TEX_ARRAY handles
   from cbuf_off on, each (1 << shift) bytes after the last, with the shift in bits 4-7 of ShTex.flags. */
#define SNX_TEX_ARRAY    256
#define SNX_TEX_INDEXED  4
/* Device capabilities the renderer sets before translating: */
/* sampled image arrays indexed per invocation (non-uniform indexing) */
extern int g_shader_texture_arrays;
/* tessellation and geometry programs may write the point size (shaderTessellationAndGeometryPointSize) */
extern int g_shader_stage_point_size;
/* vertex and tessellation programs may write the layer (shaderOutputLayer, the ShaderLayer capability); without it
   their layer writes go away (layer 0). 1 offline */
extern int g_shader_output_layer;
/* ViewportIndex output: bit 0 vertex/evaluation, bit 1 geometry. 3 offline. */
extern int g_shader_viewport_index;
/* the Geometry capability, for fragment programs reading Layer or PrimitiveId */
extern int g_shader_geometry;
/* shaderFloat16, which fp16 texture results need. 1 offline */
extern int g_shader_float16;
/* the Tessellation capability, which also gives a fragment program PrimitiveId without Geometry */
extern int g_shader_tessellation;
/* VK_KHR_shader_clock's scopes: bit 0 subgroup, bit 1 device */
extern int g_shader_clock_scopes;
/* VK_KHR_shader_maximal_reconvergence: a structured program's lanes reconverge where its constructs merge, so one
   whose lanes must reconverge (subgroup operations, barriers, derivatives) takes structured control flow too */
extern int g_shader_maximal_reconvergence;
/* in clip_enable: a later stage before rasterization (tessellation or geometry) follows, and the last one applies
   the depth mode and the clip distances */
#define SNX_SH_NOT_LAST (1u << 31)
/* the descriptor set of a stage's bindings */
int shader_vk_set(int stage);
/* can_flush: how the module gets the console's FP32 flush to zero (FTZ on FADD, FMUL and FFMA). SH_FTZ_EMULATE: a
   test and a select on each such operand and result; SH_FTZ_MODE: the DenormFlushToZero execution mode, in a module
   whose every such instruction flushes; SH_FTZ_NATIVE: neither, on a device that flushes them by itself (vk/ftz_probe.c) */
#define SH_FTZ_EMULATE 0
#define SH_FTZ_MODE    1
#define SH_FTZ_NATIVE  2
/* Translates a program (code, readable up to max_bytes) to SPIR-V: malloc'd words, their count in word_count. NULL
   when the program uses something the translator can't express, with si->unsupported and the reason set. */
u32* shader_to_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* word_count, int can_flush,
                     int can_preserve, int can_preserve_f64, int can_subgroup);
u32* shader_compute_to_spirv(const u8* code, u32 max_bytes, ShaderInfo* si, size_t* word_count, int can_flush,
                             int can_preserve);
/* reads word off (bytes) of constant buffer idx for jump tables, into *out as a host number: 1 when it could. Set by
   whoever translates, NULL offline */
extern int (*g_shader_cb_read)(u32 idx, u32 off, u32* out);
/* the targets of the BRX at word index from its jump table, at most max_targets, or -1 */
int shader_brx_targets(const u64* words, u32 index, u32 count, u32* targets, int max_targets);
/* the program's hash, and its length in bytes in out_len */
u64 shader_hash(const u8* code, u32 max_bytes, u32* out_len);
/* The shader corpus (shader_corpus=dir): each translated program's code and ShaderInfo, so tools/snx_shaderc.c
   can translate it again offline. A file only loads into a build with the same ShaderInfo. */
int shader_corpus_save(const char* dir, const u8* code, u32 len, u32 max_bytes, int stage, const ShaderInfo* si,
                       u64 key);
/* malloc'd code, the capture's max_bytes long with zeros past its recorded length, which goes in *max_bytes */
u8* shader_corpus_load(const char* path, u32* max_bytes, int* stage, ShaderInfo* si);
/* VertexA and VertexB as one program, A first; 0 when it doesn't fit */
int shader_merge_dual(const u8* a, const u8* b, u8* out, u32 max_bytes);
#ifdef __cplusplus
}
#endif
#endif
