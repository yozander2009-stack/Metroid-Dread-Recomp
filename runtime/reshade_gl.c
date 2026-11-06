/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* ReShade (reshade.me) depth. ReShade's own depth search found nothing usable in Dread: the scene's depth target is
   cleared again before the frame is shown, so effects read 1.0 everywhere (MEASURED 2026-11-05, ReShade 6.8.0,
   OpenGL). When ReShade is loaded the game registers itself as a ReShade add-on and, before ReShade's effects run each
   frame, binds a copy of the scene depth (taken where the scene is finished, the point DLAA/TAA use: gpu_gl.c for
   OpenGL, the native Vulkan renderer's dlss.c for Vulkan) to every effect texture with the DEPTH semantic. Only
   add-on events that ReShade's normal (not add-on) build allows are used.
     OpenGL: ReShade is opengl32.dll next to the game. Vulkan (renderer=vulkan_native): ReShade64.dll, its layer.
   ReShade's add-on interface is C++ (crosire/reshade include/, BSD 3-clause); the two functions and the one virtual
   call needed are declared here: effect_runtime::update_texture_bindings is slot 46 of ReShade's MSVC vtable in API
   version 20 (ReShade 6.8). SWR_NO_RESHADE_DEPTH=1 leaves ReShade alone. */
#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define RS_API_VERSION 20
#define RS_EV_BEGIN_EFFECTS 76          /* addon_event::reshade_begin_effects */
#define RS_SLOT_UPDATE_TEXTURE_BINDINGS 46
#define RS_GL_TEXTURE_2D 0x0DE1u

static volatile unsigned long long g_rs_view;   /* ReShade resource_view handle: GL target << 40 | name, or a VkImageView */
typedef void (*rs_utb_fn)(void* self, const char* semantic, uint64_t srv, uint64_t srv_srgb);

/* addon_event::reshade_begin_effects(effect_runtime*, command_list*, resource_view rtv, resource_view rtv_srgb) */
static void rs_begin_effects(void* runtime, void* cmd_list, uint64_t rtv, uint64_t rtv_srgb){
  (void)cmd_list; (void)rtv; (void)rtv_srgb;
  uint64_t v=g_rs_view; if(!v || !runtime) return;
  void** vt=*(void***)runtime;
  ((rs_utb_fn)vt[RS_SLOT_UPDATE_TEXTURE_BINDINGS])(runtime,"DEPTH",v,v); }

static int rs_register(const char* dll, const char* what){
  if(getenv("SWR_NO_RESHADE_DEPTH")) return 0;
  HMODULE m=GetModuleHandleA(dll); if(!m) return 0;
  typedef unsigned char (*reg_fn)(void* module, uint32_t api_version);
  typedef void (*ev_fn)(uint32_t ev, void* callback);
  reg_fn reg=(reg_fn)(void*)GetProcAddress(m,"ReShadeRegisterAddon");
  ev_fn ev=(ev_fn)(void*)GetProcAddress(m,"ReShadeRegisterEvent");
  if(!reg || !ev) return 0;   /* the system's opengl32.dll: no ReShade */
  if(!reg(GetModuleHandleA(NULL),RS_API_VERSION)){
    LOG("reshade (%s): found, but it refused the game's add-on (API version %d; ReShade 6.8 or later needed): depth is left to ReShade\n",what,RS_API_VERSION);
    return 0; }
  ev(RS_EV_BEGIN_EFFECTS,(void*)rs_begin_effects);
  LOG("reshade (%s): found; the scene depth goes to ReShade effects (DEPTH). SWR_NO_RESHADE_DEPTH=1 = leave it to ReShade\n",what);
  return 1; }

int reshade_gl_on(void){ static int on=-1; if(on<0) on=rs_register("opengl32.dll","OpenGL"); return on; }
int reshade_vk_on(void){ static int on=-1; if(on<0) on=rs_register("ReShade64.dll","Vulkan"); return on; }
void reshade_set_view(unsigned long long view){ g_rs_view=view; }
void reshade_gl_set_depth(unsigned tex){ g_rs_view= tex? ((unsigned long long)RS_GL_TEXTURE_2D<<40)|tex : 0; }

/* Is ReShade's Vulkan layer installed (the Vulkan loader's implicit layers, a value naming ReShade, data 0 = on)?
   Asked before the native Vulkan renderer starts: its command recording thread stays off when the depth copy may be
   needed (the copy is recorded directly into the frame, as DLSS is) */
/* the installed layer only switches itself on for the games its installer set it up for: ReShadeApps.ini (next to
   the layer's manifest, lower-cased path json) lists this EXE, or a ReShade.ini sits next to it */
static int rs_layer_for_us(const char* json){
  char exe[MAX_PATH], low[MAX_PATH], p[MAX_PATH+32]; DWORD n=GetModuleFileNameA(NULL,exe,sizeof exe);
  if(!n || n>=sizeof exe) return 1;   /* can't tell: assume it is */
  for(DWORD i=0;i<=n;i++) low[i]=(exe[i]>='A'&&exe[i]<='Z')? exe[i]+32 : exe[i];
  { char* s=strrchr(exe,'\\'); if(s){ snprintf(p,sizeof p,"%.*s\\ReShade.ini",(int)(s-exe),exe); if(GetFileAttributesA(p)!=INVALID_FILE_ATTRIBUTES) return 1; } }
  const char* s=strrchr(json,'\\'); if(!s) return 0;
  snprintf(p,sizeof p,"%.*s\\ReShadeApps.ini",(int)(s-json),json);
  FILE* f=fopen(p,"rb"); if(!f) return 0;
  static char buf[1<<16]; size_t m=fread(buf,1,sizeof buf-1,f); fclose(f); buf[m]=0;
  for(size_t i=0;i<m;i++) if(buf[i]>='A'&&buf[i]<='Z') buf[i]+=32;
  return strstr(buf,low)!=NULL; }

int reshade_vk_layer_installed(void){
  if(getenv("SWR_NO_RESHADE_DEPTH")) return 0;
  { static const char* envs[3]={"VK_INSTANCE_LAYERS","VK_ADD_IMPLICIT_LAYER_PATH","VK_ADD_LAYER_PATH"};   /* set for this run only */
    for(int i=0;i<3;i++){ const char* e=getenv(envs[i]); char low[1024]; size_t n=0;
      if(!e) continue;
      for(;e[n] && n<sizeof low-1;n++) low[n]=(e[n]>='A'&&e[n]<='Z')? e[n]+32 : e[n];
      low[n]=0; if(strstr(low,"reshade")) return 1; } }
  static const HKEY roots[2]={HKEY_LOCAL_MACHINE,HKEY_CURRENT_USER};
  for(int r=0;r<2;r++){ HKEY k;
    if(RegOpenKeyExA(roots[r],"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers",0,KEY_READ,&k)!=ERROR_SUCCESS) continue;
    char name[1024]; DWORD i=0, nl, type, data, dl;
    for(;;){ nl=sizeof name; dl=sizeof data;
      LONG e=RegEnumValueA(k,i++,name,&nl,NULL,&type,(BYTE*)&data,&dl);
      if(e==ERROR_NO_MORE_ITEMS) break;
      if(e!=ERROR_SUCCESS || type!=REG_DWORD) continue;
      for(char* c=name;*c;c++) if(*c>='A'&&*c<='Z') *c+=32;
      if(strstr(name,"reshade") && data==0 && rs_layer_for_us(name)){ RegCloseKey(k); return 1; } }
    RegCloseKey(k); }
  return 0; }
#else
int reshade_gl_on(void){ return 0; }
int reshade_vk_on(void){ return 0; }
void reshade_set_view(unsigned long long view){ (void)view; }
void reshade_gl_set_depth(unsigned tex){ (void)tex; }
int reshade_vk_layer_installed(void){ return 0; }
#endif
