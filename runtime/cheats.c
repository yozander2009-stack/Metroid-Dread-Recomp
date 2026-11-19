/* SwitchRecomp - Copyright 2026 MZ Labs. Apache-2.0; see LICENSE.txt and NOTICE.txt. */
/* Atmosphere-format cheats (GitHub issue #11): the dmnt cheat VM's opcodes, run 12 times a second like dmnt.
   Files: <game>_data/cheats/*.txt (Atmosphere's <build id>.txt, or any name); a file named after a different build
   id is skipped. Entries are [Name] blocks; a {Master} block always runs. Which ones run: cheats/enabled.txt, one
   name per line (the Play screen writes it). Memory types: 0 main, 1 heap, 2 alias, 3 ASLR region.
   The game's code is recompiled to native code, so cheats that patch instructions (writes into main's .text) can't
   change it: those writes are refused and the log says which cheat made them, unless the patch is a known one (KNOWN
   below), which then switches the matching built-in game patch. Data cheats (values and pointer chains in the heap)
   work as on a console. Bad pointers are checked against the memory map, never followed blindly.
   Built-in cheats (GitHub issue #70): infinite HP, infinite ammo and an always-successful E.M.M.I. counter are game
   patches (setup's dread_v210_patches.json: code_043.c, code_071.c, code_150.c) switched by swr_cheats, a variable
   the patched game code exports; the moon jump is a dmnt cheat (pointer chain) the VM runs. Built-ins are listed first
   and switched like the others (enabled.txt: "built-in: name"). */
#include "rt.h"
#include <ctype.h>

#define CH_MAX 256
#define CH_OPS 1024
typedef struct { char name[96]; u32 op[CH_OPS]; u32 n; int master, on, warned, traced, kind, builtin; u32 bits; } Cheat;
static Cheat* g_ch; static int g_nch;
static u64 g_main_base, g_main_size, g_text_lo, g_text_hi;
volatile u64 g_cheat_keys;   /* the buttons the game gets (backends), for the keypress conditional (8kkkkkkk) */

enum { CB_MISSILES=1, CB_PBOMBS=2, CB_EMMI=4, CB_HP=8 };   /* swr_cheats bits, as the game patches read them */
/* the built-in cheats: game patches (bits), or a cheat in the usual dmnt format that the VM below runs (code) */
static const struct { const char* name; const char* help; u32 bits; const char* code; } BUILTIN[]={
  {"Infinite HP","Samus's energy never goes down; the HUD shows an infinity sign for it.",CB_HP,NULL},
  {"Infinite ammo","Missiles and power bombs never go down; the HUD shows an infinity sign for them.",CB_MISSILES|CB_PBOMBS,NULL},
  {"E.M.M.I. counter always succeeds","An E.M.M.I. that catches Samus always lets her break free.",CB_EMMI,NULL},
  /* while B (jump) is held: Samus's vertical speed set to 1536 up (a jump starts at ~1600). Samus as the game finds her
     (F_8DF4CD4): the game object (the pointer in GOT slot main+0x1CBA088), its +0x2998 -> +0x10 = her entity; her
     movement component at +0x1F0, vertical speed (float, up positive) at +0x1DC. Plain dmnt: works on a console too */
  {"Moon jump","Hold the jump button and Samus keeps rising.",0,
   "[Moon jump]\n80000002\n580F0000 01CBA088\n580F1000 00000000\n580F1000 00002998\n580F1000 00000010\n580F1000 000001F0\n"
   "780F0000 000001DC\n640F0000 00000000 44C00000\n20000000\n"} };
#define NBUILTIN ((int)(sizeof BUILTIN/sizeof *BUILTIN))
#define BUILTIN_KEY "built-in: "   /* a built-in cheat's line in enabled.txt: this, then its name */
/* code patches of well-known 2.1.0 cheats (offsets in main) and the game patch each one switches here. bit 0 is the
   game's own instruction, which "off" cheats write back: that switches the patch at that place off again */
static const struct { u32 off, word, bit; } KNOWN[]={
  {0x84F274,0x1E2703E1,CB_MISSILES}, {0x84F274,0x1E3E1001,0},   /* "Missiles not reduced" (#70): fmov s1,wzr for fmov s1,#-1.0 (F_885422C) */
  {0x85694C,0x1E2703E1,CB_PBOMBS},   {0x85694C,0x1E3E1001,0},   /* "Power bombs not reduced": the same in F_885B7EC */
  {0x50EB4C,0x52800028,CB_EMMI},     {0x50EB4C,0x39417108,0},   /* "Auto Break free (Counter) E.M.M.I": mov w8,#1 for ldrb w8,[x8,#92] (F_85139E4) */
  {0x1167224,0x52800020,CB_HP},      {0x1167224,0x2A1403E0,0},  /* "Invincible" (F_916BC6C): the energy patch here */
  {0x1160494,0x34000123,CB_HP},      {0x1160494,0x5400012C,0},  /* "Invincible (ASM)" (F_916546C): the energy patch here */
  {0x1160484,0x2D4A8008,0} };                                   /* "health restore code": the game's own instruction */
#define NKNOWN ((int)(sizeof KNOWN/sizeof *KNOWN))
static volatile unsigned* g_swr_cheats;   /* the patched game code's swr_cheats; NULL = game code made before the patches */
static u32 g_fbits;   /* this tick: the game patches that cheat files' known code patches ask for */
static int known_word(u64 off, u32 word){ for(int j=0;j<NKNOWN;j++) if(KNOWN[j].off==off && KNOWN[j].word==word) return j; return -1; }
static u32 known_bits_at(u64 off){ u32 b=0; for(int j=0;j<NKNOWN;j++) if(KNOWN[j].off==off) b|=KNOWN[j].bit; return b; }

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
/* a write into main's code: a known patch switches its game patch (words of 4 bytes, an 8-byte write is two) */
static int code_write(Cheat* c, u64 a, u32 w, u64 v){
  u64 off=a-g_main_base; int ok=(w==4 || w==8) && !(off&3);
  for(u32 k=0; ok && k<w; k+=4) if(known_word(off+k,(u32)(v>>(k*8)))<0) ok=0;
  if(!ok){ if(!c->warned){ c->warned=1; WARN("cheats: \"%s\" changes the game's code (main+%llx), which is recompiled here: that part does nothing. "
      "The built-in cheats (Play screen > Cheats) give infinite HP, infinite ammo and an E.M.M.I. counter that always works\n",c->name,(unsigned long long)off); }
    return 0; }
  if(!g_swr_cheats){ if(!c->warned){ c->warned=1; WARN("cheats: \"%s\" needs the game code rebuilt (PLAY on the Play screen does it)\n",c->name); } return 0; }
  for(u32 k=0;k<w;k+=4){ int j=known_word(off+k,(u32)(v>>(k*8))); if(KNOWN[j].bit) g_fbits|=KNOWN[j].bit; else g_fbits&=~known_bits_at(off+k); }
  TRACE(c,"cheats: \"%s\": its code patch at main+%llx runs as a built-in game patch\n",c->name,(unsigned long long)off);
  return 1; }
static int wr(Cheat* c, u64 a, u32 w, u64 v){
  if(a<g_text_hi && a+w>g_text_lo) return code_write(c,a,w,v);
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

/* what a cheat can do here: 0 changes values (works as on a console; also "off" cheats that only write the game's own
   instructions back), 1 its code patches are known ones that switch built-in game patches on, 2 it patches other game
   code (that part does nothing). Static writes (type 0) into main's code [lo, hi) count; offsets are main's */
static int classify(const Cheat* c, u64 lo, u64 hi){
  int k=0;
  for(u32 i=0;i<c->n;){ const u32* p=c->op+i; u32 a=p[0], l=op_len(p,c->n-i); if(!l || i+l>c->n) break;
    if((a>>28)==0 && !((a>>20)&0xF)){ u32 w=(a>>24)&0xF; u64 off=((u64)(a&0xFF)<<32)|p[1];
      if(off<hi && off+w>lo){ u64 v= w==8? ((u64)p[2]<<32)|p[3] : p[2]; int ok=(w==4 || w==8) && !(off&3), on=0;
        for(u32 q=0; ok && q<w; q+=4){ int j=known_word(off+q,(u32)(v>>(q*8))); if(j<0) ok=0; else if(KNOWN[j].bit) on=1; }
        if(!ok) k=2; else if(on && !k) k=1; } }
    i+=l; }
  return k; }

static int hexword(const char* s, u32* v){ u32 x=0; for(int k=0;k<8;k++){ int ch=(unsigned char)s[k]; if(!isxdigit(ch)) return 0; x=x*16+(u32)(isdigit(ch)? ch-'0' : (tolower(ch)-'a'+10)); } *v=x; return 1; }
/* a cheat file's text: its [Name] and {Master} blocks with their words, into out[0..max) */
static int parse_text(const char* t, size_t len, Cheat* out, int max){
  int n=0; Cheat* cur=NULL;
  for(size_t i=0;i<len;){
    const char *s=t+i, *end=memchr(s,'\n',len-i); if(!end) end=t+len; i=(size_t)(end-t)+1;
    while(s<end && (*s==' '||*s=='\t'||*s=='\r'||(unsigned char)*s==0xEF||(unsigned char)*s==0xBB||(unsigned char)*s==0xBF)) s++;
    if(s<end && (*s=='[' || *s=='{')){ char close= *s=='['? ']' : '}'; const char* e=memchr(s+1,close,(size_t)(end-s-1));
      if(!e || n>=max){ cur=NULL; continue; }
      cur=&out[n++]; memset(cur,0,sizeof *cur); cur->master= *s=='{';
      size_t k=(size_t)(e-s-1); if(k>=sizeof cur->name) k=sizeof cur->name-1; memcpy(cur->name,s+1,k); cur->name[k]=0; continue; }
    if(!cur) continue;
    for(;;){ while(s<end && (*s==' '||*s=='\t')) s++; u32 v; if(end-s<8 || !hexword(s,&v)) break; if(cur->n<CH_OPS) cur->op[cur->n++]=v; s+=8; } }
  return n; }
static void load_file(const char* path){
  FILE* f=fopen(path,"rb"); if(!f) return;
  fseek(f,0,SEEK_END); long L=ftell(f); fseek(f,0,SEEK_SET);
  char* t= L>0 && L<(16l<<20)? malloc((size_t)L) : NULL;
  if(t && fread(t,1,(size_t)L,f)==(size_t)L){
    int k=parse_text(t,(size_t)L,g_ch+g_nch,CH_MAX-g_nch);
    for(int i=g_nch;i<g_nch+k;i++) g_ch[i].kind=classify(&g_ch[i],g_text_lo-g_main_base,g_text_hi-g_main_base);
    g_nch+=k; }
  free(t); fclose(f); }

static int is_bid_name(const char* n){ int k=0; for(;k<16;k++) if(!isxdigit((unsigned char)n[k])) return 0; return !strcmp(n+16,".txt") || !strcmp(n+16,".TXT"); }

/* enabled.txt: which cheats run. Read at start and again whenever it changes (the in-game menu writes it), so a
   cheat switches on or off within a second */
static char g_dir[1200], g_enabled_path[1300]; static u64 g_enabled_stamp;
static u64 file_stamp(const char* path){   /* a hash of the contents, 0 = no file */
  FILE* f=fopen(path,"rb"); if(!f) return 0; u64 h=1469598103934665603ull; int ch; while((ch=fgetc(f))!=EOF) h=(h^(u64)ch)*1099511628211ull; fclose(f); return h|1; }
static int key_is(const Cheat* c, const char* line){   /* is this enabled.txt line c's? */
  if(!c->builtin) return !strcmp(c->name,line);
  return !strncmp(line,BUILTIN_KEY,sizeof BUILTIN_KEY-1) && !strcmp(line+sizeof BUILTIN_KEY-1,c->name); }
static void read_enabled(void){
  for(int i=0;i<g_nch;i++) g_ch[i].on=g_ch[i].master;
  FILE* f=fopen(g_enabled_path,"rb");
  if(f){ char line[200]; while(fgets(line,sizeof line,f)){ size_t L=strlen(line); while(L && (line[L-1]=='\n'||line[L-1]=='\r')) line[--L]=0;
      for(int i=0;i<g_nch;i++) if(!g_ch[i].master && key_is(&g_ch[i],line)) g_ch[i].on=1; } fclose(f); }
  int on=0; for(int i=0;i<g_nch;i++) if(g_ch[i].on){ on++; LOG("cheats: on: %s%s%s%s\n",g_ch[i].builtin? BUILTIN_KEY : "",g_ch[i].name,g_ch[i].master? " (master code)" : "",
      g_ch[i].kind==2? " (patches game code: that part does nothing)" : g_ch[i].kind==1? " (its code patch runs as a built-in game patch)" : ""); }
  LOG("cheats: %d of %d on\n",on,g_nch); }
static void write_enabled(void){
  FILE* f=fopen(g_enabled_path,"wb"); if(!f){ plat_mkdir(g_dir); f=fopen(g_enabled_path,"wb"); } if(!f) return;
  for(int i=0;i<g_nch;i++) if(g_ch[i].on && !g_ch[i].master) fprintf(f,"%s%s\n",g_ch[i].builtin? BUILTIN_KEY : "",g_ch[i].name);
  fclose(f); g_enabled_stamp=file_stamp(g_enabled_path); }
/* for the in-game menu (dread_ui/ov.cpp): the list, and switching one on or off (saved to enabled.txt) */
int cheats_count(void){ return g_nch; }
const char* cheats_name(int i){ return i>=0 && i<g_nch? g_ch[i].name : ""; }
int cheats_is_on(int i){ return i>=0 && i<g_nch && g_ch[i].on; }
int cheats_is_master(int i){ return i>=0 && i<g_nch && g_ch[i].master; }
int cheats_is_builtin(int i){ return i>=0 && i<g_nch && g_ch[i].builtin; }
int cheats_kind(int i){ return i>=0 && i<g_nch? g_ch[i].kind : 0; }   /* classify(): 2 = patches game code, does nothing */
void cheats_set(int i, int on){ if(i<0 || i>=g_nch || g_ch[i].master) return; g_ch[i].on=on!=0; write_enabled();
  LOG("cheats: %s%s %s (menu)\n",g_ch[i].builtin? BUILTIN_KEY : "",g_ch[i].name,on? "on" : "off"); }

/* for the Play screen (dread_ui fe.cpp, fe_sys*.cpp), before the game runs: the built-in cheats, and what each [cheat]
   of a cheat file's text can do here (classify(), in file order). text_size = exefs/main's code size. Returns the
   number of blocks */
int cheats_builtin_count(void){ return NBUILTIN; }
const char* cheats_builtin_name(int i){ return i>=0 && i<NBUILTIN? BUILTIN[i].name : ""; }
const char* cheats_builtin_help(int i){ return i>=0 && i<NBUILTIN? BUILTIN[i].help : ""; }
const char* cheats_builtin_key(void){ return BUILTIN_KEY; }
int cheats_text_scan(const char* text, size_t len, unsigned text_size, void (*fn)(void* u, const char* name, int master, int kind, int words), void* u){
  Cheat* tmp=calloc(CH_MAX,sizeof *tmp); if(!tmp) return 0;
  int n=parse_text(text,len,tmp,CH_MAX);
  for(int i=0;i<n;i++) fn(u,tmp[i].name,tmp[i].master,classify(&tmp[i],0,text_size),(int)tmp[i].n);
  free(tmp); return n; }

void cheats_init(void){
  g_trace= getenv("SWR_CHEAT_TRACE")!=NULL;
  const SwrModInfo* main_mi=NULL;
  for(u32 i=0;i<g_mod->nmods;i++) if(!strcmp(g_mod->mods[i].name,"main")) main_mi=&g_mod->mods[i];
  if(!main_mi) return;
  g_main_base=main_mi->base; g_main_size=main_mi->size; g_text_lo=main_mi->base+main_mi->text_off; g_text_hi=g_text_lo+main_mi->text_size;
  char bid[17]; for(int k=0;k<8;k++) snprintf(bid+k*2,3,"%02X",main_mi->build_id[k]);
  snprintf(g_dir,sizeof g_dir,"%scheats",g_datadir);
  snprintf(g_enabled_path,sizeof g_enabled_path,"%s/enabled.txt",g_dir);
  { extern void* g_code_mod; if(g_code_mod) g_swr_cheats=(volatile unsigned*)plat_dl_sym(g_code_mod,"swr_cheats"); }
  g_ch=calloc(CH_MAX,sizeof *g_ch); if(!g_ch) return;
  for(int k=0;k<NBUILTIN;k++){   /* game-patch ones only with game code that has the patches */
    if(BUILTIN[k].bits && !g_swr_cheats) continue;
    Cheat* c=&g_ch[g_nch];
    if(BUILTIN[k].code){ if(parse_text(BUILTIN[k].code,strlen(BUILTIN[k].code),c,1)!=1) continue; }
    else memset(c,0,sizeof *c);
    snprintf(c->name,sizeof c->name,"%s",BUILTIN[k].name); c->builtin=1; c->bits=BUILTIN[k].bits; g_nch++; }
  if(!g_swr_cheats) LOG("cheats: this game code was made before the built-in game patches: they come with its next rebuild (PLAY on the Play screen)\n");
  PDir d;
  if(!plat_dir_open(&d,g_dir)){
    while(plat_dir_next(&d)){ const char* n=d.name; size_t L=strlen(n);
      if(d.is_dir || L<5 || (strcmp(n+L-4,".txt") && strcmp(n+L-4,".TXT")) || !strcmp(n,"enabled.txt")) continue;
      if(is_bid_name(n) && strncasecmp(n,bid,16)){ LOG("cheats: %s is for another version of the game (this one is %s): skipped\n",n,bid); continue; }
      char p[1400]; snprintf(p,sizeof p,"%s/%s",g_dir,n); int before=g_nch; load_file(p);
      LOG("cheats: %s: %d cheat(s) (build id %s)\n",n,g_nch-before,bid); }
    plat_dir_close(&d); }
  if(g_nch){ read_enabled(); g_enabled_stamp=file_stamp(g_enabled_path); } }

/* SWR_CHEAT_TRACE: Samus's energy and vertical speed twice a second (checks Infinite HP and the moon jump). Samus as the
   game finds her: the game object ([GOT 0x9CBF088]), its player (F_8DF1788), her life component at +448 (+80 maximum
   and +84 current energy, floats), her movement component at +0x1F0 (+0x1DC vertical speed). Energy is never written
   from here: the HUD only follows the game's own setter, so Infinite HP is the damage-routine patch (code_150.c) */
static void samus_trace(void){
  static u64 said; u64 now=plat_ns(); if(now<said) return; said=now+500000000ull;
  u64 slot,g,t,p,l,mx,cur,mv=0,vy=0;
  if(!rd(g_main_base+0x1CBA088,8,&slot) || !rd(slot,8,&g) || !g || !rd(g+10680,8,&t) || !t || !rd(t+16,8,&p) || !p
     || !rd(p+448,8,&l) || !l || !rd(l+80,4,&mx) || !rd(l+84,4,&cur)) return;
  u32 m32=(u32)mx, c32=(u32)cur, v32=0; float fm, fc, fv; memcpy(&fm,&m32,4); memcpy(&fc,&c32,4);
  if(rd(p+0x1F0,8,&mv) && mv && rd(mv+0x1DC,4,&vy)) v32=(u32)vy;
  memcpy(&fv,&v32,4);
  LOG("cheats: Samus's energy %.1f of %.1f, vertical speed %.0f, buttons %llx\n",fc,fm,fv,(unsigned long long)g_cheat_keys); }

/* main loop, every frame: runs the VM at 12 Hz */
void cheats_tick(void){
  if(!g_nch) return;
  static u64 next, check; u64 now=plat_ns(); if(now<next) return; next=now+83333333ull;
  if(now>=check){ check=now+1000000000ull; u64 st=file_stamp(g_enabled_path); if(st!=g_enabled_stamp){ g_enabled_stamp=st; read_enabled(); } }
  u32 bits=0; g_fbits=0;
  for(int i=0;i<g_nch;i++) if(g_ch[i].on){ bits|=g_ch[i].bits; if(g_ch[i].n) run(&g_ch[i]); }
  bits|=g_fbits;
  if(g_swr_cheats && *g_swr_cheats!=bits){ *g_swr_cheats=bits;
    LOG("cheats: game patches:%s%s%s%s%s\n",bits&CB_HP? " infinite HP" : "",bits&CB_MISSILES? " infinite missiles" : "",
        bits&CB_PBOMBS? " infinite power bombs" : "",bits&CB_EMMI? " E.M.M.I. break-free" : "",bits? "" : " none"); }
  if(g_trace) samus_trace(); }
