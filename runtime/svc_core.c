/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* sm, am, set, time, acc, apm, plus a bunch of stub services that just say ok (nifm, shop, prepo...) */
#include "rt.h"

KShmem* kshmem_new(u64 size, const char* name);
u8* kshmem_ptr(KShmem* s);

/* The language the game is given: one it has. A Switch never hands a game a language it doesn't list; it picks
   the nearest it has. Metroid Dread has no Portuguese (9, pt-BR 17): its text and button glyphs went missing
   (GitHub issue #15), so those get English (US), as on a console */
static int game_lang(void){ extern int g_opt_lang; int l=g_opt_lang; return l==9 || l==17? 1 : l; }
/* its language code. Chinese as current firmware names it (zh-Hans / zh-Hant): with zh-CN / zh-TW the game opened
   no localization file and showed raw text keys (GitHub #30). 0..17 as nn::settings::Language, the newer ones too */
static const char* game_lang_code(void){
  static const char* codes[]={"ja","en-US","fr","de","it","es","zh-Hans","ko","nl","pt","ru","zh-Hant","en-GB","fr-CA","es-419","zh-Hans","zh-Hant","pt-BR"};
  int l=game_lang(); return codes[l>=0 && l<18? l : 1]; }
/* ================================================================ sm */
static int sm_disp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 0: return 0;                                   /* RegisterClient */
    case 1: {                                           /* GetServiceHandle */
      char name[9]={0}; memcpy(name,c->raw,8);
      SvcObj* s=svc_open(name);
      if(!s){ WARN("sm: service '%s' does not exist in SwitchRecomp: using a stub\n",name);
        extern SvcObj* svc_generic(const char* n); s=svc_generic(name); }
      LOG("sm: GetServiceHandle(%s)\n",name);
      out_move_h(c,session_new(s)); return 0; }
    case 2: case 3: case 4: return 0;
  }
  return stub_ok(o,c);
}

static SvcObj* new_sm(void){ return svcobj_new("sm:",sm_disp,0); }

/* unknown service -> generic stub, everything succeeds */
typedef struct { char name[16]; } GenData;

static int generic_disp(SvcObj* o, IpcCtx* c){ extern int ipc_warn_once(const char*,u32); if(ipc_warn_once(((GenData*)o->data)->name,c->cmd)) WARN("  [not implemented] %s cmd %u -> ok\n",((GenData*)o->data)->name,c->cmd); return 0; }

SvcObj* svc_generic(const char* n){ SvcObj* o=svcobj_new("generic",generic_disp,sizeof(GenData)); strncpy(((GenData*)o->data)->name,n,15); o->name=((GenData*)o->data)->name; return o; }

static int quiet_ok(SvcObj* o, IpcCtx* c){ (void)o; (void)c; return 0; }

static SvcObj* quiet(const char* name){ return svcobj_new(name,quiet_ok,0); }

/* ================================================================ shared events */
static u32 ev_handle(KEvent* e){ return h_new(&e->o); }

/* ================================================================ am */
static KEvent* g_msg_event; static u32 g_msgq[16]; static int g_nmsg;
static KEvent* g_launch_ev; static KEvent* g_res_ev; static KEvent* g_opmode_ev;
static u64 g_user_id[2]={0x0123456789ABCDEFull,0xFEDCBA9876543210ull};

static void am_push_msg(u32 m){ pm_lock(&g_klock); if(g_nmsg<16) g_msgq[g_nmsg++]=m; pm_unlock(&g_klock); kevent_signal(g_msg_event); }

/* IStorage / IStorageAccessor */
typedef struct { u8* d; u64 size; } StorageData;

static int storage_acc_disp(SvcObj* o, IpcCtx* c){
  StorageData* s=(StorageData*)(size_t)o->u[0];
  switch(c->cmd){
    case 0: out64(c,s->size); return 0;
    /* compare against size-off: off+n can wrap around for a huge off */
    case 10: { u64 off=in64(c,0), a, n; buf_in(c,0,&a,&n); if(off>s->size || n>s->size-off) return RC(128,503); memcpy(s->d+off,GP(a),n); return 0; }
    case 11: { u64 off=in64(c,0), a, n; buf_out(c,0,&a,&n); if(off>s->size) return RC(128,503); if(n>s->size-off) n=s->size-off; memcpy(GP(a),s->d+off,n); return 0; }
  }
  return stub_ok(o,c); }

static int storage_disp(SvcObj* o, IpcCtx* c){
  if(c->cmd==0 || c->cmd==1){ SvcObj* a=svcobj_new("IStorageAccessor",storage_acc_disp,0); a->u[0]=(u64)(size_t)o->data; o->refs++; out_obj(c,a); return 0; }
  return stub_ok(o,c); }

SvcObj* am_storage_new(u64 size, const void* init){
  SvcObj* o=svcobj_new("IStorage",storage_disp,sizeof(StorageData)); StorageData* s=o->data; s->d=calloc(1,size?size:1); s->size=size;
  if(init) memcpy(s->d,init,size); return o; }

static int am_csg(SvcObj* o, IpcCtx* c){           /* ICommonStateGetter */
  switch(c->cmd){
    case 0: out_copy_h(c,ev_handle(g_msg_event)); return 0;
    case 1: { pm_lock(&g_klock);
      if(!g_nmsg){ g_msg_event->signaled=0; pm_unlock(&g_klock); return RC(128,3); }
      u32 m=g_msgq[0]; memmove(g_msgq,g_msgq+1,(--g_nmsg)*4); if(!g_nmsg) g_msg_event->signaled=0; pm_unlock(&g_klock);
      out32(c,m); return 0; }
    case 5: out8(c,(u8)g_opt_docked); return 0;                      /* GetOperationMode */
    case 6: out32(c,(u32)g_opt_docked); return 0;                    /* GetPerformanceMode */
    case 8: out8(c,0); return 0;                                     /* GetBootMode */
    case 9: out8(c,1); return 0;                                     /* GetCurrentFocusState: InFocus */
    case 13: out_copy_h(c,ev_handle(kevent_new("sleep lock",0))); return 0;
    case 10: case 11: case 12: case 50: case 51: case 52: case 53: case 54: case 55: case 59: case 66: case 67: case 68: case 900: return 0;
    case 60: out32(c,g_opt_docked? 1920 : 1280); out32(c,g_opt_docked? 1080 : 720); return 0;   /* GetDefaultDisplayResolution */
    case 61: out_copy_h(c,ev_handle(g_res_ev)); return 0;
    case 62: out32(c,g_opt_docked? 1920 : 1280); out32(c,g_opt_docked? 1080 : 720); return 0;
    case 63: out_copy_h(c,ev_handle(g_res_ev)); return 0;
    case 100: out8(c,0); return 0;
    case 300: out32(c,0); return 0;                                  /* GetSettingsPlatformRegion */
    case 400: case 401: case 403: out_copy_h(c,ev_handle(kevent_new("csg ev",0))); return 0;
  }
  return stub_ok(o,c); }

static int am_self(SvcObj* o, IpcCtx* c){          /* ISelfController */
  switch(c->cmd){
    case 0: { extern void process_exit(int); process_exit(0); return 0; }   /* Exit: the system ends the process, libnx just sleeps waiting for it */
    case 9: out_copy_h(c,ev_handle(g_launch_ev)); return 0;            /* GetLibraryAppletLaunchableEvent */
    case 40: { static u64 layer=1; out64(c,layer++); return 0; }       /* CreateManagedDisplayLayer */
    case 41: out64(c,0); return 0;
    case 44: out64(c,1); out64(c,2); return 0;
    case 62: return 0;
    case 63: out32(c,0); return 0;                                     /* GetIdleTimeDetectionExtension */
    case 69: out8(c,0); return 0;                                      /* IsAutoSleepDisabled */
    case 90: out64(c,0); return 0;                                     /* GetAccumulatedSuspendedTickValue */
    case 91: out_copy_h(c,ev_handle(kevent_new("suspend",0))); return 0;
    case 100: case 110: case 120: return 0;
  }
  return quiet_ok(o,c); }

static int am_window(SvcObj* o, IpcCtx* c){
  switch(c->cmd){ case 1: out64(c,0x1234); return 0; case 10: case 11: case 12: return 0; }
  return stub_ok(o,c); }

static int am_appfn(SvcObj* o, IpcCtx* c){         /* IApplicationFunctions */
  switch(c->cmd){
    case 1: { u32 kind=in32(c,0);
      if(kind==2){ /* PreselectedUser */
        static int given; if(given) return RC(128,2); given=1;
        u8 buf[0x88]; memset(buf,0,sizeof buf); u32 magic=0xC79497CA; memcpy(buf,&magic,4); u32 one=1; memcpy(buf+4,&one,4); memcpy(buf+8,g_user_id,16);
        out_obj(c,am_storage_new(sizeof buf,buf)); return 0; }
      return RC(128,2); }
    case 10: out_obj(c,am_storage_new(in64(c,0),NULL)); return 0;
    case 20: out64(c,0); return 0;                                     /* EnsureSaveData */
    case 21: { u64 v=0; const char* s=game_lang_code(); memcpy(&v,s,strlen(s)); out64(c,v); return 0; }   /* GetDesiredLanguage */
    case 25: out64(c,0); return 0;                                     /* ExtendSaveData */
    case 26: out64(c,0x4000000); out64(c,0x4000000); return 0;       /* GetSaveDataSize */
    case 110: out32(c,0); return 0;
    case 22: case 27: case 28: case 32: case 33: case 34: case 35: case 36: case 37: case 66: case 67: case 68: case 90: case 100: case 101: case 120: case 124: return 0;
    case 151: return RC(128,2);                                      /* TryPopFromNotificationStorageChannel: nothing queued */
    case 23: { char v[16]="1.0.0"; outbytes(c,v,16); return 0; }         /* GetDisplayVersion */
    case 40: out8(c,0); return 0;                                      /* NotifyRunning */
    case 50: out64(c,0x1111222233334444ull); out64(c,0x5555666677778888ull); return 0;
    case 60: case 61: return 0;
    case 130: case 140: case 150: case 160: case 210: out_copy_h(c,ev_handle(kevent_new("appfn ev",0))); return 0;   /* events (210 is new in recent SDKs) */
    case 141: return RC(128,2);
    case 170: out8(c,0); return 0;
  }
  return stub_ok(o,c); }

/* library applets (swkbd, error...) finish immediately with an empty result */
static int am_lib_accessor(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 0: { KEvent* e=kevent_new("applet state",0); e->signaled=1; out_copy_h(c,h_new(&e->o)); return 0; }
    case 1: out8(c,1); return 0;
    case 10: case 20: case 25: case 30: case 100: case 103: case 50: case 60: case 150: return 0;
    case 101: case 104: { if(o->u[1]++) return RC(128,2);
      /* applet output. web applets (Web, Shop, OfflineWeb, LoginShare, WifiWebAuth) return a 0x1010 byte
         WebCommonReturnValue (exit reason 0, last url, url size); callers check the size */
      u32 id=(u32)o->u[2]; u64 n=0x10;
      if(id==0x13 || id==0x14 || id==0x17 || id==0x18 || id==0x19) n=0x1010;
      SvcObj* st=am_storage_new(n,NULL);
      { static int said; if(said<4){ said++; LOG("applet: library applet %02x closed right away (no applet UI here), %llu bytes of output\n",id,(unsigned long long)n); } }
      out_obj(c,st); return 0; }
    case 105: case 106: { KEvent* e=kevent_new("applet data",0); e->signaled=1; out_copy_h(c,h_new(&e->o)); return 0; }
    case 120: out64(c,0); return 0;
  }
  return 0; }

static int am_lac(SvcObj* o, IpcCtx* c){          /* ILibraryAppletCreator */
  switch(c->cmd){
    case 0: { SvcObj* a=svcobj_new("ILibraryAppletAccessor",am_lib_accessor,0); a->u[2]=in32(c,0); out_obj(c,a); return 0; } case 10: out_obj(c,am_storage_new(in64(c,0),NULL)); return 0; case 11: out_obj(c,am_storage_new(in64(c,0),NULL)); return 0; }
  return stub_ok(o,c); }

static int am_audio(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==1){ out32(c,0x3F800000); return 0; } if(c->cmd==2){ out32(c,0x3F800000); out32(c,0x3F800000); return 0; } return 0; }

static int am_display(SvcObj* o, IpcCtx* c){ (void)o; (void)c; return 0; }

static int am_debug(SvcObj* o, IpcCtx* c){ (void)o; (void)c; return 0; }

static int am_proxy(SvcObj* o, IpcCtx* c){        /* IApplicationProxy */
  switch(c->cmd){
    case 0: out_obj(c,svcobj_new("ICommonStateGetter",am_csg,0)); return 0;
    case 1: out_obj(c,svcobj_new("ISelfController",am_self,0)); return 0;
    case 2: out_obj(c,svcobj_new("IWindowController",am_window,0)); return 0;
    case 3: out_obj(c,svcobj_new("IAudioController",am_audio,0)); return 0;
    case 4: out_obj(c,svcobj_new("IDisplayController",am_display,0)); return 0;
    case 10: out_obj(c,quiet("IProcessWindingController")); return 0;
    case 11: out_obj(c,svcobj_new("ILibraryAppletCreator",am_lac,0)); return 0;
    case 20: out_obj(c,svcobj_new("IApplicationFunctions",am_appfn,0)); return 0;
    case 1000: out_obj(c,svcobj_new("IDebugFunctions",am_debug,0)); return 0;
  }
  return stub_ok(o,c); }

static int am_oe(SvcObj* o, IpcCtx* c){
  if(c->cmd==0){ out_obj(c,svcobj_new("IApplicationProxy",am_proxy,0)); return 0; }
  return stub_ok(o,c); }

static SvcObj* new_appletoe(void){ return svcobj_new("appletOE",am_oe,0); }

static SvcObj* new_appletae(void){ return svcobj_new("appletAE",am_oe,0); }

/* ================================================================ set */
static int set_disp(SvcObj* o, IpcCtx* c){
  static const char* codes[]={"ja","en-US","fr","de","it","es","zh-CN","ko","nl","pt","ru","zh-TW","en-GB","fr-CA","es-419","zh-Hans","zh-Hant","pt-BR"};
  switch(c->cmd){
    case 0: { u64 v=0; const char* s=game_lang_code(); memcpy(&v,s,strlen(s)); out64(c,v); return 0; }     /* GetLanguageCode */
    case 1: case 5: { u64 a,s; buf_out(c,0,&a,&s); u32 n=(u32)(s/8); u32 max= c->cmd==1? 15 : 18; if(n>max) n=max;
      for(u32 i=0;i<n;i++){ u64 v=0; memcpy(&v,codes[i],strlen(codes[i])); gw64(a+i*8,v); } out32(c,n); return 0; }
    case 2: { u32 idx=in32(c,0); u64 v=0; if(idx<18) memcpy(&v,codes[idx],strlen(codes[idx])); out64(c,v); return 0; }   /* MakeLanguageCode */
    case 3: out32(c,15); return 0;
    case 6: out32(c,18); return 0;
    case 4: out32(c,1); return 0;                     /* GetRegionCode: usa */
    case 7: case 9: { u64 a,s; buf_out(c,0,&a,&s); memset(GP(a),0,s); return 0; }
    case 8: out32(c,0); return 0;
    case 11: { u64 a,s; buf_out(c,0,&a,&s); memset(GP(a),0,s); strcpy((char*)GP(a),"SwitchRecomp"); return 0; }
  }
  return stub_ok(o,c); }

static SvcObj* new_set(void){ return svcobj_new("set",set_disp,0); }

static int setsys_disp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 3: case 4: { u64 a,s; buf_out(c,0,&a,&s); u8 fw[0x100]; memset(fw,0,sizeof fw); fw[0]=12; fw[1]=1; fw[2]=0; fw[4]=12; fw[5]=1; fw[6]=0; fw[7]=0;
      strcpy((char*)fw+8,"NintendoSDK Firmware for NX"); strcpy((char*)fw+0x28,"12.1.0-1.0"); strcpy((char*)fw+0x68,"12.1.0"); strcpy((char*)fw+0x80,"NintendoSDK Firmware for NX 12.1.0-1.0");
      memcpy(GP(a),fw,s<0x100?s:0x100); return 0; }
    case 23: out32(c,0); return 0;                  /* GetColorSetId */
    case 38: case 62: out32(c,0); return 0;
    case 37: { u64 a,s; buf_out(c,0,&a,&s); memset(GP(a),0,s); out64(c,0); return 0; }  /* GetSettingsItemValueSize */
    case 90: case 91: out32(c,0); return 0;
  }
  return quiet_ok(o,c); }

static SvcObj* new_setsys(void){ return svcobj_new("set:sys",setsys_disp,0); }

/* ================================================================ time */
static KShmem* g_time_shm;

static int sysclock_disp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 0: out64(c,(u64)plat_unix_time()); return 0;
    case 1: return 0;
    case 2: { u8 ctx[0x20]; memset(ctx,0,sizeof ctx); i64 t=plat_unix_time(); memcpy(ctx,&t,8); outbytes(c,ctx,0x20); return 0; }
    case 3: return 0;
    case 4: out_copy_h(c,ev_handle(kevent_new("clock op",0))); return 0;   /* GetOperationEventReadableHandle */
  }
  return stub_ok(o,c); }
static u8 g_clock_id[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};

static int steady_disp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 0: { u64 s=plat_ns()/1000000000ull; out64(c,s); outbytes(c,g_clock_id,16); return 0; }
    case 2: case 3: out64(c,0); return 0;
    case 100: out64(c,0); return 0;
    case 200: out64(c,0); out64(c,0); return 0;
    case 300: out64(c,0); return 0;
  }
  return stub_ok(o,c); }
static i64 g_tz_off;

static void to_calendar(i64 t, IpcCtx* c){
  i64 days=t/86400, rem=t%86400; if(rem<0){ rem+=86400; days--; }
  int hh=(int)(rem/3600), mm=(int)(rem%3600/60), ss=(int)(rem%60);
  /* days -> y/m/d */
  i64 z=days+719468; i64 era=(z>=0?z:z-146096)/146097; unsigned doe=(unsigned)(z-era*146097);
  unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365; i64 y=(i64)yoe+era*400; unsigned doy=doe-(365*yoe+yoe/4-yoe/100);
  unsigned mp=(5*doy+2)/153; unsigned d=doy-(153*mp+2)/5+1; unsigned m=mp<10?mp+3:mp-9; y+= m<=2;
  u16 yy=(u16)y; outbytes(c,&yy,2); out8(c,(u8)m); out8(c,(u8)d); out8(c,(u8)hh); out8(c,(u8)mm); out8(c,(u8)ss); out8(c,0);
  /* CalendarAdditionalInfo */
  u32 wday=(u32)((days%7+11)%7); out32(c,wday); u32 yday=0; out32(c,yday); char tz[8]="UTC"; outbytes(c,tz,8); out32(c,0); out32(c,(u32)(i32)g_tz_off);
}

static int tz_disp(SvcObj* o, IpcCtx* c){
  { static int init; if(!init){ init=1; g_tz_off=plat_tz_offset(); LOG("time: PC time zone UTC%+.1f\n",g_tz_off/3600.0); } }
  switch(c->cmd){
    case 0: { char n[0x24]; memset(n,0,sizeof n); strcpy(n,"UTC"); outbytes(c,n,0x24); return 0; }
    case 1: return 0;
    case 2: out32(c,1); return 0;
    case 3: { out32(c,1); return 0; }
    case 4: { u64 a,s; buf_out(c,0,&a,&s); memset(GP(a),0,s); return 0; }
    case 5: { char v[16]="2021a"; outbytes(c,v,16); return 0; }
    /* guest wall clock = host local time */
    case 100: case 101: { i64 t=(i64)in64(c,0)+g_tz_off; to_calendar(t,c); return 0; }
    case 201: case 202: { u64 a,s; buf_out(c,0,&a,&s); /* rough inverse */
      u16 y; memcpy(&y,c->raw,2); u8 mo=c->raw[2], d=c->raw[3], h=c->raw[4], mi=c->raw[5], se=c->raw[6];
      i64 yy=y-(mo<=2); i64 era=(yy>=0?yy:yy-399)/400; unsigned yoe=(unsigned)(yy-era*400); unsigned doy=(153*(mo+(mo>2?-3:9))+2)/5+d-1;
      unsigned doe=yoe*365+yoe/4-yoe/100+doy; i64 days=era*146097+(i64)doe-719468; i64 t=days*86400+h*3600+mi*60+se;
      t-=g_tz_off; if(a && s>=8) gw64(a,(u64)t); out32(c,1); return 0; }
  }
  return stub_ok(o,c); }

static void time_shm_update(void){
  if(!g_time_shm) return; u8* p=kshmem_ptr(g_time_shm);
  /* steady clock {i64 base_time_ns; u8 id[16]} x2 @0x08/0x20; system clocks {i64 offset; i64 time_point; u8 id[16]} x2 */
  u32 cnt=0; memcpy(p,&cnt,4);
  for(int k=0;k<2;k++){ i64 base=0; memcpy(p+8+k*0x18,&base,8); memcpy(p+8+k*0x18+8,g_clock_id,16); }
  extern u64 g_t0_ns(void);
  i64 elapsed=(i64)((plat_ns()-g_t0_ns())/1000000000ull);
  i64 off=plat_unix_time()-elapsed;
  for(int c2=0;c2<2;c2++){ u8* q=p+(c2? 0x80 : 0x38); memcpy(q,&cnt,4);
    for(int k=0;k<2;k++){ u8* e=q+8+k*0x20; i64 tp=0; memcpy(e,&off,8); memcpy(e+8,&tp,8); memcpy(e+16,g_clock_id,16); } }
  /* auto correction {u32 counter; bool[2]} @0xC8 */
  memcpy(p+0xC8,&cnt,4); p[0xCC]=1; p[0xCD]=1;
  /* ContinuousAdjustmentTimePoint (17.0+) {u32 counter; pad; 2 x {rtc_offset, diff_scale, shift+pad, lower{t,id}, upper{t,id}}} @0xD0 */
  memcpy(p+0xD0,&cnt,4);
  for(int k=0;k<2;k++){ u8* e=p+0xD8+k*0x48; memset(e,0,0x48); memcpy(e+0x18+8,g_clock_id,16); memcpy(e+0x30+8,g_clock_id,16); }
}

static int time_disp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 0: case 1: case 4: out_obj(c,svcobj_new("ISystemClock",sysclock_disp,0)); return 0;
    case 2: out_obj(c,svcobj_new("ISteadyClock",steady_disp,0)); return 0;
    case 3: out_obj(c,svcobj_new("ITimeZoneService",tz_disp,0)); return 0;
    case 5: out_obj(c,svcobj_new("IEphemeralNetworkSystemClock",sysclock_disp,0)); return 0;
    case 20: { if(!g_time_shm){ g_time_shm=kshmem_new(0x1000,"time"); kobj_ref(&g_time_shm->o); time_shm_update(); } out_copy_h(c,h_new(&g_time_shm->o)); return 0; }
    case 100: case 101: out8(c,1); return 0;
    case 200: out8(c,1); return 0;
    case 300: { i64 t=plat_unix_time(); out64(c,(u64)t); return 0; }
    case 400: out8(c,0); return 0;
    case 500: case 501: { u8 ctx[0x20]; memset(ctx,0,0x20); outbytes(c,ctx,0x20); return 0; }
  }
  return stub_ok(o,c); }

static SvcObj* new_time(void){ return svcobj_new("time",time_disp,0); }

/* ================================================================ acc */
static int profile_disp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 0: case 1: {
      if(c->cmd==0){ u64 a,s; buf_out(c,0,&a,&s); if(a){ memset(GP(a),0,s); } }
      u8 base[0x38]; memset(base,0,sizeof base); memcpy(base,g_user_id,16); u64 ts=1600000000; memcpy(base+0x10,&ts,8); strcpy((char*)base+0x18,"Player");
      outbytes(c,base,0x38); return 0; }
    case 10: out32(c,0); return 0;
    case 11: out32(c,0); return 0;
  }
  return stub_ok(o,c); }

static int acc_disp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 0: out32(c,1); return 0;
    case 1: out8(c,1); return 0;
    case 2: case 3: { u64 a,s; buf_out(c,0,&a,&s); if(a){ memset(GP(a),0,s); if(s>=16) memcpy(GP(a),g_user_id,16); } return 0; }
    case 4: outbytes(c,g_user_id,16); return 0;
    case 5: out_obj(c,svcobj_new("IProfile",profile_disp,0)); return 0;
    case 50: out8(c,0); return 0;
    case 51: outbytes(c,g_user_id,16); return 0;
    case 60: out32(c,1); return 0;
    case 100: case 140: case 150: case 111: return 0;
    case 101: out_obj(c,svc_generic("IManagerForApplication")); return 0;
    case 110: return 0;
    case 130: case 131: return 0;
  }
  return stub_ok(o,c); }

static SvcObj* new_acc(void){ return svcobj_new("acc:u0",acc_disp,0); }

/* ================================================================ apm and misc */
static int apm_session(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==1) out32(c,0x00020003); return 0; }

static int apm_disp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){ case 0: out_obj(c,svcobj_new("apm:ISession",apm_session,0)); return 0; case 1: out32(c,(u32)g_opt_docked); return 0; case 6: out8(c,0); return 0; }
  return stub_ok(o,c); }

static SvcObj* new_apm(void){ return svcobj_new("apm",apm_disp,0); }

static int lm_logger(SvcObj* o, IpcCtx* c){
  (void)o;
  if(c->cmd==0){ u64 a,s; buf_in(c,0,&a,&s); if(a && s>0x18){ /* lm packet: 0x18 hdr, then (type,len,data) fields */
      u8* p=GP(a); u64 i=0x18; char msg[1024]; int n=0;
      while(i+2<s){ u8 t=p[i], l=p[i+1]; if(i+2+l>s) break; if(t==2 && n+l<1000){ memcpy(msg+n,p+i+2,l); n+=l; } i+=2+l; }
      if(n){ msg[n]=0; LOG("[game lm] %s%s",msg,(msg[n-1]=='\n')?"":"\n"); } } }
  return 0; }

static int lm_disp(SvcObj* o, IpcCtx* c){ if(c->cmd==0){ out_obj(c,svcobj_new("ILogger",lm_logger,0)); return 0; } return quiet_ok(o,c); }

static SvcObj* new_lm(void){ return svcobj_new("lm",lm_disp,0); }

static int pctl_obj(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){ case 1001: case 1002: case 1004: return 0; case 1031: out8(c,0); return 0; case 1032: out32(c,0); return 0; case 1403: out8(c,0); return 0;
    case 1018: out8(c,1); return 0;                                    /* IsFreeCommunicationAvailable */
    case 1039: out32(c,0); return 0;
    case 1065: out8(c,1); return 0;                                    /* IsStereoVisionPermitted */
    case 1453: out8(c,0); return 0;                                    /* IsPlayTimerEnabled */
    case 1473: out8(c,0); return 0;
    case 1432: case 1433: case 1434: case 1457: out_copy_h(c,ev_handle(kevent_new("pctl ev",0))); return 0; }
  return 0; }

static int pctl_disp(SvcObj* o, IpcCtx* c){ if(c->cmd==0||c->cmd==1){ out_obj(c,svcobj_new("IParentalControlService",pctl_obj,0)); return 0; } return quiet_ok(o,c); }

static SvcObj* new_pctl(void){ return svcobj_new("pctl",pctl_disp,0); }

static int nifm_req(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){ case 0: out32(c,1); return 0; case 1: return RC(110,300);
    case 2: { KEvent* a=kevent_new("nifm0",0); KEvent* b=kevent_new("nifm1",0); a->signaled=1; out_copy_h(c,ev_handle(a)); out_copy_h(c,ev_handle(b)); return 0; }
    case 3: case 4: case 5: case 6: case 11: case 21: return 0; }
  return 0; }

static int nifm_gen(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){ case 4: out_obj(c,svcobj_new("IRequest",nifm_req,0)); return 0; case 2: out_obj(c,svcobj_new("IScanRequest",nifm_req,0)); return 0; case 5: case 12: return RC(110,300);
    case 18: out32(c,0); return 0; case 21: out8(c,0); return 0; case 22: out8(c,0); return 0; case 1: outbytes(c,"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10",16); return 0; }
  return 0; }

static int nifm_disp(SvcObj* o, IpcCtx* c){ if(c->cmd==4||c->cmd==5){ out_obj(c,svcobj_new("IGeneralService",nifm_gen,0)); return 0; } return quiet_ok(o,c); }

static SvcObj* new_nifm(void){ return svcobj_new("nifm",nifm_disp,0); }

static int friend_svc(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==0){ out_copy_h(c,ev_handle(kevent_new("friend",0))); return 0; } if(c->cmd>=10100 && c->cmd<=10102){ out32(c,0); return 0; } return 0; }

static int friend_notif(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==0){ out_copy_h(c,ev_handle(kevent_new("friend notif",0))); return 0; } if(c->cmd==2) return RC(121,15); return 0; }

static int friend_disp(SvcObj* o, IpcCtx* c){ if(c->cmd==0){ out_obj(c,svcobj_new("IFriendService",friend_svc,0)); return 0; } if(c->cmd==1){ out_obj(c,svcobj_new("INotificationService",friend_notif,0)); return 0; }
  if(c->cmd==2){ out_obj(c,quiet("IDaemonSuspendSessionService")); return 0; } return quiet_ok(o,c); }

static SvcObj* new_friend(void){ return svcobj_new("friend",friend_disp,0); }

static int ns_disp(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){ case 7996: case 7989: case 7995: case 7988: case 7994: case 7997: case 7998: case 7999: case 7991: out_obj(c,quiet("ns:sub")); return 0; }
  return 0; }

static SvcObj* new_ns(void){ return svcobj_new("ns",ns_disp,0); }

/* audio: pretend, no output */
static KEvent* g_aud_ev;
static void aout_tick(void);

/* renderer event fires every 5ms (240 samples @ 48k) like HOS */
static void audio_clock(void* arg){ plat_diag_role("audio-clock"); (void)arg; u64 next=plat_ns();
  for(;;){ next+=5000000ull; u64 now=plat_ns(); if(next>now) plat_sleep_ns((i64)(next-now)); else if(now-next>50000000ull) next=now;
    kevent_signal(g_aud_ev);
    aout_tick(); } }

static int audren_obj(SvcObj* o, IpcCtx* c){
  extern int audren_update(SvcObj* o, IpcCtx* c);
  switch(c->cmd){
    case 0: out32(c,(u32)o->u[0]); return 0;
    case 1: out32(c,(u32)o->u[1]); return 0;
    case 2: out32(c,(u32)o->u[2]); return 0;
    case 3: out32(c,(u32)o->u[3]); return 0;
    case 4: case 10: return audren_update(o,c);
    case 5: o->u[3]=0; return 0;
    case 6: o->u[3]=1; return 0;
    case 7: out_copy_h(c,ev_handle(g_aud_ev)); return 0;
    case 8: case 9: return 0;
    case 11: out32(c,0); return 0;
  }
  return quiet_ok(o,c); }

static int auddev_disp(SvcObj* o, IpcCtx* c){ (void)o;
  static const char* nm="AudioTvOutput";
  switch(c->cmd){
    case 0: case 6: { u64 a,s; if(buf_out(c,0,&a,&s) && a && s>=0x100){ memset(GP(a),0,0x100); strcpy((char*)GP(a),nm); out32(c,1); } else out32(c,0); return 0; }
    case 2: case 8: out32(c,0x3F800000); return 0;
    case 3: case 10: case 13: { u64 a,s; if(buf_out(c,0,&a,&s) && a){ memset(GP(a),0,s); strncpy((char*)GP(a),nm,s); } return 0; }
    case 4: case 11: case 12: { KEvent* e=kevent_new("audio device",0); e->signaled=1; out_copy_h(c,ev_handle(e)); return 0; }
    case 5: out32(c,2); return 0;
  }
  return 0; }

static int audren_disp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 0: { SvcObj* r=svcobj_new("IAudioRenderer",audren_obj,0x100);
      memcpy(r->data,c->raw,0x34<0x100?0x34:0x100);
      r->u[0]=in32(c,0); r->u[1]=in32(c,4); r->u[2]=in32(c,8); r->u[3]=1;
      out_obj(c,r); return 0; }
    case 1: { u64 sz=0x4000; u32 voices=in32(c,0x10), sinks=in32(c,0x14), effects=in32(c,0x1C); sz+= (u64)voices*0x400 + sinks*0x200 + effects*0x800 + 0x40000; out64(c,sz); return 0; }
    case 2: case 4: out_obj(c,svcobj_new("IAudioDevice",auddev_disp,0)); return 0;
  }
  return quiet_ok(o,c); }

static SvcObj* new_audren(void){ return svcobj_new("audren:u",audren_disp,0); }
/* audout. Nothing is played, but the buffers have to come back at the speed
   real hardware would play them. Unreal's audio thread (Yoshi's Crafted World)
   waits for released buffers, and when none ever came back it decided the
   device was dead, reopened it and tore down the old buffer event while its
   thread was still waiting on it -> nn::os assert. */
#define AOUT_MAX 32
typedef struct AudOut {
  struct AudOut* next;
  KEvent* ev;
  int started;
  u64 tag[AOUT_MAX], dur[AOUT_MAX]; int nq;   /* queued, oldest first */
  u64 rel[AOUT_MAX]; int nrel;                /* played, waiting for GetReleased */
  u64 deadline;                               /* when the head buffer is done, 0 = not timed yet */
  u64 played;                                 /* samples */
} AudOut;
static AudOut* g_aouts;
static PMutex g_aout_lk;

/* 2 channels of PCM16 at 48 kHz, which is what we told the game in OpenAudioOut */
static u64 aout_ns(u64 bytes){ return bytes/4*1000000000ull/48000; }

/* call with g_aout_lk held */
static int aout_advance(AudOut* a, u64 now){
  int moved=0;
  if(!a->started) return 0;
  while(a->nq){
    if(!a->deadline) a->deadline=now+a->dur[0];
    if(now<a->deadline) break;
    if(a->nrel<AOUT_MAX) a->rel[a->nrel++]=a->tag[0];
    a->played+=a->dur[0]*48000/1000000000ull;
    a->nq--; memmove(a->tag,a->tag+1,a->nq*8); memmove(a->dur,a->dur+1,a->nq*8);
    /* the next one starts where this one ended, not at "now", so a late tick doesn't drift */
    a->deadline= a->nq? a->deadline+a->dur[0] : 0;
    moved=1; }
  return moved; }

static void aout_tick(void){
  u64 now=plat_ns();
  pm_lock(&g_aout_lk);
  for(AudOut* a=g_aouts; a; a=a->next) if(aout_advance(a,now)) kevent_signal(a->ev);
  pm_unlock(&g_aout_lk); }

static void aout_destroy(SvcObj* o){
  AudOut* a=o->data;
  pm_lock(&g_aout_lk);
  for(AudOut** p=&g_aouts; *p; p=&(*p)->next) if(*p==a){ *p=a->next; break; }
  pm_unlock(&g_aout_lk);
  o->data=NULL; free(a); }

static int audout_obj(SvcObj* o, IpcCtx* c){
  AudOut* a=o->data; int r=0;
  pm_lock(&g_aout_lk);
  switch(c->cmd){
    case 0: out32(c,a->started? 0 : 1); break;                 /* GetAudioOutState: 0 started, 1 stopped */
    case 1: a->started=1; break;
    case 2: a->started=0; a->deadline=0; break;
    case 3: case 7: {                                          /* AppendAudioOutBuffer(Auto) */
      u64 tag=in64(c,0), b, n, bytes=0;
      if(buf_in(c,0,&b,&n) && b && n>=0x20) bytes=gr64(b+0x18);   /* AudioOutBuffer.size */
      if(a->nq==AOUT_MAX){ r=RC(153,8); break; }               /* hw takes 32 at most */
      a->tag[a->nq]=tag; a->dur[a->nq]=aout_ns(bytes); a->nq++;
      break; }
    case 4: out_copy_h(c,ev_handle(a->ev)); break;            /* RegisterBufferEvent */
    case 5: case 8: {                                          /* GetReleasedAudioOutBuffers(Auto) */
      aout_advance(a,plat_ns());
      u64 b, n; u32 cnt=0;
      if(buf_out(c,0,&b,&n) && b){ u32 cap=(u32)(n/8);
        while(cnt<cap && cnt<(u32)a->nrel){ gw64(b+cnt*8ull,a->rel[cnt]); cnt++; }
        a->nrel-=cnt; memmove(a->rel,a->rel+cnt,a->nrel*8); }
      out32(c,cnt); break; }
    case 6: { u64 tag=in64(c,0); int has=0;                   /* ContainsAudioOutBuffer */
      for(int i=0;i<a->nq;i++) if(a->tag[i]==tag) has=1;
      out8(c,(u8)has); break; }
    case 9: out32(c,(u32)a->nq); break;                        /* GetAudioOutBufferCount */
    case 10: out64(c,a->played); break;                        /* GetAudioOutPlayedSampleCount */
    case 11: {                                                 /* FlushAudioOutBuffers: give everything back now */
      int any=a->nq>0;
      while(a->nq && a->nrel<AOUT_MAX){ a->rel[a->nrel++]=a->tag[0]; a->nq--; memmove(a->tag,a->tag+1,a->nq*8); memmove(a->dur,a->dur+1,a->nq*8); }
      a->deadline=0; if(any) kevent_signal(a->ev);
      out8(c,(u8)any); break; }
    case 12: break;                                            /* SetAudioOutVolume */
    case 13: out32(c,0x3F800000); break;                       /* GetAudioOutVolume: 1.0f */
  }
  pm_unlock(&g_aout_lk);
  return r; }

static SvcObj* aout_new(void){
  SvcObj* o=svcobj_new("IAudioOut",audout_obj,0);
  AudOut* a=calloc(1,sizeof *a); a->ev=kevent_new("audout buffers",0);
  o->data=a; o->destroy=aout_destroy;
  pm_lock(&g_aout_lk); a->next=g_aouts; g_aouts=a; pm_unlock(&g_aout_lk);
  return o; }

static int audout_disp(SvcObj* o, IpcCtx* c){
  if(c->cmd==0||c->cmd==2){ u64 a,s; if(buf_out(c,0,&a,&s) && a){ strcpy((char*)GP(a),"DeviceOut"); } out32(c,1); return 0; }
  if(c->cmd==1||c->cmd==3){
    u64 a,s; if(buf_out(c,0,&a,&s) && a && s>=10) strcpy((char*)GP(a),"DeviceOut");   /* the name we actually opened */
    out32(c,48000); out32(c,2); out32(c,2); out32(c,1); out_obj(c,aout_new()); return 0; }
  return quiet_ok(o,c); }

static SvcObj* new_audout(void){ return svcobj_new("audout:u",audout_disp,0); }

static int hwopus_dec(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==0||c->cmd==4){ out32(c,0); out32(c,0); return 0; } return 0; }

static int hwopus_disp(SvcObj* o, IpcCtx* c){ switch(c->cmd){ case 0: case 2: case 4: out_obj(c,svcobj_new("IHardwareOpusDecoder",hwopus_dec,0)); return 0; case 1: case 3: case 5: out32(c,0x10000); return 0; } return quiet_ok(o,c); }

static SvcObj* new_hwopus(void){ return svcobj_new("hwopus",hwopus_disp,0); }

static int bsd_disp(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){ case 0: out32(c,0); return 0; case 1: return 0; case 2: out32(c,(u32)-1); out32(c,106); return 0; }
  out32(c,(u32)-1); out32(c,106); return 0; }

static SvcObj* new_bsd(void){ return svcobj_new("bsd",bsd_disp,0); }

static int ssl_disp(SvcObj* o, IpcCtx* c){ if(c->cmd==0){ out_obj(c,quiet("ISslContext")); return 0; } return quiet_ok(o,c); }

static SvcObj* new_ssl(void){ return svcobj_new("ssl",ssl_disp,0); }

static int prepo_disp(SvcObj* o, IpcCtx* c){ (void)o; (void)c; return 0; }

static SvcObj* new_prepo(void){ return svcobj_new("prepo",prepo_disp,0); }

static int psm_session(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==0){ out_copy_h(c,ev_handle(kevent_new("psm",0))); return 0; } return 0; }

/* the PC's own battery (100% and charging on a desktop) */
static int psm_disp(SvcObj* o, IpcCtx* c){ (void)o; int pct, chg; plat_battery(&pct,&chg); switch(c->cmd){ case 0: out32(c,(u32)pct); return 0; case 1: out32(c,chg? 1 : 0); return 0; case 7: out_obj(c,svcobj_new("IPsmSession",psm_session,0)); return 0; case 4: case 5: out8(c,0); return 0; } return 0; }

static SvcObj* new_psm(void){ return svcobj_new("psm",psm_disp,0); }

static int fatal_disp(SvcObj* o, IpcCtx* c){ (void)o; fatal("The game called fatal:u (code %x)",in32(c,0)); return 0; }

static SvcObj* new_fatal(void){ return svcobj_new("fatal:u",fatal_disp,0); }

static int csrng_disp(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==0){ u64 a,s; buf_out(c,0,&a,&s); static u64 x=0x9E3779B97F4A7C15ull; for(u64 i=0;i<s;i++){ x^=x<<13; x^=x>>7; x^=x<<17; GP(a)[i]=(u8)x; } } return 0; }

static SvcObj* new_csrng(void){ return svcobj_new("csrng",csrng_disp,0); }

static int mm_disp(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==4||c->cmd==6) out32(c,1); return 0; }

static SvcObj* new_mm(void){ return svcobj_new("mm:u",mm_disp,0); }

static int nfp_user(SvcObj* o, IpcCtx* c){ (void)o; switch(c->cmd){ case 1: out32(c,0); return 0; case 17: case 18: out_copy_h(c,ev_handle(kevent_new("nfp",0))); return 0; case 19: out32(c,1); return 0; } return 0; }

static int nfp_disp(SvcObj* o, IpcCtx* c){ if(c->cmd==0){ out_obj(c,svcobj_new("IUser",nfp_user,0)); return 0; } return quiet_ok(o,c); }

static SvcObj* new_nfp(void){ return svcobj_new("nfp:user",nfp_disp,0); }

static int caps_disp(SvcObj* o, IpcCtx* c){ (void)o; (void)c; return 0; }

static SvcObj* new_caps(void){ return svcobj_new("caps",caps_disp,0); }

/* BCAT. behave like an offline console: sync completes immediately (Done/OK) and the delivery cache
   is empty. games do wait on the progress object, so it has to exist */
static int bcat_progress(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){
    case 0: { KEvent* e=kevent_new("bcat progress",0); e->signaled=1; out_copy_h(c,ev_handle(e)); return 0; }   /* GetEvent */
    case 1: { u64 a,sz; buf_out(c,0,&a,&sz); if(a && sz){ memset(GP(a),0,sz); if(sz>=8){ u32 st=9, rs=0; memcpy(GP(a),&st,4); memcpy(GP(a)+4,&rs,4); } } return 0; }   /* GetImpl: Done/OK */
  }
  return 0; }

static int bcat_dir(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){ case 0: return 0;                                   /* Open */
    case 1: case 2: out32(c,0); return 0; }                             /* Read / GetCount: no files */
  return 0; }

static int bcat_file(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){ case 0: return RC(122,2);                            /* Open: not found */
    case 1: out64(c,0); return 0; case 2: out64(c,0); return 0; case 3: { u8 d[0x10]; memset(d,0,sizeof d); outbytes(c,d,0x10); return 0; } }
  return 0; }

static int bcat_storage(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){
    case 0: out_obj(c,svcobj_new("IDeliveryCacheFileService",bcat_file,0)); return 0;
    case 1: out_obj(c,svcobj_new("IDeliveryCacheDirectoryService",bcat_dir,0)); return 0;
    case 10: out32(c,0); return 0; }                                    /* EnumerateDeliveryCacheDirectory: none */
  return 0; }

static int bcat_service(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){
    case 10100: case 10101: case 20100: case 20101: case 20300: case 20301:
      out_obj(c,svcobj_new("IDeliveryCacheProgressService",bcat_progress,0)); return 0; }
  return 0; }

static int bcat_disp(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){
    case 0: out_obj(c,svcobj_new("IBcatService",bcat_service,0)); return 0;
    case 1: case 2: out_obj(c,svcobj_new("IDeliveryCacheStorageService",bcat_storage,0)); return 0; }
  return 0; }

static SvcObj* new_bcat(void){ return svcobj_new("bcat",bcat_disp,0); }

static int aoc_purchase(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==2){ out_copy_h(c,ev_handle(kevent_new("aoc purchase",0))); return 0; } if(c->cmd==3||c->cmd==4) return RC(164,400); return 0; }

static int aoc_disp(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){
    case 0: case 1: case 2: case 3: out32(c,0); return 0;                     /* Count/List: no dlc */
    case 5: case 6: out64(c,(g_title_id&~0xFFFull)+0x1000); return 0;         /* GetAddOnContentBaseId */
    case 7: case 9: case 11: case 12: case 13: case 50: return 0;
    case 8: case 10: out_copy_h(c,ev_handle(kevent_new("aoc",0))); return 0;   /* GetAddOnContentListChangedEvent */
    case 100: case 101: out_obj(c,svcobj_new("IPurchaseEventManager",aoc_purchase,0)); return 0; }
  return 0; }

static SvcObj* new_aoc(void){ return svcobj_new("aoc:u",aoc_disp,0); }

static int irs_disp(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==303||c->cmd==304){ KShmem* s=kshmem_new(0x8000,"irs"); out_copy_h(c,h_new(&s->o)); return 0; } if(c->cmd==311){ out32(c,0); return 0; } return 0; }

static SvcObj* new_irs(void){ return svcobj_new("irs",irs_disp,0); }

static int mii_db(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==0||c->cmd==2||c->cmd==3) out32(c,0); return 0; }

static int mii_disp(SvcObj* o, IpcCtx* c){ if(c->cmd==0){ out_obj(c,svcobj_new("IDatabaseService",mii_db,0)); return 0; } return quiet_ok(o,c); }

static SvcObj* new_mii(void){ return svcobj_new("mii:u",mii_disp,0); }

static int ldn_disp(SvcObj* o, IpcCtx* c){ if(c->cmd==0){ out_obj(c,quiet("IUserLocalCommunicationService")); return 0; } return quiet_ok(o,c); }

static SvcObj* new_ldn(void){ return svcobj_new("ldn:u",ldn_disp,0); }

static int ectx_disp(SvcObj* o, IpcCtx* c){ (void)o; (void)c; return 0; }

static SvcObj* new_ectx(void){ return svcobj_new("ectx:aw",ectx_disp,0); }

static int spl_disp(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==0){ out64(c,0); return 0; } if(c->cmd==7){ u64 a,s; buf_out(c,0,&a,&s); memset(GP(a),0x5A,s); } return 0; }

static SvcObj* new_spl(void){ return svcobj_new("spl:",spl_disp,0); }

void svc_core_init(void){
  g_msg_event=kevent_new("applet msg",0); g_launch_ev=kevent_new("launchable",0); g_res_ev=kevent_new("res change",0); g_opmode_ev=kevent_new("opmode",0);
  g_aud_ev=kevent_new("audio renderer",0);
  pm_init(&g_aout_lk);
  /* the runtime keeps signalling these: hold a reference so a guest closing its handles can't free them */
  kobj_ref(&g_msg_event->o); kobj_ref(&g_launch_ev->o); kobj_ref(&g_res_ev->o); kobj_ref(&g_opmode_ev->o); kobj_ref(&g_aud_ev->o);
  plat_thread(audio_clock,NULL,64*1024);
  am_push_msg(0xF);          /* FocusStateChanged */
  am_push_msg(0x1F);         /* OperationModeChanged / resume */
  kevent_signal(g_launch_ev);
  svc_register("sm:",new_sm);
  svc_register("appletOE",new_appletoe); svc_register("appletAE",new_appletae);
  svc_register("set",new_set); svc_register("set:sys",new_setsys);
  svc_register("time:u",new_time); svc_register("time:a",new_time); svc_register("time:s",new_time);
  svc_register("acc:u0",new_acc); svc_register("acc:u1",new_acc); svc_register("acc:su",new_acc);
  svc_register("apm",new_apm); svc_register("apm:p",new_apm); svc_register("apm:am",new_apm);
  svc_register("lm",new_lm); svc_register("pctl",new_pctl); svc_register("pctl:a",new_pctl); svc_register("pctl:s",new_pctl); svc_register("pctl:r",new_pctl);
  svc_register("nifm:u",new_nifm); svc_register("nifm:a",new_nifm); svc_register("nifm:s",new_nifm);
  svc_register("friend:u",new_friend); svc_register("friend:a",new_friend); svc_register("friend:v",new_friend);
  svc_register("ns:am2",new_ns); svc_register("ns:ec",new_ns); svc_register("ns:rid",new_ns); svc_register("ns:rt",new_ns); svc_register("ns:web",new_ns); svc_register("ns:ro",new_ns);
  svc_register("audren:u",new_audren); svc_register("audout:u",new_audout); svc_register("audin:u",new_audout); svc_register("hwopus",new_hwopus);
  svc_register("bsd:u",new_bsd); svc_register("bsd:s",new_bsd); svc_register("ssl",new_ssl);
  svc_register("prepo:u",new_prepo); svc_register("prepo:a",new_prepo); svc_register("prepo:a2",new_prepo); svc_register("psm",new_psm);
  svc_register("fatal:u",new_fatal); svc_register("csrng",new_csrng); svc_register("mm:u",new_mm);
  svc_register("nfp:user",new_nfp); svc_register("nfc:user",new_nfp); svc_register("caps:u",new_caps); svc_register("caps:a",new_caps); svc_register("caps:su",new_caps); svc_register("caps:ss",new_caps);
  svc_register("bcat:u",new_bcat); svc_register("bcat:a",new_bcat); svc_register("aoc:u",new_aoc); svc_register("irs",new_irs);
  svc_register("mii:e",new_mii); svc_register("mii:u",new_mii); svc_register("ldn:u",new_ldn); svc_register("ectx:aw",new_ectx); svc_register("spl:",new_spl); svc_register("spl:mig",new_spl);
}
