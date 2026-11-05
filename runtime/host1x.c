/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* nvhost-nvdec / nvhost-vic (host1x channels).
   We parse the host1x stream, decode NVDEC H.264 jobs with OpenH264 (SPS/PPS get rebuilt from the
   picture setup the game fills in) and VIC jobs copy the last decoded picture into the output surface.
   Struct layouts / method offsets come from NVIDIA's open-gpu-doc (nvdec_drv.h, clc5b0.h, VIC). */
#include "rt.h"
#ifndef SWR_NO_H264
#include "wels/codec_api.h"
#endif

u64 nvmap_addr(u32 h); u64 nvmap_addr_by_id(u32 id);
u32 syncpt_read(u32 id); void syncpt_incr(u32 id);

/* ------------------------------------------------------------ IOVA (what the video engines see through the SMMU) */
typedef struct { u32 iova, size; u64 addr; u32 h,refs; } IoMap;
static IoMap g_iom[1024]; static int g_niom;  static PMutex g_hlk; static int g_hinit;

static void h_init(void){ if(!g_hinit){ pm_init(&g_hlk); g_hinit=1; } }

/* romfs hook (svc_fs.c): remember the .webm files the game reads, newest first. VP9 video uses them (vp9video.inc) */
/* Every .webm the game has opened, most recent first. Dread opens each video once
   (the title videos at boot) and replays it later from memory, so the list must not
   forget files: a replayed video that cannot be matched to its file used to be shown
   as frames of whatever video played last (e.g. the Game Over logo on save select). */
#define V9_FILES 64
static const char* g_v9_recent[V9_FILES];
void vp9_note_file(const char* path){
  if(g_v9_recent[0]==path) return;
  size_t n=strlen(path); if(n<5 || strcmp(path+n-5,".webm")) return;
  h_init(); pm_lock(&g_hlk);
  int k=0; for(;k<V9_FILES-1;k++) if(g_v9_recent[k]==path || !g_v9_recent[k]) break;
  for(;k>0;k--) g_v9_recent[k]=g_v9_recent[k-1];
  g_v9_recent[0]=path; pm_unlock(&g_hlk);
  if(getenv("SWR_TRACE_VP9")) LOG("vp9 note: game opened %s\n",path); }

/* Mod videos at another size than the game's (GitHub #31). The game's own decoder is set up for the size it ships with
   and never starts on a different one (the game then waits forever or calls through NULL). So the game keeps reading
   its own file (svc_fs.c overlay_scan) and the runtime decodes the mod's file instead, frame for frame by position,
   scaled into the game's picture (vic_execute) */
static int webm_walk_size(const u8* p, const u8* e, u32* w, u32* h, int depth){
  while(p<e && depth<6){
    u8 b=p[0]; int il=1; u8 m=0x80; while(il<=4 && !(b&m)){ il++; m>>=1; } if(il>4 || p+il>e) return 0;
    u32 id=0; for(int i=0;i<il;i++) id=(id<<8)|p[i]; p+=il; if(p>=e) return 0;
    b=p[0]; int sl=1; m=0x80; while(sl<=8 && !(b&m)){ sl++; m>>=1; } if(sl>8 || p+sl>e) return 0;
    u64 sz=b&(m-1); for(int i=1;i<sl;i++) sz=(sz<<8)|p[i]; p+=sl;
    const u8* q= sz>(u64)(e-p)? e : p+sz;
    if(id==0x18538067 || id==0x1654AE6B || id==0xAE || id==0xE0){ if(webm_walk_size(p,q,w,h,depth+1)) return 1; }   /* Segment, Tracks, TrackEntry, Video */
    else if(id==0xB0 || id==0xBA){ u32 v=0; for(const u8* t=p;t<q;t++) v=(v<<8)|*t; if(id==0xB0) *w=v; else *h=v; if(*w && *h) return 1; }
    else if(id==0x1F43B675) return 0;   /* first Cluster: no track header before it */
    p=q; }
  return 0; }
int vp9_webm_size(const char* path, u32* w, u32* h){
  *w=*h=0; FILE* f=fopen(path,"rb"); if(!f) return 0;
  static u8 buf[65536]; size_t n=fread(buf,1,sizeof buf,f); fclose(f);
  return webm_walk_size(buf,buf+n,w,h,0); }
#define V9_SUBS 64
static struct { char* game; char* mod; } g_v9sub[V9_SUBS];
void vp9_substitute(const char* game_path, const char* mod_path){
  for(int i=0;i<V9_SUBS;i++){ if(g_v9sub[i].game && !strcmp(g_v9sub[i].game,game_path)){ free(g_v9sub[i].mod); g_v9sub[i].mod=strdup(mod_path); return; }
    if(!g_v9sub[i].game){ g_v9sub[i].game=strdup(game_path); g_v9sub[i].mod=strdup(mod_path); return; } } }
static int g_pic_sub;   /* the last decoded picture is a substitute's */
static const char* v9_sub_for(const char* game_path){
  for(int i=0;i<V9_SUBS && g_v9sub[i].game;i++) if(!strcmp(g_v9sub[i].game,game_path)) return g_v9sub[i].mod; return NULL; }

static u32 iova_map(u32 h){
  u64 a=nvmap_addr(h); if(!a) a=nvmap_addr_by_id(h); if(!a) return 0;
  int slot=-1;
  for(int i=0;i<g_niom;i++){if(g_iom[i].refs&&g_iom[i].h==h&&g_iom[i].addr==a){g_iom[i].refs++;return g_iom[i].iova;}if(!g_iom[i].refs&&slot<0)slot=i;}
  extern u32 nvmap_size(u32 h);u32 sz=nvmap_size(h);if(!sz)sz=0x100000;
  if(slot<0){if(g_niom>=1024)return 0;slot=g_niom++;}
  /* Reclaim gaps instead of wrapping a monotonic 32-bit address counter. */
  u64 span=((u64)sz+0xffff)&~0xffffull,base=0x10000000ull;int again;
  do{again=0;for(int i=0;i<g_niom;i++)if(g_iom[i].refs){u64 lo=g_iom[i].iova,hi=lo+(((u64)g_iom[i].size+0xffff)&~0xffffull)+0x10000;
    if(base<hi&&base+span+0x10000>lo){base=hi;again=1;break;}}}while(again&&base+span<=0xffff0000ull);
  if(base+span>0xffff0000ull)return 0;
  g_iom[slot]=(IoMap){(u32)base,sz,a,h,1};return (u32)base;
}
static void iova_unmap(u32 h){
  for(int i=0;i<g_niom;i++)if(g_iom[i].refs&&g_iom[i].h==h){g_iom[i].refs--;return;}
}

/* 40-bit iova (engines get it >>8) -> host pointer */
static u8* iova_ptr(u64 iova, u64 len){
  for(int i=0;i<g_niom;i++){ IoMap* m=&g_iom[i]; if(m->refs && iova>=m->iova && len<=m->size && iova+len>=iova && iova+len<=(u64)m->iova+m->size) return GP(m->addr+(iova-m->iova)); }
  return NULL; }

/* Host-written video surfaces: renderer checks revisions instead of texture polling.
   host1x_ioctl serializes writers. Atomic fields keep renderer reads lock-free,
   avoiding a lock-order dependency between the renderer and video services. */
typedef struct { u64 cpu, size, gen; } VideoWrite;
static VideoWrite g_video_writes[16]; static u32 g_video_slot;
static u64 g_video_generation;
u64 video_write_generation(void){ return __atomic_load_n(&g_video_generation,__ATOMIC_ACQUIRE); }
static void video_written(u8* p,u64 size){
  u64 cpu=(u64)(uintptr_t)p; u32 k=0;
  for(;k<16;k++) if(__atomic_load_n(&g_video_writes[k].cpu,__ATOMIC_RELAXED)==cpu) break;
  if(k==16) k=g_video_slot++%16;
  u64 gen=video_write_generation()+1;
  __atomic_store_n(&g_video_writes[k].cpu,cpu,__ATOMIC_RELAXED);
  __atomic_store_n(&g_video_writes[k].size,size,__ATOMIC_RELAXED);
  __atomic_store_n(&g_video_writes[k].gen,gen,__ATOMIC_RELEASE);
  __atomic_store_n(&g_video_generation,gen,__ATOMIC_RELEASE);
}
u64 video_surface_generation(const u8* p,u64 size){
  if(!p || !size) return 0; u64 cpu=(u64)(uintptr_t)p, latest=0;
  for(u32 k=0;k<16;k++){
    u64 gen=__atomic_load_n(&g_video_writes[k].gen,__ATOMIC_ACQUIRE);
    u64 a=__atomic_load_n(&g_video_writes[k].cpu,__ATOMIC_RELAXED);
    u64 n=__atomic_load_n(&g_video_writes[k].size,__ATOMIC_RELAXED);
    if(gen && a<cpu+size && cpu<a+n && gen>latest) latest=gen;
  }
  return latest;
}

/* ------------------------------------------------------------ H.264: SPS/PPS rebuilt from nvdec_h264_pic_s */
typedef struct { u8* p; u32 cap, bytes; u32 acc; int nb; } BW;

static void bw_bit(BW* w, u32 b){ w->acc=(w->acc<<1)|(b&1); if(++w->nb==8){ if(w->bytes<w->cap) w->p[w->bytes++]=(u8)w->acc; w->acc=0; w->nb=0; } }

static void bw_u(BW* w, u32 v, int n){ for(int i=n-1;i>=0;i--) bw_bit(w,(v>>i)&1); }

static void bw_ue(BW* w, u32 v){ u32 x=v+1; int n=0; while((x>>n)>1) n++; for(int i=0;i<n;i++) bw_bit(w,0); bw_u(w,x,n+1); }

static void bw_se(BW* w, i32 v){ bw_ue(w, v>0? (u32)(2*v-1) : (u32)(-2*v)); }

static void bw_trailing(BW* w){ bw_bit(w,1); while(w->nb) bw_bit(w,0); }

/* start code + emulation prevention bytes */
static u32 nal_out(u8* dst, u32 cap, u8 hdr, const u8* rbsp, u32 n){
  u32 o=0; if(cap<5) return 0; dst[o++]=0; dst[o++]=0; dst[o++]=0; dst[o++]=1; dst[o++]=hdr; int z=0;
  for(u32 i=0;i<n && o+2<cap;i++){ if(z>=2 && rbsp[i]<=3){ dst[o++]=3; z=0; } dst[o++]=rbsp[i]; z= rbsp[i]==0? z+1 : 0; }
  return o; }

static u32 rd32(const u8* p, u32 off){ u32 v; memcpy(&v,p+off,4); return v; }

static i32 sext(u32 v, int bits){ return (i32)(v<<(32-bits))>>(32-bits); }
static const u8 zz4[16]={0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15};
static const u8 zz8[64]={0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
  35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

static void scaling_list(BW* w, const u8* raster, int n, const u8* zz){
  int last=8; for(int j=0;j<n;j++){ int v=raster[zz[j]]; int d=v-last; if(d>127) d-=256; if(d<-128) d+=256; bw_se(w,d); last=v; } }

static u32 build_headers(const u8* ps, u8* out, u32 cap){
  u8 rb[1024]; BW w; u32 o=0;
  u32 log2_poc=rd32(ps,0x58), dpoc0=rd32(ps,0x5C), fmo=rd32(ps,0x60), wmb=rd32(ps,0x64), hmb=rd32(ps,0x68);
  u32 ecm=rd32(ps,0x70), pop=rd32(ps,0x74), l0=rd32(ps,0x78), l1=rd32(ps,0x7C), dfc=rd32(ps,0x80), rpc=rd32(ps,0x84), t8=rd32(ps,0x88);
  u32 b0=rd32(ps,0xB0), b1=rd32(ps,0xB4), bl=rd32(ps,0x2D0);
  u32 mbaff=b0&1, d8=(b0>>1)&1, wp=(b0>>2)&1, cip=(b0>>3)&1, l2fn=(b0>>8)&15, cfi=(b0>>12)&3, poct=(b0>>14)&3;
  i32 qp26=sext((b0>>16)&63,6), cqo=sext((b0>>22)&31,5), cqo2=sext((b0>>27)&31,5);
  u32 wbi=b1&3, qpz=(bl>>1)&1;
  /* SPS, High profile so the PPS may use 8x8 transform / scaling lists */
  memset(&w,0,sizeof w); w.p=rb; w.cap=sizeof rb;
  bw_u(&w,100,8); bw_u(&w,0,8); bw_u(&w,51,8); bw_ue(&w,0);
  bw_ue(&w,cfi? cfi : 1); bw_ue(&w,0); bw_ue(&w,0); bw_u(&w,qpz,1); bw_u(&w,0,1);
  bw_ue(&w,l2fn); bw_ue(&w,poct);
  if(poct==0) bw_ue(&w,log2_poc);
  else if(poct==1){ bw_u(&w,dpoc0,1); bw_se(&w,0); bw_se(&w,0); bw_ue(&w,0); }
  bw_ue(&w,16); bw_u(&w,0,1);
  bw_ue(&w,wmb? wmb-1 : 0); bw_ue(&w, fmo? (hmb? hmb-1 : 0) : (hmb/2? hmb/2-1 : 0));
  bw_u(&w,fmo,1); if(!fmo) bw_u(&w,mbaff,1);
  bw_u(&w,d8,1); bw_u(&w,0,1); bw_u(&w,0,1); bw_trailing(&w);
  o+=nal_out(out+o,cap-o,0x67,rb,w.bytes);
  /* PPS, scaling matrices taken from the pic setup */
  memset(&w,0,sizeof w); w.p=rb; w.cap=sizeof rb;
  bw_ue(&w,0); bw_ue(&w,0); bw_u(&w,ecm,1); bw_u(&w,pop,1); bw_ue(&w,0);
  bw_ue(&w,l0); bw_ue(&w,l1); bw_u(&w,wp,1); bw_u(&w,wbi,2); bw_se(&w,qp26); bw_se(&w,0); bw_se(&w,cqo);
  bw_u(&w,dfc,1); bw_u(&w,cip,1); bw_u(&w,rpc,1);
  bw_u(&w,t8,1); bw_u(&w,1,1);
  for(int i=0;i<6;i++){ bw_u(&w,1,1); scaling_list(&w,ps+0x1C0+i*16,16,zz4); }
  if(t8) for(int i=0;i<2;i++){ bw_u(&w,1,1); scaling_list(&w,ps+0x220+i*64,64,zz8); }
  bw_se(&w,cqo2); bw_trailing(&w);
  o+=nal_out(out+o,cap-o,0x68,rb,w.bytes);
  return o; }

/* ------------------------------------------------------------ decoder and the last picture */
#ifndef SWR_NO_H264
static ISVCDecoder* g_dec;
#endif
static int g_dec_fail;
static u8* g_fy; static u8* g_fuv; static u32 g_fw, g_fh, g_fcap, g_fuv_cap; static u32 g_vframes;
/* NVDEC current-picture surfaces are addressed by VIC's input plane offsets.
   Keep independent pictures instead of converting whichever stream decoded last. */
typedef struct {u64 luma,display,used,owner;u8*y,*uv;u32 w,h,cap,frame;} DecodedSurface;
static DecodedSurface decoded_surfaces[32];static u64 decoded_clock;
static void decoded_surface_save(u32*r,u64 owner,u32 frame,int cached){
  u64 luma=(u64)r[0x43c/4]<<8,display=(u64)r[0x428/4]<<8;
  if(!luma)luma=display;if(!luma||!g_fw||!g_fh||!g_fy||!g_fuv)return;
  u8*sy=g_fy,*suv=g_fuv;u32 w=g_fw,h=g_fh;if(cached){DecodedSurface*prior=NULL;for(int i=0;i<32;i++)if(decoded_surfaces[i].used&&decoded_surfaces[i].owner==owner){if(decoded_surfaces[i].frame==frame){prior=&decoded_surfaces[i];break;}if(!prior||decoded_surfaces[i].used>prior->used)prior=&decoded_surfaces[i];}if(!prior)return;sy=prior->y;suv=prior->uv;w=prior->w;h=prior->h;}
  int k=-1;for(int i=0;i<32;i++)if(decoded_surfaces[i].luma==luma){k=i;break;}
  if(k<0){k=0;for(int i=1;i<32;i++)if(decoded_surfaces[i].used<decoded_surfaces[k].used)k=i;}
  DecodedSurface*d=&decoded_surfaces[k];u32 need=w*h;if(d->cap<need){u8*y=malloc(need),*uv=malloc(need/2+w+16);if(!y||!uv){free(y);free(uv);return;}free(d->y);free(d->uv);d->y=y;d->uv=uv;d->cap=need;}
  d->luma=luma;d->display=display;d->w=w;d->h=h;d->owner=owner;d->frame=frame;d->used=++decoded_clock;if(d->y!=sy)memcpy(d->y,sy,need);if(d->uv!=suv)memcpy(d->uv,suv,(size_t)w*((h+1)/2));
  if(getenv("SWR_TRACE_VIC")){static u64 seen[64];static u32 ns;u64 key=luma^((u64)g_fw<<32)^g_fh;int i;for(i=0;i<ns;i++)if(seen[i]==key)break;if(i==ns&&ns<64){seen[ns++]=key;LOG("nvdec surface: current=%llx display=%llx picture=%ux%u planes=%x,%x,%x,%x,%x,%x,%x,%x current16=%x picindex=%x\n",(unsigned long long)luma,(unsigned long long)display,g_fw,g_fh,r[0x430/4],r[0x434/4],r[0x438/4],r[0x43c/4],r[0x440/4],r[0x444/4],r[0x448/4],r[0x44c/4],r[0x470/4],r[0x40c/4]);}}
}
static DecodedSurface*decoded_surface_find(u64 luma){if(!luma)return NULL;for(int i=0;i<32;i++)if(decoded_surfaces[i].used&&(decoded_surfaces[i].luma==luma||decoded_surfaces[i].display==luma))return &decoded_surfaces[i];return NULL;}

static u8 g_last_hdr[512]; static u32 g_last_hdr_n;

static int dec_open(void){
#ifdef SWR_NO_H264
  if(!g_dec_fail){ g_dec_fail=1; LOG("video: this build has no H.264 decoder (the game's videos stay black)\n"); } return 0;
#else
  if(g_dec) return 1; if(g_dec_fail) return 0;
  if(WelsCreateDecoder(&g_dec) || !g_dec){ g_dec_fail=1; LOG("video: OpenH264 decoder could not be created\n"); return 0; }
  SDecodingParam p; memset(&p,0,sizeof p); p.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC; p.eEcActiveIdc=ERROR_CON_SLICE_COPY;
  if((*g_dec)->Initialize(g_dec,&p)){ g_dec_fail=1; LOG("video: OpenH264 decoder initialize failed\n"); return 0; }
  LOG("video: H.264 decoder ready (OpenH264, NVDEC jobs are decoded on the CPU)\n"); return 1;
#endif
}
#ifndef SWR_NO_H264
static void keep_frame(unsigned char** pl, SBufferInfo* bi){
  u32 w=(u32)bi->UsrData.sSystemBuffer.iWidth, h=(u32)bi->UsrData.sSystemBuffer.iHeight; int sy=bi->UsrData.sSystemBuffer.iStride[0], sc=bi->UsrData.sSystemBuffer.iStride[1];
  if(!w || !h || w>4096 || h>4096) return;
  u32 need=w*h; if(g_fcap<need){ free(g_fy); free(g_fuv); g_fy=malloc(need); g_fuv=malloc(need/2+w); g_fuv_cap=need/2+w; g_fcap=need; }
  for(u32 y=0;y<h;y++) memcpy(g_fy+(u64)y*w,pl[0]+(u64)y*sy,w);
  for(u32 y=0;y<h/2;y++){ u8* d=g_fuv+(u64)y*w; const u8* u=pl[1]+(u64)y*sc; const u8* v=pl[2]+(u64)y*sc; for(u32 x=0;x<w/2;x++){ d[2*x]=u[x]; d[2*x+1]=v[x]; } }
  g_fw=w; g_fh=h; g_vframes++; }
#endif

#ifdef SWR_HAVE_VP9
#define SWR_NATIVE_VIDEO_RETIRE 1
#include "vp9video.inc"
#endif

static void nvdec_execute(u32* r){
  { static int said; if(said<4){ said++; LOG("video: NVDEC job, control params %08x (codec %u)\n",r[0x400/4],r[0x400/4]&0xF); } }
#ifdef SWR_HAVE_VP9
  if(vp9_job(r,r[0x400/4]&0xF)) return;      /* a VP9 frame of a .webm the game read: decoded with libvpx */
#endif
  if(!dec_open()) return;
#ifndef SWR_NO_H264
  u64 ps_iova=(u64)r[0x404/4]<<8, bs_iova=(u64)r[0x408/4]<<8;
  u8* ps=iova_ptr(ps_iova,0x300); if(!ps){ static int said; if(said<4){ said++; LOG("video: NVDEC picture setup at iova %llx not mapped\n",(unsigned long long)ps_iova); } return; }
  u32 slen=rd32(ps,0x48); if(!slen || slen>(16u<<20)) return;
  u8* bs=iova_ptr(bs_iova,slen); if(!bs){ static int said; if(said<4){ said++; LOG("video: NVDEC bitstream at iova %llx (%u bytes) not mapped\n",(unsigned long long)bs_iova,slen); } return; }
  u8 hdr[512]; u32 hn=build_headers(ps,hdr,sizeof hdr);
  static u8* buf; static u32 bcap; u32 need=hn+slen+8; if(bcap<need){ buf=realloc(buf,need); bcap=need; }
  u32 o=0;
  if(hn!=g_last_hdr_n || memcmp(hdr,g_last_hdr,hn)){ memcpy(buf,hdr,hn); o=hn; memcpy(g_last_hdr,hdr,hn); g_last_hdr_n=hn; }
  if(bs[0]==0 && bs[1]==0 && (bs[2]==1 || (bs[2]==0 && bs[3]==1))){ memcpy(buf+o,bs,slen); o+=slen; }
  else {
    /* mp4-style 4 byte BE lengths -> start codes; otherwise just prepend one */
    u32 p=0, ok=1; while(p+4<=slen){ u32 L=((u32)bs[p]<<24)|((u32)bs[p+1]<<16)|((u32)bs[p+2]<<8)|bs[p+3]; if(!L || (u64)p+4+L>slen || (bs[p+4]&0x80)){ ok=0; break; }   /* 64-bit: a huge L must not wrap past the check */ p+=4+L; }
    if(ok && p==slen){ static u8* t; static u32 tc; if(tc<slen+64){ t=realloc(t,slen+64); tc=slen+64; } (void)t;
      if(bcap<o+slen+slen/4+16){ bcap=o+slen+slen/4+16; buf=realloc(buf,bcap); }
      for(p=0;p<slen;){ u32 L=((u32)bs[p]<<24)|((u32)bs[p+1]<<16)|((u32)bs[p+2]<<8)|bs[p+3]; buf[o++]=0; buf[o++]=0; buf[o++]=0; buf[o++]=1; memcpy(buf+o,bs+p+4,L); o+=L; p+=4+L; } }
    else { buf[o++]=0; buf[o++]=0; buf[o++]=1; memcpy(buf+o,bs,slen); o+=slen; } }
  unsigned char* pl[3]={0,0,0}; SBufferInfo bi; memset(&bi,0,sizeof bi);
  DECODING_STATE st=(*g_dec)->DecodeFrameNoDelay(g_dec,buf,(int)o,pl,&bi);
  if(bi.iBufferStatus==1 && pl[0]) keep_frame(pl,&bi);
  /* nvdec_status_s at method 0x424. players check it, an all-zero status means the frame failed */
  { u64 st_iova=(u64)r[0x424/4]<<8; u8* sp= st_iova? iova_ptr(st_iova,16) : NULL;
    if(sp){ u32 mbs=rd32(ps,0x64)*rd32(ps,0x68); u32 v[4]={ st? 0 : mbs, st? mbs : 0, 100000, st? 1 : 0 }; memcpy(sp,v,16); } }
  { static u32 njobs; njobs++; if(njobs%120==0) LOG("video: %u NVDEC jobs so far, %u pictures decoded\n",njobs,g_vframes); }
  { static int said; if(said<3 || (st && said<8)){ said++; LOG("video: NVDEC H.264 job: %ux%u MBs, %u bytes, decoder state %x, picture %s (%ux%u)\n",rd32(ps,0x64),rd32(ps,0x68),slen,(unsigned)st,bi.iBufferStatus==1? "out" : "pending",g_fw,g_fh); } }
#endif
}
/* VIC output surface config, u64 at config(0x708)+0x20:
   0-6 format (0x1F A8B8G8R8, 0x20 A8R8G8B8, 0x23 X8B8G8R8, 0x24 X8R8G8B8, 0x44 NV12), 11-14 bl kind, 15-18 block height log2,
   32-45 width-1, 46-59 height-1 */
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2);

static u8 clamp8(int v){ return v<0? 0 : v>255? 255 : (u8)v; }

static void vic_rgba_scalar(const u8*Y,const u8*UV,u8*out,u32 start,u32 w,int rgb_order){
  int ri=rgb_order?0:2,bi=rgb_order?2:0;
  for(u32 x=start;x<w;x++){int c=((int)Y[x]-16)*298,d=(int)UV[x&~1u]-128,e=(int)UV[(x&~1u)+1]-128;u8*o=out+x*4;
    o[ri]=clamp8((c+409*e+128)>>8);o[1]=clamp8((c-100*d-208*e+128)>>8);o[bi]=clamp8((c+516*d+128)>>8);o[3]=255;}
}
#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__) && !defined(__TINYC__)
#include <immintrin.h>
#define SWR_VIC_HAS_AVX2 1
__attribute__((target("avx2")))
static void vic_rgba_avx2(const u8*Y,const u8*UV,u8*out,u32 w,int rgb_order){
  const __m128i um=_mm_setr_epi8(0,0,2,2,4,4,6,6,-128,-128,-128,-128,-128,-128,-128,-128);
  const __m128i vm=_mm_setr_epi8(1,1,3,3,5,5,7,7,-128,-128,-128,-128,-128,-128,-128,-128);
  const __m256i zero=_mm256_setzero_si256(),max=_mm256_set1_epi32(255),bias=_mm256_set1_epi32(128);
  u32 x=0;
  for(;x+8<=w;x+=8){
    __m128i uv=_mm_loadl_epi64((const __m128i*)(UV+x));
    __m256i c=_mm256_mullo_epi32(_mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*)(Y+x))),_mm256_set1_epi32(16)),_mm256_set1_epi32(298));
    __m256i d=_mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_shuffle_epi8(uv,um)),bias);
    __m256i e=_mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_shuffle_epi8(uv,vm)),bias);
    __m256i r=_mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(c,_mm256_mullo_epi32(e,_mm256_set1_epi32(409))),bias),8);
    __m256i g=_mm256_srai_epi32(_mm256_add_epi32(_mm256_sub_epi32(_mm256_sub_epi32(c,_mm256_mullo_epi32(d,_mm256_set1_epi32(100))),_mm256_mullo_epi32(e,_mm256_set1_epi32(208))),bias),8);
    __m256i b=_mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(c,_mm256_mullo_epi32(d,_mm256_set1_epi32(516))),bias),8);
    r=_mm256_min_epi32(_mm256_max_epi32(r,zero),max);g=_mm256_min_epi32(_mm256_max_epi32(g,zero),max);b=_mm256_min_epi32(_mm256_max_epi32(b,zero),max);
    __m256i lo=rgb_order?r:b,hi=rgb_order?b:r;
    __m256i pixels=_mm256_or_si256(_mm256_or_si256(lo,_mm256_slli_epi32(g,8)),_mm256_or_si256(_mm256_slli_epi32(hi,16),_mm256_set1_epi32((int)0xff000000u)));
    _mm256_storeu_si256((__m256i*)(out+x*4),pixels);
  }
  vic_rgba_scalar(Y,UV,out,x,w,rgb_order);
}
#endif
static void vic_rgba_row(const u8*Y,const u8*UV,u8*out,u32 w,int rgb_order){
#ifdef SWR_VIC_HAS_AVX2
  static int fast=-1;if(fast<0){extern int g_opt_video_avx2;fast=!getenv("SWR_NO_VIC_AVX2")&&(getenv("SWR_VIC_AVX2")||g_opt_video_avx2)&&__builtin_cpu_supports("avx2");if(fast)LOG("video: AVX2 exact integer RGBA conversion active\n");}
  if(fast){vic_rgba_avx2(Y,UV,out,w,rgb_order);return;}
#endif
  vic_rgba_scalar(Y,UV,out,0,w,rgb_order);
}

static void vic_put(u8* base, u64 off, const u8* src, u32 n, u64 lim){ if(off+n<=lim) memcpy(base+off,src,n); }

static void vic_block_row(u8*base,const u8*src,u32 bytes,u32 y,u32 pitch,u32 height,u32 bh,u64 lim){
  static int fast=-1;if(fast<0){extern int g_opt_video_block_copy;fast=!getenv("SWR_NO_VIC_BLOCK_COPY")&&(getenv("SWR_VIC_BLOCK_COPY")||g_opt_video_block_copy);}
  u32 x=0;
  if(fast){u64 row=bl_offset(0,y,0,pitch,height,bh,0),column_stride=512ull<<bh;
    for(;bytes-x>=64;x+=64){u64 off=row+(u64)(x>>6)*column_stride;
      vic_put(base,off,src+x,16,lim);vic_put(base,off+32,src+x+16,16,lim);
      vic_put(base,off+256,src+x+32,16,lim);vic_put(base,off+288,src+x+48,16,lim);
    }
  }
  for(;x<bytes;x+=16){u32 n=bytes-x<16?bytes-x:16;vic_put(base,bl_offset(x,y,0,pitch,height,bh,0),src+x,n,lim);}
}

/* a picture of another size than the surface (a mod video, #31): bilinear resample of the luma and the interleaved
   chroma planes to the surface size, so the rest of vic_execute sees a picture of the game's size */
static void vic_resample(const u8* sy, const u8* suv, u32 fw, u32 fh, u32 sw, u32 sh, u8** oy, u8** ouv){
  static u8 *ry, *ruv; static u32 cap; u32 need=sw*sh+sw+16; if(cap<need){ free(ry); free(ruv); ry=malloc(need); ruv=malloc(need/2+sw+16); cap= (ry && ruv)? need : 0; }
  if(!cap){ *oy=NULL; return; }
  for(u32 y=0;y<sh;y++){ float fyf=((float)y+0.5f)*(float)fh/(float)sh-0.5f; if(fyf<0) fyf=0; u32 y0=(u32)fyf, y1= y0+1<fh? y0+1 : y0; u32 ty=(u32)((fyf-(float)y0)*256.0f);
    const u8* a=sy+(u64)y0*fw; const u8* b=sy+(u64)y1*fw; u8* d=ry+(u64)y*sw;
    for(u32 x=0;x<sw;x++){ float fxf=((float)x+0.5f)*(float)fw/(float)sw-0.5f; if(fxf<0) fxf=0; u32 x0=(u32)fxf, x1= x0+1<fw? x0+1 : x0; u32 tx=(u32)((fxf-(float)x0)*256.0f);
      u32 t=(a[x0]*(256-tx)+a[x1]*tx), u=(b[x0]*(256-tx)+b[x1]*tx); d[x]=(u8)((t*(256-ty)+u*ty+32768)>>16); } }
  u32 cw=fw/2, ch=fh/2, dw=sw/2, dh=(sh+1)/2; if(!cw || !ch){ *oy=NULL; return; }
  for(u32 y=0;y<dh;y++){ float fyf=((float)y+0.5f)*(float)ch/(float)dh-0.5f; if(fyf<0) fyf=0; u32 y0=(u32)fyf, y1= y0+1<ch? y0+1 : y0; u32 ty=(u32)((fyf-(float)y0)*256.0f);
    const u8* a=suv+(u64)y0*fw; const u8* b=suv+(u64)y1*fw; u8* d=ruv+(u64)y*sw;
    for(u32 x=0;x<dw;x++){ float fxf=((float)x+0.5f)*(float)cw/(float)dw-0.5f; if(fxf<0) fxf=0; u32 x0=(u32)fxf, x1= x0+1<cw? x0+1 : x0; u32 tx=(u32)((fxf-(float)x0)*256.0f);
      for(int c=0;c<2;c++){ u32 t=(a[2*x0+c]*(256-tx)+a[2*x1+c]*tx), u=(b[2*x0+c]*(256-tx)+b[2*x1+c]*tx); d[2*x+c]=(u8)((t*(256-ty)+u*ty+32768)>>16); } } }
  *oy=ry; *ouv=ruv; }

static void vic_execute(u32* r){
  if(!g_vframes) return;
  u8*fy=g_fy,*fuv=g_fuv;u32 fw=g_fw,fh=g_fh;
  DecodedSurface*picture=decoded_surface_find((u64)r[0x400/4]<<8);
  if(picture){fy=picture->y;fuv=picture->uv;fw=picture->w;fh=picture->h;}
  { static u32 nv; nv++; if(nv%120==0) LOG("video: %u VIC jobs so far\n",nv); }
  u64 ly=(u64)r[0x720/4]<<8, lc=(u64)r[0x724/4]<<8, cfg_iova=(u64)r[0x708/4]<<8;
  u8* cs=iova_ptr(cfg_iova,0x28); u64 cfg=0; if(cs) memcpy(&cfg,cs+0x20,8);
  if(!picture){u8*sc=iova_ptr(cfg_iova,0xe0);if(sc){u64 sf=0;memcpy(&sf,sc+0xd0,8);u32 iw=(u32)((sf>>32)&0x3fff)+1,ih=(u32)((sf>>46)&0x3fff)+1;for(int i=0;i<32;i++){DecodedSurface*d=&decoded_surfaces[i];if(d->used&&d->w==iw&&d->h==ih&&(!picture||d->used>picture->used))picture=d;}if(picture){fy=picture->y;fuv=picture->uv;fw=picture->w;fh=picture->h;}}}
  u32 fmt=(u32)(cfg&0x7F), blk=(u32)((cfg>>11)&15), bh=(u32)((cfg>>15)&15);
  u32 sw=(u32)((cfg>>32)&0x3FFF)+1, sh=(u32)((cfg>>46)&0x3FFF)+1;
  if(!cs){ fmt=0x44; sw=fw; sh=fh; }
  {static int checked;static FILE*csv;if(!checked){checked=1;const char*e=getenv("SWR_VIC_JOB_CSV");if(e&&*e){csv=fopen(e,"w");if(csv)fprintf(csv,"ns,input,selected_owner,selected_frame,selected_used,exact_owner,exact_frame,exact_used,width,height\n");}}
    if(csv){u64 input=(u64)r[0x400/4]<<8;DecodedSurface*exact=NULL;for(int i=0;i<32;i++)if(decoded_surfaces[i].used&&decoded_surfaces[i].luma==input)exact=&decoded_surfaces[i];
      fprintf(csv,"%llu,%llu,%s,%d,%llu,%s,%d,%llu,%u,%u\n",(unsigned long long)plat_ns(),(unsigned long long)input,picture&&picture->owner?(const char*)(uintptr_t)picture->owner:"",picture?(int)picture->frame:-1,(unsigned long long)(picture?picture->used:0),exact&&exact->owner?(const char*)(uintptr_t)exact->owner:"",exact?(int)exact->frame:-1,(unsigned long long)(exact?exact->used:0),fw,fh);fflush(csv);}}

  if(getenv("SWR_VIC_FMT")) fmt=(u32)strtoul(getenv("SWR_VIC_FMT"),0,0);
  if(cs && fw && fh && (picture? (picture->owner && v9_sub_for((const char*)(uintptr_t)picture->owner)) : g_pic_sub) && sw>=16 && sh>=16 && sw<=4096 && sh<=4096 && (fw>sw+16 || fh>sh+16 || fw+16<sw || fh+16<sh)){   /* a mod video of another size (#31) */
    u8 *ry=NULL, *ruv=NULL; vic_resample(fy,fuv,fw,fh,sw,sh,&ry,&ruv);
    if(ry){ { static int said; if(said<2){ said++; LOG("video: picture %ux%u scaled to the game's %ux%u\n",fw,fh,sw,sh); } } fy=ry; fuv=ruv; fw=sw; fh=sh; } }
  u32 w=fw<sw? fw : sw, h=fh<sh? fh : sh;
  if(getenv("SWR_TRACE_VIC")){static u64 seen[128];static u32 ns;u8*dst=iova_ptr(ly,1);u64 key=ly^cfg;int k;for(k=0;k<ns;k++)if(seen[k]==key)break;
    if(k==ns&&ns<128){seen[ns++]=key;u8*all=iova_ptr(cfg_iova,0x610);u32 enabled=0;char slots[700]={0};int used=0;
      if(all)for(u32 slot=0;slot<8;slot++){u64 sc=0,sf=0;memcpy(&sc,all+0x90+slot*0xb0,8);memcpy(&sf,all+0xd0+slot*0xb0,8);if(sc&1){enabled++;used+=snprintf(slots+used,sizeof slots-used," slot%u fmt=%x input=%x",slot,(u32)(sf&127),r[0x400/4+slot*24]);}}
      LOG("vic trace: output guest=%llx iova=%llx fmt=%x %ux%u movie=%ux%u enabled=%u source_bound=%u%s\n",(unsigned long long)(dst?(u64)(dst-GP(0)):0),(unsigned long long)ly,fmt,sw,sh,fw,fh,enabled,picture!=NULL,slots);
    }}

  { static int said; if(said<3){ said++; LOG("video: VIC config %016llx: format %x, %s (block height %u), surface %ux%u, picture %ux%u\n",(unsigned long long)cfg,fmt,blk? "block linear" : "pitch linear",1u<<bh,sw,sh,fw,fh); } }
  if(fmt==0x1F || fmt==0x20 || fmt==0x23 || fmt==0x24){
    u32 pitch=sw*4; u64 lim= blk? (u64)pitch*(((sh+(8u<<bh)-1)/(8u<<bh))*(8u<<bh)) : (u64)pitch*sh;
    u8* py=iova_ptr(ly,lim); if(!py){ py=iova_ptr(ly,(u64)pitch*h); lim=(u64)pitch*h; }
    if(!py){ static int said; if(said<4){ said++; LOG("video: VIC output at iova %llx not mapped\n",(unsigned long long)ly); } return; }
    static u8* row; static u32 rcap; if(rcap<pitch){ row=realloc(row,pitch); rcap=pitch; }
    int rgb_order=(fmt==0x1F || fmt==0x23);
    for(u32 y=0;y<h;y++){
      const u8* Y=fy+(u64)y*fw; const u8* UV=fuv+(u64)(y/2)*fw;
      vic_rgba_row(Y,UV,row,w,rgb_order);
      if(!blk) vic_put(py,(u64)y*pitch,row,w*4,lim);
      else vic_block_row(py,row,w*4,y,pitch,sh,bh,lim); }
    { static int said; if(said<2){ said++; LOG("video: VIC wrote a %ux%u RGBA picture at iova %llx\n",w,h,(unsigned long long)ly); } }
    video_written(py,lim);
    return; }
  /* NV12. pitch linear rows are 256 aligned */
  u32 pitch= blk? sw : (sw+255)&~255u;
  u64 ylim=(u64)pitch*sh, clim=(u64)pitch*((sh+1)/2);
  if(blk){ u32 gy=8u<<bh; ylim=(u64)pitch*((sh+gy-1)/gy*gy); clim=(u64)pitch*(((sh+1)/2+gy-1)/gy*gy); }
  u8* py=iova_ptr(ly,ylim); u8* pc= lc? iova_ptr(lc,clim) : NULL;
  if(!py){ static int said; if(said<4){ said++; LOG("video: VIC output luma at iova %llx not mapped\n",(unsigned long long)ly); } return; }
  for(u32 y=0;y<h;y++){ const u8* s=fy+(u64)y*fw;
    if(!blk) vic_put(py,(u64)y*pitch,s,w,ylim);
    else vic_block_row(py,s,w,y,pitch,sh,bh,ylim); }
  if(pc) for(u32 y=0;y<h/2;y++){ const u8* s=fuv+(u64)y*fw;
    if(!blk) vic_put(pc,(u64)y*pitch,s,w,clim);
    else vic_block_row(pc,s,w,y,pitch,(sh+1)/2,bh,clim); }
  video_written(py,ylim); if(pc) video_written(pc,clim);
  { static int said; if(said<2){ said++; LOG("video: VIC wrote a %ux%u NV12 picture (luma iova %llx, chroma %llx, pitch %u)\n",w,h,(unsigned long long)ly,(unsigned long long)lc,pitch); } } }

/* ------------------------------------------------------------ host1x command stream */
typedef struct { int is_vic; u32 syncpt; u32 regs[0x1000/4]; u32 method; } H1Ch;

static void dev_method(H1Ch* ch, u32 m, u32 v){
  u32 off=m<<2; if(off<0x1000) ch->regs[off/4]=v;
  if(off==0x300){ if(ch->is_vic) vic_execute(ch->regs); else nvdec_execute(ch->regs); } }

static void reg_write(H1Ch* ch, u32 cls, u32 off, u32 v){
  if(cls==0x1) return;                       /* host1x class itself, incrs are already counted from the submit */
  if(off==0x10) ch->method=v; else if(off==0x11) dev_method(ch,ch->method,v); }

static void run_words(H1Ch* ch, const u32* w, u32 n){
  u32 cls=ch->is_vic? 0x5D : 0xF0;
  for(u32 i=0;i<n;){
    u32 x=w[i++], op=x>>28;
    switch(op){
      case 0: { cls=(x>>6)&0x3FF; u32 off=(x>>16)&0xFFF, mask=x&0x3F; for(int b=0;b<6;b++) if(mask&(1u<<b)){ if(i>=n) return; reg_write(ch,cls,off+b,w[i++]); } break; }
      case 1: { u32 off=(x>>16)&0xFFF, c=x&0xFFFF; for(u32 k=0;k<c && i<n;k++) reg_write(ch,cls,off+k,w[i++]); break; }
      case 2: { u32 off=(x>>16)&0xFFF, c=x&0xFFFF; for(u32 k=0;k<c && i<n;k++) reg_write(ch,cls,off,w[i++]); break; }
      case 3: { u32 off=(x>>16)&0xFFF, mask=x&0xFFFF; for(int b=0;b<16;b++) if(mask&(1u<<b)){ if(i>=n) return; reg_write(ch,cls,off+b,w[i++]); } break; }
      case 4: reg_write(ch,cls,(x>>16)&0xFFF,x&0xFFFF); break;
      default: { static int said; if(said<6){ said++; LOG("video: host1x opcode %u not handled (word %08x)\n",op,x); } return; }
    } } }

/* ------------------------------------------------------------ channel ioctls */
static H1Ch g_ch[2]; static u32 g_ch_syncpt_next=200;

u32 host1x_ioctl(int is_vic, u32 cmd, u8* d, u32 n){
  h_init(); pm_lock(&g_hlk);
  H1Ch* ch=&g_ch[is_vic? 1 : 0]; ch->is_vic=is_vic; if(!ch->syncpt) ch->syncpt=g_ch_syncpt_next++;
  u32 r=0, grp=(cmd>>8)&0xFF, nr=cmd&0xFF;
  if(grp==0x48){ /* SET_NVMAP_FD etc */ }
  else if(grp==0) switch(nr){
    case 0x01: { /* SUBMIT: counts, cmdbufs {memid, offset, words}, relocs, reloc shifts, syncpt incrs {id, n, ?x3}, fences {id, value} */
      u32 ncb=rd32(d,0), nrel=rd32(d,4), nsp=rd32(d,8), nfe=rd32(d,12); u64 o=16;   /* 64-bit: guest counts must not wrap past the size check */
      u32 cbo=(u32)o; o+=(u64)ncb*12+(u64)nrel*20; u32 spo=(u32)o; o+=(u64)nsp*20; u32 feo=(u32)o;
      if(o>n){ r=1; break; }
      for(u32 i=0;i<ncb;i++){ u32 mem=rd32(d,cbo+i*12), off=rd32(d,cbo+i*12+4), wc=rd32(d,cbo+i*12+8);
        u64 a=nvmap_addr(mem); if(!a) a=nvmap_addr_by_id(mem); if(!a || !wc || wc>0x100000) continue;
        run_words(ch,(const u32*)GP(a+off),wc); }
      for(u32 i=0;i<nsp;i++){ u32 id=rd32(d,spo+i*20), inc=rd32(d,spo+i*20+4); if(!id) id=ch->syncpt;
        for(u32 k=0;k<inc && k<64;k++) syncpt_incr(id);
        if(i<nfe && feo+i*4+4<=n){ u32 v=syncpt_read(id); memcpy(d+feo+i*4,&v,4); } }   /* fence thresholds, u32 each */
      { static int said; if(said<3){ said++; LOG("video: %s submit: %u command buffers, %u relocs, %u syncpoint increments, %u fences (%u bytes)\n",is_vic? "vic" : "nvdec",ncb,nrel,nsp,nfe,n); } }
      break; }
    case 0x02: { u32 v=ch->syncpt; memcpy(d+4,&v,4); break; }          /* GET_SYNCPOINT */
    case 0x03: { u32 v=0; memcpy(d+4,&v,4); break; }                    /* GET_WAITBASE */
    case 0x07: break;                                                   /* SET_SUBMIT_TIMEOUT */
    case 0x09: { u32 ne=rd32(d,0); for(u32 i=0;i<ne && 12+i*8+8<=n;i++){ u32 h=rd32(d,12+i*8); u32 io=iova_map(h); memcpy(d+12+i*8+4,&io,4); } break; }   /* MAP_BUFFER */
    case 0x0A: {u32 ne=rd32(d,0);for(u32 i=0;i<ne&&12+i*8+8<=n;i++)iova_unmap(rd32(d,12+i*8));break;} /* UNMAP_BUFFER */
    default: { static int said; if(said<8){ said++; LOG("video: %s ioctl %08x not handled\n",is_vic? "vic" : "nvdec",cmd); } break; }
  }
  else { static int said; if(said<8){ said++; LOG("video: %s ioctl %08x not handled\n",is_vic? "vic" : "nvdec",cmd); } }
  pm_unlock(&g_hlk); return r; }
