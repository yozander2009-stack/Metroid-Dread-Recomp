/* vp9video.inc - NVDEC VP9 support for host1x.c (only compiled when SWR_HAVE_VP9 is defined).

   The game plays its .webm videos by parsing the file itself and handing each VP9 frame to NVDEC.
   Rebuilding a VP9 stream from NVDEC's picture setup is a lot of work, so we do something simpler:
   the runtime remembers which .webm files the game read from the romfs, indexes the VP9 frames of
   the file, finds which frame the NVDEC job is (the job's bitstream is the tail of that frame) and
   decodes the file with libvpx. The decoded picture goes to the same buffers the H.264 path uses,
   so VIC copies it to the game's surface as usual. */
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

#define V9_TAIL 24            /* bytes at the end of a frame used to recognise it */
typedef struct { u32 off, size; u64 h; u8* pixels; u32 w,hgt,bytes; u64 cached_used; } V9F;
typedef struct {
  const char* path; u8* data; u64 dlen; V9F* fr; u32 nfr, cap,cache_count; int cur,dec_cur,last_visible; int ctx_ok; vpx_codec_ctx_t ctx; u64 used;
  void* sub;   /* a mod's video of another size, decoded in place of this one (#31) */
} V9Vid;
#define V9N 8           /* videos indexed at once; Dread plays up to 3 together and returns to older ones */
static V9Vid g_v9[V9N]; static u64 g_v9clk; static V9Vid* g_v9act;
static u32 g_v9_slen_off;     /* which offset of the picture setup holds the bitstream size (found at run time) */

/* Immutable presentation frames, bounded across all loaded videos. Keep the
   decoder cursor separate: serving an older picture must not rewind codec references. */
static u64 v9_cache_bytes,v9_cache_clock,v9_cache_hits,v9_decoder_calls;
#ifndef V9_CACHE_LIMIT
#define V9_CACHE_LIMIT (256ull<<20)
#endif
static int v9_cache_enabled(void){extern char g_exename[];return !getenv("SWR_NO_VP9_FRAME_CACHE")&&(getenv("SWR_VP9_FRAME_CACHE")||!strcmp(g_exename,"Metroid Dread"));}
static void v9_cache_drop(V9Vid*V,V9F*f){if(f->pixels){V->cache_count--;v9_cache_bytes-=f->bytes;free(f->pixels);f->pixels=NULL;f->bytes=0;f->cached_used=0;}}
static int v9_cache_space(u32 bytes){
  while(v9_cache_bytes+bytes>V9_CACHE_LIMIT){V9F*old=NULL;V9Vid*owner=NULL;
    for(int v=0;v<V9N;v++)for(u32 i=0;i<g_v9[v].nfr;i++){
      V9F*f=&g_v9[v].fr[i];if(!f->pixels||(int)i==g_v9[v].last_visible)continue;
      if(!old||f->cached_used<old->cached_used){old=f;owner=&g_v9[v];}
    }
    if(!old)return 0;v9_cache_drop(owner,old);
  }return 1;
}
static void v9_cache_store(V9Vid*V,u32 index,const u8*y,const u8*uv,u32 w,u32 h){
  if(!v9_cache_enabled()||!y||!uv||!w||!h)return;
  V9F*f=&V->fr[index];if(f->pixels){f->cached_used=++v9_cache_clock;return;}
  u32 yn=w*h,un=w*((h+1)/2),bytes=yn+un;if(!v9_cache_space(bytes))return;
  u8*p=malloc(bytes);if(!p)return;memcpy(p,y,yn);memcpy(p+yn,uv,un);
  f->pixels=p;f->w=w;f->hgt=h;f->bytes=bytes;f->cached_used=++v9_cache_clock;v9_cache_bytes+=bytes;V->cache_count++;
}
static int v9_cache_restore(V9F*f){
  if(!f->pixels)return 0;u32 yn=f->w*f->hgt,un=f->w*((f->hgt+1)/2);
  u8*new_y=g_fcap<yn?malloc(yn):NULL,*new_uv=g_fuv_cap<un?malloc(un+16):NULL;
  if((g_fcap<yn&&!new_y)||(g_fuv_cap<un&&!new_uv)){free(new_y);free(new_uv);return 0;}
  if(new_y){free(g_fy);g_fy=new_y;g_fcap=yn;}if(new_uv){free(g_fuv);g_fuv=new_uv;g_fuv_cap=un+16;}
  memcpy(g_fy,f->pixels,yn);memcpy(g_fuv,f->pixels+yn,un);g_fw=f->w;g_fh=f->hgt;
  f->cached_used=++v9_cache_clock;v9_cache_hits++;return 1;
}

/* NVDEC output surface -> the video that last decoded into it. Each stream owns its
   surfaces, so a job that cannot be matched by its bitstream (tiny frames) continues
   the video bound to its surface, never an unrelated one. */
typedef struct { u64 luma; V9Vid* V; } V9Bind;
static V9Bind g_v9bind[64]; static u32 g_v9bind_next;
static void v9_bind(u64 luma, V9Vid* V){ if(!luma) return; for(int i=0;i<64;i++) if(g_v9bind[i].luma==luma){ g_v9bind[i].V=V; return; }
  g_v9bind[g_v9bind_next++&63]=(V9Bind){luma,V}; }
static V9Vid* v9_bound(u64 luma){ if(!luma) return NULL; for(int i=0;i<64;i++) if(g_v9bind[i].luma==luma) return g_v9bind[i].V; return NULL; }
static void v9_unbind_video(V9Vid* V){ for(int i=0;i<64;i++) if(g_v9bind[i].V==V) g_v9bind[i].luma=0, g_v9bind[i].V=NULL; }

static u64 v9_hash(const u8* p, u32 n){ u64 h=1469598103934665603ull; for(u32 i=0;i<n;i++){ h^=p[i]; h*=1099511628211ull; } return h; }
static u64 v9_tailhash(const u8* f, u32 n){ u32 t=n<V9_TAIL? n : V9_TAIL; return v9_hash(f+n-t,t); }

/* ---- WebM (EBML) walking: only what is needed to list the frames of the VP9 track */
static int eb_id(const u8* p, const u8* e, u32* id, int* n){
  if(p>=e) return 0; u8 b=p[0]; int l=1; u8 m=0x80; while(l<=4 && !(b&m)){ l++; m>>=1; }
  if(l>4 || p+l>e) return 0; u32 v=0; for(int i=0;i<l;i++) v=(v<<8)|p[i]; *id=v; *n=l; return 1; }
static int eb_size(const u8* p, const u8* e, u64* sz, int* n){
  if(p>=e) return 0; u8 b=p[0]; int l=1; u8 m=0x80; while(l<=8 && !(b&m)){ l++; m>>=1; }
  if(l>8 || p+l>e) return 0; u64 v=b&(m-1); int ones=((b&(m-1))==(u8)(m-1));
  for(int i=1;i<l;i++){ v=(v<<8)|p[i]; if(p[i]!=0xFF) ones=0; } *sz= ones? ~0ull : v; *n=l; return 1; }

typedef struct { V9Vid* V; u64 vtrack, tnum; int tvp9; } V9P;

static void v9_addf(V9Vid* V, const u8* f, u32 n){
  if(!n) return;
  if(V->nfr>=V->cap){ u32 nc=V->cap? V->cap*2 : 1024; V9F* t=realloc(V->fr,(size_t)nc*sizeof(V9F)); if(!t) return; V->fr=t; V->cap=nc; }
  V9F* x=&V->fr[V->nfr++]; memset(x,0,sizeof *x); x->off=(u32)(f-V->data); x->size=n; x->h=v9_tailhash(f,n); }

/* a packet may be a "superframe" (hidden frames + a shown one, with an index at the end) */
static void v9_addpkt(V9Vid* V, const u8* p, u32 n){
  if(!n) return; u8 mk=p[n-1];
  if((mk&0xE0)==0xC0){
    u32 nf=(mk&7)+1, mag=((mk>>3)&3)+1, isz=2+mag*nf;
    if(n>=isz && p[n-isz]==mk){
      const u8* ix=p+n-isz+1; u32 off=0;
      for(u32 i=0;i<nf;i++){ u32 s=0; for(u32 k=0;k<mag;k++) s|=(u32)ix[i*mag+k]<<(8*k); if(!s || off+s>n-isz) break; v9_addf(V,p+off,s); off+=s; }
      return; } }
  v9_addf(V,p,n); }

static void v9_block(V9P* S, const u8* p, const u8* q){
  if(p>=q) return; u8 b=p[0]; int l=1; u8 m=0x80; while(l<=8 && !(b&m)){ l++; m>>=1; }
  if(l>8 || p+l+3>q) return; u64 t=b&(m-1); for(int i=1;i<l;i++) t=(t<<8)|p[i]; p+=l;
  if(S->vtrack && t!=S->vtrack) return;
  u8 flags=p[2]; p+=3; if(flags&0x06) return;        /* laced blocks are not used for video */
  v9_addpkt(S->V,p,(u32)(q-p)); }

static void v9_walk(V9P* S, const u8* p, const u8* e, int depth){
  if(depth>8) return;
  while(p<e){
    u32 id; int il; u64 sz; int sl;
    if(!eb_id(p,e,&id,&il)) return; p+=il; if(!eb_size(p,e,&sz,&sl)) return; p+=sl;
    const u8* q=(sz==~0ull || sz>(u64)(e-p))? e : p+sz;
    switch(id){
      case 0x18538067: case 0x1F43B675: case 0xA0: case 0x1654AE6B: v9_walk(S,p,q,depth+1); break;   /* Segment, Cluster, BlockGroup, Tracks */
      case 0xAE: S->tnum=0; S->tvp9=0; v9_walk(S,p,q,depth+1); if(S->tvp9 && !S->vtrack) S->vtrack=S->tnum; break;   /* TrackEntry */
      case 0xD7: { u64 v=0; for(const u8* t=p;t<q;t++) v=(v<<8)|*t; S->tnum=v; } break;                 /* TrackNumber */
      case 0x86: if(q-p>=5 && !memcmp(p,"V_VP9",5)) S->tvp9=1; break;                                   /* CodecID */
      case 0xA3: case 0xA1: v9_block(S,p,q); break;                                                       /* SimpleBlock, Block */
      default: break; }
    p=q; } }

#ifdef SWR_NATIVE_VIDEO_RETIRE
/* Detached pictures remain charged to the cache budget until physically freed.
   Only one retirement may be pending; further evictions use synchronous cleanup. */
typedef struct {u8*data;V9F*frames;u32 nframes;} V9Retired;
static PMutex v9_retire_mutex;static PCond v9_retire_cv;
static V9Retired*v9_retire_job;static int v9_retire_busy,v9_retire_state;
static void v9_retire_thread(void*arg){
  (void)arg;
  for(;;){
    pm_lock(&v9_retire_mutex);while(!v9_retire_job)pc_wait(&v9_retire_cv,&v9_retire_mutex,-1);
    V9Retired*j=v9_retire_job;v9_retire_job=NULL;pm_unlock(&v9_retire_mutex);
    for(u32 i=0;i<j->nframes;i++)if(j->frames[i].pixels){u32 bytes=j->frames[i].bytes;
      free(j->frames[i].pixels);pm_lock(&g_hlk);v9_cache_bytes-=bytes;pm_unlock(&g_hlk);
    }
    free(j->frames);free(j->data);free(j);
    pm_lock(&v9_retire_mutex);v9_retire_busy=0;pm_unlock(&v9_retire_mutex);
  }
}
static int v9_retire(V9Vid*V){
  extern int g_opt_video_async_retire;if(getenv("SWR_NO_ASYNC_VIDEO_FREE")||(!getenv("SWR_ASYNC_VIDEO_FREE")&&!g_opt_video_async_retire)||!V->cache_count)return 0;
  if(!v9_retire_state){pm_init(&v9_retire_mutex);pc_init(&v9_retire_cv);v9_retire_state=plat_thread(v9_retire_thread,NULL,256*1024)?-1:1;}
  if(v9_retire_state<0)return 0;
  pm_lock(&v9_retire_mutex);if(v9_retire_busy){pm_unlock(&v9_retire_mutex);return 0;}
  V9Retired*j=malloc(sizeof *j);if(!j){pm_unlock(&v9_retire_mutex);return 0;}
  j->data=V->data;j->frames=V->fr;j->nframes=V->nfr;
  v9_retire_job=j;v9_retire_busy=1;pc_signal(&v9_retire_cv);pm_unlock(&v9_retire_mutex);return 1;
}
#endif

static void v9_free(V9Vid* V){
  if(V->sub){ V9Vid* S=(V9Vid*)V->sub; V->sub=NULL; for(u32 i=0;i<S->nfr;i++) v9_cache_drop(S,&S->fr[i]); if(S->ctx_ok) vpx_codec_destroy(&S->ctx); free((void*)S->path); free(S->data); free(S->fr); free(S); }
  if(V->ctx_ok){ vpx_codec_destroy(&V->ctx); V->ctx_ok=0; }
#ifdef SWR_NATIVE_VIDEO_RETIRE
  if(v9_retire(V)){memset(V,0,sizeof *V);return;}
#endif
  for(u32 i=0;i<V->nfr;i++)v9_cache_drop(V,&V->fr[i]);
  free(V->data); free(V->fr); memset(V,0,sizeof *V); }

static int v9_read(V9Vid* V, const char* path){
  FILE* f=fopen(path,"rb"); if(!f){ LOG("video: cannot open %s\n",path); return 0; }
  fseek(f,0,SEEK_END); long len=ftell(f); fseek(f,0,SEEK_SET);
  if(len<64 || len>(300l<<20)){ fclose(f); return 0; }
  V->data=malloc((size_t)len); if(!V->data || fread(V->data,1,(size_t)len,f)!=(size_t)len){ fclose(f); free(V->data); V->data=NULL; return 0; } fclose(f);
  V->dlen=(u64)len;
  V9P S; memset(&S,0,sizeof S); S.V=V; v9_walk(&S,V->data,V->data+V->dlen,0);
  LOG("video: VP9 file %s: %u frames indexed (track %u)\n",path,V->nfr,(unsigned)S.vtrack);
  return 1; }
static V9Vid* v9_load(const char* path){
  u64 load_start=getenv("SWR_TRACE_VP9")?plat_ns():0;
  V9Vid* V=NULL; for(int i=0;i<V9N;i++){ if(!g_v9[i].path){ V=&g_v9[i]; break; } }
  if(!V){ V=&g_v9[0]; for(int i=1;i<V9N;i++) if(g_v9[i].used<V->used) V=&g_v9[i]; if(V==g_v9act) g_v9act=NULL; v9_unbind_video(V); v9_free(V); }
  V->path=path; V->cur=V->dec_cur=V->last_visible=-1; V->used=++g_v9clk;
  if(!v9_read(V,path)) return V;
  { const char* m=v9_sub_for(path); V9Vid* S= m? calloc(1,sizeof *S) : NULL;
    if(S){ S->path=strdup(m); S->cur=S->dec_cur=S->last_visible=-1;
      if(S->path && v9_read(S,S->path) && S->nfr){ V->sub=S; LOG("video: %s plays the mod's %s (%u frames for the game's %u)\n",path,m,S->nfr,V->nfr); }
      else { free((void*)S->path); free(S->data); free(S->fr); free(S); } } }
  if(load_start)LOG("vp9 load: %s %.3f ms for %llu bytes\n",path,(plat_ns()-load_start)/1e6,(unsigned long long)V->dlen);
  return V; }

/* ---- libvpx */
static u64 v9_t_reset, v9_t_dec, v9_t_keep, v9_t_store;   /* SWR_TRACE_VP9: ns per step, summed over a job */
/* decoder threads: 2. MEASURED 2026-10-22 (3 OFF/ON pairs, owner's setup, vp9off2/vp9on2): 8 threads decoded no faster
   (mean 1.9 ms per job, first frames 9.5-15 ms both ways; the videos give libvpx little to split) and 76 title-video
   pictures came out different (same setting: 0 differences run to run). Row multithreading (VP9D_SET_ROW_MT) froze the
   boot video in 4 of 4 runs. Both dead ends. SWR_VP9_THREADS=N is for tests only */
static int v9_threads(void){
  static int n; if(n) return n;
  const char* e=getenv("SWR_VP9_THREADS"); n=e? atoi(e) : 2;
  if(n<1) n=1; if(n>16) n=16;
  return n; }
/* SWR_VP9_PIC_HASH=file: a hash of every decoded picture (path, frame, hash), to compare decoder settings */
static void v9_pic_hash(const V9Vid* V, u32 i){
  static int on=-1; static FILE* f; if(on<0){ const char* e=getenv("SWR_VP9_PIC_HASH"); on=e && *e && (f=fopen(e,"w"))!=NULL; }
  if(!on) return;
  u64 h=1469598103934665603ull; u32 n=g_fw*g_fh;
  for(u32 k=0;k<n;k++) h=(h^g_fy[k])*1099511628211ull;
  for(u32 k=0;k<n/2;k++) h=(h^g_fuv[k])*1099511628211ull;
  fprintf(f,"%s,%u,%016llx\n",V->path,i,(unsigned long long)h); fflush(f); }
static int v9_dec_reset_(V9Vid* V);
static int v9_dec_reset(V9Vid* V){ u64 t0=plat_ns(); int r=v9_dec_reset_(V); v9_t_reset+=plat_ns()-t0; return r; }
static int v9_dec_reset_(V9Vid* V){
  V->dec_cur=V->last_visible=-1;
  if(V->ctx_ok){ vpx_codec_destroy(&V->ctx); V->ctx_ok=0; }
  vpx_codec_dec_cfg_t cfg; memset(&cfg,0,sizeof cfg); cfg.threads=(unsigned)v9_threads();
  if(vpx_codec_dec_init(&V->ctx,vpx_codec_vp9_dx(),&cfg,0)){ static int said; if(said<3){ said++; LOG("video: libvpx VP9 decoder could not be created\n"); } return 0; }
  { static int said; if(!said){ said=1; LOG("video: VP9 decoder, %d threads\n",v9_threads()); } }
  V->ctx_ok=1; return 1; }

static int v9_is_key(const u8* f, u32 n){
  if(!n) return 0; u8 b=f[0]; if((b&0xC0)!=0x80) return 0;
  int prof=((b>>5)&1)|(((b>>4)&1)<<1);
  return prof==3? (!(b&0x04) && !(b&0x02)) : (!(b&0x08) && !(b&0x04)); }

static const vpx_image_t* v9_feed(V9Vid* V, u32 i){
  v9_decoder_calls++;
  u64 t0=plat_ns(); int bad=vpx_codec_decode(&V->ctx,V->data+V->fr[i].off,V->fr[i].size,NULL,0); v9_t_dec+=plat_ns()-t0;
  if(bad) return NULL;
  vpx_codec_iter_t it=NULL; const vpx_image_t *im, *img=NULL; while((im=vpx_codec_get_frame(&V->ctx,&it))) img=im; return img; }

static void v9_keep(const vpx_image_t* im){
  if(!im || im->fmt!=VPX_IMG_FMT_I420) return;
  u32 w=im->d_w, h=im->d_h; if(!w || !h || w>4096 || h>4096) return;
  u32 need=w*h; if(g_fcap<need){ free(g_fy); free(g_fuv); g_fy=malloc(need); g_fuv=malloc(need/2+w+16); g_fuv_cap=need/2+w+16; g_fcap=need; }
  for(u32 y=0;y<h;y++) memcpy(g_fy+(u64)y*w,im->planes[0]+(u64)y*im->stride[0],w);
  u32 cw=(w+1)/2, ch=(h+1)/2;
  for(u32 y=0;y<ch;y++){ u8* d=g_fuv+(u64)y*w; const u8* u=im->planes[1]+(u64)y*im->stride[1]; const u8* v=im->planes[2]+(u64)y*im->stride[2];
    for(u32 x=0;x<cw && 2*x+1<w+1;x++){ d[2*x]=u[x]; if(2*x+1<w) d[2*x+1]=v[x]; } }
  g_fw=w; g_fh=h; g_vframes++; }

/* decode frame n of the video (decoding from the previous key frame if we are not simply going forward) */
static int v9_show(V9Vid* V, u32 n){
  if(n>=V->nfr) return 0;
  if(V->sub){ V9Vid* S=(V9Vid*)V->sub; u32 m=(u32)(((u64)n*S->nfr+V->nfr/2)/V->nfr); if(m>=S->nfr) m=S->nfr-1;   /* the same position in the mod's video */
    int r=v9_show(S,m); V->cur=V->dec_cur=(int)n; V->used=++g_v9clk; g_v9act=V; return r; }
  /* A partial cache may replay backward requests; forward playback still advances codec references so a later cache hole cannot accumulate decode work. */
  if(v9_cache_enabled()&&((V->dec_cur>=0&&n<=(u32)V->dec_cur)||V->cache_count==V->nfr)&&v9_cache_restore(&V->fr[n])){V->cur=(int)n;V->used=++g_v9clk;g_v9act=V;return 1;}
  u32 from=n;
  if(V->dec_cur==(int)n && V->ctx_ok){ V->used=++g_v9clk; g_v9act=V; return 0; }
  if(V->dec_cur>=0 && (u32)V->dec_cur<n && V->ctx_ok) from=(u32)V->dec_cur+1;
  else { from=n; while(from>0 && !v9_is_key(V->data+V->fr[from].off,V->fr[from].size)) from--; if(!v9_dec_reset(V)) return 0; }
  if(!V->ctx_ok && !v9_dec_reset(V)) return 0;
  const vpx_image_t* img=NULL;int visible=0;
  for(u32 i=from;i<=n;i++){
    img=v9_feed(V,i);
    if(img){u64 t0=plat_ns();v9_keep(img);u64 t1=plat_ns();v9_t_keep+=t1-t0;visible=1;if(img->fmt==VPX_IMG_FMT_I420&&img->d_w&&img->d_h&&img->d_w<=4096&&img->d_h<=4096){V->last_visible=(int)i;v9_cache_store(V,i,g_fy,g_fuv,g_fw,g_fh);v9_t_store+=plat_ns()-t1;v9_pic_hash(V,i);}else V->last_visible=-1;}
    else if(V->ctx.err==VPX_CODEC_OK&&V->last_visible>=0){V9F*last=&V->fr[V->last_visible];if(last->pixels)v9_cache_store(V,i,last->pixels,last->pixels+last->w*last->hgt,last->w,last->hgt);}
  }
  if(!visible&&v9_cache_enabled()&&v9_cache_restore(&V->fr[n]))visible=1;
  V->cur=V->dec_cur=(int)n; V->used=++g_v9clk; g_v9act=V;return visible;
  }

static int v9_find(V9Vid* V, u64 h, int want){
  int best=-1; long bd=0x7fffffff;
  for(u32 i=0;i<V->nfr;i++) if(V->fr[i].h==h){ long d=(long)i-want; if(d<0) d=-d; if(d<bd){ bd=d; best=(int)i; } }
  return best; }

static int vp9_job(u32* r, u32 codec){
  /* no .webm was ever read: nothing to do here (H.264 games never get past this line) */
  { int any=g_v9_recent[0]!=NULL; for(int i=0;i<V9N && !any;i++) any=g_v9[i].path!=NULL; if(!any) return 0; }
  int job_trace=getenv("SWR_TRACE_VP9")!=NULL;u64 job_start=job_trace?plat_ns():0;
  if(job_trace){static u64 first_job,delay;if(!first_job){first_job=job_start;const char*e=getenv("SWR_TRACE_VP9_DELAY_MS");int ms=e?atoi(e):0;if(ms>0&&ms<=600000)delay=(u64)ms*1000000ull;}if(job_start-first_job<delay)job_trace=0;}
  static u32 njobs; njobs++;
  u64 ps_iova=(u64)r[0x404/4]<<8, bs_iova=(u64)r[0x408/4]<<8;
  u64 job_luma=(u64)r[0x43c/4]<<8; if(!job_luma) job_luma=(u64)r[0x428/4]<<8;   /* same key decoded_surface_save uses */
  u8* ps=iova_ptr(ps_iova,0x200); if(!ps) return 0;
  int matched=0;
  V9Vid* V=NULL; int N=-1; u32 slen=0;
  /* the bitstream size sits at a fixed offset of the picture setup; try the likely ones until a frame matches */
  static const u32 offs[2]={0x30,0x48};
  for(int oi=0;oi<2 && N<0;oi++){
    u32 o=offs[(oi+(g_v9_slen_off==offs[1]))&1]; u32 L=rd32(ps,o); if(L<V9_TAIL || L>(16u<<20)) continue;
    u8* bs=iova_ptr(bs_iova,L); if(!bs) continue; u64 h=v9_tailhash(bs,L);
    { V9Vid* B=v9_bound(job_luma); if(B && B->path){ int k=v9_find(B,h,B->cur+1); if(k>=0){ V=B; N=k; } } }
    for(int i=0;i<V9N && N<0;i++) if(g_v9[i].path){ int want=g_v9[i].cur+1; int k=v9_find(&g_v9[i],h,want); if(k>=0){ V=&g_v9[i]; N=k; } }
    /* not in a loaded video: index known .webm files that are not loaded, most recent first, until one matches.
       host1x_ioctl already holds g_hlk throughout vp9_job. */
    { static u64 miss[256]; static u32 nmiss; int known_miss=0; for(int m=0;m<256;m++) if(miss[m]==h){ known_miss=1; break; }
      for(int k=0;k<V9_FILES && N<0 && !known_miss;k++){ const char* p=g_v9_recent[k]; if(!p) break; int have=0; for(int i=0;i<V9N;i++) if(g_v9[i].path==p) have=1; if(have) continue;
        V9Vid* L=v9_load(p); if(L && L->path==p){ int f=v9_find(L,h,0); if(f>=0){ V=L; N=f; } } }
      if(N<0 && !known_miss) miss[nmiss++&255]=h; }   /* a bitstream no known file contains: search once, not on every job */
    if(N>=0){ slen=L; g_v9_slen_off=o; matched=1; } }
  if(N<0 && codec==9){ V9Vid* B=v9_bound(job_luma);   /* no match (tiny frame): the next frame of the video that owns this surface */
    if(B && B->path && B->nfr){ matched=1; V=B; N=(V->cur+1<(int)V->nfr)? V->cur+1 : 0; }
    else { matched=1; V=NULL; N=-1; } }   /* unknown stream: consume the job, show nothing rather than another video's frame */
  { static int said; if(said<6){ said++; LOG("video: NVDEC job %u codec %u: bitstream %u bytes -> %s frame %d\n",njobs,codec,slen,V? V->path : "(no video)",N); 
      if(said<=2){ char b[400]; int n=0; for(int i=0;i<32;i++) n+=snprintf(b+n,sizeof b-n,"%08x ",rd32(ps,i*4)); LOG("video: picture setup words: %s\n",b); } } }
  if(!matched) return 0;          /* not a VP9 frame of a known video: let the H.264 path have it */
  if(V && N>=0) v9_bind(job_luma,V);
  if(V && N>=0){
    v9_t_reset=v9_t_dec=v9_t_keep=v9_t_store=0;
    int trace=job_trace,previous=V->dec_cur;u32 decoded=g_vframes;u64 before=trace?plat_ns():0,hits=v9_cache_hits,calls=v9_decoder_calls;
    int visible=v9_show(V,(u32)N);g_pic_sub=V->sub!=NULL;decoded_surface_save(r,(u64)(uintptr_t)V->path,(u32)N,!visible);
    {static int checked;static FILE*csv;if(!checked){checked=1;const char*e=getenv("SWR_VP9_JOB_CSV");if(e&&*e){csv=fopen(e,"w");if(csv)fprintf(csv,"ns,job,path,previous,requested,bitstream_bytes,packet_bytes,full_match,visible,fed,elapsed_ms\n");}}
      if(csv){V9F*f=&V->fr[N];u8*b=slen?iova_ptr(bs_iova,slen):NULL;int exact=b&&slen==f->size&&!memcmp(b,V->data+f->off,slen);
        fprintf(csv,"%llu,%u,%s,%d,%d,%u,%u,%d,%d,%llu,%.6f\n",(unsigned long long)plat_ns(),njobs,V->path,previous,N,slen,f->size,exact,visible,(unsigned long long)(v9_decoder_calls-calls),(plat_ns()-before)/1e6);if(njobs%60==0)fflush(csv);}}

    if(trace){u64 elapsed=plat_ns()-before;static u32 logs,slow;int is_slow=elapsed>5000000ull||plat_ns()-job_start>5000000ull;
      if((is_slow? slow++<400 : logs++<4))LOG("vp9 work: %s previous=%d requested=%d decoded=%u fed=%llu cache_hit=%d cache_MB=%.1f elapsed=%.3f ms total_job=%.3f ms (decoder setup %.2f, decode %.2f, copy %.2f, cache store %.2f)\n",V->path,previous,N,g_vframes-decoded,(unsigned long long)(v9_decoder_calls-calls),v9_cache_hits!=hits,v9_cache_bytes/1048576.0,elapsed/1e6,(plat_ns()-job_start)/1e6,v9_t_reset/1e6,v9_t_dec/1e6,v9_t_keep/1e6,v9_t_store/1e6);}
  }
  { u64 st_iova=(u64)r[0x424/4]<<8; u8* sp= st_iova? iova_ptr(st_iova,16) : NULL;
    if(sp){ u32 mbs= g_fw? ((g_fw+15)/16)*((g_fh+15)/16) : 1; u32 v[4]={ mbs,0,100000,0 }; memcpy(sp,v,16); } }
  if(njobs%120==0) LOG("video: %u NVDEC VP9 jobs so far, %u pictures decoded\n",njobs,g_vframes);
  return 1; }
