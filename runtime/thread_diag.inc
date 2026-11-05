/* SwitchRecomp - Copyright 2026 MZ Labs; Apache-2.0 (LICENSE.txt/NOTICE.txt).
   Windows CPU accounting and opt-in hitch sampling. Guest clocks/logic are
   unchanged; tracing adds measurement overhead. */
#include <tlhelp32.h>
#define TD_MAX 512
#define TD_RING 256
typedef struct {u64 begin,end,object,caller; i64 requested; int kind;} TDWait;
typedef struct {
 DWORD tid; HANDLE handle; char role[48]; KThread* guest;
 volatile LONG ready; volatile int wait_kind; u64 wait_object,wait_caller; i64 wait_requested;
 u64 wait_begin; TDWait events[TD_RING]; u32 head,tail,dropped; u64 last_cpu;
} TDThread;
static TDThread td_threads[TD_MAX];static LONG td_count;static SRWLOCK td_lock=SRWLOCK_INIT;
static __thread TDThread* td_self;static int td_trace_enabled;
static u64 td_cpu(HANDLE h){FILETIME c,e,k,u;if(!GetThreadTimes(h,&c,&e,&k,&u))return 0;
 return ((((u64)k.dwHighDateTime<<32)|k.dwLowDateTime)+(((u64)u.dwHighDateTime<<32)|u.dwLowDateTime))*100;}
static TDThread* td_find(DWORD tid){for(int i=0;i<td_count&&i<TD_MAX;i++)if(td_threads[i].ready&&td_threads[i].tid==tid)return &td_threads[i];return NULL;}
static TDThread* td_add(DWORD tid){
 AcquireSRWLockExclusive(&td_lock);TDThread*r=td_find(tid);
 if(!r&&td_count<TD_MAX){HANDLE h=OpenThread(THREAD_QUERY_INFORMATION|THREAD_GET_CONTEXT|THREAD_SUSPEND_RESUME,FALSE,tid);
   if(!h)h=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,tid);
   if(h){r=&td_threads[td_count++];r->tid=tid;r->handle=h;strcpy(r->role,"native");__atomic_store_n(&r->ready,1,__ATOMIC_RELEASE);}}
 ReleaseSRWLockExclusive(&td_lock);return r;
}
static void td_enumerate(void){HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);if(s==INVALID_HANDLE_VALUE)return;
 THREADENTRY32 e={0};e.dwSize=sizeof e;if(Thread32First(s,&e))do{if(e.th32OwnerProcessID==GetCurrentProcessId())td_add(e.th32ThreadID);}while(Thread32Next(s,&e));CloseHandle(s);}
const char* plat_diag_role_get(void){ return td_self && td_self->role[0]? td_self->role : NULL; }   /* crash reports */
void plat_diag_role(const char*name){if(!td_self)td_self=td_add(GetCurrentThreadId());if(td_self)snprintf(td_self->role,sizeof td_self->role,"%s",name);}
void plat_diag_guest(KThread*t){plat_diag_role(t->is_main?"guest-main":"guest");if(td_self)td_self->guest=t;}
static void td_wait_begin(int kind,u64 object,i64 requested,u64 caller){
 if(!td_trace_enabled||!td_self)return;
 td_self->wait_object=object;td_self->wait_requested=requested;td_self->wait_caller=caller;td_self->wait_begin=plat_ns();
 __atomic_store_n(&td_self->wait_kind,kind,__ATOMIC_RELEASE);
}
static void td_wait_end(void){
 if(!td_trace_enabled||!td_self)return;TDThread*r=td_self;u64 end=plat_ns();int kind=__atomic_load_n(&r->wait_kind,__ATOMIC_ACQUIRE);
 __atomic_store_n(&r->wait_kind,0,__ATOMIC_RELEASE);
 if(!kind||end-r->wait_begin<100000)return;
 u32 head=r->head,tail=__atomic_load_n(&r->tail,__ATOMIC_ACQUIRE);
 if(head-tail==TD_RING){r->dropped++;return;}
 r->events[head&(TD_RING-1)]=(TDWait){r->wait_begin,end,r->wait_object,r->wait_caller,r->wait_requested,kind};
 __atomic_store_n(&r->head,head+1,__ATOMIC_RELEASE);
}
static const char* td_wait_name(int kind){return kind==1?"mutex":kind==2?"condition":kind==3?"sleep":kind==4?"yield":"none";}
/* CreateToolhelp32Snapshot walks every thread on the PC (thousands) and took ~20 ms. It used to run inside
   plat_thread_cpu_report, i.e. on the render thread every 5 s, which made one frame late every 5 s at any
   frame rate. Now a low-priority helper does it; the report only reads the table. */
static volatile LONG td_enum_started;
static unsigned __stdcall td_enumerator(void*unused){(void)unused;SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
 for(;;){td_enumerate();Sleep(5000);}return 0;}
void plat_thread_cpu_report(FILE*f,u64 now,int prime){
 static u64 previous,process_previous;
 if(!InterlockedExchange(&td_enum_started,1)){uintptr_t h=_beginthreadex(NULL,0,td_enumerator,NULL,0,NULL);if(h)CloseHandle((HANDLE)h);}u64 process=plat_process_cpu_ns(),dt=previous?now-previous:0;
 u64 total=process-process_previous,sum=0;
 if(!prime&&previous&&dt&&f)fprintf(f,"      threads CPU: core%%=100*thread CPU/wall (100%%=one logical core); share%%=thread/process CPU: ");
 for(int i=0;i<td_count&&i<TD_MAX;i++){TDThread*r=&td_threads[i];if(!r->ready)continue;u64 cpu=td_cpu(r->handle),delta=cpu>=r->last_cpu?cpu-r->last_cpu:0;
  sum+=delta;if(!prime&&previous&&dt&&f){KThread*t=r->guest;
   fprintf(f,"T%lu[%s",(unsigned long)r->tid,r->role);if(t)fprintf(f,":%llx:%s",(unsigned long long)t->id,t->name);
   fprintf(f,"]=%.2f%%/%.2f%%",100.0*delta/dt,total?100.0*delta/total:0.0);if(td_trace_enabled)fprintf(f,"(wait-trace drops=%u)",r->dropped);fprintf(f,"; ");}
  r->last_cpu=cpu;
 }
 if(!prime&&previous&&dt&&f){fprintf(f,"accounted=%.2f%% process CPU; interval=%.6fs\n",total?100.0*sum/total:0.0,dt/1e9);fflush(f);}
 previous=now;process_previous=process;
}
static int td_in_window(int frame){const char*s=getenv("SWR_THREAD_TRACE_FRAMES");if(!s||!*s)return 1;
 while(*s){char*end;int lo=(int)strtol(s,&end,10);if(end==s)break;s=end;int hi=lo;if(*s=='-'){s++;hi=(int)strtol(s,&end,10);s=end;}if(frame>=lo&&frame<=hi)return 1;if(*s!=',')break;s++;}return 0;}
static unsigned __stdcall td_sampler(void*unused){(void)unused;plat_diag_role("hitch-sampler");DWORD me=GetCurrentThreadId();
 const char*prefix=getenv("SWR_THREAD_TRACE");char path[1200];snprintf(path,sizeof path,"%s.samples.csv",prefix);FILE*f=fopen(path,"w");snprintf(path,sizeof path,"%s.waits.csv",prefix);FILE*w=fopen(path,"w");
 if(!f||!w){if(f)fclose(f);if(w)fclose(w);return 0;}
 setvbuf(f,NULL,_IOFBF,1<<20);setvbuf(w,NULL,_IOFBF,1<<20);
 int no_suspend=!getenv("SWR_THREAD_TRACE_RIP")||getenv("SWR_THREAD_TRACE_NO_SUSPEND")!=NULL;
 fprintf(f,"ns,frame,tid,role,cpu_ns,rip,module,base,rva,guest_tid,guest_entry,guest_pc,guest_lr,guest_wait_kind,guest_wait_addr,guest_wait_timeout,host_wait,host_wait_object,host_wait_caller,guest_wait_name,guest_wait_key\n");
 fprintf(w,"tid,begin_ns,end_ns,elapsed_ms,kind,object,caller,requested_ns\n");u64 enumeration=0,flush=0;extern int g_frames;
 for(;;){u64 begin=plat_ns();if(begin-enumeration>1000000000ull){td_enumerate();enumeration=begin;}
  int active=td_in_window(g_frames);LONG count=td_count;
  for(int i=0;i<count&&i<TD_MAX;i++){TDThread*r=&td_threads[i];if(!r->ready)continue;
   u32 tail=r->tail,head=__atomic_load_n(&r->head,__ATOMIC_ACQUIRE);while(tail!=head){TDWait e=r->events[tail&(TD_RING-1)];if(active)fprintf(w,"%lu,%llu,%llu,%.6f,%s,%llx,%llx,%lld\n",(unsigned long)r->tid,(unsigned long long)e.begin,(unsigned long long)e.end,(e.end-e.begin)/1e6,td_wait_name(e.kind),(unsigned long long)e.object,(unsigned long long)e.caller,(long long)e.requested);tail++;}__atomic_store_n(&r->tail,tail,__ATOMIC_RELEASE);
   if(!active||r->tid==me)continue;
   u64 cpu=td_cpu(r->handle),rip=0,guest_id=0,entry=0,pc=0,lr=0,addr=0,key=0;int wait=0;i64 timeout=0;char guest_name[64]={0};
   int host_wait=__atomic_load_n(&r->wait_kind,__ATOMIC_ACQUIRE);u64 object=r->wait_object,caller=r->wait_caller;
   if(!no_suspend&&SuspendThread(r->handle)!=(DWORD)-1){CONTEXT cx={0};cx.ContextFlags=CONTEXT_CONTROL;
    if(GetThreadContext(r->handle,&cx))rip=cx.Rip;
    KThread*t=r->guest;if(t){guest_id=t->id;entry=t->entry;pc=t->cpu.pc;lr=t->cpu.x[30];wait=t->wait_kind;addr=t->wait_addr;timeout=t->wait_timeout;key=t->wait_key;
     if(wait&&t->wait_name)for(int j=0;j<63&&t->wait_name[j];j++){char c=t->wait_name[j];guest_name[j]=c==','||c=='\n'||c=='\r'?'_':c;}}
    ResumeThread(r->handle);
   }else if(no_suspend){KThread*t=r->guest;if(t){guest_id=t->id;entry=t->entry;pc=t->cpu.pc;lr=t->cpu.x[30];wait=t->wait_kind;addr=t->wait_addr;timeout=t->wait_timeout;key=t->wait_key;
     if(wait&&t->wait_name)for(int j=0;j<63&&t->wait_name[j];j++){char c=t->wait_name[j];guest_name[j]=c==','||c=='\n'||c=='\r'?'_':c;}}}
   HMODULE module=NULL;char name[MAX_PATH]="unknown";
   if(rip&&GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)rip,&module))GetModuleFileNameA(module,name,sizeof name);
   char*shortname=strrchr(name,'\\');if(shortname)shortname++;else shortname=name;
   fprintf(f,"%llu,%d,%lu,%s,%llu,%llx,%s,%llx,%llx,%llx,%llx,%llx,%llx,%d,%llx,%lld,%s,%llx,%llx,%s,%llx\n",(unsigned long long)plat_ns(),g_frames,(unsigned long)r->tid,r->role,(unsigned long long)cpu,(unsigned long long)rip,shortname,(unsigned long long)(uintptr_t)module,(unsigned long long)(rip-(u64)(uintptr_t)module),(unsigned long long)guest_id,(unsigned long long)entry,(unsigned long long)pc,(unsigned long long)lr,wait,(unsigned long long)addr,(long long)timeout,td_wait_name(host_wait),(unsigned long long)object,(unsigned long long)caller,guest_name,(unsigned long long)key);
  }
  if(begin-flush>1000000000ull){fflush(f);fflush(w);flush=begin;}
  u64 elapsed=plat_ns()-begin;if(elapsed<10000000ull)Sleep((DWORD)((10000000ull-elapsed+999999)/1000000));else Sleep(1);
 }
 return 0;
}
void plat_thread_diag_start(void){plat_diag_role("frontend");const char*trace=getenv("SWR_THREAD_TRACE");if(!trace||!*trace)return;
 td_trace_enabled=1;uintptr_t h=_beginthreadex(NULL,0,td_sampler,NULL,0,NULL);if(h)CloseHandle((HANDLE)h);
}
