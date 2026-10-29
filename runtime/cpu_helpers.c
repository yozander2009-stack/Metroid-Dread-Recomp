/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* Helpers that get compiled into the game code module: FP with exact arm semantics (saturation, NaN
   handling, min/max, recip estimates), the AdvSIMD ops we don't emit inline, crypto (AES/SHA/CRC/PMULL)
   and sysregs. No libm on purpose, this has to build the same with gcc, clang and tcc. */
#include "cpu.h"

/* =============================================================== rounding */
static f64 fabs_(f64 x){ u64 b; memcpy(&b,&x,8); b&=~(1ull<<63); memcpy(&x,&b,8); return x; }

static int isnan64(f64 x){ return x!=x; }

static int isnan32(f32 x){ return x!=x; }

static f64 trunc_(f64 x){ if(!(fabs_(x)<4503599627370496.0)) return x; f64 r=(f64)(i64)x; if(r==0 && x<0) return -0.0; return r; }

static f64 floor_(f64 x){ f64 t=trunc_(x); if(t>x) t-=1.0; if(t==0 && x<0) return x==0? x : -0.0; return t; }

static f64 ceil_(f64 x){ f64 t=trunc_(x); if(t<x) t+=1.0; if(t==0 && x<0) return -0.0; return t; }

static f64 rne_(f64 x){ if(!(fabs_(x)<4503599627370496.0)) return x;
  f64 t=floor_(x), d=x-t; f64 r;
  if(d>0.5) r=t+1; else if(d<0.5) r=t; else r= (((i64)t)&1)? t+1 : t;
  if(r==0 && (x<0 || (x==0 && 1/x<0))) return -0.0; return r; }

static f64 rna_(f64 x){ if(!(fabs_(x)<4503599627370496.0)) return x; f64 t=trunc_(x); if(fabs_(x-t)>=0.5) t+= x<0? -1.0 : 1.0; if(t==0 && x<0) return -0.0; return t; }

f64 fp_round(f64 x, int rm){
  if(x==0) return x;
  switch(rm){ case 0: return rne_(x); case 1: return ceil_(x); case 2: return floor_(x); case 3: return trunc_(x); default: return rna_(x); } }

f32 fp_round32(f32 x, int rm){ return (f32)fp_round((f64)x,rm); }

f32 fp_frintx32(f32 x){ return (f32)rne_(x); }

f64 fp_frintx64(f64 x){ return rne_(x); }

i64 fcvt_s64(f64 x, int rm){ if(isnan64(x)) return 0; x=fp_round(x,rm);
  if(x>=9223372036854775808.0) return 0x7FFFFFFFFFFFFFFFll; if(x<-9223372036854775808.0) return (i64)0x8000000000000000ull; return (i64)x; }

u64 fcvt_u64(f64 x, int rm){ if(isnan64(x)) return 0; x=fp_round(x,rm);
  if(x>=18446744073709551616.0) return ~0ull; if(x<=0) return 0;
  if(x>=9223372036854775808.0) return (u64)(i64)(x-9223372036854775808.0)+0x8000000000000000ull; return (u64)(i64)x; }

i32 fcvt_s32(f64 x, int rm){ if(isnan64(x)) return 0; x=fp_round(x,rm);
  if(x>=2147483648.0) return 0x7FFFFFFF; if(x<-2147483648.0) return (i32)0x80000000u; return (i32)x; }

u32 fcvt_u32(f64 x, int rm){ if(isnan64(x)) return 0; x=fp_round(x,rm);
  if(x>=4294967296.0) return 0xFFFFFFFFu; if(x<=0) return 0; return (u32)(i64)x; }

/* ================================================================ min/max */
f32 fp_max32(f32 a,f32 b){ if(isnan32(a)) return a+a; if(isnan32(b)) return b+b; if(a==0&&b==0){ u32 x,y; memcpy(&x,&a,4); memcpy(&y,&b,4); return (x&y)>>31? a : (x>>31? b : a); } return a>b?a:b; }

f32 fp_min32(f32 a,f32 b){ if(isnan32(a)) return a+a; if(isnan32(b)) return b+b; if(a==0&&b==0){ u32 x,y; memcpy(&x,&a,4); memcpy(&y,&b,4); return (x|y)>>31? (x>>31?a:b) : a; } return a<b?a:b; }

f64 fp_max64(f64 a,f64 b){ if(isnan64(a)) return a+a; if(isnan64(b)) return b+b; if(a==0&&b==0){ u64 x,y; memcpy(&x,&a,8); memcpy(&y,&b,8); return (x&y)>>63? a : (x>>63? b : a); } return a>b?a:b; }

f64 fp_min64(f64 a,f64 b){ if(isnan64(a)) return a+a; if(isnan64(b)) return b+b; if(a==0&&b==0){ u64 x,y; memcpy(&x,&a,8); memcpy(&y,&b,8); return (x|y)>>63? (x>>63?a:b) : a; } return a<b?a:b; }

static int qnan32(f32 a){ u32 x; memcpy(&x,&a,4); return isnan32(a) && (x&0x00400000); }

static int qnan64(f64 a){ u64 x; memcpy(&x,&a,8); return isnan64(a) && (x&0x0008000000000000ull); }

f32 fp_maxnm32(f32 a,f32 b){ if(qnan32(a)&&!isnan32(b)) return b; if(qnan32(b)&&!isnan32(a)) return a; return fp_max32(a,b); }

f32 fp_minnm32(f32 a,f32 b){ if(qnan32(a)&&!isnan32(b)) return b; if(qnan32(b)&&!isnan32(a)) return a; return fp_min32(a,b); }

f64 fp_maxnm64(f64 a,f64 b){ if(qnan64(a)&&!isnan64(b)) return b; if(qnan64(b)&&!isnan64(a)) return a; return fp_max64(a,b); }

f64 fp_minnm64(f64 a,f64 b){ if(qnan64(a)&&!isnan64(b)) return b; if(qnan64(b)&&!isnan64(a)) return a; return fp_min64(a,b); }

f32 fp_nan32(f32 r, f32 a, f32 b){ (void)a;(void)b; return r; }

f64 fp_nan64(f64 r, f64 a, f64 b){ (void)a;(void)b; return r; }

f32 fp_abd32(f32 a,f32 b){ f32 d=a-b; return d<0? -d : d; }

void fp_cmp64(CPU* c, f64 a, f64 b){
  if(isnan64(a)||isnan64(b)) set_nzcv(c,3); else if(a==b) set_nzcv(c,6); else if(a<b) set_nzcv(c,8); else set_nzcv(c,2); }

void fp_cmp32(CPU* c, f32 a, f32 b){ fp_cmp64(c,a,b); }

/* sqrt: one Newton step on top of the hw estimate */
#if defined(__TINYC__)
f64 fp_sqrt64(f64 x){
  if(isnan64(x)||x==0) return x; if(x<0) { u64 n=0x7FF8000000000000ull; f64 r; memcpy(&r,&n,8); return r; }
  /* hardware sqrtsd is correctly rounded; tcc's assembler has no mnemonic for it */
  f64 r; __asm__ __volatile__("movq %1, %%xmm0\n\t.byte 0xF2,0x0F,0x51,0xC0\n\tmovq %%xmm0, %0" : "=m"(r) : "m"(x) : "memory");
  return r; }
#else
f64 fp_sqrt64(f64 x){ if(x<0){ u64 n=0x7FF8000000000000ull; memcpy(&x,&n,8); return x; } return __builtin_sqrt(x); }
#endif

f32 fp_sqrt32(f32 x){ if(x<0){ u32 n=0x7FC00000u; memcpy(&x,&n,4); return x; } return (f32)fp_sqrt64((f64)x); }
#if defined(__FMA__) && !defined(__TINYC__)
f32 fp_fma32(f32 a, f32 b, f32 c){ return __builtin_fmaf(a,b,c); }

f64 fp_fma64(f64 a, f64 b, f64 c){ return __builtin_fma(a,b,c); }
#else
f32 fp_fma32(f32 a, f32 b, f32 c){ return (f32)((f64)a*(f64)b+(f64)c); }   /* product is exact in double */

f64 fp_fma64(f64 a, f64 b, f64 c){ return a*b+c; }
#endif

f32 fp_mulx32(f32 a, f32 b){ u32 x,y; memcpy(&x,&a,4); memcpy(&y,&b,4);
  if((!(x<<1) && (y<<1)==0xFF000000u) || (!(y<<1) && (x<<1)==0xFF000000u)) return ((x^y)>>31)? -2.0f : 2.0f; return a*b; }

f64 fp_mulx64(f64 a, f64 b){ u64 x,y; memcpy(&x,&a,8); memcpy(&y,&b,8);
  if((!(x<<1) && (y<<1)==0xFFE0000000000000ull) || (!(y<<1) && (x<<1)==0xFFE0000000000000ull)) return ((x^y)>>63)? -2.0 : 2.0; return a*b; }

/* ---- recip estimates, straight from the ARM ARM (RecipEstimate / RecipSqrtEstimate) ---- */
static u32 recip_est(u32 a){ /* a in [256,511] */ a=a*2+1; u32 b=(1u<<19)/a; return (b+1)>>1; }

static u32 rsqrt_est(u32 a){ /* a in [128,511] */
  if(a<256) a=a*2+1; else { a=(a>>1)<<1; a=(a+1)*2; }
  u32 b=512; while(a*(b+1)*(b+1) < (1u<<28)) b++; return (b+1)>>1; }

/* FPRecipEstimate / FPRSqrtEstimate, f32 and f64, assumes RN and no FZ */
static u64 recpe_bits(u64 op, int N){
  int eb= N==64? 11:8, fbits= N==64? 52:23; u64 s=op>>(N-1)<<(N-1); int e=(int)((op>>fbits)&((1u<<eb)-1)); u64 f=op&((1ull<<fbits)-1);
  u64 emax=(1u<<eb)-1;
  if(e==(int)emax && f) return op|(1ull<<(fbits-1));                   /* NaN -> quiet */
  if(e==(int)emax) return s;                                           /* inf -> 0 */
  if(e==0 && f==0) return s|(emax<<fbits);                             /* 0 -> inf */
  u64 fr= N==64? f : f<<29;                                            /* 52 bits */
  if(e==0 && !(fr>>50)) return s|(emax<<fbits);                        /* overflow -> inf (RN) */
  if(e==0){ if(!(fr&(1ull<<51))){ e=-1; fr=(fr<<2)&((1ull<<52)-1); } else fr=(fr<<1)&((1ull<<52)-1); }
  u32 sc=(u32)(256+(fr>>44)); u32 est=recip_est(sc);
  int re=(N==64? 2045 : 253)-e; u64 rf=(u64)(est&0xFF)<<44;
  if(re==0) rf=(1ull<<51)|(rf>>1); else if(re==-1){ rf=(1ull<<50)|(rf>>2); re=0; }
  return s|((u64)re<<fbits)|(N==64? rf : rf>>29); }

static u64 rsqrte_bits(u64 op, int N){
  int eb= N==64? 11:8, fbits= N==64? 52:23; u64 s=op>>(N-1); int e=(int)((op>>fbits)&((1u<<eb)-1)); u64 f=op&((1ull<<fbits)-1);
  u64 emax=(1u<<eb)-1;
  if(e==(int)emax && f) return op|(1ull<<(fbits-1));
  if(e==0 && f==0) return (s<<(N-1))|(emax<<fbits);
  if(s) return (emax<<fbits)|(1ull<<(fbits-1));                        /* default NaN */
  if(e==(int)emax) return 0;
  u64 fr= N==64? f : f<<29;
  if(e==0){ while(!(fr&(1ull<<51))){ fr<<=1; e--; } fr=(fr<<1)&((1ull<<52)-1); }
  u32 sc= !(e&1)? (u32)(256+(fr>>44)) : (u32)(128+(fr>>45));
  u32 est=rsqrt_est(sc); int re=((N==64? 3068 : 380)-e)/2;
  return ((u64)re<<fbits)|((u64)(est&0xFF)<<(fbits-8)); }

f32 fp_recpe32(f32 x){ u32 b; memcpy(&b,&x,4); b=(u32)recpe_bits(b,32); memcpy(&x,&b,4); return x; }

f64 fp_recpe64(f64 x){ u64 b; memcpy(&b,&x,8); b=recpe_bits(b,64); memcpy(&x,&b,8); return x; }

f32 fp_rsqrte32(f32 x){ u32 b; memcpy(&b,&x,4); b=(u32)rsqrte_bits(b,32); memcpy(&x,&b,4); return x; }

f64 fp_rsqrte64(f64 x){ u64 b; memcpy(&b,&x,8); b=rsqrte_bits(b,64); memcpy(&x,&b,8); return x; }

f32 fp_recps32(f32 a, f32 b){ if((a==0&&(b>3.4e38f||b<-3.4e38f))||(b==0&&(a>3.4e38f||a<-3.4e38f))) return 2.0f; return (f32)(2.0-(f64)a*(f64)b); }

f64 fp_recps64(f64 a, f64 b){ if((a==0&&(b>1.7e308||b<-1.7e308))||(b==0&&(a>1.7e308||a<-1.7e308))) return 2.0; return 2.0-a*b; }

f32 fp_rsqrts32(f32 a, f32 b){ if((a==0&&(b>3.4e38f||b<-3.4e38f))||(b==0&&(a>3.4e38f||a<-3.4e38f))) return 1.5f; return (f32)((3.0-(f64)a*(f64)b)*0.5); }

f64 fp_rsqrts64(f64 a, f64 b){ if((a==0&&(b>1.7e308||b<-1.7e308))||(b==0&&(a>1.7e308||a<-1.7e308))) return 1.5; return (3.0-a*b)/2.0; }

u32 urecpe32(u32 x){ if(!(x&0x80000000u)) return 0xFFFFFFFFu; return (recip_est(x>>23)|0x100)<<23; }

u32 ursqrte32(u32 x){ if(!(x&0xC0000000u)) return 0xFFFFFFFFu; return (rsqrt_est(x>>23)|0x100)<<23; }

/* ---- half precision ---- */
u16 f32_to_f16(f32 x){
  u32 b; memcpy(&b,&x,4); u32 s=(b>>16)&0x8000; int e=(int)((b>>23)&0xFF)-127+15; u32 m=b&0x7FFFFF;
  if(((b>>23)&0xFF)==0xFF) return (u16)(s|0x7C00|(m? 0x200|(m>>13) : 0));
  if(e>=31) return (u16)(s|0x7C00);
  if(e<=0){ if(e<-10) return (u16)s; m|=0x800000; u32 sh=(u32)(14-e); u32 r=m>>sh, rem=m&((1u<<sh)-1), half=1u<<(sh-1);
    if(rem>half || (rem==half && (r&1))) r++; return (u16)(s|r); }
  u32 r=((u32)e<<10)|(m>>13); u32 rem=m&0x1FFF; if(rem>0x1000 || (rem==0x1000 && (r&1))) r++; return (u16)(s|r); }

f32 f16_to_f32(u16 h){
  u32 s=(u32)(h&0x8000)<<16; int e=(h>>10)&0x1F; u32 m=h&0x3FF, r;
  if(e==0x1F) r=s|0x7F800000|(m<<13);
  else if(e==0){ if(!m) r=s; else { e=1; while(!(m&0x400)){ m<<=1; e--; } m&=0x3FF; r=s|((u32)(e-15+127)<<23)|(m<<13); } }
  else r=s|((u32)(e-15+127)<<23)|(m<<13);
  f32 f; memcpy(&f,&r,4); return f; }

u16 f64_to_f16(f64 x){
  u64 b; memcpy(&b,&x,8); u32 s=(u32)(b>>48)&0x8000; int E=(int)((b>>52)&0x7FF), e=E-1023+15; u64 m=b&0xFFFFFFFFFFFFFull;
  if(E==0x7FF) return (u16)(s|0x7C00|(m? 0x200|(u32)(m>>42) : 0));
  if(e>=31) return (u16)(s|0x7C00);
  if(e<=0){ if(e<-10) return (u16)s; m|=1ull<<52; u32 sh=(u32)(43-e); u64 r=m>>sh, rem=m&((1ull<<sh)-1), half=1ull<<(sh-1);
    if(rem>half || (rem==half && (r&1))) r++; return (u16)(s|r); }
  u64 r=((u64)e<<10)|(m>>42), rem=m&((1ull<<42)-1); if(rem>(1ull<<41) || (rem==(1ull<<41) && (r&1))) r++; return (u16)(s|r); }

/* ================================================================ integer */
u64 swr_decode_bitmask(u32 N, u32 imms, u32 immr, u32 ds){
  u32 v=(N<<6)|(~imms&0x3F); int len=-1; for(int i=6;i>=0;i--) if(v&(1u<<i)){ len=i; break; }
  if(len<1) return 0; u32 esize=1u<<len, levels=esize-1; u32 S=imms&levels, R=immr&levels;
  if(S==levels) return 0;
  u64 welem=bmask(S+1); if(R) welem=((welem>>R)|(welem<<(esize-R)))&bmask(esize);
  u64 r=0; for(u32 i=0;i<ds;i+=esize) r|=welem<<i; return ds==32? (u32)r : r; }

u32 swr_crc32(u32 acc, u64 val, int bytes, int castagnoli){
  u32 poly= castagnoli? 0x82F63B78u : 0xEDB88320u;
  for(int i=0;i<bytes;i++){ acc^=(u8)(val>>(i*8)); for(int k=0;k<8;k++) acc= (acc>>1)^((acc&1)? poly : 0); }
  return acc; }

#if defined(__TINYC__)
/* tcc has no __atomic but does have inline asm. Exclusives need a real lock cmpxchg here since the
   kernel side touches the same words (mutex/condvar) with real atomics; a spinlock-emulated CAS isn't
   atomic against that. */
u32 swr_cas64(u64* p, u64 o, u64 n){ u64 prev=o; __asm__ __volatile__("lock; cmpxchgq %2,%1" : "+a"(prev), "+m"(*p) : "r"(n) : "memory","cc"); return prev==o; }

u32 swr_cas32(u32* p, u32 o, u32 n){ u32 prev=o; __asm__ __volatile__("lock; cmpxchgl %2,%1" : "+a"(prev), "+m"(*p) : "r"(n) : "memory","cc"); return prev==o; }

u32 swr_cas16(u16* p, u16 o, u16 n){ u16 prev=o; __asm__ __volatile__("lock; cmpxchgw %2,%1" : "+a"(prev), "+m"(*p) : "r"(n) : "memory","cc"); return prev==o; }

u32 swr_cas8(u8* p, u8 o, u8 n){ u8 prev=o; __asm__ __volatile__("lock; cmpxchgb %2,%1" : "+a"(prev), "+m"(*p) : "q"(n) : "memory","cc"); return prev==o; }

/* cmpxchg16b clobbers rbx (and we use rdi); tcc won't save them so the asm does */
u32 swr_cas128(u64* p, u64 o0, u64 o1, u64 n0, u64 n1){
  u64 a[5]; u64 r; a[0]=(u64)p; a[1]=o0; a[2]=o1; a[3]=n0; a[4]=n1;
  if(((u64)p)&15){ u32 ok=swr_cas64(p,o0,n0); if(ok && !swr_cas64(p+1,o1,n1)){ swr_cas64(p,n0,o0); ok=0; } return ok; }   /* unaligned, shouldn't happen */
  __asm__ __volatile__(
    "push %%rbx\n\t" "push %%rdi\n\t"
    "movq 8(%%rsi),%%rax\n\t" "movq 16(%%rsi),%%rdx\n\t" "movq 24(%%rsi),%%rbx\n\t" "movq 32(%%rsi),%%rcx\n\t" "movq (%%rsi),%%rdi\n\t"
    "lock; cmpxchg16b (%%rdi)\n\t"
    "sete %%al\n\t" "movzbq %%al,%%rax\n\t"
    "pop %%rdi\n\t" "pop %%rbx"
    : "=a"(r) : "S"(a) : "rcx","rdx","memory","cc");
  return (u32)r; }
#if defined(_WIN32)
#include <windows.h>
static volatile LONG g_fz; void swr_fence(void){ InterlockedExchange(&g_fz,0); }   /* xchg is a full barrier on x86 (no __faststorefence in tcc) */
#else
void swr_fence(void){ __asm__ __volatile__("mfence" : : : "memory"); }
#endif
#else
u32 swr_cas128(u64* p, u64 o0, u64 o1, u64 n0, u64 n1){
#if (defined(__x86_64__) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)) || (defined(__aarch64__) && defined(__clang__))
  /* x86-64 cmpxchg16b; arm64 hosts (clang: casp or an ldaxp/stlxp loop, inline) */
  unsigned __int128 o=((unsigned __int128)o1<<64)|o0, n=((unsigned __int128)n1<<64)|n0;
  return __sync_bool_compare_and_swap((unsigned __int128*)p,o,n);
#else
  /* no cmpxchg16b: CAS the low half, compare the high half */
  if(__atomic_load_n(&p[1],__ATOMIC_SEQ_CST)!=o1) return 0;
  if(!swr_cas64(&p[0],o0,n0)) return 0; __atomic_store_n(&p[1],n1,__ATOMIC_SEQ_CST); return 1;
#endif
}
#endif

/* ================================================================ system */
#define SYSREG(op0,op1,crn,crm,op2) ((((op0)&1)<<14)|((op1)<<11)|((crn)<<7)|((crm)<<3)|(op2))

u64 swr_mrs(CPU* c, u32 r){
  switch(r){
    case SYSREG(3,3,13,0,2): return c->tpidr;
    case SYSREG(3,3,13,0,3): return c->tpidrro;
    case SYSREG(3,3,4,2,0): return get_nzcv(c);
    case SYSREG(3,3,4,4,0): return c->fpcr;
    case SYSREG(3,3,4,4,1): return c->fpsr;
    case SYSREG(3,3,14,0,0): return 19200000;
    case SYSREG(3,3,14,0,1): case SYSREG(3,3,14,0,2): return c->rt->ticks();
    case SYSREG(3,3,0,0,7): return 4;              /* DCZID = 64 bytes */
    case SYSREG(3,3,0,0,1): return 0x8444C004u;    /* CTR_EL0 (A57) */
    case SYSREG(3,0,0,0,0): return 0x411FD071u;    /* MIDR */
    case SYSREG(3,0,0,0,5): return 0x80000000u;    /* MPIDR */
    default: return 0;
  }
}

void swr_msr(CPU* c, u32 r, u64 v){
  switch(r){
    case SYSREG(3,3,13,0,2): c->tpidr=v; break;
    case SYSREG(3,3,4,2,0): set_nzcv(c,(u32)(v>>28)); break;
    case SYSREG(3,3,4,4,0): c->fpcr=(u32)v; if(c->rt->fpcr_changed) c->rt->fpcr_changed(c); break;
    case SYSREG(3,3,4,4,1): c->fpsr=(u32)v; break;
    default: break;
  }
}

/* =============================================================== AdvSIMD */
#define VR(n) (&c->v[n])

static u64 eget(const V128* v, int sz, int i){ switch(sz){ case 0: return v->b[i]; case 1: return v->h[i]; case 2: return v->s[i]; default: return v->d[i]; } }

static void eset(V128* v, int sz, int i, u64 x){ switch(sz){ case 0: v->b[i]=(u8)x; break; case 1: v->h[i]=(u16)x; break; case 2: v->s[i]=(u32)x; break; default: v->d[i]=x; } }

static i64 sx(u64 x, int eb){ return eb>=64? (i64)x : (i64)(x<<(64-eb))>>(64-eb); }

static u64 zx(u64 x, int eb){ return eb>=64? x : x&((1ull<<eb)-1); }

static void qc(CPU* c){ c->fpsr|=1u<<27; }

static i64 ssat(CPU* c, i64 v, int eb){ i64 r=sat_s(v,eb); if(r!=v) qc(c); return r; }

static u64 usat(CPU* c, i64 v, int eb){ u64 r=sat_u(v,eb); if((i64)r!=v) qc(c); return r; }

/* sat add/sub, 64 bit safe */
static u64 sqadd_(CPU* c, u64 a, u64 b, int eb){
  if(eb<64) return zx((u64)ssat(c,sx(a,eb)+sx(b,eb),eb),eb);
  u64 r=a+b; if((i64)((a^r)&(b^r))<0){ qc(c); return ((i64)a<0)? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull; } return r; }

static u64 uqadd_(CPU* c, u64 a, u64 b, int eb){
  if(eb<64) return usat(c,(i64)(zx(a,eb)+zx(b,eb)),eb);
  u64 r=a+b; if(r<a){ qc(c); return ~0ull; } return r; }

static u64 sqsub_(CPU* c, u64 a, u64 b, int eb){
  if(eb<64) return zx((u64)ssat(c,sx(a,eb)-sx(b,eb),eb),eb);
  u64 r=a-b; if((i64)((a^b)&(a^r))<0){ qc(c); return ((i64)a<0)? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull; } return r; }

static u64 uqsub_(CPU* c, u64 a, u64 b, int eb){
  if(eb<64) return usat(c,(i64)zx(a,eb)-(i64)zx(b,eb),eb);
  if(a<b){ qc(c); return 0; } return a-b; }

/* register shifts: SSHL USHL SRSHL URSHL SQSHL UQSHL SQRSHL UQRSHL */
static u64 vshl(CPU* c, u64 a, u64 b, int eb, int U, int round, int sat){
  int sh=(i8)(u8)b;
  if(sh>=0){
    if(!U){ i64 x=sx(a,eb);
      if(!sat) return sh>=eb? 0 : zx((u64)x<<sh,eb);
      if(x==0) return 0;
      if(sh<eb){ i64 r=(i64)((u64)x<<sh); if((r>>sh)==x && sx(zx((u64)r,eb),eb)==r) return zx((u64)r,eb); }
      qc(c); return zx(x<0? (u64)1<<(eb-1) : ((u64)1<<(eb-1))-1,eb);
    } else { u64 x=zx(a,eb);
      if(!sat) return sh>=eb? 0 : zx(x<<sh,eb);
      if(x==0) return 0;
      if(sh==0 || (sh<eb && !(x>>(eb-sh)))) return zx(x<<sh,eb);
      qc(c); return zx(~0ull,eb);
    }
  }
  int n=-sh;
  if(!U){ i64 x=sx(a,eb);
    if(round){ if(n>64) return 0; i64 hi= n>=64? (x<0?-1:0) : x>>n; return zx((u64)(hi+((x>>(n-1))&1)),eb); }
    return zx((u64)(n>=64? (x<0?-1:0) : x>>n),eb);
  } else { u64 x=zx(a,eb);
    if(round){ if(n>64) return 0; u64 hi= n>=64? 0 : x>>n; return zx(hi+((x>>(n-1))&1),eb); }
    return n>=64? 0 : x>>n;
  }
}

static int fprm(CPU* c){ return (c->fpcr>>22)&3; }

/* ---- per-lane fp; doing +,-,*,/,sqrt in double then rounding to single is exact ---- */
static f64 rdf(const V128* v, int sz, int i){ return sz? v->df[i] : (f64)v->f[i]; }

static void wrf(V128* v, int sz, int i, f64 x){ if(sz) v->df[i]=x; else v->f[i]=(f32)x; }

static f64 fmax_(f64 a,f64 b,int sz){ return sz? fp_max64(a,b) : fp_max32((f32)a,(f32)b); }

static f64 fmin_(f64 a,f64 b,int sz){ return sz? fp_min64(a,b) : fp_min32((f32)a,(f32)b); }

static f64 fmaxnm_(f64 a,f64 b,int sz){ return sz? fp_maxnm64(a,b) : fp_maxnm32((f32)a,(f32)b); }

static f64 fminnm_(f64 a,f64 b,int sz){ return sz? fp_minnm64(a,b) : fp_minnm32((f32)a,(f32)b); }

static f64 fmla_(f64 d, f64 a, f64 b, int sz){ return sz? fp_fma64(a,b,d) : (f64)fp_fma32((f32)a,(f32)b,(f32)d); }

static f64 fop2(int sel, f64 a, f64 b, int sz){ /* sel = our own op code */
  switch(sel){
    case 0: return a+b; case 1: return a-b; case 2: return a*b; case 3: return a/b;
    case 4: return fmax_(a,b,sz); case 5: return fmin_(a,b,sz); case 6: return fmaxnm_(a,b,sz); case 7: return fminnm_(a,b,sz);
    case 8: { f64 d=a-b; u64 u; memcpy(&u,&d,8); u&=~(1ull<<63); memcpy(&d,&u,8); return d; }
    case 9: return sz? fp_mulx64(a,b) : fp_mulx32((f32)a,(f32)b);
    case 10: return sz? fp_recps64(a,b) : fp_recps32((f32)a,(f32)b);
    case 11: return sz? fp_rsqrts64(a,b) : fp_rsqrts32((f32)a,(f32)b);
  } return 0; }

static u64 fcmpmask(int sel, f64 a, f64 b, int sz){
  int r=0; switch(sel){ case 0: r=a==b; break; case 1: r=a>=b; break; case 2: r=a>b; break;
    case 3: r=(a<0?-a:a)>=(b<0?-b:b); break; case 4: r=(a<0?-a:a)>(b<0?-b:b); break; case 5: r=a<=b; break; case 6: r=a<b; break; }
  return r? (sz? ~0ull : 0xFFFFFFFFull) : 0; }

/* three-same, vector + scalar */
static int simd_3same(CPU* c, u32 w, int scalar){
  int Q=(w>>30)&1, U=(w>>29)&1, size=(w>>22)&3, rm=(w>>16)&31, op=(w>>11)&31, rn=(w>>5)&31, rd=w&31;
  V128 a=*VR(rn), b=*VR(rm), d=*VR(rd), r; memset(&r,0,16);
  if(scalar) Q=0;
  if(op>=0x18){
    int sz=size&1, hi=size>>1; int n= scalar? 1 : (sz? 2 : (Q?4:2)); if(sz && !Q && !scalar) return 0;
    int pair=0, sel=-1, cmp=-1, acc=0;
    if(!U){ if(!hi){ switch(op){ case 0x18: sel=6; break; case 0x19: acc=1; break; case 0x1A: sel=0; break; case 0x1B: sel=9; break; case 0x1C: cmp=0; break; case 0x1E: sel=4; break; case 0x1F: sel=10; break; default: return 0; } }
            else { switch(op){ case 0x18: sel=7; break; case 0x19: acc=2; break; case 0x1A: sel=1; break; case 0x1E: sel=5; break; case 0x1F: sel=11; break; default: return 0; } } }
    else { if(!hi){ switch(op){ case 0x18: sel=6; pair=1; break; case 0x1A: sel=0; pair=1; break; case 0x1B: sel=2; break; case 0x1C: cmp=1; break; case 0x1D: cmp=3; break; case 0x1E: sel=4; pair=1; break; case 0x1F: sel=3; break; default: return 0; } }
            else { switch(op){ case 0x18: sel=7; pair=1; break; case 0x1A: sel=8; break; case 0x1C: cmp=2; break; case 0x1D: cmp=4; break; case 0x1E: sel=5; pair=1; break; default: return 0; } } }
    if(scalar && (pair||acc||sel==0||sel==1||sel==2||sel==3||(sel>=4&&sel<=7))) return 0;
    for(int i=0;i<n;i++){
      f64 x,y;
      if(pair){ int h=n/2; const V128* s= i<h? &a : &b; int k=(i<h? i : i-h)*2; x=rdf(s,sz,k); y=rdf(s,sz,k+1); }
      else { x=rdf(&a,sz,i); y=rdf(&b,sz,i); }
      if(cmp>=0){ eset(&r,sz?3:2,i,fcmpmask(cmp,x,y,sz)); continue; }
      if(acc==1){ wrf(&r,sz,i,fmla_(rdf(&d,sz,i),x,y,sz)); continue; }
      if(acc==2){ wrf(&r,sz,i,fmla_(rdf(&d,sz,i),-x,y,sz)); continue; }
      wrf(&r,sz,i,fop2(sel,x,y,sz));
    }
    *VR(rd)=r; return 1;
  }
  int eb=8<<size; int n= scalar? 1 : (Q?128:64)/eb;
  if(op==3){ /* 128 bit logic */
    if(scalar) return 0;
    for(int i=0;i<2;i++){ u64 x=a.d[i], y=b.d[i], z=d.d[i], o;
      if(!U){ switch(size){ case 0: o=x&y; break; case 1: o=x&~y; break; case 2: o=x|y; break; default: o=x|~y; } }
      else { switch(size){ case 0: o=x^y; break; case 1: o=(z&x)|(~z&y); break; case 2: o=(z&~y)|(x&y); break; default: o=(z&y)|(x&~y); } }
      r.d[i]=o; }
    if(!Q) r.d[1]=0; *VR(rd)=r; return 1;
  }
  if(size==3 && !Q && !scalar) return 0;
  for(int i=0;i<n;i++){
    u64 x,y,z=eget(&d,size,i), o=0;
    int pairop = (op>=0x14 && op<=0x15) || (op==0x17 && !U);
    if(pairop){ int h=n/2; const V128* s= i<h? &a : &b; int k=(i<h? i : i-h)*2; x=eget(s,size,k); y=eget(s,size,k+1); }
    else { x=eget(&a,size,i); y=eget(&b,size,i); }
    i64 sa=sx(x,eb), sb=sx(y,eb); u64 ua=zx(x,eb), ub=zx(y,eb);
    switch(op){
      case 0x00: if(eb==64) return 0; o= U? (ua+ub)>>1 : (u64)((sa+sb)>>1); break;
      case 0x01: o= U? uqadd_(c,x,y,eb) : sqadd_(c,x,y,eb); break;
      case 0x02: if(eb==64) return 0; o= U? (ua+ub+1)>>1 : (u64)((sa+sb+1)>>1); break;
      case 0x04: if(eb==64) return 0; o= U? (u64)((i64)(ua-ub)>>1) : (u64)((sa-sb)>>1); break;
      case 0x05: o= U? uqsub_(c,x,y,eb) : sqsub_(c,x,y,eb); break;
      case 0x06: o= (U? ua>ub : sa>sb)? ~0ull : 0; break;
      case 0x07: o= (U? ua>=ub : sa>=sb)? ~0ull : 0; break;
      case 0x08: o=vshl(c,x,y,eb,U,0,0); break;
      case 0x09: o=vshl(c,x,y,eb,U,0,1); break;
      case 0x0A: o=vshl(c,x,y,eb,U,1,0); break;
      case 0x0B: { /* rounding sat: round then saturate */
        int sh=(i8)(u8)y; if(sh<0) o=vshl(c,x,y,eb,U,1,0); else o=vshl(c,x,y,eb,U,0,1); } break;
      case 0x0C: if(eb==64) return 0; o= U? (ua>ub?ua:ub) : (u64)(sa>sb?sa:sb); break;
      case 0x0D: if(eb==64) return 0; o= U? (ua<ub?ua:ub) : (u64)(sa<sb?sa:sb); break;
      case 0x0E: if(eb==64) return 0; o= U? (ua>ub?ua-ub:ub-ua) : (u64)(sa>sb?sa-sb:sb-sa); break;
      case 0x0F: if(eb==64) return 0; o= z+(U? (ua>ub?ua-ub:ub-ua) : (u64)(sa>sb?sa-sb:sb-sa)); break;
      case 0x10: o= U? x-y : x+y; break;
      case 0x11: o= U? (ua==ub? ~0ull:0) : ((x&y)? ~0ull : 0); break;
      case 0x12: if(eb==64) return 0; o= U? z-x*y : z+x*y; break;
      case 0x13: if(eb==64) return 0;
        if(U){ if(size) return 0; u32 p=0; for(int k=0;k<8;k++) if((ua>>k)&1) p^=(u32)ub<<k; o=p; } else o=x*y; break;
      case 0x14: if(eb==64) return 0; o= U? (ua>ub?ua:ub) : (u64)(sa>sb?sa:sb); break;
      case 0x15: if(eb==64) return 0; o= U? (ua<ub?ua:ub) : (u64)(sa<sb?sa:sb); break;
      case 0x16: { if(size!=1 && size!=2) return 0;
        i64 p=2*sa*sb; if(U) p+=(i64)1<<(eb-1); p>>=eb;
        if(sa==sb && sa==-((i64)1<<(eb-1))){ p=((i64)1<<(eb-1))-1; qc(c); } o=(u64)p; } break;
      case 0x17: if(U) return 0; o=x+y; break;
      default: return 0;
    }
    if(scalar && size!=3 && !(op==0x01||op==0x05||op==0x09||op==0x0B||op==0x16)) return 0;
    eset(&r,size,i,zx(o,eb));
  }
  *VR(rd)=r; return 1;
}

/* three-different (long, wide, narrow) */
static int simd_3diff(CPU* c, u32 w, int scalar){
  int Q=(w>>30)&1, U=(w>>29)&1, size=(w>>22)&3, rm=(w>>16)&31, op=(w>>12)&15, rn=(w>>5)&31, rd=w&31;
  V128 a=*VR(rn), b=*VR(rm), d=*VR(rd), r; memset(&r,0,16);
  if(size==3 && op!=14) return 0;
  int eb=8<<size, wb=eb*2, n=64/eb, off=Q? n : 0;
  if(scalar){ if(!(op==9||op==11||op==13) || U) return 0; n=1; off=0; }
  if(op==14){ /* PMULL */
    if(U) return 0;
    if(size==0){ for(int i=0;i<8;i++){ u32 x=a.b[i+off], y=b.b[i+off], p=0; for(int k=0;k<8;k++) if((x>>k)&1) p^=y<<k; r.h[i]=(u16)p; } }
    else if(size==3){ v_pmull(&r,&a,&b,3,Q); }
    else return 0;
    *VR(rd)=r; return 1;
  }
  if(op==4||op==6){ /* ADDHN/SUBHN/RADDHN/RSUBHN */
    V128 o=Q? d : r; if(!Q) memset(&o,0,16);
    for(int i=0;i<n;i++){ u64 x=eget(&a,size+1,i), y=eget(&b,size+1,i), s= op==4? x+y : x-y; if(U) s+=(u64)1<<(eb-1); eset(&o,size,i+off,zx(s>>eb,eb)); }
    *VR(rd)=o; return 1;
  }
  for(int i=0;i<n;i++){
    u64 xa, xb=eget(&b,size,i+off); i64 sa, sb=sx(xb,eb); u64 ua, ub=zx(xb,eb);
    if(op==1||op==3){ xa=eget(&a,size+1,i); sa=sx(xa,wb); ua=zx(xa,wb); }
    else { xa=eget(&a,size,i+off); sa=sx(xa,eb); ua=zx(xa,eb); }
    u64 z=eget(&d,size+1,i), o;
    switch(op){
      case 0: case 1: o= U? ua+ub : (u64)(sa+sb); break;
      case 2: case 3: o= U? ua-ub : (u64)(sa-sb); break;
      case 5: o= z+(U? (ua>ub?ua-ub:ub-ua) : (u64)(sa>sb?sa-sb:sb-sa)); break;
      case 7: o= U? (ua>ub?ua-ub:ub-ua) : (u64)(sa>sb?sa-sb:sb-sa); break;
      case 8: o= z+(U? ua*ub : (u64)(sa*sb)); break;
      case 10: o= z-(U? ua*ub : (u64)(sa*sb)); break;
      case 12: o= U? ua*ub : (u64)(sa*sb); break;
      case 9: case 11: case 13: { if(U || size==0) return 0;
        i64 p=2*sa*sb; if(sa==sb && sa==-((i64)1<<(eb-1))){ p=sat_s(0x7FFFFFFFFFFFFFFFll,wb); qc(c); }
        if(op==13) o=(u64)p; else if(op==9) o=sqadd_(c,z,(u64)p,wb); else o=sqsub_(c,z,(u64)p,wb); } break;
      default: return 0;
    }
    eset(&r,size+1,i,zx(o,wb));
  }
  *VR(rd)=r; return 1;
}

static u64 sqxtn_(CPU* c, u64 x, int eb, int kind){ /* 0 SQXTN 1 UQXTN 2 SQXTUN */
  i64 s=sx(x,eb*2); u64 u=zx(x,eb*2);
  if(kind==0) return zx((u64)ssat(c,s,eb),eb);
  if(kind==1){ u64 mx=zx(~0ull,eb); if(u>mx){ qc(c); return mx; } return u; }
  return usat(c,s,eb); }

/* two-reg misc, vector + scalar */
static int simd_2misc(CPU* c, u32 w, int scalar){
  int Q=(w>>30)&1, U=(w>>29)&1, size=(w>>22)&3, op=(w>>12)&31, rn=(w>>5)&31, rd=w&31;
  V128 a=*VR(rn), d=*VR(rd), r; memset(&r,0,16);
  if(scalar) Q=0;
  int eb=8<<size, n= scalar? 1 : (Q?128:64)/eb;
  int sz=size&1, fn= scalar? 1 : (sz? 2 : (Q?4:2));
  int hi=size>>1;
  /* ---- fp ---- */
  if(op>=0x0C && op<=0x0F && hi){ if(sz&&!Q&&!scalar) return 0;
    for(int i=0;i<fn;i++){ f64 x=rdf(&a,sz,i);
      if(op==0x0F){ if(scalar) return 0; if(U){ u64 v=eget(&a,sz?3:2,i)^(sz? 0x8000000000000000ull:0x80000000ull); eset(&r,sz?3:2,i,v);} else { u64 v=eget(&a,sz?3:2,i)&(sz? 0x7FFFFFFFFFFFFFFFull:0x7FFFFFFFull); eset(&r,sz?3:2,i,v);} continue; }
      int k= op==0x0C? (U?1:2) : op==0x0D? (U?5:0) : op==0x0E? (U? -1 : 6) : -1; if(k<0) return 0;
      eset(&r,sz?3:2,i,fcmpmask(k,x,0.0,sz)); }
    *VR(rd)=r; return 1; }
  if(op>=0x16){
    if(op==0x16 && !hi){ /* FCVTN / FCVTXN */
      if(scalar && !U) return 0;
      V128 o= Q? d : r; int off= Q? (sz?2:4) : 0;
      if(sz){ for(int i=0;i<2;i++){ f64 x=a.df[i]; f32 y=(f32)x;
          if(U){ /* round to odd */ u32 yb; memcpy(&yb,&y,4); if((f64)y!=x && !isnan64(x) && !(yb&1)){ f64 dd=(f64)y; if((dd>x)==!(yb>>31)) yb--; else yb++; yb|=1; memcpy(&y,&yb,4);} }
          if(scalar){ o.d[0]=0; o.d[1]=0; o.f[0]=y; break; } o.f[i+off]=y; } }
      else { if(U) return 0; for(int i=0;i<4;i++) o.h[i+off]=f32_to_f16(a.f[i]); }
      if(!Q && !scalar) o.d[1]=0; *VR(rd)=o; return 1; }
    if(op==0x17 && !hi && !U){ /* FCVTL */
      int off= Q? (sz?2:4) : 0;
      if(sz){ for(int i=0;i<2;i++) r.df[i]=(f64)a.f[i+off]; } else { for(int i=0;i<4;i++) r.f[i]=f16_to_f32(a.h[i+off]); }
      *VR(rd)=r; return 1; }
    if(sz&&!Q&&!scalar) return 0;
    for(int i=0;i<fn;i++){ f64 x=rdf(&a,sz,i); u64 bits=eget(&a,sz?3:2,i); int eb2= sz?64:32;
      int k=(U<<6)|(hi<<5)|op;
      switch(k){
        case 0x18: wrf(&r,sz,i,fp_round(x,0)); break;            /* FRINTN */
        case 0x38: wrf(&r,sz,i,fp_round(x,1)); break;            /* FRINTP */
        case 0x19: wrf(&r,sz,i,fp_round(x,2)); break;            /* FRINTM */
        case 0x39: wrf(&r,sz,i,fp_round(x,3)); break;            /* FRINTZ */
        case 0x58: wrf(&r,sz,i,fp_round(x,4)); break;            /* FRINTA */
        case 0x59: wrf(&r,sz,i,fp_round(x,fprm(c))); break;      /* FRINTX */
        case 0x79: wrf(&r,sz,i,fp_round(x,fprm(c))); break;      /* FRINTI */
        case 0x1A: case 0x3A: case 0x1B: case 0x3B: case 0x1C:
        case 0x5A: case 0x7A: case 0x5B: case 0x7B: case 0x5C: {
          int rm= (k&0x1F)==0x1C? 4 : (k&0x1F)==0x1A? (hi?1:0) : (hi?3:2); int uns=U;
          u64 o= sz? (uns? fcvt_u64(x,rm) : (u64)fcvt_s64(x,rm)) : (uns? fcvt_u32(x,rm) : (u64)(u32)fcvt_s32(x,rm));
          eset(&r,sz?3:2,i,o); } break;
        case 0x1D: wrf(&r,sz,i, sz? (f64)(i64)bits : (f64)(f32)(i32)(u32)bits); break;   /* SCVTF */
        case 0x5D: wrf(&r,sz,i, sz? (f64)bits : (f64)(f32)(u32)bits); break;           /* UCVTF */
        case 0x3D: wrf(&r,sz,i, sz? fp_recpe64(x) : (f64)fp_recpe32((f32)x)); break;
        case 0x7D: wrf(&r,sz,i, sz? fp_rsqrte64(x) : (f64)fp_rsqrte32((f32)x)); break;
        case 0x3C: if(sz) return 0; r.s[i]=urecpe32((u32)bits); break;
        case 0x7C: if(sz) return 0; r.s[i]=ursqrte32((u32)bits); break;
        case 0x7F: wrf(&r,sz,i, sz? fp_sqrt64(x) : (f64)fp_sqrt32((f32)x)); break;
        case 0x3F: { if(!scalar) return 0; /* FRECPX */
          if(sz){ u64 b=bits, e=(b>>52)&0x7FF; if(e==0x7FF && (b&0xFFFFFFFFFFFFFull)) r.d[0]=b|0x8000000000000ull; else r.d[0]=(b&0x8000000000000000ull)|((e? (~e&0x7FF) : 0x7FE)<<52); }
          else { u32 b=(u32)bits, e=(b>>23)&0xFF; if(e==0xFF && (b&0x7FFFFF)) r.s[0]=b|0x400000; else r.s[0]=(b&0x80000000u)|((e? (~e&0xFF) : 0xFE)<<23); } } break;
        default: (void)eb2; return 0;
      }
    }
    *VR(rd)=r; return 1;
  }
  /* ---- int ---- */
  if(op==0x12 || op==0x14 || (op==0x13 && U)){ /* XTN SQXTN UQXTN SQXTUN SHLL */
    if(size==3) return 0;
    if(op==0x13){ if(scalar) return 0; int off=Q?(64/eb):0; for(int i=0;i<64/eb;i++) eset(&r,size+1,i,eget(&a,size,i+off)<<eb); *VR(rd)=r; return 1; }
    V128 o= Q? d : r; int m=64/eb, off= Q? m : 0; if(scalar){ m=1; off=0; memset(&o,0,16); }
    for(int i=0;i<m;i++){ u64 x=eget(&a,size+1,i), v;
      if(op==0x12){ if(scalar && !U) return 0; v= U? sqxtn_(c,x,eb,2) : zx(x,eb); } else v=sqxtn_(c,x,eb,U?1:0);
      eset(&o,size,i+off,v); }
    if(!Q) o.d[1]=0; *VR(rd)=o; return 1; }
  if(op==0x02 || op==0x06){ /* SADDLP UADDLP SADALP UADALP */
    if(scalar || size==3) return 0; int m=n/2;
    for(int i=0;i<m;i++){ u64 x=eget(&a,size,2*i), y=eget(&a,size,2*i+1);
      u64 s= U? zx(x,eb)+zx(y,eb) : (u64)(sx(x,eb)+sx(y,eb)); if(op==6) s+=eget(&d,size+1,i); eset(&r,size+1,i,zx(s,eb*2)); }
    *VR(rd)=r; return 1; }
  if(size==3 && !Q && !scalar) return 0;
  for(int i=0;i<n;i++){ u64 x=eget(&a,size,i), o; i64 s=sx(x,eb); u64 z=eget(&d,size,i);
    int k=(U<<5)|op;
    switch(k){
      case 0x00: case 0x20: case 0x01: { if(scalar) return 0; /* REV64 REV32 REV16 */
        int cont= k==0x00? 64 : k==0x20? 32 : 16; if(eb>=cont) return 0; int per=cont/eb; int base=i-(i%per); o=eget(&a,size,base+per-1-(i%per)); } break;
      case 0x03: o=sqadd_(c,z,x,eb); /* SUQADD (approx) */ { i64 zz=sx(z,eb); u64 xx=zx(x,eb); i64 t; if(eb<64){ t=zz+(i64)xx; o=zx((u64)ssat(c,t,eb),eb);} } break;
      case 0x23: { u64 zz=zx(z,eb); i64 xx=sx(x,eb); if(eb<64){ i64 t=(i64)zz+xx; o=usat(c,t,eb);} else { o=zz+(u64)xx; if(xx>=0? o<zz : o>zz){ qc(c); o= xx>=0? ~0ull : 0; } } } break;
      case 0x04: if(size==3) return 0; o=(u64)(clz64(zx(x,eb)^zx((u64)(s>>1),eb))-(64-eb))-1; break;
      case 0x24: if(size==3) return 0; o=clz64(zx(x,eb))-(64-eb); break;
      case 0x05: if(size) return 0; o=popc8((u8)x); break;
      case 0x25: if(size==0) o=~x; else if(size==1){ o=(rbit32((u8)x)>>24)|((rbit32((u8)(x>>8))>>24)<<8); } else return 0; break;
      case 0x07: if(s==-((i64)1<<(eb-1)) || (eb==64 && x==0x8000000000000000ull)){ qc(c); o=zx(((u64)1<<(eb-1))-1,eb); } else o=(u64)(s<0?-s:s); break;
      case 0x27: if(s==-((i64)1<<(eb-1)) || (eb==64 && x==0x8000000000000000ull)){ qc(c); o=zx(((u64)1<<(eb-1))-1,eb); } else o=(u64)(-s); break;
      case 0x08: o= s>0? ~0ull:0; break;
      case 0x28: o= s>=0? ~0ull:0; break;
      case 0x09: o= s==0? ~0ull:0; break;
      case 0x29: o= s<=0? ~0ull:0; break;
      case 0x0A: o= s<0? ~0ull:0; break;
      case 0x0B: o=(u64)(s<0?-s:s); break;
      case 0x2B: o=(u64)(-s); break;
      default: return 0;
    }
    if(scalar && size!=3 && !(k==0x03||k==0x23||k==0x07||k==0x27)) return 0;
    eset(&r,size,i,zx(o,eb)); }
  *VR(rd)=r; return 1;
}

static int simd_across(CPU* c, u32 w){
  int Q=(w>>30)&1, U=(w>>29)&1, size=(w>>22)&3, op=(w>>12)&31, rn=(w>>5)&31, rd=w&31;
  V128 a=*VR(rn), r; memset(&r,0,16);
  if(op==0x0C || op==0x0F){ /* FMAXNMV FMINNMV FMAXV FMINV, .4s only */
    if(!U || !Q || (size&1)) return 0; int mn=size>>1;
    f32 x=a.f[0]; for(int i=1;i<4;i++){ f32 y=a.f[i]; x= op==0x0C? (mn? fp_minnm32(x,y):fp_maxnm32(x,y)) : (mn? fp_min32(x,y) : fp_max32(x,y)); }
    /* hw reduces pairwise: (0,1),(2,3) */
    { f32 p0,p1; if(op==0x0C){ p0= mn? fp_minnm32(a.f[0],a.f[1]):fp_maxnm32(a.f[0],a.f[1]); p1= mn? fp_minnm32(a.f[2],a.f[3]):fp_maxnm32(a.f[2],a.f[3]); x= mn? fp_minnm32(p0,p1):fp_maxnm32(p0,p1); }
      else { p0= mn? fp_min32(a.f[0],a.f[1]):fp_max32(a.f[0],a.f[1]); p1= mn? fp_min32(a.f[2],a.f[3]):fp_max32(a.f[2],a.f[3]); x= mn? fp_min32(p0,p1):fp_max32(p0,p1); } }
    r.f[0]=x; *VR(rd)=r; return 1; }
  if(size==3 || (size==2 && !Q)) return 0;
  int eb=8<<size, n=(Q?128:64)/eb;
  i64 acc; u64 uacc;
  switch(op){
    case 0x03: { i64 s=0; u64 u=0; for(int i=0;i<n;i++){ u64 x=eget(&a,size,i); s+=sx(x,eb); u+=zx(x,eb); } eset(&r,size+1,0, zx(U? u : (u64)s, eb*2)); } break;
    case 0x0A: case 0x1A: { int mx= op==0x0A; acc=sx(eget(&a,size,0),eb); uacc=zx(eget(&a,size,0),eb);
      for(int i=1;i<n;i++){ u64 x=eget(&a,size,i); i64 s=sx(x,eb); u64 u=zx(x,eb); if(mx){ if(s>acc) acc=s; if(u>uacc) uacc=u; } else { if(s<acc) acc=s; if(u<uacc) uacc=u; } }
      eset(&r,size,0,zx(U? uacc : (u64)acc,eb)); } break;
    case 0x1B: { if(U) return 0; u64 s=0; for(int i=0;i<n;i++) s+=eget(&a,size,i); eset(&r,size,0,zx(s,eb)); } break;
    default: return 0;
  }
  *VR(rd)=r; return 1;
}

static int simd_copy(CPU* c, u32 w){
  int Q=(w>>30)&1, op=(w>>29)&1, imm5=(w>>16)&31, imm4=(w>>11)&15, rn=(w>>5)&31, rd=w&31;
  int size=0; while(size<4 && !((imm5>>size)&1)) size++; if(size>3) return 0;
  int idx=imm5>>(size+1), eb=8<<size;
  V128 r; memset(&r,0,16);
  if(op){ if(!Q) return 0; int i2=imm4>>size; V128 d=*VR(rd); eset(&d,size,idx,eget(VR(rn),size,i2)); *VR(rd)=d; return 1; }
  switch(imm4){
    case 0: { if(size==3 && !Q) return 0; u64 x=eget(VR(rn),size,idx); for(int i=0;i<(Q?128:64)/eb;i++) eset(&r,size,i,x); *VR(rd)=r; return 1; }
    case 1: { if(size==3 && !Q) return 0; u64 x=c->x[rn]; if(rn==31) x=0; for(int i=0;i<(Q?128:64)/eb;i++) eset(&r,size,i,x); *VR(rd)=r; return 1; }
    case 3: { if(!Q) return 0; u64 x= rn==31? 0 : c->x[rn]; eset(VR(rd),size,idx,x); return 1; }
    case 5: { if(size==3 || (size==2 && !Q)) return 0; i64 v=sx(eget(VR(rn),size,idx),eb); if(rd!=31) c->x[rd]= Q? (u64)v : (u32)v; return 1; }
    case 7: { if((size==3) != Q) return 0; u64 v=eget(VR(rn),size,idx); if(rd!=31) c->x[rd]=v; return 1; }
  }
  return 0;
}

u64 v_expand_imm(u32 op, u32 cmode, u32 imm8){
  u64 i=imm8, r=0;
  switch(cmode>>1){
    case 0: r=i|(i<<32); break; case 1: r=(i<<8)|(i<<40); break; case 2: r=(i<<16)|(i<<48); break; case 3: r=(i<<24)|(i<<56); break;
    case 4: r=i|(i<<16); r|=r<<32; break; case 5: r=(i<<8)|(i<<24); r|=r<<32; break;
    case 6: if(!(cmode&1)) r=(i<<8)|0xFF; else r=(i<<16)|0xFFFF; r|=r<<32; break;
    case 7:
      if(!(cmode&1) && !op){ for(int k=0;k<8;k++) r|=i<<(k*8); }
      else if(!(cmode&1) && op){ for(int k=0;k<8;k++) if((i>>k)&1) r|=0xFFull<<(k*8); }
      else if((cmode&1) && !op){ u32 b=((i&0x80)<<24)|(((i&0x40)? 0x3E000000u:0x40000000u))|((u32)(i&0x3F)<<19); r=b|((u64)b<<32); }
      else { r=((i&0x80)<<56)|((i&0x40)? 0x3FC0000000000000ull : 0x4000000000000000ull)|((i&0x3F)<<48); }
      break;
  }
  return r;
}

static int simd_modimm(CPU* c, u32 w){
  int Q=(w>>30)&1, op=(w>>29)&1, cmode=(w>>12)&15, rd=w&31, o2=(w>>11)&1;
  u32 imm8=(((w>>16)&7)<<5)|((w>>5)&31);
  if(o2) return 0;
  if(cmode==15 && op && !Q) return 0;
  u64 imm=v_expand_imm(op,cmode,imm8); V128* d=VR(rd);
  int logic = (cmode<12 && (cmode&1)) ; /* ORR / BIC */
  if(logic){ if(!op){ d->d[0]|=imm; d->d[1]= Q? d->d[1]|imm : 0; } else { d->d[0]&=~imm; d->d[1]= Q? d->d[1]&~imm : 0; } return 1; }
  if(op && cmode!=14 && cmode!=15) imm=~imm;  /* MVNI */
  d->d[0]=imm; d->d[1]= Q? imm : 0; return 1;
}

/* imm shifts */
static int simd_shimm(CPU* c, u32 w, int scalar){
  int Q=(w>>30)&1, U=(w>>29)&1, immh=(w>>19)&15, immb=(w>>16)&7, op=(w>>11)&31, rn=(w>>5)&31, rd=w&31;
  if(!immh) return 0; if(scalar) Q=0;
  int size=3; while(size>=0 && !((immh>>size)&1)) size--; int eb=8<<size; int imm=(immh<<3)|immb;
  int shr=2*eb-imm, shl=imm-eb;
  V128 a=*VR(rn), d=*VR(rd), r; memset(&r,0,16);
  int n= scalar? 1 : (Q?128:64)/eb;
  if(op==0x14){ /* SSHLL USHLL */ if(scalar||size==3) return 0; int m=64/eb, off=Q?m:0;
    for(int i=0;i<m;i++){ u64 x=eget(&a,size,i+off); u64 v= U? zx(x,eb)<<shl : (u64)(sx(x,eb)<<shl); eset(&r,size+1,i,zx(v,eb*2)); } *VR(rd)=r; return 1; }
  if(op>=0x10 && op<=0x13){ /* narrowing: SHRN RSHRN SQSHRN SQRSHRN UQSHRN SQSHRUN ... */
    if(size==3) return 0; int wb=eb*2; shr=2*wb-imm-wb; shr= wb*2 - ((immh<<3)|immb) - wb; /* sh = 2*eb - imm, eb = dst size */
    shr = 2*eb - imm; /* eb = dst size */
    int m= scalar? 1 : 64/eb, off= Q? m : 0; V128 o= Q? d : r; if(scalar){ memset(&o,0,16); off=0; }
    for(int i=0;i<m;i++){ u64 x=eget(&a,size+1,i); int rnd= op&1; u64 v;
      if(op==0x10 && !U){ if(scalar) return 0; v=zx(x>>shr,eb); }
      else if(op==0x11 && !U){ if(scalar) return 0; v=zx((x+((u64)1<<(shr-1)))>>shr,eb); }
      else { int kind; i64 sval; u64 uval;
        if(U && op<0x12){ kind=2; } else kind= U? 1 : 0;
        if(kind==1){ uval=zx(x,wb); uval= rnd? (uval>>shr)+((uval>>(shr-1))&1) : uval>>shr; if(uval>zx(~0ull,eb)){ qc(c); uval=zx(~0ull,eb);} v=uval; }
        else { sval=sx(x,wb); sval= rnd? (sval>>shr)+((sval>>(shr-1))&1) : sval>>shr; v= kind==0? zx((u64)ssat(c,sval,eb),eb) : usat(c,sval,eb); }
      }
      eset(&o,size,i+off,v); }
    if(!Q) o.d[1]=0; *VR(rd)=o; return 1; }
  if(op==0x1C || op==0x1F){ /* SCVTF/UCVTF/FCVTZS/FCVTZU fixed point */
    if(size<2) return 0; if(size==3 && !Q && !scalar) return 0; int fb=shr; f64 sc=1.0; for(int k=0;k<fb;k++) sc*=2.0;
    for(int i=0;i<n;i++){ u64 x=eget(&a,size,i);
      if(op==0x1C){ f64 v= size==3? (U? (f64)x : (f64)(i64)x) : (U? (f64)(u32)x : (f64)(i32)(u32)x); v/=sc; wrf(&r,size==3,i,v); }
      else { f64 v=rdf(&a,size==3,i)*sc; u64 o= size==3? (U? fcvt_u64(v,3) : (u64)fcvt_s64(v,3)) : (U? fcvt_u32(v,3) : (u64)(u32)fcvt_s32(v,3)); eset(&r,size,i,o); } }
    *VR(rd)=r; return 1; }
  if(size==3 && !Q && !scalar) return 0;
  if(scalar && size!=3 && !(op==0x0E||op==0x0C)) return 0;
  for(int i=0;i<n;i++){ u64 x=eget(&a,size,i), z=eget(&d,size,i), o; i64 s=sx(x,eb); u64 u=zx(x,eb);
    switch(op){
      case 0x00: case 0x02: case 0x04: case 0x06: { int rnd=(op>>2)&1; u64 v;
        if(U){ if(shr>=64) v= rnd? (u>>63) : 0; else { v=u>>shr; if(rnd) v+=(u>>(shr-1))&1; } }
        else { i64 t; if(shr>=64) t= rnd? 0 : (s<0?-1:0); else { t=s>>shr; if(rnd) t+=(s>>(shr-1))&1; } if(shr>=64 && rnd) t=0; v=(u64)t; }
        o= op>=0x02 && (op==0x02||op==0x06)? z+v : v; } break;
      case 0x08: if(!U) return 0; { u64 m= shr>=64? 0 : zx(~0ull,eb)>>shr; o=(z&~m)|(shr>=64?0:(u>>shr)); } break;
      case 0x0A: if(!U) o=u<<shl; else { u64 m=zx(~0ull,eb)<<shl; o=(z&~m)|((u<<shl)&m); } break;
      case 0x0C: if(!U) return 0; /* SQSHLU */ if(s<0){ if(s) qc(c); o=0; } else { u64 v=u<<shl; if(shl && (u>>(eb-shl))){ qc(c); v=~0ull; } o=v; } break;
      case 0x0E: if(U){ u64 v=u<<shl; if(shl && (u>>(eb-shl))){ qc(c); v=~0ull; } o=v; }
                 else { i64 v=(i64)((u64)s<<shl); if(sx(zx((u64)v,eb),eb)!=v || (v>>shl)!=s){ qc(c); v= s<0? -((i64)1<<(eb-1)) : ((i64)1<<(eb-1))-1; } o=(u64)v; } break;
      default: return 0;
    }
    eset(&r,size,i,zx(o,eb)); }
  *VR(rd)=r; return 1;
}

/* by-element */
static int simd_elem(CPU* c, u32 w, int scalar){
  int Q=(w>>30)&1, U=(w>>29)&1, size=(w>>22)&3, L=(w>>21)&1, Mb=(w>>20)&1, rm=(w>>16)&15, op=(w>>12)&15, H=(w>>11)&1, rn=(w>>5)&31, rd=w&31;
  if(scalar) Q=0;
  V128 a=*VR(rn), d=*VR(rd), r; memset(&r,0,16);
  int fp = !U? (op==1||op==5||op==9) : (op==9);
  if(fp){ int sz=size&1; if(size<2) return 0; int idx= sz? H : (H<<1)|L; if(sz && L) return 0; int m=(Mb<<4)|rm;
    if(sz && !Q && !scalar) return 0;
    f64 b=rdf(VR(m),sz,idx); int n= scalar? 1 : (sz?2:(Q?4:2));
    for(int i=0;i<n;i++){ f64 x=rdf(&a,sz,i), v;
      if(op==1) v=fmla_(rdf(&d,sz,i),x,b,sz); else if(op==5) v=fmla_(rdf(&d,sz,i),-x,b,sz); else if(!U) v=x*b; else v= sz? fp_mulx64(x,b) : fp_mulx32((f32)x,(f32)b);
      wrf(&r,sz,i,v); }
    *VR(rd)=r; return 1; }
  if(size!=1 && size!=2) return 0;
  int idx, m; if(size==1){ idx=(H<<2)|(L<<1)|Mb; m=rm; } else { idx=(H<<1)|L; m=(Mb<<4)|rm; }
  int eb=8<<size; u64 bx=eget(VR(m),size,idx); i64 sb=sx(bx,eb); u64 ub=zx(bx,eb);
  int lng = (op==2||op==3||op==6||op==7||op==10||op==11);
  if(lng){ int nn= scalar? 1 : 64/eb, off= Q? nn : 0;
    for(int i=0;i<nn;i++){ u64 x=eget(&a,size,i+off); i64 sa=sx(x,eb); u64 ua=zx(x,eb); u64 z=eget(&d,size+1,i), o;
      switch(op){
        case 2: o= z+(U? ua*ub : (u64)(sa*sb)); break; case 6: o= z-(U? ua*ub : (u64)(sa*sb)); break; case 10: o= U? ua*ub : (u64)(sa*sb); break;
        case 3: case 7: case 11: { if(U) return 0; i64 p=2*sa*sb; if(sa==sb && sa==-((i64)1<<(eb-1))){ p=sat_s(0x7FFFFFFFFFFFFFFFll,eb*2); qc(c); }
          o= op==11? (u64)p : op==3? sqadd_(c,z,(u64)p,eb*2) : sqsub_(c,z,(u64)p,eb*2); } break;
        default: return 0; }
      if(scalar && !(op==3||op==7||op==11)) return 0;
      eset(&r,size+1,i,zx(o,eb*2)); }
    *VR(rd)=r; return 1; }
  int n= scalar? 1 : (Q?128:64)/eb;
  for(int i=0;i<n;i++){ u64 x=eget(&a,size,i), z=eget(&d,size,i), o; i64 sa=sx(x,eb);
    switch(op){
      case 0: if(!U) return 0; o=z+x*bx; break;
      case 4: if(!U) return 0; o=z-x*bx; break;
      case 8: if(U) return 0; o=x*bx; break;
      case 12: case 13: { if(U) return 0; i64 p=2*sa*sb; if(op==13) p+=(i64)1<<(eb-1); p>>=eb; if(sa==sb && sa==-((i64)1<<(eb-1))){ p=((i64)1<<(eb-1))-1; qc(c);} o=(u64)p; } break;
      default: return 0; }
    if(scalar && op<12) return 0;
    eset(&r,size,i,zx(o,eb)); }
  *VR(rd)=r; return 1;
}

static int simd_perm(CPU* c, u32 w){
  int Q=(w>>30)&1, size=(w>>22)&3, rm=(w>>16)&31, op=(w>>12)&7, rn=(w>>5)&31, rd=w&31;
  if(size==3 && !Q) return 0;
  V128 a=*VR(rn), b=*VR(rm), r; memset(&r,0,16); int eb=8<<size, n=(Q?128:64)/eb, h=n/2;
  int part=(op>>2)&1;
  for(int i=0;i<n;i++){ u64 v;
    switch(op&3){
      case 1: { int k=2*i+part; v= k<n? eget(&a,size,k) : eget(&b,size,k-n); } break;                       /* UZP */
      case 2: { int p=i/2*2+part; v= (i&1)? eget(&b,size,p) : eget(&a,size,p); } break;                    /* TRN */
      case 3: { int base=part*h; v= (i&1)? eget(&b,size,base+i/2) : eget(&a,size,base+i/2); } break;        /* ZIP */
      default: return 0; }
    eset(&r,size,i,v); }
  *VR(rd)=r; return 1;
}

void v_tbl(CPU* c, u32 d, u32 n, u32 len, u32 m, int q, int tbx){
  u8 tab[64]; for(u32 k=0;k<=len;k++) memcpy(tab+16*k,&c->v[(n+k)&31],16);
  V128 idx=c->v[m], r= tbx? c->v[d] : c->v[d]; if(!tbx) memset(&r,0,16);
  u32 lim=16*(len+1);
  for(int i=0;i<(q?16:8);i++){ u32 x=idx.b[i]; if(x<lim) r.b[i]=tab[x]; else if(!tbx) r.b[i]=0; }
  if(!q) r.d[1]=0; c->v[d]=r;
}

/* ---- AES ---- */
static const u8 SBOX[256]={
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
static u8 ISBOX[256]; static int isbox_ok;

static u8 xt(u8 x){ return (u8)((x<<1)^((x&0x80)?0x1b:0)); }

static u8 gm(u8 a, u8 b){ u8 p=0; while(b){ if(b&1) p^=a; a=xt(a); b>>=1; } return p; }

void v_aes(V128* d, const V128* k, int dec){
  if(!isbox_ok){ for(int i=0;i<256;i++) ISBOX[SBOX[i]]=(u8)i; isbox_ok=1; }
  V128 s=*d; for(int i=0;i<16;i++) s.b[i]^=k->b[i];
  V128 t;
  if(!dec){ for(int i=0;i<16;i++){ int col=i/4, row=i%4; t.b[i]=SBOX[s.b[((col+row)%4)*4+row]]; } }
  else { for(int i=0;i<16;i++){ int col=i/4, row=i%4; t.b[((col+row)%4)*4+row]=ISBOX[s.b[i]]; } }
  *d=t; }

void v_aesmc(V128* d, const V128* s, int inv){
  V128 r; for(int col=0;col<4;col++){ const u8* a=&s->b[col*4];
    for(int row=0;row<4;row++){
      if(!inv) r.b[col*4+row]=(u8)(gm(a[row],2)^gm(a[(row+1)%4],3)^a[(row+2)%4]^a[(row+3)%4]);
      else r.b[col*4+row]=(u8)(gm(a[row],14)^gm(a[(row+1)%4],11)^gm(a[(row+2)%4],13)^gm(a[(row+3)%4],9)); } }
  *d=r; }

/* ---- SHA1 / SHA256 ---- */
static u32 rol32(u32 x, int s){ return (x<<s)|(x>>(32-s)); }

static u32 ror32_(u32 x, int s){ return (x>>s)|(x<<(32-s)); }

void v_sha1(CPU* c, u32 op, u32 d, u32 n, u32 m){
  V128 X=c->v[d], Y=c->v[n], W=c->v[m];
  if(op<3){ u32 y=Y.s[0];
    for(int i=0;i<4;i++){ u32 t; u32 b=X.s[1], cc=X.s[2], dd=X.s[3];
      if(op==0) t=(b&cc)|(~b&dd); else if(op==1) t=b^cc^dd; else t=(b&cc)|(b&dd)|(cc&dd);
      y=y+rol32(X.s[0],5)+t+W.s[i]; X.s[1]=rol32(X.s[1],30);
      u32 ny=X.s[3]; X.s[3]=X.s[2]; X.s[2]=X.s[1]; X.s[1]=X.s[0]; X.s[0]=y; y=ny; }
    c->v[d]=X; return; }
  if(op==3){ V128 r; r.d[0]=X.d[1]; r.d[1]=Y.d[0]; for(int i=0;i<4;i++) r.s[i]^=X.s[i]^W.s[i]; c->v[d]=r; return; }
  if(op==0x10){ V128 r; memset(&r,0,16); r.s[0]=rol32(Y.s[0],30); c->v[d]=r; return; }
  if(op==0x11){ V128 T; for(int i=0;i<4;i++) T.s[i]=X.s[i]^(i<3? Y.s[i+1] : 0);
    V128 r; r.s[0]=rol32(T.s[0],1); r.s[1]=rol32(T.s[1],1); r.s[2]=rol32(T.s[2],1); r.s[3]=rol32(T.s[3],1)^rol32(T.s[0],2); c->v[d]=r; return; }
}

void v_sha256(CPU* c, u32 op, u32 d, u32 n, u32 m){
  V128 X=c->v[d], Y=c->v[n], W=c->v[m];
  if(op==4||op==5){
    V128 x= op==4? X : Y, y= op==4? Y : X;
    for(int i=0;i<4;i++){
      u32 ch=(y.s[0]&y.s[1])^(~y.s[0]&y.s[2]);
      u32 maj=(x.s[0]&x.s[1])^(x.s[0]&x.s[2])^(x.s[1]&x.s[2]);
      u32 t=y.s[3]+(ror32_(y.s[0],6)^ror32_(y.s[0],11)^ror32_(y.s[0],25))+ch+W.s[i];
      x.s[3]+=t; y.s[3]=t+(ror32_(x.s[0],2)^ror32_(x.s[0],13)^ror32_(x.s[0],22))+maj;
      /* rotate y:x */
      u32 all[8]={x.s[0],x.s[1],x.s[2],x.s[3],y.s[0],y.s[1],y.s[2],y.s[3]};
      u32 rr[8]; for(int k=0;k<8;k++) rr[k]=all[(k+7)%8];
      for(int k=0;k<4;k++){ x.s[k]=rr[k]; y.s[k]=rr[k+4]; }
    }
    c->v[d]= op==4? x : y; return; }
  if(op==6){ V128 r; u32 T0[4]={X.s[1],X.s[2],X.s[3],Y.s[0]};
    (void)T0;
    { u32 T1[4]={W.s[2],W.s[3],0,0}; u32 T2[4]={Y.s[1],Y.s[2],Y.s[3],W.s[0]};
      for(int i=0;i<2;i++){ u32 e=T1[i]; r.s[i]=X.s[i]+(ror32_(e,17)^ror32_(e,19)^(e>>10))+T2[i]; }
      for(int i=2;i<4;i++){ u32 e=r.s[i-2]; r.s[i]=X.s[i]+(ror32_(e,17)^ror32_(e,19)^(e>>10))+T2[i]; } }
    c->v[d]=r; return; }
  if(op==0x12){ V128 r; u32 T[4]={X.s[1],X.s[2],X.s[3],Y.s[0]};
    for(int i=0;i<4;i++){ u32 e=T[i]; r.s[i]=X.s[i]+(ror32_(e,7)^ror32_(e,18)^(e>>3)); } c->v[d]=r; return; }
}

void v_pmull(V128* d, const V128* a, const V128* b, int size, int hi){
  (void)size; u64 x=a->d[hi], y=b->d[hi], lo=0, h=0;
  for(int k=0;k<64;k++) if((x>>k)&1){ lo^=y<<k; if(k) h^=y>>(64-k); }
  d->d[0]=lo; d->d[1]=h; }

/* ---- LD1-4 / ST1-4 / LDnR / single lane ---- */
static int simd_ldst(CPU* c, u8* M, u32 w){
  int Q=(w>>30)&1, L=(w>>22)&1, post=(w>>23)&1, rm=(w>>16)&31, rn=(w>>5)&31, rt=w&31;
  u64 addr=c->x[rn]; u64 total=0;
  if(!((w>>24)&1)){ /* multiple structures */
    int op=(w>>12)&15, size=(w>>10)&3, rpt, sel;
    switch(op){ case 0: rpt=1; sel=4; break; case 2: rpt=4; sel=1; break; case 4: rpt=1; sel=3; break; case 6: rpt=3; sel=1; break;
      case 7: rpt=1; sel=1; break; case 8: rpt=1; sel=2; break; case 10: rpt=2; sel=1; break; default: return 0; }
    if(size==3 && !Q && sel>1) return 0;
    int eb=8<<size, eby=eb/8, n=(Q?128:64)/eb; u64 a=addr;
    for(int r=0;r<rpt;r++) for(int e=0;e<n;e++) for(int s=0;s<sel;s++){
      int t=(rt+r+s)&31;
      if(L){ u64 v; switch(size){ case 0: v=RD8(a); break; case 1: v=RD16(a); break; case 2: v=RD32(a); break; default: v=RD64(a); } eset(VR(t),size,e,v); }
      else { u64 v=eget(VR(t),size,e); switch(size){ case 0: WR8(a,v); break; case 1: WR16(a,v); break; case 2: WR32(a,v); break; default: WR64(a,v); } }
      a+=eby; }
    if(L && !Q) for(int r=0;r<rpt;r++) for(int s=0;s<sel;s++) c->v[(rt+r+s)&31].d[1]=0;
    total=a-addr;
  } else { /* single element / replicate */
    int R=(w>>21)&1, op=(w>>13)&7, S=(w>>12)&1, size=(w>>10)&3; int sel=(((op&1)<<1)|R)+1; int sc=op>>1, idx;
    if(sc==3){ /* LDnR */ if(!L || S) return 0; int eb=8<<size, n=(Q?128:64)/eb; u64 a=addr;
      for(int s=0;s<sel;s++){ u64 v; switch(size){ case 0: v=RD8(a); break; case 1: v=RD16(a); break; case 2: v=RD32(a); break; default: v=RD64(a); }
        V128 r; memset(&r,0,16); for(int e=0;e<n;e++) eset(&r,size,e,v); c->v[(rt+s)&31]=r; a+=eb/8; }
      total=a-addr;
    } else {
      int esz;
      if(sc==0){ esz=0; idx=(Q<<3)|(S<<2)|size; }
      else if(sc==1){ if(size&1) return 0; esz=1; idx=(Q<<2)|(S<<1)|(size>>1); }
      else { if(size&2) return 0; if(size&1){ if(S) return 0; esz=3; idx=Q; } else { esz=2; idx=(Q<<1)|S; } }
      u64 a=addr; int by=1<<esz;
      for(int s=0;s<sel;s++){ int t=(rt+s)&31;
        if(L){ u64 v; switch(esz){ case 0: v=RD8(a); break; case 1: v=RD16(a); break; case 2: v=RD32(a); break; default: v=RD64(a); } eset(VR(t),esz,idx,v); }
        else { u64 v=eget(VR(t),esz,idx); switch(esz){ case 0: WR8(a,v); break; case 1: WR16(a,v); break; case 2: WR32(a,v); break; default: WR64(a,v); } }
        a+=by; }
      total=a-addr;
    }
  }
  if(post){ if(rm==31) c->x[rn]=addr+total; else c->x[rn]=addr+c->x[rm]; }
  return 1;
}

/* ---- dispatcher for anything AdvSIMD / crypto ---- */
int swr_simd(CPU* c, u8* M, u32 w){
  if((w&0xBF800000u)==0x0C000000u || (w&0xBF800000u)==0x0C800000u || (w&0xBF000000u)==0x0D000000u) return simd_ldst(c,M,w);
  if((w&0x9F200400u)==0x0E200400u) return simd_3same(c,w,0);
  if((w&0xDF200400u)==0x5E200400u) return simd_3same(c,w,1);
  if((w&0x9F200C00u)==0x0E200000u) return simd_3diff(c,w,0);
  if((w&0xDF200C00u)==0x5E200000u) return simd_3diff(c,w,1);
  if((w&0x9F3E0C00u)==0x0E200800u) return simd_2misc(c,w,0);
  if((w&0xDF3E0C00u)==0x5E200800u) return simd_2misc(c,w,1);
  if((w&0x9F3E0C00u)==0x0E300800u) return simd_across(c,w);
  if((w&0xDF3E0C00u)==0x5E300800u){ /* scalar pairwise */
    int U=(w>>29)&1, size=(w>>22)&3, op=(w>>12)&31, rn=(w>>5)&31, rd=w&31; V128 a=*VR(rn), r; memset(&r,0,16);
    if(!U && op==0x1B){ if(size!=3) return 0; r.d[0]=a.d[0]+a.d[1]; *VR(rd)=r; return 1; }
    if(!U) return 0; int sz=size&1, hi=size>>1; f64 x=rdf(&a,sz,0), y=rdf(&a,sz,1), v;
    switch(op){ case 0x0D: if(hi) return 0; v=x+y; break; case 0x0C: v= hi? fminnm_(x,y,sz) : fmaxnm_(x,y,sz); break; case 0x0F: v= hi? fmin_(x,y,sz) : fmax_(x,y,sz); break; default: return 0; }
    wrf(&r,sz,0,v); *VR(rd)=r; return 1; }
  if((w&0x9FE08400u)==0x0E000400u) return simd_copy(c,w);
  if((w&0xFFE0FC00u)==0x5E000400u){ /* scalar DUP */
    int imm5=(w>>16)&31, rn=(w>>5)&31, rd=w&31, size=0; while(size<4 && !((imm5>>size)&1)) size++; if(size>3) return 0;
    V128 r; memset(&r,0,16); eset(&r,size,0,eget(VR(rn),size,imm5>>(size+1))); *VR(rd)=r; return 1; }
  if((w&0x9FF80400u)==0x0F000400u) return simd_modimm(c,w);
  if((w&0x9F800400u)==0x0F000400u) return simd_shimm(c,w,0);
  if((w&0xDF800400u)==0x5F000400u) return simd_shimm(c,w,1);
  if((w&0x9F000400u)==0x0F000000u) return simd_elem(c,w,0);
  if((w&0xDF000400u)==0x5F000000u) return simd_elem(c,w,1);
  if((w&0xBF208C00u)==0x0E000800u) return simd_perm(c,w);
  if((w&0xBFE08400u)==0x2E000000u){ int Q=(w>>30)&1, rm=(w>>16)&31, imm4=(w>>11)&15, rn=(w>>5)&31, rd=w&31;
    if(!Q && (imm4&8)) return 0; u8 t[32]; memcpy(t,VR(rn),16); memcpy(t+(Q?16:8),VR(rm),16); V128 r; memset(&r,0,16); memcpy(&r,t+imm4,Q?16:8); *VR(rd)=r; return 1; }
  if((w&0xBFE08C00u)==0x0E000000u){ int Q=(w>>30)&1; v_tbl(c,w&31,(w>>5)&31,(w>>13)&3,(w>>16)&31,Q,(w>>12)&1); return 1; }
  if((w&0xFFFF0C00u)==0x4E280800u){ int op=(w>>12)&31, rn=(w>>5)&31, rd=w&31;
    switch(op){ case 4: v_aes(VR(rd),VR(rn),0); return 1; case 5: v_aes(VR(rd),VR(rn),1); return 1;
      case 6: v_aesmc(VR(rd),VR(rn),0); return 1; case 7: v_aesmc(VR(rd),VR(rn),1); return 1; } return 0; }
  if((w&0xFFE08C00u)==0x5E000000u){ int op=(w>>12)&7, rm=(w>>16)&31, rn=(w>>5)&31, rd=w&31;
    if(op<=3){ v_sha1(c,op,rd,rn,rm); return 1; } if(op<=6){ v_sha256(c,op,rd,rn,rm); return 1; } return 0; }
  if((w&0xFFFF0C00u)==0x5E280800u){ int op=(w>>12)&31, rn=(w>>5)&31, rd=w&31;
    if(op==0){ v_sha1(c,0x10,rd,rn,0); return 1; } if(op==1){ v_sha1(c,0x11,rd,rn,0); return 1; } if(op==2){ v_sha256(c,0x12,rd,rn,0); return 1; } return 0; }
  return 0;
}
