/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* software 3D fallback, only does clears for now */
#include "gpu.h"

static u32 bpp_of_rt(u32 f){
  switch(f){ case 0xC0: return 16; case 0xC6: case 0xC8: case 0xCA: case 0xCB: case 0xCC: case 0xCE: return 8; case 0xE8: case 0xF3: return 1; case 0xE9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: return 2; default: return 4; } }
extern int g_gl; extern void r3d_clear_gl(GpuChannel*,u32); extern void r3d_draw_gl(GpuChannel*,int,u32,u32,u32,u32,u32,u32); extern void r3d_cb_bind_gl(GpuChannel*,int,u32);

void r3d_clear(GpuChannel* ch, u32 v){
  if(g_gl){ r3d_clear_gl(ch,v); return; }
  u32* r=ch->r3d; u32 rt=(v>>6)&15; if(!(v&0x3C)) return;
  u32* t=&r[0x200+rt*0x10]; u64 addr=((u64)t[0]<<32)|t[1]; u32 w=t[2], h=t[3], fmt=t[4], tm=t[5];
  if(!addr || !w || !h) return;
  float cc[4]; memcpy(cc,&r[0x360],16);
  u32 bpp=bpp_of_rt(fmt); u8 px[16];
  if(bpp==4){ u32 c=0; for(int i=0;i<4;i++){ float f=cc[i]<0?0:cc[i]>1?1:cc[i]; c|=(u32)(f*255.f+0.5f)<<(i*8); } if(fmt==0xCF||fmt==0xD0) c=(c&0xFF00FF00u)|((c&0xFF)<<16)|((c>>16)&0xFF); memcpy(px,&c,4); }
  else memset(px,0,16);
  int linear=(tm>>12)&1; u32 bh=(tm>>4)&15;
  if(linear){ /* pitch-linear targets keep the pitch in bytes in the width register */
    u32 pitch=w, wpx=pitch/bpp; u8* lb=gmem_ptr(addr,(u64)pitch*h); if(!lb) return;
    for(u32 y=0;y<h;y++) for(u32 x=0;x<wpx;x++) memcpy(lb+(u64)y*pitch+(u64)x*bpp,px,bpp);
    return; }
  u8* base=gmem_ptr(addr,1);
  if(!base) return;
  { u8* row=malloc((size_t)w*bpp); for(u32 x=0;x<w;x++) memcpy(row+x*bpp,px,bpp);
    for(u32 y=0;y<h;y++) for(u32 x=0;x<w*bpp;x+=16){ u64 o=bl_offset(x,y,0,w*bpp,h,bh,0); u32 n=w*bpp-x<16? w*bpp-x : 16; if(gmmu_translate(addr+o)) memcpy(GP(gmmu_translate(addr+o)),row+x,n); }
    free(row); }
}

void r3d_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance, u32 instances){
  if(g_gl){ r3d_draw_gl(ch,indexed,topology,first,count,base_vertex,base_instance,instances); return; }
  extern u64 g_draws; (void)ch;(void)indexed;(void)topology;(void)first;(void)count;(void)base_vertex;(void)base_instance;(void)instances; g_draws++; }

void r3d_const_upload(GpuChannel* ch, u32 v){
  u32* r=ch->r3d; u64 a=((u64)r[0x8E1]<<32)|r[0x8E2]; u32 off=r[0x8E3]; gmem_w32(a+off,v); r[0x8E3]=off+4; }
/* the same for a run of words in one method batch (run_pushbuffer): what n r3d_const_upload calls leave in memory and in the
   registers, in one copy. A scene can send 2 MB of constants a frame, which word by word cost 40% of the GPU thread (Deck,
   Corpius death scene, MEASURED 2026-11-01). m: the method of w[0]; inc: it moves on by one for each word */
void r3d_const_run(GpuChannel* ch, u32 m, int inc, const u32* w, u32 n){
  u32* r=ch->r3d; u64 a=((u64)r[0x8E1]<<32)|r[0x8E2]; u32 off=r[0x8E3];
  gmem_write(a+off,w,(u64)n*4); r[0x8E3]=off+n*4;
  if(inc) memcpy(&r[m],w,(u64)n*4); else r[m]=w[n-1]; }

void r3d_cb_bind(GpuChannel* ch, int stage, u32 v){ if(g_gl) r3d_cb_bind_gl(ch,stage,v); }
extern int r3d_blit_gl(void* s, void* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx, double sdy);

int r3d_blit(void* s, void* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx, double sdy){ if(g_gl) return r3d_blit_gl(s,d,dx,dy,dw,dh,sx,sy,sdx,sdy); return 0; }

void r3d_flush(GpuChannel* ch){ (void)ch; }

/* write log for GPU-side writes (cb_data, I2M, DMA, 2D, reports) so we can tell a range changed
   without rehashing it. Back-to-back writes (cb_data goes word by word) get merged. */
#define GW_N 1024
u64 g_gw_gen;                       /* entries in the log */
static u64 gw_a[GW_N], gw_e[GW_N], gw_g0[GW_N], gw_g1[GW_N]; static u64 gw_n;
/* last write gen per 4K block (hashed) so dirty_since doesn't have to walk the log.
   cb checks run thousands of times per frame, the walk showed up high in profiles */
#define GWB_N 65536
static u64 gw_blk[GWB_N];

void gpu_mem_written(u64 gva, u64 size){
  if(!size) return; g_gw_gen++;
  { u64 b0=gva>>12, b1=(gva+size-1)>>12; if(b1-b0>=GWB_N) b1=b0+GWB_N-1; for(u64 b=b0;b<=b1;b++) gw_blk[b&(GWB_N-1)]=g_gw_gen; }
  if(gw_n){ u32 k=(u32)((gw_n-1)%GW_N); if(gw_e[k]==gva || (gva>=gw_a[k] && gva+size<=gw_e[k])){ if(gva+size>gw_e[k]) gw_e[k]=gva+size; gw_g1[k]=g_gw_gen; return; } }
  u32 k=(u32)(gw_n%GW_N); gw_a[k]=gva; gw_e[k]=gva+size; gw_g0[k]=gw_g1[k]=g_gw_gen; gw_n++; }

/* nonzero if [a,a+sz) may have been written after gen 'since' */
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz){
  if(since>=g_gw_gen) return 0;
  if(sz && sz<=(1u<<20)){ for(u64 b=a>>12, e=(a+sz-1)>>12; b<=e; b++) if(gw_blk[b&(GWB_N-1)]>since) return 1; return 0; }
  u64 lim= gw_n>GW_N? gw_n-GW_N : 0;
  for(u64 n=gw_n;n>lim;n--){ u32 k=(u32)((n-1)%GW_N); if(gw_g1[k]<=since) return 0;
    if(gw_a[k]<a+sz && a<gw_e[k]) return 1;
    if(gw_g0[k]<=since) return 0; }
  return 1; }   /* older than the log, assume dirty */
void gpu3d_init(void){}
