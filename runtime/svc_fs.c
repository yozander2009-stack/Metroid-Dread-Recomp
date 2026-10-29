/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* fs: RomFS (built on the fly from an extracted romfs/ folder, or a romfs.bin), plus save data
   and SD card mapped to normal folders. */
#include "rt.h"
#include <ctype.h>

#define FS_NOTFOUND RC(2,1)
#define FS_EXISTS   RC(2,2)

/* ============================================================ RomFS virtual */
typedef struct { char* path; u64 off, size; int logged; } RFile;
static size_t g_romdir_len;
static RFile* rfiles; static int nrfiles, caprfiles;
static u8* rmeta; static u64 rmeta_size;        /* header + tables sit at the start of the image */
static u64 rdata_off, rtotal;
static FILE* rraw;                               /* plain romfs.bin */
#define NOPEN 48
typedef struct {const char*path;FILE*f;u64 last;PMutex io;u32 refs;int reload;} ROpen;
static ROpen openf[NOPEN];static u64 openclk;static PCond open_available;
volatile u64 g_fs_read_ns,g_fs_lock_ns,g_fs_open_ns,g_fs_read_calls,g_fs_read_bytes;


/* romfs.bin in chunks (Android, stored uncompressed in the apk) */
static struct { int fd; u64 base, len; } rch[64]; static int nrch;
char g_romfs_dir_override[1024];

int romfs_add_chunk(int fd, u64 base, u64 len){ if(nrch>=64) return -1; rch[nrch].fd=fd; rch[nrch].base=base; rch[nrch].len=len; nrch++; return 0; }
#if !defined(_WIN32)
#include <unistd.h>

static int chunk_read(u64 off, u8* d, u64 size){
  for(int i=0;i<nrch && size;i++){ if(off>=rch[i].len){ off-=rch[i].len; continue; }
    u64 n=rch[i].len-off; if(n>size) n=size; u64 got=0;
    while(got<n){ ssize_t r=pread(rch[i].fd,d+got,n-got,(off_t)(rch[i].base+off+got)); if(r<=0) return -1; got+=(u64)r; }
    d+=n; size-=n; off=0; }
  if(size) memset(d,0,size); return 0; }
#else
static int chunk_read(u64 off, u8* d, u64 size){ (void)off; memset(d,0,size); return -1; }
#endif
static PMutex g_romlock;

typedef struct DNode { char* name; int parent; int first_child, next_sib, first_file; u32 off; } DNode;
typedef struct FNode { char* name; int parent, next_sib; u32 off; u64 size; char* host; u64 img; } FNode;   /* img: its place in the image */
static DNode* dn; static int ndn, capdn; static FNode* fn; static int nfn, capfn;

static int add_dir(const char* name, int parent){ if(ndn==capdn){ capdn=capdn?capdn*2:256; dn=realloc(dn,sizeof(DNode)*capdn); }
  DNode* d=&dn[ndn]; d->name=strdup(name); d->parent=parent; d->first_child=-1; d->next_sib=-1; d->first_file=-1; return ndn++; }

static int add_file(const char* name, int parent, u64 size, const char* host){ if(nfn==capfn){ capfn=capfn?capfn*2:1024; fn=realloc(fn,sizeof(FNode)*capfn); }
  FNode* f=&fn[nfn]; f->name=strdup(name); f->parent=parent; f->next_sib=-1; f->size=size; f->host=strdup(host); return nfn++; }

static int cmpname(const void* a, const void* b){ return strcmp(*(char* const*)a,*(char* const*)b); }

static void scan(const char* host, int di){
  PDir d; if(plat_dir_open(&d,host)) return;
  char** names=NULL; int* isdir=NULL; u64* sizes=NULL; int n=0, cap=0;
  while(plat_dir_next(&d)){ if(n==cap){ cap=cap?cap*2:64; names=realloc(names,sizeof(char*)*cap); isdir=realloc(isdir,sizeof(int)*cap); sizes=realloc(sizes,8*cap); }
    names[n]=strdup(d.name); isdir[n]=d.is_dir; sizes[n]=d.size; n++; }
  plat_dir_close(&d);
  /* dirs first, then alphabetical, so the layout is stable */
  for(int pass=0;pass<2;pass++){
    int lastd=-1, lastf=-1;
    char** idx=malloc(sizeof(char*)*(n?n:1)); int m=0;
    for(int i=0;i<n;i++) if(isdir[i]==(pass==0)) idx[m++]=names[i];
    qsort(idx,m,sizeof(char*),cmpname);
    for(int k=0;k<m;k++){ int i=0; while(names[i]!=idx[k]) i++;
      char p[2048]; snprintf(p,sizeof p,"%s/%s",host,names[i]);
      if(pass==0){ int c=add_dir(names[i],di); if(lastd<0) dn[di].first_child=c; else dn[lastd].next_sib=c; lastd=c; scan(p,c); }
      else { int f=add_file(names[i],di,sizes[i],p); if(lastf<0) dn[di].first_file=f; else fn[lastf].next_sib=f; lastf=f; } }
    free(idx);
  }
  for(int i=0;i<n;i++) free(names[i]); free(names); free(isdir); free(sizes);
}

static u32 rhash(u32 parent, const char* s){ u32 h=parent^123456789u; for(;*s;s++){ h=(h>>5)|(h<<27); h^=(u8)*s; } return h; }

static u32 hcount(u32 n){ if(n<3) return 3; if(n<19) return n|1; u32 c=n; while(!(c%2)||!(c%3)||!(c%5)||!(c%7)||!(c%11)||!(c%13)||!(c%17)) c++; return c; }

static int rf_cmp(const void* a, const void* b){ const RFile* x=a,*y=b; return x->off<y->off? -1 : x->off>y->off; }

/* LayeredFS-style mods: <data>/mods/<name>/romfs/... replaces or adds RomFS files (texture packs,
   model/text/sound replacements). Load order: <data>/mods/load_order.txt, one mod folder per line,
   first line = lowest priority, lines starting with '#' or '-' are disabled. Without that file every
   mod folder is applied in alphabetical order. */
static int find_child_dir(int di, const char* name){ for(int c=dn[di].first_child;c>=0;c=dn[c].next_sib) if(!strcmp(dn[c].name,name)) return c; return -1; }
static int find_file_in(int di, const char* name){ for(int f=dn[di].first_file;f>=0;f=fn[f].next_sib) if(!strcmp(fn[f].name,name)) return f; return -1; }
static int g_mod_replaced, g_mod_added;
static void overlay_scan(const char* host, int di){
  PDir d; if(plat_dir_open(&d,host)) return;
  while(plat_dir_next(&d)){
    if(!strcmp(d.name,".") || !strcmp(d.name,"..")) continue;
    char p[2048]; snprintf(p,sizeof p,"%s/%s",host,d.name);
    if(d.is_dir){ char nm[512]; snprintf(nm,sizeof nm,"%s",d.name);
      int c=find_child_dir(di,nm); if(c<0){ c=add_dir(nm,di); dn[c].next_sib=dn[di].first_child; dn[di].first_child=c; }
      overlay_scan(p,c); }
    else { int f=find_file_in(di,d.name);
      if(f>=0){ free(fn[f].host); fn[f].host=strdup(p); fn[f].size=d.size; g_mod_replaced++; }
      else { f=add_file(d.name,di,d.size,p); fn[f].next_sib=dn[di].first_file; dn[di].first_file=f; g_mod_added++; } } }
  plat_dir_close(&d); }
static void romfs_apply_mod(const char* root, const char* name){
  char p[1400]; snprintf(p,sizeof p,"%s/%s/romfs",root,name);
  if(!plat_is_dir(p)) return;
  int r0=g_mod_replaced, a0=g_mod_added; overlay_scan(p,0);
  LOG("mods: %s (%d files replaced, %d added)\n",name,g_mod_replaced-r0,g_mod_added-a0); }
/* Button-icon packs (Controls > Button icons): mod folders with these names are applied by [Input] button_icons, not
   by load_order.txt, and last, so they win. auto asks the backend which controller is connected (backend_pad_family:
   1 Xbox and everything else, Steam Deck included; 2 DualShock 4; 3 DualSense; 4 Switch controllers; 0 none) */
static const char* const ICON_PACK[]={"Switch to XBOX Layout","Switch to PS4 Layout","Switch to PS5 Layout"};
static int icon_pack(const char* name){ for(int i=0;i<3;i++) if(!strcmp(name,ICON_PACK[i])) return 1; return 0; }
/* The Brazilian Portuguese fan translation by JumpmanClub Brasil (GitHub issue #15): it replaces the English text, the
   Game Over pictures and the font atlas (accented letters), so it applies only with language 9 (Portuguese; the game
   is given English, svc_core.c game_lang), and after the icon packs, whose atlas would drop the accents otherwise */
static const char* const PT_PACK="Portuguese (Brazil) - JumpmanClub Brasil";
static int lang_pack(const char* name){ return !strcmp(name,PT_PACK); }
static void lang_pack_apply(const char* root){
  extern int g_opt_lang; if(g_opt_lang!=9 && g_opt_lang!=17) return;
  char p[1400]; snprintf(p,sizeof p,"%s/%s/romfs",root,PT_PACK);
  if(!plat_is_dir(p)){ LOG("language: Portuguese, but the translation is not installed: English text\n"); return; }
  LOG("language: Portuguese (Brazil), fan translation by JumpmanClub Brasil\n"); romfs_apply_mod(root,PT_PACK); }
static void icon_pack_apply(const char* root){
  extern char g_opt_button_icons[]; extern int backend_pad_family(void);
  const char* want=g_opt_button_icons; int fam=-1;
  if(!strcmp(want,"auto")){ fam=backend_pad_family(); want= fam==1? "xbox" : fam==2? "ps4" : fam==3? "ps5" : "switch"; }
  const char* order[3]={NULL,NULL,NULL};   /* the pack for the choice, then a PlayStation stand-in */
  if(!strcmp(want,"xbox")) order[0]=ICON_PACK[0];
  else if(!strcmp(want,"ps4")){ order[0]=ICON_PACK[1]; order[1]=ICON_PACK[2]; }
  else if(!strcmp(want,"ps5")){ order[0]=ICON_PACK[2]; order[1]=ICON_PACK[1]; }
  for(int i=0;i<3 && order[i];i++){ char p[1400]; snprintf(p,sizeof p,"%s/%s/romfs",root,order[i]);
    if(plat_is_dir(p)){ LOG("button icons: %s%s -> %s\n",g_opt_button_icons,fam>=0? (fam==1? " (Xbox / other controller)" : fam==2? " (DualShock 4)" : fam==3? " (DualSense)" : fam==4? " (Switch controller)" : " (no controller)") : "",order[i]);
      romfs_apply_mod(root,order[i]); return; } }
  LOG("button icons: %s%s -> the game's own%s\n",g_opt_button_icons,fam==0? " (no controller)" : fam==4? " (Switch controller)" : "",strcmp(want,"switch")? " (that icon pack is not installed)" : ""); }
static void romfs_apply_mods(void){
  extern char g_datadir[]; char root[1200]; snprintf(root,sizeof root,"%smods",g_datadir);
  if(!plat_is_dir(root)) return;
  char lo[1300]; snprintf(lo,sizeof lo,"%s/load_order.txt",root);
  FILE* f=fopen(lo,"r");
  if(f){ char line[600];
    while(fgets(line,sizeof line,f)){ char* s=line; while(*s==' '||*s=='\t') s++; size_t n=strlen(s); while(n && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' ')) s[--n]=0;
      if(!*s || *s=='#' || *s=='-' || icon_pack(s) || lang_pack(s)) continue; romfs_apply_mod(root,s); }
    fclose(f); icon_pack_apply(root); lang_pack_apply(root); return; }
  PDir d; if(plat_dir_open(&d,root)) return;
  char** names=NULL; int n=0, cap=0;
  while(plat_dir_next(&d)) if(d.is_dir && strcmp(d.name,".") && strcmp(d.name,"..")){ if(n==cap){ cap=cap?cap*2:16; names=realloc(names,sizeof(char*)*cap); } names[n++]=strdup(d.name); }
  plat_dir_close(&d);
  qsort(names,n,sizeof(char*),cmpname);
  for(int i=0;i<n;i++){ if(!icon_pack(names[i]) && !lang_pack(names[i])) romfs_apply_mod(root,names[i]); free(names[i]); }
  free(names); icon_pack_apply(root); lang_pack_apply(root); }

static void romfs_build(const char* dir){
  add_dir("",0); dn[0].parent=0; g_romdir_len=strlen(dir);
  scan(dir,0);
  romfs_apply_mods();
  /* table offsets */
  u32 doff=0; for(int i=0;i<ndn;i++){ dn[i].off=doff; doff+=0x18+((strlen(dn[i].name)+3)&~3u); }
  u32 foff=0; for(int i=0;i<nfn;i++){ fn[i].off=foff; foff+=0x20+((strlen(fn[i].name)+3)&~3u); }
  u32 dhc=hcount(ndn), fhc=hcount(nfn);
  u64 hdr=0x50, dht=hdr, dmt=dht+dhc*4, fht=dmt+doff, fmt=fht+fhc*4, end=fmt+foff;
  rdata_off=(end+0x1FF)&~0x1FFull;
  rmeta_size=rdata_off; rmeta=calloc(1,rmeta_size);
  u64 H[10]={0x50,dht,dhc*4ull,dmt,doff,fht,fhc*4ull,fmt,foff,rdata_off}; memcpy(rmeta,H,0x50);
  u32* dh=(u32*)(rmeta+dht); for(u32 i=0;i<dhc;i++) dh[i]=0xFFFFFFFFu;
  u32* fh=(u32*)(rmeta+fht); for(u32 i=0;i<fhc;i++) fh[i]=0xFFFFFFFFu;
  u64 data=0;
  rfiles=calloc(nfn?nfn:1,sizeof(RFile));
  for(int i=0;i<nfn;i++){ fn[i].size=fn[i].size; data=(data+0xF)&~0xFull;
    /* an empty file has the same offset as the next one: listing it would make romfs_read find it
       instead and read the real file back as padding (zeros). it is never read anyway */
    fn[i].img=rdata_off+data;
    if(fn[i].size){ rfiles[nrfiles].path=fn[i].host; rfiles[nrfiles].off=rdata_off+data; rfiles[nrfiles].size=fn[i].size; nrfiles++; }
    u8* e=rmeta+fmt+fn[i].off; u32 w[2]={dn[fn[i].parent].off, fn[i].next_sib>=0? fn[fn[i].next_sib].off : 0xFFFFFFFFu};
    memcpy(e,w,8); memcpy(e+8,&data,8); memcpy(e+16,&fn[i].size,8);
    u32 h=rhash(dn[fn[i].parent].off,fn[i].name)%fhc; u32 nl=(u32)strlen(fn[i].name);
    memcpy(e+0x18,&fh[h],4); fh[h]=fn[i].off; memcpy(e+0x1C,&nl,4); memcpy(e+0x20,fn[i].name,nl);
    data+=fn[i].size; }
  for(int i=0;i<ndn;i++){ u8* e=rmeta+dmt+dn[i].off;
    u32 w[4]={dn[dn[i].parent].off, dn[i].next_sib>=0? dn[dn[i].next_sib].off : 0xFFFFFFFFu, dn[i].first_child>=0? dn[dn[i].first_child].off : 0xFFFFFFFFu,
      dn[i].first_file>=0? fn[dn[i].first_file].off : 0xFFFFFFFFu};
    memcpy(e,w,16); u32 h=rhash(i? dn[dn[i].parent].off : 0,dn[i].name)%dhc; u32 nl=(u32)strlen(dn[i].name);
    memcpy(e+16,&dh[h],4); dh[h]=dn[i].off; memcpy(e+20,&nl,4); memcpy(e+24,dn[i].name,nl); }
  rtotal=rdata_off+data;
  qsort(rfiles,nrfiles,sizeof(RFile),rf_cmp);
  LOG("RomFS: %d folders, %d files, %.1f MB\n",ndn,nfn,(double)rtotal/1048576.0);
}

void romfs_init(void){
  pm_init(&g_romlock);
  pc_init(&open_available);for(int i=0;i<NOPEN;i++)pm_init(&openf[i].io);
  char p[1200];
  if(nrch){ rtotal=0; for(int i=0;i<nrch;i++) rtotal+=rch[i].len; LOG("RomFS: inside the APK, %d parts (%.1f MB)\n",nrch,(double)rtotal/1048576.0); return; }
  {const char*test_rom=getenv("SWR_TEST_ROMFS");if(test_rom&&*test_rom&&strlen(test_rom)<sizeof g_romfs_dir_override)strcpy(g_romfs_dir_override,test_rom);}
  if(g_romfs_dir_override[0] && plat_is_dir(g_romfs_dir_override)){ LOG("RomFS: %s\n",g_romfs_dir_override); romfs_build(g_romfs_dir_override); return; }
  snprintf(p,sizeof p,"%sromfs.bin",g_datadir); rraw=fopen(p,"rb");
  if(rraw){ rtotal=plat_file_size(p); LOG("RomFS: image %s (%.1f MB)\n",p,(double)rtotal/1048576.0); return; }
  snprintf(p,sizeof p,"%sromfs",g_datadir);
  if(plat_is_dir(p)){ romfs_build(p); return; }
  LOG("RomFS: none (folder %s)\n",p);
  char q[1200]; snprintf(q,sizeof q,"%sexefs/main.nro",g_datadir);
  if(!plat_file_size(q)){
    char m[1600]; snprintf(m,sizeof m,"The game's romfs is missing (the data: models, textures, sound).\n\n"
      "Extract the RomFS of your own copy of the game (with the update applied) and copy the folder to:\n%s\n\nThe game will close when it tries to read its data.",p);
#if defined(_WIN32)
    extern void win_fatal_box(const char*); win_fatal_box(m);
#elif !defined(__ANDROID__)
    plat_message(m,1,1);
#endif
    LOG("%s\n",m); }
}

u64 romfs_size(void){ return rtotal; }
/* the RomFS's files in folder (as "/system/shd/") by their game path, place in the image and size, mods applied: the
   native Vulkan renderer's shader pass (snxvk_glue.c romfs_files). -1 without a file table (romfs.bin, APK), 1 when
   each said stop, else 0 */
int romfs_list(const char* folder, int (*each)(void* ctx, const char* name, u64 off, u64 size), void* ctx){
  if(!nfn) return -1;
  size_t fl=strlen(folder);
  for(int i=0;i<nfn;i++){ if(!fn[i].size) continue;
    char nm[1024]; size_t at=sizeof nm-1; nm[at]=0;   /* built from the end: file, then each parent folder */
    int ok=1; const char* parts[64]; int np=0; parts[np++]=fn[i].name;
    for(int d=fn[i].parent; d>0 && ok; d=dn[d].parent){ if(np==64) ok=0; else parts[np++]=dn[d].name; }
    for(int k=0;k<np && ok;k++){ size_t l=strlen(parts[k]); if(l+1>at) { ok=0; break; } at-=l; memcpy(nm+at,parts[k],l); nm[--at]='/'; }
    if(!ok || strncmp(nm+at,folder,fl)) continue;
    if(each(ctx,nm+at,fn[i].img,fn[i].size)) return 1; }
  return 0; }

/* cache of open host files */
/* Reserve the cache entry under its short metadata lock, then perform
   open/read operations under that file's lock. Independent asset reads no
   longer queue behind an unrelated filesystem operation. */
static ROpen* rf_acquire(const char*path){
  u64 start=plat_ns();pm_lock(&g_romlock);int selected=-1;
  for(;;){int lru=-1;for(int i=0;i<NOPEN;i++){
    if(openf[i].path==path){selected=i;break;}
    if(!openf[i].refs&&(lru<0||openf[i].last<openf[lru].last))lru=i;
  }
  if(selected>=0)break;
  if(lru>=0){selected=lru;openf[selected].path=path;openf[selected].reload=1;break;}
  pc_wait(&open_available,&g_romlock,1000000);
  }
  ROpen*r=&openf[selected];r->refs++;r->last=++openclk;pm_unlock(&g_romlock);pm_lock(&r->io);
  __atomic_add_fetch(&g_fs_lock_ns,plat_ns()-start,__ATOMIC_RELAXED);
  if(r->reload){u64 begin=plat_ns();if(r->f)fclose(r->f);r->f=fopen(path,"rb");r->reload=0;
    __atomic_add_fetch(&g_fs_open_ns,plat_ns()-begin,__ATOMIC_RELAXED);}
  return r;
}
static void rf_release(ROpen*r){pm_unlock(&r->io);pm_lock(&g_romlock);r->refs--;pc_signal(&open_available);pm_unlock(&g_romlock);}

int romfs_read(u64 off, void* dst, u64 size){
  u8* d=dst;u64 begin=plat_ns(),requested=size;const char*last_path=NULL;
  if(nrch) return chunk_read(off,d,size);
  if(rraw){ pm_lock(&g_romlock); int r=plat_pread(rraw,d,size,off); pm_unlock(&g_romlock); return r<0? -1 : 0; }
  if(off<rmeta_size){ u64 n=rmeta_size-off; if(n>size) n=size; memcpy(d,rmeta+off,n); d+=n; off+=n; size-=n; }
  while(size){
    int lo=0, hi=nrfiles-1, k=-1;
    while(lo<=hi){ int m=(lo+hi)/2; if(rfiles[m].off<=off){ k=m; lo=m+1; } else hi=m-1; }
    if(k<0){ memset(d,0,size); break; }
    RFile* f=&rfiles[k];
    if(off>=f->off+f->size){ /* inter-file padding */
      u64 nx= k+1<nrfiles? rfiles[k+1].off : rtotal; u64 n=nx>off? nx-off : size; if(n>size) n=size; if(!n) n=size; memset(d,0,n); d+=n; off+=n; size-=n; continue; }
    u64 n=f->off+f->size-off; if(n>size) n=size;
    { extern void vp9_note_file(const char* path); vp9_note_file(f->path); }   /* VP9 video: which .webm is the game reading */
    if(!__atomic_exchange_n(&f->logged,1,__ATOMIC_RELAXED)){ LOGV("romfs: %s (%.1f KB)\n",f->path+g_romdir_len,(double)f->size/1024.0); }
    last_path=f->path;ROpen*file=rf_acquire(f->path);
    if(!file->f || plat_pread(file->f,d,n,off-f->off)<(int)n){memset(d,0,n);}
    rf_release(file);
    d+=n; off+=n; size-=n;
  }
  u64 elapsed=plat_ns()-begin;__atomic_add_fetch(&g_fs_read_ns,elapsed,__ATOMIC_RELAXED);
  __atomic_add_fetch(&g_fs_read_calls,1,__ATOMIC_RELAXED);__atomic_add_fetch(&g_fs_read_bytes,requested,__ATOMIC_RELAXED);
  if(elapsed>10000000ull){KThread*thread=kthread_cur();LOG("io: romfs read %.2f ms, %llu bytes, thread %llu, %s\n",elapsed/1e6,(unsigned long long)requested,(unsigned long long)(thread?thread->id:0),last_path?last_path+g_romdir_len:"metadata");}
  return 0;
}

/* ============================================================ IStorage (RomFS) */
static int romstorage(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 0: { u64 off=in64(c,0), sz=in64(c,8), a, bs; buf_out(c,0,&a,&bs); if(sz>bs) sz=bs;
      if(off>rtotal) return RC(2,6001); if(off+sz>rtotal) sz=rtotal-off;
      romfs_read(off,GP(a),sz); return 0; }
    case 4: out64(c,rtotal); return 0;
    case 1: case 2: case 3: return RC(2,6300);
    case 5: { u8 z[0x40]; memset(z,0,sizeof z); outbytes(c,z,0x40); return 0; }
  }
  return stub_ok(o,c); }

/* ============================================================ PC folders as IFileSystem */
typedef struct { char root[1024]; } FsData;
typedef struct { FILE* f; char path[1100]; u32 mode; } FileData;
typedef struct { PDir d; int ok; u32 filter; char path[1100]; int done; } DirData;

static void read_path(IpcCtx* c, int idx, char* out, u32 max){
  u64 a,s; if(!buf_xs(c,idx,&a,&s) || !a){ buf_in(c,idx,&a,&s); }
  u32 n=0; if(a){ const char* p=(const char*)GP(a); while(n<max-1 && n<0x300 && p[n]){ out[n]=p[n]; n++; } } out[n]=0; }

static void host_path(FsData* fs, const char* p, char* out, size_t max){
  while(*p=='/') p++;
  snprintf(out,max,"%s/%s",fs->root,p);
  size_t l=strlen(out); while(l>0 && (out[l-1]=='/')) out[--l]=0;
  for(char* q=out;*q;q++) if(*q=='\\') *q='/';
}

static void mkdirs(const char* path){ char t[1100]; strncpy(t,path,1099); t[1099]=0; for(char* p=t+1;*p;p++){ if(*p=='/'){ *p=0; plat_mkdir(t); *p='/'; } } plat_mkdir(t); }

/* Horizon rejects paths that climb out of the mount. So do we: otherwise a game bug like
   DeleteDirectoryRecursively("/../..") would delete files outside the save folder */
static int path_escapes(const char* p){
  for(const char* q=p;*q;){ while(*q=='/'||*q=='\\') q++; const char* e=q; while(*e && *e!='/' && *e!='\\') e++;
    if((e-q==2 && q[0]=='.' && q[1]=='.') || memchr(q,':',(size_t)(e-q))) return 1;
    q=e; }
  return 0; }

static void copy_tree(const char* src, const char* dst){
  PDir d; if(plat_dir_open(&d,src)) return; mkdirs(dst);
  while(plat_dir_next(&d)){ char s[1600], t[1600]; snprintf(s,sizeof s,"%s/%s",src,d.name); snprintf(t,sizeof t,"%s/%s",dst,d.name);
    if(d.is_dir){ copy_tree(s,t); continue; }
    FILE* fi=fopen(s,"rb"); if(!fi) continue; FILE* fo=fopen(t,"wb"); if(!fo){ fclose(fi); continue; }
    static char buf[1<<16]; size_t n; while((n=fread(buf,1,sizeof buf,fi))>0) fwrite(buf,1,n,fo);
    fclose(fi); fclose(fo); }
  plat_dir_close(&d); }

static int file_disp(SvcObj* o, IpcCtx* c){
  FileData* f=o->data;
  switch(c->cmd){
    case 0: { u64 off=in64(c,8), sz=in64(c,16), a, bs; buf_out(c,0,&a,&bs); if(sz>bs) sz=bs;
      fseek(f->f,(long)off,SEEK_SET); u64 n=fread(GP(a),1,sz,f->f); out64(c,n); return 0; }
    case 1: { u64 off=in64(c,8), sz=in64(c,16), a, bs; buf_in(c,0,&a,&bs); if(sz>bs) sz=bs;
      fseek(f->f,(long)off,SEEK_SET); fwrite(GP(a),1,sz,f->f); if(in32(c,0)&1) fflush(f->f); return 0; }
    case 2: fflush(f->f); return 0;
    case 3: { u64 n=in64(c,0); fflush(f->f); fseek(f->f,0,SEEK_END); u64 cur=(u64)ftell(f->f);
      if(n>cur){ u64 k=n-cur; static const u8 z[4096]; while(k){ u64 m=k>4096?4096:k; fwrite(z,1,m,f->f); k-=m; } fflush(f->f); }
      else if(n<cur){ /* truncate = rewrite */ u8* buf=malloc(n?n:1); fseek(f->f,0,SEEK_SET); if(fread(buf,1,n,f->f)){} fclose(f->f); f->f=fopen(f->path,"wb+"); fwrite(buf,1,n,f->f); fflush(f->f); free(buf); }
      return 0; }
    case 4: { fflush(f->f); fseek(f->f,0,SEEK_END); out64(c,(u64)ftell(f->f)); return 0; }
    case 5: { u8 z[0x40]; memset(z,0,sizeof z); outbytes(c,z,0x40); return 0; }
  }
  return stub_ok(o,c); }

static void file_destroy(SvcObj* o){ FileData* f=o->data; if(f->f) fclose(f->f); }

static int dir_disp(SvcObj* o, IpcCtx* c){
  DirData* d=o->data;
  switch(c->cmd){
    case 0: { u64 a,s; buf_out(c,0,&a,&s); u64 n=0, max=s/0x310;
      while(n<max && d->ok && !d->done){
        if(!plat_dir_next(&d->d)){ d->done=1; break; }
        if(d->d.is_dir && !(d->filter&1)) continue; if(!d->d.is_dir && !(d->filter&2)) continue;
        u8* e=GP(a+n*0x310); memset(e,0,0x310); strncpy((char*)e,d->d.name,0x300); e[0x304]=d->d.is_dir? 0 : 1; u64 sz=d->d.is_dir? 0 : d->d.size; memcpy(e+0x308,&sz,8); n++; }
      out64(c,n); return 0; }
    case 1: { PDir t; u64 n=0; if(!plat_dir_open(&t,d->path)){ while(plat_dir_next(&t)){ if(t.is_dir? (d->filter&1) : (d->filter&2)) n++; } plat_dir_close(&t); } out64(c,n); return 0; }
  }
  return stub_ok(o,c); }

static void dir_destroy(SvcObj* o){ DirData* d=o->data; if(d->ok) plat_dir_close(&d->d); }

static int fs_disp(SvcObj* o, IpcCtx* c){
  FsData* fs=o->data; char p[0x301], hp[1100], p2[0x301], hp2[1100];
  if(c->cmd<=15 && c->cmd!=10){ read_path(c,0,p,sizeof p); if(path_escapes(p)) return FS_NOTFOUND; }
  if(c->cmd==5 || c->cmd==6){ read_path(c,1,p2,sizeof p2); if(path_escapes(p2)) return FS_NOTFOUND; }
  switch(c->cmd){
    case 0: { read_path(c,0,p,sizeof p); host_path(fs,p,hp,sizeof hp); u64 sz=in64(c,8);
      FILE* t=fopen(hp,"rb"); if(t){ fclose(t); return FS_EXISTS; }
      t=fopen(hp,"wb"); if(!t) return FS_NOTFOUND; if(sz){ fseek(t,(long)sz-1,SEEK_SET); fputc(0,t); } fclose(t); return 0; }
    case 1: read_path(c,0,p,sizeof p); host_path(fs,p,hp,sizeof hp); return remove(hp)? FS_NOTFOUND : 0;
    case 2: read_path(c,0,p,sizeof p); host_path(fs,p,hp,sizeof hp); if(plat_is_dir(hp)) return FS_EXISTS; return plat_mkdir(hp)? FS_NOTFOUND : 0;
    case 3: case 4: case 13: { read_path(c,0,p,sizeof p); host_path(fs,p,hp,sizeof hp);
      /* simple recursive delete */
      extern void rm_rf(const char*, int keep); rm_rf(hp,c->cmd==13); return 0; }
    case 5: case 6: read_path(c,0,p,sizeof p); read_path(c,1,p2,sizeof p2); host_path(fs,p,hp,sizeof hp); host_path(fs,p2,hp2,sizeof hp2); return rename(hp,hp2)? FS_NOTFOUND : 0;
    case 7: { read_path(c,0,p,sizeof p); host_path(fs,p,hp,sizeof hp);
      if(plat_is_dir(hp) || !strcmp(p,"/") || !p[0]){ out32(c,0); return 0; }
      FILE* t=fopen(hp,"rb"); if(!t) return FS_NOTFOUND; fclose(t); out32(c,1); return 0; }
    case 8: { read_path(c,0,p,sizeof p); host_path(fs,p,hp,sizeof hp); u32 mode=in32(c,0);
      FILE* t=fopen(hp,(mode&2)? "rb+" : "rb"); if(!t) return FS_NOTFOUND;
      { /* diagnostic, opt-in: SWR_TRACE_FS=path logs plat_ns and each file opened through IFileSystem */
        static int checked; static FILE* tr; if(!checked){ const char* e=getenv("SWR_TRACE_FS"); checked=1; if(e && e[0]){ tr=fopen(e,"w"); if(tr) fprintf(tr,"ns,mode,path\n"); } }
        if(tr){ fprintf(tr,"%llu,%u,%s\n",(unsigned long long)plat_ns(),mode,p); fflush(tr); } }
      SvcObj* f=svcobj_new("IFile",file_disp,sizeof(FileData)); FileData* fd=f->data; fd->f=t; fd->mode=mode; strcpy(fd->path,hp); f->destroy=file_destroy;
      out_obj(c,f); return 0; }
    case 9: { read_path(c,0,p,sizeof p); host_path(fs,p,hp,sizeof hp);
      if(!plat_is_dir(hp)) return FS_NOTFOUND;
      SvcObj* d=svcobj_new("IDirectory",dir_disp,sizeof(DirData)); DirData* dd=d->data; dd->filter=in32(c,0); if(!dd->filter) dd->filter=3; strcpy(dd->path,hp);
      dd->ok=!plat_dir_open(&dd->d,hp); d->destroy=dir_destroy; out_obj(c,d); return 0; }
    case 10: return 0;
    case 11: out64(c,4ull<<30); return 0;
    case 12: out64(c,8ull<<30); return 0;
    case 14: { u8 z[0x20]; memset(z,0,sizeof z); outbytes(c,z,0x20); return 0; }
    case 15: return 0;
    case 16: out64(c,0); return 0;
  }
  return stub_ok(o,c); }

SvcObj* hostfs_new(const char* root){
  SvcObj* o=svcobj_new("IFileSystem",fs_disp,sizeof(FsData)); FsData* d=o->data; strncpy(d->root,root,1023);
  size_t l=strlen(d->root); while(l && (d->root[l-1]=='/'||d->root[l-1]=='\\')) d->root[--l]=0;
  mkdirs(d->root); return o; }

#if defined(_WIN32)
void rm_rf(const char* p, int keep){ PDir d; if(!plat_dir_open(&d,p)){ while(plat_dir_next(&d)){ char q[1600]; snprintf(q,sizeof q,"%s/%s",p,d.name); if(d.is_dir) rm_rf(q,0); else remove(q); } plat_dir_close(&d); } if(!keep) RemoveDirectoryA(p); }
#else
#include <unistd.h>

void rm_rf(const char* p, int keep){ PDir d; if(!plat_dir_open(&d,p)){ while(plat_dir_next(&d)){ char q[1600]; snprintf(q,sizeof q,"%s/%s",p,d.name); if(d.is_dir) rm_rf(q,0); else remove(q); } plat_dir_close(&d); } if(!keep) rmdir(p); }
#endif

static int savereader(SvcObj* o, IpcCtx* c){ (void)o; if(c->cmd==0){ out64(c,0); return 0; } return 0; }
/* system archives (NgWord, fonts...) aren't available, hand out an empty but valid RomFS */
static u8 g_emptyrom[0x200];

static void emptyrom_init(void){ static int done; if(done) return; done=1; u8* b=g_emptyrom;
  u64 H[10]={0x50, 0x50,4, 0x54,0x18, 0x6C,4, 0x70,0, 0x200}; memcpy(b,H,0x50);
  u32 dh=0; memcpy(b+0x50,&dh,4);
  u32 root[6]={0,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0}; memcpy(b+0x54,root,0x18);
  u32 fh=0xFFFFFFFFu; memcpy(b+0x6C,&fh,4); }

static int emptystorage(SvcObj* o, IpcCtx* c){ (void)o;
  switch(c->cmd){
    case 0: { u64 off=in64(c,0), sz=in64(c,8), a, bs; buf_out(c,0,&a,&bs); if(sz>bs) sz=bs; u8* d=GP(a);
      for(u64 i=0;i<sz;i++) d[i]= off+i<sizeof g_emptyrom? g_emptyrom[off+i] : 0; return 0; }
    case 4: out64(c,sizeof g_emptyrom); return 0;
    case 5: { u8 z[0x40]; memset(z,0,sizeof z); outbytes(c,z,0x40); return 0; }
  }
  return RC(2,6300); }

static int fsp_disp(SvcObj* o, IpcCtx* c){
  char p[1200];
  switch(c->cmd){
    case 1: return 0;
    case 18: snprintf(p,sizeof p,"%ssdmc",g_datadir); out_obj(c,hostfs_new(p)); return 0;
    case 51: case 52: case 53: {
      /* in: u8 space, pad[7], then attr = { u64 program_id, uid[16], u64 system_save_id, u8 type, u8 rank, u16 index } */
      u8 type=in8(c,8+0x20);
      /* System 0, Account 1, Bcat 2, Device 3, Temporary 4, Cache 5, SystemBcat 6 */
      const char* kind= type==1? "user" : (type==2||type==6)? "bcat" : type==3? "device" : type==5? "cache" : type==0? "system" : "temp";
      snprintf(p,sizeof p,"%ssave/%s",g_datadir,kind);
      /* older builds read the type from the wrong offset and kept every save in save/system:
         the first time an account or device save is opened, start it from a copy of that folder */
      if((type==1 || type==3) && !plat_is_dir(p)){ char old[1200]; snprintf(old,sizeof old,"%ssave/system",g_datadir);
        if(plat_is_dir(old)){ LOG("fs: copying %s to %s (save folders are now separated by type)\n",old,p); copy_tree(old,p); } }
      out_obj(c,hostfs_new(p)); return 0; }
    case 22: case 23: case 25: case 26: case 27: case 28: case 32: case 33: case 34: case 35: case 36: case 37: return 0;
    case 60: case 61: case 62: out_obj(c,svcobj_new("ISaveDataInfoReader",savereader,0)); return 0;
    case 200: { if(!rtotal) { WARN("fs: the game requests the RomFS but there is none (put the romfs folder in %s)\n",g_datadir); }
      out_obj(c,svcobj_new("IStorage(romfs)",romstorage,0)); return 0; }
    case 202: { u64 id=in64(c,8); emptyrom_init(); LOG("fs: system data %016llx (not available on PC: provided empty)\n",(unsigned long long)id);
      out_obj(c,svcobj_new("IStorage(system)",emptystorage,0)); return 0; }
    case 203: return RC(2,1001);
    case 400: out_obj(c,svc_generic("IDeviceOperator")); return 0;
    case 500: out_obj(c,svc_generic("IEventNotifier")); return 0;
    case 1003: case 1004: case 1006: case 1007: case 1008: case 1011: case 1012: case 1013: return 0;
    case 1005: out32(c,0); return 0;
    case 1009: out_obj(c,svc_generic("IFileSystemProxyAccessLog")); return 0;
    case 620: case 630: case 631: case 632: case 800: return 0;
    case 8: case 11: case 17: case 19: case 31: return RC(2,1001);
    case 2: case 7: return RC(2,1001);
  }
  return stub_ok(o,c); }

static SvcObj* new_fsp(void){ return svcobj_new("fsp-srv",fsp_disp,0); }

static int fsppr_disp(SvcObj* o, IpcCtx* c){ (void)o; (void)c; return 0; }

static SvcObj* new_fsppr(void){ return svcobj_new("fsp-pr",fsppr_disp,0); }

void svc_fs_init(void){
  svc_register("fsp-srv",new_fsp); svc_register("fsp-pr",new_fsppr); svc_register("fsp-ldr",new_fsppr);
}
