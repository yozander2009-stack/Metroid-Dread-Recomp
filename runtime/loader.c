/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* NSO/NRO loading. The code segment has to match what we recompiled byte for byte. */
#include "rt.h"

u64 g_code_lo, g_code_hi;

static int lz4_dec(const u8* src, u32 slen, u8* dst, u32 dlen){
  u32 i=0, o=0;
  while(i<slen){
    u8 tok=src[i++]; u32 l=tok>>4;
    if(l==15){ u8 b; do{ b=src[i++]; l+=b; }while(b==255); }
    if(o+l>dlen || i+l>slen) return -1; memcpy(dst+o,src+i,l); i+=l; o+=l;
    if(i>=slen) break;
    u32 off=src[i]|(src[i+1]<<8); i+=2; u32 m=tok&15;
    if(m==15){ u8 b; do{ b=src[i++]; m+=b; }while(b==255); }
    m+=4; if(off>o || o+m>dlen) return -1;
    for(u32 k=0;k<m;k++){ dst[o]=dst[o-off]; o++; }
  }
  return (int)o; }

static u8* read_all(const char* path, u64* size){
  FILE* f=fopen(path,"rb"); if(!f) return NULL; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  u8* b=malloc(n>0?n:1); if(fread(b,1,n,f)!=(size_t)n){ fclose(f); free(b); return NULL; } fclose(f); *size=(u64)n; return b; }

/* ini ultrawide=1 (or SWR_ULTRAWIDE=1): theboy181's "21:9" patch for Metroid Dread v2.1.0
   (github.com/theboy181/switch-ptchtxt-mods, commissioned by Nikorasu081): the game's base render size in main's data,
   main+0x1A1BE18, the docked render size 1600x900 -> 1680x720, so it draws a 21:9 picture. Only applied to that exact main (build id) and
   only over the expected original bytes.
   ultrawide=2 (ini aspect=16:10, SWR_ULTRAWIDE=2) uses the same spot for a 16:10 picture: 1440x900, the docked height, which
   scales exactly to 1280x800 (Steam Deck) and 1920x1200.
   ultrawide=3 (aspect=4:3) uses 1200x900 for 4:3 handheld displays. */
int g_opt_ultrawide;
static void dread_ultrawide(const SwrModInfo* mi){
  static const u8 id[20]={0x64,0x67,0x61,0xF6,0x43,0xAF,0xEB,0xB3,0x79,0xED,0xD5,0xE6,0xA5,0x15,0x1A,0xF2,0xCE,0xF9,0x3D,0xC1};
  static const u8 orig[6]={0x40,0x06,0x00,0x00,0x84,0x03}, uw[6]={0x90,0x06,0x00,0x00,0xD0,0x02},   /* 1600x900 -> 1680x720 (low halves of two u32s) */
                  w1610[6]={0xA0,0x05,0x00,0x00,0x84,0x03},                                        /* 1600x900 -> 1440x900 */
                  w43[6]={0xB0,0x04,0x00,0x00,0x84,0x03};                                          /* 1600x900 -> 1200x900 */
  const char* e=getenv("SWR_ULTRAWIDE"); int on= e? e[0]-'0' : g_opt_ultrawide;
  if(on!=1 && on!=2 && on!=3) return;
  const u8* wide= on==3? w43 : on==2? w1610 : uw;
  if(!on || strcmp(mi->name,"main") || memcmp(mi->build_id,id,20)) return;
  u64 off=0x1A1BE18ull; if(off+6>mi->size) return;
  u8* p=GP(mi->base+off);
  if(!memcmp(p,wide,6)) return;
  if(memcmp(p,orig,6)){ WARN("ultrawide: unexpected bytes at main+%llx (%02x %02x %02x %02x %02x %02x): not applied\n",(unsigned long long)off,p[0],p[1],p[2],p[3],p[4],p[5]); return; }
  memcpy(p,wide,6);
  if(on==3) LOG("aspect: 4:3 patch applied (1200x900 base)\n");
  else if(on==2) LOG("aspect: 16:10 patch applied (1440x900 base; at theboy181's 21:9 mod location)\n");
  else LOG("ultrawide: 21:9 patch applied (1680x720 base; theboy181's 21:9 mod)\n"); }

static int load_one(const SwrModInfo* mi){
  char p[1200]; u64 fsz=0; u8* f=NULL;
  const char* dirs[]={"exefs/",""};
  for(int k=0;k<2 && !f;k++){ snprintf(p,sizeof p,"%s%s%s",g_datadir,dirs[k],mi->name); f=read_all(p,&fsz);
    if(!f && mi->is_nro){ snprintf(p,sizeof p,"%s%s%s.nro",g_datadir,dirs[k],mi->name); f=read_all(p,&fsz); } }
  if(!f){ WARN("Cannot find module %s in %sexefs/\n",mi->name,g_datadir); return -1; }
  u64 size=mi->size;
  if(mem_map(mi->base,size,MS_CODE,PERM_RW)) { WARN("could not map %s at %llx\n",mi->name,(unsigned long long)mi->base); return -1; }
  u8* dst=GP(mi->base);
  if(!memcmp(f,"NSO0",4)){
    u32 flags; memcpy(&flags,f+0xC,4); u32 seg[3][3]; for(int i=0;i<3;i++) memcpy(seg[i],f+0x10+i*0x10,12); u32 fs[3]; memcpy(fs,f+0x60,12);
    if(memcmp(f+0x40,mi->build_id,32)){ WARN("WARNING: module %s is not the one that was recompiled (different build id).\n",mi->name); free(f); return -2; }
    for(int i=0;i<3;i++){
      if(flags&(1u<<i)){ if(lz4_dec(f+seg[i][0],fs[i],dst+seg[i][1],seg[i][2])!=(int)seg[i][2]){ WARN("LZ4 failed in %s\n",mi->name); free(f); return -1; } }
      else memcpy(dst+seg[i][1],f+seg[i][0],seg[i][2]); }
  } else if(fsz>0x60 && !memcmp(f+0x10,"NRO0",4)){
    if(memcmp(f+0x40,mi->build_id,32)){ WARN("WARNING: module %s is not the one that was recompiled (different build id).\n",mi->name); free(f); return -2; }
    u32 isz; memcpy(&isz,f+0x18,4); u64 n= isz<fsz? isz : fsz; if(n>size) n=size;   /* never past the mapping */
    memcpy(dst,f,n);
  } else { WARN("%s: unknown format\n",p); free(f); return -1; }
  free(f);
  dread_ultrawide(mi);
  /* .text RX, .rodata R, .data/.bss RW */
  u64 t0=mi->base+mi->text_off, r0=mi->base+mi->ro_off, d0=mi->base+mi->data_off;
  mem_set_state(t0,((mi->text_size+0xFFF)&~0xFFFu),MS_CODE,PERM_RX);
  mem_set_state(r0,((mi->ro_size+0xFFF)&~0xFFFu),MS_CODE,PERM_R);
  mem_set_state(d0,size-(d0-mi->base),MS_CODEDATA,PERM_RW);
  LOG("module %-8s at %010llx (%.1f MB)\n",mi->name,(unsigned long long)mi->base,(double)size/1048576.0);
  return 0; }

int loader_load(void){
  for(u32 i=0;i<g_mod->nmods;i++) if(load_one(&g_mod->mods[i])) return -1;
  return 0; }
