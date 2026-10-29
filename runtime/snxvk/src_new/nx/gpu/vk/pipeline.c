/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Shaders and pipelines. A guest shader is translated directly to SPIR-V and kept as a module.
   Every stage has a descriptor set of the same bindings, one set per stage
   (shader_vk_set). Pipelines are made per PipeKey: the shaders, vertex layout, attachment formats and blend state;
   everything else is dynamic state. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/shader/shader.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/pipeline_file.h"
#include "nx/gpu/vk/internal_spv.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/shader/shipped.h"
#include "nx/gpu/shipped_pass.h"
#include "nx/runtime/platform/tracy.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include <sys/stat.h>

/* the bytes of a program vk_shader_get reads, from its SPH. The SPH has no size, so shader_hash ends the program at
   its EXIT or the zero padding after it, and NVN packs programs into one pool: the window has to reach past the
   largest program's end (FINAL FANTASY RESONANCE has one of 70,728 bytes), without going on into the next program */
#define SH_CODE 0x20000u

/* The caches live in shader_cache/ of the game's cache folder (g_cachedir), wherever the runtime was started from */
static char g_cache_dir[1100], g_pcache_path[1200], g_pcache_tmp[1230];
static int g_pcache_dirty; /* pipelines made since the cache was last written (atomic: it is saved outside vk_lock) */
static u64 g_pcache_used;  /* plat_ns when a pipeline last went into the cache (atomic) */
static void pcache_note(void) {
    __atomic_store_n(&g_pcache_used, plat_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&g_pcache_dirty, 1, __ATOMIC_RELAXED);
}
#if defined(SNX_TRACY)
/* the stages' names, for the shader translate zone's text */
static const char* const vk_tracy_stage[6] = {"vert", "tess ctrl", "tess eval", "geom", "frag", "comp"};
#endif

VkPipelineLayout g_vk_playout;
VkDescriptorSetLayout g_vk_dslayout[2][5];
static VkPipelineLayout g_playouts[1u << 5]; /* by the sets with texture arrays, made when first used */
static PMutex g_playout_lock = PMUTEX_INIT;  /* making one: the file's reading thread asks for them too */
u32 g_vk_stage_sets;
static VkPipelineCache g_pcache;
int g_vk_ftz; /* SH_FTZ_*: how translations flush FP32 subnormals (vk_pipeline_init) */
static void pf_start_load(void);
static void pf_merge(void);
static void pf_shader_record(const VkSh* s, const u32* spv, size_t nw, const u8* code, u32 ncode, const ShXfb* xfb);
static u32 src_bytes(u32 mlen);
static void pf_ship_found(VkSh* s);
static VkSh* pf_ship_take(u64 h, int stage, const u32 mask[20], u32 len, u64 qh, u32 len_a, u64 qh_a);
/* the channel and stage whose bound constant buffers the translator reads, for the vk_shader_get call running */
static const GpuChannel* g_sh_ch;
static int g_sh_bind;

static int sh_cb_read(u32 idx, u32 off, u32* out) {
    if (idx >= 18) return 0;
    u64 a = g_sh_ch->cbind[g_sh_bind][idx];
    u32 sz = g_sh_ch->cbind_size[g_sh_bind][idx];
    u32 limit = sz ? sz : 65536;
    if (!a || limit < 4 || off > limit - 4) return 0;
    *out = gmem_r32(a + off);
    return 1;
}
static VkCbReader g_rec_read;
static VkCbReads* g_rec;
static int g_rec_failed;
static int rec_read(u32 idx, u32 off, u32* out) {
    int ok = g_rec_read(idx, off, out);
    VkCbReads* reads = g_rec;
    if (reads->n % 16 == 0) {
        VkCbRead* grown = realloc(reads->r, sizeof *grown * (reads->n + 16));
        if (!grown) {
            g_rec_failed = 1;
            return ok;
        }
        reads->r = grown;
    }
    reads->r[reads->n++] = (VkCbRead){idx, off, (u32)ok, ok ? *out : 0};
    return ok;
}
const char* vk_corpus_dir(void) {
    static char dir[sizeof g_exedir + sizeof "captures/shaders"];
    static const char* at = (const char*)-1;
    if (at != (const char*)-1) return at;
    at = g_settings.shader_corpus[0] ? g_settings.shader_corpus : NULL;
    if (!at && g_settings.shader_captures) { /* a phone's game has no environment to set */
        snprintf(dir, sizeof dir, "%scaptures", g_exedir);
        plat_mkdir(dir);
        snprintf(dir, sizeof dir, "%scaptures/shaders", g_exedir);
        plat_mkdir(dir);
        at = dir;
    }
    return at;
}

const char* vk_spirv_unavailable(const u32* spv, size_t words) {
    for (size_t at = 5; at < words;) { /* the capabilities come first */
        u32 width = spv[at] >> 16;
        if (!width || at + width > words || (spv[at] & 0xffffu) != 17) break;
        if (width == 2 && spv[at + 1] == 10 && !g_vk.feat.shaderFloat64) return "shaderFloat64 is unavailable";
        if (width == 2 && spv[at + 1] == 11 && !g_vk.feat.shaderInt64) return "shaderInt64 is unavailable";
        if (width == 2 && spv[at + 1] == 9 && !g_shader_float16) return "shaderFloat16 is unavailable";
        if (width == 2 && spv[at + 1] == 2 && !g_vk.feat.geometryShader) return "geometryShader is unavailable";
        if (width == 2 && spv[at + 1] == 3 && !g_vk.feat.tessellationShader) return "tessellationShader is unavailable";
        if (width == 2 && spv[at + 1] == 25 && !g_vk.feat.shaderImageGatherExtended)
            return "shaderImageGatherExtended is unavailable";
        if (width == 2 && spv[at + 1] == 32 && !g_vk.feat.shaderClipDistance)
            return "shaderClipDistance is unavailable";
        if (width == 2 && spv[at + 1] == 35 && !g_vk.feat.sampleRateShading) return "sampleRateShading is unavailable";
        if (width == 2 && spv[at + 1] == 4427 && !g_vk.has_shader_draw_parameters)
            return "shaderDrawParameters is unavailable";
        if (width == 2 && spv[at + 1] == 5055 && !g_shader_clock_scopes) return "shaderClock is unavailable";
        at += width;
    }
    return NULL;
}

/* The translator's reader (g_shader_cb_read) and what rec_read records into are one for the process: the GPU thread's
   draws and dispatches and the pass over the programs the game ships, on the shader files' thread (ship_program),
   translate and hash one at a time */
static PMutex g_xlate_lock = PMUTEX_INIT;
u32* vk_translate_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* words, VkCbReader read,
                        VkCbReads* reads) {
    pm_lock(&g_xlate_lock);
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_rec_read = read;
    g_rec = reads;
    g_rec_failed = 0;
    g_shader_cb_read = rec_read;
    const VkShaderStageFlagBits stages[] = {VK_SHADER_STAGE_VERTEX_BIT,
                                            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                            VK_SHADER_STAGE_GEOMETRY_BIT,
                                            VK_SHADER_STAGE_FRAGMENT_BIT,
                                            VK_SHADER_STAGE_COMPUTE_BIT};
    VkShaderStageFlagBits shader_stage = stages[stage];
    u32* result = shader_to_spirv(code, max_bytes, stage, si, words, g_vk_ftz, g_vk_preserve_infnan,
                                  g_vk_preserve_infnan64, (g_vk_subgroup_stages & shader_stage) != 0);
    g_shader_cb_read = old;
    g_rec = NULL;
    int failed = g_rec_failed;
    pm_unlock(&g_xlate_lock);
    if (failed) {
        free(result);
        return NULL;
    }
    if (result) {
        const char* reason = vk_spirv_unavailable(result, *words);
        /* Texture uploads and render targets are single-sampled. Reject a shader whose descriptor needs MS before
           the draw reaches vk_stage_set's null descriptor path. */
        for (int i = 0; i < si->ntex && !reason; i++)
            if (si->tex[i].flags & 2) reason = "multisample texture binding unavailable";
        if (reason) {
            snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s", reason);
            si->unsupported = 1;
            free(result);
            return NULL;
        }
    }
    return result;
}

/* shader_hash with the jump tables of BRX read through read, as the translation reads them: without a reader the
   length stops at the first EXIT and leaves out code only a jump table reaches */
u64 vk_shader_hash(const u8* code, u32 max_bytes, u32* out_len, VkCbReader read) {
    pm_lock(&g_xlate_lock);
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_shader_cb_read = read;
    u64 h = shader_hash(code, max_bytes, out_len);
    g_shader_cb_read = old;
    pm_unlock(&g_xlate_lock);
    return h;
}
/* VertexA a and VertexB b (SH_CODE bytes each) merged into m, sizing them with shader_hash through read as the
   translation would: 1 when merged */
static int sh_merge_dual(const u8* a, const u8* b, u8* m, VkCbReader read) {
    pm_lock(&g_xlate_lock);
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_shader_cb_read = read;
    int merged = shader_merge_dual(a, b, m, SH_CODE);
    g_shader_cb_read = old;
    pm_unlock(&g_xlate_lock);
    return merged;
}
int vk_cb_reads_hold(const VkCbReads* reads, VkCbReader read) {
    for (u32 i = 0; i < reads->n; i++) {
        const VkCbRead* c = &reads->r[i];
        u32 v = 0;
        int ok = read(c->idx, c->off, &v);
        if ((u32)ok != c->ok || (ok && v != c->v)) return 0;
    }
    return 1;
}

u32 g_vk_buffer_image_slots;

/* descriptors of each type in one set: the binding ranges in shader.h */
static VkDescriptorType binding_type(u32 i) {
    if (i == SNX_VKB_SMAP) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (i == SNX_VKB_TEXARRAY || i == SNX_VKB_TEXDEPTH) return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if (i == SNX_VKB_TEXFUNCS || i == SNX_VKB_TEXMAP) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (i >= SNX_VKB_IMGBUF) return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    return i < SNX_VKB_SSBO  ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
           : i < SNX_VKB_TEX ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
           : i < SNX_VKB_IMG ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
           : i < SNX_VKB_RS  ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                             : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}
void vk_pool_sizes(VkDescriptorPoolSize* sz, u32 sets) {
    static const VkDescriptorType ty[VK_POOL_TYPES] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    int k;
    for (k = 0; k < VK_POOL_TYPES; k++) {
        u32 n = 0;
        u32 i;
        for (i = 0; i < SNX_VKB_END; i++)
            if (binding_type(i) == ty[k]) n += (i == SNX_VKB_TEXARRAY || i == SNX_VKB_TEXDEPTH) ? SNX_TEX_ARRAY : 1;
        sz[k] = (VkDescriptorPoolSize){ty[k], n * sets};
    }
}

VkPipelineLayout vk_playout(u32 arrays) {
    VkPipelineLayout* l = &g_playouts[arrays & ((1u << 5) - 1)];
    VkPipelineLayout made = __atomic_load_n(l, __ATOMIC_ACQUIRE);
    if (made) return made;
    pm_lock(&g_playout_lock);
    if (!*l) {
        VkDescriptorSetLayout sets[5];
        for (u32 s = 0; s < g_vk_stage_sets; s++) sets[s] = g_vk_dslayout[(arrays >> s) & 1][s];
        /* push constants: viewport/depth pc[0..3], fragment alpha test pc[4..6], sprite T flip pc[7], scaled textures
           pc[8..9] */
        VkPushConstantRange pc = {VK_PUSH_STAGES, 0, 48};
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = g_vk_stage_sets,
                                         .pSetLayouts = sets,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &pc};
        if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &made))) made = VK_NULL_HANDLE;
        __atomic_store_n(l, made, __ATOMIC_RELEASE);
    }
    made = *l;
    pm_unlock(&g_playout_lock);
    return made;
}

void vk_pipeline_init(void) {
    pm_lock_announce(&g_playout_lock, "pipeline layout");
    pm_lock_announce(&g_xlate_lock, "shader translate");
    /* a layout per set, the same bindings, each visible to the stage that uses the set (shader_vk_set): within the
       per-stage limits a device has, which all stages seeing every set would multiply by five */
    static const VkShaderStageFlags vis[5] = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                              VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                              VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                              VK_SHADER_STAGE_GEOMETRY_BIT};
    /* the tessellation and geometry stages' sets come after the first two, where the device binds that many */
    g_vk_stage_sets = g_vk.props.limits.maxBoundDescriptorSets >= 5 ? 5 : 2;
    for (int s = 0; s < 5; s++) {
        VkDescriptorSetLayoutBinding b[SNX_VKB_END];
        u32 count = 0;
        u32 image_slots = g_vk.props.limits.maxPerStageDescriptorStorageImages;
        u32 buffer_slots;
        u32 i;
        if (image_slots > g_vk.props.limits.maxDescriptorSetStorageImages)
            image_slots = g_vk.props.limits.maxDescriptorSetStorageImages;
        /* storage texel buffers count against the storage image limits, so buffer images get what images leave */
        buffer_slots = image_slots;
        if (image_slots > SNX_VKB_RS - SNX_VKB_IMG) image_slots = SNX_VKB_RS - SNX_VKB_IMG;
        buffer_slots -= image_slots;
        if (buffer_slots > SNX_VKB_TEXARRAY - SNX_VKB_IMGBUF) buffer_slots = SNX_VKB_TEXARRAY - SNX_VKB_IMGBUF;
        g_vk_buffer_image_slots = buffer_slots;
        /* A compact array holds the unique handles selected by all indexed tables. */
        if (g_vk.props.limits.maxPerStageDescriptorSamplers < SH_MAXTEX + 2 * SNX_TEX_ARRAY ||
            g_vk.props.limits.maxPerStageDescriptorSampledImages < SH_MAXTEX + 2 * SNX_TEX_ARRAY ||
            g_vk.props.limits.maxDescriptorSetSamplers < 5 * (SH_MAXTEX + 2 * SNX_TEX_ARRAY) ||
            g_vk.props.limits.maxDescriptorSetSampledImages < 5 * (SH_MAXTEX + 2 * SNX_TEX_ARRAY))
            g_shader_texture_arrays = 0;
        for (i = 0; i < SNX_VKB_END; i++) {
            int image = i >= SNX_VKB_IMG && i < SNX_VKB_RS;
            if (image && (s >= 2 || i - SNX_VKB_IMG >= image_slots)) continue; /* compute, vertex and fragment */
            if (i >= SNX_VKB_TEXARRAY && !g_shader_texture_arrays) continue;
            if (i >= SNX_VKB_IMGBUF && i < SNX_VKB_TEXARRAY && (s != 0 || i - SNX_VKB_IMGBUF >= buffer_slots)) continue;
            if (i >= SNX_VKB_IMGBUF && i < SNX_VKB_TEXARRAY) image = 1;
            b[count++] = (VkDescriptorSetLayoutBinding){
                .binding = i,
                .descriptorType = binding_type(i),
                .descriptorCount = (i == SNX_VKB_TEXARRAY || i == SNX_VKB_TEXDEPTH) ? SNX_TEX_ARRAY : 1,
                .stageFlags = !image               ? vis[s]
                              : s == 1             ? VK_SHADER_STAGE_FRAGMENT_BIT
                              : i < SNX_VKB_IMGBUF ? VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT
                                                   : VK_SHADER_STAGE_COMPUTE_BIT};
        }
        for (int arrays = 0; arrays < 2; arrays++) {
            u32 n = count;
            while (!arrays && n && b[n - 1].binding >= SNX_VKB_TEXARRAY) n--; /* the arrays are the last bindings */
            VkDescriptorSetLayoutCreateInfo dl = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b};
            if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_vk_dslayout[arrays][s])))
                fatal("vulkan: cannot create shader descriptor layout");
        }
    }
    {
        VkDescriptorPoolSize n[VK_POOL_TYPES];
        vk_pool_sizes(n, 1);
        const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
        if (n[0].descriptorCount > l->maxPerStageDescriptorUniformBuffers ||
            n[1].descriptorCount > l->maxPerStageDescriptorStorageBuffers ||
            n[2].descriptorCount > l->maxPerStageDescriptorSamplers ||
            n[2].descriptorCount > l->maxPerStageDescriptorSampledImages ||
            n[3].descriptorCount > l->maxPerStageDescriptorStorageImages ||
            n[0].descriptorCount + n[1].descriptorCount + n[2].descriptorCount + n[3].descriptorCount + 8 >
                l->maxPerStageResources)
            WARN("vulkan: a stage's descriptor set (%u uniform and %u storage buffers, %u textures, %u images) is "
                 "past this GPU's per-stage limits (%u, %u, %u, %u): pipelines may fail\n",
                 n[0].descriptorCount, n[1].descriptorCount, n[2].descriptorCount, n[3].descriptorCount,
                 l->maxPerStageDescriptorUniformBuffers, l->maxPerStageDescriptorStorageBuffers,
                 l->maxPerStageDescriptorSamplers, l->maxPerStageDescriptorStorageImages);
    }
    g_vk_playout = vk_playout(0);
    if (!g_vk_playout) fatal("vulkan: cannot create shader pipeline layout");
    snprintf(g_cache_dir, sizeof g_cache_dir, "%sshader_cache", g_cachedir);
    plat_mkdir(g_cache_dir);
    snprintf(g_pcache_path, sizeof g_pcache_path, "%s/vk_pipelines.bin", g_cache_dir);
    snx_path_native(g_pcache_path);
    /* the temporary name is the process's own: two games running at once would otherwise write the same file */
    snprintf(g_pcache_tmp, sizeof g_pcache_tmp, "%s.%u.tmp", g_pcache_path, pf_pid());
    /* the driver's compiled pipelines from earlier runs; it ignores data from another device or driver version */
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    PFileMap data;
    if (!plat_file_map(&data, g_pcache_path) && data.data) {
        ci.initialDataSize = (size_t)data.size;
        ci.pInitialData = data.data;
    }
    if (!VK_OK(vkCreatePipelineCache(g_vk.dev, &ci, NULL, &g_pcache)) && ci.initialDataSize) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        VK_OK(vkCreatePipelineCache(g_vk.dev, &ci, NULL, &g_pcache));
    }
    if (ci.initialDataSize) LOG("vulkan: pipeline cache loaded (%zu KB)\n", ci.initialDataSize >> 10);
    plat_file_unmap(&data); /* the cache copied it, and a mapped file can't be replaced on Windows */
    /* how shaders get the console's FP32 flush to zero, settled before the reading thread translates any */
    if (g_vk_flush_denorm)
        g_vk_ftz = SH_FTZ_MODE;
    else if (g_vk_preserve_infnan && vk_ftz_native()) {
        g_vk_ftz = SH_FTZ_NATIVE;
        LOG("vulkan: this GPU flushes FP32 subnormals as the console's FTZ instructions do, so shaders are translated "
            "without a flush of their own\n");
    } else {
        g_vk_ftz = SH_FTZ_EMULATE;
        WARN("vulkan: FP32 denormal flush mode unavailable; graphics shaders use per-instruction FTZ fallback\n");
    }
    pf_start_load();
}

/* ------------------------------------------------------------ shaders */

static VkSh* g_shh[1024];
/* the shader last found for an address and key, two per set. It is checked against memory once per submit: unchanged
   by write tracking since then, or else by the hash of its code */
typedef struct {
    u64 gva, gva_a, gw;
	u64 shader_generation;
    u32 mask[20], epoch, wseen;
    int stage;
    VkSh* s;
} ShA;
static ShA g_sha[4096];

static int code_hash(u64 gva, u32 len, u64* hash) {
    u8* p = gmem_ptr(gva, len);
    if (p) {
        *hash = vk_hash(p, len);
        return 1;
    }
    u8* t = malloc(len ? len : 1);
    if (!t) return 0;
    gmem_read(gva, t, len);
    *hash = vk_hash(t, len);
    free(t);
    return 1;
}
static int code_same(const ShA* ca, u64 gva, u32 len, u64 qh) {
    if (!gpu_mem_dirty_since(ca->gw, gva, len) && vk_written_since(gmem_ptr(gva, len), len, ca->wseen) == 0) return 1;
    u64 hash;
    return code_hash(gva, len, &hash) && hash == qh;
}

/* for the stats line: pipelines made, the GPU thread's time waiting for new ones, shaders translated and the time
   their translation and modules took (all on the GPU thread) */
static u64 g_pipes_new, g_sh_new, g_sh_ns;
static u64 g_pipe_wait_ns; /* the GPU thread's and the recording thread's waits, added atomically */
static u64 g_pipe_make_ns; /* the workers' time making pipelines, added atomically */
static u64 g_pipe_hits;    /* pipelines the pipeline cache had */
/* what a shader's key words (vk_shader_get's mask) tell its translation, into si zeroed */
static void sh_key_info(ShaderInfo* si, int stage, const u32 mask[20], const ShXfb* xfb) {
    u64 ints = mask[0] | (u64)mask[1] << 32, sints = mask[2] | (u64)mask[3] << 32;
    memset(si, 0, sizeof *si);
    if (stage == ST_FS) {
        si->tex_int_mask = ints;
        si->tex_sint_mask = sints;
        si->rt_int_mask = mask[4];
        si->rt_sint_mask = mask[5];
        si->tex_dc_color_mask = mask[7] | (u64)mask[8] << 32;
        si->tex_dc_color_funcs = mask[9];
        si->in_unfed_mask = mask[10];
        si->early_fragment_tests = mask[11];
    } else {
        si->attr_int_mask = ints;
        si->attr_sint_mask = sints;
        si->rt_int_mask = mask[4]; /* the tessellation or geometry primitive */
        si->clip_enable = mask[6];
        memcpy(si->gp_skip_mask, mask + 12, sizeof si->gp_skip_mask);
        if (xfb) si->xfb = *xfb;
    }
}
/* s for its key, from code (SH_CODE bytes: the merged program, for VertexA and VertexB) whose whole code hashes to h,
   each part with its length and hash, translated: its SPIR-V (malloc'd, nw words), the constant buffers the
   translation read through read in s->cbr. NULL when it can't be (si.first_unsupported says why). shipped: a program
   the game ships, translated before any draw (ship_make), which fails when its translation read a constant buffer: a
   draw's lookup checks those reads against its own buffers, which no read without them matches */
static u32* sh_spirv(VkSh* s, const u8* code, u64 h, int stage, const u32 mask[20], u32 len, u64 qh, u32 len_a,
                     u64 qh_a, const ShXfb* xfb, VkCbReader read, int shipped, size_t* nw) {
    s->shipped = shipped;
    s->hash = h;
    s->stage = stage;
    memcpy(s->mask, mask, sizeof s->mask);
    s->clen = len;
    s->qh = qh;
    s->clen_a = len_a;
    s->qh_a = qh_a;
    sh_key_info(&s->si, stage, mask, xfb);
    *nw = 0;
    SNX_ZONE(z_translate, "shader translate");
    SNX_ZONE_TEXT(z_translate, "%s hash %08x", stage < 6 ? vk_tracy_stage[stage] : "stage?", (unsigned)h);
    u32* spv = vk_translate_spirv(code, SH_CODE, stage, &s->si, nw, read, &s->cbr);
    SNX_ZONE_VALUE(z_translate, *nw);
    SNX_ZONE_END(z_translate);
    for (int i = 0; i < s->si.ntex; i++) s->indexed |= (s->si.tex[i].flags & SNX_TEX_INDEXED) != 0;
    if (spv && shipped && s->cbr.n) {
        free(spv);
        spv = NULL;
        snprintf(s->si.first_unsupported, sizeof s->si.first_unsupported, "a jump table in a constant buffer");
    }
    if (spv && s->si.nimg) {
        u32 image_slots = g_vk.props.limits.maxPerStageDescriptorStorageImages;
        if (image_slots > g_vk.props.limits.maxDescriptorSetStorageImages)
            image_slots = g_vk.props.limits.maxDescriptorSetStorageImages;
        int needs_unformatted = 0;
        for (int i = 0; i < s->si.nimg; i++) needs_unformatted |= s->si.img[i].kind != 1;
        /* a vertex program's stores need the device's vertexPipelineStoresAndAtomics */
        if ((stage != ST_FS && (stage != ST_VS || !g_vk.feat.vertexPipelineStoresAndAtomics)) ||
            (u32)s->si.nimg > image_slots || (needs_unformatted && !g_vk.feat.shaderStorageImageWriteWithoutFormat)) {
            free(spv);
            spv = NULL;
            snprintf(s->si.first_unsupported, sizeof s->si.first_unsupported,
                     "storage image stage, feature or descriptor limit");
        }
    }
    return spv;
}
/* A draw's shader (sh_spirv's arguments, and mlen, the bytes of code the translation reads): translated, its module
   made, kept in the shader file with its program. One that couldn't be translated comes back failed; NULL when out
   of memory */
static VkSh* sh_make(const u8* code, u32 mlen, u64 h, int stage, const u32 mask[20], u32 len, u64 qh, u32 len_a,
                     u64 qh_a, const ShXfb* xfb, VkCbReader read) {
    size_t nw;
    VkSh* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    SNX_ZONE(z, "shader translate");
    u32* spv = sh_spirv(s, code, h, stage, mask, len, qh, len_a, qh_a, xfb, read, 0, &nw);
    SNX_ZONE_END(z);
    if (spv) {
        VkShaderModuleCreateInfo mi = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
        if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &s->mod))) {
            s->failed = 1;
            snprintf(s->si.first_unsupported, sizeof s->si.first_unsupported, "vkCreateShaderModule failed");
        } else
            pf_shader_record(s, spv, nw, code, src_bytes(mlen), xfb);
        free(spv);
    } else
        s->failed = 1;
    return s;
}
VkSh* vk_shader_get(const GpuChannel* ch, u64 gva, u64 gva_a, int stage, u64 int_mask, u64 sint_mask, u32 rt_int,
                    u32 rt_sint, u32 clip, u64 dc_color, u32 dc_funcs, u32 unfed, const ShXfb* xfb) {
    pf_merge();
    g_sh_ch = ch;
    g_sh_bind = stage <= ST_FS ? stage : 0;
    u32 mask[20] = {(u32)int_mask,
                    (u32)(int_mask >> 32),
                    (u32)sint_mask,
                    (u32)(sint_mask >> 32),
                    rt_int,
                    rt_sint,
                    clip,
                    (u32)dc_color,
                    (u32)(dc_color >> 32),
                    dc_funcs,
                    unfed};
    if (stage == ST_FS && ch) mask[11] = ch->r3d[0x084] & 1; /* MANDATED_EARLY_Z */
    if (stage == ST_GS && ch) memcpy(mask + 12, ch->r3d + 0x490, 8 * sizeof(u32));
    /* a capture's layout, in the words only the fragment stage's key uses otherwise */
    if (stage != ST_FS && xfb) {
        u64 xh = vk_hash(xfb, sizeof *xfb);
        mask[7] = (u32)xh | 1;
        mask[8] = (u32)(xh >> 32);
    }
    u32 mix = (u32)vk_hash(mask, 12 * sizeof(u32));
    if (stage == ST_GS) mix ^= (u32)vk_hash(mask + 12, 8 * sizeof(u32));
    /* the set from every bit of the key, mixed by multiplying: programs are 256-byte aligned, and a shift and xor of
       their addresses left most sets unused */
    u64 hk = (gva * 0x9E3779B97F4A7C15ull) ^ (gva_a * 0xC2B2AE3D27D4EB4Full) ^ ((u64)mix << 24) ^ (u64)stage;
    ShA* set = &g_sha[(u32)((hk * 0x9E3779B97F4A7C15ull) >> 52) & 4094];
    ShA* ca = NULL;
    for (int w = 0; w < 2 && !ca; w++)
        if (set[w].s && set[w].gva == gva && set[w].gva_a == gva_a && set[w].stage == stage &&
            !memcmp(set[w].mask, mask, sizeof mask))
            ca = &set[w];
    if (ca) {
        VkSh* s = ca->s;
	int same = (ca->epoch == g_vk_epoch && ca->shader_generation == g_vk.shader_generation) ||
		(code_same(ca, gva, s->clen, s->qh) &&
		 (!gva_a || code_same(ca, gva_a, s->clen_a, s->qh_a)));
        if (same && (ca->epoch != g_vk_epoch || ca->shader_generation != g_vk.shader_generation)) {
            ca->shader_generation = g_vk.shader_generation;
            ca->epoch = g_vk_epoch;
            ca->wseen = vk_ww_now();
            ca->gw = gpu_mem_generation();
        }
        if (same && vk_cb_reads_hold(&s->cbr, sh_cb_read)) return s;
    } else /* the way used longest ago */
        ca = !set[0].s || (set[1].s && (i32)(set[0].epoch - set[1].epoch) <= 0) ? &set[0] : &set[1];
    u32 wseen = vk_ww_now();
    u64 gw = gpu_mem_generation();
    u8* code = malloc(SH_CODE);
    if (!code) return NULL;
    gmem_read_code(gva, code, SH_CODE);
    u32 len, len_a = 0;
    u64 qh, qh_a = 0;
    vk_shader_hash(code, SH_CODE, &len, sh_cb_read);
    qh = vk_hash(code, len);
    if (gva_a) { /* VertexA + VertexB: one merged program */
        u8* a = malloc(SH_CODE);
        if (!a) {
            free(code);
            return NULL;
        }
        gmem_read_code(gva_a, a, SH_CODE);
        vk_shader_hash(a, SH_CODE, &len_a, sh_cb_read);
        qh_a = vk_hash(a, len_a);
        u8* m = malloc(SH_CODE);
        if (!m) {
            free(a);
            free(code);
            return NULL;
        }
        if (sh_merge_dual(a, code, m, sh_cb_read)) {
            free(code);
            code = m;
        } else {
            free(m);
            WARN_LIMIT(8, "vulkan: VertexA %llx and VertexB %llx couldn't be merged: VertexB runs alone\n",
                       (unsigned long long)gva_a, (unsigned long long)gva);
        }
        free(a);
    }
    u32 mlen;
    u64 h = vk_shader_hash(code, SH_CODE, &mlen, sh_cb_read);
    VkSh* s = NULL;
    /* one made for the same program: its whole code hashes the same (the merged program's, and each part's with its
       length, which ShA's checks compare memory with later), with the same key and constant buffer reads. Shaders of
       earlier runs (pf_merge) are found only so */
    for (VkSh* q = g_shh[h & 1023]; q && !s; q = q->next)
        if (q->hash == h && q->stage == stage && !memcmp(q->mask, mask, sizeof mask) && q->clen == len &&
            q->qh == qh && q->clen_a == len_a && q->qh_a == qh_a && vk_cb_reads_hold(&q->cbr, sh_cb_read))
            s = q;
    if (s && s->shipped == 1) pf_ship_found(s);
    if (!s && (s = pf_ship_take(h, stage, mask, len, qh, len_a, qh_a)) != NULL) {
        s->next = g_shh[h & 1023];
        g_shh[h & 1023] = s;
    }
    if (!s) {
        if (vk_corpus_dir()) { /* the key's words as sh_make takes them into si, which the corpus keeps */
            ShaderInfo si;
            sh_key_info(&si, stage, mask, xfb);
            shader_corpus_save(vk_corpus_dir(), code, mlen, SH_CODE, stage, &si, h ^ mix);
        }
        u64 t0 = plat_ns();
        s = sh_make(code, mlen, h, stage, mask, len, qh, len_a, qh_a, xfb, sh_cb_read);
        if (!s) {
            free(code);
            return NULL;
        }
        if (s->failed)
            WARN("vulkan: shader %016llx (stage %d) not translated: %s\n", (unsigned long long)h, stage,
                 s->si.first_unsupported[0] ? s->si.first_unsupported : "unsupported feature or float controls");
        g_sh_new++;
        g_sh_ns += plat_ns() - t0;
        s->next = g_shh[h & 1023];
        g_shh[h & 1023] = s;
    }
    free(code);
    ca->gva = gva;
    ca->gva_a = gva_a;
    memcpy(ca->mask, mask, sizeof mask);
    ca->stage = stage;
    ca->epoch = g_vk_epoch;
    ca->wseen = wseen;
    ca->gw = gw;
	ca->shader_generation = g_vk.shader_generation;
    ca->s = s;
    return s;
}

/* whether the constant buffer words s's translation read (BRX jump tables) still hold in ch's bound buffers, as
   vk_shader_get checks them on each hit */
int vk_shader_cb_hold(const GpuChannel* ch, const VkSh* s) {
    g_sh_ch = ch;
    g_sh_bind = s->stage <= ST_FS ? s->stage : 0;
    return vk_cb_reads_hold(&s->cbr, sh_cb_read);
}

VkShaderModule vk_module_spv(const u32* spv, size_t nw) {
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m));
    return m;
}
VkShaderModule vk_module_spv_location(const u32* source, size_t nw, u32 location) {
    u32* spv = malloc(nw * sizeof *spv);
    if (!spv) return VK_NULL_HANDLE;
    memcpy(spv, source, nw * sizeof *spv);
    int found = 0;
    for (size_t i = 5; i < nw;) {
        u32 count = spv[i] >> 16, op = spv[i] & 0xffffu;
        if (!count || i + count > nw) break;
        if (op == 71 && count == 4 && spv[i + 2] == 30) {
            spv[i + 3] = location;
            found++;
        }
        i += count;
    }
    VkShaderModule result = found == 1 ? vk_module_spv(spv, nw) : VK_NULL_HANDLE;
    free(spv);
    return result;
}
/* an empty fragment shader for depth-only passes */
static VkShaderModule null_fs(void) {
    static VkShaderModule m;
    if (!m) m = vk_module_spv(snx_null_fs_spv, sizeof snx_null_fs_spv / sizeof *snx_null_fs_spv);
    return m;
}

/* ------------------------------------------------------------ pipelines */
/* A draw's new pipeline is made on the worker threads (pipeline_threads, vk_pipeline_worker_count): the GPU thread hands
   the recording thread a stand-in for it and decodes on, and the recording thread waits for the pipeline at its bind
   (vk_pipeline_resolve; without that thread, vk_record=0, the draw waits on the GPU thread). The shader file's
   pipelines (pf_merge) are made on the workers from startup, and a draw that asks for one before then waits for it
   too, at the head of the workers' queue. Skipping a draw whose pipeline was still being made, as Eden's asynchronous
   shaders do, left
   one game's menus over a black backdrop and its loading screen without its map whenever their one draw was
   skipped, and failed 190 of edenprobe's cases: a draw is never skipped for its pipeline. What a worker needs that is
   made lazily, the layout and the empty fragment shader, is made on the GPU thread first */
typedef struct Pipe {
    PipeKey k;
    u64 h;
    VkPipeline p;      /* once ready */
    int ready;         /* set by the worker after p, with release order */
    VkPipelineLayout layout;
    VkShaderModule fs; /* k.fs, or the empty fragment shader */
    int prewarm;       /* one of the file's, made before a draw asked for it */
    int queued;        /* in the workers' queue (g_pj_lock): 1 a draw's, 2 the file's */
    u32 standin;       /* its index + 1 in the stand-ins' table (vk_pipeline_get_deferred), 0 for none yet */
    struct Pipe *next, *job;
} Pipe;
#define PIPEH 4096
static Pipe* g_pipeh[PIPEH];

/* The shaders and pipelines of earlier runs (pipeline_file.c), so that each run doesn't translate the same shaders
   and make the same pipelines again as views come into sight: one game made 440 pipelines and 500 shaders
   every session, each pipeline 3.4 ms on NVIDIA even from the driver's cache and 125 ms without, its draws skipped
   meanwhile. A thread of its own reads the file at startup, makes the modules and queues the pipelines to the workers
   behind any draw's; the first draw puts what it made in the tables (pf_merge), where vk_shader_get finds a shader
   only as it finds one made this run, by its code hashed in guest memory, its key and its constant buffer reads */
/* a shader of the shipped file: its key's words, the clip word of the key the pass gave it (ship_clip), and where its
   record starts (at 0: a free slot) */
typedef struct {
    u64 hash, qh, qh_a, at;
    u32 clen, clen_a, stage, clip;
} ShipIdx;
static struct {
    PFile file;
    char path[1400];
    int on;          /* the files are used (not when shaders are recorded, which wants every one translated) */
    PMutex lock;     /* loaded, and done's */
    PCond done;
    int loaded;      /* the reading thread is done with the main file (lock) */
    int merged;      /* what it made is in g_shh and g_pipeh (vk_lock) */
    VkSh *sh, *sh_tail; /* its shaders, in the file's order, linked by next until merged */
    Pipe* pipes;        /* its pipelines, linked by next until merged */
    u32 nsh, npipe;
    /* The shader programs the game ships (pf_ship_pass), translated before a draw asks for them, in a file of their
       own (ship, <game>.vkshipped) that is read as they are used: a game ships thousands, and a session uses a few of
       them. Its PF_SCAN records say what the pass read. The reading thread alone writes the index and the scans' set,
       and the draws read the index once ship_ready says it no longer changes */
    PFile ship;
    char ship_path[1400];
    /* the files the pass found nothing in (SHIP_NONE), kept across builds and drivers in a file of their own (scan,
       <game>.vkscan) under the walk's version, which an update of either leaves: the ship file's are started over */
    PFile scan;
    char scan_path[1400];
    ShipIdx* idx; /* its shaders by hash, open addressing (icap a power of two) */
    u32 icap, nidx;
    u64* scans; /* the PF_SCAN records, scan_key's, open addressing (scap a power of two) */
    u32 scap, nscans;
    PMutex note_lock; /* the pass's lines, for the screen: the latest, round */
    char note[RENDERER_NOTES][RENDERER_NOTE_SIZE];
    u32 nnote; /* the lines so far */
    /* atomics, written by the reading thread */
    ShipProgress progress;
    u32 ship_new;                   /* the pass's new shaders */
    int scanning;                   /* the pass runs */
    int ship_ready;                 /* the pass is over, or won't run: the draws may read the index (release) */
    u32 shaders_total, pipes_total; /* nsh and npipe once the file was read */
    char apart[64];
    /* atomics, written by the GPU thread and the pipeline workers */
    u64 made, hits; /* the file's pipelines made, and those the pipeline cache had */
    u32 ship_used;  /* the shipped file's shaders a draw found */
    u32 ship_said;  /* ship_used as vk_pipeline_save last logged it (g_psave) */
} g_pf;
static void pf_pipe_record(const PipeKey* k);

static VkPipeline pipe_create(const Pipe* e) {
    const PipeKey* k = &e->k;
    VkPipelineShaderStageCreateInfo st[5];
    VkShaderModule modules[5];
    static const VkShaderStageFlagBits bits[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                                  VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkPipelineTessellationStateCreateInfo ts;
    u32 nst = 0, s;
    modules[0] = k->vs;
    modules[1] = k->tcs;
    modules[2] = k->tes;
    modules[3] = k->gs;
    modules[4] = e->fs;
    for (s = 0; s < 5; s++) {
        if (!modules[s]) continue;
        memset(&st[nst], 0, sizeof st[nst]);
        st[nst].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[nst].stage = bits[s];
        st[nst].module = modules[s];
        st[nst].pName = "main";
        nst++;
    }
    memset(&ts, 0, sizeof ts);
    ts.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    ts.patchControlPoints = k->patch;
    VkVertexInputBindingDescription vb[33];
    VkVertexInputBindingDivisorDescriptionEXT dv[33];
    u32 ndv = 0;
    for (u32 i = 0; i < k->nbind; i++) {
        vb[i] = (VkVertexInputBindingDescription){.binding = i,
                                                  .stride = k->bind[i].stride, /* 0: dynamic */
                                                  .inputRate = k->bind[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                                  : VK_VERTEX_INPUT_RATE_VERTEX};
        if (k->bind[i].divisor > 1 && g_vk.has_divisor)
            dv[ndv++] = (VkVertexInputBindingDivisorDescriptionEXT){i, k->bind[i].divisor};
    }
    VkVertexInputAttributeDescription va[32];
    for (u32 i = 0; i < k->nattr; i++)
        va[i] = (VkVertexInputAttributeDescription){k->attr[i].loc, k->attr[i].binding, (VkFormat)k->attr[i].format,
                                                    k->attr[i].offset};
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
        .vertexBindingDivisorCount = ndv,
        .pVertexBindingDivisors = dv};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .pNext = ndv ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = k->nbind,
                                               .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = k->nattr,
                                               .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = (VkPrimitiveTopology)k->topo_class};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .pNext = k->provoking_last ? &provoking : NULL,
                                                 .depthClampEnable = k->depth_clamp,
                                                 .polygonMode = (VkPolygonMode)k->polygon,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8];
    int changed = 0;
    for (u32 i = 0; i < k->ncolor; i++) {
        cba[i] = (VkPipelineColorBlendAttachmentState){.blendEnable = k->blend[i].en,
                                                       .srcColorBlendFactor = (VkBlendFactor)k->blend[i].sc,
                                                       .dstColorBlendFactor = (VkBlendFactor)k->blend[i].dc,
                                                       .colorBlendOp = (VkBlendOp)k->blend[i].cop,
                                                       .srcAlphaBlendFactor = (VkBlendFactor)k->blend[i].sa,
                                                       .dstAlphaBlendFactor = (VkBlendFactor)k->blend[i].da,
                                                       .alphaBlendOp = (VkBlendOp)k->blend[i].aop,
                                                       /* no fragment shader: its outputs would be undefined */
                                                       .colorWriteMask = k->fs ? k->blend[i].mask : 0};
        if (!g_vk.feat.dualSrcBlend) { /* the second source's factors become the first's */
            VkBlendFactor* f[4] = {&cba[i].srcColorBlendFactor, &cba[i].dstColorBlendFactor,
                                   &cba[i].srcAlphaBlendFactor, &cba[i].dstAlphaBlendFactor};
            for (int j = 0; j < 4; j++) {
                VkBlendFactor o = *f[j];
                *f[j] = o == VK_BLEND_FACTOR_SRC1_COLOR             ? VK_BLEND_FACTOR_SRC_COLOR
                        : o == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ? VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR
                        : o == VK_BLEND_FACTOR_SRC1_ALPHA           ? VK_BLEND_FACTOR_SRC_ALPHA
                        : o == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                                                    : o;
                changed |= *f[j] != o;
            }
        }
    }
    if (!g_vk.feat.independentBlend) /* every attachment blends as the first */
        for (u32 i = 1; i < k->ncolor; i++) {
            changed |= memcmp(&cba[i], &cba[0], sizeof cba[0]) != 0;
            cba[i] = cba[0];
        }
    if (changed) {
        LOG_LIMIT(
            1, "vulkan: blending that needs dual-source or independent blend, which this GPU lacks, is simplified\n");
    }
    /* a logic operation takes the place of blending on integer and normalized targets; float and sRGB ones pass
       their color through, as the 3D engine's LOGIC_OP does */
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .logicOpEnable = k->logic_op != 0,
                                              .logicOp = k->logic_op ? (VkLogicOp)(k->logic_op - 1) : VK_LOGIC_OP_COPY,
                                              .attachmentCount = k->ncolor,
                                              .pAttachments = cba};
    VkDynamicState dyn[21 + 2] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
                                  VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
                                  VK_DYNAMIC_STATE_CULL_MODE,
                                  VK_DYNAMIC_STATE_FRONT_FACE,
                                  VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                                  VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                  VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                  VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                  VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
                                  VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                                  VK_DYNAMIC_STATE_STENCIL_OP,
                                  VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
                                  VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
                                  VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
                                  VK_DYNAMIC_STATE_LINE_WIDTH,
                                  VK_DYNAMIC_STATE_DEPTH_BIAS,
                                  VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                  VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                  VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                  VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                  VK_DYNAMIC_STATE_DEPTH_BOUNDS};
    u32 ndyn = 21; /* the states above, which every pipeline has */
    /* without the feedback loop's dynamic state, a pipeline enables the loop with a flag (k->feedback) instead */
    if (g_vk.has_feedback_dynamic) dyn[ndyn++] = VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT;
    /* the stride is set by vkCmdBindVertexBuffers2, which a draw without vertex buffers never calls: a pipeline without
       bindings leaves it out */
    if (k->nbind && g_vk.has_dynamic_stride) dyn[ndyn++] = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = ndyn,
                                           .pDynamicStates = dyn};
    VkFormat cf[8];
    for (u32 i = 0; i < k->ncolor; i++) cf[i] = (VkFormat)k->color[i];
    /* whether the pipeline cache had it, for the stats line (core in Vulkan 1.3) */
    VkPipelineCreationFeedback made = {0};
    VkPipelineCreationFeedbackCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                               .pPipelineCreationFeedback = &made};
    /* Select strict vertex bounds without enabling robustness2 globally for
       storage/uniform buffers or compute pipelines. Per-pipeline selection
       requires the feature to be supported, not enabled device-wide. */
    VkPipelineRobustnessCreateInfoEXT robustness = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT,
        .pNext = &fi,
        .vertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .pNext = g_vk.has_vertex_robustness2 ? (void*)&robustness : (void*)&fi,
                                        .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = cf,
                                        .depthAttachmentFormat = (VkFormat)k->depth,
                                        .stencilAttachmentFormat = (VkFormat)k->stencil};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &ri,
                                       .flags =
                                           k->feedback ? VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0,
                                       .stageCount = nst,
                                       .pStages = st,
                                       .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia,
                                       .pTessellationState = k->tes ? &ts : NULL,
                                       .pViewportState = &vp,
                                       .pRasterizationState = &rs,
                                       .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb,
                                       .pDynamicState = &dy,
                                       .layout = e->layout};
    VkPipeline p = VK_NULL_HANDLE;
    VK_OK(vkCreateGraphicsPipelines(g_vk.dev, g_pcache, 1, &gi, NULL, &p));
    if (made.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)
        __atomic_add_fetch(e->prewarm ? &g_pf.hits : &g_pipe_hits, 1, __ATOMIC_RELAXED);
    return p;
}
/* e's pipeline, made here. One a draw asked for is kept in the file (pf_pipe_record): once made when the GPU thread
   makes it (record), and when it queues it for a worker otherwise, as the file's tables are the GPU thread's */
static VkPipeline pipe_make(const Pipe* e, int record) {
    SNX_ZONE_T(z_pipe, "pipeline make", "hash %016llx", (unsigned long long)e->h);
    u64 t0 = plat_ns();
    SNX_ZONE(z, "pipeline make");
    VkPipeline p = pipe_create(e);
    SNX_ZONE_END(z);
    pcache_note();
    if (e->prewarm)
        __atomic_add_fetch(&g_pf.made, 1, __ATOMIC_RELAXED);
    else {
        __atomic_add_fetch(&g_pipe_make_ns, plat_ns() - t0, __ATOMIC_RELAXED);
        if (p && record) pf_pipe_record(&e->k);
    }
    return p;
}

/* the workers' queues, and the wait for one pipeline to be ready. The file's pipelines a draw asked for are made first,
   in the order asked for; the rest of the file's (pf_load_pipe) after */
static PMutex g_pj_lock = PMUTEX_INIT;
static PCond g_pj_work, g_pj_done;
static Pipe *g_pj_head, *g_pj_tail; /* a draw's */
static Pipe *g_pw_head, *g_pw_tail; /* the file's */
static int g_pj_started, g_pj_threads;
static void pipe_worker(void* arg) {
    (void)arg;
    SNX_THREAD_NAME("Vulkan pipelines");
    for (;;) {
        Pipe* e;
        VkPipeline p;
        pm_lock(&g_pj_lock);
        while (!g_pj_head && !g_pw_head) pc_wait(&g_pj_work, &g_pj_lock, -1);
        if (g_pj_head) {
            e = g_pj_head;
            g_pj_head = e->job;
            if (!g_pj_head) g_pj_tail = NULL;
        } else {
            e = g_pw_head;
            g_pw_head = e->job;
            if (!g_pw_head) g_pw_tail = NULL;
        }
        e->queued = 0;
        pm_unlock(&g_pj_lock);
        p = pipe_make(e, 0);
        pm_lock(&g_pj_lock);
        e->p = p;
        __atomic_store_n(&e->ready, 1, __ATOMIC_RELEASE);
        pc_broadcast(&g_pj_done);
        pm_unlock(&g_pj_lock);
    }
}
int vk_pipeline_worker_count(int setting, int cpus) {
    int n = cpus - 2;
    if (setting > 0) return setting;
    /* all the CPUs but two, two to twelve. A first run's burst of new pipelines goes to the workers side by side
       (vk_pipeline_get_deferred), and the frame waits for them at its binds, so the game's threads have little to do
       meanwhile, and an idle worker costs nothing. A quarter of 8 CPUs (two workers) froze one game's first scene for
       4.6 s with its caches empty, where six took 1.7 s and eight 1.5 s: past six the CPUs the game's own threads use
       take back what more would give */
    if (n < 2) n = 2;
    if (n > 12) n = 12;
    return n;
}
/* the workers, started the first time one is needed: how many there are (g_pj_lock) */
static int pipe_workers(void) {
    if (!g_pj_started) {
        int ids[256], n = vk_pipeline_worker_count(g_settings.pipeline_threads, plat_cpus(ids, 256));
        g_pj_started = 1;
        pc_init(&g_pj_work);
        pc_init(&g_pj_done);
        for (int i = 0; i < n; i++)
            if (!plat_thread(pipe_worker, NULL, (size_t)1 << 20)) g_pj_threads++;
    }
    return g_pj_threads;
}
/* e at the end of a queue (g_pj_lock): 1 a draw's, 2 the file's */
static void pipe_queue(Pipe* e, int queue) {
    Pipe **head = queue == 1 ? &g_pj_head : &g_pw_head, **tail = queue == 1 ? &g_pj_tail : &g_pw_tail;
    e->job = NULL;
    if (*tail)
        (*tail)->job = e;
    else
        *head = e;
    *tail = e;
    e->queued = queue;
    pc_signal(&g_pj_work);
}
/* a pipeline of the file that a draw asks for before a worker took it goes to the draws' queue: 1 when it did */
static int pipe_promote(Pipe* e) {
    int moved = 0;
    pm_lock(&g_pj_lock);
    if (e->queued == 2) {
        Pipe *q, *before = NULL;
        for (q = g_pw_head; q != e; q = q->job) before = q;
        if (before)
            before->job = e->job;
        else
            g_pw_head = e->job;
        if (g_pw_tail == e) g_pw_tail = before;
        pipe_queue(e, 1);
        moved = 1;
    }
    pm_unlock(&g_pj_lock);
    return moved;
}
/* e's pipeline once ready, waiting at most ns for it (a negative ns: until it is) */
static VkPipeline pipe_wait(Pipe* e, i64 ns) {
    u64 until = ns < 0 ? 0 : plat_ns() + (u64)ns;
    SNX_ZONE(z, "pipeline wait");
    pm_lock(&g_pj_lock);
    while (!e->ready) {
        u64 now = plat_ns();
        if (ns >= 0 && now >= until) break;
        pc_wait(&g_pj_done, &g_pj_lock, ns < 0 ? -1 : (i64)(until - now));
    }
    pm_unlock(&g_pj_lock);
    SNX_ZONE_END(z);
    return __atomic_load_n(&e->ready, __ATOMIC_ACQUIRE) ? e->p : VK_NULL_HANDLE;
}
/* Stand-ins for pipelines still being made, which the GPU thread hands the recording thread in their place
   (vk_pipeline_get_deferred): PIPE_STANDIN in the top bits, which no handle has (record.c's REC_STANDIN tells the same
   of descriptor sets), and the pipeline's index in this table, pages the GPU thread adds before a stand-in naming
   them goes through the ring. Pipelines live as long as the run, so a stand-in does too */
#define PIPE_STANDIN   0x5EC6000000000000ull
#define PSTAND_PAGE    1024
static Pipe** g_pstand[1024];
static u32 g_npstand;
static VkPipeline pipe_standin(Pipe* p) {
    if (!p->standin) {
        u32 page = g_npstand / PSTAND_PAGE;
        if (page >= sizeof g_pstand / sizeof *g_pstand) return VK_NULL_HANDLE;
        if (!g_pstand[page] && !(g_pstand[page] = calloc(PSTAND_PAGE, sizeof **g_pstand))) return VK_NULL_HANDLE;
        g_pstand[page][g_npstand % PSTAND_PAGE] = p;
        p->standin = ++g_npstand;
    }
    return (VkPipeline)(uintptr_t)(PIPE_STANDIN | p->standin);
}
/* the GPU thread's wait for p, made by a worker */
static VkPipeline pipe_wait_here(Pipe* p) {
    u64 t0 = plat_ns();
    gpu_wait_note("a pipeline being made", p->h);
    VkPipeline got = pipe_wait(p, -1);
    gpu_wait_note(NULL, 0);
    __atomic_add_fetch(&g_pipe_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
    return got;
}
static void rc_merge(void);
/* k's entry, found or added (NULL with no memory): *found says whether it was there */
static Pipe* pipe_entry_in(const PipeKey* k, int* found);
static Pipe* pipe_entry(const PipeKey* k, int* found) {
    pf_merge();
    rc_merge();
    return pipe_entry_in(k, found);
}
static Pipe* pipe_entry_in(const PipeKey* k, int* found) {
    u64 h = vk_hash(k, sizeof *k);
    u32 b = (u32)(h ^ (h >> 32)) & (PIPEH - 1);
    for (Pipe* p = g_pipeh[b]; p; p = p->next)
        if (p->h == h && !memcmp(&p->k, k, sizeof *k)) {
            *found = 1;
            return p;
        }
    Pipe* p = calloc(1, sizeof *p);
    *found = 0;
    if (!p) return NULL;
    p->k = *k;
    p->h = h;
    p->layout = vk_playout(k->arrays);
    p->fs = k->fs ? k->fs : null_fs();
    p->next = g_pipeh[b];
    g_pipeh[b] = p;
    g_pipes_new++;
    return p;
}
/* the pipeline for k: made here when new, waited for when one of the file's is still being made */
VkPipeline vk_pipeline_get(const PipeKey* k) {
    int found;
    Pipe* p = pipe_entry(k, &found);
    if (!p) return VK_NULL_HANDLE;
    if (found) {
        if (__atomic_load_n(&p->ready, __ATOMIC_ACQUIRE)) return p->p;
        if (p->prewarm) pipe_promote(p);
        return pipe_wait_here(p);
    }
    u64 t0 = plat_ns();
    p->p = pipe_make(p, 1);
    __atomic_store_n(&p->ready, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_pipe_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
    return p->p;
}
/* A draw's pipeline when its commands go through the recording thread: the pipeline if it is ready, otherwise a
   stand-in, with the pipeline made on a worker in the order draws asked for them. The GPU thread goes on with the
   frame and the recording thread waits for each pipeline when it reaches its bind (vk_pipeline_resolve). A burst of
   new pipelines (a first run makes hundreds as views come into sight, 87 ms each on one driver without its cache)
   is made by the workers side by side while the frame is decoded, where the GPU thread made them one after another
   and stopped the game for 45 s once. No draw is skipped for them */
VkPipeline vk_pipeline_get_deferred(const PipeKey* k) {
    if (!vk_rec_threaded()) return vk_pipeline_get(k);
    int found;
    Pipe* p = pipe_entry(k, &found);
    if (!p) return VK_NULL_HANDLE;
    if (found) {
        if (__atomic_load_n(&p->ready, __ATOMIC_ACQUIRE)) return p->p;
        if (p->prewarm) pipe_promote(p);
    } else {
        int queued = 0;
        pf_pipe_record(&p->k);
        pm_lock(&g_pj_lock);
        if (pipe_workers()) {
            pipe_queue(p, 1);
            queued = 1;
        }
        pm_unlock(&g_pj_lock);
        if (!queued) { /* no worker: made here */
            u64 t0 = plat_ns();
            p->p = pipe_make(p, 0);
            __atomic_store_n(&p->ready, 1, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_pipe_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
            return p->p;
        }
    }
    VkPipeline s = pipe_standin(p);
    return s ? s : pipe_wait_here(p);
}
/* on the recording thread: the pipeline a handle from vk_pipeline_get_deferred names, waited for while a worker makes
   it. VK_NULL_HANDLE when it couldn't be made */
VkPipeline vk_pipeline_resolve(VkPipeline h) {
    u64 v = (u64)(uintptr_t)h;
    if ((v & ~(u64)0xFFFFFFFFull) != PIPE_STANDIN) return h;
    u32 i = (u32)v - 1;
    Pipe* p = g_pstand[i / PSTAND_PAGE][i % PSTAND_PAGE];
    if (__atomic_load_n(&p->ready, __ATOMIC_ACQUIRE)) return p->p;
    u64 t0 = plat_ns();
    VkPipeline got = pipe_wait(p, -1);
    __atomic_add_fetch(&g_pipe_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
    return got;
}
/* the pipeline a handle from vk_pipeline_get_deferred names once it is ready, else the handle: without waiting */
VkPipeline vk_pipeline_ready(VkPipeline h) {
    u64 v = (u64)(uintptr_t)h;
    if ((v & ~(u64)0xFFFFFFFFull) != PIPE_STANDIN) return h;
    u32 i = (u32)v - 1;
    Pipe* p = g_pstand[i / PSTAND_PAGE][i % PSTAND_PAGE];
    return __atomic_load_n(&p->ready, __ATOMIC_ACQUIRE) ? p->p : h;
}
/* the pipeline h names without waiting for it: VK_NULL_HANDLE with *pending set while a worker still makes it, and
   VK_NULL_HANDLE alone when it couldn't be made */
VkPipeline vk_pipeline_now(VkPipeline h, int* pending) {
    VkPipeline p = vk_pipeline_ready(h);
    *pending = (((u64)(uintptr_t)p) & ~(u64)0xFFFFFFFFull) == PIPE_STANDIN;
    return *pending ? VK_NULL_HANDLE : p;
}
void vk_pipeline_stats(u64* pipes, u64* hits, u64* make_ms, u64* wait_ms, u64* shaders, u64* shader_ms) {
    *pipes = g_pipes_new;
    *hits = __atomic_load_n(&g_pipe_hits, __ATOMIC_RELAXED);
    *make_ms = __atomic_load_n(&g_pipe_make_ns, __ATOMIC_RELAXED) / 1000000;
    *wait_ms = __atomic_load_n(&g_pipe_wait_ns, __ATOMIC_RELAXED) / 1000000;
    *shaders = g_sh_new;
    *shader_ms = g_sh_ns / 1000000;
}

VkPipelineCache vk_pipeline_cache(void) {
    pcache_note();
    return g_pcache;
}
/* writes the pipeline cache when pipelines were made since the last write. Asked every few seconds and at exit (the
   renderer's save), outside vk_lock: the driver synchronizes the cache itself. The driver holds the cache's lock while
   it copies the cache out, so a pipeline made meanwhile waits for the whole copy, and the copies came every five seconds
   while pipelines were being made: a pipeline once waited 3.2 s with recording stopped behind it. So the asking every
   few seconds writes it only after PCACHE_QUIET_NS without a new pipeline, and at exit it is written regardless. Saves
   one at a time, so the one at exit waits for one in progress to finish its file before the process ends */
#define PCACHE_QUIET_NS 20000000000ull
static PMutex g_psave = PMUTEX_INIT;
static void rc_record(void);
void vk_pipeline_save(int at_exit) {
    if (at_exit && getenv("SWR_VK_RECIPES_RECORD")) rc_record();
    if (!at_exit && plat_ns() - __atomic_load_n(&g_pcache_used, __ATOMIC_RELAXED) < PCACHE_QUIET_NS) return;
    pm_lock(&g_psave);
    /* the shipped file's shaders the draws found, when that changed: nidx is settled once ship_ready */
    if (__atomic_load_n(&g_pf.ship_ready, __ATOMIC_ACQUIRE) && g_pf.nidx) {
        u32 used = __atomic_load_n(&g_pf.ship_used, __ATOMIC_RELAXED);
        if (used != g_pf.ship_said) {
            g_pf.ship_said = used;
            LOG("vulkan: %u of the %u shaders translated from the game's files found by its draws so far\n", used,
                g_pf.nidx);
        }
    }
    if (!g_pcache || !__atomic_exchange_n(&g_pcache_dirty, 0, __ATOMIC_RELAXED)) {
        pm_unlock(&g_psave);
        return;
    }
    size_t n = 0;
    void* d = NULL;
    if (vkGetPipelineCacheData(g_vk.dev, g_pcache, &n, NULL) == VK_SUCCESS && n) d = malloc(n);
    if (d && vkGetPipelineCacheData(g_vk.dev, g_pcache, &n, d) == VK_SUCCESS) {
        FILE* f = fopen(g_pcache_tmp, "wb");
        if (f) {
            int ok = fwrite(d, 1, n, f) == n;
            if (fclose(f)) ok = 0;
            if (ok)
                plat_replace_file(g_pcache_tmp, g_pcache_path); /* the old cache stays whole until this */
            else
                remove(g_pcache_tmp);
        }
    }
    free(d);
    pm_unlock(&g_psave);
}

/* ------------------------------------------------------------ the shaders and pipelines of earlier runs */
/* a module's handle as the file knows it, and back (a pointer, or a 64-bit integer on 32-bit hosts) */
static u64 mod_bits(VkShaderModule m) {
    u64 v = 0;
    memcpy(&v, &m, sizeof m);
    return v;
}
static VkShaderModule mod_handle(u64 v) {
    VkShaderModule m = VK_NULL_HANDLE;
    memcpy(&m, &v, sizeof m);
    return m;
}
typedef char pf_cbr_words[sizeof(VkCbRead) == 16 ? 1 : -1]; /* the file's reads are VkCbRead's four words */

/* the bytes of a program kept with its shader (PF_SOURCE): mlen, the program as shader_hash sizes it, which is what
   the translation reads, with the words after it that a lookahead past its last instruction would read */
static u32 src_bytes(u32 mlen) {
    u32 n = ((mlen + 7u) & ~7u) + 64u;
    return n < SH_CODE ? n : SH_CODE;
}
/* s's record, of spv (nw words), into pf for module (0 for none): where it starts in the file, 0 when not written.
   With code (its first ncode bytes, src_bytes) and xfb, the program it was translated from goes before it
   (PF_SOURCE), which a later build or driver translates again (pf_load_source) */
static u64 pf_shader_into(PFile* pf, const VkSh* s, u64 module, const u32* spv, size_t nw, const u8* code, u32 ncode,
                          const ShXfb* xfb) {
    PfShader r = {0};
    PfSource src = {0};
    if (!g_pf.on || nw > 0xFFFFFFFFu) return 0;
    if (code) {
        src.hash = s->hash;
        src.qh = s->qh;
        src.qh_a = s->qh_a;
        src.stage = (u32)s->stage;
        src.clen = s->clen;
        src.clen_a = s->clen_a;
        src.ncbr = s->cbr.n;
        src.ncode = ncode;
        src.flags = xfb ? PF_SRC_XFB : 0;
        memcpy(src.mask, s->mask, sizeof src.mask);
    }
    r.hash = s->hash;
    r.qh = s->qh;
    r.qh_a = s->qh_a;
    r.stage = (u32)s->stage;
    r.clen = s->clen;
    r.clen_a = s->clen_a;
    r.flags = s->shipped ? PF_SHIPPED : 0;
    r.ncbr = s->cbr.n;
    r.nspv = (u32)nw;
    memcpy(r.mask, s->mask, sizeof r.mask);
    memcpy(&r.si, &s->si, sizeof r.si);
    return pf_shader(pf, module, &r, (const u32*)(const void*)s->cbr.r, spv, code ? &src : NULL, code, xfb);
}
/* s, made this run from spv translated from code (ncode bytes of it kept, with xfb): kept for the next one */
static void pf_shader_record(const VkSh* s, const u32* spv, size_t nw, const u8* code, u32 ncode, const ShXfb* xfb) {
    pf_shader_into(&g_pf.file, s, mod_bits(s->mod), spv, nw, code, ncode, xfb);
}
/* a draw's pipeline, made: kept for the next run, its modules by their shaders' records (any thread) */
static void pf_pipe_record(const PipeKey* k) {
    PipeKey c;
    u64 m[5];
    if (!g_pf.on) return;
    m[0] = mod_bits(k->vs);
    m[1] = mod_bits(k->tcs);
    m[2] = mod_bits(k->tes);
    m[3] = mod_bits(k->gs);
    m[4] = mod_bits(k->fs);
    memcpy(&c, k, sizeof c);
    c.vs = c.tcs = c.tes = c.gs = c.fs = VK_NULL_HANDLE;
    pf_pipe(&g_pf.file, m, &c, sizeof c);
}

/* s after the file's shaders, which pf_merge puts in the tables (the reading thread, before loaded) */
static void pf_sh_append(VkSh* s) {
    if (g_pf.sh_tail)
        g_pf.sh_tail->next = s;
    else
        g_pf.sh = s;
    g_pf.sh_tail = s;
    g_pf.nsh++;
}
/* a shader record (its reads and SPIR-V after it) made into a VkSh as vk_shader_get makes one, without translating:
   NULL when it can't be */
static VkSh* sh_from_record(const PfShader* r, const u32* cbr, const u32* spv) {
    VkSh* s;
    if (r->si.ntex < 0 || r->si.ntex > SH_MAXTEX || vk_spirv_unavailable(spv, r->nspv)) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (r->ncbr) {
        s->cbr.r = malloc(r->ncbr * sizeof *s->cbr.r);
        if (!s->cbr.r) {
            free(s);
            return NULL;
        }
        memcpy(s->cbr.r, cbr, r->ncbr * sizeof *s->cbr.r);
        s->cbr.n = r->ncbr;
    }
    s->hash = r->hash;
    s->shipped = (r->flags & PF_SHIPPED) != 0;
    s->stage = (int)r->stage;
    memcpy(s->mask, r->mask, sizeof s->mask);
    memcpy(&s->si, &r->si, sizeof s->si);
    s->clen = r->clen;
    s->qh = r->qh;
    s->clen_a = r->clen_a;
    s->qh_a = r->qh_a;
    for (int i = 0; i < s->si.ntex; i++) s->indexed |= (s->si.tex[i].flags & SNX_TEX_INDEXED) != 0;
    s->mod = vk_module_spv(spv, r->nspv);
    if (!s->mod) {
        free(s->cbr.r);
        free(s);
        return NULL;
    }
    return s;
}
/* on the reading thread: a shader of the file */
static u64 pf_load_shader(void* ctx, const PfShader* r, const u32* cbr, const u32* spv) {
    VkSh* s = sh_from_record(r, cbr, spv);
    (void)ctx;
    if (!s) return 0;
    pf_sh_append(s);
    return mod_bits(s->mod);
}
/* the reads a shader of a file being rebuilt was translated with, which its translation again answers from (the
   reading thread's alone). A read it didn't make answers that the buffer isn't bound: the shader then keeps that read,
   and a draw whose buffers hold more translates its own (vk_shader_get checks every read) */
static const VkCbReads* g_replay;
static int replay_read(u32 idx, u32 off, u32* out) {
    for (u32 i = 0; i < g_replay->n; i++)
        if (g_replay->r[i].idx == idx && g_replay->r[i].off == off) {
            *out = g_replay->r[i].v;
            return (int)g_replay->r[i].ok;
        }
    return 0;
}
/* on the reading thread, in a file of another build or driver being rebuilt: the program a shader was translated
   from, translated again with this build's translator as vk_shader_get would, and recorded in the new file */
static u64 pf_load_source(void* ctx, const PfSource* r, const u32* cbr, const u8* code, const ShXfb* xfb) {
    VkCbRead* reads = r->ncbr ? malloc(r->ncbr * sizeof *reads) : NULL;
    VkCbReads replay = {reads, r->ncbr};
    u8* window = calloc(1, SH_CODE);
    VkSh* s = calloc(1, sizeof *s);
    u32* spv = NULL;
    size_t nw = 0;
    (void)ctx;
    if ((r->ncbr && !reads) || !window || !s || r->ncode > SH_CODE) goto fail;
    if (r->ncbr) memcpy(reads, cbr, r->ncbr * sizeof *reads);
    memcpy(window, code, r->ncode);
    g_replay = &replay;
    spv = sh_spirv(s, window, r->hash, (int)r->stage, r->mask, r->clen, r->qh, r->clen_a, r->qh_a, xfb, replay_read, 0,
                   &nw);
    g_replay = NULL;
    if (!spv || vk_spirv_unavailable(spv, nw) || !(s->mod = vk_module_spv(spv, nw))) goto fail;
    pf_shader_into(&g_pf.file, s, mod_bits(s->mod), spv, nw, window, r->ncode, xfb);
    free(spv);
    free(window);
    free(reads);
    pf_sh_append(s);
    return mod_bits(s->mod);
fail:
    free(spv);
    free(window);
    free(reads);
    if (s) free(s->cbr.r);
    free(s);
    return 0;
}
/* In a rebuild, a pipeline record keeps the earlier build's key with the modules of the new translation. Two of its
   fields come from that translation: the sets with texture arrays, from each stage's indexed flag, and the attributes,
   one for each one the vertex shader reads. A translator that came to index a shader's textures would have its
   pipeline made with the layout without the arrays, which its module declares, and the record written into the new
   file as it was. 1 when both fields are what the rebuilt shaders say; a record they don't hold for is dropped, and a
   draw that needs its pipeline makes it from its own key */
static int pf_key_holds(const PipeKey* k) {
    const VkShaderModule mods[5] = {k->vs, k->tcs, k->tes, k->gs, k->fs};
    u32 arrays = 0, attrs = 0, in = 0;
    for (int i = 0; i < 5; i++) {
        const VkSh* sh = NULL;
        if (!mods[i]) continue;
        for (const VkSh* q = g_pf.sh; q && !sh; q = q->next)
            if (q->mod == mods[i]) sh = q;
        if (!sh) return 0;
        if (sh->indexed) arrays |= 1u << shader_vk_set(sh->stage);
        if (i == 0) in = sh->si.in_mask;
    }
    for (u32 i = 0; i < k->nattr; i++) attrs |= 1u << k->attr[i].loc;
    return arrays == k->arrays && attrs == in;
}
/* on the reading thread: a pipeline of the file, whose shaders were made, queued to the workers behind the draws'. In
   a file being rebuilt it is recorded in the new one, whose shader records its modules have */
static void pf_load_pipe(void* ctx, const u64 m[5], const void* key, u32 keylen) {
    Pipe* p;
    if (keylen != sizeof p->k || !m[0]) return;
    p = calloc(1, sizeof *p);
    if (!p) return;
    memcpy(&p->k, key, sizeof p->k);
    if (p->k.nattr > 32 || p->k.nbind > 33 || p->k.ncolor > 8) { /* pipe_create's arrays */
        free(p);
        return;
    }
    p->k.vs = mod_handle(m[0]);
    p->k.tcs = mod_handle(m[1]);
    p->k.tes = mod_handle(m[2]);
    p->k.gs = mod_handle(m[3]);
    p->k.fs = mod_handle(m[4]);
    if (((const PfLoad*)ctx)->rebuild && !pf_key_holds(&p->k)) {
        free(p);
        return;
    }
    p->h = vk_hash(&p->k, sizeof p->k);
    for (Pipe* q = g_pf.pipes; q; q = q->next) /* the file has it twice */
        if (q->h == p->h && !memcmp(&q->k, &p->k, sizeof p->k)) {
            free(p);
            return;
        }
    p->layout = vk_playout(p->k.arrays);
    p->fs = p->k.fs ? p->k.fs : null_fs();
    p->prewarm = 1;
    if (((const PfLoad*)ctx)->rebuild) pf_pipe_record(&p->k);
    pm_lock(&g_pj_lock);
    if (!pipe_workers()) { /* made when a draw asks for it, then */
        pm_unlock(&g_pj_lock);
        free(p);
        return;
    }
    pipe_queue(p, 2);
    pm_unlock(&g_pj_lock);
    p->next = g_pf.pipes; /* the workers don't use next */
    g_pf.pipes = p;
    g_pf.npipe++;
}

/* PF_GUEST_VERSION: what a PF_SOURCE or PF_PIPE record means, beyond the sizes the key holds. Raised when a PipeKey
   field or a key word (vk_shader_get's mask) comes to mean something else, so that the records of an earlier build
   are dropped rather than made into pipelines and shaders of the wrong state */
#define PF_GUEST_VERSION 1
/* The file's key: whatever changes the SPIR-V translation makes or what a PipeKey means. First its guest part, which
   the PF_SOURCE and PF_PIPE records depend on (pf_open): the device; the features and limits the translator,
   vk_shader_get and pipe_create consult; the sizes of what those records hold whole. Then the rest, which only the
   SPIR-V and ShaderInfo of the PF_SHADER records depend on: the build (commit, compiler, host), and for a tree with
   changes, which has one commit for every build of it, the runtime's own file; the driver. The guest part's length
   goes in *guest */
static void pf_key(char* out, size_t n, size_t* guest) {
    const VkPhysicalDeviceProperties* d = &g_vk.props;
    const char* commit = snx_build_commit();
    size_t len = strlen(commit);
    char exe[64] = "", uuid[33];
    u32 caps[] = {(u32)g_vk.has_divisor,
                  (u32)g_vk.has_dynamic_stride,
                  (u32)g_vk.has_shader_draw_parameters,
                  (u32)g_vk.has_feedback,
                  (u32)g_vk.has_feedback_dynamic,
                  (u32)g_shader_texture_arrays,
                  (u32)g_shader_stage_point_size,
                  (u32)g_shader_output_layer,
                  (u32)g_shader_geometry,
                  (u32)g_shader_float16,
                  (u32)g_shader_tessellation,
                  (u32)g_shader_clock_scopes,
                  (u32)g_vk_flush_denorm,
                  (u32)g_vk_preserve_infnan,
                  (u32)g_vk_preserve_infnan64,
                  (u32)g_vk_subgroup_stages,
                  (u32)g_vk.has_provoking_last,
                  g_vk_stage_sets,
                  g_vk_buffer_image_slots,
                  d->limits.maxPerStageDescriptorStorageImages,
                  d->limits.maxDescriptorSetStorageImages};
    /* a tree with changes builds under one commit name: the runtime's own bytes tell those builds apart. Not its
       modification time, which a copy of the same runtime (each game has one) changes */
    if (len >= 6 && !strcmp(commit + len - 6, "-dirty")) {
        char* self = snx_executable("scarletnx");
        PFileMap m;
        if (self && !plat_file_map(&m, self) && m.data) {
            snprintf(exe, sizeof exe, " exe=%016llx", (unsigned long long)vk_hash(m.data, (size_t)m.size));
            plat_file_unmap(&m);
        }
        free(self);
    }
    for (int i = 0; i < 16; i++) snprintf(uuid + 2 * i, 3, "%02x", d->pipelineCacheUUID[i]);
    int g = snprintf(out, n, "guest=%d gpu=%04x:%04x features=%016llx caps=%016llx sizes=%u,%u,%u,%u;",
                     PF_GUEST_VERSION, d->vendorID, d->deviceID,
                     (unsigned long long)vk_hash(&g_vk.feat, sizeof g_vk.feat),
                     (unsigned long long)vk_hash(caps, sizeof caps), (unsigned)sizeof(PipeKey),
                     (unsigned)sizeof(PfSource), (unsigned)sizeof(PfPipe), (unsigned)sizeof(ShXfb));
    *guest = g > 0 && (size_t)g < n ? (size_t)g : 0;
    if (*guest)
        snprintf(out + *guest, n - *guest, " %s%s driver=%u api=%u uuid=%s ftz=%d sizes=%u,%u vertex_bounds=%d", snx_build_info(), exe,
                 d->driverVersion, d->apiVersion, uuid, g_vk_ftz, (unsigned)sizeof(ShaderInfo),
                 (unsigned)sizeof(PfShader), g_vk.has_vertex_robustness2);
}

/* ---- the shaders of the programs the game ships (gpu/shipped_pass.c finds them), translated before a draw asks */
#define SHIP_MAX (4ull << 30) /* the shipped file's limit: one game's 2,700 programs made 245 MB of SPIR-V */
/* the scan records (shipped_pass.h's SHIP_FILE and SHIP_ALL) as a set, by what and id mixed: never 0, a free slot */
static u64 scan_key(u64 what, u64 id) {
    u64 k = (id ^ what * 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
    return k ? k : 1;
}
static u64* scan_slot(u64 k) {
    u32 i = (u32)(k >> 32) & (g_pf.scap - 1);
    while (g_pf.scans[i] && g_pf.scans[i] != k) i = (i + 1) & (g_pf.scap - 1);
    return &g_pf.scans[i];
}
static void scan_add(u64 what, u64 id) {
    u64 k = scan_key(what, id), *e;
    if ((g_pf.nscans + 1) * 2 > g_pf.scap) { /* at most half full */
        u32 cap = g_pf.scap ? g_pf.scap * 2 : 1024, old = g_pf.scap, i;
        u64 *was = g_pf.scans, *grown = calloc(cap, sizeof *grown);
        if (!grown) return;
        g_pf.scans = grown;
        g_pf.scap = cap;
        for (i = 0; i < old; i++)
            if (was[i]) *scan_slot(was[i]) = was[i];
        free(was);
    }
    e = scan_slot(k);
    if (!*e) {
        *e = k;
        g_pf.nscans++;
    }
}
static int scan_has(u64 what, u64 id) { return g_pf.scap && *scan_slot(scan_key(what, id)); }
/* a line of the pass: in the log, and among the latest the screen shows under its steps */
static void pf_note(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    LOG("vulkan: %s\n", line); /* whole; the screen's, as much as its row holds */
    pm_lock(&g_pf.note_lock);
    snprintf(g_pf.note[g_pf.nnote++ % RENDERER_NOTES], sizeof g_pf.note[0], "%s", line);
    pm_unlock(&g_pf.note_lock);
}
static void pf_ship_found(VkSh* s) {
    s->shipped = 2;
    __atomic_add_fetch(&g_pf.ship_used, 1, __ATOMIC_RELAXED);
}

/* the index's slot for a key: its entry, or the free one where it would go (NULL with no index) */
static ShipIdx* idx_slot(u64 hash, int stage, u32 clip, u32 len, u64 qh, u32 len_a, u64 qh_a) {
    u32 i;
    if (!g_pf.icap) return NULL;
    for (i = (u32)(hash * 0x9E3779B97F4A7C15ull >> 32) & (g_pf.icap - 1); g_pf.idx[i].at;
         i = (i + 1) & (g_pf.icap - 1)) {
        const ShipIdx* e = &g_pf.idx[i];
        if (e->hash == hash && e->stage == (u32)stage && e->clip == clip && e->clen == len && e->qh == qh &&
            e->clen_a == len_a && e->qh_a == qh_a)
            break;
    }
    return &g_pf.idx[i];
}
static void idx_add(u64 hash, int stage, u32 clip, u32 len, u64 qh, u32 len_a, u64 qh_a, u64 at) {
    ShipIdx* e;
    if ((g_pf.nidx + 1) * 2 > g_pf.icap) { /* at most half full */
        u32 cap = g_pf.icap ? g_pf.icap * 2 : 4096, old = g_pf.icap, i;
        ShipIdx *was = g_pf.idx, *grown = calloc(cap, sizeof *grown);
        if (!grown) return;
        g_pf.idx = grown;
        g_pf.icap = cap;
        for (i = 0; i < old; i++)
            if (was[i].at)
                *idx_slot(was[i].hash, (int)was[i].stage, was[i].clip, was[i].clen, was[i].qh, was[i].clen_a,
                          was[i].qh_a) = was[i];
        free(was);
    }
    e = idx_slot(hash, stage, clip, len, qh, len_a, qh_a);
    if (e->at) return; /* the file has it twice */
    *e = (ShipIdx){hash, qh, qh_a, at, len, len_a, (u32)stage, clip};
    g_pf.nidx++;
}
/* The key's clip word the pass translates a stage with, as the draws ask for it (render.c): the clip distances the 3D
   engine enables, all eight (0xff) in the NVN games tried, for the stages that can be the last before rasterization
   (docs/NVN_HLE.md); SNX_SH_NOT_LAST for tessellation control, which never is; none for fragment */
static u32 ship_clip(int stage) { return stage == ST_TCS ? SNX_SH_NOT_LAST : stage == ST_FS ? 0 : 0xFFu; }
/* a key whose words are all zero but the clip word: the keys the pass translates with */
static int ship_key_only_clip(const u32 mask[20]) {
    for (int i = 0; i < 20; i++)
        if (i != 6 && mask[i]) return 0;
    return 1;
}
/* a shader record of the shipped file (len bytes at p) into r, and its SPIR-V: NULL unless sh_from_record can make it
   (no constant buffer reads, a stage a draw binds, its words within it). They are 8-byte aligned in the view: a
   record, its header and its fixed part are multiples of 8 */
static const u32* ship_record(const u8* p, u32 len, PfShader* r) {
    if (!p || len < sizeof *r) return NULL;
    memcpy(r, p, sizeof *r);
    if (r->ncbr || r->stage > ST_FS || r->nspv < 5 || (u64)r->nspv * 4 > len - sizeof *r) return NULL;
    return (const u32*)(const void*)(p + sizeof *r);
}
/* on the reading thread: a record of the shipped file, into the index (a shader) or the scans' set */
static void pf_ship_rec(void* ctx, u32 type, const u8* p, u32 len, u64 at) {
    PfShader r;
    (void)ctx;
    if (type == PF_SCAN && len == 16) {
        u64 w[2];
        memcpy(w, p, sizeof w);
        scan_add(w[0], w[1]);
    } else if (type == PF_SHADER && ship_record(p, len, &r) && ship_key_only_clip(r.mask))
        idx_add(r.hash, (int)r.stage, r.mask[6], r.clen, r.qh, r.clen_a, r.qh_a, at);
}
/* on the reading thread: a record of the scan file, into the scans' set */
static void pf_scan_rec(void* ctx, u32 type, const u8* p, u32 len, u64 at) {
    u64 w[2];
    (void)ctx;
    (void)at;
    if (type != PF_SCAN || len != sizeof w) return;
    memcpy(w, p, sizeof w);
    if (w[0] == SHIP_NONE) scan_add(w[0], w[1]);
}
/* the shipped file's record that starts at at, into r, its check and its SPIR-V: NULL when it isn't one */
static const u32* ship_payload(u64 at, PfShader* r, u64* check) {
    u32 n = 0;
    const u8* p = pf_payload(&g_pf.ship, at, &n, check);
    return ship_record(p, n, r);
}
/* on the reading thread: a PF_SHIPREF of the main file, a shader of the shipped file that an earlier run's draw found,
   made into a module as the main file's own shaders are */
static u64 pf_load_shipref(void* ctx, u64 at, u64 check) {
    PfShader r;
    u64 c = 0;
    const u32* spv = ship_payload(at, &r, &c);
    VkSh* s = spv && c == check ? sh_from_record(&r, NULL, spv) : NULL;
    (void)ctx;
    if (!s) return 0;
    pf_sh_append(s);
    return mod_bits(s->mod);
}
/* A draw's shader the game ships, from the shipped file (vk_shader_get, after the tables had none, vk_lock), once the
   pass is done with it (ship_ready): a key of zeros but its clip word, the way the pass translated it (ship_clip). Its
   record is made into a module now, and named in the main file by a PF_SHIPREF, so that the pipelines made with it
   are kept too. NULL when the file has none */
static VkSh* pf_ship_take(u64 h, int stage, const u32 mask[20], u32 len, u64 qh, u32 len_a, u64 qh_a) {
    const ShipIdx* e;
    const u32* spv;
    PfShader r;
    u64 check = 0;
    VkSh* s;
    if (!g_pf.on || !__atomic_load_n(&g_pf.ship_ready, __ATOMIC_ACQUIRE) || !ship_key_only_clip(mask) ||
        !(e = idx_slot(h, stage, mask[6], len, qh, len_a, qh_a)) || !e->at)
        return NULL;
    spv = ship_payload(e->at, &r, &check);
    /* the record there is the one indexed, unless another process rewrote the file before it was mapped again */
    if (!spv || r.hash != h || r.stage != (u32)stage || r.clen != len || r.qh != qh || r.clen_a != len_a ||
        r.qh_a != qh_a || memcmp(r.mask, mask, sizeof r.mask))
        return NULL;
    s = sh_from_record(&r, NULL, spv);
    if (!s) return NULL;
    pf_shipref(&g_pf.file, mod_bits(s->mod), e->at, check);
    pf_ship_found(s);
    return s;
}

/* ------------------------------------------------------------ SWR: pipeline recipes
   A PC's first run makes each pipeline as its view comes into sight: 110 ms apiece on a GTX 1080 without the driver's
   cache, 9 s of waits in three minutes. The shader file only helps the runs after. A recipe names a pipeline by its
   shaders' keys (the game's program hashes, as the draws and the shipped pass hash them) and its PipeKey without
   modules: no SPIR-V, no game code. Once the shipped pass translated the game's programs on this PC (ship_ready), the
   first draw's lookup (rc_merge) finds each recipe's shaders there and queues its pipeline to the workers behind the
   draws', so most are made while the title screen plays. A recipe whose shader the pass didn't make, or whose key this
   device would fill in another way, just goes unused. File: g_cachedir's pipeline_recipes.bin; SWR_VK_RECIPES_RECORD=1
   adds the pipelines of this run to it at exit (test runs) */
#define RC_MAGIC 0x31434552524B5653ull /* "SVKRREC1" */
typedef struct {
    u64 hash, qh, qh_a;
    u32 len, len_a, stage, clip;
} RcSh;
typedef struct {
    RcSh sh[5];      /* vs, tcs, tes, gs, fs */
    u32 present, pad; /* a bit per stage with a shader */
    PipeKey k;        /* its module handles zeroed */
} Recipe;
typedef struct {
    u64 magic;
    u32 keysize, count;
} RcHead;
static int g_rc_merged;
static char* rc_path(void) {
    static char p[1200];
    if (!p[0]) snprintf(p, sizeof p, "%spipeline_recipes.bin", g_cachedir);
    return p;
}
/* the recipes in the file (malloc'd, *n of them), NULL for none or another build's */
static Recipe* rc_read(u32* n) {
    RcHead h;
    Recipe* r = NULL;
    FILE* f = fopen(rc_path(), "rb");
    *n = 0;
    if (!f) return NULL;
    if (fread(&h, sizeof h, 1, f) == 1 && h.magic == RC_MAGIC && h.keysize == sizeof(PipeKey) && h.count &&
        h.count < 1000000 && (r = malloc((size_t)h.count * sizeof *r)) != NULL) {
        *n = (u32)fread(r, sizeof *r, h.count, f);
        if (!*n) {
            free(r);
            r = NULL;
        }
    }
    fclose(f);
    return r;
}
/* the shader a recipe's stage names, as vk_shader_get finds one: in the tables, else made from the shipped file */
static VkSh* rc_shader(const RcSh* c) {
    u32 mask[20] = {0};
    mask[6] = c->clip;
    for (VkSh* q = g_shh[c->hash & 1023]; q; q = q->next)
        if (q->hash == c->hash && q->stage == (int)c->stage && !memcmp(q->mask, mask, sizeof mask) && q->clen == c->len &&
            q->qh == c->qh && q->clen_a == c->len_a && q->qh_a == c->qh_a && !q->cbr.n && !q->failed)
            return q;
    VkSh* s = pf_ship_take(c->hash, (int)c->stage, mask, c->len, c->qh, c->len_a, c->qh_a);
    if (s) {
        s->shipped = 1; /* not found by a draw yet: pf_ship_take counted it as found */
        __atomic_sub_fetch(&g_pf.ship_used, 1, __ATOMIC_RELAXED);
        s->next = g_shh[c->hash & 1023];
        g_shh[c->hash & 1023] = s;
    }
    return s;
}
/* on the GPU thread (vk_lock), at the first lookup after the shipped pass ended: the recipes' pipelines queued */
static void rc_merge(void) {
    u32 n, queued = 0, known = 0, lacked = 0;
    Recipe* r;
    u64 t0;
    if (g_rc_merged || !g_pf.on || !__atomic_load_n(&g_pf.ship_ready, __ATOMIC_ACQUIRE)) return;
    g_rc_merged = 1;
    if (!(r = rc_read(&n))) return;
    t0 = plat_ns();
    for (u32 i = 0; i < n; i++) {
        PipeKey k = r[i].k;
        VkShaderModule* mod[5] = {&k.vs, &k.tcs, &k.tes, &k.gs, &k.fs};
        int ok = (r[i].present & 1) != 0, found;
        for (int s = 0; s < 5 && ok; s++) {
            *mod[s] = VK_NULL_HANDLE;
            if (!(r[i].present >> s & 1)) continue;
            VkSh* sh = rc_shader(&r[i].sh[s]);
            if (!sh || !sh->mod) ok = 0;
            else *mod[s] = sh->mod;
        }
        if (!ok || k.nattr > 32 || k.nbind > 33 || k.ncolor > 8) {
            lacked++;
            continue;
        }
        Pipe* p = pipe_entry_in(&k, &found);
        if (!p) break;
        if (found) {
            known++;
            continue;
        }
        g_pipes_new--; /* counted as a draw's by pipe_entry_in */
        p->prewarm = 1;
        pm_lock(&g_pj_lock);
        if (pipe_workers()) {
            pipe_queue(p, 2);
            queued++;
        } else
            p->prewarm = 0; /* made when a draw asks for it, then */
        pm_unlock(&g_pj_lock);
    }
    free(r);
    g_pf.npipe += queued;
    __atomic_add_fetch(&g_pf.pipes_total, queued, __ATOMIC_RELAXED);
    LOG("vulkan: pipeline recipes: %u queued to be made, %u already there, %u without their shaders on this PC (%u in "
        "%s, %llu ms)\n",
        queued, known, lacked, n, rc_path(), (unsigned long long)((plat_ns() - t0) / 1000000));
}
/* at exit, SWR_VK_RECIPES_RECORD=1: this run's pipelines whose shaders the shipped pass would make (keys of zeros
   but their clip word, no constant buffer reads) added to the file */
static void rc_record(void) {
    u32 n = 0, added = 0, cap;
    Recipe* r = rc_read(&n);
    vk_lock();
    cap = n + (u32)g_pipes_new + 64;
    Recipe* grown = realloc(r, (size_t)cap * sizeof *r);
    if (!grown) {
        vk_unlock();
        free(r);
        return;
    }
    r = grown;
    for (int b = 0; b < PIPEH; b++)
        for (Pipe* p = g_pipeh[b]; p; p = p->next) {
            Recipe c;
            VkShaderModule mod[5] = {p->k.vs, p->k.tcs, p->k.tes, p->k.gs, p->k.fs};
            int ok = __atomic_load_n(&p->ready, __ATOMIC_ACQUIRE) && p->p && mod[0];
            memset(&c, 0, sizeof c);
            for (int s = 0; s < 5 && ok; s++) {
                VkSh* f = NULL;
                if (!mod[s]) continue;
                for (int i = 0; i < 1024 && !f; i++)
                    for (VkSh* q = g_shh[i]; q && !f; q = q->next)
                        if (q->mod == mod[s]) f = q;
                if (!f || f->failed || f->cbr.n || !ship_key_only_clip(f->mask)) {
                    ok = 0;
                    break;
                }
                c.sh[s] = (RcSh){f->hash, f->qh, f->qh_a, f->clen, f->clen_a, (u32)f->stage, f->mask[6]};
                c.present |= 1u << s;
            }
            if (!ok) continue;
            c.k = p->k;
            c.k.vs = c.k.tcs = c.k.tes = c.k.gs = c.k.fs = VK_NULL_HANDLE;
            int dup = 0;
            for (u32 i = 0; i < n && !dup; i++) dup = !memcmp(&r[i], &c, sizeof c);
            if (dup) continue;
            if (n == cap) break;
            r[n++] = c;
            added++;
        }
    vk_unlock();
    if (added) {
        RcHead h = {RC_MAGIC, (u32)sizeof(PipeKey), n};
        char tmp[1300];
        snprintf(tmp, sizeof tmp, "%s.tmp", rc_path());
        FILE* f = fopen(tmp, "wb");
        if (f) {
            int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(r, sizeof *r, n, f) == n;
            if (fclose(f)) ok = 0;
            if (ok)
                plat_replace_file(tmp, rc_path());
            else
                remove(tmp);
        }
    }
    LOG("vulkan: pipeline recipes recorded: %u new, %u in %s\n", added, n, rc_path());
    free(r);
}

/* What the pass hands each program to (a ShipSink's, on the reading thread). No constant buffer is read: a program
   without a draw has none, and a translation that reads one fails (sh_spirv's shipped) */
static int ship_no_read(u32 idx, u32 off, u32* out) {
    (void)idx;
    (void)off;
    (void)out;
    return 0;
}
/* SH_CODE bytes from p (avail of them), as vk_shader_get reads a program from guest memory: zeros past them */
static u8* ship_window(const u8* p, size_t avail) {
    u8* code = calloc(1, SH_CODE);
    if (code) memcpy(code, p, avail < SH_CODE ? avail : SH_CODE);
    return code;
}
/* the shader of code (hashing to h, as vk_shader_get hashes it) a draw asks for first: float attributes, textures
   and outputs, no capture, not early-Z (a fragment shader's first lookup, render.c), and the clip word of ship_clip.
   Translated into the shipped file unless it has it: SHIP_NEW, SHIP_KNOWN, SHIP_FAILED, or SHIP_STOP when the file
   takes no more */
static int ship_make(const u8* code, u64 h, int stage, u32 len, u64 qh, u32 len_a, u64 qh_a) {
    u32 mask[20] = {0};
    VkSh s = {0};
    size_t nw;
    u64 at = 0;
    const ShipIdx* e;
    mask[6] = ship_clip(stage);
    e = idx_slot(h, stage, mask[6], len, qh, len_a, qh_a);
    if (e && e->at) return SHIP_KNOWN;
    u32* spv = sh_spirv(&s, code, h, stage, mask, len, qh, len_a, qh_a, NULL, ship_no_read, 1, &nw);
    int translated = spv != NULL;
    if (spv) at = pf_shader_into(&g_pf.ship, &s, 0, spv, nw, NULL, 0, NULL);
    free(spv);
    free(s.cbr.r);
    /* An allocation failing inside the translator comes back as a program it can't translate, with no reason given,
       and is counted as failed, for this build and driver. Taking every failure without a reason as one to try again
       would walk the files again on every run of a game with a program that fails so */
    if (!translated) {
        LOG("vulkan: %s %016llx not translated: %s\n", shipped_stage_name(stage), (unsigned long long)h,
            s.si.first_unsupported[0] ? s.si.first_unsupported : "unsupported feature or float controls");
        return SHIP_FAILED;
    }
    if (!at) return SHIP_STOP;
    idx_add(h, stage, mask[6], len, qh, len_a, qh_a, at);
    __atomic_add_fetch(&g_pf.ship_new, 1, __ATOMIC_RELAXED);
    return SHIP_NEW;
}
/* A VertexA with the VertexB after it, merged as a draw that binds the two merges them (vk_shader_get): the VertexB is
   the first SPH of one in the 256-byte slots past A's code, where NVN's compiler puts it (docs/NVN_HLE.md). A
   VertexA without one, or that won't merge, is SHIP_SKIPPED: no draw asks for it alone */
static int ship_dual(const u8* sph, size_t avail) {
    u8 *a = ship_window(sph, avail), *b = NULL, *m = NULL;
    u32 len_a = 0, len = 0, mlen;
    size_t at;
    int rc = SHIP_SKIPPED;
    u64 qh_a;
    if (!a) return SHIP_LACKED;
    vk_shader_hash(a, SH_CODE, &len_a, ship_no_read);
    qh_a = vk_hash(a, len_a);
    for (at = ((size_t)len_a + 255) & ~(size_t)255; at < SH_CODE && avail >= 0x50 && at <= avail - 0x50; at += 256)
        if (shipped_sph_stage(sph + at) == ST_VS) break;
    if (at < SH_CODE && avail >= 0x50 && at <= avail - 0x50) {
        b = ship_window(sph + at, avail - at);
        m = malloc(SH_CODE);
        if (!b || !m)
            rc = SHIP_LACKED;
        else {
            vk_shader_hash(b, SH_CODE, &len, ship_no_read);
            if (sh_merge_dual(a, b, m, ship_no_read)) {
                u64 h = vk_shader_hash(m, SH_CODE, &mlen, ship_no_read);
                rc = ship_make(m, h, ST_VS, len, vk_hash(b, len), len_a, qh_a);
            }
        }
    }
    free(m);
    free(b);
    free(a);
    return rc;
}
static int ship_program(void* ctx, const u8* sph, size_t avail, int stage) {
    u8* code;
    u32 len;
    u64 h;
    int rc;
    (void)ctx;
    if (stage == SHIPPED_VERTEX_A) return ship_dual(sph, avail);
    /* their keys hold the draw's state (geometry's input primitive and passthrough words, the evaluation's domain), so
       no key made without a draw is one a draw asks for */
    if (stage == ST_GS || stage == ST_TES) return SHIP_SKIPPED;
    if (!(code = ship_window(sph, avail))) return SHIP_LACKED;
    h = vk_shader_hash(code, SH_CODE, &len, ship_no_read);
    rc = ship_make(code, h, stage, len, vk_hash(code, len), 0, 0);
    free(code);
    return rc;
}
static int ship_scanned(void* ctx, u32 what, u64 id) {
    (void)ctx;
    return scan_has(what, id);
}
static void ship_mark(void* ctx, u32 what, u64 id) {
    (void)ctx;
    pf_scan(what == SHIP_NONE ? &g_pf.scan : &g_pf.ship, what, id);
    scan_add(what, id);
}
/* the shipped file takes no more (a write failed, or it is full): the pass's translations would be thrown away. The
   pass alone appends to it, so its f is this thread's to read */
static int ship_stopped(void* ctx) {
    (void)ctx;
    return !g_pf.ship.f;
}
static void ship_note(void* ctx, const char* line, int shown) {
    (void)ctx;
    if (shown)
        pf_note("%s", line);
    else
        LOG("vulkan: %s\n", line);
}
/* on the reading thread, once the files were read: the programs the game ships translated into the shipped file. The
   draws translate their own meanwhile, and find these once it ends (ship_ready) */
static void pf_ship_pass(void) {
    static const ShipSink sink = {NULL, ship_scanned, ship_mark, ship_program, ship_stopped, ship_note};
    u64 t0 = plat_ns();
    pf_unseal(&g_pf.ship);
    /* a file that takes no more (its limit, which the smallest record would pass): every program would be counted,
       read and translated only to be thrown away, on every run */
    if (g_pf.ship.f && g_pf.ship.size + PF_REC + sizeof(PfShader) + 32 > (g_pf.ship.max ? g_pf.ship.max : PF_MAX))
        LOG("vulkan: %s is full, so the game's own shaders aren't looked for\n", g_pf.ship_path);
    else if (g_pf.ship.f) {
        char key[64];
        snprintf(key, sizeof key, "walk=%s", ship_walk_version());
        pf_open_mapped(&g_pf.scan, g_pf.scan_path, key, pf_scan_rec, NULL);
        pf_unseal(&g_pf.scan);
        __atomic_store_n(&g_pf.scanning, 1, __ATOMIC_RELAXED);
        int rc = ship_walk(&sink, &g_pf.progress, g_settings.prepare_shaders == 2);
        __atomic_store_n(&g_pf.scanning, 0, __ATOMIC_RELAXED);
        pf_close(&g_pf.scan);
        LOG("vulkan: the game's own shaders: %u translated new, %u in %s, in %llu ms%s\n",
            __atomic_load_n(&g_pf.ship_new, __ATOMIC_RELAXED), g_pf.nidx, g_pf.ship_path,
            (unsigned long long)((plat_ns() - t0) / 1000000),
            rc > 0   ? " (stopped: the file takes no more)"
            : rc < 0 ? " (a later run reads what wasn't)"
                     : "");
    } /* else pf_open_mapped or pf_unseal said why the file can't be written */
    pf_seal(&g_pf.ship); /* mapped again with the records appended, for the draws to read */
}
void vk_pipeline_prepare(RendererPrepare* p) {
    int loaded;
    *p = (RendererPrepare){0};
    if (!g_pf.on) {
        p->done = 1;
        return;
    }
    pm_lock(&g_pf.lock);
    loaded = g_pf.loaded;
    pm_unlock(&g_pf.lock);
    p->files = __atomic_load_n(&g_pf.progress.files, __ATOMIC_RELAXED);
    p->files_seen = __atomic_load_n(&g_pf.progress.files_seen, __ATOMIC_RELAXED);
    p->programs = __atomic_load_n(&g_pf.progress.programs, __ATOMIC_RELAXED);
    p->programs_done = __atomic_load_n(&g_pf.progress.programs_done, __ATOMIC_RELAXED);
    p->shipped = __atomic_load_n(&g_pf.ship_new, __ATOMIC_RELAXED);
    p->scanning = __atomic_load_n(&g_pf.scanning, __ATOMIC_RELAXED);
    p->shaders = __atomic_load_n(&g_pf.shaders_total, __ATOMIC_RELAXED);
    p->pipes = __atomic_load_n(&g_pf.pipes_total, __ATOMIC_RELAXED);
    p->pipes_made = (u32)__atomic_load_n(&g_pf.made, __ATOMIC_RELAXED);
    p->loaded = loaded;
    p->done = loaded && __atomic_load_n(&g_pf.ship_ready, __ATOMIC_ACQUIRE) && p->pipes_made >= p->pipes;
    pm_lock(&g_pf.note_lock);
    p->notes = g_pf.nnote < RENDERER_NOTES ? (int)g_pf.nnote : RENDERER_NOTES;
    for (int i = 0; i < p->notes; i++) /* oldest first */
        memcpy(p->note[i], g_pf.note[(g_pf.nnote - (u32)p->notes + (u32)i) % RENDERER_NOTES], sizeof p->note[i]);
    pm_unlock(&g_pf.note_lock);
}

/* the reading thread (arg non-NULL), or vk_pipeline_init's when there is none, which doesn't run the pass: the
   shipped file first, whose records the main file's PF_SHIPREFs name, then the main file, then the pass */
static void pf_load(void* arg) {
    PfLoad load = {pf_load_shader, pf_load_pipe, pf_load_shipref, pf_load_source, NULL, 0};
    char key[1024];
    size_t guest;
    u32 loaded[2] = {0, 0};
    u64 t0 = plat_ns();
    load.ctx = &load; /* pf_load_pipe reads rebuild */
    SNX_ZONE(z_load, "pipeline file load");
    pf_key(key, sizeof key, &guest);
    g_pf.ship.max = SHIP_MAX;
    pf_open_mapped(&g_pf.ship, g_pf.ship_path, key, pf_ship_rec, NULL);
    pf_open(&g_pf.file, g_pf.path, key, guest, &load, loaded);
    LOG("vulkan: %u shaders and %u pipelines of earlier runs loaded from %s in %llu ms, the pipelines to be made in "
        "the background; %u shaders of the game's own programs in %s\n",
        g_pf.nsh, g_pf.npipe, g_pf.path, (unsigned long long)((plat_ns() - t0) / 1000000), g_pf.nidx, g_pf.ship_path);
    __atomic_store_n(&g_pf.shaders_total, g_pf.nsh, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pf.pipes_total, g_pf.npipe, __ATOMIC_RELAXED);
    pm_lock(&g_pf.lock);
    g_pf.loaded = 1;
    pc_broadcast(&g_pf.done);
    pm_unlock(&g_pf.lock);
    if (arg && g_settings.prepare_shaders) pf_ship_pass(); /* translating needs this thread's stack */
    ship_release();
    __atomic_store_n(&g_pf.ship_ready, 1, __ATOMIC_RELEASE); /* the index and the view no longer change */
}
static void pf_load_thread(void* arg) {
    SNX_THREAD_NAME("pipeline load");
    pf_load(arg);
}
/* at the end of vk_pipeline_init: the files are read on a thread of its own, so startup doesn't wait for them */
static void pf_start_load(void) {
    pm_init(&g_pf.file.lock);
    pm_init(&g_pf.ship.lock);
    pm_init(&g_pf.scan.lock);
    pm_init(&g_pf.note_lock);
    pm_init(&g_pf.lock);
    pc_init(&g_pf.done);
    if (vk_corpus_dir()) {
        LOG("vulkan: shaders are recorded (shader_corpus or shader_captures), so every one is translated and none is "
            "kept for the next run\n");
        return;
    }
    g_pf.on = 1;
    null_fs(); /* made here, before the reading thread uses it */
    snprintf(g_pf.path, sizeof g_pf.path, "%s/%s.vkshaders", g_cache_dir, g_exename);
    snprintf(g_pf.ship_path, sizeof g_pf.ship_path, "%s/%s.vkshipped", g_cache_dir, g_exename);
    snprintf(g_pf.scan_path, sizeof g_pf.scan_path, "%s/%s.vkscan", g_cache_dir, g_exename);
    snx_path_native(g_pf.path);
    snx_path_native(g_pf.ship_path);
    snx_path_native(g_pf.scan_path);
    if (g_settings.prepare_shaders) ship_snapshot(); /* before the game's first thread writes its data */
    /* the translator's recursion wants the GPU thread's stack */
    if (plat_thread(pf_load_thread, &g_pf, (size_t)16 << 20)) pf_load(NULL); /* no thread: read here */
}
/* the reading thread's shaders and pipelines into the tables, waiting for it the first time a draw needs them
   (vk_lock). Not for the pass, which may run on long after */
static void pf_merge(void) {
    VkSh* s;
    Pipe* p;
    if (g_pf.merged) return;
    g_pf.merged = 1;
    if (!g_pf.on) return;
    gpu_wait_note("the shaders and pipelines of earlier runs read", 0);
    pm_lock(&g_pf.lock);
    while (!g_pf.loaded) pc_wait(&g_pf.done, &g_pf.lock, -1);
    pm_unlock(&g_pf.lock);
    gpu_wait_note(NULL, 0);
    /* in the file's order, so the last made is found first, as in the run that made them */
    while ((s = g_pf.sh) != NULL) {
        g_pf.sh = s->next;
        s->next = g_shh[s->hash & 1023];
        g_shh[s->hash & 1023] = s;
    }
    g_pf.sh_tail = NULL;
    while ((p = g_pf.pipes) != NULL) {
        u32 b = (u32)(p->h ^ (p->h >> 32)) & (PIPEH - 1);
        g_pf.pipes = p->next;
        p->next = g_pipeh[b];
        g_pipeh[b] = p;
    }
}
void vk_pipeline_prewarm_stats(u64* made, u64* hits, u64* left) {
    *made = __atomic_load_n(&g_pf.made, __ATOMIC_RELAXED);
    *hits = __atomic_load_n(&g_pf.hits, __ATOMIC_RELAXED);
    *left = g_pf.merged && g_pf.npipe > *made ? g_pf.npipe - *made : 0; /* npipe is settled once merged */
}
