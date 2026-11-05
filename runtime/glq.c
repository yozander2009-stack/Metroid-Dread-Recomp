/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* GL call queue.
   The GPU thread records GL calls into a ring (pointer args get copied) and a dedicated thread replays them.
   Main reason is Proton: every GL call crosses Wine, which ate ~26% of the GPU thread on the Deck, and
   Mesa's glthread can't help since it sits below Wine.
   Calls that return something (glGet*, glGen*, glMapBuffer...) sync with the GL thread.
   glFenceSync is virtualized, the GL thread owns the real fences.
   SWR_NO_GLQ=1 turns it off. */
#include "glq_int.h"
#include "glq.h"
__thread int t_glq_direct;
extern void* backend_gl_proc(const char*); extern int backend_gl_create(void); extern void backend_gl_swap(void); extern int backend_gl_worker_current(void);
extern int backend_glq_ok(void);

#define Q_CAP (64u<<20)
#define Q_BIG (8u<<20)
typedef struct { u32 size, big; void (*ex)(void*); void* data; } QHdr;   /* record size in the ring, bytes */
static u8* g_q; static volatile u64 g_qh, g_qt;   /* head = producer, tail = consumer */
static u64 g_qh_pend; static int g_active; static volatile int g_ready;
static volatile int g_sleeping; static PMutex g_qm; static PCond g_qc;
static PCond g_swap_cv;
static volatile u32 g_lock;
static volatile u64 g_swaps_sent, g_swaps_done;
#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __builtin_ia32_pause()
#else
#define cpu_relax() __asm__ volatile("yield")
#endif
/* ini sleep_waits (default 1, from SwitchRecomp v185): threads waiting on the GL thread sleep on a condition
   variable after a short spin instead of yield-looping. SwitchToThread returns at once when nothing else is ready
   on that core, so a yield loop burns a whole core while it waits; on 2-4 core CPUs that is the core the driver
   and the game need. SWR_SPIN_WAITS=1 restores the old loops (A/B) */
static int q_sleepw(void){ static int v=-1; if(v<0){ extern int g_opt_sleep_waits; v= getenv("SWR_SPIN_WAITS")? 0 : g_opt_sleep_waits!=0; } return v; }
static PMutex g_dm; static PCond g_dc; static volatile int g_dwaiters;
static void q_done_signal(void){
  if(__atomic_load_n(&g_dwaiters,__ATOMIC_SEQ_CST)){ pm_lock(&g_dm); pc_broadcast(&g_dc); pm_unlock(&g_dm); } }
static void q_wait(int (*ready)(void*), void* a){
  for(int i=0;i<100;i++){ if(ready(a)) return; cpu_relax(); }
  pm_lock(&g_dm); __atomic_add_fetch(&g_dwaiters,1,__ATOMIC_SEQ_CST);
  while(!ready(a)) pc_wait(&g_dc,&g_dm,2000000);   /* 2 ms cap, in case a signal is ever missed */
  __atomic_sub_fetch(&g_dwaiters,1,__ATOMIC_SEQ_CST); pm_unlock(&g_dm); }

static void q_wrap_ex(void* p){ (void)p; }

static void q_lock(void){ while(__atomic_exchange_n(&g_lock,1,__ATOMIC_ACQUIRE)){ while(__atomic_load_n(&g_lock,__ATOMIC_RELAXED)) ; } }

static void q_unlock(void){ __atomic_store_n(&g_lock,0,__ATOMIC_RELEASE); }

int glq_active(void){ return g_active; }

/* ---------- producer */
static u8* q_reserve(u32 total){
  for(int spins=0;;spins++){
    u64 h=g_qh, t=__atomic_load_n(&g_qt,__ATOMIC_ACQUIRE); u64 pos=h%Q_CAP;
    u64 need= (pos+total>Q_CAP)? (Q_CAP-pos)+total : total;
    if(h+need-t<=Q_CAP){
      if(pos+total>Q_CAP){ QHdr* w=(QHdr*)(g_q+pos); w->size=(u32)(Q_CAP-pos); w->big=0; w->ex=q_wrap_ex; w->data=NULL; h+=Q_CAP-pos; pos=0; }
      g_qh_pend=h+total; return g_q+pos; }
    if(spins>64){ if(q_sleepw()) plat_sleep_ns(200000); else plat_yield(); } else if(q_sleepw()) cpu_relax(); }   /* queue full: the GL thread needs the CPU to drain it */
}

void* glq_alloc(size_t sz, size_t extra, void (*ex)(void*)){
  q_lock();
  size_t pay=q_al16(sz)+q_al16(extra);
  if(pay>Q_BIG){ QHdr* hd=(QHdr*)q_reserve((u32)sizeof(QHdr)); u8* d=malloc(pay); hd->size=sizeof(QHdr); hd->big=1; hd->ex=ex; hd->data=d; return d; }
  u32 total=(u32)(sizeof(QHdr)+pay); QHdr* hd=(QHdr*)q_reserve(total); hd->size=total; hd->big=0; hd->ex=ex; hd->data=(u8*)(hd+1); return hd->data; }

static void q_wake(void){ if(__atomic_load_n(&g_sleeping,__ATOMIC_ACQUIRE)){ pm_lock(&g_qm); pc_signal(&g_qc); pm_unlock(&g_qm); } }

void glq_commit(void){ __atomic_store_n(&g_qh,g_qh_pend,__ATOMIC_RELEASE); q_unlock(); q_wake(); }

typedef struct { void (*ex)(void*); void* arg; volatile int done; } QSync;

static void q_sync_ex(void* p){ QSync* s=*(QSync**)p; s->ex(s->arg); __atomic_store_n(&s->done,1,__ATOMIC_SEQ_CST); q_done_signal(); }
static int q_sync_ready(void* p){ return __atomic_load_n(&((QSync*)p)->done,__ATOMIC_SEQ_CST); }

void glq_sync(void (*ex)(void*), void* arg){
  if(t_glq_direct || !g_active){ ex(arg); return; }
  QSync s={ex,arg,0}; QSync** p=(QSync**)glq_alloc(sizeof(QSync*),0,q_sync_ex); *p=&s; glq_commit();
  if(q_sleepw()){ q_wait(q_sync_ready,&s); return; }
  for(int i=0;!__atomic_load_n(&s.done,__ATOMIC_ACQUIRE);i++){ if(i>200) plat_yield(); } }

static void q_nop(void* arg){ (void)arg; }

void glq_finish_all(void){ if(g_active && !t_glq_direct) glq_sync(q_nop,NULL); }

/* ---------- unpack state, needed to know how many bytes to copy */
static GLint g_un_align=4, g_un_row=0, g_un_imgh=0, g_un_skp=0, g_un_skr=0, g_un_ski=0; static GLuint g_un_pbo;

int q_unpack_pbo(void){ return g_un_pbo!=0; }

static size_t q_px(GLenum f, GLenum t){
  int c=4;
  switch(f){ case GL_RED: case GL_RED_INTEGER: case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX: case GL_GREEN: case GL_BLUE: c=1; break;
    case GL_RG: case GL_RG_INTEGER: c=2; break; case GL_RGB: case GL_BGR: case GL_RGB_INTEGER: case GL_BGR_INTEGER: c=3; break;
    case GL_DEPTH_STENCIL: c=1; break; default: c=4; }
  switch(t){ case GL_UNSIGNED_BYTE: case GL_BYTE: return c; case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: return 2*c;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4*c;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
    case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV: return 2;
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: return 1;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
    default: return 4; } }   /* 8_8_8_8, 10_10_10_2, 2_10_10_10_REV, 24_8, 10F_11F_11F_REV, 5_9_9_9_REV */
size_t q_texel_size(GLenum f, GLenum t){ return q_px(f,t); }

size_t q_img_size(GLsizei w, GLsizei h, GLsizei d, GLenum f, GLenum t){
  if(w<=0 || h<=0 || d<=0) return 0;
  size_t px=q_px(f,t), rl=(size_t)(g_un_row>0? g_un_row : w), al=(size_t)(g_un_align>0? g_un_align : 1);
  size_t row=(rl*px+al-1)/al*al, ih=(size_t)(g_un_imgh>0? g_un_imgh : h);
  return ((size_t)g_un_ski*ih + (size_t)g_un_skr)*row + (size_t)g_un_skp*px + ((size_t)(d-1)*ih + (size_t)(h-1))*row + (size_t)w*px; }

/* ---------- hand-written wrappers (glq_gen.c has the r_ pointers) */
typedef void (APIENTRY *T_ps)(GLenum,GLint); extern T_ps r_glPixelStorei;
typedef void (APIENTRY *T_bb)(GLenum,GLuint); extern T_bb r_glBindBuffer;
typedef GLsync (APIENTRY *T_fs)(GLenum,GLbitfield); extern T_fs r_glFenceSync;
typedef GLenum (APIENTRY *T_cws)(GLsync,GLbitfield,GLuint64); extern T_cws r_glClientWaitSync;
typedef void (APIENTRY *T_ds)(GLsync); extern T_ds r_glDeleteSync;
typedef struct { GLenum a; GLint b; } A2;

static void e_ps(void* p){ A2* a=p; r_glPixelStorei(a->a,a->b); }

void APIENTRY q_glPixelStorei(GLenum pname, GLint param){
  if(t_glq_direct){ r_glPixelStorei(pname,param); return; }
  switch(pname){ case GL_UNPACK_ALIGNMENT: g_un_align=param; break; case GL_UNPACK_ROW_LENGTH: g_un_row=param; break; case GL_UNPACK_IMAGE_HEIGHT: g_un_imgh=param; break;
    case GL_UNPACK_SKIP_PIXELS: g_un_skp=param; break; case GL_UNPACK_SKIP_ROWS: g_un_skr=param; break; case GL_UNPACK_SKIP_IMAGES: g_un_ski=param; break; }
  A2* a=glq_alloc(sizeof *a,0,e_ps); a->a=pname; a->b=param; glq_commit(); }

static void e_bb(void* p){ A2* a=p; r_glBindBuffer(a->a,(GLuint)a->b); }

void APIENTRY q_glBindBuffer(GLenum target, GLuint buffer){
  if(t_glq_direct){ r_glBindBuffer(target,buffer); return; }
  if(target==GL_PIXEL_UNPACK_BUFFER) g_un_pbo=buffer;
  A2* a=glq_alloc(sizeof *a,0,e_bb); a->a=target; a->b=(GLint)buffer; glq_commit(); }
/* virtual fences */
typedef struct VF { GLsync real; volatile int sig; struct VF* next; } VF;
static VF* g_vf_list;   /* GL thread side: fences still pending */
typedef struct { VF* v; GLenum c; GLbitfield f; } AF;

static void e_fs(void* p){ AF* a=p; a->v->real=r_glFenceSync(a->c,a->f); a->v->next=g_vf_list; g_vf_list=a->v; }

GLsync APIENTRY q_glFenceSync(GLenum c, GLbitfield f){
  VF* v=calloc(1,sizeof *v);
  if(t_glq_direct){ v->real=r_glFenceSync(c,f); return (GLsync)v; }
  AF* a=glq_alloc(sizeof *a,0,e_fs); a->v=v; a->c=c; a->f=f; glq_commit(); return (GLsync)v; }
typedef struct { VF* v; GLbitfield f; GLuint64 t; GLenum r; } AW;

static void e_cws(void* p){ AW* a=p; if(a->v->sig){ a->r=GL_ALREADY_SIGNALED; return; } a->r= a->v->real? r_glClientWaitSync(a->v->real,a->f,a->t) : GL_WAIT_FAILED;
  if(a->r==GL_ALREADY_SIGNALED || a->r==GL_CONDITION_SATISFIED) __atomic_store_n(&a->v->sig,1,__ATOMIC_RELEASE); }

GLenum APIENTRY q_glClientWaitSync(GLsync s, GLbitfield f, GLuint64 t){
  VF* v=(VF*)s; if(!v) return GL_WAIT_FAILED;
  if(__atomic_load_n(&v->sig,__ATOMIC_ACQUIRE)) return GL_ALREADY_SIGNALED;
  AW a={v,f,t,0};
  if(t_glq_direct){ e_cws(&a); return a.r; }
  if(t==0) return GL_TIMEOUT_EXPIRED;   /* GL thread polls it, don't block here */
  glq_sync(e_cws,&a); return a.r; }

static void e_ds(void* p){ VF* v=*(VF**)p; for(VF** pp=&g_vf_list;*pp;pp=&(*pp)->next) if(*pp==v){ *pp=v->next; break; } if(v->real) r_glDeleteSync(v->real); free(v); }

void APIENTRY q_glDeleteSync(GLsync s){
  VF* v=(VF*)s; if(!v) return;
  if(t_glq_direct){ if(v->real) r_glDeleteSync(v->real); free(v); return; }
  VF** p=glq_alloc(sizeof *p,0,e_ds); *p=v; glq_commit(); }

static void* volatile g_crumb_cur; static void* volatile g_crumb[16]; static volatile u32 g_crumb_n; static volatile int g_crumb_poll;
static void q_poll_fences(void){
  g_crumb_poll=1; for(VF* v=g_vf_list;v;v=v->next) if(!v->sig && v->real){ GLenum r=r_glClientWaitSync(v->real,0,0); if(r==GL_ALREADY_SIGNALED || r==GL_CONDITION_SATISFIED) __atomic_store_n(&v->sig,1,__ATOMIC_RELEASE); }
  g_crumb_poll=0; }

/* ---------- swap */
/* Bound the final precision wait to 0.5 ms; the longer portion sleeps. */
static void present_wait_deadline(u64 deadline){
  u64 now=plat_ns();if(now>=deadline)return;
  if(deadline-now>500000ull)plat_sleep_ns((i64)(deadline-now-500000ull));
  while(plat_ns()<deadline){
#if defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause");
#else
    plat_yield();
#endif
  }
}
static int q_swap_ready(void* p){ (void)p; return g_swaps_sent-__atomic_load_n(&g_swaps_done,__ATOMIC_SEQ_CST)<=1; }
static void e_swap(void* p){
  (void)p;static int configured;static FILE*trace;static u64 first,last;static u32 rows;
  if(!configured){configured=1;const char*path=getenv("SWR_TRACE_PRESENTS");if(path&&path[0]){trace=fopen(path,"w");if(trace)fprintf(trace,"swap_index,elapsed_ms,interval_ms,swap_call_ms,guest_present_count,completed_ns\n");else WARN("present trace: cannot open output\n");}}
  {extern int gpu_gl_present_pacing(void);extern int g_opt_fps;static u64 deadline;static int announced;
    if(gpu_gl_present_pacing()){
      if(!announced){announced=1;LOG("pacing: frame cap on presentation thread, guest frame reads synchronized\n");}
      extern volatile int g_pace_fps;int pf=g_pace_fps;   /* g_opt_fps, or 60 during the game's slow motion (main.c pace_update) */
      u64 now=plat_ns(),period=pf>=30&&pf<=240?1000000000ull/(u64)pf:16666667ull;   /* 30: cinematics (main.c pace_update) */
      u64 next=deadline+period;if(deadline&&now<next)present_wait_deadline(next);now=plat_ns();
      if(pf>=120) deadline= deadline&&(i64)(now-next)<(i64)(period/2)? next : now;   /* high refresh: a late frame starts a new cadence instead of being followed by short catch-up frames */
      else deadline=deadline&&now-deadline<period*2?next:now;
    }else deadline=0;
  }
  {extern int g_opt_vsync;extern void backend_gl_vsync(int);static int vs=-1;if(vs!=g_opt_vsync){vs=g_opt_vsync;backend_gl_vsync(vs);}}   /* forced_vsync applies live */
  {extern void gpu_gl_fps_overlay(void);gpu_gl_fps_overlay();}   /* [Graphics] show_fps */
  {extern void gpu_gl_skip_prompt(void);gpu_gl_skip_prompt();}   /* "Hold (-) to skip" during a skippable cutscene */
  {extern void ov_gl_frame(void);ov_gl_frame();}   /* in-game menu (dread_ui/ov.cpp) over the finished picture */
  u64 begin=trace?plat_ns():0;backend_gl_swap();
  u64 completed=__atomic_add_fetch(&g_swaps_done,1,__ATOMIC_SEQ_CST); q_done_signal();
  {pm_lock(&g_qm);pc_broadcast(&g_swap_cv);pm_unlock(&g_qm);}
  if(trace){extern int g_frames;u64 now=plat_ns();if(!first)first=now;
    fprintf(trace,"%llu,%.6f,%.6f,%.6f,%d,%llu\n",(unsigned long long)completed,(now-first)/1e6,last?(now-last)/1e6:0.0,(now-begin)/1e6,g_frames,(unsigned long long)now);
    last=now;if((++rows&255)==0)fflush(trace);
  }
}

void glq_swap(void){
  if(!g_active || t_glq_direct){ backend_gl_swap(); return; }
  g_swaps_sent++; glq_alloc(16,0,e_swap); glq_commit();
  /* max one frame ahead, more just adds input lag. At 120+ FPS allow two: one slow GPU frame (an upload burst)
     is then absorbed instead of missing the deadline, and a frame there is only 4-8 ms of extra latency */
  {extern int gpu_gl_present_pacing(void); extern int g_opt_fps;
    static int ahead_env=-1; if(ahead_env<0){ const char* e=getenv("SWR_FRAMES_AHEAD"); ahead_env= e? atoi(e) : 0; }
    u32 ahead= ahead_env>0? (u32)ahead_env : g_opt_fps>=120? 2u : 1u;
    if(gpu_gl_present_pacing()){
      pm_lock(&g_qm);while(g_swaps_sent-__atomic_load_n(&g_swaps_done,__ATOMIC_ACQUIRE)>ahead)pc_wait(&g_swap_cv,&g_qm,-1);pm_unlock(&g_qm);
    }else if(q_sleepw()) q_wait(q_swap_ready,NULL);
    else for(int i=0;g_swaps_sent-__atomic_load_n(&g_swaps_done,__ATOMIC_ACQUIRE)>1;i++){if(i>200)plat_yield();}
  } }

int glq_worker_current(void){ int r=backend_gl_worker_current(); if(r) t_glq_direct=1; return r; }

/* ---------- proc lookup */
void* glq_proc(const char* n){
  if(!g_active) return backend_gl_proc(n);
  for(const GlqEnt* e=g_glq_tab;e->name;e++) if(!strcmp(e->name,n)) return *e->real? e->q : NULL;
  { static int said; if(said<8){ said++; LOG("glq: %s is not in the queue table (called directly)\n",n); } }
  return backend_gl_proc(n); }

/* ---------- GL thread */
static int g_start_ok;
static int trace_driver; static void* trace_upload_2d; static void* trace_compressed_2d;

static void APIENTRY driver_debug_message(GLenum source,GLenum type,GLuint id,GLenum severity,GLsizei length,const GLchar*message,const void*arg){
  (void)source;(void)severity;(void)arg;static u32 count;
  if(type==0x8251)return; /* omit informational buffer-placement messages */
  if(__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)<256)LOG("GL driver debug: type=%x id=%u %.*s\n",type,id,length>1500?1500:length,message);
}
/* crash breadcrumbs (#39: AMD crashes inside the driver on the GL thread): the entry running now + the last 16 */
static const char* crumb_name(void* x, char* b, int n){ if(x==(void*)e_swap) return "SwapBuffers"; if(x==(void*)e_ps) return "glPixelStorei"; if(x==(void*)e_bb) return "glBindBuffer";
  if(x==(void*)e_fs) return "glFenceSync"; if(x==(void*)e_cws) return "glClientWaitSync"; if(x==(void*)e_ds) return "glDeleteSync";
  for(const GlqEnt* e=g_glq_tab;e->name;e++) if(e->execute==x) return e->name;
#ifdef _WIN32
  { extern char __ImageBase; snprintf(b,(size_t)n,"custom entry at exe +%llx",(unsigned long long)((u64)(size_t)x-(u64)(size_t)&__ImageBase)); return b; }
#else
  snprintf(b,(size_t)n,"custom entry %p",x); return b;
#endif
}
void glq_crash_note(void){ if(!g_active) return; char b[64];
  if(g_crumb_poll) logf_(0,"  OpenGL thread: polling fences (glClientWaitSync)\n");
  else logf_(0,"  OpenGL thread: running %s\n", g_crumb_cur? crumb_name(g_crumb_cur,b,sizeof b) : "nothing (between calls)");
  char l[1200]; size_t k=0; u32 n=g_crumb_n; for(u32 i= n>16? n-16 : 0;i<n && k<sizeof l-80;i++) k+=(size_t)snprintf(l+k,sizeof l-k," %s",crumb_name(g_crumb[i&15],b,sizeof b));
  l[k]=0; logf_(0,"  OpenGL thread, last calls (oldest first):%s\n",l); }
static u64 g_test_crash, g_test_n;
static void gl_thread(void* arg){ plat_diag_role("GL-driver"); (void)arg; trace_driver=getenv("SWR_TRACE_GL_DRIVER")!=NULL;
  { extern void plat_thread_high_priority(void); plat_thread_high_priority(); }
  t_glq_direct=1;
  { const char* tc=getenv("SWR_TEST_GL_CRASH"); if(tc) g_test_crash=strtoull(tc,NULL,10); }
  int ok= backend_gl_create() && swr_gl_load(backend_gl_proc);
  extern int swr_gl_is_gles(void);
  if(ok && swr_gl_is_gles()) ok=0;   /* not on GLES */
  if(ok){ for(const GlqEnt* e=g_glq_tab;e->name;e++){ void* r= e->pvar? *e->pvar : NULL; if(!r) r=backend_gl_proc(e->name); *e->real=r; }
    for(const GlqEnt* e=g_glq_tab;e->name;e++) if(e->pvar && *e->real) *e->pvar=e->q; }
  if(trace_driver){for(const GlqEnt*e=g_glq_tab;e->name;e++){if(!strcmp(e->name,"glTexSubImage2D"))trace_upload_2d=e->execute;if(!strcmp(e->name,"glCompressedTexSubImage2D"))trace_compressed_2d=e->execute;}}
  if(ok&&getenv("SWR_GL_DRIVER_DEBUG")){
    typedef void(APIENTRY*DebugCallback)(GLDEBUGPROC,const void*);
    DebugCallback callback=(DebugCallback)backend_gl_proc("glDebugMessageCallback");
    if(callback){p_glEnable(GL_DEBUG_OUTPUT);p_glEnable(0x8242);callback(driver_debug_message,NULL);LOG("GL driver state diagnostics enabled\n");}
  }
  g_start_ok=ok; __atomic_store_n(&g_ready,1,__ATOMIC_RELEASE);
  if(!ok) return;
  u32 n=0;
  for(;;){
    u64 h=__atomic_load_n(&g_qh,__ATOMIC_ACQUIRE), t=g_qt;
    if(t==h){
      if(g_vf_list) q_poll_fences();
      if(q_sleepw()){ extern int plat_ncpu(void); static u64 spin; if(!spin) spin= plat_ncpu()<=2? 15000 : 50000;   /* less on 2 cores, the game needs them */
        u64 t0=plat_ns(); while(__atomic_load_n(&g_qh,__ATOMIC_ACQUIRE)==t && plat_ns()-t0<spin) cpu_relax(); }   /* then sleep on g_qc (woken by glq_commit) */
      else for(int i=0;i<2000 && __atomic_load_n(&g_qh,__ATOMIC_ACQUIRE)==t;i++){ if(i>50) plat_yield(); }
      if(__atomic_load_n(&g_qh,__ATOMIC_ACQUIRE)!=t) continue;
      pm_lock(&g_qm); __atomic_store_n(&g_sleeping,1,__ATOMIC_RELEASE);
      if(__atomic_load_n(&g_qh,__ATOMIC_ACQUIRE)==t) pc_wait(&g_qc,&g_qm, g_vf_list? 500000 : 2000000);
      __atomic_store_n(&g_sleeping,0,__ATOMIC_RELEASE); pm_unlock(&g_qm); continue; }
    while(t!=h){
      QHdr* hd=(QHdr*)(g_q+t%Q_CAP); u32 sz=hd->size;
      void* executed=(void*)hd->ex;u64 operation_start=0;int upload_info[8]={0};int image_upload=0;
      if(trace_driver){if(hd->ex==q_sync_ex)executed=(void*)(*(QSync**)hd->data)->ex;if(executed==trace_upload_2d||executed==trace_compressed_2d){memcpy(upload_info,hd->data,sizeof upload_info);image_upload=1;}operation_start=plat_ns();}
      { void* cx= hd->ex==q_sync_ex? (void*)(*(QSync**)hd->data)->ex : (void*)hd->ex; g_crumb[g_crumb_n++&15]=cx; g_crumb_cur=cx; }
      if(g_test_crash && ++g_test_n==g_test_crash) *(volatile int*)(size_t)0x1001b=0;   /* SWR_TEST_GL_CRASH=N: tests the crash report */
      hd->ex(hd->data); g_crumb_cur=NULL;
      if(trace_driver){u64 elapsed=plat_ns()-operation_start;static u32 logged;if(elapsed>10000000ull&&executed!=(void*)e_swap&&logged++<256){const char*name=hd->ex==e_swap?"SwapBuffers":"queue/custom";for(const GlqEnt*entry=g_glq_tab;entry->name;entry++)if(entry->execute==executed){name=entry->name;break;}LOG("glq driver: %s %.2f ms, queued bytes=%llu begin_ns=%llu end_ns=%llu\n",name,elapsed/1e6,(unsigned long long)(__atomic_load_n(&g_qh,__ATOMIC_ACQUIRE)-t),(unsigned long long)operation_start,(unsigned long long)(operation_start+elapsed));if(image_upload)LOG("glq upload detail: level=%d %dx%d format=%x %s=%x\n",upload_info[1],upload_info[4],upload_info[5],upload_info[6],executed==trace_compressed_2d?"bytes":"type",upload_info[7]);}}
      if(hd->big) free(hd->data);
      t+=sz; __atomic_store_n(&g_qt,t,__ATOMIC_RELEASE);
      if(((++n)&255)==0 && g_vf_list) q_poll_fences(); }
  } }

int glq_start(void){
  if(getenv("SWR_NO_GLQ") || !backend_glq_ok()) return 0;
  g_q=malloc(Q_CAP+256);   /* slack so a header written at the end can't run past the buffer */ if(!g_q) return 0;
  pm_init(&g_qm); pc_init(&g_qc);pc_init(&g_swap_cv); pm_init(&g_dm); pc_init(&g_dc);
  if(plat_thread(gl_thread,NULL,8u<<20)) return 0;
  while(!__atomic_load_n(&g_ready,__ATOMIC_ACQUIRE)) plat_sleep_ns(1000000);
  if(!g_start_ok) return -1;
  g_active=1; LOG("gpu: OpenGL calls run on their own thread (the GPU thread and the driver work at the same time)\n");
  return 1; }
