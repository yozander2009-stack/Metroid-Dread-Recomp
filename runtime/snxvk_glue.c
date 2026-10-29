/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* renderer=vulkan_native (experimental): the adapter between our runtime and ScarletNX's GPU front end + Vulkan
   renderer (runtime/snxvk, GPL-3.0, built into libsnxvk.a with every symbol renamed snxv_*). When the library isn't
   linked (no SWR_HAVE_SNXVK) this file only holds the "off" answers, and renderer=vulkan_native falls back to OpenGL.

   With it on, our nvdrv hands GPFIFO submissions to snxv_gpu_submit instead of our gpu.c, the vi present goes to the
   Vulkan renderer, mapping and memory changes are reported to its caches, and guest memory is reserved with
   MEM_WRITE_WATCH (its caches see CPU writes through GetWriteWatch). Our own GPU front end then simply gets no work. */
#include "rt.h"
#include "gpu.h"

int g_snxvk_want;   /* renderer=vulkan_native in the .ini (backend_ini) */
int g_snxvk_on;     /* the Vulkan renderer started: submissions and presents go to it */
int snxvk_dlss_mode;   /* ini dlss: 0 off, 1 DLAA, 2 quality, 3 balanced, 4 performance (native Vulkan only) */

#if defined(SWR_HAVE_SNXVK) && defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- what the ScarletNX objects call that our runtime doesn't have under that name */
char g_cachedir[1024];
int g_opt_headless;
int snxvk_opt_vsync=1;      /* their g_opt_vsync: FIFO presentation (ours means "force vsync" for GL) */

int pm_trylock(PMutex* m){ return TryAcquireSRWLockExclusive(&m->l)? 1 : 0; }

typedef struct { const u8* data; u64 size; int borrowed; } PFileMap;
int plat_file_map(PFileMap* m, const char* path){
  memset(m,0,sizeof *m); FILE* f=fopen(path,"rb"); if(!f) return -1;
  _fseeki64(f,0,SEEK_END); i64 n=_ftelli64(f); _fseeki64(f,0,SEEK_SET);
  if(n<0){ fclose(f); return -1; }
  if(n){ u8* p=malloc((size_t)n); if(!p || fread(p,1,(size_t)n,f)!=(size_t)n){ free(p); fclose(f); return -1; } m->data=p; m->size=(u64)n; }
  fclose(f); return 0; }
void plat_file_unmap(PFileMap* m){ if(!m->borrowed) free((void*)m->data); memset(m,0,sizeof *m); }
int plat_replace_file(const char* tmp, const char* dst){ return MoveFileExA(tmp,dst,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)? 0 : -1; }

extern int g_ww_ok;
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)){
  PVOID a[1024]; if(!g_ww_ok) return -1;
  for(u64 it=0, max=size/(4096ull*1024)+2; it<max; it++){
    ULONG_PTR n=1024; ULONG gran=0;
    if(GetWriteWatch(WRITE_WATCH_FLAG_RESET,p,(SIZE_T)size,a,&n,&gran)) return -1;
    for(ULONG_PTR i=0;i<n;i++) cb((u8*)a[i],gran);
    if(n<1024) return 0; }
  return 1; }


/* no on-screen controls or menu overlay here: an empty list, and a 1x1 clear atlas so the renderer's upload is valid */
static const u32 g_atlas_px[1]={0};
const u32* ui_atlas(int* w, int* h, u32* version){ *w=1; *h=1; *version=1; return g_atlas_px; }
void ui_atlas_done(void){}
static PMutex g_ui_lk; static int g_ui_lk_init;
void ui_lock(void){ if(!g_ui_lk_init){ pm_init(&g_ui_lk); g_ui_lk_init=1; } pm_lock(&g_ui_lk); }
void ui_unlock(void){ pm_unlock(&g_ui_lk); }
void overlay_draw(void* l, int sw, int sh){ (void)l; (void)sw; (void)sh; }
void vpad_draw(void* l, const int safe[4]){ (void)l; (void)safe; }


/* ---- ScarletNX main b5f4f14f2bfc asks these of its own runtime. Ours has no GPU-owned memory traps, no shipped-shader
   pass (its loader and RomFS readers), no NVN trace, no launcher overlay and no FFmpeg NVDEC, so those answer "none" */
typedef struct { u64 base, end, heap, heap_size, alias, alias_size, stack, stack_size, kern, kern_end; } SnxMemLayout;
SnxMemLayout g_layout={AS_BASE,AS_SIZE,HEAP_BASE,HEAP_SIZE,ALIAS_BASE,ALIAS_SIZE,STACK_BASE,STACK_SIZE,KERN_BASE,KERN_END};
int g_own_any;
int own_init(const void* hooks){ (void)hooks; return -1; }   /* no traps: the renderer lands GPU writes instead (owned.c) */
int own_begin(u64 a, u64 n, void* r, u64 o){ (void)a; (void)n; (void)r; (void)o; return -1; }
void own_drop(u64 a, u64 n){ (void)a; (void)n; }
int own_hash(u64 a, u64 n, u64* h){ (void)a; (void)n; (void)h; return -1; }
void own_acquire(u64 a, u64 n){ (void)a; (void)n; }
u32 own_stage(u64 a, u64 n, u8* dst, void* out, u32 max){ (void)a; (void)n; (void)dst; (void)out; (void)max; return 0; }
u64 own_landings(void){ return 0; }
/* syncpoint increments made under the renderer's lock: ScarletNX defers their wake-ups because its kernel lock can wait
   on a memory trap answered under that lock; ours can't, so they wake at once */
void syncpt_add_quiet(u32 id, u32 n){ extern void syncpt_incr(u32); while(n--) syncpt_incr(id); }
void syncpt_events(void){}
u64 kernel_find_symbol_exact(const char* name){ (void)name; return 0; }
void loader_segments(void* each, void* ctx){ (void)each; (void)ctx; }
/* the shipped-shader pass's RomFS readers (shipped_pass.c, prepare_shaders on): Metroid Dread keeps every shader in
   system/shd/*.bshdat (the OpenGL pre-build reads the same), so only that folder is listed, not the 5 GB of packs */
int romfs_files(int (*each)(void*,const char*,u64,u64), void* ctx){
  extern int romfs_list(const char*, int (*)(void*,const char*,u64,u64), void*); return romfs_list("/system/shd/",each,ctx); }
int romfs_peek(u64 off, void* dst, u64 n){ return romfs_read(off,dst,n)? -1 : 0; }
int overlay_display_percent(void){ return 100; }
void prepare_screen_draw(void* l, int sw, int sh, const int safe[4], int pct){ (void)l; (void)sw; (void)sh; (void)safe; (void)pct; }
int prepare_screen_shown(void){ return 0; }
void vpad_showcase_draw(void* l, const int safe[4], int fps){ (void)l; (void)safe; (void)fps; }
int vpad_showcase_fps(u32 presents, u64 now){ (void)presents; (void)now; return 0; }
u64 unattended_ms(void){ return 0; }
int vic_gpu_aligned(const void* j){ (void)j; return 0; }   /* their VIC path: our host1x composes video itself */
void vic_gpu_params(const void* j, void* p, const u32 in_base[16], u32 oc_base){ (void)j; (void)p; (void)in_base; (void)oc_base; }
void* vk_nvdec_hw_device(void){ return NULL; }
int vk_nvdec_write(const void* p){ (void)p; return 0; }
int plat_cpus(int* ids, int max){ int n=(int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); if(n>max) n=max; for(int i=0;i<n;i++) ids[i]=i; return n; }
const char* plat_dl_error(void){ static char b[64]; snprintf(b,sizeof b,"error %lu",(unsigned long)GetLastError()); return b; }
int plat_setenv(const char* name, const char* value){ char b[600]; snprintf(b,sizeof b,"%s=%s",name,value? value : ""); SetEnvironmentVariableA(name,value); return _putenv(b); }
u32 plat_tid(void){ return (u32)GetCurrentThreadId(); }
#ifdef SWR_SNXVK_V2
const char* snx_build_commit(void){ return "112db409"; }
const char* snx_build_info(void){ return "ScarletNX renderer 112db409 in the SwitchRecomp runtime"; }
/* ScarletNX main 112db409 asks these too: no HUD/stats overlay, no save states, no profiler lock names, the guest's
   address space as the old fixed 128 GiB (AS_SIZE), no GPU-owned memory traps (own_init says none) */
u64 g_as_size=0x2000000000ull;
void* backend_vk_driver(void){ return NULL; }   /* the system's Vulkan loader serves */
void hud_draw(void* l, const int safe[4], int scale){ (void)l; (void)safe; (void)scale; }
void stats_draw(void* l, const int safe[4], int scale, u64 now){ (void)l; (void)safe; (void)scale; (void)now; }
void stats_frame(u64 now){ (void)now; }
void overlay_place(int sw, int sh, int* x, int* y, int* scale){ (void)sw; (void)sh; *x=0; *y=0; *scale=1; }
u32 own_lookup(u64 a, u64 n, void* out, u32 max){ (void)a; (void)n; (void)out; (void)max; return 0; }
int own_traps(void){ return 0; }
void pm_lock_announce(PMutex* m, const char* name){ (void)m; (void)name; }
void snx_path_native(char* p){ for(; p && *p; p++) if(*p=='/') *p='\\'; }
void* state_fn_find(const char* name){ (void)name; return NULL; }
const char* state_fn_name(void* fn){ (void)fn; return NULL; }
const u8* ui_image_get(u32* version){ if(version) *version=0; return NULL; }
#else
const char* snx_build_commit(void){ return "b5f4f14f2bfc"; }
const char* snx_build_info(void){ return "ScarletNX renderer b5f4f14f2bfc in the SwitchRecomp runtime"; }
#endif
char* snx_executable(const char* argv0){ (void)argv0; char b[1300]; snprintf(b,sizeof b,"%s%s.exe",g_exedir,g_exename); return _strdup(b); }
char* snx_format(const char* fmt, ...){ va_list a; va_start(a,fmt); int n=vsnprintf(NULL,0,fmt,a); va_end(a); if(n<0) return NULL;
  char* p=malloc((size_t)n+1); if(!p) return NULL; va_start(a,fmt); vsnprintf(p,(size_t)n+1,fmt,a); va_end(a); return p; }
char* snx_os_error(char* out, size_t size){ DWORD e=GetLastError(); if(!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,e,0,out,(DWORD)size,NULL)) snprintf(out,size,"error %lu",(unsigned long)e);
  size_t n=strlen(out); while(n && (out[n-1]=='\r'||out[n-1]=='\n'||out[n-1]=='.')) out[--n]=0; return out; }
/* ---- the ScarletNX side, renamed */
#ifdef SWR_SNXVK_V2   /* the renderer's entries through swr_settings.c's accessors: no mirror of its Renderer struct */
extern int snxv_swr_r_start(void);
extern void snxv_swr_r_present(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4], u32 transform);
extern u64 snxv_swr_r_screenshot(const char* path);
extern void snxv_swr_r_save(int at_exit);
#define SNXR_START() snxv_swr_r_start()
#define SNXR_PRESENT snxv_swr_r_present
#define SNXR_SCREENSHOT(p) snxv_swr_r_screenshot(p)
#define SNXR_SAVE(x) snxv_swr_r_save(x)
#else
typedef struct {   /* mirror of their Renderer (gpu/renderer.h, ScarletNX main b5f4f14f2bfc), same order */
  const char* name; int (*start)(void); void (*mem_may_have_changed)(void);
#ifdef SWR_SNXVK_V2
  void* stream_run;   /* main 112db409 */
#endif
  void* clear; void* draw; void* method_barrier; void* draw_texture; void* blit; void* vic_compose;
  void* nvdec_write; void* nvdec_hw_device; void* dma_copy; void* blit_mem;
  void (*rt_flush)(u64); void (*rt_reload)(u64); void* dispatch; void* cs_mem_need; void* mem_pending; void* gfx_mem_need;
  void* before_syncwait; void* stream_incr; void* release_write; void* release_need; void* release_read; void* release_value;
  void* render_predicate; void* render_predicate_drop;
  void* samples_reset; void* samples_enable; void* samples_report; void* stream_end; void* release_retire; void* report_stall;
#ifdef SWR_SNXVK_V2
  void* idle; void* reset; void* flush_targets; void* save_targets; void* load_targets;
#endif
  void (*present_buffer)(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4], u32 transform);
  u64 (*screenshot)(const char* path);
#ifdef SWR_SNXVK_V2
  void* thumbnail;
#endif
  void (*refresh)(void); void (*save)(int at_exit); int (*release_window)(void);
  void* prepare;
} SnxRenderer;
extern const SnxRenderer snxv_vk_renderer;
#define SNXR_START() snxv_vk_renderer.start()
#define SNXR_PRESENT snxv_vk_renderer.present_buffer
#define SNXR_SCREENSHOT(p) (snxv_vk_renderer.screenshot? snxv_vk_renderer.screenshot(p) : 0)
#define SNXR_SAVE(x) do{ if(snxv_vk_renderer.save) snxv_vk_renderer.save(x); }while(0)
#endif
extern void snxv_gpu_init(void);
extern void snxv_gpu_submit(const void* entries, u32 n, u32 channel, u32 incs, u32 wait_id, u32 wait_val, u32 expect);
extern void snxv_gpu_kick(void);
extern void snxv_gpu_channel_close(u32 channel);
extern void snxv_gpu_release_retire(u32 channel);
extern void snxv_gpu_mem_written(u64 gva, u64 size);
extern void snxv_gpu_mem_changed(u64 cpu_addr, u64 len);
extern void (*snxv_g_gpu_mapping_begin)(u64 gva, u64 size, int full);
extern void (*snxv_g_gpu_mapping_end)(void);
extern int snxv_g_renderer_up;

/* before mem_init: their caches need write tracking on guest memory */
void snxvk_ini(const char* v){
  static int set_ww;   /* SWR_WW set here: a later renderer= line that isn't vulkan_native takes it back (the OpenGL
                          path with write tracking on spent ~85% of its thread in GetWriteWatch: 19 FPS) */
  g_snxvk_want= !strcmp(v,"vulkan_native");
  if(g_snxvk_want){ if(!getenv("SWR_WW")){ _putenv("SWR_WW=1"); set_ww=1; } }
  else if(set_ww){ _putenv("SWR_WW="); set_ww=0; } }

void snxvk_gpu_init(void){ if(g_snxvk_want) snxv_gpu_init(); }

/* the window is up (backend_win32): start Vulkan; 0 = it can't, the caller falls back to OpenGL */
int snxvk_start(void){
  if(!g_snxvk_want) return 0;
  extern int g_ww_ok;
  if(!g_ww_ok){ WARN("vulkan_native: guest memory has no write tracking: using OpenGL\n"); return 0; }
  snprintf(g_cachedir,sizeof g_cachedir,"%svulkan_native/",g_exedir); plat_mkdir(g_cachedir);
  { extern int g_opt_vsync; snxvk_opt_vsync=g_opt_vsync; }
  { /* our .ini into their g_settings (swr_settings.c). Their recording thread records the frame's commands; DLSS and
       HDR output record directly into the frame's command buffer, so with either on it stays off (both need a restart) */
    extern void snxv_swr_settings_set(float resolution, int shadows, int vsync, int anisotropy, int recording_thread);
    extern float g_opt_resf; extern int g_opt_shadows, g_opt_aniso, g_opt_hdr;
    int rec= !(snxvk_dlss_mode>0 || g_opt_hdr) && !getenv("SWR_SNXVK_NO_RECORD_THREAD");
    snxv_swr_settings_set(g_opt_resf,g_opt_shadows,snxvk_opt_vsync,g_opt_aniso,rec);
    LOG("vulkan_native: command recording thread %s\n",rec? "on" : "off (DLSS or HDR output records directly)");
    /* prebuild_shaders: the game's shaders translated in the background at start, and the pipeline recipes made from
       them (pipeline.c rc_merge); SWR_VK_NO_PREPARE=1 = off (tests) */
    { extern void snxv_swr_settings_prepare(int); extern int g_opt_prebuild;
      int prep= g_opt_prebuild && !getenv("SWR_VK_NO_PREPARE"); snxv_swr_settings_prepare(prep);
      if(prep) LOG("vulkan_native: the game's shaders are translated in the background, then the pipeline recipes made\n"); } }   /* FIFO only with forced_vsync=1, like the GL path: otherwise mailbox, paced by snxvk_present (FIFO at 144/240 FPS on a 239 Hz screen lost frames to vblank waits) */
  if(!SNXR_START()){ WARN("vulkan_native: Vulkan 1.3 renderer could not start (see above): using OpenGL\n");
    /* the guest memory was set up with write tracking for Vulkan; OpenGL would scan it on every submit (measured 19 FPS,
       GPU thread 98% busy) - it runs without, as when OpenGL is chosen */
    g_ww_ok=0; return 0; }
  g_snxvk_on=1;
  LOG("renderer: native Vulkan (experimental, ScarletNX renderer; shaders translated to SPIR-V)\n");
  return 1; }

void snxvk_submit(const void* entries, u32 n, u32 channel, u32 host_incs, u32 wait_id, u32 wait_val, u32 promised){
  snxv_gpu_submit(entries,n,channel,host_incs,wait_id,wait_val,promised); }
void snxvk_kick(void){ if(g_snxvk_on) snxv_gpu_kick(); }
void snxvk_channel_close(u32 id){ if(g_snxvk_on){ snxv_gpu_release_retire(id); snxv_gpu_channel_close(id); } }
/* GPU page table changes, for caches of what gmmu_translate returns (maxwell3d.c phys_dirty_since): gen moves at the
   start and end of every change, busy is nonzero while one runs */
volatile LONG64 snxvk_gmmu_gen; volatile LONG snxvk_gmmu_busy;
void snxvk_map_begin(u64 gva, u64 size){ InterlockedIncrement(&snxvk_gmmu_busy); InterlockedIncrement64(&snxvk_gmmu_gen); if(g_snxvk_on && snxv_g_gpu_mapping_begin) snxv_g_gpu_mapping_begin(gva,size,0); }
void snxvk_map_end(u64 gva, u64 size){ InterlockedIncrement64(&snxvk_gmmu_gen); InterlockedDecrement(&snxvk_gmmu_busy); if(!g_snxvk_on) return; snxv_gpu_mem_written(gva,size); if(snxv_g_gpu_mapping_end) snxv_g_gpu_mapping_end(); }
void snxvk_cpu_map_begin(u64 addr, u64 size){ if(g_snxvk_on && snxv_g_gpu_mapping_begin) snxv_g_gpu_mapping_begin(addr,size,1); }
void snxvk_cpu_map_end(u64 addr, u64 size, int changed){ if(!g_snxvk_on) return; if(snxv_g_gpu_mapping_end) snxv_g_gpu_mapping_end(); if(changed) snxv_gpu_mem_changed(addr,size); }
/* frame pacing, as our GL presentation thread does it (glq.c e_swap): the present waits for its deadline, so images
   reach the swapchain on an even cadence instead of whenever the game thread finishes (measured 12-20 ms spread
   without it, against 16.7 ms on GL). A frame more than half a period late starts a new cadence. vi_show_buffer's own
   limiter is skipped while this runs (it slept after the present, which left the present itself uneven) */
static void wait_deadline(u64 deadline){
  u64 now=plat_ns(); if(now>=deadline) return;
  if(deadline-now>500000ull) plat_sleep_ns((i64)(deadline-now-500000ull));
  while(plat_ns()<deadline) __builtin_ia32_pause(); }
/* HDR output (snxvk hdr.c): the peak brightness the window's display reports through DXGI (Windows 10 1803+), in
   nits; 0 if unknown. dxgi.dll is loaded here, so the EXE doesn't import it */
#if !defined(__TINYC__)
#define COBJMACROS
#include <dxgi1_6.h>
#endif
int backend_hdr_peak_nits(void){
  static int cached=-1; if(cached>=0) return cached; cached=0;
#if !defined(__TINYC__)
  static const GUID iid_f1={0x770aae78,0xf26f,0x4dba,{0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87}};
  static const GUID iid_o6={0x068346e8,0xaaec,0x4b84,{0xad,0xd7,0x13,0x7f,0x51,0x3f,0x77,0xa1}};
  HMODULE m=LoadLibraryA("dxgi.dll"); if(!m) return 0;
  typedef HRESULT (WINAPI *CF)(REFIID,void**); CF cf=(CF)(void*)GetProcAddress(m,"CreateDXGIFactory1"); if(!cf) return 0;
  IDXGIFactory1* f=NULL; if(FAILED(cf(&iid_f1,(void**)&f)) || !f) return 0;
  extern void* backend_window_monitor(void); HMONITOR want=(HMONITOR)backend_window_monitor();
  IDXGIAdapter1* a; float best=0;
  for(UINT i=0; IDXGIFactory1_EnumAdapters1(f,i,&a)==S_OK; i++){
    IDXGIOutput* o;
    for(UINT j=0; IDXGIAdapter1_EnumOutputs(a,j,&o)==S_OK; j++){
      IDXGIOutput6* o6=NULL;
      if(SUCCEEDED(IDXGIOutput_QueryInterface(o,&iid_o6,(void**)&o6))){ DXGI_OUTPUT_DESC1 d; if(SUCCEEDED(IDXGIOutput6_GetDesc1(o6,&d)) && (!want || d.Monitor==want) && d.MaxLuminance>best) best=d.MaxLuminance; IDXGIOutput6_Release(o6); }
      IDXGIOutput_Release(o); }
    IDXGIAdapter1_Release(a); }
  IDXGIFactory1_Release(f);
  cached=(int)(best+0.5f);
#endif
  return cached; }
int snxvk_pacing(void){ extern int g_opt_fps; return g_snxvk_on && !getenv("SWR_SNXVK_NO_PACING") && (g_opt_fps==1 || (g_opt_fps>=60 && g_opt_fps<=240)); }
void snxvk_present(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt){
  /* the game's crop from QueueBuffer (svc_vi.c): handheld Dread draws 1280x720 into a 1600x900 buffer. Passing none
     showed the whole buffer, the picture in the top-left 80% of the screen (2026-10-29) */
  extern volatile i32 g_vi_crop[4]; i32 crop[4]; for(int i=0;i<4;i++) crop[i]=g_vi_crop[i];
  static u64 st_n, st_wait, st_pres, st_gap, last_out;   /* SWR_SNXVK_TRACE: averages every 600 presents */
  u64 t_in=plat_ns(), t_w=t_in;
  if(snxvk_pacing()){ extern volatile int g_pace_fps; static u64 deadline; int pf=g_pace_fps;   /* g_opt_fps, or 60 during slow motion */
    u64 per= pf>=30 && pf<=240? 1000000000ull/(u64)pf : 16666667ull;   /* 30: cinematics (main.c pace_update) */
    u64 next=deadline+per; if(deadline) wait_deadline(next);
    u64 now=plat_ns(); t_w=now; deadline= deadline && (i64)(now-next)<(i64)(per/2)? next : now; }
  { static u64 last_in; u64 t0=plat_ns(); SNXR_PRESENT(a,w,h,pitch,layout,bhl,fmt,crop,0); u64 t1=plat_ns();
    static int tr=-1; if(tr<0) tr=getenv("SWR_SNXVK_TRACE")!=NULL;
    if(t1-t0>250000000ull) LOG("vulkan_native: present_buffer took %.0f ms\n",(t1-t0)/1e6);
    if(tr && last_in && (t0-last_in>30000000ull || t1-t0>12000000ull)) LOG("vulkan_native: slow frame: game arrived %.1f ms after the last present, present took %.1f ms\n",(t0-last_in)/1e6,(t1-t0)/1e6);
    if(tr && last_out){ st_n++; st_wait+=t_w-t_in; st_pres+=t1-t0; st_gap+=t_in-last_out;
      if(st_n==600){ LOG("vulkan_native: per frame: game work %.2f ms, deadline wait %.2f ms, present call %.2f ms\n",st_gap/6e8,st_wait/6e8,st_pres/6e8); st_n=st_wait=st_pres=st_gap=0; } }
    last_out=t1; last_in=t1; } }
u64 snxvk_screenshot(const char* path){ return g_snxvk_on? SNXR_SCREENSHOT(path) : 0; }
void snxvk_tick(int tick){   /* frontend loop, 60 Hz */
  if(!g_snxvk_on) return;
  if(tick%300==0) SNXR_SAVE(0); }   /* it writes after a quiet stretch */
void snxvk_exit(void){ if(g_snxvk_on) SNXR_SAVE(1); }

#else   /* built without the library: the option is ignored */
void snxvk_ini(const char* v){ if(!strcmp(v,"vulkan_native")) WARN("renderer=vulkan_native: this build has no native Vulkan renderer (runtime/snxvk/libsnxvk.a): using OpenGL\n"); }
void snxvk_gpu_init(void){}
int snxvk_start(void){ return 0; }
void snxvk_submit(const void* e, u32 n, u32 c, u32 i, u32 wi, u32 wv, u32 p){ (void)e;(void)n;(void)c;(void)i;(void)wi;(void)wv;(void)p; }
void snxvk_kick(void){}
void snxvk_channel_close(u32 id){ (void)id; }
void snxvk_map_begin(u64 g, u64 s){ (void)g;(void)s; }
void snxvk_map_end(u64 g, u64 s){ (void)g;(void)s; }
void snxvk_cpu_map_begin(u64 a, u64 s){ (void)a;(void)s; }
void snxvk_cpu_map_end(u64 a, u64 s, int c){ (void)a;(void)s;(void)c; }
void snxvk_present(u64 a, u32 w, u32 h, u32 p, u32 l, u32 b, u32 f){ (void)a;(void)w;(void)h;(void)p;(void)l;(void)b;(void)f; }
int snxvk_pacing(void){ return 0; }
u64 snxvk_screenshot(const char* p){ (void)p; return 0; }
void snxvk_tick(int t){ (void)t; }
void snxvk_exit(void){}
#endif
