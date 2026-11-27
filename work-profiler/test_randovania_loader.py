"""Compile and exercise the actual runtime activation guard without game files."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', required=True)
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    source = (repo / 'runtime/loader.c').read_text()
    start = source.index('void loader_enable_randovania(void){')
    end = source.index('\nint loader_load(void){', start)
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
typedef struct {const char* name; u64 base,size; u8 build_id[32];} SwrModInfo;
typedef struct {u32 nmods; const SwrModInfo* mods;} SwrModule;
static SwrModule module;
static SwrModule* g_mod=&module;
static u8* memory;
static unsigned enabled;
static int exported;
static jmp_buf failed;
void* g_code_mod=(void*)1;
#define GP(a) (memory+(a))
#define LOG(...) ((void)0)
static void fatal(const char* fmt,...){(void)fmt;longjmp(failed,1);}
static void* plat_dl_sym(void* module,const char* name){
 assert(module==g_code_mod);assert(!strcmp(name,"swr_randovania_enabled"));
 return exported? &enabled : NULL;
}
'''
    harness += source[start:end]
    harness += r'''
static SwrModInfo main_mod;
static const u64 offset=0x15D9780;
static const char original[]="IsInSTEAM_PC_FINAL_RETAIL";
static void reset(void){
 const u8 id[]={0x64,0x67,0x61,0xF6,0x43,0xAF,0xEB,0xB3,0x79,0xED,0xD5,0xE6,0xA5,0x15,0x1A,0xF2,0xCE,0xF9,0x3D,0xC1};
 memset(&main_mod,0,sizeof main_mod);main_mod.name="main";
 main_mod.base=16;main_mod.size=offset+sizeof original;
 memcpy(main_mod.build_id,id,sizeof id);memcpy(GP(main_mod.base+offset),original,sizeof original);
 module.nmods=1;module.mods=&main_mod;exported=1;enabled=0;
}
static void reject(void){
 if(!setjmp(failed)){loader_enable_randovania();assert(!"expected rejection");}
 assert(enabled==0);
}
int main(void){
 memory=calloc(1,offset+128);assert(memory);
 reset();loader_enable_randovania();assert(enabled==1);
 assert(!strcmp((char*)GP(main_mod.base+offset),"HasRandomizerPatches"));
 reset();exported=0;reject();assert(!memcmp(GP(main_mod.base+offset),original,sizeof original));
 reset();main_mod.build_id[0]^=1;reject();assert(!memcmp(GP(main_mod.base+offset),original,sizeof original));
 reset();module.nmods=0;reject();
 reset();main_mod.size=offset;reject();
 reset();GP(main_mod.base+offset)[0]='?';reject();
 free(memory);puts("Randovania loader: six activation/rejection cases PASS");return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='dread-rando-loader-') as tmp:
        c = Path(tmp) / 'loader_test.c'
        exe = Path(tmp) / ('loader_test.exe' if os.name == 'nt' else 'loader_test')
        c.write_text(harness)
        env = dict(os.environ, PATH=str(Path(args.cc).resolve().parent) + os.pathsep + os.environ.get('PATH', ''))
        subprocess.run([args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', str(c), '-o', str(exe)], check=True, env=env)
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == '__main__':
    main()
