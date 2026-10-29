/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RenderDoc captures of chosen frames, for unattended runs: renderdoc=PREFIX ([Debug] or --renderdoc=) loads RenderDoc's library before the
   Vulkan instance exists (its capture layer is an implicit one, which the loader enables with
   ENABLE_VULKAN_RENDERDOC_CAPTURE), and renderdoc_frames=N,M,... names the frames to capture: a number is a
   present, a number with s a time in seconds on the clock --script counts from (the first frame after it), which a
   loaded machine doesn't move. A capture holds one guest frame: everything recorded after present N-1 up to present
   N, written to PREFIX_capture.rdc and then numbered ones. Frames that never reach the swapchain (offscreen, headless)
   are captured the same way, because the capture starts and ends here rather than at RenderDoc's present hook. The
   in-application API is RenderDoc's renderdoc_app.h (MIT, Baldur Karlsson), fetched by cmake/dependencies.cmake. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "renderdoc_app.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/unattended.h"

#define RD_FRAMES 64 /* frames to capture at most */

typedef struct {
    u64 at;   /* the present, or the time in ms; 0 once taken */
    int secs; /* at is a time */
} RdFrame;
static RENDERDOC_API_1_1_2* g_rd;
static RdFrame g_rd_frames[RD_FRAMES];
static int g_rd_nframes, g_rd_open;

void vk_renderdoc_load(void) {
    const char *prefix = g_settings.render_doc_captures, *frames = g_settings.render_doc_frames, *p;
    pRENDERDOC_GetAPI get_api;
    void* lib;
    if (!*prefix) return;
#ifdef _WIN32
    lib = plat_dl_open("renderdoc.dll");
#else
    lib = plat_dl_open("librenderdoc.so");
#endif
    if (!lib) {
        WARN("renderdoc: renderdoc is set but RenderDoc's library didn't load: %s\n", plat_dl_error());
        return;
    }
    get_api = (pRENDERDOC_GetAPI)plat_dl_sym(lib, "RENDERDOC_GetAPI");
    void* api = NULL; /* GetAPI stores a void*, which g_rd isn't */
    if (!get_api || !get_api(eRENDERDOC_API_Version_1_1_2, &api) || !(g_rd = api)) {
        WARN("renderdoc: the library has no in-application API 1.1.2\n");
        g_rd = NULL;
        return;
    }
    /* the capture layer comes in through the loader, which only enables it with this set */
    if (plat_setenv("ENABLE_VULKAN_RENDERDOC_CAPTURE", "1")) WARN("renderdoc: couldn't enable the capture layer\n");
    g_rd->SetCaptureFilePathTemplate(prefix);
    /* no overlay in the captured frames, and nothing taken but the presents asked for */
    g_rd->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    g_rd->SetCaptureKeys(NULL, 0);
    for (p = frames; *p && g_rd_nframes < RD_FRAMES;) {
        char* e;
        unsigned long long n = strtoull(p, &e, 10);
        if (e == p) break;
        g_rd_frames[g_rd_nframes].secs = *e == 's';
        g_rd_frames[g_rd_nframes].at = *e == 's' ? n * 1000 : n;
        if (*e == 's') e++;
        if (g_rd_frames[g_rd_nframes].at) g_rd_nframes++;
        p = *e == ',' ? e + 1 : e;
    }
    if (*p)
        WARN("renderdoc: renderdoc_frames: \"%s\" is no frame (N, or Ns for seconds): it captures the %d "
             "before it\n",
             p, g_rd_nframes);
    LOG("renderdoc: loaded, %d frame(s) to capture to %s\n", g_rd_nframes, prefix);
}

/* whether the frame after this present is one to capture (each is taken once) */
static int rd_wanted(u64 present) {
    u64 ms = unattended_ms();
    int i, want = 0;
    for (i = 0; i < g_rd_nframes; i++)
        if (g_rd_frames[i].at && (g_rd_frames[i].secs ? ms >= g_rd_frames[i].at : g_rd_frames[i].at == present + 1)) {
            g_rd_frames[i].at = 0;
            want = 1;
        }
    return want;
}
/* at the present that ends frame `present`, after its last submit and before the swapchain present, under vk_lock: the
   capture of this frame ends here, and the next frame's starts */
void vk_renderdoc_present(u64 present) {
    void* dev;
    int want;
    if (!g_rd || !g_vk.instance) return;
    dev = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(g_vk.instance);
    want = rd_wanted(present);            /* asked once: it takes the frame it answers for */
    if (g_rd_open || want) vk_rec_sync(); /* the frame's commands are recorded within the capture */
    if (g_rd_open) {
        g_rd_open = 0;
        if (g_rd->EndFrameCapture(dev, NULL)) {
            char path[1024];
            u32 len = 0, n = g_rd->GetNumCaptures();
            /* GetCapture writes the whole path, unbounded: its length first */
            if (n && g_rd->GetCapture(n - 1, NULL, &len, NULL) && len < sizeof path &&
                g_rd->GetCapture(n - 1, path, NULL, NULL))
                LOG("renderdoc: captured present %llu to %s\n", (unsigned long long)present, path);
        } else
            WARN("renderdoc: the capture of present %llu failed\n", (unsigned long long)present);
    }
    if (want) {
        g_rd->StartFrameCapture(dev, NULL);
        g_rd_open = 1;
    }
}
