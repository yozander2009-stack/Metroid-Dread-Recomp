# Copyright 2026 SwitchRecomp contributors. Apache-2.0; see LICENSE.txt.
"""Regression for #52: sparse texture writes must reach the full cache check.

Extracts the production functions rather than duplicating the fixed algorithm.
Run: python work-profiler/test_texture_hash.py --cc /path/to/gcc
No game files, graphics context or third-party Python packages are needed.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
a = parser.parse_args()
source = (ROOT / 'runtime/gpu_gl.c').read_text()
start = source.index('static u64 fast_hash(')
end = source.index('\nstatic u64 rt_sig', start)
functions = source[start:end]
video_start = source.index('      u64 vg=video_surface_generation(')
video_end = source.index('      /* Diagnostic mode for gameplay:', video_start)
video_check = source[video_start:video_end]
poll_start = source.index('      /* Budget limits routine checks;', video_end)
poll_end = source.index('      return t; } }', poll_start)
poll_check = source[poll_start:poll_end]
sample_start = source.index('static u64 sample_hash(u64 gva, u64 len){')
sample_end = source.index('\nstatic int g_nosample=', sample_start)
sample_function = source[sample_start:sample_end]
head = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8; typedef int32_t i32; typedef int64_t i64;
static u8 bytes[1048595]; static u64 limit; static int fragmented;
static u8* gmem_ptr(u64 a,u64 n){assert(a<=limit && n<=limit-a);return fragmented?NULL:bytes+a;}
static void gmem_read(u64 a,void* out,u64 n){assert(a<=limit && n<=limit-a);memcpy(out,bytes+a,(size_t)n);}
'''
video_head = r'''
typedef struct {u64 size,video_gen,hash,shash;u32 frame_checked,stable,next_check,tfull;}Tex;
static u32 g_frame,g_tex_bud_frame;static i64 g_tex_bud;static u64 st_tex_chk,video_revision;static int uploads;
static u64 video_surface_generation(const u8*p,u64 n){return p&&n?video_revision:0;}
static void tex_upload(Tex*t){(void)t;uploads++;}
static Tex* video_check(Tex*t,u64 gva){
'''
video_tail = '\nreturn NULL; /* continuation into normal texture polling */\n}\n'
poll_head = '\nstatic Tex* poll_check(Tex*t,u64 gva){\n'
poll_tail = '\nreturn t;\n}\n'
body = r'''
/* Negative controls: the pre-fix direct path and its page-prefix fallback. */
static u64 old_direct(u64 n){return fast_hash(bytes,n,1);}
static u64 old_fragmented(u64 n){u64 h=1469598103934665603ull;for(u64 o=0;o<n;o+=4096){u64 v[8]={0};memcpy(v,bytes+o,n-o<64?n-o:64);for(int i=0;i<8;i++){h^=v[i];h*=1099511628211ull;}}return h;}
static u64 check(u64 n){limit=n;fragmented=0;u64 h=mem_hash(0,n);fragmented=1;assert(mem_hash(0,n)==h);return h;}
/* Model a late-drawn texture: earlier draws consume its whole per-frame budget.
   Run the real production polling block, including cheap sample/backoff logic. */
static void starvation(u64 size,u32 stable,u32 base,int split){
 limit=size;fragmented=split;memset(bytes,0,(size_t)size);uploads=0;g_nosample=0;
 Tex t={.size=size,.stable=stable,.frame_checked=base,.tfull=base,.next_check=base+1};
 t.hash=mem_hash(0,size);t.shash=sample_hash(0,size);bytes[72]=1;
 assert(sample_hash(0,size)==t.shash || size<16384);
 for(u32 elapsed=1;elapsed<=120;elapsed++){
  g_frame=base+elapsed;g_tex_bud_frame=g_frame;g_tex_bud=0;
  assert(poll_check(&t,0)==&t);
  if(elapsed<120){assert(uploads==0);assert(t.tfull==base);}
 }
 assert(uploads==1 && t.tfull==g_frame && t.hash==mem_hash(0,size) && t.stable==0);
 /* A new full-check deadline starts after any actual check, not just a
    long-stable one; further polling follows its existing backoff. */
 g_frame++;g_tex_bud_frame=g_frame;g_tex_bud=0;poll_check(&t,0);assert(uploads==1 && t.tfull==base+120);
 g_frame=base+124;g_tex_bud_frame=g_frame;g_tex_bud=0;poll_check(&t,0);assert(t.tfull==g_frame && uploads==1 && t.stable==1);
}
int main(void){
 const u64 sizes[]={0,1,7,8,9,63,64,65,4095,4096,4097,65536,65537,131072,1048595};
 unsigned checks=0;u64 seed=72191;
 for(unsigned s=0;s<sizeof sizes/sizeof *sizes;s++){
  u64 n=sizes[s];for(u64 k=0;k<n;k++)bytes[k]=(u8)(k*29+s);
  u64 h=check(n);checks++;
  unsigned edits=n<=4097?(unsigned)n:128;
  for(unsigned i=0;i<edits;i++){
   seed=seed*6364136223846793005ull+1;u64 at=n<=4097?i:seed%n;
   if(n>4097 && i<8){const u64 edge[]={0,7,63,64,72,127,4096,4096+72};at=edge[i];}
   if(n>4097 && i==8)at=n-1;
   bytes[at]^=0x5a;assert(check(n)!=h);bytes[at]^=0x5a;assert(check(n)==h);checks+=2;
  }
 }
 /* A reveal edit at byte 72 is outside both historical sample patterns.
    The periodic full check must see it even if the cheap sample did not. */
 limit=131072;memset(bytes,0,(size_t)limit);
 u64 hd=old_direct(limit),hf=old_fragmented(limit),h=check(limit);
 bytes[72]=1;assert(old_direct(limit)==hd);assert(old_fragmented(limit)==hf);assert(check(limit)!=h);
 /* A following edit, and a partial final piece, must remain visible too. */
 h=check(limit);bytes[4096+72]=2;assert(check(limit)!=h);
 limit=1048595;h=check(limit);bytes[limit-1]^=1;assert(check(limit)!=h);
 /* Video addresses outlive their last decoder write. A CPU texture reusing
    such memory must fall through to the full check, rather than freeze. */
 Tex t={.size=131072,.video_gen=7,.stable=40};limit=t.size;fragmented=0;g_frame=500;video_revision=7;
 assert(video_check(&t,0)==NULL && uploads==0);
 h=check(t.size);bytes[72]^=0x35;fragmented=0;
 assert(video_check(&t,0)==NULL);assert(check(t.size)!=h);
 /* Actual decoder revisions are still uploaded immediately, once only. */
 fragmented=0;video_revision=8;assert(video_check(&t,0)==&t);assert(uploads==1 && t.video_gen==8 && t.frame_checked==500 && t.stable==0);
 assert(video_check(&t,0)==NULL && uploads==1);video_revision=0;assert(video_check(&t,0)==NULL);
 video_revision=9;assert(video_check(&t,0)==&t && uploads==2);
 starvation(131072,40,1000,0);starvation(131072,12,2000,0);starvation(4096,40,3000,0);starvation(131072,40,UINT32_MAX-40,1);
 printf("PASS: %u full-hash checks, sparse reveal edits, partial tails, reused video memory, immediate video revisions and budget-starved full checks at exactly 120 presents (including frame wrap); old hash misses reproduced\n",checks+3);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='texture-hash-', dir=ROOT / 'work-profiler') as tmp:
    temp = Path(tmp)
    c = temp / 'check.c'
    exe = temp / ('check.exe' if os.name == 'nt' else 'check')
    c.write_text(head + functions + sample_function + video_head + video_check + video_tail + poll_head + poll_check + poll_tail + body)
    subprocess.run([a.cc, '-O2', '-std=gnu11', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
