"""Issue #53: test the actual generated map-pan arithmetic, using the player's build.

No game code is stored here. The patch is hash-checked and the small arithmetic
block is extracted into a temporary C harness, then compiled with --cc.
Usage: python test_map_pan.py --build-dir PATH --cc PATH_TO_GCC
       python test_map_pan.py --build-dir PATH --check-only
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def sha(s):
    return hashlib.sha256(s.encode('latin-1')).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-dir', type=Path, required=True)
    ap.add_argument('--cc')
    ap.add_argument('--check-only', action='store_true')
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    spec = json.loads((repo / 'dread/dread_v210_patches.json').read_text())['files']['code_097.c']
    source = (args.build_dir / 'code_097.c').read_bytes().decode('latin-1').replace('\r\n', '\n')
    if sha(source) != spec['pristine']:
        raise SystemExit('Expected the pristine v2.1.0 code_097.c; input files are never modified.')
    lines = source.split('\n')
    for a, b, new in sorted(spec['hunks'], reverse=True):
        lines[a:b] = new
    patched = '\n'.join(lines)
    assert sha(patched) == spec['result'], 'Patch result hash mismatch'
    assert len(spec['hunks']) == 1 and spec['hunks'][0][0] == spec['hunks'][0][1]
    print('Map pan patch: pristine/result hashes and insertion-only scope PASS', flush=True)
    if args.check_only:
        return
    if not args.cc:
        ap.error('--cc is required unless --check-only is used')

    def arithmetic(s):
        a = s.index('void F_8B426B8(CPU* RESTRICT c, u64 e){')
        a = s.index('  SETS(3,c->v[2].f[0]*c->v[2].f[0]);'.lstrip(), a)
        b = s.index('  c->x[1]=(u64)(c->x[31]+24);'.lstrip(), a)
        return s[a:b]

    head = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32;
typedef union { float f[4]; uint64_t d[2]; } Vec;
typedef struct { uint64_t x[32]; Vec v[32]; } CPU;
static unsigned char memory[1024];
static u32 rd(u64 a) {u32 v; assert(a+4<=sizeof memory); memcpy(&v,memory+a,4); return v;}
static void wr(u64 a,float f) {assert(a+4<=sizeof memory);memcpy(memory+a,&f,4);}
static float rf(u64 a) {float f;memcpy(&f,memory+a,4);return f;}
#define RD32(a) rd(a)
#define SETS(i,value) (c->v[i].f[0]=(float)(value))
static float fp_maxnm32(float a,float b){return fmaxf(a,b);}
'''
    tail = r'''
typedef void (*Pan)(CPU*);
static void simulate(Pan pan,int fps,float speed,float x,float y,float zoom,float out[2]) {
 memset(memory,0,sizeof memory);wr(128+188,zoom);wr(512,speed);
 for(int i=0;i<fps;i++) {
  CPU c={0};float mag=sqrtf(x*x+y*y);c.x[20]=128;c.x[23]=256;c.x[8]=512;
  c.v[0].f[0]=mag?x/mag:0;c.v[1].f[0]=mag?y/mag:0;c.v[2].f[0]=mag;
  c.v[4].f[0]=2;c.v[8].f[0]=1.0f/fps;c.v[11].f[0]=2;c.v[12].f[0]=0;c.v[13].f[0]=.5f;
  pan(&c);wr(256,c.v[0].f[0]);wr(260,c.v[1].f[0]);
 }
 out[0]=rf(256);out[1]=rf(260);
}
int main(void) {
 const int rates[]={30,60,120,144,165,240,360,480};
 const float axes[][2]={{1,0},{0,-1},{.6f,.8f},{-.3f,.4f},{0,0}};
 unsigned checks=0;
 for(unsigned s=0;s<2;s++)for(unsigned z=0;z<3;z++)for(unsigned a=0;a<5;a++) {
  float speed=s?16:4,zoom=.5f+z*.5f,base[2],actual[2],sixty[2];
  simulate(original,60,speed,axes[a][0],axes[a][1],zoom,base);
  simulate(fixed,60,speed,axes[a][0],axes[a][1],zoom,sixty);
  assert(!memcmp(base,sixty,sizeof base));checks++;
  for(unsigned r=0;r<sizeof rates/sizeof *rates;r++) {
   simulate(fixed,rates[r],speed,axes[a][0],axes[a][1],zoom,actual);
   for(int k=0;k<2;k++)assert(fabsf(actual[k]-base[k])<.0002f*fmaxf(1,fabsf(base[k])));
   checks++;
  }
 }
 float base[2],fast[2];simulate(original,60,4,1,0,.5f,base);simulate(original,480,4,1,0,.5f,fast);
 assert(fast[0]>base[0]*7.9f);checks++;
 printf("Map pan: %u checks PASS; 60 FPS bit-identical; equal one-second movement at 30..480 FPS; original 480 FPS bug reproduced\n",checks);
 return 0;
}
'''
    csource = head + '\nstatic void original(CPU*c){\n' + arithmetic(source) + '\n}\nstatic void fixed(CPU*c){\n' + arithmetic(patched) + '\n}\n' + tail
    with tempfile.TemporaryDirectory(prefix='dread-map-pan-') as td:
        td = Path(td)
        cfile, exe = td / 'map_pan.c', td / ('map_pan.exe' if os.name == 'nt' else 'map_pan')
        cfile.write_text(csource)
        env = dict(os.environ, PATH=str(Path(args.cc).resolve().parent)+os.pathsep+os.environ.get('PATH', ''))
        subprocess.run([args.cc, '-std=c11', '-O2', str(cfile), '-lm', '-o', str(exe)], env=env, check=True)
        subprocess.run([str(exe)], env=env, check=True)


if __name__ == '__main__':
    main()
