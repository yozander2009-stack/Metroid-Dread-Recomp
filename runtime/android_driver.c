/* SwitchRecomp - Copyright 2026 MZ Labs. Apache-2.0; see LICENSE.txt and NOTICE.txt. */
#include "rt.h"
#include <jni.h>
#include <dlfcn.h>
#ifdef SWR_HAVE_SNXVK
#include <adrenotools/driver.h>
#endif

static char driver_id[65], hook_dir[1200], driver_dir[1400], library[256], label[256], failure[600], status_path[1400];
static int custom;

static void copy_java(JNIEnv* env,jstring value,char* target,size_t size){
  const char* text=value?(*env)->GetStringUTFChars(env,value,NULL):NULL;
  snprintf(target,size,"%s",text?text:"");
  if(text) (*env)->ReleaseStringUTFChars(env,value,text);
}
void android_driver_status(const char* message){
  if(!status_path[0]) return;
  char temporary[1450]; snprintf(temporary,sizeof temporary,"%s.tmp",status_path);
  FILE* f=fopen(temporary,"w"); if(!f) return;
  fprintf(f,"%s\n",message); fclose(f); rename(temporary,status_path);
}
JNIEXPORT void JNICALL Java_com_switchrecomp_metroiddread_GameActivity_configureDriver(
    JNIEnv* env,jclass cls,jstring id,jstring hooks,jstring directory,jstring soname,jstring name,jstring error,jstring status){
  (void)cls;
  copy_java(env,id,driver_id,sizeof driver_id); custom=driver_id[0]!=0;
  copy_java(env,hooks,hook_dir,sizeof hook_dir); copy_java(env,directory,driver_dir,sizeof driver_dir);
  copy_java(env,soname,library,sizeof library); copy_java(env,name,label,sizeof label);
  copy_java(env,error,failure,sizeof failure); copy_java(env,status,status_path,sizeof status_path);
  if(custom && (strlen(driver_id)!=64 || strspn(driver_id,"0123456789abcdef")!=64)){
    snprintf(driver_id,sizeof driver_id,"invalid"); snprintf(failure,sizeof failure,"Invalid driver selection");
  }
  char message[600]; snprintf(message,sizeof message,"Starting %s driver%s%s",custom?"custom":"system",custom?": ":"",custom?label:"");
  android_driver_status(message);
}
int android_driver_custom(void){ return custom; }
const char* android_driver_cache_id(void){ return custom?driver_id:"system"; }
const char* android_driver_error(void){ return failure[0]?failure:"The selected driver could not initialize Vulkan. Choose System driver or another compatible package."; }
void* android_vulkan_loader(void){
  static void* handle;
  if(handle) return dlsym(handle,"vkGetInstanceProcAddr");
  if(custom){
#ifdef SWR_HAVE_SNXVK
    if(failure[0] || !driver_dir[0] || !library[0]) return NULL;
    LOG("Android: loading custom Vulkan driver %s (%s%s)\n",label,driver_dir,library);
    handle=adrenotools_open_libvulkan(RTLD_NOW|RTLD_LOCAL,ADRENOTOOLS_DRIVER_CUSTOM,NULL,hook_dir,driver_dir,library,NULL,NULL);
    if(!handle){ const char* error=dlerror(); snprintf(failure,sizeof failure,"Custom Vulkan loader failed: %.400s",error?error:"libadrenotools could not load the selected driver"); }
#else
    snprintf(failure,sizeof failure,"Custom drivers require a Vulkan-enabled build.");
#endif
  }else handle=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
  return handle?dlsym(handle,"vkGetInstanceProcAddr"):NULL;
}
int android_driver_confirm(void){
  if(!custom){ android_driver_status("Using system Vulkan driver"); return 1; }
  char expected[1700],line[4096]; snprintf(expected,sizeof expected,"%s%s",driver_dir,library);
  int found=0; FILE* maps=fopen("/proc/self/maps","r");
  if(maps){ while(fgets(line,sizeof line,maps)) if(strstr(line,expected)){ found=1; break; } fclose(maps); }
  if(!found){ snprintf(failure,sizeof failure,"Selected custom driver was not mapped. System-driver fallback was rejected. Choose System driver or another package."); return 0; }
  char message[600]; snprintf(message,sizeof message,"Loaded custom Vulkan driver: %s\nLibrary: %s",label,library);
  android_driver_status(message); LOG("Android: custom driver verified: %s (%s)\n",label,library); return 1;
}
