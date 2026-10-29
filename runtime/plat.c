/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
#define _GNU_SOURCE

/* platform glue: threads, timers, VM, files. win32 + posix */
#include "rt.h"
#include <time.h>

__thread void* tls_thread;
int g_verbose = 1;
static FILE* g_logf;
static PMutex g_logm; static int g_logm_ok;

/* The log is written by a thread of its own. A write to the file can take seconds (MEASURED 2026-10-22, native Vulkan
   soak: single writes of 0.4-2.3 s, and the renderer logs on the present path, so each was a frozen frame). The game's
   threads only copy the line into g_lq under g_logm; log_drain writes it out under g_logw. Level 0 (crashes, fatal)
   drains at once, as does log_flush (every normal exit calls it before fflush(NULL)). Records: a u32 header (bit 31:
   to stderr too, bits 16-30: the timestamp prefix's length, bits 0-15: the record's length) and the text */
#define LQ_CAP (1u<<20)
static char *g_lq, *g_lq_out; static u32 g_lq_n; static u64 g_lq_dropped; static int g_lq_on;
static PMutex g_logw; static PCond g_lq_cv;

static void log_emit(const char* q, u32 n){
  for(u32 at=0; at+4<=n;){ u32 h; memcpy(&h,q+at,4); at+=4; u32 L=h&0xFFFF, pre=(h>>16)&0x7FFF;
    if(!g_logf){ char p[1200], q[1220]; const char* n=g_exename[0]?g_exename:"switchrecomp";
      /* keep the last run's log (a crash report needs it; the player restarts the game to send it) */
      snprintf(p,sizeof p,"%s%s.log",g_exedir,n); snprintf(q,sizeof q,"%s%s.previous.log",g_exedir,n); remove(q); rename(p,q);
      g_logf=fopen(p,"w"); }
    if(g_logf) fwrite(q+at,1,L,g_logf);
    if(h>>31){ fwrite(q+at+pre,1,L-pre,stderr);
#if defined(__ANDROID__)
      { extern void android_log(const char*); char b[2100]; u32 k=L-pre<sizeof b-1? L-pre : (u32)sizeof b-1; memcpy(b,q+at+pre,k); b[k]=0; android_log(b); }
#endif
    }
    at+=L; }
  if(g_logf) fflush(g_logf); }
static void log_drain(void){
  pm_lock(&g_logw);
  pm_lock(&g_logm); char* q=g_lq; u32 n=g_lq_n; u64 dropped=g_lq_dropped; g_lq=g_lq_out; g_lq_out=q; g_lq_n=0; g_lq_dropped=0; pm_unlock(&g_logm);
  u64 t0=plat_ns();
  if(n) log_emit(q,n);
  if(dropped && g_logf){ fprintf(g_logf,"[log: %llu lines dropped, the queue was full]\n",(unsigned long long)dropped); fflush(g_logf); }
  u64 dt=plat_ns()-t0;
  pm_unlock(&g_logw);
  if(dt>250000000ull) logf_(2,"log: a write to the log took %.0f ms (on the log's own thread, not the game's)\n",dt/1e6); }
void log_flush(void){ if(g_logm_ok) log_drain(); }
static void log_thread(void* a){ (void)a;
  for(;;){ pm_lock(&g_logm); while(!g_lq_n) pc_wait(&g_lq_cv,&g_logm,100000000); pm_unlock(&g_logm); log_drain(); } }

__thread FILE* g_log_redirect;   /* redirect for the threads.txt dumps so they don't end up in the log; per thread, so other threads keep logging normally */
void logf_(int lvl, const char* fmt, ...){
  if(!g_logm_ok){ pm_init(&g_logm); pm_init(&g_logw); pc_init(&g_lq_cv); g_lq=malloc(LQ_CAP); g_lq_out=malloc(LQ_CAP); g_logm_ok=1;
    if(g_lq && g_lq_out && plat_thread(log_thread,NULL,256*1024)==0) g_lq_on=1; }
  char buf[2048]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
  { FILE* rf=g_log_redirect; if(rf){ fputs(buf,rf); return; } }
  static u64 t0; u64 now=plat_ns(); if(!t0) t0=now;
  char tb[24]; snprintf(tb,sizeof tb,"[%7.2f] ",(double)(now-t0)/1e9);
  u32 L=(u32)strlen(buf);
  pm_lock(&g_logm);
  static int bol=1; u32 pre=bol? (u32)strlen(tb) : 0, need=4+pre+L;
  if(!g_lq || !g_lq_out) {}
  else if(g_lq_n+need>LQ_CAP) g_lq_dropped++;
  else { u32 h=(pre+L)|(pre<<16)|((lvl<=1 || g_verbose>=2)? 0x80000000u : 0); int was_empty=!g_lq_n;
    memcpy(g_lq+g_lq_n,&h,4); memcpy(g_lq+g_lq_n+4,tb,pre); memcpy(g_lq+g_lq_n+4+pre,buf,L); g_lq_n+=need;
    if(was_empty) pc_signal(&g_lq_cv); }
  bol= L && buf[L-1]=='\n';
  pm_unlock(&g_logm);
  if(lvl<=0 || !g_lq_on) log_drain();   /* a crash or fatal error: the process ends next */
}

void fatal(const char* fmt, ...){
  char buf[4096]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
  logf_(0,"FATAL: %s\n",buf);
#if defined(_WIN32)
  { extern void win_fatal_box(const char*); win_fatal_box(buf); }
#elif !defined(__ANDROID__)
  { char m[4400]; snprintf(m,sizeof m,"%s\n\nDetails are in %s.log",buf,g_exename); plat_message(m,1,1); }
#endif
#if defined(__ANDROID__)
  { extern void overlay_message(const char*); extern void gpu_gl_present_cpu(const u32*,int,int,int); char m[600]; snprintf(m,sizeof m,"Error: %.500s\n(see %s.log)",buf,g_exename); overlay_message(m);
    static u32 black[64*36]; for(int i=0;i<150;i++){ gpu_gl_present_cpu(black,64,36,64*4); plat_sleep_ns(200000000); } }
#endif
  log_flush(); fflush(NULL);
  _exit(1);
}

#if defined(_WIN32)
/* ------------------------------------------------------------------ Windows */
#include <process.h>
#include <io.h>        /* _get_osfhandle */
#include "thread_diag.inc"
void pm_init(PMutex* m){ InitializeSRWLock(&m->l); }
/* the file name of the module (EXE or DLL) holding pc, and pc's offset in it: the crash report names a graphics
   driver or system DLL that crashed. 0 when no module holds it */
int plat_module_of(const void* pc, char* name, int n, u64* off){
  HMODULE m=NULL; char p[MAX_PATH];
  if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)pc,&m) || !m) return 0;
  if(!GetModuleFileNameA(m,p,sizeof p)) return 0;
  const char* s=strrchr(p,'\\'); snprintf(name,n,"%s",s? s+1 : p); *off=(u64)(uintptr_t)pc-(u64)(uintptr_t)m; return 1; }

void pm_lock(PMutex* m){ td_wait_begin(1,(u64)(uintptr_t)m,0,(u64)(uintptr_t)__builtin_return_address(0)); AcquireSRWLockExclusive(&m->l); td_wait_end(); }

void pm_unlock(PMutex* m){ ReleaseSRWLockExclusive(&m->l); }

void pc_init(PCond* c){ InitializeConditionVariable(&c->c); }

void pc_signal(PCond* c){ WakeConditionVariable(&c->c); }

void pc_broadcast(PCond* c){ WakeAllConditionVariable(&c->c); }

int pc_wait(PCond* c, PMutex* m, i64 ns){
  DWORD ms= ns<0? INFINITE : (DWORD)((ns+999999)/1000000);
  td_wait_begin(2,(u64)(uintptr_t)c,ns,(u64)(uintptr_t)__builtin_return_address(0));
  BOOL ok=SleepConditionVariableSRW(&c->c,&m->l,ms,0);DWORD error=ok?0:GetLastError();td_wait_end();
  return !ok&&error==ERROR_TIMEOUT?1:0; }
typedef struct { PThreadFn fn; void* arg; } TArg;

static unsigned __stdcall tstart(void* p){ TArg a=*(TArg*)p; free(p); plat_diag_role("runtime"); a.fn(a.arg); return 0; }

int plat_thread(PThreadFn fn, void* arg, size_t stack){
  TArg* a=malloc(sizeof *a); a->fn=fn; a->arg=arg;
  uintptr_t h=_beginthreadex(NULL,(unsigned)stack,tstart,a,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);
  if(!h) return -1;
  { extern void plat_prof_add_thread(HANDLE); plat_prof_add_thread((HANDLE)h); }
  CloseHandle((HANDLE)h); return 0; }

void plat_thread_exit(void){ _endthreadex(0); }
static LONG g_hires;

void plat_sleep_ns(i64 ns){
  if(!g_hires){ typedef UINT (WINAPI*TBP)(UINT); HMODULE w=LoadLibraryA("winmm.dll"); if(w){ TBP f=(TBP)GetProcAddress(w,"timeBeginPeriod"); if(f) f(1);} g_hires=1; }
  if(ns<=0){ SwitchToThread(); return; }
  td_wait_begin(3,0,ns,(u64)(uintptr_t)__builtin_return_address(0));
  /* use the high-res waitable timer when the OS has it */
  static __thread HANDLE tmr; if(!tmr){ tmr=CreateWaitableTimerExW(NULL,NULL,0x00000002/*HIGH_RESOLUTION*/,TIMER_ALL_ACCESS); if(!tmr) tmr=CreateWaitableTimerW(NULL,TRUE,NULL); }
  if(tmr){ LARGE_INTEGER due; due.QuadPart=-(ns/100); if(!due.QuadPart) due.QuadPart=-1; SetWaitableTimer(tmr,&due,0,NULL,NULL,FALSE); WaitForSingleObject(tmr,INFINITE); }
  else Sleep((DWORD)(ns/1000000)); td_wait_end(); }

void plat_yield(void){ SwitchToThread(); }

void plat_thread_low_priority(void){ SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL); }

void plat_thread_high_priority(void){ SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL); }

u64 plat_thread_cpu_ns(void){ FILETIME c,e,k,u; if(!GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u)) return 0; return ((((u64)k.dwHighDateTime<<32)|k.dwLowDateTime)+(((u64)u.dwHighDateTime<<32)|u.dwLowDateTime))*100; }

u64 plat_process_cpu_ns(void){ FILETIME c,e,k,u; if(!GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u)) return 0; return ((((u64)k.dwHighDateTime<<32)|k.dwLowDateTime)+(((u64)u.dwHighDateTime<<32)|u.dwLowDateTime))*100; }

/* ---- guest clock, integer fast path (perf patch 2a) -------------------------
   The old form recomputed (double)t*1e9/(double)f on every call; `f` is a
   mutable static written under a branch so the divide could not be hoisted.
   vdivsd is ~13-14 cycle latency and poorly pipelined, and this is the hottest
   callee in the program (every guest MRS CNTVCT_EL0 and svcGetSystemTick).
   Replaced with a precomputed multiply-shift. The shift is the largest value
   <=48 whose multiplier still fits in 63 bits, so the result is EXACT for any
   frequency where the ratio is dyadic-representable -- including the 10 MHz
   every supported Windows build reports -- and bounded far below 1 ns else.
   Verified against the exact rational floor(q*19200000/f) over 6M samples in
   two uptime bands: worst deviation 1 tick (52.083 ns), zero non-monotonic
   steps, i.e. no worse than the double path it replaces.
   SWR_NO_FAST_CLOCK=1 restores the original arithmetic. */
#if defined(__SIZEOF_INT128__) && !defined(__TINYC__)
static u64 g_ns_mul; static int g_ns_sh;
static u64 g_tk_mul; static int g_tk_sh;
static int g_clock_slow;

static void clock_init(void){
  LARGE_INTEGER f; QueryPerformanceFrequency(&f);
  u64 freq=(u64)f.QuadPart; if(!freq) freq=10000000ull;
  int sh=48;
  while(sh>0 && ((unsigned __int128)1000000000u<<sh)/freq > 0x7FFFFFFFFFFFFFFFull) sh--;
  g_ns_sh=sh; g_ns_mul=(u64)(((unsigned __int128)1000000000u<<sh)/freq);
  sh=48;
  while(sh>0 && ((unsigned __int128)19200000u<<sh)/freq > 0x7FFFFFFFFFFFFFFFull) sh--;
  g_tk_sh=sh; g_tk_mul=(u64)(((unsigned __int128)19200000u<<sh)/freq);
  g_clock_slow= getenv("SWR_NO_FAST_CLOCK")? 1 : 0;
  LOG("clock: QPC %llu Hz, ns scale %llu>>%d, tick scale %llu>>%d%s\n",
      (unsigned long long)freq,(unsigned long long)g_ns_mul,g_ns_sh,
      (unsigned long long)g_tk_mul,g_tk_sh, g_clock_slow? " (fast path disabled)" : "");
}

u64 plat_ns(void){
  LARGE_INTEGER t;
  if(UNLIKELY(!g_ns_mul)) clock_init();
  QueryPerformanceCounter(&t);
  if(UNLIKELY(g_clock_slow)){
    static LARGE_INTEGER f; if(!f.QuadPart) QueryPerformanceFrequency(&f);
    return (u64)((double)t.QuadPart*1e9/(double)f.QuadPart); }
  return (u64)(((unsigned __int128)(u64)t.QuadPart * g_ns_mul) >> g_ns_sh); }

/* Raw 19.2 MHz counter straight off QPC, skipping the nanosecond round trip.
   Only used while the virtual clock is inactive (the normal case). */
u64 plat_ticks_raw(void){
  LARGE_INTEGER t;
  if(UNLIKELY(!g_tk_mul)) clock_init();
  QueryPerformanceCounter(&t);
  return (u64)(((unsigned __int128)(u64)t.QuadPart * g_tk_mul) >> g_tk_sh); }
#else
/* TCC has no __int128: keep exactly today's arithmetic. */
u64 plat_ns(void){ static LARGE_INTEGER f; LARGE_INTEGER t; if(!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
  return (u64)((double)t.QuadPart*1e9/(double)f.QuadPart); }
u64 plat_ticks_raw(void){ return plat_ns()*192ull/10000ull; }
#endif

i64 plat_unix_time(void){ return (i64)time(NULL); }

/* UTC offset in seconds, DST included */
i64 plat_tz_offset(void){ TIME_ZONE_INFORMATION z; DWORD r=GetTimeZoneInformation(&z); if(r==TIME_ZONE_ID_INVALID) return 0;
  long b=z.Bias+(r==TIME_ZONE_ID_DAYLIGHT? z.DaylightBias : r==TIME_ZONE_ID_STANDARD? z.StandardBias : 0); return -(i64)b*60; }
int g_ww_ok;   /* MEM_WRITE_WATCH: Windows tracks written pages, so unchanged buffers don't need a reread */
u8* plat_vm_reserve(u64 size){ void* p=!getenv("SWR_WW")? NULL : VirtualAlloc(NULL,size,MEM_RESERVE|MEM_WRITE_WATCH,PAGE_NOACCESS);
  if(p){ g_ww_ok=1; return (u8*)p; } return (u8*)VirtualAlloc(NULL,size,MEM_RESERVE,PAGE_NOACCESS); }

int plat_ww_collect(u8* base, u64 size, void (*cb)(u64)){
  static PVOID a[16384]; if(!g_ww_ok) return -1;
  for(int it=0;it<64;it++){ ULONG_PTR n=16384; ULONG gran=0;
    if(GetWriteWatch(WRITE_WATCH_FLAG_RESET,base,(SIZE_T)size,a,&n,&gran)) return -1;
    for(ULONG_PTR i=0;i<n;i++) cb((u64)((u8*)a[i]-base));
    if(n<16384) break; }
  return 0; }

int plat_vm_commit(u8* p, u64 size){ return VirtualAlloc(p,size,MEM_COMMIT,PAGE_READWRITE)? 0 : -1; }

void plat_vm_decommit(u8* p, u64 size){ if(size) VirtualFree(p,size,MEM_DECOMMIT); }   /* size 0 at the base would decommit the whole reservation */
void* plat_dl_open(const char* path){ return (void*)LoadLibraryA(path); }

void* plat_dl_sym(void* h, const char* n){ return (void*)GetProcAddress((HMODULE)h,n); }

int plat_mkdir(const char* p){ return CreateDirectoryA(p,NULL)? 0 : -1; }

int plat_is_dir(const char* p){ DWORD a=GetFileAttributesA(p); return a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_DIRECTORY); }

int plat_dir_open(PDir* d, const char* path){ char p[1100]; snprintf(p,sizeof p,"%s\\*",path); WIN32_FIND_DATAA fd; HANDLE h=FindFirstFileA(p,&fd);
  if(h==INVALID_HANDLE_VALUE) return -1; d->h=h; d->first=1; strncpy(d->name,fd.cFileName,511); d->is_dir=(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
  d->size=((u64)fd.nFileSizeHigh<<32)|fd.nFileSizeLow; return 0; }

int plat_dir_next(PDir* d){
  for(;;){
    if(d->first){ d->first=0; }
    else { WIN32_FIND_DATAA fd; if(!FindNextFileA((HANDLE)d->h,&fd)) return 0; strncpy(d->name,fd.cFileName,511); d->is_dir=(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0; d->size=((u64)fd.nFileSizeHigh<<32)|fd.nFileSizeLow; }
    if(strcmp(d->name,".") && strcmp(d->name,"..")) return 1; } }

void plat_dir_close(PDir* d){ FindClose((HANDLE)d->h); }

u64 plat_file_size(const char* p){ WIN32_FILE_ATTRIBUTE_DATA a; if(!GetFileAttributesExA(p,GetFileExInfoStandard,&a)) return 0; return ((u64)a.nFileSizeHigh<<32)|a.nFileSizeLow; }

int plat_pread(FILE* f, void* buf, u64 size, u64 off){
  HANDLE h=(HANDLE)_get_osfhandle(_fileno(f)); OVERLAPPED ov; memset(&ov,0,sizeof ov); ov.Offset=(DWORD)off; ov.OffsetHigh=(DWORD)(off>>32);
  DWORD got=0; if(!ReadFile(h,buf,(DWORD)size,&got,&ov)) return -1; return (int)got; }

int plat_ncpu(void){ SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }

void plat_battery(int* pct, int* chg){ SYSTEM_POWER_STATUS s; *pct=100; *chg=1;
  if(!GetSystemPowerStatus(&s) || (s.BatteryFlag&128) || s.BatteryLifePercent>100) return;
  *pct=s.BatteryLifePercent; *chg= s.ACLineStatus==1; }

/* a crash inside a system DLL (memcpy in msvcrt, the video driver) says nothing by itself: log the
   return addresses into the runtime found on the stack, they name the code that called it */
static void log_runtime_callers(CONTEXT* c){
  extern char __ImageBase; u64 b=(u64)(size_t)&__ImageBase;
  IMAGE_NT_HEADERS* nt=(IMAGE_NT_HEADERS*)(b+(u64)((IMAGE_DOS_HEADER*)b)->e_lfanew); u64 end=b+nt->OptionalHeader.SizeOfImage;
  if(c->Rip>=b && c->Rip<end) return;
  u64* sp=(u64*)(size_t)c->Rsp; u64* top=(u64*)((NT_TIB*)NtCurrentTeb())->StackBase;
  int n=0; for(; sp<top && n<8; sp++) if(*sp>b+0x1000 && *sp<end){ if(!n) logf_(0,"  called from the runtime at"); logf_(0," +%llx",(unsigned long long)(*sp-b)); n++; }
  if(n) logf_(0," (exe offsets)\n"); }

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* e){
  extern void crash_report(void* host_pc, void* fault_addr, int sig);
  log_runtime_callers(e->ContextRecord);
  crash_report((void*)e->ContextRecord->Rip,(void*)e->ExceptionRecord->ExceptionInformation[1],(int)e->ExceptionRecord->ExceptionCode);
  return EXCEPTION_EXECUTE_HANDLER; }

static LONG WINAPI veh_fatal(EXCEPTION_POINTERS* e){
  DWORD c=e->ExceptionRecord->ExceptionCode;
  if(c==EXCEPTION_STACK_OVERFLOW || c==0xC0000409 /* STACK_BUFFER_OVERRUN / fastfail */ || c==0xC0000374 /* heap corruption */){
    static volatile LONG once; if(!InterlockedExchange(&once,1)){ logf_(0,"\n*** Shutdown: exception %08lx at %p (stack/heap) ***\n",(unsigned long)c,(void*)e->ContextRecord->Rip); } }
  return EXCEPTION_CONTINUE_SEARCH; }
#include <signal.h>

static void on_abort(int s){ logf_(0,"\n*** Shutdown: abort() (signal %d), probably the video driver or out of memory ***\n",s); }

static void on_exit_log(void){ logf_(0,"program shutdown\n"); }

static void inv_param(const wchar_t* e, const wchar_t* f, const wchar_t* fi, unsigned l, uintptr_t r){ (void)e;(void)f;(void)fi;(void)l;(void)r; logf_(0,"warning: invalid parameter in the C library\n"); }

void plat_install_crash_handler(void){ SetUnhandledExceptionFilter(crash_filter);
  AddVectoredExceptionHandler(0,veh_fatal); signal(SIGABRT,on_abort); atexit(on_exit_log); _set_invalid_parameter_handler(inv_param);
  /* keep the display/system awake while playing */
  SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED); }
/* ------------------------------------------------------------------ sampling profiler (profile=1)
   ~1 ms tick: suspend each thread, grab rip, map it to a game function / exe offset / module.
   profile.txt gets rewritten every 10 s. */
#include "rt.h"
static HANDLE g_pth[256]; static volatile LONG g_npth;
static volatile DWORD g_prof_main_tid;
void plat_prof_mark_main(void){__atomic_store_n(&g_prof_main_tid,GetCurrentThreadId(),__ATOMIC_RELEASE);}
static volatile DWORD g_prof_gpu_tid;
void plat_prof_mark_gpu(void){__atomic_store_n(&g_prof_gpu_tid,GetCurrentThreadId(),__ATOMIC_RELEASE);}   /* snxvk gpu_thread */


void plat_prof_add_thread(HANDLE h){ HANDLE d=NULL; if(!DuplicateHandle(GetCurrentProcess(),h,GetCurrentProcess(),&d,0,FALSE,DUPLICATE_SAME_ACCESS)) return;
  LONG i=InterlockedIncrement(&g_npth)-1; if(i<256) g_pth[i]=d; else CloseHandle(d); }
typedef struct { u64 host; u32 idx; } PEnt;
static PEnt* g_pe; static u32 g_npe; static u64 g_plo, g_phi; static u32* g_pcnt;
#define PMOD 32
static struct { u64 base; char name[64]; u32 n; } g_pmod[PMOD]; static int g_npmod;
#define PEXE 65536   /* was 4096 -- too small for a 6 MB .text: once full, new windows were silently dropped */
static struct { u32 off; u32 n; } g_pexe[PEXE];
static u64 g_ptot, g_pguest, g_pexe_n, g_pexe_drop;

static int pe_cmp(const void* a, const void* b){ u64 x=((const PEnt*)a)->host, y=((const PEnt*)b)->host; return x<y? -1 : x>y; }

static void prof_note(u64 rip){
  g_ptot++;
  if(rip>=g_plo && rip<g_phi+0x4000){ u32 lo=0, hi=g_npe; while(hi-lo>1){ u32 m=(lo+hi)/2; if(g_pe[m].host<=rip) lo=m; else hi=m; } g_pcnt[lo]++; g_pguest++; return; }
  HMODULE hm=NULL; if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)rip,&hm) || !hm){ hm=NULL; }
  u64 base=(u64)(uintptr_t)hm;
  if(hm==GetModuleHandleA(NULL)){ u32 off=(u32)((rip-base)&~0x3Full); u32 k=(off*2654435761u)>>16; int placed=0; for(int t=0;t<PEXE;t++){ u32 s=(k+t)&(PEXE-1); if(!g_pexe[s].n || g_pexe[s].off==off){ g_pexe[s].off=off; g_pexe[s].n++; placed=1; break; } } if(!placed) g_pexe_drop++; g_pexe_n++; }
  int i; for(i=0;i<g_npmod;i++) if(g_pmod[i].base==base) break;
  if(i==g_npmod && g_npmod<PMOD){ g_pmod[i].base=base; g_pmod[i].n=0; if(!hm) strcpy(g_pmod[i].name,"(no module: generated/JIT code)"); else { char pth[MAX_PATH]; GetModuleFileNameA(hm,pth,MAX_PATH); const char* s=strrchr(pth,'\\'); snprintf(g_pmod[i].name,64,"%s",s? s+1 : pth); } g_npmod++; }
  if(i<g_npmod) g_pmod[i].n++; }
static const char* g_pfile="profile.txt"; static int g_pboot;

static void prof_write(void){
  FILE* f=fopen(g_pfile,"w"); if(!f) return;
  /* idle threads sit in ntdll/kernelbase/win32u, so percentages are of non-idle samples */
  u64 idle=0; for(int i=0;i<g_npmod;i++){ const char* n=g_pmod[i].name; if(!_stricmp(n,"ntdll.dll")||!_stricmp(n,"kernelbase.dll")||!_stricmp(n,"win32u.dll")||!_stricmp(n,"kernel32.dll")) idle+=g_pmod[i].n; }
  u64 act= g_ptot>idle? g_ptot-idle : 1;
  fprintf(f,"SwitchRecomp profile: %llu samples, %llu active (excluding waits). %% of active time\n\n",(unsigned long long)g_ptot,(unsigned long long)act);
  fprintf(f,"modules:\n"); for(int i=0;i<g_npmod;i++) { const char* n=g_pmod[i].name; int w=!_stricmp(n,"ntdll.dll")||!_stricmp(n,"kernelbase.dll")||!_stricmp(n,"win32u.dll")||!_stricmp(n,"kernel32.dll"); if(w) fprintf(f,"   (waits) %s: %llu samples\n",n,(unsigned long long)g_pmod[i].n); else fprintf(f,"  %6.2f%%  %s\n",100.0*g_pmod[i].n/act,n); }
  fprintf(f,"  %6.2f%%  game code (functions below)\n",100.0*g_pguest/act); fprintf(f,"\ngame functions (guest address):\n");
  u64 top[6]; int ntop=0;
  for(int r=0;r<60;r++){ u32 best=~0u, bn=0; for(u32 i=0;i<g_npe;i++) if(!(g_pcnt[i]&0x80000000u) && g_pcnt[i]>bn){ bn=g_pcnt[i]; best=i; }
    if(best==~0u || !bn || bn*2000ull<act) break; fprintf(f,"  %6.2f%%  %llx\n",100.0*bn/act,(unsigned long long)g_mod->entry_addr[g_pe[best].idx]);
    if(r<6) top[ntop++]=g_mod->entry_addr[g_pe[best].idx]; g_pcnt[best]|=0x80000000u; }
  for(u32 i=0;i<g_npe;i++) g_pcnt[i]&=0x7FFFFFFFu;
  /* raw A64 words of the top functions, for disassembling later */
  fprintf(f,"\ncode of the hottest game functions (first 96 instructions):\n");
  for(int t=0;t<ntop;t++){ fprintf(f,"  %llx:",(unsigned long long)top[t]); const u32* w=(const u32*)GP(top[t]);
    for(int k=0;k<96;k++){ if(k%16==0) fprintf(f,"\n   "); fprintf(f," %08x",w[k]); } fprintf(f,"\n"); }
  fprintf(f,"\nruntime (offset in the exe):\n");
  if(g_pexe_drop) fprintf(f,"  NOTE: %llu of %llu runtime samples were dropped (offset table full)\n",(unsigned long long)g_pexe_drop,(unsigned long long)g_pexe_n);
  for(int r=0;r<40;r++){ int best=-1; u32 bn=0; for(int s=0;s<PEXE;s++) if(g_pexe[s].n>bn && !(g_pexe[s].n&0x80000000u)){ bn=g_pexe[s].n; best=s; }
    if(best<0 || bn*2000ull<act) break; fprintf(f,"  %6.2f%%  +%x\n",100.0*bn/act,g_pexe[best].off); g_pexe[best].n|=0x80000000u; }
  for(int s=0;s<PEXE;s++) g_pexe[s].n&=0x7FFFFFFFu;
  fclose(f); }

/* SWR_PROFILE_RING=path: per-sample record of the guest main thread -- host time
   (plat_ns, same clock as SWR_TRACE_PRESENTS completed_ns), rip, and the return
   addresses found on its stack -- so samples can be joined to one specific long
   frame offline instead of read from a cumulative whole-run ranking. The stack
   is copied into a preallocated buffer while the thread is suspended (no locks,
   no allocation); filtering happens after it resumes. A stack word counts as a
   frame only if it points into the exe or the game DLL just after a call
   instruction. That is a scan, not an unwind: stale return addresses left in
   dead stack slots can still appear below the live frames. */
#define PRING_FR 24
#define PRING_STK 4096
typedef struct { u64 t, rip; u32 nfr, tid; u64 fr[PRING_FR]; } PRingEnt;
static PRingEnt* g_pring; static u32 g_pring_cap; static volatile LONG g_pring_n, g_pring_stop; static u64 g_pring_lost;
static const char* g_pring_path; static u64 g_pstk[PRING_STK]; static u32 g_pstk_n;
static u64 g_rexe_lo, g_rexe_hi, g_rdll_lo, g_rdll_hi;
static void pring_mod_range(u64 addr, u64* lo, u64* hi){ HMODULE hm=NULL;
  if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)addr,&hm) || !hm) return;
  u64 b=(u64)(uintptr_t)hm; IMAGE_NT_HEADERS* nt=(IMAGE_NT_HEADERS*)(b+(u64)((IMAGE_DOS_HEADER*)b)->e_lfanew);
  *lo=b; *hi=b+nt->OptionalHeader.SizeOfImage; }
static int pring_is_ret(u64 v){
  if(!((v>=g_rexe_lo+0x1008 && v<g_rexe_hi) || (v>=g_rdll_lo+0x1008 && v<g_rdll_hi))) return 0;
  const unsigned char* p=(const unsigned char*)(uintptr_t)v;
  if(p[-5]==0xE8) return 1;                                   /* call rel32 */
  static const int ks[]={2,3,4,6,7};                          /* call r/m64: FF /2 */
  for(int i=0;i<5;i++){ int k=ks[i]; if(p[-k]==0xFF && ((p[-k+1]>>3)&7)==2) return 1; }
  return 0; }
/* called with the main thread suspended: copy only */
static void pring_grab(const CONTEXT* cx){
  MEMORY_BASIC_INFORMATION mbi; g_pstk_n=0;
  if(!VirtualQuery((void*)(uintptr_t)cx->Rsp,&mbi,sizeof mbi)) return;
  u64 end=(u64)(uintptr_t)mbi.BaseAddress+mbi.RegionSize; u64 n=(end-cx->Rsp)/8; if(n>PRING_STK) n=PRING_STK;
  memcpy(g_pstk,(const void*)(uintptr_t)cx->Rsp,(size_t)n*8); g_pstk_n=(u32)n; }
/* called after resume */
static void pring_add(const CONTEXT* cx, DWORD tid){
  if(g_pring_stop) return;
  LONG i=g_pring_n; if((u32)i>=g_pring_cap){ g_pring_lost++; return; }
  PRingEnt* e=&g_pring[i]; e->t=plat_ns(); e->rip=cx->Rip; e->tid=tid; e->nfr=0;
  for(u32 k=0;k<g_pstk_n && e->nfr<PRING_FR;k++) if(pring_is_ret(g_pstk[k])) e->fr[e->nfr++]=g_pstk[k];
  InterlockedExchange(&g_pring_n,i+1); }
static void pring_name(FILE* f, u64 a){
  if(a>=g_plo && a<g_phi+0x4000 && g_npe){ u32 lo=0, hi=g_npe; while(hi-lo>1){ u32 m=(lo+hi)/2; if(g_pe[m].host<=a) lo=m; else hi=m; }
    if(g_pe[lo].host<=a){ fprintf(f,"G:%llx+%llx",(unsigned long long)g_mod->entry_addr[g_pe[lo].idx],(unsigned long long)(a-g_pe[lo].host)); return; } }
  if(a>=g_rexe_lo && a<g_rexe_hi){ fprintf(f,"X:%llx",(unsigned long long)(a-g_rexe_lo)); return; }
  HMODULE hm=NULL;
  if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)a,&hm) && hm){
    char pth[MAX_PATH]; GetModuleFileNameA(hm,pth,MAX_PATH); const char* s=strrchr(pth,'\\');
    fprintf(f,"M:%s+%llx",s? s+1 : pth,(unsigned long long)(a-(u64)(uintptr_t)hm)); return; }
  fprintf(f,"?:%llx",(unsigned long long)a); }
/* Appends samples recorded since the last flush, so a run the watchdog kills
   still leaves everything up to the last few seconds on disk. */
static volatile LONG g_pring_wr; static LONG g_pring_done;
static void pring_flush(int final){
  if(!g_pring) return;
  while(InterlockedExchange(&g_pring_wr,1)) Sleep(1);
  if(final && InterlockedExchange(&g_pring_stop,1)){ InterlockedExchange(&g_pring_wr,0); return; }
  FILE* f=fopen(g_pring_path,"a");
  if(f){ LONG n=g_pring_n;
    for(LONG i=g_pring_done;i<n;i++){ PRingEnt* e=&g_pring[i];
      fprintf(f,"%llu,%u,",(unsigned long long)e->t,e->tid); pring_name(f,e->rip); fputc(',',f);
      for(u32 k=0;k<e->nfr;k++){ if(k) fputc(';',f); pring_name(f,e->fr[k]); } fputc('\n',f); }
    g_pring_done=n;
    if(final) fprintf(f,"# end: %ld samples, %llu not recorded (ring full)\n",(long)n,(unsigned long long)g_pring_lost);
    fclose(f); }
  InterlockedExchange(&g_pring_wr,0); }
static void pring_dump(void){ pring_flush(1); }
static void pring_atexit(void){ pring_dump(); }

static unsigned __stdcall prof_thread(void* a){ (void)a;
  u64 last=GetTickCount64(); DWORD me=GetCurrentThreadId();
  u64 start=last;int main_only=getenv("SWR_PROFILE_MAIN_ONLY")!=NULL; int ring_host=getenv("SWR_PROFILE_RING_HOST")!=NULL, ring_all=getenv("SWR_PROFILE_RING_ALL")!=NULL;
  const char*delay_env=getenv("SWR_PROFILE_DELAY_MS"),*duration_env=getenv("SWR_PROFILE_DURATION_MS");
  u64 delay=delay_env?strtoull(delay_env,NULL,10):0,duration=duration_env?strtoull(duration_env,NULL,10):0;
  for(;;){ Sleep(g_pboot? 2 : 1);
    u64 elapsed=GetTickCount64()-start;
    if(elapsed<delay)continue;
    if(duration && elapsed>=delay+duration){prof_write();pring_dump();break;}
    LONG n=g_npth; if(n>256) n=256;
    for(LONG i=0;i<n;i++){ HANDLE h=g_pth[i]; if(!h || GetThreadId(h)==me) continue; if(main_only && GetThreadId(h)!=__atomic_load_n(&g_prof_main_tid,__ATOMIC_ACQUIRE))continue;
      DWORD tid=GetThreadId(h); int ring=g_pring && tid==__atomic_load_n(&g_prof_main_tid,__ATOMIC_ACQUIRE);
      if(SuspendThread(h)==(DWORD)-1) continue; CONTEXT cx; memset(&cx,0,sizeof cx); cx.ContextFlags=CONTEXT_CONTROL;
      if(GetThreadContext(h,&cx)){ if(g_pring && !ring && (ring_all || (ring_host && tid==__atomic_load_n(&g_prof_gpu_tid,__ATOMIC_ACQUIRE)))) ring=1;   /* SWR_PROFILE_RING_HOST: the native Vulkan GPU thread too */
        if(ring) pring_grab(&cx); ResumeThread(h); prof_note(cx.Rip); if(ring) pring_add(&cx,tid); } else ResumeThread(h); }
    if(g_pring && !g_pring_stop && g_pring_n-g_pring_done>=2000) pring_flush(0);
    if(GetTickCount64()-last>=(g_pboot? 5000u : 10000u)){ last=GetTickCount64(); prof_write(); }
    if(g_pboot && GetTickCount64()-start>=90000){ prof_write(); break; } }
  return 0; }

void plat_prof_start(int boot){
  if(!g_mod || !g_mod->nentries) return;
  g_pboot=boot; if(boot) g_pfile="startup.txt"; const char*output=getenv("SWR_PROFILE_PATH");if(output&&output[0])g_pfile=output;
  g_npe=g_mod->nentries; g_pe=malloc(sizeof(PEnt)*g_npe); g_pcnt=calloc(g_npe,4); g_plo=~0ull; g_phi=0;
  for(u32 i=0;i<g_npe;i++){ g_pe[i].host=(u64)(uintptr_t)g_mod->entry_fn[i]; g_pe[i].idx=i; if(g_pe[i].host<g_plo) g_plo=g_pe[i].host; if(g_pe[i].host>g_phi) g_phi=g_pe[i].host; }
  qsort(g_pe,g_npe,sizeof(PEnt),pe_cmp);
  { const char* rp=getenv("SWR_PROFILE_RING"); if(rp && rp[0]){ const char* mx=getenv("SWR_PROFILE_RING_MAX");
      g_pring_cap= mx? (u32)strtoul(mx,NULL,10) : 262144u; g_pring=calloc(g_pring_cap,sizeof(PRingEnt));
      if(g_pring){ extern char __ImageBase; g_pring_path=rp; g_rexe_lo=(u64)(uintptr_t)&__ImageBase;
        { IMAGE_NT_HEADERS* nt=(IMAGE_NT_HEADERS*)(g_rexe_lo+(u64)((IMAGE_DOS_HEADER*)g_rexe_lo)->e_lfanew); g_rexe_hi=g_rexe_lo+nt->OptionalHeader.SizeOfImage; }
        pring_mod_range(g_plo,&g_rdll_lo,&g_rdll_hi); atexit(pring_atexit);
        { FILE* f=fopen(rp,"w"); if(f){
            fprintf(f,"# main-thread sample ring, %u samples max; exe_base=%llx dll_base=%llx\n",g_pring_cap,(unsigned long long)g_rexe_lo,(unsigned long long)g_rdll_lo);
            fprintf(f,"# G:guest_entry+host_offset  X:exe_offset (VMA=imagebase+offset)  M:module+offset. frames = call-preceded stack words, innermost first\n");
            fprintf(f,"t_ns,tid,rip,frames\n"); fclose(f); } }
        LOG("profile: main-thread sample ring -> %s (%u samples max)\n",rp,g_pring_cap); } } }
  { HANDLE d=NULL; DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&d,0,FALSE,DUPLICATE_SAME_ACCESS); plat_prof_add_thread(d); CloseHandle(d); }
  uintptr_t h=_beginthreadex(NULL,0,prof_thread,NULL,0,NULL); if(h){ SetThreadPriority((HANDLE)h,THREAD_PRIORITY_TIME_CRITICAL); CloseHandle((HANDLE)h); } }
#else
void plat_thread_cpu_report(FILE*f,u64 now,int prime){(void)now;if(f&&!prime)fprintf(f,"      threads CPU: unavailable on this platform\n");}
void plat_diag_role(const char*s){(void)s;}
void plat_diag_guest(KThread*t){(void)t;}
void plat_thread_diag_start(void){}
/* ------------------------------------------------------------------ POSIX */
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sched.h>
#include <errno.h>
#include <limits.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#include <pthread/qos.h>
#endif

void pm_init(PMutex* m){ pthread_mutex_init(&m->l,NULL); }

void pm_lock(PMutex* m){ pthread_mutex_lock(&m->l); }

void pm_unlock(PMutex* m){ pthread_mutex_unlock(&m->l); }

#if defined(__APPLE__)
/* no pthread_condattr_setclock: timed waits are relative instead (pthread_cond_timedwait_relative_np) */
void pc_init(PCond* c){ pthread_cond_init(&c->c,NULL); }
#else
void pc_init(PCond* c){ pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a,CLOCK_MONOTONIC); pthread_cond_init(&c->c,&a); }
#endif

void pc_signal(PCond* c){ pthread_cond_signal(&c->c); }

void pc_broadcast(PCond* c){ pthread_cond_broadcast(&c->c); }

int pc_wait(PCond* c, PMutex* m, i64 ns){
  if(ns<0){ pthread_cond_wait(&c->c,&m->l); return 0; }
#if defined(__APPLE__)
  { struct timespec rel={(time_t)(ns/1000000000),(long)(ns%1000000000)}; return pthread_cond_timedwait_relative_np(&c->c,&m->l,&rel)==ETIMEDOUT; }
#endif
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); u64 t=(u64)ts.tv_sec*1000000000ull+ts.tv_nsec+(u64)ns;
  ts.tv_sec=t/1000000000ull; ts.tv_nsec=t%1000000000ull;
  return pthread_cond_timedwait(&c->c,&m->l,&ts)==ETIMEDOUT; }
typedef struct { PThreadFn fn; void* arg; } TArg;

static void* tstart(void* p){ TArg a=*(TArg*)p; free(p);
#if defined(__APPLE__)
  /* Apple silicon: the game's, GPU and audio threads belong on the performance cores; at the default class macOS
     also puts them on the efficiency cores */
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
#endif
  a.fn(a.arg); return NULL; }

int plat_thread(PThreadFn fn, void* arg, size_t stack){
  TArg* a=malloc(sizeof *a); a->fn=fn; a->arg=arg; pthread_attr_t at; pthread_attr_init(&at);
  /* a size that isn't a whole number of pages (16K on Apple silicon) is refused, and the thread silently gets the
     system's default stack instead (512K on macOS) */
  if(stack){ long pg=sysconf(_SC_PAGESIZE); if(pg>0) stack=(stack+(size_t)pg-1)&~((size_t)pg-1); if(stack<PTHREAD_STACK_MIN) stack=PTHREAD_STACK_MIN; pthread_attr_setstacksize(&at,stack); }
  pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED); pthread_t t; int r=pthread_create(&t,&at,tstart,a); pthread_attr_destroy(&at); return r? -1 : 0; }

void plat_thread_exit(void){ pthread_exit(NULL); }

void plat_sleep_ns(i64 ns){ if(ns<=0){ sched_yield(); return; } struct timespec ts={ns/1000000000,ns%1000000000}; while(nanosleep(&ts,&ts)==-1 && errno==EINTR); }

void plat_yield(void){ sched_yield(); }

#if defined(__APPLE__)
void plat_thread_low_priority(void){ pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0); }

void plat_thread_high_priority(void){ pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0); }
#else
void plat_thread_low_priority(void){ }

void plat_thread_high_priority(void){ }
#endif

u64 plat_thread_cpu_ns(void){ struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }

u64 plat_process_cpu_ns(void){ struct timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }

u64 plat_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }
/* the 19.2 MHz counter from the monotonic clock, split so the product can't overflow (main.c rt_ticks fast path) */
u64 plat_ticks_raw(void){ u64 n=plat_ns(); return n/10000ull*192ull + (n%10000ull)*192ull/10000ull; }

i64 plat_unix_time(void){ return (i64)time(NULL); }

i64 plat_tz_offset(void){ time_t t=time(NULL); struct tm lt; localtime_r(&t,&lt); return (i64)lt.tm_gmtoff; }
int g_ww_ok;

int plat_ww_collect(u8* base, u64 size, void (*cb)(u64)){ (void)base; (void)size; (void)cb; return -1; }

/* The guest maps in 4K pages. A host with bigger pages (16K on Apple silicon) commits every host page that holds a
   committed 4K page, and keeps a bit per 4K page of the reservation (mem.c calls these under its lock): a host page
   is only given back once none of its 4K pages is committed; until then a decommitted 4K page is zeroed in place, as
   it would read after a fresh commit */
static u8* g_vm_base; static u64 g_vm_size, g_vm_page=4096; static u8* g_vm_bits;
static int vm_bit(u64 pg){ return (g_vm_bits[pg>>3]>>(pg&7))&1; }
static void vm_bits_set(u64 o, u64 n, int on){ for(u64 pg=o>>12, e=(o+n)>>12; pg<e; pg++){ if(on) g_vm_bits[pg>>3]|=(u8)(1u<<(pg&7)); else g_vm_bits[pg>>3]&=(u8)~(1u<<(pg&7)); } }
/* fresh, inaccessible pages: the memory goes back to the system and reads as zero when committed again. macOS's
   MADV_DONTNEED keeps the old contents, so it maps new pages over the range there */
static void vm_release(u8* p, u64 size){
#if defined(__APPLE__)
  if(mmap(p,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0)==MAP_FAILED){ memset(p,0,size); }
#else
  madvise(p,size,MADV_DONTNEED); mprotect(p,size,PROT_NONE);
#endif
}

u8* plat_vm_reserve(u64 size){ void* p=mmap(NULL,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0); if(p==MAP_FAILED) return NULL;
  long pg=sysconf(_SC_PAGESIZE);
  if(pg>4096 && !g_vm_base){ g_vm_bits=calloc((size_t)(size>>15)+1,1);
    if(g_vm_bits){ g_vm_base=(u8*)p; g_vm_size=size; g_vm_page=(u64)pg; LOG("memory: host pages of %ld KB, guest pages of 4 KB tracked\n",pg>>10); } }
  return (u8*)p; }

static int vm_tracked(u8* p, u64 size){ return g_vm_bits && p>=g_vm_base && p+size<=g_vm_base+g_vm_size; }

int plat_vm_commit(u8* p, u64 size){
  if(!vm_tracked(p,size)) return mprotect(p,size,PROT_READ|PROT_WRITE);
  u64 o=(u64)(p-g_vm_base), a=o&~(g_vm_page-1), e=(o+size+g_vm_page-1)&~(g_vm_page-1);
  if(mprotect(g_vm_base+a,e-a,PROT_READ|PROT_WRITE)) return -1;
  vm_bits_set(o,size,1); return 0; }

void plat_vm_decommit(u8* p, u64 size){
  if(!size) return;
  if(!vm_tracked(p,size)){ vm_release(p,size); return; }
  u64 o=(u64)(p-g_vm_base), end=o+size;
  vm_bits_set(o,size,0);
  for(u64 a=o&~(g_vm_page-1); a<end; a+=g_vm_page){
    u64 lo= a>o? a : o, hi= a+g_vm_page<end? a+g_vm_page : end;
    int used=0; for(u64 pg=a>>12; pg<(a+g_vm_page)>>12 && !used; pg++) used=vm_bit(pg);
    if(!used) vm_release(g_vm_base+a,g_vm_page);   /* no committed 4K page left in it */
    else memset(g_vm_base+lo,0,hi-lo); } }   /* still committed around it: only these 4K pages are dropped */

void* plat_dl_open(const char* path){ return dlopen(path,RTLD_NOW|RTLD_LOCAL); }

void* plat_dl_sym(void* h, const char* n){ return dlsym(h,n); }

int plat_mkdir(const char* p){ return mkdir(p,0755); }

int plat_is_dir(const char* p){ struct stat s; return !stat(p,&s) && S_ISDIR(s.st_mode); }
typedef struct { DIR* d; char path[1024]; } PD_;

int plat_dir_open(PDir* d, const char* path){ PD_* p=malloc(sizeof *p); p->d=opendir(path); if(!p->d){ free(p); return -1; } strncpy(p->path,path,1023); p->path[1023]=0; d->h=p; return 0; }

int plat_dir_next(PDir* d){ PD_* p=d->h; struct dirent* e;
  while((e=readdir(p->d))){ if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue; strncpy(d->name,e->d_name,511); d->name[511]=0;
    char f[1600]; snprintf(f,sizeof f,"%s/%s",p->path,e->d_name); struct stat s; if(stat(f,&s)) continue; d->is_dir=S_ISDIR(s.st_mode); d->size=(u64)s.st_size; return 1; }
  return 0; }

void plat_dir_close(PDir* d){ PD_* p=d->h; closedir(p->d); free(p); }

u64 plat_file_size(const char* p){ struct stat s; if(stat(p,&s)) return 0; return (u64)s.st_size; }

int plat_pread(FILE* f, void* buf, u64 size, u64 off){ return (int)pread(fileno(f),buf,size,(off_t)off); }

int plat_ncpu(void){ long n=sysconf(_SC_NPROCESSORS_ONLN); return n>0? (int)n : 1; }

void plat_battery(int* pct, int* chg){ *pct=100; *chg=1;
  FILE* f=fopen("/sys/class/power_supply/BAT0/capacity","r"); if(!f) return;
  if(fscanf(f,"%d",pct)!=1 || *pct<0 || *pct>100) *pct=100; fclose(f);
  f=fopen("/sys/class/power_supply/BAT0/status","r"); if(!f) return;
  char s[32]={0}; if(fgets(s,sizeof s,f)) *chg= strncmp(s,"Discharging",11)!=0; fclose(f); }

static void sighandler(int sig, siginfo_t* si, void* uc_){
  extern void crash_report(void* host_pc, void* fault_addr, int sig);
  void* pc=NULL;
#if defined(__APPLE__) && defined(__aarch64__)
  ucontext_t* uc=(ucontext_t*)uc_; pc=(void*)(uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
#elif defined(__APPLE__) && defined(__x86_64__)
  ucontext_t* uc=(ucontext_t*)uc_; pc=(void*)(uintptr_t)uc->uc_mcontext->__ss.__rip;
#elif defined(__x86_64__)
  #include <ucontext.h>
  ucontext_t* uc=(ucontext_t*)uc_; pc=(void*)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
  #include <ucontext.h>
  ucontext_t* uc=(ucontext_t*)uc_; pc=(void*)uc->uc_mcontext.pc;
#else
  (void)uc_;
#endif
  crash_report(pc,si->si_addr,sig); _exit(1); }
#if !defined(__ANDROID__)
/* errors and the F1 help as a desktop dialog: zenity (GNOME and most desktops) or kdialog (KDE, the
   Steam Deck). Without either the text is only in the log. wait=0 leaves the dialog open while the game runs */
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;

static void reap(void* p){ int st; waitpid((pid_t)(size_t)p,&st,0); }

#if defined(__APPLE__)
/* macOS: a dialog through osascript (any thread, unlike an AppKit alert, which only the main thread may show) */
void plat_message(const char* text, int error, int wait){
  char esc[4600]; size_t n=0;
  for(const char* s=text; *s && n<sizeof esc-3; s++){ if(*s=='"' || *s=='\\') esc[n++]='\\'; esc[n++]=*s; }
  esc[n]=0;
  char script[5000]; snprintf(script,sizeof script,"display dialog \"%s\" with title \"Metroid Dread\" buttons {\"OK\"} default button 1 with icon %s",esc,error? "stop" : "note");
  char* a[]={"osascript","-e",script,NULL}; pid_t pid;
  if(posix_spawnp(&pid,a[0],NULL,NULL,a,environ)) return;
  if(wait) reap((void*)(size_t)pid); else plat_thread(reap,(void*)(size_t)pid,1<<16); }
#else
void plat_message(const char* text, int error, int wait){
  if(!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) return;
  char* z[]={"zenity",error? "--error" : "--info","--no-markup","--no-wrap","--title=SwitchRecomp","--text",(char*)text,NULL};
  char* k[]={"kdialog","--title","SwitchRecomp",error? "--error" : "--msgbox",(char*)text,NULL};
  pid_t pid;
  if(posix_spawnp(&pid,z[0],NULL,NULL,z,environ) && posix_spawnp(&pid,k[0],NULL,NULL,k,environ)) return;
  if(wait) reap((void*)(size_t)pid); else plat_thread(reap,(void*)(size_t)pid,1<<16); }
#endif
#endif

void plat_install_crash_handler(void){
  struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=sighandler; sa.sa_flags=SA_SIGINFO|SA_ONSTACK;
  static char altstack[65536]; stack_t ss; ss.ss_sp=altstack; ss.ss_size=sizeof altstack; ss.ss_flags=0; sigaltstack(&ss,NULL);
  sigaction(SIGSEGV,&sa,NULL); sigaction(SIGBUS,&sa,NULL); sigaction(SIGILL,&sa,NULL); sigaction(SIGFPE,&sa,NULL); }
#endif
#ifndef _WIN32
void plat_prof_start(int boot){ (void)boot; }
void plat_prof_mark_main(void){}
void plat_prof_mark_gpu(void){}
#endif
