/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* AudioRenderer sample playback. Wire offsets follow the audren service ABI.
   Decoding and mixing run on the guest audio update thread; waveOut owns a
   bounded set of copied PCM buffers, never pointers into guest memory. */
#include "rt.h"
#include <math.h>
#ifdef _WIN32
#include <mmsystem.h>
#if !defined(__TINYC__)
#include <mmdeviceapi.h>   /* WASAPI: the output when waveOut has no device (GitHub issue #16) */
#include <audioclient.h>
#endif
#elif !defined(__ANDROID__)
#include <dlfcn.h>   /* SDL2 audio, loaded at run time */
#endif
#define AF 240
int g_opt_audio_51;   /* [Audio] audio_channels=5.1 (main.c): aoutput6 below, Windows */
#define AC 6
#define AM 64
#define AR 1024
#define AV 4096
static u32 au32(const u8*p){u32 v;memcpy(&v,p,4);return v;}
static i32 ai32(const u8*p){i32 v;memcpy(&v,p,4);return v;}
static u64 au64(const u8*p){u64 v;memcpy(&v,p,8);return v;}
static float af32(const u8*p){float v;memcpy(&v,p,4);return isfinite(v)?v:0;}
static i16 as16(const u8*p){i16 v;memcpy(&v,p,2);return v;}
static int avalid(u64 addr,u64 bytes){
  if(!addr || !bytes || addr>=AS_SIZE || bytes>AS_SIZE-addr)return 0;
  while(bytes){MemBlock b;mem_query(addr,&b);if(!(b.perm&PERM_R)||!b.size||addr<b.addr||addr-b.addr>=b.size)return 0;
    u64 n=b.size-(addr-b.addr);if(n>bytes)n=bytes;addr+=n;bytes-=n;}return 1;
}
typedef struct {float *pcm;u32 frames;u8 channels,loop,slot,eos;} ABuffer;
typedef struct {
  ABuffer q[8];int nq;double position,played_fraction;u64 played;u32 consumed;
  u8 submitted[4],was_playing,format_warned; i16 h1[AC],h2[AC];
  float filter[AC][2][4];
  float hist[AC][4]; float vol_prev; u8 vol_ok;   /* last 4 source samples before q[0] (resampler taps); volume of the last frame (ramp) */
} AVoice;
typedef struct {u8 valid,used,gprev_ok;u32 count,dest;float volume,matrix[24][24],gprev[24][24];} AMix;
/* I3DL2 reverb (effect type 5). Dread runs one in place on buffers 6-11 of its 24-buffer submix with a dry gain of
   0.25: without it that whole send bus played dry at 4x (+12 dB, ADAM's voice among it) and with no room sound.
   Own implementation of the I3DL2 model: room/HF input gain, a pre-delay with early-reflection taps, two diffusion
   all-passes and a 4-line feedback delay network with HF damping for the late reverb */
#define RV_PRE 32768
#define RV_LINE 4096
#define RV_AP 1024
typedef struct {
  float pre[RV_PRE],line[4][RV_LINE],ap[2][RV_AP],lp[4],inlp;u32 pos;
  u8 key[0x48];float room,hf,inlp_c,refl,rev,dry,diff,g[4],damp[4];u32 refl_d,rev_d,len[4],ap_len[2];
} AReverb;
struct AR3d;
typedef struct {u8 type,enabled,is_new;u32 mix;u8 param[0xA0];AReverb*rv;struct AR3d*r3;} AEffect;
#define AE 32
typedef struct {
  AVoice *voices;AMix mix[AM];u8 resources[AR][0x70];u8 resource_valid[AR];
  float *samples;u8 sink_inputs[AC];u32 sink_count;u64 ticks;int logged_voice,logged_routes;
  float downmix[4];AEffect effect[AE];u32 effect_count;
} ARenderer;
static void atrace_voice(const char*event,u32 id,AVoice*v,const u8*vi,const u8*wb){
  static int checked;static FILE*f;if(!checked){checked=1;const char*p=getenv("SWR_AUDIO_VOICE_CSV");if(p&&*p){f=fopen(p,"w");if(f)fprintf(f,"ns,event,id,played,consumed,queued,state,rate,format,address,bytes,start,end,loop,eos,context,context_bytes,first16\n");}}
  if(!f)return;
  char hex[33]={0};u64 addr=wb?au64(wb):0,bytes=wb?au64(wb+8):0;
  if(wb&&bytes>=16&&avalid(addr,16))for(int k=0;k<16;k++)sprintf(hex+k*2,"%02x",((u8*)GP(addr))[k]);
  fprintf(f,"%llu,%s,%u,%llu,%u,%d,%u,%u,%u,%llx,%llu,%d,%d,%u,%u,%llx,%llu,%s\n",(unsigned long long)plat_ns(),event,id,(unsigned long long)v->played,v->consumed,v->nq,vi[0xA],au32(vi+0xC),vi[0xB],(unsigned long long)addr,(unsigned long long)bytes,wb?ai32(wb+0x10):0,wb?ai32(wb+0x14):0,wb?wb[0x18]:0,wb?wb[0x19]:0,(unsigned long long)(wb?au64(wb+0x20):0),(unsigned long long)(wb?au64(wb+0x28):0),hex);fflush(f);
}
static void ahist_push(AVoice*v,const ABuffer*b){   /* keep the buffer's last samples for the next buffer's first taps */
  if(!b->pcm||!b->frames||!b->channels)return;u32 take=b->frames<4?b->frames:4;
  for(u32 ch=0;ch<AC;ch++){u32 c=ch<b->channels?ch:0;for(u32 k=0;k<4-take;k++)v->hist[ch][k]=v->hist[ch][k+take];
    for(u32 k=0;k<take;k++)v->hist[ch][4-take+k]=b->pcm[(size_t)(b->frames-take+k)*b->channels+c];}}
static void apop(AVoice*v){if(!v->nq)return;ahist_push(v,&v->q[0]);free(v->q[0].pcm);memmove(v->q,v->q+1,(v->nq-1)*sizeof(v->q[0]));v->nq--;v->consumed++;}
static void aclear(AVoice*v,int reset){while(v->nq)apop(v);v->position=0;v->played_fraction=0;if(reset){v->played=0;v->vol_ok=0;}memset(v->hist,0,sizeof v->hist);}
/* source sample k of channel ch, counted from the start of q[0]: earlier ones from hist, later ones from the queued
   buffers (a looping q[0] wraps). Past the end of what is queued: the last sample */
static float asrc_at(const AVoice*v,u32 ch,i64 k){
  if(k<0)return k>=-4?v->hist[ch][4+k]:v->hist[ch][0];
  float last=0;for(int i=0;i<v->nq;i++){const ABuffer*b=&v->q[i];if(!b->pcm||!b->frames)continue;u32 c=ch<b->channels?ch:0;
    if(i==0&&b->loop){k%=b->frames;return b->pcm[(size_t)k*b->channels+c];}
    if(k<b->frames)return b->pcm[(size_t)k*b->channels+c];
    last=b->pcm[(size_t)(b->frames-1)*b->channels+c];k-=b->frames;}
  return last;}
/* resampler: 8-tap Lanczos (a=4), 256 phases. Linear interpolation left images of 22 kHz voices (ADAM) above 11 kHz,
   heard as harsh distortion (MEASURED: -51.6 dB above 11 kHz while ADAM speaks vs -73 dB without). SWR_AUDIO_LINEAR=1 = old */
static float g_lz[257][8];static int g_lz_ok,g_lz_lin=-1;
static void alz_init(void){for(int ph=0;ph<=256;ph++){double f=ph/256.0,sum=0,w[8];
    for(int t=0;t<8;t++){double x=(t-3)-f;double s=fabs(x)<1e-9?1.0:sin(M_PI*x)/(M_PI*x),l=fabs(x)<1e-9?1.0:(fabs(x)<4?sin(M_PI*x/4)/(M_PI*x/4):0);w[t]=s*l;sum+=w[t];}
    for(int t=0;t<8;t++)g_lz[ph][t]=(float)(w[t]/sum);}g_lz_ok=1;}
/* DSP ADPCM: each 8-byte frame has one predictor/scale byte and 14 signed
   four-bit samples. Coefficients are signed Q11; histories are saturated PCM16. */
static i16 adpcm_sample(const u8*data,u32 sample,const i16*coef,i16*h1,i16*h2){
  u32 block=sample/14,sub=sample%14;u8 header=data[block*8];u32 predictor=header>>4;
  if(predictor>=8)return 0;
  u8 packed=data[block*8+1+sub/2];int nibble=(sub&1)?packed&15:packed>>4;if(nibble>=8)nibble-=16;
  i64 value=((i64)nibble*(1u<<(header&15))*2048+(i64)coef[predictor*2]*(*h1)+(i64)coef[predictor*2+1]*(*h2)+1024)>>11;
  if(value>32767)value=32767;if(value< -32768)value= -32768;*h2=*h1;*h1=(i16)value;return (i16)value;
}
static ABuffer adecode(AVoice*v,const u8*vi,const u8*wb,int slot){
  ABuffer b={0};u64 address=au64(wb),size=au64(wb+8);i32 start=ai32(wb+0x10),end=ai32(wb+0x14);
  u32 channels=au32(vi+0x18),format=vi[0xB];
  if(start<0||end<=start||end>8000000||!channels||channels>AC||!avalid(address,size))return b;
  u32 bytes=format==1?1:format==2?2:format==3?3:format==4||format==5?4:0;
  if(format==6){if(channels!=1||(u64)((end+13)/14)*8>size)return b;}
  else if(!bytes||(u64)end*channels*bytes>size)return b;
  b.frames=(u32)(end-start);b.channels=(u8)channels;b.loop=wb[0x18];b.eos=wb[0x19];b.slot=(u8)slot;
  b.pcm=malloc((size_t)b.frames*channels*sizeof(float));if(!b.pcm){b.frames=0;return b;}
  const u8*data=GP(address);
  if(format==6){i16 coef[16]={0};u64 ep=au64(vi+0x48),esz=au64(vi+0x50);
    if(esz<32||!avalid(ep,32)){free(b.pcm);b.pcm=NULL;b.frames=0;return b;}memcpy(coef,GP(ep),32);
    i16 h1=v->h1[0],h2=v->h2[0];u64 context=au64(wb+0x20),context_size=au64(wb+0x28);
    if(context_size>=6&&avalid(context,6)){h1=as16(GP(context)+2);h2=as16(GP(context)+4);}
    for(i32 n=0;n<end;n++){i16 sample=adpcm_sample(data,(u32)n,coef,&h1,&h2);if(n>=start)b.pcm[n-start]=sample/32768.0f;}
    v->h1[0]=h1;v->h2[0]=h2;
  }else for(u32 n=0;n<b.frames;n++)for(u32 ch=0;ch<channels;ch++){
    const u8*p=data+((size_t)n+start)*channels*bytes+ch*bytes;float sample=0;
    if(format==1)sample=(i8)*p/128.0f;
    else if(format==2)sample=as16(p)/32768.0f;
    else if(format==3){i32 a=(i32)((u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16));if(a&0x800000)a-=0x1000000;sample=a/8388608.0f;}
    else if(format==4)sample=(float)(ai32(p)/2147483648.0);
    else if(format==5)sample=af32(p);
    b.pcm[n*channels+ch]=sample;
  }
  return b;
}
static void abiquad_trace(const u8*p){   /* SWR_AUDIO_BIQUAD_TRACE=1: each distinct coefficient set once, with its DC gain both ways */
  static int on=-1;static u64 seen[64];static int nseen;if(on<0)on=getenv("SWR_AUDIO_BIQUAD_TRACE")!=NULL;if(!on||nseen>=64)return;
  u64 key=0;memcpy(&key,p+2,8);key^=(u64)as16(p+10)<<48;for(int i=0;i<nseen;i++)if(seen[i]==key)return;seen[nseen++]=key;
  float b0=as16(p+2)/16384.f,b1=as16(p+4)/16384.f,b2=as16(p+6)/16384.f,a1=as16(p+8)/16384.f,a2=as16(p+10)/16384.f;
  LOG("audio biquad: b %.4f %.4f %.4f  a %.4f %.4f  DC gain %.3f (feedback added)\n",b0,b1,b2,a1,a2,(b0+b1+b2)/(1-a1-a2));}
static float abiquad(AVoice*v,const u8*vi,u32 channel,float value){
  for(int k=0;k<2;k++){const u8*p=vi+0x24+k*12;if(!p[0])continue;float*h=v->filter[channel][k];abiquad_trace(p);
    /* the audio renderer stores the feedback coefficients already negated: y = b0 x + b1 x1 + b2 x2 + a1 y1 + a2 y2
       (as Ryujinx's BiquadFilterHelper runs it). Subtracting them turned the game's underwater low-pass unstable:
       the output overflowed, the guard below zeroed it, and Samus was silent underwater instead of muffled */
    float y=(as16(p+2)*value+as16(p+4)*h[0]+as16(p+6)*h[1]+as16(p+8)*h[2]+as16(p+10)*h[3])/16384.0f;
    if(!isfinite(y))y=0;h[1]=h[0];h[0]=value;h[3]=h[2];h[2]=y;value=y;}return value;
}
#ifndef SWR_AUDIO_TEST
#if defined(_WIN32) && !defined(__TINYC__)
/* WASAPI shared mode, the output when waveOut can't open one: waveOutOpen(WAVE_MAPPER) answered 2 (no device) on a
   laptop whose other programs played sound (GitHub issue #16). Pushed from aoutput, AF frames a call, at most
   100 ms queued: a chunk past that is dropped, as with waveOut's 32 buffers. Windows converts our 48 kHz stereo PCM16
   to the device's own format (AUTOCONVERTPCM). SWR_AUDIO_WASAPI=1 uses it first (tests) */
static const GUID swr_CLSID_MMDeviceEnumerator={0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID swr_IID_IMMDeviceEnumerator={0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID swr_IID_IAudioClient={0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const GUID swr_IID_IAudioRenderClient={0xF294ACFC,0x3146,0x4483,{0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2}};
static IAudioClient* g_wclient; static IAudioRenderClient* g_wrender; static UINT32 g_wframes;
static void wasapi_close(void){
  if(g_wclient) g_wclient->lpVtbl->Stop(g_wclient);
  if(g_wrender){ g_wrender->lpVtbl->Release(g_wrender); g_wrender=NULL; }
  if(g_wclient){ g_wclient->lpVtbl->Release(g_wclient); g_wclient=NULL; } }
static int wasapi_open(void){
  IMMDeviceEnumerator* en=NULL; IMMDevice* dev=NULL; HRESULT hr;
  CoInitializeEx(NULL,COINIT_MULTITHREADED);   /* this thread's; S_FALSE / RPC_E_CHANGED_MODE when it already was */
  hr=CoCreateInstance(&swr_CLSID_MMDeviceEnumerator,NULL,CLSCTX_ALL,&swr_IID_IMMDeviceEnumerator,(void**)&en);
  if(FAILED(hr)){ WARN("audio: WASAPI not available (%08lx)\n",(unsigned long)hr); return 0; }
  hr=en->lpVtbl->GetDefaultAudioEndpoint(en,eRender,eConsole,&dev); en->lpVtbl->Release(en);
  if(FAILED(hr)){ WARN("audio: WASAPI found no default output device (%08lx)\n",(unsigned long)hr); return 0; }
  hr=dev->lpVtbl->Activate(dev,&swr_IID_IAudioClient,CLSCTX_ALL,NULL,(void**)&g_wclient); dev->lpVtbl->Release(dev);
  if(FAILED(hr)){ WARN("audio: WASAPI could not use the output device (%08lx)\n",(unsigned long)hr); g_wclient=NULL; return 0; }
  WAVEFORMATEX f={0}; f.wFormatTag=WAVE_FORMAT_PCM; f.nChannels=2; f.nSamplesPerSec=48000; f.wBitsPerSample=16; f.nBlockAlign=4; f.nAvgBytesPerSec=192000;
  hr=g_wclient->lpVtbl->Initialize(g_wclient,AUDCLNT_SHAREMODE_SHARED,0x80000000u|0x08000000u /* AUTOCONVERTPCM, SRC_DEFAULT_QUALITY */,1000000 /* 100 ms */,0,&f,NULL);
  if(SUCCEEDED(hr)) hr=g_wclient->lpVtbl->GetBufferSize(g_wclient,&g_wframes);
  if(SUCCEEDED(hr)) hr=g_wclient->lpVtbl->GetService(g_wclient,&swr_IID_IAudioRenderClient,(void**)&g_wrender);
  if(SUCCEEDED(hr)) hr=g_wclient->lpVtbl->Start(g_wclient);
  if(FAILED(hr)){ WARN("audio: WASAPI output could not start (%08lx)\n",(unsigned long)hr); wasapi_close(); return 0; }
  LOG("audio: Windows output opened through WASAPI, stereo PCM16 at 48000 Hz (%u frames buffered at most)\n",(unsigned)g_wframes);
  return 1; }
/* one chunk: 1 written, 0 no room (dropped), -1 the device went away (unplugged, disabled): closed, to be opened again */
static int wasapi_write(const i16* pcm){
  UINT32 pad=0; BYTE* d=NULL; HRESULT hr=g_wclient->lpVtbl->GetCurrentPadding(g_wclient,&pad);
  if(SUCCEEDED(hr) && g_wframes-pad<AF) return 0;
  if(SUCCEEDED(hr)) hr=g_wrender->lpVtbl->GetBuffer(g_wrender,AF,&d);
  if(SUCCEEDED(hr)){ memcpy(d,pcm,AF*4); hr=g_wrender->lpVtbl->ReleaseBuffer(g_wrender,AF,0); }
  if(FAILED(hr)){ WARN("audio: WASAPI output lost (%08lx), trying again in a few seconds\n",(unsigned long)hr); wasapi_close(); return -1; }
  return 1; }
/* audio_channels=5.1 (GitHub issue #28): the game mixes into a six-channel device sink (FL FR C LFE RL RR) whatever
   the output; the stereo path plays FL/FR only. This sends all six to the default device through a second WASAPI shared
   client (WAVEFORMATEXTENSIBLE, KSAUDIO_SPEAKER_5POINT1, AUTOCONVERTPCM: Windows downmixes for a stereo device).
   Same chunking and 100 ms cap as the stereo WASAPI path. Returns 0 when it can't play (not 5.1, it would not open):
   the caller then uses the stereo output. Tried again every 3 s after a failure. */
static IAudioClient* g_w6client; static IAudioRenderClient* g_w6render; static UINT32 g_w6frames;
static void wasapi6_close(void){
  if(g_w6client) g_w6client->lpVtbl->Stop(g_w6client);
  if(g_w6render){ g_w6render->lpVtbl->Release(g_w6render); g_w6render=NULL; }
  if(g_w6client){ g_w6client->lpVtbl->Release(g_w6client); g_w6client=NULL; } }
static int wasapi6_open(void){
  IMMDeviceEnumerator* en=NULL; IMMDevice* dev=NULL; HRESULT hr;
  CoInitializeEx(NULL,COINIT_MULTITHREADED);
  hr=CoCreateInstance(&swr_CLSID_MMDeviceEnumerator,NULL,CLSCTX_ALL,&swr_IID_IMMDeviceEnumerator,(void**)&en);
  if(FAILED(hr)) return 0;
  hr=en->lpVtbl->GetDefaultAudioEndpoint(en,eRender,eConsole,&dev); en->lpVtbl->Release(en);
  if(FAILED(hr)) return 0;
  hr=dev->lpVtbl->Activate(dev,&swr_IID_IAudioClient,CLSCTX_ALL,NULL,(void**)&g_w6client); dev->lpVtbl->Release(dev);
  if(FAILED(hr)){ g_w6client=NULL; return 0; }
  static const GUID pcm_sub={0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};   /* KSDATAFORMAT_SUBTYPE_PCM */
  WAVEFORMATEXTENSIBLE f; memset(&f,0,sizeof f);
  f.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE; f.Format.nChannels=6; f.Format.nSamplesPerSec=48000; f.Format.wBitsPerSample=16;
  f.Format.nBlockAlign=12; f.Format.nAvgBytesPerSec=48000*12; f.Format.cbSize=22;
  f.Samples.wValidBitsPerSample=16; f.dwChannelMask=0x3F /* FL FR FC LFE BL BR */; f.SubFormat=pcm_sub;
  hr=g_w6client->lpVtbl->Initialize(g_w6client,AUDCLNT_SHAREMODE_SHARED,0x80000000u|0x08000000u,1000000,0,(WAVEFORMATEX*)&f,NULL);
  if(SUCCEEDED(hr)) hr=g_w6client->lpVtbl->GetBufferSize(g_w6client,&g_w6frames);
  if(SUCCEEDED(hr)) hr=g_w6client->lpVtbl->GetService(g_w6client,&swr_IID_IAudioRenderClient,(void**)&g_w6render);
  if(SUCCEEDED(hr)) hr=g_w6client->lpVtbl->Start(g_w6client);
  if(FAILED(hr)){ WARN("audio: 5.1 output could not start (%08lx): stereo instead\n",(unsigned long)hr); wasapi6_close(); return 0; }
  LOG("audio: 5.1 output opened through WASAPI, six channels PCM16 at 48000 Hz (%u frames buffered at most)\n",(unsigned)g_w6frames);
  return 1; }
static int aoutput6(const i16* pcm6){
  static int failed; static u64 retry_ns, writes, dropped; static SRWLOCK lock=SRWLOCK_INIT;
  if(!g_opt_audio_51 || getenv("SWR_MUTE_AUDIO")) return 0;
  AcquireSRWLockExclusive(&lock);
  if(!g_w6client && (!failed || plat_ns()-retry_ns>3000000000ull)){ retry_ns=plat_ns(); failed= !wasapi6_open(); }
  int ok=0;
  if(g_w6client){
    UINT32 pad=0; BYTE* d=NULL; HRESULT hr=g_w6client->lpVtbl->GetCurrentPadding(g_w6client,&pad);
    if(SUCCEEDED(hr) && g_w6frames-pad<AF){ dropped++; ok=1; }   /* full: this chunk is dropped, as on the stereo path */
    else{
      if(SUCCEEDED(hr)) hr=g_w6render->lpVtbl->GetBuffer(g_w6render,AF,&d);
      if(SUCCEEDED(hr)){ memcpy(d,pcm6,AF*12); hr=g_w6render->lpVtbl->ReleaseBuffer(g_w6render,AF,0); }
      if(FAILED(hr)){ WARN("audio: 5.1 output lost (%08lx), trying again in a few seconds\n",(unsigned long)hr); wasapi6_close(); failed=1; }
      else{ writes++; ok=1; if(writes%2000==0) LOG("audio: %llu 5.1 chunks queued, %llu overflow drops\n",(unsigned long long)writes,(unsigned long long)dropped); } } }
  ReleaseSRWLockExclusive(&lock);
  return ok; }
#endif
/* the output devices the old waveOut interface sees, in the log when it couldn't open one */
#ifdef _WIN32
static void waveout_devices(void){
  UINT n=waveOutGetNumDevs(); LOG("audio: waveOut sees %u output device(s)\n",n);
  for(UINT i=0;i<n && i<8;i++){ WAVEOUTCAPSA caps; if(waveOutGetDevCapsA(i,&caps,sizeof caps)==MMSYSERR_NOERROR) LOG("audio:   %u: %s, %u channel(s)\n",i,caps.szPname,caps.wChannels); } }
#endif
static void aoutput(const i16*pcm){
#ifdef _WIN32
  enum {NB=32};static HWAVEOUT device;static WAVEHDR headers[NB];static i16 samples[NB][AF*2];static u8 busy[NB];
  static int attempted;static u64 writes,dropped,retry_ns;static SRWLOCK lock=SRWLOCK_INIT;static int use_wasapi;
  static int trace_checked;static FILE*trace;static u64 previous_ns;
  if(!trace_checked){trace_checked=1;const char*path=getenv("SWR_TEST_AUDIO_STATS");if(path&&*path){trace=fopen(path,"w");if(trace)fprintf(trace,"ns,guest_frame,dt_ms,writes,drops,queued,clipped,silent,potential_empty\n");}}
  AcquireSRWLockExclusive(&lock);
  /* waveOut first, then WASAPI; with neither, tried again every 3 s (a headset that connects late, a device that comes
     back), and the log says so once */
  if(!device && !use_wasapi && (!attempted || plat_ns()-retry_ns>3000000000ull) && !getenv("SWR_MUTE_AUDIO")){
    int first=!attempted; attempted=1; retry_ns=plat_ns();
    MMRESULT result=MMSYSERR_ERROR;
    if(!getenv("SWR_AUDIO_WASAPI")){
      WAVEFORMATEX format={0};format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=2;format.nSamplesPerSec=48000;format.wBitsPerSample=16;format.nBlockAlign=4;format.nAvgBytesPerSec=192000;
      result=waveOutOpen(&device,WAVE_MAPPER,&format,0,0,CALLBACK_NULL);
      if(result!=MMSYSERR_NOERROR){device=NULL;if(first){WARN("audio: waveOutOpen failed (%u)\n",result);waveout_devices();}}
      else{for(int k=0;k<NB;k++){headers[k].lpData=(LPSTR)samples[k];headers[k].dwBufferLength=sizeof samples[k];headers[k].dwFlags=0;busy[k]=0;waveOutPrepareHeader(device,&headers[k],sizeof(WAVEHDR));}
        LOG("audio: Windows output opened, stereo PCM16 at 48000 Hz\n");}}
#if !defined(__TINYC__)
    if(!device && wasapi_open()) use_wasapi=1;
#endif
    if(!device && !use_wasapi && first) WARN("audio: no sound output could be opened; trying again every 3 seconds\n");}
#if !defined(__TINYC__)
  if(use_wasapi){ int w=wasapi_write(pcm); if(w>0) writes++; else if(w==0) dropped++; else use_wasapi=0;
    if(writes&&writes%2000==0&&w>0)LOG("audio: %llu chunks queued, %llu overflow drops\n",(unsigned long long)writes,(unsigned long long)dropped); }
#endif
  if(device){int slot=-1,queued=0;for(int k=0;k<NB;k++){if(busy[k]&&(headers[k].dwFlags&WHDR_DONE))busy[k]=0;if(!busy[k]&&slot<0)slot=k;if(busy[k])queued++;}
    int potential_empty=queued==0&&writes>0;
    if(slot>=0){memcpy(samples[slot],pcm,sizeof samples[slot]);MMRESULT result=waveOutWrite(device,&headers[slot],sizeof(WAVEHDR));if(result==MMSYSERR_NOERROR){busy[slot]=1;writes++;}else WARN("audio: waveOutWrite failed (%u)\n",result);}else dropped++;
    if(trace){u64 now=plat_ns();u32 clipped=0,silent=0;for(u32 n=0;n<AF*2;n++){if(pcm[n]==32767||pcm[n]==-32768)clipped++;if(pcm[n]==0)silent++;}
      fprintf(trace,"%llu,%d,%.6f,%llu,%llu,%d,%u,%u,%d\n",(unsigned long long)now,g_frames,previous_ns?(now-previous_ns)/1e6:0,(unsigned long long)writes,(unsigned long long)dropped,queued,clipped,silent,potential_empty);previous_ns=now;if(writes%200==0)fflush(trace);}
    if(writes&&writes%2000==0)LOG("audio: %llu chunks queued, %llu overflow drops\n",(unsigned long long)writes,(unsigned long long)dropped);
  }
  ReleaseSRWLockExclusive(&lock);
#elif !defined(__ANDROID__)
  /* Linux / Steam Deck: SDL2's queued audio (PipeWire, PulseAudio or ALSA, whichever the system has), loaded at run
     time like the controllers (backend_x11.c). Same format and the same cap as the Windows path: 32 chunks of
     5 ms queued at most, a chunk past that is dropped rather than letting latency grow. The queue has no reserve of
     its own: chunks arrive every 5 ms, PipeWire takes them in larger periods, and whatever it finds short it fills
     with silence. So at the start and whenever the queue ran dry it gets 40 ms of silence first, and from then on it
     rides out late chunks (SWR_AUDIO_RESERVE=N chunks, 0 = none) */
  typedef struct { int freq; u16 format; u8 channels, silence; u16 samples, padding; u32 size; void* callback; void* userdata; } SdlSpec;
  static int attempted, reserve=8; static u32 dev, minq=~0u; static u64 writes, dropped, dry, maxgap;
  static int (*init_sub)(u32); static u32 (*open_dev)(const char*,int,const SdlSpec*,SdlSpec*,int);
  static void (*pause_dev)(u32,int); static int (*queue)(u32,const void*,u32); static u32 (*queued)(u32);
  if(!attempted){ attempted=1;
    { const char* e=getenv("SWR_AUDIO_RESERVE"); if(e) reserve=atoi(e); if(reserve<0) reserve=0; if(reserve>24) reserve=24; }
    if(!getenv("SWR_MUTE_AUDIO")){
      void* h=dlopen("libSDL2-2.0.so.0",RTLD_NOW|RTLD_LOCAL); if(!h) h=dlopen("libSDL2.so",RTLD_NOW|RTLD_LOCAL);
      if(h){ *(void**)&init_sub=dlsym(h,"SDL_InitSubSystem"); *(void**)&open_dev=dlsym(h,"SDL_OpenAudioDevice"); *(void**)&pause_dev=dlsym(h,"SDL_PauseAudioDevice");
        *(void**)&queue=dlsym(h,"SDL_QueueAudio"); *(void**)&queued=dlsym(h,"SDL_GetQueuedAudioSize"); }
      if(init_sub && open_dev && pause_dev && queue && queued && init_sub(0x10)==0){   /* SDL_INIT_AUDIO */
        SdlSpec want={48000,0x8010,2,0,AF,0,0,NULL,NULL}, have;   /* AUDIO_S16LSB, stereo, one chunk per period */
        memset(&have,0,sizeof have);
        dev=open_dev(NULL,0,&want,&have,0);
        if(dev){ pause_dev(dev,0); LOG("audio: SDL2 output opened, stereo PCM16 at 48000 Hz\n"); }
        else WARN("audio: SDL2 could not open an output device\n"); }
      else WARN("audio: no sound (libSDL2-2.0.so.0 not found or its audio failed to start)\n"); } }
  if(dev){
    u32 q=queued(dev);
    { static u64 last; u64 now=plat_ns(); if(last && now-last>maxgap) maxgap=now-last; last=now; if(writes && q<minq) minq=q; }
    if(!q){ static const i16 quiet[AF*2]; if(writes) dry++; for(int k=0;k<reserve;k++) queue(dev,quiet,AF*4); }
    /* the game's chunks and the sound device run on different clocks: the Deck's device takes a little more than the game makes (MEASURED: the queue fell
       from 90 ms to empty in 60 s with chunks on time, a cut each time; the owner's save-room stutter). So the queue is held between 40 and 120 ms by one
       sample frame more or less in a chunk, 0.4% for as long as it takes, which isn't heard. SWR_AUDIO_RESERVE=0 = old */
    if(q < 32u*AF*4u){ u32 lo=40u*192u, hi=120u*192u, bytes=AF*4; static i16 st[(AF+1)*2];
      if(reserve && q<lo){ memcpy(st,pcm,AF*4); st[AF*2]=pcm[AF*2-2]; st[AF*2+1]=pcm[AF*2-1]; if(queue(dev,st,(AF+1)*4)==0) writes++; }
      else { if(reserve && q>hi) bytes=(AF-1)*4; if(queue(dev,pcm,bytes)==0) writes++; } } else dropped++;
    if(writes && writes%2000==0){ LOG("audio: %llu chunks queued, %llu overflow drops, ran dry %llu times; last 10 s: queue never under %.1f ms, longest gap between chunks %.1f ms\n",(unsigned long long)writes,(unsigned long long)dropped,(unsigned long long)dry,minq/192.0,maxgap/1e6); minq=~0u; maxgap=0; } }
#else
  (void)pcm;
#endif
}
#else
static void aoutput(const i16*pcm);
#endif
static void acapture(const i16*pcm){
  static int checked;static FILE*f;static u32 bytes;static u32 limit=48000*4*12;
  if(!checked){checked=1;const char*seconds=getenv("SWR_DUMP_AUDIO_SECONDS");if(seconds){int n=atoi(seconds);if(n>0&&n<=900)limit=48000*4*(u32)n;}const char*path=getenv("SWR_DUMP_AUDIO");if(path&&*path){f=fopen(path,"wb+");if(f){u8 header[44]={0};fwrite(header,1,44,f);}}}
  if(f&&bytes<limit){fwrite(pcm,1,AF*4,f);bytes+=AF*4;u8 h[44]={0};memcpy(h,"RIFF",4);u32 n=bytes+36;memcpy(h+4,&n,4);memcpy(h+8,"WAVEfmt ",8);n=16;memcpy(h+16,&n,4);h[20]=1;h[22]=2;n=48000;memcpy(h+24,&n,4);n=192000;memcpy(h+28,&n,4);h[32]=4;h[34]=16;memcpy(h+36,"data",4);memcpy(h+40,&bytes,4);fseek(f,0,SEEK_SET);fwrite(h,1,44,f);fseek(f,0,SEEK_END);fflush(f);if(bytes>=limit){fclose(f);f=NULL;}}
}
static void aroutes(ARenderer*r,const u32*ih,const u8*input,u64 size){
  u64 off=64ull+ih[1]+ih[2];if(off+ih[4]>size)return;
  for(u32 i=0;i<ih[4]/0x70;i++){const u8*p=input+off+i*0x70;u32 id=au32(p);if(id<AR){memcpy(r->resources[id],p,0x70);r->resource_valid[id]=1;}}
  {u64 eo=off+ih[4]+(u64)ih[3];u32 n=ih[5]/0xC0;if(n>AE)n=AE;if(eo+(u64)n*0xC0<=size){r->effect_count=n;
    for(u32 i=0;i<n;i++){const u8*e=input+eo+i*0xC0;AEffect*f=&r->effect[i];if(f->type!=e[0]||e[1]){if(f->rv)memset(f->rv,0,sizeof *f->rv);}
      f->type=e[0];f->is_new=e[1];f->enabled=e[2];f->mix=au32(e+4);memcpy(f->param,e+0x20,0xA0);}}}
  off+=ih[4]+(u64)ih[3]+ih[5]+ih[9];if(off+ih[6]>size)return;
  u32 count=ih[6]/0x930;const u8*p=input+off;
  if(ih[6]>=0x20&&(ih[6]-0x20)%0x930==0){count=au32(p+4);if(count>(ih[6]-0x20)/0x930)return;p+=0x20;}
  for(u32 i=0;i<count;i++,p+=0x930){u32 id=au32(p+0x10);if(id>=AM)continue;AMix*m=&r->mix[id];m->valid=1;m->used=p[0xC];m->count=au32(p+8);if(m->count>24)m->count=24;m->volume=af32(p);m->dest=au32(p+0x924);memcpy(m->matrix,p+0x24,sizeof m->matrix);}
  off+=ih[6];if(off+ih[7]>size)return;
  for(u32 i=0;i<ih[7]/0x140;i++){p=input+off+i*0x140;if(p[0]==1&&p[1]){u32 count=au32(p+0x120);if(count>AC)count=AC;r->sink_count=count;memcpy(r->sink_inputs,p+0x124,count);
    const float defaults[4]={1.f,.707f,.251f,.707f};
    for(int k=0;k<4;k++)r->downmix[k]=p[0x12B]?af32(p+0x12C+k*4):defaults[k];}}
}
/* Device sink channel order: FL, FR, center, LFE, rear L, rear R.
   Coefficients come from the audren device-sink ABI (or standard defaults).
   Keep source buffers untouched; other sinks/mixes may still reference them. */
static float astereo_sample(const ARenderer*r,int n,int ch,float volume){
  extern int g_opt_audio_surround_downmix;
  if(g_opt_audio_surround_downmix&&r->sink_count==6){
    const int inputs[4]={ch,2,3,4+ch};float sum=0;
    for(int k=0;k<4;k++){u32 source=r->sink_inputs[inputs[k]];
      if(source<24)sum+=r->samples[(size_t)source*AF+n]*r->downmix[k];}
    return sum*volume;
  }
  u32 source=r->sink_count?r->sink_inputs[ch<r->sink_count?ch:0]:(u32)ch;
  if(source>=24)source=ch;
  return r->samples[(size_t)source*AF+n]*volume;
}
static float amb(float mb){return powf(10.f,mb/2000.f);}   /* millibels to linear gain */
static void areverb_setup(AReverb*v,const u8*q){
  if(!memcmp(v->key,q,0x48))return;memcpy(v->key,q,0x48);
  const float fs=48000.f;float room_hf=af32(q+0x18),ref_hf=af32(q+0x1C),decay=af32(q+0x20),hf_ratio=af32(q+0x24);
  float refl_delay=af32(q+0x38),rev_delay=af32(q+0x3C),diffusion=af32(q+0x34),density=af32(q+0x40);
  if(!(ref_hf>20.f&&ref_hf<20000.f))ref_hf=5000.f;if(!(decay>=0.1f&&decay<=20.f))decay=decay<0.1f?0.1f:20.f;
  if(!(hf_ratio>=0.1f&&hf_ratio<=2.f))hf_ratio=hf_ratio<0.1f?0.1f:2.f;
  if(!(refl_delay>=0&&refl_delay<=0.3f))refl_delay=refl_delay<0?0:0.3f;if(!(rev_delay>=0&&rev_delay<=0.1f))rev_delay=rev_delay<0?0:0.1f;
  if(!(diffusion>=0&&diffusion<=100.f))diffusion=100.f;if(!(density>=0&&density<=100.f))density=100.f;
  v->room=amb(af32(q+0x28));v->hf=amb(room_hf);if(v->hf>1)v->hf=1;v->refl=amb(af32(q+0x2C));v->rev=amb(af32(q+0x30));
  v->dry=af32(q+0x44);if(!(v->dry>=0&&v->dry<=1))v->dry=v->dry>1?1:0;
  float w=2.f*3.14159265f*ref_hf/fs;v->inlp_c=1.f-expf(-w);   /* one-pole split at the reference frequency */
  v->refl_d=(u32)(refl_delay*fs);v->rev_d=v->refl_d+(u32)(rev_delay*fs);
  static const u32 base[4]={1433,1601,1867,2053};float scale=0.6f+0.4f*density/100.f;
  for(int k=0;k<4;k++){v->len[k]=(u32)(base[k]*scale);
    float t=(float)v->len[k]/fs;v->g[k]=powf(10.f,-3.f*t/decay);
    /* HF damping: a one-pole low-pass whose gain at the reference frequency gives the HF decay time decay*hf_ratio */
    float ghf=powf(10.f,-3.f*t/(decay*(hf_ratio<1.f?hf_ratio:1.f))),want=ghf/v->g[k],a=0;
    if(want<0.999f){float lo=0,hi=0.999f;for(int it=0;it<40;it++){float m=(lo+hi)*0.5f,mag=(1-m)/sqrtf(1-2*m*cosf(w)+m*m);if(mag>want)lo=m;else hi=m;}a=(lo+hi)*0.5f;}
    v->damp[k]=a;}
  v->diff=0.6f*diffusion/100.f;v->ap_len[0]=(u32)(142*scale)+1;v->ap_len[1]=(u32)(379*scale)+1;
}
static void areverb(AReverb*v,float*const*io,int nch,int lfe){
  static const u32 taps[4]={0,211,523,877};static const float tapg[4]={0.5f,0.4f,0.3f,0.25f};
  for(int n=0;n<AF;n++){float x=0;int used=0;for(int c=0;c<nch;c++)if(io[c]&&c!=lfe){x+=io[c][n];used++;}if(used)x/=(float)used;
    v->inlp+=(x-v->inlp)*v->inlp_c;x=(v->inlp+(x-v->inlp)*v->hf)*v->room;
    u32 p=v->pos;v->pre[p&(RV_PRE-1)]=x;
    float early=0;for(int t=0;t<4;t++)early+=v->pre[(p-v->refl_d-taps[t])&(RV_PRE-1)]*tapg[t];early*=v->refl;
    float u=v->pre[(p-v->rev_d)&(RV_PRE-1)];
    for(int a=0;a<2;a++){float d=v->ap[a][(p-v->ap_len[a])&(RV_AP-1)],y=d-v->diff*u;v->ap[a][p&(RV_AP-1)]=u+v->diff*y;u=y;}
    float o[4],sum=0;for(int k=0;k<4;k++){o[k]=v->line[k][(p-v->len[k])&(RV_LINE-1)];v->lp[k]=o[k]+(v->lp[k]-o[k])*v->damp[k];o[k]=v->lp[k];sum+=o[k];}
    for(int k=0;k<4;k++){float f=(o[k]-0.5f*sum)*v->g[k]+u;if(!isfinite(f))f=0;v->line[k][p&(RV_LINE-1)]=f;}   /* Householder feedback */
    float late=v->rev*0.5f,wl=early+(o[0]+o[2])*late,wr=early+(o[1]+o[3])*late;v->pos=p+1;
    float wet[6]={wl,wr,0.5f*(wl+wr),0,early+(o[0]-o[3])*late,early+(o[1]-o[2])*late};
    if(nch==1)wet[0]=0.5f*(wl+wr);
    for(int c=0;c<nch;c++)if(io[c])io[c][n]=io[c][n]*v->dry+(c==lfe?0:wet[c<6?c:0]);}
}
/* effects of one mix, run after its voices and child mixes are summed and before it is routed on. Only I3DL2 reverb
   is modelled; a disabled in-place effect is a bypass, as on the console */
/* I3DL2 reverb as the console's audio DSP runs it: a C port of Ryujinx's Reverb3dCommand / Reverb3dState
   (MIT License, Copyright (c) Ryujinx Team and Contributors; see licenses). Dread runs it in place on the dialogue bus
   (buffers 6-11 of submix 1) with a dry gain of 0.25, so ADAM's voice is mostly this effect's wet signal. The earlier
   approximation (4 early taps, input averaged over the channels) made that wet signal much quieter and duller than the
   console's (20 early taps, input summed): ADAM was buried under the background (#34) and sounded off (#36).
   SWR_AUDIO_OLD_REVERB=1 = the approximation */
typedef struct {float*b;u32 max,cnt,cur,last;float sr;} R3Line;
static void r3_init(R3Line*l,float srk,float tmax){l->sr=srk;l->max=(u32)lroundf(srk*tmax);l->b=calloc(l->max+1,sizeof(float));l->cur=0;l->last=0;l->cnt=0;}
static void r3_cfg(R3Line*l,u32 n){if(l->max>=n){l->cnt=n;l->last=(l->cur+n)%(l->max+1);}}
static void r3_set(R3Line*l,float t){r3_cfg(l,(u32)lroundf(l->sr*t));}
static float r3_read(const R3Line*l){return l->b?l->b[l->cur]:0;}
static float r3_upd(R3Line*l,float v){if(!l->b)return 0;if(!isfinite(v))v=0;l->b[l->last++]=v;float o=l->b[l->cur];l->cur++;if(l->cur>=l->max)l->cur=0;if(l->last>=l->max)l->last=0;return o;}
static float r3_tap(const R3Line*l,u32 idx,int off){if(!l->b)return 0;int t=(int)l->last-((int)idx+off);if(t<0)t+=(int)l->max+1;if(t<0||t>(int)l->max)return 0;return l->b[t];}
typedef struct {R3Line l;float rate;} R3Decay;
static float r3d_upd(R3Decay*d,float v){float x=r3_read(&d->l),p=v-d->rate*x;return r3_upd(&d->l,p)+p*d->rate;}
typedef struct AR3d {
  u8 key[0x48];int ok;
  R3Line fdn[4],pre,fc;R3Decay d1[4],d2[4];
  float dry,prev_pre,prev_pre_gain,target_pre_gain,early_gain,late_gain,decay_direct[4],decay_cur_fdn[4],decay_cur_out[4],prev_fb[4];
  u32 early_t[20],refl_t;
} AR3d;
static float r3_pow10(float x){if(x>=0)return 1;if(x<=-5.3f)return 0;return powf(10.f,x);}
static float r3_cosd(float d){return cosf(d*3.14159265f/180.f);}
static float r3_sind(float d){return sinf(d*3.14159265f/180.f);}
static const float r3_fdn_min[4]={5.0f,6.0f,13.0f,14.0f},r3_fdn_max[4]={45.704f,82.782f,149.94f,271.58f},
  r3_dd1[4]={17.0f,13.0f,9.0f,7.0f},r3_dd2[4]={19.0f,11.0f,10.0f,6.0f},
  r3_early_t[20]={0.017136f,0.059154f,0.16173f,0.39019f,0.42526f,0.45541f,0.68974f,0.74591f,0.83384f,0.8595f,0.0f,0.075024f,0.16879f,0.2999f,0.33744f,0.3719f,0.59901f,0.71674f,0.81786f,0.85166f},
  r3_early_g[20]={0.67096f,0.61027f,1.0f,0.35680f,0.68361f,0.65978f,0.51939f,0.24712f,0.45945f,0.45021f,0.64196f,0.54879f,0.92925f,0.38270f,0.72867f,0.69794f,0.5464f,0.24563f,0.45214f,0.44042f};
static void ar3_param(AR3d*s,const u8*q){
  float sr=(float)au32(q+0x14);if(!(sr>=8000&&sr<=192000))sr=48000;float srk=(float)(au32(q+0x14)/1000);if(!(srk>=8))srk=48;
  float room_hf=af32(q+0x18),hf_ref=af32(q+0x1C),decay=af32(q+0x20),hf_ratio=af32(q+0x24),room=af32(q+0x28),refl=af32(q+0x2C),rev=af32(q+0x30),
    diffusion=af32(q+0x34),refl_delay=af32(q+0x38),rev_delay=af32(q+0x3C),density=af32(q+0x40);
  if(!(decay>0))decay=0.1f;if(!(hf_ratio>0))hf_ratio=0.1f;
  if(!s->ok){for(int i=0;i<4;i++){r3_init(&s->fdn[i],srk,r3_fdn_max[i]);r3_set(&s->fdn[i],r3_fdn_max[i]);r3_init(&s->d1[i].l,srk,r3_dd1[i]);r3_set(&s->d1[i].l,r3_dd1[i]);
      r3_init(&s->d2[i].l,srk,r3_dd2[i]);r3_set(&s->d2[i].l,r3_dd2[i]);}
    r3_init(&s->pre,srk,400);r3_set(&s->pre,400);r3_init(&s->fc,srk,5);r3_set(&s->fc,5);s->ok=1;}
  s->dry=af32(q+0x44);if(!isfinite(s->dry))s->dry=0;
  memset(s->prev_fb,0,sizeof s->prev_fb);s->prev_pre=0;
  s->early_gain=r3_pow10(fminf(room+refl,5000.f)/2000.f);s->late_gain=r3_pow10(fminf(room+rev,5000.f)/2000.f);
  float hfg=r3_pow10(room_hf/2000.f);
  if(hfg<1.f){float a=1.f-hfg,b=2.f-((2.f*hfg)*r3_cosd(256.f*hf_ref/sr)),c=sqrtf(fmaxf(b*b-4.f*(1.f-hfg)*(1.f-hfg),0.f));s->prev_pre_gain=(b-c)/(2.f*a);s->target_pre_gain=1.f-s->prev_pre_gain;}
  else{s->prev_pre_gain=0;s->target_pre_gain=1;}
  s->refl_t=(u32)lroundf(srk*(1000.f*(refl_delay+rev_delay)));
  float echo=0.6f*diffusion*0.01f;
  for(int i=0;i<4;i++){r3_set(&s->fdn[i],r3_fdn_min[i]+(density/100.f*(r3_fdn_max[i]-r3_fdn_min[i])));
    u32 n=s->fdn[i].cnt+s->d1[i].l.cnt+s->d2[i].l.cnt;
    float ta=(-60.f*(float)n)/(decay*sr),tb=ta/hf_ratio,tc=r3_cosd(128.f*0.5f*hf_ref/sr)/r3_sind(128.f*0.5f*hf_ref/sr),td=r3_pow10((tb-ta)/40.f),te=r3_pow10((tb+ta)/40.f)*0.7071f;
    s->decay_direct[i]=te*((td*tc)+1.f)/(tc+td);s->decay_cur_fdn[i]=te*(1.f-(td*tc))/(tc+td);s->decay_cur_out[i]=(tc-td)/(tc+td);
    s->d1[i].rate=echo;s->d2[i].rate=echo*-0.9f;}
  for(int i=0;i<20;i++){u32 n=(u32)lroundf(srk*((refl_delay*1000.f)+(r3_early_t[i]*1000.f*((rev_delay*0.9998f)+0.02f))));if(n>s->pre.max)n=s->pre.max;s->early_t[i]=n;}
}
static void ar3_run(AR3d*s,float*const*in,float*const*out,int nch){
  static const int oe_mono[20]={0},oe_st[20]={0,0,0,1,1,1,1,0,0,0,1,1,1,0,0,0,0,1,1,1},oe_quad[20]={0,0,0,1,1,1,1,2,2,2,1,1,1,0,0,0,0,3,3,3},
    oe_sur[40]={4,5,0,5,0,5,1,5,1,5,1,5,1,5,2,5,2,5,2,5,1,5,1,5,1,5,0,5,0,5,0,5,0,5,3,5,3,5,3,5},
    te_sur[40]={0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18,19,19},
    fb_mono[1]={0},fb_st[2]={0,1},fb_quad[4]={0,1,2,3},fb_sur[6]={0,1,2,3,-1,3};
  const int*oe,*fbt;int ne,nfb;static const int te_id[20]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19};const int*te=te_id;
  if(nch==1){oe=oe_mono;ne=20;fbt=fb_mono;nfb=1;}else if(nch==2){oe=oe_st;ne=20;fbt=fb_st;nfb=2;}else if(nch==4){oe=oe_quad;ne=20;fbt=fb_quad;nfb=4;}
  else if(nch==6){oe=oe_sur;te=te_sur;ne=40;fbt=fb_sur;nfb=6;}else return;
  for(int n=0;n<AF;n++){float ov[6]={0},ci[6]={0},fbo[4],fb[4],val[4];
    float tap=r3_tap(&s->pre,s->refl_t,1);
    for(int i=0;i<ne;i++){int ei=te[i];int oi=oe[ei];ov[oi]+=r3_tap(&s->pre,s->early_t[ei],1)*r3_early_g[ei];}
    float pv=0;for(int c=0;c<nch;c++){ci[c]=in[c]?in[c][n]:0;pv+=ci[c];}
    for(int c=0;c<nch;c++)ov[c]*=s->early_gain;
    s->prev_pre=(pv*s->target_pre_gain)+(s->prev_pre*s->prev_pre_gain);if(!isfinite(s->prev_pre))s->prev_pre=0;
    r3_upd(&s->pre,s->prev_pre);
    for(int i=0;i<4;i++){float f=r3_read(&s->fdn[i]);float o=f*s->decay_direct[i]+s->prev_fb[i];s->prev_fb[i]=(f*s->decay_cur_fdn[i])+(o*s->decay_cur_out[i]);if(!isfinite(s->prev_fb[i]))s->prev_fb[i]=0;fbo[i]=o;}
    fb[0]=fbo[2]+fbo[1];fb[1]=-fbo[0]-fbo[3];fb[2]=fbo[0]-fbo[3];fb[3]=fbo[1]-fbo[2];
    for(int i=0;i<4;i++){float t=r3d_upd(&s->d1[i],tap*s->late_gain+fb[i]);val[i]=r3d_upd(&s->d2[i],t);r3_upd(&s->fdn[i],val[i]);}
    for(int c=0;c<nfb;c++){int t=fbt[c];if(t>=0&&out[c])out[c][n]=ov[c]+val[t]+ci[c]*s->dry;}
    if(nch==1&&out[0])out[0][n]+=val[1];
    if(nch==6&&out[4])out[4][n]+=(ov[4]+r3_upd(&s->fc,(val[2]-val[3])*0.5f)+ci[4]*s->dry);}
}
static void aeffects_run(ARenderer*r,u32 mix){
  for(u32 i=0;i<r->effect_count;i++){AEffect*f=&r->effect[i];if(f->mix!=mix||!f->enabled)continue;
    if(f->type==5){const u8*q=f->param;int nch=au32(q+0xE)&0xFFFF;if(nch<1||nch>6)continue;
      static int oldrv=-1;if(oldrv<0)oldrv=getenv("SWR_AUDIO_OLD_REVERB")!=NULL;
      float*io[6]={0};
      for(int c=0;c<nch;c++){int in=(signed char)q[c],out=(signed char)q[6+c];if(in<0||in>=24||out<0||out>=24)continue;
        float*src=r->samples+((size_t)mix*24+in)*AF,*dst=r->samples+((size_t)mix*24+out)*AF;if(dst!=src)memcpy(dst,src,AF*sizeof(float));io[c]=dst;}
      if(oldrv){if(!f->rv){f->rv=calloc(1,sizeof *f->rv);if(!f->rv)continue;}areverb_setup(f->rv,q);areverb(f->rv,io,nch,nch==6?3:-1);}
      else{if(nch!=1&&nch!=2&&nch!=4&&nch!=6)continue;if(!f->r3){f->r3=calloc(1,sizeof *f->r3);if(!f->r3)continue;}
        if(!f->r3->ok||memcmp(f->r3->key,q,0x48)){memcpy(f->r3->key,q,0x48);ar3_param(f->r3,q);}
        ar3_run(f->r3,io,io,nch);}}
    else{static u32 warned;if(f->type<32&&!(warned&(1u<<f->type))){warned|=1u<<f->type;LOG("audio: effect type %u is not modelled (mix %u)\n",f->type,mix);}}}
}
static void agroups(ARenderer*r,double*e){for(int g=0;g<4;g++)for(int c=0;c<6;c++){const float*x=r->samples+((size_t)24+g*6+c)*AF;for(int n=0;n<AF;n++)e[g]+=(double)x[n]*x[n];}}
static void aeffects(ARenderer*r,u32 mix){
  static int on=-1;static double e[2][4];static u32 ticks;if(on<0)on=getenv("SWR_AUDIO_GROUP_TRACE")!=NULL;   /* RMS per 6-buffer group of submix 1, before/after its effects */
  if(!on||mix!=1){aeffects_run(r,mix);return;}
  agroups(r,e[0]);aeffects_run(r,mix);agroups(r,e[1]);
  if(++ticks%200==0){LOG("audio groups: before %.4f %.4f %.4f %.4f  after %.4f %.4f %.4f %.4f\n",sqrt(e[0][0]/48000),sqrt(e[0][1]/48000),sqrt(e[0][2]/48000),sqrt(e[0][3]/48000),sqrt(e[1][0]/48000),sqrt(e[1][1]/48000),sqrt(e[1][2]/48000),sqrt(e[1][3]/48000));memset(e,0,sizeof e);}
}
/* SWR_AUDIO_ROUTE_TRACE=1: once a second, when it changed, log the effects, splitters, mixes and which mix/splitter
   the playing voices use (what the mixer below does and does not model) */
static void aroute_trace(ARenderer*r,const u32*ih,const u8*input,u64 size,const u8*voices,u32 count){
  static int on=-1;static u64 last;if(on<0)on=getenv("SWR_AUDIO_ROUTE_TRACE")!=NULL;if(!on||r->ticks%200)return;
  u64 off=64ull+ih[1]+ih[2]+ih[4]+ih[3];if(off+ih[5]+ih[9]>size)return;
  char line[4096];int n=0;u64 sig=1469598103934665603ull;
#define AT_ADD(...) do{int k_=snprintf(line+n,sizeof line-n,__VA_ARGS__);if(k_>0&&n+k_<(int)sizeof line)n+=k_;}while(0)
  for(u32 i=0;i<ih[5]/0xC0;i++){const u8*e=input+off+i*0xC0;if(!e[0])continue;AT_ADD(" E%u:t%u en%u mix%u",i,e[0],e[2],au32(e+4));
    if(e[0]==1){u32 c=au32(e+0x20+48+96);if(c>24)c=24;AT_ADD("[");for(u32 k=0;k<c;k++)AT_ADD("%d>%d*%.2f ",(signed char)e[0x20+k],(signed char)e[0x20+24+k],af32(e+0x20+48+k*4));AT_ADD("]");}
    if(e[0]==5){const u8*q=e+0x20;AT_ADD("[in");for(int k=0;k<6;k++)AT_ADD(" %d",(signed char)q[k]);AT_ADD(" out");for(int k=0;k<6;k++)AT_ADD(" %d",(signed char)q[6+k]);
      AT_ADD(" ch%u room%.0f refl%.0f rev%.0f dry%.2f st%u]",au32(q+0xE)&0xFFFF,af32(q+0x28),af32(q+0x2C),af32(q+0x30),af32(q+0x44),q[0x48]);}
    if(e[0]==2){u32 c=au32(e+0x20+48);if(c>24)c=24;AT_ADD("[");for(u32 k=0;k<c;k++)AT_ADD("%d>%d ",(signed char)e[0x20+k],(signed char)e[0x20+24+k]);AT_ADD("]");}}
  off+=ih[5];
  if(ih[9]>=0x20){const u8*h=input+off;AT_ADD(" | split %.4s info %u data %u",(const char*)h,au32(h+4),au32(h+8));}
  AT_ADD(" | mixes");for(u32 id=0;id<AM;id++){AMix*m=&r->mix[id];if(!m->valid||!m->used)continue;AT_ADD(" M%u>%d v%.2f c%u",id,(int)m->dest,m->volume,m->count);
    if(id){AT_ADD("{");for(u32 a=0;a<m->count;a++)for(u32 b=0;b<24;b++)if(m->matrix[a][b])AT_ADD("%u>%u:%.2f ",a,b,m->matrix[a][b]);AT_ADD("}");}}
  u32 hist[8][3];int nh=0;
  for(u32 i=0;i<count;i++){const u8*vi=voices+i*0x170;if(!vi[9]||vi[0xA]!=0)continue;u32 mix=au32(vi+0x58),sp=au32(vi+0x5C);int f=-1;
    for(int k=0;k<nh;k++)if(hist[k][0]==mix&&hist[k][1]==sp)f=k;if(f<0&&nh<8){f=nh++;hist[f][0]=mix;hist[f][1]=sp;hist[f][2]=0;}if(f>=0)hist[f][2]++;}
  for(u32 i=0;i<count;i++){const u8*vi=voices+i*0x170;if(!vi[9]||vi[0xA]!=0)continue;for(int w=0;w<4;w++){const u8*wb=vi+0x60+w*0x38;
    if(au64(wb)&&wb[0x18])AT_ADD(" L%u:%d,%d cnt%d ls%u le%u",au32(vi)&(AV-1),ai32(wb+0x10),ai32(wb+0x14),ai32(wb+0x1C),au32(wb+0x30),au32(wb+0x34));}}
  AT_ADD(" | voices");for(int k=0;k<nh;k++)AT_ADD(" mix%d/split%d x%u",(int)hist[k][0],(int)hist[k][1],hist[k][2]);
#undef AT_ADD
  for(int k=0;k<n;k++)sig=(sig^(u8)line[k])*1099511628211ull;
  if(sig!=last){last=sig;LOG("audio route:%s\n",line);}
}
extern int g_opt_audio_eos_sample_count;
static void audio_eos_env(void){ static int done; if(!done){ done=1; if(getenv("SWR_AUDIO_OLD_EOS")) g_opt_audio_eos_sample_count=0; } }   /* tests only */
void audio_render(SvcObj*o,const u32*ih,const u8*input,u64 size,const u8*voices,u32 count,u8*status){
  audio_eos_env();
  if(!o->u[4]){ARenderer*r=calloc(1,sizeof *r);if(!r)return;r->voices=calloc(AV,sizeof(AVoice));r->samples=calloc(AM*24*AF,sizeof(float));if(!r->voices||!r->samples){free(r->voices);free(r->samples);free(r);return;}o->u[4]=(u64)(size_t)r;}
  ARenderer*r=(ARenderer*)(size_t)o->u[4];aroutes(r,ih,input,size);aroute_trace(r,ih,input,size,voices,count);memset(r->samples,0,AM*24*AF*sizeof(float));u32 active=0;
  for(u32 i=0;i<count;i++){const u8*vi=voices+i*0x170;u32 id=au32(vi)&(AV-1);AVoice*v=&r->voices[id];u8 in_use=vi[9],play=vi[0xA];
    if(vi[8]){atrace_voice("new",id,v,vi,NULL);aclear(v,1);memset(v,0,sizeof *v);}
    if(!in_use){if(v->nq){atrace_voice("unused",id,v,vi,NULL);aclear(v,1);}v->was_playing=0;}
    else{
      u32 head=au32(vi+0x40)&3;
      for(u32 x=0;x<4;x++){int slot=(head+x)&3;const u8*wb=vi+0x60+slot*0x38;
        if(wb[0x1A])v->submitted[slot]=0;
        else if(au64(wb)&&au64(wb+8)&&!v->submitted[slot]){v->submitted[slot]=1;atrace_voice("submit",id,v,vi,wb);ABuffer b=adecode(v,vi,wb,slot);
          if(b.pcm&&v->nq<8)v->q[v->nq++]=b;else{free(b.pcm);v->consumed++;if(!v->format_warned){v->format_warned=1;WARN("audio: voice %u buffer rejected, format=%u channels=%u start=%d end=%d bytes=%llu\n",id,vi[0xB],au32(vi+0x18),ai32(wb+0x10),ai32(wb+0x14),(unsigned long long)au64(wb+8));}}}}
      for(u8 n=vi[0x159];n&&v->nq;n--){apop(v);v->position=0;}
      if(play==1&&v->was_playing){atrace_voice("stop",id,v,vi,NULL);aclear(v,1);memset(v->filter,0,sizeof v->filter);}
      v->was_playing=play!=1;
      if(play==0&&v->nq&&o->u[3]==0){active++;u32 rate=au32(vi+0xC);float pitch=af32(vi+0x1C),volume=af32(vi+0x20);if(!rate||rate>192000)rate=48000;if(!(pitch>0)||pitch>8)pitch=1;
        double step=(double)rate*pitch/48000.0;u32 mix=au32(vi+0x58);if(mix>=AM)mix=0;u32 nc=r->mix[mix].valid?r->mix[mix].count:2;if(nc>24)nc=24;
        if(!r->logged_voice){r->logged_voice=1;LOG("audio: first voice %u, format %u, %u channels, %u Hz, volume %.3f, mix %u, %u buffers\n",id,vi[0xB],au32(vi+0x18),rate,volume,mix,v->nq);}
        if(!g_lz_ok)alz_init();if(g_lz_lin<0)g_lz_lin=getenv("SWR_AUDIO_LINEAR")!=NULL;
        float vol0=v->vol_ok?v->vol_prev:volume;   /* the console ramps a voice's volume across the frame; a jump clicks */
        for(int n=0;n<AF&&v->nq;n++){ABuffer*b=&v->q[0];u32 at=(u32)v->position;if(at>=b->frames)at=b->frames-1;u32 next=at+1;if(next>=b->frames)next=b->loop?0:at;float fraction=(float)(v->position-at);
          float vol_n=vol0+(volume-vol0)*((float)(n+1)/AF);int ph=(int)(fraction*256.f+0.5f);if(ph>256)ph=256;if(ph<0)ph=0;const float*lw=g_lz[ph];
          for(u32 ch=0;ch<b->channels;ch++){float value;
            if(g_lz_lin)value=b->pcm[at*b->channels+ch]+(b->pcm[next*b->channels+ch]-b->pcm[at*b->channels+ch])*fraction;
            else{value=0;for(int t=0;t<8;t++){i64 k=(i64)at+t-3;float x= k>=0&&k<b->frames? b->pcm[(size_t)k*b->channels+ch] : asrc_at(v,ch,k);value+=x*lw[t];}}
            value=abiquad(v,vi,ch,value)*vol_n;
            u32 resource=au32(vi+0x140+ch*4);const u8*res=resource<AR&&r->resource_valid[resource]?r->resources[resource]:NULL;
            for(u32 dst=0;dst<nc;dst++){float gain=res?af32(res+4+dst*4):(b->channels==1?1.f:dst==ch?1.f:0.f);if(gain)r->samples[((size_t)mix*24+dst)*AF+n]+=value*gain;}}
          v->position+=step;v->played_fraction+=step;u64 whole=(u64)v->played_fraction;v->played+=whole;v->played_fraction-=whole;
          while(v->nq&&v->position>=v->q[0].frames){u32 frames=v->q[0].frames;v->position-=frames;if(v->q[0].loop){if(v->position>=frames)v->position=fmod(v->position,frames);break;}
            /* EOS resets the reported sample position, not the PCM cursor.
               Preserve source-sample overshoot across resampling boundaries. */
            if(g_opt_audio_eos_sample_count&&v->q[0].eos){v->played=(u64)v->position;v->played_fraction=v->position-v->played;}
            apop(v);}
        }
        v->vol_prev=volume;v->vol_ok=1;
      }
    }
    memcpy(status+i*16,&v->played,8);memcpy(status+i*16+8,&v->consumed,4);
  }
  /* Route leaves towards the final mix in dependency order. Cycles and
     invalid destinations are ignored instead of recursively feeding audio. */
  u8 done[AM]={0};done[0]=1;
  for(int pass=0;pass<AM;pass++){int progressed=0;for(u32 id=1;id<AM;id++){AMix*m=&r->mix[id];if(done[id]||!m->valid||!m->used)continue;int child=0;for(u32 k=1;k<AM;k++)if(!done[k]&&r->mix[k].valid&&r->mix[k].used&&r->mix[k].dest==id){child=1;break;}if(child)continue;done[id]=1;progressed=1;aeffects(r,id);u32 dest=m->dest;if(dest>=AM||dest==id)continue;u32 dc=r->mix[dest].valid?r->mix[dest].count:2;
    for(u32 src=0;src<m->count;src++)for(u32 d=0;d<dc;d++){float gain=m->matrix[src][d]*m->volume;if(!isfinite(gain))gain=0;
      float g0=m->gprev_ok?m->gprev[src][d]:gain;m->gprev[src][d]=gain;if(!gain&&!g0)continue;   /* ramped like the console's mix volume */
      float*s=r->samples+((size_t)id*24+src)*AF,*out=r->samples+((size_t)dest*24+d)*AF;
      if(g0==gain)for(int n=0;n<AF;n++)out[n]+=s[n]*gain;else for(int n=0;n<AF;n++)out[n]+=s[n]*(g0+(gain-g0)*((float)(n+1)/AF));}
    m->gprev_ok=1;}
    if(!progressed)break;}
  aeffects(r,0);
  if(o->u[3]==0){i16 pcm[AF*2];extern int g_opt_volume;float final_volume=(r->mix[0].valid?r->mix[0].volume:1)*(float)g_opt_volume*0.01f;   /* [Audio] volume, live */float peak=0;
    {static int checked;static FILE*csv;if(!checked){checked=1;const char*e=getenv("SWR_AUDIO_MIX_CSV");if(e&&*e){csv=fopen(e,"w");if(csv)fprintf(csv,"ns,tick,active,sink_count,input,source,rms,peak\n");}}
      if(csv&&r->ticks%40==0){for(u32 ch=0;ch<r->sink_count;ch++){u32 source=r->sink_inputs[ch];if(source>=24)continue;double sum=0;float peak=0;for(int n=0;n<AF;n++){float value=r->samples[(size_t)source*AF+n]*final_volume;sum+=(double)value*value;if(fabsf(value)>peak)peak=fabsf(value);}fprintf(csv,"%llu,%llu,%u,%u,%u,%u,%.8f,%.8f\n",(unsigned long long)plat_ns(),(unsigned long long)r->ticks,active,r->sink_count,ch,source,sqrt(sum/AF),peak);}fflush(csv);}}
    for(int n=0;n<AF;n++)for(int ch=0;ch<2;ch++){float sample=astereo_sample(r,n,ch,final_volume);if(!isfinite(sample))sample=0;float absolute=fabsf(sample);if(absolute>peak)peak=absolute;if(sample>0.9999695f)sample=0.9999695f;if(sample< -1)sample= -1;pcm[n*2+ch]=(i16)lrintf(sample*32768);}
    {int sent6=0;
#if defined(_WIN32) && !defined(__TINYC__)
      if(g_opt_audio_51&&r->sink_count==6){i16 pcm6[AF*6];   /* audio_channels=5.1: the six sink channels as they are */
        for(int n=0;n<AF;n++)for(int ch=0;ch<6;ch++){u32 source=r->sink_inputs[ch];float sample=source<24?r->samples[(size_t)source*AF+n]*final_volume:0;
          if(!isfinite(sample))sample=0;if(sample>0.9999695f)sample=0.9999695f;if(sample< -1)sample= -1;pcm6[n*6+ch]=(i16)lrintf(sample*32768);}
        sent6=aoutput6(pcm6);}
#endif
      if(!sent6)aoutput(pcm);}
    acapture(pcm);if(r->ticks%1000==0)LOG("audio: %u active voices, final mix volume %.3f, peak %.4f, sink channels %u\n",active,final_volume,peak,r->sink_count);
  }r->ticks++;
}
