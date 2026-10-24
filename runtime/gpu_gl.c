/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* GL 4.3 renderer for the Maxwell command stream. Runs on its own thread like the real GPU: the game
   submits pushbuffers, we execute them and signal syncpoints. Textures, buffers and shaders are
   translated once and cached. */
#include "gpu.h"
#include <time.h>
#include <math.h>
#include "glfn.h"
#include "shader.h"
#ifdef _WIN32
#include <windows.h>
#include <stdbool.h>
#include "renderdoc_app.h"
#endif

int g_gl=0;                     /* active renderer */
int gl_frontend_available;
static int g_gl_ready;
#include "glq.h"

/* GL calls in this file go through glq when it's on */
#define backend_gl_proc glq_proc
#define backend_gl_swap glq_swap
#define backend_gl_worker_current glq_worker_current

/* Object-name queries synchronously drain glq. Reserve names in batches so
   first-use texture/buffer setup can remain queued behind preceding work. */
typedef void (APIENTRY*NameGenerator)(GLsizei,GLuint*);
typedef struct {GLuint ids[1024];u32 left;} NamePool;
static NamePool tex_names,buffer_names,fbo_names,vao_names,sampler_names;
static u64 name_allocations,name_batches,name_wait_ns;
static void name_pool_get(NamePool*pool,NameGenerator gen,GLsizei n,GLuint*out){
  if(n<=0){gen(n,out);return;}
  if(getenv("SWR_NO_NAME_POOL")){u64 start=plat_ns();gen(n,out);name_wait_ns+=plat_ns()-start;name_batches++;name_allocations+=(u32)n;return;}
  for(GLsizei i=0;i<n;i++){if(!pool->left){u64 start=plat_ns();gen(1024,pool->ids);name_wait_ns+=plat_ns()-start;name_batches++;pool->left=1024;}out[i]=pool->ids[--pool->left];name_allocations++;}
}
static void pooled_textures(GLsizei n,GLuint*out){name_pool_get(&tex_names,p_glGenTextures,n,out);}
static void pooled_buffers(GLsizei n,GLuint*out){name_pool_get(&buffer_names,p_glGenBuffers,n,out);}
static void pooled_fbos(GLsizei n,GLuint*out){name_pool_get(&fbo_names,p_glGenFramebuffers,n,out);}
static void pooled_vaos(GLsizei n,GLuint*out){name_pool_get(&vao_names,p_glGenVertexArrays,n,out);}
static u64 sampler_query_ns,sampler_query_calls;
static void pooled_samplers(GLsizei n,GLuint*out){u64 start=plat_ns();if(getenv("SWR_NO_SAMPLER_POOL"))p_glGenSamplers(n,out);else name_pool_get(&sampler_names,p_glGenSamplers,n,out);sampler_query_ns+=plat_ns()-start;sampler_query_calls++;}
#undef glGenTextures
#undef glGenBuffers
#undef glGenFramebuffers
#undef glGenVertexArrays
#undef glGenSamplers
#define glGenTextures pooled_textures
#define glGenBuffers pooled_buffers
#define glGenFramebuffers pooled_fbos
#define glGenVertexArrays pooled_vaos
#define glGenSamplers pooled_samplers

extern int backend_gl_create(void); extern void* backend_gl_proc(const char*); extern void backend_gl_swap(void); extern void backend_window_size(int*,int*);
u64 g_draws;
static __thread int t_gpu_thread;   /* GL context owner */
static void cs_complete(int wait); static void cs_kick_async(void); static int cs_inflight(void); static void cs_submit_done(u32 id, u32 n);
int g_diag_draws; static u64 g_gpu_busy; static u64 st_tex_ok, st_tex_null, st_tex_mismatch, st_tex_rt;

u64 gpu_busy_ns(void){ return __atomic_load_n(&g_gpu_busy,__ATOMIC_RELAXED); }
extern u32 g_submit_epoch;
static u64 st_compiling, st_cb_skip, st_cb_bytes, st_vb_hashed, st_vb_up, st_tex_up; static u64 st_noshader, st_shfail, st_noprog, st_nort, st_drawn, st_clears, st_pres_rt, st_pres_cpu, st_shaders;
extern int g_opt_res;
static float g_scale=1.0f;       /* internal res scale, 0.25..4 */
static u32 SC(u32 v){ u32 r=(u32)(v*g_scale+0.5f); return r? r : 1; }
static u32 g_last_cw, g_last_ch;   /* last color target size, used to spot shadow passes */
static float fabsf_(float x){ return x<0? -x : x; }
u32 fmt_bpp_2d(u32 f);

/* ============================================================ work queue */
enum { WK_SUBMIT, WK_PRESENT_CPU, WK_PRESENT_ADDR, WK_SHOT, WK_SYNC, WK_DUMP };
typedef struct { int kind; u64* ent; u32 n, syncpt, incs; u32* px; int w,h; u64 addr; u32 fmt; PCond* done_cv; volatile int* done; char path[256]; u64 result; u64 sub_ns,queued_ns; } Work;
#define QN 1024
static int trace_work_enabled;
static Work q[QN]; static int qh, qt; static PMutex g_ql; static PCond g_qcv, g_qspace;
static PCond g_frame_consumed_cv;

static void q_push(Work* w){
  if(trace_work_enabled)w->queued_ns=plat_ns();
  pm_lock(&g_ql); while(((qh+1)%QN)==qt) pc_wait(&g_qspace,&g_ql,1000000); q[qh]=*w; qh=(qh+1)%QN; pc_signal(&g_qcv); pm_unlock(&g_ql); }

static int q_pop(Work* w, i64 timeout){
  pm_lock(&g_ql); if(qh==qt) pc_wait(&g_qcv,&g_ql,timeout); if(qh==qt){ pm_unlock(&g_ql); return 0; }
  *w=q[qt]; qt=(qt+1)%QN; pc_signal(&g_qspace); pm_unlock(&g_ql); return 1; }

/* ============================================================ GL helpers */
static GLuint compile(GLenum type, const char* src, char* err, int errn){
  GLuint s=glCreateShader(type); glShaderSource(s,1,&src,NULL); glCompileShader(s); GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
  if(!ok){ if(err) glGetShaderInfoLog(s,errn,NULL,err); glDeleteShader(s); return 0; } return s; }

static GLuint link2(GLuint a, GLuint b, GLuint c3){
  GLuint p=glCreateProgram(); if(a) glAttachShader(p,a); if(b) glAttachShader(p,b); if(c3) glAttachShader(p,c3); glLinkProgram(p);
  GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok); if(!ok){ char log[2048]; glGetProgramInfoLog(p,2048,NULL,log); WARN("GL link: %s\n",log); return 0; } return p; }

/* DLAA (dlss=1): vertex shaders expose gl_Position to transform feedback (buffer 0, 16 bytes a vertex), so the scene's
   depth-prepass draws can be captured and replayed for motion vectors. Only with DLAA on: the default build's shader
   text, and so its cached binaries, stay as they were (the binary cache is keyed by the text) */
extern int snxvk_dlss_mode;
static char* dlg_vs_xfb(char* src){
  static const char* decl="out gl_PerVertex { invariant vec4 gl_Position;";
#ifdef SWR_RELEASE
  return src;
#endif
  if(snxvk_dlss_mode<=0 || !src) return src;
  char* d=strstr(src,decl); char* nl=strchr(src,'\n'); if(!d || !nl || nl>d) return src;
  static const char* ext="#extension GL_ARB_enhanced_layouts : require\n";
  static const char* nd="out gl_PerVertex { layout(xfb_buffer=0, xfb_offset=0) invariant vec4 gl_Position;";
  size_t n=strlen(src)+strlen(ext)+strlen(nd)+1; char* o=malloc(n); if(!o) return src;
  size_t a=(size_t)(nl+1-src), b=(size_t)(d-src);
  memcpy(o,src,a); strcpy(o+a,ext); memcpy(o+a+strlen(ext),src+a,b-a); strcpy(o+a+strlen(ext)+(b-a),nd); strcat(o,d+strlen(decl));
  free(src); return o; }

/* ============================================================ formats */
typedef struct { GLenum ifmt, fmt, type; u8 bpp, bw, bh, compressed, depth, is_int, astc, dec; } GlFmt;
/* GLES: anything the hw can't sample gets decoded on the CPU */
int g_gles; static int g_has_astc, g_has_s3tc, g_has_rgtc, g_has_bptc;
extern int swr_bc_decode(int kind, const u8* src, u32 w, u32 h, u32* out);

static GlFmt F(GLenum i, GLenum f, GLenum t, int bpp){ GlFmt r={i,f,t,(u8)bpp,1,1,0,0,0,0,0}; r.is_int=(f==GL_RGBA_INTEGER||f==GL_RG_INTEGER||f==GL_RED_INTEGER); return r; }

static GlFmt FC(GLenum i, int bpp){ GlFmt r={i,0,0,(u8)bpp,4,4,1,0,0,0,0}; return r; }

static GlFmt FASTC(int bw, int bh){ GlFmt r={GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,16,(u8)bw,(u8)bh,1,0,0,1,0}; return r; }

static GlFmt FD(GLenum i, GLenum f, GLenum t, int bpp){ GlFmt r={i,f,t,(u8)bpp,1,1,0,1,0,0,0}; return r; }

/* TIC: format, type of the first component, sRGB */
static GlFmt tic_format(u32 w0, int srgb){
  u32 f=w0&0x7F, ct=(w0>>7)&7;
  int flt= ct==7, sn= ct==1, ui= ct==4, si= ct==3;
  GlFmt r;
  switch(f){
    case 0x01: r= flt? F(GL_RGBA32F,GL_RGBA,GL_FLOAT,16) : ui? F(GL_RGBA32UI,GL_RGBA_INTEGER,GL_UNSIGNED_INT,16) : F(GL_RGBA32I,GL_RGBA_INTEGER,GL_INT,16); break;
    case 0x02: r= F(GL_RGB32F,GL_RGB,GL_FLOAT,12); break;
    case 0x03: r= flt? F(GL_RGBA16F,GL_RGBA,GL_HALF_FLOAT,8) : sn? F(GL_RGBA16_SNORM,GL_RGBA,GL_SHORT,8) : ui? F(GL_RGBA16UI,GL_RGBA_INTEGER,GL_UNSIGNED_SHORT,8) : si? F(GL_RGBA16I,GL_RGBA_INTEGER,GL_SHORT,8) : F(GL_RGBA16,GL_RGBA,GL_UNSIGNED_SHORT,8); break;
    case 0x04: r= flt? F(GL_RG32F,GL_RG,GL_FLOAT,8) : ui? F(GL_RG32UI,GL_RG_INTEGER,GL_UNSIGNED_INT,8) : F(GL_RG32I,GL_RG_INTEGER,GL_INT,8); break;
    case 0x07: case 0x08: r= sn? F(GL_RGBA8_SNORM,GL_RGBA,GL_BYTE,4) : ui? F(GL_RGBA8UI,GL_RGBA_INTEGER,GL_UNSIGNED_BYTE,4) : si? F(GL_RGBA8I,GL_RGBA_INTEGER,GL_BYTE,4) : F(srgb? GL_SRGB8_ALPHA8 : GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,4); break;
    case 0x09: r= ui? F(GL_RGB10_A2UI,GL_RGBA_INTEGER,GL_UNSIGNED_INT_2_10_10_10_REV,4) : F(GL_RGB10_A2,GL_RGBA,GL_UNSIGNED_INT_2_10_10_10_REV,4); break;
    case 0x0C: r= flt? F(GL_RG16F,GL_RG,GL_HALF_FLOAT,4) : sn? F(GL_RG16_SNORM,GL_RG,GL_SHORT,4) : ui? F(GL_RG16UI,GL_RG_INTEGER,GL_UNSIGNED_SHORT,4) : si? F(GL_RG16I,GL_RG_INTEGER,GL_SHORT,4) : F(GL_RG16,GL_RG,GL_UNSIGNED_SHORT,4); break;
    case 0x0F: r= flt? F(GL_R32F,GL_RED,GL_FLOAT,4) : ui? F(GL_R32UI,GL_RED_INTEGER,GL_UNSIGNED_INT,4) : F(GL_R32I,GL_RED_INTEGER,GL_INT,4); break;
    case 0x12: r= F(GL_RGBA4,GL_RGBA,GL_UNSIGNED_SHORT_4_4_4_4_REV,2); break;
    case 0x13: case 0x14: r= F(GL_RGB5_A1,GL_RGBA,GL_UNSIGNED_SHORT_1_5_5_5_REV,2); break;
    case 0x15: r= F(GL_RGB565,GL_RGB,GL_UNSIGNED_SHORT_5_6_5_REV,2); break;
    case 0x16: r= F(GL_RGB565,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,2); break;
    case 0x18: r= sn? F(GL_RG8_SNORM,GL_RG,GL_BYTE,2) : ui? F(GL_RG8UI,GL_RG_INTEGER,GL_UNSIGNED_BYTE,2) : si? F(GL_RG8I,GL_RG_INTEGER,GL_BYTE,2) : F(GL_RG8,GL_RG,GL_UNSIGNED_BYTE,2); break;
    case 0x1B: r= flt? F(GL_R16F,GL_RED,GL_HALF_FLOAT,2) : sn? F(GL_R16_SNORM,GL_RED,GL_SHORT,2) : ui? F(GL_R16UI,GL_RED_INTEGER,GL_UNSIGNED_SHORT,2) : si? F(GL_R16I,GL_RED_INTEGER,GL_SHORT,2) : F(GL_R16,GL_RED,GL_UNSIGNED_SHORT,2); break;
    case 0x1D: case 0x1C: r= sn? F(GL_R8_SNORM,GL_RED,GL_BYTE,1) : ui? F(GL_R8UI,GL_RED_INTEGER,GL_UNSIGNED_BYTE,1) : si? F(GL_R8I,GL_RED_INTEGER,GL_BYTE,1) : F(GL_R8,GL_RED,GL_UNSIGNED_BYTE,1); break;
    case 0x20: r= F(GL_RGB9_E5,GL_RGB,GL_UNSIGNED_INT_5_9_9_9_REV,4); break;
    case 0x21: r= F(GL_R11F_G11F_B10F,GL_RGB,GL_UNSIGNED_INT_10F_11F_11F_REV,4); break;
    case 0x24: r= FC(srgb? 0x8C4D : 0x83F1,8); break;
    case 0x25: r= FC(srgb? 0x8C4E : 0x83F2,16); break;
    case 0x26: r= FC(srgb? 0x8C4F : 0x83F3,16); break;
    case 0x27: r= FC(sn? 0x8DBC : 0x8DBB,8); break;
    case 0x28: r= FC(sn? 0x8DBE : 0x8DBD,16); break;
    case 0x17: r= FC(srgb? 0x8E8D : 0x8E8C,16); break;
    case 0x10: r= FC(0x8E8E,16); break;
    case 0x11: r= FC(0x8E8F,16); break;
    case 0x40: r=FASTC(4,4); break; case 0x50: r=FASTC(5,4); break; case 0x41: r=FASTC(5,5); break; case 0x51: r=FASTC(6,5); break;
    case 0x42: r=FASTC(6,6); break; case 0x55: r=FASTC(8,5); break; case 0x52: r=FASTC(8,6); break; case 0x44: r=FASTC(8,8); break;
    case 0x56: r=FASTC(10,5); break; case 0x57: r=FASTC(10,6); break; case 0x53: r=FASTC(10,8); break; case 0x45: r=FASTC(10,10); break;
    case 0x54: r=FASTC(12,10); break; case 0x46: r=FASTC(12,12); break;
    case 0x29: case 0x2A: case 0x2B: r= FD(GL_DEPTH24_STENCIL8,GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,4); break;
    case 0x2F: r= FD(GL_DEPTH_COMPONENT32F,GL_DEPTH_COMPONENT,GL_FLOAT,4); break;
    case 0x30: r= FD(GL_DEPTH32F_STENCIL8,GL_DEPTH_STENCIL,GL_FLOAT_32_UNSIGNED_INT_24_8_REV,8); break;   /* ZF32_X24S8 */
    case 0x3A: r= FD(GL_DEPTH_COMPONENT16,GL_DEPTH_COMPONENT,GL_UNSIGNED_SHORT,2); break;
    case 0x19: r= FD(GL_DEPTH32F_STENCIL8,GL_DEPTH_STENCIL,GL_FLOAT_32_UNSIGNED_INT_24_8_REV,8); break;
    default: r= F(GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,4); r.bpp=0; { static u32 said[4]; int k; for(k=0;k<4&&said[k]!=f+1;k++); if(k==4){ for(k=0;k<4&&said[k];k++); if(k<4){ said[k]=f+1; WARN("gpu: unsupported texture format %x (ignored)\n",f); } } } break;
  }
  if(r.astc && srgb) r.ifmt=GL_SRGB8_ALPHA8;
  if(g_gles){
    int bc= f==0x24? 2 : f==0x25? 3 : f==0x26? 4 : f==0x27? 5 : f==0x28? 6 : f==0x17? 7 : (f==0x10||f==0x11)? 8 : 0;
    int native= (bc>=2&&bc<=4&&g_has_s3tc) || ((bc==5||bc==6)&&g_has_rgtc) || (bc>=7&&g_has_bptc);
    if(bc && !native){ r.dec=(u8)bc; r.ifmt= srgb? GL_SRGB8_ALPHA8 : GL_RGBA8; r.fmt=GL_RGBA; r.type=GL_UNSIGNED_BYTE; }
    /* GLES has no REV 16-bit packed formats, expand to RGBA8 */
    if(f==0x12||f==0x13||f==0x14||f==0x15){ r.dec= f==0x12? 10 : f==0x15? 12 : 11; r.ifmt=GL_RGBA8; r.fmt=GL_RGBA; r.type=GL_UNSIGNED_BYTE; }
    /* native ASTC on Mali/Adreno, upload compressed with all mips */
    if(r.astc && g_has_astc){ static const u8 bwh[14][2]={{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
      for(int k=0;k<14;k++) if(bwh[k][0]==r.bw && bwh[k][1]==r.bh){ r.ifmt=(srgb? 0x93D0 : 0x93B0)+k; r.astc=0; break; } }
  }
  /* desktop with hw ASTC (Intel Gen9-11 etc): upload compressed too */
  if(!g_gles && r.astc && g_has_astc && !getenv("SWR_ASTC_CPU")){ static const u8 bwh[14][2]={{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
    for(int k=0;k<14;k++) if(bwh[k][0]==r.bw && bwh[k][1]==r.bh){ r.ifmt=(srgb? 0x93D0 : 0x93B0)+k; r.astc=0; break; } }
  r.is_int= (r.fmt==GL_RGBA_INTEGER||r.fmt==GL_RG_INTEGER||r.fmt==GL_RED_INTEGER);
  return r; }
/* SNORM color targets aren't required to be renderable in GL. If the driver gets them wrong (see
   gl_selftest) store them as half float; transfers still convert to/from the guest's snorm data */
static int g_snorm_fb;

static GlFmt rt_format(u32 f){
  switch(f){
    case 0xC0: return F(GL_RGBA32F,GL_RGBA,GL_FLOAT,16);
    case 0xC2: return F(GL_RGBA32UI,GL_RGBA_INTEGER,GL_UNSIGNED_INT,16);
    case 0xC6: return F(GL_RGBA16,GL_RGBA,GL_UNSIGNED_SHORT,8);
    case 0xC7: return F(g_snorm_fb? GL_RGBA16F : GL_RGBA16_SNORM,GL_RGBA,GL_SHORT,8);
    case 0xCA: case 0xCE: return F(GL_RGBA16F,GL_RGBA,GL_HALF_FLOAT,8);
    case 0xCB: return F(GL_RG32F,GL_RG,GL_FLOAT,8);
    case 0xCD: return F(GL_RG32UI,GL_RG_INTEGER,GL_UNSIGNED_INT,8);
    case 0xCF: case 0xD5: return F(GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,4);
    case 0xD0: case 0xD6: return F(GL_SRGB8_ALPHA8,GL_RGBA,GL_UNSIGNED_BYTE,4);
    case 0xD1: return F(GL_RGB10_A2,GL_RGBA,GL_UNSIGNED_INT_2_10_10_10_REV,4);
    case 0xD7: return F(g_snorm_fb? GL_RGBA16F : GL_RGBA8_SNORM,GL_RGBA,GL_BYTE,4);
    case 0xD9: return F(GL_RGBA8UI,GL_RGBA_INTEGER,GL_UNSIGNED_BYTE,4);
    case 0xDA: return F(GL_RG16,GL_RG,GL_UNSIGNED_SHORT,4);
    case 0xDB: return F(g_snorm_fb? GL_RG16F : GL_RG16_SNORM,GL_RG,GL_SHORT,4);
    case 0xDE: return F(GL_RG16F,GL_RG,GL_HALF_FLOAT,4);
    case 0xE0: return F(GL_R11F_G11F_B10F,GL_RGB,GL_UNSIGNED_INT_10F_11F_11F_REV,4);
    case 0xE4: return F(GL_R32UI,GL_RED_INTEGER,GL_UNSIGNED_INT,4);
    case 0xE5: return F(GL_R32F,GL_RED,GL_FLOAT,4);
    case 0xE8: return F(GL_RGB565,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,2);
    case 0xE9: return F(GL_RGB5_A1,GL_RGBA,GL_UNSIGNED_SHORT_1_5_5_5_REV,2);
    case 0xEA: return F(GL_RG8,GL_RG,GL_UNSIGNED_BYTE,2);
    case 0xEB: return F(g_snorm_fb? GL_RG16F : GL_RG8_SNORM,GL_RG,GL_BYTE,2);
    case 0xEE: return F(GL_R16,GL_RED,GL_UNSIGNED_SHORT,2);
    case 0xF1: return F(GL_R16UI,GL_RED_INTEGER,GL_UNSIGNED_SHORT,2);
    case 0xF2: return F(GL_R16F,GL_RED,GL_HALF_FLOAT,2);
    case 0xF3: return F(GL_R8,GL_RED,GL_UNSIGNED_BYTE,1);
    case 0xF6: return F(GL_R8UI,GL_RED_INTEGER,GL_UNSIGNED_BYTE,1);
    /* depth */
    case 0x0A: return FD(GL_DEPTH_COMPONENT32F,GL_DEPTH_COMPONENT,GL_FLOAT,4);
    case 0x13: return FD(GL_DEPTH_COMPONENT16,GL_DEPTH_COMPONENT,GL_UNSIGNED_SHORT,2);
    case 0x14: case 0x15: case 0x16: case 0x18: return FD(GL_DEPTH24_STENCIL8,GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,4);
    case 0x19: return FD(GL_DEPTH32F_STENCIL8,GL_DEPTH_STENCIL,GL_FLOAT_32_UNSIGNED_INT_24_8_REV,8);
  }
  WARN("gpu: unsupported render target format %x\n",f);
  return F(GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,4); }

/* ============================================================ texture cache */
typedef struct Tex {
  u64 gva; u32 w, h, d, levels, target; u32 fkey; GlFmt f;
  u32 bl, bh, bd, pitch; u64 size;
  GLuint id; u64 hash; int is_rt, gpu_written; u32 frame_checked, last_used; GLint swz[4]; u64 tr_last; u32 stable, next_check; u64 video_gen; GLuint view; GLenum view_target; GLint vswz[4]; u32 wgen, crop_gen; struct Tex* crop; struct Tex* lay; struct Tex* vol; int skipclr, zero3d; u64 alias; u64 shash; u32 tfull;
  GLuint pre; u32 pre_wgen, pre_w, pre_h; int pre_lv;   /* box-averaged copy for downsampling reads at a high resolution scale */
  struct Tex* next;
} Tex;
#define TEXH 4096
static Tex* texh[TEXH]; static u32 g_frame;

static u32 hkey(u64 a){ return (u32)((a>>8)^(a>>20))&(TEXH-1); }
/* GPU thread time per category, for performance.txt */
enum { TM_SH, TM_SHREADY, TM_STATE, TM_TEX, TM_BUF, TM_DRAW, TM_SWAP, TM_CB, TM_GB, TM_N };
static u64 g_tm[TM_N], g_tmp[TM_N]; static u64 st_sh_new;
#define TM(k_,code_) do{ u64 t_=plat_ns(); code_; g_tm[k_]+=plat_ns()-t_; }while(0)

/* full size of a bl texture, all layers and mips */
static u64 bl_level_size(u32 w, u32 h, u32 d, const GlFmt* f, u32 bh, u32 bd, u32 level, u32* out_bh){
  u32 lw=w>>level, lh=h>>level, ld=d>>level; if(!lw) lw=1; if(!lh) lh=1; if(!ld) ld=1;
  u32 tw=(lw+f->bw-1)/f->bw, th=(lh+f->bh-1)/f->bh; u32 rowb=tw*f->bpp;
  u32 gx=(rowb+63)/64, gy=(th+7)/8;
  u32 b=bh; while(b>0 && gy<=(1u<<(b-1))) b--;
  u32 bdd=bd; while(bdd>0 && ld<=(1u<<(bdd-1))) bdd--;
  if(out_bh) *out_bh=b;
  u64 gys=((gy+(1u<<b)-1)>>b)<<b; u64 lds=((ld+(1u<<bdd)-1)>>bdd)<<bdd;
  return (u64)gx*gys*lds*512; }
extern void astc_decode_image(const u8* src, u32 w, u32 h, u32 bw, u32 bh, u8* dst);

/* hack for SMO's Cap Kingdom sea: the VS offsets the surface by (h-0.5)*k using an R16F heightmap that's
   still all zeros at this point (haven't found what writes it). zeros push the sea up into the sky, 0.5 is
   neutral. only applied while the memory really is all zero */
static void mb_reset(void);

static int tex_zero_height_fix(Tex* t){
  static int off=-1; if(off<0) off=getenv("SWR_NO_SEA_FIX")!=NULL; if(off) return 0;
  if((t->fkey&0x7FFFF)!=0x7ff9b || t->is_rt || t->target!=GL_TEXTURE_2D || t->levels<6 || t->w!=t->h || t->w<256 || (t->w&(t->w-1)) || !t->size) return 0;
  const u8* p=gmem_ptr(t->gva,t->size); if(!p) return 0;
  const u64* q=(const u64*)p; for(u64 k=0;k<t->size/8;k++) if(q[k]) return 0;
  u16* half=malloc((size_t)t->w*t->h*2); if(!half) return 0; for(u64 k=0;k<(u64)t->w*t->h;k++) half[k]=0x3800;
  glBindTexture(GL_TEXTURE_2D,t->id); glPixelStorei(GL_UNPACK_ALIGNMENT,1); glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
  for(u32 l=0;l<t->levels;l++){ u32 lw=t->w>>l, lh=t->h>>l; if(!lw) lw=1; if(!lh) lh=1; glTexSubImage2D(GL_TEXTURE_2D,l,0,0,lw,lh,GL_RED,GL_HALF_FLOAT,half); }
  free(half); mb_reset();
  { static int said; if(said<8){ said++; LOG("gpu: R16F heightmap %ux%u at %llx is all zero: filled with neutral height 0.5 (sea workaround, SWR_NO_SEA_FIX=1 disables)\n",t->w,t->h,(unsigned long long)t->gva); } }
  return 1; }

/* Render-thread scratch storage. Queued GL uploads copy client data before
   returning; direct GL uploads consume it before returning. Oversize requests
   stay temporary so a rare huge texture does not permanently retain memory. */
typedef struct {u8*data;u64 capacity;} UploadScratch;
static UploadScratch upload_linear,upload_source,upload_decoded;
static u64 upload_allocations;
static u8* upload_buffer(UploadScratch*b,u64 n){
  if(!n)n=1;
  if(n>16ull*1024*1024 || getenv("SWR_NO_UPLOAD_SCRATCH")){upload_allocations++;return malloc((size_t)n);}
  if(n>b->capacity){u8*p=realloc(b->data,(size_t)n);upload_allocations++;if(!p)return NULL;b->data=p;b->capacity=n;}
  return b->data;
}
static void upload_release(UploadScratch*b,u8*p){if(p!=b->data)free(p);}

/* ---- parallel for (from SwitchRecomp v185): texture loads are deswizzled by row ranges over the CPU cores.
   one job per call; a thread only takes the job under the lock while it is published, and the caller waits
   for every thread that took it, so a late thread never touches the next job. ini texture_threads: 0 = auto
   (cores-1, at most 7 helpers), 1 = off (the old single-thread unpack), N = N threads. SWR_DECODE_THREADS
   overrides for tests */
typedef void (*PfFn)(void* c, u32 a, u32 b);
typedef struct { PfFn fn; void* ctx; u32 n, ch, refs; volatile u32 next; } PfJob;
static struct { PMutex lk; PCond cv, dn; PfJob* job; u32 gen; int nth, init; } g_pf;
static void pf_work(PfJob* j){ for(;;){ u32 a=__atomic_fetch_add(&j->next,j->ch,__ATOMIC_ACQ_REL); if(a>=j->n) break; u32 b=a+j->ch; if(b>j->n) b=j->n; j->fn(j->ctx,a,b); } }
static void pf_thread(void* arg){ (void)arg; u32 seen=0;
  for(;;){ pm_lock(&g_pf.lk); while(g_pf.gen==seen) pc_wait(&g_pf.cv,&g_pf.lk,1000000000ll);
    seen=g_pf.gen; PfJob* j=g_pf.job; if(!j){ pm_unlock(&g_pf.lk); continue; } j->refs++; pm_unlock(&g_pf.lk);
    pf_work(j);
    pm_lock(&g_pf.lk); if(!--j->refs) pc_broadcast(&g_pf.dn); pm_unlock(&g_pf.lk); } }
static void pf_run(PfFn fn, void* ctx, u32 n, u32 min_par){
  if(!g_pf.init){ g_pf.init=1; pm_init(&g_pf.lk); pc_init(&g_pf.cv); pc_init(&g_pf.dn);
    extern int plat_ncpu(void); extern int g_opt_tex_threads; int nc=plat_ncpu(); g_pf.nth= nc>1? nc-1 : 0; if(g_pf.nth>7) g_pf.nth=7;
    if(g_opt_tex_threads>0) g_pf.nth=g_opt_tex_threads-1;
    const char* e=getenv("SWR_DECODE_THREADS"); if(e) g_pf.nth=atoi(e)-1;
    if(g_pf.nth<0) g_pf.nth=0; if(g_pf.nth>15) g_pf.nth=15;
    int ok=0; for(int i=0;i<g_pf.nth;i++) if(!plat_thread(pf_thread,NULL,1u<<20)) ok++; g_pf.nth=ok;
    LOG("textures: %d thread(s) deswizzle texture loads\n",g_pf.nth+1); }
  if(!g_pf.nth || n<min_par){ fn(ctx,0,n); return; }
  PfJob j; j.fn=fn; j.ctx=ctx; j.n=n; j.ch=n/((u32)(g_pf.nth+1)*4); if(!j.ch) j.ch=1; j.refs=0; j.next=0;
  pm_lock(&g_pf.lk); g_pf.job=&j; g_pf.gen++; pc_broadcast(&g_pf.cv); pm_unlock(&g_pf.lk);
  pf_work(&j);
  pm_lock(&g_pf.lk); g_pf.job=NULL; while(j.refs) pc_wait(&g_pf.dn,&g_pf.lk,1000000000ll); pm_unlock(&g_pf.lk); }
typedef struct { u8* lin; const u8* src; u64 lsz; u32 rowb, th, lbh, lbd; } DsJob;
static void ds_rows(void* c, u32 a, u32 b){ DsJob* j=c; deswizzle_rows(j->lin,j->rowb,j->src,j->lsz,j->th,j->lbh,j->lbd,a,b); }

static u64 tex_stage_totals[5];
static int g_upload_direct=1;   /* SWR_UPLOAD_COPY=1 restores the scratch copy (A/B) */
static void tex_upload(Tex* t){ st_tex_up+=t->size; u64 upload_start=plat_ns(), upload_read_ns=0, upload_unpack_ns=0, upload_decode_ns=0, upload_driver_ns=0, upload_stage;
  if(tex_zero_height_fix(t)) return;
  { static int e=-1; if(e<0){ e=getenv("SWR_UPLOAD_COPY")!=NULL; if(e) g_upload_direct=0; } }
  const GlFmt* f=&t->f;
  u32 layers= t->target==GL_TEXTURE_3D? 1 : t->d;
  u32 depth3d= t->target==GL_TEXTURE_3D? t->d : 1;
  u64 layer_size=0; for(u32 l=0;l<t->levels;l++) layer_size+=bl_level_size(t->w,t->h,depth3d,f,t->bh,t->bd,l,NULL);
  if(t->bl && layers>1){ u64 al=512ull<<t->bh; layer_size=(layer_size+al-1)/al*al; }
  glBindTexture(t->target,t->id);
  glPixelStorei(GL_UNPACK_ALIGNMENT,1); glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
  int was_zero3d= t->target==GL_TEXTURE_3D? t->zero3d : 0; (void)was_zero3d;
  if(t->target==GL_TEXTURE_3D) t->zero3d=1;
  for(u32 layer=0;layer<layers;layer++){
    u64 off=layer*layer_size;
    for(u32 l=0;l<t->levels;l++){
      u32 lw=t->w>>l, lh=t->h>>l, ld=depth3d>>l; if(!lw) lw=1; if(!lh) lh=1; if(!ld) ld=1;
      u32 tw=(lw+f->bw-1)/f->bw, th=(lh+f->bh-1)/f->bh; u32 rowb=tw*f->bpp; u32 lbh;
      u64 lsz= t->bl? bl_level_size(t->w,t->h,depth3d,f,t->bh,t->bd,l,&lbh) : (u64)t->pitch*th;
      u64 lin_size=(u64)rowb*th*ld; u8* lin=upload_buffer(&upload_linear,lin_size);
      /* unpack straight from guest memory when the range is contiguous (it almost always is): this used to
         copy the whole level into a scratch buffer first, about a third of all upload time */
      u8* direct= g_upload_direct? gmem_ptr(t->gva+off,lsz) : NULL;
      u8* src= direct? direct : upload_buffer(&upload_source,lsz);
      if(!lin || !src){ upload_release(&upload_linear,lin); if(!direct) upload_release(&upload_source,src); static int said; if(said<4){ said++; LOG("gpu: out of memory uploading texture %ux%ux%u (%llu MB): skipped\n",t->w,t->h,t->d,(unsigned long long)(lsz>>20)); } off+= t->bl? lsz : (u64)t->pitch*th; continue; }
      upload_stage=plat_ns(); if(!direct) gmem_read(t->gva+off,src,lsz); upload_read_ns+=plat_ns()-upload_stage;
      if(t->target==GL_TEXTURE_3D && t->zero3d){ for(u64 k=0;k<lsz;k++) if(src[k]){ t->zero3d=0; break; }
        if(!t->zero3d && was_zero3d){ static int said; if(said<16){ said++; LOG("gpu: 3D texture %ux%ux%u at %llx got its data in memory (frame %u)\n",t->w,t->h,t->d,(unsigned long long)t->gva,g_frame); } } }
      upload_stage=plat_ns();
      if(t->bl){ u32 lbd=t->bd; while(lbd>0 && ld<=(1u<<(lbd-1))) lbd--;   /* 3D: block depth (LUTs) */
        DsJob dj={lin,src,lsz,rowb,th,lbh,lbd}; pf_run(ds_rows,&dj,ld*th,(u32)((256u<<10)/(rowb? rowb : 1))); }   /* split when the level is 256 KB or more */
      else { for(u32 y=0;y<th;y++) memcpy(lin+(u64)y*rowb,src+(u64)y*t->pitch,rowb); }
      upload_unpack_ns+=plat_ns()-upload_stage; upload_stage=plat_ns();
      const u8* up=lin; u8* dec=NULL;
      if((f->astc||f->dec) && !(dec=upload_buffer(&upload_decoded,(u64)lw*lh*4+64))){ upload_release(&upload_linear,lin); if(!direct) upload_release(&upload_source,src); off+= t->bl? lsz : (u64)t->pitch*th; continue; }
      if(f->astc){ astc_decode_image(lin,lw,lh,f->bw,f->bh,dec); up=dec; }
      else if(f->dec){ u32* o=(u32*)dec;
        if(f->dec<10){ swr_bc_decode(f->dec,lin,lw,lh,o); }
        else { const u16* q=(const u16*)lin; for(u32 y=0;y<lh;y++) for(u32 x=0;x<lw;x++){ u32 v=q[(size_t)y*(rowb/2)+x], rr,gg,bb,aa;
            if(f->dec==10){ rr=(v&15)*17; gg=((v>>4)&15)*17; bb=((v>>8)&15)*17; aa=((v>>12)&15)*17; }
            else if(f->dec==11){ rr=(v&31)*255/31; gg=((v>>5)&31)*255/31; bb=((v>>10)&31)*255/31; aa=(v>>15)? 255 : 0; }
            else { rr=(v&31)*255/31; gg=((v>>5)&63)*255/63; bb=((v>>11)&31)*255/31; aa=255; }
            o[(size_t)y*lw+x]=rr|(gg<<8)|(bb<<16)|(aa<<24); } }
        up=dec; }
      upload_decode_ns+=plat_ns()-upload_stage;
      { static int dmp=-1, nd; if(dmp<0) dmp=getenv("SWR_DUMP_TEX")!=NULL;
        if(dmp && l==0 && layer==0 && nd<20 && (f->astc || (!f->compressed && f->bpp==4))){ char pth[128]; snprintf(pth,sizeof pth,"tex_%02d_%llx_%ux%u.ppm",nd++,(unsigned long long)t->gva,lw,lh);
          FILE* fo=fopen(pth,"wb"); if(fo){ fprintf(fo,"P6\n%u %u\n255\n",lw,lh); for(u64 k=0;k<(u64)lw*lh;k++) fwrite(up+k*4,1,3,fo); fclose(fo); } } }
      upload_stage=plat_ns();
      if(f->compressed && !f->astc && !f->dec){
        GLsizei isz=(GLsizei)(rowb*th);
        if(t->target==GL_TEXTURE_2D) glCompressedTexSubImage2D(GL_TEXTURE_2D,l,0,0,lw,lh,f->ifmt,isz,up);
        else if(t->target==GL_TEXTURE_2D_ARRAY || t->target==GL_TEXTURE_CUBE_MAP_ARRAY) glCompressedTexSubImage3D(t->target,l,0,0,layer,lw,lh,1,f->ifmt,isz,up);
        else if(t->target==GL_TEXTURE_CUBE_MAP) glCompressedTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+layer,l,0,0,lw,lh,f->ifmt,isz,up);
        else if(t->target==GL_TEXTURE_3D) glCompressedTexSubImage3D(GL_TEXTURE_3D,l,0,0,0,lw,lh,ld,f->ifmt,isz*ld,up);
      } else {
        GLenum fm= (f->astc||f->dec)? GL_RGBA : f->fmt, ty= (f->astc||f->dec)? GL_UNSIGNED_BYTE : f->type;
        if(t->target==GL_TEXTURE_2D) glTexSubImage2D(GL_TEXTURE_2D,l,0,0,lw,lh,fm,ty,up);
        else if(t->target==GL_TEXTURE_2D_ARRAY || t->target==GL_TEXTURE_CUBE_MAP_ARRAY) glTexSubImage3D(t->target,l,0,0,layer,lw,lh,1,fm,ty,up);
        else if(t->target==GL_TEXTURE_CUBE_MAP) glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+layer,l,0,0,lw,lh,fm,ty,up);
        else if(t->target==GL_TEXTURE_3D) glTexSubImage3D(GL_TEXTURE_3D,l,0,0,0,lw,lh,ld,fm,ty,up);
        else if(t->target==GL_TEXTURE_1D) glTexSubImage2D(GL_TEXTURE_2D,l,0,0,lw,1,fm,ty,up);
      }
      upload_driver_ns+=plat_ns()-upload_stage;
      upload_release(&upload_decoded,dec); upload_release(&upload_linear,lin); if(!direct) upload_release(&upload_source,src);
      off+= t->bl? lsz : (u64)t->pitch*th;
    }
  }
  u64 upload_total=plat_ns()-upload_start;
  tex_stage_totals[0]+=upload_total; tex_stage_totals[1]+=upload_read_ns; tex_stage_totals[2]+=upload_unpack_ns; tex_stage_totals[3]+=upload_decode_ns; tex_stage_totals[4]+=upload_driver_ns;
  if(upload_total>10000000ull) LOG("texture upload: %ux%ux%u levels=%u format=%x astc=%u decode=%u bytes=%llu total=%.2f read=%.2f unpack=%.2f decode=%.2f driver=%.2f ms\n",t->w,t->h,t->d,t->levels,t->fkey,f->astc,f->dec,(unsigned long long)t->size,upload_total/1e6,upload_read_ns/1e6,upload_unpack_ns/1e6,upload_decode_ns/1e6,upload_driver_ns/1e6);

}
static u32 g_tex_bud_frame; static i64 g_tex_bud; static u64 st_tex_chk;

static u64 fast_hash(const u8* p, u64 size, int sampled){
  const u64* q=(const u64*)p; u64 n=size/8, step= (sampled && n>8192)? 16 : 1;   /* sample one word per 128 bytes */
  u64 a=0x9E3779B97F4A7C15ull, b=0xC2B2AE3D27D4EB4Full, c=0x165667B19E3779F9ull, d=0x27D4EB2F165667C5ull; u64 i=0;
  for(; i+3*step<n; i+=4*step){ a=(a^q[i])*0x100000001B3ull; b=(b^q[i+step])*0x100000001B3ull; c=(c^q[i+2*step])*0x100000001B3ull; d=(d^q[i+3*step])*0x100000001B3ull; }
  for(; i<n; i+=step){ a=(a^q[i])*0x100000001B3ull; }
  for(u64 k=n*8;k<size;k++){ b=(b^p[k])*0x100000001B3ull; }
  return a ^ (b<<1) ^ (c<<2) ^ (d<<3) ^ size; }
static u64 sample_hash(u64 gva, u64 len); static int g_nosample;

static u64 mem_hash(u64 gva, u64 size){
  /* quick sample: one word per 64 bytes + the first 4K */
  u64 h=1469598103934665603ull; u8* p=gmem_ptr(gva,size);
  if(p) return fast_hash(p,size,1);
  for(u64 o=0;o<size;o+=4096){ u64 v[8]; gmem_read(gva+o,v,size-o<64? size-o : 64); for(int i=0;i<8;i++){ h^=v[i]; h*=1099511628211ull; } }
  return h; }

static u64 rt_sig(u64 gva){ u8* p=gmem_ptr(gva,4096); if(p) return fast_hash(p,4096,0); u8 b[4096]; gmem_read(gva,b,4096); return fast_hash(b,4096,0); }

static GLenum tex_target(u32 type, u32 depth){
  switch(type){ case 0: return GL_TEXTURE_2D; case 1: case 7: return depth>1? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D; case 2: return GL_TEXTURE_3D; case 3: return GL_TEXTURE_CUBE_MAP;
    case 4: case 5: return GL_TEXTURE_2D_ARRAY; case 8: return GL_TEXTURE_CUBE_MAP_ARRAY; default: return GL_TEXTURE_2D; } }
/* generation counter for the TIC -> texture mapping. Bump it on anything that can change what a TIC
   resolves to (tex created/freed, RT dropped or recreated, FBO attachments, clears, new frame).
   tex_from_tic memoizes its result while this stays the same */
static u32 g_tmap_gen=1;

static u64 tex_creation_ns,tex_creation_names_ns,tex_creation_storage_ns,tex_creation_calls;
static Tex* tex_create(u64 gva, u32 w, u32 h, u32 d, u32 levels, GLenum target, GlFmt f, u32 fkey, u32 bl, u32 bh, u32 bd, u32 pitch, int is_rt){
  u64 creation_start=plat_ns(),creation_stage; tex_creation_calls++;
  g_tmap_gen++;
  Tex* t=calloc(1,sizeof *t); t->swz[0]=GL_RED; t->swz[1]=GL_GREEN; t->swz[2]=GL_BLUE; t->swz[3]=GL_ALPHA; t->gva=gva; t->w=w; t->h=h; t->d=d; t->levels=levels; t->target=target; t->f=f; t->fkey=fkey; t->bl=bl; t->bh=bh; t->bd=bd; t->pitch=pitch; t->is_rt=is_rt;
  creation_stage=plat_ns();
  glGenTextures(1,&t->id); glBindTexture(target,t->id);
  tex_creation_names_ns+=plat_ns()-creation_stage; creation_stage=plat_ns();
  GLenum ifmt=f.ifmt; u32 sw=is_rt? SC(w) : w, sh=is_rt? SC(h) : h;
  if(target==GL_TEXTURE_2D || target==GL_TEXTURE_CUBE_MAP) glTexStorage2D(target,levels,ifmt,sw,sh);
  else if(target==GL_TEXTURE_CUBE_MAP_ARRAY) glTexStorage3D(target,levels,ifmt,sw,sh,d*6>6? d : 6);
  else glTexStorage3D(target,levels,ifmt,sw,sh,d);
  tex_creation_storage_ns+=plat_ns()-creation_stage;
  glTexParameteri(target,GL_TEXTURE_MAX_LEVEL,(GLint)levels-1);
  t->last_used=g_frame; u32 k=hkey(gva); t->next=texh[k]; texh[k]=t;
  tex_creation_ns+=plat_ns()-creation_start;
  return t; }

static Tex* tex_find_rt(u64 gva){ for(Tex* t=texh[hkey(gva)];t;t=t->next) if(t->gva==gva && t->is_rt) return t; return NULL; }

/* the game samples an RT through a smaller TIC (DRS, crops). On hw uv 1.0 is the TIC edge, not the RT
   edge, so hand out a copy of just that region */
static PFNGLCOPYIMAGESUBDATAPROC p_cis; static u64 st_rt_crop;

static Tex* rt_crop(Tex* rt, u32 w, u32 h){
  if(!p_cis || getenv("SWR_NO_RTCROP")) return rt;
  Tex* c=rt->crop;
  if(c && (c->w!=w || c->h!=h)){ glDeleteTextures(1,&c->id); if(c->view) glDeleteTextures(1,&c->view); free(c); c=rt->crop=NULL; }
  if(!c){ g_tmap_gen++; c=calloc(1,sizeof *c); c->gva=rt->gva; c->w=w; c->h=h; c->d=1; c->levels=1; c->target=GL_TEXTURE_2D; c->f=rt->f; c->fkey=rt->fkey; c->is_rt=1; c->bl=rt->bl; c->bh=rt->bh;
    c->swz[0]=GL_RED; c->swz[1]=GL_GREEN; c->swz[2]=GL_BLUE; c->swz[3]=GL_ALPHA;
    glGenTextures(1,&c->id); glBindTexture(GL_TEXTURE_2D,c->id); glTexStorage2D(GL_TEXTURE_2D,1,rt->f.ifmt,SC(w),SC(h));
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0); c->crop_gen=~0u; rt->crop=c;
    static int said; if(said<10){ said++; LOG("gpu: %ux%u texture reads the %ux%u render target (%llx): only that region is used\n",w,h,rt->w,rt->h,(unsigned long long)rt->gva); } }
  if(c->crop_gen!=rt->wgen){ p_cis(rt->id,GL_TEXTURE_2D,0,0,0,0,c->id,GL_TEXTURE_2D,0,0,0,0,(GLsizei)SC(w),(GLsizei)SC(h),1); c->crop_gen=rt->wgen; st_rt_crop++; }
  c->last_used=g_frame; return c; }
/* copies into mip levels of textures we assemble from RTs. Drivers disagree here (RX 7600: levels > 0
   come back black with CopyImageSubData, NVIDIA is fine), so gl_selftest() decides at startup:
   0 = glCopyImageSubData, 1 = blit. g_mipgen_manual: glGenerateMipmap from base level > 0 is broken,
   build the levels with linear blits */
static GLuint g_rfbo, g_dfbo; static GLuint g_fbo, g_scratch_vao;
static int g_lvl_mode, g_mipgen_manual;

static void lvl_attach(GLenum fbt, GLuint tex, GLenum target, int level, int layer){
  if(target==GL_TEXTURE_2D || !tex) glFramebufferTexture2D(fbt,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,level);
  else if(target==GL_TEXTURE_CUBE_MAP) glFramebufferTexture2D(fbt,GL_COLOR_ATTACHMENT0,GL_TEXTURE_CUBE_MAP_POSITIVE_X+(GLenum)layer,tex,level);
  else glFramebufferTextureLayer(fbt,GL_COLOR_ATTACHMENT0,tex,level,layer); }

/* color blit src(target/level/layer) -> dst(level/layer); saves and restores scissor, srgb, color mask */
static void lvl_blit(GLuint src, GLenum st, int sl, int slay, GLsizei sw, GLsizei sh, GLuint dst, GLenum dtg, int dl, int dlay, GLsizei dw, GLsizei dh, GLenum filt){
  /* glGetIntegerv, not glGetBooleanv: only the former goes through the GL call queue */
  GLboolean sc=glIsEnabled(GL_SCISSOR_TEST), sr=glIsEnabled(GL_FRAMEBUFFER_SRGB); GLint cm[4]={1,1,1,1}; glGetIntegerv(GL_COLOR_WRITEMASK,cm);
  glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB); glColorMaski(0,1,1,1,1);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,g_rfbo); lvl_attach(GL_READ_FRAMEBUFFER,src,st,sl,slay); glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,g_dfbo); lvl_attach(GL_DRAW_FRAMEBUFFER,dst,dtg,dl,dlay); { GLenum b=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&b); }
  glBlitFramebuffer(0,0,sw,sh,0,0,dw,dh,GL_COLOR_BUFFER_BIT,filt);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0); glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0);
  glBindFramebuffer(GL_FRAMEBUFFER,g_fbo); if(sc) glEnable(GL_SCISSOR_TEST); if(sr) glEnable(GL_FRAMEBUFFER_SRGB); glColorMaski(0,cm[0]!=0,cm[1]!=0,cm[2]!=0,cm[3]!=0); }

/* read one texel of a level via the read fbo (glReadPixels is queued, glGetTextureSubImage isn't) */
static void lvl_read1(GLuint tex, GLenum target, int level, int layer, GLint x, GLint y, float* out){
  glBindFramebuffer(GL_READ_FRAMEBUFFER,g_rfbo); lvl_attach(GL_READ_FRAMEBUFFER,tex,target,level,layer); glReadBuffer(GL_COLOR_ATTACHMENT0);
  glReadPixels(x,y,1,1,GL_RGBA,GL_FLOAT,out);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0); glBindFramebuffer(GL_FRAMEBUFFER,g_fbo); }

static void lvl_copy(GLuint src, GLuint dst, GLenum target, int level, int layer, GLsizei w, GLsizei h, int color){
  if(!g_lvl_mode || !color){ p_cis(src,GL_TEXTURE_2D,0,0,0,0,dst,target,level,0,0,target==GL_TEXTURE_2D? 0 : layer,w,h,1); return; }
  lvl_blit(src,GL_TEXTURE_2D,0,0,w,h,dst,target,level,layer,w,h,GL_NEAREST); }

/* fill levels from+1..levels-1 from level 'from' */
static void lvl_gen(GLuint tex, GLenum target, int from, int levels, u32 layers, u32 W0, u32 H0){
  if(!g_mipgen_manual){
    glBindTexture(target,tex); glTexParameteri(target,GL_TEXTURE_BASE_LEVEL,from); glTexParameteri(target,GL_TEXTURE_MAX_LEVEL,levels-1);
    glGenerateMipmap(target); glTexParameteri(target,GL_TEXTURE_BASE_LEVEL,0); return; }
  u32 nl= target==GL_TEXTURE_CUBE_MAP? 6 : target==GL_TEXTURE_2D? 1 : layers;
  for(int l=from;l+1<levels;l++){ GLsizei sw=(GLsizei)(W0>>l), sh=(GLsizei)(H0>>l), dw=(GLsizei)(W0>>(l+1)), dh=(GLsizei)(H0>>(l+1));
    if(sw<1) sw=1; if(sh<1) sh=1; if(dw<1) dw=1; if(dh<1) dh=1;
    for(u32 y=0;y<nl;y++) lvl_blit(tex,target,l,(int)y,sw,sh,tex,target,l+1,(int)y,dw,dh,GL_LINEAR); }
  glBindTexture(target,tex); glTexParameteri(target,GL_TEXTURE_MAX_LEVEL,levels-1); }
/* cubemaps / arrays drawn face by face: each face+mip is its own 2D RT at
   gva + layer*layer_size + mip_offset. gather them into a real cube texture (GPU copies, only when a
   face changed) so cube samplers get something */
static u64 st_lay_copy; static int g_fb_sig_ok; static void mb_reset(void);
/* 3D texture rendered slice by slice. In 3D block linear the slices of a block interleave per GOB:
   base + (z % block_depth) * (block_height * 512) + (z / block_depth) * block_layer_size.
   gather them into a 3D texture, copying only changed slices */
static u64 st_vol_copy;

static u64 slice3d_addr(u64 gva, u32 w, u32 h, u32 d, const GlFmt* f, u32 bh, u32 bd, u32 z){
  u32 tw=(w+f->bw-1)/f->bw, th=(h+f->bh-1)/f->bh; u32 rowb=tw*f->bpp; u32 gx=(rowb+63)/64, gy=(th+7)/8;
  u32 b=bh; while(b>0 && gy<=(1u<<(b-1))) b--;
  u32 bdd=bd; while(bdd>0 && d<=(1u<<(bdd-1))) bdd--;
  u64 gys=((u64)(gy+(1u<<b)-1)>>b)<<b;
  return gva + (u64)(z>>bdd)*gx*gys*(1ull<<bdd)*512ull + ((u64)(z&((1u<<bdd)-1))<<b)*512ull; }

static Tex* vol_from_slices(Tex* rt0, u64 gva, u32 w, u32 h, u32 d, GlFmt f, u32 bh, u32 bd){
  if(!p_cis || d<2 || d>256 || !rt0->bl) return NULL;
  Tex* sl[256]; u32 sig=2166136261u;
  static int dbg=-1; if(dbg<0) dbg=getenv("SWR_LAYDBG")!=0;
  for(u32 z=0;z<d;z++){ u64 sa=slice3d_addr(gva,w,h,d,&f,bh,bd,z); Tex* r=tex_find_rt(sa);
    if(!r || r->target!=GL_TEXTURE_2D || r->w!=w || r->h!=h || r->f.ifmt!=rt0->f.ifmt){
      if(dbg){ static int n; if(n<20){ n++; LOG("voldbg %llx %ux%ux%u bh=%u bd=%u z=%u at %llx: %s\n",(unsigned long long)gva,w,h,d,bh,bd,z,(unsigned long long)sa,r? "different rt" : "no rt"); } }
      return NULL; }
    sl[z]=r; sig=(sig^(r->wgen+z*31))*16777619u; sig=(sig^(u32)(uintptr_t)r)*16777619u; }
  Tex* c=rt0->lay;
  if(c && (c->w!=w || c->h!=h || c->d!=d || c->target!=GL_TEXTURE_3D || c->fkey!=rt0->fkey || c->pitch!=SC(w))){
    glDeleteTextures(1,&c->id); if(c->view) glDeleteTextures(1,&c->view); free(c); c=rt0->lay=NULL; g_fb_sig_ok=0; g_tmap_gen++; }
  if(!c){ g_tmap_gen++; c=calloc(1,sizeof *c); c->gva=gva; c->w=w; c->h=h; c->d=d; c->levels=1; c->target=GL_TEXTURE_3D; c->f=rt0->f; c->fkey=rt0->fkey; c->is_rt=1; c->bl=1; c->bh=bh; c->pitch=SC(w);
    c->swz[0]=GL_RED; c->swz[1]=GL_GREEN; c->swz[2]=GL_BLUE; c->swz[3]=GL_ALPHA;
    glGenTextures(1,&c->id); glBindTexture(GL_TEXTURE_3D,c->id); glTexStorage3D(GL_TEXTURE_3D,1,rt0->f.ifmt,SC(w),SC(h),d);
    glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_MAX_LEVEL,0); c->crop_gen=~0u; rt0->lay=c; mb_reset();
    static int said; if(said<12){ said++; LOG("gpu: 3D texture %ux%ux%u (%llx) assembled from its %u GPU-rendered slices\n",w,h,d,(unsigned long long)gva,d); } }
  if(c->crop_gen!=sig){
    for(u32 z=0;z<d;z++) p_cis(sl[z]->id,GL_TEXTURE_2D,0,0,0,0,c->id,GL_TEXTURE_3D,0,0,0,(GLint)z,(GLsizei)SC(w),(GLsizei)SC(h),1);
    c->crop_gen=sig; st_vol_copy++; }
  c->last_used=g_frame; return c; }

static int getenv_cached_nomip(void){ static int v=-1; if(v<0) v=getenv("SWR_NO_RT_MIPS")!=NULL; return v; }

static Tex* layered_from_rts(Tex* rt0, u64 gva, u32 w, u32 h, u32 d, u32 levels, GLenum target, GlFmt f, u32 fkey, u32 bl, u32 bh, u32 bd){
  if(!p_cis || levels>15 || d<1) return NULL;
  u64 loff[16], lsz=0; for(u32 l=0;l<levels;l++){ loff[l]=lsz; lsz+= bl? bl_level_size(w,h,1,&f,bh,bd,l,NULL) : 0; }
  if(!bl) return NULL;
  if(d>1){ u64 al=512ull<<bh; lsz=(lsz+al-1)/al*al; }
  Tex* c=rt0->lay;
  if(c && (c->w!=w || c->h!=h || c->d!=d || c->levels!=levels || c->target!=target || c->fkey!=rt0->fkey)){
    glDeleteTextures(1,&c->id); if(c->view) glDeleteTextures(1,&c->view); free(c); c=rt0->lay=NULL; g_fb_sig_ok=0; g_tmap_gen++; }
  u32 W0=SC(w), H0=SC(h);
  if(!c){ g_tmap_gen++; c=calloc(1,sizeof *c); c->gva=gva; c->w=w; c->h=h; c->d=d; c->levels=levels; c->target=target; c->f=rt0->f; c->fkey=rt0->fkey; c->is_rt=1; c->bl=1; c->bh=bh;
    c->swz[0]=GL_RED; c->swz[1]=GL_GREEN; c->swz[2]=GL_BLUE; c->swz[3]=GL_ALPHA;
    glGenTextures(1,&c->id); glBindTexture(target,c->id);
    if(target==GL_TEXTURE_CUBE_MAP || target==GL_TEXTURE_2D) glTexStorage2D(target,levels,rt0->f.ifmt,W0,H0); else glTexStorage3D(target,levels,rt0->f.ifmt,W0,H0,d);
    glTexParameteri(target,GL_TEXTURE_MAX_LEVEL,0); c->crop_gen=~0u; c->stable=0; rt0->lay=c; mb_reset();
    static int said; if(said<6){ said++; LOG("gpu: %s %ux%u (%u layers, %u mips) assembled from its render targets (%llx)\n",target==GL_TEXTURE_CUBE_MAP? "cubemap":target==GL_TEXTURE_2D? "mip chain":"array",w,h,d,levels,(unsigned long long)gva); } }
  /* signature: which RTs exist and their write counters */
  u32 sig=2166136261u; int maxl=-1;
  for(u32 l=0;l<levels;l++){ int all=1;
    for(u32 y=0;y<d;y++){ Tex* r=tex_find_rt(gva+(u64)y*lsz+loff[l]); u32 lw=w>>l, lh=h>>l; if(!lw) lw=1; if(!lh) lh=1;
      /* tiny mips get rendered into RTs padded to a GOB row (64 bytes), e.g. 8x4 / 8x2 for the 4x4 / 2x2 mips of an RGBA16F chain */
      u32 gobpx= rt0->f.bpp? 64u/rt0->f.bpp : 16u; if(!gobpx) gobpx=1;
      if(r && r->target==GL_TEXTURE_2D && (r->w==lw || (l>0 && r->w>=lw && (r->w<lw*2 || r->w<=gobpx))) && (r->h==lh || (l>0 && r->h>=lh && r->h<lh*2)) && r->f.ifmt==rt0->f.ifmt){ sig=(sig^(r->wgen+y*31+l*977))*16777619u; sig=(sig^(u32)(uintptr_t)r)*16777619u; } else all=0; }
    if(!all) break; maxl=(int)l; }
  if(getenv("SWR_LAYDBG")){ static int n; if(n<3){ n++; LOG("laydbg gva=%llx lsz=%llx bh=%u levels=%u maxl=%d\n",(unsigned long long)gva,(unsigned long long)lsz,bh,levels,maxl);
      for(u32 l=0;l<levels;l++) for(u32 y=0;y<d;y++){ u64 a=gva+(u64)y*lsz+loff[l]; Tex* r=tex_find_rt(a); LOG("  l%u c%u %llx -> %s %ux%u\n",l,y,(unsigned long long)a,r?"rt":"-",r?r->w:0,r?r->h:0); }
      for(int k=0;k<TEXH;k++) for(Tex* r=texh[k];r;r=r->next) if(r->is_rt && r->w<=64 && r->h<=64) LOG("  RT %llx %ux%u\n",(unsigned long long)r->gva,r->w,r->h); } }
  if(maxl<0) return NULL;   /* missing faces -> normal path */
  if(c->crop_gen!=sig){
    int col=!rt0->f.depth && !rt0->f.is_int;
    for(int pass=0;pass<2;pass++){
      for(int l=0;l<=maxl;l++) for(u32 y=0;y<d;y++){ Tex* r=tex_find_rt(gva+(u64)y*lsz+loff[l]);
        u32 cw=W0>>l, ch=H0>>l; if(!cw) cw=1; if(!ch) ch=1; u32 rw=SC(r->w), rh=SC(r->h); if(rw<cw) cw=rw; if(rh<ch) ch=rh;
        lvl_copy(r->id,c->id,target,l,(int)y,(GLsizei)cw,(GLsizei)ch,col); }
      /* sanity check the first few chains: center of level 1 must match its source RT. if the driver dropped the
         copy, switch to blits for good */
      static int nchk; if(pass || g_lvl_mode || !col || maxl<1 || nchk>=6) break; nchk++;
      Tex* r1=tex_find_rt(gva+loff[1]); u32 cw=W0>>1, ch=H0>>1; u32 rw=SC(r1->w), rh=SC(r1->h); if(rw<cw) cw=rw; if(rh<ch) ch=rh; if(!cw||!ch) break;
      float a4[4]={0}, b4[4]={0}; GLint px=(GLint)cw/2, py=(GLint)ch/2;
      lvl_read1(r1->id,GL_TEXTURE_2D,0,0,px,py,a4); lvl_read1(c->id,target,1,0,px,py,b4);
      int same=1; for(int k=0;k<4;k++){ float e=a4[k]-b4[k]; if(e<0) e=-e; float m=a4[k]<0? -a4[k] : a4[k]; if(e>0.02f+0.02f*m) same=0; }
      if(same) break;
      g_lvl_mode=1; LOG("gpu: mip level copy check failed in game (%.3f %.3f %.3f -> %.3f %.3f %.3f): copies switched to blits\n",a4[0],a4[1],a4[2],b4[0],b4[1],b4[2]); }
    /* mips the game didn't render itself: generate them from the last real one. SMO samples its env cubemaps
       at LOD 5 (diffuse) and roughness*5 (spec, RGBM); with only level 0 those get the sharp image. a box chain
       isn't the game's prefilter but it's way closer than nothing */
    int topl=maxl; static int nogen=-1; if(nogen<0) nogen=getenv("SWR_NO_CUBE_MIPGEN")!=NULL;
    if(maxl<(int)levels-1 && !nogen && !rt0->f.is_int && !rt0->f.depth && (target==GL_TEXTURE_CUBE_MAP || target==GL_TEXTURE_2D_ARRAY || target==GL_TEXTURE_2D)){
      lvl_gen(c->id,target,maxl,(int)levels,d,W0,H0); topl=(int)levels-1; mb_reset();
      static int said; if(said<8){ said++; LOG("gpu: %s %ux%u at %llx: %d of %u mips rendered by the game, the rest generated\n",target==GL_TEXTURE_CUBE_MAP? "cubemap":target==GL_TEXTURE_2D? "mip chain":"array",w,h,(unsigned long long)gva,maxl+1,levels); } }
    if((int)c->stable!=topl){ glBindTexture(target,c->id); glTexParameteri(target,GL_TEXTURE_MAX_LEVEL,topl); c->stable=(u32)topl; mb_reset(); }
    c->crop_gen=sig; st_lay_copy++; }
  c->last_used=g_frame; rt0->last_used=g_frame; return c; }
/* startup driver checks for the stuff the post-processing relies on. results go to the log as
   "gpu: driver check". each test writes a known color and reads it back */
static GLuint st_prog, st_samp, st_out, st_ofbo;

static int st_near(const float* a, const float* b, int n){ for(int i=0;i<n;i++){ float d=a[i]-b[i]; if(d<0) d=-d; if(d>0.03f || a[i]!=a[i]) return 0; } return 1; }

static void st_clear_level(GLuint tex, int level, const float* c){
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,g_dfbo); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,level);
  { GLenum b=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&b); } glClearBufferfv(GL_COLOR,0,c);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0); }

/* draw one pixel sampling tex at a fixed lod (min=max=lod, like the bloom passes) and return it */
static void st_sample(GLuint tex, float lod, float* out){
  glSamplerParameterf(st_samp,GL_TEXTURE_MIN_LOD,lod); glSamplerParameterf(st_samp,GL_TEXTURE_MAX_LOD,lod);
  glBindFramebuffer(GL_FRAMEBUFFER,st_ofbo); { GLenum b=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&b); } glViewport(0,0,1,1);
  glUseProgram(st_prog); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex); glBindSampler(0,st_samp);
  glBindVertexArray(g_scratch_vao); glDrawArrays(GL_TRIANGLES,0,3);
  glReadBuffer(GL_COLOR_ATTACHMENT0); float px[4]={-9,-9,-9,-9}; glReadPixels(0,0,1,1,GL_RGBA,GL_FLOAT,px); memcpy(out,px,16);
  glBindSampler(0,0); glUseProgram(0); }

/* mip 1 filled from a separate RT, then sampled at lod 1 */
static int st_mip_copy(GLenum ifmt, int mode, float* got){
  static const float c0[4]={0.1f,0.1f,0.1f,1}, c1[4]={0.5f,0.25f,0.75f,1};
  GLuint dst, src; glGenTextures(1,&dst); glBindTexture(GL_TEXTURE_2D,dst); glTexStorage2D(GL_TEXTURE_2D,3,ifmt,8,8); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
  glGenTextures(1,&src); glBindTexture(GL_TEXTURE_2D,src); glTexStorage2D(GL_TEXTURE_2D,1,ifmt,4,4); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
  st_clear_level(dst,0,c0); st_clear_level(dst,1,c0); st_clear_level(src,0,c1);
  int om=g_lvl_mode; g_lvl_mode=mode; lvl_copy(src,dst,GL_TEXTURE_2D,1,0,4,4,1); g_lvl_mode=om;
  glBindTexture(GL_TEXTURE_2D,dst); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,1);
  st_sample(dst,1.0f,got); glDeleteTextures(1,&dst); glDeleteTextures(1,&src);
  return st_near(got,c1,3); }

/* draw level 1, generate 2.., sample level 2 */
static int st_mip_gen(GLenum ifmt, float* got){
  static const float c0[4]={0.1f,0.1f,0.1f,1}, c1[4]={0.5f,0.25f,0.75f,1};
  GLuint t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t); glTexStorage2D(GL_TEXTURE_2D,3,ifmt,8,8); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
  st_clear_level(t,0,c0); st_clear_level(t,1,c1); st_clear_level(t,2,c0);
  lvl_gen(t,GL_TEXTURE_2D,1,3,1,8,8); glBindTexture(GL_TEXTURE_2D,t); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,2);
  st_sample(t,2.0f,got); glDeleteTextures(1,&t);
  return st_near(got,c1,3); }

/* shader writes negative values to an SNORM target */
static int st_snorm(GLenum ifmt, int n, float* got){
  static const char* fs="#version 430 core\nout vec4 o; void main(){ o=vec4(-0.5,0.25,-1.0,0.75); }";
  static const char* vs="#version 430 core\nvoid main(){ vec2 p=vec2(float((gl_VertexID&1)*4-1),float((gl_VertexID>>1)*4-1)); gl_Position=vec4(p,0.0,1.0); }";
  static GLuint pr; if(!pr){ GLuint a=compile(GL_VERTEX_SHADER,vs,NULL,0), b=compile(GL_FRAGMENT_SHADER,fs,NULL,0); if(a&&b) pr=link2(a,b,0); if(!pr) return 1; }
  GLuint t, fb; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t); glTexStorage2D(GL_TEXTURE_2D,1,ifmt,1,1);
  glGenFramebuffers(1,&fb); glBindFramebuffer(GL_FRAMEBUFFER,fb); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t,0);
  { GLenum b=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&b); }
  int ok=0; got[0]=got[1]=got[2]=got[3]=-9;
  if(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE){
    glViewport(0,0,1,1); glUseProgram(pr); glBindVertexArray(g_scratch_vao); glDrawArrays(GL_TRIANGLES,0,3);
    /* read back with a shader, glReadPixels can clamp fixed point to 0..1 */
    glUseProgram(0); glBindFramebuffer(GL_FRAMEBUFFER,0); st_sample(t,0.0f,got);
    static const float want[4]={-0.5f,0.25f,-1.0f,0.75f}; ok=st_near(got,want,n); }
  glUseProgram(0); glBindFramebuffer(GL_FRAMEBUFFER,0); glDeleteFramebuffers(1,&fb); glDeleteTextures(1,&t);
  return ok; }
int g_texoff_emul;

static void gl_selftest(void){
  /* non-constant texture offsets: NVIDIA takes them, the spec / AMD / Mesa don't */
  if(g_gles) g_texoff_emul=1;
  else { static const char* fs="#version 430 core\nlayout(binding=0) uniform sampler2D t; uniform int k; out vec4 o; void main(){ o=textureOffset(t,vec2(0.5),ivec2(bitfieldExtract(k,0,4),bitfieldExtract(k,4,4))); }";
    GLuint sh=compile(GL_FRAGMENT_SHADER,fs,NULL,0); g_texoff_emul= sh? 0 : 1; if(sh) glDeleteShader(sh);
    if(getenv("SWR_TEXOFF_EMUL")) g_texoff_emul=1;
    LOG("gpu: driver check: texture offsets from a register %s\n",g_texoff_emul? "refused by the driver: added to the coordinates instead" : "accepted"); }
  if(g_gles || !p_cis || getenv("SWR_NO_DRIVER_CHECK")) return;
  static const char* vs="#version 430 core\nvoid main(){ vec2 p=vec2(float((gl_VertexID&1)*4-1),float((gl_VertexID>>1)*4-1)); gl_Position=vec4(p,0.0,1.0); }";
  static const char* fs="#version 430 core\nlayout(binding=0) uniform sampler2D t; out vec4 o; void main(){ o=texture(t,vec2(0.5)); }";
  GLuint a=compile(GL_VERTEX_SHADER,vs,NULL,0), b=compile(GL_FRAGMENT_SHADER,fs,NULL,0); st_prog= a&&b? link2(a,b,0) : 0;
  if(!st_prog){ LOG("gpu: driver check skipped (test shader did not compile)\n"); return; }
  glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glColorMaski(0,1,1,1,1);
  glGenSamplers(1,&st_samp); glSamplerParameteri(st_samp,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR); glSamplerParameteri(st_samp,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  glSamplerParameteri(st_samp,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glSamplerParameteri(st_samp,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  glGenTextures(1,&st_out); glBindTexture(GL_TEXTURE_2D,st_out); glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA32F,1,1);
  glGenFramebuffers(1,&st_ofbo); glBindFramebuffer(GL_FRAMEBUFFER,st_ofbo); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,st_out,0);
  static const GLenum fmts[2]={GL_R11F_G11F_B10F,GL_RGBA16F}; static const char* fnm[2]={"R11G11B10F","RGBA16F"};
  float g[4]; int force=-1; { const char* e=getenv("SWR_LEVEL_COPY"); if(e) force=atoi(e); }
  /* copies into mip levels */
  int okm[2]={1,1};
  for(int m=0;m<2;m++) for(int k=0;k<2;k++){ int ok=st_mip_copy(fmts[k],m,g); okm[m]&=ok;
    LOG("gpu: driver check: mip level copy (%s, %s): %.2f %.2f %.2f -> %s\n",m? "blit" : "CopyImageSubData",fnm[k],g[0],g[1],g[2],ok? "ok" : "WRONG"); }
  g_lvl_mode= force>=0? (force? 1 : 0) : okm[0]? 0 : okm[1]? 1 : 0;
  /* mips generated from a level above 0 */
  int okg=1; for(int k=0;k<2;k++){ int ok=st_mip_gen(fmts[k],g); okg&=ok; LOG("gpu: driver check: mip generation from level 1 (%s): %.2f %.2f %.2f -> %s\n",fnm[k],g[0],g[1],g[2],ok? "ok" : "WRONG"); }
  if(!okg){ g_mipgen_manual=1; int okg2=1; for(int k=0;k<2;k++){ int ok=st_mip_gen(fmts[k],g); okg2&=ok; LOG("gpu: driver check: mip generation with blits (%s): %.2f %.2f %.2f -> %s\n",fnm[k],g[0],g[1],g[2],ok? "ok" : "WRONG"); }
    if(!okg2) g_mipgen_manual=0; }
  if(getenv("SWR_MIPGEN_BLIT")) g_mipgen_manual=1;
  /* SNORM render targets */
  int oks=st_snorm(GL_RGBA8_SNORM,4,g); LOG("gpu: driver check: SNORM render target: %.2f %.2f %.2f %.2f -> %s\n",g[0],g[1],g[2],g[3],oks? "ok" : "WRONG");
  if(!oks){ int okf=st_snorm(GL_RGBA16F,4,g); g_snorm_fb=okf; LOG("gpu: driver check: half float instead: %.2f %.2f %.2f %.2f -> %s\n",g[0],g[1],g[2],g[3],okf? "ok" : "WRONG"); }
  if(getenv("SWR_SNORM_FLOAT")) g_snorm_fb=1;
  LOG("gpu: driver check: mip level copies by %s, mip generation %s, SNORM targets %s\n",g_lvl_mode? "blit" : "CopyImageSubData",g_mipgen_manual? "with blits" : "by the driver",g_snorm_fb? "stored as half float" : "native");
  glDeleteFramebuffers(1,&st_ofbo); glDeleteTextures(1,&st_out); glDeleteProgram(st_prog);
  glBindFramebuffer(GL_FRAMEBUFFER,0); glBindVertexArray(0); glBindTexture(GL_TEXTURE_2D,0); glUseProgram(0);
  while(glGetError()!=GL_NO_ERROR){} }
/* texture for a TIC */
static Tex* tex_from_tic_(u32* tic);
extern u64 video_write_generation(void);
extern u64 video_surface_generation(const u8*,u64);
static struct { u64 video_gen; u32 gen, frame; u32 tic[8]; Tex* t; } g_tdc[512];
extern u64 g_draws;
static u64 st_tdc_hit, st_tdc_miss;

static Tex* tex_from_tic(u32* tic){
  u32 k=(tic[1]^(tic[1]>>9)^tic[0]^(tic[2]<<7)^tic[4]^(tic[5]<<3))&511;
  static int off=-1; if(off<0) off=getenv("SWR_NO_TEX_MEMO")!=NULL;
  u64 video_gen=video_write_generation();
  if(!off && g_tdc[k].video_gen==video_gen && g_tdc[k].gen==g_tmap_gen && g_tdc[k].frame==(u32)g_frame && !memcmp(g_tdc[k].tic,tic,32)){ st_tdc_hit++; return g_tdc[k].t; }
  Tex* t=tex_from_tic_(tic); st_tdc_miss++;
  /* tex_from_tic_ may have bumped the generation itself, cache under the new one */
  g_tdc[k].video_gen=video_gen; g_tdc[k].gen=g_tmap_gen; g_tdc[k].frame=(u32)g_frame; memcpy(g_tdc[k].tic,tic,32); g_tdc[k].t=t; return t; }

static Tex* tex_from_tic_(u32* tic){
  u64 gva=((u64)(tic[2]&0xFFFF)<<32)|tic[1];
  u32 hv=(tic[2]>>21)&7; int bl= hv==3||hv==4;
  u32 w=(tic[4]&0xFFFF)+1, h=(tic[5]&0xFFFF)+1, d=((tic[5]>>16)&0x3FFF)+1; u32 type=(tic[4]>>23)&0xF;
  int srgb=(tic[4]>>22)&1; u32 levels=((tic[3]>>28)&0xF)+1;
  u32 bh=(tic[3]>>3)&7, bd=(tic[3]>>6)&7, pitch=bl? 0 : ((tic[3]&0xFFFF)<<5);
  if(type==0 || type==4) h=1;
  if(type==3){ d=6; } else if(type==8){ d=d*6; }
  GlFmt f=tic_format(tic[0],srgb);
  /* garbage TIC (bad format, null address, silly size): return null instead of trying to upload
     hundreds of MB of junk */
  if(!f.bpp || !gva || w>16384 || h>16384 || d>2048 || levels>15){ return NULL; }
  /* a pitch shorter than a row (header version 0 texture buffers keep the width's high half there)
     would make the upload copy rows past the end of what it read */
  if(!bl){ u32 rowb=((w+f.bw-1)/f.bw)*f.bpp; if(pitch<rowb) pitch=rowb; }
  { u64 est=(u64)w*h*(type==2? d : (d>0? d : 1))*(f.compressed? 1 : f.bpp); if(est>(512ull<<20)) return NULL; }
  u32 fkey=(tic[0]&0x7FFFF)|(srgb<<31);
  /* there's an RT at that address, use it */
  Tex* rt=tex_find_rt(gva);
  if(rt && rt->frame_checked!=g_frame){ rt->frame_checked=g_frame; if(rt_sig(gva)!=rt->hash){ rt->gva=~0ull; rt=NULL; g_tmap_gen++; } }
  if(rt && rt->vol && rt->w==w && rt->h==h && d<=rt->vol->d && ((type==2 && rt->vol->target==GL_TEXTURE_3D) || ((type==5 || type==1) && rt->vol->target==GL_TEXTURE_2D_ARRAY))){
    rt->vol->last_used=g_frame; return rt->vol; }
  /* 3D TIC over a 2D RT: build from slices if they're all RTs, else the data is in memory */
  if(rt && type==2 && d>1 && !f.compressed && !f.astc){ Tex* v=vol_from_slices(rt,gva,w,h,d,f,bh,bd); if(v) return v; rt=NULL; }
  else if(rt && type==2 && d>1) rt=NULL;
  if(rt && !f.compressed && !f.astc && rt->target==GL_TEXTURE_2D && rt->w==w && rt->h==h && (type==3 || type==8 || (type==5 && d>1))){
    GLenum lt= type==3? GL_TEXTURE_CUBE_MAP : type==8? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_2D_ARRAY;
    Tex* lc=layered_from_rts(rt,gva,w,h,d,levels,lt,f,fkey,bl,bh,bd); if(lc) return lc; }
  /* mipped 2D TIC over an RT: the game renders each mip to its own RT and picks one via the sampler LOD clamp
     (SMO bloom: 160x180, 5 mips, read at lod 1..4). returning just level 0 makes every blur pass read the
     sharp image */
  if(rt && type==1 && levels>1 && d==1 && !f.compressed && !f.astc && rt->target==GL_TEXTURE_2D && rt->w==w && rt->h==h && !rt->f.depth && !getenv_cached_nomip()){
    Tex* lc=layered_from_rts(rt,gva,w,h,1,levels,GL_TEXTURE_2D,f,fkey,bl,bh,bd); if(lc) return lc; }
  if(rt && !f.compressed && !f.astc && rt->w>=w && rt->h>=h){ rt->last_used=g_frame; return (rt->w==w && rt->h==h)? rt : rt_crop(rt,w,h); }   /* smaller view -> just that region */
  GLenum target=tex_target(type,d);
  if(target==GL_TEXTURE_CUBE_MAP && d!=6) target=GL_TEXTURE_2D_ARRAY;
  if(f.astc && levels>1) levels=1;
  for(Tex* t=texh[hkey(gva)];t;t=t->next){
    if(t->gva==gva && t->w==w && t->h==h && t->d==d && t->fkey==fkey && t->target==target && !t->is_rt){
      t->last_used=g_frame;
      /* Video frames are explicit host writes; never delay them behind static-texture polling. */
      u64 vg=video_surface_generation(gmem_ptr(gva,1),t->size);
      if(vg){ if(vg!=t->video_gen){ tex_upload(t); t->video_gen=vg; t->frame_checked=g_frame; t->stable=0; } return t; }
      /* Diagnostic mode for gameplay: check CPU textures every frame without polling backoff. */
      if(getenv("SWR_TEXTURE_COHERENCE_TEST")){ if(t->frame_checked!=g_frame){ t->frame_checked=g_frame; u64 hh=mem_hash(gva,t->size); if(hh!=t->hash){ t->hash=hh; tex_upload(t); } } return t; }
      /* change check: frequent at first, backs off while the texture stays the same, capped in MB per frame */
      if(t->frame_checked!=g_frame && (i32)(g_frame-t->next_check)>=0){
        if(g_tex_bud_frame!=g_frame){ g_tex_bud_frame=g_frame; g_tex_bud=24u<<20; }
        /* long-stable: cheap sampled check every 20 frames, sampled-hash check every 120 */
        if(g_nosample<0) g_nosample=getenv("SWR_NO_SAMPLE_CHECK")!=NULL;
        if(!g_nosample && t->stable>=40 && t->size>=16384 && (i32)(g_frame-t->tfull)<120){
          u64 sh=sample_hash(gva,t->size); t->frame_checked=g_frame;
          if(sh==t->shash){ t->next_check=g_frame+20+((u32)(gva>>9)&7); return t; }
          t->stable=0; }
        if(g_tex_bud>0 || t->stable<4){ t->frame_checked=g_frame; g_tex_bud-=(i64)(t->size>>(t->size>65536? 1 : 0)); st_tex_chk+=t->size;
          u64 hh=mem_hash(gva,t->size); if(hh!=t->hash){ t->hash=hh; tex_upload(t); t->stable=0; } else if(t->stable<100000) t->stable++;
          if(t->stable>=39 && t->size>=16384){ t->shash=sample_hash(gva,t->size); t->tfull=g_frame; }
          t->next_check=g_frame+(t->stable<4? 4 : t->stable<12? 8 : 20)+((u32)(gva>>9)&3); } }
      return t; } }
  { /* debug: a texture that lives inside an RT is being read from memory */
    static int nd; if(nd<24 && g_verbose>=2){ for(int k=0;k<TEXH && nd<24;k++) for(Tex* q=texh[k];q;q=q->next){ if(!q->is_rt || q->gva==~0ull) continue;
        u64 rsz=(u64)q->w*q->h*(q->f.bpp? q->f.bpp : 4);
        if(gva>=q->gva && gva<q->gva+rsz){ nd++; LOG("gpu: texture %ux%u fmt %x at %llx falls inside render target %llx %ux%u (+%llx)%s: read from memory\n",w,h,tic[0]&0x7F,(unsigned long long)gva,
            (unsigned long long)q->gva,q->w,q->h,(unsigned long long)(gva-q->gva),gva==q->gva? " and is larger than it" : ""); break; } } } }
  Tex* t=tex_create(gva,w,h,d,levels,target,f,fkey,bl,bh,bd,pitch,0);
  u64 sz=0; u32 dd= target==GL_TEXTURE_3D? d : 1; for(u32 l=0;l<levels;l++) sz+=bl? bl_level_size(w,h,dd,&f,bh,bd,l,NULL) : (u64)pitch*((h+f.bh-1)/f.bh);
  if(target!=GL_TEXTURE_3D) sz*=d;
  t->size=sz; t->hash=mem_hash(gva,sz); t->frame_checked=g_frame; t->last_used=g_frame;
  tex_upload(t); t->video_gen=video_surface_generation(gmem_ptr(gva,1),sz);
  return t; }

/* ============================================================ samplers */
typedef struct { u32 tsc[8]; GLuint id; } Samp;
static Samp samps[1024]; static int nsamps;

static GLenum wrap(u32 m){ switch(m){ case 0: return GL_REPEAT; case 1: return GL_MIRRORED_REPEAT; case 2: case 4: return GL_CLAMP_TO_EDGE; case 3: case 6: return GL_CLAMP_TO_BORDER;
  case 5: case 7: return g_gles? GL_MIRRORED_REPEAT : GL_MIRROR_CLAMP_TO_EDGE; } return GL_REPEAT; }
static short samp_ht[4096];

static GLuint sampler_get(const u32* tsc){
  u32 hk=2166136261u; for(int i=0;i<8;i++){ hk^=tsc[i]; hk*=16777619u; } hk&=4095;
  for(u32 j=hk;;j=(j+1)&4095){ int e=samp_ht[j]; if(!e) break; if(!memcmp(samps[e-1].tsc,tsc,32)) return samps[e-1].id; }
  GLuint s; glGenSamplers(1,&s);
  glSamplerParameteri(s,GL_TEXTURE_WRAP_S,wrap(tsc[0]&7)); glSamplerParameteri(s,GL_TEXTURE_WRAP_T,wrap((tsc[0]>>3)&7)); glSamplerParameteri(s,GL_TEXTURE_WRAP_R,wrap((tsc[0]>>6)&7));
  u32 mag=tsc[1]&3, min=(tsc[1]>>4)&3, mip=(tsc[1]>>6)&3;
  glSamplerParameteri(s,GL_TEXTURE_MAG_FILTER,mag==2? GL_LINEAR : GL_NEAREST);
  GLenum mf= min==2? (mip==3? GL_LINEAR_MIPMAP_LINEAR : mip==2? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR) : (mip==3? GL_NEAREST_MIPMAP_LINEAR : mip==2? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
  glSamplerParameteri(s,GL_TEXTURE_MIN_FILTER,mf);
  if((tsc[0]>>9)&1){ glSamplerParameteri(s,GL_TEXTURE_COMPARE_MODE,GL_COMPARE_REF_TO_TEXTURE); glSamplerParameteri(s,GL_TEXTURE_COMPARE_FUNC,GL_NEVER+((tsc[0]>>10)&7)); }
  { /* anisotropy: the game's (TSC bits 20-22), raised to ini anisotropy= on trilinear samplers (floors and walls at an
       angle), never on depth-compare ones; the same rule as the native Vulkan renderer (gpu/vk/texture.c) */
    extern int g_opt_aniso; u32 aniso=(tsc[0]>>20)&7; float af= aniso? (float)(1<<aniso) : 1.0f;
    if(g_opt_aniso>1 && (float)g_opt_aniso>af && min==2 && mag==2 && mip>=2 && !((tsc[0]>>9)&1)){
      static float maxaf; if(maxaf==0){ GLint m=0; glGetIntegerv(0x84FF,&m); maxaf= m>=1? (float)m : 1.0f; }   /* GL_MAX_TEXTURE_MAX_ANISOTROPY */
      af= (float)g_opt_aniso<maxaf? (float)g_opt_aniso : maxaf; }
    if(af>1) glSamplerParameterf(s,0x84FE,af); }
  float minl=(float)(tsc[2]&0xFFF)/256.f, maxl=(float)((tsc[2]>>12)&0xFFF)/256.f; glSamplerParameterf(s,GL_TEXTURE_MIN_LOD,minl); glSamplerParameterf(s,GL_TEXTURE_MAX_LOD,maxl);
  i32 bias=(i32)((tsc[1]>>12)&0x1FFF); if(bias&0x1000) bias|=~0x1FFF; if(!g_gles) glSamplerParameterf(s,GL_TEXTURE_LOD_BIAS,(float)bias/256.f);
  float bc[4]; { u32 b4[4]; memcpy(b4,&tsc[4],16); if(b4[3]&0x80000000u){ b4[3]&=0x7FFFFFFFu; glSamplerParameteri(s,0x8A48,0x8A4A); } memcpy(bc,b4,16); } glSamplerParameterfv(s,GL_TEXTURE_BORDER_COLOR,bc);   /* tsc[7] bit 31 = skip sRGB decode */
  if(nsamps<1024){ memcpy(samps[nsamps].tsc,tsc,32); samps[nsamps].id=s; nsamps++; u32 j=hk; while(samp_ht[j]) j=(j+1)&4095; samp_ht[j]=(short)nsamps; }
  return s; }

/* ============================================================ shaders and programs */
extern int g_glsl_version;
struct WJob;
typedef struct Sh { u64 hash; u32 key2; int stage; GLuint gl; ShaderInfo si; struct Sh* next; int failed; char* src;
  GLuint sp; GLint uloc; int sps; struct WJob* sj; u32 clen; u64 qh; float lu[4]; int lu_ok; u64 skey; int xfb; } Sh;   /* sp: this shader's own separable program */
static Sh* shh[1024];
/* per-address cache. shader_hash walks the whole program (~50us per stage per draw on big shaders),
   within a submit a quick hash of the same range is enough */
typedef struct { u64 gva; u32 key2, epoch; int stage; Sh* s; } ShA;
static ShA sha[4096]; static u64 st_sh_fast;
static int g_sh_bind; static u32* g_last_r3d;
static int sh_cb_read(u32 idx, u32 off, u32* out);
static void sh_dump_cbs(u64 h);
static u64 shader_hash_ns,shader_translate_ns,shader_reuse_ns,shader_uniform_ns;
static u32 g_fs_rtk, g_fs_rtm, g_fs_rts;   /* FS integer output key */
static void aw_write(const char* path, const void* d, size_t n, int append);

/* common epilogue stuff: alpha test for FS, Y flip for VS */

#define TR_VERSION 0x5174u   /* bump when the GLSL output changes, invalidates caches */
#define TRANSLATED_PATH "shader_cache/translated_v5174.bin"
typedef struct {u32 idx,off,value;} TransDep;
typedef struct Translated {u64 key;ShaderInfo si;char*src;u32 ndep;TransDep dep[128];struct Translated*next;} Translated;
static Translated* translated[512];static u32 translated_count,translated_hits;
static TransDep trans_dep[128];static u32 trans_ndep;static int trans_overflow;
static int trans_cb_read(u32 idx,u32 off,u32*out){
  int ok=sh_cb_read(idx,off,out);/* Record failed reads too: their availability can change. */
  if(!ok){trans_overflow=1;return 0;}
  if(trans_ndep<128)trans_dep[trans_ndep++]=(TransDep){idx,off,*out};else trans_overflow=1;
  return ok;
}
static void translated_put(Translated*t){u32 b=(u32)(t->key^(t->key>>32))&511;t->next=translated[b];translated[b]=t;translated_count++;}
static void translated_load(void){
  FILE*f=fopen(TRANSLATED_PATH,"rb");if(!f)return;
  for(;;){u32 hd[5];u64 key;if(fread(hd,sizeof hd,1,f)!=1)break;
    if(hd[0]!=0x544c4753||hd[1]!=TR_VERSION||hd[2]!=sizeof(ShaderInfo)||hd[3]>(8u<<20)||hd[4]>128||fread(&key,8,1,f)!=1)break;
    Translated*t=calloc(1,sizeof*t);t->key=key;t->ndep=hd[4];t->src=malloc((size_t)hd[3]+1);
    if(fread(&t->si,sizeof t->si,1,f)!=1||fread(t->dep,sizeof(TransDep),t->ndep,f)!=t->ndep||fread(t->src,1,hd[3],f)!=hd[3]){free(t->src);free(t);break;}
    t->src[hd[3]]=0;translated_put(t);
  }fclose(f);if(translated_count)LOG("shaders: %u translated stages loaded before starting the game\n",translated_count);
}
static u64 translated_key(u64 hash,int stage,u32 key2,const ShaderInfo*input){
  extern int g_glsl_version,g_glsl_es,g_vs_layer_ext,g_rs_binding;
  /* Defaults preserve entries written by the first Windows cache build. */
  u64 config=(u32)(g_glsl_version^440)|((u64)(u32)g_glsl_es<<16)|((u64)(u32)(g_vs_layer_ext^1)<<20)|((u64)(u32)(g_rs_binding^83)<<24)|((u64)(getenv("SWR_NO_PRECISE_VS")!=NULL)<<56);
  u64 v[3]={hash,((u64)(u32)stage<<32)|key2,TR_VERSION};return fast_hash((const u8*)input,sizeof *input,0)^fast_hash((const u8*)v,sizeof v,0)^(config*0x9E3779B97F4A7C15ull);
}
static Translated*translated_get(u64 key){
  for(Translated*t=translated[(u32)(key^(key>>32))&511];t;t=t->next)if(t->key==key){
    int ok=1;for(u32 i=0;i<t->ndep;i++){u32 v;if(!sh_cb_read(t->dep[i].idx,t->dep[i].off,&v)||v!=t->dep[i].value){ok=0;break;}}
    if(ok){translated_hits++;return t;}
  }return NULL;
}
static void translated_save(u64 key,const ShaderInfo*si,const char*src){
  if(!src||trans_overflow)return;u32 n=(u32)strlen(src);Translated*t=calloc(1,sizeof*t);t->key=key;t->si=*si;t->src=malloc((size_t)n+1);memcpy(t->src,src,(size_t)n+1);t->ndep=trans_ndep;memcpy(t->dep,trans_dep,t->ndep*sizeof(TransDep));translated_put(t);
  u32 hd[5]={0x544c4753,TR_VERSION,sizeof(ShaderInfo),n,t->ndep};size_t bytes=sizeof hd+8+sizeof *si+t->ndep*sizeof(TransDep)+n;u8*d=malloc(bytes),*q=d;
  memcpy(q,hd,sizeof hd);q+=sizeof hd;memcpy(q,&key,8);q+=8;memcpy(q,si,sizeof *si);q+=sizeof *si;memcpy(q,t->dep,t->ndep*sizeof(TransDep));q+=t->ndep*sizeof(TransDep);memcpy(q,src,n);
  aw_write(TRANSLATED_PATH,d,bytes,1);free(d);
}

static char* sh_post(char* src, int stage){
  /* alpha test: discard before every return */
  if(stage==ST_FS && src && strstr(src,"out vec4 frag_color0")){ char* p=strstr(src,"void main(){"); if(p){ size_t pre=p-src;
      const char* fn="uniform vec4 swr_at;\nbool swr_atest(float a){ int f=int(swr_at.y); float r=swr_at.z;\n"
        "  if(f==0) return false; if(f==1) return a<r; if(f==2) return a==r; if(f==3) return a<=r; if(f==4) return a>r; if(f==5) return a!=r; if(f==6) return a>=r; return true; }\n";
      char* out=malloc(strlen(src)*2+strlen(fn)+64); memcpy(out,src,pre); out[pre]=0; strcat(out,fn); char* o=out+strlen(out); const char* q2=p;
      while(*q2){ if(!strncmp(q2,"return;",7)){ o+=sprintf(o,"{ if(swr_at.x!=0.0 && !swr_atest(frag_color0.w)) discard; return; }"); q2+=7; } else *o++=*q2++; }
      *o=0; free(src); src=out; } }
  /* Y flip */
  if(stage==ST_VS && src){ char* p=strstr(src,"void main(){"); if(p){ size_t pre=p-src; char* ns=malloc(strlen(src)+512);
      memcpy(ns,src,pre); ns[pre]=0; strcat(ns,"uniform vec4 swr_vp;\n"); strcat(ns,p); free(src); src=ns;
      /* patch every return */
      char* out=malloc(strlen(src)*3+64); char* o=out; const char* q2=src;
      while(*q2){ if(!strncmp(q2,"return;",7)){ o+=sprintf(o,"{ gl_Position.y *= swr_vp.x; if(swr_vp.y!=0.0) gl_Position.z = gl_Position.z*2.0 - gl_Position.w; return; }"); q2+=7; } else *o++=*q2++; }
      *o=0; free(src); src=out; } }
  return src; }

static Sh* shader_get(u64 gva_code, int stage, u32 attr_int, u32 attr_sint){
  st_shaders++; g_sh_bind= stage==ST_VS? 0 : 4; g_shader_cb_read=sh_cb_read; /* BRX table is only used to mark targets, length doesn't change */
  u32 key2= stage==ST_VS? (attr_int^(attr_sint*31)) : (attr_int*0x9E3779B1u ^ attr_sint*0x85EBCA6Bu ^ g_fs_rtk);
  ShA* ca=&sha[(u32)((gva_code>>6)^(gva_code>>18)^((u64)key2*0x9E3779B1u)^((u32)stage<<11))&4095];
  if(ca->s && ca->gva==gva_code && ca->stage==stage && ca->key2==key2){
    if(ca->epoch==g_submit_epoch){ st_sh_fast++; return ca->s; }
    Sh* s=ca->s; u8* p=gmem_ptr(gva_code,s->clen); u64 q;
    if(p) q=fast_hash(p,s->clen,0); else { u8* t2=malloc(s->clen? s->clen : 1); gmem_read(gva_code,t2,s->clen); q=fast_hash(t2,s->clen,0); free(t2); }
    if(q==s->qh){ ca->epoch=g_submit_epoch; st_sh_fast++; return s; } }
  u8* code=gmem_ptr(gva_code,0x10000); u8* tmp=NULL;
  if(!code){ tmp=malloc(0x10000); gmem_read(gva_code,tmp,0x10000); code=tmp; }
  u64 hash_start=plat_ns();u32 len; u64 h=shader_hash(code,0x10000,&len);shader_hash_ns+=plat_ns()-hash_start;
  for(Sh* s=shh[h&1023];s;s=s->next) if(s->hash==h && s->stage==stage && s->key2==key2){
    ca->gva=gva_code; ca->key2=key2; ca->stage=stage; ca->epoch=g_submit_epoch; ca->s=s; free(tmp); return s; }
  Sh* s=calloc(1,sizeof *s); s->hash=h; s->stage=stage; s->key2=key2; st_sh_new++;
  s->clen=len; s->qh=fast_hash(code,len,0);
  ca->gva=gva_code; ca->key2=key2; ca->stage=stage; ca->epoch=g_submit_epoch; ca->s=s;
  if(stage==ST_FS){ s->si.tex_int_mask=attr_int; s->si.tex_sint_mask=attr_sint; s->si.rt_int_mask=g_fs_rtm; s->si.rt_sint_mask=g_fs_rts; }
  else { s->si.attr_int_mask=attr_int; s->si.attr_sint_mask=attr_sint; }
  u64 trans_start=plat_ns();u64 trans_key=translated_key(h,stage,key2,&s->si);Translated*cached=translated_get(trans_key);
  char*src=NULL;trans_ndep=0;trans_overflow=0;
  if(cached){s->si=cached->si;src=malloc(strlen(cached->src)+1);strcpy(src,cached->src);}
  else {g_shader_cb_read=trans_cb_read;src=shader_to_glsl(code,0x10000,stage,&s->si);g_shader_cb_read=sh_cb_read;}
  if(cached)shader_reuse_ns+=plat_ns()-trans_start;else shader_translate_ns+=plat_ns()-trans_start;
  /* dump shaders with indirect branches (code + cbufs) for offline analysis */
  { extern int g_opt_diag; if(g_opt_diag && src && strstr(src,"uint t_ = uint(")){ static int nb; if(nb<24){ nb++; plat_mkdir("captures"); plat_mkdir("captures/failed"); char fp[200];
      snprintf(fp,sizeof fp,"captures/failed/%016llx_%s.bin",(unsigned long long)h,stage==ST_VS?"vs":"fs"); FILE* f=fopen(fp,"wb"); if(f){ fwrite(code,1,0x10000,f); fclose(f); }
      sh_dump_cbs(h);
      u32* rr=g_last_r3d; if(rr){ snprintf(fp,sizeof fp,"captures/failed/%016llx_stages.txt",(unsigned long long)h); f=fopen(fp,"w");
        if(f){ for(int k=0;k<6;k++) fprintf(f,"slot %d: cfg=%08x offset=%08x\n",k,rr[0x800+k*0x10],rr[0x801+k*0x10]); fclose(f); } } } } }
  if(stage!=ST_FS) s->si.attr_sint_mask=attr_sint;
  if(s->si.unsupported) WARN("shader %016llx: %d untranslated instructions (first: %s)\n",(unsigned long long)h,s->si.unsupported,s->si.first_unsupported);
  if(s->si.unsupported && strstr(s->si.first_unsupported,"untracked")){ static int nd; if(nd<8){ nd++; char fp[200]; plat_mkdir("captures"); plat_mkdir("captures/failed");
      snprintf(fp,sizeof fp,"captures/failed/%016llx_%s.bin",(unsigned long long)h,stage==ST_VS?"vs":"fs"); aw_write(fp,code,0x10000,0);
      LOG("shader %016llx saved to %s (send it so the missing texture can be analyzed)\n",(unsigned long long)h,fp); } }
  if(!cached){src=sh_post(src,stage);translated_save(trans_key,&s->si,src);}
  GLenum ty= stage==ST_VS? GL_VERTEX_SHADER : stage==ST_FS? GL_FRAGMENT_SHADER : stage==ST_GS? GL_GEOMETRY_SHADER : stage==ST_CS? GL_COMPUTE_SHADER : stage==ST_TCS? GL_TESS_CONTROL_SHADER : GL_TESS_EVALUATION_SHADER;
  (void)ty; if(!src) s->failed=1;
  if(stage==ST_VS) src=dlg_vs_xfb(src);   /* DLAA: positions captured for motion vectors */
  s->src=src; src=NULL;   /* only compiled if it isn't in the disk cache */
  s->xfb= stage==ST_VS && s->src && strstr(s->src,"xfb_offset")!=NULL;
  { static int dump=-1; if(dump<0){ dump=getenv("SWR_DUMP_SHADERS")!=NULL; } if(dump && s->src){ char pth[256]; snprintf(pth,sizeof pth,"shader_%016llx_%d.glsl",(unsigned long long)h,stage); FILE* f=fopen(pth,"w"); if(f){ fputs(s->src,f); fclose(f);} } }
  free(src); free(tmp);
  s->next=shh[h&1023]; shh[h&1023]=s; return s; }

/* deferred compile + on-disk program binaries */
static int sh_ensure(Sh* s){
  if(s->gl) return 1; if(s->failed || !s->src) return 0;
  GLenum ty= s->stage==ST_VS? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER; char err[4096]; err[0]=0;
  s->gl=compile(ty,s->src,err,sizeof err);
  if(!s->gl){ s->failed=1; WARN("shader %016llx (stage %d) failed to compile:\n%s\n",(unsigned long long)s->hash,s->stage,err); if(g_verbose>=2) WARN("---- GLSL ----\n%s\n",s->src); return 0; }
  return 1; }
static int g_pcache=-1; static char g_pc_tag[64];
static volatile u64 g_cmp_ns, g_cmp_n, g_cmp_max, g_bin_n, g_bin_ns;
static void aw_write(const char*,const void*,size_t,int);
typedef struct WarmStage { u64 key; GLuint program; GLint uloc; struct WarmStage* next; } WarmStage;
static WarmStage* warm_stages[512];static u32 warm_stage_count,warm_stage_used;
static void warm_stage_put(u64 key,GLuint program,GLint uloc){
  u32 h=(u32)(key^(key>>32))&511;WarmStage*w=malloc(sizeof *w);
  w->key=key;w->program=program;w->uloc=uloc;w->next=warm_stages[h];warm_stages[h]=w;warm_stage_count++;
}
static GLuint warm_stage_take(u64 key,GLint*uloc){
  u32 h=(u32)(key^(key>>32))&511;
  for(WarmStage*w=warm_stages[h];w;w=w->next)if(w->key==key&&w->program){GLuint p=w->program;*uloc=w->uloc;w->program=0;warm_stage_used++;return p;}
  return 0;
}
/* Shared by synchronous and background paths: metrics must count both. */
static GLuint binary_load(const char*path,int separable){
  u64 start=plat_ns();FILE*f=fopen(path,"rb");if(!f)return 0;
  u32 format=0,length=0;GLuint p=0;GLint ok=0;
  if(fread(&format,4,1,f)==1&&fread(&length,4,1,f)==1&&length&&length<(64u<<20)){
    void*bytes=malloc(length);
    if(bytes&&fread(bytes,1,length,f)==length){p=glCreateProgram();if(separable)p_glProgramParameteri(p,0x8258,GL_TRUE);
      p_glProgramBinary(p,format,bytes,(GLsizei)length);glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){glDeleteProgram(p);p=0;}}
    free(bytes);
  }fclose(f);
  if(ok){__atomic_add_fetch(&g_bin_n,1,__ATOMIC_RELAXED);__atomic_add_fetch(&g_bin_ns,plat_ns()-start,__ATOMIC_RELAXED);}
  else while(glGetError()!=GL_NO_ERROR){}
  return p;
}
/* Disk writes are owned by the writer thread, including program binaries. */
static void binary_save(GLuint p,u64 key,int separable){
  if(g_pcache<=0)return;GLint length=0;glGetProgramiv(p,GL_PROGRAM_BINARY_LENGTH,&length);if(length<=0)return;
  u8*bytes=malloc((size_t)length+8);if(!bytes)return;GLenum format=0;GLsizei got=0;
  p_glGetProgramBinary(p,length,&got,&format,bytes+8);
  if(got>0){u32 fm=format,n=(u32)got;memcpy(bytes,&fm,4);memcpy(bytes+4,&n,4);char path[160];
    snprintf(path,sizeof path,"shader_cache/%s_%s%016llx.bin",g_pc_tag,separable?"s":"",(unsigned long long)key);
    aw_write(path,bytes,(size_t)got+8,0);}
  free(bytes);
}



/* binary cache key is the GLSL text, so a new exe that emits the same source reuses old binaries */
static u64 src_key(Sh* s){ if(!s->skey){ u64 h= s->src? fast_hash((const u8*)s->src,strlen(s->src),0) : s->hash; s->skey=(h^((u64)(s->stage+1)<<58))|1; } return s->skey; }

static u64 prog_key(Sh* vs, Sh* fs){ return src_key(vs)*0x9E3779B97F4A7C15ull ^ src_key(fs)*0xC2B2AE3D27D4EB4Full ^ 0x5E9A0001ull; }

static u64 sh_key(Sh* s){ return src_key(s)*0x9E3779B97F4A7C15ull ^ 0x5E9A0000ull; }
static int g_par=-1;

static void pcache_init(void){
  if(g_pcache<0){ g_pcache= p_glProgramBinary && p_glGetProgramBinary && p_glProgramParameteri; if(g_pcache){ plat_mkdir("shader_cache");
      const char* rn=(const char*)glGetString(GL_RENDERER); const char* vv=(const char*)glGetString(GL_VERSION); u64 h=1469598103934665603ull;
      for(const char* q=rn;q&&*q;q++){ h^=(u8)*q; h*=1099511628211ull; } for(const char* q=vv;q&&*q;q++){ h^=(u8)*q; h*=1099511628211ull; }
      snprintf(g_pc_tag,sizeof g_pc_tag,"%08x",(u32)(h^(h>>32))); } } }
static void pack_add(u64 key, Sh* vs, Sh* fs);
static void pack_add_sh(u64 key, Sh* s);

static int sh_start(Sh* s){ if(s->gl) return 1; if(s->failed || !s->src) return 0;
  s->gl=glCreateShader(s->stage==ST_VS? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER); const char* src=s->src; glShaderSource(s->gl,1,&src,NULL); glCompileShader(s->gl); return 1; }
static GLuint prog_finish(Sh* vs, Sh* fs, GLuint p);
static u64 prog_key(Sh* vs, Sh* fs);
static int prog_cached(Sh* vs, Sh* fs);

static GLuint prog_build(Sh* vs, Sh* fs, int* pending){
  if(!vs || !fs || vs->failed || fs->failed) return 0;
  pcache_init();
  char path[160]; u64 key=prog_key(vs,fs);
  if(g_pcache){ snprintf(path,sizeof path,"shader_cache/%s_%016llx.bin",g_pc_tag,(unsigned long long)key);
    GLuint p=binary_load(path,0);if(p){pack_add(key,vs,fs);return p;} }
  if(g_par>0 && pending){ /* parallel compile: kick it off, poll on later draws */
    if(!sh_start(vs) || !sh_start(fs)) return 0;
    GLuint p=glCreateProgram(); glAttachShader(p,vs->gl); glAttachShader(p,fs->gl);
    if(g_pcache) p_glProgramParameteri(p,GL_PROGRAM_BINARY_RETRIEVABLE_HINT,GL_TRUE);
    glLinkProgram(p); *pending=1; return p; }
  if(!sh_ensure(vs) || !sh_ensure(fs)) return 0;
  GLuint p=glCreateProgram(); glAttachShader(p,vs->gl); glAttachShader(p,fs->gl);
  if(g_pcache) p_glProgramParameteri(p,GL_PROGRAM_BINARY_RETRIEVABLE_HINT,GL_TRUE);
  glLinkProgram(p); return prog_finish(vs,fs,p); }

static GLuint prog_finish(Sh* vs, Sh* fs, GLuint p){
  char path[160]; u64 key=prog_key(vs,fs); if(g_pcache) snprintf(path,sizeof path,"shader_cache/%s_%016llx.bin",g_pc_tag,(unsigned long long)key);
  GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
  if(!ok){ Sh* ss[2]={vs,fs}; for(int k=0;k<2;k++){ GLint cok=1; glGetShaderiv(ss[k]->gl,GL_COMPILE_STATUS,&cok);
      if(!cok){ char e[4096]; glGetShaderInfoLog(ss[k]->gl,sizeof e,NULL,e); WARN("shader %016llx (stage %d) failed to compile:\n%s\n",(unsigned long long)ss[k]->hash,ss[k]->stage,e); ss[k]->failed=1; } } }
  if(!ok){ char log[2048]; glGetProgramInfoLog(p,2048,NULL,log); WARN("GL link: %s\n",log); glDeleteProgram(p);
    static int nd; if(nd<12 && vs->src && fs->src){ nd++; plat_mkdir("captures"); plat_mkdir("captures/failed"); char fp[200];
      snprintf(fp,sizeof fp,"captures/failed/%016llx_vs.glsl",(unsigned long long)vs->hash); FILE* f=fopen(fp,"w"); if(f){ fputs(vs->src,f); fclose(f); }
      snprintf(fp,sizeof fp,"captures/failed/%016llx_fs.glsl",(unsigned long long)fs->hash); f=fopen(fp,"w"); if(f){ fputs(fs->src,f); fclose(f); } }
    return 0; }
  pack_add(key,vs,fs);
  binary_save(p,key,0);
  return p; }

/* ============================================================ shader compile thread
   second shared context that compiles + links, so the GPU thread never blocks on the driver compiler.
   draws whose program isn't ready yet get skipped for a few frames */
static int prog_cached(Sh* vs, Sh* fs){ pcache_init(); if(g_pcache<=0) return 0; char path[160]; snprintf(path,sizeof path,"shader_cache/%s_%016llx.bin",g_pc_tag,(unsigned long long)prog_key(vs,fs)); FILE* f=fopen(path,"rb"); if(!f) return 0; fclose(f); return 1; }
typedef struct WJob { const char* vs; const char* fs; char* own_vs; char* own_fs; u64 key; volatile int state; GLuint prog; int keep; int sep; GLenum stype; char log[1536]; int try_bin; char bin_path[160]; } WJob;
#define WQN 8192
typedef struct { WJob* volatile e[WQN]; volatile u32 h, t; } WRing;
static WRing wq_hi, wq_lo; static volatile int g_worker;   /* 0 not started, 1 running, -1 unavailable */
static int wr_push(WRing* r, WJob* j){ u32 t=__atomic_load_n(&r->t,__ATOMIC_RELAXED), h=__atomic_load_n(&r->h,__ATOMIC_ACQUIRE); if(t-h>=WQN) return 0;
  r->e[t%WQN]=j; __atomic_store_n(&r->t,t+1,__ATOMIC_RELEASE); return 1; }

static WJob* wr_pop(WRing* r){ u32 h=__atomic_load_n(&r->h,__ATOMIC_RELAXED), t=__atomic_load_n(&r->t,__ATOMIC_ACQUIRE); if(h==t) return NULL;
  WJob* j=r->e[h%WQN]; __atomic_store_n(&r->h,h+1,__ATOMIC_RELEASE); return j; }
extern int backend_gl_worker_current(void);

static void shader_worker(void* arg){ plat_diag_role("shader-worker");
  (void)arg; if(!backend_gl_worker_current()){ __atomic_store_n(&g_worker,-1,__ATOMIC_RELEASE); return; }
  __atomic_store_n(&g_worker,1,__ATOMIC_RELEASE);
  { extern int g_opt_cmp_low; if(g_opt_cmp_low) plat_thread_low_priority(); }   /* only use the CPU the game isn't using */
  for(;;){
    WJob* j=wr_pop(&wq_hi); int lo=0; if(!j){ j=wr_pop(&wq_lo); lo=1; }
    if(!j){ plat_sleep_ns(1000000); continue; }
    u64 tc0=plat_ns();
    if(j->try_bin){GLuint p=binary_load(j->bin_path,j->sep);if(p){glFinish();j->prog=p;__atomic_store_n(&j->state,1,__ATOMIC_RELEASE);continue;}}
    GLuint a=glCreateShader(j->sep? j->stype : GL_VERTEX_SHADER), b= j->sep? 0 : glCreateShader(GL_FRAGMENT_SHADER);
    const char* s1=j->vs; const char* s2=j->fs; glShaderSource(a,1,&s1,NULL); glCompileShader(a); if(b){ glShaderSource(b,1,&s2,NULL); glCompileShader(b); }
    GLuint p=glCreateProgram(); glAttachShader(p,a); if(b) glAttachShader(p,b);
    if(j->sep) p_glProgramParameteri(p,0x8258 /* GL_PROGRAM_SEPARABLE */,GL_TRUE);
    if(g_pcache>0) p_glProgramParameteri(p,GL_PROGRAM_BINARY_RETRIEVABLE_HINT,GL_TRUE);
    glLinkProgram(p); GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ GLint c=1; char e[1024]; e[0]=0; glGetShaderiv(a,GL_COMPILE_STATUS,&c); if(!c){ glGetShaderInfoLog(a,sizeof e,NULL,e); snprintf(j->log,sizeof j->log,"%s: %s",j->sep? (j->stype==GL_VERTEX_SHADER? "VS" : "FS") : "VS",e); }
      else if(b && (glGetShaderiv(b,GL_COMPILE_STATUS,&c),!c)){ glGetShaderInfoLog(b,sizeof e,NULL,e); snprintf(j->log,sizeof j->log,"FS: %s",e); } else glGetProgramInfoLog(p,sizeof j->log,NULL,j->log);
      glDeleteProgram(p); p=0; }
    else binary_save(p,j->key,j->sep);
    glDeleteShader(a); if(b) glDeleteShader(b);   /* flagged for deletion, freed together with the program */
    if(p && !j->keep){ glDeleteProgram(p); p=0; }
    glFinish();
    { u64 dt=plat_ns()-tc0; __atomic_add_fetch(&g_cmp_ns,dt,__ATOMIC_RELAXED); __atomic_add_fetch(&g_cmp_n,1,__ATOMIC_RELAXED); if(dt>g_cmp_max) g_cmp_max=dt; }
    j->prog=p; __atomic_store_n(&j->state, ok? 1 : 2, __ATOMIC_RELEASE);
    if(lo) plat_sleep_ns(2000000);   /* background work yields to the game */
  } }

static void worker_start(void){
  if(getenv("SWR_SYNC_SHADERS")){ g_worker=-1; return; }
  if(plat_thread(shader_worker,NULL,8u<<20)){ g_worker=-1; return; }
  u64 t0=plat_ns(); while(!__atomic_load_n(&g_worker,__ATOMIC_ACQUIRE) && plat_ns()-t0<2000000000ull) plat_sleep_ns(1000000);
  if(g_worker!=1) g_worker=-1;
  LOG("shaders: compiler thread %s\n",g_worker==1? "active (shaders compile in the background without freezing the game)" : "not available (shaders compile on the GPU thread)"); }
/* ============================================================ separable programs
   one program per shader stage, compiled once; draws just bind the stages instead of linking every
   vs+fs combination */
static int g_sep=-1; static GLuint g_pipe; static int g_pipe_on; static GLuint g_pipe_vs, g_pipe_fs;
typedef void (APIENTRY*PFN_UPS_)(GLuint,GLbitfield,GLuint); typedef void (APIENTRY*PFN_BPP_)(GLuint); typedef void (APIENTRY*PFN_GPP_)(GLsizei,GLuint*);
static PFN_UPS_ p_ups; static PFN_BPP_ p_bpp;

static void sh_uloc(Sh* s){u64 start=plat_ns(); s->uloc= s->sp? glGetUniformLocation(s->sp, s->stage==ST_VS? "swr_vp" : "swr_at") : -1;shader_uniform_ns+=plat_ns()-start; }

static int sh_sep_ready(Sh* s){
  if(s->sps>=2) return 1;
  if(s->sps==1){ int st=__atomic_load_n(&s->sj->state,__ATOMIC_ACQUIRE); if(!st) return 0;
    struct WJob* j=s->sj; s->sj=NULL;
    if(st==1){ s->sp=j->prog; s->sps=2; pack_add_sh(j->key,s); }
    else { s->sp=0; s->sps=3; WARN("shader %016llx (stage %d) failed to compile: %s\n",(unsigned long long)s->hash,s->stage,j->log);
      static int nd; if(nd<16 && s->src){ nd++; plat_mkdir("captures"); plat_mkdir("captures/failed"); char fp[200];
        snprintf(fp,sizeof fp,"captures/failed/%016llx_%s.glsl",(unsigned long long)s->hash,s->stage==ST_VS?"vs":"fs"); FILE* f=fopen(fp,"w"); if(f){ fputs(s->src,f); fclose(f); } } }
    free(j); sh_uloc(s); return 1; }
  s->sps=3; if(s->failed || !s->src) return 1;
  u64 key=sh_key(s); pcache_init();
  {GLuint p=warm_stage_take(key,&s->uloc);if(p){s->sp=p;s->sps=2;return 1;}}

  if(g_worker==1){ /* disk binary or compile, both on the compile thread */
    struct WJob* j=calloc(1,sizeof *j); j->vs=s->src; j->key=key; j->keep=1; j->sep=1; j->stype= s->stage==ST_VS? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    if(g_pcache>0){ j->try_bin=1; snprintf(j->bin_path,sizeof j->bin_path,"shader_cache/%s_s%016llx.bin",g_pc_tag,(unsigned long long)key); }
    if(wr_push(&wq_hi,j)){ s->sj=j; s->sps=1; return 0; }
    free(j); }
  if(g_pcache>0){char path[160];snprintf(path,sizeof path,"shader_cache/%s_s%016llx.bin",g_pc_tag,(unsigned long long)key);
    GLuint p=binary_load(path,1);if(p){s->sp=p;s->sps=2;sh_uloc(s);pack_add_sh(key,s);return 1;}}
  struct WJob* j=calloc(1,sizeof *j); j->vs=s->src; j->key=key; j->keep=1; j->sep=1; j->stype= s->stage==ST_VS? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
  if(g_worker==1 && wr_push(&wq_hi,j)){ s->sj=j; s->sps=1; return 0; }
  /* no compile thread, do it inline */
  u64 inline_start=plat_ns();
  GLuint a=glCreateShader(j->stype); const char* s1=s->src; glShaderSource(a,1,&s1,NULL); glCompileShader(a);
  GLuint p=glCreateProgram(); glAttachShader(p,a); p_glProgramParameteri(p,0x8258,GL_TRUE); if(g_pcache>0) p_glProgramParameteri(p,GL_PROGRAM_BINARY_RETRIEVABLE_HINT,GL_TRUE);
  glLinkProgram(p); glDeleteShader(a); GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
  if(!ok){ glGetProgramInfoLog(p,sizeof j->log,NULL,j->log); WARN("shader %016llx failed to compile: %s\n",(unsigned long long)s->hash,j->log); glDeleteProgram(p); p=0; }
  else binary_save(p,key,1);
  {u64 dt=plat_ns()-inline_start;__atomic_add_fetch(&g_cmp_ns,dt,__ATOMIC_RELAXED);__atomic_add_fetch(&g_cmp_n,1,__ATOMIC_RELAXED);if(dt>g_cmp_max)g_cmp_max=dt;}
  free(j); s->sp=p; s->sps= p? 2 : 3; sh_uloc(s); if(p) pack_add_sh(key,s); return 1; }

static void sep_init(void){
  g_sep=0; if(g_gles || getenv("SWR_NO_SEP") || !p_glProgramParameteri) goto out;
  p_ups=(PFN_UPS_)backend_gl_proc("glUseProgramStages"); p_bpp=(PFN_BPP_)backend_gl_proc("glBindProgramPipeline"); PFN_GPP_ gpp=(PFN_GPP_)backend_gl_proc("glGenProgramPipelines");
  if(!p_ups || !p_bpp || !gpp) goto out;
  gpp(1,&g_pipe); if(g_pipe) g_sep=1;
out: LOG("shaders: %s\n",g_sep? "one program per shader (each shader is compiled only once)" : "one program per combination"); }
typedef struct Prog { Sh* vs; Sh* fs; GLuint p; GLint vp_loc, at_loc; int pending; WJob* job; struct Prog* next; } Prog;
static Prog* progs[512];

static Prog* program_get(Sh* vs, Sh* fs){
  u32 k=(u32)((((size_t)vs)>>4)^(((size_t)fs)>>6))&511;
  for(Prog* p=progs[k];p;p=p->next) if(p->vs==vs && p->fs==fs) return p;
  Prog* p=calloc(1,sizeof *p); p->vs=vs; p->fs=fs;
  if(g_worker==1 && vs && fs && !vs->failed && !fs->failed && vs->src && fs->src && !prog_cached(vs,fs)){
    WJob* j=calloc(1,sizeof *j); j->vs=vs->src; j->fs=fs->src; j->key=prog_key(vs,fs); j->keep=1;
    if(wr_push(&wq_hi,j)){ p->job=j; p->next=progs[k]; progs[k]=p; return p; } free(j); }
  p->p=prog_build(vs,fs,&p->pending);
  if(!p->pending){ p->vp_loc= p->p? glGetUniformLocation(p->p,"swr_vp") : -1; p->at_loc= p->p? glGetUniformLocation(p->p,"swr_at") : -1; }
  p->next=progs[k]; progs[k]=p; return p; }

/* 1 = ready, 0 = driver still compiling (skip the draw) */
static int program_ready(Prog* p){
  if(p->job){ int s=__atomic_load_n(&p->job->state,__ATOMIC_ACQUIRE); if(!s) return 0;
    WJob* j=p->job; p->job=NULL;
    if(s==1){ p->p=j->prog; pack_add(j->key,p->vs,p->fs); p->vp_loc=glGetUniformLocation(p->p,"swr_vp"); p->at_loc=glGetUniformLocation(p->p,"swr_at"); }
    else { p->p=0; WARN("GL link (thread): %s\n",j->log);
      static int nd; if(nd<12){ nd++; plat_mkdir("captures"); plat_mkdir("captures/failed"); char fp[200];
        snprintf(fp,sizeof fp,"captures/failed/%016llx_vs.glsl",(unsigned long long)p->vs->hash); FILE* f=fopen(fp,"w"); if(f){ fputs(p->vs->src,f); fclose(f); }
        snprintf(fp,sizeof fp,"captures/failed/%016llx_fs.glsl",(unsigned long long)p->fs->hash); f=fopen(fp,"w"); if(f){ fputs(p->fs->src,f); fclose(f); } } }
    free(j); return 1; }
  if(!p->pending) return 1;
  GLint done=0; glGetProgramiv(p->p,0x91B1 /* GL_COMPLETION_STATUS_KHR */,&done); if(!done) return 0;
  p->pending=0; p->p=prog_finish(p->vs,p->fs,p->p);
  p->vp_loc= p->p? glGetUniformLocation(p->p,"swr_vp") : -1; p->at_loc= p->p? glGetUniformLocation(p->p,"swr_at") : -1; return 1; }

/* ============================================================ framebuffers */
static GLuint g_fbo, g_vao, g_vbuf[32], g_ibuf, g_ubuf[5][18], g_scratch_vao;
typedef struct { u64 cb[5][18]; u32 cbsize[5][18]; } CbState;
static CbState g_cb;

static void sh_dump_cbs(u64 h){ char fp[200]; FILE* f;
      for(int ci=0;ci<18;ci++){ u64 ca=g_cb.cb[g_sh_bind][ci]; if(!ca) continue; u32 csz=g_cb.cbsize[g_sh_bind][ci]; if(!csz||csz>0x10000) csz=0x10000;
        u8* cbb=malloc(csz); gmem_read(ca,cbb,csz); snprintf(fp,sizeof fp,"captures/failed/%016llx_cb%d.bin",(unsigned long long)h,ci); f=fopen(fp,"wb"); if(f){ fwrite(cbb,1,csz,f); fclose(f); } free(cbb); }
}

static int sh_cb_read(u32 idx, u32 off, u32* out){ if(idx>=18) return 0; u64 a=g_cb.cb[g_sh_bind][idx]; u32 sz=g_cb.cbsize[g_sh_bind][idx];
  if(!a || off+4>(sz? sz : 65536)) return 0; gmem_read(a+off,out,4); return 1; }
static Tex* g_rt[8]; static Tex* g_zt; static int g_nrt;

/* RT memory signature; if the CPU or DMA overwrote it something else lives there now */
static Tex* rt_texture(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth){
  GlFmt f= rt_format(fmt);
  u32 fkey=fmt|(depth? 0x10000 : 0);
  for(Tex* t=texh[hkey(gva)];t;t=t->next) if(t->gva==gva && t->is_rt && t->w==w && t->h==h && t->fkey==fkey){ t->last_used=g_frame; return t; }
  /* games reuse the same memory for RTs of different size/format within a frame (SMO does every frame).
     instead of destroying the old one, park it as an alias and reuse it when that size comes back.
     recreating meant a GL sync plus a deswizzle each time */
  Tex* rv=NULL;
  for(Tex* t=texh[hkey(gva)];t;t=t->next) if(t->gva==~0ull && t->alias==gva && t->is_rt && t->w==w && t->h==h && t->fkey==fkey && !rv) rv=t;
  for(Tex* t=texh[hkey(gva)];t;t=t->next) if(t->gva==gva && t->is_rt){ t->gva=~0ull; t->alias=gva; g_tmap_gen++; }
  if(rv && !getenv("SWR_NO_RT_ALIAS")){ rv->gva=gva; rv->alias=0; rv->last_used=g_frame; rv->hash=rt_sig(gva); rv->frame_checked=g_frame; return rv; }
  Tex* t=tex_create(gva,w,h,1,1,GL_TEXTURE_2D,f,fkey,!((tile>>12)&1),(tile>>4)&15,(tile>>8)&15,0,1);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  /* seed from memory in case the CPU wrote it */
  if(!depth && !f.compressed && f.fmt==GL_RGBA && f.bpp==4){
    u64 sz=(u64)w*h*4; u8* lin=malloc(sz); u8* src=gmem_ptr(gva,sz*2);
    if(src && t->bl){ deswizzle(lin,w*4,src,w*4,h,t->bh); glBindTexture(GL_TEXTURE_2D,t->id); glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,lin); }
    free(lin); }
  t->hash=rt_sig(gva);
  return t; }

/* 3D RT (tile mode bit 16) or array (layers @0x206). drawn either in one pass with the VS picking the layer
   or slice by slice (base layer @0x208). backed by a 3D/array texture that 3D/array TICs at the same
   address then use directly (32^3 LUTs) */
static u64 st_rt_vol, st_waited; int g_draw_layered;

static Tex* rt_vol(Tex* t, u32 dep, int is3d){
  GLenum tg= is3d? GL_TEXTURE_3D : GL_TEXTURE_2D_ARRAY; Tex* v=t->vol; u32 sw=SC(t->w), sh=SC(t->h);
  if(v && (v->d!=dep || v->target!=tg || v->pitch!=sw || v->size!=sh)){ glDeleteTextures(1,&v->id); if(v->view) glDeleteTextures(1,&v->view); free(v); v=t->vol=NULL; g_fb_sig_ok=0; g_tmap_gen++; mb_reset(); }
  if(!v){ v=calloc(1,sizeof *v); v->gva=t->gva; v->w=t->w; v->h=t->h; v->d=dep; v->levels=1; v->target=tg; v->f=t->f; v->fkey=t->fkey; v->is_rt=1; v->gpu_written=1;
    v->bl=t->bl; v->bh=t->bh; v->pitch=sw; v->size=sh; v->swz[0]=GL_RED; v->swz[1]=GL_GREEN; v->swz[2]=GL_BLUE; v->swz[3]=GL_ALPHA;
    glGenTextures(1,&v->id); glBindTexture(tg,v->id); glTexStorage3D(tg,1,t->f.ifmt,sw,sh,dep);
    glTexParameteri(tg,GL_TEXTURE_MAX_LEVEL,0); glTexParameteri(tg,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(tg,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    t->vol=v; st_rt_vol++;
    static int said; if(said<8){ said++; LOG("gpu: %s render target %ux%ux%u at %llx (fmt %x)\n",is3d? "3D" : "array",t->w,t->h,dep,(unsigned long long)t->gva,t->fkey&0xFFFF); } }
  return v; }
/* current g_fbo attachments; skip reattaching if unchanged, every change makes the driver revalidate */
static GLuint g_fb_sig[10]; static int g_fb_lay[8]; static int g_fb_sig_ok; static u64 st_fb_skip; static int g_clear_layer=-1;

static void bind_framebuffer(GpuChannel* ch){
  u32* r=ch->r3d;
  u32 ctl=r[0x487]; u32 count=ctl&15; if(count>8) count=8;
  int n=0; g_nrt=0;
  glBindFramebuffer(GL_FRAMEBUFFER,g_fbo);
  GLuint sig[10]; int lay[8]; int anylay=0;
  for(u32 i=0;i<8;i++){
    Tex* t=NULL; lay[i]=-2;
    if(i<count){ u32 idx=(ctl>>(4+i*3))&7; u32* rt=&r[0x200+idx*0x10]; u64 a=((u64)rt[0]<<32)|rt[1];
      { static int dbg=-1; if(dbg<0) dbg=getenv("SWR_RTDBG")!=0; if(dbg) LOG("rtdbg %d: %llx %ux%u fmt=%x tile=%x arr=%x stride=%x base=%x\n",i,(unsigned long long)a,rt[2],rt[3],rt[4],rt[5],rt[6],rt[7],rt[8]); }
      if(a && rt[4] && rt[2] && rt[3]) t=rt_texture(a,rt[2],rt[3],rt[4],rt[5],0);
      u32 dep=rt[6]&0xFFFF; int is3d=(rt[5]>>16)&1;
      if(t && !t->f.depth && dep>1 && dep<=2048 && !getenv("SWR_NO_RTVOL")){ Tex* v=rt_vol(t,dep,is3d);
        if(v){ v->wgen++; v->last_used=g_frame;
          if(g_clear_layer>=0) lay[i]=(int)(rt[8]&0xFFFF)+g_clear_layer; else lay[i]= g_draw_layered? -1 : (int)(rt[8]&0xFFFF);
          if(lay[i]>=(int)dep) lay[i]=(int)dep-1; if(lay[i]==-1) anylay=1;
          g_rt[i]=t; sig[i]=v->id; n=i+1; t->gpu_written=1; t->wgen++; continue; } } }
    g_rt[i]=t; sig[i]= t? t->id : 0;
    if(t){ n=i+1; t->gpu_written=1; t->wgen++; }
  }
  g_nrt=n;
  g_zt=NULL;
  if(r[0x54E]){ u64 za=((u64)r[0x3F8]<<32)|r[0x3F9]; u32 zw=r[0x48A], zh=r[0x48B];
    if(za && zw && zh) g_zt=rt_texture(za,zw,zh,r[0x3FA],r[0x3FB],1); }
  if(anylay && g_zt){ g_zt=NULL; static int said; if(!said){ said=1; LOG("gpu: layered draw with depth: drawn without depth\n"); } }
  if(g_zt) g_zt->wgen++;
  GLenum att= g_zt? (g_zt->f.fmt==GL_DEPTH_STENCIL? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT) : GL_DEPTH_STENCIL_ATTACHMENT;
  sig[8]= g_zt? g_zt->id : 0; sig[9]=att;
  if(g_fb_sig_ok && !memcmp(sig,g_fb_sig,sizeof sig) && !memcmp(lay,g_fb_lay,sizeof lay)){ st_fb_skip++; return; }
  g_tmap_gen++;
  GLenum bufs[8];
  for(u32 i=0;i<8;i++){ if(!g_fb_sig_ok || g_fb_sig[i]!=sig[i] || g_fb_lay[i]!=lay[i]){
      if(lay[i]==-2) glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0+i,GL_TEXTURE_2D,sig[i],0);
      else if(lay[i]==-1) glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0+i,sig[i],0);
      else glFramebufferTextureLayer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0+i,sig[i],0,lay[i]); }
    bufs[i]= sig[i]? GL_COLOR_ATTACHMENT0+i : GL_NONE; }
  memcpy(g_fb_lay,lay,sizeof lay);
  { static int dbg=-1; if(dbg<0) dbg=getenv("SWR_RTDBG")!=0; if(dbg){ GLenum st=glCheckFramebufferStatus(GL_FRAMEBUFFER); LOG("rtdbg fbo %x lay %d %d layered=%d err=%x\n",st,lay[0],lay[1],g_draw_layered,glGetError()); } }
  glDrawBuffers(8,bufs);
  if(!g_fb_sig_ok || g_fb_sig[8]!=sig[8] || g_fb_sig[9]!=sig[9]){
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,att,GL_TEXTURE_2D,sig[8],0); }
  memcpy(g_fb_sig,sig,sizeof sig); g_fb_sig_ok=1;
}

/* ============================================================ RT <-> memory, GPU 2D blits */
typedef struct { u32 format, linear, bh, depth, layer, pitch, width, height; u64 addr; } Surf2Dg;
static GLuint g_rfbo, g_dfbo;

static int rt_plain(Tex* t){ return t && !t->f.depth && !t->f.is_int && t->f.fmt==GL_RGBA && t->f.type==GL_UNSIGNED_BYTE && t->f.bpp==4; }
static void trace_note(const char* fmt, ...);
/* write RT contents back to guest memory for CPU-side DMA/2D */
static void state_dirty(void);

/* res scale != 1: GPU copy between the scaled RT and native size */
static GLuint rt_unscaled_copy(Tex* t, int to_real){
  GLuint tmp=0; glGenTextures(1,&tmp); glBindTexture(GL_TEXTURE_2D,tmp); glTexStorage2D(GL_TEXTURE_2D,1,t->f.ifmt,t->w,t->h);
  if(to_real){ glBindFramebuffer(GL_READ_FRAMEBUFFER,g_rfbo); glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t->id,0); glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,g_dfbo); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tmp,0); { GLenum b=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&b); }
    glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB); glColorMaski(0,1,1,1,1); state_dirty();
    glBlitFramebuffer(0,0,(GLint)SC(t->w),(GLint)SC(t->h),0,0,(GLint)t->w,(GLint)t->h,GL_COLOR_BUFFER_BIT,GL_LINEAR); }
  return tmp; }

static void rt_scaled_put(Tex* t, GLuint tmp){
  glBindFramebuffer(GL_READ_FRAMEBUFFER,g_rfbo); glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tmp,0); glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,g_dfbo); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t->id,0); { GLenum b=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&b); }
  glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB); glColorMaski(0,1,1,1,1); state_dirty();
  glBlitFramebuffer(0,0,(GLint)t->w,(GLint)t->h,0,0,(GLint)SC(t->w),(GLint)SC(t->h),GL_COLOR_BUFFER_BIT,GL_LINEAR);
  glBindFramebuffer(GL_FRAMEBUFFER,g_fbo); }

/* any uncompressed color RT (R16F heightmaps, RG16F flow maps...), games render these once and DMA them
   into the final texture */
static int rt_flushable(Tex* t){ return t && !t->f.depth && !t->f.compressed && t->f.bpp>0 && t->f.bpp<=16 && t->target==GL_TEXTURE_2D && !t->vol; }

void gpu_gl_rt_flush(u64 gva){
  Tex* t=tex_find_rt(gva); if(!rt_flushable(t) || (g_gles && !rt_plain(t))) return;
  { static int nlog; if(nlog<40 && !rt_plain(t)){ nlog++; LOG("rt %llx %ux%u (fkey %x) copied to memory for a DMA/2D copy\n",(unsigned long long)gva,t->w,t->h,t->fkey); } }
  u32 w=t->w, h=t->h, bp=t->f.bpp; u8* px=malloc((size_t)w*h*bp); if(!px) return;
  GLuint tmp= g_scale!=1.0f? rt_unscaled_copy(t,1) : 0;
  glBindFramebuffer(GL_READ_FRAMEBUFFER,g_rfbo); glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tmp? tmp : t->id,0); glReadBuffer(GL_COLOR_ATTACHMENT0);
  glPixelStorei(GL_PACK_ALIGNMENT,1); glPixelStorei(GL_PACK_ROW_LENGTH,0);
  if(rt_plain(t)) glReadPixels(0,0,(GLsizei)w,(GLsizei)h,GL_RGBA,GL_UNSIGNED_BYTE,px);
  else { glBindTexture(GL_TEXTURE_2D,tmp? tmp : t->id); glGetTexImage(GL_TEXTURE_2D,0,t->f.fmt,t->f.type,px); }
  glBindFramebuffer(GL_FRAMEBUFFER,g_fbo); if(tmp) glDeleteTextures(1,&tmp); state_dirty();
  u64 sz= t->bl? bl_level_size(w,h,1,&t->f,t->bh,0,0,NULL) : (u64)w*h*bp; u8* dst=gmem_ptr(gva,sz);
  if(dst){ if(t->bl) swizzle(dst,px,w*bp,w*bp,h,t->bh); else memcpy(dst,px,(size_t)w*h*bp); }
  free(px); t->hash=rt_sig(gva); trace_note("  (rt %llx -> memory, %u bytes/px)\n",(unsigned long long)gva,bp); }

/* CPU copy changed the memory, reload the RT */
void gpu_gl_rt_reload(u64 gva){
  Tex* t=tex_find_rt(gva);
  /* DMA over an RT we can't reload (R8 video luma etc): drop the RT and read memory instead */
  if(t && !rt_plain(t)){ t->gva=~0ull; g_tmap_gen++; trace_note("  (new memory over rt %llx: rt discarded)\n",(unsigned long long)gva); return; }
  if(!rt_plain(t)) return;
  u32 w=t->w, h=t->h; u64 sz= t->bl? bl_level_size(w,h,1,&t->f,t->bh,0,0,NULL) : (u64)w*h*4; u8* src=gmem_ptr(gva,sz); if(!src) return;
  u8* lin=malloc((size_t)w*h*4); if(t->bl) deswizzle(lin,w*4,src,w*4,h,t->bh); else memcpy(lin,src,(size_t)w*h*4);
  GLuint tmp= g_scale!=1.0f? rt_unscaled_copy(t,0) : 0;
  glBindTexture(GL_TEXTURE_2D,tmp? tmp : t->id); glPixelStorei(GL_UNPACK_ALIGNMENT,1); glPixelStorei(GL_UNPACK_ROW_LENGTH,0); glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,lin);
  if(tmp){ rt_scaled_put(t,tmp); glDeleteTextures(1,&tmp); }
  free(lin); t->hash=rt_sig(gva); t->wgen++; trace_note("  (memory -> rt %llx)\n",(unsigned long long)gva); }
/* 2D blit from an RT, done on the GPU */
static void state_dirty(void);

int r3d_blit_gl(Surf2Dg* s, Surf2Dg* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx, double sdy){ state_dirty();
  Tex* st=tex_find_rt(s->addr);
  trace_note("BLIT2D %llx(%ux%u f%x%s) -> %llx(%ux%u f%x) dst(%d,%d %dx%d) src(%.1f,%.1f step %.3f,%.3f)\n",(unsigned long long)s->addr,s->width,s->height,s->format,st?" RT":"",
    (unsigned long long)d->addr,d->width,d->height,d->format,dx,dy,dw,dh,sx,sy,sdx,sdy);
  if(!st || st->f.depth) return 0;
  u32 tile=(d->linear? 0x1000u : 0)|(d->bh<<4);
  Tex* dt=rt_texture(d->addr,d->width,d->height,d->format,tile,0); if(!dt || dt==st || dt->f.depth) return 0;
  if(st->f.is_int!=dt->f.is_int) return 0;
  glBindFramebuffer(GL_READ_FRAMEBUFFER,g_rfbo); glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,st->id,0); glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,g_dfbo); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,dt->id,0); { GLenum b=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&b); }
  glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB); glColorMaski(0,1,1,1,1);
  float S=g_scale; int scaled= sdx!=1.0 || sdy!=1.0 || S!=1.0f;
  glBlitFramebuffer((GLint)(sx*S),(GLint)(sy*S),(GLint)((sx+sdx*dw)*S),(GLint)((sy+sdy*dh)*S),(GLint)(dx*S),(GLint)(dy*S),(GLint)((dx+dw)*S),(GLint)((dy+dh)*S),GL_COLOR_BUFFER_BIT,(scaled && !st->f.is_int)? GL_LINEAR : GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER,g_fbo); dt->last_used=g_frame; dt->wgen++;
  return 1; }

/* ============================================================ fixed state */
static GLenum blend_factor(u32 v){
  if(v&0x4000) return v&~0x4000u;
  switch(v){ case 1: return GL_ZERO; case 2: return GL_ONE; case 3: return GL_SRC_COLOR; case 4: return GL_ONE_MINUS_SRC_COLOR; case 5: return GL_SRC_ALPHA; case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return GL_DST_ALPHA; case 8: return GL_ONE_MINUS_DST_ALPHA; case 9: return GL_DST_COLOR; case 10: return GL_ONE_MINUS_DST_COLOR; case 11: return GL_SRC_ALPHA_SATURATE;
    case 14: return GL_CONSTANT_COLOR; case 15: return GL_ONE_MINUS_CONSTANT_COLOR; case 16: return GL_SRC1_COLOR; case 17: return GL_ONE_MINUS_SRC1_COLOR; case 18: return GL_SRC1_ALPHA; case 19: return GL_ONE_MINUS_SRC1_ALPHA; }
  return GL_ONE; }

static GLenum blend_eq(u32 v){ if(v>=0x8000) return v; switch(v){ case 1: return GL_FUNC_ADD; case 2: return GL_FUNC_SUBTRACT; case 3: return GL_FUNC_REVERSE_SUBTRACT; case 4: return GL_MIN; case 5: return GL_MAX; } return GL_FUNC_ADD; }

static GLenum cmp_func(u32 v){ if(v>=0x200 && v<=0x207) return v; if(v>=1 && v<=8) return GL_NEVER+(v-1); return GL_ALWAYS; }

static GLenum stencil_op(u32 v){ switch(v){ case 1: return GL_KEEP; case 2: return GL_ZERO; case 3: return GL_REPLACE; case 4: return GL_INCR; case 5: return GL_DECR; case 6: return GL_INVERT; case 7: return GL_INCR_WRAP; case 8: return GL_DECR_WRAP; case 0: return GL_ZERO; } return v; }

/* fixed-state signature: unchanged regs since the last draw -> skip ~70 GL calls
   (each call costs on Intel's driver and we do ~1000 draws a frame) */
static int g_state_ok; static u32 g_state_sig[256]; static u64 st_state_skip;
/* native 0..1 depth (GL 4.5 clip control). remapping z*2-w in the shader loses precision far away */
static PFNGLCLIPCONTROLPROC p_clip; static int g_clip_mode=-1;
static float g_dl_jx, g_dl_jy;              /* DLAA: this draw's sub-pixel jitter (0 unless a jittered scene draw) */
static float g_dl_vp[4], g_dl_dr[2], g_dl_po[3];   /* the draw's unjittered viewport, depth range, polygon offset (for the replay) */
static int g_gs_ok;

static void state_dirty(void){ g_state_ok=0; g_gs_ok=0; }
void gpu_gl_state_dirty(void){ state_dirty(); }   /* the in-game menu drew with its own GL state (dread_ui/ov.cpp) */
/* FPS counter (main.c fps_overlay_rects) onto the window's framebuffer, as scissored clears */
void gpu_gl_fps_overlay(void){
  extern int fps_overlay_rects(int,int,int(*)[5],int); int (*r)[5]; int buf[64][5]; r=buf; int w=1,h=1; backend_window_size(&w,&h);
  int n=fps_overlay_rects(w,h,r,64); if(!n) return;
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0); glEnable(GL_SCISSOR_TEST); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  for(int i=0;i<n;i++){ glScissor(r[i][0],h-r[i][1]-r[i][3],r[i][2],r[i][3]);
    if(r[i][4]) glClearColor(1.0f,0.92f,0.2f,1.0f); else glClearColor(0.0f,0.0f,0.0f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT); }
  glDisable(GL_SCISSOR_TEST); state_dirty(); }

/* "Hold (-) to skip" in Metroid Dread's own font, top left, while main.c says a skippable cutscene plays.
   The font is read from the game's romfs at first use: textures/system/fonts/textures/occ_atlas.bctex (MTXT: gzip of an
   NVN XTX texture, BC3 2048x1024, block linear, block height 16 GOBs; uploaded as BC3 after deswizzling) and
   system/fonts/occ_32.bfont (MFNT: 14-byte glyph records x, y, w, h, x offset, y offset, advance; glyph = ASCII - 31).
   The round - button is in the same atlas at 725,144 32x32 (no glyph table entry). zlib is loaded at run time. */
typedef struct { const u8* next_in; unsigned avail_in; unsigned long total_in; u8* next_out; unsigned avail_out; unsigned long total_out;
  const char* msg; void* state; void* zalloc; void* zfree; void* opaque; int data_type; unsigned long adler; unsigned long reserved; } SkZ;
static GLuint g_sk_tex, g_sk_prog; static int g_sk_state; static short g_sk_gl[96][7]; static GLint g_sk_u_rect, g_sk_u_uv, g_sk_u_col, g_sk_u_scr;
static u8* sk_read(const char* rel, size_t* n){
  extern char g_romfs_dir_override[1024]; char p[1400];
  if(g_romfs_dir_override[0]) snprintf(p,sizeof p,"%s/%s",g_romfs_dir_override,rel); else snprintf(p,sizeof p,"%sromfs/%s",g_datadir,rel);
  FILE* f=fopen(p,"rb"); if(!f) return NULL; fseek(f,0,SEEK_END); long l=ftell(f); fseek(f,0,SEEK_SET);
  u8* d= l>0? malloc((size_t)l) : NULL; if(d && fread(d,1,(size_t)l,f)!=(size_t)l){ free(d); d=NULL; } fclose(f); *n=(size_t)l; return d; }
/* the font on the CPU, once: the atlas as linear BC3 blocks (2048x1024) and the ASCII glyph records. Shared by the
   OpenGL prompt (uploaded as BC3) and the native Vulkan one (skip_prompt_rgba, composed here and blended by snxvk hdr.c) */
static u8* g_sk_lin; static int g_sk_cpu;
static int sk_font_cpu(void){
  if(g_sk_cpu) return g_sk_cpu>0;
  g_sk_cpu=-1;
  size_t fn=0, an=0; u8* fnt=sk_read("system/fonts/occ_32.bfont",&fn); u8* mt=sk_read("textures/system/fonts/textures/occ_atlas.bctex",&an);
  u8 *xtx=NULL, *lin=NULL;
  if(!fnt || !mt || fn<0x60+96*14 || memcmp(fnt,"MFNT",4) || an<16 || memcmp(mt,"MTXT",4)) goto out;
  memcpy(g_sk_gl,fnt+0x60,sizeof g_sk_gl);
  { void* z=plat_dl_open(
#ifdef _WIN32
      "zlib1.dll"
#else
      "libz.so.1"
#endif
      ); if(!z) goto out;
    int (*init)(SkZ*,int,const char*,int)=(int(*)(SkZ*,int,const char*,int))plat_dl_sym(z,"inflateInit2_");
    int (*inf)(SkZ*,int)=(int(*)(SkZ*,int))plat_dl_sym(z,"inflate"); int (*end)(SkZ*)=(int(*)(SkZ*))plat_dl_sym(z,"inflateEnd");
    if(!init || !inf || !end) goto out;
    size_t cap=4u<<20; xtx=malloc(cap); SkZ s; memset(&s,0,sizeof s); s.next_in=mt+8; s.avail_in=(unsigned)(an-8); s.next_out=xtx; s.avail_out=(unsigned)cap;
    if(!xtx || init(&s,31,"1.2.11",(int)sizeof s)!=0) goto out;
    int r=inf(&s,4); size_t got=s.total_out; end(&s); if(r!=1) goto out;   /* Z_FINISH -> Z_STREAM_END */
    /* the image block: NvBH type 3, data at its offset */
    for(size_t i=0;i+28<=got;i++) if(!memcmp(xtx+i,"HBvN",4)){ u64 ds,doff; u32 ty; memcpy(&ds,xtx+i+8,8); memcpy(&doff,xtx+i+16,8); memcpy(&ty,xtx+i+24,4);
        if(ty==3 && ds==2048u*1024u && i+doff+ds<=got){ lin=malloc(ds); if(!lin) goto out; deswizzle(lin,8192,xtx+i+doff,8192,256,4); break; } }
    if(!lin) goto out; }
  g_sk_lin=lin; lin=NULL; g_sk_cpu=1; LOG("skip prompt: game font loaded\n");
out:
  if(g_sk_cpu<0) LOG("skip prompt: the game's font could not be loaded (romfs or zlib missing)\n");
  free(fnt); free(mt); free(xtx); free(lin); return g_sk_cpu>0; }
/* one BC3 texel of the atlas, RGBA8 */
static void sk_texel(int x, int y, u8* o){
  const u8* b=g_sk_lin+((size_t)(y>>2)*512+(size_t)(x>>2))*16; int i=(y&3)*4+(x&3);
  int a0=b[0], a1=b[1]; u64 ab=0; for(int k=0;k<6;k++) ab|=(u64)b[2+k]<<(8*k); int ai=(int)((ab>>(3*i))&7); int a;
  if(a0>a1) a= ai==0? a0 : ai==1? a1 : ((8-ai)*a0+(ai-1)*a1)/7; else a= ai==0? a0 : ai==1? a1 : ai==6? 0 : ai==7? 255 : ((6-ai)*a0+(ai-1)*a1)/5;
  u16 c0=(u16)(b[8]|b[9]<<8), c1=(u16)(b[10]|b[11]<<8); u32 cb=(u32)b[12]|(u32)b[13]<<8|(u32)b[14]<<16|(u32)b[15]<<24; int ci=(int)((cb>>(2*i))&3);
  int r0=(c0>>11)*255/31, g0=((c0>>5)&63)*255/63, b0=(c0&31)*255/31, r1=(c1>>11)*255/31, g1=((c1>>5)&63)*255/63, b1=(c1&31)*255/31;
  int w0= ci==0? 3 : ci==1? 0 : ci==2? 2 : 1;   /* BC3 colour is always the 4-colour mode */
  o[0]=(u8)((r0*w0+r1*(3-w0))/3); o[1]=(u8)((g0*w0+g1*(3-w0))/3); o[2]=(u8)((b0*w0+b1*(3-w0))/3); o[3]=(u8)a; }
/* "Hold (-) to skip" at 1080p size as premultiplied RGBA (shadow included); *tw = the text's width (the hold bar's) */
static void sk_blit(float* img, int W, int H, int sx, int sy, int w, int h, int dx, int dy, float tint, float alpha){
  for(int y=0;y<h;y++) for(int x=0;x<w;x++){ int X=dx+x, Y=dy+y; if(X<0||Y<0||X>=W||Y>=H) continue;
    u8 t[4]; sk_texel(sx+x,sy+y,t); float a=t[3]/255.0f*alpha; float* d=img+((size_t)Y*W+X)*4;
    for(int k=0;k<3;k++) d[k]=t[k]/255.0f*tint*a+d[k]*(1-a); d[3]=a+d[3]*(1-a); } }
static int sk_text_cpu(float* img, int W, int H, const char* s, int x, int y){
  for(;*s;s++){ int c=(unsigned char)*s; if(c<32 || c>126) continue; const short* g=g_sk_gl[c-31];
    if(img && g[2]>0 && g[3]>0){ int gx=x+g[4], gy=y+30-g[5]; sk_blit(img,W,H,g[0],g[1],g[2],g[3],gx+2,gy+2,0,0.6f); sk_blit(img,W,H,g[0],g[1],g[2],g[3],gx,gy,1,1); }
    x+=g[6]; }
  return x; }
int skip_prompt_rgba(u8** px, int* w, int* h, int* tw){
  if(!sk_font_cpu()) return 0;
  enum { W=320, H=48 }; float* img=calloc((size_t)W*H*4,sizeof(float)); u8* out=malloc((size_t)W*H*4); if(!img || !out){ free(img); free(out); return 0; }
  int x=sk_text_cpu(img,W,H,"Hold",2,4)+8;
  sk_blit(img,W,H,725,144,32,32,x+2,4+2-1,0,0.6f); sk_blit(img,W,H,725,144,32,32,x,4-1,1,1);
  x=sk_text_cpu(img,W,H,"to skip",x+32+8,4);
  for(size_t i=0;i<(size_t)W*H*4;i++){ float v=img[i]*255.0f+0.5f; out[i]=(u8)(v>255? 255 : v<0? 0 : v); }
  free(img); *px=out; *w=W; *h=H; *tw=x-2; return 1; }
static int skip_prompt_load(void){
  int ok=0; u8* lin=NULL;
  if(!sk_font_cpu()) goto out;
  lin=g_sk_lin;
  glGenTextures(1,&g_sk_tex); glBindTexture(GL_TEXTURE_2D,g_sk_tex);
  glTexStorage2D(GL_TEXTURE_2D,1,0x83F3,2048,1024); glCompressedTexSubImage2D(GL_TEXTURE_2D,0,0,0,2048,1024,0x83F3,2048*1024,lin);   /* COMPRESSED_RGBA_S3TC_DXT5 */
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  { static const char* vs="#version 430 core\nuniform vec4 rect; uniform vec4 uvr; uniform vec4 scr; out vec2 uv;\n"
      "void main(){ vec2 c=vec2(gl_VertexID&1,gl_VertexID>>1); uv=uvr.xy+c*uvr.zw; vec2 p=(rect.xy+c*rect.zw)/scr.xy; gl_Position=vec4(p.x*2.0-1.0,1.0-p.y*2.0,0.0,1.0); }";
    static const char* fs="#version 430 core\nin vec2 uv; uniform sampler2D t; uniform vec4 col; out vec4 o;\n"
      "void main(){ vec4 c= uv.x<0.0? vec4(1.0) : texture(t,uv); o=vec4(c.rgb*col.rgb,c.a*col.a); }";
    GLuint a=compile(GL_VERTEX_SHADER,vs,NULL,0), b=compile(GL_FRAGMENT_SHADER,fs,NULL,0); g_sk_prog= a&&b? link2(a,b,0) : 0; }
  if(!g_sk_prog) goto out;
  g_sk_u_rect=glGetUniformLocation(g_sk_prog,"rect"); g_sk_u_uv=glGetUniformLocation(g_sk_prog,"uvr");
  g_sk_u_col=glGetUniformLocation(g_sk_prog,"col"); g_sk_u_scr=glGetUniformLocation(g_sk_prog,"scr");
  ok=1;
out:
  (void)lin; return ok; }
static void sk_quad(float x, float y, float w, float h, float u, float v, float uw, float vh, float r, float g, float b, float a){
  float q[3][4]={{x,y,w,h},{u,v,uw,vh},{r,g,b,a}}; glUniform4fv(g_sk_u_rect,1,q[0]); glUniform4fv(g_sk_u_uv,1,q[1]); glUniform4fv(g_sk_u_col,1,q[2]); glDrawArrays(GL_TRIANGLE_STRIP,0,4); }
static float sk_text(const char* s, float x, float y, float k, float a, int draw){   /* y = baseline top of a 34 px line */
  for(;*s;s++){ int c=(unsigned char)*s; if(c<32 || c>126) continue; const short* g=g_sk_gl[c-31];
    if(draw && g[2]>0 && g[3]>0){ float gx=x+g[4]*k, gy=y+(30-g[5])*k;
      sk_quad(gx+2*k,gy+2*k,g[2]*k,g[3]*k,g[0]/2048.f,g[1]/1024.f,g[2]/2048.f,g[3]/1024.f,0,0,0,a*0.6f);
      sk_quad(gx,gy,g[2]*k,g[3]*k,g[0]/2048.f,g[1]/1024.f,g[2]/2048.f,g[3]/1024.f,1,1,1,a); }
    x+=g[6]*k; }
  return x; }
void gpu_gl_skip_prompt(void){
  extern volatile int g_skip_prompt; extern volatile float g_skip_hold; extern int g_opt_show_fps;
  static float alpha; static u64 last; u64 now=plat_ns(); float dt= last? (float)(now-last)/1e9f : 0; last=now; if(dt>0.1f) dt=0.1f;
  float want= g_skip_prompt? 1.0f : 0.0f; alpha+= (want-alpha)*(dt*8.0f>1? 1 : dt*8.0f); if(alpha<0.01f && !want){ alpha=0; return; }
  if(g_sk_state==0) g_sk_state= skip_prompt_load()? 1 : -1;
  if(g_sk_state<0 || g_gles) return;
  int w=1,h=1; backend_window_size(&w,&h); float k=(float)h/1080.0f; if(k<0.5f) k=0.5f;
  float x=48*k, y=(g_opt_show_fps? 96 : 44)*k, ts=0.95f*k;
  glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,w,h); glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST); glDisable(GL_FRAMEBUFFER_SRGB); glColorMaski(0,1,1,1,1);
  glEnable(GL_BLEND); glBlendFuncSeparatei(0,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA); glBlendEquationSeparatei(0,GL_FUNC_ADD,GL_FUNC_ADD);
  glUseProgram(g_sk_prog); { float sc[4]={(float)w,(float)h,0,0}; glUniform4fv(g_sk_u_scr,1,sc); } glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,g_sk_tex); glBindSampler(0,0);
  glUniform1i(glGetUniformLocation(g_sk_prog,"t"),0); glBindVertexArray(g_scratch_vao);
  float ix=x, cx=sk_text("Hold",x,y,ts,alpha,1)+8*k, isz=34*k;
  sk_quad(cx+2*k,y+2*k-1*k,isz,isz,725/2048.f,144/1024.f,32/2048.f,32/1024.f,0,0,0,alpha*0.6f);
  sk_quad(cx,y-1*k,isz,isz,725/2048.f,144/1024.f,32/2048.f,32/1024.f,1,1,1,alpha);
  float end=sk_text("to skip",cx+isz+8*k,y,ts,alpha,1);
  { float hold=g_skip_hold; if(hold>0){ float bw=(end-ix)*hold;   /* hold progress under the text */
      sk_quad(ix,y+42*k,end-ix,4*k,-1,0,0,0,0,0,0,alpha*0.5f); sk_quad(ix,y+42*k,bw,4*k,-1,0,0,0,1,1,1,alpha); } }
  glDisable(GL_BLEND); glUseProgram(0); state_dirty(); }

static void apply_state(GpuChannel* ch){
  u32* r=ch->r3d;
  { u32 sig[256]; int n=0;
#define SR(a,b) for(u32 q_=(a);q_<=(b);q_++) sig[n++]=r[q_];
    SR(0x280,0x285) SR(0x300,0x303) SR(0x35F,0x35F) SR(0x36B,0x36B) SR(0x372,0x372) SR(0x380,0x382) SR(0x3D5,0x3D7) SR(0x3E4,0x3E4)
    SR(0x4B3,0x4B3) SR(0x4B8,0x4BA) SR(0x4C3,0x4C3) SR(0x4C7,0x4CA) SR(0x4D0,0x4DF) SR(0x4E0,0x4E7) SR(0x4ED,0x4ED) SR(0x546,0x546)
    SR(0x55B,0x55B) SR(0x565,0x569) SR(0x56F,0x56F) SR(0x591,0x592) SR(0x646,0x648) SR(0x680,0x687) SR(0x780,0x7BF)
#undef SR
    sig[n++]= g_zt? 1u : 0u; memcpy(&sig[n++],&g_scale,4); memcpy(&sig[n++],&g_dl_jx,4); memcpy(&sig[n++],&g_dl_jy,4);
    if(g_state_ok && !memcmp(sig,g_state_sig,(size_t)n*4)){ st_state_skip++; return; }
    memcpy(g_state_sig,sig,(size_t)n*4); g_state_ok=1; }
  if(p_clip){ int m=r[0x35F]&1; if(m!=g_clip_mode){ p_clip(0x8CA1 /* LOWER_LEFT */, m? 0x935F /* ZERO_TO_ONE */ : 0x935E /* NEGATIVE_ONE_TO_ONE */); g_clip_mode=m; } }
  /* grouped so only what changed hits GL */
  static u32 gs[16][20]; int ok=g_gs_ok; g_gs_ok=1;
#define GRP(id_, ...) u32 k_##id_[]={__VA_ARGS__}; if(!ok || memcmp(gs[id_],k_##id_,sizeof k_##id_)) if(memcpy(gs[id_],k_##id_,sizeof k_##id_))
  u32 gsc; memcpy(&gsc,&g_scale,4);
  float vt[6]; memcpy(vt,&r[0x280],24);
  float sx=vt[0], sy=vt[1], tx=vt[3], ty=vt[4];
  u32 gjx, gjy; memcpy(&gjx,&g_dl_jx,4); memcpy(&gjy,&g_dl_jy,4);
  { GRP(0, r[0x280],r[0x281],r[0x282],r[0x283],r[0x284],r[0x285],r[0x35F],r[0x300],r[0x301],r[0x302],r[0x303],r[0x380],r[0x381],r[0x382],gsc,gjx,gjy){
    float x0=tx-fabsf_(sx), y0=ty-fabsf_(sy), w=2*fabsf_(sx), h=2*fabsf_(sy);
    if(w<1||h<1){ u32 vx=r[0x300], vy=r[0x301]; x0=(float)(vx&0xFFFF); w=(float)(vx>>16); y0=(float)(vy&0xFFFF); h=(float)(vy>>16); }
    glViewportIndexedf(0,x0*g_scale+g_dl_jx,y0*g_scale+g_dl_jy,w*g_scale,h*g_scale);
    g_dl_vp[0]=x0*g_scale; g_dl_vp[1]=y0*g_scale; g_dl_vp[2]=w*g_scale; g_dl_vp[3]=h*g_scale;
    float n, f; memcpy(&n,&r[0x302],4); memcpy(&f,&r[0x303],4);
    /* depth goes through the viewport transform (z*scale_z+translate_z) like on hw / yuzu's vk backend.
       SMO's fog pass draws at z=1 with NOTEQUAL to skip the sky, so this has to be exact */
    if((vt[2]!=0.0f || vt[5]!=0.0f) && !getenv("SWR_DEPTH_REGS")){ float red= (r[0x35F]&1)? 0.0f : 1.0f; n=vt[5]-vt[2]*red; f=vt[5]+vt[2];
      if(fabsf_(n)<1e-6f) n=0.0f; if(fabsf_(n-1.0f)<1e-6f) n=1.0f; if(fabsf_(f)<1e-6f) f=0.0f; if(fabsf_(f-1.0f)<1e-6f) f=1.0f; }
    glDepthRangeIndexed(0,n,f); g_dl_dr[0]=n; g_dl_dr[1]=f;
    if(r[0x380]&1){ u32 xs=r[0x381], ys=r[0x382]; glEnable(GL_SCISSOR_TEST); glScissorIndexed(0,(GLint)((xs&0xFFFF)*g_scale),(GLint)((ys&0xFFFF)*g_scale),(GLsizei)(((xs>>16)-(xs&0xFFFF))*g_scale+0.5f),(GLsizei)(((ys>>16)-(ys&0xFFFF))*g_scale+0.5f)); }
    else glDisable(GL_SCISSOR_TEST); } }
  /* raster state (winding flips with the Y flip) */
  { float w=2*fabsf_(sx), h=2*fabsf_(sy); int yflip= (w<1||h<1)? 0 : (sy<0);
    GRP(1, r[0x646],r[0x647],r[0x648],r[0x36B],r[0x372],r[0x55B],r[0x56F],(u32)yflip){
    if(r[0x646]){ glEnable(GL_CULL_FACE); glCullFace(r[0x648]==0x404? GL_FRONT : r[0x648]==0x408? GL_FRONT_AND_BACK : GL_BACK); } else glDisable(GL_CULL_FACE);
    { GLenum ff= r[0x647]==0x900? GL_CW : GL_CCW; if(yflip) ff= ff==GL_CW? GL_CCW : GL_CW; glFrontFace(ff); }
    glPolygonMode(GL_FRONT_AND_BACK, r[0x36B]==0x1B01? GL_LINE : r[0x36B]==0x1B00? GL_POINT : GL_FILL);
    if(r[0x372]){ float fac, un; memcpy(&fac,&r[0x55B],4); memcpy(&un,&r[0x56F],4); glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(fac,un); g_dl_po[0]=1; g_dl_po[1]=fac; g_dl_po[2]=un; } else { glDisable(GL_POLYGON_OFFSET_FILL); g_dl_po[0]=0; } } }
  /* depth / stencil */
  { u32 z= g_zt? 1u : 0u;
    GRP(2, z,r[0x4B3],r[0x4C3],r[0x4BA],r[0x4E0],r[0x4E1],r[0x4E2],r[0x4E3],r[0x4E4],r[0x4E5],r[0x4E6],r[0x4E7],r[0x565],r[0x566],r[0x567],r[0x568],r[0x569],r[0x3D5],r[0x3D6],r[0x3D7]){
    if(r[0x4B3] && g_zt){ glEnable(GL_DEPTH_TEST); glDepthFunc(cmp_func(r[0x4C3])); } else glDisable(GL_DEPTH_TEST);
    glDepthMask(r[0x4BA] && g_zt? GL_TRUE : GL_FALSE);
    if(r[0x4E0] && g_zt){ glEnable(GL_STENCIL_TEST);
      glStencilFuncSeparate(GL_FRONT,cmp_func(r[0x4E4]),r[0x4E5],r[0x4E6]); glStencilOpSeparate(GL_FRONT,stencil_op(r[0x4E1]),stencil_op(r[0x4E2]),stencil_op(r[0x4E3])); glStencilMaskSeparate(GL_FRONT,r[0x4E7]);
      if(r[0x565]){ glStencilFuncSeparate(GL_BACK,cmp_func(r[0x569]),r[0x3D5],r[0x3D7]); glStencilOpSeparate(GL_BACK,stencil_op(r[0x566]),stencil_op(r[0x567]),stencil_op(r[0x568])); glStencilMaskSeparate(GL_BACK,r[0x3D6]); }
      else { glStencilFuncSeparate(GL_BACK,cmp_func(r[0x4E4]),r[0x4E5],r[0x4E6]); glStencilOpSeparate(GL_BACK,stencil_op(r[0x4E1]),stencil_op(r[0x4E2]),stencil_op(r[0x4E3])); glStencilMaskSeparate(GL_BACK,r[0x4E7]); } }
    else glDisable(GL_STENCIL_TEST); } }
  /* blend + color masks */
  { GRP(3, r[0x4B8]&1,r[0x4C7],r[0x4C8],r[0x4C9],r[0x4CA]){
    if(r[0x4B8]&1) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);   /* framebuffer_srgb */
    float bc[4]; memcpy(bc,&r[0x4C7],16); glBlendColor(bc[0],bc[1],bc[2],bc[3]); } }
  for(int i=0;i<8;i++){
    u32 cm= r[0x3E4]? r[0x680] : r[0x680+i]; int en= r[0x4D8+i]; u32* b=&r[0x780+i*8];
    u32 k[12]={cm&0xFFFF,(u32)en,0,0,0,0,0,0,0,0,0,0};
    if(en){ k[2]=r[0x4B9]; if(r[0x4B9]){ k[3]=b[1]; k[4]=b[2]; k[5]=b[3]; k[6]=b[4]; k[7]=b[5]; k[8]=b[6]; } else { k[3]=r[0x4D0]; k[4]=r[0x4D1]; k[5]=r[0x4D2]; k[6]=r[0x4D3]; k[7]=r[0x4D4]; k[8]=r[0x4D6]; } }
    if(ok && !memcmp(gs[4+i],k,sizeof k)) continue; memcpy(gs[4+i],k,sizeof k);
    glColorMaski(i,(cm&0xF)!=0,(cm&0xF0)!=0,(cm&0xF00)!=0,(cm&0xF000)!=0);
    if(en){ glEnablei(GL_BLEND,i);
      /* hw always uses the alpha fields, separate_alpha doesn't disable them (yuzu agrees). menus' alpha masks need this */
      if(r[0x4B9]){ glBlendEquationSeparatei(i,blend_eq(b[1]),blend_eq(b[4]? b[4] : b[1]));
        glBlendFuncSeparatei(i,blend_factor(b[2]),blend_factor(b[3]),blend_factor(b[5]? b[5] : b[2]),blend_factor(b[6]? b[6] : b[3])); }
      else { glBlendEquationSeparatei(i,blend_eq(r[0x4D0]),blend_eq(r[0x4D3]? r[0x4D3] : r[0x4D0]));
        glBlendFuncSeparatei(i,blend_factor(r[0x4D1]),blend_factor(r[0x4D2]),blend_factor(r[0x4D4]? r[0x4D4] : r[0x4D1]),blend_factor(r[0x4D6]? r[0x4D6] : r[0x4D2])); } }
    else glDisablei(GL_BLEND,i);
  }
  { GRP(12, r[0x591],r[0x592],r[0x4ED],r[0x546]){
    if(r[0x591]){ glEnable(GL_PRIMITIVE_RESTART); glPrimitiveRestartIndex(r[0x592]); } else glDisable(GL_PRIMITIVE_RESTART);
    { float lw; memcpy(&lw,&r[0x4ED],4); if(lw>0 && lw<64) glLineWidth(lw); }
    { float ps; memcpy(&ps,&r[0x546],4); if(ps>0 && ps<256) glPointSize(ps); } } }
#undef GRP
}

/* ============================================================ drawing */
static u32 attr_count(u32 size){ switch(size){ case 0x01: return 4; case 0x02: return 3; case 0x03: return 4; case 0x04: return 2; case 0x05: return 3; case 0x0A: return 4; case 0x0F: return 2;
  case 0x12: return 1; case 0x13: return 3; case 0x18: return 2; case 0x1B: return 1; case 0x1D: return 1; case 0x30: return 4; case 0x31: return 3; } return 4; }

static u32 attr_bits(u32 size){ switch(size){ case 0x01: case 0x02: case 0x04: case 0x12: return 32; case 0x03: case 0x05: case 0x0F: case 0x1B: return 16; case 0x30: case 0x31: return 0; } return 8; }

static GLenum topo(u32 t){ switch(t){ case 0: return GL_POINTS; case 1: return GL_LINES; case 2: return GL_LINE_LOOP; case 3: return GL_LINE_STRIP; case 4: return GL_TRIANGLES; case 5: return GL_TRIANGLE_STRIP;
  case 6: return GL_TRIANGLE_FAN; case 10: return GL_LINES_ADJACENCY; case 11: return GL_LINE_STRIP_ADJACENCY; case 12: return GL_TRIANGLES_ADJACENCY; case 13: return GL_TRIANGLE_STRIP_ADJACENCY; case 14: return GL_PATCHES; } return GL_TRIANGLES; }

void r3d_cb_bind_gl(GpuChannel* ch, int stage, u32 v){
  u32* r=ch->r3d; u32 idx=(v>>4)&31; if(stage>4 || idx>=18) return;
  if(v&1){ g_cb.cb[stage][idx]=((u64)r[0x8E1]<<32)|r[0x8E2]; g_cb.cbsize[stage][idx]=r[0x8E0]; }
  else { g_cb.cb[stage][idx]=0; g_cb.cbsize[stage][idx]=0; } }

/* full draw capture to screenshots/draw_N/ for replaying elsewhere */
static void cap_write(const char* dir, const char* name, const void* d, size_t n){ char p[300]; snprintf(p,sizeof p,"%s/%s",dir,name); FILE* f=fopen(p,"wb"); if(f){ fwrite(d,1,n,f); fclose(f); } }

static void cap_mem(const char* dir, const char* name, u64 gva, u64 n){ if(!gva||!n) return; if(n>(1u<<20)) n=1u<<20; u8* b=malloc(n); gmem_read(gva,b,n); cap_write(dir,name,b,n); free(b); }

static void capture_draw(GpuChannel* ch, int indexed, u32 first, u32 count){
  static int ncap; if(ncap>=8) return; ncap++;
  u32* r=ch->r3d; char dir[128]; plat_mkdir("captures"); snprintf(dir,sizeof dir,"captures/draw_%d",ncap); plat_mkdir(dir);
  cap_write(dir,"regs.bin",r,0x1000*4);
  { char info[256]; int n=snprintf(info,sizeof info,"indexed=%d first=%u count=%u\n",indexed,first,count); cap_write(dir,"draw.txt",info,(size_t)n); }
  u64 code=((u64)r[0x582]<<32)|r[0x583];
  for(int i=0;i<6;i++){ u32 cfg=r[0x800+i*0x10]; if(i!=1 && !(cfg&1)) continue; char nm[32]; snprintf(nm,sizeof nm,"shader%d.bin",i); cap_mem(dir,nm,code+r[0x801+i*0x10],0x8000);
    u8* cb=malloc(0x8000); gmem_read(code+r[0x801+i*0x10],cb,0x8000); ShaderInfo si; memset(&si,0,sizeof si); char* g=shader_to_glsl(cb,0x8000,i==1? ST_VS : i==5? ST_FS : ST_GS,&si);
    if(g){ snprintf(nm,sizeof nm,"shader%d.glsl",i); cap_write(dir,nm,g,strlen(g)); free(g); } free(cb); }
  for(int st=0;st<5;st++) for(int i=0;i<18;i++) if(g_cb.cb[st][i]){ char nm[32]; snprintf(nm,sizeof nm,"cb_%d_%d.bin",st,i); u32 sz=g_cb.cbsize[st][i]; cap_mem(dir,nm,g_cb.cb[st][i],sz? sz : 16); }
  for(int b=0;b<16;b++){ u32* va=&r[0x700+b*4]; if(!(va[0]&0x1000)) continue; u64 start=((u64)va[1]<<32)|va[2]; u64 lim=((u64)r[0x7C0+b*2]<<32)|r[0x7C1+b*2];
    char nm[32]; snprintf(nm,sizeof nm,"vb_%d.bin",b); if(lim>=start) cap_mem(dir,nm,start,lim-start+1); }
  if(indexed){ u32 fmt=r[0x5F6]; u32 isz= fmt==0? 1 : fmt==1? 2 : 4; u64 ia=((u64)r[0x5F2]<<32)|r[0x5F3]; cap_mem(dir,"ib.bin",ia,(u64)(first+count)*isz); }
  { u64 tb=((u64)r[0x55D]<<32)|r[0x55E], sb=((u64)r[0x557]<<32)|r[0x558]; cap_mem(dir,"tic.bin",tb,0x8000); cap_mem(dir,"tsc.bin",sb,0x4000); }
  LOG("gpu: draw captured in %s\n",dir);
}
/* NVN ssbo table at c0[0x110+16k]: u64 addr + u32 size, for LDG */
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif
static GLuint g_sbuf[3][8];
static GLuint buf_cached(GLenum target, u64 start, u64 len);

static void bind_gbufs(int stage, Sh* s){
  if(!s->si.ngbuf) return; int slot= stage==0? 0 : stage==4? 1 : 2; u64 cb0=g_cb.cb[stage][0];
  static u8* tmp; static size_t cap;
  for(int i=0;i<s->si.ngbuf && i<8;i++){
    if(!g_sbuf[slot][i]) glGenBuffers(1,&g_sbuf[slot][i]);
    u64 addr=0; u32 size=16; if(cb0){ u32 v[3]; gmem_read(cb0+s->si.gbuf_cbuf_off[i],v,12); addr=((u64)v[1]<<32)|v[0]; size=v[2]; }
    if(!size) size=16; if(size>(16u<<20)) size=16u<<20; size=(size+3)&~3u;
    { static int nc=-1; if(nc<0) nc=getenv("SWR_NO_SSBO_CACHE")!=NULL;
      if(!nc && addr && !((s->si.gbuf_wmask>>i)&1)){ GLuint id=buf_cached(GL_SHADER_STORAGE_BUFFER,addr,size); if(id){ glBindBufferBase(GL_SHADER_STORAGE_BUFFER,slot*8+i,id); continue; } } }
    if(cap<size){ tmp=realloc(tmp,size); cap=size; }
    if(addr) gmem_read(addr,tmp,size); else memset(tmp,0,size);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER,g_sbuf[slot][i]); glBufferData(GL_SHADER_STORAGE_BUFFER,size,tmp,GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,slot*8+i,g_sbuf[slot][i]); } }
/* persistent ring for cbufs (GL 4.4 buffer_storage): memcpy + glBindBufferRange, no 64K realloc per update.
   fences per segment so we never overwrite something the GPU is still reading */
#define UR_SEGS 4
static GLuint g_ur; static u8* g_ur_ptr; static u32 g_ur_size, g_ur_head, g_ur_align=256; static u64 g_ur_abs; static GLsync g_ur_f[UR_SEGS];
static PFNGLFENCESYNCPROC p_fs; static PFNGLCLIENTWAITSYNCPROC p_cws; static PFNGLDELETESYNCPROC p_ds;
static u64 st_ur_wait; static u32 g_ub_bound[64];

static void ur_init(void){
  if(g_gles || getenv("SWR_NO_UBO_RING")) return;
  PFNGLBUFFERSTORAGEPROC bs=(PFNGLBUFFERSTORAGEPROC)backend_gl_proc("glBufferStorage"); PFNGLMAPBUFFERRANGEPROC mbr=(PFNGLMAPBUFFERRANGEPROC)backend_gl_proc("glMapBufferRange");
  p_fs=(PFNGLFENCESYNCPROC)backend_gl_proc("glFenceSync"); p_cws=(PFNGLCLIENTWAITSYNCPROC)backend_gl_proc("glClientWaitSync"); p_ds=(PFNGLDELETESYNCPROC)backend_gl_proc("glDeleteSync");
  if(!bs || !mbr || !p_fs || !p_cws || !p_ds) return;
  GLint al=256; glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT,&al); if(al<16) al=16; g_ur_align=(u32)al;
  g_ur_size=32u<<20;
  GLuint b=0; glGenBuffers(1,&b); glBindBuffer(GL_UNIFORM_BUFFER,b);
  GLbitfield fl=GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT;
  bs(GL_UNIFORM_BUFFER,(GLsizeiptr)g_ur_size+65536,NULL,fl);
  u8* ptr= glGetError()==GL_NO_ERROR? (u8*)mbr(GL_UNIFORM_BUFFER,0,(GLsizeiptr)g_ur_size+65536,fl) : NULL;
  while(glGetError()!=GL_NO_ERROR){}
  if(!ptr) return;
  g_ur=b; g_ur_ptr=ptr; }

static u32 ur_alloc(u32 sz){
  u32 need=(sz+g_ur_align-1)/g_ur_align*g_ur_align, seg=g_ur_size/UR_SEGS;
  u32 off=g_ur_head; if(off+need>g_ur_size){ g_ur_abs+=g_ur_size-off; off=0; }
  u32 cs=g_ur_head/seg; if(cs>=UR_SEGS) cs=UR_SEGS-1;
  u32 es=(off+need-1)/seg; if(es>=UR_SEGS) es=UR_SEGS-1;
  while(cs!=es){ if(g_ur_f[cs]) p_ds(g_ur_f[cs]); g_ur_f[cs]=p_fs(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
    cs=(cs+1)%UR_SEGS;
    if(g_ur_f[cs]){ GLenum rr=p_cws(g_ur_f[cs],GL_SYNC_FLUSH_COMMANDS_BIT,0);
      if(rr==GL_TIMEOUT_EXPIRED){ st_ur_wait++; p_cws(g_ur_f[cs],GL_SYNC_FLUSH_COMMANDS_BIT,2000000000ull); }
      p_ds(g_ur_f[cs]); g_ur_f[cs]=0; } }
  g_ur_head=off+need; g_ur_abs+=need; return off; }
typedef struct { u64 a, gen, abs; u32 sz, epoch, off; } UrC;
static UrC urc[4096];

static void upload_cbufs_ring(int stage, Sh* s){
  extern u64 g_gw_gen; extern int gpu_mem_dirty_since(u64,u64,u64);
  int bb=(stage==0? 0 : stage==4? 1 : 2)*18;
  for(int i=0;i<18;i++){ if(!(s->si.cbuf_mask&(1u<<i))) continue;
    u64 a=g_cb.cb[stage][i]; u32 sz=g_cb.cbsize[stage][i]; if(sz>65536) sz=65536; if(!sz) sz=16;
    if(s->si.cbuf_max[i] && sz>s->si.cbuf_max[i]) sz=s->si.cbuf_max[i];   /* only the bytes the shader reads */
    UrC* e=&urc[(u32)((a>>4)^(a>>16)^(a>>28))&4095]; u32 off;
    /* same submit/address, not GPU-written, ring hasn't wrapped: reuse */
    if(a && e->a==a && e->epoch==g_submit_epoch && e->sz>=sz && g_ur_abs-e->abs<g_ur_size/2 && !gpu_mem_dirty_since(e->gen,a,sz) && gmem_ptr(a,sz) && !memcmp(g_ur_ptr+e->off,gmem_ptr(a,sz),sz)){ off=e->off; st_cb_skip++; }
    else { st_cb_bytes+=sz; off=ur_alloc(sz); u8* dst=g_ur_ptr+off; u8* src= a? gmem_ptr(a,sz) : NULL;
      if(src) memcpy(dst,src,sz); else if(a) gmem_read(a,dst,sz); else memset(dst,0,sz);
      if(a){ e->a=a; e->sz=sz; e->epoch=g_submit_epoch; e->gen=g_gw_gen; e->off=off; e->abs=g_ur_abs; } }
    if(g_ub_bound[bb+i]!=off+1){ glBindBufferRange(GL_UNIFORM_BUFFER,bb+i,g_ur,off,65536); g_ub_bound[bb+i]=off+1; } } }

static void upload_cbufs(int stage, Sh* s){
  if(g_ur_ptr){ upload_cbufs_ring(stage,s); return; }
  for(int i=0;i<18;i++){ if(!(s->si.cbuf_mask&(1u<<i))) continue;
    u64 a=g_cb.cb[stage][i]; u32 sz=g_cb.cbsize[stage][i]; if(sz>65536) sz=65536; if(!sz) sz=16;
    if(s->si.cbuf_max[i] && sz>s->si.cbuf_max[i]) sz=s->si.cbuf_max[i];   /* only the bytes the shader reads */
    static u8* last[5][18]; static u32 last_sz[5][18];
    if(!last[stage][i]) last[stage][i]=calloc(1,65536);
    /* Guest CPU writes are not in gpu_mem_written. Always compare current bytes;
       address/submission identity alone does not establish unchanged contents. */
    st_cb_bytes+=sz;
    u8* src= a? gmem_ptr(a,sz) : NULL; u8 tmp_local[256]; u8* tmp=NULL;
    if(!src){ static u8 big[65536]; tmp= sz<=256? tmp_local : big; if(a) gmem_read(a,tmp,sz); else memset(tmp,0,sz); src=tmp; }
    if(last_sz[stage][i]<sz || memcmp(last[stage][i],src,sz)){      /* exact compare */
      memcpy(last[stage][i],src,sz); last_sz[stage][i]=sz;
      glBindBuffer(GL_UNIFORM_BUFFER,g_ubuf[stage][i]); glBufferData(GL_UNIFORM_BUFFER,65536,NULL,GL_STREAM_DRAW); glBufferSubData(GL_UNIFORM_BUFFER,0,sz,src); }
    glBindBufferBase(GL_UNIFORM_BUFFER,(stage==0? 0 : stage==4? 1 : 2)*18+i,g_ubuf[stage][i]); } }
/* multi-bind (GL 4.4): a stage's textures + samplers in two calls, none if nothing changed.
   other glBindTexture users (uploads, creation) use g_scratch_unit */
typedef void (APIENTRY*PFN_BT_)(GLuint,GLsizei,const GLuint*);
static PFN_BT_ p_bt, p_bs; static int g_scratch_unit=-1;
static GLuint g_tb_last[3][32], g_sb_last[3][32]; static int g_tb_n[3], g_tb_ok[3]; static u64 st_tb_skip;

static void mb_init(void){
  if(g_gles || getenv("SWR_NO_MULTIBIND")) return;
  GLint mu=0; glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS,&mu); if(mu<65) return;
  p_bt=(PFN_BT_)backend_gl_proc("glBindTextures"); p_bs=(PFN_BT_)backend_gl_proc("glBindSamplers");
  if(!p_bt || !p_bs){ p_bt=p_bs=NULL; return; }
  g_scratch_unit=mu-1; glActiveTexture(GL_TEXTURE0+g_scratch_unit); }
/* sampler type mismatch (2D vs array, cube vs array): use a texture view */
static PFNGLTEXTUREVIEWPROC p_tview; static u64 st_tex_view;

static GLuint tex_view(Tex* t, GLenum want){
  if(!p_tview || !t) return 0;
  if(t->view && t->view_target==want) return t->view;
  GLenum a=t->target; u32 layers= a==GL_TEXTURE_2D? 1 : a==GL_TEXTURE_CUBE_MAP? 6 : t->d; u32 nl;
  int ok2d= a==GL_TEXTURE_2D||a==GL_TEXTURE_2D_ARRAY||a==GL_TEXTURE_CUBE_MAP||a==GL_TEXTURE_CUBE_MAP_ARRAY;
  if(!ok2d) return 0;
  if(want==GL_TEXTURE_2D) nl=1; else if(want==GL_TEXTURE_2D_ARRAY) nl=layers;
  else if(want==GL_TEXTURE_CUBE_MAP){ if(layers<6) return 0; nl=6; }
  else if(want==GL_TEXTURE_CUBE_MAP_ARRAY){ if(layers<6) return 0; nl=layers/6*6; }
  else return 0;
  if(t->f.fmt==0 && !t->f.compressed) return 0;
  if(t->view){ glDeleteTextures(1,&t->view); t->view=0; g_fb_sig_ok=0; }
  GLuint v=0; glGenTextures(1,&v); p_tview(v,want,t->id,t->f.ifmt,0,t->levels,0,nl);
  if(glGetError()!=GL_NO_ERROR){ glDeleteTextures(1,&v); return 0; }
  t->view=v; t->view_target=want; st_tex_view++;
  t->vswz[0]=GL_RED; t->vswz[1]=GL_GREEN; t->vswz[2]=GL_BLUE; t->vswz[3]=GL_ALPHA;
  return v; }

static void mb_reset(void){ g_tb_ok[0]=g_tb_ok[1]=g_tb_ok[2]=0; if(g_scratch_unit>=0) glActiveTexture(GL_TEXTURE0+g_scratch_unit); }

static int ifmt_signed(GLenum f){ return f==GL_RGBA8I||f==GL_RGBA16I||f==GL_RGBA32I||f==GL_RG8I||f==GL_RG16I||f==GL_RG32I||f==GL_R8I||f==GL_R16I||f==GL_R32I; }

/* FS key: integer textures (u/isampler) and integer RT outputs (u/ivec4) */
/* texture handle for a slot: from the texture cbuf, or for bindless from whatever bank the shader read.
   split handles (tex | sampler) are combined the same way the shader does it */
static u32 sh_tex_handle(int stage, u32 tcb, const ShTex* st){
  u32 bank=(st->bindless? st->bl_cbuf : tcb)&31; if(bank>=18) return 0;   /* 18 banks per stage */
  u64 cba=g_cb.cb[stage][bank]; u32 h= cba? gmem_r32(cba+st->cbuf_off*4) : 0;
  if(st->bindless && st->bl2){ u32 b2=st->bl2_cbuf&31; u64 c2= b2<18? g_cb.cb[stage][b2] : 0; if(c2) h|=gmem_r32(c2+st->bl2_off*4); }
  return h; }

static Sh* fs_get_keyed(GpuChannel* ch, u64 addr){
  u32* r=ch->r3d; u32 ctl=r[0x487]; u32 count=ctl&15; if(count>8) count=8; u32 rm=0, rs=0;
  for(u32 i=0;i<count;i++){ u32 idx=(ctl>>(4+i*3))&7; u32* rt=&r[0x200+idx*0x10]; if(!rt[4]) continue; GlFmt f=rt_format(rt[4]); if(f.is_int){ rm|=1u<<i; if(ifmt_signed(f.ifmt)) rs|=1u<<i; } }
  g_fs_rtk=0; g_fs_rtm=0; g_fs_rts=0;
  Sh* s0=shader_get(addr,ST_FS,0,0); if(!s0) return s0;
  u32 tm=0, ts=0; int nt=s0->si.ntex; if(nt>32) nt=32;
  if(nt){ u32 tcb=r[0x982]; u64 tic_base=((u64)r[0x55D]<<32)|r[0x55E]; u64 cba=g_cb.cb[4][tcb&31];
    for(int i=0;i<nt;i++){ ShTex* st=&s0->si.tex[i]; if(st->shadow) continue; u32 handle=sh_tex_handle(4,tcb,st); (void)cba;
      u32 tic[8]; gmem_read(tic_base+(handle&0xFFFFF)*32ull,tic,32); u32 ty=(tic[0]>>7)&7;
      Tex* t=tex_from_tic(tic); if(t? t->f.is_int : (ty==3 || ty==4)){ tm|=1u<<i; if((t && ifmt_signed(t->f.ifmt)) || (!t && ty==3)) ts|=1u<<i; } } }
  rm&=s0->si.sph[18]? 0xFF : 0; if(!tm && !rm) return s0;
  g_fs_rtm=rm; g_fs_rts=rs; g_fs_rtk=rm*0x27D4EB2Fu ^ rs*0x165667B1u ^ 0x5A5A0000u;
  Sh* s=shader_get(addr,ST_FS,tm,ts); g_fs_rtk=0; g_fs_rtm=0; g_fs_rts=0; return s; }

/* transparent 0,0,0,0 for bad TICs; an empty GL unit returns opaque black */
static GLuint null_tex(GLenum tg){
  static GLuint n2, na, nc, nca; static const u8 z[4*6]={0};
  GLuint* p= tg==GL_TEXTURE_2D? &n2 : tg==GL_TEXTURE_2D_ARRAY? &na : tg==GL_TEXTURE_CUBE_MAP? &nc : tg==GL_TEXTURE_CUBE_MAP_ARRAY? &nca : NULL;
  if(!p) return 0; if(*p) return *p;
  glGenTextures(1,p); glBindTexture(tg,*p);
  if(tg==GL_TEXTURE_2D){ glTexStorage2D(tg,1,GL_RGBA8,1,1); glTexSubImage2D(tg,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,z); }
  else if(tg==GL_TEXTURE_CUBE_MAP){ glTexStorage2D(tg,1,GL_RGBA8,1,1); for(int f=0;f<6;f++) glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+f,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,z); }
  else { int d= tg==GL_TEXTURE_CUBE_MAP_ARRAY? 6 : 1; glTexStorage3D(tg,1,GL_RGBA8,1,1,d); glTexSubImage3D(tg,0,0,0,0,1,1,d,GL_RGBA,GL_UNSIGNED_BYTE,z); }
  mb_reset(); return *p; }

static int getenv_cached_rtswz(void){ static int v=-1; if(v<0) v=getenv("SWR_OLD_RT_SWIZZLE")!=NULL; return v; }   /* 1 = old behaviour, ignore reorders on RTs */
/* DLAA on the OpenGL renderer ([Graphics] dlss=1, dlss_gl.c runs NGX through Vulkan). Where it runs in Dread's frame
   (measured with the native Vulkan renderer, see snxvk dlss.c): the scene is the last full-size R11G11B10F target read by
   a full-size R11G11B10F pass before the first small (<= 1/4 width) pass, i.e. the post-effects input. DLSS runs on it at
   its first read after it was drawn this frame, so post effects, bloom and the HUD get the anti-aliased image. A new
   target has to win 30 frames in a row before DLSS moves to it; 30 frames without one (menus, videos) drop it */
extern int snxvk_dlss_mode; extern int dlgl_failed(void);
extern void dlgl_queue(GLuint color, GLuint depth, GLuint mv, GLenum zfmt, u32 w, u32 h, float jx, float jy, int reset);
static struct {
  u64 reads[16]; int nreads; u64 written[8]; int nwritten;
  u64 cand, cand_z, last_z, frame_best, frame_best_z; int small_seen; u32 scene_w, scene_h; Tex* last_rt0;
  u64 stable, stable_z, chosen, chosen_z; int streak, missing, done, reset, evals, evaluated;
  Tex *need, *need_z;    /* DLSS runs before the draw that first reads the scene target */
} g_dlg;
#ifdef SWR_RELEASE   /* the public release has no DLSS (NVIDIA's licence question is open): off whatever the .ini says */
static int dlg_on(void){ return 0; }
#else
static int dlg_on(void){ return snxvk_dlss_mode>0 && !g_gles && !dlgl_failed(); }
#endif
static int dlg_full_hdr(const Tex* t){ return t && t->f.ifmt==GL_R11F_G11F_B10F && g_dlg.scene_w && t->w==g_dlg.scene_w && t->h==g_dlg.scene_h; }
static int dlg_written(u64 a){ for(int i=0;i<g_dlg.nwritten;i++) if(g_dlg.written[i]==a) return 1; return 0; }
static Tex* dlg_find(u64 gva, int depth){ for(Tex* t=texh[hkey(gva)];t;t=t->next) if(t->gva==gva && t->is_rt && !!t->f.depth==depth) return t; return NULL; }
/* bind_textures: a render target about to be sampled. The chosen scene target's first read this frame runs DLSS */
static void dlg_read(Tex* t){
  if(!dlg_on() || !t->is_rt) return;
  if(dlg_full_hdr(t) && g_dlg.nreads<16){ int dup=0; for(int i=0;i<g_dlg.nreads;i++) dup|=g_dlg.reads[i]==t->gva; if(!dup) g_dlg.reads[g_dlg.nreads++]=t->gva; }
  if(g_dlg.done || !g_dlg.chosen || t->gva!=g_dlg.chosen || !dlg_written(t->gva)) return;
  g_dlg.done=1;
  Tex* z= g_dlg.chosen_z? dlg_find(g_dlg.chosen_z,1) : NULL;
  if(!z || z->w!=t->w || z->h!=t->h || t->target!=GL_TEXTURE_2D || z->target!=GL_TEXTURE_2D) return;
  g_dlg.need=t; g_dlg.need_z=z; }
/* r3d_draw_gl, after the draw's textures are bound: a new small pass ends the scene part of the frame; what it writes */
static void dlg_draw(void){
  if(!dlg_on()){ g_dlg.nreads=0; return; }
  Tex* rt0= g_nrt? g_rt[0] : NULL;
  if(g_zt && g_zt->f.depth && rt0 && rt0->w==g_zt->w && rt0->h==g_zt->h && g_zt->w>=640){ g_dlg.last_z=g_zt->gva; g_dlg.scene_w=g_zt->w; g_dlg.scene_h=g_zt->h; }
  if(rt0 && rt0!=g_dlg.last_rt0 && !g_dlg.small_seen && g_dlg.scene_w && rt0->w*4<=g_dlg.scene_w){   /* the first small pass */
    g_dlg.small_seen=1; g_dlg.frame_best=g_dlg.cand;
    for(int i=0;i<g_dlg.nreads;i++) g_dlg.frame_best=g_dlg.reads[i];   /* the target the bloom pass reads: after the post effects */
    g_dlg.frame_best_z=g_dlg.cand_z; }
  g_dlg.last_rt0=rt0;
  if(dlg_full_hdr(rt0)){
    if(!dlg_written(rt0->gva) && g_dlg.nwritten<8) g_dlg.written[g_dlg.nwritten++]=rt0->gva;
    for(int i=0;i<g_dlg.nreads;i++) if(g_dlg.reads[i]!=rt0->gva){ g_dlg.cand=g_dlg.reads[i]; g_dlg.cand_z=g_dlg.last_z; } }
  g_dlg.nreads=0; }
/* ---- jitter and motion vectors (as in the native Vulkan renderer's dlss.c): each depth-prepass draw of the scene
   (depth writes on, triangle list, no geometry/tessellation stage) has its positions captured by transform feedback;
   at the DLSS point the frame's captured draws are drawn again with this frame's positions and last frame's of the same
   draw (same vertex shader, buffers and draw parameters, the n-th such draw of the frame), depth test EQUAL against the
   scene depth, so a pixel gets the motion of the surface the game shows there. Jitter: the scene draws that write depth
   or test it EQUAL (prepass and G-buffer) move by a sub-pixel Halton(2,3) offset through the viewport */
#define DMV_BYTES (64u<<20)
#define DMV_MAX 2048
typedef struct { u64 key; u32 occ, off, verts; float vp[4], vpj[4], dr[2], po[3]; int clip; } DmvRec;
static struct { GLuint buf[2]; u32 used[2], nrec[2], cur; DmvRec rec[2][DMV_MAX];
  float jx, jy; u32 jframe; int jit_ok, jit_used, hold, misses, failed;
  GLuint prog, vao, fbo, mv; u32 mvw, mvh; GLint loc_c, loc_p; } g_dmv;
extern void* glq_alloc(size_t sz, size_t extra, void (*ex)(void*)); extern void glq_commit(void); extern int glq_active(void);
typedef void (APIENTRY* PFN_BTF)(GLenum); typedef void (APIENTRY* PFN_ETF)(void);
static PFN_BTF r_btf; static PFN_ETF r_etf;
static volatile int g_tf_missing;
static void e_tf(void* p){   /* on the GL thread, where the driver hands out the functions */
  if(!r_btf){ r_btf=(PFN_BTF)backend_gl_proc("glBeginTransformFeedback"); r_etf=(PFN_ETF)backend_gl_proc("glEndTransformFeedback"); }
  if(!r_btf || !r_etf){ g_tf_missing=1; return; }
  if(*(int*)p) r_btf(GL_TRIANGLES); else r_etf(); }
static void dmv_tf(int begin){   /* begin/end transform feedback: not in the queue's table, a custom entry on the GL thread */
  if(!glq_active()){ e_tf(&begin); return; }
  int* q=(int*)glq_alloc(sizeof(int),0,e_tf); *q=begin; glq_commit(); }
static float halton(u32 i, u32 b){ float f=1, r=0; for(;i;i/=b){ f/=(float)b; r+=f*(float)(i%b); } return r; }
static int dlg_scene_draw(void){ return dlg_on() && !g_dmv.failed && g_dlg.chosen && !g_dlg.done && g_zt && g_zt->gva==g_dlg.chosen_z; }
/* r3d_draw_gl, before apply_state: this draw's jitter */
static void dlg_jitter(u32* r){
  g_dl_jx=g_dl_jy=0;
  { static int nj=-1; if(nj<0) nj=getenv("SWR_DLSS_NO_JITTER")!=NULL; if(nj) return; }
  if(!g_dmv.jit_ok || !dlg_scene_draw()) return;
  int zw= r[0x4BA] && r[0x4B3], zeq= r[0x4B3] && cmp_func(r[0x4C3])==GL_EQUAL;
  if(!zw && !zeq) return;
  g_dl_jx=g_dmv.jx; g_dl_jy=g_dmv.jy; g_dmv.jit_used=1; }
/* r3d_draw_gl, around the draw call: capture a prepass draw's positions. 1 = capturing (end it after the draw) */
static int dlg_capture_begin(u32* r, Sh* vs, GLenum mode, int topology, int indexed, u32 first, u32 count, u32 base_vertex, u32 instances){
  if(g_tf_missing && !g_dmv.failed){ g_dmv.failed=1; LOG("dlaa (OpenGL): no transform feedback: no motion vectors\n"); }
  if(!vs->xfb || mode!=GL_TRIANGLES || topology!=4 || !dlg_scene_draw() || !(r[0x4BA] && r[0x4B3]) || ((r[0x820]|r[0x830]|r[0x840])&1)) return 0;
  u64 verts=(u64)count/3*3*instances; if(verts<3 || count%3) return 0;
  u32 c=g_dmv.cur; u64 bytes=verts*16;
  if(g_dmv.nrec[c]>=DMV_MAX || g_dmv.used[c]+bytes>DMV_BYTES) return 0;
  if(!g_dmv.buf[0]){
    glGenBuffers(2,g_dmv.buf);
    for(int i=0;i<2;i++){ glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,g_dmv.buf[i]); glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,DMV_BYTES,NULL,GL_DYNAMIC_COPY); }
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,0); }
  DmvRec* d=&g_dmv.rec[c][g_dmv.nrec[c]];
  u64 vb=((u64)r[0x701]<<32)|r[0x702], ib= indexed? (((u64)r[0x5F2]<<32)|r[0x5F3]) : 0;
  d->key=vs->hash ^ vb*0x9E3779B97F4A7C15ull ^ ib*0xC2B2AE3D27D4EB4Full ^ ((u64)first<<32 ^ count) ^ ((u64)base_vertex<<20) ^ ((u64)instances<<48) ^ (u64)indexed;
  d->occ=0; for(u32 i=0;i<g_dmv.nrec[c];i++) if(g_dmv.rec[c][i].key==d->key) d->occ++;
  d->off=g_dmv.used[c]; d->verts=(u32)verts;
  memcpy(d->vp,g_dl_vp,16); d->vpj[0]=g_dl_vp[0]+g_dl_jx; d->vpj[1]=g_dl_vp[1]+g_dl_jy; d->vpj[2]=g_dl_vp[2]; d->vpj[3]=g_dl_vp[3];
  memcpy(d->dr,g_dl_dr,8); memcpy(d->po,g_dl_po,12); d->clip=r[0x35F]&1;
  glBindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER,0,g_dmv.buf[c],(GLintptr)d->off,(GLsizeiptr)bytes);
  dmv_tf(1);
  g_dmv.nrec[c]++; g_dmv.used[c]+=(u32)bytes;
  return 1; }
static void dlg_capture_end(void){ dmv_tf(0); }
/* the motion-vector image for this frame (render size); 0 when there is nothing to draw */
static GLuint dlg_mv_pass(Tex* z, u32 w, u32 h){
  u32 c=g_dmv.cur, pv=c^1;
  if(g_dmv.failed || !g_dmv.nrec[c] || getenv("SWR_DLSS_NO_MV")) return 0;
  if(!g_dmv.prog){
    static const char* vs="#version 430 core\nlayout(location=0) in vec4 cur; layout(location=1) in vec4 prev; out vec4 vc; out vec4 vp;\n"
      "invariant gl_Position;\nvoid main(){ vc=cur; vp=prev; gl_Position=cur; }";
    static const char* fs="#version 430 core\nin vec4 vc; in vec4 vp; uniform vec4 vcur; uniform vec4 vprev; out vec2 mv;\n"
      "void main(){ vec2 nc=vc.xy/vc.w, np=vp.xy/vp.w; vec2 sc=vcur.xy+(nc*0.5+0.5)*vcur.zw; vec2 sp=vprev.xy+(np*0.5+0.5)*vprev.zw; mv=sp-sc; }";
    char err[512]; GLuint a=compile(GL_VERTEX_SHADER,vs,err,sizeof err), b=compile(GL_FRAGMENT_SHADER,fs,err,sizeof err);
    g_dmv.prog= a&&b? link2(a,b,0) : 0;
    if(!g_dmv.prog){ g_dmv.failed=1; LOG("dlaa (OpenGL): motion-vector program failed: no motion vectors\n"); return 0; }
    g_dmv.loc_c=glGetUniformLocation(g_dmv.prog,"vcur"); g_dmv.loc_p=glGetUniformLocation(g_dmv.prog,"vprev");
    glGenVertexArrays(1,&g_dmv.vao); glGenFramebuffers(1,&g_dmv.fbo); }
  if(g_dmv.mvw!=w || g_dmv.mvh!=h){
    if(g_dmv.mv) glDeleteTextures(1,&g_dmv.mv);
    glGenTextures(1,&g_dmv.mv); glBindTexture(GL_TEXTURE_2D,g_dmv.mv); glTexStorage2D(GL_TEXTURE_2D,1,GL_RG16F,(GLsizei)w,(GLsizei)h);
    g_dmv.mvw=w; g_dmv.mvh=h; mb_reset(); }
  glBindFramebuffer(GL_FRAMEBUFFER,g_dmv.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,g_dmv.mv,0);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
  glFramebufferTexture2D(GL_FRAMEBUFFER,z->f.fmt==GL_DEPTH_STENCIL? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,z->id,0);
  { GLenum b0=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&b0); }
  glDisable(GL_SCISSOR_TEST); glDisablei(GL_BLEND,0); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glDisable(GL_FRAMEBUFFER_SRGB);
  glColorMaski(0,1,1,1,1); glViewportIndexedf(0,0,0,(float)w,(float)h);
  { float zero[4]={0,0,0,0}; glClearBufferfv(GL_COLOR,0,zero); }
  glColorMaski(0,1,1,0,0); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_EQUAL); glDepthMask(GL_FALSE);
  glUseProgram(g_dmv.prog); glBindVertexArray(g_dmv.vao); glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
  u32 drawn=0; int clip=-1, po=-1;
  for(u32 i=0;i<g_dmv.nrec[c];i++){ const DmvRec* d=&g_dmv.rec[c][i]; const DmvRec* q=NULL;
    for(u32 k=0;k<g_dmv.nrec[pv] && !q;k++){ const DmvRec* e=&g_dmv.rec[pv][k]; if(e->key==d->key && e->occ==d->occ && e->verts==d->verts) q=e; }
    if(!q) continue;   /* new this frame: no previous position, zero motion */
    glBindBuffer(GL_ARRAY_BUFFER,g_dmv.buf[c]); glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,(void*)(size_t)d->off);
    glBindBuffer(GL_ARRAY_BUFFER,g_dmv.buf[pv]); glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,16,(void*)(size_t)q->off);
    glUniform4fv(g_dmv.loc_c,1,d->vp); glUniform4fv(g_dmv.loc_p,1,q->vp);
    glViewportIndexedf(0,d->vpj[0],d->vpj[1],d->vpj[2],d->vpj[3]); glDepthRangeIndexed(0,d->dr[0],d->dr[1]);
    if(p_clip && d->clip!=clip){ clip=d->clip; p_clip(0x8CA1,clip? 0x935F : 0x935E); }
    if((int)d->po[0]!=po || po){ po=(int)d->po[0]; if(po){ glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(d->po[1],d->po[2]); } else glDisable(GL_POLYGON_OFFSET_FILL); }
    glDrawArrays(GL_TRIANGLES,0,(GLsizei)d->verts); drawn++; }
  glDepthMask(GL_TRUE);
  /* the renderer's cached GL state is stale now: everything is set again by the next draw */
  state_dirty(); g_fb_sig_ok=0; g_clip_mode=-1; g_pipe_on=0;
  { static int said; if(said<2){ said++; LOG("dlaa (OpenGL): motion vectors from %u of %u captured draws\n",drawn,g_dmv.nrec[c]); } }
  return g_dmv.mv; }
/* r3d_draw_gl, after the draw's textures are bound: DLSS on the scene target this draw is about to read. The draw's own
   framebuffer, program and state are set again afterwards */
static int dlg_eval(void){
  Tex* t=g_dlg.need; Tex* z=g_dlg.need_z; g_dlg.need=g_dlg.need_z=NULL;
  if(!t || !z) return 0;
  u32 w=SC(t->w), h=SC(t->h);
  GLuint mv=dlg_mv_pass(z,w,h);
  dlgl_queue(t->id,z->id,mv,z->f.ifmt,w,h,g_dmv.jit_used? g_dmv.jx : 0.0f,g_dmv.jit_used? g_dmv.jy : 0.0f,g_dlg.reset);
  g_dlg.reset=0; t->wgen++; g_dlg.evals++; g_dlg.evaluated=1;
  { static int said; if(!said){ said=1; LOG("dlaa (OpenGL): scene target %llx %ux%u, depth %llx\n",(unsigned long long)t->gva,w,h,(unsigned long long)z->gva); } }
  return 1; }

/* each present */
static void dlg_frame(void){
  if(!dlg_on()) return;
  { static int started; if(!started){ started=1; dlgl_queue(0,0,0,0,0,0,0.0f,0.0f,0); } }   /* NGX up now, not at the first DLAA frame */
  if(g_dlg.small_seen && g_dlg.frame_best){ g_dlg.missing=0;
    if(g_dlg.frame_best==g_dlg.stable) g_dlg.streak=0;
    else if(!g_dlg.stable || ++g_dlg.streak>=30){ g_dlg.stable=g_dlg.frame_best; g_dlg.stable_z=g_dlg.frame_best_z; g_dlg.streak=0; g_dlg.reset=1; } }
  else if(++g_dlg.missing>=30) g_dlg.stable=0;
  g_dlg.chosen=g_dlg.stable; g_dlg.chosen_z=g_dlg.stable_z;
  g_dmv.cur^=1; g_dmv.nrec[g_dmv.cur]=0; g_dmv.used[g_dmv.cur]=0;
  { u32 i=(g_dmv.jframe++%8)+1; g_dmv.jx=halton(i,2)-0.5f; g_dmv.jy=halton(i,3)-0.5f; }
  if(g_dmv.jit_used && !g_dlg.evaluated){ g_dmv.hold=60; g_dmv.misses++; } else if(g_dmv.hold) g_dmv.hold--;   /* jittered without DLSS: that frame shook */
  g_dmv.jit_ok= g_dlg.evaluated && !g_dmv.hold; g_dmv.jit_used=0; g_dlg.evaluated=0;
  g_dlg.nwritten=g_dlg.nreads=0; g_dlg.cand=g_dlg.cand_z=0; g_dlg.frame_best=g_dlg.frame_best_z=0;
  g_dlg.small_seen=0; g_dlg.done=0; g_dlg.last_rt0=NULL; }

/* Resolution scale, downsampling and small-buffer passes: the game shrinks the scene for its bloom and glow with a few taps per
   output pixel, placed for the guest resolution. At 4x those taps meet 1 rendered pixel in 16, so small bright things
   (sparks, lamps) came out as dotted grids of glow and the lighting looked wrong (MEASURED 2026-10-22: light-res4 vs
   res1/res2). A filtered read of a scaled render target by a draw at least 2x smaller (guest sizes) gets a copy of the
   target box-averaged by 2^lv (a mip chain, read from level lv), so its taps see the averages they do at 1x. Only
   scales of 2 and more; SWR_NO_RT_PREFILTER=1 turns it off */
static u64 st_prefilter;
static GLuint rt_prefiltered(Tex* t){
  int lv=0; for(float k=2.0f; k<=g_scale+0.01f; k*=2.0f) lv++;
  if(!lv) return 0;
  u32 W=SC(t->w), H=SC(t->h);
  if(!t->pre || t->pre_lv!=lv || t->pre_w!=W || t->pre_h!=H){
    if(t->pre) glDeleteTextures(1,&t->pre);
    glGenTextures(1,&t->pre); glBindTexture(GL_TEXTURE_2D,t->pre); glTexStorage2D(GL_TEXTURE_2D,lv+1,t->f.ifmt,W,H);
    t->pre_lv=lv; t->pre_w=W; t->pre_h=H; t->pre_wgen=~0u; }
  if(t->pre_wgen!=t->wgen){
    glCopyImageSubData(t->id,GL_TEXTURE_2D,0,0,0,0,t->pre,GL_TEXTURE_2D,0,0,0,0,(GLsizei)W,(GLsizei)H,1);
    glBindTexture(GL_TEXTURE_2D,t->pre); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,lv);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,lv);   /* every read lands on the averaged level */
    t->pre_wgen=t->wgen; st_prefilter++; }
  return t->pre; }
static int downsample_read(const Tex* t, const ShTex* st){
  static int off=-1; if(off<0) off=getenv("SWR_NO_RT_PREFILTER")!=NULL;
  if(off || g_scale<1.99f || !t || !t->is_rt || t->target!=GL_TEXTURE_2D || t->f.depth || t->f.is_int || st->fetch_only || st->type!=1 || st->array || st->shadow) return 0;
  const Tex* o=g_rt[0]; if(!o || o==t) return 0;
  if(t->w>=2*o->w && t->h>=2*o->h) return 1;   /* shrinking it (the scene into the glow chain) */
  /* a small buffer (the glow chain: 320x180 in Dread) blurred in place or spread over the screen: its taps are a
     guest texel apart, which at 4x skipped 3 texels in 4 and left a grid every 20 pixels (MEASURED: light4-fix
     profile). Glow is soft by nature, so the averaged copy loses nothing */
  return t->w<=480 && t->h<=270; }

static void bind_textures(GpuChannel* ch, int stage, Sh* s){
  u32* r=ch->r3d; u32 tcb=r[0x982];
  u64 tic_base=((u64)r[0x55D]<<32)|r[0x55E], tsc_base=((u64)r[0x557]<<32)|r[0x558];
  int mb= p_bt!=NULL, slot=(stage==0? 0 : stage==4? 1 : 2), nt=s->si.ntex; if(nt>32) nt=32;
  GLuint tids[32], sids[32];
  for(int i=0;i<nt;i++){ ShTex* st=&s->si.tex[i];
    u32 handle=sh_tex_handle(stage,tcb,st);
    u32 tic_id=handle&0xFFFFF, tsc_id=(handle>>20)&0xFFF;
    u32 tic[8], tsc[8]; gmem_read(tic_base+tic_id*32ull,tic,32); gmem_read(tsc_base+tsc_id*32ull,tsc,32);
    Tex* t=tex_from_tic(tic);
    int unit=(stage==0? 0 : stage==4? 1 : 2)*(g_gles? 16 : 32)+i; if(!mb) glActiveTexture(GL_TEXTURE0+unit);
    /* the shader's sampler type wins */
    GLenum want= st->type==2? GL_TEXTURE_3D : st->type==3? (st->array? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_CUBE_MAP) : st->array? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    tids[i]=0;
    if(t && t->target==want && !(want==GL_TEXTURE_3D && t->zero3d && !t->is_rt)){ if(t->is_rt) dlg_read(t); GLuint use=t->id;
      if(want==GL_TEXTURE_2D && downsample_read(t,st)){ GLuint pf=rt_prefiltered(t); if(pf) use=pf; }
      { static int dbg=-1; if(dbg<0) dbg=getenv("SWR_PREFILTER_DEBUG")!=NULL;
        if(dbg && t->is_rt && g_rt[0] && g_rt[0]!=t && (t->w>g_rt[0]->w || t->h>g_rt[0]->h)){ static u64 seen[64]; static int ns; u64 k=((u64)t->w<<48)^((u64)t->h<<32)^((u64)g_rt[0]->w<<16)^g_rt[0]->h^((u64)st->fetch_only<<63)^((u64)stage<<60);
          int f=0; for(int q=0;q<ns;q++) if(seen[q]==k) f=1;
          if(!f && ns<64){ seen[ns++]=k; LOG("prefilter: stage %d reads rt %ux%u fmt %x into %ux%u: fetch_only %d type %d array %d shadow %d int %d -> %s\n",stage,t->w,t->h,t->f.ifmt,g_rt[0]->w,g_rt[0]->h,st->fetch_only,st->type,st->array,st->shadow,t->f.is_int,use!=t->id? "PREFILTERED" : "as is"); } } }
      if(mb) tids[i]=use; else glBindTexture(want,use); if(t->is_rt) st_tex_rt++; else st_tex_ok++;
      if(want==GL_TEXTURE_3D){ static u64 seen3[16]; static int n3; int k=0; for(;k<n3;k++) if(seen3[k]==t->gva) break;
        if(k==n3 && n3<16){ seen3[n3++]=t->gva; LOG("gpu: 3D texture %ux%ux%u at %llx %s\n",t->w,t->h,t->d,(unsigned long long)t->gva,t->is_rt? "rendered by the GPU (3D render target)" : "read from memory (has data)"); } } }
    else if(want==GL_TEXTURE_3D){ /* 3D LUT with no data (its writer isn't emulated yet): hand out a neutral table instead of black */
      /* identity table at the LUT's size. an sRGB->linear curve is the theoretically right thing for float
         LUTs but compared to real hw it came out much too dark; identity is close. SWR_LUT_LINEAR=1 for the curve */
      int lin_out= 0;
      static int force_lin=-1; if(force_lin<0) force_lin=getenv("SWR_LUT_LINEAR")!=NULL;
      if(force_lin && t && (t->f.ifmt==GL_R11F_G11F_B10F || t->f.ifmt==GL_RGBA16F || t->f.ifmt==GL_RGBA32F)) lin_out=1;
      /* must match the LUT's real size: the shader samples texel centers (c*(N-1)+0.5)/N, so a 32^3 read as 8^3
         lifts black to ~6% gray */
      int NN= t && t->w>=2 && t->w<=64 && t->w==t->h && t->d==t->w? (int)t->w : 32;
      static GLuint lut_tex[65][2]; GLuint* lt=&lut_tex[NN][lin_out];
      if(!*lt){ const int N=NN; float* px=malloc((size_t)N*N*N*4*4);
        for(int z=0;z<N;z++) for(int y=0;y<N;y++) for(int x=0;x<N;x++){ float* q=px+((size_t)(z*N+y)*N+x)*4; int c3[3]={x,y,z};
          for(int k=0;k<3;k++){ float v=(float)c3[k]/(float)(N-1); if(lin_out) v= v<=0.04045f? v/12.92f : powf((v+0.055f)/1.055f,2.4f); q[k]=v; } q[3]=1.f; }
        glGenTextures(1,lt); glBindTexture(GL_TEXTURE_3D,*lt); glTexStorage3D(GL_TEXTURE_3D,1,GL_RGBA16F,N,N,N); glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_MAX_LEVEL,0);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1); glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        glTexSubImage3D(GL_TEXTURE_3D,0,0,0,0,N,N,N,GL_RGBA,GL_FLOAT,px); free(px); }
      if(mb) tids[i]=*lt; else glBindTexture(GL_TEXTURE_3D,*lt); st_tex_mismatch++;
      { static u64 seenz[8]; static int nz; int k=0; u64 g= t? t->gva : 0; for(;k<nz;k++) if(seenz[k]==g) break;
        if(k==nz && nz<8){ seenz[nz++]=g; LOG("gpu: 3D texture (color LUT) %ux%ux%u at %llx (fmt %x) has no data: using a %s LUT (SWR_LUT_LINEAR=1: sRGB->linear)\n",
            t? t->w:0,t? t->h:0,t? t->d:0,(unsigned long long)g,t? t->fkey:0,lin_out? "neutral sRGB->linear" : "identity"); } } }
    else if(t && tex_view(t,want)){ GLuint v=t->view; if(mb) tids[i]=v; else glBindTexture(want,v); }
    else if(!t && null_tex(want)){ GLuint z=null_tex(want); if(mb) tids[i]=z; else glBindTexture(want,z); st_tex_null++; }
    else { if(!mb) glBindTexture(want,0); if(!t) st_tex_null++; else { st_tex_mismatch++;
        static int said; if(said<6){ said++; LOG("gpu: texture %ux%ux%u TIC type %u does not match the shader sampler (wants %x, has %x)\n",t->w,t->h,t->d,(tic[4]>>23)&0xF,want,t->target); } } }
    /* sRGB storage read through a non-sRGB TIC returns raw values on hw */
    /* sRGB storage read through a non-sRGB TIC returns raw values on hw. in GL the sampler's SRGB_DECODE wins
       over the texture's, so it's a sampler flag (tsc[7] bit 31) */
    int srgb_raw= t && t->f.ifmt==GL_SRGB8_ALPHA8 && !((tic[4]>>22)&1);
    /* compare mode from the shader's sampler type; a non-shadow sampler with compare on reads 0 in GL */
    if(st->shadow){ if(!((tsc[0]>>9)&1)){ tsc[0]|=1u<<9; tsc[0]=(tsc[0]&~(7u<<10))|(3u<<10); } } else tsc[0]&=~(1u<<9);
    if(t && t->f.is_int) tsc[1]=(tsc[1]&~0xF3u)|0x51u;   /* int textures must be NEAREST, LINEAR makes them incomplete */
    if(srgb_raw) tsc[7]|=0x80000000u;
    if(mb) sids[i]=sampler_get(tsc); else glBindSampler(unit,sampler_get(tsc));
    /* TIC swizzle */
    if(t && (t->target==want || (t->view && t->view_target==want))){ static const GLint sw[8]={GL_ZERO,GL_ZERO,GL_RED,GL_GREEN,GL_BLUE,GL_ALPHA,GL_ONE,GL_ONE};
      GLint m[4]={sw[(tic[0]>>19)&7],sw[(tic[0]>>22)&7],sw[(tic[0]>>25)&7],sw[(tic[0]>>28)&7]};
      /* RTs: only apply swizzles with constant 0/1 (e.g. an R8 HUD mask read as 1,1,1,R). pure reorders are
         left alone since the RT format already has the host order */
      /* RTs: the TIC swizzle is in memory component order but the GL texture is in logical order, so only RTs
         stored BGRA (A8R8G8B8 / X8R8G8B8) need R/B swapped. SMO reads its light buffer as GBAR and R8 masks as RRRR */
      if(t->is_rt && !getenv_cached_rtswz()){ u32 rf=t->fkey&0xFF; int bgra= rf==0xCF || rf==0xD0 || rf==0xE6 || rf==0xE7;
        if(bgra) for(int k=0;k<4;k++){ if(m[k]==GL_RED) m[k]=GL_BLUE; else if(m[k]==GL_BLUE) m[k]=GL_RED; } }
      else if(t->is_rt){ int hc=0; for(int k=0;k<4;k++) if(m[k]==GL_ZERO||m[k]==GL_ONE) hc=1;
        if(!hc){ m[0]=GL_RED; m[1]=GL_GREEN; m[2]=GL_BLUE; m[3]=GL_ALPHA; } }
      if(t->f.depth){ m[0]=GL_RED; m[1]=GL_RED; m[2]=GL_RED; m[3]=GL_RED; }
      int isv= t->target!=want; GLint* cur= isv? t->vswz : t->swz; GLuint tid= isv? t->view : t->id;
      if(memcmp(cur,m,sizeof m)){ memcpy(cur,m,sizeof m); if(mb) glBindTexture(want,tid); /* use the scratch unit */ else glBindTexture(want,tid); glTexParameteriv(want,GL_TEXTURE_SWIZZLE_RGBA,m); } }
  }
  if(mb && nt>0){ int base=slot*32;
    if(g_tb_ok[slot] && g_tb_n[slot]==nt && !memcmp(g_tb_last[slot],tids,(size_t)nt*4) && !memcmp(g_sb_last[slot],sids,(size_t)nt*4)){ st_tb_skip++; return; }
    if(!g_tb_ok[slot] || g_tb_n[slot]!=nt || memcmp(g_tb_last[slot],tids,(size_t)nt*4)) p_bt(base,nt,tids);
    if(!g_tb_ok[slot] || g_tb_n[slot]!=nt || memcmp(g_sb_last[slot],sids,(size_t)nt*4)) p_bs(base,nt,sids);
    memcpy(g_tb_last[slot],tids,(size_t)nt*4); memcpy(g_sb_last[slot],sids,(size_t)nt*4); g_tb_n[slot]=nt; g_tb_ok[slot]=1; } }

static u8* g_stage_buf; static size_t g_stage_cap;
/* buffer cache keyed by guest range, validated once per submit */
u32 g_submit_epoch;
typedef struct VbC { u64 start, len, hash, cpu, write_gen; u32 epoch, used; GLuint id; struct VbC* next; u32 stable, chk_epoch; int dirty; u32 csseen; u64 shash; u32 schk; } VbC;
/* 64K blocks (by gpu va, hashed) that got compute output, with a generation. a cached buffer on one of them
   gets reread. avoids walking the whole cache on every compute result */
static u32 g_csblk[65536], g_csgen;

static int cs_blk_dirty(u64 a, u64 n, u32 seen){ if(g_csgen==seen) return 0; for(u64 b=a>>16, e=(a+n-1)>>16; b<=e; b++) if(g_csblk[b&0xFFFF]>seen) return 1; return 0; }
static VbC* vbc[2048];
/* write-watch generation per 64K block; untouched ranges skip the reread/hash */
static u32* g_cgen; static int g_ww=-1; static u64 st_ww_skip; static int g_va_inval;
static void ww_touch(u64 cpu);

static void ww_mark(u64 off){ if(off<AS_SIZE) g_cgen[off>>16]=g_submit_epoch; }

static void ww_poll(void){
  if(!g_ww) return;
  if(g_ww<0){ g_ww= g_ww_ok? 1 : 0; if(g_ww){ g_cgen=calloc((size_t)(AS_SIZE>>16),4); if(!g_cgen) g_ww=0; else { extern void (*g_ww_touch)(u64); g_ww_touch=ww_touch; } } if(!g_ww) return; LOG("gpu: write tracking active (unchanged buffers are not re-read)\n"); }
  if(plat_ww_collect(g_mem,AS_SIZE,ww_mark)){ g_ww=0; LOG("gpu: write watch not available\n"); } }

static void ww_touch(u64 cpu){ if(g_cgen && cpu<AS_SIZE) g_cgen[cpu>>16]=g_submit_epoch+1; }

static int ww_dirty(u64 ga, u64 len, u32 since){
  for(u64 c=ga>>16, e=(ga+len-1)>>16; c<=e; c++) if(g_cgen[c]>since) return 1;
  return 0; }
void cs_mem_need(u64 a, u64 n); extern volatile int g_cs_any;

/* cheap check for big ranges: first/last 64 bytes + 62 spread out 32-byte blocks (~2K read). good at catching
   a new level loaded over old data, not small edits; the full check handles those */
static u64 sample_hash(u64 gva, u64 len){
  u8 tmp[2048+128]; u32 n=0;
  if(len<=sizeof tmp){ gmem_read(gva,tmp,len); return fast_hash(tmp,len,7); }
  gmem_read(gva,tmp,64); n=64; gmem_read(gva+len-64,tmp+n,64); n+=64;
  u64 step=(len-32)/62; for(int i=0;i<62;i++){ gmem_read(gva+(u64)i*step,tmp+n,32); n+=32; }
  return fast_hash(tmp,n,7); }
static int g_nosample=-1;
/* range fully inside a compute result that hasn't landed in guest memory yet: copy GPU->GPU instead of
   waiting, reading back and reuploading (SMO particles hit this every frame) */
static int cs_gpu_src(u64 a, u64 n, GLuint* buf, u32* off);
static u64 st_cs_gpucopy, st_cs_sync, st_cs_sync_ns;

static GLuint buf_cached(GLenum target, u64 start, u64 len){
  if(g_cs_any){ GLuint sb; u32 so;
    if(cs_gpu_src(start,len,&sb,&so)){
      u32 k=(u32)((start>>6)^(start>>20)^len)&2047;
      VbC* e=vbc[k]; for(;e;e=e->next) if(e->start==start && e->len==len) break;
      if(!e){ e=calloc(1,sizeof *e); e->start=start; e->len=len; glGenBuffers(1,&e->id); e->next=vbc[k]; vbc[k]=e; }
      glBindBuffer(GL_COPY_WRITE_BUFFER,e->id); glBufferData(GL_COPY_WRITE_BUFFER,(GLsizeiptr)len,NULL,GL_STATIC_DRAW);
      glBindBuffer(GL_COPY_READ_BUFFER,sb); glCopyBufferSubData(GL_COPY_READ_BUFFER,GL_COPY_WRITE_BUFFER,so,0,(GLsizeiptr)len);
      /* the CPU-side copy lands later (cs_apply marks the block); the reread then finds identical data */
      e->hash=~0ull; e->stable=0; e->epoch=g_submit_epoch; e->chk_epoch=g_submit_epoch; e->csseen=g_csgen; e->cpu=0;
      st_cs_gpucopy++; { static int said; if(!said){ said=1; LOG("gpu: compute results reused on the GPU (no wait for the readback)\n"); } }
      glBindBuffer(target,e->id); return e->id; }
    cs_mem_need(start,len); }
  u32 k=(u32)((start>>6)^(start>>20)^len)&2047;
  VbC* e=vbc[k]; for(;e;e=e->next) if(e->start==start && e->len==len) break;
  if(e && e->csseen!=g_csgen){ if(cs_blk_dirty(start,len,e->csseen)){ e->epoch=0; e->chk_epoch=g_submit_epoch-100000u; e->stable=0; } e->csseen=g_csgen; }   /* written by a compute, reread */
  extern u64 g_gw_gen; extern int gpu_mem_dirty_since(u64,u64,u64);
  if(e && gpu_mem_dirty_since(e->write_gen,start,len)){
    e->epoch=0; e->chk_epoch=g_submit_epoch-100000u; e->stable=0;
  }
  /* Small vertex/index streams include full-screen postprocessing quads.
     CPU writes are not tracked by the GPU write log; validate their bytes
     even when a command submission or write-watch page stays unchanged. */
  if(e && len>4096 && e->epoch==g_submit_epoch){ glBindBuffer(target,e->id); return e->id; }
  /* nobody touched it since last time (still rechecked every 240 submits) */
  if(e && len>4096 && g_ww>0 && e->cpu && g_submit_epoch-e->chk_epoch<240 && gmmu_translate(start)==e->cpu && !ww_dirty(e->cpu,len,e->chk_epoch)){
    e->epoch=g_submit_epoch; st_ww_skip++; glBindBuffer(target,e->id); return e->id; }
  /* big buffers that never changed (static geometry): check every 8 submits */
  if(e && len>4096 && g_ww<=0 && e->stable>=16 && len>=(64u<<10) && g_submit_epoch-e->chk_epoch<8){ e->epoch=g_submit_epoch; glBindBuffer(target,e->id); return e->id; }
  if(e && len>4096 && g_ww<=0 && e->stable>=32 && len<(64u<<10) && g_submit_epoch-e->chk_epoch<4){ e->epoch=g_submit_epoch; glBindBuffer(target,e->id); return e->id; }
  /* long-stable buffers: full check every 64/240 submits, sampled every 4 in between.
     no write tracking on Proton/Linux, without this the Deck hashed ~9 MB of vertices a frame */
  if(g_nosample<0) g_nosample=getenv("SWR_NO_SAMPLE_CHECK")!=NULL;
  if(e && len>4096 && g_ww<=0 && !g_nosample && e->stable>=64 && len>=4096 && g_submit_epoch-e->chk_epoch<(e->stable>=256? 240u : 64u)){
    if(g_submit_epoch-e->schk<4){ e->epoch=g_submit_epoch; glBindBuffer(target,e->id); return e->id; }
    u64 sh=sample_hash(start,len); e->schk=g_submit_epoch; st_vb_hashed+=2048;
    if(sh==e->shash){ e->epoch=g_submit_epoch; glBindBuffer(target,e->id); return e->id; } }
  u8* src=gmem_ptr(start,len); u64 cpu= src? (u64)(src-g_mem) : 0;
  if(!src){ if(g_stage_cap<len){ u8* nb=realloc(g_stage_buf,len); if(!nb) return 0; g_stage_buf=nb; g_stage_cap=len; } gmem_read(start,g_stage_buf,len); src=g_stage_buf; }
  u64 h=fast_hash(src,len,0); st_vb_hashed+=len;
  if(!e){ e=calloc(1,sizeof *e); e->start=start; e->len=len; e->csseen=g_csgen; glGenBuffers(1,&e->id); e->next=vbc[k]; vbc[k]=e; e->hash=h^1; }
  glBindBuffer(target,e->id);
  /* 2026-10-23: orphaning rewritten buffers and uploading them as STREAM_DRAW measured no gain at a 240 cap (worse
     seconds under 170): the per-draw driver stall in heavy rooms is not this upload */
  if(e->hash!=h){ e->hash=h; glBufferData(target,(GLsizeiptr)len,src,GL_STATIC_DRAW); st_vb_up+=len; e->stable=0; } else if(e->stable<100000) e->stable++;
  if(e->stable>=63 && len>=4096){ e->shash=sample_hash(start,len); e->schk=g_submit_epoch; }
  e->write_gen=g_gw_gen;
  e->cpu=cpu; e->epoch=g_submit_epoch; e->chk_epoch=g_submit_epoch; return e->id; }
/* ============================================================ F5 frame trace */
static int g_tr_arm, g_tr_on, g_tr_n, g_tr_img; static FILE* g_tr;
static Tex* g_tr_tex[2048]; static int g_tr_ntex;
static u64 g_tr_sh[1024]; static int g_tr_nsh;
static GLuint g_tr_fb, g_tr_tx;
#define TRW 256
#define TRH 144

static void trace_note(const char* fmt, ...){ if(!g_tr_on || !g_tr) return; va_list ap; va_start(ap,fmt); vfprintf(g_tr,fmt,ap); va_end(ap); }

void gpu_gl_note_compute(void){ }

void gpu_gl_trace(const char* s){ trace_note("%s",s); }

/* color | alpha side by side, row 0 at the top */
static void tr_save(const char* path, const u8* rgba, u32 w, u32 h){
  FILE* f=fopen(path,"wb"); if(!f) return; fprintf(f,"P6\n%u %u\n255\n",w*2,h);
  u8* row=malloc((size_t)w*6);
  for(u32 y=0;y<h;y++){ const u8* q=rgba+(size_t)y*w*4;
    for(u32 x=0;x<w;x++){ row[x*3]=q[x*4]; row[x*3+1]=q[x*4+1]; row[x*3+2]=q[x*4+2]; u8 a=q[x*4+3]; row[(w+x)*3]=a; row[(w+x)*3+1]=a; row[(w+x)*3+2]=a; }
    fwrite(row,1,(size_t)w*6,f); }
  free(row); fclose(f); }

static void tr_thumb(Tex* t, char* out){ state_dirty();
  strcpy(out,"-"); if(!t || t->f.depth || t->f.is_int || g_tr_img>=2500) return;
  if(!g_tr_fb){ glGenFramebuffers(1,&g_tr_fb); glGenTextures(1,&g_tr_tx); glBindTexture(GL_TEXTURE_2D,g_tr_tx); glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,TRW,TRH); }
  GLboolean sc=glIsEnabled(GL_SCISSOR_TEST), sr=glIsEnabled(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB); glColorMaski(0,1,1,1,1);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,g_rfbo); glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t->id,0); glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,g_tr_fb); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,g_tr_tx,0); { GLenum b=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&b); }
  glBlitFramebuffer(0,0,(GLint)SC(t->w),(GLint)SC(t->h),0,0,TRW,TRH,GL_COLOR_BUFFER_BIT,GL_LINEAR);
  static u8 px[TRW*TRH*4];
  glBindFramebuffer(GL_READ_FRAMEBUFFER,g_tr_fb); glReadBuffer(GL_COLOR_ATTACHMENT0); glPixelStorei(GL_PACK_ALIGNMENT,1); glReadPixels(0,0,TRW,TRH,GL_RGBA,GL_UNSIGNED_BYTE,px);
  glBindFramebuffer(GL_FRAMEBUFFER,g_fbo); if(sc) glEnable(GL_SCISSOR_TEST); if(sr) glEnable(GL_FRAMEBUFFER_SRGB);
  u64 h=fast_hash(px,sizeof px,0); if(h==t->tr_last){ strcpy(out,"(no change)"); return; }
  t->tr_last=h; char p[128]; snprintf(p,sizeof p,"captures/traces/d%04d.ppm",g_tr_n); tr_save(p,px,TRW,TRH); g_tr_img++; snprintf(out,32,"d%04d.ppm",g_tr_n); }

static int tr_tex_id(Tex* t){ if(!t) return -1; for(int i=0;i<g_tr_ntex;i++) if(g_tr_tex[i]==t) return i; if(g_tr_ntex<2048){ g_tr_tex[g_tr_ntex]=t; return g_tr_ntex++; } return -1; }

static void tr_shader(u64 addr, Sh* sh, int stage){
  if(!sh) return; for(int i=0;i<g_tr_nsh;i++) if(g_tr_sh[i]==sh->hash) return; if(g_tr_nsh>=1024) return; g_tr_sh[g_tr_nsh++]=sh->hash;
  u8* b=malloc(0x8000); gmem_read(addr,b,0x8000); char p[128];
  snprintf(p,sizeof p,"captures/traces/sh_%016llx_%s.bin",(unsigned long long)sh->hash,stage==ST_VS?"vs":"fs"); FILE* f=fopen(p,"wb"); if(f){ fwrite(b,1,0x8000,f); fclose(f); }
  ShaderInfo si; memset(&si,0,sizeof si); si.attr_int_mask=sh->si.attr_int_mask; si.attr_sint_mask=sh->si.attr_sint_mask; char* g=shader_to_glsl(b,0x8000,stage,&si);
  if(g){ snprintf(p,sizeof p,"captures/traces/sh_%016llx_%s.glsl",(unsigned long long)sh->hash,stage==ST_VS?"vs":"fs"); f=fopen(p,"wb"); if(f){ fputs(g,f); fclose(f); } free(g); }
  free(b);
  trace_note("  shader %016llx %s: %d untranslated%s%s, failed=%d\n",(unsigned long long)sh->hash,stage==ST_VS?"vs":"fs",sh->si.unsupported,sh->si.unsupported?" first: ":"",sh->si.first_unsupported,sh->failed); }

static void tr_textures(GpuChannel* ch, int stage, Sh* sh){
  u32* r=ch->r3d; u32 tcb=r[0x982];
  u64 tic_base=((u64)r[0x55D]<<32)|r[0x55E], tsc_base=((u64)r[0x557]<<32)|r[0x558];
  for(int i=0;i<sh->si.ntex;i++){ ShTex* st=&sh->si.tex[i];
    u32 handle=sh_tex_handle(stage,tcb,st);
    u32 tic[8], tsc[8]; gmem_read(tic_base+(handle&0xFFFFF)*32ull,tic,32); gmem_read(tsc_base+((handle>>20)&0xFFF)*32ull,tsc,32);
    Tex* t=tex_from_tic(tic);
    trace_note("  %s tex%d: sampler type%u%s%s  tic fmt=%05x %ux%ux%u type=%u levels=%u srgb=%u addr=%llx swz=%03x -> %s #%d %ux%u  tsc %08x %08x %08x\n",stage==0?"vs":"fs",i,
      st->type,st->array?" array":"",st->shadow?" shadow":"",tic[0]&0x7FFFF,(tic[4]&0xFFFF)+1,(tic[5]&0xFFFF)+1,((tic[5]>>16)&0x3FFF)+1,(tic[4]>>23)&0xF,((tic[3]>>28)&0xF)+1,(tic[4]>>22)&1,
      (unsigned long long)(((u64)(tic[2]&0xFFFF)<<32)|tic[1]),(tic[0]>>19)&0xFFF,t? (t->is_rt? "RT" : "tex") : "NULL",tr_tex_id(t),t?t->w:0,t?t->h:0,tsc[0],tsc[1],tsc[2]); } }

static void tr_draw(GpuChannel* ch, Sh* vs, Sh* fs, u64 vsa, u64 fsa, int indexed, u32 topology, u32 count, u32 instances){
  if(!g_tr_on || g_tr_n>=6000) return; u32* r=ch->r3d; g_tr_n++;
  float vt[6]; memcpy(vt,&r[0x280],24); float ref; memcpy(&ref,&r[0x4C4],4);
  trace_note("#%d DRAW vs=%016llx fs=%016llx count=%u inst=%u topo=%u idx=%d\n",g_tr_n,(unsigned long long)vs->hash,(unsigned long long)fs->hash,count,instances,topology,indexed);
  for(int i=0;i<g_nrt;i++) if(g_rt[i]) trace_note("  rt%d %llx %ux%u fmt=%x%s\n",i,(unsigned long long)g_rt[i]->gva,g_rt[i]->w,g_rt[i]->h,g_rt[i]->fkey,
      g_rt[i]->vol? (g_fb_lay[i]==-1? " (volume: all layers)" : " (volume: one layer)") : "");
  if(g_zt){ float dn,df; memcpy(&dn,&r[0x302],4); memcpy(&df,&r[0x303],4);
    trace_note("  z %llx %ux%u fmt=%x test=%u func=%x write=%u stencil=%u zscale=%.9g ztrans=%.9g range=%.9g..%.9g mode=%u\n",(unsigned long long)g_zt->gva,g_zt->w,g_zt->h,g_zt->fkey,r[0x4B3],r[0x4C3],r[0x4BA],r[0x4E0],vt[2],vt[5],dn,df,r[0x35F]&1); }
  trace_note("  blend en=%u,%u indep=%u common[sep=%u eq=%x src=%x dst=%x eqa=%x srca=%x dsta=%x] ind0[%x %x %x %x %x %x %x] mask=%x/%x common=%u srgb=%u alphatest=%u f=%x ref=%.3f cull=%u/%x vp(%.0f,%.0f,%.0f,%.0f) origin=%x yctl=%x vpctl=%x\n",
    r[0x4D8],r[0x4D9],r[0x4B9],r[0x4CF],r[0x4D0],r[0x4D1],r[0x4D2],r[0x4D3],r[0x4D4],r[0x4D6],r[0x780],r[0x781],r[0x782],r[0x783],r[0x784],r[0x785],r[0x786],r[0x680],r[0x681],r[0x3E4],r[0x4B8],r[0x4BB],r[0x4C5],ref,r[0x646],r[0x648],vt[0],vt[1],vt[3],vt[4],r[0x4EB],r[0x5E4],r[0x35F]);
  tr_shader(vsa,vs,ST_VS); if(fsa) tr_shader(fsa,fs,ST_FS);
  tr_textures(ch,0,vs); if(fsa) tr_textures(ch,4,fs);
  { static int all=-1; if(all<0) all=getenv("SWR_TRACE_CB_ALL")!=NULL;   /* test only: constants of every draw (camera matrices) */
  if(count<=6 || all){ /* fullscreen passes (tonemap, fog, lighting): dump their constants */
    int nv= all? 32 : 16;
    for(int st=0;st<2;st++){ Sh* sh= st? fs : vs; int stg= st? 4 : 0; if(!sh || (st && !fsa)) continue;
      for(int ci=0;ci<18;ci++) if(sh->si.cbuf_mask&(1u<<ci)){ u64 ca=g_cb.cb[stg][ci]; if(!ca) continue; float v[128]; gmem_read(ca,v,sizeof v);
        trace_note("  %s cb%d @%llx:",st? "fs" : "vs",ci,(unsigned long long)ca); for(int k=0;k<nv;k++) trace_note(" d[%d]=(%.6g,%.6g,%.6g,%.6g)",k,v[k*4],v[k*4+1],v[k*4+2],v[k*4+3]); trace_note("\n"); } } } }
  char nm[40]; tr_thumb(g_rt[0],nm); trace_note("  result rt0: %s\n",nm); }

static void trace_frame(void){
  if(g_tr_on){
    trace_note("==== end of frame. textures used: %d ====\n",g_tr_ntex);
    for(int i=0;i<g_tr_ntex;i++){ Tex* t=g_tr_tex[i];
      trace_note("tex #%d %llx %ux%ux%u fmt=%x target=%x levels=%u rt=%d\n",i,(unsigned long long)t->gva,t->w,t->h,t->d,t->fkey,t->target,t->levels,t->is_rt);
      if(!t->f.depth && !t->f.is_int && !t->f.compressed && (u64)t->w*t->h*t->d<=4096 && !g_gles && (t->target==GL_TEXTURE_2D || t->target==GL_TEXTURE_3D)){
        /* small textures (exposure, LUTs): first texel + average */
        u32 W=t->is_rt? SC(t->w) : t->w, H=t->is_rt? SC(t->h) : t->h, D= t->target==GL_TEXTURE_3D? t->d : 1; float* px=malloc((size_t)W*H*D*16);
        if(px){ glBindTexture(t->target,t->id); glPixelStorei(GL_PACK_ALIGNMENT,4); glGetTexImage(t->target,0,GL_RGBA,GL_FLOAT,px); double m[4]={0};
          for(u32 k=0;k<W*H*D;k++) for(int c=0;c<4;c++) m[c]+=px[k*4+c];
          trace_note("   texel0=(%.4g,%.4g,%.4g,%.4g) mean=(%.4g,%.4g,%.4g,%.4g)\n",px[0],px[1],px[2],px[3],m[0]/(W*H*D),m[1]/(W*H*D),m[2]/(W*H*D),m[3]/(W*H*D)); free(px); mb_reset(); } }
      if(t->f.depth || t->f.is_int || (t->target!=GL_TEXTURE_2D && t->target!=GL_TEXTURE_2D_ARRAY)) continue;
      /* GPU downsizes to max 512 and we read only that; layer 0 for arrays */
      u32 w=t->is_rt? SC(t->w) : t->w, h=t->is_rt? SC(t->h) : t->h;
      u32 st=1; while(w/st>512 || h/st>512) st*=2; u32 ow=w/st? w/st : 1, oh=h/st? h/st : 1;
      u8* o=malloc((size_t)ow*oh*4); if(!o) continue; int ok=0;
      if(!t->f.compressed){ GLuint tt=0, fb=0; glGenTextures(1,&tt); glBindTexture(GL_TEXTURE_2D,tt); glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,ow,oh);
        glGenFramebuffers(1,&fb); glBindFramebuffer(GL_READ_FRAMEBUFFER,g_rfbo);
        if(t->target==GL_TEXTURE_2D_ARRAY) glFramebufferTextureLayer(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,t->id,0,0); else glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t->id,0);
        glReadBuffer(GL_COLOR_ATTACHMENT0); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fb); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tt,0);
        if(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE && glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE){
          glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB); glColorMaski(0,1,1,1,1); { GLenum bb=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&bb); }
          glBlitFramebuffer(0,0,(GLint)w,(GLint)h,0,0,(GLint)ow,(GLint)oh,GL_COLOR_BUFFER_BIT,GL_LINEAR);
          glBindFramebuffer(GL_READ_FRAMEBUFFER,fb); glReadBuffer(GL_COLOR_ATTACHMENT0); glPixelStorei(GL_PACK_ALIGNMENT,1); glReadPixels(0,0,ow,oh,GL_RGBA,GL_UNSIGNED_BYTE,o); ok=1; }
        glBindFramebuffer(GL_FRAMEBUFFER,g_fbo); glDeleteFramebuffers(1,&fb); glDeleteTextures(1,&tt); state_dirty(); mb_reset(); }
      if(!ok && t->target==GL_TEXTURE_2D && (u64)w*h<=(1u<<20)){ u8* px=malloc((size_t)w*h*4);
        if(px){ glBindTexture(GL_TEXTURE_2D,t->id); glPixelStorei(GL_PACK_ALIGNMENT,1); glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
          for(u32 y=0;y<oh;y++) for(u32 x=0;x<ow;x++) memcpy(o+((size_t)y*ow+x)*4,px+((size_t)y*st*w+x*st)*4,4); free(px); ok=1; } }
      if(ok){ char p[160]; snprintf(p,sizeof p,"captures/traces/tex%03d_%llx_%ux%u_f%x%s.ppm",i,(unsigned long long)t->gva,t->w,t->h,t->fkey,t->is_rt?"_rt":""); tr_save(p,o,ow,oh); }
      free(o); }
    fclose(g_tr); g_tr=NULL; g_tr_on=0; LOG("trace: frame saved in captures/traces (%d draws, %d images)\n",g_tr_n,g_tr_img); fflush(NULL); }
  { static int envf=-2; if(envf==-2){ const char* e=getenv("SWR_TRACE"); envf= e? atoi(e) : -1; } if(envf>=0 && (int)g_frame==envf) g_tr_arm=1; }
  if(g_tr_arm){ g_tr_arm=0; plat_mkdir("captures"); plat_mkdir("captures/traces"); g_tr=fopen("captures/traces/trace.txt","w");
    g_tr_on= g_tr!=NULL; g_tr_n=0; g_tr_img=0; g_tr_ntex=0; g_tr_nsh=0;
    for(int k=0;k<TEXH;k++) for(Tex* t=texh[k];t;t=t->next) t->tr_last=0;
    LOG("trace: recording the next frame...\n"); }
}
static void lsfg_pump(void); static int g_lfn;
void cs_flush_pending(void);

/* Detailed paired lighting capture. Only enabled by SWR_CAPTURE_LIGHTING. */
typedef struct { u64 hash, addr; u32 size; float values[128]; } LightingPayload;
static LightingPayload lp_current[512],lp_previous[512];static u32 lp_count,lp_prev_count,lp_frame=~0u;
static void lighting_payload(Sh* fs){
  static int on=-1;if(on<0)on=getenv("SWR_CAPTURE_LIGHTING")!=NULL;if(!on)return;
  if(lp_frame!=g_frame){lp_frame=g_frame;lp_count=0;}
  if(lp_count>=512)return;LightingPayload*p=&lp_current[lp_count++];memset(p,0,sizeof *p);p->hash=fs->hash;p->addr=g_cb.cb[4][3];
  u32 n=g_cb.cbsize[4][3];if(n>sizeof p->values)n=sizeof p->values;p->size=n;if(p->addr&&n)gmem_read(p->addr,p->values,n);
}
void r3d_draw_gl(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance, u32 instances){
  u32* r=ch->r3d; g_draws++;
  if(g_lfn && !(g_draws&15)) lsfg_pump();   /* push pending generated frames out on time even mid-frame */
  if(!count) return;
  /* shaders */
  u64 code=((u64)r[0x582]<<32)|r[0x583];
  Sh* vs=NULL; Sh* fs=NULL;
  u32 attr_int=0, attr_sint=0;
  for(int i=0;i<32;i++){ u32 a=r[0x458+i]; u32 ty=(a>>27)&7; if(ty==3||ty==4){ attr_int|=1u<<i; if(ty==3) attr_sint|=1u<<i; } }
  /* stage slots: 0 VertexA, 1 VertexB, 2 TessCtrl, 3 TessEval, 4 Geometry, 5 Fragment.
     VertexB is always on even with the enable bit clear (NVN doesn't set it) */
  g_last_r3d=r;
  for(int i=1;i<6;i+=4){ u32 cfg=r[0x800+i*0x10]; if(i!=1 && !(cfg&1)) continue; u64 addr=code+r[0x801+i*0x10];
    if(i==1) TM(TM_SH,vs=shader_get(addr,ST_VS,attr_int,attr_sint)); else TM(TM_SH,fs=fs_get_keyed(ch,addr)); }
  if((r[0x820]|r[0x830]|r[0x840])&1){ /* tess / geometry: not translated yet, draw with just VS+FS */
    static u64 gseen[16]; static int ng; u64 ga=code+r[0x841]; int k=0; for(;k<ng;k++) if(gseen[k]==ga) break;
    if(k==ng && ng<16){ gseen[ng++]=ga; LOG("gpu: draw uses unsupported stages:%s%s%s (gs at %llx, rt0 %ux%u layers %u 3d %u) - drawn without them\n",(r[0x820]&1)?" TessCtrl":"",(r[0x830]&1)?" TessEval":"",(r[0x840]&1)?" Geometry":"",
        (unsigned long long)ga,r[0x202],r[0x203],r[0x206]&0xFFFF,(r[0x205]>>16)&1);
      if(r[0x840]&1){ plat_mkdir("captures"); char fp[160]; snprintf(fp,sizeof fp,"captures/gs_%02d.bin",ng); u8* b=malloc(0x8000); gmem_read(ga,b,0x8000); FILE* f=fopen(fp,"wb"); if(f){ fwrite(b,1,0x8000,f); fclose(f); } free(b); } }
    trace_note("WARNING: unsupported stages enabled (tess/geometry)\n"); }
  if(vs && !fs){ /* depth-only passes: use an empty FS */
    static Sh nullfs; static char nsrc[64]; if(!nullfs.gl && !nullfs.failed){ char e[512]; if(g_gles) snprintf(nsrc,sizeof nsrc,"#version 320 es\nvoid main(){}\n"); else snprintf(nsrc,sizeof nsrc,"#version %d core\nvoid main(){}\n",g_glsl_version); nullfs.stage=ST_FS; nullfs.hash=0x4E554C4C46530001ull; nullfs.src=nsrc; nullfs.gl=compile(GL_FRAGMENT_SHADER,nsrc,e,sizeof e); if(!nullfs.gl) nullfs.failed=1; }
    fs=&nullfs; }
  if(!vs || !fs){ st_noshader++; trace_note("DRAW SKIPPED: no shader\n"); return; }
  if(vs->failed || fs->failed){ st_shfail++; trace_note("DRAW SKIPPED: failed shader vs=%016llx(%d) fs=%016llx(%d)\n",(unsigned long long)vs->hash,vs->failed,(unsigned long long)fs->hash,fs->failed); return; }
  Prog* p=NULL; GLuint vpp=0, atp=0; GLint vpl=-1, atl=-1;
  /* draws into tables (3D/array RTs, or <= 64x64) usually happen once at level load. if we skip them while the
     shader compiles the table stays empty forever, so wait for the compile here (one hitch) */
  int must_wait=0;
  { u32 ctl=r[0x487]; if((ctl&15)>=1){ u32* q=&r[0x200+((ctl>>4)&7)*0x10];
      if(q[0]|q[1]){ if(((q[5]>>16)&1) || (q[6]&0xFFFF)>1 || (q[2] && q[3] && q[2]<=64 && q[3]<=64)) must_wait=1; } } }
  if(must_wait && g_sep>0){ u64 t0=plat_ns(); while((!sh_sep_ready(vs) || !sh_sep_ready(fs)) && plat_ns()-t0<3000000000ull) plat_sleep_ns(500000); st_waited++; }
  if(g_sep>0){ int rv, rf; TM(TM_SHREADY,(rv=sh_sep_ready(vs), rf=sh_sep_ready(fs)));
    if(!rv || !rf){ st_compiling++; trace_note("DRAW SKIPPED: compiling\n"); return; }
    if(!vs->sp || !fs->sp){ st_noprog++; trace_note("DRAW SKIPPED: shader without program vs=%016llx fs=%016llx\n",(unsigned long long)vs->hash,(unsigned long long)fs->hash); return; }
    vpp=vs->sp; vpl=vs->uloc; atp=fs->sp; atl=fs->uloc; }
  else {
  p=program_get(vs,fs);
  if(must_wait){ u64 t0=plat_ns(); while(!program_ready(p) && plat_ns()-t0<3000000000ull) plat_sleep_ns(500000); st_waited++; }
  if(!program_ready(p)){ st_compiling++; trace_note("DRAW SKIPPED: compiling\n"); return; }
  vpp=atp=p->p; vpl=p->vp_loc; atl=p->at_loc;
  if(!p->p){ st_noprog++;
    { static int said; if(said<5){ said++; LOG("gpu: program not linked vs=%016llx fs=%016llx active stages:%s%s%s%s%s%s\n",(unsigned long long)vs->hash,(unsigned long long)fs->hash,
        (r[0x800]&1)?" VertexA":"",(r[0x810]&1)?" VertexB":"",(r[0x820]&1)?" TessCtrl":"",(r[0x830]&1)?" TessEval":"",(r[0x840]&1)?" Geometry":"",(r[0x850]&1)?" Fragment":""); } } trace_note("DRAW SKIPPED: link failed vs=%016llx fs=%016llx\n",(unsigned long long)vs->hash,(unsigned long long)fs->hash); return; } }
  g_draw_layered=vs->si.writes_layer;
  TM(TM_STATE,bind_framebuffer(ch));
  if(g_nrt && g_rt[0]){ g_last_cw=g_rt[0]->w; g_last_ch=g_rt[0]->h; }
  { extern int g_opt_shadows; /* shadows=0: skip depth-only passes at a different size (shadow maps) */
    if(!g_opt_shadows && !g_nrt && g_zt && (g_zt->w!=g_last_cw || g_zt->h!=g_last_ch)){ trace_note("DRAW SKIPPED: shadow\n");
      /* ...and clear the map to far (1.0) once a frame so it reads as fully lit instead of black */
      { extern int g_frames; if(g_zt->skipclr!=g_frames+1){ g_zt->skipclr=g_frames+1;
        static PFNGLCLEARTEXIMAGEPROC p_cz; if(!p_cz) p_cz=(PFNGLCLEARTEXIMAGEPROC)backend_gl_proc("glClearTexImage");
        if(p_cz){ if(g_zt->f.fmt==GL_DEPTH_STENCIL){ struct { float d; u32 s; } ds={1.0f,0}; p_cz(g_zt->id,0,GL_DEPTH_STENCIL,GL_FLOAT_32_UNSIGNED_INT_24_8_REV,&ds); }
          else { float one=1.0f; p_cz(g_zt->id,0,GL_DEPTH_COMPONENT,GL_FLOAT,&one); } g_zt->wgen++; } } }
      return; } }
  if(!g_nrt && !g_zt){ st_nort++; trace_note("DRAW SKIPPED: no target\n"); return; }
  st_drawn++;
  if(g_sep>0){ if(!g_pipe_on){ glUseProgram(0); p_bpp(g_pipe); g_pipe_on=1; }
    if(g_pipe_vs!=vpp){ p_ups(g_pipe,0x00000001 /* VERTEX_SHADER_BIT */,vpp); g_pipe_vs=vpp; }
    if(g_pipe_fs!=atp){ p_ups(g_pipe,0x00000002 /* FRAGMENT_SHADER_BIT */,atp); g_pipe_fs=atp; } }
  else glUseProgram(p->p);
  dlg_jitter(r);
  TM(TM_STATE,apply_state(ch));
  lighting_payload(fs);
  /* Targeted diagnostic for Metroid Dread's deferred lighting shader only.
     Invalidate both state caches after an override so the next draw restores
     the guest state. Normal launches do not override any lighting tests. */
  if(fs->hash==0x9da586a770c2665aull){
    static int mask_test=-1; if(mask_test<0){ const char* e=getenv("SWR_LIGHTING_MASK_TEST"); mask_test=e? atoi(e):0; }
    if(mask_test==1 || mask_test==2){
      static int said; if(!said){ said=1; LOG("lighting diagnostic: shader %016llx, bypass %s; stencil front func/ref/mask=%x/%x/%x ops=%x/%x/%x write=%x, back enable=%x func/ref/mask=%x/%x/%x, depth enable/func=%x/%x\n",
        (unsigned long long)fs->hash, mask_test==1? "stencil":"depth",
        r[0x4E4],r[0x4E5],r[0x4E6],r[0x4E1],r[0x4E2],r[0x4E3],r[0x4E7],
        r[0x565],r[0x569],r[0x3D5],r[0x3D7],r[0x4B3],r[0x4C3]); }
      if(mask_test==1) glDisable(GL_STENCIL_TEST); else glDisable(GL_DEPTH_TEST);
      state_dirty();
    }
  }

  { float vt[2]; memcpy(vt,&r[0x280],8); float fl[4]={ vt[1]<0? -1.f : 1.f, p_clip? 0.f : (float)(r[0x35F]&1), 0, 0 };
    if(vpl>=0){ if(g_sep>0){ if(!vs->lu_ok || memcmp(vs->lu,fl,16)){ memcpy(vs->lu,fl,16); vs->lu_ok=1; glProgramUniform4fv(vpp,vpl,1,fl); } } else glProgramUniform4fv(vpp,vpl,1,fl); } }
  if(atl>=0){ /* alpha test: 0x4BB enable, 0x4C4 ref, 0x4C5 func (GL 0x200.. or D3D 1..8) */
    u32 fn=r[0x4C5]; int f= fn>=0x200 && fn<=0x207? (int)(fn-0x200) : fn>=1 && fn<=8? (int)fn-1 : 7; float ref; memcpy(&ref,&r[0x4C4],4);
    float at[4]={ (r[0x4BB]&1)? 1.f : 0.f, (float)f, ref, 0 };
    if(g_sep>0){ if(!fs->lu_ok || memcmp(fs->lu,at,16)){ memcpy(fs->lu,at,16); fs->lu_ok=1; glProgramUniform4fv(atp,atl,1,at); } } else glProgramUniform4fv(atp,atl,1,at); }
  TM(TM_CB,(upload_cbufs(0,vs), upload_cbufs(4,fs)));
  TM(TM_GB,(bind_gbufs(0,vs), bind_gbufs(4,fs)));
  TM(TM_TEX,(bind_textures(ch,0,vs), bind_textures(ch,4,fs)));
  dlg_draw();
  if(g_dlg.need && dlg_eval()){   /* DLSS ran before this draw: its framebuffer, program and state again */
    bind_framebuffer(ch);
    if(g_sep>0){ glUseProgram(0); p_bpp(g_pipe); g_pipe_on=1; p_ups(g_pipe,0x00000001,vpp); g_pipe_vs=vpp; p_ups(g_pipe,0x00000002,atp); g_pipe_fs=atp; }
    else glUseProgram(p->p);
    dlg_jitter(r); apply_state(ch); mb_reset(); TM(TM_TEX,(bind_textures(ch,0,vs), bind_textures(ch,4,fs))); }
  if(g_diag_draws>0 && g_rt[0] && g_rt[0]->w>=640){ g_diag_draws--; capture_draw(ch,indexed,first,count);
    float vt[6]; memcpy(vt,&r[0x280],24);
    LOG("draw %llu: vs %016llx fs %016llx rt0 %ux%u fmt=%x zt=%s count=%u inst=%u topo=%u\n",(unsigned long long)g_draws,(unsigned long long)vs->hash,(unsigned long long)fs->hash,
      g_rt[0]->w,g_rt[0]->h,g_rt[0]->fkey,g_zt? "yes":"no",count,instances,topology);
    LOG("   viewport scale(%.1f,%.1f,%.2f) center(%.1f,%.1f,%.2f) zmode=%u scissor=%u mask0=%x common=%u blend0=%u eq=%x src=%x dst=%x ztest=%u zfunc=%x zwrite=%u cull=%u\n",
      vt[0],vt[1],vt[2],vt[3],vt[4],vt[5],r[0x35F]&1,r[0x380]&1,r[0x680],r[0x3E4],r[0x4D8],r[0x4B9]? r[0x781] : r[0x4D0],r[0x4B9]? r[0x782] : r[0x4D1],r[0x4B9]? r[0x783] : r[0x4D2],r[0x4B3],r[0x4C3],r[0x4BA],r[0x646]);
    LOG("   vs: %d textures, cbufs %x   fs: %d textures, cbufs %x, sph outputs %x\n",vs->si.ntex,vs->si.cbuf_mask,fs->si.ntex,fs->si.cbuf_mask,fs->si.sph[18]);
    for(int k=0;k<fs->si.ntex && k<6;k++){ ShTex* st=&fs->si.tex[k]; u32 tcb=r[0x982]; u32 hd=sh_tex_handle(4,tcb,st);
      u64 tb=((u64)r[0x55D]<<32)|r[0x55E]; u32 tic[8]; gmem_read(tb+(hd&0xFFFFF)*32ull,tic,32);
      LOG("   fs tex%d: handle=%x tic fmt=%x %ux%u type=%u addr=%llx\n",k,hd,tic[0]&0x7FFFF,(tic[4]&0xFFFF)+1,(tic[5]&0xFFFF)+1,(tic[4]>>23)&0xF,(unsigned long long)(((u64)(tic[2]&0xFFFF)<<32)|tic[1])); }
  }
  /* vertex attribs */
  u64 tb0_=plat_ns();
  glBindVertexArray(g_vao);
  u32 used=vs->si.in_mask;
  /* attrib state cache for g_vao */
  static u32 va_en; static u32 va_key[16][8]; static int va_const[16];
  if(g_va_inval){ g_va_inval=0; memset(va_key,0,sizeof va_key); }
#define VA_OFF(i_) do{ if(va_en&(1u<<(i_))){ glDisableVertexAttribArray(i_); va_en&=~(1u<<(i_)); } }while(0)
  for(int i=0;i<16;i++){
    if(!(used&(1u<<i))){ VA_OFF(i); continue; }
    u32 a=r[0x458+i]; u32 buf=a&31; int constant=(a>>6)&1; u32 off=(a>>7)&0x3FFF; u32 size=(a>>21)&0x3F, ty=(a>>27)&7; int bgra=(a>>31)&1;
    u32* va=&r[0x700+buf*4];
    if(constant || !(va[0]&0x1000)){ VA_OFF(i); if(!va_const[i]){ glVertexAttrib4f(i,0,0,0,1); va_const[i]=1; } continue; }
    u64 start=((u64)va[1]<<32)|va[2]; u64 lim=((u64)r[0x7C0+buf*2]<<32)|r[0x7C1+buf*2]; u32 stride=va[0]&0xFFF;
    u64 len= lim>=start? lim-start+1 : 0; if(len>64u<<20) len=64u<<20;
    if(!len) { VA_OFF(i); continue; }
    GLuint vbid=buf_cached(GL_ARRAY_BUFFER,start,len);
    u32 cnt=attr_count(size), bits=attr_bits(size);
    GLenum gt; GLboolean norm= ty==1||ty==2;
    if(size==0x30) gt= ty==1? GL_INT_2_10_10_10_REV : GL_UNSIGNED_INT_2_10_10_10_REV;
    else if(size==0x31) gt=GL_UNSIGNED_INT_10F_11F_11F_REV;
    else if(ty==7) gt= bits==32? GL_FLOAT : GL_HALF_FLOAT;
    else if(ty==1||ty==3||ty==6) gt= bits==32? GL_INT : bits==16? GL_SHORT : GL_BYTE;
    else gt= bits==32? GL_UNSIGNED_INT : bits==16? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
    GLint gcnt= (bgra && cnt==4)? GL_BGRA : (GLint)cnt; if(gcnt==GL_BGRA) norm=GL_TRUE;
    u32 div= r[0x620+buf]? (va[3]? va[3] : 1) : 0;
    u32 key[8]={vbid,(u32)gcnt,gt,(u32)norm|((ty==3||ty==4)? 2u : 0u),stride,off,div,cnt};
    if(!(va_en&(1u<<i))){ glEnableVertexAttribArray(i); va_en|=1u<<i; }
    va_const[i]=0;
    if(memcmp(va_key[i],key,sizeof key)){ memcpy(va_key[i],key,sizeof key);
      if(ty==3||ty==4) glVertexAttribIPointer(i,cnt,gt,stride,(void*)(size_t)off);
      else glVertexAttribPointer(i,gcnt,gt,norm,stride,(void*)(size_t)off);
      glVertexAttribDivisor(i,div); }
  }
  g_tm[TM_BUF]+=plat_ns()-tb0_; tb0_=plat_ns();
  GLenum mode=topo(topology);
  if(topology==7 || topology==8 || topology==9){ mode=GL_TRIANGLE_FAN; if(topology==7) mode=GL_TRIANGLES; if(topology==8) mode=GL_TRIANGLE_STRIP; }   /* quad strip = tri strip */
  if(indexed){
    u32 fmt=r[0x5F6]; u32 isz= fmt==0? 1 : fmt==1? 2 : 4; u64 ia=((u64)r[0x5F2]<<32)|r[0x5F3];
    u64 ilen=((u64)first+count)*isz; u8* src=NULL;   /* 64-bit: first+count can wrap */
    if(topology==7){ src=gmem_ptr(ia,ilen); if(!src){ if(g_stage_cap<ilen){ u8* nb=realloc(g_stage_buf,ilen); if(!nb) return; g_stage_buf=nb; g_stage_cap=ilen; } gmem_read(ia,g_stage_buf,ilen); src=g_stage_buf; } }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,g_ibuf);
    GLenum it= isz==1? GL_UNSIGNED_BYTE : isz==2? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    if(topology==7){ /* quads -> tris */
      u32 nq=count/4; u32* idx=malloc((size_t)nq*6*4); if(!idx) return;
      for(u32 k=0;k<nq;k++){ u32 v[4]; for(int j=0;j<4;j++){ u64 o=((u64)first+(u64)k*4+j)*isz; v[j]= isz==1? src[o] : isz==2? *(u16*)(src+o) : *(u32*)(src+o); }
        u32 t6[6]={v[0],v[1],v[2],v[0],v[2],v[3]}; memcpy(idx+k*6,t6,24); }
      glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)nq*24,idx,GL_STREAM_DRAW); free(idx);
      glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES,nq*6,GL_UNSIGNED_INT,0,instances,(GLint)base_vertex,base_instance);
    } else {
      buf_cached(GL_ELEMENT_ARRAY_BUFFER,ia+(u64)first*isz,(u64)count*isz);   /* just this draw's range */
      int cap=dlg_capture_begin(r,vs,mode,(int)topology,1,first,count,base_vertex,instances);
      glDrawElementsInstancedBaseVertexBaseInstance(mode,count,it,(void*)0,instances,(GLint)base_vertex,base_instance);
      if(cap) dlg_capture_end();
    }
  } else {
    if(topology==7){ u32 nq=count/4; u32* idx=malloc((size_t)nq*24+4); for(u32 k=0;k<nq;k++){ u32 b=first+k*4; u32 t6[6]={b,b+1,b+2,b,b+2,b+3}; memcpy(idx+k*6,t6,24); }
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,g_ibuf); glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)nq*24,idx,GL_STREAM_DRAW); free(idx);
      glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES,nq*6,GL_UNSIGNED_INT,0,instances,0,base_instance); }
    else { int cap=dlg_capture_begin(r,vs,mode,(int)topology,0,first,count,0,instances);
      glDrawArraysInstancedBaseInstance(mode,first,count,instances,base_instance);
      if(cap) dlg_capture_end(); }
  }
  g_tm[TM_DRAW]+=plat_ns()-tb0_;
  tr_draw(ch,vs,fs,code+r[0x801+1*0x10],(r[0x800+5*0x10]&1)? code+r[0x801+5*0x10] : 0,indexed,topology,count,instances);
}

void r3d_clear_gl(GpuChannel* ch, u32 v){ state_dirty(); g_tmap_gen++;
  u32* r=ch->r3d; st_clears++;
  u32 rt=(v>>6)&15;
  g_clear_layer=(int)((v>>10)&0x7FF); bind_framebuffer(ch); g_clear_layer=-1;
  if(r[0x43E]&0x10 && (r[0x380]&1)){ u32 xs=r[0x381], ys=r[0x382]; glEnable(GL_SCISSOR_TEST); glScissorIndexed(0,(GLint)((xs&0xFFFF)*g_scale),(GLint)((ys&0xFFFF)*g_scale),(GLsizei)(((xs>>16)-(xs&0xFFFF))*g_scale+0.5f),(GLsizei)(((ys>>16)-(ys&0xFFFF))*g_scale+0.5f)); }
  else glDisable(GL_SCISSOR_TEST);
  if(v&0x3C){ Tex* t= rt<8? g_rt[rt] : NULL;
    if(!t && rt<8){ /* clear targets an RT that isn't bound (Kirby does this): clear the texture directly */
      u32* q=&r[0x200+rt*0x10]; u64 a=((u64)q[0]<<32)|q[1];
      if(a && q[2] && q[3] && q[4]){ Tex* ct=rt_texture(a,q[2],q[3],q[4],q[5],0);
        static PFNGLCLEARTEXIMAGEPROC p_cti; if(!p_cti) p_cti=(PFNGLCLEARTEXIMAGEPROC)backend_gl_proc("glClearTexImage");
        if(ct && p_cti && !ct->f.depth){ float cc[4]; memcpy(cc,&r[0x360],16);
          if(ct->f.is_int){ GLuint ui[4]={(GLuint)cc[0],(GLuint)cc[1],(GLuint)cc[2],(GLuint)cc[3]}; p_cti(ct->id,0,ct->f.fmt,GL_UNSIGNED_INT,ui); }
          else p_cti(ct->id,0,GL_RGBA,GL_FLOAT,cc);
          ct->wgen++; static int said; if(!said){ said=1; LOG("gpu: clear of a RT outside the active list: cleared directly\n"); } } } }
    /* single float RT with color + depth (+stencil): one glClear so the driver can fast clear it all at once */
    if(t && rt==0 && g_nrt==1 && !t->f.is_int && (v&0x3C)==0x3C && (v&1) && g_zt && !getenv("SWR_NO_CLEARMERGE")){
      float cc[4], dz; memcpy(cc,&r[0x360],16); memcpy(&dz,&r[0x364],4);
      glColorMaski(0,1,1,1,1); glClearColor(cc[0],cc[1],cc[2],cc[3]); glDepthMask(GL_TRUE); glClearDepth((double)dz);
      GLbitfield m=GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT;
      if((v&2) && g_zt->f.fmt==GL_DEPTH_STENCIL){ glStencilMaskSeparate(GL_FRONT_AND_BACK,0xFF); glClearStencil((GLint)r[0x368]); m|=GL_STENCIL_BUFFER_BIT; }
      glClear(m); v&=~3u; t=NULL; }
    if(t){ glColorMaski(rt,(v>>2)&1,(v>>3)&1,(v>>4)&1,(v>>5)&1); float cc[4]; memcpy(cc,&r[0x360],16);
      if(t->f.is_int){ GLuint ui[4]={(GLuint)cc[0],(GLuint)cc[1],(GLuint)cc[2],(GLuint)cc[3]}; glClearBufferuiv(GL_COLOR,rt,ui); }
      else glClearBufferfv(GL_COLOR,rt,cc); } }
  if(g_tr_on && g_tr_n<6000){ g_tr_n++; float cc[4]; memcpy(cc,&r[0x360],16); char nm[40]; Tex* t= rt<8? g_rt[rt] : NULL;
    { float dz; memcpy(&dz,&r[0x364],4); trace_note("#%d CLEAR rt%u %llx mask=%x color=(%.2f,%.2f,%.2f,%.2f) depth=%.9g z=%llx\n",g_tr_n,rt,(unsigned long long)(t?t->gva:0),v&0x3F,cc[0],cc[1],cc[2],cc[3],dz,(unsigned long long)(g_zt?g_zt->gva:0)); }
    if(v&0x3C){ tr_thumb(t,nm); trace_note("  result: %s\n",nm); } }
  if((v&3) && g_zt){ float dz; memcpy(&dz,&r[0x364],4); glDepthMask(GL_TRUE); glStencilMaskSeparate(GL_FRONT_AND_BACK,0xFF);
    if((v&3)==3 && g_zt->f.fmt==GL_DEPTH_STENCIL) glClearBufferfi(GL_DEPTH_STENCIL,0,dz,(GLint)r[0x368]);
    else if(v&1) glClearBufferfv(GL_DEPTH,0,&dz); else { GLint s=(GLint)r[0x368]; glClearBufferiv(GL_STENCIL,0,&s); } }
}

/* ============================================================ present */
static GLuint g_blit_prog, g_cpu_tex; static int g_cpu_w, g_cpu_h;
/* Final scale to the window when the game image is larger (resolution > 1): one bilinear tap per pixel skips
   source pixels and moving detail shimmers. Average a 4x4 grid of bilinear taps over each pixel's footprint
   instead (covers every source pixel up to 8 texels per pixel). At 1:1 or upscaling it is a single tap. */
static GLuint g_down_prog; static GLint g_down_xf=-1, g_down_px=-1;
/* px.z = 1: anti-aliasing (ini antialiasing=1), FXAA 3.11 quality (12 taps of edge search) on the game image's own
   texels, on the gamma-encoded values as FXAA expects; only where no supersampling already averages the image */
int g_opt_aa;
/* px.w > 0: sharpening (ini sharpening=1..100), contrast-adaptive like AMD CAS: each pixel minus a share of its four
   neighbours, the share scaled down where the neighbourhood is already near black or white, so no halos or clipping.
   Neighbours one window pixel away (the picture as it is shown, after downscale or FXAA) */
int g_opt_sharp;
static const char* DOWN_FS="#version 430 core\nin vec2 uv; out vec4 o; layout(binding=0) uniform sampler2D t; uniform vec4 px;\n"
  "float L(vec2 p){ return dot(texture(t,p).rgb,vec3(0.299,0.587,0.114)); }\n"
  "vec3 fxaa(vec2 uv){ vec2 rc=1.0/vec2(textureSize(t,0)); vec3 rgbM=texture(t,uv).rgb; float M=dot(rgbM,vec3(0.299,0.587,0.114));\n"
  " float N=L(uv+vec2(0,-rc.y)), S=L(uv+vec2(0,rc.y)), E=L(uv+vec2(rc.x,0)), W=L(uv+vec2(-rc.x,0));\n"
  " float mx=max(max(N,W),max(E,max(S,M))), mn=min(min(N,W),min(E,min(S,M))), range=mx-mn;\n"
  " if(range<max(0.0312,mx*0.125)) return rgbM;\n"
  " float NW=L(uv+vec2(-rc.x,-rc.y)), NE=L(uv+vec2(rc.x,-rc.y)), SW=L(uv+vec2(-rc.x,rc.y)), SE=L(uv+vec2(rc.x,rc.y));\n"
  " float NS=N+S, WE=W+E, subNSWE=NS+WE, eH1=-2.0*M+NS, eV1=-2.0*M+WE, NESE=NE+SE, NWNE=NW+NE, eH2=-2.0*E+NESE, eV2=-2.0*N+NWNE;\n"
  " float NWSW=NW+SW, SWSE=SW+SE, eH4=abs(eH1)*2.0+abs(eH2), eV4=abs(eV1)*2.0+abs(eV2), eH3=-2.0*W+NWSW, eV3=-2.0*S+SWSE;\n"
  " float eH=abs(eH3)+eH4, eV=abs(eV3)+eV4; bool horz=eH>=eV; float len=horz? rc.y : rc.x;\n"
  " float subA=subNSWE*2.0+NWSW+NESE; if(!horz){ N=W; S=E; } float subB=subA*(1.0/12.0)-M;\n"
  " float gN=N-M, gS=S-M, lNN=N+M, lSS=S+M; bool pairN=abs(gN)>=abs(gS); float grad=max(abs(gN),abs(gS)); if(pairN) len=-len;\n"
  " float subC=clamp(abs(subB)/range,0.0,1.0); vec2 posB=uv; vec2 off=horz? vec2(rc.x,0.0) : vec2(0.0,rc.y);\n"
  " if(!horz) posB.x+=len*0.5; else posB.y+=len*0.5; vec2 pN=posB-off, pP=posB+off; float subD=-2.0*subC+3.0, subE=subC*subC;\n"
  " if(!pairN) lNN=lSS; float gsc=grad*0.25, MM=M-lNN*0.5, subF=subD*subE; bool mLT0=MM<0.0;\n"
  " float eN=L(pN)-lNN*0.5, eP=L(pP)-lNN*0.5; bool dN=abs(eN)>=gsc, dP=abs(eP)>=gsc;\n"
  " const float st[11]=float[](1.5,2.0,2.0,2.0,2.0,4.0,8.0,8.0,8.0,8.0,8.0);\n"
  " for(int i=0;i<11 && !(dN&&dP);i++){ if(!dN){ pN-=off*st[i]; eN=L(pN)-lNN*0.5; dN=abs(eN)>=gsc; } if(!dP){ pP+=off*st[i]; eP=L(pP)-lNN*0.5; dP=abs(eP)>=gsc; } }\n"
  " float dstN= horz? uv.x-pN.x : uv.y-pN.y, dstP= horz? pP.x-uv.x : pP.y-uv.y;\n"
  " bool goodN=(eN<0.0)!=mLT0, goodP=(eP<0.0)!=mLT0; float spanLen=dstP+dstN; bool dirN=dstN<dstP; float dst=min(dstN,dstP);\n"
  " bool good=dirN? goodN : goodP; float subG=subF*subF, pxOff=dst*(-1.0/spanLen)+0.5, subH=subG*0.75;\n"
  " float o2=max(good? pxOff : 0.0,subH); vec2 p=uv; if(!horz) p.x+=o2*len; else p.y+=o2*len; return texture(t,p).rgb; }\n"
  "vec3 cas(vec3 c,vec3 n,vec3 s,vec3 e,vec3 w,float k){ vec3 mn=min(c,min(min(n,s),min(e,w))), mx=max(c,max(max(n,s),max(e,w)));\n"
  " vec3 a=sqrt(clamp(min(mn,1.0-mx)/max(mx,vec3(1e-4)),0.0,1.0)); vec3 wt=-a*mix(0.125,0.2,k); return clamp((c+(n+s+e+w)*wt)/(1.0+4.0*wt),0.0,1.0); }\n"
  "vec3 pick(vec2 p){ vec2 r=px.xy*vec2(textureSize(t,0)); if(max(r.x,r.y)<=1.05) return texture(t,p).rgb;\n"
  " vec3 c=vec3(0.0); for(int j=0;j<4;j++) for(int i=0;i<4;i++) c+=texture(t,p+((vec2(i,j)+0.5)*0.25-0.5)*px.xy).rgb; return c*(1.0/16.0); }\n"
  "void main(){ vec2 r=px.xy*vec2(textureSize(t,0)); bool one=max(r.x,r.y)<=1.05;\n"
  " vec3 c= one&&px.z>0.5? fxaa(uv) : pick(uv);\n"
  " if(px.w>0.0){ vec2 d=one? 1.0/vec2(textureSize(t,0)) : px.xy;\n"
  "  vec3 n=texture(t,uv+vec2(0,-d.y)).rgb, s=texture(t,uv+vec2(0,d.y)).rgb, e=texture(t,uv+vec2(d.x,0)).rgb, w=texture(t,uv+vec2(-d.x,0)).rgb;\n"
  "  c=cas(c,n,s,e,w,px.w); }\n"
  " o=vec4(c,1.0); }";
static GLuint g_last_present_tex; static int g_last_flip, g_last_w, g_last_h; static int g_last_is_rt;
static const char* BLIT_VS_ES="#version 310 es\nprecision highp float;\nout vec2 uv; uniform vec4 xf; void main(){ vec2 p=vec2(float(gl_VertexID&1)*2.0-1.0,float(gl_VertexID>>1)*2.0-1.0); uv=(p*0.5+0.5)*xf.zw+xf.xy; gl_Position=vec4(p,0.0,1.0); }";
static const char* BLIT_FS_ES="#version 310 es\nprecision highp float;\nin vec2 uv; out vec4 o; layout(binding=0) uniform highp sampler2D t; void main(){ o=vec4(texture(t,uv).rgb,1.0); }";
static const char* BLIT_VS="#version 430 core\nout vec2 uv; uniform vec4 xf; void main(){ vec2 p=vec2((gl_VertexID&1)*2.0-1.0,(gl_VertexID>>1)*2.0-1.0); uv=(p*0.5+0.5)*xf.zw+xf.xy; gl_Position=vec4(p,0.0,1.0); }";
static const char* BLIT_FS="#version 430 core\nin vec2 uv; out vec4 o; layout(binding=0) uniform sampler2D t; void main(){ o=vec4(texture(t,uv).rgb,1.0); }";
volatile i32 g_vi_crop[4]; volatile u32 g_vi_transform;   /* crop/transform from QueueBuffer */
#ifdef _WIN32
/* ============================================================ F7 recorder
   each presented frame gets converted to NV12 (BT.709) on the GPU at window size, same letterboxing as on
   screen. read back through a PBO two frames later so the GPU thread never waits. Windows does the H.264
   encode on its own thread */
extern volatile int g_rec_toggle; extern void backend_rec_state(int on);
extern int rec_open(const wchar_t* path, int w, int h, int fps); extern void rec_frame(const u8* nv12, i64 t100, i64 dur100); extern void rec_close(void);
#define REC_N 3
extern volatile int g_vt_on; extern int g_opt_rec_fps; extern u64 guest_ns(void); static u64 g_rec_n, g_rec_vt0, g_rec_lastts;
static int g_rec_on, g_rec_w, g_rec_h; static u64 g_rec_t0, g_rec_last; static u32 g_rec_k; static u64 g_rec_out, g_rec_base; static int g_rec_have;
static GLuint g_rec_fbo[2], g_rec_tex[2], g_rec_prog[2], g_rec_pbo[REC_N]; static GLsync g_rec_sync[REC_N]; static u64 g_rec_ts[REC_N];
static PFNGLMAPBUFFERRANGEPROC p_rmap; static PFNGLUNMAPBUFFERPROC p_runmap; static PFNGLFENCESYNCPROC p_rfs; static PFNGLCLIENTWAITSYNCPROC p_rcws; static PFNGLDELETESYNCPROC p_rds;
static const char* REC_VS="#version 430 core\nout vec2 uv; uniform vec4 xf; void main(){ vec2 p=vec2((gl_VertexID&1)*2.0-1.0,(gl_VertexID>>1)*2.0-1.0); vec2 q=p*0.5+0.5; q.y=1.0-q.y; uv=q*xf.zw+xf.xy; gl_Position=vec4(p,0.0,1.0); }";
static const char* REC_FS_Y="#version 430 core\nin vec2 uv; out vec4 o; layout(binding=0) uniform sampler2D t; void main(){ vec3 c=clamp(texture(t,uv).rgb,0.0,1.0); o=vec4(16.0/255.0+dot(c,vec3(0.2126,0.7152,0.0722))*(219.0/255.0)); }";
static const char* REC_FS_UV="#version 430 core\nin vec2 uv; out vec4 o; layout(binding=0) uniform sampler2D t; void main(){ vec3 c=clamp(texture(t,uv).rgb,0.0,1.0);\n"
  " o=vec4(128.0/255.0+dot(c,vec3(-0.1146,-0.3854,0.5))*(224.0/255.0), 128.0/255.0+dot(c,vec3(0.5,-0.4542,-0.0458))*(224.0/255.0), 0.0, 0.0); }";

static void rec_gl_free(void){
  for(int i=0;i<REC_N;i++){ if(g_rec_sync[i]){ p_rds(g_rec_sync[i]); g_rec_sync[i]=0; } }
  glDeleteBuffers(REC_N,g_rec_pbo); glDeleteFramebuffers(2,g_rec_fbo); glDeleteTextures(2,g_rec_tex); memset(g_rec_pbo,0,sizeof g_rec_pbo); memset(g_rec_fbo,0,sizeof g_rec_fbo); memset(g_rec_tex,0,sizeof g_rec_tex); }

static void rec_start(void){
  if(!p_rmap){ p_rmap=(PFNGLMAPBUFFERRANGEPROC)backend_gl_proc("glMapBufferRange"); p_runmap=(PFNGLUNMAPBUFFERPROC)backend_gl_proc("glUnmapBuffer");
    p_rfs=(PFNGLFENCESYNCPROC)backend_gl_proc("glFenceSync"); p_rcws=(PFNGLCLIENTWAITSYNCPROC)backend_gl_proc("glClientWaitSync"); p_rds=(PFNGLDELETESYNCPROC)backend_gl_proc("glDeleteSync"); }
  if(!p_rmap || !p_runmap || !p_rfs || !p_rcws || !p_rds){ WARN("recorder: the video driver lacks the required features\n"); return; }
  if(!g_rec_prog[0]){ GLuint v=compile(GL_VERTEX_SHADER,REC_VS,NULL,0);
    g_rec_prog[0]=link2(v,compile(GL_FRAGMENT_SHADER,REC_FS_Y,NULL,0),0); g_rec_prog[1]=link2(compile(GL_VERTEX_SHADER,REC_VS,NULL,0),compile(GL_FRAGMENT_SHADER,REC_FS_UV,NULL,0),0); }
  int ww,wh; backend_window_size(&ww,&wh); g_rec_w=ww&~3; g_rec_h=wh&~3;
  if(g_rec_w<64 || g_rec_h<64 || !g_rec_prog[0] || !g_rec_prog[1]) return;
  wchar_t dir[MAX_PATH], path[MAX_PATH]; GetModuleFileNameW(NULL,dir,MAX_PATH); wchar_t* sl=wcsrchr(dir,L'\\'); wchar_t name[128]=L"game";
  if(sl){ wcsncpy(name,sl+1,127); name[127]=0; wchar_t* dot=wcsrchr(name,L'.'); if(dot) *dot=0; sl[1]=0; } else dir[0]=0;
  wcscat(dir,L"recordings"); CreateDirectoryW(dir,NULL);
  SYSTEMTIME st; GetLocalTime(&st);
  _snwprintf(path,MAX_PATH,L"%ls\\%ls_%04d-%02d-%02d_%02d-%02d-%02d.mp4",dir,name,st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
  if(!rec_open(path,g_rec_w,g_rec_h,g_opt_rec_fps)) return;
  glGenTextures(2,g_rec_tex); glGenFramebuffers(2,g_rec_fbo);
  for(int i=0;i<2;i++){ glBindTexture(GL_TEXTURE_2D,g_rec_tex[i]); glTexStorage2D(GL_TEXTURE_2D,1,i? GL_RG8 : GL_R8,i? g_rec_w/2 : g_rec_w,i? g_rec_h/2 : g_rec_h);
    glBindFramebuffer(GL_FRAMEBUFFER,g_rec_fbo[i]); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,g_rec_tex[i],0); }
  glGenBuffers(REC_N,g_rec_pbo);
  for(int i=0;i<REC_N;i++){ glBindBuffer(GL_PIXEL_PACK_BUFFER,g_rec_pbo[i]); glBufferData(GL_PIXEL_PACK_BUFFER,(GLsizeiptr)g_rec_w*g_rec_h*3/2,NULL,GL_STREAM_READ); g_rec_sync[i]=0; }
  glBindBuffer(GL_PIXEL_PACK_BUFFER,0); glBindFramebuffer(GL_FRAMEBUFFER,0);
  g_rec_t0=plat_ns(); g_rec_last=0; g_rec_k=0; g_rec_on=1; g_rec_n=0; g_rec_out=0; g_rec_base=0; g_rec_have=0; backend_rec_state(1);
  { extern void vt_set(int); extern volatile int g_vt_on; extern volatile int g_rec_active; g_rec_active=1; vt_set(1); g_rec_vt0=guest_ns(); g_rec_lastts=0; if(g_vt_on) LOG("recorder: smooth mode (the game runs in slow motion if the PC can't keep up; the video comes out at a steady %d FPS)\n",g_opt_rec_fps); } }

/* PBO from REC_N-1 frames ago: hand it over if it's ready, otherwise skip (never wait) */
static void rec_drain(int all){
  for(u32 n=0;n<REC_N;n++){ u32 i=(g_rec_k+n)%REC_N; if(!g_rec_sync[i]) continue;
    GLenum r=p_rcws(g_rec_sync[i],0,all? 2000000000ull : 0); if(r!=GL_ALREADY_SIGNALED && r!=GL_CONDITION_SATISFIED){ if(all){ p_rds(g_rec_sync[i]); g_rec_sync[i]=0; continue; } else return; }
    p_rds(g_rec_sync[i]); g_rec_sync[i]=0;
    glBindBuffer(GL_PIXEL_PACK_BUFFER,g_rec_pbo[i]); size_t n12=(size_t)g_rec_w*g_rec_h*3/2;
    const u8* p=(const u8*)p_rmap(GL_PIXEL_PACK_BUFFER,0,(GLsizeiptr)n12,GL_MAP_READ_BIT);
    if(p){ /* constant-fps output: output frame k sits at k/fps. each captured frame covers its guest time up to the
         next one, so it gets repeated or dropped. weird clock jumps are clamped */
      u64 per=1000000000ull/(u64)(g_opt_rec_fps>0? g_opt_rec_fps : 60);
      u64 ts=g_rec_ts[i];
      if(g_rec_out==0 && !g_rec_have){ g_rec_base=ts; }
      u64 rel= ts>g_rec_base? ts-g_rec_base : 0;
      u64 want=rel/per+1;                                   /* output frames that should exist up to this one */
      if(g_rec_have && want>g_rec_out+30){ g_rec_base+= (want-(g_rec_out+1))*per; want=g_rec_out+1; }   /* > 0.5s jump: emit one frame */
      if(!g_rec_have) want= want>g_rec_out? want : g_rec_out+1;
      u64 fps=(u64)(g_opt_rec_fps>0? g_opt_rec_fps : 60); while(g_rec_out<want){ rec_frame(p,(i64)(g_rec_out*10000000ull/fps),(i64)((g_rec_out+1)*10000000ull/fps-g_rec_out*10000000ull/fps)); g_rec_out++; }
      g_rec_have=1; p_runmap(GL_PIXEL_PACK_BUFFER); }
    glBindBuffer(GL_PIXEL_PACK_BUFFER,0); } }

static void rec_stop(void){ if(!g_rec_on) return; rec_drain(1); rec_close(); rec_gl_free(); g_rec_on=0; backend_rec_state(0); { extern void vt_set(int); extern volatile int g_rec_active; vt_set(0); g_rec_active=0; } }
volatile int g_rec_exit;   /* at exit: 1 = finalize, 2 = done */
void gpu_gl_rec_finish(void){ if(!g_rec_on) return; g_rec_exit=1; u64 t0=plat_ns(); while(g_rec_exit!=2 && plat_ns()-t0<4000000000ull) plat_sleep_ns(10000000); }

static void rec_capture(GLuint tex, const float* xf, int tw, int th){
  if(g_rec_toggle){ g_rec_toggle=0; if(g_rec_on) rec_stop(); else rec_start(); }
  if(!g_rec_on) return;
  u64 now=plat_ns(); if(!g_vt_on && g_rec_last && now-g_rec_last<15500000ull) return;   /* max 60/s (smooth mode: every real frame) */
  g_rec_last=now;
  rec_drain(0);
  u32 i=g_rec_k%REC_N; if(g_rec_sync[i]){ if(!g_vt_on) return; rec_drain(1); }   /* smooth mode never drops */
  int dw=g_rec_w, dh=g_rec_w*th/(tw?tw:1); if(dh>g_rec_h){ dh=g_rec_h; dw=g_rec_h*tw/(th?th:1); } dw&=~1; dh&=~1;
  int ox=((g_rec_w-dw)/2)&~1, oy=((g_rec_h-dh)/2)&~1;
  glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_FRAMEBUFFER_SRGB);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex); glBindSampler(0,0);
  for(int k=0;k<2;k++){ int sx= k? 2 : 1;
    glBindFramebuffer(GL_FRAMEBUFFER,g_rec_fbo[k]); { GLenum b=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&b); }
    glViewport(0,0,g_rec_w/sx,g_rec_h/sx);
    if(k) glClearColor(128.0f/255,128.0f/255,0,0); else glClearColor(16.0f/255,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glViewport(ox/sx,oy/sx,dw/sx,dh/sx);
    glUseProgram(g_rec_prog[k]); glUniform4fv(glGetUniformLocation(g_rec_prog[k],"xf"),1,xf);
    glBindVertexArray(g_scratch_vao); glDrawArrays(GL_TRIANGLE_STRIP,0,4); }
  glBindBuffer(GL_PIXEL_PACK_BUFFER,g_rec_pbo[i]); glPixelStorei(GL_PACK_ALIGNMENT,1);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,g_rec_fbo[0]); glReadPixels(0,0,g_rec_w,g_rec_h,GL_RED,GL_UNSIGNED_BYTE,(void*)0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,g_rec_fbo[1]); glReadPixels(0,0,g_rec_w/2,g_rec_h/2,GL_RG,GL_UNSIGNED_BYTE,(void*)(size_t)((size_t)g_rec_w*g_rec_h));
  glBindBuffer(GL_PIXEL_PACK_BUFFER,0); glPixelStorei(GL_PACK_ALIGNMENT,4);
  g_rec_sync[i]=p_rfs(GL_SYNC_GPU_COMMANDS_COMPLETE,0); { u64 ts= g_vt_on? guest_ns()-g_rec_vt0 : now-g_rec_t0; if(g_rec_n && ts<g_rec_lastts) ts=g_rec_lastts; g_rec_lastts=ts; g_rec_ts[i]=ts; } g_rec_n++; g_rec_k++;
  glBindFramebuffer(GL_FRAMEBUFFER,0); glUseProgram(g_blit_prog); }
#else
static void rec_capture(GLuint tex, const float* xf, int tw, int th){ (void)tex; (void)xf; (void)tw; (void)th; }

void gpu_gl_rec_finish(void){}
#endif
/* ============================================================ F8 full-res screenshot (png)
   GPU copies the frame into a PBO, we map it a frame or two later and a worker thread writes the png */
volatile int g_shot8_req;
static GLuint g_s8_fbo, g_s8_tex, g_s8_pbo; static GLsync g_s8_sync; static int g_s8_w, g_s8_h, g_s8_tw, g_s8_th;
static PFNGLMAPBUFFERRANGEPROC p_s8map; static PFNGLUNMAPBUFFERPROC p_s8unmap; static PFNGLFENCESYNCPROC p_s8fs; static PFNGLCLIENTWAITSYNCPROC p_s8cws; static PFNGLDELETESYNCPROC p_s8ds;
typedef struct { u8* px; int w, h; char path[320]; } S8Job;
static u32 s8_crc_tab[256];

static u32 s8_crc(u32 c, const u8* d, size_t n){ if(!s8_crc_tab[1]) for(u32 i=0;i<256;i++){ u32 k=i; for(int j=0;j<8;j++) k= (k&1)? 0xEDB88320u^(k>>1) : k>>1; s8_crc_tab[i]=k; }
  c=~c; for(size_t i=0;i<n;i++) c=s8_crc_tab[(c^d[i])&255]^(c>>8); return ~c; }

static void s8_be32(u8* p, u32 v){ p[0]=(u8)(v>>24); p[1]=(u8)(v>>16); p[2]=(u8)(v>>8); p[3]=(u8)v; }

static void s8_chunk(FILE* f, const char* t, const u8* d, u32 n){ u8 h[8]; s8_be32(h,n); memcpy(h+4,t,4); fwrite(h,1,8,f); if(n) fwrite(d,1,n,f);
  u32 c=s8_crc(0,(const u8*)t,4); c=s8_crc(c,d,n); u8 cc[4]; s8_be32(cc,c); fwrite(cc,1,4,f); }

/* RGB png, Sub filter per row, stored (uncompressed) deflate. fast, ~3 bytes/px */
static void s8_thread(void* arg){
  S8Job* j=(S8Job*)arg; int w=j->w, h=j->h; size_t row=(size_t)w*3+1, raw=row*h;
  u8* r=malloc(raw); if(!r){ free(j->px); free(j); return; }
  for(int y=0;y<h;y++){ const u8* s=j->px+(size_t)(h-1-y)*w*4; u8* o=r+(size_t)y*row; o[0]=1; u8 pr=0,pg=0,pb=0;
    for(int x=0;x<w;x++){ u8 R=s[x*4],G=s[x*4+1],B=s[x*4+2]; o[1+x*3]=(u8)(R-pr); o[2+x*3]=(u8)(G-pg); o[3+x*3]=(u8)(B-pb); pr=R; pg=G; pb=B; } }
  free(j->px);
  size_t nb=(raw+65534)/65535; size_t zn=2+raw+nb*5+4; u8* z=malloc(zn);
  if(z){ size_t o=0; z[o++]=0x78; z[o++]=0x01; u32 a=1,b=0;
    for(size_t i=0;i<raw;i+=65535){ size_t n=raw-i; if(n>65535) n=65535; z[o++]= (i+n>=raw)? 1 : 0; z[o++]=(u8)n; z[o++]=(u8)(n>>8); z[o++]=(u8)~n; z[o++]=(u8)(~n>>8);
      memcpy(z+o,r+i,n); o+=n; for(size_t k=0;k<n;k++){ a=(a+r[i+k])%65521u; b=(b+a)%65521u; } }
    s8_be32(z+o,(b<<16)|a); o+=4;
    FILE* f=fopen(j->path,"wb");
    if(f){ static const u8 sig[8]={0x89,'P','N','G',13,10,26,10}; fwrite(sig,1,8,f);
      u8 ih[13]; s8_be32(ih,(u32)w); s8_be32(ih+4,(u32)h); ih[8]=8; ih[9]=2; ih[10]=0; ih[11]=0; ih[12]=0; s8_chunk(f,"IHDR",ih,13);
      s8_chunk(f,"IDAT",z,(u32)o); s8_chunk(f,"IEND",NULL,0); fclose(f); LOG("F8 screenshot: %s (%dx%d)\n",j->path,w,h); }
    else WARN("F8 screenshot: could not write %s\n",j->path);
    free(z); }
  free(r); free(j); }

static void shot8_poll(void){
  if(!g_s8_sync) return;
  GLenum st=p_s8cws(g_s8_sync,0,0); if(st!=GL_ALREADY_SIGNALED && st!=GL_CONDITION_SATISFIED) return;
  p_s8ds(g_s8_sync); g_s8_sync=0;
  size_t n=(size_t)g_s8_w*g_s8_h*4; glBindBuffer(GL_PIXEL_PACK_BUFFER,g_s8_pbo);
  const u8* p=(const u8*)p_s8map(GL_PIXEL_PACK_BUFFER,0,(GLsizeiptr)n,GL_MAP_READ_BIT);
  S8Job* j=calloc(1,sizeof *j);
  if(p && j && (j->px=malloc(n))){ memcpy(j->px,p,n); j->w=g_s8_w; j->h=g_s8_h;
    time_t tt=time(NULL); struct tm* tm=localtime(&tt); static int seq;
    plat_mkdir("captures"); snprintf(j->path,sizeof j->path,"captures/F8_%04d%02d%02d_%02d%02d%02d_%d.png",tm->tm_year+1900,tm->tm_mon+1,tm->tm_mday,tm->tm_hour,tm->tm_min,tm->tm_sec,++seq);
    if(plat_thread(s8_thread,j,4u<<20)){ s8_thread(j); } j=NULL; }
  if(j){ free(j->px); free(j); }
  if(p) p_s8unmap(GL_PIXEL_PACK_BUFFER);
  glBindBuffer(GL_PIXEL_PACK_BUFFER,0); }

static void shot8_capture(GLuint tex, const float* xf, int tw, int th){
  shot8_poll();
  { static int tf; if(!tf){ tf= getenv("SWR_SHOT8_TEST")? 1 : -1; } if(tf>0 && ++tf==40) g_shot8_req=1; }   /* automated test hook */
  if(!g_shot8_req || g_s8_sync) return;
  g_shot8_req=0;
  if(!p_s8map){ p_s8map=(PFNGLMAPBUFFERRANGEPROC)backend_gl_proc("glMapBufferRange"); p_s8unmap=(PFNGLUNMAPBUFFERPROC)backend_gl_proc("glUnmapBuffer");
    p_s8fs=(PFNGLFENCESYNCPROC)backend_gl_proc("glFenceSync"); p_s8cws=(PFNGLCLIENTWAITSYNCPROC)backend_gl_proc("glClientWaitSync"); p_s8ds=(PFNGLDELETESYNCPROC)backend_gl_proc("glDeleteSync"); }
  if(!p_s8map || !p_s8unmap || !p_s8fs || !p_s8cws || !p_s8ds){ WARN("F8 screenshot: the video driver lacks the required features\n"); return; }
  /* actual size of the shown content (visible part of the texture) */
  int w=(int)(tw*fabsf(xf[2])+0.5f), h=(int)(th*fabsf(xf[3])+0.5f); if(w<16||h<16||w>8192||h>8192) return;
  if(w!=g_s8_w || h!=g_s8_h){
    if(g_s8_tex){ glDeleteTextures(1,&g_s8_tex); glDeleteFramebuffers(1,&g_s8_fbo); glDeleteBuffers(1,&g_s8_pbo); }
    glGenTextures(1,&g_s8_tex); glBindTexture(GL_TEXTURE_2D,g_s8_tex); glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,w,h);
    glGenFramebuffers(1,&g_s8_fbo); glBindFramebuffer(GL_FRAMEBUFFER,g_s8_fbo); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,g_s8_tex,0);
    glGenBuffers(1,&g_s8_pbo); glBindBuffer(GL_PIXEL_PACK_BUFFER,g_s8_pbo); glBufferData(GL_PIXEL_PACK_BUFFER,(GLsizeiptr)w*h*4,NULL,GL_STREAM_READ); glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
    g_s8_w=w; g_s8_h=h; }
  (void)g_s8_tw; (void)g_s8_th;
  glBindFramebuffer(GL_FRAMEBUFFER,g_s8_fbo); { GLenum b=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&b); }
  glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_FRAMEBUFFER_SRGB); glColorMaski(0,1,1,1,1);
  glViewport(0,0,w,h); glUseProgram(g_blit_prog); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex); glBindSampler(0,0);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  { static GLint xl=-2; static GLuint xp; if(xl==-2 || xp!=g_blit_prog){ xl=glGetUniformLocation(g_blit_prog,"xf"); xp=g_blit_prog; } glUniform4fv(xl,1,xf); }
  glBindVertexArray(g_scratch_vao); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  glBindBuffer(GL_PIXEL_PACK_BUFFER,g_s8_pbo); glPixelStorei(GL_PACK_ALIGNMENT,4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,g_s8_fbo); glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,(void*)0);
  glBindBuffer(GL_PIXEL_PACK_BUFFER,0); glBindFramebuffer(GL_FRAMEBUFFER,0);
  g_s8_sync=p_s8fs(GL_SYNC_GPU_COMMANDS_COMPLETE,0); glFlush(); }

/* blit a texture to the window, 16:9 letterboxed. xf = uv transform (crop/flip) */
static void screen_quad(GLuint tex, const float* xf, int tw, int th){
  int ww,wh; backend_window_size(&ww,&wh);
  glBindFramebuffer(GL_FRAMEBUFFER,0); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glBindTexture(GL_TEXTURE_2D,tex); glTexParameteri(GL_TEXTURE_2D,0x8A48,0x8A4A);   /* TEXTURE_SRGB_DECODE = SKIP_DECODE */
  glColorMaski(0,1,1,1,1);
  glViewport(0,0,ww,wh); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
  int dw=ww, dh=ww*th/(tw?tw:1); if(dh>wh){ dh=wh; dw=wh*tw/(th?th:1); }
  glViewport((ww-dw)/2,(wh-dh)/2,dw,dh);
  glUseProgram(g_down_prog? g_down_prog : g_blit_prog); g_pipe_on=0; glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex); glBindSampler(0,0);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  { static const GLint idm[4]={GL_RED,GL_GREEN,GL_BLUE,GL_ALPHA}; glTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_RGBA,idm); }
  if(g_down_prog){ glUniform4fv(g_down_xf,1,xf); float pxv[4]={ fabsf_(xf[2])/(float)(dw>0? dw : 1), fabsf_(xf[3])/(float)(dh>0? dh : 1), g_opt_aa? 1.0f : 0.0f, g_opt_sharp>0? (float)g_opt_sharp/100.0f : 0.0f }; glUniform4fv(g_down_px,1,pxv); }
  else { static GLint xl=-2; static GLuint xp; if(xl==-2 || xp!=g_blit_prog){ xl=glGetUniformLocation(g_blit_prog,"xf"); xp=g_blit_prog; } glUniform4fv(xl,1,xf); }
  glBindVertexArray(g_scratch_vao); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  { /* test only: SWR_TEST_BACKBUFFER=frame,frame,...:prefix writes the window's back buffer (what goes on screen,
       after the window pass) as prefix<frame>.ppm. Desktop grabs need the game in front; this does not */
    static int cfg; static const char* list; static char pre[512]; extern int g_frames; static int last=-1;
    if(!cfg){ cfg=1; const char* e=getenv("SWR_TEST_BACKBUFFER"); const char* c=e? strchr(e,':') : NULL; if(c){ list=e; snprintf(pre,sizeof pre,"%s",c+1); } }
    if(list && g_frames!=last){ last=g_frames; const char* p=list;
      while(*p && *p!=':'){ int f=atoi(p); if(f==g_frames){ u8* px=malloc((size_t)ww*wh*3);
          if(px){ glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT,1); glReadPixels(0,0,ww,wh,GL_RGB,GL_UNSIGNED_BYTE,px);
            char fn[600]; snprintf(fn,sizeof fn,"%s%d.ppm",pre,f); FILE* o=fopen(fn,"wb");
            if(o){ fprintf(o,"P6\n%d %d\n255\n",ww,wh); for(int y=wh-1;y>=0;y--) fwrite(px+(size_t)y*ww*3,1,(size_t)ww*3,o); fclose(o); } free(px); } }
        while(*p && *p!=',' && *p!=':') p++; if(*p==',') p++; } } }
  rec_capture(tex,xf,tw,th); shot8_capture(tex,xf,tw,th); glUseProgram(g_blit_prog); glBindFramebuffer(GL_FRAMEBUFFER,0); glBindTexture(GL_TEXTURE_2D,tex);
  glTexParameteri(GL_TEXTURE_2D,0x8A48,0x8A49); }   /* restore DECODE for when the game samples it */
static void screen_swap(void){ TM(TM_SWAP,backend_gl_swap()); state_dirty(); g_pipe_on=0; mb_reset(); }   /* Android overlay may touch state/program */

/* ============================================================ LSFG frame gen (user's own Lossless.dll) */
extern int lsfg_init(void); extern int lsfg_ready(void); extern int lsfg_context(u32,u32); extern GLuint lsfg_in_tex(void); extern void lsfg_reset(void); extern int lsfg_process(int, GLuint*);
extern int g_lsfg_mult, g_lsfg_height; extern float g_lsfg_target;
static GLuint g_lf_fbo; static u64 g_lf_last, g_lf_int=33333333ull; static int g_lf_on, g_lf_W, g_lf_H;
typedef struct { GLuint tex; u64 t; } LfQ; static LfQ g_lfq[6]; static int g_lfn; static u64 st_lsfg_gen, st_lsfg_ns;
static const float XF_ID[4]={0,0,1,1};

static void lf_flush(void){ for(int i=0;i<g_lfn;i++){ screen_quad(g_lfq[i].tex,XF_ID,g_lf_W,g_lf_H); screen_swap(); } g_lfn=0; }

static void lsfg_pump(void){ if(!g_lfn) return; u64 now=plat_ns(); int k=0;
  while(k<g_lfn && now>=g_lfq[k].t){ screen_quad(g_lfq[k].tex,XF_ID,g_lf_W,g_lf_H); screen_swap(); k++; }
  if(k){ memmove(g_lfq,g_lfq+k,(size_t)(g_lfn-k)*sizeof *g_lfq); g_lfn-=k; } }

/* real frame: copy into the LSFG input, generate the in-between frames and present them spread over the
   interval, real one last. 0 = not active (already at target, or no dll) */
static int g_blit_rt;   /* shown image is an RT, i.e. scaled by the res scale */
static int lsfg_frame(GLuint tex, const float* xf, int tw, int th){
  if(!lsfg_ready() || tw<=0 || th<=0) return 0;
#ifdef _WIN32
  { extern volatile int g_vt_on; if(g_vt_on){ lf_flush(); g_lf_on=0; return 0; } }   /* smooth recording: only real frames */
#endif
  u64 now=plat_ns();
  if(g_lf_last){ u64 d=now-g_lf_last; if(d>250000000ull) d=250000000ull; g_lf_int=(g_lf_int*7+d)/8; }
  g_lf_last=now;
  double base=1e9/(double)g_lf_int;
  int m=(int)(g_lsfg_target/base+0.5); if(m>g_lsfg_mult) m=g_lsfg_mult;
  if(base>g_lsfg_target*(g_lf_on? 0.92 : 0.85)) m=1;     /* already at target */
  { static int ls=-1; int st= m>=2? m : 0; if(st!=ls){ ls=st;
#ifdef _WIN32
      extern void backend_lsfg_state(int,int); backend_lsfg_state(st!=0,st);
#endif
      LOG("lsfg: %s\n",st? "ON" : "standby (the game already reaches the target)"); } }
  if(m<2){ lf_flush(); g_lf_on=0; return 0; }
  /* at a low res scale the real image is smaller than the window; generating bigger just burns GPU on
     stretched pixels (x0.25 at 720p cost ~29ms on an iGPU). min 360 */
  int content= g_blit_rt? (int)(th*g_scale+0.5f) : th; if(content<360) content=360; if(content>th) content=th;
  u32 H=(u32)(content<g_lsfg_height? content : g_lsfg_height), W=(u32)((u64)H*(u32)tw/(u32)th)&~1u; if(W<16 || H<16) return 0;
  if(!lsfg_context(W,H)){ return 0; }
  if(!g_lf_on){ g_lf_on=1; lsfg_reset(); }
  lf_flush();
  g_lf_W=(int)W; g_lf_H=(int)H;
  GLuint in=lsfg_in_tex();
  if(!g_lf_fbo) glGenFramebuffers(1,&g_lf_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER,g_lf_fbo); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,in,0); { GLenum b=GL_COLOR_ATTACHMENT0; glDrawBuffers(1,&b); }
  glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST); glDisable(GL_FRAMEBUFFER_SRGB); glColorMaski(0,1,1,1,1);
  glViewport(0,0,(GLsizei)W,(GLsizei)H);
  glUseProgram(g_blit_prog); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex); glBindSampler(0,0);
  glTexParameteri(GL_TEXTURE_2D,0x8A48,0x8A4A); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  { static const GLint idm[4]={GL_RED,GL_GREEN,GL_BLUE,GL_ALPHA}; glTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_RGBA,idm); }
  { static GLint xl=-2; static GLuint xp; if(xl==-2 || xp!=g_blit_prog){ xl=glGetUniformLocation(g_blit_prog,"xf"); xp=g_blit_prog; } glUniform4fv(xl,1,xf); }
  glBindVertexArray(g_scratch_vao); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  glTexParameteri(GL_TEXTURE_2D,0x8A48,0x8A49);
  GLuint outs[4]; u64 t0=plat_ns(); int n=lsfg_process(m-1,outs); st_lsfg_ns+=plat_ns()-t0;
  { static int nl; if(nl<4 && n>0){ nl++; glFinish(); LOG("lsfg: frame generated in %.1f ms (GPU included, x%d, base %.1f FPS)\n",(plat_ns()-t0)/1e6,m,base); } }
  { static int shot=-1, cnt; if(shot<0){ const char* e=getenv("SWR_LSFG_SHOT"); shot= e? atoi(e) : 0; }
    if(shot>0 && n>0 && ++cnt==shot){ extern void lsfg_debug_tex(GLuint*,GLuint*,u32*,u32*); GLuint a,b; u32 w,h; lsfg_debug_tex(&a,&b,&w,&h);
      GLuint tx[3]={a,outs[0],b}; u8* px=malloc((size_t)w*h*4*3); u8* row=malloc((size_t)w*3*3);
      for(int k=0;k<3;k++){ glBindTexture(GL_TEXTURE_2D,tx[k]); glPixelStorei(GL_PACK_ALIGNMENT,1); glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,px+(size_t)k*w*h*4); }
      FILE* f=fopen("lsfg_shot.ppm","wb"); if(f){ fprintf(f,"P6\n%u %u\n255\n",w*3,h);
        for(int y=(int)h-1;y>=0;y--){ for(int k=0;k<3;k++) for(u32 x=0;x<w;x++){ u8* q=px+(size_t)k*w*h*4+((size_t)y*w+x)*4; memcpy(row+((size_t)k*w+x)*3,q,3); } fwrite(row,1,(size_t)w*9,f); }
        fclose(f); LOG("lsfg: previous | generated | current saved to lsfg_shot.ppm\n"); }
      free(px); free(row); mb_reset(); } }
  /* the LSFG dispatches clobbered programs, textures, samplers and UBO 0 */
  g_pipe_on=0; state_dirty(); mb_reset(); memset(g_ub_bound,0,sizeof g_ub_bound); glBindFramebuffer(GL_FRAMEBUFFER,g_fbo);
  g_last_present_tex=in; g_last_flip=0; g_last_w=(int)W; g_last_h=(int)H; g_last_is_rt=0;
  if(n<=0){ screen_quad(in,XF_ID,(int)W,(int)H); screen_swap(); return 1; }
  u64 step=g_lf_int/(u64)(n+1);
  screen_quad(outs[0],XF_ID,(int)W,(int)H); screen_swap(); st_lsfg_gen+=(u64)n;
  for(int k=1;k<n;k++){ g_lfq[g_lfn].tex=outs[k]; g_lfq[g_lfn].t=now+step*(u64)k; g_lfn++; }
  g_lfq[g_lfn].tex=in; g_lfq[g_lfn].t=now+step*(u64)n; g_lfn++;
  return 1; }

/* Diagnostic only: sample the actual final image before presentation and
   preserve render targets at abrupt brightness drops, without timing F5. */
static void dump_rts(void);
/* Only active under RenderDoc injection and the diagnostic launcher.
   Queue a short run of full frames after the first measured brightness drop. */
static void renderdoc_capture_drop(void){
#ifdef _WIN32
  static int queued;
  if(queued || !getenv("SWR_CAPTURE_RENDERDOC"))return;
  HMODULE module=GetModuleHandleA("renderdoc.dll");
  pRENDERDOC_GetAPI get_api=module?(pRENDERDOC_GetAPI)GetProcAddress(module,"RENDERDOC_GetAPI"):NULL;
  RENDERDOC_API_1_6_0* api=NULL;
  if(!get_api || !get_api(eRENDERDOC_API_Version_1_6_0,(void**)&api)){
    queued=1;LOG("RenderDoc: capture API unavailable\n");return;
  }
  queued=1;
  api->TriggerMultiFrameCapture(4);
  LOG("RenderDoc: queued four full frames after brightness drop at frame %u\n",g_frame);
#endif
}
static void lighting_auto_capture(GLuint tex,int tw,int th){
  static int enabled=-1; if(enabled<0) enabled=getenv("SWR_CAPTURE_LIGHTING")!=NULL;
  if(!enabled || g_frame<1200 || tw<16 || th<16) return;
  static GLuint sf,df,small; static u8 previous[16*9*4]; static float last;
  static float raw_previous[5][16*9*4];
  static int have,captures; if(captures>=3)return;
  if(!small){ glGenTextures(1,&small); glBindTexture(GL_TEXTURE_2D,small); glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,16,9); glGenFramebuffers(1,&sf);glGenFramebuffers(1,&df); }
  glBindFramebuffer(GL_READ_FRAMEBUFFER,sf);glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,df);glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,small,0);{GLenum b=GL_COLOR_ATTACHMENT0;glDrawBuffers(1,&b);}
  glDisable(GL_SCISSOR_TEST);glDisable(GL_FRAMEBUFFER_SRGB);glColorMaski(0,1,1,1,1);
  glBlitFramebuffer(0,0,SC(tw),SC(th),0,0,16,9,GL_COLOR_BUFFER_BIT,GL_LINEAR);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,df);glReadBuffer(GL_COLOR_ATTACHMENT0);u8 pixels[16*9*4];glReadPixels(0,0,16,9,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
  float mean=0;for(int y=1;y<8;y++)for(int x=1;x<15;x++){u8*p=pixels+(y*16+x)*4;mean+=(p[0]+p[1]+p[2])/3.0f;}mean/=98;
  /* Preserve linear floating-point inputs, not 8-bit thumbnails: HDR values
     and non-finite values disappear when converted to screenshot bytes. */
  static GLuint fsmall; if(!fsmall){glGenTextures(1,&fsmall);glBindTexture(GL_TEXTURE_2D,fsmall);glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA32F,16,9);}
  static const u64 addresses[5]={0x5c0550000ull,0x5bf890000ull,0x5bfef0000ull,0x5c1210000ull,0x5c1ed0000ull};
  float raw[5][16*9*4];memset(raw,0,sizeof raw);
  for(int k=0;k<5;k++){
    Tex*q=tex_find_rt(addresses[k]);if(!q)continue;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,sf);glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,q->id,0);glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,df);glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,fsmall,0);{GLenum b=GL_COLOR_ATTACHMENT0;glDrawBuffers(1,&b);}
    glBlitFramebuffer(0,0,SC(q->w),SC(q->h),0,0,16,9,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,df);glReadBuffer(GL_COLOR_ATTACHMENT0);glReadPixels(0,0,16,9,GL_RGBA,GL_FLOAT,raw[k]);
  }
  glBindFramebuffer(GL_FRAMEBUFFER,g_fbo);state_dirty();mb_reset();
  if(have && last>10 && mean<last*0.78f){
    renderdoc_capture_drop();
    captures++;plat_mkdir("captures");char path[256];snprintf(path,sizeof path,"captures/lighting-drop-%d.txt",captures);FILE*f=fopen(path,"w");
    if(f){fprintf(f,"frame=%u previous_mean=%.3f current_mean=%.3f\n",g_frame,last,mean);fclose(f);}
    for(int side=0;side<2;side++){snprintf(path,sizeof path,"captures/lighting-drop-%d-%s.ppm",captures,side?"dark":"before");f=fopen(path,"wb");if(f){fprintf(f,"P6\n16 9\n255\n");u8*p=side?pixels:previous;for(int y=8;y>=0;y--)for(int x=0;x<16;x++)fwrite(p+(y*16+x)*4,1,3,f);fclose(f);}}
    for(int side=0;side<2;side++){
      snprintf(path,sizeof path,"captures/lighting-drop-%d-%s.f32",captures,side?"dark":"before");f=fopen(path,"wb");if(f){fwrite(side?raw:raw_previous,1,sizeof raw,f);fclose(f);}
      snprintf(path,sizeof path,"captures/lighting-drop-%d-%s-constants.bin",captures,side?"dark":"before");f=fopen(path,"wb");if(f){u32 n=side?lp_count:lp_prev_count;fwrite(&n,4,1,f);fwrite(side?lp_current:lp_previous,sizeof(LightingPayload),n,f);fclose(f);}
    }
    LOG("lighting capture: drop %d, frame %u, brightness %.2f -> %.2f; dumping current render targets\n",captures,g_frame,last,mean);dump_rts();
  }
  memcpy(raw_previous,raw,sizeof raw);lp_prev_count=lp_count;memcpy(lp_previous,lp_current,lp_count*sizeof(LightingPayload));
  memcpy(previous,pixels,sizeof pixels);last=mean;have=1;
}
static void blit_to_screen(GLuint tex, int flip, int tw, int th){ state_dirty();
  lighting_auto_capture(tex,tw,th);
  /* game-requested crop (e.g. 1280x720 inside a 1920x1080 buffer) */
  float u0=0, v0=0, u1=1, v1=1; { i32 l=g_vi_crop[0], t=g_vi_crop[1], r=g_vi_crop[2], b=g_vi_crop[3];
    if(r>l && b>t && tw>0 && th>0 && r<=tw*4 && b<=th*4){ u0=(float)l/tw; u1=(float)r/tw; v0=(float)t/th; v1=(float)b/th; tw=r-l; th=b-t; } }
  if(g_vi_transform&2){ float x=v0; v0=v1; v1=x; }   /* FLIP_V */
  if(g_vi_transform&1){ float x=u0; u0=u1; u1=x; }   /* FLIP_H */
  float xf[4]; if(flip){ xf[0]=u0; xf[1]=v1; xf[2]=u1-u0; xf[3]=v0-v1; } else { xf[0]=u0; xf[1]=1-v1; xf[2]=u1-u0; xf[3]=v1-v0; }
  if(lsfg_frame(tex,xf,tw,th)) return;
  screen_quad(tex,xf,tw,th); screen_swap();
  g_last_present_tex=tex; g_last_flip=flip; g_last_w=tw; g_last_h=th;
}

static void present_cpu(u32* px, int w, int h){
  if(!g_cpu_tex || g_cpu_w!=w || g_cpu_h!=h){ if(g_cpu_tex){ glDeleteTextures(1,&g_cpu_tex); g_fb_sig_ok=0; } glGenTextures(1,&g_cpu_tex); glBindTexture(GL_TEXTURE_2D,g_cpu_tex); glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,w,h); g_cpu_w=w; g_cpu_h=h; }
  glBindTexture(GL_TEXTURE_2D,g_cpu_tex); glPixelStorei(GL_UNPACK_ROW_LENGTH,0); glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px);
  blit_to_screen(g_cpu_tex,1,w,h); g_last_is_rt=0; }
/* eviction: free memory-backed textures unused for ~40s and orphaned RTs, otherwise switching worlds slowly
   eats the iGPU's shared RAM. walks part of the table each frame; textures get recreated from memory if needed */
static u64 st_tex_evict, st_vb_evict;

static void tex_free(Tex* t){
  glDeleteTextures(1,&t->id); if(t->view) glDeleteTextures(1,&t->view); if(t->pre) glDeleteTextures(1,&t->pre);
  if(t->crop){ glDeleteTextures(1,&t->crop->id); if(t->crop->view) glDeleteTextures(1,&t->crop->view); free(t->crop); }
  if(t->vol){ glDeleteTextures(1,&t->vol->id); if(t->vol->view) glDeleteTextures(1,&t->vol->view); free(t->vol); }
  if(t->lay){ glDeleteTextures(1,&t->lay->id); if(t->lay->view) glDeleteTextures(1,&t->lay->view); free(t->lay); }
  free(t); }

static void gpu_evict_step(void){
  static u32 cur, vcur; if(g_tr_on) return; int any=0;
  for(int n=0;n<TEXH/64;n++,cur=(cur+1)&(TEXH-1)){
    for(Tex** pp=&texh[cur];*pp;){ Tex* t=*pp; u32 age=g_frame-t->last_used; int busy=(t==g_zt); for(int i=0;i<8;i++) if(g_rt[i]==t) busy=1;
      int old= t->is_rt? (t->gva==~0ull && age>300) : (age>2400);
      if(old && !busy){ *pp=t->next; tex_free(t); st_tex_evict++; any=1; } else pp=&t->next; } }
  if(any){ mb_reset(); g_fb_sig_ok=0; memset(g_tdc,0,sizeof g_tdc); g_tmap_gen++; }
  int vany=0;
  for(int n=0;n<2048/64;n++,vcur=(vcur+1)&2047){
    for(VbC** pp=&vbc[vcur];*pp;){ VbC* e=*pp;
      if(g_submit_epoch-e->epoch>4800){ *pp=e->next; glDeleteBuffers(1,&e->id); free(e); st_vb_evict++; vany=1; } else pp=&e->next; } }
  if(vany) g_va_inval=1; }

static int present_addr(u64 cpu_addr, int w, int h){
  /* find the RT whose gva maps this memory */
  Tex* best=NULL;
  for(int k=0;k<TEXH;k++) for(Tex* t=texh[k];t;t=t->next) if(t->is_rt && gmmu_translate(t->gva)==cpu_addr){ if(!best || (t->w==(u32)w && t->h==(u32)h) || t->last_used>best->last_used) best=t; }
  trace_note("PRESENT %llx %dx%d -> %s %llx\n",(unsigned long long)cpu_addr,w,h,best?"rt":"(none, CPU)",(unsigned long long)(best?best->gva:0));
  if(best){ best->last_used=g_frame; g_blit_rt=1; blit_to_screen(best->id,1,(int)best->w,(int)best->h); g_blit_rt=0; g_last_is_rt=1; st_pres_rt++; return 1; }
  st_pres_cpu++;
  static int said; if(said<3){ said++;
    LOG("gpu: the image to present (%llx %dx%d) is not a known render target. Render targets:\n",(unsigned long long)cpu_addr,w,h);
    int n=0; for(int k=0;k<TEXH && n<12;k++) for(Tex* t=texh[k];t && n<12;t=t->next) if(t->is_rt){ n++;
      LOG("   rt gva=%llx cpu=%llx %ux%u fmt=%x used=%u\n",(unsigned long long)t->gva,(unsigned long long)gmmu_translate(t->gva),t->w,t->h,t->fkey,t->last_used); }
    if(!n) LOG("   (none: the game has not drawn with the GPU yet)\n"); }
  return 0; }

/* ============================================================ GPU thread */
extern void gpu_submit_now(u64* e, u32 n, u32 channel);
static u64 g_last_shot_hash; static u32 g_shot_done;   /* sequence number of the last screenshot taken */
static void do_screenshot(const char* path){
  int w=g_last_w? g_last_w : 1280, h=g_last_h? g_last_h : 720; if(g_last_is_rt){ w=(int)SC((u32)w); h=(int)SC((u32)h); }
  u8* px=malloc((size_t)w*h*4);
  if(g_last_present_tex){ GLuint fb; glGenFramebuffers(1,&fb); glBindFramebuffer(GL_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,g_last_present_tex,0); glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT,1); glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px); glBindFramebuffer(GL_FRAMEBUFFER,0); glDeleteFramebuffers(1,&fb); }
  else memset(px,0,(size_t)w*h*4);
  u64 hh=1469598103934665603ull;
  for(int y=0;y<h;y++){ int sy= g_last_flip? y : h-1-y; for(int x=0;x<w;x++){ u32 v; memcpy(&v,px+((size_t)sy*w+x)*4,4); v|=0xFF000000u; hh^=v; hh*=1099511628211ull; } }
  if(path){ char p[300]; snprintf(p,sizeof p,"%s.ppm",path); FILE* f=fopen(p,"wb");
    if(f){ fprintf(f,"P6\n%d %d\n255\n",w,h); for(int y=0;y<h;y++){ int sy= g_last_flip? y : h-1-y; for(int x=0;x<w;x++) fwrite(px+((size_t)sy*w+x)*4,1,3,f); } fclose(f); } }
  free(px); g_last_shot_hash=hh; }

/* debug: dump the recently used RTs to screenshots/ with some stats */
static void dump_rts(void){
  Tex* list[16]; int n=0;
  for(int k=0;k<TEXH;k++) for(Tex* t=texh[k];t;t=t->next) if(t->is_rt && !t->f.depth && !t->f.is_int){
    if(n<16){ list[n++]=t; } else { int mi=0; for(int i=1;i<16;i++) if(list[i]->last_used<list[mi]->last_used) mi=i; if(t->last_used>list[mi]->last_used) list[mi]=t; } }
  static int ndump; ndump++;
  LOG("---- render targets (dump %d, frame %u) ----\n",ndump,g_frame);
  plat_mkdir("captures");
  GLuint fb; glGenFramebuffers(1,&fb); glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);
  for(int i=0;i<n;i++){ Tex* t=list[i]; u32 w=SC(t->w), h=SC(t->h); if(!w||!h||w>8192||h>8192) continue;
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t->id,0);
    if(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){ LOG("  rt %llx %ux%u fmt=%x: cannot be read\n",(unsigned long long)t->gva,t->w,t->h,t->fkey); continue; }
    u8* px=malloc((size_t)w*h*4); glReadBuffer(GL_COLOR_ATTACHMENT0); glPixelStorei(GL_PACK_ALIGNMENT,1); glReadPixels(0,0,(GLsizei)w,(GLsizei)h,GL_RGBA,GL_UNSIGNED_BYTE,px);
    u64 sr=0,sg=0,sb=0,sa=0,nz=0; size_t np=(size_t)w*h;
    for(size_t k=0;k<np;k++){ u8* q=px+k*4; sr+=q[0]; sg+=q[1]; sb+=q[2]; sa+=q[3]; if(q[0]+q[1]+q[2]>12) nz++; }
    LOG("  rt %llx %ux%u fmt=%x used=%u  average color=(%llu,%llu,%llu,a%llu)  non-black=%llu%%\n",(unsigned long long)t->gva,t->w,t->h,t->fkey,t->last_used,
      (unsigned long long)(sr/np),(unsigned long long)(sg/np),(unsigned long long)(sb/np),(unsigned long long)(sa/np),(unsigned long long)(nz*100/np));
    /* downscaled, max 480 wide */
    u32 step=(w+479)/480; if(!step) step=1; u32 ow=w/step, oh=h/step; char path[256]; snprintf(path,sizeof path,"captures/rt_%d_%02d_%llx_%ux%u.ppm",ndump,i,(unsigned long long)t->gva,t->w,t->h);
    FILE* f=fopen(path,"wb"); if(f){ fprintf(f,"P6\n%u %u\n255\n",ow,oh); for(u32 y=0;y<oh;y++) for(u32 x=0;x<ow;x++){ u8* q=px+(((size_t)(h-1-y*step))*w+x*step)*4; fwrite(q,1,3,f); } fclose(f); }
    free(px); }
  glBindFramebuffer(GL_READ_FRAMEBUFFER,0); glDeleteFramebuffers(1,&fb);
  LOG("--------------------------------------------\n"); fflush(NULL);
}

static void APIENTRY gl_dbg(GLenum src, GLenum type, GLuint id, GLenum sev, GLsizei len, const GLchar* msg, const void* up){
  (void)src;(void)id;(void)len;(void)up; if(sev==GL_DEBUG_SEVERITY_NOTIFICATION) return; if(type==GL_DEBUG_TYPE_ERROR) WARN("GL: %s\n",msg); }
/* ============================================================ shader pack
   every new program's GLSL (driver independent) gets appended to shader_cache/shader_pack.bin. at startup
   whatever isn't in the binary cache yet is compiled in the background. the pack can be moved to another PC */
#define PACK_PATH "shader_cache/shader_pack_v3.bin"   /* versioned with the translator */
static u64* pk_keys; static u32 pk_n, pk_cap;

static int pk_has(u64 k){ if(!pk_cap) return 0; u32 i=(u32)(k^(k>>29))&(pk_cap-1); while(pk_keys[i]){ if(pk_keys[i]==k) return 1; i=(i+1)&(pk_cap-1); } return 0; }

static void pk_put(u64 k){ if(!k) k=1; if(pk_n*2>=pk_cap){ u32 oc=pk_cap; u64* ok=pk_keys; pk_cap= oc? oc*2 : 4096; pk_keys=calloc(pk_cap,8); pk_n=0; for(u32 i=0;i<oc;i++) if(ok[i]) pk_put(ok[i]); free(ok); }
  u32 i=(u32)(k^(k>>29))&(pk_cap-1); while(pk_keys[i]){ if(pk_keys[i]==k) return; i=(i+1)&(pk_cap-1); } pk_keys[i]=k; pk_n++; }
/* disk writes happen on a background thread; with AV scanning, an fopen on Windows can take tens of ms */
typedef struct AwJob { struct AwJob* next; char path[200]; u8* data; size_t n; int append; } AwJob;
static AwJob* volatile g_aw_head; static AwJob* g_aw_tail; static PMutex g_aw_m; static volatile int g_aw_on;

static void aw_thread(void* arg){ plat_diag_role("cache-writer"); (void)arg; plat_thread_low_priority();
  for(;;){ pm_lock(&g_aw_m); AwJob* j=g_aw_head; if(j){ g_aw_head=j->next; if(!g_aw_head) g_aw_tail=NULL; } pm_unlock(&g_aw_m);
    if(!j){ plat_sleep_ns(20000000); continue; }
    FILE* f=fopen(j->path,j->append? "ab" : "wb"); if(f){ fwrite(j->data,1,j->n,f); fclose(f); } free(j->data); free(j); } }

static void aw_write(const char* path, const void* d, size_t n, int append){
  if(!g_aw_on){ pm_init(&g_aw_m); g_aw_on= plat_thread(aw_thread,NULL,256*1024)? -1 : 1; }
  if(g_aw_on<0){ FILE* f=fopen(path,append? "ab" : "wb"); if(f){ fwrite(d,1,n,f); fclose(f); } return; }
  AwJob* j=calloc(1,sizeof *j); snprintf(j->path,sizeof j->path,"%s",path); j->data=malloc(n?n:1); memcpy(j->data,d,n); j->n=n; j->append=append;
  pm_lock(&g_aw_m); if(g_aw_tail) g_aw_tail->next=j; else g_aw_head=j; g_aw_tail=j; pm_unlock(&g_aw_m); }

static void pack_add(u64 key, Sh* vs, Sh* fs){
  if(!vs->src || !fs->src || pk_has(key)) return; pk_put(key);
  u32 a=(u32)strlen(vs->src), b=(u32)strlen(fs->src), mg=0x4B435053; size_t n=4+8+4+a+4+b; u8* d=malloc(n), *q=d;
  memcpy(q,&mg,4); q+=4; memcpy(q,&key,8); q+=8; memcpy(q,&a,4); q+=4; memcpy(q,vs->src,a); q+=a; memcpy(q,&b,4); q+=4; memcpy(q,fs->src,b);
  aw_write(PACK_PATH,d,n,1); free(d); }

static void pack_add_sh(u64 key, Sh* s){
  if(!s->src || pk_has(key)) return; pk_put(key);
  u32 a=(u32)strlen(s->src), mg=0x33504853, st=(u32)s->stage; u8* d=malloc(20+a); { u8* q=d; memcpy(q,&mg,4); q+=4; memcpy(q,&key,8); q+=8; memcpy(q,&st,4); q+=4; memcpy(q,&a,4); q+=4; memcpy(q,s->src,a); }
  aw_write(PACK_PATH,d,20+a,1); free(d); }
typedef struct { u64 key; char* vs; char* fs; int sep; u32 stage; } PackEnt;
static PackEnt* wq; static u32 wq_n, wq_i, wq_done;
typedef struct { GLuint p, a, b; u64 key; } WarmJob;
static WarmJob wj[6]; static int wj_n;

static void warm_load(void){
  pcache_init();translated_load(); FILE* f=fopen(PACK_PATH,"rb"); if(!f) return; u32 cap=0;
  for(;;){ u32 mg,a,b; u64 key; if(fread(&mg,4,1,f)!=1 || (mg!=0x4B435053 && mg!=0x33504853) || fread(&key,8,1,f)!=1) break;
    if(mg==0x33504853){ u32 st=0; if(fread(&st,4,1,f)!=1 || fread(&a,4,1,f)!=1 || a>(8u<<20)) break;
      char* src=malloc(a+1); if(fread(src,1,a,f)!=a){ free(src); break; } src[a]=0;
      if(pk_has(key)){free(src);continue;}pk_put(key);
      if(g_worker!=1){
        if(g_pcache>0 && (st!=ST_VS || strstr(src,"gl_Position = vec4(0.0, 0.0, 0.0, 1.0)"))){
          char path[160];snprintf(path,sizeof path,"shader_cache/%s_s%016llx.bin",g_pc_tag,(unsigned long long)key);
          GLuint p=binary_load(path,1);if(p){GLint uloc=glGetUniformLocation(p,st==ST_VS?"swr_vp":"swr_at");warm_stage_put(key,p,uloc);}
        }free(src);continue;
      }
      char path[160]; snprintf(path,sizeof path,"shader_cache/%s_s%016llx.bin",g_pc_tag,(unsigned long long)key);
      FILE* t= g_pcache? fopen(path,"rb") : NULL; if(t){ fclose(t); free(src); continue; }
      if(wq_n>=cap){ cap= cap? cap*2 : 256; wq=realloc(wq,cap*sizeof *wq); } wq[wq_n].key=key; wq[wq_n].vs=src; wq[wq_n].fs=NULL; wq[wq_n].sep=1; wq[wq_n].stage=st; wq_n++; continue; }
    if(fread(&a,4,1,f)!=1 || a>(8u<<20)) break;
    char* vs=malloc(a+1); if(fread(vs,1,a,f)!=a){ free(vs); break; } vs[a]=0;
    if(fread(&b,4,1,f)!=1 || b>(8u<<20)){ free(vs); break; } char* fs=malloc(b+1); if(fread(fs,1,b,f)!=b){ free(vs); free(fs); break; } fs[b]=0;
    if(pk_has(key)){ free(vs); free(fs); continue; } pk_put(key);
    char path[160]; snprintf(path,sizeof path,"shader_cache/%s_%016llx.bin",g_pc_tag,(unsigned long long)key);
    FILE* t= g_pcache? fopen(path,"rb") : NULL; if(t){ fclose(t); free(vs); free(fs); continue; }   /* already have a binary for this PC */
    if(wq_n>=cap){ cap= cap? cap*2 : 256; wq=realloc(wq,cap*sizeof *wq); } wq[wq_n].key=key; wq[wq_n].vs=vs; wq[wq_n].fs=fs; wq[wq_n].sep=0; wq[wq_n].stage=0; wq_n++; }
  fclose(f);
  if(warm_stage_count)LOG("shaders: %u cached stage programs prepared before starting the game\n",warm_stage_count);
  if(wq_n) LOG("shaders: %u from the pack are precompiled in the background while you play\n",wq_n); }
static WJob* wlo[16]; static int wlo_n;

static void warm_step(void){
  if(g_worker==1){   /* compile thread available: feed it a few low priority jobs at a time */
    for(int j=0;j<wlo_n;){ if(__atomic_load_n(&wlo[j]->state,__ATOMIC_ACQUIRE)){ free(wlo[j]->own_vs); free(wlo[j]->own_fs); free(wlo[j]); wlo[j]=wlo[--wlo_n]; wq_done++;
        if(wq_done==wq_n) LOG("shaders: precompiled pack (%u programs)\n",wq_n); } else j++; }
    while(wlo_n<8 && wq_i<wq_n){ PackEnt* e=&wq[wq_i]; WJob* j=calloc(1,sizeof *j); j->own_vs=e->vs; j->own_fs=e->fs; j->vs=e->vs; j->fs=e->fs; j->key=e->key; j->keep=0; j->sep=e->sep; j->stype= e->stage==ST_VS? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
      if(!wr_push(&wq_lo,j)){ free(j); break; } e->vs=e->fs=NULL; wq_i++; wlo[wlo_n++]=j; }
    return; }
  if(g_par<=0 || !g_pcache) return;
  for(int j=0;j<wj_n;){ GLint done=0; glGetProgramiv(wj[j].p,0x91B1,&done); if(!done){ j++; continue; }
    GLint ok=0; glGetProgramiv(wj[j].p,GL_LINK_STATUS,&ok);
    if(ok)binary_save(wj[j].p,wj[j].key,0);
    glDeleteProgram(wj[j].p); glDeleteShader(wj[j].a); glDeleteShader(wj[j].b); wj[j]=wj[--wj_n]; wq_done++;
    if(wq_done==wq_n) LOG("shaders: precompiled pack (%u programs)\n",wq_n); }
  while(wj_n<4 && wq_i<wq_n){ PackEnt* e=&wq[wq_i++]; if(e->sep){ free(e->vs); e->vs=NULL; wq_done++; continue; } WarmJob* j=&wj[wj_n++]; j->key=e->key;
    j->a=glCreateShader(GL_VERTEX_SHADER); const char* s1=e->vs; glShaderSource(j->a,1,&s1,NULL); glCompileShader(j->a);
    j->b=glCreateShader(GL_FRAGMENT_SHADER); const char* s2=e->fs; glShaderSource(j->b,1,&s2,NULL); glCompileShader(j->b);
    j->p=glCreateProgram(); glAttachShader(j->p,j->a); glAttachShader(j->p,j->b); p_glProgramParameteri(j->p,GL_PROGRAM_BINARY_RETRIEVABLE_HINT,GL_TRUE); glLinkProgram(j->p);
    free(e->vs); free(e->fs); e->vs=e->fs=NULL; } }
/* ============================================================ shader pre-build (ini prebuild_shaders=1)
   Metroid Dread keeps every shader as NVN compiled programs in romfs/system/shd/*.bshdat. Before the game draws,
   every vertex and pixel program found there goes through what a draw does on first use (translate, sh_post, the
   separable program's binary under the same key), so a later draw loads a binary instead of compiling. Programs are
   keyed as a draw with all-float vertex attributes, textures and render targets keys them (key2 0): integer variants
   (skinning indices, integer targets) still build at first use. A file once done is listed (name and size) in
   shader_cache/prebuilt_<tag>.txt and skipped next time, so only new or changed files cost time */
int g_opt_prebuild;
extern int shader_program_check(const u8* code, u32 max_bytes, u32* out_len, int* type);
void (*g_backend_status)(const char* text);   /* the window title (backend_win32.c sets it); NULL elsewhere */
static void backend_status(const char* t){ if(g_backend_status) g_backend_status(t); }
static int pb_no_cb(u32 idx, u32 off, u32* out){ (void)idx; (void)off; *out=0; trans_overflow=1; return 0; }   /* no GPU state yet */
static void prebuild_shaders(void){
  if(!g_opt_prebuild || g_gles || g_sep!=1) return;
  pcache_init(); if(g_pcache<=0){ LOG("shaders: pre-build needs program binaries, which this driver lacks\n"); return; }
  extern char g_romfs_dir_override[1024]; char dir[1200];
  if(g_romfs_dir_override[0]) snprintf(dir,sizeof dir,"%s/system/shd",g_romfs_dir_override); else snprintf(dir,sizeof dir,"%sromfs/system/shd",g_datadir);
  PDir d; if(plat_dir_open(&d,dir)){ LOG("shaders: pre-build: no shader folder at %s\n",dir); return; }
  typedef struct { char name[256]; u64 size; } PbFile; PbFile* fl=NULL; u32 nf=0, cap=0;
  while(plat_dir_next(&d)){ size_t l=strlen(d.name); if(d.is_dir || l<7 || l>=256 || strcmp(d.name+l-7,".bshdat")) continue;
    if(nf==cap){ cap= cap? cap*2 : 512; fl=realloc(fl,cap*sizeof *fl); } snprintf(fl[nf].name,256,"%s",d.name); fl[nf].size=d.size; nf++; }
  plat_dir_close(&d);
  char donep[200]; snprintf(donep,sizeof donep,"shader_cache/prebuilt_%s.txt",g_pc_tag);
  char* done=NULL; { FILE* f=fopen(donep,"rb"); if(f){ fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); done=calloc(1,(size_t)n+2); done[0]='\n'; if(fread(done+1,1,(size_t)n,f)){} fclose(f); } }
  u32 todo=0; for(u32 i=0;i<nf;i++){ char ln[300]; snprintf(ln,sizeof ln,"\n%s %llu\n",fl[i].name,(unsigned long long)fl[i].size);
    if(done && strstr(done,ln)) fl[i].size=~0ull; else todo++; }   /* size ~0: already built */
  free(done);
  if(!todo){ LOG("shaders: pre-build: all %u shader files were built before\n",nf); free(fl); return; }
  LOG("shaders: pre-build: %u of %u shader files to go through\n",todo,nf);
  u64* seen=calloc(1u<<16,8); u32 nseen=0;   /* program hash ^ stage, open addressing */
  u64 t0=plat_ns(), last_ui=0; u32 progs=0, built=0, had=0, failed=0, filesdone=0; u64 compile_ns=0;
  FILE* df=fopen(donep,"ab");
  for(u32 fi=0;fi<nf;fi++){ if(fl[fi].size==~0ull) continue;
    char path[1500]; snprintf(path,sizeof path,"%s/%s",dir,fl[fi].name);
    FILE* f=fopen(path,"rb"); if(!f) continue; u8* buf=malloc(fl[fi].size+8); size_t n=fread(buf,1,fl[fi].size,f); fclose(f);
    for(size_t o=0;o+128<=n;o+=4){
      u32 w0; memcpy(&w0,buf+o,4); if(((w0>>5)&31)!=3 || ((w0&31)!=1 && (w0&31)!=2)) continue;
      u32 avail= n-o>0x10000? 0x10000 : (u32)(n-o), len=0; int ty=0;
      if(!shader_program_check(buf+o,avail,&len,&ty)) continue;
      int stage= ty==1? ST_VS : ty==5? ST_FS : -1; if(stage<0) continue;
      u32 hl; u64 h=shader_hash(buf+o,avail,&hl);
      { u64 k=(h^(u64)(stage+1))|1; u32 i=(u32)(k^(k>>31))&0xFFFF; while(seen[i] && seen[i]!=k) i=(i+1)&0xFFFF;
        if(seen[i]) continue; if(nseen>=60000) continue; seen[i]=k; nseen++; }
      progs++;
      ShaderInfo si; memset(&si,0,sizeof si); g_sh_bind= stage==ST_VS? 0 : 4;
      u64 tk=translated_key(h,stage,0,&si); Translated* c=translated_get(tk); char* src=NULL;
      if(c){ si=c->si; src=malloc(strlen(c->src)+1); strcpy(src,c->src); }
      else { trans_ndep=0; trans_overflow=0; g_shader_cb_read=pb_no_cb; src=shader_to_glsl(buf+o,avail,stage,&si); g_shader_cb_read=sh_cb_read;
        if(!src || si.unsupported){ free(src); failed++; continue; }
        src=sh_post(src,stage); translated_save(tk,&si,src); }   /* translated_save skips one that needed constant buffers */
      if(stage==ST_VS) src=dlg_vs_xfb(src);
      Sh* s=calloc(1,sizeof *s); s->stage=stage; s->hash=h; s->src=src; u64 key=sh_key(s);
      char bp[200]; snprintf(bp,sizeof bp,"shader_cache/%s_s%016llx.bin",g_pc_tag,(unsigned long long)key);
      FILE* t=fopen(bp,"rb");
      if(t){ fclose(t); had++; }
      else { u64 c0=plat_ns();
        GLuint a=glCreateShader(stage==ST_VS? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER); const char* s1=src; glShaderSource(a,1,&s1,NULL); glCompileShader(a);
        GLuint p=glCreateProgram(); glAttachShader(p,a); p_glProgramParameteri(p,0x8258,GL_TRUE); p_glProgramParameteri(p,GL_PROGRAM_BINARY_RETRIEVABLE_HINT,GL_TRUE);
        glLinkProgram(p); glDeleteShader(a); GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
        if(ok){ binary_save(p,key,1); built++;
          /* kept ready for the game, as warm_load does with binaries from earlier runs (same stage rule): warm_load ran
             before this, so a program built now would otherwise be loaded back from disk at its first draw (measured
             10-20 ms each on a GTX 1080: 264 such loads in a first session) */
          if(stage!=ST_VS || strstr(src,"gl_Position = vec4(0.0, 0.0, 0.0, 1.0)")){ warm_stage_put(key,p,glGetUniformLocation(p,stage==ST_VS?"swr_vp":"swr_at")); p=0; } }
        else failed++;
        if(p) glDeleteProgram(p); compile_ns+=plat_ns()-c0; }
      pack_add_sh(key,s); free(src); free(s);
      u64 now=plat_ns(); if(now-last_ui>250000000ull){ last_ui=now; char st[160];
        snprintf(st,sizeof st,"Building shaders: file %u of %u, %u shaders (%u new)",filesdone+1,todo,progs,built); backend_status(st); } }
    free(buf); filesdone++;
    if(df){ fprintf(df,"%s %llu\n",fl[fi].name,(unsigned long long)fl[fi].size); fflush(df); } }
  if(df) fclose(df);
  backend_status(NULL); free(seen); free(fl);
  LOG("shaders: pre-build: %u files, %u programs: %u compiled now (%.1f s), %u had a binary, %u failed; %.1f s in all\n",
      filesdone,progs,built,compile_ns/1e9,had,failed,(plat_ns()-t0)/1e9); }
static FILE* g_rf; static u64 g_rf_t0;
static u64 g_fr_t0, g_fr_max; static int g_fr_slow; static u64 g_fr_shnew;
/* ---- dynamic res: move every RT to the new scale with a GPU copy */
/* UBO with 1/scale for gl_FragCoord math (shaders do uv = pos * 1/native size) */
static GLuint g_rs_ubo;

static void rs_update(void){ extern int g_rs_binding;
  if(!g_rs_ubo){ GLint mx=84; glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS,&mx); if(mx<2) return; g_rs_binding=mx-1; glGenBuffers(1,&g_rs_ubo); glBindBuffer(GL_UNIFORM_BUFFER,g_rs_ubo); glBufferData(GL_UNIFORM_BUFFER,16,NULL,GL_DYNAMIC_DRAW); }
  float v[4]={1.0f/g_scale,1.0f/g_scale,g_scale,0}; glBindBuffer(GL_UNIFORM_BUFFER,g_rs_ubo); glBufferSubData(GL_UNIFORM_BUFFER,0,16,v); glBindBufferBase(GL_UNIFORM_BUFFER,(GLuint)g_rs_binding,g_rs_ubo); }

static void rt_rescale(float ns){
  float old=g_scale; if(ns==old) return;
  glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB); glColorMaski(0,1,1,1,1); glDepthMask(GL_TRUE); glStencilMaskSeparate(GL_FRONT_AND_BACK,0xFF);
  int n=0;
  /* parked aliases (gva ~0, alias set) come back through rt_texture without a size check: resize them too */
  for(int k=0;k<TEXH;k++) for(Tex* t=texh[k];t;t=t->next){ if(!t->is_rt || (t->gva==~0ull && !t->alias)) continue;
    u32 ow=(u32)(t->w*old+0.5f), oh=(u32)(t->h*old+0.5f), nw=(u32)(t->w*ns+0.5f), nh=(u32)(t->h*ns+0.5f); if(!ow) ow=1; if(!oh) oh=1; if(!nw) nw=1; if(!nh) nh=1;
    GLuint nt=0; glGenTextures(1,&nt); glBindTexture(GL_TEXTURE_2D,nt); glTexStorage2D(GL_TEXTURE_2D,1,t->f.ifmt,nw,nh);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    int ds= t->f.depth && t->f.fmt==GL_DEPTH_STENCIL;
    GLenum att= t->f.depth? (ds? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT) : GL_COLOR_ATTACHMENT0;
    GLbitfield mask= t->f.depth? (GL_DEPTH_BUFFER_BIT|(ds? GL_STENCIL_BUFFER_BIT : 0)) : GL_COLOR_BUFFER_BIT;
    GLenum none=GL_NONE, c0=GL_COLOR_ATTACHMENT0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,g_rfbo); glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0); glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,att,GL_TEXTURE_2D,t->id,0); glReadBuffer(t->f.depth? GL_NONE : GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,g_dfbo); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,att,GL_TEXTURE_2D,nt,0); glDrawBuffers(1,t->f.depth? &none : &c0);
    glBlitFramebuffer(0,0,(GLint)ow,(GLint)oh,0,0,(GLint)nw,(GLint)nh,mask,(t->f.depth||t->f.is_int)? GL_NEAREST : GL_LINEAR);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,att,GL_TEXTURE_2D,0,0); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,att,GL_TEXTURE_2D,0,0);
    if(g_last_present_tex==t->id) g_last_present_tex=nt;
    glDeleteTextures(1,&t->id); t->id=nt; t->wgen++;
    if(t->crop){ glDeleteTextures(1,&t->crop->id); if(t->crop->view) glDeleteTextures(1,&t->crop->view); free(t->crop); t->crop=NULL; }
    /* assembled cubemaps/arrays/mip chains have scaled storage too: rebuild them at the new size */
    if(t->lay){ glDeleteTextures(1,&t->lay->id); if(t->lay->view) glDeleteTextures(1,&t->lay->view); free(t->lay); t->lay=NULL; }
    if(t->view){ glDeleteTextures(1,&t->view); t->view=0; }
    n++; }
  { GLenum c0=GL_COLOR_ATTACHMENT0; glBindFramebuffer(GL_READ_FRAMEBUFFER,g_rfbo); glReadBuffer(GL_COLOR_ATTACHMENT0); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,g_dfbo); glDrawBuffers(1,&c0); }
  while(glGetError()!=GL_NO_ERROR){}
  g_scale=ns; rs_update(); g_fb_sig_ok=0; g_tmap_gen++; mb_reset(); state_dirty(); glBindFramebuffer(GL_FRAMEBUFFER,g_fbo);
  LOG("gpu: internal resolution %.0f%% -> %.0f%% (%d render targets)\n",old*100,ns*100,n);
  if(g_rf){ fprintf(g_rf,"   internal resolution %.0f%% -> %.0f%%\n",old*100,ns*100); fflush(g_rf); } }
static u64 rd_t0, rd_f0, rd_busy0, rd_cpu0, rd_swap; static int rd_hold, rd_up; static u64 rd_lastdown;

static void res_dyn_step(void){
  extern int g_opt_resdyn; extern float g_opt_resmin, g_opt_resf;
  if(!g_opt_resdyn || g_gles || !p_cis) return;
  u64 now=plat_ns(); extern u64 plat_thread_cpu_ns(void);
  if(!rd_t0){ rd_t0=now; rd_f0=g_frame; rd_busy0=g_gpu_busy; rd_cpu0=plat_thread_cpu_ns(); rd_swap=0; return; }
  u64 dt=now-rd_t0; if(dt<1000000000ull) return;
  double fps=(double)(g_frame-rd_f0)*1e9/dt, swapw=(double)rd_swap/dt, busy=(double)(g_gpu_busy-rd_busy0)/dt; u64 c=plat_thread_cpu_ns(); double cpu=(double)(c-rd_cpu0)/dt;
  rd_t0=now; rd_f0=g_frame; rd_busy0=g_gpu_busy; rd_cpu0=c; rd_swap=0;
  { static int tst=-1; if(tst<0) tst=getenv("SWR_RES_TEST")!=NULL; if(tst){ rt_rescale(g_scale>0.95f? 0.7f : 1.0f); return; } }   /* test: flip the scale back and forth */
  { /* compiling shaders / loading: those hitches aren't GPU load, don't drop res */
    static u64 last_new; u32 pend=__atomic_load_n(&wq_hi.t,__ATOMIC_RELAXED)-__atomic_load_n(&wq_hi.h,__ATOMIC_RELAXED);
    if(pend>0 || st_sh_new!=last_new){ last_new=st_sh_new; rd_hold=2; return; } }
  if(rd_hold>0){ rd_hold--; return; }
  float mx=g_opt_resf, mn=g_opt_resmin; if(mn>mx) mn=mx;
  /* GPU-bound if the GPU thread spends its time in swap or busy without using CPU */
  int gpu_bound= swapw>0.20 || (busy>0.90 && cpu<0.55*busy);
  int headroom= swapw<0.04 && busy<0.65;
  if(fps<28.5 && gpu_bound && g_scale>mn+0.01f){ float ns=g_scale-0.1f; if(ns<mn) ns=mn; rt_rescale(ns); rd_hold=2; rd_up=0; rd_lastdown=now; }
  else if(headroom && g_scale<mx-0.01f && now-rd_lastdown>20000000000ull){ if(++rd_up>=4){ float ns=g_scale+0.1f; if(ns>mx) ns=mx; rt_rescale(ns); rd_hold=3; rd_up=0; } }
  else rd_up=0; }

/* end of frame: was it slow, and where did the time go */
static void frame_stats(void){
  u64 now=plat_ns(); if(!g_fr_t0){ g_fr_t0=now; memset(g_tm,0,sizeof g_tm); g_fr_shnew=st_sh_new; return; }
  u64 dt=now-g_fr_t0; g_fr_t0=now; if(dt>g_fr_max) g_fr_max=dt;
  if(dt>50000000ull && g_rf && g_fr_slow<300){   /* 50 ms: low-end PCs should show their drops too */ g_fr_slow++; u32 pend=__atomic_load_n(&wq_hi.t,__ATOMIC_RELAXED)-__atomic_load_n(&wq_hi.h,__ATOMIC_RELAXED);
    fprintf(g_rf,"   slow frame %.0f ms (GPU thread: shaders %.0f+%.0f, state %.0f, textures %.0f, buffers %.0f, driver %.0f, present %.0f ms) new shaders %llu, queued %u\n",
      dt/1e6,g_tm[TM_SH]/1e6,g_tm[TM_SHREADY]/1e6,g_tm[TM_STATE]/1e6,g_tm[TM_TEX]/1e6,(g_tm[TM_BUF]+g_tm[TM_CB]+g_tm[TM_GB])/1e6,g_tm[TM_DRAW]/1e6,g_tm[TM_SWAP]/1e6,(unsigned long long)(st_sh_new-g_fr_shnew),pend); }
  rd_swap+=g_tm[TM_SWAP];
  for(int k=0;k<TM_N;k++){ g_tmp[k]+=g_tm[k]; g_tm[k]=0; } g_fr_shnew=st_sh_new;
  { /* resolution edited in the ini while running: apply now */
    extern int g_opt_resdyn; extern float g_opt_resf; if(!g_opt_resdyn && !g_gles && p_cis && (g_scale>g_opt_resf+0.01f || g_scale<g_opt_resf-0.01f)) rt_rescale(g_opt_resf); }
  res_dyn_step(); }

static void gpu_thread(void* arg){ plat_diag_role("GPU-submit"); t_gpu_thread=1; if(getenv("SWR_PROFILE_RING_HOST")){ extern void plat_prof_mark_gpu(void); plat_prof_mark_gpu(); }   /* ring samples this thread too */
  (void)arg;
  { extern void plat_thread_high_priority(void); plat_thread_high_priority(); }
  { int qs=glq_start();   /* 1: GL lives on its own thread (context + fn pointers made there), 0: this thread */
    if(qs<0 || (qs==0 && (!backend_gl_create() || !swr_gl_load(backend_gl_proc)))){ WARN("No OpenGL 4.3: running without a graphics card\n"); g_gl=0; g_gl_ready=1; return; } }
  { extern int g_glsl_version, g_glsl_es; GLint mj=4, mn=3; glGetIntegerv(GL_MAJOR_VERSION,&mj); glGetIntegerv(GL_MINOR_VERSION,&mn); g_glsl_version= (mj>4||(mj==4&&mn>=4))? 440 : 430;
    const char* vs=(const char*)glGetString(GL_VERSION); if(vs && strstr(vs,"OpenGL ES")){ g_gles=1; g_glsl_es=1; }
    if(g_gles){ GLint n=0; glGetIntegerv(GL_NUM_EXTENSIONS,&n); typedef const GLubyte* (APIENTRY*GSI)(GLenum,GLuint); GSI gsi=(GSI)backend_gl_proc("glGetStringi");
      for(GLint k=0;k<n && gsi;k++){ const char* e=(const char*)gsi(GL_EXTENSIONS,(GLuint)k); if(!e) continue;
        if(!strcmp(e,"GL_KHR_texture_compression_astc_ldr")) g_has_astc=1;
        if(!strcmp(e,"GL_EXT_texture_compression_s3tc")) g_has_s3tc=1;
        if(!strcmp(e,"GL_EXT_texture_compression_rgtc")) g_has_rgtc=1;
        if(!strcmp(e,"GL_EXT_texture_compression_bptc")) g_has_bptc=1; }
      if(getenv("SWR_FORCE_DECODE")){ g_has_s3tc=g_has_rgtc=g_has_bptc=0; }
      LOG("GLES: native ASTC=%d, BC1-3=%d, BC4-5=%d, BC6-7=%d (the rest are decoded on the CPU)\n",g_has_astc,g_has_s3tc,g_has_rgtc,g_has_bptc); } }
  LOG("OpenGL: %s / %s / %s\n",(const char*)glGetString(GL_VENDOR),(const char*)glGetString(GL_RENDERER),(const char*)glGetString(GL_VERSION));
  { /* compile on the driver's threads so we don't freeze */
    g_par=0; int vl=0; GLint n=0; glGetIntegerv(GL_NUM_EXTENSIONS,&n); typedef const GLubyte* (APIENTRY*GSI2)(GLenum,GLuint); GSI2 gsi=(GSI2)backend_gl_proc("glGetStringi");
    for(GLint k=0;k<n && gsi;k++){ const char* e=(const char*)gsi(GL_EXTENSIONS,(GLuint)k); if(!e) continue;
      if(!strcmp(e,"GL_KHR_parallel_shader_compile")||!strcmp(e,"GL_ARB_parallel_shader_compile")) g_par=1;
      if(!g_gles && !strcmp(e,"GL_KHR_texture_compression_astc_ldr")) g_has_astc=1;
      if(!strcmp(e,"GL_ARB_shader_viewport_layer_array")) vl|=1; if(!strcmp(e,"GL_AMD_vertex_shader_layer")) vl|=2; if(!strcmp(e,"GL_NV_viewport_array2")) vl|=4; }
    { extern int g_vs_layer_ext; g_vs_layer_ext= g_gles? 0 : (vl&1)? 1 : (vl&2)? 2 : (vl&4)? 3 : 0;
      LOG("gpu: render target layer from the vertex shader: %s\n",g_vs_layer_ext==1? "ARB_shader_viewport_layer_array" : g_vs_layer_ext==2? "AMD_vertex_shader_layer" : g_vs_layer_ext==3? "NV_viewport_array2" : "not available (drawn to layer 0)"); }
    /* Desktop ASTC from the driver only on Intel, whose GPUs decode it in hardware. AMD's driver lists the extension
       on GPUs without ASTC hardware, and two AMD cards (RX 9060 XT, RX 7800 XT) crashed inside the driver on the same
       instruction as the first gameplay area's textures loaded (GitHub issues #12, #7), on the path no NVIDIA card
       takes: everyone else gets our CPU decoder (Dread has few ASTC textures: 0.2 ms in all). SWR_ASTC_GPU=1 = trust
       any driver that lists it (tests) */
    if(!g_gles && g_has_astc && !getenv("SWR_ASTC_GPU")){ const char* v=(const char*)glGetString(GL_VENDOR); if(!v || !strstr(v,"Intel")) g_has_astc=0; }
    if(!g_gles) LOG("ASTC textures: %s\n",g_has_astc && !getenv("SWR_ASTC_CPU")? "decoded by the GPU (fast, with mipmaps)" : "decoded on the CPU");
    if(getenv("SWR_SYNC_SHADERS")) g_par=0;
    if(g_par){ typedef void (APIENTRY*MSCT)(GLuint); MSCT f=(MSCT)backend_gl_proc("glMaxShaderCompilerThreadsKHR"); if(!f) f=(MSCT)backend_gl_proc("glMaxShaderCompilerThreadsARB"); if(f) f(0xFFFFFFFFu); }
    LOG("shaders: parallel compilation %s\n",g_par? "enabled (no stutter: anything still compiling is drawn as soon as it is ready)" : "not available in this driver"); }
  { GLint v[6]={0}; glGetIntegerv(0x8B4D,&v[0]); glGetIntegerv(0x8872,&v[1]); glGetIntegerv(0x8A2F,&v[2]); glGetIntegerv(0x8A2B,&v[3]); glGetIntegerv(0x8A2D,&v[4]); glGetIntegerv(0x826E,&v[5]);
    LOG("OpenGL limits: textures %d (per stage %d), UBO %d (VS %d, FS %d), uniform locations %d\n",v[0],v[1],v[2],v[3],v[4],v[5]); }
  if(g_verbose>=2){ typedef void (APIENTRY*DBGCB)(GLDEBUGPROC,const void*); DBGCB f=(DBGCB)backend_gl_proc("glDebugMessageCallback"); if(f){ glEnable(GL_DEBUG_OUTPUT); f(gl_dbg,NULL); } }
  glGenFramebuffers(1,&g_fbo); glGenFramebuffers(1,&g_rfbo); glGenFramebuffers(1,&g_dfbo); glGenVertexArrays(1,&g_vao); glGenVertexArrays(1,&g_scratch_vao); glGenBuffers(32,g_vbuf); glGenBuffers(1,&g_ibuf);
  for(int s=0;s<5;s++) glGenBuffers(18,g_ubuf[s]);
  { GLuint a=compile(GL_VERTEX_SHADER,g_gles? BLIT_VS_ES : BLIT_VS,NULL,0), b=compile(GL_FRAGMENT_SHADER,g_gles? BLIT_FS_ES : BLIT_FS,NULL,0); g_blit_prog=link2(a,b,0); }
  if(!g_gles){ GLuint a=compile(GL_VERTEX_SHADER,BLIT_VS,NULL,0), b=compile(GL_FRAGMENT_SHADER,DOWN_FS,NULL,0); g_down_prog=link2(a,b,0);
    if(g_down_prog){ g_down_xf=glGetUniformLocation(g_down_prog,"xf"); g_down_px=glGetUniformLocation(g_down_prog,"px"); } }
  if(!g_gles){ glEnable(GL_FRAMEBUFFER_SRGB); glDisable(GL_FRAMEBUFFER_SRGB); glEnable(0x8642); }   /* GL_PROGRAM_POINT_SIZE, gl_PointSize comes from the shader */
  { extern float g_opt_resf; g_scale=g_opt_resf; if(g_scale!=1.0f) LOG("internal resolution x%.2f\n",g_scale); }
  rs_update();
  worker_start();
  sep_init();
  mb_init(); ur_init();
  if(!g_gles && !getenv("SWR_NO_CLIPCONTROL")) p_clip=(PFNGLCLIPCONTROLPROC)backend_gl_proc("glClipControl");
  if(!g_gles && !getenv("SWR_NO_TEXVIEW")) p_tview=(PFNGLTEXTUREVIEWPROC)backend_gl_proc("glTextureView");
  if(!g_gles) lsfg_init();
  p_cis=(PFNGLCOPYIMAGESUBDATAPROC)backend_gl_proc("glCopyImageSubData");
  gl_selftest(); state_dirty(); mb_reset();
  LOG("gpu: native 0..1 depth %s, texture views %s\n",p_clip? "yes" : "no",p_tview? "yes" : "no");
  LOG("gpu: texture multi-bind %s, persistent constant ring %s\n",p_bt? "yes" : "no",g_ur_ptr? "yes" : "no");
  warm_load();
  prebuild_shaders();
  FILE*work_trace=NULL;const char*work_path=getenv("SWR_TRACE_GPU_WORK");
  if(work_path&&work_path[0]){work_trace=fopen(work_path,"w");if(work_trace)fprintf(work_trace,"kind,frame,queued_ns,dequeued_ns,finished_ns,queue_ms,execute_ms,cpu_ms\n");}
  unsigned work_trace_rows=0;
  g_gl=1; g_gl_ready=1;
  for(;;){
    warm_step();
    { /* loading screens etc: log how long we went without a new image */
      static u64 lp_t, lp_f; u64 tn=plat_ns(); if(g_frame!=lp_f){ if(lp_t && tn-lp_t>1500000000ull && g_rf){ fprintf(g_rf,"   -> %.1f s without a new image (loading or waiting)\n",(double)(tn-lp_t)/1e9); fflush(g_rf); } lp_f=g_frame; lp_t=tn; } }
    { static u64 next_st; u64 now=plat_ns(); if(!next_st) next_st=now+5000000000ull;
      if(now>=next_st){ static int nst; nst++; next_st=now+5000000000ull;
        extern volatile u64 g_gpu_submits; extern int g_frames; extern volatile u64 g_dma_copies; extern volatile u64 g_compute_launches;
        LOG("gpu: frames=%d submits=%llu draws=%llu (drawn %llu, no shader %llu, failed shader %llu, no program %llu, no target %llu) clears=%llu shaders=%llu present: rt=%llu cpu=%llu | textures ok=%llu rt=%llu null=%llu type-mismatch=%llu, dma copies=%llu, compute=%llu, waiting for shader=%llu\n",
          g_frames,(unsigned long long)g_gpu_submits,(unsigned long long)g_draws,(unsigned long long)st_drawn,(unsigned long long)st_noshader,(unsigned long long)st_shfail,(unsigned long long)st_noprog,(unsigned long long)st_nort,
          (unsigned long long)st_clears,(unsigned long long)st_shaders,(unsigned long long)st_pres_rt,(unsigned long long)st_pres_cpu,
          (unsigned long long)st_tex_ok,(unsigned long long)st_tex_rt,(unsigned long long)st_tex_null,(unsigned long long)st_tex_mismatch,(unsigned long long)g_dma_copies,(unsigned long long)g_compute_launches,(unsigned long long)st_compiling);
        static u64 last_frames_pf;
        { static u64 last_t, last_cpu, last_frames; u64 cpu_now=plat_thread_cpu_ns(),dt=now-last_t; if(last_t && dt){
            LOG("performance: %.1f FPS, GPU thread CPU busy %d%% (host rendering-thread CPU time; not hardware GPU utilization)\n",
              (double)(g_frame-last_frames)*1e9/(double)dt,(int)((cpu_now-last_cpu)*100/dt)); }
          last_t=now; last_cpu=cpu_now; last_frames=g_frame; }
          { /* performance.txt: compact summary for diagnosing on someone else's PC */
            static u64 l_now, l_pcpu, l_gcpu, l_cn, l_cns, l_fr2, l_skip, l_dr2; FILE* rf=g_rf; u64 t_start=g_rf_t0; if(!rf){ rf=g_rf=fopen("performance.txt","w"); t_start=g_rf_t0=now; if(rf){ extern int plat_ncpu(void);
                fprintf(rf,"SwitchRecomp - performance (each line = last few seconds). CPU: %d logical processors. GPU: %s\n",plat_ncpu(),(const char*)glGetString(GL_RENDERER));
                { extern int g_code_tcc; if(g_code_tcc) fprintf(rf,"WARNING: the game code was compiled with TCC and runs several times slower than with GCC. Recompile with GCC.\n"); }
                fprintf(rf,"CPU accounting: 1.000 logical-core equivalents = one logical processor fully busy during the interval.\n");
                fprintf(rf," sec |  FPS | process CPU (logical-core equivalents) | GPU thread CPU (percent of one core) | draws/frame | shaders compiled (avg ms, max) | draws waiting for shader\n"); } }
            u64 pc=plat_process_cpu_ns(), gc=plat_thread_cpu_ns(); u64 dtn=now-l_now;
            if(rf && l_now && dtn){ extern int plat_ncpu(void); u64 cn=g_cmp_n-l_cn, cns=g_cmp_ns-l_cns; u64 frn=g_frame-l_fr2;
              fprintf(rf,"%4llu | %4.1f | %.3f core equivalents | %3d%% | %6llu | %4llu (%5.1f, %5.1f) | %llu\n",(unsigned long long)((now-t_start)/1000000000ull),(double)frn*1e9/(double)dtn,
                (double)(pc-l_pcpu)/dtn,(int)((gc-l_gcpu)*100/dtn),(unsigned long long)(frn? (st_drawn-l_dr2)/frn : 0),(unsigned long long)cn,cn? (double)cns/cn/1e6 : 0.0,(double)g_cmp_max/1e6,(unsigned long long)(st_compiling-l_skip));
              { double fr= frn? (double)frn : 1.0; static u64 l_bn, l_bns, l_csw;
                fprintf(rf,"      GPU thread breakdown (ms per frame): shaders %.1f + load %.1f | state %.1f | textures %.1f | buffers %.1f (constants %.1f, storage %.1f, vertices %.1f, compute waits %.1f) | driver draw %.1f | present/wait GPU %.1f | slowest frame %.0f ms | cache binaries %llu (%.1f ms)\n",
                  g_tmp[TM_SH]/1e6/fr,g_tmp[TM_SHREADY]/1e6/fr,g_tmp[TM_STATE]/1e6/fr,g_tmp[TM_TEX]/1e6/fr,(g_tmp[TM_BUF]+g_tmp[TM_CB]+g_tmp[TM_GB])/1e6/fr,
                  g_tmp[TM_CB]/1e6/fr,g_tmp[TM_GB]/1e6/fr,g_tmp[TM_BUF]/1e6/fr,(double)(st_cs_sync_ns-l_csw)/1e6/fr,g_tmp[TM_DRAW]/1e6/fr,g_tmp[TM_SWAP]/1e6/fr,g_fr_max/1e6,
                  (unsigned long long)(g_bin_n-l_bn),(g_bin_n-l_bn)? (double)(g_bin_ns-l_bns)/(g_bin_n-l_bn)/1e6 : 0.0);
                l_bn=g_bin_n; l_bns=g_bin_ns; l_csw=st_cs_sync_ns; memset(g_tmp,0,sizeof g_tmp); g_fr_max=0;
                {extern volatile u64 g_fs_read_ns,g_fs_lock_ns,g_fs_open_ns,g_fs_read_calls,g_fs_read_bytes,g_guest_poll_ns,g_guest_poll_calls;
                  static u64 io0,lock0,open0,call0,bytes0,poll0,pcall0;
                  u64 io=g_fs_read_ns,lk=g_fs_lock_ns,op=g_fs_open_ns,n=g_fs_read_calls,by=g_fs_read_bytes,po=g_guest_poll_ns,pn=g_guest_poll_calls;
                  fprintf(rf,"      asset IO: %llu calls, %.1f MB, %.1f ms total, %.1f ms lock wait, %.1f ms open; main load-group sleeps: %llu / %.1f ms; warmed programs used %u/%u\n",(unsigned long long)(n-call0),(by-bytes0)/1048576.0,(io-io0)/1e6,(lk-lock0)/1e6,(op-open0)/1e6,(unsigned long long)(pn-pcall0),(po-poll0)/1e6,warm_stage_used,warm_stage_count);fprintf(rf,"      translated shader cache: %u reused / %u stored\n",translated_hits,translated_count);{static u64 ha0,tr0,re0,un0;fprintf(rf,"      shader work totals: hash %.2f ms, translate %.2f ms, reuse %.2f ms, uniform query %.2f ms\n",(shader_hash_ns-ha0)/1e6,(shader_translate_ns-tr0)/1e6,(shader_reuse_ns-re0)/1e6,(shader_uniform_ns-un0)/1e6);ha0=shader_hash_ns;tr0=shader_translate_ns;re0=shader_reuse_ns;un0=shader_uniform_ns;}{static u64 na0,nb0,nw0;fprintf(rf,"      GL object names: %llu allocated, %llu queue round trips, %.2f ms waiting\n",(unsigned long long)(name_allocations-na0),(unsigned long long)(name_batches-nb0),(name_wait_ns-nw0)/1e6);na0=name_allocations;nb0=name_batches;nw0=name_wait_ns;}
                  {static u64 previous[5]; double elapsed[5]; extern int g_frames,g_opt_fps; static int previous_presented; int presented=g_frames; fprintf(rf,"      frame cap: active fps_limit=%d, guest presents=%d, GPU presents=%llu in %.3f seconds\n",g_opt_fps,presented-previous_presented,(unsigned long long)frn,dtn/1e9); previous_presented=presented; {static u64 sc0,sn0;fprintf(rf,"      sampler name requests: %llu calls, %.2f ms\n",(unsigned long long)(sampler_query_calls-sc0),(sampler_query_ns-sn0)/1e6);sc0=sampler_query_calls;sn0=sampler_query_ns;} {static u64 last_total,last_names,last_storage,last_calls;fprintf(rf,"      texture creation: %llu objects, %.2f ms total, %.2f names/bind, %.2f storage submission\n",(unsigned long long)(tex_creation_calls-last_calls),(tex_creation_ns-last_total)/1e6,(tex_creation_names_ns-last_names)/1e6,(tex_creation_storage_ns-last_storage)/1e6);last_total=tex_creation_ns;last_names=tex_creation_names_ns;last_storage=tex_creation_storage_ns;last_calls=tex_creation_calls;} {static u64 previous_allocations;fprintf(rf,"      upload scratch: %llu allocation calls, %.1f MB retained\n",(unsigned long long)(upload_allocations-previous_allocations),(upload_linear.capacity+upload_source.capacity+upload_decoded.capacity)/1048576.0);previous_allocations=upload_allocations;} for(int j=0;j<5;j++){elapsed[j]=(tex_stage_totals[j]-previous[j])/1e6; previous[j]=tex_stage_totals[j];} fprintf(rf,"      texture upload totals: %.2f ms, read %.2f, unpack %.2f, decode %.2f, driver %.2f\n",elapsed[0],elapsed[1],elapsed[2],elapsed[3],elapsed[4]);}
                  io0=io;lock0=lk;open0=op;call0=n;bytes0=by;poll0=po;pcall0=pn;}
                { static u64 l_lg, l_lns; u64 ng=st_lsfg_gen-l_lg; if(ng){ fprintf(rf,"      frame generation (LSFG): +%llu frames -> %.1f FPS on screen, %.1f ms of GPU thread per generated frame\n",
                    (unsigned long long)ng,(double)(frn+ng)*1e9/(double)dtn,(double)(st_lsfg_ns-l_lns)/(double)ng/1e6); } l_lg=st_lsfg_gen; l_lns=st_lsfg_ns; } }
              fflush(rf); g_cmp_max=0; }
            plat_thread_cpu_report(rf,now,!l_now);
            l_now=now; l_pcpu=pc; l_gcpu=gc; l_cn=g_cmp_n; l_cns=g_cmp_ns; l_fr2=g_frame; l_skip=st_compiling; l_dr2=st_drawn; }
          { static u64 l_cbs, l_cbb, l_vbh, l_vbu, l_tex, l_dr; u64 fr=g_frame-last_frames_pf; if(!fr) fr=1;
            static u64 l_shf, l_sh, l_fbs, l_tbs, l_urw, l_tck, l_ss, l_dm, l_csg, l_css, l_csn, l_th, l_tm; extern u64 g_draws_merged;
            LOG("per-frame profile: %llu draws | constants: %.1f KB copied, %llu reused | vertices/indices: %.1f KB checked, %.1f KB uploaded | textures: %.1f KB checked, %.1f KB uploaded | shaders via fast cache %llu/%llu | same fbo %llu, same textures %llu, same state %llu, ring waits %llu, merged instances %llu | compute: %llu GPU copies, %llu waits (%.1f ms) | texture lookups reused %.0f%%\n",
              (unsigned long long)((st_drawn-l_dr)/fr),(double)(st_cb_bytes-l_cbb)/1024.0/fr,(unsigned long long)((st_cb_skip-l_cbs)/fr),(double)(st_vb_hashed-l_vbh)/1024.0/fr,(double)(st_vb_up-l_vbu)/1024.0/fr,
              (double)(st_tex_chk-l_tck)/1024.0/fr,(double)(st_tex_up-l_tex)/1024.0/fr,(unsigned long long)((st_sh_fast-l_shf)/fr),(unsigned long long)((st_shaders-l_sh)/fr),
              (unsigned long long)((st_fb_skip-l_fbs)/fr),(unsigned long long)((st_tb_skip-l_tbs)/fr),(unsigned long long)((st_state_skip-l_ss)/fr),(unsigned long long)(st_ur_wait-l_urw),(unsigned long long)((g_draws_merged-l_dm)/fr),
              (unsigned long long)((st_cs_gpucopy-l_csg)/fr),(unsigned long long)((st_cs_sync-l_css)/fr),(double)(st_cs_sync_ns-l_csn)/1e6/fr,
              (st_tdc_hit-l_th)+(st_tdc_miss-l_tm)? 100.0*(st_tdc_hit-l_th)/(double)((st_tdc_hit-l_th)+(st_tdc_miss-l_tm)) : 0.0);
            l_th=st_tdc_hit; l_tm=st_tdc_miss;
            l_csg=st_cs_gpucopy; l_css=st_cs_sync; l_csn=st_cs_sync_ns;
            l_dm=g_draws_merged; l_shf=st_sh_fast; l_sh=st_shaders; l_fbs=st_fb_skip; l_tbs=st_tb_skip; l_urw=st_ur_wait; l_tck=st_tex_chk; l_ss=st_state_skip;
            l_cbs=st_cb_skip; l_cbb=st_cb_bytes; l_vbh=st_vb_hashed; l_vbu=st_vb_up; l_tex=st_tex_up; l_dr=st_drawn; last_frames_pf=g_frame; } } }
    lsfg_pump();
    i64 to_=100000000; if(g_lfn){ u64 nw=plat_ns(); to_= g_lfq[0].t>nw? (i64)(g_lfq[0].t-nw) : 0; if(to_<200000) to_=200000; }
#ifdef _WIN32
    if(g_rec_exit==1){ rec_stop(); g_rec_exit=2; }
    if(g_rec_toggle && g_rec_on){ g_rec_toggle=0; rec_stop(); }   /* let F7 stop even when nothing is being presented */
#endif
    if(cs_inflight()){ cs_complete(0); if(cs_inflight() && to_>1000000) to_=1000000; }
    Work w; if(!q_pop(&w,to_)){ lsfg_pump(); continue; }
    u64 t_busy0=plat_ns(),work_cpu=work_trace?plat_thread_cpu_ns():0;u32 work_frame=g_frame;
    switch(w.kind){
      case WK_SUBMIT: g_submit_epoch++; ww_poll(); { extern volatile u64 g_gpu_sub_ns; static u32 fr=~0u; if(fr!=g_frame || !g_gpu_sub_ns){ fr=g_frame; g_gpu_sub_ns=w.sub_ns; } }   /* stamp the whole frame with the time of its first submit */ gpu_submit_now(w.ent,w.n,w.syncpt); cs_kick_async(); free(w.ent); cs_submit_done(w.syncpt,w.incs); break;
      case WK_PRESENT_CPU: trace_note("PRESENT CPU image %dx%d\n",w.w,w.h); present_cpu(w.px,w.w,w.h); free(w.px); g_frame++; gpu_evict_step(); frame_stats(); trace_frame(); break;
      case WK_PRESENT_ADDR: if(!present_addr(w.addr,w.w,w.h)){ extern void vi_convert_cpu(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, u32* out);
          u32* px=malloc((size_t)w.w*w.h*4); vi_convert_cpu(w.addr,w.w,w.h,w.n,(w.fmt>>8)&15,(w.fmt>>12)&15,w.fmt&0xFF,px); present_cpu(px,w.w,w.h); free(px); } g_frame++; dlg_frame(); gpu_evict_step(); frame_stats(); trace_frame(); break;
      case WK_SYNC: pm_lock(&g_ql);__atomic_store_n(w.done,1,__ATOMIC_RELEASE);pc_broadcast(&g_frame_consumed_cv);pm_unlock(&g_ql);break;
      case WK_DUMP: dump_rts(); g_tr_arm=1; break;
      case WK_SHOT: do_screenshot(w.path[0]? w.path : NULL); __atomic_store_n(&g_shot_done,w.n,__ATOMIC_SEQ_CST); break;
    }
    u64 finished=plat_ns();g_gpu_busy+=finished-t_busy0;
    if(work_trace){u64 cpu=plat_thread_cpu_ns()-work_cpu;
      fprintf(work_trace,"%d,%u,%llu,%llu,%llu,%.6f,%.6f,%.6f\n",w.kind,work_frame,(unsigned long long)w.queued_ns,(unsigned long long)t_busy0,(unsigned long long)finished,(t_busy0-w.queued_ns)/1e6,(finished-t_busy0)/1e6,cpu/1e6);
      if((++work_trace_rows&255)==0)fflush(work_trace);
    }
  }
}

void gpu_gl_start(void){
  trace_work_enabled=getenv("SWR_TRACE_GPU_WORK")!=NULL;
  pm_init(&g_ql); pc_init(&g_qcv); pc_init(&g_qspace);pc_init(&g_frame_consumed_cv);
  gl_frontend_available=1;
  if(plat_thread(gpu_thread,NULL,64u<<20)) fatal("could not create the GPU thread");
  while(!g_gl_ready) plat_sleep_ns(1000000);
}

int gpu_gl_present_pacing(void){
#if defined(_WIN32)
  extern int g_present_pacing_candidate,g_opt_fps,g_opt_vsync,g_lsfg_opt;extern volatile int g_vt_on,g_rec_active;
  return g_present_pacing_candidate&&g_gl&&glq_active()&&g_opt_fps&&!g_opt_vsync&&!g_lsfg_opt&&!g_vt_on&&!g_rec_active;
#else
  return 0;
#endif
}
void gpu_gl_frame_consumed(void){
  volatile int consumed=0;Work marker;memset(&marker,0,sizeof marker);marker.kind=WK_SYNC;marker.done=&consumed;q_push(&marker);
  pm_lock(&g_ql);while(!__atomic_load_n(&consumed,__ATOMIC_ACQUIRE))pc_wait(&g_frame_consumed_cv,&g_ql,-1);pm_unlock(&g_ql);
}
/* requests from other threads */
int gpu_async_submit(u64* e, u32 n, u32 syncpt, u32 incs){
  if(!g_gl) return 0;
  Work w; memset(&w,0,sizeof w); w.kind=WK_SUBMIT; { extern u64 guest_ns(void); w.sub_ns=guest_ns(); } w.ent=malloc((size_t)n*8+8); memcpy(w.ent,e,(size_t)n*8); w.n=n; w.syncpt=syncpt; w.incs=incs; q_push(&w); return 1; }

void gpu_gl_present_cpu(const u32* px, int w, int h, int stride){
  if(!g_gl) return;
  Work k; memset(&k,0,sizeof k); k.kind=WK_PRESENT_CPU; k.px=malloc((size_t)w*h*4); for(int y=0;y<h;y++) memcpy(k.px+(size_t)y*w,px+(size_t)y*stride,(size_t)w*4); k.w=w; k.h=h; q_push(&k); }

void gpu_gl_present_buffer(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt){
  Work k; memset(&k,0,sizeof k); k.kind=WK_PRESENT_ADDR; k.addr=cpu_addr; k.w=(int)w; k.h=(int)h; k.fmt=fmt|(layout<<8)|(bhl<<12); k.n=pitch; q_push(&k); }

u64 gpu_gl_screenshot(const char* path){
  { extern int g_snxvk_on; extern u64 snxvk_screenshot(const char*); if(g_snxvk_on) return snxvk_screenshot(path); }   /* renderer=vulkan_native */
  if(!g_gl) return 0;
  /* wait on a sequence number, not a flag on this stack: after a timeout the GPU thread would
     write into a stack frame that is gone */
  static u32 seq; u32 my=++seq;
  Work k; memset(&k,0,sizeof k); k.kind=WK_SHOT; k.n=my; if(path) strncpy(k.path,path,255); q_push(&k);
  u64 t0=plat_ns(); while((i32)(__atomic_load_n(&g_shot_done,__ATOMIC_SEQ_CST)-my)<0 && plat_ns()-t0<30000000000ull) plat_sleep_ns(2000000);
  return g_last_shot_hash; }

void gpu_gl_dump_rts(void){ if(!g_gl) return; Work k; memset(&k,0,sizeof k); k.kind=WK_DUMP; q_push(&k); }

/* ============================================================ compute (Kepler compute class, QMD)
   SMO fires ~25 dispatches a frame (particles, sims) touching global memory and a 3D texture.
   QMD -> compute GLSL, bind cbufs, textures and ssbos ({addr,size} in c0), and copy whatever got written
   back to guest memory afterwards. */
/* compute output goes GPU-side into a staging buffer and is read back in one go: before the next draw,
   at the end of the submit, or earlier if another dispatch reads that memory */
static GLuint g_cs_stage; static u32 g_cs_stage_cap, g_cs_stage_used; static int g_cs_npend;
static struct { u64 addr; u32 off, size; } g_cs_pend[256];

/* readback is async: at the end of each submit the staging copy gets a fence and we pick it up when it's
   signaled. only an actual read of that memory (draw, texture, DMA, another compute) has to wait */
typedef struct { u64 addr; u32 off, size; } CsPend;
static struct { GLuint buf; u32 cap, used; GLsync fence; int n; CsPend p[256]; } g_cs_fl;   /* in flight */
static u64 g_cs_lo=~0ull, g_cs_hi;   /* bounds of everything pending/in flight, for a quick reject */
static int g_cs_busy; volatile int g_cs_any; static struct { u32 id, n; } g_cs_incs[64]; static int g_cs_nincs;   /* g_cs_any: anything pending/in flight (gmem_ptr/gmem_read check it) */
static void cs_apply(const u8* tmp, const CsPend* p, int n){
  g_cs_busy=1;
  /* cached vertex/storage buffers on this range are stale */
  g_csgen++; for(int i=0;i<n;i++) for(u64 b=p[i].addr>>16, e=(p[i].addr+p[i].size-1)>>16; b<=e; b++) g_csblk[b&0xFFFF]=g_csgen;
  for(int i=0;i<n;i++){ u8* dst=gmem_ptr(p[i].addr,p[i].size);
    if(dst){ memcpy(dst,tmp+p[i].off,p[i].size); ww_touch((u64)(dst-g_mem)); } else gmem_write(p[i].addr,tmp+p[i].off,p[i].size); }
  g_cs_busy=0; }

static u8* cs_tmp(u32 n){ static u8* tmp; static u32 cap; if(cap<n){ tmp=realloc(tmp,n); cap=n; } return tmp; }

/* wait=0: only if the GPU is already done */
static void cs_complete(int wait){
  if(!g_cs_fl.fence) return;
  GLenum r=p_cws? p_cws(g_cs_fl.fence,GL_SYNC_FLUSH_COMMANDS_BIT,wait? 4000000000ull : 0) : GL_ALREADY_SIGNALED;
  if(r==GL_TIMEOUT_EXPIRED) return;
  if(p_ds) p_ds(g_cs_fl.fence); g_cs_fl.fence=0;
  u8* tmp=cs_tmp(g_cs_fl.used); glBindBuffer(GL_COPY_READ_BUFFER,g_cs_fl.buf); glGetBufferSubData(GL_COPY_READ_BUFFER,0,g_cs_fl.used,tmp);
  cs_apply(tmp,g_cs_fl.p,g_cs_fl.n); g_cs_fl.n=0; g_cs_fl.used=0; g_cs_any= g_cs_npend>0; if(!g_cs_any){ g_cs_lo=~0ull; g_cs_hi=0; }
  /* submits that completed while this was in flight only count as done now; the game waits on them before reading */
  extern void syncpt_incr(u32 id); for(int i=0;i<g_cs_nincs;i++) for(u32 k=0;k<g_cs_incs[i].n;k++) syncpt_incr(g_cs_incs[i].id); g_cs_nincs=0; }

/* syncpoint incrs of a submit get held back (in order) while compute results are in flight */
static void cs_submit_done(u32 id, u32 n){
  extern void syncpt_incr(u32 id);
  if(g_cs_fl.fence && g_cs_nincs>=64){ cs_complete(1);
    /* still full after a timeout: release what is held rather than writing past g_cs_incs */
    if(g_cs_nincs>=64){ for(int i=0;i<g_cs_nincs;i++) for(u32 k=0;k<g_cs_incs[i].n;k++) syncpt_incr(g_cs_incs[i].id); g_cs_nincs=0; } }
  if(g_cs_fl.fence){ g_cs_incs[g_cs_nincs].id=id; g_cs_incs[g_cs_nincs].n=n; g_cs_nincs++; return; }
  for(u32 k=0;k<n;k++) syncpt_incr(id); }

/* an incr inside the stream means everything before it is done, so it has to go after any pending compute results */
void gpu_gl_stream_incr(u32 id){
  extern void syncpt_incr(u32 id);
  if(!t_gpu_thread){ syncpt_incr(id); return; }
  if(g_cs_npend) cs_kick_async();
  cs_submit_done(id,1); }

/* the stream is about to wait on a syncpoint: flush everything in flight */
void gpu_gl_before_syncwait(void){ if(t_gpu_thread && g_cs_fl.fence) cs_complete(1); }

static int cs_inflight(void){ return g_cs_fl.fence!=0; }

/* sync path: land everything pending/in flight in guest memory now */
void cs_flush_pending(void){
  cs_complete(1);
  if(!g_cs_npend){ g_cs_any=0; g_cs_lo=~0ull; g_cs_hi=0; return; }
  glMemoryBarrier(0x00000200 /* GL_BUFFER_UPDATE_BARRIER_BIT */); glBindBuffer(GL_COPY_READ_BUFFER,g_cs_stage);
  u8* tmp=cs_tmp(g_cs_stage_used); glGetBufferSubData(GL_COPY_READ_BUFFER,0,g_cs_stage_used,tmp);
  cs_apply(tmp,(const CsPend*)g_cs_pend,g_cs_npend);
  g_cs_npend=0; g_cs_stage_used=0; g_cs_any=0; g_cs_lo=~0ull; g_cs_hi=0; }

/* end of submit: pending copy goes in flight with a fence */
static void cs_kick_async(void){
  if(!g_cs_npend) return;
  static int sync=-1; if(sync<0) sync=getenv("SWR_CS_SYNC")!=NULL || !p_fs || !p_cws || !p_ds;
  if(sync){ cs_flush_pending(); return; }
  cs_complete(1);   /* one in flight at a time */
  glMemoryBarrier(0x00000200);
  GLuint b=g_cs_fl.buf; u32 c=g_cs_fl.cap;
  g_cs_fl.buf=g_cs_stage; g_cs_fl.cap=g_cs_stage_cap; g_cs_fl.used=g_cs_stage_used; g_cs_fl.n=g_cs_npend; memcpy(g_cs_fl.p,g_cs_pend,sizeof(CsPend)*g_cs_npend);
  g_cs_fl.fence=p_fs(GL_SYNC_GPU_COMMANDS_COMPLETE,0); glFlush();
  g_cs_stage=b; g_cs_stage_cap=c;   /* the other staging buffer (0 = create on next use) */
  g_cs_npend=0; g_cs_stage_used=0; g_cs_any=1; }

/* gmem_ptr/gmem_read on the GPU thread hit memory a compute wrote that hasn't landed yet -> pull it now */
void cs_mem_need(u64 a, u64 n){
  if(a>=g_cs_hi || a+n<=g_cs_lo) return;
  if(g_cs_busy || !t_gpu_thread) return;   /* GL from the GPU thread only */
  int hit=0; for(int i=0;i<g_cs_fl.n && !hit;i++) if(a<g_cs_fl.p[i].addr+g_cs_fl.p[i].size && g_cs_fl.p[i].addr<a+n) hit=1;
  for(int i=0;i<g_cs_npend && !hit;i++) if(a<g_cs_pend[i].addr+g_cs_pend[i].size && g_cs_pend[i].addr<a+n) hit=1;
  if(hit){ u64 t0=plat_ns(); cs_flush_pending(); st_cs_sync++; st_cs_sync_ns+=plat_ns()-t0; } }

static int cs_pending_overlaps(u64 a, u32 n){ for(int i=0;i<g_cs_npend;i++) if(a<g_cs_pend[i].addr+g_cs_pend[i].size && g_cs_pend[i].addr<a+n) return 1; return 0; }

/* newest overlapping result must cover all of [a,a+n) (pending is newer than in flight) */
static int cs_gpu_src(u64 a, u64 n, GLuint* buf, u32* off){
  static int off_=-1; if(off_<0) off_=getenv("SWR_CS_NO_GPUCOPY")!=NULL; if(off_) return 0;
  if(!t_gpu_thread || g_cs_busy || a>=g_cs_hi || a+n<=g_cs_lo || n==0) return 0;
  for(int i=g_cs_npend-1;i>=0;i--){ u64 pa=g_cs_pend[i].addr, pe=pa+g_cs_pend[i].size;
    if(a<pe && pa<a+n){ if(a>=pa && a+n<=pe && g_cs_stage){ *buf=g_cs_stage; *off=g_cs_pend[i].off+(u32)(a-pa); return 1; } return 0; } }
  if(g_cs_fl.fence) for(int i=g_cs_fl.n-1;i>=0;i--){ u64 pa=g_cs_fl.p[i].addr, pe=pa+g_cs_fl.p[i].size;
    if(a<pe && pa<a+n){ if(a>=pa && a+n<=pe && g_cs_fl.buf){ *buf=g_cs_fl.buf; *off=g_cs_fl.p[i].off+(u32)(a-pa); return 1; } return 0; } }
  return 0; }

/* format class for raw SUST.D / SULD.D: 0 f32, 1 f16, 2 unorm8, 3 unorm16 */
static int fmt_cls(GLenum f){ switch(f){ case GL_R16F: case GL_RG16F: case GL_RGBA16F: return 1;
  case GL_R8: case GL_RG8: case GL_RGBA8: case GL_SRGB8_ALPHA8: return 2; case GL_R16: case GL_RG16: case GL_RGBA16: return 3; default: return 0; } }

static GLenum img_fmt(GLenum f){ switch(f){ case GL_SRGB8_ALPHA8: return GL_RGBA8;
  case GL_RGBA32F: case GL_RGBA16F: case GL_RG32F: case GL_RG16F: case GL_R11F_G11F_B10F: case GL_R32F: case GL_R16F: case GL_RGBA32UI: case GL_RGBA16UI:
  case GL_RGB10_A2UI: case GL_RGBA8UI: case GL_RG32UI: case GL_RG16UI: case GL_RG8UI: case GL_R32UI: case GL_R16UI: case GL_R8UI: case GL_RGBA32I: case GL_RGBA16I:
  case GL_RGBA8I: case GL_RG32I: case GL_RG16I: case GL_RG8I: case GL_R32I: case GL_R16I: case GL_R8I: case GL_RGBA16: case GL_RGB10_A2: case GL_RGBA8: case GL_RG16:
  case GL_RG8: case GL_R16: case GL_R8: case GL_RGBA16_SNORM: case GL_RGBA8_SNORM: case GL_RG16_SNORM: case GL_RG8_SNORM: case GL_R16_SNORM: case GL_R8_SNORM: return f;
  default: return 0; } }
typedef struct CsProg { u64 key; GLuint prog; int failed; ShaderInfo si; struct CsProg* next; GLint loc[4]; int loc_ok; } CsProg;
static CsProg* g_csp[256]; static u64 st_cs_run, st_cs_fail;
static u8 g_cs_cb_[8][0x10000]; static u32 g_cs_cbn[8];

static int cs_cb_read(u32 idx, u32 off, u32* out){ if(idx<8 && off+4<=g_cs_cbn[idx]){ memcpy(out,g_cs_cb_[idx]+off,4); return 1; } return 0; }

void gpu_gl_dispatch(GpuChannel* ch, const u32* q, u64 code){
  if(getenv("SWR_NO_COMPUTE")) return;
  extern void r3d_pending_flush(void); r3d_pending_flush();
  u32* rc=ch->rcomp; u64 prog=code+q[8];
  u32 gx=q[12]&0x7FFFFFFF, gy=q[13]&0xFFFF, gz=q[13]>>16, lx=q[18]>>16, ly=q[19]&0xFFFF, lz=q[19]>>16, shm=q[17]&0x3FFFF;
  if(!gx || !gy || !gz || !lx || !ly || !lz) return;
  /* cbufs from the QMD */
  u64 cba[8]={0}; u32 cbs[8]={0};
  for(int i=0;i<8;i++) if((q[20]>>i)&1){ cba[i]=((u64)(q[30+2*i]&0xFF)<<32)|q[29+2*i]; u32 sz=(q[30+2*i]>>15)&0x1FFFF; if(!sz || sz>0x10000) sz=0x10000; cbs[i]=sz;
    g_cs_cbn[i]=sz; gmem_read(cba[i],g_cs_cb_[i],sz); } else g_cs_cbn[i]=0;
  static u8 buf[0x10000+80];
  memset(buf,0,80); gmem_read(prog,buf+80,0x10000);
  u64 key=fast_hash(buf+80,0x4000,0)^((u64)lx<<40)^((u64)ly<<48)^((u64)lz<<56)^((u64)shm<<20);
  CsProg* p=g_csp[key&255]; while(p && p->key!=key) p=p->next;
  if(!p){ p=calloc(1,sizeof *p); p->key=key; p->next=g_csp[key&255]; g_csp[key&255]=p;
    memset(&p->si,0,sizeof p->si); p->si.cs_local[0]=lx; p->si.cs_local[1]=ly; p->si.cs_local[2]=lz; p->si.shared_size=(int)shm;
    int (*old)(u32,u32,u32*)=g_shader_cb_read; g_shader_cb_read=cs_cb_read;
    char* src=shader_to_glsl(buf,sizeof buf,ST_CS,&p->si); g_shader_cb_read=old;
    if(!src || p->si.unsupported){ p->failed=1; LOG("compute %016llx: not translated (%d instructions, first %s)\n",(unsigned long long)key,p->si.unsupported,p->si.first_unsupported); }
    else { char err[4096]; GLuint sh=compile(GL_COMPUTE_SHADER,src,err,sizeof err);
      if(!sh){ p->failed=1; LOG("compute %016llx failed to compile: %.600s\n",(unsigned long long)key,err); }
      else { GLuint pr=glCreateProgram(); glAttachShader(pr,sh); glLinkProgram(pr); GLint ok=0; glGetProgramiv(pr,GL_LINK_STATUS,&ok); glDeleteShader(sh);
        if(!ok){ glGetProgramInfoLog(pr,sizeof err,NULL,err); LOG("compute %016llx failed to link: %.600s\n",(unsigned long long)key,err); glDeleteProgram(pr); p->failed=1; }
        else { p->prog=pr; static int said; if(said<12){ said++; LOG("compute %016llx ready: block %ux%ux%u, %d global buffers, %d textures\n",(unsigned long long)key,lx,ly,lz,p->si.ngbuf,p->si.ntex); } } } }
    if(src && getenv("SWR_DUMP_SHADERS")){ char pth[128]; snprintf(pth,sizeof pth,"shader_%016llx_cs.glsl",(unsigned long long)key); FILE* f=fopen(pth,"w"); if(f){ fputs(src,f); fclose(f); } }
    free(src); }
  if(p->failed || !p->prog){ st_cs_fail++; trace_note("COMPUTE %016llx (not translated)\n",(unsigned long long)key); return; }
  glUseProgram(p->prog); g_pipe_on=0;
  /* cbufs at binding 36+i (translator's 'other stages' range) */
  static GLuint ub[8]; for(int i=0;i<8;i++) if(p->si.cbuf_mask&(1u<<i)){ if(!ub[i]) glGenBuffers(1,&ub[i]);
      glBindBuffer(GL_UNIFORM_BUFFER,ub[i]); static u8 z[0x10000]; u32 n=g_cs_cbn[i];
      if(n){ if(n<0x10000){ memcpy(z,g_cs_cb_[i],n); memset(z+n,0,0x10000-n); glBufferData(GL_UNIFORM_BUFFER,0x10000,z,GL_STREAM_DRAW); } else glBufferData(GL_UNIFORM_BUFFER,0x10000,g_cs_cb_[i],GL_STREAM_DRAW); }
      else { memset(z,0,sizeof z); glBufferData(GL_UNIFORM_BUFFER,0x10000,z,GL_STREAM_DRAW); }
      glBindBufferRange(GL_UNIFORM_BUFFER,36+i,ub[i],0,0x10000); g_ub_bound[36+i]=0; }
  /* texture handles come from bank tex_cb_index (0x982) of the compute engine */
  GLint tcls[32]={0}, tbpp[32]={0}, icls[8]={0}, ibpp[8]={0}; int nimg_ok=0;
  { u64 tic_base=((u64)rc[0x55D]<<32)|rc[0x55E], tsc_base=((u64)rc[0x557]<<32)|rc[0x558]; u32 tcb=rc[0x982]&7;
    for(int i=0;i<p->si.ntex && i<32;i++){ ShTex* st=&p->si.tex[i]; u32 cb= st->bindless? (st->bl_cbuf&7) : tcb; u32 h=0; cs_cb_read(cb,st->cbuf_off*4,&h);
      if(st->bindless && st->bl2){ u32 h2=0; cs_cb_read(st->bl2_cbuf&7,st->bl2_off*4,&h2); h|=h2; }
      u32 tic[8], tsc[8]; gmem_read(tic_base+(h&0xFFFFF)*32ull,tic,32); gmem_read(tsc_base+((h>>20)&0xFFF)*32ull,tsc,32);
      GLenum want= st->type==2? GL_TEXTURE_3D : st->type==3? (st->array? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_CUBE_MAP) : st->array? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
      Tex* t=tex_from_tic(tic); GLuint id= t && t->target==want? t->id : (t && tex_view(t,want)? t->view : null_tex(want));
      glActiveTexture(GL_TEXTURE0+64+i); glBindTexture(want,id); glBindSampler(64+i,sampler_get(tsc));
      tcls[i]= t? fmt_cls(t->f.ifmt) : 0; tbpp[i]= t && t->f.bpp? (int)t->f.bpp : 4; }
    /* SUST targets */
    for(int i=0;i<p->si.nimg && i<8;i++){ u32 cb= p->si.img[i].bindless? (p->si.img[i].bl_cbuf&7) : tcb; u32 h=0; cs_cb_read(cb,p->si.img[i].cbuf_off*4,&h);
      u32 tic[8]; gmem_read(tic_base+(h&0xFFFFF)*32ull,tic,32); Tex* t=tex_from_tic(tic); GLenum f= t? img_fmt(t->f.ifmt) : 0;
      icls[i]= t? fmt_cls(t->f.ifmt) : 0; ibpp[i]= t && t->f.bpp? (int)t->f.bpp : 4;
      if(t && f){ int ty=p->si.img[i].type; glBindImageTexture(i,t->id,0,(ty==2||ty==4||ty==5)? GL_TRUE : GL_FALSE,0,GL_WRITE_ONLY,f); t->gpu_written=1; t->wgen++; nimg_ok++;
        static int said; if(said<8){ said++; LOG("compute: writes image %ux%ux%u at %llx (format %x)\n",t->w,t->h,t->d,(unsigned long long)t->gva,f); } }
      else { glBindImageTexture(i,0,0,GL_FALSE,0,GL_WRITE_ONLY,GL_R32F); static int said; if(said<4){ said++; LOG("compute: image %d without a usable texture (tic %x)\n",i,h); } } }
    { /* cache uniform locations per program; a glGet per dispatch stalls mesa_glthread */
      if(!p->loc_ok){ p->loc[0]=glGetUniformLocation(p->prog,"swr_ic"); p->loc[1]=glGetUniformLocation(p->prog,"swr_ib"); p->loc[2]=glGetUniformLocation(p->prog,"swr_tc"); p->loc[3]=glGetUniformLocation(p->prog,"swr_tb"); p->loc_ok=1; }
      if(p->loc[0]>=0) glUniform1iv(p->loc[0],8,icls); if(p->loc[1]>=0) glUniform1iv(p->loc[1],8,ibpp);
      if(p->loc[2]>=0) glUniform1iv(p->loc[2],32,tcls); if(p->loc[3]>=0) glUniform1iv(p->loc[3],32,tbpp); }
    mb_reset(); }
  /* ssbos */
  static GLuint sb[16]; u64 sa[16]; u32 ss[16];
  for(int i=0;i<p->si.ngbuf && i<16;i++){ if(!sb[i]) glGenBuffers(1,&sb[i]);
    u32 v[3]={0,0,0}; cs_cb_read(0,p->si.gbuf_cbuf_off[i],&v[0]); cs_cb_read(0,p->si.gbuf_cbuf_off[i]+4,&v[1]); cs_cb_read(0,p->si.gbuf_cbuf_off[i]+8,&v[2]);
    sa[i]=((u64)v[1]<<32)|v[0]; ss[i]=v[2]; if(!ss[i]) ss[i]=16; if(ss[i]>(16u<<20)) ss[i]=16u<<20; ss[i]=(ss[i]+3)&~3u;
    { GLuint gsb; u32 gso; if(sa[i] && g_cs_any && cs_gpu_src(sa[i],ss[i],&gsb,&gso)){
        glBindBuffer(GL_COPY_WRITE_BUFFER,sb[i]); glBufferData(GL_COPY_WRITE_BUFFER,ss[i],NULL,GL_DYNAMIC_COPY);
        glBindBuffer(GL_COPY_READ_BUFFER,gsb); glCopyBufferSubData(GL_COPY_READ_BUFFER,GL_COPY_WRITE_BUFFER,gso,0,ss[i]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER,i,sb[i]); st_cs_gpucopy++; continue; } }
    if(sa[i] && cs_pending_overlaps(sa[i],ss[i])){ u64 t0=plat_ns(); cs_flush_pending(); st_cs_sync++; st_cs_sync_ns+=plat_ns()-t0; }
    static u8* tmp; static u32 cap; if(cap<ss[i]){ tmp=realloc(tmp,ss[i]); cap=ss[i]; }
    if(sa[i]) gmem_read(sa[i],tmp,ss[i]); else memset(tmp,0,ss[i]);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER,sb[i]); glBufferData(GL_SHADER_STORAGE_BUFFER,ss[i],tmp,GL_DYNAMIC_COPY); glBindBufferBase(GL_SHADER_STORAGE_BUFFER,i,sb[i]); }
  glDispatchCompute(gx,gy,gz); st_cs_run++;
  if(nimg_ok) glMemoryBarrier(0x20|0x08|0x100|0x400|0x80);   /* images -> textures, copies, fbos */
  trace_note("COMPUTE %016llx grid %ux%ux%u block %ux%ux%u buffers %d (writes %x)\n",(unsigned long long)key,gx,gy,gz,lx,ly,lz,p->si.ngbuf,p->si.gbuf_wmask);
  if(p->si.gbuf_wmask){ glMemoryBarrier(0x00000200|0x00002000 /* BUFFER_UPDATE | SHADER_STORAGE */);
    for(int i=0;i<p->si.ngbuf && i<16;i++) if(((p->si.gbuf_wmask>>i)&1) && sa[i]){
      if(g_cs_npend>=256 || g_cs_stage_used+ss[i]>(32u<<20)) cs_flush_pending();
      u32 need=g_cs_stage_used+ss[i];
      if(!g_cs_stage || need>g_cs_stage_cap){ /* growing: read back what's pending first */
        cs_flush_pending(); u32 nc=g_cs_stage_cap? g_cs_stage_cap*2 : (1u<<20); while(nc<ss[i]) nc*=2;
        if(!g_cs_stage) glGenBuffers(1,&g_cs_stage); glBindBuffer(GL_COPY_WRITE_BUFFER,g_cs_stage); glBufferData(GL_COPY_WRITE_BUFFER,nc,NULL,GL_DYNAMIC_READ); g_cs_stage_cap=nc; }
      glBindBuffer(GL_COPY_READ_BUFFER,sb[i]); glBindBuffer(GL_COPY_WRITE_BUFFER,g_cs_stage);
      glCopyBufferSubData(GL_COPY_READ_BUFFER,GL_COPY_WRITE_BUFFER,0,g_cs_stage_used,ss[i]);
      g_cs_pend[g_cs_npend].addr=sa[i]; g_cs_pend[g_cs_npend].off=g_cs_stage_used; g_cs_pend[g_cs_npend].size=ss[i]; g_cs_npend++; g_cs_any=1;
      if(sa[i]<g_cs_lo) g_cs_lo=sa[i]; if(sa[i]+ss[i]>g_cs_hi) g_cs_hi=sa[i]+ss[i];
      g_cs_stage_used=(g_cs_stage_used+ss[i]+255)&~255u; }
    /* fresh SSBOs for the next dispatch so it doesn't wait on these */
    for(int i=0;i<p->si.ngbuf && i<16;i++) if((p->si.gbuf_wmask>>i)&1){ glDeleteBuffers(1,&sb[i]); sb[i]=0; } }
  glUseProgram(0); state_dirty(); }
