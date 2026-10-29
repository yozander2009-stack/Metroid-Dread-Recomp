/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vulkan instance, device, queues and swapchain. Vulkan 1.3 is required: timeline semaphores (1.2),
   synchronization2 and dynamic rendering (1.3) are what the renderer is built on. */
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/options.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "common/gpu/vk/swapchain.h"
#include "common/gpu/vk/vk_common.h"

int g_vk_preserve_infnan;
int g_vk_preserve_infnan64;
int g_vk_flush_denorm;
VkShaderStageFlags g_vk_subgroup_stages;

VkState g_vk;
/* reentrant on the thread that holds it: landing compute results writes guest memory, which can come back here */
static PMutex g_vklock;
static _Thread_local int t_vkdepth;
static u32 g_vk_owner; /* the thread holding it (plat_tid), 0 when none: a trap's fault on it can't be answered */
void vk_lock(void) {
    if (!t_vkdepth++) {
        if (!pm_trylock(&g_vklock)) { /* held: the GPU thread says so for the stall report */
            /* Only a contended acquisition gets a zone. */
            SNX_ZONE(z_wait, "vk lock wait");
            gpu_wait_note("vk_lock, which another thread holds", 0);
            pm_lock(&g_vklock);
            gpu_wait_note(NULL, 0);
        }
        __atomic_store_n(&g_vk_owner, plat_tid(), __ATOMIC_RELAXED);
    }
}
/* lets go of this thread's last hold, then signals the event slots its releases under it reached */
static void vk_let_go(void) {
    __atomic_store_n(&g_vk_owner, 0, __ATOMIC_RELAXED);
    pm_unlock(&g_vklock);
    vk_release_events();
}
void vk_unlock(void) {
    if (!--t_vkdepth) vk_let_go();
}
int vk_trylock(void) {
    if (!t_vkdepth && !pm_trylock(&g_vklock)) return 0;
    if (!t_vkdepth++) __atomic_store_n(&g_vk_owner, plat_tid(), __ATOMIC_RELAXED);
    return 1;
}
u32 vk_lock_owner(void) { return __atomic_load_n(&g_vk_owner, __ATOMIC_RELAXED); }

int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    if (r >= 0) return 1;
    WARN("vulkan: %s failed: %s (%d) at %s:%d\n", what, snx_vk_result_str(r), (int)r, file, line);
    return 0;
}

u32 g_vk_validation_messages;
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT* d, void* user) {
    (void)type;
    (void)user;
    g_vk_validation_messages++;
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        WARN("vulkan validation: %s\n", d->pMessage);
    else
        LOG("vulkan validation: %s\n", d->pMessage);
    return VK_FALSE;
}

/* VkPhysicalDevicePortabilitySubsetFeaturesKHR, which vulkan_beta.h holds back */
#define PORTABILITY_FEATURES_STYPE ((VkStructureType)1000163000)
typedef struct {
    VkStructureType sType;
    void* pNext;
    VkBool32 constantAlphaColorBlendFactors, events, imageViewFormatReinterpretation, imageViewFormatSwizzle,
        imageView2DOn3DImage, multisampleArrayImage, mutableComparisonSamplers, pointPolygons, samplerMipLodBias,
        separateStencilMaskRef, shaderSampleRateInterpolationFunctions, tessellationIsolines, tessellationPointMode,
        triangleFans, vertexAttributeAccessBeyondStride;
} PortabilityFeatures;
/* MoltenVK (macOS, iOS) is a portability driver: the loader lists it only for an instance that asks for those */
void vk_dlss_instance_exts(const char** ext, u32* next, u32 max);
void vk_dlss_device_exts(VkPhysicalDevice pd, const char** ext, u32* next, u32 max);
int vk_dlss_init(void);
static int create_instance(void) {
    u32 nbe = 0;
    const char* const* be = g_opt_headless ? NULL : backend_vk_extensions(&nbe);
    const char* ext[16]; /* SWR: the window's, then DLSS's and HDR's */
    u32 next = 0;
    for (u32 i = 0; be && i < nbe && next < 15; i++) ext[next++] = be[i];
    if (nbe > 15) WARN("vulkan: the window backend wants %u instance extensions, only 15 are taken\n", nbe);
    vk_dlss_instance_exts(ext, &next, 16); /* SWR: DLSS (dlss.c) */
    {   /* SWR: HDR output (hdr.c) needs the HDR colour spaces; only asked for when hdr=1 and the loader has it */
        extern int g_opt_hdr, g_vk_hdr_ext;
        if (g_opt_hdr && !g_opt_headless && next < 16 && snx_vk_has_instance_extension("VK_EXT_swapchain_colorspace")) {
            ext[next++] = "VK_EXT_swapchain_colorspace";
            g_vk_hdr_ext = 1;
        }
    }
    SnxVkInstanceInfo info = {.app_name = "ScarletNX",
                              .extensions = ext,
                              .extension_count = next,
                              .validation = g_settings.vulkan_validation_layer != 0,
                              .portability = true,
                              .debug = debug_cb};
    SnxVkInstance made;
    if (!VK_OK(snx_vk_instance_create(&info, &made))) return 0;
    if (info.validation && !made.validation)
        WARN("vulkan: validation was asked for (vk_validation), but "
             "VK_LAYER_KHRONOS_validation is not installed\n");
    g_vk.instance = made.instance;
    g_vk.debug = made.messenger;
    return 1;
}

/* the queue family that decodes H.264 with Vulkan Video, other than the graphics and compute ones; ~0u when none */
static u32 video_decode_family(void) {
    u32 n = 0, found = ~0u;
    vkGetPhysicalDeviceQueueFamilyProperties2(g_vk.phys, &n, NULL);
    VkQueueFamilyProperties2* q = calloc(n ? n : 1, sizeof *q);
    VkQueueFamilyVideoPropertiesKHR* v = calloc(n ? n : 1, sizeof *v);
    if (!q || !v) fatal("vulkan: out of memory for the queue families");
    for (u32 i = 0; i < n; i++) {
        v[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        q[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        q[i].pNext = &v[i];
    }
    vkGetPhysicalDeviceQueueFamilyProperties2(g_vk.phys, &n, q);
    for (u32 i = 0; i < n && found == ~0u; i++)
        if ((q[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) &&
            (v[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) && i != g_vk.gfx_family &&
            i != g_vk.compute_family)
            found = i;
    free(q);
    free(v);
    return found;
}

/* Vulkan 1.3 and a graphics queue. Only windowed runs need WSI and presentation support.
   *gfx_queues: how many queues the graphics family has */
static int device_score(VkPhysicalDevice pd, const VkPhysicalDeviceProperties* p, u32* gfx, u32* compute,
                        u32* gfx_queues) {
    SnxVkExtensions e;
    int swapchain = snx_vk_device_extensions(pd, &e) && snx_vk_extensions_has(&e, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    snx_vk_extensions_free(&e);
    if (!g_opt_headless && !swapchain) return -1;
    u32 nq = 0;
    VkQueueFamilyProperties* q = snx_vk_queue_families(pd, &nq);
    /* ASTC uploads use compute */
    *gfx = snx_vk_queue_family(pd, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
                               g_opt_headless ? VK_NULL_HANDLE : g_vk.surface, 0);
    *compute = ~0u;
    /* a compute family without graphics runs alongside the graphics queue */
    for (u32 i = 0; i < nq; i++)
        if ((q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && *compute == ~0u)
            *compute = i;
    if (*compute == ~0u) *compute = *gfx;
    *gfx_queues = *gfx < nq ? q[*gfx].queueCount : 0;
    free(q);
    if (*gfx == ~0u) return -1;
    return p->deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
           : p->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                     : 1;
}
/* device_score as snx_vk_best_device rates a device */
static int device_rating(VkPhysicalDevice pd, const VkPhysicalDeviceProperties* p, void* user) {
    u32 gf, cf, gq;
    (void)user;
    return device_score(pd, p, &gf, &cf, &gq);
}

static int create_device(void) {
    g_vk.phys = snx_vk_best_device(g_vk.instance, VK_API_VERSION_1_3, device_rating, NULL);
    if (!g_vk.phys) {
        WARN("vulkan: no Vulkan 1.3 device with a suitable %s queue\n", g_opt_headless ? "graphics" : "presentation");
        return 0;
    }
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    u32 gfx_queues = 0;
    device_score(g_vk.phys, &g_vk.props, &g_vk.gfx_family, &g_vk.compute_family, &gfx_queues);
    VkPhysicalDeviceSubgroupProperties subgroup = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
                                              .pNext = &subgroup};
    VkPhysicalDeviceProperties2 pp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &pp2);
    g_vk_subgroup_stages =
        subgroup.subgroupSize >= 32 &&
                (subgroup.supportedOperations & (VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                                                 VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)) ==
                    (VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                     VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)
            ? subgroup.supportedStages
            : 0;
    g_vk_preserve_infnan = p12.shaderSignedZeroInfNanPreserveFloat32 != 0;
    /* MoltenVK fails every pipeline that lists VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE (Metal's dynamic stride) */
    g_vk.has_dynamic_stride = p12.driverID != VK_DRIVER_ID_MOLTENVK;
    g_vk_preserve_infnan64 = p12.shaderSignedZeroInfNanPreserveFloat64 != 0;
    /* 32 bit alone: a device whose denorm modes can't differ per width would need 16 and 64 bit declared too.
       Qualcomm's driver claims the mode but miscompiles with it: a fragment shader of one game's waterfalls, all of
       its FMUL, FADD and FFMA FTZ as NVIDIA's compiler made them, drew the background's colour on an Adreno 740
       (driver 512.676), and right with the execution mode taken out of the same SPIR-V. Its FTZ is done per
       instruction there, as on a device without the mode */
    g_vk_flush_denorm = p12.shaderDenormFlushToZeroFloat32 &&
                        p12.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE &&
                        p12.driverID != VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
    /* without it, vk_pipeline_init measures whether the device flushes by itself (vk_ftz_native) */
    if (!g_vk_preserve_infnan) WARN("vulkan: FP32 signed-zero/Inf/NaN preservation unavailable on this GPU\n");

    /* compute gets a queue of its own: of a compute-only family, or else a second one of the graphics family */
    int second = g_vk.compute_family == g_vk.gfx_family && gfx_queues >= 2;
    float prio[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo qci[3] = {{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueFamilyIndex = g_vk.gfx_family,
                                       .queueCount = second ? 2 : 1,
                                       .pQueuePriorities = prio},
                                      {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueFamilyIndex = g_vk.compute_family,
                                       .queueCount = 1,
                                       .pQueuePriorities = prio}};
    /* the features drawing uses, where the device has them */
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(g_vk.phys, &have);
    VkPhysicalDeviceFeatures* want = &g_vk.feat;
#define WANT(f) want->f = have.f
    /* constant buffers are bound with the bytes the shader reads, not the 64K it declares: reads past them stay in
       bounds (their values are undefined) */
    WANT(robustBufferAccess);
    WANT(imageCubeArray);
    WANT(independentBlend);
    WANT(dualSrcBlend);
    WANT(logicOp);
    WANT(depthClamp);
    WANT(depthBiasClamp);
    WANT(fillModeNonSolid);
    WANT(depthBounds);
    WANT(wideLines);
    WANT(largePoints);
    WANT(multiViewport);
    WANT(samplerAnisotropy);
    WANT(occlusionQueryPrecise); /* samples-passed counts (query.c) are exact */
    WANT(sampleRateShading);
    /* BCn, ASTC and ETC2 sampled as they are (format.c) where the settings ask for it and the device has them, else
       decoded on the GPU as they are uploaded */
    if (g_settings.native_bc) WANT(textureCompressionBC);
    if (g_settings.native_astc) WANT(textureCompressionASTC_LDR);
    if (g_settings.native_etc2) WANT(textureCompressionETC2);
    if (g_settings.native_bc && !want->textureCompressionBC)
        WARN("vulkan: the GPU can't sample BCn textures: they are decoded on the GPU as they are loaded\n");
    if (g_settings.native_astc && !want->textureCompressionASTC_LDR)
        WARN("vulkan: native ASTC textures were asked for, but the GPU can't sample ASTC: they are decoded to RGBA8\n");
    if (g_settings.native_etc2 && !want->textureCompressionETC2)
        WARN("vulkan: native ETC2 textures were asked for, but the GPU can't sample ETC2: they are decoded\n");
    WANT(vertexPipelineStoresAndAtomics);
    WANT(fragmentStoresAndAtomics);
    WANT(shaderImageGatherExtended);
    WANT(shaderStorageImageExtendedFormats);
    WANT(shaderStorageImageWriteWithoutFormat);
    WANT(shaderStorageImageReadWithoutFormat);
    WANT(shaderClipDistance);
    WANT(shaderCullDistance);
    WANT(shaderFloat64);
    WANT(shaderInt64);
    WANT(shaderInt16);
    WANT(geometryShader);
    WANT(tessellationShader);
    WANT(shaderTessellationAndGeometryPointSize);
    /* images bigger than one allocation may be are bound in pieces, on the graphics queue (memory.c) */
    int sparse_queue = 0;
    {
        u32 nq = 0;
        VkQueueFamilyProperties* q = snx_vk_queue_families(g_vk.phys, &nq);
        sparse_queue = g_vk.gfx_family < nq && (q[g_vk.gfx_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT);
        free(q);
    }
    if (sparse_queue) WANT(sparseBinding);
#undef WANT
    /* points out of tessellation and geometry programs take the size those write, as the vertex program's do */
    g_shader_stage_point_size = want->shaderTessellationAndGeometryPointSize != 0;
    g_shader_geometry = want->geometryShader != 0;
    g_shader_tessellation = want->tessellationShader != 0;
    const char** ext = g_vk.dev_ext;
    int has_pipeline_robustness = 0, has_robustness2 = 0;
    int has_budget = 0;
    u32 next = 0;
    int has_shader_clock = 0, has_feedback_dynamic = 0, has_cond = 0, has_provoking = 0, video_exts = 0;
    int has_portability = 0, has_reconvergence = 0;
    int has_calibrated = 0;
    if (!g_opt_headless) ext[next++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    {
        SnxVkExtensions e;
        snx_vk_device_extensions(g_vk.phys, &e);
        g_vk.has_divisor = snx_vk_extensions_has(&e, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        g_vk.has_xfb = snx_vk_extensions_has(&e, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
        g_vk.has_border_color = snx_vk_extensions_has(&e, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        has_shader_clock = snx_vk_extensions_has(&e, VK_KHR_SHADER_CLOCK_EXTENSION_NAME);
        has_reconvergence = snx_vk_extensions_has(&e, VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME);
        g_vk.has_feedback = snx_vk_extensions_has(&e, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
        has_feedback_dynamic = snx_vk_extensions_has(&e, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
        g_vk.has_local_read = snx_vk_extensions_has(&e, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
        has_cond = snx_vk_extensions_has(&e, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
        has_provoking = snx_vk_extensions_has(&e, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
        has_pipeline_robustness = snx_vk_extensions_has(&e, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
        has_robustness2 = snx_vk_extensions_has(&e, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        /* the driver's budget for each heap, the other processes' use taken out, for VMA: without it VMA took
           80% of a heap's size as its budget, blind to what the desktop and other programs hold */
        has_budget = snx_vk_extensions_has(&e, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        /* the GPU's clock next to the client's, for the profiler's GPU track (gpuprof.c's calibration) */
        has_calibrated = snx_vk_extensions_has(&e, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        /* a portability driver's subset must be enabled when it has one (MoltenVK) */
        has_portability = snx_vk_extensions_has(&e, "VK_KHR_portability_subset");
        if (has_portability) ext[next++] = "VK_KHR_portability_subset";
        video_exts = snx_vk_extensions_has(&e, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) +
                     snx_vk_extensions_has(&e, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME) +
                     snx_vk_extensions_has(&e, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);
        snx_vk_extensions_free(&e);
    }
    if (has_budget) ext[next++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    /* the extension only with a host time domain among the calibrateable ones: CLOCK_MONOTONIC_RAW is the clock
       the Tracy client keeps its own time in, so the two domains' pair maps the GPU's timestamps onto it */
    if (has_calibrated) {
        u32 nd = 0;
        if (VK_OK(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(g_vk.phys, &nd, NULL)) && nd) {
            VkTimeDomainEXT* dom = malloc(sizeof *dom * nd);
            if (dom) {
                if (VK_OK(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(g_vk.phys, &nd, dom)))
                    for (u32 i = 0; i < nd; i++)
                        if (dom[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT) g_vk.has_calibrated_host = 1;
                free(dom);
            }
        }
        if (g_vk.has_calibrated_host) ext[next++] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    }
    g_vk.video_family = g_settings.vulkan_video_decode && video_exts == 3 ? video_decode_family() : ~0u;
    if (g_vk.video_family != ~0u) {
        ext[next++] = VK_KHR_VIDEO_QUEUE_EXTENSION_NAME;
        ext[next++] = VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME;
        ext[next++] = VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME;
        LOG("vulkan: H.264 movies decode with Vulkan Video, on queue family %u (experimental)\n", g_vk.video_family);
    } else if (g_settings.vulkan_video_decode)
        WARN("vulkan: Vulkan Video decode was asked for, but the device has no H.264 decode queue: FFmpeg decodes "
             "movies on the CPU\n");
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT fdiv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT fxfb = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceShaderClockFeaturesKHR qclock = {.sType =
                                                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR qreconv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR};
    VkPhysicalDevicePipelineRobustnessFeaturesEXT qpipeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT qrobust = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features q12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features q11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT qloop = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT qloop_dyn = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR qlocal = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT qcond = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT qprov = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    /* with the subset enabled, each of its features is off until the device asks for it: a view's swizzle, for one */
    PortabilityFeatures qport = {.sType = PORTABILITY_FEATURES_STYPE};
    int robustness = has_pipeline_robustness && has_robustness2;
    void* const query[] = {&f2,
                           &q11,
                           &q12,
                           g_vk.has_divisor ? &fdiv : NULL,
                           g_vk.has_border_color ? &fbc : NULL,
                           g_vk.has_xfb ? &fxfb : NULL,
                           g_vk.has_feedback ? &qloop : NULL,
                           has_feedback_dynamic ? &qloop_dyn : NULL,
                           g_vk.has_local_read ? &qlocal : NULL,
                           has_cond ? &qcond : NULL,
                           has_provoking ? &qprov : NULL,
                           has_portability ? &qport : NULL,
                           has_reconvergence ? &qreconv : NULL,
                           robustness ? &qpipeline : NULL,
                           robustness ? &qrobust : NULL,
                           has_shader_clock ? &qclock : NULL};
    snx_vk_chain(query, sizeof query / sizeof *query);
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
    g_vk.has_vertex_robustness2 = qpipeline.pipelineRobustness && qrobust.robustBufferAccess2;
    g_vk.has_cond_render = has_cond && qcond.conditionalRendering;
    g_vk.has_provoking_last = has_provoking && qprov.provokingVertexLast;
    /* the dynamic state and the barriers within a pass only serve feedback loops */
    g_vk.has_feedback = g_vk.has_feedback && qloop.attachmentFeedbackLoopLayout;
    g_vk.has_feedback_dynamic =
        g_vk.has_feedback && has_feedback_dynamic && qloop_dyn.attachmentFeedbackLoopDynamicState;
    g_vk.has_local_read = g_vk.has_feedback && g_vk.has_local_read && qlocal.dynamicRenderingLocalRead;
    g_shader_maximal_reconvergence = has_reconvergence && qreconv.shaderMaximalReconvergence;
    g_shader_clock_scopes =
        has_shader_clock ? (qclock.shaderSubgroupClock ? 1 : 0) | (qclock.shaderDeviceClock ? 2 : 0) : 0;
    g_vk.has_divisor = g_vk.has_divisor && fdiv.vertexAttributeInstanceRateDivisor;
    g_vk.has_xfb = g_vk.has_xfb && fxfb.transformFeedback;
    g_vk.has_border_color = g_vk.has_border_color && fbc.customBorderColors && fbc.customBorderColorWithoutFormat;
    if (g_vk.has_border_color) { /* how many samplers may have a custom border color: as few as 32 */
        VkPhysicalDeviceCustomBorderColorPropertiesEXT cbp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 cp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &cbp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &cp2);
        g_vk.border_color_max = cbp.maxCustomBorderColorSamplers;
    }
    /* FFmpeg's decoder makes views of its two plane pictures */
    VkPhysicalDeviceVulkan11Features f11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .shaderDrawParameters = q11.shaderDrawParameters,
        .samplerYcbcrConversion = g_vk.video_family != ~0u && q11.samplerYcbcrConversion};
    g_vk.has_shader_draw_parameters = q11.shaderDrawParameters != 0;
    /* Enable mirror clamp and vertex shader Layer output when the device supports them. */
    g_vk.has_mirror_clamp = q12.samplerMirrorClampToEdge;
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &f11,
                                            .samplerMirrorClampToEdge = q12.samplerMirrorClampToEdge,
                                            .timelineSemaphore = VK_TRUE,
                                            .shaderFloat16 = q12.shaderFloat16,
                                            .shaderOutputViewportIndex = q12.shaderOutputViewportIndex,
                                            .shaderOutputLayer = q12.shaderOutputLayer,
                                            .shaderSampledImageArrayNonUniformIndexing =
                                                q12.shaderSampledImageArrayNonUniformIndexing};
    /* shaders pick an indexed texture per invocation from an array (SNX_VKB_TEXARRAY) */
    g_shader_texture_arrays = q12.shaderSampledImageArrayNonUniformIndexing != 0;
    g_shader_float16 = q12.shaderFloat16 != 0;
    g_shader_output_layer = q12.shaderOutputLayer != 0; /* ShaderLayer: a vertex or tessellation program's layer */
    g_shader_viewport_index = have.multiViewport ? 2 | (q12.shaderOutputViewportIndex ? 1 : 0) : 0;
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = &f12,
                                            .synchronization2 = VK_TRUE,
                                            .dynamicRendering = VK_TRUE,
                                            /* KIL demotes the lane; S2R queries its helper status */
                                            .shaderDemoteToHelperInvocation = VK_TRUE};
    void* chain = &f13;
    if (g_vk.has_divisor) {
        fdiv = (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
            .pNext = chain,
            .vertexAttributeInstanceRateDivisor = VK_TRUE};
        chain = &fdiv;
        ext[next++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    }
    if (g_vk.has_border_color) {
        fbc = (VkPhysicalDeviceCustomBorderColorFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
            .pNext = chain,
            .customBorderColors = VK_TRUE,
            .customBorderColorWithoutFormat = VK_TRUE};
        chain = &fbc;
        ext[next++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    }
    if (g_vk.has_xfb) {
        fxfb = (VkPhysicalDeviceTransformFeedbackFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
            .pNext = chain,
            .transformFeedback = VK_TRUE};
        chain = &fxfb;
        ext[next++] = VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME;
    }
    VkPhysicalDeviceShaderClockFeaturesKHR fclock = {.sType =
                                                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR,
                                                     .pNext = chain,
                                                     .shaderSubgroupClock = !!(g_shader_clock_scopes & 1),
                                                     .shaderDeviceClock = !!(g_shader_clock_scopes & 2)};
    if (g_shader_clock_scopes) {
        chain = &fclock;
        ext[next++] = VK_KHR_SHADER_CLOCK_EXTENSION_NAME;
    }
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR freconv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR,
        .pNext = chain,
        .shaderMaximalReconvergence = VK_TRUE};
    if (g_shader_maximal_reconvergence) {
        chain = &freconv;
        ext[next++] = VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME;
    }
    PortabilityFeatures fport = qport;
    fport.pNext = chain;
    if (has_portability) chain = &fport;
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT floop = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT,
        .pNext = chain,
        .attachmentFeedbackLoopLayout = VK_TRUE};
    if (g_vk.has_feedback) {
        chain = &floop;
        ext[next++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;
    }
    VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT floop_dyn = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT,
        .pNext = chain,
        .attachmentFeedbackLoopDynamicState = VK_TRUE};
    if (g_vk.has_feedback_dynamic) {
        chain = &floop_dyn;
        ext[next++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME;
    }
    /* a barrier within a vkCmdBeginRendering pass needs this feature (VUID-vkCmdPipelineBarrier2-None-09553) */
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flocal = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR,
        .pNext = chain,
        .dynamicRenderingLocalRead = VK_TRUE};
    if (g_vk.has_local_read) {
        chain = &flocal;
        ext[next++] = VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
    }
    /* the render enable's comparison of an occlusion query's count, made on the GPU (gpu/vk/query.c) */
    VkPhysicalDeviceConditionalRenderingFeaturesEXT fcond = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT,
        .pNext = chain,
        .conditionalRendering = VK_TRUE};
    if (g_vk.has_cond_render) {
        chain = &fcond;
        ext[next++] = VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME;
    }
    VkPhysicalDeviceProvokingVertexFeaturesEXT fprov = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
        .pNext = chain,
        .provokingVertexLast = VK_TRUE};
    if (g_vk.has_provoking_last) {
        chain = &fprov;
        ext[next++] = VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME;
    }
    VkPhysicalDevicePipelineRobustnessFeaturesEXT fpipeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT,
        .pNext = chain,
        .pipelineRobustness = VK_TRUE};
    if (g_vk.has_vertex_robustness2) {
        chain = &fpipeline;
        ext[next++] = VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME;
        ext[next++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    }
    u32 nq = g_vk.compute_family == g_vk.gfx_family ? 1 : 2;
    if (g_vk.video_family != ~0u)
        qci[nq++] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueFamilyIndex = g_vk.video_family,
                                              .queueCount = 1,
                                              .pQueuePriorities = prio};
    /* SWR: DLSS's device extensions and its feature chain (dlss.c) */
    vk_dlss_device_exts(g_vk.phys, ext, &next, (u32)(sizeof g_vk.dev_ext / sizeof g_vk.dev_ext[0]));
    { const void* vk_dlss_device_chain(const void* next); chain = (void*)vk_dlss_device_chain(chain); }
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .pNext = chain,
                              .queueCreateInfoCount = nq,
                              .pQueueCreateInfos = qci,
                              .enabledExtensionCount = next,
                              .ppEnabledExtensionNames = ext,
                              .pEnabledFeatures = &g_vk.feat};
    if (!VK_OK(vkCreateDevice(g_vk.phys, &dci, NULL, &g_vk.dev))) return 0;
    g_vk.gfx_queues = second ? 2 : 1;
    g_vk.dev_ext_n = next;
    g_vk.feat11 = f11;
    g_vk.feat12 = f12;
    g_vk.feat13 = f13;
    g_vk.feat11.pNext = &g_vk.feat12;
    g_vk.feat12.pNext = &g_vk.feat13;
    g_vk.feat13.pNext = NULL;
    g_vk.feat2 = (VkPhysicalDeviceFeatures2){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &g_vk.feat11, .features = g_vk.feat};
    volkLoadDevice(g_vk.dev);
    pm_lock_announce(&g_vklock, "vulkan"); /* the device's calls, every thread's */
    vkGetDeviceQueue(g_vk.dev, g_vk.gfx_family, 0, &g_vk.gfx);
    vkGetDeviceQueue(g_vk.dev, g_vk.compute_family, second ? 1 : 0, &g_vk.compute);
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr,
                             .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    vk_mem_init(sparse_queue);
    /* VMA's blocks are 256 MiB at most already, and kept within a smaller cap */
    VmaAllocatorCreateInfo vai = {.physicalDevice = g_vk.phys,
                                  .device = g_vk.dev,
                                  .instance = g_vk.instance,
                                  .preferredLargeHeapBlockSize = g_vk.alloc_cap < (256ull << 20) ? g_vk.alloc_cap : 0,
                                  .vulkanApiVersion = VK_API_VERSION_1_3,
                                  .pVulkanFunctions = &vf,
                                  .flags = has_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0};
    if (!VK_OK(vmaCreateAllocator(&vai, &g_vk.vma))) return 0;
    vk_dlss_init();
    LOG("vulkan: %s, Vulkan %u.%u.%u, driver %08x, graphics queue family %u, compute %u%s\n", g_vk.props.deviceName,
        VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion),
        VK_API_VERSION_PATCH(g_vk.props.apiVersion), g_vk.props.driverVersion, g_vk.gfx_family, g_vk.compute_family,
        g_vk.compute_family != g_vk.gfx_family ? " (separate family)"
        : second                               ? " (a second queue)"
                                               : " (the graphics queue)");
    if (g_vk.has_feedback)
        LOG("vulkan: draws sample the color targets they render to in place (attachment feedback loops, %s, %s)\n",
            g_vk.has_feedback_dynamic ? "enabled by dynamic state" : "enabled per pipeline",
            g_vk.has_local_read ? "ordered by barriers within the pass" : "ordered by beginning the pass again");
    else
        LOG("vulkan: draws that sample a color target they render to read a copy of it (no attachment feedback "
            "loops)\n");
    return 1;
}

/* the swapchain destroyed. Its images' semaphores: a present may still wait on one after the device is idle (the
   presentation engine isn't covered by vkDeviceWaitIdle), so they go a swapchain later, when those presents are long
   done */
static void swapchain_retire(void) {
    static VkSemaphore retired[VK_SC_MAX];
    static u32 nretired;
    for (u32 i = 0; i < nretired; i++) vkDestroySemaphore(g_vk.dev, retired[i], NULL);
    nretired = 0;
    for (u32 i = 0; i < g_vk.sc_count; i++) retired[nretired++] = g_vk.sc_done[i];
    vk_ui_swapchain_gone();
    {
        void vk_hdr_swapchain_gone(void);
        vk_hdr_swapchain_gone(); /* SWR: HDR output's views of the images (hdr.c) */
    }
    vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    g_vk.sc_count = 0;
}

/* the window's surface made anew, its swapchain to follow: Android destroys a window's surface while the app is in
   the background, and the Vulkan surface on it is lost for good */
int vk_surface_recreate(void) {
    vk_rec_sync(); /* the device wait covers only what was submitted */
    vkDeviceWaitIdle(g_vk.dev);
    if (g_vk.swapchain) swapchain_retire();
    vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    if (backend_vk_surface(g_vk.instance, &g_vk.surface)) {
        VkBool32 present = VK_FALSE; /* the queue chosen for the old surface, asked again for the new one */
        if (vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, g_vk.gfx_family, g_vk.surface, &present) == VK_SUCCESS &&
            present)
            return 1;
        WARN_LIMIT(4, "vulkan: the window's new surface can't be presented to from the graphics queue\n");
        return 0;
    }
    WARN_LIMIT(4, "vulkan: the window's surface could not be made again\n");
    return 0;
}

int vk_window_release(void) {
    int waited = 0;
    /* a thread that holds the renderer and doesn't let go (stuck on a lost device) must not keep the error unseen */
    while (!vk_trylock()) {
        if (++waited == 200) return 0;
        plat_sleep_ns(10000000);
    }
    vk_rec_sync();
    vkDeviceWaitIdle(g_vk.dev); /* a lost device answers at once */
    if (g_vk.swapchain) swapchain_retire();
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    /* every hold of this thread's at once: it won't return to the callers that took them */
    t_vkdepth = 0;
    vk_let_go();
    return 1;
}

int vk_swapchain_create(int srgb) {
    if (g_opt_headless) return 0; /* no WSI calls, even on a resize/recreate path */
    VkSurfaceCapabilitiesKHR caps;
    VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
    if (cr != VK_SUCCESS) { /* asked again on each present while it fails: said a few times */
        WARN_LIMIT(4, "vulkan: the window's surface could not be queried: %s (%d)\n", snx_vk_result_str(cr), (int)cr);
        if (cr == VK_ERROR_SURFACE_LOST_KHR) g_vk.sc_lost = 1;
        return 0;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        WARN("vulkan: the window's surface can't be drawn to by copies, which presenting needs\n");
        return 0;
    }
    int ww, wh; /* what presents compare against, whatever the surface makes of it */
    backend_window_size(&ww, &wh);
    /* Wayland: the swapchain decides, so the window's size */
    VkExtent2D ext = snx_vk_swap_extent(&caps, (VkExtent2D){ww > 0 ? (u32)ww : 0, wh > 0 ? (u32)wh : 0});
    if (!ext.width || !ext.height) return 0;

    /* 8-bit, in the encoding of the frames presented: a blit from an sRGB target to a UNORM image decodes the bytes,
       where the console's display shows them as they are */
    static const VkFormat srgb_formats[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
    static const VkFormat unorm_formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    VkSurfaceFormatKHR fmt;
    if (!(srgb && snx_vk_surface_format(g_vk.phys, g_vk.surface, srgb_formats, 2, &fmt)))
        snx_vk_surface_format(g_vk.phys, g_vk.surface, unorm_formats, 2, &fmt);
    {   /* SWR: HDR output (hdr.c): scRGB FP16, else HDR10 PQ, when hdr=1 and the display (Windows HDR on) offers it */
        extern int g_vk_sc_hdr;
        int vk_hdr_wanted(void);
        int was = g_vk_sc_hdr;
        g_vk_sc_hdr = 0;
        if (vk_hdr_wanted()) {
            u32 nf = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
            VkSurfaceFormatKHR* fs = calloc(nf ? nf : 1, sizeof *fs);
            if (fs) vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fs);
            else nf = 0;
            for (int want = 1; want <= 2 && !g_vk_sc_hdr; want++)
                for (u32 i = 0; i < nf; i++)
                    if ((want == 1 && fs[i].format == VK_FORMAT_R16G16B16A16_SFLOAT &&
                         fs[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) ||
                        (want == 2 && fs[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
                         fs[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)) {
                        fmt = fs[i];
                        g_vk_sc_hdr = want;
                        break;
                    }
            free(fs);
            if (!g_vk_sc_hdr && !was)
                LOG_LIMIT(1, "vulkan: hdr=1 but the display offers no HDR format (is Windows HDR on?): SDR output\n");
        }
        if (g_vk_sc_hdr) srgb = 0; /* the HDR pass writes the swapchain; the frame goes to its own SDR image */
    }
    if (srgb && fmt.format != VK_FORMAT_B8G8R8A8_SRGB && fmt.format != VK_FORMAT_R8G8B8A8_SRGB) {
        LOG_LIMIT(1, "vulkan: the window has no sRGB format: frames in sRGB show darker\n");
    }

    /* vsync=1: FIFO, which every device has. Otherwise mailbox, else immediate, else FIFO all the same. Read once: the
       main thread's .ini reload writes it */
    int vsync = __atomic_load_n(&g_settings.vsync, __ATOMIC_RELAXED);
    u32 nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &nm, NULL);
    VkPresentModeKHR* ms = calloc(nm ? nm : 1, sizeof *ms);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &nm, ms);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (u32 i = 0; i < nm && !vsync; i++)
        if (ms[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = ms[i];
    if (mode == VK_PRESENT_MODE_FIFO_KHR && !vsync)
        for (u32 i = 0; i < nm; i++)
            if (ms[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = ms[i];
    free(ms);

    /* an image for each frame in flight beyond what the driver keeps: a frame's present waits in the recording ring
       (vk_rec_present) while the next frame acquires, which takes no wait, so with one image fewer it found none */
    u32 count = snx_vk_swap_image_count(&caps, VK_FRAMES, VK_SC_MAX);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) /* the lowest one it has */
        alpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    /* frames are drawn upright: a display held other than its own way (a phone held wide) has its compositor turn
       them, where its current transform would say they come turned already */
    VkSurfaceTransformFlagBitsKHR transform = caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                  ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                  : caps.currentTransform;
    VkSwapchainKHR old = g_vk.swapchain;
    if (old) {
        vk_rec_sync();
        vkDeviceWaitIdle(g_vk.dev);
    }
    vk_rec_present_result(); /* the old swapchain's presents, all made (a retire drained the ring), say nothing of it */
    VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                   .surface = g_vk.surface,
                                   .minImageCount = count,
                                   .imageFormat = fmt.format,
                                   .imageColorSpace = fmt.colorSpace,
                                   .imageExtent = ext,
                                   .imageArrayLayers = 1,
                                   .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .preTransform = transform,
                                   .compositeAlpha = alpha,
                                   .presentMode = mode,
                                   .clipped = VK_TRUE,
                                   .oldSwapchain = old};
    /* old is retired by the create, made or not: on any failure it goes too, and the next present starts anew */
    VkSwapchainKHR sc;
    VkSemaphore done[VK_SC_MAX];
    u32 nimg = 0, ndone = 0;
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &sc);
    if (cr != VK_SUCCESS) { /* tried again on each present while it fails: said a few times */
        WARN_LIMIT(4, "vulkan: the swapchain could not be made: %s (%d)\n", snx_vk_result_str(cr), (int)cr);
        if (cr == VK_ERROR_SURFACE_LOST_KHR) g_vk.sc_lost = 1;
        if (old) swapchain_retire();
        return 0;
    }
    vkGetSwapchainImagesKHR(g_vk.dev, sc, &nimg, NULL);
    if (nimg > VK_SC_MAX) WARN("vulkan: the swapchain has %u images, more than the %d taken\n", nimg, VK_SC_MAX);
    while (nimg <= VK_SC_MAX && ndone < nimg && VK_OK(vkCreateSemaphore(g_vk.dev, &si, NULL, &done[ndone]))) ndone++;
    if (nimg > VK_SC_MAX || ndone < nimg) {
        while (ndone) vkDestroySemaphore(g_vk.dev, done[--ndone], NULL);
        vkDestroySwapchainKHR(g_vk.dev, sc, NULL);
        if (old) swapchain_retire();
        return 0;
    }
    if (old) swapchain_retire();
    g_vk.swapchain = sc;
    g_vk.sc_extent = ext;
    g_vk.sc_win_w = ww;
    g_vk.sc_win_h = wh;
    g_vk.sc_srgb = fmt.format == VK_FORMAT_B8G8R8A8_SRGB || fmt.format == VK_FORMAT_R8G8B8A8_SRGB;
    g_vk.sc_format = fmt.format;
    g_vk.sc_turned = transform != caps.currentTransform;
    g_vk.sc_vsync = vsync;
    g_vk.sc_count = nimg;
    vkGetSwapchainImagesKHR(g_vk.dev, sc, &g_vk.sc_count, g_vk.sc_images);
    memcpy(g_vk.sc_done, done, sizeof done[0] * nimg);
    LOG("vulkan: swapchain %ux%u, %u images%s, %s\n", ext.width, ext.height, g_vk.sc_count,
        g_vk.sc_srgb ? ", sRGB" : "",
        mode == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
        : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                : "fifo");
    return 1;
}

int vk_init(void) {
    pm_init(&g_vklock);
    if (g_opt_headless) {
        /* Load libvulkan/vulkan-1 directly, or the driver the program's folders carry (an Apple app's MoltenVK): no SDL
           video driver or headless-surface extension is needed. */
        void* own = backend_vk_driver();
        if (own) volkInitializeCustom((PFN_vkGetInstanceProcAddr)own);
        else if (!VK_OK(volkInitialize())) return 0;
    } else {
        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)backend_vk_loader();
        if (!gipa) {
            WARN("vulkan: no Vulkan loader (is a Vulkan driver installed?)\n");
            return 0;
        }
        volkInitializeCustom(gipa);
    }
    vk_renderdoc_load(); /* its capture layer is enabled when the instance is created */
    if (!create_instance()) return 0;
    if (!g_opt_headless && !backend_vk_surface(g_vk.instance, &g_vk.surface)) {
        WARN("vulkan: the window backend could not create a surface\n");
        return 0;
    }
    if (!create_device()) return 0;
    return g_opt_headless ? 1 : vk_swapchain_create(0);
}
