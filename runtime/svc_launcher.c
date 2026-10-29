/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* swr:l, the service behind the launcher (launcher/ in the repo). Not a Nintendo service: on a real
   console it doesn't exist and the launcher just shows its UI with no games.
   A game is any NAME_code.dll (.so on Linux) with its NAME.exe next to it, in the launcher's folder
   or in games/. Launching starts the game and sets SWR_LAUNCHER, so the game reopens the launcher
   when it quits. */
#include "rt.h"
#include <sys/stat.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <limits.h>
#include <unistd.h>
extern char** environ;
#endif

#if defined(_WIN32)
#define CODE_SUFFIX "_code.dll"
#define EXE_SUFFIX ".exe"
#elif defined(__APPLE__)
#define CODE_SUFFIX "_code.dylib"
#define EXE_SUFFIX ""
#else
#define CODE_SUFFIX "_code.so"
#define EXE_SUFFIX ""
#endif

typedef struct { char name[128], title[128], dir[1024]; i64 played; } Game;
static Game g_games[256]; static int g_ngames;
static int g_is_launcher, g_launched;

static int file_exists(const char* p){ struct stat st; return !stat(p,&st); }

static u8* slurp(const char* p, u64* n){
  FILE* f=fopen(p,"rb"); if(!f) return NULL; fseek(f,0,SEEK_END); long s=ftell(f); fseek(f,0,SEEK_SET);
  if(s<=0){ fclose(f); return NULL; } u8* d=malloc((size_t)s); if(fread(d,1,(size_t)s,f)!=(size_t)s){ free(d); d=NULL; } fclose(f); *n=(u64)s; return d; }

static void played_path(char* p, size_t n){ snprintf(p,n,"%slauncher_played.txt",g_exedir); }

static i64 played_get(const char* name){
  char p[1200]; played_path(p,sizeof p); FILE* f=fopen(p,"r"); if(!f) return 0;
  char ln[400]; i64 t=0; while(fgets(ln,sizeof ln,f)){ char* tab=strchr(ln,'\t'); if(!tab) continue; *tab=0; if(!strcmp(ln,name)) t=atoll(tab+1); }
  fclose(f); return t; }

static void played_set(const char* name){
  char p[1200]; played_path(p,sizeof p); FILE* f=fopen(p,"a"); if(!f) return;
  fprintf(f,"%s\t%lld\n",name,(long long)plat_unix_time()); fclose(f); }

/* the English title from the game's control.nacp, if the dump has one */
static void nacp_title(const Game* g, char* out, size_t n){
  char p[1300]; snprintf(p,sizeof p,"%s%s_data/control.nacp",g->dir,g->name);
  u64 sz=0; u8* d=slurp(p,&sz); if(d && sz>=0x300 && d[0]){ snprintf(out,n,"%.*s",0x200,(const char*)d); free(d); return; }
  free(d); snprintf(out,n,"%s",g->name); }

static void scan_dir(const char* dir){
  PDir d; if(plat_dir_open(&d,dir)) return;
  while(plat_dir_next(&d)){
    if(d.is_dir) continue;
    size_t l=strlen(d.name), sl=strlen(CODE_SUFFIX); if(l<=sl || strcmp(d.name+l-sl,CODE_SUFFIX) || g_ngames>=256) continue;
    Game* g=&g_games[g_ngames]; memset(g,0,sizeof *g);
    snprintf(g->name,sizeof g->name,"%.*s",(int)(l-sl),d.name);
    if(!strcmp(g->name,g_exename)) continue;
    snprintf(g->dir,sizeof g->dir,"%s",dir);
    char exe[1300]; snprintf(exe,sizeof exe,"%s%s" EXE_SUFFIX,dir,g->name); if(!file_exists(exe)) continue;
    nacp_title(g,g->title,sizeof g->title);
    g->played=played_get(g->name);
    g_ngames++;
  }
  plat_dir_close(&d); }

/* last played first, like the HOME menu; never played ones after, by name */
static int game_cmp(const void* a, const void* b){
  const Game* x=a; const Game* y=b;
  if(x->played!=y->played) return x->played>y->played? -1 : 1;
  return strcmp(x->title,y->title); }

static void rescan(void){
  g_ngames=0; scan_dir(g_exedir);
  char p[1100]; snprintf(p,sizeof p,"%sgames/",g_exedir); scan_dir(p);
  qsort(g_games,g_ngames,sizeof(Game),game_cmp); }

static u8* game_icon(const Game* g, u64* n){
  static const char* names[]={"%s%s_data/icon.jpg","%s%s_data/icon.png","%s%s_data/icon_AmericanEnglish.dat","%s%s.jpg","%s%s.png"};
  for(int i=0;i<5;i++){ char p[1300]; snprintf(p,sizeof p,names[i],g->dir,g->name); u8* d=slurp(p,n); if(d) return d; }
  return NULL; }

/* key=value in the game's ini. The game rewrites the file in its own format when it starts */
static void ini_path(const Game* g, char* p, size_t n){ snprintf(p,n,"%s%s.ini",g->dir,g->name); }

static int ini_read(const Game* g, const char* key, char* val, size_t n){
  char p[1300]; ini_path(g,p,sizeof p); FILE* f=fopen(p,"r"); if(!f) return 0;
  char ln[1200]; int found=0; size_t kl=strlen(key);
  while(fgets(ln,sizeof ln,f)) if(!strncmp(ln,key,kl) && ln[kl]=='='){ char* v=ln+kl+1; v[strcspn(v,"\r\n")]=0; snprintf(val,n,"%s",v); found=1; }
  fclose(f); return found; }

static void ini_edit(const Game* g, const char* key, const char* val){
  char p[1300]; ini_path(g,p,sizeof p); u64 sz=0; u8* d=slurp(p,&sz);
  char tmp[1350]; snprintf(tmp,sizeof tmp,"%s.tmp",p); FILE* f=fopen(tmp,"w"); if(!f){ free(d); return; }
  int done=0; size_t kl=strlen(key);
  for(u64 i=0;d && i<sz;){
    u64 e=i; while(e<sz && d[e]!='\n') e++;
    if(!done && e-i>kl && !memcmp(d+i,key,kl) && d[i+kl]=='='){ fprintf(f,"%s=%s\n",key,val); done=1; }
    else fprintf(f,"%.*s\n",(int)(e-i),(const char*)d+i);
    i=e+1; }
  if(!done) fprintf(f,"%s=%s\n",key,val);
  fclose(f); free(d); remove(p); rename(tmp,p); }

static int spawn(const char* exe_, const char* dir_){
#if defined(_WIN32)
  char exe[MAX_PATH], dir[MAX_PATH];
  if(!GetFullPathNameA(exe_,sizeof exe,exe,NULL) || !GetFullPathNameA(dir_[0]? dir_ : ".",sizeof dir,dir,NULL)) return -1;
  STARTUPINFOA si; PROCESS_INFORMATION pi; memset(&si,0,sizeof si); si.cb=sizeof si;
  char cmd[MAX_PATH+4]; snprintf(cmd,sizeof cmd,"\"%s\"",exe);
  if(!CreateProcessA(exe,cmd,NULL,NULL,FALSE,0,NULL,dir,&si,&pi)) return -1;
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return 0;
#else
  /* posix_spawn, not fork: the GL driver and X11 have fork handlers we don't want to run */
  char exe[PATH_MAX], dir[PATH_MAX];
  if(!realpath(exe_,exe) || !realpath(dir_[0]? dir_ : ".",dir)) return -1;
  posix_spawn_file_actions_t fa; posix_spawnattr_t at;
  posix_spawn_file_actions_init(&fa);
#if !defined(__ANDROID__)
  posix_spawn_file_actions_addchdir_np(&fa,dir);
#endif
  posix_spawnattr_init(&at); posix_spawnattr_setflags(&at,POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&at,0);
  char* argv[2]={exe,NULL}; pid_t pid;
  int r=posix_spawn(&pid,exe,&fa,&at,argv,environ);
  posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
  return r? -1 : 0;
#endif
}

static int launch(const Game* g){
  char exe[1300]; snprintf(exe,sizeof exe,"%s%s" EXE_SUFFIX,g->dir,g->name);
  char self[1300]; snprintf(self,sizeof self,"%s%s" EXE_SUFFIX,g_exedir,g_exename);
#if defined(_WIN32)
  SetEnvironmentVariableA("SWR_LAUNCHER",self);
#else
  setenv("SWR_LAUNCHER",self,1);
#endif
  if(spawn(exe,g->dir)){ WARN("launcher: could not start %s\n",exe); return -1; }
  LOG("launcher: started %s\n",exe);
  played_set(g->name); g_launched=1; return 0; }

/* a game started from the launcher brings it back when it quits */
void launcher_on_exit(void){
  static int done; if(done || g_is_launcher) return; done=1;
  const char* l=getenv("SWR_LAUNCHER"); if(!l || !l[0] || !file_exists(l)) return;
  char dir[1100]; snprintf(dir,sizeof dir,"%s",l); char* s1=strrchr(dir,'/'), *s2=strrchr(dir,'\\'), *s=s1>s2? s1 : s2; if(s) s[1]=0; else dir[0]=0;
  spawn(l,dir); }

enum { L_COUNT, L_INFO, L_ICON, L_LAUNCH, L_GET_OPT, L_SET_OPT };
typedef struct { char name[128], title[128]; u32 icon_size, flags; } LauncherGameInfo;

static int launcher_disp(SvcObj* o, IpcCtx* c){
  (void)o; g_is_launcher=1;
  /* index ~0 is the launcher itself, for its own settings */
  static Game self; snprintf(self.name,sizeof self.name,"%s",g_exename); snprintf(self.dir,sizeof self.dir,"%s",g_exedir);
  u32 idx=in32(c,0); const Game* g= idx<(u32)g_ngames? &g_games[idx] : idx==0xFFFFFFFFu && c->cmd>=L_GET_OPT? &self : NULL;
  switch(c->cmd){
    case L_COUNT: rescan(); out32(c,(u32)g_ngames); return 0;
    case L_INFO: {
      if(!g) return RC(0x1A0,1);
      LauncherGameInfo info; memset(&info,0,sizeof info);
      snprintf(info.name,sizeof info.name,"%s",g->name); snprintf(info.title,sizeof info.title,"%s",g->title);
      u64 n=0; u8* ic=game_icon(g,&n); free(ic); info.icon_size=(u32)n;
      u64 a,s; if(buf_out(c,0,&a,&s) && a) memcpy(GP(a),&info,s<sizeof info? s : sizeof info);
      return 0; }
    case L_ICON: {
      if(!g) return RC(0x1A0,1);
      u64 n=0; u8* ic=game_icon(g,&n); u64 a,s; u32 w=0;
      if(ic && buf_out(c,0,&a,&s) && a){ w=(u32)(n<s? n : s); memcpy(GP(a),ic,w); }
      free(ic); out32(c,w); return 0; }
    case L_LAUNCH: return !g || launch(g)? RC(0x1A0,2) : 0;
    case L_GET_OPT: case L_SET_OPT: {
      if(!g) return RC(0x1A0,1);
      u64 ka,ks; char key[64]={0}; if(!buf_in(c,0,&ka,&ks) || !ka) return RC(0x1A0,3);
      memcpy(key,GP(ka),ks<63? ks : 63);
      if(c->cmd==L_SET_OPT){ u64 va,vs; char val[256]={0}; if(!buf_in(c,1,&va,&vs) || !va) return RC(0x1A0,3); memcpy(val,GP(va),vs<255? vs : 255); ini_edit(g,key,val); return 0; }
      char val[256]; u64 a,s; if(!ini_read(g,key,val,sizeof val)) val[0]=0;
      if(buf_out(c,0,&a,&s) && a && s){ size_t l=strlen(val); if(l>=s) l=s-1; memcpy(GP(a),val,l); GP(a)[l]=0; }
      return 0; }
  }
  return 0; }

static SvcObj* new_launcher(void){ return svcobj_new("swr:l",launcher_disp,0); }

void svc_launcher_init(void){ svc_register("swr:l",new_launcher); }

int launcher_did_launch(void){ return g_launched; }
