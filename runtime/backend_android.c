/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* Android backend. NativeActivity (no java code), EGL/GLES 3.2, touch overlay, gamepads,
   game data either in the apk or on storage. */
#include "rt.h"
#include <EGL/egl.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <math.h>

static ANativeActivity* g_act;
static ANativeWindow* g_win; static int g_win_gen;           /* bumped whenever we get a new ANativeWindow */
static AInputQueue* g_queue; static AInputQueue* g_queue_att; static int g_queue_dying;
static pthread_mutex_t g_mx=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv=PTHREAD_COND_INITIALIZER;
static int g_started;
static char g_ext[1024], g_name[128]="BowsersFury";
extern int g_overlay_visible;
extern void overlay_touch(int id, int action, float x, float y);
extern void overlay_state(u64* b, i32* lx, i32* ly, i32* rx, i32* ry);
extern void overlay_message(const char* m);
extern void overlay_set_opacity(float o);
extern void overlay_draw(int w, int h);

void android_log(const char* s){ __android_log_write(ANDROID_LOG_INFO,"SwitchRecomp",s); }

/* ---------------- ini */
/* [Input] mouse_buttons: the game mouse (free aim, buttons) is the Windows backend's only; nothing to re-parse here */
void mouse_map_reset(void){}
void backend_ini(const char* k, const char* v){
  if(!strcmp(k,"touch_controls_opacity")) overlay_set_opacity((float)atof(v));
  else if(!strcmp(k,"touch_controls")) g_overlay_visible=atoi(v); }

int video_has_gl(void){ return 1; }

void video_init(const char* title){ (void)title; extern void gpu_gl_start(void); gpu_gl_start(); }

void video_present_rgba(const u32* px, int w, int h, int stride){ extern void gpu_gl_present_cpu(const u32*,int,int,int); gpu_gl_present_cpu(px,w,h,stride); }

void frontend_frame(void){}

/* ---------------- EGL, GPU thread */
static EGLDisplay edpy=EGL_NO_DISPLAY; static EGLContext ectx=EGL_NO_CONTEXT; static EGLSurface esurf=EGL_NO_SURFACE, epbuf=EGL_NO_SURFACE; static EGLConfig ecfg;
static int e_gen=-1; static int g_sw=1280, g_sh=720;

static int make_surface(void){
  /* g_mx held */
  if(esurf!=EGL_NO_SURFACE){ eglMakeCurrent(edpy,epbuf,epbuf,ectx); eglDestroySurface(edpy,esurf); esurf=EGL_NO_SURFACE; }
  e_gen=g_win_gen;
  if(!g_win) return 0;
  EGLint fmt=0; eglGetConfigAttrib(edpy,ecfg,EGL_NATIVE_VISUAL_ID,&fmt); ANativeWindow_setBuffersGeometry(g_win,0,0,fmt);
  esurf=eglCreateWindowSurface(edpy,ecfg,(EGLNativeWindowType)g_win,NULL);
  if(esurf==EGL_NO_SURFACE){ WARN("EGL: could not create the surface (%x)\n",eglGetError()); eglMakeCurrent(edpy,epbuf,epbuf,ectx); return 0; }
  eglMakeCurrent(edpy,esurf,esurf,ectx);
  extern int g_opt_vsync; eglSwapInterval(edpy,g_opt_vsync? 1 : 0);
  eglQuerySurface(edpy,esurf,EGL_WIDTH,&g_sw); eglQuerySurface(edpy,esurf,EGL_HEIGHT,&g_sh);
  LOG("Android: surface %dx%d\n",g_sw,g_sh);
  return 1; }

int backend_gl_create(void){
  pthread_mutex_lock(&g_mx); while(!g_win) pthread_cond_wait(&g_cv,&g_mx);
  edpy=eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(edpy,NULL,NULL);
  static const EGLint att[]={EGL_RENDERABLE_TYPE,0x40 /*ES3*/,EGL_SURFACE_TYPE,EGL_WINDOW_BIT|EGL_PBUFFER_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
  EGLint n=0; if(!eglChooseConfig(edpy,att,&ecfg,1,&n) || !n){ pthread_mutex_unlock(&g_mx); WARN("EGL: no ES3 config\n"); return 0; }
  eglBindAPI(EGL_OPENGL_ES_API);
  static const EGLint ca[]={EGL_CONTEXT_MAJOR_VERSION,3,EGL_CONTEXT_MINOR_VERSION,2,EGL_NONE};
  ectx=eglCreateContext(edpy,ecfg,EGL_NO_CONTEXT,ca);
  if(ectx==EGL_NO_CONTEXT){ pthread_mutex_unlock(&g_mx); WARN("EGL: the GPU does not support OpenGL ES 3.2\n"); return 0; }
  static const EGLint pb[]={EGL_WIDTH,16,EGL_HEIGHT,16,EGL_NONE}; epbuf=eglCreatePbufferSurface(edpy,ecfg,pb);
  eglMakeCurrent(edpy,epbuf,epbuf,ectx);
  make_surface();
  pthread_mutex_unlock(&g_mx);
  return 1; }

void* backend_gl_proc(const char* n){ return (void*)eglGetProcAddress(n); }

void backend_gl_swap(void){
  pthread_mutex_lock(&g_mx);
  if(e_gen!=g_win_gen){ make_surface(); pthread_cond_broadcast(&g_cv); }
  if(esurf!=EGL_NO_SURFACE){
    EGLint w=0, h=0; eglQuerySurface(edpy,esurf,EGL_WIDTH,&w); eglQuerySurface(edpy,esurf,EGL_HEIGHT,&h); if(w>0 && h>0){ g_sw=w; g_sh=h; }
    overlay_draw(g_sw,g_sh);
    if(!eglSwapBuffers(edpy,esurf)){ EGLint e=eglGetError(); if(e==EGL_BAD_SURFACE || e==EGL_BAD_NATIVE_WINDOW){ eglMakeCurrent(edpy,epbuf,epbuf,ectx); eglDestroySurface(edpy,esurf); esurf=EGL_NO_SURFACE; } } }
  pthread_mutex_unlock(&g_mx); }

void backend_window_size(int* w, int* h){ *w=g_sw; *h=g_sh; }

/* ---------------- input */
static u64 g_keys; static float g_ax[4]; static float g_hat[2]; static float g_trig[2];

static u64 keybit(int kc){
  switch(kc){
    case AKEYCODE_BUTTON_A: return 1ull<<1;   /* by position: bottom = B */
    case AKEYCODE_BUTTON_B: return 1ull<<0;   /* right = A */
    case AKEYCODE_BUTTON_X: return 1ull<<3;   /* left = Y */
    case AKEYCODE_BUTTON_Y: return 1ull<<2;   /* top = X */
    case AKEYCODE_BUTTON_L1: return 1ull<<6; case AKEYCODE_BUTTON_R1: return 1ull<<7;
    case AKEYCODE_BUTTON_L2: return 1ull<<8; case AKEYCODE_BUTTON_R2: return 1ull<<9;
    case AKEYCODE_BUTTON_THUMBL: return 1ull<<4; case AKEYCODE_BUTTON_THUMBR: return 1ull<<5;
    case AKEYCODE_BUTTON_START: return 1ull<<10; case AKEYCODE_BUTTON_SELECT: return 1ull<<11;
    case AKEYCODE_DPAD_LEFT: return 1ull<<12; case AKEYCODE_DPAD_UP: return 1ull<<13; case AKEYCODE_DPAD_RIGHT: return 1ull<<14; case AKEYCODE_DPAD_DOWN: return 1ull<<15;
  }
  return 0; }

static int handle_event(AInputEvent* ev){
  int type=AInputEvent_getType(ev); int src=AInputEvent_getSource(ev);
  if(type==AINPUT_EVENT_TYPE_KEY){
    int kc=AKeyEvent_getKeyCode(ev), act=AKeyEvent_getAction(ev);
    if(kc==AKEYCODE_BACK) return 1;                         /* swallow BACK */
    u64 b=keybit(kc); if(!b) return 0;
    if(act==AKEY_EVENT_ACTION_DOWN){ g_keys|=b; g_overlay_visible=0; } else if(act==AKEY_EVENT_ACTION_UP) g_keys&=~b;
    return 1; }
  if(type==AINPUT_EVENT_TYPE_MOTION){
    if((src&AINPUT_SOURCE_JOYSTICK)==AINPUT_SOURCE_JOYSTICK || (src&AINPUT_SOURCE_GAMEPAD)==AINPUT_SOURCE_GAMEPAD){
      g_ax[0]=AMotionEvent_getAxisValue(ev,AMOTION_EVENT_AXIS_X,0); g_ax[1]=AMotionEvent_getAxisValue(ev,AMOTION_EVENT_AXIS_Y,0);
      g_ax[2]=AMotionEvent_getAxisValue(ev,AMOTION_EVENT_AXIS_Z,0); g_ax[3]=AMotionEvent_getAxisValue(ev,AMOTION_EVENT_AXIS_RZ,0);
      g_hat[0]=AMotionEvent_getAxisValue(ev,AMOTION_EVENT_AXIS_HAT_X,0); g_hat[1]=AMotionEvent_getAxisValue(ev,AMOTION_EVENT_AXIS_HAT_Y,0);
      float lt=AMotionEvent_getAxisValue(ev,AMOTION_EVENT_AXIS_LTRIGGER,0), rt=AMotionEvent_getAxisValue(ev,AMOTION_EVENT_AXIS_RTRIGGER,0);
      float br=AMotionEvent_getAxisValue(ev,AMOTION_EVENT_AXIS_BRAKE,0), gs=AMotionEvent_getAxisValue(ev,AMOTION_EVENT_AXIS_GAS,0);
      g_trig[0]= lt>br? lt : br; g_trig[1]= rt>gs? rt : gs;
      if(fabsf(g_ax[0])>0.3f||fabsf(g_ax[1])>0.3f||fabsf(g_ax[2])>0.3f||fabsf(g_ax[3])>0.3f) g_overlay_visible=0;
      return 1; }
    int a=AMotionEvent_getAction(ev), am=a&AMOTION_EVENT_ACTION_MASK, pi=(a&AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)>>AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    size_t n=AMotionEvent_getPointerCount(ev);
    switch(am){
      case AMOTION_EVENT_ACTION_DOWN: case AMOTION_EVENT_ACTION_POINTER_DOWN:
        overlay_touch(AMotionEvent_getPointerId(ev,pi),0,AMotionEvent_getX(ev,pi),AMotionEvent_getY(ev,pi)); break;
      case AMOTION_EVENT_ACTION_MOVE:
        for(size_t i=0;i<n;i++) overlay_touch(AMotionEvent_getPointerId(ev,i),1,AMotionEvent_getX(ev,i),AMotionEvent_getY(ev,i)); break;
      case AMOTION_EVENT_ACTION_UP: case AMOTION_EVENT_ACTION_POINTER_UP:
        overlay_touch(AMotionEvent_getPointerId(ev,pi),2,AMotionEvent_getX(ev,pi),AMotionEvent_getY(ev,pi)); break;
      case AMOTION_EVENT_ACTION_CANCEL: overlay_touch(0,3,0,0); break;
    }
    return 1; }
  return 0; }

static i32 axis(float v){ if(fabsf(v)<0.12f) return 0; if(v>1) v=1; if(v<-1) v=-1; return (i32)(v*32767); }

void video_poll(void){
  pthread_mutex_lock(&g_mx);
  if(g_queue_att && (g_queue_att!=g_queue || g_queue_dying)){ AInputQueue_detachLooper(g_queue_att); g_queue_att=NULL; g_queue_dying=0; pthread_cond_broadcast(&g_cv); }
  if(g_queue && !g_queue_att && !g_queue_dying){ ALooper* lp=ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS); AInputQueue_attachLooper(g_queue,lp,1,NULL,NULL); g_queue_att=g_queue; }
  AInputQueue* q=g_queue_att;
  pthread_mutex_unlock(&g_mx);
  if(q){ int dummy; void* data; while(ALooper_pollAll(0,NULL,&dummy,&data)>=0){}
    AInputEvent* ev=NULL; while(AInputQueue_getEvent(q,&ev)>=0){ if(AInputQueue_preDispatchEvent(q,ev)) continue; int h=handle_event(ev); AInputQueue_finishEvent(q,ev,h); } }
  u64 b=g_keys;
  if(g_hat[0]<-0.5f) b|=1ull<<12; if(g_hat[0]>0.5f) b|=1ull<<14; if(g_hat[1]<-0.5f) b|=1ull<<13; if(g_hat[1]>0.5f) b|=1ull<<15;
  if(g_trig[0]>0.5f) b|=1ull<<8; if(g_trig[1]>0.5f) b|=1ull<<9;
  i32 lx=axis(g_ax[0]), ly=-axis(g_ax[1]), rx=axis(g_ax[2]), ry=-axis(g_ax[3]);
  overlay_state(&b,&lx,&ly,&rx,&ry);
  g_pad.buttons=b; g_pad.lx=lx; g_pad.ly=ly; g_pad.rx=rx; g_pad.ry=ry;
}

/* ---------------- apk assets */
static void mkdirs(const char* p){ char t[1200]; strncpy(t,p,sizeof t-1); t[sizeof t-1]=0; for(char* q=t+1;*q;q++) if(*q=='/'){ *q=0; mkdir(t,0755); *q='/'; } mkdir(t,0755); }

static char* asset_text(const char* name){ AAsset* a=AAssetManager_open(g_act->assetManager,name,AASSET_MODE_BUFFER); if(!a) return NULL;
  off_t n=AAsset_getLength(a); char* s=malloc(n+1); AAsset_read(a,s,n); s[n]=0; AAsset_close(a); char* e=strpbrk(s,"\r\n"); if(e) *e=0; return s; }

static void extract_dir(const char* adir, const char* dst){
  AAssetDir* d=AAssetManager_openDir(g_act->assetManager,adir); if(!d) return; mkdirs(dst);
  const char* fnm; while((fnm=AAssetDir_getNextFileName(d))){
    char ap[512], dp[1400]; snprintf(ap,sizeof ap,"%s/%s",adir,fnm); snprintf(dp,sizeof dp,"%s/%s",dst,fnm);
    AAsset* a=AAssetManager_open(g_act->assetManager,ap,AASSET_MODE_STREAMING); if(!a) continue;
    off_t n=AAsset_getLength(a); struct stat st; if(!stat(dp,&st) && st.st_size==n){ AAsset_close(a); continue; }
    FILE* f=fopen(dp,"wb"); if(f){ char buf[65536]; int r; while((r=AAsset_read(a,buf,sizeof buf))>0) fwrite(buf,1,r,f); fclose(f); }
    AAsset_close(a); }
  AAssetDir_close(d); }
/* romfs in the apk as romfs/000, 001... stored uncompressed so we can use the fds directly */
extern int romfs_add_chunk(int fd, u64 base, u64 len);

static int open_romfs_assets(void){
  int n=0;
  for(int i=0;i<64;i++){ char nm[64]; snprintf(nm,sizeof nm,"romfs/%03d",i); AAsset* a=AAssetManager_open(g_act->assetManager,nm,AASSET_MODE_RANDOM); if(!a) break;
    off64_t start=0, len=0; int fd=AAsset_openFileDescriptor64(a,&start,&len);
    if(fd<0){ LOG("Android: %s is compressed inside the APK (cannot be read directly)\n",nm); AAsset_close(a); break; }
    romfs_add_chunk(fd,(u64)start,(u64)len); n++; /* intentionally never closed */ }
  return n; }

/* ---------------- storage permission (JNI, UI thread) */
static int has_all_files_access(JNIEnv* env){
  jclass env_c=(*env)->FindClass(env,"android/os/Environment"); if(!env_c){ (*env)->ExceptionClear(env); return 0; }
  jmethodID m=(*env)->GetStaticMethodID(env,env_c,"isExternalStorageManager","()Z"); if(!m){ (*env)->ExceptionClear(env); return 1; }  /* Android < 11 */
  return (*env)->CallStaticBooleanMethod(env,env_c,m); }

static void request_all_files(JNIEnv* env){
  jclass ic=(*env)->FindClass(env,"android/content/Intent"); jclass uc=(*env)->FindClass(env,"android/net/Uri");
  if(!ic||!uc){ (*env)->ExceptionClear(env); return; }
  jmethodID ictor=(*env)->GetMethodID(env,ic,"<init>","(Ljava/lang/String;Landroid/net/Uri;)V");
  jmethodID parse=(*env)->GetStaticMethodID(env,uc,"parse","(Ljava/lang/String;)Landroid/net/Uri;");
  char pk[300]; jclass ac=(*env)->GetObjectClass(env,g_act->clazz); jmethodID gpn=(*env)->GetMethodID(env,ac,"getPackageName","()Ljava/lang/String;");
  jstring js=(jstring)(*env)->CallObjectMethod(env,g_act->clazz,gpn); const char* cs=(*env)->GetStringUTFChars(env,js,NULL); snprintf(pk,sizeof pk,"package:%s",cs); (*env)->ReleaseStringUTFChars(env,js,cs);
  jobject uri=(*env)->CallStaticObjectMethod(env,uc,parse,(*env)->NewStringUTF(env,pk));
  jobject it=(*env)->NewObject(env,ic,ictor,(*env)->NewStringUTF(env,"android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION"),uri);
  jmethodID sa=(*env)->GetMethodID(env,ac,"startActivity","(Landroid/content/Intent;)V"); (*env)->CallVoidMethod(env,g_act->clazz,sa,it);
  if((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); }

static void immersive(JNIEnv* env){
  jclass ac=(*env)->GetObjectClass(env,g_act->clazz); jmethodID gw=(*env)->GetMethodID(env,ac,"getWindow","()Landroid/view/Window;");
  jobject w=(*env)->CallObjectMethod(env,g_act->clazz,gw); if(!w){ (*env)->ExceptionClear(env); return; }
  jclass wc=(*env)->GetObjectClass(env,w); jmethodID gd=(*env)->GetMethodID(env,wc,"getDecorView","()Landroid/view/View;");
  jobject v=(*env)->CallObjectMethod(env,w,gd); if(!v){ (*env)->ExceptionClear(env); return; }
  jclass vc=(*env)->GetObjectClass(env,v); jmethodID ss=(*env)->GetMethodID(env,vc,"setSystemUiVisibility","(I)V");
  (*env)->CallVoidMethod(env,v,ss,0x1000|0x4|0x2|0x400|0x200|0x100);
  if((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); }

/* ---------------- game thread */
static void* game_thread(void* arg){ (void)arg;
  char argv0[1400]; snprintf(argv0,sizeof argv0,"%s/%s",g_ext,g_name); char* argv[2]={argv0,NULL};
  extern int swr_main(int,char**); swr_main(1,argv); return NULL; }

static int dir_exists(const char* p){ struct stat s; return !stat(p,&s) && S_ISDIR(s.st_mode); }
extern char g_romfs_dir_override[1024];

/* ---------------- activity callbacks */
static void onWindowCreated(ANativeActivity* a, ANativeWindow* w){ (void)a; pthread_mutex_lock(&g_mx); g_win=w; g_win_gen++; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mx); }

static void onWindowDestroyed(ANativeActivity* a, ANativeWindow* w){ (void)a; (void)w;
  pthread_mutex_lock(&g_mx); g_win=NULL; g_win_gen++;
  /* give the GPU thread up to 1s to let go of the surface */
  struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); ts.tv_sec+=1;
  while(e_gen!=g_win_gen && esurf!=EGL_NO_SURFACE){ if(pthread_cond_timedwait(&g_cv,&g_mx,&ts)) break; }
  pthread_mutex_unlock(&g_mx); }

static void onQueueCreated(ANativeActivity* a, AInputQueue* q){ (void)a; pthread_mutex_lock(&g_mx); g_queue=q; pthread_mutex_unlock(&g_mx); }

static void onQueueDestroyed(ANativeActivity* a, AInputQueue* q){ (void)a;
  pthread_mutex_lock(&g_mx); if(g_queue==q) g_queue=NULL;
  if(g_queue_att==q){ g_queue_dying=1; struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); ts.tv_sec+=1;
    while(g_queue_att==q){ if(pthread_cond_timedwait(&g_cv,&g_mx,&ts)) break; } }
  pthread_mutex_unlock(&g_mx); }

static void onFocus(ANativeActivity* a, int f){ if(f) immersive(a->env); }

static void onDestroy(ANativeActivity* a){ (void)a; LOG("Android: activity is closing\n"); log_flush(); fflush(NULL); _exit(0); }

JNIEXPORT void ANativeActivity_onCreate(ANativeActivity* a, void* saved, size_t saved_size){
  (void)saved; (void)saved_size;
  g_act=a;
  a->callbacks->onNativeWindowCreated=onWindowCreated; a->callbacks->onNativeWindowDestroyed=onWindowDestroyed;
  a->callbacks->onInputQueueCreated=onQueueCreated; a->callbacks->onInputQueueDestroyed=onQueueDestroyed;
  a->callbacks->onWindowFocusChanged=onFocus; a->callbacks->onDestroy=onDestroy;
  ANativeActivity_setWindowFlags(a,0x80 /*KEEP_SCREEN_ON*/|0x400 /*FULLSCREEN*/,0);
  immersive(a->env);
  if(g_started) return;      /* activity recreated, game is still running */
  g_started=1;
  { /* game name asset; APKs packaged by older versions call it nombre.txt */
    char* nm=asset_text("switchrecomp/name.txt"); if(!nm) nm=asset_text("switchrecomp/nombre.txt");
    if(nm){ strncpy(g_name,nm,sizeof g_name-1); free(nm); } }
  const char* base= a->externalDataPath? a->externalDataPath : a->internalDataPath;
  strncpy(g_ext,base,sizeof g_ext-1); mkdirs(g_ext);
  /* game binaries + default ini from the apk */
  char dd[1400]; snprintf(dd,sizeof dd,"%s/%s_data/exefs",g_ext,g_name); extract_dir("exefs",dd);
  { char ini[1400]; snprintf(ini,sizeof ini,"%s/%s.ini",g_ext,g_name); struct stat st; if(stat(ini,&st)){ AAsset* x=AAssetManager_open(a->assetManager,"switchrecomp/game.ini",AASSET_MODE_BUFFER);
      if(x){ FILE* f=fopen(ini,"wb"); if(f){ fwrite(AAsset_getBuffer(x),1,AAsset_getLength(x),f); fclose(f); } AAsset_close(x); } } }
  /* romfs lookup order: apk, app dir, /sdcard/SwitchRecomp/<game>_data/romfs */
  int chunks=open_romfs_assets();
  char r2[1400], r3[1400]; snprintf(r2,sizeof r2,"%s/%s_data/romfs",g_ext,g_name); snprintf(r3,sizeof r3,"/storage/emulated/0/SwitchRecomp/%s_data/romfs",g_name);
  if(!chunks && !dir_exists(r2)){
    if(dir_exists(r3) && access(r3,R_OK|X_OK)==0){ strncpy(g_romfs_dir_override,r3,sizeof g_romfs_dir_override-1); }
    else {
      char m[1024]; snprintf(m,sizeof m,"Game data is missing (romfs folder).\nCopy it from the PC (USB cable) to:\nAndroid/data/%s/files/%s_data/romfs\nor to: SwitchRecomp/%s_data/romfs\n(for the latter, enable the files permission)",
        "com.switchrecomp.bowsersfury",g_name,g_name);
      overlay_message(m);
      if(!has_all_files_access(a->env)) request_all_files(a->env);
    }
  }
  pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,8u<<20); pthread_create(&t,&at,game_thread,NULL);
}
/* bionic on arm64 wants the TLS segment 64-aligned */
__thread __attribute__((aligned(64))) char swr_tls_align64;

int backend_gl_worker_current(void){ return 0; }

int backend_glq_ok(void){ return 0; }   /* GL calls go through glq */

int backend_pad_family(void){ return 0; }   /* no controller probe here: the game's own button icons */
