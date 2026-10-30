/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* headless backend for the test runner, writes screenshots to ppm/png */
#include "rt.h"
static int g_maxframes=-1, g_shot_every=0; static char g_shot[1024]; static int g_shots;
static u32* g_last; static int g_lw, g_lh; static PMutex g_hl; static u64 g_start;
static int g_seconds=0; static u64 g_buttons_at=0, g_buttons=0;

/* [Input] mouse_buttons: the game mouse (free aim, buttons) is the Windows backend's only; nothing to re-parse here */
void mouse_map_reset(void){}
void backend_ini(const char* k, const char* v){
  if(!strcmp(k,"frames")) g_maxframes=atoi(v);
  else if(!strcmp(k,"seconds")) g_seconds=atoi(v);
  else if(!strcmp(k,"shot")) strncpy(g_shot,v,1023);
  else if(!strcmp(k,"shot_every")) g_shot_every=atoi(v);
  else if(!strcmp(k,"press")) g_buttons=strtoull(v,NULL,16);
  else if(!strcmp(k,"press_at")) g_buttons_at=strtoull(v,NULL,10);
}

int video_has_gl(void){ return 0; }

void video_init(const char* t){ (void)t; pm_init(&g_hl); g_start=plat_ns(); }

static void save_ppm(const char* path){
  if(!g_last) return; FILE* f=fopen(path,"wb"); if(!f) return; fprintf(f,"P6\n%d %d\n255\n",g_lw,g_lh);
  for(int i=0;i<g_lw*g_lh;i++){ u32 p=g_last[i]; u8 rgb[3]={(u8)p,(u8)(p>>8),(u8)(p>>16)}; fwrite(rgb,1,3,f); } fclose(f); }

void video_present_rgba(const u32* px, int w, int h, int stride){
  pm_lock(&g_hl);
  if(g_lw!=w||g_lh!=h){ free(g_last); g_last=malloc((size_t)w*h*4); g_lw=w; g_lh=h; }
  for(int y=0;y<h;y++) memcpy(g_last+(size_t)y*w,px+(size_t)y*stride,(size_t)w*4);
  if(g_shot_every && g_shot[0] && g_frames%g_shot_every==0){ char p[1100]; snprintf(p,sizeof p,"%s_%04d.ppm",g_shot,g_shots++); save_ppm(p); }
  pm_unlock(&g_hl);
}

void video_poll(void){
  u64 el=(plat_ns()-g_start)/1000000ull;
  g_pad.buttons= (g_buttons && el>=g_buttons_at && el<g_buttons_at+300)? g_buttons : 0;
}

void frontend_frame(void){
  u64 el=plat_ns()-g_start;
  if((g_maxframes>=0 && g_frames>=g_maxframes) || (g_seconds && el>=(u64)g_seconds*1000000000ull)){
    pm_lock(&g_hl); if(g_shot[0]){ char p[1100]; snprintf(p,sizeof p,"%s.ppm",g_shot); save_ppm(p); }
    u64 h=1469598103934665603ull; if(g_last) for(int i=0;i<g_lw*g_lh;i++){ h^=g_last[i]; h*=1099511628211ull; }
    fprintf(stderr,"\nframes=%d hash=%016llx %dx%d\n",g_frames,(unsigned long long)h,g_lw,g_lh); pm_unlock(&g_hl);
    extern void save_hints(void); save_hints(); log_flush(); fflush(NULL); _exit(0); }
}
#if defined(_WIN32)
void win_fatal_box(const char* msg){ MessageBoxA(NULL,msg,"SwitchRecomp",MB_OK|MB_ICONERROR); }
#endif

int backend_pad_family(void){ return 0; }   /* no controller probe here: the game's own button icons */
