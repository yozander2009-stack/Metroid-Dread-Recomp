/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* GM20B front end: GPFIFO, puller, semaphores/syncpoints, I2M, DMA, 2D and the bits of 3D that
   write memory (clears, reports). Actual drawing is the renderer's job. */
#include "rt.h"
#include "gpu.h"

GpuState g_gpu;
static PMutex g_gpulock;

/* ============================================================ GPU memory */
extern volatile int g_cs_any; void cs_mem_need(u64 a, u64 n);

int gmem_read(u64 gva, void* dst, u64 size){
  u8* d=dst; if(g_cs_any) cs_mem_need(gva,size);
  while(size){ u64 cpu=gmmu_translate(gva); u64 n=0x1000-(gva&0xFFF); if(n>size) n=size;
    if(!cpu){ memset(d,0,n); } else memcpy(d,GP(cpu),n);
    d+=n; gva+=n; size-=n; }
  return 0; }
void (*g_ww_touch)(u64 cpu);   /* GPU wrote guest memory, record it for write tracking */
int gmem_write(u64 gva, const void* src, u64 size){
  const u8* s=src; u64 g0=gva, s0=size;
  while(size){ u64 cpu=gmmu_translate(gva); u64 n=0x1000-(gva&0xFFF); if(n>size) n=size;
    if(cpu){ memcpy(GP(cpu),s,n); if(g_ww_touch) g_ww_touch(cpu); } s+=n; gva+=n; size-=n; }
  gpu_mem_written(g0,s0);
  return 0; }

u8* gmem_ptr(u64 gva, u64 size){
  /* host pointer, only if the whole range is contiguous */
  if(g_cs_any) cs_mem_need(gva,size);
  u64 c0=gmmu_translate(gva); if(!c0) return NULL;
  for(u64 o=(0x1000-(gva&0xFFF));o<size;o+=0x1000){ if(gmmu_translate(gva+o)!=c0+o) return NULL; }
  return GP(c0); }

u32 gmem_r32(u64 gva){ u32 v=0; gmem_read(gva,&v,4); return v; }

void gmem_w32(u64 gva, u32 v){ gmem_write(gva,&v,4); }

void gmem_w64(u64 gva, u64 v){ gmem_write(gva,&v,8); }

/* ============================================================ block linear */
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2){
  u32 gob_w=(width_bytes+63)/64; u32 bh=1u<<bh_log2, bd=1u<<bd_log2;
  u32 blocks_h=((height+7)/8+bh-1)/bh;
  u32 gx=x_bytes/64, gy=y/8;
  u32 block_y=gy/bh, gob_in=gy%bh; u32 block_z=z/bd, z_in=z%bd;
  u64 block_size=512ull*bh*bd;
  u64 off=((u64)block_z*blocks_h*gob_w + (u64)block_y*gob_w + gx)*block_size + ((u64)z_in*bh + gob_in)*512;
  u32 xb=x_bytes%64, yb=y%8;
  off+= ((xb%64)/32)*256 + (yb/2)*64 + ((xb%32)/16)*32 + (yb%2)*16 + (xb%16);
  return off; }

/* rows r0..r1 of the z*height+y sequence: texture loads split a level over several threads by row ranges */
void deswizzle_rows(u8*dst,u32 rowb,const u8*src,u64 source_size,u32 height,u32 bh,u32 bd,u32 r0,u32 r1){
  u64 column_stride=(512ull<<bh)<<bd;
  for(u32 r=r0;r<r1;r++){ u32 z=r/height, y=r%height;
    u8*d=dst+(u64)r*rowb;u64 row=bl_offset(0,y,z,rowb,height,bh,bd);
    u32 x=0;
    for(;rowb-x>=64;x+=64){u64 o=row+(u64)(x>>6)*column_stride;
      if(o<=source_size && source_size-o>=304){
        memcpy(d+x,src+o,16); memcpy(d+x+16,src+o+32,16);
        memcpy(d+x+32,src+o+256,16); memcpy(d+x+48,src+o+288,16);
      }else{for(u32 k=0;k<64;k+=16){u64 a=o+((u64)(k&32)<<3)+((u64)(k&16)<<1);if(a<=source_size && source_size-a>=16)memcpy(d+x+k,src+a,16);}}
    }
    for(;x<rowb;x+=16){u64 o=row+(u64)(x>>6)*column_stride+((u64)(x&32)<<3)+((u64)(x&16)<<1);u32 n=rowb-x<16?rowb-x:16;if(o<=source_size && source_size-o>=n)memcpy(d+x,src+o,n);}
  }
}
void deswizzle_blocks(u8*dst,u32 rowb,const u8*src,u64 source_size,u32 height,u32 depth,u32 bh,u32 bd){
  deswizzle_rows(dst,rowb,src,source_size,height,bh,bd,0,depth*height); }
void deswizzle(u8*dst,u32 dst_pitch,const u8*src,u32 w_bytes,u32 h,u32 bh){
  if(dst_pitch==w_bytes){deswizzle_blocks(dst,w_bytes,src,~0ull,h,1,bh,0);return;}
  u64 column_stride=512ull<<bh;
  for(u32 y=0;y<h;y++){u8*d=dst+(u64)y*dst_pitch;u64 row=bl_offset(0,y,0,w_bytes,h,bh,0);
    for(u32 x=0;x<w_bytes;x+=16){u64 o=row+(u64)(x>>6)*column_stride+((u64)(x&32)<<3)+((u64)(x&16)<<1);u32 n=w_bytes-x<16?w_bytes-x:16;memcpy(d+x,src+o,n);}}
}

void swizzle(u8* dst, const u8* src, u32 src_pitch, u32 w_bytes, u32 h, u32 bh_log2){
  for(u32 y=0;y<h;y++){ const u8* s=src+(u64)y*src_pitch;
    for(u32 x=0;x<w_bytes;x+=16){ u64 o=bl_offset(x,y,0,w_bytes,h,bh_log2,0); u32 n=w_bytes-x<16? w_bytes-x : 16; memcpy(dst+o,s+x,n); } } }

/* ============================================================ puller */
static void puller_method(GpuChannel* ch, u32 m, u32 v){
  u32* r=ch->puller;
  r[m]=v;
  switch(m){
    case 0x00: break;
    case 0x07: { /* SemaphoreOperation */
      u64 addr=((u64)(r[4]&0xFF)<<32)|r[5]; u32 op=v&0x1F;
      if(op==2){ /* release */
        if(v&(1u<<24)) { gmem_w32(addr,r[6]); }                     /* 4 bytes only */
        else { extern u64 gpu_ticks(void); u32 q[4]={r[6],0,0,0}; u64 t=gpu_ticks(); memcpy(&q[2],&t,8); gmem_write(addr,q,16); }
      } else if(op==1 || op==4){ /* acquire, with a timeout so a bad value can't hang us */
        u64 end=plat_ns()+2000000000ull;
        for(;;){ u32 cur=gmem_r32(addr); if(op==1? cur==r[6] : (i32)(cur-r[6])>=0) break; if(plat_ns()>end){ WARN("gpu: semaphore %llx waits for %x (has %x)\n",(unsigned long long)addr,r[6],cur); break; } plat_yield(); }
      }
      break; }
    case 0x1D: { /* Syncpoint */
      u32 id=(v>>8)&0xFFF; if(v&1){ extern void gpu_gl_stream_incr(u32); gpu_gl_stream_incr(id); } else { extern void gpu_gl_before_syncwait(void); gpu_gl_before_syncwait(); syncpt_wait(id,r[0x1C],2000000000); } break; }
    default: break;
  }
}

/* ============================================================ inline to memory */
void i2m_launch(GpuChannel* ch, u32* regs, int base){
  (void)ch;
  u32 line_len=regs[base+0], lines=regs[base+1]; u64 addr=((u64)regs[base+2]<<32)|regs[base+3];
  /* 64-bit: line_len*lines can wrap in 32 bits and leave a tiny buffer for a huge upload */
  u64 len=(u64)line_len*(lines?lines:1); if(len>(256ull<<20)) len=0;
  ch->i2m_len=(u32)len; ch->i2m_pos=0; ch->i2m_addr=addr; ch->i2m_linear=(regs[base+0xC]&1);
  ch->i2m_pitch=regs[base+4]; ch->i2m_w=line_len; ch->i2m_lines=lines;
  /* block linear dst. 0x65 = block dims (w 3:0, h 7:4, d 11:8), 0x66..0x68 size, 0x69 z, 0x6A x, 0x6B y.
     note the width field is bits 3:0, height is 7:4 */
  ch->i2m_bh=(regs[base+5]>>4)&0xF; ch->i2m_bd=(regs[base+5]>>8)&0xF;
  ch->i2m_dw=regs[base+6]; ch->i2m_dh=regs[base+7]; ch->i2m_z=regs[base+9]; ch->i2m_x=regs[base+0xA]; ch->i2m_y=regs[base+0xB];
  if(ch->i2m_buf_cap<ch->i2m_len){ u8* nb=realloc(ch->i2m_buf,(size_t)ch->i2m_len+4); if(!nb){ ch->i2m_len=0; return; } ch->i2m_buf=nb; ch->i2m_buf_cap=ch->i2m_len; }
}

void i2m_data(GpuChannel* ch, u32 v){
  if(ch->i2m_pos>=ch->i2m_len) return;
  u32 n=ch->i2m_len-ch->i2m_pos; if(n>4) n=4; memcpy(ch->i2m_buf+ch->i2m_pos,&v,n); ch->i2m_pos+=n;
  if(ch->i2m_pos>=ch->i2m_len){
    if(ch->i2m_linear || (ch->i2m_lines<=1 && !ch->i2m_dw)){   /* single line, no surface: plain copy */
      if(ch->i2m_lines<=1 || !ch->i2m_pitch || ch->i2m_pitch==ch->i2m_w) gmem_write(ch->i2m_addr,ch->i2m_buf,ch->i2m_len);
      else for(u32 y=0;y<ch->i2m_lines;y++) gmem_write(ch->i2m_addr+(u64)y*ch->i2m_pitch,ch->i2m_buf+(u64)y*ch->i2m_w,ch->i2m_w);
      gpu_mem_written(ch->i2m_addr,ch->i2m_lines>1 && ch->i2m_pitch? (u64)ch->i2m_pitch*ch->i2m_lines : ch->i2m_len); }
    else { /* bl dst */
      u32 w=ch->i2m_w, h=ch->i2m_lines? ch->i2m_lines : 1; u32 wb=ch->i2m_dw? ch->i2m_dw : w, hh=ch->i2m_dh? ch->i2m_dh : h;
      u64 hi=0;
      for(u32 y=0;y<h;y++) for(u32 x=0;x<w;){ u32 xb=ch->i2m_x+x, n=16-(xb&15); if(n>w-x) n=w-x;
        u64 o=bl_offset(xb,ch->i2m_y+y,ch->i2m_z,wb,hh,ch->i2m_bh,ch->i2m_bd); gmem_write(ch->i2m_addr+o,ch->i2m_buf+(u64)y*w+x,n); if(o+n>hi) hi=o+n; x+=n; }
      gpu_mem_written(ch->i2m_addr,hi); }
  }
}

/* ============================================================ dispatch */
extern void r3d_pending_flush(void); extern int r3d_pending_on(void);

static void call_method(GpuChannel* ch, u32 sub, u32 m, u32 v, int last){
  if(m<0x40){ if(r3d_pending_on()) r3d_pending_flush(); puller_method(ch,m,v); if(m==0){ ch->sub_class[sub]=v&0xFFFF; } return; }
  u32 cls=ch->sub_class[sub];
  if(r3d_pending_on() && !(cls==0xB197 || (sub==0 && cls!=0xB1C0 && cls!=0xA140 && cls!=0x902D && cls!=0xB0B5))) r3d_pending_flush();   /* switching engines: flush pending draws */
  switch(cls){
    case 0xB197: maxwell3d_method(ch,m,v,last); break;
    case 0xB1C0: compute_method(ch,m,v,last); break;
    case 0xA140: kepler_i2m_method(ch,m,v); break;
    case 0x902D: fermi2d_method(ch,m,v); break;
    case 0xB0B5: dma_method(ch,m,v); break;
    default:
      if(sub==0) maxwell3d_method(ch,m,v,last);
      else if(sub==1) compute_method(ch,m,v,last);
      else if(sub==2) kepler_i2m_method(ch,m,v);
      else if(sub==3) fermi2d_method(ch,m,v);
      else if(sub==4) dma_method(ch,m,v);
      break;
  }
}

extern void r3d_const_run(GpuChannel* ch, u32 m, int inc, const u32* w, u32 n);
static int g_cb_runs=-1;   /* SWR_NO_CB_RUNS=1: word by word, as before */
static void run_pushbuffer(GpuChannel* ch, const u32* w, u32 n){
  if(g_cb_runs<0) g_cb_runs=getenv("SWR_NO_CB_RUNS")==NULL;
  /* method batches can spill over into the next GPFIFO entry (hw does this, so does yuzu's DmaPusher).
     keep the batch state across entries or the leftover data gets parsed as headers */
  u32 i=0;
  for(;;){
    if(ch->pb_k<ch->pb_cnt){
      u32 mode=ch->pb_mode, sub=ch->pb_sub, m=ch->pb_m;
      while(ch->pb_k<ch->pb_cnt && i<n){ u32 k=ch->pb_k++; u32 mm= mode==1? m+k : mode==5? (k==0? m : m+1) : m;
        call_method(ch,sub,mm,w[i++],ch->pb_k==ch->pb_cnt);
        /* constant data (cb_data): the rest of the run in one copy, after the first word took the whole path (it flushes pending draws) */
        if(mm>=0x8E4 && mm<=0x8F3 && g_cb_runs && ch->sub_class[sub]==0xB197){ u32 left=ch->pb_cnt-ch->pb_k, nm= mode==1? mm+1 : mode==5? m+1 : m; if(left>n-i) left=n-i;
          if(mode==1){ u32 room= mm<0x8F3? 0x8F3-mm : 0; if(left>room) left=room; }
          if(left>=4 && nm>=0x8E4 && nm<=0x8F3){ r3d_const_run(ch,nm,mode==1,w+i,left); i+=left; ch->pb_k+=left; } } }
      if(ch->pb_k<ch->pb_cnt){ static int said; if(said<6){ said++; LOG("gpu: method batch continues in the next GPFIFO entry (sub %u method %x mode %u, %u of %u words done)\n",ch->pb_sub,ch->pb_m,ch->pb_mode,ch->pb_k,ch->pb_cnt); } return; }    /* rest comes in the next entry */
    }
    if(i>=n) return;
    u32 h=w[i++];
    u32 mode=h>>29, sub=(h>>13)&7, m=h&0x1FFF, cnt=(h>>16)&0x1FFF;
    if(h && (mode==0 || mode==2 || mode>=6)){ /* shouldn't happen in a sane stream, log it */
      static int said; if(said<6){ said++; LOG("gpu: odd command header %08x (mode %u) at word %u of %u in channel %u; words around:",h,mode,i-1,n,ch->id);
        for(u32 j= i>5? i-5 : 0; j<i+6 && j<n; j++) LOG(" %s%08x",j==i-1? "*" : "",w[j]); LOG("\n"); } }
    switch(mode){
      case 1: case 3: case 5: ch->pb_mode=mode; ch->pb_sub=sub; ch->pb_m=m; ch->pb_cnt=cnt; ch->pb_k=0; break;
      /* modes 0/2 (old Fermi), 6, 7 carry no data words (same as DmaPusher). don't treat them as batches,
         a stray one would eat the following methods incl. syncpoint incrs */
      case 4: call_method(ch,sub,m,cnt,1); break;
      default: break;
    }
  }
}

static GpuChannel* chan_get(u32 id){
  for(int i=0;i<g_gpu.nchan;i++) if(g_gpu.chan[i]->id==id) return g_gpu.chan[i];
  if(g_gpu.nchan>=64){ static int said; if(!said){ said=1; WARN("gpu: more than 64 channels, sharing the last one\n"); } return g_gpu.chan[63]; }
  GpuChannel* c=calloc(1,sizeof *c); c->id=id; c->sub_class[0]=0xB197; c->sub_class[1]=0xB1C0; c->sub_class[2]=0xA140; c->sub_class[3]=0x902D; c->sub_class[4]=0xB0B5;
  /* hw reset values that NVN/deko3d rely on and never write. sRGB writes default to on,
     otherwise sRGB targets are stored linear and come out too dark */
  c->r3d[0x4B8]=1;
  g_gpu.chan[g_gpu.nchan++]=c; return c; }

void gpu_submit_now(u64* e, u32 n, u32 channel){
  pm_lock(&g_gpulock);
  GpuChannel* ch=chan_get(channel);
  for(u32 k=0;k<n;k++){
    u64 ent=e[k]; u64 va=ent&0xFFFFFFFFFCull; u32 words=(u32)((ent>>42)&0x1FFFFF);
    if(!words) continue;
    u32* buf=(u32*)gmem_ptr(va,(u64)words*4);
    if(buf) run_pushbuffer(ch,buf,words);
    else { u32* tmp=malloc((u64)words*4); gmem_read(va,tmp,(u64)words*4); run_pushbuffer(ch,tmp,words); free(tmp); }
  }
  r3d_pending_flush();
  gpu_flush(ch);
  pm_unlock(&g_gpulock);
}

void gpu_init(void){ pm_init(&g_gpulock); gpu3d_init(); }

/* with GL, submissions go to the GPU thread */
volatile u64 g_gpu_submits;

void gpu_submit(u64* e, u32 n, u32 channel, u32 incs){ g_gpu_submits++;
  extern int gpu_async_submit(u64* e, u32 n, u32 syncpt, u32 incs);
  if(gpu_async_submit(e,n,channel,incs)) return;
  gpu_submit_now(e,n,channel);
  for(u32 i=0;i<incs;i++) syncpt_incr(channel);
}
