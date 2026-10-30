/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* X11 + EGL + GLES 3.2. Lets us test the Android GL path on a PC. */
#include "rt.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <EGL/egl.h>

static Display* dpy; static Window win; static Atom wm_delete; static int g_ww=1280, g_wh=720;
static int g_seconds=0, g_maxframes=-1; static char g_shot[1024]; static u64 g_start; static u64 g_press=0, g_press_at=0, g_press_len=300;
static char g_script[1024];
static u64 keys_held;
extern int gl_frontend_available;

/* [Input] mouse_buttons: the game mouse (free aim, buttons) is the Windows backend's only; nothing to re-parse here */
void mouse_map_reset(void){}
void backend_ini(const char* k, const char* v){
  if(!strcmp(k,"seconds")) g_seconds=atoi(v);
  else if(!strcmp(k,"frames")) g_maxframes=atoi(v);
  else if(!strcmp(k,"shot")) strncpy(g_shot,v,1023);
  else if(!strcmp(k,"press")) g_press=strtoull(v,NULL,16);
  else if(!strcmp(k,"press_at")) g_press_at=strtoull(v,NULL,10);
  else if(!strcmp(k,"press_len")) g_press_len=strtoull(v,NULL,10);
  else if(!strcmp(k,"script")) strncpy(g_script,v,1023);
}

int video_has_gl(void){ return 1; }
static EGLDisplay edpy; static EGLSurface esurf; static EGLContext ectx; static EGLConfig ecfg;

void video_init(const char* title){
  XInitThreads();
  dpy=XOpenDisplay(NULL); if(!dpy) fatal("No X11 display (DISPLAY).");
  int scr=DefaultScreen(dpy);
  edpy=eglGetDisplay((EGLNativeDisplayType)dpy); if(!eglInitialize(edpy,NULL,NULL)) fatal("EGL: failed to initialize");
  static const EGLint att[]={EGL_RENDERABLE_TYPE,0x40 /*ES3*/,EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
  EGLint n=0; if(!eglChooseConfig(edpy,att,&ecfg,1,&n) || !n) fatal("EGL: no config");
  EGLint vid=0; eglGetConfigAttrib(edpy,ecfg,EGL_NATIVE_VISUAL_ID,&vid);
  XVisualInfo tmpl; memset(&tmpl,0,sizeof tmpl); tmpl.visualid=vid; int nv=0; XVisualInfo* vi=XGetVisualInfo(dpy,VisualIDMask,&tmpl,&nv);
  if(!vi){ XVisualInfo t2; memset(&t2,0,sizeof t2); t2.screen=scr; vi=XGetVisualInfo(dpy,VisualScreenMask,&t2,&nv); }
  XSetWindowAttributes swa; memset(&swa,0,sizeof swa); swa.colormap=XCreateColormap(dpy,RootWindow(dpy,vi->screen),vi->visual,AllocNone);
  swa.event_mask=KeyPressMask|KeyReleaseMask|StructureNotifyMask|ButtonPressMask|ButtonReleaseMask|PointerMotionMask;
  win=XCreateWindow(dpy,RootWindow(dpy,vi->screen),0,0,g_ww,g_wh,0,vi->depth,InputOutput,vi->visual,CWColormap|CWEventMask,&swa);
  XStoreName(dpy,win,title); wm_delete=XInternAtom(dpy,"WM_DELETE_WINDOW",False); XSetWMProtocols(dpy,win,&wm_delete,1);
  XMapWindow(dpy,win); XFlush(dpy);
  g_start=plat_ns();
  extern void gpu_gl_start(void); gpu_gl_start();
}

int backend_gl_create(void){
  eglBindAPI(EGL_OPENGL_ES_API);
  static const EGLint ca[]={EGL_CONTEXT_MAJOR_VERSION,3,EGL_CONTEXT_MINOR_VERSION,2,EGL_NONE};
  ectx=eglCreateContext(edpy,ecfg,EGL_NO_CONTEXT,ca); if(!ectx) return 0;
  esurf=eglCreateWindowSurface(edpy,ecfg,(EGLNativeWindowType)win,NULL); if(!esurf) return 0;
  if(!eglMakeCurrent(edpy,esurf,esurf,ectx)) return 0;
  eglSwapInterval(edpy,0);
  return 1; }

void* backend_gl_proc(const char* n){ return (void*)eglGetProcAddress(n); }

void backend_gl_swap(void){ extern void overlay_draw(int w, int h); overlay_draw(g_ww,g_wh); eglSwapBuffers(edpy,esurf); }

void backend_window_size(int* w, int* h){ *w=g_ww; *h=g_wh; }

void video_present_rgba(const u32* px, int w, int h, int stride){ extern void gpu_gl_present_cpu(const u32*,int,int,int); gpu_gl_present_cpu(px,w,h,stride); }

/* keys -> HidNpadButton bits */
static u64 keymap(KeySym k){
  switch(k){ case XK_x: case XK_space: return 1ull<<0; /* A */ case XK_z: return 1ull<<1; /* B */ case XK_s: return 1ull<<2; /* X */ case XK_a: return 1ull<<3; /* Y */
    case XK_q: return 1ull<<6; /* L */ case XK_e: return 1ull<<7; /* R */ case XK_1: return 1ull<<8; /* ZL */ case XK_3: return 1ull<<9; /* ZR */
    case XK_Return: return 1ull<<10; /* + */ case XK_BackSpace: return 1ull<<11; /* - */
    case XK_Left: return 1ull<<12; case XK_Up: return 1ull<<13; case XK_Right: return 1ull<<14; case XK_Down: return 1ull<<15; }
  return 0; }

void video_poll(void){
  while(XPending(dpy)){ XEvent e; XNextEvent(dpy,&e);
    if(e.type==KeyPress){ KeySym k=XLookupKeysym(&e.xkey,0); if(k==XK_Escape) g_quit=1; keys_held|=keymap(k); }
    else if(e.type==KeyRelease){ KeySym k=XLookupKeysym(&e.xkey,0); keys_held&=~keymap(k); }
    else if(e.type==ButtonPress){ extern void overlay_touch(int,int,float,float); overlay_touch(0,0,(float)e.xbutton.x,(float)e.xbutton.y); }
    else if(e.type==MotionNotify){ extern void overlay_touch(int,int,float,float); if(e.xmotion.state&Button1Mask) overlay_touch(0,1,(float)e.xmotion.x,(float)e.xmotion.y); }
    else if(e.type==ButtonRelease){ extern void overlay_touch(int,int,float,float); overlay_touch(0,2,(float)e.xbutton.x,(float)e.xbutton.y); }
    else if(e.type==ConfigureNotify){ g_ww=e.xconfigure.width; g_wh=e.xconfigure.height; }
    else if(e.type==ClientMessage && (Atom)e.xclient.data.l[0]==wm_delete) g_quit=1; }
  u64 el=(plat_ns()-g_start)/1000000ull, b=keys_held;
  if(g_press && el>=g_press_at && el<g_press_at+g_press_len) b|=g_press;
  if(g_script[0]){ /* "ms:hex,ms:hex,..." */ const char* p=g_script; while(*p){ u64 t=strtoull(p,(char**)&p,10); if(*p!=':') break; p++; u64 m=strtoull(p,(char**)&p,16); if(el>=t && el<t+g_press_len) b|=m; if(*p==',') p++; } }
  g_pad.buttons=b; g_pad.lx=g_pad.ly=g_pad.rx=g_pad.ry=0;
  { extern void overlay_state(u64*,i32*,i32*,i32*,i32*); overlay_state(&g_pad.buttons,&g_pad.lx,&g_pad.ly,&g_pad.rx,&g_pad.ry); }
}

void frontend_frame(void){
  u64 el=plat_ns()-g_start;
  if((g_maxframes>=0 && g_frames>=g_maxframes) || (g_seconds && el>=(u64)g_seconds*1000000000ull)){
    extern u64 gpu_gl_screenshot(const char* path); u64 h=gpu_gl_screenshot(g_shot[0]? g_shot : NULL);
    fprintf(stderr,"\nframes=%d hash=%016llx 1280x720\n",g_frames,(unsigned long long)h);
    extern void save_hints(void); save_hints(); log_flush(); fflush(NULL); _exit(0); }
}

int backend_gl_worker_current(void){ return 0; }

int backend_glq_ok(void){ return 0; }   /* GL calls go through glq */

int backend_pad_family(void){ return 0; }   /* no controller probe here: the game's own button icons */
