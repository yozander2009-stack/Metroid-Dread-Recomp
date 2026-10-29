/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* SDL2 window backend, the macOS counterpart of backend_x11.c. macOS has no OpenGL 4.3, so the window carries a
   Vulkan surface (MoltenVK) and the native Vulkan renderer (snxvk_glue.c, renderer=vulkan_native) draws; the OpenGL
   renderer is compiled in but never started. Controllers, keyboard and sound (audio.c) come from the same SDL2.
   Cocoa wants its windows on the main thread: video_init and video_poll run there (main.c's frontend loop).
   F1 help  F3 screenshot  F4 fps limit  F5 thread dump  Cmd+F / Cmd+Enter / F11 fullscreen  Esc / Cmd+Q quit */
#include "rt.h"
#include "pad_remap.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <time.h>
#if defined(__APPLE__)
#include <pthread.h>
#endif

static SDL_Window* win; static volatile int g_ww=1280, g_wh=720, g_shown=1; static int g_fullscreen=0, g_want_fs=1;
static int g_seconds=0, g_maxframes=-1; static char g_shot[1024]; static u64 g_start; static u64 g_press=0, g_press_at=0, g_press_len=300;
static char g_script[1024]; static char g_title[256]; static volatile int g_shot_req;
static u64 keys_held;
extern int g_opt_fps;

void backend_ini(const char* k, const char* v){
  if(!strcmp(k,"fullscreen")) g_want_fs=atoi(v);
  else if(!strcmp(k,"seconds")) g_seconds=atoi(v);
  else if(!strcmp(k,"frames")) g_maxframes=atoi(v);
  else if(!strcmp(k,"shot")) strncpy(g_shot,v,1023);
  else if(!strcmp(k,"press")) g_press=strtoull(v,NULL,16);
  else if(!strcmp(k,"press_at")) g_press_at=strtoull(v,NULL,10);
  else if(!strcmp(k,"press_len")) g_press_len=strtoull(v,NULL,10);
  else if(!strcmp(k,"script")) strncpy(g_script,v,1023);
  else if(!strcmp(k,"renderer")){ extern void snxvk_ini(const char*); snxvk_ini(v); }   /* always vulkan_native here */
}

int video_has_gl(void){ return 0; }

/* ---------------------------------------------------------------- game controllers */
static SDL_GameController* g_pad_gc; static u64 g_pad_scan; static int g_sdl_pads;

static int on_main_thread(void){
#if defined(__APPLE__)
  return pthread_main_np()!=0;
#else
  return 1;
#endif
}

static void pads_init(void){
  if(g_sdl_pads) return;
  /* buttons by position like on Windows, also for Nintendo controllers that SDL would map by label */
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS,"0");
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
  g_sdl_pads= SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)==0? 1 : -1;
  if(g_sdl_pads<0) LOG("controllers: SDL could not start its controller support (%s), keyboard only\n",SDL_GetError()); }

/* the kind of controller connected, for the button icons (svc_fs.c), asked before the window exists: 1 Xbox or any
   other, 2 DualShock 4, 3 DualSense, 4 Switch, 0 none. SDL's controllers belong to the main thread on macOS */
int backend_pad_family(void){
  if(!g_sdl_pads){ if(!on_main_thread()) return 0; pads_init(); }
  if(g_sdl_pads<0) return 0;
  for(int i=0;i<SDL_NumJoysticks();i++) if(SDL_IsGameController(i)){
    int t=SDL_GameControllerTypeForIndex(i);
    return t==SDL_CONTROLLER_TYPE_PS4||t==SDL_CONTROLLER_TYPE_PS3? 2 : t==SDL_CONTROLLER_TYPE_PS5? 3
         : t==SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO||t>=SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT? 4 : 1; }
  return 0; }
static i32 dz(i32 v){ return (v>-7000 && v<7000)? 0 : v; }

static void pad_poll(u64* b, i32* lx, i32* ly, i32* rx, i32* ry){
  if(g_sdl_pads<=0) return;
  SDL_GameControllerUpdate();
  if(g_pad_gc && !SDL_GameControllerGetAttached(g_pad_gc)){ SDL_GameControllerClose(g_pad_gc); g_pad_gc=NULL; LOG("controllers: disconnected\n"); }
  if(!g_pad_gc){ u64 now=plat_ns(); if(now-g_pad_scan<1000000000ull) return; g_pad_scan=now;
    for(int i=0;i<SDL_NumJoysticks() && !g_pad_gc;i++) if(SDL_IsGameController(i)) g_pad_gc=SDL_GameControllerOpen(i);
    if(!g_pad_gc) return;
    LOG("controllers: %s\n",SDL_GameControllerName(g_pad_gc)? SDL_GameControllerName(g_pad_gc) : "connected"); }
  SDL_GameController* g=g_pad_gc;
  /* SDL names buttons by Xbox position: A bottom, B right, X left, Y top (the same table as backend_x11.c) */
  static const struct { int sdl, bit; } m[]={{1,0},{0,1},{3,2},{2,3},{7,4},{8,5},{9,6},{10,7},{6,10},{4,11},{13,12},{11,13},{14,14},{12,15}};
  for(unsigned i=0;i<sizeof m/sizeof m[0];i++) if(SDL_GameControllerGetButton(g,(SDL_GameControllerButton)m[i].sdl)) *b|=1ull<<m[i].bit;
  if(SDL_GameControllerGetButton(g,SDL_CONTROLLER_BUTTON_GUIDE)) *b|=1ull<<30;   /* Home: never the game (video_poll) */
  if(SDL_GameControllerGetAxis(g,SDL_CONTROLLER_AXIS_TRIGGERLEFT)>7700) *b|=1ull<<8;
  if(SDL_GameControllerGetAxis(g,SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>7700) *b|=1ull<<9;
  /* SDL's Y axes point down, the Switch's up */
  i32 x=SDL_GameControllerGetAxis(g,SDL_CONTROLLER_AXIS_LEFTX), y=-(i32)SDL_GameControllerGetAxis(g,SDL_CONTROLLER_AXIS_LEFTY);
  i32 x2=SDL_GameControllerGetAxis(g,SDL_CONTROLLER_AXIS_RIGHTX), y2=-(i32)SDL_GameControllerGetAxis(g,SDL_CONTROLLER_AXIS_RIGHTY);
  if(y>32767) y=32767; if(y2>32767) y2=32767;
  if(!*lx && !*ly){ *lx=dz(x); *ly=dz(y); } if(!*rx && !*ry){ *rx=dz(x2); *ry=dz(y2); }
  { /* rumble (svc_hid.c hid_rumble_get): on a change, else refreshed every 100 ms */
    extern void hid_rumble_get(float*,float*); float lo, hi; hid_rumble_get(&lo,&hi);
    static u16 plo=1, phi=1; static u64 last; u64 now=plat_ns(); u16 ulo=(u16)(lo*65535.0f), uhi=(u16)(hi*65535.0f);
    if(ulo!=plo || uhi!=phi || ((ulo||uhi) && now-last>100000000ull)){ plo=ulo; phi=uhi; last=now; SDL_GameControllerRumble(g,ulo,uhi,ulo||uhi? 250 : 0); } }
}

/* ---------------------------------------------------------------- window */
static void update_size(void){
  int w=0, h=0; SDL_Vulkan_GetDrawableSize(win,&w,&h);   /* pixels, not points: the swapchain's size on a Retina screen */
  g_ww= w>0? w : 1; g_wh= h>0? h : 1; }

static void set_fullscreen(int on){
  if(on==g_fullscreen || !win) return;
  SDL_SetWindowFullscreen(win,on? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  SDL_ShowCursor(on? SDL_DISABLE : SDL_ENABLE);
  g_fullscreen=on; update_size(); }

static void set_title(const char* t){ if(win) SDL_SetWindowTitle(win,t); }
/* fast forward on/off in the title, as backend_win32.c does it (main.c ff_*); from the main thread */
void backend_ff_status(int on, int speed){ if(!win) return; char t[320];
  if(on) snprintf(t,sizeof t,"%s  |  FAST FORWARD x%d (Tab or L3+R3)",g_title,speed); else snprintf(t,sizeof t,"%s",g_title); set_title(t); }

static const char* HELP=
  "Metroid Dread - controls\n\n"
  "Controller: same as the Switch controller (buttons by position).\n\n"
  "Keyboard:\n"
  "  Arrows = D-Pad     W A S D = Left stick    I J K L = Right stick\n"
  "  X / Space = A    Z = B    C = X    V = Y\n"
  "  Q = L   E = R   1 = ZL   3 = ZR   Enter = +   Backspace = -\n\n"
  "F1 = this help   F3 = screenshot   F4 = FPS limit 60 / unlimited   F5 = thread dump\n"
  "Cmd+F, Cmd+Enter or F11 = fullscreen   Esc or Cmd+Q = quit\n"
  "(On a Mac keyboard the F keys may need the fn key.)";

/* the Vulkan library SDL hands vkGetInstanceProcAddr from: the game folder's MoltenVK (the packaged game), the Vulkan
   loader, then Homebrew's, then SDL's own search. MoltenVK on its own is enough: it is a whole Vulkan implementation */
static int vk_library(void){
  char p[1300]; const char* tried[8]; int n=0;
  extern char g_bindir[];   /* the game folder, or the .app's Contents/MacOS (main.c app_dirs) */
  snprintf(p,sizeof p,"%slibMoltenVK.dylib",g_bindir); tried[n++]=p;
  static char p2[1300]; snprintf(p2,sizeof p2,"%slibvulkan.1.dylib",g_bindir); tried[n++]=p2;
  tried[n++]="/opt/homebrew/lib/libvulkan.1.dylib"; tried[n++]="/usr/local/lib/libvulkan.1.dylib";
  tried[n++]="/opt/homebrew/lib/libMoltenVK.dylib"; tried[n++]="/usr/local/lib/libMoltenVK.dylib";
  const char* env=getenv("SWR_VULKAN_LIBRARY");
  if(env && *env){ if(!SDL_Vulkan_LoadLibrary(env)){ LOG("vulkan: %s\n",env); return 1; } WARN("vulkan: SWR_VULKAN_LIBRARY=%s: %s\n",env,SDL_GetError()); }
  for(int i=0;i<n;i++){ if(!plat_file_size(tried[i])) continue; if(!SDL_Vulkan_LoadLibrary(tried[i])){ LOG("vulkan: %s\n",tried[i]); return 1; } }
  if(!SDL_Vulkan_LoadLibrary(NULL)){ LOG("vulkan: the system's Vulkan library\n"); return 1; }
  WARN("vulkan: no Vulkan library (MoltenVK) found: %s\n",SDL_GetError()); return 0; }

void video_init(const char* title){
  snprintf(g_title,sizeof g_title,"%s",title && *title? title : "Metroid Dread");
  SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES,"0");   /* Cmd+F: fullscreen at once, not an animated Space */
  SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK,"1");
  if(SDL_InitSubSystem(SDL_INIT_VIDEO)) fatal("Could not open a window (SDL: %s).",SDL_GetError());
  pads_init();
  if(!vk_library()) fatal("Vulkan (MoltenVK) was not found.\n\nInstall it with Homebrew (brew install molten-vk) or put libMoltenVK.dylib next to the game.");
  win=SDL_CreateWindow(g_title,SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,1280,720,SDL_WINDOW_VULKAN|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
  if(!win) fatal("Could not open a Vulkan window (SDL: %s).",SDL_GetError());
  if(g_want_fs) set_fullscreen(1);
  SDL_RaiseWindow(win);
  update_size();
  { SDL_Event e; while(SDL_PollEvent(&e)){} }   /* let Cocoa put the window up before the swapchain asks its size */
  update_size();
  g_start=plat_ns();
  extern int snxvk_start(void);
  if(!snxvk_start()) fatal("The Vulkan renderer could not start on this Mac (details in the .log).\n\nmacOS 12 or later on a Mac with Metal is needed, and MoltenVK 1.2 or later.");
}

/* the window side of renderer=vulkan_native (snxvk_glue.c, ScarletNX gpu/vk/device.c) */
void* backend_vk_loader(void){ return SDL_Vulkan_GetVkGetInstanceProcAddr(); }
const char* const* backend_vk_extensions(u32* n){
  static const char* e[16]; static unsigned cnt; static int done;
  if(!done){ done=1; unsigned c=0; if(!SDL_Vulkan_GetInstanceExtensions(win,&c,NULL) || c>16){ c=0; }
    else if(!SDL_Vulkan_GetInstanceExtensions(win,&c,e)) c=0;
    cnt=c; for(unsigned i=0;i<c;i++) LOG("vulkan: window needs %s\n",e[i]); }
  *n=cnt; return e; }
/* the surface is made on the thread that started the renderer: video_init's, the main thread, as Cocoa wants */
int backend_vk_surface(void* instance, u64* surface){
  VkSurfaceKHR s=0;
  if(!SDL_Vulkan_CreateSurface(win,(VkInstance)instance,&s)){ LOG("vulkan_native: no Vulkan surface on the window (%s)\n",SDL_GetError()); return 0; }
  *surface=(u64)s; return 1; }
/* 0, the renderer's own starting value: any other number makes its first frame make a new surface, from its own
   thread, and SDL then puts a new Metal view on the window off the main thread, which never shows (a black window).
   The window's surface never goes away here; the swapchain follows the window size by itself */
u32 backend_window_generation(void){ return 0; }
void backend_window_safe(int* x, int* y, int* w, int* h){ *x=0; *y=0; *w=g_ww; *h=g_wh; }
int backend_window_shown(void){ return g_shown; }
int backend_hdr_peak_nits(void){ return 0; }   /* hdr=1 isn't offered here */

/* the OpenGL renderer is compiled in but has no context here (gpu_gl.c asks only when native Vulkan didn't start) */
int backend_gl_create(void){ return 0; }
void* backend_gl_proc(const char* n){ (void)n; return NULL; }
void backend_gl_swap(void){}
void backend_gl_vsync(int on){ (void)on; }
int backend_gl_worker_current(void){ return 0; }
int backend_glq_ok(void){ return 0; }

/* fullscreen from another thread (settings): done by the event loop on the main thread */
static volatile int g_fs_req=-1;
void backend_set_fullscreen(int on){ g_fs_req= on!=0; }
int backend_is_fullscreen(void){ return g_fullscreen; }
void backend_release_keys(void){ keys_held=0; }
void backend_window_size(int* w, int* h){ *w=g_ww; *h=g_wh; }
void video_present_rgba(const u32* px, int w, int h, int stride){ (void)px; (void)w; (void)h; (void)stride; }
void backend_request_quit(void){ g_quit=1; }
void mouse_map_reset(void){}   /* mouse_buttons: the Windows mouse mapping, not here */

/* keys -> HidNpadButton bits, the same layout as on Windows and Linux */
static u64 keymap(SDL_Keycode k){
  switch(k){ case SDLK_x: case SDLK_SPACE: return 1ull<<0; /* A */ case SDLK_z: return 1ull<<1; /* B */ case SDLK_c: return 1ull<<2; /* X */ case SDLK_v: return 1ull<<3; /* Y */
    case SDLK_q: return 1ull<<6; /* L */ case SDLK_e: return 1ull<<7; /* R */ case SDLK_1: return 1ull<<8; /* ZL */ case SDLK_3: return 1ull<<9; /* ZR */
    case SDLK_RETURN: case SDLK_KP_ENTER: return 1ull<<10; /* + */ case SDLK_BACKSPACE: return 1ull<<11; /* - */
    case SDLK_LEFT: return 1ull<<12; case SDLK_UP: return 1ull<<13; case SDLK_RIGHT: return 1ull<<14; case SDLK_DOWN: return 1ull<<15;
    /* stick keys, turned into axes later */
    case SDLK_a: return 1ull<<40; case SDLK_w: return 1ull<<41; case SDLK_d: return 1ull<<42; case SDLK_s: return 1ull<<43;
    case SDLK_j: return 1ull<<44; case SDLK_i: return 1ull<<45; case SDLK_l: return 1ull<<46; case SDLK_k: return 1ull<<47; }
  return 0; }

static void key_down(const SDL_KeyboardEvent* e){
  SDL_Keycode k=e->keysym.sym; int once=!e->repeat; u16 mod=e->keysym.mod;
  if(mod&KMOD_GUI){   /* Cmd shortcuts are the window's, never the game's */
    if(k==SDLK_q){ LOG("window: Cmd+Q\n"); g_quit=1; }
    else if((k==SDLK_f || k==SDLK_RETURN) && once) set_fullscreen(!g_fullscreen);
    return; }
  if(k==SDLK_RETURN && (mod&KMOD_ALT)){ if(once) set_fullscreen(!g_fullscreen); return; }
  switch(k){
    case SDLK_ESCAPE: g_quit=1; return;
    case SDLK_F1: if(once) plat_message(HELP,0,0); return;
    case SDLK_F11: if(once) set_fullscreen(!g_fullscreen); return;
    case SDLK_F3: if(once) g_shot_req=1; return;
    case SDLK_F5: if(once){ extern void kernel_dump_threads(void); extern __thread FILE* g_log_redirect;
        plat_mkdir("captures"); FILE* f=fopen("captures/threads.txt","w");
        if(f){ g_log_redirect=f; kernel_dump_threads(); g_log_redirect=NULL; fclose(f); }
        kernel_dump_threads(); } return;
    case SDLK_F4: if(once){ g_opt_fps= g_opt_fps==0? 1 : 0; char t[300]; snprintf(t,sizeof t,"%s  |  FPS limit: %s",g_title,g_opt_fps==0? "unlimited" : "60"); set_title(t);
        extern void ini_set(const char*, const char*); char v[8]; snprintf(v,sizeof v,"%d",g_opt_fps); ini_set("fps_limit",v); } return;
  }
  keys_held|=keymap(k); }

static i32 axis(u64 b, int neg, int pos){ return ((b>>pos)&1? 32767:0) - ((b>>neg)&1? 32767:0); }

/* Test-only frame-indexed replay, as backend_win32.c and backend_x11.c have it (the same route files run on all
   three): SWR_TEST_SCRIPT CSV guest_frame,hex_buttons,lx,ly,rx,ry, held until the next row; SWR_TEST_SETENV=
   FRAME:NAME=VALUE; captures at SWR_TEST_CAPTURE_FRAMES (SWR_TEST_CAPTURE_PREFIX-fN); SWR_TEST_EXIT_FRAME */
typedef struct { u32 frame; u64 buttons; i32 lx, ly, rx, ry; } TestInput;
static TestInput* test_inputs; static u32 test_n, test_i; static int test_loaded, test_replay;
static void test_input_apply(u64* b, i32* lx, i32* ly, i32* rx, i32* ry){
  if(!test_loaded){ test_loaded=1; const char* path=getenv("SWR_TEST_SCRIPT");
    if(path && *path){ FILE* f=fopen(path,"r"); char line[256]; u32 cap=8192;
      if(!f){ WARN("test input: cannot open replay %s\n",path); _exit(2); }
      test_inputs=calloc(cap,sizeof *test_inputs);
      while(fgets(line,sizeof line,f)){ if(line[0]=='#'||line[0]=='\n'||line[0]=='\r') continue; TestInput v={0}; unsigned long long m;
        if(sscanf(line,"%u,%llx,%d,%d,%d,%d",&v.frame,&m,&v.lx,&v.ly,&v.rx,&v.ry)!=6 || test_n==cap){ fclose(f); WARN("test input: invalid replay row\n"); _exit(2); }
        v.buttons=(u64)m; test_inputs[test_n++]=v; }
      fclose(f); test_replay= test_n>0; LOG("test input: frame replay active, %u states\n",test_n); } }
  u32 frame=(u32)g_frames;
  { static int done; const char* se; if(!done && (se=getenv("SWR_TEST_SETENV")) && *se){ u32 at=(u32)strtoul(se,NULL,10); const char* kv=strchr(se,':');
      if(!kv) done=1; else if(frame>=at){ char k[64]; const char* eq=strchr(kv+1,'=');
        if(eq && eq-kv-1<64){ memcpy(k,kv+1,eq-kv-1); k[eq-kv-1]=0; setenv(k,eq+1,1); LOG("test: frame %u set %s=%s\n",frame,k,eq+1); } done=1; } } }
  if(test_replay){ while(test_i+1<test_n && test_inputs[test_i+1].frame<=frame) test_i++; TestInput* v=&test_inputs[test_i];
    *b=v->buttons; *lx=v->lx; *ly=v->ly; *rx=v->rx; *ry=v->ry; }
}
static u64 screenshot(const char* path){ extern u64 snxvk_screenshot(const char*); return snxvk_screenshot(path); }
static void test_frame_hooks(void){
  if(!getenv("SWR_TEST_SCRIPT")) return;
  static int configured; static const char* times; static const char* prefix; static u64 next;
  if(!configured){ configured=1; times=getenv("SWR_TEST_CAPTURE_FRAMES"); prefix=getenv("SWR_TEST_CAPTURE_PREFIX"); if(times&&prefix) next=strtoull(times,(char**)&times,10); else times=NULL; }
  if(times && next && (u64)g_frames>=next){ char path[1024]; snprintf(path,sizeof path,"%s-f%llu",prefix,(unsigned long long)next);
    LOG("test capture: requested_frame=%llu actual_frame=%d ns=%llu\n",(unsigned long long)next,g_frames,(unsigned long long)plat_ns()); screenshot(path);
    if(*times==','){ times++; next=strtoull(times,(char**)&times,10); } else { times=NULL; next=0; } }
  static u64 heartbeat; u64 now=plat_ns(); if(!heartbeat || now-heartbeat>=1000000000ull){ heartbeat=now; LOG("test heartbeat: frame=%d fps_limit=%d ns=%llu\n",g_frames,g_opt_fps,(unsigned long long)now); }
  const char* stop=getenv("SWR_TEST_EXIT_FRAME"); if(stop && (u64)g_frames>=strtoull(stop,NULL,10)){ LOG("test complete: frame=%d\n",g_frames); extern void save_hints(void); save_hints(); log_flush(); fflush(NULL); _exit(0); }
}

void video_poll(void){
  { int r=g_fs_req; if(r>=0){ g_fs_req=-1; set_fullscreen(r); } }
  SDL_Event e;
  while(SDL_PollEvent(&e)){
    switch(e.type){
      case SDL_KEYDOWN: key_down(&e.key); break;
      case SDL_KEYUP: keys_held&=~keymap(e.key.keysym.sym); break;
      case SDL_QUIT: LOG("window: closed by the user\n"); g_quit=1; break;
      case SDL_WINDOWEVENT:
        switch(e.window.event){
          case SDL_WINDOWEVENT_SIZE_CHANGED: case SDL_WINDOWEVENT_RESIZED: update_size(); break;
          case SDL_WINDOWEVENT_MINIMIZED: case SDL_WINDOWEVENT_HIDDEN: g_shown=0; break;
          case SDL_WINDOWEVENT_RESTORED: case SDL_WINDOWEVENT_SHOWN: case SDL_WINDOWEVENT_EXPOSED: g_shown=1; update_size(); break;
          case SDL_WINDOWEVENT_FOCUS_LOST: keys_held=0; break; }
        break; } }
  u64 k=keys_held, b=0;
  i32 lx=axis(k,40,42), ly=axis(k,43,41), rx=axis(k,44,46), ry=axis(k,47,45);
  pad_poll(&b,&lx,&ly,&rx,&ry);
  b&=~(1ull<<30);   /* Home opens the in-game menu elsewhere; it needs the OpenGL renderer, so here it does nothing */
  b=swr_pad_remap(b)|(k&0xFFFFull);
  /* stick-as-button bits 16..23 */
  if(lx<-16000) b|=1ull<<16; if(ly>16000) b|=1ull<<17; if(lx>16000) b|=1ull<<18; if(ly<-16000) b|=1ull<<19;
  if(rx<-16000) b|=1ull<<20; if(ry>16000) b|=1ull<<21; if(rx>16000) b|=1ull<<22; if(ry<-16000) b|=1ull<<23;
  /* test input: --press=hex --press_at=ms, or --script=ms:hex,ms:hex,... */
  u64 el=(plat_ns()-g_start)/1000000ull;
  if(g_press && el>=g_press_at && el<g_press_at+g_press_len) b|=g_press;
  if(g_script[0]){ const char* p=g_script; while(*p){ u64 t=strtoull(p,(char**)&p,10); if(*p!=':') break; p++; u64 m=strtoull(p,(char**)&p,16); if(el>=t && el<t+g_press_len) b|=m; if(*p==',') p++; } }
  test_input_apply(&b,&lx,&ly,&rx,&ry);
  { extern u64 skip_hold_filter(u64); b=skip_hold_filter(b); }   /* main.c: hold - to skip a cutscene */
  g_pad.buttons=b; g_pad.lx=lx; g_pad.ly=ly; g_pad.rx=rx; g_pad.ry=ry;
}

void frontend_frame(void){
  test_frame_hooks();
  if(g_shot_req){ g_shot_req=0; char p[300]; time_t now=time(NULL); struct tm t; localtime_r(&now,&t); plat_mkdir("captures");
    snprintf(p,sizeof p,"captures/screenshot_%04d%02d%02d_%02d%02d%02d",t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec); screenshot(p); }
  u64 el=plat_ns()-g_start;
  if((g_maxframes>=0 && g_frames>=g_maxframes) || (g_seconds && el>=(u64)g_seconds*1000000000ull)){
    u64 h=screenshot(g_shot[0]? g_shot : NULL);
    fprintf(stderr,"\nframes=%d hash=%016llx\n",g_frames,(unsigned long long)h);
    extern void save_hints(void); save_hints(); log_flush(); fflush(NULL); _exit(0); }
}
