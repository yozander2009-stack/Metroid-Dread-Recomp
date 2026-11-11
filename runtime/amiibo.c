/* SwitchRecomp - Copyright 2026 MZ Labs. Apache-2.0; see LICENSE.txt and NOTICE.txt. */
/* amiibo (GitHub issue #63): nfp:user with a tag read from an amiibo dump, <game>_data/amiibo/*.bin (NTAG215: 540,
   532 or 572 bytes). Only the parts a dump keeps in plain text are used: the UID (tag info) and the model info at 0x54
   (character, variant, type, model number, series), which is what a game checks to know which figure it is. The owner
   data and the application area are encrypted with the console's amiibo keys, which are not used or shipped: the
   registration info reports "not registered", and an application area the game creates lives in memory only. The dump
   file is never written.
   A tag is placed from the in-game menu (or SWR_AMIIBO_AUTO=file for tests): the game finds it the next time it searches
   (or at once if it is searching), and it is lifted again 3 s after it was found, like taking a figure off the reader.
   SWR_AMIIBO_TRACE=1 logs every call. */
#include "rt.h"
#include <time.h>

enum { DS_INIT=0, DS_SEARCH=1, DS_FOUND=2, DS_REMOVED=3, DS_MOUNTED=4 };
#define NFP_RC(d) (0x73u | ((u32)(d)<<9))
#define RC_WRONG_STATE   NFP_RC(73)
#define RC_NOT_REGISTERED NFP_RC(120)
#define RC_AREA_NONE     NFP_RC(128)
#define RC_AREA_WRONG_ID NFP_RC(152)
#define RC_AREA_EXISTS   NFP_RC(168)
#define AREA_SIZE 0xD8
#define DEV_HANDLE 0x0ull     /* one reader: player 1's controller (npad 0) */

static PMutex g_lk; static int g_lk_ok;
static int g_inited, g_ds; static KEvent *g_act, *g_deact, *g_avail;
static u8 g_uid[7], g_model[8]; static char g_tag_name[128]; static u64 g_found_ns;
static int g_pend; static u8 p_uid[7], p_model[8]; static char p_name[128];   /* placed, waiting for a search */
static u8 g_area[AREA_SIZE]; static u32 g_area_id, g_area_len; static int g_area_ok, g_area_open;
static int g_trace;
static char g_status[160]="";

/* the menu's list: dumps in <data>/amiibo */
#define AM_MAX 128
static char g_files[AM_MAX][128]; static int g_nfiles;

static void lk(void){ if(!g_lk_ok){ pm_init(&g_lk); g_lk_ok=1; } pm_lock(&g_lk); }
static void ulk(void){ pm_unlock(&g_lk); }
static u32 ev_h(KEvent* e){ return h_new(&e->o); }

static void tag_found(void){   /* the placed figure is on the reader now (lock held) */
  memcpy(g_uid,p_uid,7); memcpy(g_model,p_model,8); snprintf(g_tag_name,sizeof g_tag_name,"%s",p_name); g_pend=0;
  g_ds=DS_FOUND; g_found_ns=plat_ns(); g_area_ok=0; g_area_open=0; g_area_len=0;
  kevent_signal(g_act);
  snprintf(g_status,sizeof g_status,"%s read by the game",g_tag_name);
  LOG("amiibo: %s found (character %02x%02x, model %02x%02x, series %02x)\n",g_tag_name,g_model[0],g_model[1],g_model[4],g_model[5],g_model[6]); }

static void tag_lift(void){   /* lock held */
  if(g_ds!=DS_FOUND && g_ds!=DS_MOUNTED) return;
  g_ds=DS_REMOVED; g_area_open=0; kevent_signal(g_deact); LOG("amiibo: %s lifted off the reader\n",g_tag_name); }

static void poll_lift(void){ if((g_ds==DS_FOUND || g_ds==DS_MOUNTED) && plat_ns()-g_found_ns>3000000000ull) tag_lift(); }

/* reads a dump; 0 = ok */
static int load_dump(const char* path, u8 uid[7], u8 model[8], char* err, int errn){
  FILE* f=fopen(path,"rb"); if(!f){ snprintf(err,errn,"can't open it"); return 1; }
  u8 d[600]; size_t n=fread(d,1,sizeof d,f); fclose(f);
  if(n!=540 && n!=532 && n!=572){ snprintf(err,errn,"%u bytes is not an amiibo dump (540 expected)",(unsigned)n); return 1; }
  if(d[0x10]!=0xA5 || d[0x0C]!=0xF1 || d[0x0D]!=0x10){ snprintf(err,errn,"not an amiibo dump"); return 1; }
  uid[0]=d[0]; uid[1]=d[1]; uid[2]=d[2]; memcpy(uid+3,d+4,4); memcpy(model,d+0x54,8); return 0; }

static int place_path(const char* path, const char* name){
  u8 uid[7], model[8]; char err[120];
  if(load_dump(path,uid,model,err,sizeof err)){ LOG("amiibo: %s: %s\n",name,err); lk(); snprintf(g_status,sizeof g_status,"%s: %s",name,err); ulk(); return 0; }
  lk();
  if(g_ds==DS_FOUND || g_ds==DS_MOUNTED) tag_lift();
  memcpy(p_uid,uid,7); memcpy(p_model,model,8); snprintf(p_name,sizeof p_name,"%s",name); g_pend=1;
  LOG("amiibo: %s placed on the reader\n",name);
  if(g_inited && g_ds==DS_SEARCH) tag_found(); else snprintf(g_status,sizeof g_status,"%s is ready: open amiibo in the game",name);
  ulk(); return 1; }

/* ---- for the in-game menu (dread_ui/ov.cpp) */
int amiibo_count(void){ return g_nfiles; }
const char* amiibo_name(int i){ return i>=0 && i<g_nfiles? g_files[i] : ""; }
const char* amiibo_status(void){ return g_status; }
int amiibo_place(int i){ if(i<0 || i>=g_nfiles) return 0; char p[1400]; snprintf(p,sizeof p,"%samiibo/%s",g_datadir,g_files[i]);
  char nm[128]; snprintf(nm,sizeof nm,"%s",g_files[i]); size_t L=strlen(nm); if(L>4) nm[L-4]=0; return place_path(p,nm); }
const char* amiibo_dir(void){ static char d[1200]; snprintf(d,sizeof d,"%samiibo",g_datadir); return d; }

static int cmp_name(const void* a, const void* b){ return strcasecmp((const char*)a,(const char*)b); }
void amiibo_rescan(void){
  PDir d; char dir[1200]; snprintf(dir,sizeof dir,"%samiibo",g_datadir); g_nfiles=0;
  if(plat_dir_open(&d,dir)) return;
  while(plat_dir_next(&d) && g_nfiles<AM_MAX){ const char* n=d.name; size_t L=strlen(n);
    if(d.is_dir || L<5 || L>=sizeof g_files[0] || strcasecmp(n+L-4,".bin")) continue; snprintf(g_files[g_nfiles++],sizeof g_files[0],"%s",n); }
  plat_dir_close(&d);
  qsort(g_files,g_nfiles,sizeof g_files[0],cmp_name); }

void amiibo_init(void){
  g_trace= getenv("SWR_AMIIBO_TRACE")!=NULL;
  /* the service keeps its own reference: kevent_new starts at 0, and the game closes its handles on every Finalize,
     which would otherwise free the events the next screen visit hands out again (heap corruption) */
  lk(); if(!g_act){ g_act=kevent_new("nfp activate",0); g_deact=kevent_new("nfp deactivate",0); g_avail=kevent_new("nfp availability",0);
    kobj_ref(&g_act->o); kobj_ref(&g_deact->o); kobj_ref(&g_avail->o); } ulk();
  amiibo_rescan(); if(g_nfiles) LOG("amiibo: %d dump(s) in %samiibo\n",g_nfiles,g_datadir);
  const char* a=getenv("SWR_AMIIBO_AUTO"); if(a && *a) place_path(a,"test amiibo"); }

/* main loop, every frame */
void amiibo_tick(void){ if(!g_inited) return; lk(); poll_lift(); ulk(); }

/* ---- the service */
static int date_out(u8* p){ time_t t=time(NULL); struct tm* tm=localtime(&t); u16 y=tm? (u16)(tm->tm_year+1900) : 2026;
  memcpy(p,&y,2); p[2]=tm? (u8)(tm->tm_mon+1) : 1; p[3]=tm? (u8)tm->tm_mday : 1; return 4; }

static int out_buf(IpcCtx* c, const void* src, u64 n){ u64 a,s; buf_out(c,0,&a,&s); if(!a) return 0; memset(GP(a),0,s); memcpy(GP(a),src,n<s? n : s); return 1; }

static int nfp_user(SvcObj* o, IpcCtx* c){ (void)o;
  int r=0; lk(); poll_lift();
  if(g_trace) LOG("amiibo: IUser cmd %u (device state %d)\n",c->cmd,g_ds);
  switch(c->cmd){
    case 0: case 1:   /* Initialize / Finalize */
      if(c->cmd==0){ g_inited=1; g_ds=DS_INIT; } else { g_inited=0; g_ds=DS_INIT; } break;
    case 2: { u64 a,s; buf_out(c,0,&a,&s); int n=0; if(a && s>=8){ u64 h=DEV_HANDLE; memcpy(GP(a),&h,8); n=1; } out32(c,(u32)n); break; }   /* ListDevices */
    case 3: if(!g_inited){ r=RC_WRONG_STATE; break; }   /* StartDetection */
      g_ds=DS_SEARCH; kevent_clear(g_act); if(g_pend) tag_found(); break;
    case 4: if(g_ds==DS_FOUND || g_ds==DS_MOUNTED) tag_lift(); g_ds=DS_INIT; break;   /* StopDetection */
    case 5: if(g_ds!=DS_FOUND){ r=RC_WRONG_STATE; break; } g_ds=DS_MOUNTED; break;   /* Mount */
    case 6: if(g_ds==DS_MOUNTED) g_ds=DS_FOUND; g_area_open=0; break;   /* Unmount */
    case 7: { u32 id=in32(c,8); if(g_ds!=DS_MOUNTED){ r=RC_WRONG_STATE; break; }   /* OpenApplicationArea */
      if(!g_area_ok){ r=RC_AREA_NONE; break; } if(id!=g_area_id){ r=RC_AREA_WRONG_ID; break; } g_area_open=1; break; }
    case 8: { if(!g_area_open){ r=RC_AREA_NONE; break; } u64 a,s; buf_out(c,0,&a,&s); u32 n=g_area_len;   /* GetApplicationArea */
      if(a){ if(n>s) n=(u32)s; memcpy(GP(a),g_area,n); } out32(c,n); break; }
    case 9: { if(!g_area_open){ r=RC_AREA_NONE; break; } u64 a,s; buf_in(c,0,&a,&s); if(s>AREA_SIZE) s=AREA_SIZE;   /* SetApplicationArea */
      memset(g_area,0,AREA_SIZE); if(a) memcpy(g_area,GP(a),s); g_area_len=(u32)s; break; }
    case 10: case 11: if(g_ds!=DS_MOUNTED) r=RC_WRONG_STATE; break;   /* Flush / Restore: memory only */
    case 12: case 24: { u32 id=in32(c,8); if(g_ds!=DS_MOUNTED){ r=RC_WRONG_STATE; break; }   /* Create / RecreateApplicationArea */
      if(c->cmd==12 && g_area_ok){ r=RC_AREA_EXISTS; break; }
      u64 a,s; buf_in(c,0,&a,&s); if(s>AREA_SIZE) s=AREA_SIZE; memset(g_area,0,AREA_SIZE); if(a) memcpy(g_area,GP(a),s);
      g_area_len=(u32)s; g_area_id=id; g_area_ok=1; LOG("amiibo: the game made an application area (id %08x, %u bytes; kept in memory)\n",id,(unsigned)s); break; }
    case 13: { if(g_ds!=DS_FOUND && g_ds!=DS_MOUNTED){ r=RC_WRONG_STATE; break; }   /* GetTagInfo */
      u8 t[0x58]={0}; memcpy(t,g_uid,7); t[10]=7; u32 proto=1, type=2; memcpy(t+0x20,&proto,4); memcpy(t+0x24,&type,4); out_buf(c,t,sizeof t); break; }
    case 14: if(g_ds!=DS_MOUNTED){ r=RC_WRONG_STATE; break; } r=RC_NOT_REGISTERED; break;   /* GetRegisterInfo */
    case 15: { if(g_ds!=DS_MOUNTED){ r=RC_WRONG_STATE; break; }   /* GetCommonInfo */
      u8 t[0x40]={0}; date_out(t); u32 sz=AREA_SIZE; memcpy(t+8,&sz,4); out_buf(c,t,sizeof t); break; }
    case 16: { if(g_ds!=DS_MOUNTED){ r=RC_WRONG_STATE; break; }   /* GetModelInfo: character id as stored, model number big-endian on the tag */
      u8 t[0x40]={0}; t[0]=g_model[0]; t[1]=g_model[1]; t[2]=g_model[2]; t[3]=g_model[3]; t[4]=g_model[5]; t[5]=g_model[4]; t[6]=g_model[6];
      out_buf(c,t,sizeof t); break; }
    case 17: out_copy_h(c,ev_h(g_act)); break;
    case 18: out_copy_h(c,ev_h(g_deact)); break;
    case 19: out32(c,g_inited? 1 : 0); break;   /* GetState */
    case 20: out32(c,(u32)g_ds); break;          /* GetDeviceState */
    case 21: out32(c,0); break;                  /* GetNpadId: player 1 */
    case 22: if(g_ds!=DS_MOUNTED){ r=RC_WRONG_STATE; break; } out32(c,AREA_SIZE); break;   /* GetApplicationAreaSize */
    case 23: out_copy_h(c,ev_h(g_avail)); break; /* AttachAvailabilityChangeEvent */
    default: LOG("amiibo: IUser cmd %u not handled\n",c->cmd); break;
  }
  if(g_trace && r) LOG("amiibo: -> result %08x\n",r);
  ulk(); return r; }

static int nfp_disp(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==0){ out_obj(c,svcobj_new("IUser",nfp_user,0)); return 0; } return 0; }
SvcObj* amiibo_new_service(void){ return svcobj_new("nfp:user",nfp_disp,0); }
