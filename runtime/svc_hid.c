/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* hid. Games read the shared memory directly, so we fill it in the real layout (17 entry LIFOs). */
#include "rt.h"
#include "hid_layout.h"

KShmem* kshmem_new(u64 size, const char* name);
u8* kshmem_ptr(KShmem* s);
static KShmem* g_hid_shm;
PadState g_pad;
static u64 g_samp;
static u32 g_style_supported=0x7F;
static KEvent* g_style_ev[10];

static void lifo_push(u8* lifo, u32 esize, const void* state, u32 ssize, u64 samp){
  /* lifo header: unused, buffer_count, tail, count. entries: {u64 sampling_number; state} */
  u64 cnt=17, tail, count; memcpy(&tail,lifo+0x10,8); memcpy(&count,lifo+0x18,8);
  tail=(tail+1)%cnt; u8* e=lifo+0x20+tail*esize;
  memcpy(e,&samp,8); memcpy(e+8,state,ssize);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  memcpy(lifo+8,&cnt,8); memcpy(lifo+0x10,&tail,8); if(count<16) count++; memcpy(lifo+0x18,&count,8);
}

/* npad index: 0..7 = No1..No8, 8 = Handheld (0x20), 9 = Other (0x10) */
static void npad_setup(u8* base, int idx, u32 style, u32 devtype){
  u8* e=base+HO_HidSharedMemory_npad+idx*HS_HidNpadSharedMemoryEntry;
  memcpy(e+HO_HidNpadInternalState_style_set,&style,4);
  u32 jam=0; memcpy(e+HO_HidNpadInternalState_joy_assignment_mode,&jam,4);
  u32 col[3]={1,0xFF323232u,0xFF0A0A0A}; memcpy(e+HO_HidNpadInternalState_full_key_color,col,12);
  u32 jc[5]={1,0xFF3C3C3Cu,0xFF0A0A0A,0xFF3C3C3Cu,0xFF0A0A0A}; memcpy(e+HO_HidNpadInternalState_joy_color,jc,20);
  memcpy(e+HO_HidNpadInternalState_device_type,&devtype,4);
  u64 props=(1ull<<0)|(1ull<<1)|(1ull<<2)|(1ull<<3)|(1ull<<11)|(1ull<<12); /* charging, powered, dual joy... + abxy + plus/minus */
  memcpy(e+HO_HidNpadInternalState_system_properties,&props,8);
  u32 bat[3]={4,4,4}; memcpy(e+HO_HidNpadInternalState_battery_level,bat,12);
  u32 fattr=0; memcpy(e+HO_HidNpadInternalState_applet_footer_ui_attribute,&fattr,4);
  u8 ftype= idx==8? 4 : 3; e[HO_HidNpadInternalState_applet_footer_ui_type]=ftype;   /* handheld / pro controller */
}

void hid_update(void){
  if(!g_hid_shm) return;
  u8* b=kshmem_ptr(g_hid_shm);
  g_samp++;
  u8 st[0x28]; memset(st,0,sizeof st);
  u64 btn=g_pad.buttons;
  /* sticks also set the stick-direction button bits, like hw */
  if(g_pad.lx<-16000) btn|=1ull<<16; if(g_pad.ly>16000) btn|=1ull<<17; if(g_pad.lx>16000) btn|=1ull<<18; if(g_pad.ly<-16000) btn|=1ull<<19;
  if(g_pad.rx<-16000) btn|=1ull<<20; if(g_pad.ry>16000) btn|=1ull<<21; if(g_pad.rx>16000) btn|=1ull<<22; if(g_pad.ry<-16000) btn|=1ull<<23;
  memcpy(st+HO_HidNpadCommonState_sampling_number,&g_samp,8);
  memcpy(st+HO_HidNpadCommonState_buttons,&btn,8);
  i32 s4[4]={g_pad.lx,g_pad.ly,g_pad.rx,g_pad.ry}; memcpy(st+HO_HidNpadCommonState_analog_stick_l,s4,16);
  u32 attr=1; memcpy(st+HO_HidNpadCommonState_attributes,&attr,4);   /* IsConnected */
  /* No1 = pro / dual joycon, Handheld = attached */
  u8* e0=b+HO_HidSharedMemory_npad;
  lifo_push(e0+HO_HidNpadInternalState_full_key_lifo,HS_HidNpadCommonStateAtomicStorage,st,0x28,g_samp);
  lifo_push(e0+HO_HidNpadInternalState_joy_dual_lifo,HS_HidNpadCommonStateAtomicStorage,st,0x28,g_samp);
  lifo_push(e0+HO_HidNpadInternalState_handheld_lifo,HS_HidNpadCommonStateAtomicStorage,st,0x28,g_samp);
  u8* eh=b+HO_HidSharedMemory_npad+8*HS_HidNpadSharedMemoryEntry;
  lifo_push(eh+HO_HidNpadInternalState_handheld_lifo,HS_HidNpadCommonStateAtomicStorage,st,0x28,g_samp);
  lifo_push(eh+HO_HidNpadInternalState_full_key_lifo,HS_HidNpadCommonStateAtomicStorage,st,0x28,g_samp);
  /* unused npads still need a sampling number, the SDK looks at it */
  u8 z[0x28]; memset(z,0,sizeof z); memcpy(z,&g_samp,8);
  for(int i=1;i<10;i++){ if(i==8) continue; u8* e=b+HO_HidSharedMemory_npad+i*HS_HidNpadSharedMemoryEntry;
    lifo_push(e+HO_HidNpadInternalState_full_key_lifo,HS_HidNpadCommonStateAtomicStorage,z,0x28,g_samp); }
  /* six-axis: at rest, gravity down */
  u8 six[HS_HidSixAxisSensorState]; memset(six,0,sizeof six);
  memcpy(six,&g_samp,8); memcpy(six+8,&g_samp,8);
  float acc[3]={0,0,-1}; memcpy(six+HO_HidSixAxisSensorState_acceleration,acc,12);
  float dir[9]={1,0,0, 0,1,0, 0,0,1}; memcpy(six+HO_HidSixAxisSensorState_direction,dir,36);
  u32 sattr=1; memcpy(six+HO_HidSixAxisSensorState_attributes,&sattr,4);
  lifo_push(e0+HO_HidNpadInternalState_full_key_six_axis_sensor_lifo,HS_HidSixAxisSensorStateAtomicStorage,six,sizeof six,g_samp);
  lifo_push(e0+HO_HidNpadInternalState_joy_dual_left_six_axis_sensor_lifo,HS_HidSixAxisSensorStateAtomicStorage,six,sizeof six,g_samp);
  lifo_push(e0+HO_HidNpadInternalState_joy_dual_right_six_axis_sensor_lifo,HS_HidSixAxisSensorStateAtomicStorage,six,sizeof six,g_samp);
  lifo_push(eh+HO_HidNpadInternalState_handheld_six_axis_sensor_lifo,HS_HidSixAxisSensorStateAtomicStorage,six,sizeof six,g_samp);
  /* touch */
  u8 ts[HS_HidTouchScreenState]; memset(ts,0,sizeof ts); memcpy(ts,&g_samp,8);
  if(g_pad.touch){ u32 cnt=1; memcpy(ts+HO_HidTouchScreenState_count,&cnt,4);
    u8* t=ts+HO_HidTouchScreenState_touches; u64 dt=0; memcpy(t,&dt,8); u32 attr2=0, fid=0; memcpy(t+8,&attr2,4); memcpy(t+12,&fid,4);
    u32 xy[4]={(u32)g_pad.tx,(u32)g_pad.ty,20,20}; memcpy(t+16,xy,16); }
  lifo_push(b+HO_HidSharedMemory_touchscreen+HO_HidTouchScreenSharedMemoryFormat_lifo,HS_HidTouchScreenStateAtomicStorage,ts,sizeof ts,g_samp);
  /* keyboard/mouse/debug pad: only bump the counters */
  u8 kb[HS_HidKeyboardState]; memset(kb,0,sizeof kb); memcpy(kb,&g_samp,8);
  lifo_push(b+HO_HidSharedMemory_keyboard,8+HS_HidKeyboardState,kb,sizeof kb,g_samp);
  u8 ms[HS_HidMouseState]; memset(ms,0,sizeof ms); memcpy(ms,&g_samp,8);
  lifo_push(b+HO_HidSharedMemory_mouse,8+HS_HidMouseState,ms,sizeof ms,g_samp);
  u8 dp[HS_HidDebugPadState]; memset(dp,0,sizeof dp); memcpy(dp,&g_samp,8);
  lifo_push(b+HO_HidSharedMemory_debug_pad,8+HS_HidDebugPadState,dp,sizeof dp,g_samp);
}

static int hid_applet_res(SvcObj* o, IpcCtx* c){
  if(c->cmd==0){ out_copy_h(c,h_new(&g_hid_shm->o)); return 0; }
  return stub_ok(o,c); }

static int hid_vibration_list(SvcObj* o, IpcCtx* c){ (void)o; (void)c; return 0; }   /* IActiveVibrationDeviceList: 0 ActivateVibrationDevice */
static u64 g_hold_type=0, g_comm_mode=0, g_handheld_mode=0;

/* controller rumble: the newest HD rumble value the game sent for each controller (VibrationDeviceHandle: npad id in
   bits 8-15, 0x20 = handheld; device index in bits 16-23, 0 left, 1 right) as {low amplitude, low frequency, high
   amplitude, high frequency}. The backends read player 1's (npad 0 or handheld) as a standard pad's two motors
   (hid_rumble_get). Kept per controller: one slot per side let the zeros the game sends the other npads in the same
   SendVibrationValues batch overwrite player 1's, so nothing rumbled in game (GitHub #20) */
int g_opt_rumble=100;   /* ini [Input] rumble: strength in percent, 0 = off */
#define VIB_PADS 9      /* npads 0..7, then handheld */
static volatile float g_vib[VIB_PADS][2][4]; static volatile u64 g_vib_ns[VIB_PADS][2]; static u64 g_vib_n;
static volatile float g_vib_peak[2];   /* player 1's strongest low / high since the backend last read: a pulse shorter
                                          than the backend's refresh still reaches the motors */
static void vib_set(u32 handle, const float* v){
  u32 npad=(handle>>8)&0xFF, dev=(handle>>16)&0xFF; if(dev>1) dev=1;
  u32 p= npad==0x20? 8 : npad;
  if(p>=VIB_PADS) return;
  for(int i=0;i<4;i++) g_vib[p][dev][i]= v[i]==v[i] && v[i]>0? v[i] : 0;   /* NaN or negative: silent */
  g_vib_ns[p][dev]=plat_ns();
  if(p==0 || p==8){ if(g_vib[p][dev][0]>g_vib_peak[0]) g_vib_peak[0]=g_vib[p][dev][0]; if(g_vib[p][dev][2]>g_vib_peak[1]) g_vib_peak[1]=g_vib[p][dev][2]; }
  if(!g_vib_n++) LOG("input: the game sent its first rumble (npad %u, amplitudes %.2f low / %.2f high)\n",npad,v[0],v[2]); }
/* the pad's motors, 0..1: the heavy (low-frequency) one takes the stronger low band of player 1's sides, the light one
   the stronger high band, and at least the peak since the last read. A side not refreshed for 200 ms counts as stopped
   (the game stops sending when it ends). A weak effect (beam shots send 0.03) is raised to 0.1, which a pad's motors
   can be felt at; 0 stays 0 */
void hid_rumble_get(float* lo, float* hi){
  u64 now=plat_ns(); float l=g_vib_peak[0], h=g_vib_peak[1];
  g_vib_peak[0]=g_vib_peak[1]=0;
  for(int p=0;p<VIB_PADS;p+=8) for(int d=0;d<2;d++){ if(!g_vib_ns[p][d] || now-g_vib_ns[p][d]>200000000ull) continue;
    if(g_vib[p][d][0]>l) l=g_vib[p][d][0]; if(g_vib[p][d][2]>h) h=g_vib[p][d][2]; }
  if(l>0 && l<0.1f) l=0.1f;
  if(h>0 && h<0.1f) h=0.1f;
  float s= g_opt_rumble>0? g_opt_rumble/100.0f : 0; l*=s; h*=s;
  *lo= l>1? 1 : l; *hi= h>1? 1 : h;
  { static int said; if(!said && (*lo>0 || *hi>0)){ said=1; LOG("input: first rumble to the pad (%.2f low / %.2f high)\n",*lo,*hi); } } }

static int hid_disp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 0: out_obj(c,svcobj_new("IAppletResource",hid_applet_res,0)); return 0;
    case 40: out_copy_h(c,h_new(&kevent_new("xpad id",0)->o)); return 0;         /* AcquireXpadIdEventHandle */
    case 55: case 59: out64(c,0); return 0;                                        /* GetXpadIds: none */
    case 58: case 62: out_copy_h(c,h_new(&kshmem_new(0x1000,"hid lifo")->o)); return 0;
    case 68: case 82: out8(c,1); return 0;                                         /* IsSixAxisSensorFusionEnabled / AtRest */
    case 69: case 70: case 71: case 72: case 73: case 74: case 75: case 76: case 77: case 78: case 79: case 80: case 81: case 83: case 84: case 85: case 86: case 87: case 88: case 89:
      if(c->cmd==71||c->cmd==73||c->cmd==79||c->cmd==84||c->cmd==86||c->cmd==88){ out32(c,0); out32(c,0); } return 0;
    case 100: g_style_supported=in32(c,0); return 0;                               /* SetSupportedNpadStyleSet */
    case 101: out32(c,g_style_supported); return 0;
    case 106: { u32 id=in32(c,0); int i= id==0x20? 8 : id==0x10? 9 : (int)(id&7);
      if(!g_style_ev[i]){ g_style_ev[i]=kevent_new("npad style",0); kobj_ref(&g_style_ev[i]->o); kevent_signal(g_style_ev[i]); }
      out_copy_h(c,h_new(&g_style_ev[i]->o)); return 0; }
    case 108: { u32 id=in32(c,0); u64 led= id<8? (1ull<<id) : 0; out64(c,led); return 0; }   /* GetPlayerLedPattern */
    case 120: g_hold_type=in64(c,8); return 0;
    case 121: out64(c,g_hold_type); return 0;                                      /* GetNpadJoyHoldType */
    case 127: g_handheld_mode=in64(c,8); return 0;
    case 128: out64(c,g_handheld_mode); return 0;                                  /* GetNpadHandheldActivationMode */
    case 129: out32(c,0); return 0;                                                /* GetNpadFullKeyGripColor... */
    case 133: case 134: case 135: case 136: case 137: case 138: out8(c,0); return 0;
    case 200: { u32 h=in32(c,0); u32 dev=(h>>16)&0xFF; out32(c,1); out32(c,dev==0? 1 : 2); return 0; }  /* GetVibrationDeviceInfo: LRA, left/right */
    case 201: { float v[4]; for(int i=0;i<4;i++){ u32 w=in32(c,4+4*i); memcpy(&v[i],&w,4); } vib_set(in32(c,0),v); return 0; }   /* SendVibrationValue(handle, value, aruid) */
    case 206: if(c->nx>=2){ u64 nh=c->x[0].size/4, nv=c->x[1].size/16, n= nh<nv? nh : nv;   /* SendVibrationValues(aruid, handles[], values[]) */
        for(u64 i=0;i<n && i<8;i++){ float v[4]; memcpy(v,GP(c->x[1].addr+i*16),16); vib_set(gr32(c->x[0].addr+i*4),v); } } return 0;
    case 202: { u32 h=in32(c,0), dev=(h>>16)&0xFF, npad=(h>>8)&0xFF, p= npad==0x20? 8 : npad; if(dev>1) dev=1; if(p>=VIB_PADS) p=0;   /* GetActualVibrationValue: what was last sent */
      float v[4]={g_vib[p][dev][0], g_vib[p][dev][1]>0? g_vib[p][dev][1] : 160.f, g_vib[p][dev][2], g_vib[p][dev][3]>0? g_vib[p][dev][3] : 320.f}; outbytes(c,v,16); return 0; }
    case 203: out_obj(c,svcobj_new("IActiveVibrationDeviceList",hid_vibration_list,0)); return 0;
    case 205: case 211: out8(c,1); return 0;                                       /* IsVibrationPermitted / Mounted */
    case 208: out32(c,0); return 0;
    case 400: out8(c,0); return 0;                                                 /* IsUsbFullKeyControllerEnabled */
    case 1000: g_comm_mode=in64(c,8); return 0;
    case 1001: out64(c,g_comm_mode); return 0;
    case 1003: out8(c,0); return 0;
    case 102: case 103: case 104: case 107: case 109: case 122: case 123: case 124: case 125: case 126: case 130: case 131: case 132:
    case 204: case 207: case 209: case 210: case 212:
    case 1002: case 1004:
    case 300: case 301: case 302: case 303: case 304: case 305: case 306: case 307: case 308: case 309: case 310: case 311: case 312: case 313: case 314: case 315: case 316: case 317:
    case 60: case 61: case 63: case 64: case 65: case 66: case 67: case 91:
    case 1: case 11: case 21: case 31: case 32: case 51: case 56: return 0;
  }
  return stub_ok(o,c); }

static SvcObj* new_hid(void){ return svcobj_new("hid",hid_disp,0); }

static int hidsys_disp(SvcObj* o, IpcCtx* c){ (void)o; (void)c; return 0; }

static SvcObj* new_hidsys(void){ return svcobj_new("hid:sys",hidsys_disp,0); }

void svc_hid_init(void){
  g_hid_shm=kshmem_new(0x40000,"hid"); kobj_ref(&g_hid_shm->o);
  u8* b=kshmem_ptr(g_hid_shm);
  npad_setup(b,0,(1u<<0)|(1u<<2),(1u<<0)|(1u<<5));    /* No1: FullKey + JoyDual */
  npad_setup(b,8,1u<<1,(1u<<2)|(1u<<3));              /* Handheld */
  hid_update(); hid_update();
  svc_register("hid",new_hid); svc_register("hid:sys",new_hidsys);
}
