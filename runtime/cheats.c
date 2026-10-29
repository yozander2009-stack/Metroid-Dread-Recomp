/* SwitchRecomp - Copyright 2026 MZ Labs. Apache-2.0; see LICENSE.txt and NOTICE.txt. */
/* Atmosphere-format cheats (GitHub issue #11): the dmnt cheat VM's opcodes, run 12 times a second like dmnt.
   Files: <game>_data/cheats/*.txt (Atmosphere's <build id>.txt, or any name); a file named after a different build
   id is skipped. Entries are [Name] blocks; a {Master} block always runs. Which ones run: cheats/enabled.txt, one
   name per line (the Play screen writes it). Memory types: 0 main, 1 heap, 2 alias, 3 ASLR region.
   The game's code is recompiled to native code, so cheats that patch instructions (writes into main's .text) can't
   work here: those writes are refused and the log says which cheat made them. Data cheats (values and pointer chains
   in the heap) work as on a console. Bad pointers are checked against the memory map, never followed blindly. */
#include "rt.h"
#include <ctype.h>

#define CH_MAX 256
#define CH_OPS 1024
typedef struct { char name[96]; u32 op[CH_OPS]; u32 n; int master, on, warned, traced; } Cheat;
static Cheat* g_ch; static int g_nch;
static u64 g_main_base, g_main_size, g_text_lo, g_text_hi;
volatile u64 g_cheat_keys;   /* the buttons the game gets (backends), for the keypress conditional (8kkkkkkk) */

static int g_trace;   /* SWR_CHEAT_TRACE=1: the first few writes and broken chains of each cheat */
#define TRACE(c,...) do{ if(g_trace && (c)->traced<6){ (c)->traced++; LOG(__VA_ARGS__); } }while(0)
static u64 base_of(u32 m){ return m==0? g_main_base : m==1? HEAP_BASE : m==2? ALIAS_BASE : AS_BASE; }
/* a guest range is usable when it lies in one mapped block with the needed permission */
static int ok_range(u64 a, u32 w, int write){
  if(!w || a+w<a) return 0;
  MemBlock b; mem_query(a,&b);
  if(b.state==MS_FREE || b.state==MS_INACCESSIBLE || a+w>b.addr+b.size) return 0;
  return (b.perm & (write? PERM_W : PERM_R)) || b.state==MS_CODEDATA; }
static int rd(u64 a, u32 w, u64* v){
  if(!ok_range(a,w,0)) return 0;
  u64 x=0; memcpy(&x,GP(a),w); *v=x; return 1; }
static int wr(Cheat* c, u64 a, u32 w, u64 v){
  if(a<g_text_hi && a+w>g_text_lo){
    if(!c->warned){ c->warned=1; WARN("cheats: \"%s\" patches the game's code, which is recompiled here: that part does nothing\n",c->name); }
    return 0; }
  if(!ok_range(a,w,1)){ TRACE(c,"cheats: \"%s\": %llx is not writable\n",c->name,(unsigned long long)a); return 0; }
  TRACE(c,"cheats: \"%s\": wrote %llx (%u bytes) to %llx\n",c->name,(unsigned long long)v,w,(unsigned long long)a);
  memcpy(GP(a),&v,w); return 1; }
static u64 mask_w(u64 v, u32 w){ return w>=8? v : v&((1ull<<(w*8))-1); }
static int width_ok(u32 w){ return w==1||w==2||w==4||w==8; }

/* the length in words of the instruction at p, 0 = not understood */
static u32 op_len(const u32* p, u32 left){
  u32 a=p[0], t=a>>28, w=(a>>24)&0xF;
  switch(t){
    case 0: case 1: return w==8? 4 : 3;
    case 2: case 3: return t==3 && !((a>>24)&1)? 2 : 1;
    case 4: return 3;
    case 5: return 2;
    case 6: return 3;
    case 7: return 2;
    case 8: return 1;
    case 9: return ((a>>8)&0xF)? (w==8? 3 : 2) : 1;
    case 0xA: { u32 o=(a>>8)&0xF; return (o==2||o==4||o==5)? 2 : 1; }
    case 0xC: { u32 s=(a>>24)&0xF;
      if(s==0){ u32 x=(a>>8)&0xF, cw=(a>>20)&0xF; return x==0||x==2? 2 : x==4? (cw==8? 3 : 2) : 1; }   /* C0TcSX..: width in bits 20-23 */
      return 1; }
    case 0xF: return 1;
  }
  (void)left; return 0; }

/* skip from the instruction after a failed condition to its matching end (or else), counting nesting */
static u32 skip_block(const Cheat* c, u32 i, int* hit_else){
  int depth=0; *hit_else=0;
  while(i<c->n){ u32 a=c->op[i], t=a>>28, l=op_len(c->op+i,c->n-i); if(!l) return c->n;
    if(t==1 || t==8 || (t==0xC && ((a>>24)&0xF)==0)) depth++;
    else if(t==2){ if(depth==0){ if((a>>24)&0xF){ *hit_else=1; } return i+l; } if(!((a>>24)&0xF)) depth--; }
    i+=l; }
  return c->n; }

static int cmp(u32 cond, u64 x, u64 y){
  switch(cond){ case 1: return x>y; case 2: return x>=y; case 3: return x<y; case 4: return x<=y; case 5: return x==y; case 6: return x!=y; }
  return 0; }

static void run(Cheat* c){
  u64 reg[16]={0}, saved[16]={0}; u32 loop_at[16]={0};
  u32 i=0, steps=0;
  while(i<c->n && steps++<100000){
    const u32* p=c->op+i; u32 a=p[0], t=a>>28, l=op_len(p,c->n-i); if(!l || i+l>c->n) return;
    u32 w=(a>>24)&0xF;
    switch(t){
      case 0: { u32 m=(a>>20)&0xF, r=(a>>16)&0xF; u64 addr=((u64)(a&0xFF)<<32)|p[1];
        u64 v= w==8? ((u64)p[2]<<32)|p[3] : p[2]; if(width_ok(w)) wr(c,base_of(m)+addr+reg[r],w,mask_w(v,w)); break; }
      case 1: { u32 m=(a>>20)&0xF, cond=(a>>16)&0xF; u64 addr=((u64)(a&0xFF)<<32)|p[1];
        u64 v= w==8? ((u64)p[2]<<32)|p[3] : p[2], x;
        if(!width_ok(w) || !rd(base_of(m)+addr,w,&x) || !cmp(cond,x,mask_w(v,w))){ int e; i=skip_block(c,i+l,&e); continue; }
        break; }
      case 2: if(w){ int e; i=skip_block(c,i+l,&e); continue; } break;   /* else reached from the taken branch: skip to end */
      case 3: { u32 r=(a>>16)&0xF;
        if(!((a>>24)&1)){ reg[r]=p[1]; loop_at[r]=i+l; }
        else if(reg[r]>1){ reg[r]--; i=loop_at[r]; continue; }
        break; }
      case 4: reg[(a>>16)&0xF]=((u64)p[1]<<32)|p[2]; break;
      case 5: { u32 m=(a>>20)&0xF, r=(a>>16)&0xF, ind=(a>>12)&0xF; u64 addr=((u64)(a&0xFF)<<32)|p[1], v=0;
        u64 at= ind==1? reg[r]+addr : base_of(m)+addr;
        if(width_ok(w) && rd(at,w,&v)) reg[r]=v; else { TRACE(c,"cheats: \"%s\": pointer chain broken at %llx\n",c->name,(unsigned long long)at); return; }
        break; }
      case 6: { u32 r=(a>>16)&0xF, inc=(a>>12)&0xF, useo=(a>>8)&0xF, ro=(a>>4)&0xF; u64 v=((u64)p[1]<<32)|p[2];
        if(width_ok(w)) wr(c,reg[r]+(useo? reg[ro] : 0),w,mask_w(v,w)); if(inc) reg[r]+=w; break; }
      case 7: { u32 r=(a>>16)&0xF, op=(a>>12)&0xF; u64 v=p[1], x=reg[r];
        switch(op){ case 0: x+=v; break; case 1: x-=v; break; case 2: x*=v; break; case 3: x<<=v; break; case 4: x>>=v; break; }
        reg[r]=mask_w(x,width_ok(w)? w : 8); break; }
      case 8: { u32 k=a&0x0FFFFFFF; if((g_cheat_keys&k)!=k){ int e; i=skip_block(c,i+l,&e); continue; } break; }
      case 9: { u32 op=(a>>20)&0xF, r=(a>>16)&0xF, s=(a>>12)&0xF, imm=(a>>8)&0xF, s2=(a>>4)&0xF;
        u64 x=reg[s], y= imm? (w==8? ((u64)p[1]<<32)|p[2] : p[1]) : reg[s2], z=0;
        switch(op){ case 0: z=x+y; break; case 1: z=x-y; break; case 2: z=x*y; break; case 3: z=x<<y; break; case 4: z=x>>y; break;
          case 5: z=x&y; break; case 6: z=x|y; break; case 7: z=~x; break; case 8: z=x^y; break; case 9: z=x; break; }
        reg[r]=mask_w(z,width_ok(w)? w : 8); break; }
      case 0xA: { u32 s=(a>>20)&0xF, r=(a>>16)&0xF, inc=(a>>12)&0xF, o=(a>>8)&0xF, x=(a>>4)&0xF; u64 imm= l>1? (((u64)(a&0xF)<<32)|p[1]) : 0, at;
        switch(o){ case 0: at=reg[r]; break; case 1: at=reg[r]+reg[x]; break; case 2: at=reg[r]+imm; break;
          case 3: at=base_of(x)+reg[r]; break; case 4: at=base_of(x)+imm; break; case 5: at=base_of(x)+reg[r]+imm; break; default: return; }
        if(width_ok(w)) wr(c,at,w,mask_w(reg[s],w)); if(inc) reg[r]+=w; break; }
      case 0xC: { u32 sub=(a>>24)&0xF;
        if(sub==0){ u32 cw=(a>>20)&0xF, cond=(a>>16)&0xF, s=(a>>12)&0xF, x=(a>>8)&0xF, b4=(a>>4)&0xF, b0=a&0xF; u64 y=0; int got=1;
          switch(x){ case 0: got=rd(base_of(b4)+(((u64)b0<<32)|p[1]),cw,&y); break; case 1: got=rd(base_of(b4)+reg[b0],cw,&y); break;
            case 2: got=rd(reg[b4]+(((u64)b0<<32)|p[1]),cw,&y); break; case 3: got=rd(reg[b4]+reg[b0],cw,&y); break;
            case 4: y= cw==8? ((u64)p[1]<<32)|p[2] : p[1]; break; case 5: y=reg[b4]; break; default: got=0; }
          if(!width_ok(cw) || !got || !cmp(cond,mask_w(reg[s],cw),mask_w(y,cw))){ int e; i=skip_block(c,i+l,&e); continue; } }
        else if(sub==1){ u32 d=(a>>16)&0xF, s=(a>>8)&0xF, x=(a>>4)&0xF;   /* C10D0Sx0: save/restore one register */
          if(x==0) reg[d]=saved[s]; else if(x==1) saved[d]=reg[s]; else if(x==2) saved[d]=0; else if(x==3) reg[d]=0; }
        else if(sub==2){ u32 x=(a>>20)&0xF, mk=a&0xFFFF;   /* C2x0XXXX: by mask */
          for(int k=0;k<16;k++) if(mk&(1u<<k)){ if(x==0) reg[k]=saved[k]; else if(x==1) saved[k]=reg[k]; else if(x==2) saved[k]=0; else if(x==3) reg[k]=0; } }
        break; }
      case 0xF: break;   /* pause / resume / debug log: nothing to do here */
      default: return;
    }
    i+=l; } }

static int hexword(const char* s, u32* v){ u32 x=0; for(int k=0;k<8;k++){ int ch=s[k]; if(!isxdigit(ch)) return 0; x=x*16+(u32)(isdigit(ch)? ch-'0' : (tolower(ch)-'a'+10)); } *v=x; return 1; }
static void load_file(const char* path){
  FILE* f=fopen(path,"rb"); if(!f) return; char line[512]; Cheat* cur=NULL;
  while(fgets(line,sizeof line,f)){
    char* s=line; while(*s==' '||*s=='\t'||*s=='\r'||*s=='\n'||(unsigned char)*s==0xEF||(unsigned char)*s==0xBB||(unsigned char)*s==0xBF) s++;
    if(*s=='[' || *s=='{'){ char close= *s=='['? ']' : '}'; char* e=strchr(s+1,close); if(!e || g_nch>=CH_MAX){ cur=NULL; continue; }
      cur=&g_ch[g_nch++]; memset(cur,0,sizeof *cur); cur->master= *s=='{';
      size_t n=(size_t)(e-s-1); if(n>=sizeof cur->name) n=sizeof cur->name-1; memcpy(cur->name,s+1,n); cur->name[n]=0; continue; }
    if(!cur) continue;
    while(*s){ while(*s==' '||*s=='\t') s++; u32 v; if(!hexword(s,&v)) break; if(cur->n<CH_OPS) cur->op[cur->n++]=v; s+=8; } }
  fclose(f); }

static int is_bid_name(const char* n){ int k=0; for(;k<16;k++) if(!isxdigit((unsigned char)n[k])) return 0; return !strcmp(n+16,".txt") || !strcmp(n+16,".TXT"); }

/* enabled.txt: which cheats run. Read at start and again whenever it changes (the in-game menu writes it), so a
   cheat switches on or off within a second */
static char g_enabled_path[1300]; static u64 g_enabled_stamp;
static u64 file_stamp(const char* path){   /* a hash of the contents, 0 = no file */
  FILE* f=fopen(path,"rb"); if(!f) return 0; u64 h=1469598103934665603ull; int ch; while((ch=fgetc(f))!=EOF) h=(h^(u64)ch)*1099511628211ull; fclose(f); return h|1; }
static void read_enabled(void){
  for(int i=0;i<g_nch;i++) g_ch[i].on=g_ch[i].master;
  FILE* f=fopen(g_enabled_path,"rb");
  if(f){ char line[200]; while(fgets(line,sizeof line,f)){ size_t L=strlen(line); while(L && (line[L-1]=='\n'||line[L-1]=='\r')) line[--L]=0;
      for(int i=0;i<g_nch;i++) if(!g_ch[i].master && !strcmp(g_ch[i].name,line)) g_ch[i].on=1; } fclose(f); }
  int on=0; for(int i=0;i<g_nch;i++) if(g_ch[i].on){ on++; LOG("cheats: on: %s%s\n",g_ch[i].name,g_ch[i].master? " (master code)" : ""); }
  LOG("cheats: %d of %d on\n",on,g_nch); }
static void write_enabled(void){
  FILE* f=fopen(g_enabled_path,"wb"); if(!f) return;
  for(int i=0;i<g_nch;i++) if(g_ch[i].on && !g_ch[i].master) fprintf(f,"%s\n",g_ch[i].name);
  fclose(f); g_enabled_stamp=file_stamp(g_enabled_path); }
/* for the in-game menu (dread_ui/ov.cpp): the list, and switching one on or off (saved to enabled.txt) */
int cheats_count(void){ return g_nch; }
const char* cheats_name(int i){ return i>=0 && i<g_nch? g_ch[i].name : ""; }
int cheats_is_on(int i){ return i>=0 && i<g_nch && g_ch[i].on; }
int cheats_is_master(int i){ return i>=0 && i<g_nch && g_ch[i].master; }
void cheats_set(int i, int on){ if(i<0 || i>=g_nch || g_ch[i].master) return; g_ch[i].on=on!=0; write_enabled();
  LOG("cheats: %s %s (menu)\n",g_ch[i].name,on? "on" : "off"); }

void cheats_init(void){
  g_trace= getenv("SWR_CHEAT_TRACE")!=NULL;
  const SwrModInfo* main_mi=NULL;
  for(u32 i=0;i<g_mod->nmods;i++) if(!strcmp(g_mod->mods[i].name,"main")) main_mi=&g_mod->mods[i];
  if(!main_mi) return;
  g_main_base=main_mi->base; g_main_size=main_mi->size; g_text_lo=main_mi->base+main_mi->text_off; g_text_hi=g_text_lo+main_mi->text_size;
  char bid[17]; for(int k=0;k<8;k++) snprintf(bid+k*2,3,"%02X",main_mi->build_id[k]);
  char dir[1200]; snprintf(dir,sizeof dir,"%scheats",g_datadir);
  PDir d; if(plat_dir_open(&d,dir)) return;
  g_ch=calloc(CH_MAX,sizeof *g_ch); if(!g_ch){ plat_dir_close(&d); return; }
  while(plat_dir_next(&d)){ const char* n=d.name; size_t L=strlen(n);
    if(d.is_dir || L<5 || (strcmp(n+L-4,".txt") && strcmp(n+L-4,".TXT")) || !strcmp(n,"enabled.txt")) continue;
    if(is_bid_name(n) && strncasecmp(n,bid,16)){ LOG("cheats: %s is for another version of the game (this one is %s): skipped\n",n,bid); continue; }
    char p[1400]; snprintf(p,sizeof p,"%s/%s",dir,n); int before=g_nch; load_file(p);
    LOG("cheats: %s: %d cheat(s) (build id %s)\n",n,g_nch-before,bid); }
  plat_dir_close(&d);
  snprintf(g_enabled_path,sizeof g_enabled_path,"%s/enabled.txt",dir);
  if(g_nch){ read_enabled(); g_enabled_stamp=file_stamp(g_enabled_path); } }

/* main loop, every frame: runs the VM at 12 Hz */
void cheats_tick(void){
  if(!g_nch) return;
  static u64 next, check; u64 now=plat_ns(); if(now<next) return; next=now+83333333ull;
  if(now>=check){ check=now+1000000000ull; u64 st=file_stamp(g_enabled_path); if(st!=g_enabled_stamp){ g_enabled_stamp=st; read_enabled(); } }
  for(int i=0;i<g_nch;i++) if(g_ch[i].on) run(&g_ch[i]); }
