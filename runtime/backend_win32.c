/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* win32 backend: window, WGL, keyboard, XInput.
   F1 menu  F3 screenshot  F4 fps limit  F11 fullscreen  Esc quit
   The same window first shows the Play screen / setup (dread_ui/fe.cpp, Direct3D 11, no pixel format), then the game. */
#include "rt.h"
#include <math.h>
#include <ctype.h>
#include "pad_remap.h"
#include <windows.h>
#include <mmsystem.h>  /* timeBeginPeriod (windows.h leaves it out with WIN32_LEAN_AND_MEAN) */

static HWND g_hwnd; static HDC g_hdc; static HGLRC g_ctx, g_wctx;
static HWND g_glwnd; static int g_want_vk;   /* renderer=vulkan: GL renders on the hidden g_glwnd, Vulkan presents on g_hwnd */
static volatile int g_ww=1280, g_wh=720; static u64 keys_held,keys_tapped;static int test_tap_sample; static int g_fullscreen=0, g_want_fs=1;
static WINDOWPLACEMENT g_wp={sizeof(WINDOWPLACEMENT)};
static int g_seconds=0; static char g_shot[1024]; static u64 g_start; static volatile int g_shot_req;
static char g_script[8192];
static HANDLE g_ready;
static volatile int g_fe_mode;   /* the front end owns the window (its messages go to fe_wndproc) */
static HANDLE g_fe_done, g_video_go; static int g_fe_result, g_win_started;
static HWND g_fe_hwnd;
/* mouse free aim: raw movement since the last poll (WM_INPUT, any thread adds, the input thread takes), and the buttons
   held (1 left, 2 right, 4 middle, 8 back, 16 forward) */
static volatile LONG g_mdx, g_mdy, g_mbtn;   /* the front end's window, retired at Play (its messages go to DefWindowProc) */
#define PAD_HOME (1ull<<30)   /* Home / Guide / PS: physical only, opens the in-game menu, never reaches the game */
extern int g_opt_fps;

static int g_ls_mode=0; static char g_ls_path[1024];

void backend_ini(const char* k, const char* v){
  if(!strcmp(k,"fullscreen")) g_want_fs=atoi(v);
  else if(!strcmp(k,"lossless_scaling")) g_ls_mode=atoi(v);
  else if(!strcmp(k,"lossless_path")) strncpy(g_ls_path,v,1023);
  else if(!strcmp(k,"seconds")) g_seconds=atoi(v);
  else if(!strcmp(k,"shot")) strncpy(g_shot,v,1023);
  else if(!strcmp(k,"script")) {strncpy(g_script,v,sizeof g_script-1);g_script[sizeof g_script-1]=0;}
  else if(!strcmp(k,"renderer")){ g_want_vk= !strcmp(v,"vulkan"); extern void snxvk_ini(const char*); snxvk_ini(v); }   /* vulkan_native: snxvk_glue.c */
}

/* a status after the window title (shader pre-build progress); NULL restores the title */
static void backend_status_win(const char* s){ if(!g_hwnd) return; char* c=NULL; if(s){ size_t n=strlen(s)+1; c=malloc(n); if(!c) return; memcpy(c,s,n); }
  if(!PostMessageA(g_hwnd,WM_APP+10,0,(LPARAM)c)) free(c); }
void backend_ff_status(int on, int speed){ if(g_hwnd) PostMessageA(g_hwnd,WM_APP+9,(WPARAM)on,(LPARAM)speed); }
int video_has_gl(void){ return 1; }

void win_fatal_box(const char* msg){ MessageBoxA(g_hwnd,msg,"Metroid Dread",MB_OK|MB_ICONERROR); }

/* ---------------------------------------------------------------- XInput, loaded dynamically */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XGP;
typedef struct { DWORD dwPacketNumber; XGP Gamepad; } XST;
typedef DWORD (WINAPI *XGetState)(DWORD, XST*);
static XGetState xget, xget_ex;   /* xget_ex: XInputGetStateEx (ordinal 100), the same plus the Guide button 0x0400 */
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XVIB;
typedef DWORD (WINAPI *XSetState)(DWORD, XVIB*);
static XSetState xset; static int g_xpad=-1;   /* the XInput pad input last came from: rumble goes there */

static void xinput_load(void){
  static int done; if(done) return; done=1;   /* also called early by backend_pad_family */
  const char* dl[]={"xinput1_4.dll","xinput1_3.dll","xinput9_1_0.dll"};
  for(int i=0;i<3 && !xget;i++){ HMODULE h=LoadLibraryA(dl[i]); if(h){ xget=(XGetState)GetProcAddress(h,"XInputGetState"); xset=(XSetState)GetProcAddress(h,"XInputSetState"); if(i<2) xget_ex=(XGetState)GetProcAddress(h,(LPCSTR)(size_t)100); } }
  if(xset){ extern void xinput_rumble_off(void); atexit(xinput_rumble_off); }
}
void xinput_rumble_off(void){ if(xset){ XVIB v={0,0}; for(DWORD i=0;i<4;i++) xset(i,&v); } }   /* a motor left on would keep going after exit */

/* ---------------------------------------------------------------- SDL2 game controllers, loaded dynamically
   SDL2.dll next to the game (or on PATH) adds every controller SDL knows over USB and Bluetooth:
   Switch Pro / Joy-Con pair, DualShock 4, DualSense, 8BitDo, Xbox. Buttons map by position like
   XInput (Switch A = right face button), using SDL's positional layout for Nintendo pads.
   [Input] controller_api: auto (SDL when available, else XInput), sdl, xinput. */
typedef void* SDLGC;
static int (*sdl_init)(unsigned); static int (*sdl_sethint)(const char*,const char*);
static int (*sdl_numjoy)(void); static int (*sdl_isgc)(int); static SDLGC (*sdl_gcopen)(int);
static int (*sdl_gcattached)(SDLGC); static void (*sdl_gcclose)(SDLGC); static const char* (*sdl_gcname)(SDLGC);
static unsigned char (*sdl_gcbutton)(SDLGC,int); static short (*sdl_gcaxis)(SDLGC,int); static void (*sdl_gcupdate)(void);
static void (*sdl_pump)(void);
static int (*sdl_gcrumble)(SDLGC,unsigned short,unsigned short,unsigned);
static int sdl_ok; static SDLGC sdl_pad[8]; static int sdl_npad; static u64 sdl_rescan;
static int (*sdl_gctype)(int);   /* SDL_GameControllerTypeForIndex, SDL 2.0.12+ (optional) */
static void sdl_load(void){
  static int done; if(done) return; done=1;   /* also called early by backend_pad_family */
  extern int g_opt_ctrl_api; if(g_opt_ctrl_api==2) return;
  extern char g_exedir[]; char p[1200]; snprintf(p,sizeof p,"%sSDL2.dll",g_exedir);
  HMODULE h=LoadLibraryA(p); if(!h) h=LoadLibraryA("SDL2.dll");
  if(!h){ if(g_opt_ctrl_api==1) LOG("input: controller_api=sdl but SDL2.dll was not found next to the game\n"); return; }
#define SDLF(v,n) *(FARPROC*)&v=GetProcAddress(h,n); if(!v){ LOG("input: SDL2.dll lacks %s, using XInput\n",n); return; }
  SDLF(sdl_init,"SDL_Init") SDLF(sdl_sethint,"SDL_SetHint") SDLF(sdl_numjoy,"SDL_NumJoysticks") SDLF(sdl_isgc,"SDL_IsGameController")
  SDLF(sdl_gcopen,"SDL_GameControllerOpen") SDLF(sdl_gcattached,"SDL_GameControllerGetAttached") SDLF(sdl_gcclose,"SDL_GameControllerClose")
  SDLF(sdl_gcname,"SDL_GameControllerName") SDLF(sdl_gcbutton,"SDL_GameControllerGetButton") SDLF(sdl_gcaxis,"SDL_GameControllerGetAxis")
  SDLF(sdl_gcupdate,"SDL_GameControllerUpdate") SDLF(sdl_pump,"SDL_PumpEvents")
#undef SDLF
  *(FARPROC*)&sdl_gcrumble=GetProcAddress(h,"SDL_GameControllerRumble");
  *(FARPROC*)&sdl_gctype=GetProcAddress(h,"SDL_GameControllerTypeForIndex");   /* SDL 2.0.9+; optional: no rumble without it */
  sdl_sethint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS","0");   /* positions, not printed labels */
  sdl_sethint("SDL_JOYSTICK_HIDAPI_PS4","1"); sdl_sethint("SDL_JOYSTICK_HIDAPI_PS5","1");
  /* PS4/PS5 pads rumble over Bluetooth only with these (SDL2 keeps their simple report mode otherwise): a DualSense
     felt nothing in game (MEASURED 2026-10-29, GitHub #20) */
  sdl_sethint("SDL_JOYSTICK_HIDAPI_PS4_RUMBLE","1"); sdl_sethint("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE","1");
  sdl_sethint("SDL_JOYSTICK_HIDAPI_SWITCH","1"); sdl_sethint("SDL_JOYSTICK_HIDAPI_JOY_CONS","1"); sdl_sethint("SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS","1");
  sdl_sethint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS","1");
  /* We poll from the game's input thread, not the thread that started SDL, and pump events only once a second.
     RawInput (SDL's default for XInput pads) only updates through window messages, so it would see no presses:
     use the XInput backend for those pads and let SDL service device messages on its own thread. */
  sdl_sethint("SDL_JOYSTICK_RAWINPUT","0"); sdl_sethint("SDL_JOYSTICK_THREAD","1");
  if(sdl_init(0x00002000u|0x00000200u)!=0){ LOG("input: SDL2 could not start, using XInput\n"); return; }   /* GAMECONTROLLER|JOYSTICK */
  sdl_ok=1; LOG("input: SDL2 controllers active (XInput pads also read)\n"); }
/* the kind of controller player 1 has, for the button icons (svc_fs.c), asked before the window and the input thread
   exist: 1 Xbox (or any other), 2 DualShock 4, 3 DualSense, 4 Switch controllers, 0 none */
static int sdl_family(int i){
  int t= sdl_gctype? sdl_gctype(i) : 0;   /* SDL_GameControllerType: 1/2 Xbox, 3 PS3, 4 PS4, 5 Switch Pro, 7 PS5, 11-13 Joy-Con */
  return t==4||t==3? 2 : t==7? 3 : t==5||t==11||t==12||t==13? 4 : 1; }
int backend_pad_family(void){
  extern int g_opt_ctrl_index; sdl_load(); xinput_load();
  if(sdl_ok){ sdl_pump(); int n=sdl_numjoy(), k=0;
    for(int j=0;j<n;j++) if(sdl_isgc(j)){ k++; if(g_opt_ctrl_index<=0 || k==g_opt_ctrl_index) return sdl_family(j); } }
  if(xget){ for(DWORD i=0;i<4;i++){ XST s; if(xget(i,&s)==0) return 1; } }
  return 0; }
static void sdl_scan(void){
  for(int i=0;i<sdl_npad;) if(!sdl_gcattached(sdl_pad[i])){ LOG("input: controller disconnected: %s\n",sdl_gcname(sdl_pad[i])? sdl_gcname(sdl_pad[i]) : "?"); sdl_gcclose(sdl_pad[i]); sdl_pad[i]=sdl_pad[--sdl_npad]; } else i++;
  int n=sdl_numjoy();
  for(int j=0;j<n && sdl_npad<8;j++){ if(!sdl_isgc(j)) continue; SDLGC g=sdl_gcopen(j); if(!g) continue;
    int have=0; for(int i=0;i<sdl_npad;i++) if(sdl_pad[i]==g) have=1;
    if(have) sdl_gcclose(g);   /* Open on an open pad only adds a reference */
    else { sdl_pad[sdl_npad++]=g; LOG("input: controller %d connected: %s\n",sdl_npad,sdl_gcname(g)? sdl_gcname(g) : "?"); } } }
/* stick_deadzone (main.c), round: a stick inside it reads 0, outside it is rescaled from the dead zone's edge to full,
   in its own direction. Per axis it was a cross: aiming near an axis snapped onto it, and diagonals came in late
   (GitHub #20) */
static void stick_dz(i32* x, i32* y){
  extern int g_opt_deadzone; float dzv=(float)g_opt_deadzone, fx=(float)*x, fy=(float)*y, m=sqrtf(fx*fx+fy*fy);
  if(m<=dzv || m<1.0f){ *x=*y=0; return; }
  float out=(m-dzv)/(32767.0f-dzv); if(out>1.0f) out=1.0f;
  float k=out*32767.0f/m; *x=(i32)lrintf(fx*k); *y=(i32)lrintf(fy*k); }
/* returns 1 when SDL handled input this frame */
static int sdl_poll(u64* b, i32* lx, i32* ly, i32* rx, i32* ry){
  if(!sdl_ok) return 0;
  u64 now=plat_ns(); if(now-sdl_rescan>1000000000ull){ sdl_rescan=now; sdl_pump(); sdl_scan(); }
  sdl_gcupdate();
  extern int g_opt_ctrl_index, g_opt_deadzone;
  int first= g_opt_ctrl_index>0? g_opt_ctrl_index-1 : 0, last= g_opt_ctrl_index>0? g_opt_ctrl_index : sdl_npad;
  for(int i=first;i<last && i<sdl_npad;i++){ SDLGC g=sdl_pad[i];
    /* SDL: 0 A(south) 1 B(east) 2 X(west) 3 Y(north) 4 back 5 guide 6 start 7 lstick 8 rstick 9 lb 10 rb 11 up 12 down 13 left 14 right */
    if(sdl_gcbutton(g,1)) *b|=1<<0; if(sdl_gcbutton(g,0)) *b|=1<<1; if(sdl_gcbutton(g,3)) *b|=1<<2; if(sdl_gcbutton(g,2)) *b|=1<<3;
    if(sdl_gcbutton(g,7)) *b|=1<<4; if(sdl_gcbutton(g,8)) *b|=1<<5; if(sdl_gcbutton(g,9)) *b|=1<<6; if(sdl_gcbutton(g,10)) *b|=1<<7;
    if(sdl_gcaxis(g,4)>8000) *b|=1<<8; if(sdl_gcaxis(g,5)>8000) *b|=1<<9;
    if(sdl_gcbutton(g,6)) *b|=1<<10; if(sdl_gcbutton(g,4)) *b|=1<<11; if(sdl_gcbutton(g,5)) *b|=PAD_HOME;
    if(sdl_gcbutton(g,13)) *b|=1<<12; if(sdl_gcbutton(g,11)) *b|=1<<13; if(sdl_gcbutton(g,14)) *b|=1<<14; if(sdl_gcbutton(g,12)) *b|=1<<15;
    i32 ax=sdl_gcaxis(g,0), ay=-(i32)sdl_gcaxis(g,1), bx=sdl_gcaxis(g,2), by=-(i32)sdl_gcaxis(g,3);
    if(ay<-32767) ay=-32767; if(ay>32767) ay=32767; if(by<-32767) by=-32767; if(by>32767) by=32767;   /* SDL y is down-positive */
    stick_dz(&ax,&ay); stick_dz(&bx,&by);
    if(!*lx && !*ly){ *lx=ax; *ly=ay; } if(!*rx && !*ry){ *rx=bx; *ry=by; } }
  return 1; }

static void set_fullscreen(int on){
  DWORD st=GetWindowLong(g_hwnd,GWL_STYLE);
  if(on && !g_fullscreen){
    MONITORINFO mi={sizeof mi};
    if(GetWindowPlacement(g_hwnd,&g_wp) && GetMonitorInfo(MonitorFromWindow(g_hwnd,MONITOR_DEFAULTTOPRIMARY),&mi)){
      SetWindowLong(g_hwnd,GWL_STYLE,st&~WS_OVERLAPPEDWINDOW);
      SetWindowPos(g_hwnd,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_NOOWNERZORDER|SWP_FRAMECHANGED); }
    ShowCursor(FALSE); g_fullscreen=1;
  } else if(!on && g_fullscreen){
    SetWindowLong(g_hwnd,GWL_STYLE,st|WS_OVERLAPPEDWINDOW); SetWindowPlacement(g_hwnd,&g_wp);
    SetWindowPos(g_hwnd,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_FRAMECHANGED);
    ShowCursor(TRUE); g_fullscreen=0; }
}

/* fullscreen on/off from any thread (in-game menu, settings): done on the window thread */
/* the front end (same thread as the window) switches fullscreen with the game's own code, cursor kept visible */
void backend_fe_fullscreen(int on){ if((on!=0)==(g_fullscreen!=0)) return; set_fullscreen(on); if(on) ShowCursor(TRUE); }
void backend_set_fullscreen(int on){ if(g_hwnd) PostMessageA(g_hwnd,WM_APP+11,(WPARAM)(on!=0),0); }
int backend_is_fullscreen(void){ return g_fullscreen; }
void backend_release_keys(void){ keys_held=0; keys_tapped=0; }
void backend_request_quit(void){ if(g_hwnd) PostMessageA(g_hwnd,WM_CLOSE,0,0); }   /* the in-game menu's Quit */
void* backend_hwnd(void){ return g_hwnd; }

/* optional: launch the Lossless Scaling app next to the game (lossless_scaling=1).
   This path doesn't touch its dll, the app just captures our window. */
static int file_exists_(const char* p){ DWORD a=GetFileAttributesA(p); return a!=INVALID_FILE_ATTRIBUTES && !(a&FILE_ATTRIBUTE_DIRECTORY); }

static int ls_find(char* out){
  if(g_ls_path[0] && file_exists_(g_ls_path)){ strcpy(out,g_ls_path); return 1; }
  static char libs[10][600]; int nl=0;
  typedef LONG (WINAPI*RGV)(HKEY,LPCSTR,LPCSTR,DWORD,LPDWORD,PVOID,LPDWORD);
  HMODULE a=LoadLibraryA("advapi32.dll"); RGV rgv= a? (RGV)GetProcAddress(a,"RegGetValueA") : NULL;
  char sp[600]; DWORD sz=sizeof sp;
  if(rgv && rgv(HKEY_CURRENT_USER,"Software\\Valve\\Steam","SteamPath",0x0000ffff,NULL,sp,&sz)==0 && sp[0]){ strncpy(libs[nl],sp,599); libs[nl][599]=0; nl++; }
  strcpy(libs[nl++],"C:\\Program Files (x86)\\Steam"); strcpy(libs[nl++],"C:\\Program Files\\Steam");
  { char vdf[700]; snprintf(vdf,sizeof vdf,"%s\\steamapps\\libraryfolders.vdf",libs[0]); FILE* f=fopen(vdf,"r");
    if(f){ char ln[800]; while(fgets(ln,sizeof ln,f) && nl<10){ char* p=strstr(ln,"\"path\""); if(!p) continue; p=strchr(p+6,'"'); if(!p) continue; p++; char* e=strchr(p,'"'); if(!e) continue; *e=0;
        int k=0; for(char* q=p;*q && k<599;q++){ if(q[0]=='\\' && q[1]=='\\') q++; libs[nl][k++]=*q; } libs[nl][k]=0; nl++; }
      fclose(f); } }
  for(int i=0;i<nl;i++){ char c[800]; snprintf(c,sizeof c,"%s\\steamapps\\common\\Lossless Scaling\\LosslessScaling.exe",libs[i]); for(char* q=c;*q;q++) if(*q=='/') *q='\\';
    if(file_exists_(c)){ strcpy(out,c); return 1; } }
  return 0; }

static void ls_launch(void){
  if(!g_ls_mode) return;
  if(FindWindowA(NULL,"Lossless Scaling")){ LOG("lossless scaling: already running\n"); return; }
  char p[1024]; if(!ls_find(p)){ LOG("lossless scaling: not found (install it from Steam, or set lossless_path=C:\\...\\LosslessScaling.exe in the .ini)\n"); return; }
  char dir[1024]; strcpy(dir,p); char* sl=strrchr(dir,'\\'); if(sl) *sl=0;
  STARTUPINFOA si; memset(&si,0,sizeof si); si.cb=sizeof si; si.dwFlags=STARTF_USESHOWWINDOW; si.wShowWindow=SW_SHOWMINNOACTIVE; PROCESS_INFORMATION pi;
  if(CreateProcessA(p,NULL,NULL,NULL,FALSE,0,NULL,dir,&si,&pi)){ CloseHandle(pi.hThread); CloseHandle(pi.hProcess); LOG("lossless scaling: started (%s)\n",p); }
  else LOG("lossless scaling: could not open %s (error %lu)\n",p,(unsigned long)GetLastError());
  SetForegroundWindow(g_hwnd); }

/* window title update for lsfg on/off, posted from the GPU thread */
static char g_title[256];

void backend_lsfg_state(int on, int mult){ if(g_hwnd) PostMessageA(g_hwnd,WM_APP+7,(WPARAM)on,(LPARAM)mult); }
/* F7 recording state in the title bar */
volatile int g_rec_toggle;

void backend_rec_state(int on){ if(g_hwnd) PostMessageA(g_hwnd,WM_APP+8,(WPARAM)on,0); }
static const char* HELP=
  "Metroid Dread - controls\n\n"
  "XInput (Xbox) controller: same as the Switch controller (A/B swapped by position).\n\n"
  "Keyboard:\n"
  "  Arrows = D-Pad     W A S D = Left stick    I J K L = Right stick\n"
  "  X / Space = A    Z = B    C = X    V = Y\n"
  "  Q = L   E = R   1 = ZL   3 = ZR   Enter = +   Backspace = -\n"
  "  Shift = L3 (left stick click)   F = R3 (right stick click)\n\n"
  "F1 = menu   F3 = screenshot   F4 = FPS limit 60 / your limit   F5 = diagnostic dump (trace)\n"
  "F7 = record .mp4 video (press F7 again to stop; saved in the recordings folder)\n"
  "F8 = instant full-resolution screenshot (PNG in the captures folder)\n"
  "F11 / Alt+Enter = fullscreen   Esc = quit";

/* keyboard_map (main.c): 24 entries, buttons 0..15 then the stick keys 40..47 (turned into axes later), each up to
   two Windows key codes joined by '+', 0 = none. Esc, Tab and the F keys are handled before this table. */
extern char g_opt_kb_map[256];
static const char KB_DEFAULT[]="88+32,90,67,86,16,70,81,69,49,51,13,8,37,38,39,40,65,87,68,83,74,73,76,75";   /* L3 Shift, R3 F: R3 had no key (GitHub #32, Phantom Cloak) */
static u64 keymap(WPARAM k){
  static u64 t[256]; static char built[256]; static int ready;
  if(!ready || strcmp(built,g_opt_kb_map)){
    u64 n[256]; memset(n,0,sizeof n); int ok=1;
    for(int pass=0;pass<2;pass++){
      const char* p= pass? KB_DEFAULT : g_opt_kb_map; if(!pass && !*p) continue;
      memset(n,0,sizeof n); ok=1;
      for(int i=0;i<24 && ok;i++){
        int bit= i<16? i : 40+(i-16);
        for(;;){ char* e; long v=strtol(p,&e,10); if(e==p || v<0 || v>255){ ok=0; break; }
          if(v) n[v]|=1ull<<bit; p=e; if(*p=='+'){ p++; continue; } break; }
        if(ok && i<23){ if(*p!=',') ok=0; else p++; }
      }
      if(ok && *p) ok=0;
      if(ok) break;
      if(!pass){ static int said; if(!said){ said=1; LOG("input: keyboard_map is not valid, using the default keys\n"); } }
    }
    memcpy(t,n,sizeof t); snprintf(built,sizeof built,"%s",g_opt_kb_map); ready=1;
  }
  return t[k&255]; }

extern int fe_wndproc(void* h, unsigned m, unsigned long long w, long long l, long long* result);   /* dread_ui/fe.cpp */
extern int ov_wndproc(void* h, unsigned m, unsigned long long w, long long l);   /* dread_ui/ov.cpp: 1 = the menu took it */
extern int ov_is_open(void);
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l){
  if(h==g_fe_hwnd) return DefWindowProcA(h,m,w,l);
  if(m==WM_SIZE){ g_ww=LOWORD(l); g_wh=HIWORD(l); if(g_ww<1) g_ww=1; if(g_wh<1) g_wh=1; }
  if(g_fe_mode){ long long r=0; if(fe_wndproc(h,m,w,l,&r)) return (LRESULT)r; return DefWindowProcA(h,m,w,l); }
  if(m==WM_APP+11){ set_fullscreen((int)w); return 0; }
  if(!(m==WM_KEYDOWN && w==VK_F11) && !(m==WM_SYSKEYDOWN && w==VK_RETURN) && ov_wndproc(h,m,w,l)) return 0;
  switch(m){
  case WM_CLOSE: LOG("window: closed by the user or the system\n"); g_quit=1; return 0;
  case WM_QUERYENDSESSION: LOG("window: Windows is logging off / shutting down\n"); break;
  case WM_POWERBROADCAST: if(w==PBT_APMSUSPEND) LOG("window: the PC is going to sleep\n"); else if(w==PBT_APMRESUMEAUTOMATIC) LOG("window: the PC resumed from sleep\n"); break;
  /* GL/Vulkan cover the whole client area once frames come: a GDI erase then blinks the screen (fullscreen Vulkan), so
     no erase. Before the first frames nothing would paint it and Windows showed the window white at boot: paint it
     black until then */
  case WM_ERASEBKGND: { extern int g_frames; if(g_frames<3){ RECT rc; GetClientRect(h,&rc); FillRect((HDC)w,&rc,(HBRUSH)GetStockObject(BLACK_BRUSH)); } return 1; }
  case WM_PAINT: { extern int g_frames; if(g_frames<3){ PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); FillRect(dc,&ps.rcPaint,(HBRUSH)GetStockObject(BLACK_BRUSH)); EndPaint(h,&ps); return 0; } break; }
  case WM_SIZE: g_ww=LOWORD(l); g_wh=HIWORD(l); if(g_ww<1) g_ww=1; if(g_wh<1) g_wh=1; return 0;
  case WM_SYSKEYDOWN: if(w==VK_RETURN){ if(!(l&(1<<30))) set_fullscreen(!g_fullscreen); return 0; } if(w==VK_F4) break; /* let Alt+F4 through */
  /* fallthrough */
  case WM_KEYDOWN:
    /* a held key auto-repeats (bit 30): the one-shot actions below only fire on the first press,
       or holding F7 would start and stop the recording every frame */
    if((l&(1<<30)) && (w==VK_ESCAPE||w==VK_F1||w==VK_F3||w==VK_F4||w==VK_F5||w==VK_F7||w==VK_F8||w==VK_F11)) return 0;
    if(w==VK_ESCAPE){ g_quit=1; return 0; }
    if(w==VK_F11){ set_fullscreen(!g_fullscreen); return 0; }
    if(w==VK_F1){ return 0; }   /* the in-game menu (ov_wndproc) */
    if(0){ MessageBoxA(h,HELP,"Metroid Dread",MB_OK); }
    if(w==VK_F3){ g_shot_req=1; return 0; }
    if(w==VK_F7){ g_rec_toggle=1; return 0; }
    if(w==VK_TAB){ if(!(l & (1L<<30))){ extern volatile int g_ff_presses; g_ff_presses++; } return 0; }   /* fast forward toggle (fast_forward=1) */
    if(w==VK_F8){ extern volatile int g_shot8_req; g_shot8_req=1; return 0; }
    if(w==VK_F5){ extern void gpu_gl_dump_rts(void); extern void kernel_dump_threads(void); extern __thread FILE* g_log_redirect;
      CreateDirectoryA("captures",NULL); FILE* f=fopen("captures/threads.txt","w");   /* written next to the trace */
      if(f){ g_log_redirect=f; kernel_dump_threads(); g_log_redirect=NULL; fclose(f); }
      kernel_dump_threads(); gpu_gl_dump_rts(); return 0; }
    if(w==VK_F4){ if(l & (1L<<30)) return 0;
      /* 60 and back to the player's own cap (unlimited when that is 60), for this session: it used to flip between 60 and
         unlimited and save that, which replaced a 240 cap in the .ini (GitHub #20) */
      static int cap=-1; if(cap<0) cap= g_opt_fps==1? 0 : g_opt_fps;
      if(g_opt_fps==1) g_opt_fps=cap; else { cap=g_opt_fps; g_opt_fps=1; }
      char t[64]; if(g_opt_fps==0) snprintf(t,sizeof t,"FPS limit: unlimited"); else snprintf(t,sizeof t,"FPS limit: %d",g_opt_fps==1? 60 : g_opt_fps);
      SetWindowTextA(h,t); LOG("settings: F4 fps_limit=%d (not saved)\n",g_opt_fps); return 0; }
    keys_held|=keymap(w);if(test_tap_sample)keys_tapped|=keymap(w); return 0;
  case WM_SYSKEYUP: case WM_KEYUP: keys_held&=~keymap(w); return 0;
  case WM_KILLFOCUS: keys_held=0;keys_tapped=0; InterlockedExchange(&g_mbtn,0); ClipCursor(NULL); ReleaseCapture(); return 0;
  case WM_APP+8: { char t[300]; if(w) snprintf(t,sizeof t,"%s  |  RECORDING (F7 to stop)",g_title); else snprintf(t,sizeof t,"%s  |  Video saved in the recordings folder",g_title); SetWindowTextA(h,t); return 0; }
  case WM_APP+10: { char t[400]; char* s=(char*)l; if(s){ snprintf(t,sizeof t,"%s  |  %s",g_title,s); free(s); } else snprintf(t,sizeof t,"%s",g_title); SetWindowTextA(h,t); return 0; }   /* backend_status_win */
  case WM_APP+9: { char t[160]; if(w) snprintf(t,sizeof t,"%s  |  FAST FORWARD x%d (Tab or L3+R3)",g_title,(int)l); else snprintf(t,sizeof t,"%s",g_title); SetWindowTextA(h,t); return 0; }
  case WM_APP+7: { char t[160]; if(w) snprintf(t,sizeof t,"%s  |  LSFG ON x%d (frame generation)",g_title,(int)l); else snprintf(t,sizeof t,"%s  |  LSFG standby",g_title); SetWindowTextA(h,t); return 0; }
  case WM_INPUT: { RAWINPUT ri; UINT sz=sizeof ri;
      if(GetRawInputData((HRAWINPUT)l,RID_INPUT,&ri,&sz,sizeof(RAWINPUTHEADER))!=(UINT)-1 && ri.header.dwType==RIM_TYPEMOUSE &&
         !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)){ InterlockedExchangeAdd(&g_mdx,ri.data.mouse.lLastX); InterlockedExchangeAdd(&g_mdy,ri.data.mouse.lLastY); }
      break; }   /* DefWindowProc cleans up after the read */
  case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
  case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: {
      LONG bit= (m==WM_LBUTTONDOWN||m==WM_LBUTTONUP)? 1 : (m==WM_RBUTTONDOWN||m==WM_RBUTTONUP)? 2 : (m==WM_MBUTTONDOWN||m==WM_MBUTTONUP)? 4
              : (HIWORD(w)==XBUTTON1? 8 : 16);
      int down= m==WM_LBUTTONDOWN||m==WM_RBUTTONDOWN||m==WM_MBUTTONDOWN||m==WM_XBUTTONDOWN;
      LONG now= down? (g_mbtn|bit) : (g_mbtn&~bit); InterlockedExchange(&g_mbtn,now);
      extern int mouse_aim_button(void);
      if(now && GetCapture()!=h) SetCapture(h); else if(!now) ReleaseCapture();   /* releases still arrive outside the window */
      if(down && (bit&mouse_aim_button())){ RECT c; GetClientRect(h,&c); MapWindowPoints(h,NULL,(POINT*)&c,2); ClipCursor(&c); }   /* aiming: the cursor stays in */
      else if(!down && (bit&mouse_aim_button())) ClipCursor(NULL);
      return (m==WM_XBUTTONDOWN||m==WM_XBUTTONUP)? TRUE : 0; }
  case WM_SETCURSOR: if(LOWORD(l)==HTCLIENT && ov_is_open()){ SetCursor(LoadCursor(NULL,IDC_ARROW)); return TRUE; }
    { extern int g_opt_mouse_aim; if(g_opt_mouse_aim && LOWORD(l)==HTCLIENT){ SetCursor(NULL); return TRUE; } }   /* free aim: no cursor over the game */
    if(g_fullscreen && LOWORD(l)==HTCLIENT){ SetCursor(NULL); return TRUE; } break;
  }
  return DefWindowProcA(h,m,w,l);
}

static char g_title[256];

static void set_pixel_format(HDC dc){
  PIXELFORMATDESCRIPTOR pfd={sizeof pfd,1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,PFD_TYPE_RGBA,32};
  pfd.cDepthBits=24; pfd.cStencilBits=8; pfd.iLayerType=PFD_MAIN_PLANE;
  SetPixelFormat(dc,ChoosePixelFormat(dc,&pfd),&pfd); }

/* window + message pump on their own thread so nothing stalls them */
extern int fe_run(void* hwnd);   /* dread_ui/fe.cpp: its own message loop; 0 = play, 1 = quit */
/* Per-monitor DPI aware, before the first window. Without it, on a display with Windows scaling (150% on a 4K or
   1440p screen) Windows told the game the window was smaller than it is and stretched the picture up: no internal
   resolution showed more detail than that smaller size (GitHub issue #17), and a Vulkan swapchain made at the real
   size got the picture placed by the smaller numbers, in part of the screen (issue #12, about a third at 150%) */
static void dpi_aware(void){
  static int done; if(done) return; done=1;
  HMODULE u=GetModuleHandleA("user32.dll");
  typedef BOOL (WINAPI *SPDAC)(HANDLE); SPDAC f= u? (SPDAC)(void*)GetProcAddress(u,"SetProcessDpiAwarenessContext") : NULL;
  if(f && f((HANDLE)(intptr_t)-4)) return;   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (Windows 10 1703+) */
  SetProcessDPIAware(); }
static void win_thread(void* arg){
  dpi_aware();
  HINSTANCE hi=GetModuleHandle(NULL);
  WNDCLASSA wc={0}; wc.style=CS_OWNDC; wc.lpfnWndProc=wndproc; wc.hInstance=hi; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
  wc.hIcon=LoadIcon(hi,MAKEINTRESOURCE(1)); if(!wc.hIcon) wc.hIcon=LoadIcon(NULL,IDI_APPLICATION);
  wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); wc.lpszClassName="MetroidDreadWnd";
  RegisterClassA(&wc);
  RECT r={0,0,1280,720}; AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);
  g_hwnd=CreateWindowA("MetroidDreadWnd",g_title,WS_OVERLAPPEDWINDOW|(arg? 0 : WS_VISIBLE),CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,NULL,NULL,hi,NULL);
  { typedef UINT (WINAPI *GDFW)(HWND); GDFW gd=(GDFW)(void*)GetProcAddress(GetModuleHandleA("user32.dll"),"GetDpiForWindow");
    if(gd && g_hwnd) LOG("display: Windows scaling %u%% (the game works in real pixels)\n",gd(g_hwnd)*100/96); }
  if(arg){   /* front end first, in this window; the game continues in it after Play */
    g_fe_mode=1; g_fe_result=fe_run(g_hwnd); g_fe_mode=0;
    if(g_fe_result){ SetEvent(g_fe_done); return; }   /* the process ends */
    if(g_fullscreen) ShowCursor(FALSE);   /* the front end kept the cursor visible in fullscreen */
    SetEvent(g_fe_done); WaitForSingleObject(g_video_go,INFINITE);
    { /* The front end drew with a D3D11 flip-model swapchain. Once DXGI has had such a swapchain on a window, OpenGL
         and Vulkan presents to that window never reach the screen (MEASURED 2026-10-22: after Play the Play screen
         stayed up while the game ran, OpenGL and Vulkan, windowed and fullscreen). The game gets a new window in the
         same place, size and fullscreen state; the front end's goes. g_wp keeps the windowed placement for F11 */
      HWND old=g_hwnd; int fs=g_fullscreen; WINDOWPLACEMENT wp={sizeof wp};
      if(fs) wp=g_wp; else GetWindowPlacement(old,&wp);   /* fullscreen: the windowed placement it was entered from */
      HWND nw=CreateWindowA("MetroidDreadWnd",g_title,WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1280,720,NULL,NULL,hi,NULL);
      if(nw){ g_fe_hwnd=old; g_hwnd=nw;
        if(fs){ wp.showCmd=SW_HIDE; SetWindowPlacement(nw,&wp); g_fullscreen=0; ShowCursor(TRUE); set_fullscreen(1); }   /* as F11 does: borderless over the monitor, g_wp saved */
        else SetWindowPlacement(nw,&wp);   /* normal or maximized, as the player left it */
        ShowWindow(nw,SW_SHOW); SetForegroundWindow(nw);
        ShowWindow(old,SW_HIDE); DestroyWindow(old);
        { RECT cr; GetClientRect(nw,&cr); g_ww=cr.right>0? cr.right : 1; g_wh=cr.bottom>0? cr.bottom : 1; }
        LOG("window: the game continues in a new window (%dx%d%s)\n",g_ww,g_wh,g_fullscreen? ", fullscreen" : ""); }
      else WARN("window: could not make the game's window: the picture may not show\n"); }
    SetWindowTextA(g_hwnd,g_title);
    if(!g_want_fs && g_fullscreen) set_fullscreen(0); }
  /* a window with a GL pixel format can't take a Vulkan swapchain reliably: with renderer=vulkan the GL
     context lives on a hidden window and the visible one is left to Vulkan */
  if(g_want_vk){ WNDCLASSA gc={0}; gc.style=CS_OWNDC; gc.lpfnWndProc=DefWindowProcA; gc.hInstance=hi; gc.lpszClassName="MetroidDreadGL"; gc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);   /* own class: its size must not reach wndproc */
    RegisterClassA(&gc); g_glwnd=CreateWindowA("MetroidDreadGL","Metroid Dread GL",WS_POPUP,0,0,64,64,NULL,NULL,hi,NULL); }
  g_hdc=GetDC(g_glwnd? g_glwnd : g_hwnd);
  { extern int g_snxvk_want; if(!g_snxvk_want) set_pixel_format(g_hdc); }   /* native Vulkan: no GL pixel format on its window (set later if it falls back) */
  if(g_want_fs) set_fullscreen(1);
  ShowWindow(g_hwnd,SW_SHOW);
  { RAWINPUTDEVICE d={1,2,0,g_hwnd}; if(!RegisterRawInputDevices(&d,1,sizeof d)) LOG("input: raw mouse input unavailable: no mouse free aim\n"); }   /* generic desktop mouse */
  ls_launch();
  SetEvent(g_ready);
  MSG msg; while(GetMessageA(&msg,NULL,0,0)>0){ TranslateMessage(&msg); DispatchMessageA(&msg); }
  g_quit=1;
}

void video_init(const char* title){
  { extern void (*g_backend_status)(const char*); g_backend_status=backend_status_win; }
  test_tap_sample=getenv("SWR_AUTOMATION_INPUT")!=NULL;
  snprintf(g_title,sizeof g_title,"%s",title);
  xinput_load(); sdl_load(); timeBeginPeriod(1);
  if(!g_win_started){ g_ready=CreateEventA(NULL,TRUE,FALSE,NULL); _beginthread(win_thread,1<<20,NULL); }
  else SetEvent(g_video_go);   /* the front end ran in the window: continue there */
  WaitForSingleObject(g_ready,INFINITE);
  g_start=plat_ns(); if(getenv("SWR_REC")) g_rec_toggle=1;
  { /* under Proton the driver is Mesa; glthread has to be enabled from the Steam launch options */
    HMODULE nt=GetModuleHandleA("ntdll.dll"); if(nt && GetProcAddress(nt,"wine_get_version"))
      LOG("Proton/Wine detected: for more FPS put   mesa_glthread=true %%command%%   in the game's Steam launch options (moves the OpenGL driver work to another core)\n"); }
  { extern int snxvk_start(void); extern int g_snxvk_want;
    if(g_snxvk_want && snxvk_start()) return;   /* renderer=vulkan_native: the ScarletNX Vulkan renderer draws, no GL */
    if(g_snxvk_want) set_pixel_format(g_hdc); }
  extern void gpu_gl_start(void); gpu_gl_start();
}

/* Play screen / first-run setup before the game starts (main.c). 0 = go on with the game, 1 = quit.
   Skipped for test runs (replays, exit frame, --no-launcher) and when the front end decides to (skip_launcher=1, --play). */
extern int fe_should_show(int argc, char** argv, const char* exedir, const char* exename);   /* dread_ui/fe.cpp */
int backend_frontend(int argc, char** argv, const char* exedir, const char* exename){
  if(!getenv("SWR_FE_TEST") && (getenv("SWR_TEST_SCRIPT") || getenv("SWR_TEST_EXIT_FRAME") || getenv("SWR_TEST_RECORD"))) return 0;   /* SWR_FE_TEST: front-end tests */
  for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--no-launcher")) return 0;
  if(!fe_should_show(argc,argv,exedir,exename)) return 0;
  snprintf(g_title,sizeof g_title,"Metroid Dread");
  g_ready=CreateEventA(NULL,TRUE,FALSE,NULL); g_fe_done=CreateEventA(NULL,TRUE,FALSE,NULL); g_video_go=CreateEventA(NULL,TRUE,FALSE,NULL);
  g_win_started=1; _beginthread(win_thread,1<<20,(void*)1);
  WaitForSingleObject(g_fe_done,INFINITE);
  return g_fe_result; }

#ifdef SWR_HAVE_SNXVK
/* the window side of renderer=vulkan_native (snxvk_glue.c): the loader, the instance extensions and a surface on g_hwnd */
void* backend_vk_loader(void){ HMODULE m=LoadLibraryA("vulkan-1.dll"); return m? (void*)GetProcAddress(m,"vkGetInstanceProcAddr") : NULL; }
const char* const* backend_vk_extensions(u32* n){ static const char* const e[2]={"VK_KHR_surface","VK_KHR_win32_surface"}; *n=2; return e; }
int backend_vk_surface(void* instance, u64* surface){
  typedef void* (__stdcall *GIPA)(void*,const char*); typedef int (__stdcall *CWS)(void*,const void*,const void*,u64*);
  struct { int sType; const void* pNext; u32 flags; HINSTANCE hinstance; HWND hwnd; } ci={1000009000,NULL,0,GetModuleHandle(NULL),g_hwnd};
  GIPA gipa=(GIPA)backend_vk_loader(); CWS cws= gipa? (CWS)gipa(instance,"vkCreateWin32SurfaceKHR") : NULL;
  if(!cws || cws(instance,&ci,NULL,surface)){ LOG("vulkan_native: no Vulkan surface on the window\n"); return 0; }
  return 1; }
u32 backend_window_generation(void){ return 1; }   /* the swapchain follows the window size by itself */
void backend_window_safe(int* x, int* y, int* w, int* h){ *x=0; *y=0; *w=g_ww; *h=g_wh; }
int backend_window_shown(void){ return g_hwnd && !IsIconic(g_hwnd); }
void* backend_window_monitor(void){ return g_hwnd? (void*)MonitorFromWindow(g_hwnd,MONITOR_DEFAULTTOPRIMARY) : NULL; }   /* HDR peak lookup (snxvk_glue.c) */
#endif

/* ---------------------------------------------------------------- GL, GPU thread */
typedef HGLRC (WINAPI *CCA)(HDC,HGLRC,const int*);
typedef BOOL (WINAPI *SWI)(int);
static void* backend_gl_proc_raw(const char* n);
#include "vk_present.inc"

int backend_gl_create(void){
  /* Right after the Play screen hands over to the new window, context creation sometimes failed on a GPU that has
     OpenGL 4.6 (3 of 12 fullscreen starts on an RTX 5070, MEASURED 2026-10-30; v0.4.0 0 of 6): log why and try again
     a few times before giving up */
  for(int attempt=0;attempt<10 && !g_ctx;attempt++){
    if(attempt){ Sleep(100); if(!GetPixelFormat(g_hdc)) set_pixel_format(g_hdc); }
    HGLRC tmp=wglCreateContext(g_hdc);
    if(!tmp){ WARN("OpenGL: no context on the window (attempt %d, pixel format %d, error %lu)\n",attempt+1,GetPixelFormat(g_hdc),GetLastError()); continue; }
    wglMakeCurrent(g_hdc,tmp);
    CCA cca=(CCA)wglGetProcAddress("wglCreateContextAttribsARB");
    static const int vers[4][2]={{4,6},{4,5},{4,4},{4,3}};
    for(int i=0;i<4 && !g_ctx && cca;i++){ int ca[]={0x2091,vers[i][0],0x2092,vers[i][1],0x9126,0x00000001,0x2094,getenv("SWR_GL_DRIVER_DEBUG")?1:0,0}; g_ctx=cca(g_hdc,NULL,ca);
      if(g_ctx) g_wctx=cca(g_hdc,g_ctx,ca); }   /* second shared context for the shader compile thread */
    if(!g_ctx){ typedef const unsigned char* (WINAPI *GGS)(unsigned); GGS ggs=(GGS)backend_gl_proc_raw("glGetString");
      const char* rn= ggs? (const char*)ggs(0x1F01) : NULL;   /* GL_RENDERER */
      WARN("OpenGL: no 4.3+ context (attempt %d, pixel format %d, %s, renderer %s, error %lu)\n",attempt+1,GetPixelFormat(g_hdc),
           cca? "attribs entry found" : "no attribs entry",rn? rn : "?",GetLastError()); }
    wglMakeCurrent(NULL,NULL); wglDeleteContext(tmp); }
  if(!g_ctx){ fatal("Could not start OpenGL 4.3 on the game window.\nUpdate your GPU driver, or pick Native Vulkan in the settings.\nMetroid Dread.log says why."); return 0; }
  wglMakeCurrent(g_hdc,g_ctx);
  if(g_glwnd && !vkp_init()){
    /* Vulkan unavailable: give the real window the same pixel format and render to it with GL as usual */
    HDC dc=GetDC(g_hwnd); set_pixel_format(dc);
    if(wglMakeCurrent(dc,g_ctx)){ g_hdc=dc; vkp.fallback_window=1; }
    else fatal("Vulkan output failed and the OpenGL fallback could not use the window.\nSet renderer=opengl in the .ini (or pick OpenGL in the launcher)."); }
  SWI si=(SWI)wglGetProcAddress("wglSwapIntervalEXT"); extern int g_opt_vsync; if(si) si(g_opt_vsync?1:0);
  return 1; }

static void* backend_gl_proc_raw(const char* n){
  void* p=(void*)wglGetProcAddress(n);
  if(!p || p==(void*)1 || p==(void*)2 || p==(void*)3 || p==(void*)-1){ static HMODULE gl; if(!gl) gl=LoadLibraryA("opengl32.dll"); p=(void*)GetProcAddress(gl,n); }
  return p; }

void* backend_gl_proc(const char* n){
  if(vkp.on && !strcmp(n,"glBindFramebuffer")) return (void*)vkp_bind_framebuffer;   /* framebuffer 0 = the image Vulkan presents */
  return backend_gl_proc_raw(n); }

void backend_gl_vsync(int on){ SWI si=(SWI)wglGetProcAddress("wglSwapIntervalEXT"); if(si) si(on?1:0); }   /* GL thread */
void backend_gl_swap(void){ if(vkp.on) vkp_swap(); else SwapBuffers(g_hdc); }

int backend_gl_worker_current(void){ return g_wctx && wglMakeCurrent(g_hdc,g_wctx); }

void backend_window_size(int* w, int* h){ *w=g_ww; *h=g_wh; }

void video_present_rgba(const u32* px, int w, int h, int stride){ extern void gpu_gl_present_cpu(const u32*,int,int,int); gpu_gl_present_cpu(px,w,h,stride); }

static i32 axis(u64 b, int neg, int pos){ return ((b>>pos)&1? 32767:0) - ((b>>neg)&1? 32767:0); }




/* Test-only virtual-controller replay. CSV: guest_frame,hex_buttons,lx,ly,rx,ry.
   State holds until the next row. Normal input is untouched when env is unset.
   Frame numbers are present submissions, not wall-clock timestamps. */
typedef struct {u32 frame;u64 buttons;i32 lx,ly,rx,ry;} TestInput;
static TestInput* test_inputs;static u32 test_n,test_i;static int test_loaded,test_replay;
static FILE*test_record;static TestInput test_previous;static int test_recorded;
/* SWR_TEST_SCRIPT_MS=1 (tests, ported from the 2026-10-26 Codex diagnostics): the replay's first column is milliseconds
   since the guest started (backend_test_guest_start) instead of presented frames, so inputs land at the same game time
   whatever the frame rate. SWR_TEST_EXIT_MS ends the run the same way */
static int test_ms; static u64 test_clock_start;
static void test_input_load(void){
  if(test_loaded)return;test_loaded=1;
  const char*path=getenv("SWR_TEST_SCRIPT");
  if(path&&*path){test_replay=1;FILE*f=fopen(path,"r");char line[256];u32 cap=8192;
    if(!f){WARN("test input: cannot open replay %s\n",path);ExitProcess(2);}
    test_inputs=calloc(cap,sizeof*test_inputs);if(!test_inputs){fclose(f);ExitProcess(2);}
    while(fgets(line,sizeof line,f)){if(line[0]=='#'||line[0]=='\n')continue;TestInput v={0};unsigned long long mask;
      if(sscanf(line,"%u,%llx,%d,%d,%d,%d",&v.frame,&mask,&v.lx,&v.ly,&v.rx,&v.ry)!=6||test_n==cap||(test_n&&v.frame<test_inputs[test_n-1].frame)||v.lx< -32768||v.lx>32767||v.ly< -32768||v.ly>32767||v.rx< -32768||v.rx>32767||v.ry< -32768||v.ry>32767){fclose(f);WARN("test input: invalid replay row\n");ExitProcess(2);}
      v.buttons=(u64)mask;test_inputs[test_n++]=v;
    }fclose(f);if(!test_n||test_inputs[0].frame!=0){WARN("test input: replay must start at frame 0\n");ExitProcess(2);}
    LOG("test input: frame replay active, %u states\n",test_n);
  }
  path=getenv("SWR_TEST_RECORD");if(path&&*path){test_record=fopen(path,"w");if(!test_record)ExitProcess(2);fprintf(test_record,"# guest_frame,buttons,lx,ly,rx,ry\n");}
}
static void test_input_apply(u64*b,i32*lx,i32*ly,i32*rx,i32*ry){
  test_input_load();u32 frame=(u32)g_frames;
  { static int done; const char*se; if(!done&&(se=getenv("SWR_TEST_SETENV"))&&*se){ u32 at=(u32)strtoul(se,NULL,10); const char*kv=strchr(se,':');   /* test only: "FRAME:NAME=VALUE" sets an env var at that guest frame */
      if(!kv){done=1;} else if(frame>=at){ char k[64]; const char*eq=strchr(kv+1,'='); if(eq&&eq-kv-1<64){ memcpy(k,kv+1,eq-kv-1); k[eq-kv-1]=0; SetEnvironmentVariableA(k,eq+1); { char pe[200]; snprintf(pe,sizeof pe,"%s=%s",k,eq+1); _putenv(pe); } LOG("test: frame %u set %s=%s\n",frame,k,eq+1); } done=1; } } }
  if(test_replay){u64 pos=frame; if(test_ms){ if(!test_clock_start) test_clock_start=plat_ns(); pos=(plat_ns()-test_clock_start)/1000000ull; }
    while(test_i+1<test_n&&test_inputs[test_i+1].frame<=pos)test_i++;TestInput*v=&test_inputs[test_i];*b=v->buttons;*lx=v->lx;*ly=v->ly;*rx=v->rx;*ry=v->ry;}
  if(test_record&&(!test_recorded||*b!=test_previous.buttons||*lx!=test_previous.lx||*ly!=test_previous.ly||*rx!=test_previous.rx||*ry!=test_previous.ry)){
    fprintf(test_record,"%u,%llx,%d,%d,%d,%d\n",frame,(unsigned long long)*b,*lx,*ly,*rx,*ry);fflush(test_record);
    test_previous=(TestInput){frame,*b,*lx,*ly,*rx,*ry};test_recorded=1;
  }
}

/* Mouse free aim. Dread aims freely while L is held, in the left stick's direction: while the mouse button set to l
   (right by default) is held, the mouse moves a virtual stick, which turns the aim. It starts centred (the arm points
   ahead), the mouse pushes it, and it stays where it was pushed (a stick needs no holding): ~150 counts at 100%
   sensitivity swing it from centre to the edge. The other mouse buttons press their Switch buttons */
static int g_mb_bit[5]={-2,-2,-2,-2,-2};   /* -2 not parsed yet, -1 none */
void mouse_map_reset(void){ g_mb_bit[0]=-2; }
static void mouse_map(void){
  if(g_mb_bit[0]!=-2) return;
  extern char g_opt_mouse_buttons[]; static const struct { const char* n; int bit; } N[]={{"a",0},{"b",1},{"x",2},{"y",3},
    {"lstick",4},{"rstick",5},{"l",6},{"r",7},{"zl",8},{"zr",9},{"plus",10},{"minus",11}};
  const char* p=g_opt_mouse_buttons;
  for(int i=0;i<5;i++){ char t[16]; int k=0; while(*p==' ') p++; while(*p && *p!=',' && k<15) t[k++]=(char)tolower((unsigned char)*p++); t[k]=0; if(*p==',') p++;
    g_mb_bit[i]=-1; for(size_t j=0;j<sizeof N/sizeof *N;j++) if(!strcmp(t,N[j].n)) g_mb_bit[i]=N[j].bit; } }
int mouse_aim_button(void){ mouse_map(); int m=0; for(int i=0;i<5;i++) if(g_mb_bit[i]==6) m|=1<<i; return m; }   /* the mouse buttons set to L */
/* test only: SWR_TEST_MOUSE="FRAME:DX,DY,BUTTONS;..." adds that movement at that guest frame and holds those buttons
   (hex, 1 left 2 right 4 middle) from there to the next entry */
static void mouse_test(LONG* dx, LONG* dy, LONG* btn){
  static const char* s; static int init, held; if(!init){ init=1; s=getenv("SWR_TEST_MOUSE"); }
  if(!s) return; extern int g_frames;
  while(*s){ char* e; long f=strtol(s,&e,10); if(e==s || *e!=':' || g_frames<f) break;
    long x=strtol(e+1,&e,10); if(*e==',') e++; long y=strtol(e,&e,10); if(*e==',') e++; long b=strtol(e,&e,16);
    *dx+=x; *dy+=y; held=(int)b; s= *e==';'? e+1 : e; }
  *btn|=held; }
static void mouse_apply(u64* b, i32* lx, i32* ly){
  extern int g_opt_mouse_aim, g_opt_mouse_sens;
  LONG dx=InterlockedExchange(&g_mdx,0), dy=InterlockedExchange(&g_mdy,0), btn=g_mbtn;
  static float vx, vy; static int was;
  mouse_test(&dx,&dy,&btn);
  if(!g_opt_mouse_aim){ was=0; return; }
  mouse_map();
  for(int i=0;i<5;i++) if((btn>>i&1) && g_mb_bit[i]>=0) *b|=1ull<<g_mb_bit[i];
  int aiming=(btn & mouse_aim_button())!=0;
  if(aiming){
    if(!was) vx=vy=0;
    float s=(float)g_opt_mouse_sens/100.0f/150.0f;
    vx+=(float)dx*s; vy-=(float)dy*s;   /* screen y is down, the stick's up */
    float m=sqrtf(vx*vx+vy*vy); if(m>1.0f){ vx/=m; vy/=m; m=1.0f; }
    if(m>=0.25f){ *lx=(i32)(vx/m*32767.0f); *ly=(i32)(vy/m*32767.0f); } else *lx=*ly=0;   /* aimed: full tilt that way */
    { static int said; if(!said){ said=1; LOG("input: mouse free aim in use\n"); } } }
  was=aiming; }

void video_poll(void){
  u64 k=keys_held|keys_tapped, b=0;keys_tapped=0;
  i32 lx=axis(k,40,42), ly=axis(k,43,41), rx=axis(k,44,46), ry=axis(k,47,45);
  int sdl_on=sdl_poll(&b,&lx,&ly,&rx,&ry);
  extern int g_opt_ctrl_api;
  { static int seen; if(sdl_on && !(seen&1) && (b&0xFFFF)){ seen|=1; LOG("input: first button press seen through SDL2\n"); } }
  if(xget && (!sdl_on || g_opt_ctrl_api!=1)){   /* auto: XInput pads also read directly, so they work even if SDL misses them */
    for(DWORD i=0;i<4;i++){ XST s; if((xget_ex? xget_ex : xget)(i,&s)) continue; XGP* g=&s.Gamepad; WORD x=g->wButtons; if(x&0x0400) b|=PAD_HOME;
      /* map by position like a Switch pad: A right (xbox B), B bottom (xbox A), X top, Y left */
      if(x&0x2000) b|=1<<0; if(x&0x1000) b|=1<<1; if(x&0x8000) b|=1<<2; if(x&0x4000) b|=1<<3;
      if(x&0x0040) b|=1<<4; if(x&0x0080) b|=1<<5; if(x&0x0100) b|=1<<6; if(x&0x0200) b|=1<<7;
      if(g->bLeftTrigger>60) b|=1<<8; if(g->bRightTrigger>60) b|=1<<9;
      if(x&0x0010) b|=1<<10; if(x&0x0020) b|=1<<11;
      if(x&0x0004) b|=1<<12; if(x&0x0001) b|=1<<13; if(x&0x0008) b|=1<<14; if(x&0x0002) b|=1<<15;
      if(!lx&&!ly){ lx=g->sThumbLX; ly=g->sThumbLY; stick_dz(&lx,&ly); } if(!rx&&!ry){ rx=g->sThumbRX; ry=g->sThumbRY; stick_dz(&rx,&ry); }
      { static int xseen; if(!xseen && x){ xseen=1; LOG("input: first button press seen through XInput (pad %lu)\n",(unsigned long)i); } }
      g_xpad=(int)i;
      break; }
  }
  u64 phys=b; i32 plx=lx, ply=ly; b&=~PAD_HOME;
  b=swr_pad_remap(b)|(k&0xFFFFull);
  /* stick-as-button bits 16..23 */
  if(lx<-16000) b|=1ull<<16; if(ly>16000) b|=1ull<<17; if(lx>16000) b|=1ull<<18; if(ly<-16000) b|=1ull<<19;
  if(rx<-16000) b|=1ull<<20; if(ry>16000) b|=1ull<<21; if(rx>16000) b|=1ull<<22; if(ry<-16000) b|=1ull<<23;
  if(g_script[0]){ /* test input like the X11 backend: "ms:hex,ms:hex,..." holds those buttons for 300 ms */
    u64 el=(plat_ns()-g_start)/1000000ull; const char* p=g_script;
    while(*p){ u64 t=strtoull(p,(char**)&p,10); if(*p!=':') break; p++; u64 m=strtoull(p,(char**)&p,16); if(el>=t && el<t+300){ b|=m; if(m&(1ull<<16))lx=-32767; if(m&(1ull<<18))lx=32767; if(m&(1ull<<17))ly=32767; if(m&(1ull<<19))ly=-32767; if(m&(1ull<<20))rx=-32767; if(m&(1ull<<22))rx=32767; if(m&(1ull<<21))ry=32767; if(m&(1ull<<23))ry=-32767; } if(*p==',') p++; } }
  test_input_apply(&b,&lx,&ly,&rx,&ry);
  { extern u64 swr_menu_buttons(u64); b=swr_menu_buttons(b); }   /* menu_buttons=swap (main.c), after the test replay like a player's own presses */
  { extern volatile u64 g_cheat_keys; g_cheat_keys=b; }   /* cheats.c keypress conditions */
  if(test_replay){ phys=b; plx=lx; ply=ly; }   /* tests drive the menu with the replayed buttons */
  { extern int ov_pad(unsigned long long phys, int lx, int ly);   /* dread_ui/ov.cpp: 1 = the menu has the controller */
    if(ov_pad(phys,plx,ply)){ b=0; lx=ly=rx=ry=0; } }
  if(!ov_is_open()) mouse_apply(&b,&lx,&ly);   /* the menu uses the mouse itself while open */
  { /* rumble (svc_hid.c hid_rumble_get) to the pads input comes from: on a change, else refreshed every 100 ms
       (SDL's rumble runs for the duration given, so it stops by itself when the game stops sending) */
    extern void hid_rumble_get(float*,float*); float lo, hi; hid_rumble_get(&lo,&hi);
    static unsigned short plo=1, phi=1; static u64 last; u64 now=plat_ns();
    unsigned short ulo=(unsigned short)(lo*65535.0f), uhi=(unsigned short)(hi*65535.0f);
    if(ulo!=plo || uhi!=phi || ((ulo||uhi) && now-last>100000000ull)){ plo=ulo; phi=uhi; last=now;
      extern int g_opt_ctrl_index;
      if(sdl_ok && sdl_gcrumble){ int first= g_opt_ctrl_index>0? g_opt_ctrl_index-1 : 0, lastp= g_opt_ctrl_index>0? g_opt_ctrl_index : sdl_npad;
        for(int i=first;i<lastp && i<sdl_npad;i++) sdl_gcrumble(sdl_pad[i],ulo,uhi,ulo||uhi? 250 : 0); }
      if(xset && g_xpad>=0){ XVIB v={ulo,uhi}; xset((DWORD)g_xpad,&v); } } }
  { extern u64 skip_hold_filter(u64); if(!ov_is_open()) b=skip_hold_filter(b); }   /* main.c: hold - to skip a cutscene */
  g_pad.buttons=b; g_pad.lx=lx; g_pad.ly=ly; g_pad.rx=rx; g_pad.ry=ry;
}

void backend_test_guest_start(void){if(g_script[0]&&getenv("SWR_SCRIPT_GUEST_CLOCK"))g_start=plat_ns();
  if(getenv("SWR_TEST_SCRIPT_MS")){ test_ms=1; test_clock_start=plat_ns(); LOG("test input clock: start_ns=%llu\n",(unsigned long long)test_clock_start); } }

void frontend_frame(void){
  extern u64 gpu_gl_screenshot(const char* path);
  if(g_script[0]){
    static int configured;static const char*times;static const char*prefix;static u64 next;
    if(!configured){configured=1;times=getenv("SWR_TEST_CAPTURE_MS");prefix=getenv("SWR_TEST_CAPTURE_PREFIX");if(times&&prefix)next=strtoull(times,(char**)&times,10);else times=NULL;}
    u64 elapsed=(plat_ns()-g_start)/1000000ull;
    if(times&&next&&elapsed>=next){char path[240];snprintf(path,sizeof path,"%s-%llu",prefix,(unsigned long long)next);
      LOG("test capture: due=%llu elapsed=%llu ns=%llu\n",(unsigned long long)next,(unsigned long long)elapsed,(unsigned long long)plat_ns());gpu_gl_screenshot(path);
      if(*times==','){times++;next=strtoull(times,(char**)&times,10);}else{times=NULL;next=0;}
    }
  }


  if(getenv("SWR_TEST_SCRIPT")){
    static int configured;static const char*times;static const char*prefix;static u64 next;
    if(!configured){configured=1;times=getenv("SWR_TEST_CAPTURE_FRAMES");prefix=getenv("SWR_TEST_CAPTURE_PREFIX");if(times&&prefix)next=strtoull(times,(char**)&times,10);else times=NULL;}
    if(times&&next&&(u64)g_frames>=next){char path[1024];snprintf(path,sizeof path,"%s-f%llu",prefix,(unsigned long long)next);
      LOG("test capture: requested_frame=%llu actual_frame=%d ns=%llu\n",(unsigned long long)next,g_frames,(unsigned long long)plat_ns());gpu_gl_screenshot(path);
      if(*times==','){times++;next=strtoull(times,(char**)&times,10);}else{times=NULL;next=0;}
    }

    {static int changed;const char*target=getenv("SWR_TEST_FPS_AFTER_LOAD");
      if(target&&!changed&&g_frames>=2300){int fps=atoi(target);if(fps!=0&&fps!=60&&fps!=120&&fps!=240){WARN("test FPS: invalid target\n");ExitProcess(2);}
        changed=1;g_opt_fps=fps;LOG("test FPS: selected=%d frame=%d ns=%llu\n",fps,g_frames,(unsigned long long)plat_ns());
      }
    }
    static u64 heartbeat;u64 now=plat_ns();if(!heartbeat||now-heartbeat>=1000000000ull){heartbeat=now;LOG("test heartbeat: frame=%d fps_limit=%d ns=%llu\n",g_frames,g_opt_fps,(unsigned long long)now);}
    /* SWR_TEST_CRASH_FRAME=N: a crash inside a C-library DLL on this runtime thread (tests the crash report) */
    { const char*cr=getenv("SWR_TEST_CRASH_FRAME"); if(cr&&(u64)g_frames>=strtoull(cr,NULL,10)){ void*(*volatile ms)(void*,int,size_t)=memset; ms((void*)(uintptr_t)0x10,0,16); } }
    const char*stop=getenv("SWR_TEST_EXIT_FRAME"),*stop_ms=getenv("SWR_TEST_EXIT_MS");
    if((stop&&(u64)g_frames>=strtoull(stop,NULL,10)) || (stop_ms&&test_clock_start&&(plat_ns()-test_clock_start)/1000000ull>=strtoull(stop_ms,NULL,10))){LOG("test complete: frame=%d\n",g_frames);extern void save_hints(void);save_hints();if(getenv("SWR_VK_RECIPES_RECORD")){extern void snxvk_exit(void);snxvk_exit();}log_flush();fflush(NULL);ExitProcess(0);}
  }

  if(g_shot_req){ g_shot_req=0; char p[MAX_PATH]; SYSTEMTIME t; GetLocalTime(&t); CreateDirectoryA("captures",NULL);
    snprintf(p,sizeof p,"captures/screenshot_%04d%02d%02d_%02d%02d%02d",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond); gpu_gl_screenshot(p); }
  if(g_seconds && plat_ns()-g_start>=(u64)g_seconds*1000000000ull){
    u64 h=gpu_gl_screenshot(g_shot[0]? g_shot : NULL); fprintf(stderr,"\nframes=%d hash=%016llx\n",g_frames,(unsigned long long)h);
    extern void save_hints(void); save_hints(); { extern void gpu_gl_rec_finish(void); gpu_gl_rec_finish(); } log_flush(); fflush(NULL); ExitProcess(0); }
}

int backend_glq_ok(void){ return 1; }   /* GL calls go through glq */
