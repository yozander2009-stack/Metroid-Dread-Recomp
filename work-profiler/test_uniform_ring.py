# Copyright 2026 SwitchRecomp contributors. Apache-2.0; see LICENSE.txt.
"""Exercise production ring code against a deliberately lagging mock GPU.

Each 16-byte cell records the last queued draw reading it. A fence completes
only draws queued before it, catching writes protected by an early/stale fence.
Run: python -B work-profiler/test_uniform_ring.py --cc /path/to/gcc
Use --source PATH for a pre-fix gpu_gl.c negative control.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
parser.add_argument('--source', type=Path, default=ROOT / 'runtime/gpu_gl.c')
a = parser.parse_args()
source = a.source.read_text()
start = source.index('static u32 g_ur_pending, g_ur_closed;') if 'static u32 g_ur_pending, g_ur_closed;' in source else source.index('static u32 ur_alloc(u32 sz){')
end = source.index('\ntypedef struct', start)
allocator = source[start:end]
if 'static void ur_begin_draw(' in allocator:
    assert 'ur_begin_draw(), upload_cbufs(0,vs), upload_cbufs(4,fs)' in source
    assert 'ur_reusable(e->off,sz)' in source
else:
    allocator += '\nstatic void ur_begin_draw(void){}\nstatic int ur_reusable(u32 o,u32 s){(void)o;(void)s;return 1;}\nstatic u32 g_ur_pending,g_ur_closed;\n'
head = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32; typedef uint64_t u64; typedef unsigned GLenum; typedef uintptr_t GLsync;
#define UR_SEGS 4
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 1
#define GL_TIMEOUT_EXPIRED 0x911B
#define GL_ALREADY_SIGNALED 0x911A
#define GL_CONDITION_SATISFIED 0x911C
#define GL_WAIT_FAILED 0x911D
#define WARN(...) ((void)0)
static u32 g_ur_size=32u<<20,g_ur_head,g_ur_align=256;
static u64 g_ur_abs,st_ur_wait;
static GLsync g_ur_f[UR_SEGS];
static u32 queued,completed,last_read[(32u<<20)/16],prefix[65536];
static unsigned char live[65536];
static unsigned fences,waits,finishes,timeouts,mode;
static const char*label;
static GLsync p_fs(GLenum c,unsigned f){
 (void)c;(void)f;if(mode==3)return 0;
 unsigned n=++fences;if(n>=65536)abort();live[n]=1;prefix[n]=queued;return n;
}
static GLenum p_cws(GLsync f,unsigned flags,u64 ns){
 (void)flags;if(!f||!live[f])abort();waits++;
 if(mode==2)return GL_WAIT_FAILED;
 if(mode==1){if(!ns)return GL_TIMEOUT_EXPIRED;if(timeouts){timeouts--;return GL_TIMEOUT_EXPIRED;}}
 if(completed<prefix[f])completed=prefix[f];
 return ns?GL_CONDITION_SATISFIED:GL_ALREADY_SIGNALED;
}
static void p_ds(GLsync f){if(!f||!live[f])abort();live[f]=0;}
static void glFinish(void){finishes++;completed=queued;}
'''
body = r'''
static int fail(const char*message,u32 off){
 fprintf(stderr,"FAIL %s: %s at %u (queued=%u completed=%u head=%u fences=%u waits=%u)\n",label,message,off,queued,completed,g_ur_head,fences,waits);return 0;
}
static void reset(u32 alignment,unsigned m){
 g_ur_size=32u<<20;g_ur_align=alignment;g_ur_head=0;g_ur_abs=st_ur_wait=0;g_ur_pending=g_ur_closed=0;
 queued=completed=fences=waits=finishes=0;mode=m;timeouts=2;
 memset(g_ur_f,0,sizeof g_ur_f);memset(last_read,0,sizeof last_read);memset(live,0,sizeof live);
}
static int write_at(u32 off,u32 size){
 if(off+size>g_ur_size)return fail("allocation past ring",off);
 for(u32 i=off/16;i<(off+size+15)/16;i++)if(last_read[i]>completed)return fail("overwrote data still read by a queued draw",off);
 return 1;
}
typedef struct {u32 off,size;u64 abs;}Entry;
static int draw(u32 size,unsigned banks,int caching,Entry*cache){
 u32 offsets[36],sizes[36];ur_begin_draw();
 for(unsigned b=0;b<banks;b++){
  u32 bytes=b==0&&caching?256:size;
  int hit=b==0&&caching&&cache->size>=bytes&&g_ur_abs-cache->abs<g_ur_size/2&&ur_reusable(cache->off,bytes);
  u32 off=hit?cache->off:ur_alloc(bytes);
  if(!hit&&!write_at(off,bytes))return 0;
  if(b==0&&caching&&!hit)*cache=(Entry){off,bytes,g_ur_abs};
  offsets[b]=off;sizes[b]=bytes;
 }
 queued++;
 for(unsigned b=0;b<banks;b++)for(u32 i=offsets[b]/16;i<(offsets[b]+sizes[b]+15)/16;i++)last_read[i]=queued;
 return 1;
}
static int run(u32 size,u32 alignment,unsigned banks,int caching,unsigned m){
 reset(alignment,m);Entry cache={0};
 while(g_ur_abs<(u64)g_ur_size*3)if(!draw(size,banks,caching,&cache))return 0;
 if(m==1&&(timeouts||!st_ur_wait))return fail("timeout retries not exercised",0);
 if((m==2||m==3)&&!finishes)return fail("failure did not finish GPU work",0);
 return 1;
}
static int spans(void){
 reset(256,0);const u32 seg=g_ur_size/UR_SEGS;
 g_ur_head=seg-256;g_ur_abs=g_ur_head;ur_begin_draw();
 u32 off=ur_alloc(512);if(!write_at(off,512))return 0;
 queued++;last_read[off/16]=queued;
 if(fences)return fail("fence inserted before current draw",off);
 if(ur_reusable(off,512))return fail("retired crossing span reused",off);
 ur_begin_draw();
 if(!g_ur_f[0]||prefix[g_ur_f[0]]!=queued)return fail("fence missed its last draw",off);
 if(ur_reusable(off,256))return fail("sealed span reused",off);
 if(!ur_reusable(seg,256))return fail("active segment reuse disabled",seg);
 /* An early return after setup still seals pending retirement next time. */
 g_ur_head=2*seg-256;g_ur_abs=g_ur_head;ur_alloc(512);ur_begin_draw();
 if(!g_ur_f[1]||prefix[g_ur_f[1]]!=queued)return fail("abandoned setup retirement lost",seg);
 return 1;
}
int main(void){
 const u32 alignments[]={16,64,256,1024};unsigned cases=0,failed=0;
 label="exact boundary";
 for(unsigned a=0;a<4;a++)for(u32 size=1024;size<=65536;size*=2){cases++;if(!run(size,alignments[a],1,0,0))failed++;}
 label="nonexact crossing within draw";
 for(unsigned a=0;a<4;a++){cases++;if(!run(20003,alignments[a],36,0,0))failed++;}
 label="cache hit before setup crossing";
 for(unsigned a=0;a<4;a++){cases++;if(!run(12337,alignments[a],8,1,0))failed++;}
 label="retired partial span";cases++;if(!spans())failed++;
 label="repeated timeout";cases++;if(!run(20003,256,36,0,1))failed++;
 label="wait failure";cases++;if(!run(20003,256,36,0,2))failed++;
 label="fence creation failure";cases++;if(!run(20003,256,36,0,3))failed++;
 label="disabled ring";cases++;reset(256,0);g_ur_size=0;ur_begin_draw();if(fences||waits||finishes){fail("disabled ring did work",0);failed++;}
 if(failed){fprintf(stderr,"%u/%u ring cases failed\n",failed,cases);return 1;}
 printf("PASS: %u ring lifetime cases; exact/nonexact boundaries, 36-bank draws, retired reuse, three wraps, repeated timeout, wait/fence failures, disabled ring\n",cases);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='uniform-ring-') as tmp:
    temp = Path(tmp)
    c = temp / 'check.c'
    exe = temp / ('check.exe' if os.name == 'nt' else 'check')
    c.write_text(head + allocator + body)
    env = dict(os.environ, PATH=str(Path(a.cc).resolve().parent)+os.pathsep+os.environ.get('PATH', ''))
    subprocess.run([a.cc, '-O2', '-std=gnu11', str(c), '-o', str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)
