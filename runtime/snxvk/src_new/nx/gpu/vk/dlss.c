/* SwitchRecomp local addition (2026-10-18), part of the vendored ScarletNX renderer build (GPL-3.0-or-later).
   DLSS / DLAA through NVIDIA NGX, loaded at run time from the graphics driver's NGX core (_nvngx.dll, found through
   HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore). No NVIDIA SDK code or header is compiled in: the few NGX types
   used are declared here, and the DLSS model itself (nvngx_dlss.dll) is loaded by the driver from the game folder.

   The core's exports take the signatures NVIDIA's own standalone loader uses for it (nvsdk_ngx_standalone_*.h), not
   the SDK's static-library wrappers. Its parameter object is a C++ class: its virtual functions are called through the
   MSVC vtable, Set overloads in slots 0-7 and Get overloads in 8-15, each group in reverse declaration order (MSVC's
   layout for overloads); vk_dlss_probe checks that layout with a set-then-get round trip before anything relies on it. */
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
/* Every command here goes through the recording thread's ring (record.c rec_Cmd*: the same arguments, recorded in
   order with the frame, or made directly when the thread is off), so TAA, ambient occlusion, the motion vectors and
   ReShade's depth copy no longer need the thread off: with it off Native Vulkan lost ~40% of its frame rate at 240.
   DLSS still does (NGX writes into the command buffer itself, snxvk_glue.c) */
#define vkCmdBeginRendering rec_CmdBeginRendering
#define vkCmdEndRendering rec_CmdEndRendering
#define vkCmdBeginTransformFeedbackEXT rec_CmdBeginTransformFeedbackEXT
#define vkCmdEndTransformFeedbackEXT rec_CmdEndTransformFeedbackEXT
#define vkCmdBindTransformFeedbackBuffersEXT rec_CmdBindTransformFeedbackBuffersEXT
#define vkCmdBindDescriptorSets rec_CmdBindDescriptorSets
#define vkCmdBindPipeline rec_CmdBindPipeline
#define vkCmdBindVertexBuffers rec_CmdBindVertexBuffers
#define vkCmdBlitImage rec_CmdBlitImage
#define vkCmdClearColorImage rec_CmdClearColorImage
#define vkCmdCopyImage rec_CmdCopyImage
#define vkCmdDispatch rec_CmdDispatch
#define vkCmdDraw rec_CmdDraw
#define vkCmdPipelineBarrier2 rec_CmdPipelineBarrier2
#define vkCmdPushConstants rec_CmdPushConstants
#define vkCmdSetScissor rec_CmdSetScissor
#define vkCmdSetViewport rec_CmdSetViewport
#ifndef SWR_NO_NGX
extern char g_exedir[1024];
#endif

#ifndef SWR_NO_NGX /* SWR_NO_NGX (public release, Linux): no NGX code at all; scene point, motion vectors, TAA and
                      the ReShade depth below still work */
typedef int NgxResult; /* NVSDK_NGX_Result: 1 success, 0xBAD00000 | n failures */
#define NGX_OK(r) (((r) & 0xFFF00000) != 0xBAD00000)
#define NGX_VERSION_API 0x15
#define NGX_FEATURE_SUPERSAMPLING 1
typedef struct NgxParam NgxParam; /* NVSDK_NGX_Parameter */
typedef struct NgxHandle NgxHandle;
typedef struct NgxParamVtbl { void* slot[17]; } NgxParamVtbl;
struct NgxParam { const NgxParamVtbl* vt; };
/* NVSDK_NGX_ImageViewInfo_VK / NVSDK_NGX_Resource_VK */
typedef struct {
    VkImageView ImageView;
    VkImage Image;
    VkImageSubresourceRange SubresourceRange;
    VkFormat Format;
    unsigned int Width, Height;
} NgxImageView;
typedef struct {
    union { NgxImageView ImageViewInfo; struct { VkBuffer Buffer; unsigned int SizeInBytes; } BufferInfo; } Resource;
    int Type; /* 0 image view, 1 buffer */
    _Bool ReadWrite;
} NgxResourceVK;

typedef NgxResult (*PFN_Init_Ext2)(unsigned long long app, const wchar_t* path, VkInstance, VkPhysicalDevice, VkDevice,
                                   PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int version, const NgxParam*);
typedef NgxResult (*PFN_ReqExt)(unsigned* ni, const char*** ie, unsigned* nd, const char*** de);
typedef NgxResult (*PFN_GetParams)(NgxParam**);
typedef NgxResult (*PFN_DestroyParams)(NgxParam*);
typedef NgxResult (*PFN_Create)(VkCommandBuffer, int feature, NgxParam*, NgxHandle**);
typedef NgxResult (*PFN_Evaluate)(VkCommandBuffer, const NgxHandle*, const NgxParam*, void* callback);
typedef NgxResult (*PFN_Release)(NgxHandle*);
typedef NgxResult (*PFN_Shutdown)(void);

static struct {
    HMODULE core;
    PFN_Init_Ext2 init;
    PFN_ReqExt req_ext;
    PFN_GetParams caps, alloc_params;
    PFN_DestroyParams destroy_params;
    PFN_Create create;
    PFN_Evaluate evaluate;
    PFN_Release release;
    PFN_Shutdown shutdown;
    int tried, loaded, up, available;
} g_ngx;

static void xfb_ext(VkExtensionProperties* have, u32 n, const char** ext, u32* next, u32 max);
static void mv_frame(int evaluated);
/* the INI's dlss= (0 off, 1 DLAA, 2 quality, 3 balanced, 4 performance), from runtime/snxvk_glue.c */
extern int snxvk_dlss_mode;
int vk_dlss_wanted(void);
int vk_dlss_wanted(void) { return snxvk_dlss_mode > 0 || getenv("SWR_DLSS_PROBE") != NULL; }

/* ---- the parameter object's virtual functions */
typedef void (*SetU64)(NgxParam*, const char*, unsigned long long);
typedef void (*SetF)(NgxParam*, const char*, float);
typedef void (*SetD)(NgxParam*, const char*, double);
typedef void (*SetU32)(NgxParam*, const char*, unsigned);
typedef void (*SetI)(NgxParam*, const char*, int);
typedef void (*SetP)(NgxParam*, const char*, void*);
typedef NgxResult (*GetU64)(const NgxParam*, const char*, unsigned long long*);
typedef NgxResult (*GetF)(const NgxParam*, const char*, float*);
typedef NgxResult (*GetU32)(const NgxParam*, const char*, unsigned*);
typedef NgxResult (*GetI)(const NgxParam*, const char*, int*);
typedef NgxResult (*GetP)(const NgxParam*, const char*, void**);
/* MSVC: each overload group in reverse declaration order */
enum { S_P = 0, S_D12, S_D11, S_I, S_U32, S_D, S_F, S_U64, G_P, G_D12, G_D11, G_I, G_U32, G_D, G_F, G_U64, RESET };
static void p_seti(NgxParam* p, const char* n, int v) { ((SetI)p->vt->slot[S_I])(p, n, v); }
static void p_setu(NgxParam* p, const char* n, unsigned v) { ((SetU32)p->vt->slot[S_U32])(p, n, v); }
static void p_setf(NgxParam* p, const char* n, float v) { ((SetF)p->vt->slot[S_F])(p, n, v); }
static void p_setp(NgxParam* p, const char* n, void* v) { ((SetP)p->vt->slot[S_P])(p, n, v); }
static int p_geti(const NgxParam* p, const char* n, int* v) { return NGX_OK(((GetI)p->vt->slot[G_I])(p, n, v)); }
static int p_getu(const NgxParam* p, const char* n, unsigned* v) { return NGX_OK(((GetU32)p->vt->slot[G_U32])(p, n, v)); }
static int p_getf(const NgxParam* p, const char* n, float* v) { return NGX_OK(((GetF)p->vt->slot[G_F])(p, n, v)); }
static int p_getp(const NgxParam* p, const char* n, void** v) { return NGX_OK(((GetP)p->vt->slot[G_P])(p, n, v)); }

static int core_load(void) {
    if (g_ngx.tried) return g_ngx.loaded;
    g_ngx.tried = 1;
    char dir[MAX_PATH] = {0}, path[MAX_PATH + 32];
    DWORD n = sizeof dir;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", "FullPath", RRF_RT_REG_SZ, NULL, dir,
                     &n) != ERROR_SUCCESS) {
        LOG("dlss: no NVIDIA NGX in the driver (registry NGXCore missing): DLSS unavailable\n");
        return 0;
    }
    snprintf(path, sizeof path, "%s\\_nvngx.dll", dir);
    g_ngx.core = LoadLibraryA(path);
    if (!g_ngx.core) {
        LOG("dlss: could not load %s (error %lu): DLSS unavailable\n", path, GetLastError());
        return 0;
    }
#define NGXFN(f, n) g_ngx.f = (void*)GetProcAddress(g_ngx.core, n)
    NGXFN(init, "NVSDK_NGX_VULKAN_Init_Ext2");
    NGXFN(req_ext, "NVSDK_NGX_VULKAN_RequiredExtensions");
    NGXFN(caps, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    NGXFN(alloc_params, "NVSDK_NGX_VULKAN_AllocateParameters");
    NGXFN(destroy_params, "NVSDK_NGX_VULKAN_DestroyParameters");
    NGXFN(create, "NVSDK_NGX_VULKAN_CreateFeature");
    NGXFN(evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature");
    NGXFN(release, "NVSDK_NGX_VULKAN_ReleaseFeature");
    NGXFN(shutdown, "NVSDK_NGX_VULKAN_Shutdown");
#undef NGXFN
    if (!g_ngx.init || !g_ngx.req_ext || !g_ngx.caps || !g_ngx.alloc_params || !g_ngx.create || !g_ngx.evaluate ||
        !g_ngx.release) {
        LOG("dlss: the driver's NGX core lacks Vulkan entry points: DLSS unavailable\n");
        return 0;
    }
    g_ngx.loaded = 1;
    LOG("dlss: NGX core %s\n", path);
    return 1;
}

static int has_name(const char* const* list, u32 n, const char* s) {
    for (u32 i = 0; i < n; i++)
        if (!strcmp(list[i], s)) return 1;
    return 0;
}

/* create_instance / create_device add NGX's extensions to theirs (ext has room for max) */
void vk_dlss_instance_exts(const char** ext, u32* next, u32 max);
void vk_dlss_instance_exts(const char** ext, u32* next, u32 max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!vk_dlss_wanted() || !core_load() || !NGX_OK(g_ngx.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < ni; i++)
        if (!has_name(ext, *next, ie[i]) && *next < max) ext[(*next)++] = ie[i];
}
/* only extensions the device has: one it lacks would fail the whole device */
void vk_dlss_device_exts(VkPhysicalDevice pd, const char** ext, u32* next, u32 max);
void vk_dlss_device_exts(VkPhysicalDevice pd, const char** ext, u32* next, u32 max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    extern int g_opt_taa;
    int ngx = vk_dlss_wanted() && core_load() && NGX_OK(g_ngx.req_ext(&ni, &ie, &nd, &de));
    if (!ngx) nd = 0;
    if (!ngx && !g_opt_taa) return; /* TAA needs only the motion vectors' transform feedback */
    u32 n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties* have = calloc(n ? n : 1, sizeof *have);
    if (!have) return;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, have);
    for (unsigned i = 0; i < nd; i++) {
        int ok = 0;
        for (u32 k = 0; k < n && !ok; k++) ok = !strcmp(have[k].extensionName, de[i]);
        if (!ok) LOG("dlss: device lacks %s (NGX asked for it)\n", de[i]);
        else if (!has_name(ext, *next, de[i]) && *next < max) ext[(*next)++] = de[i];
    }
    xfb_ext(have, n, ext, next, max); /* motion vectors */
    free(have);
}

/* the parameter vtable layout, checked on a scratch parameter object */
static int vtable_ok(void) {
    NgxParam* p = NULL;
    if (!NGX_OK(g_ngx.alloc_params(&p)) || !p) return 0;
    int i = 0;
    unsigned u = 0;
    float f = 0;
    void* v = NULL;
    p_seti(p, "SWR.Test.I", -12345);
    p_setu(p, "SWR.Test.U", 4000000000u);
    p_setf(p, "SWR.Test.F", 1.25f);
    p_setp(p, "SWR.Test.P", (void*)p);
    int ok = p_geti(p, "SWR.Test.I", &i) && i == -12345 && p_getu(p, "SWR.Test.U", &u) && u == 4000000000u &&
             p_getf(p, "SWR.Test.F", &f) && f == 1.25f && p_getp(p, "SWR.Test.P", &v) && v == (void*)p;
    if (g_ngx.destroy_params) g_ngx.destroy_params(p);
    if (!ok) LOG("dlss: NGX parameter layout check failed (i=%d u=%u f=%g p=%p): DLSS off\n", i, u, f, v);
    return ok;
}

/* after the device: NGX up, DLSS availability. 0 = no DLSS this run */
int vk_dlss_init(void);
int vk_dlss_init(void) {
    if (!vk_dlss_wanted() || !core_load()) return 0;
    wchar_t wdir[1024];
    MultiByteToWideChar(CP_UTF8, 0, g_exedir, -1, wdir, 1024);
    /* 231313132: the application id NVIDIA's DLSS samples use for development builds */
    NgxResult r = g_ngx.init(231313132ull, wdir, g_vk.instance, g_vk.phys, g_vk.dev, vkGetInstanceProcAddr,
                             vkGetDeviceProcAddr, NGX_VERSION_API, NULL);
    if (!NGX_OK(r)) {
        LOG("dlss: NGX init failed (%08x)\n", (unsigned)r);
        return 0;
    }
    g_ngx.up = 1;
    if (!vtable_ok()) return 0;
    NgxParam* caps = NULL;
    int avail = 0, needs_driver = 0;
    if (NGX_OK(g_ngx.caps(&caps)) && caps) {
        p_geti(caps, "SuperSampling.Available", &avail);
        p_geti(caps, "SuperSampling.NeedsUpdatedDriver", &needs_driver);
    }
    g_ngx.available = avail != 0;
    LOG("dlss: NGX up; DLSS %s%s\n", avail ? "available" : "NOT available",
        needs_driver ? " (the driver needs an update)" : "");
    return g_ngx.available;
}
#else
static struct { int available; } g_ngx;
typedef struct NgxParam NgxParam; /* only as pointers in g_dl */
typedef struct NgxHandle NgxHandle;
static void xfb_ext(VkExtensionProperties* have, u32 n, const char** ext, u32* next, u32 max);
static void mv_frame(int evaluated);
extern int snxvk_dlss_mode;
int vk_dlss_wanted(void);
int vk_dlss_wanted(void) { return 0; }
static int has_name(const char* const* list, u32 n, const char* s) {
    for (u32 i = 0; i < n; i++)
        if (!strcmp(list[i], s)) return 1;
    return 0;
}
void vk_dlss_instance_exts(const char** ext, u32* next, u32 max);
void vk_dlss_instance_exts(const char** ext, u32* next, u32 max) { (void)ext; (void)next; (void)max; }
void vk_dlss_device_exts(VkPhysicalDevice pd, const char** ext, u32* next, u32 max);
void vk_dlss_device_exts(VkPhysicalDevice pd, const char** ext, u32* next, u32 max) {
    extern int g_opt_taa;
    if (!g_opt_taa) return;
    u32 n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties* have = calloc(n ? n : 1, sizeof *have);
    if (!have) return;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, have);
    xfb_ext(have, n, ext, next, max); /* motion vectors */
    free(have);
}
int vk_dlss_init(void);
int vk_dlss_init(void) { return 0; }
#endif

/* ---------------------------------------------------------------- where DLSS runs in Dread's frame
   Measured (SNX_FRAME_MAP, 2026-10-18, a combat room and the save room): the lit scene goes from the lighting buffer
   through a composite pass into a full-size R11G11B10F "scene" target (transparents, when a room has them, are drawn
   into it too); the post effects read that target into another full-size R11G11B10F one; bloom then starts at 1/5
   size and the HUD is drawn at 1280x720 and composited last. The rule that holds in both rooms: the scene is the last
   full-size R11G11B10F target read by a full-size R11G11B10F pass before the first small (<= 1/4 width) pass. It is
   never the lighting buffer (the composite after it samples the jittered depth and G-buffer: that shimmered on the
   save room floor) and never the post target (read only by the small bloom pass).
   The next frame runs DLSS on that address at its first read after it was drawn to, so post effects, bloom and the HUD
   get the anti-aliased image and the HUD is never touched. A different target has to win 30 frames in a row before
   DLSS moves to it (no flip-flopping between targets, each switch drops DLSS's history); a frame jittered without DLSS
   having run turns the jitter off for 60 frames (a raw jittered frame on screen is a visible shake). */
#define DL_FMT_R11G11B10F 0xE0
static struct {
    /* this frame */
    u64 reads[16];      /* full-size HDR targets this draw samples, before its pass is known */
    int nreads;
    u64 written[8];     /* full-size HDR targets drawn to this frame */
    int nwritten;
    u64 cand, cand_z, last_z, frame_best, frame_best_z;
    int small_seen, evaluated;
    u32 scene_w, scene_h;
    /* across frames */
    u64 stable, stable_z, chosen, chosen_z;
    int streak, missing;
    int done;
    /* resources */
    u32 w, h;
    VkImage mv, out;
    VmaAllocation mv_mem, out_mem;
    VkImageView mv_view, out_view;
    VkImage zimg;
    VkImageView zview;
    NgxHandle* feat;
    NgxParam* params;
    u32 fw, fh;
    int reset, evals, failed, switches;
} g_dl;
static struct { int seen, done; } g_aamb; /* AO on the ambient light this frame (below, evaluate) */

/* The scene point serves DLSS, TAA (ini taa=1, when DLSS is not running) and ReShade's depth (runtime/reshade_gl.c) */
extern int g_opt_taa;
extern int reshade_vk_on(void);
extern void reshade_set_view(unsigned long long view);
static int g_taa_failed;
static int dl_dlss(void) { return g_ngx.available && snxvk_dlss_mode > 0 && !g_dl.failed; }
static int dl_taa(void) { return g_opt_taa && !g_taa_failed && !dl_dlss(); }
static int dl_aa(void) { return dl_dlss() || dl_taa(); }           /* jitter and motion vectors */
/* ReShade's depth copy is recorded directly into the frame: only with the recording thread off (snxvk_glue.c turns it
   off when ReShade's Vulkan layer is installed) */
static int dl_reshade(void) { return !vk_rec_threaded() && reshade_vk_on(); }
static int dl_ao(void); /* XeGTAO, below */
static int dl_active(void) { return dl_aa() || dl_reshade() || dl_ao(); } /* finding the scene target */

static int full_hdr(const Surf* s) {
    return s && (s->fkey & 0xFFFF) == DL_FMT_R11G11B10F && g_dl.scene_w && s->w == g_dl.scene_w && s->h == g_dl.scene_h;
}
static int written_now(u64 gva) {
    for (int i = 0; i < g_dl.nwritten; i++)
        if (g_dl.written[i] == gva) return 1;
    return 0;
}

/* render.c, every draw once its pass is open: what it writes, and the pending reads attributed to it */
void vk_dlss_draw(Surf* rt0, Surf* zt, int depth_test);
void vk_dlss_draw(Surf* rt0, Surf* zt, int depth_test) {
    (void)depth_test;
    if (!dl_active()) return;
    if (zt && zt->f.depth && rt0 && rt0->w == zt->w && rt0->h == zt->h && zt->w >= 640) { /* the scene's depth */
        g_dl.last_z = zt->gva;
        g_dl.scene_w = zt->w;
        g_dl.scene_h = zt->h;
    }
    if (full_hdr(rt0)) {
        if (!written_now(rt0->gva) && g_dl.nwritten < 8) g_dl.written[g_dl.nwritten++] = rt0->gva;
        for (int i = 0; i < g_dl.nreads; i++)
            if (g_dl.reads[i] != rt0->gva) {
                g_dl.cand = g_dl.reads[i];
                g_dl.cand_z = g_dl.last_z;
            }
    }
    g_dl.nreads = 0;
}

/* render.c, a new render pass: the first small one ends the scene part of the frame */
void vk_dlss_pass(Surf** rt, u32 n);
void vk_dlss_pass(Surf** rt, u32 n) {
    if (!dl_active() || g_dl.small_seen || !n || !rt[0] || !g_dl.scene_w) return;
    if (rt[0]->w * 4 <= g_dl.scene_w) {
        g_dl.small_seen = 1;
        g_dl.frame_best = g_dl.cand;
        /* SWR_DLSS_POINT=scene: before the post effects. Default (post): the target the small bloom pass reads, after
           the post effects, some of which depth-test against the jittered scene depth (measured: the save room floor
           still flickered with DLSS before them) */
        static int at_scene = -1;
        if (at_scene < 0) at_scene = getenv("SWR_DLSS_POINT") && !strcmp(getenv("SWR_DLSS_POINT"), "scene");
        if (!at_scene)
            for (int i = 0; i < g_dl.nreads; i++) g_dl.frame_best = g_dl.reads[i];
        g_dl.frame_best_z = g_dl.cand_z;
    }
}

/* renderer.c, each game present */
void vk_dlss_frame(void);
void vk_dlss_frame(void) {
    if (!dl_active()) return;
    if (g_dl.small_seen && g_dl.frame_best) {
        g_dl.missing = 0;
        if (g_dl.frame_best == g_dl.stable) {
            g_dl.streak = 0;
            if (g_dl.frame_best_z && g_dl.frame_best_z != g_dl.stable_z) { /* same colour target, another depth: follow it */
                static int n;
                if (n++ < 4) LOG("scene point (Vulkan): depth %llx -> %llx\n", (unsigned long long)g_dl.stable_z, (unsigned long long)g_dl.frame_best_z);
                g_dl.stable_z = g_dl.frame_best_z;
                g_dl.reset = 1;
            }
        } else if (!g_dl.stable || ++g_dl.streak >= 30) {
            g_dl.stable = g_dl.frame_best;
            g_dl.stable_z = g_dl.frame_best_z;
            g_dl.streak = 0;
            g_dl.reset = 1; /* a new scene target: no history to blend with */
            g_dl.switches++;
        }
    } else if (++g_dl.missing >= 30)
        g_dl.stable = 0; /* no scene for half a second: a menu, a video, a loading screen */
    g_dl.chosen = g_dl.stable;
    g_dl.chosen_z = g_dl.stable_z;
    mv_frame(g_dl.evaluated);
    g_dl.nwritten = g_dl.nreads = 0;
    g_dl.cand = g_dl.cand_z = 0;
    g_dl.frame_best = g_dl.frame_best_z = 0;
    g_dl.small_seen = 0;
    g_dl.done = 0;
    g_dl.evaluated = 0;
    g_aamb.seen = g_aamb.done = 0;
}

static int image2d(u32 w, u32 h, VkFormat f, VkImageUsageFlags usage, VkImage* img, VmaAllocation* mem, VkImageView* view) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = f,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ii, &ai, img, mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(g_vk.dev, &vi, NULL, view) == VK_SUCCESS;
}

static void free_images(void) {
    if (g_dl.mv_view) vkDestroyImageView(g_vk.dev, g_dl.mv_view, NULL);
    if (g_dl.out_view) vkDestroyImageView(g_vk.dev, g_dl.out_view, NULL);
    if (g_dl.mv) vmaDestroyImage(g_vk.vma, g_dl.mv, g_dl.mv_mem);
    if (g_dl.out) vmaDestroyImage(g_vk.vma, g_dl.out, g_dl.out_mem);
    g_dl.mv = g_dl.out = VK_NULL_HANDLE;
    g_dl.mv_view = g_dl.out_view = VK_NULL_HANDLE;
}

static int ensure_images(u32 w, u32 h) {
    if (g_dl.out && g_dl.w == w && g_dl.h == h) return 1;
    vk_finish(); /* the old images may still be in flight */
    free_images();
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!image2d(w, h, VK_FORMAT_R16G16_SFLOAT, u | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &g_dl.mv, &g_dl.mv_mem, &g_dl.mv_view) ||
        !image2d(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, u, &g_dl.out, &g_dl.out_mem, &g_dl.out_view)) {
        free_images();
        return 0;
    }
    g_dl.w = w;
    g_dl.h = h;
    g_dl.reset = 1;
    return 1;
}

static VkImageView depth_view(Surf* z) {
    if (g_dl.zimg == z->img && g_dl.zview) return g_dl.zview;
    if (g_dl.zview) {
        vk_finish();
        vkDestroyImageView(g_vk.dev, g_dl.zview, NULL);
        g_dl.zview = VK_NULL_HANDLE;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = z->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = z->f.vk,
                                .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &g_dl.zview) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_dl.zimg = z->img;
    return g_dl.zview;
}

#ifndef SWR_NO_NGX
static NgxResourceVK res_image(VkImageView v, VkImage img, VkFormat f, u32 w, u32 h, VkImageAspectFlags aspect, int rw) {
    NgxResourceVK r;
    memset(&r, 0, sizeof r);
    r.Resource.ImageViewInfo.ImageView = v;
    r.Resource.ImageViewInfo.Image = img;
    r.Resource.ImageViewInfo.SubresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    r.Resource.ImageViewInfo.Format = f;
    r.Resource.ImageViewInfo.Width = w;
    r.Resource.ImageViewInfo.Height = h;
    r.Type = 0;
    r.ReadWrite = rw;
    return r;
}

/* DLAA: DLSS at the render size. The upscaling modes would need the scene drawn smaller than the post-processing
   that follows it, and this renderer has one resolution scale for every target, some of which (the lighting buffer)
   serve both sides of the DLSS point: they run as DLAA. resolution= still sets the size DLAA works at */
static int quality_value(void) {
    static int said;
    if (snxvk_dlss_mode > 1 && !said) {
        said = 1;
        LOG("dlss: upscaling modes aren't available with this renderer: DLAA at the resolution= scale instead\n");
    }
    return 5; /* NVSDK_NGX_PerfQuality_Value_DLAA */
}

static void dl_fail(const char* what, NgxResult r) {
    LOG("dlss: %s failed (%08x): DLSS off for this run\n", what, (unsigned)r);
    g_dl.failed = 1;
}
#endif

/* ---------------------------------------------------------------- motion vectors and jitter
   Exact per-pixel motion, camera and objects alike: each depth-prepass draw of the scene (depth writes on, triangle
   list, no geometry/tessellation stage) has the game's own vertex positions captured by transform feedback. At the
   DLSS point the frame's captured draws are drawn again with dlss_mv.vert/.frag, this frame's positions with last
   frame's of the same draw (same vertex shader, buffers and draw parameters, the n-th such draw of the frame) as a
   second stream, depth test EQUAL against the scene depth: a pixel gets the motion of the surface the game shows
   there. Draws without a match last frame (new objects) and pixels without prepass geometry keep zero motion.
   Jitter: the scene draws' viewport moves by a sub-pixel Halton(2,3) offset, which shifts what they rasterize the way
   a jittered projection would; the deferred passes that read render targets by screen position are left alone. */
#include "nx/gpu/vk/dlss_mv_spv.h"
#define XFB_BYTES (64u << 20) /* per frame: 4M vertices */
#define XFB_MAX 2048          /* captured draws per frame */
typedef struct {
    u64 key;
    u32 occ, off, verts;
    VkViewport vp, vpj; /* unjittered and as drawn */
} XfbRec;
static struct {
    int ext, on; /* VK_EXT_transform_feedback asked for, enabled on the device */
    VkBuffer buf[2];
    VmaAllocation mem[2];
    XfbRec rec[2][XFB_MAX];
    u32 nrec[2], used[2], cur;
    int capturing;
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkFormat pipe_depth;
    int reads_rt; /* the draw being set up samples a render target */
    float jx, jy; /* this frame's jitter, render pixels */
    u32 jframe;
    int jit_ok, jit_used, hold, misses;
} g_mv;

int vk_dlss_xfb_wanted(void);
int vk_dlss_xfb_wanted(void) { return g_mv.on && (snxvk_dlss_mode > 0 || g_opt_taa); }

static VkPhysicalDeviceTransformFeedbackFeaturesEXT g_xfb_feat;
const void* vk_dlss_device_chain(const void* next);
const void* vk_dlss_device_chain(const void* next) {
    if (!g_mv.ext) return next;
    if (g_vk.has_xfb) { /* the renderer enables transform feedback itself (games that use it): a second feature
                           struct of the same type in the chain fails the device */
        g_mv.on = 1;
        return next;
    }
    VkPhysicalDeviceTransformFeedbackFeaturesEXT q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
    if (!q.transformFeedback) {
        LOG("dlss: no transform feedback: no motion vectors\n");
        g_mv.ext = 0;
        return next;
    }
    g_xfb_feat = (VkPhysicalDeviceTransformFeedbackFeaturesEXT){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, .pNext = (void*)next, .transformFeedback = 1};
    g_mv.on = 1;
    return &g_xfb_feat;
}
/* called from vk_dlss_device_exts: transform feedback rides with the NGX extensions */
static void xfb_ext(VkExtensionProperties* have, u32 n, const char** ext, u32* next, u32 max) {
    for (u32 k = 0; k < n; k++)
        if (!strcmp(have[k].extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) {
            if (!has_name(ext, *next, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) && *next < max)
                ext[(*next)++] = VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME;
            g_mv.ext = 1;
            return;
        }
}

static int xfb_buffers(void) {
    if (g_mv.buf[0]) return 1;
    for (int i = 0; i < 2; i++) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = XFB_BYTES,
                                 .usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_mv.buf[i], &g_mv.mem[i], NULL) != VK_SUCCESS) {
            LOG("dlss: no memory for the motion-vector capture: no motion vectors\n");
            g_mv.on = 0;
            return 0;
        }
    }
    return 1;
}

static float halton(u32 i, u32 b) {
    float f = 1, r = 0;
    for (; i; i /= b) {
        f /= (float)b;
        r += f * (float)(i % b);
    }
    return r;
}

/* render.c: a draw starts being set up / it samples a render target */
void vk_dlss_draw_begin(void);
void vk_dlss_draw_begin(void) { g_mv.reads_rt = 0; g_dl.nreads = 0; } /* reads belong to the draw being set up */
void vk_dlss_rt_read(void);
void vk_dlss_rt_read(void) { g_mv.reads_rt = 1; }

static int scene_draw(Surf* zt) {
    return dl_aa() && g_dl.chosen && !g_dl.done && zt && zt->gva == g_dl.chosen_z;
}

/* render.c, before the viewport is set: scene geometry moves by the frame's jitter. Draws that write depth or test it
   EQUAL (the prepass and the G-buffer, which must agree exactly) always do; others unless they read render targets
   (deferred lighting and screen-space passes, which sample by screen position) */
void vk_dlss_jitter(Surf* zt, int depth_write, int depth_equal, VkViewport* vp);
void vk_dlss_jitter(Surf* zt, int depth_write, int depth_equal, VkViewport* vp) {
    { static int nj = -1; if (nj < 0) nj = getenv("SWR_DLSS_NO_JITTER") != NULL; /* once: this runs at every draw */
      if (!scene_draw(zt) || nj) return; }
    { static int all = -1; if (all < 0) all = getenv("SWR_DLSS_JITTER_ALL") != NULL;
      if (!all && !depth_write && !depth_equal && g_mv.reads_rt) return; }
    if (!g_mv.jit_ok) return;
    vp->x += g_mv.jx;
    vp->y += g_mv.jy;
    g_mv.jit_used = 1;
}

/* render.c, right before the draw: capture its positions when it is a prepass draw of the scene */
int vk_dlss_capture_begin(VkCommandBuffer cb, Surf* zt, int depth_write, int plain_tris, u64 key, u32 verts,
                          const VkViewport* vp, const VkViewport* vpj);
int vk_dlss_capture_begin(VkCommandBuffer cb, Surf* zt, int depth_write, int plain_tris, u64 key, u32 verts,
                          const VkViewport* vp, const VkViewport* vpj) {
    if (!g_mv.on || !depth_write || !plain_tris || !scene_draw(zt) || verts < 3) return 0;
    u32 c = g_mv.cur, bytes = verts / 3 * 3 * 16;
    if (g_mv.nrec[c] >= XFB_MAX || g_mv.used[c] + bytes > XFB_BYTES || !xfb_buffers()) return 0;
    XfbRec* r = &g_mv.rec[c][g_mv.nrec[c]];
    r->key = key;
    r->occ = 0;
    for (u32 i = 0; i < g_mv.nrec[c]; i++)
        if (g_mv.rec[c][i].key == key) r->occ++;
    r->off = g_mv.used[c];
    r->verts = verts / 3 * 3;
    r->vp = *vp;
    r->vpj = *vpj;
    VkDeviceSize off = r->off, size = bytes;
    vkCmdBindTransformFeedbackBuffersEXT(cb, 0, 1, &g_mv.buf[c], &off, &size);
    vkCmdBeginTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
    g_mv.nrec[c]++;
    g_mv.used[c] += bytes;
    return 1;
}
void vk_dlss_capture_end(VkCommandBuffer cb);
void vk_dlss_capture_end(VkCommandBuffer cb) { vkCmdEndTransformFeedbackEXT(cb, 0, 0, NULL, NULL); }

static VkShaderModule module(const unsigned* code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g_vk.dev, &ci, NULL, &m);
    return m;
}

static int mv_pipeline(VkFormat depth) {
    if (g_mv.pipe && g_mv.pipe_depth == depth) return 1;
    if (g_mv.pipe) {
        vk_finish();
        vkDestroyPipeline(g_vk.dev, g_mv.pipe, NULL);
        g_mv.pipe = VK_NULL_HANDLE;
    }
    if (!g_mv.layout) {
        VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &pr};
        if (vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_mv.layout) != VK_SUCCESS) return 0;
    }
    VkShaderModule vs = module(g_dlss_mv_vert, sizeof g_dlss_mv_vert), fs = module(g_dlss_mv_frag, sizeof g_dlss_mv_frag);
    if (!vs || !fs) return 0;
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkVertexInputBindingDescription vb[2] = {{0, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 16, VK_VERTEX_INPUT_RATE_VERTEX}};
    VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_FALSE,
                                                .depthCompareOp = VK_COMPARE_OP_EQUAL};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &cba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat color = VK_FORMAT_R16G16_SFLOAT;
    int stencil = depth == VK_FORMAT_D24_UNORM_S8_UINT || depth == VK_FORMAT_D32_SFLOAT_S8_UINT || depth == VK_FORMAT_D16_UNORM_S8_UINT;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &color, .depthAttachmentFormat = depth,
                                        .stencilAttachmentFormat = stencil ? depth : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
                                       .layout = g_mv.layout};
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &g_mv.pipe);
    vkDestroyShaderModule(g_vk.dev, vs, NULL);
    vkDestroyShaderModule(g_vk.dev, fs, NULL);
    if (r != VK_SUCCESS) return 0;
    g_mv.pipe_depth = depth;
    return 1;
}

/* the frame's captured draws again into the motion-vector image; 0 when there was nothing to draw */
static u32 mv_pass(VkCommandBuffer cb, Surf* z, u32 w, u32 h) {
    u32 c = g_mv.cur, p = c ^ 1, drawn = 0;
    if (!g_mv.on || !g_mv.nrec[c] || !mv_pipeline(z->f.vk)) return 0;
    { static int said; if (said < 3) { said++;
        LOG("dlss: motion-vector pass %ux%u, depth %llx image %ux%u scaled %ux%u layers %u format %d stencil %d samples-type %d\n", w, h,
            (unsigned long long)z->gva, z->w, z->h, z->sw, z->sh, z->layers, (int)z->f.vk, z->f.stencil, (int)z->type); } }
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                           .dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    vk_barrier(cb, g_dl.mv, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vk_surf_use(cb, z, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_dl.mv_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo za = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = z->att,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_NONE};
    int stencil = z->f.stencil;
    VkRenderingInfo rinfo = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                             .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &za,
                             .pStencilAttachment = stencil ? &za : NULL};
    vkCmdBeginRendering(cb, &rinfo);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mv.pipe);
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    static int skipdraw = -1;
    if (skipdraw < 0) skipdraw = getenv("SWR_DLSS_MV_SKIPDRAW") != NULL;
    for (u32 i = 0; i < (skipdraw ? 0 : g_mv.nrec[c]); i++) {
        const XfbRec* r = &g_mv.rec[c][i];
        const XfbRec* q = NULL;
        for (u32 k = 0; k < g_mv.nrec[p] && !q; k++)
            if (g_mv.rec[p][k].key == r->key && g_mv.rec[p][k].occ == r->occ && g_mv.rec[p][k].verts == r->verts) q = &g_mv.rec[p][k];
        if (!q) continue; /* new this frame: no previous position, zero motion */
        VkBuffer bufs[2] = {g_mv.buf[c], g_mv.buf[p]};
        VkDeviceSize offs[2] = {r->off, q->off};
        vkCmdBindVertexBuffers(cb, 0, 2, bufs, offs);
        float pc[8] = {r->vp.x, r->vp.y, r->vp.width, r->vp.height, q->vp.x, q->vp.y, q->vp.width, q->vp.height};
        vkCmdPushConstants(cb, g_mv.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
        vkCmdSetViewport(cb, 0, 1, &r->vpj);
        vkCmdDraw(cb, r->verts, 1, 0, 0);
        drawn++;
    }
    vkCmdEndRendering(cb);
    vk_barrier(cb, g_dl.mv, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
    return drawn ? drawn : 1; /* the image is in GENERAL either way */
}

/* each present: the capture buffers swap, the jitter moves on */
static void mv_frame(int evaluated) {
    g_mv.cur ^= 1;
    g_mv.nrec[g_mv.cur] = 0;
    g_mv.used[g_mv.cur] = 0;
    u32 i = (g_mv.jframe++ % 8) + 1;
    { /* SWR_DLSS_JAMP=a scales the jitter (test) */
        static float amp = -1;
        if (amp < 0) amp = getenv("SWR_DLSS_JAMP") ? (float)atof(getenv("SWR_DLSS_JAMP")) : 1.0f;
        g_mv.jx = (halton(i, 2) - 0.5f) * amp;
        g_mv.jy = (halton(i, 3) - 0.5f) * amp;
    }
    if (g_mv.jit_used && !evaluated) { /* jittered, but DLSS didn't run: that frame shook */
        g_mv.hold = 60;
        g_mv.misses++;
    } else if (g_mv.hold)
        g_mv.hold--;
    g_mv.jit_ok = dl_aa() && evaluated && !g_mv.hold; /* jitter only after a frame DLSS / TAA ran on */
    g_mv.jit_used = 0;
}

#ifndef SWR_NO_NGX
static void evaluate_dlss(Surf* t, Surf* z) {
    u32 w = t->sw, h = t->sh;
    vk_render_end();
    if (!ensure_images(w, h)) return dl_fail("image creation", 0);
    VkImageView zv = depth_view(z);
    if (!zv) return dl_fail("depth view", 0);
    VkCommandBuffer cb = vk_cmd();
    if (!g_dl.params && !NGX_OK(g_ngx.alloc_params(&g_dl.params))) return dl_fail("parameter allocation", 0);
    NgxParam* p = g_dl.params;
    if (!g_dl.feat || g_dl.fw != w || g_dl.fh != h) {
        if (g_dl.feat) {
            vk_finish();
            g_ngx.release(g_dl.feat);
            g_dl.feat = NULL;
        }
        p_setu(p, "CreationNodeMask", 1);
        p_setu(p, "VisibilityNodeMask", 1);
        p_setu(p, "Width", w);
        p_setu(p, "Height", h);
        p_setu(p, "OutWidth", w);
        p_setu(p, "OutHeight", h);
        p_seti(p, "PerfQualityValue", quality_value());
        /* IsHDR | MVLowRes | AutoExposure: linear scene light, motion vectors at render size, no exposure texture */
        p_seti(p, "DLSS.Feature.Create.Flags", getenv("SWR_DLSS_FLAGS") ? atoi(getenv("SWR_DLSS_FLAGS")) : ((1 << 0) | (1 << 1) | (1 << 6)));
        p_seti(p, "DLSS.Enable.Output.Subrects", 0);
        { /* SWR_DLSS_PRESET=n: DLSS model preset hint for DLAA (0 default = K, transformer; 6 = F, CNN) */
            const char* e = getenv("SWR_DLSS_PRESET");
            if (e) p_setu(p, "DLSS.Hint.Render.Preset.DLAA", (unsigned)atoi(e));
        }
        cb = vk_cmd();
        NgxResult r = g_ngx.create(cb, NGX_FEATURE_SUPERSAMPLING, p, &g_dl.feat);
        if (!NGX_OK(r) || !g_dl.feat) return dl_fail("feature creation", r);
        g_dl.fw = w;
        g_dl.fh = h;
        g_dl.reset = 1;
        LOG("dlss: DLSS feature %ux%u created (quality value %d)\n", w, h, quality_value());
    }
    cb = vk_cmd();
    /* inputs readable by compute, the output writable */
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    vk_surf_use(cb, z, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    vk_barrier(cb, g_dl.mv, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g_dl.mv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
    u32 mv_draws = getenv("SWR_DLSS_NO_MV") ? 0 : mv_pass(cb, z, w, h);
    if (!mv_draws)
        vk_barrier(cb, g_dl.mv, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
    else /* the pass read the depth as an attachment: back to compute reads for DLSS */
        vk_surf_use(cb, z, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    if (mv_draws && getenv("SWR_DLSS_MV_ZERO")) { /* test: the pass ran, its vectors cleared */
        vk_barrier(cb, g_dl.mv, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                   VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb, g_dl.mv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
        vk_barrier(cb, g_dl.mv, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
    }
    vk_barrier(cb, g_dl.out, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    NgxResourceVK rc = res_image(t->att, t->img, t->f.vk, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    NgxResourceVK ro = res_image(g_dl.out_view, g_dl.out, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    NgxResourceVK rz = res_image(zv, z->img, z->f.vk, w, h, VK_IMAGE_ASPECT_DEPTH_BIT, 0);
    NgxResourceVK rm = res_image(g_dl.mv_view, g_dl.mv, VK_FORMAT_R16G16_SFLOAT, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    p_setp(p, "Color", &rc);
    p_setp(p, "Output", &ro);
    p_setp(p, "Depth", &rz);
    p_setp(p, "MotionVectors", &rm);
    { /* the jitter the scene was drawn with; SWR_DLSS_JSIGN=-1 flips its sign (convention check) */
        float js = getenv("SWR_DLSS_JSIGN") ? (float)atof(getenv("SWR_DLSS_JSIGN")) : 1.0f, sx = js, sy = js;
        if (getenv("SWR_DLSS_JSCALE")) sscanf(getenv("SWR_DLSS_JSCALE"), "%f,%f", &sx, &sy); /* per axis (test) */
        int jit = !getenv("SWR_DLSS_NO_JITTER") && g_mv.jit_used;
        p_setf(p, "Jitter.Offset.X", jit ? sx * g_mv.jx : 0.0f);
        p_setf(p, "Jitter.Offset.Y", jit ? sy * g_mv.jy : 0.0f);
    }
    p_setf(p, "Sharpness", 0.0f);
    p_seti(p, "Reset", g_dl.reset);
    { /* render pixels; SWR_DLSS_MVSCALE=x,y overrides (convention check) */
        float mx = 1.0f, my = 1.0f;
        const char* e = getenv("SWR_DLSS_MVSCALE");
        if (e) sscanf(e, "%f,%f", &mx, &my);
        p_setf(p, "MV.Scale.X", mx);
        p_setf(p, "MV.Scale.Y", my);
    }
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Width", w);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Height", h);
    p_setf(p, "DLSS.Pre.Exposure", 1.0f);
    p_setf(p, "DLSS.Exposure.Scale", 1.0f);
    p_setf(p, "FrameTimeDeltaInMsec", 16.67f);
    NgxResult r = g_ngx.evaluate(cb, g_dl.feat, p, NULL);
    if (!NGX_OK(r)) return dl_fail("evaluation", r);
    g_dl.reset = 0;
    /* the result back into the scene target, which everything after reads */
    vk_barrier(cb, g_dl.out, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{0, 0, 0}, {(i32)w, (i32)h, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstOffsets = {{0, 0, 0}, {(i32)w, (i32)h, 1}}};
    static int debug_mv = -1;
    if (debug_mv < 0) debug_mv = getenv("SWR_DLSS_DEBUG_MV") != NULL;
    if (debug_mv) { /* test: the motion vectors themselves in place of the scene (R = x, G = y, render pixels) */
        vk_barrier(cb, g_dl.mv, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdBlitImage(cb, g_dl.mv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &b, VK_FILTER_NEAREST);
    } else
        vkCmdBlitImage(cb, g_dl.out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &b, VK_FILTER_NEAREST);
    t->wgen++;
    vk_rt_drawn(t);
    /* the motion-vector pass (and NGX) bound pipelines and state of their own in this command buffer: the next draw
       sets all of its own again. Without this the draws after DLSS kept the replay pass's pipeline and viewport and
       wrote nothing, so the scene held its old picture while the HUD moved on (2026-10-22) */
    vk_render_forget_state();
    if (++g_dl.evals == 1 || g_dl.evals % 600 == 0)
        LOG("dlss: %d frames (scene %llx %ux%u, depth %llx); motion vectors from %u of %u captured draws, jitter %.3f %.3f; %d target switches, %d unjittered-safety holds\n",
            g_dl.evals, (unsigned long long)t->gva, w, h, (unsigned long long)z->gva, mv_draws > 1 ? mv_draws : 0,
            g_mv.nrec[g_mv.cur], g_mv.jx, g_mv.jy, g_dl.switches, g_mv.misses);
}

#endif /* SWR_NO_NGX */

/* ---------------------------------------------------------------- TAA (ini taa=1) and ReShade's depth
   TAA: the scene target blended with the previous result (shaders/taa.frag, the same maths as the OpenGL renderer's),
   moved by the motion vectors above and clipped to this frame's 3x3 colour range. Two attachments: the history
   (RGBA16F, tone-mapped) and the result in the scene's own format, copied back into the scene target that post
   effects, bloom and the HUD read. ReShade: a copy of the finished scene depth, handed to ReShade's effects (DEPTH). */
#include "nx/gpu/vk/hdr_spv.h" /* g_hdr_out_vert: one triangle over the target */
#include "nx/gpu/vk/taa_spv.h"
static struct {
    VkImage hist[2], tmp;
    VmaAllocation hist_mem[2], tmp_mem;
    VkImageView hist_view[2], tmp_view;
    VkImageLayout hist_layout[2];
    u32 w, h, cur;
    VkFormat fmt;
    int valid;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkDescriptorPool pool;
    VkDescriptorSet set[2];
    VkImageView set_key[2][3];
    VkSampler samp;
    VkPipeline pipe;
    VkFormat pipe_fmt;
} g_taa;

static void taa_fail(const char* what) {
    LOG("taa (vulkan): %s failed: TAA off for this run\n", what);
    g_taa_failed = 1;
}

static int taa_layout(void) {
    if (g_taa.layout) return 1;
    VkDescriptorSetLayoutBinding b[3];
    for (int i = 0; i < 3; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (u32)i, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
    if (vkCreateDescriptorSetLayout(g_vk.dev, &dci, NULL, &g_taa.dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &g_taa.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    if (vkCreatePipelineLayout(g_vk.dev, &lci, NULL, &g_taa.layout) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &size};
    if (vkCreateDescriptorPool(g_vk.dev, &pci, NULL, &g_taa.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout l2[2] = {g_taa.dsl, g_taa.dsl};
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_taa.pool,
                                      .descriptorSetCount = 2, .pSetLayouts = l2};
    if (vkAllocateDescriptorSets(g_vk.dev, &ai, g_taa.set) != VK_SUCCESS) return 0;
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                               .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    return vkCreateSampler(g_vk.dev, &sci, NULL, &g_taa.samp) == VK_SUCCESS;
}

static int taa_pipeline(VkFormat fmt) {
    if (g_taa.pipe && g_taa.pipe_fmt == fmt) return 1;
    if (g_taa.pipe) {
        vk_finish();
        vkDestroyPipeline(g_vk.dev, g_taa.pipe, NULL);
        g_taa.pipe = VK_NULL_HANDLE;
    }
    VkShaderModule vs = vk_module_spv(g_hdr_out_vert, sizeof g_hdr_out_vert / 4), fs = vk_module_spv(g_taa_frag, sizeof g_taa_frag / 4);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba[2] = {{.colorWriteMask = 0xF}, {.colorWriteMask = 0xF}};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 2, .pAttachments = ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat cf[2] = {VK_FORMAT_R16G16B16A16_SFLOAT, fmt};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 2, .pColorAttachmentFormats = cf};
    VkGraphicsPipelineCreateInfo gci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
                                        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
                                        .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
                                        .pDynamicState = &dy, .layout = g_taa.layout};
    int ok = vs && fs && vkCreateGraphicsPipelines(g_vk.dev, vk_pipeline_cache(), 1, &gci, NULL, &g_taa.pipe) == VK_SUCCESS;
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    if (!ok) g_taa.pipe = VK_NULL_HANDLE;
    g_taa.pipe_fmt = fmt;
    return ok;
}

static void taa_free(void) {
    for (int i = 0; i < 2; i++) {
        if (g_taa.hist_view[i]) vkDestroyImageView(g_vk.dev, g_taa.hist_view[i], NULL);
        if (g_taa.hist[i]) vmaDestroyImage(g_vk.vma, g_taa.hist[i], g_taa.hist_mem[i]);
        g_taa.hist[i] = VK_NULL_HANDLE;
        g_taa.hist_view[i] = VK_NULL_HANDLE;
    }
    if (g_taa.tmp_view) vkDestroyImageView(g_vk.dev, g_taa.tmp_view, NULL);
    if (g_taa.tmp) vmaDestroyImage(g_vk.vma, g_taa.tmp, g_taa.tmp_mem);
    g_taa.tmp = VK_NULL_HANDLE;
    g_taa.tmp_view = VK_NULL_HANDLE;
    memset(g_taa.set_key, 0, sizeof g_taa.set_key);
}

static int taa_images(u32 w, u32 h, VkFormat fmt) {
    if (g_taa.tmp && g_taa.w == w && g_taa.h == h && g_taa.fmt == fmt) return 1;
    vk_finish(); /* the old images may still be in flight */
    taa_free();
    VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for (int i = 0; i < 2; i++) {
        if (!image2d(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, u, &g_taa.hist[i], &g_taa.hist_mem[i], &g_taa.hist_view[i])) {
            taa_free();
            return 0;
        }
        g_taa.hist_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (!image2d(w, h, fmt, u, &g_taa.tmp, &g_taa.tmp_mem, &g_taa.tmp_view)) {
        taa_free();
        return 0;
    }
    g_taa.w = w;
    g_taa.h = h;
    g_taa.fmt = fmt;
    g_taa.valid = 0;
    LOG("taa (vulkan): on, %ux%u\n", w, h);
    return 1;
}

/* the motion-vector image for this frame, left readable by fragment shaders; 1 = vectors drawn */
static int mv_for_fragment(VkCommandBuffer cb, Surf* z, u32 w, u32 h) {
    vk_barrier(cb, g_dl.mv, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g_dl.mv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
    u32 drawn = getenv("SWR_DLSS_NO_MV") ? 0 : mv_pass(cb, z, w, h);
    if (!drawn)
        vk_barrier(cb, g_dl.mv, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                   VK_IMAGE_LAYOUT_GENERAL);
    else /* mv_pass left it in GENERAL for compute: fragment reads too */
        vk_barrier(cb, g_dl.mv, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
    return drawn > 1;
}

static void evaluate_taa(Surf* t, Surf* z) {
    u32 w = t->sw, h = t->sh;
    if (!g_mv.on) return taa_fail("motion vectors (no transform feedback)");
    if (!ensure_images(w, h)) return taa_fail("motion-vector image");
    if (!taa_layout()) return taa_fail("layout");
    if (!taa_pipeline(t->f.vk)) return taa_fail("pipeline");
    if (!taa_images(w, h, t->f.vk)) return taa_fail("images");
    if (g_dl.reset) g_taa.valid = 0;
    u32 c = (g_taa.cur ^= 1), p = c ^ 1;
    /* the set for this parity: scene, last result, motion vectors (rewritten only when one of them changed) */
    VkImageView key[3] = {t->att, g_taa.hist_view[p], g_dl.mv_view};
    if (memcmp(key, g_taa.set_key[c], sizeof key)) {
        vk_finish();
        VkDescriptorImageInfo di[3] = {{g_taa.samp, t->att, VK_IMAGE_LAYOUT_GENERAL},
                                       {g_taa.samp, g_taa.hist_view[p], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {g_taa.samp, g_dl.mv_view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet wr[3];
        for (int i = 0; i < 3; i++)
            wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_taa.set[c], .dstBinding = (u32)i,
                                           .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di[i]};
        vkUpdateDescriptorSets(g_vk.dev, 3, wr, 0, NULL);
        memcpy(g_taa.set_key[c], key, sizeof key);
    }
    VkCommandBuffer cb = vk_cmd();
    int have_mv = mv_for_fragment(cb, z, w, h);
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    if (g_taa.hist_layout[p] == VK_IMAGE_LAYOUT_UNDEFINED) { /* first frame: readable, never read (valid 0) */
        vk_barrier(cb, g_taa.hist[p], VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        g_taa.valid = 0;
    } else /* last frame's result, written as an attachment */
        vk_barrier(cb, g_taa.hist[p], VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, g_taa.hist_layout[p], VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g_taa.hist_layout[p] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vk_barrier(cb, g_taa.hist[c], VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vk_barrier(cb, g_taa.tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca[2] = {
        {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_taa.hist_view[c], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE},
        {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_taa.tmp_view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 2, .pColorAttachments = ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_taa.pipe);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_taa.layout, 0, 1, &g_taa.set[c], 0, NULL);
    float pc[8] = {g_taa.valid ? 1.0f : 0.0f, have_mv ? 1.0f : 0.0f, 0.1f, 0.04f, /* w: weight where nothing moves; + the jitter */
                   g_mv.jit_used ? g_mv.jx : 0.0f, g_mv.jit_used ? g_mv.jy : 0.0f, 0.0f, 0.0f};
    vkCmdPushConstants(cb, g_taa.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    g_taa.hist_layout[c] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    /* the result into the scene target */
    vk_barrier(cb, g_taa.tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {w, h, 1}};
    vkCmdCopyImage(cb, g_taa.tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    g_taa.valid = 1;
    g_dl.reset = 0;
    t->wgen++;
    vk_rt_drawn(t);
    vk_render_forget_state(); /* the replay pass and this one bound pipelines and state of their own */
    if (++g_dl.evals == 1 || g_dl.evals % 600 == 0)
        LOG("taa (vulkan): %d frames (scene %llx %ux%u); motion vectors from %u captured draws, %d target switches\n", g_dl.evals,
            (unsigned long long)t->gva, w, h, g_mv.nrec[g_mv.cur], g_dl.switches);
}

/* ReShade: the finished scene depth, copied into an image ReShade's effects sample (left in SHADER_READ_ONLY) */
static struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView view;
    u32 w, h;
    VkFormat fmt;
    int ready;
} g_rsd;
static void reshade_depth(Surf* z, u32 w, u32 h) {
    VkImageAspectFlags asp = VK_IMAGE_ASPECT_DEPTH_BIT | (z->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (!g_rsd.img || g_rsd.w != w || g_rsd.h != h || g_rsd.fmt != z->f.vk) {
        reshade_set_view(0);
        if (g_rsd.img) {
            vk_finish();
            vkDestroyImageView(g_vk.dev, g_rsd.view, NULL);
            vmaDestroyImage(g_vk.vma, g_rsd.img, g_rsd.mem);
            g_rsd.img = VK_NULL_HANDLE;
        }
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = z->f.vk,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &ii, &ai, &g_rsd.img, &g_rsd.mem, NULL) != VK_SUCCESS) { g_rsd.img = VK_NULL_HANDLE; return; }
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_rsd.img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = z->f.vk, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(g_vk.dev, &vi, NULL, &g_rsd.view) != VK_SUCCESS) {
            vmaDestroyImage(g_vk.vma, g_rsd.img, g_rsd.mem);
            g_rsd.img = VK_NULL_HANDLE;
            return;
        }
        g_rsd.w = w;
        g_rsd.h = h;
        g_rsd.fmt = z->f.vk;
        g_rsd.ready = 0;
        LOG("reshade (vulkan): scene depth %ux%u (format %d) copied for ReShade each frame\n", w, h, (int)z->f.vk);
    }
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, z, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_barrier(cb, g_rsd.img, asp, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
               g_rsd.ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                      .extent = {w, h, 1}};
    vkCmdCopyImage(cb, z->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_rsd.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    vk_barrier(cb, g_rsd.img, asp, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g_rsd.ready = 1;
    reshade_set_view((unsigned long long)(uintptr_t)g_rsd.view);
}

#include <math.h>
/* ---------------------------------------------------------------- XeGTAO (ini ambient_occlusion 0..100 = strength)
   Intel XeGTAO v1.30 (MIT, github.com/GameTechDev/XeGTAO), ported to GLSL (shaders/ao_pre.comp, ao_main.comp,
   ao_den.comp, ao_apply.frag; the same maths as the OpenGL renderer's gpu_gl.c AO_*): view-space depth MIPs, the AO
   term with normals from depth, one denoise pass, then the scene target is multiplied by it (blend dst * src) before
   post effects, bloom and the HUD. The camera projection comes from the game's scene draws (vertex constant bank 3,
   render.c -> vk_ao_note). All AO images stay in GENERAL. Recorded through the recording thread's ring (rec_Cmd*). */
#include "nx/gpu/vk/ao_pre_spv.h"
#include "nx/gpu/vk/ao_main_spv.h"
#include "nx/gpu/vk/ao_den_spv.h"
#include "nx/gpu/vk/ao_apply_spv.h"
extern int g_opt_ao;
static int g_ao_failed;
static struct { float p00, p11, a, b; int clip, ok; } g_vproj;
static int dl_ao(void) { return g_opt_ao > 0 && !g_ao_failed; }

void vk_ao_note(Surf* zt, u64 cb3, u32 cb3_size, u32 clip);
void vk_ao_note(Surf* zt, u64 cb3, u32 cb3_size, u32 clip) {
    if (!g_opt_ao || !zt || !g_dl.chosen_z || zt->gva != g_dl.chosen_z || !cb3 || cb3_size < 64) return;
    float m[16];
    gmem_read(cb3, m, 64);
    for (int tr = 0; tr < 2; tr++) {
        float M[16];
        for (int i = 0; i < 16; i++) M[i] = tr ? m[(i % 4) * 4 + i / 4] : m[i];
#define AZ(x) (fabsf(x) < 1e-6f)
        if (M[0] > 0.05f && M[5] > 0.05f && AZ(M[1]) && AZ(M[2]) && AZ(M[3]) && AZ(M[4]) && AZ(M[6]) && AZ(M[7]) && AZ(M[12]) &&
            AZ(M[13]) && AZ(M[15]) && fabsf(fabsf(M[11]) - 1.f) < 1e-4f) {
            g_vproj.p00 = M[0]; g_vproj.p11 = M[5]; g_vproj.a = -M[10] * M[11]; g_vproj.b = M[14]; g_vproj.clip = clip & 1;
            if (!g_vproj.ok) LOG("ao (vulkan): camera projection P00 %.4f P11 %.4f depth %g %g\n", M[0], M[5], M[10], M[14]);
            g_vproj.ok = 1;
            return;
        }
#undef AZ
    }
}

static struct {
    VkImage vz, ao, ed, fin;
    VmaAllocation vz_mem, ao_mem, ed_mem, fin_mem;
    VkImageView vz_all, vz_lv[5], ao_v, ed_v, fin_v;
    u32 w, h, n;
    VkSampler samp;
    VkDescriptorSetLayout dsl[4];
    VkPipelineLayout pl[4];
    VkDescriptorPool pool;
    VkDescriptorSet set[4];
    VkPipeline pre, mainp, den, app;
    VkFormat app_fmt;
    VkImageView key_z, key_t;
    int ready;
} g_aov;

static VkDescriptorSetLayout ao_dsl(const VkDescriptorType* types, int n) {
    VkDescriptorSetLayoutBinding b[6];
    for (int i = 0; i < n; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (u32)i, .descriptorType = types[i], .descriptorCount = 1,
                                              .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (u32)n, .pBindings = b};
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l);
    return l;
}
static VkPipeline ao_compute(const u32* words, size_t n, VkPipelineLayout pl) {
    VkShaderModule m = vk_module_spv(words, n);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = pl};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    return p;
}
static int ao_setup(void) {
    if (g_aov.pre) return 1;
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    VkDescriptorType t0[6] = {S, I, I, I, I, I}, t1[3] = {S, I, I}, t2[4] = {S, S, I, S}, t3[1] = {S};
    g_aov.dsl[0] = ao_dsl(t0, 6); g_aov.dsl[1] = ao_dsl(t1, 3); g_aov.dsl[2] = ao_dsl(t2, 4); g_aov.dsl[3] = ao_dsl(t3, 1);
    u32 pcs[4] = {16, 48, 16, 16};
    for (int i = 0; i < 4; i++) {
        if (!g_aov.dsl[i]) return 0;
        VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcs[i]};
        VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &g_aov.dsl[i], .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
        if (vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_aov.pl[i]) != VK_SUCCESS) return 0;
    }
    VkDescriptorPoolSize sizes[2] = {{S, 9}, {I, 10}};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4, .poolSizeCount = 2, .pPoolSizes = sizes};
    if (vkCreateDescriptorPool(g_vk.dev, &pci, NULL, &g_aov.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_aov.pool,
                                      .descriptorSetCount = 4, .pSetLayouts = g_aov.dsl};
    if (vkAllocateDescriptorSets(g_vk.dev, &ai, g_aov.set) != VK_SUCCESS) return 0;
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                               .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .maxLod = 4.0f};
    if (vkCreateSampler(g_vk.dev, &sci, NULL, &g_aov.samp) != VK_SUCCESS) return 0;
    g_aov.pre = ao_compute(g_ao_pre_comp, sizeof g_ao_pre_comp / 4, g_aov.pl[0]);
    g_aov.mainp = ao_compute(g_ao_main_comp, sizeof g_ao_main_comp / 4, g_aov.pl[1]);
    g_aov.den = ao_compute(g_ao_den_comp, sizeof g_ao_den_comp / 4, g_aov.pl[2]);
    if (!g_aov.pre || !g_aov.mainp || !g_aov.den) return 0;
    LOG("ao: XeGTAO on (vulkan)\n");
    return 1;
}
static int ao_apply_pipeline(VkFormat fmt) {
    if (g_aov.app && g_aov.app_fmt == fmt) return 1;
    if (g_aov.app) { vk_finish(); vkDestroyPipeline(g_vk.dev, g_aov.app, NULL); g_aov.app = VK_NULL_HANDLE; }
    VkShaderModule vs = vk_module_spv(g_hdr_out_vert, sizeof g_hdr_out_vert / 4), fs = vk_module_spv(g_ao_apply_frag, sizeof g_ao_apply_frag / 4);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("SWR_AO_DEBUG") != NULL; /* test: the AO term alone in place of the scene */
    VkPipelineColorBlendAttachmentState ba = {.blendEnable = dbg ? VK_FALSE : VK_TRUE,
                                              .srcColorBlendFactor = VK_BLEND_FACTOR_ZERO, .dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR,
                                              .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                                              .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .alphaBlendOp = VK_BLEND_OP_ADD,
                                              .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
                                        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
                                        .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
                                        .pDynamicState = &dy, .layout = g_aov.pl[3]};
    int ok = vs && fs && vkCreateGraphicsPipelines(g_vk.dev, vk_pipeline_cache(), 1, &gci, NULL, &g_aov.app) == VK_SUCCESS;
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    if (!ok) g_aov.app = VK_NULL_HANDLE;
    g_aov.app_fmt = fmt;
    return ok;
}
static void ao_free(void) {
    VkImageView* vs[8] = {&g_aov.vz_all, &g_aov.vz_lv[0], &g_aov.vz_lv[1], &g_aov.vz_lv[2], &g_aov.vz_lv[3], &g_aov.vz_lv[4], &g_aov.ao_v, &g_aov.ed_v};
    for (int i = 0; i < 8; i++) if (*vs[i]) { vkDestroyImageView(g_vk.dev, *vs[i], NULL); *vs[i] = VK_NULL_HANDLE; }
    if (g_aov.fin_v) { vkDestroyImageView(g_vk.dev, g_aov.fin_v, NULL); g_aov.fin_v = VK_NULL_HANDLE; }
    if (g_aov.vz) vmaDestroyImage(g_vk.vma, g_aov.vz, g_aov.vz_mem);
    if (g_aov.ao) vmaDestroyImage(g_vk.vma, g_aov.ao, g_aov.ao_mem);
    if (g_aov.ed) vmaDestroyImage(g_vk.vma, g_aov.ed, g_aov.ed_mem);
    if (g_aov.fin) vmaDestroyImage(g_vk.vma, g_aov.fin, g_aov.fin_mem);
    g_aov.vz = g_aov.ao = g_aov.ed = g_aov.fin = VK_NULL_HANDLE;
    g_aov.key_z = g_aov.key_t = VK_NULL_HANDLE;
    g_aov.ready = 0;
}
static int ao_image(u32 w, u32 h, u32 levels, VkImage* img, VmaAllocation* mem) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32_SFLOAT,
                            .extent = {w, h, 1}, .mipLevels = levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return vmaCreateImage(g_vk.vma, &ii, &ai, img, mem, NULL) == VK_SUCCESS;
}
static VkImageView ao_view(VkImage img, u32 level, u32 count) {
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R32_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(g_vk.dev, &vi, NULL, &v);
    return v;
}
static int ao_images(u32 w, u32 h) {
    if (g_aov.vz && g_aov.w == w && g_aov.h == h) return 1;
    vk_finish();
    ao_free();
    if (!ao_image(w, h, 5, &g_aov.vz, &g_aov.vz_mem) || !ao_image(w, h, 1, &g_aov.ao, &g_aov.ao_mem) ||
        !ao_image(w, h, 1, &g_aov.ed, &g_aov.ed_mem) || !ao_image(w, h, 1, &g_aov.fin, &g_aov.fin_mem)) { ao_free(); return 0; }
    g_aov.vz_all = ao_view(g_aov.vz, 0, 5);
    for (u32 l = 0; l < 5; l++) g_aov.vz_lv[l] = ao_view(g_aov.vz, l, 1);
    g_aov.ao_v = ao_view(g_aov.ao, 0, 1); g_aov.ed_v = ao_view(g_aov.ed, 0, 1); g_aov.fin_v = ao_view(g_aov.fin, 0, 1);
    g_aov.w = w; g_aov.h = h;
    return 1;
}
static void ao_sets(VkImageView zv, VkImageView tv) {
    VkDescriptorImageInfo d[14] = {
        {g_aov.samp, zv, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, g_aov.vz_lv[0], VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, g_aov.vz_lv[1], VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, g_aov.vz_lv[2], VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, g_aov.vz_lv[3], VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, g_aov.vz_lv[4], VK_IMAGE_LAYOUT_GENERAL},
        {g_aov.samp, g_aov.vz_all, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, g_aov.ao_v, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, g_aov.ed_v, VK_IMAGE_LAYOUT_GENERAL},
        {g_aov.samp, g_aov.ao_v, VK_IMAGE_LAYOUT_GENERAL}, {g_aov.samp, g_aov.ed_v, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, g_aov.fin_v, VK_IMAGE_LAYOUT_GENERAL},
        {g_aov.samp, g_aov.fin_v, VK_IMAGE_LAYOUT_GENERAL}, {g_aov.samp, tv, VK_IMAGE_LAYOUT_GENERAL}};
    const int setof[14] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 2}, bind[14] = {0, 1, 2, 3, 4, 5, 0, 1, 2, 0, 1, 2, 0, 3};
    VkWriteDescriptorSet wr[14];
    for (int i = 0; i < 14; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_aov.set[setof[i]], .dstBinding = (u32)bind[i],
                                       .descriptorCount = 1,
                                       .descriptorType = d[i].sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &d[i]};
    vkUpdateDescriptorSets(g_vk.dev, 14, wr, 0, NULL);
}
static void ao_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 dst) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, .dstStageMask = dst,
                           .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
static void evaluate_ao(Surf* t, Surf* z) {
    u32 w = t->sw, h = t->sh;
    if (!g_vproj.ok) return;
    if (!ao_setup()) { g_ao_failed = 1; LOG("ao (vulkan): setup failed: AO off\n"); return; }
    if (!ao_apply_pipeline(t->f.vk) || !ao_images(w, h)) { g_ao_failed = 1; LOG("ao (vulkan): images or pipeline failed: AO off\n"); return; }
    VkImageView zv = depth_view(z);
    if (!zv) return;
    if (g_aov.key_z != zv || g_aov.key_t != t->att || !g_aov.ready) { vk_finish(); ao_sets(zv, t->att); g_aov.key_z = zv; g_aov.key_t = t->att; }
    VkCommandBuffer cb = vk_cmd();
    if (!g_aov.ready) { /* first use: every AO image to GENERAL */
        VkImage im[4] = {g_aov.vz, g_aov.ao, g_aov.ed, g_aov.fin};
        for (int i = 0; i < 4; i++) {
            VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = im[i], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, i ? 1u : 5u, 0, 1}};
            VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
            vkCmdPipelineBarrier2(cb, &di);
        }
        g_aov.ready = 1;
    }
    float A = g_vproj.a, B = g_vproj.b, mul, add;
    if (g_vproj.clip) { mul = -B; add = -A; } else { mul = -0.5f * B; add = 0.5f * (1.f - A); }
    float tx = 1.f / g_vproj.p00, ty = 1.f / g_vproj.p11, radius = 30.f; /* game units (~cm): contact shadows, not a broad haze */
    float dep[4] = {mul, add, radius, 0};
    float mp[12] = {(float)w, (float)h, 1.f / (float)w, 1.f / (float)h, tx * 2.f, ty * -2.f, -tx, ty, radius, 3.f, 3.f, (float)(dl_aa() ? g_aov.n++ % 64 : 0)};
    float vp[4] = {(float)w, (float)h, 1.f / (float)w, 1.f / (float)h}, ap[4] = {(float)g_opt_ao / 100.f, 0, 0, 0};
    /* 1: view depth MIPs from the scene depth */
    vk_surf_use(cb, z, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    ao_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT); /* last frame's AO reads before this frame's writes */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_aov.pre);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_aov.pl[0], 0, 1, &g_aov.set[0], 0, NULL);
    vkCmdPushConstants(cb, g_aov.pl[0], VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, dep);
    vkCmdDispatch(cb, (w + 15) / 16, (h + 15) / 16, 1);
    ao_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    /* 2: AO term and edges */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_aov.mainp);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_aov.pl[1], 0, 1, &g_aov.set[1], 0, NULL);
    vkCmdPushConstants(cb, g_aov.pl[1], VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, mp);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    ao_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    /* 3: denoise (reads the lit scene too) */
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_aov.den);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_aov.pl[2], 0, 1, &g_aov.set[2], 0, NULL);
    vkCmdPushConstants(cb, g_aov.pl[2], VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, vp);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    ao_barrier(cb, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    /* 4: scene colour *= AO */
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = t->att,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_aov.app);
    VkViewport v = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_aov.pl[3], 0, 1, &g_aov.set[3], 0, NULL);
    vkCmdPushConstants(cb, g_aov.pl[3], VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, ap);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    t->wgen++;
    vk_rt_drawn(t);
    vk_render_forget_state();
}

/* AO on the ambient light (as the OpenGL renderer, gpu_gl.c ao_point): Dread's G-buffer pass accumulates the ambient /
   probe light in a full-size HDR target that the lighting pass then reads; AO multiplies that target before the read,
   so direct light, light shafts and effects are not darkened. SWR_AO_POINT=scene = after the effects (old) */
static int ao_at_scene(void) { static int v = -1; if (v < 0) v = getenv("SWR_AO_POINT") && strcmp(getenv("SWR_AO_POINT"), "ambient") != 0; return v; }

static void evaluate(Surf* t, Surf* z) {
    vk_render_end();
    if (dl_reshade()) reshade_depth(z, t->sw, t->sh);
    if (dl_ao() && !g_aamb.done) evaluate_ao(t, z); /* before TAA: its noise is resolved over frames */
#ifndef SWR_NO_NGX
    if (dl_dlss()) evaluate_dlss(t, z);
    else
#endif
    if (dl_taa()) evaluate_taa(t, z);
    g_dl.evaluated = 1;
}

/* texture.c, before a texture over a render target takes its pixels */
void vk_dlss_before_read(u64 gva);
void vk_dlss_before_read(u64 gva) {
    if (!dl_active()) return;
    Surf* r = vk_rt_find(gva);
    if (!r) return;
    if (g_dl.chosen && !g_dl.done) g_mv.reads_rt = 1;
    if (full_hdr(r) && g_dl.nreads < 16) {
        int dup = 0;
        for (int i = 0; i < g_dl.nreads; i++) dup |= g_dl.reads[i] == gva;
        if (!dup) g_dl.reads[g_dl.nreads++] = gva;
    }
    if (dl_ao() && !ao_at_scene() && !g_aamb.seen && full_hdr(r) && written_now(gva) && g_dl.chosen && gva != g_dl.chosen) {
        g_aamb.seen = 1; /* the frame's first read of a full-size HDR target drawn this frame: the ambient light */
        Surf* z = g_dl.chosen_z ? vk_rt_find(g_dl.chosen_z) : NULL;
        if (z && z->f.depth && r->sw == z->sw && r->sh == z->sh) {
            vk_render_end();
            evaluate_ao(r, z);
            g_aamb.done = 1;
            static int said;
            if (!said) { said = 1; LOG("ao (Vulkan): on the ambient light (target %llx, read by the lighting pass)\n", (unsigned long long)gva); }
        }
    }
    if (g_dl.done || !g_dl.chosen || gva != g_dl.chosen || !written_now(gva)) return;
    g_dl.done = 1;
    Surf* z = g_dl.chosen_z ? vk_rt_find(g_dl.chosen_z) : NULL;
    if (!z || !z->f.depth || r->sw != z->sw || r->sh != z->sh) return;
    evaluate(r, z);
}
