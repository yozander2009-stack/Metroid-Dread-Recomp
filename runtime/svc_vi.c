/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* vi + the nvnflinger buffer queue. QueueBuffer presents immediately, there's no compositor. */
#include "rt.h"

typedef struct { int used; u32 afmt; u32 nvmap_id; u32 w, h, stride, fmt, layout, bhl, pitch, offset; u64 size; u8 raw[0x16C]; u32 rawsize; int queued; } Slot;
static Slot slots[64];
u32 g_vi_buf_w, g_vi_buf_h;   /* the game's display buffer size (host1x.c: story videos at 16:9, #88) */
static KEvent* g_vsync_ev; static KEvent* g_bufavail_ev;
static PMutex g_vilock; static int g_connected;
static int g_width=1280, g_height=720;
static u64 g_present_count;
static PCond g_free_cv;

/* ============================================================ parcels */
typedef struct { u8* d; u32 size, pos; } PR;

static u32 pr_u32(PR* p){ u32 v=0; if(p->pos+4<=p->size) memcpy(&v,p->d+p->pos,4); p->pos+=4; return v; }

static void pr_skip_token(PR* p){ pr_u32(p); i32 n=(i32)pr_u32(p); if(n>0) p->pos+=((u32)(n+1)*2+3)&~3u; }
typedef struct { u8 d[0x800]; u32 n; } PW;

static void pw_u32(PW* w, u32 v){ memcpy(w->d+w->n,&v,4); w->n+=4; }

static void pw_bytes(PW* w, const void* p, u32 n){ memcpy(w->d+w->n,p,n); w->n+=(n+3)&~3u; }

static void pw_flat(PW* w, const void* p, u32 n){ pw_u32(w,n); pw_u32(w,0); pw_bytes(w,p,n); }

static void pw_out(PW* w, u64 addr, u64 size){
  u8* o=GP(addr); u32 hdr[4]={w->n,0x10,0,0x10+w->n}; if(size<0x10+w->n) return;
  memcpy(o,hdr,16); memcpy(o+0x10,w->d,w->n); }

/* ============================================================ present */
void gpu_present_slot(Slot* s);

static void present_slot(int i){
  Slot* s=&slots[i]; if(!s->used) return;
  extern void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt);
  vi_show_buffer(s->nvmap_id,s->offset,s->w,s->h,s->pitch,s->layout,s->bhl,s->afmt);
  g_present_count++;
}

static int binder_transact(u32 id, u32 code, PR* in, PW* out){
  (void)id;
  switch(code){
    case 10: { /* CONNECT */
      g_connected=1; u32 qo[4]={(u32)g_width,(u32)g_height,0,0}; pw_bytes(out,qo,16); pw_u32(out,0); return 0; }
    case 11: g_connected=0; pw_u32(out,0); return 0;
    case 14: { /* SET_PREALLOCATED_BUFFER */
      i32 slot=(i32)pr_u32(in); u32 has=pr_u32(in);
      if(slot>=0 && slot<64){ Slot* s=&slots[slot]; memset(s,0,sizeof *s);
        if(has){ u32 len=pr_u32(in); pr_u32(in); u8* g=in->d+in->pos;
          if(g_verbose>=2){ LOG("graphicbuffer len=%x:",len); for(int k=0;k<0x60;k+=4){ u32 q; memcpy(&q,g+k,4); LOG(" %x",q);} LOG("\n"); } if(len>sizeof s->raw) len=sizeof s->raw; memcpy(s->raw,g,len); s->rawsize=len;
          /* NvGraphicBuffer layout: hdr(12) unk0 nvmap_id unk2 magic pid type usage format ext_format stride total_size num_planes unk12 planes[] */
          /* GraphicBuffer: 'GBFR' + 10 words, followed by the NvGraphicBuffer minus its NativeHandle */
          u8* nb=g+0x1C; memcpy(&s->afmt,g+0x10,4);
          memcpy(&s->nvmap_id,nb+0x10,4); memcpy(&s->stride,nb+0x30,4);
          u8* pl=nb+0x40; memcpy(&s->w,pl,4); memcpy(&s->h,pl+4,4); u64 cf; memcpy(&cf,pl+8,8); s->fmt=(u32)cf; memcpy(&s->layout,pl+0x10,4);
          memcpy(&s->pitch,pl+0x14,4); memcpy(&s->offset,pl+0x1C,4); memcpy(&s->bhl,pl+0x24,4);
          s->used=1; g_vi_buf_w=s->w; g_vi_buf_h=s->h;
          LOG("vi: buffer %d  %ux%u pitch=%u layout=%u bhl=%u fmt=%x nvmap=%u\n",slot,s->w,s->h,s->pitch,s->layout,s->bhl,s->fmt,s->nvmap_id); } }
      pw_u32(out,0); return 0; }
    case 1: { /* REQUEST_BUFFER */
      i32 slot=(i32)pr_u32(in); Slot* s=(slot>=0&&slot<64)? &slots[slot] : NULL;
      if(s && s->used){ pw_u32(out,1); pw_flat(out,s->raw,s->rawsize); } else pw_u32(out,0);
      pw_u32(out,0); return 0; }
    case 3: { /* DEQUEUE_BUFFER */
      pm_lock(&g_vilock);
      int k=-1; u64 dq0=plat_ns();
      for(;;){ for(int i=0;i<64;i++) if(slots[i].used && !slots[i].queued){ k=i; break; } if(k>=0) break;
        pc_wait(&g_free_cv,&g_vilock,16000000); /* we release on present */
        for(int i=0;i<64;i++) slots[i].queued=0; }
      slots[k].queued=1;
      pm_unlock(&g_vilock);
      { u64 dq=plat_ns()-dq0; if(dq>250000000ull) LOG("vi: DEQUEUE_BUFFER waited %.0f ms for a free buffer\n",dq/1e6); }
      pw_u32(out,(u32)k); pw_u32(out,1);
      u32 fence[9]; memset(fence,0,sizeof fence); fence[1]=0xFFFFFFFFu; for(int i=1;i<4;i++) fence[1+i*2]=0xFFFFFFFFu;
      pw_flat(out,fence,0x24); pw_u32(out,0); return 0; }
    case 7: { /* QUEUE_BUFFER */
      i32 slot=(i32)pr_u32(in);
      /* wait on the frame's fence if the GPU isn't done with it yet */
      u32 len=pr_u32(in); pr_u32(in); u8* qi=in->d+in->pos;
      if(len>=0x54){ u32 nf; memcpy(&nf,qi+0x30,4); if(nf>4) nf=4;
        /* native Vulkan used to skip this wait (it puts the whole GPU frame on the game thread: measured 8 ms frames
           at a 144 cap). 2026-10-18: skipping it let the game write the next frame's data while the GPU still read this one: whole-screen
           flicker in the save room at 240 (worse with DLAA; seen by eye, OpenGL clean). So native Vulkan waits too;
           SWR_SNXVK_NO_FENCE_WAIT=1 is the old skip, for tests only */
        extern int g_snxvk_on; static int nw=-1; if(nw<0) nw=getenv("SWR_SNXVK_NO_FENCE_WAIT")!=NULL;
        /* 2026-10-23: waiting on the previous frame's fence instead (game one frame ahead) measured no gain at a 240 cap
           (232 vs 232 FPS), more >8 ms frames, and made the loading screen's spinner flicker on screen (the loading thread
           draws into the one buffer it queues, slot 0, while it is shown), so this waits on the current frame */
        if(!g_snxvk_on || !nw) for(u32 i=0;i<nf;i++){ u32 id,val; memcpy(&id,qi+0x34+i*8,4); memcpy(&val,qi+0x38+i*8,4); if(id!=0xFFFFFFFFu){ u64 f0=plat_ns(); int ok=syncpt_wait(id,val,100000000); u64 fw=plat_ns()-f0;
            if(fw>50000000ull) LOG("vi: QUEUE_BUFFER fence %u: syncpt %u wanted %u, now %u, waited %.0f ms%s\n",i,id,val,syncpt_read(id),fw/1e6,ok? "" : " (timed out)"); } } }
      if(len>=0x28){ extern volatile i32 g_vi_crop[4]; extern volatile u32 g_vi_transform; i32 cr[4]; memcpy(cr,qi+0x0C,16); for(int i=0;i<4;i++) g_vi_crop[i]=cr[i]; memcpy((void*)&g_vi_transform,qi+0x20,4);
        { static u32 lt=0xFFFFFFFFu; static i32 lc[4]; if(lt!=g_vi_transform || memcmp(lc,cr,16)){ lt=g_vi_transform; memcpy(lc,cr,16); LOG("vi: crop (%d,%d)-(%d,%d) transform %x\n",cr[0],cr[1],cr[2],cr[3],lt); } } }
      if(slot>=0 && slot<64){ u64 p0=plat_ns(); present_slot(slot); u64 pt=plat_ns()-p0; if(pt>250000000ull) LOG("vi: QUEUE_BUFFER present took %.0f ms\n",pt/1e6); }
      pm_lock(&g_vilock); if(slot>=0 && slot<64) slots[slot].queued=0; pc_broadcast(&g_free_cv); pm_unlock(&g_vilock);
      kevent_signal(g_bufavail_ev);
      u32 qo[4]={(u32)g_width,(u32)g_height,0,0}; pw_bytes(out,qo,16); pw_u32(out,0); return 0; }
    case 8: { i32 slot=(i32)pr_u32(in); pm_lock(&g_vilock); if(slot>=0&&slot<64) slots[slot].queued=0; pc_broadcast(&g_free_cv); pm_unlock(&g_vilock); pw_u32(out,0); return 0; }
    case 9: { u32 what=pr_u32(in); u32 v= what==0? (u32)g_width : what==1? (u32)g_height : what==2? 1 : what==3? 1 : 0; pw_u32(out,v); pw_u32(out,0); return 0; }
    case 2: case 13: case 12: pw_u32(out,0); return 0;
  }
  WARN("binder: code %u not implemented\n",code); pw_u32(out,0); return 0;
}

static int hos_binder(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 0: case 3: {
      u32 id=in32(c,0), code=in32(c,4);
      u64 ia,is,oa,os; buf_in(c,0,&ia,&is); buf_out(c,0,&oa,&os);
      PR in; in.d=GP(ia); u32 hdr[4]; memcpy(hdr,in.d,16); in.d+=hdr[1]; in.size=hdr[0]; in.pos=0;
      pr_skip_token(&in);
      PW* out=calloc(1,sizeof(PW));
      binder_transact(id,code,&in,out);
      pw_out(out,oa,os); free(out); return 0; }
    case 1: return 0;
    case 2: out_copy_h(c,h_new(&g_bufavail_ev->o)); return 0;
  }
  return stub_ok(o,c); }

static int sysdisp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 1200: out64(c,1); return 0;
    case 1202: case 1203: out64(c,(u64)g_width); out64(c,(u64)g_height); return 0;
    case 3200: { out32(c,(u32)g_width); out32(c,(u32)g_height); out32(c,0x42700000); out32(c,0); return 0; }
  }
  (void)o; return 0; }

static int mgrdisp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){ case 2010: { static u64 l=100; out64(c,l++); return 0; } }
  (void)o; return 0; }

static void native_window(IpcCtx* c, u32 binder){
  u64 a,s; buf_out(c,0,&a,&s); if(!a) return;
  /* HOS Parcel: flat_binder_object {type=2, flags, u64 handle, u64 0, char name[8]="dispdrv", ...} + object table {0} */
  u32 data[10]; memset(data,0,sizeof data); data[0]=2; data[1]=1; data[2]=binder; data[3]=0; memcpy(&data[6],"dispdrv\0",8);
  u32 hdr[4]={0x28,0x10,4,0x38}; u8* p=GP(a); memset(p,0,s<0x100?s:0x100); memcpy(p,hdr,16); memcpy(p+0x10,data,0x28);
}

static int appdisp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 100: case 103: out_obj(c,svcobj_new("IHOSBinderDriver",hos_binder,0)); return 0;
    case 101: out_obj(c,svcobj_new("ISystemDisplayService",sysdisp,0)); return 0;
    case 102: out_obj(c,svcobj_new("IManagerDisplayService",mgrdisp,0)); return 0;
    case 1000: { u64 a,s; buf_out(c,0,&a,&s); if(a){ u8 info[0x60]; memset(info,0,sizeof info); strcpy((char*)info,"Default"); info[0x40]=1; u64 lc=1,w=(u64)g_width,h=(u64)g_height; memcpy(info+0x48,&lc,8); memcpy(info+0x50,&w,8); memcpy(info+0x58,&h,8); memcpy(GP(a),info,s<0x60?s:0x60); } out64(c,1); return 0; }
    case 1010: case 1011: out64(c,1); return 0;
    case 1020: case 1101: return 0;
    case 1102: out64(c,(u64)g_width); out64(c,(u64)g_height); return 0;
    case 2020: { native_window(c,1); out64(c,0x3C); return 0; }
    case 2021: case 2031: return 0;
    case 2030: { static u64 l=200; out64(c,l++); native_window(c,1); out64(c,0x3C); return 0; }
    case 2101: return 0;
    case 2102: out64(c,in32(c,0)==2? 1 : 0); return 0;
    case 2450: case 2451: out64(c,0); out64(c,0); return 0;
    case 2460: out64(c,0x1000); out64(c,0x1000); return 0;
    case 5202: case 5203: out_copy_h(c,h_new(&g_vsync_ev->o)); return 0;
  }
  return stub_ok(o,c); }

static int vi_root(SvcObj* o, IpcCtx* c){
  if(c->cmd<=3){ out_obj(c,svcobj_new("IApplicationDisplayService",appdisp,0)); return 0; }
  return stub_ok(o,c); }

static SvcObj* new_vi(void){ return svcobj_new("vi",vi_root,0); }

void vi_vsync_tick(void){ kevent_signal(g_vsync_ev); }

void svc_vi_init(void){
  pm_init(&g_vilock); pc_init(&g_free_cv);
  g_vsync_ev=kevent_new("vsync",0); g_bufavail_ev=kevent_new("buffer",0); kevent_signal(g_bufavail_ev);
  kobj_ref(&g_vsync_ev->o); kobj_ref(&g_bufavail_ev->o);   /* kept by the runtime, see svc_core_init */
  if(g_opt_docked){ g_width=1920; g_height=1080; }
  svc_register("vi:u",new_vi); svc_register("vi:s",new_vi); svc_register("vi:m",new_vi);
}
