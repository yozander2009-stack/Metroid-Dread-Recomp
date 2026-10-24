/* Linux build of the game code: force-included (-include) into every generated file, so the installed (Windows)
   sources compile unchanged. The patches declare the few Windows calls they make themselves:
   GetEnvironmentVariableA (the SWR_* toggles of code_010/011/014/152/217) and LoadLibraryA/GetProcAddress
   (code_217: zlib1.dll). Here they are getenv and dlopen/dlsym; zlib1.dll is the system's libz.so.1 */
#ifndef SWR_LX_COMPAT_H
#define SWR_LX_COMPAT_H
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define __declspec(x)
#define __stdcall
static unsigned long swr_lx_getenv(const char* n, char* b, unsigned long sz) {
    const char* v = getenv(n);
    if (!v) return 0;
    unsigned long l = (unsigned long)strlen(v);
    if (b && sz) {
        unsigned long c = l < sz - 1 ? l : sz - 1;
        memcpy(b, v, c);
        b[c] = 0;
    }
    return l;
}
static void* swr_lx_loadlib(const char* n) {
    if (n && !strcmp(n, "zlib1.dll")) {
#ifdef __ANDROID__
        return dlopen("libz.so", RTLD_NOW | RTLD_LOCAL);
#else
        return dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    }
    return dlopen(n, RTLD_NOW | RTLD_LOCAL);
}
static void* swr_lx_getproc(void* h, const char* n) { return h ? dlsym(h, n) : NULL; }
#define GetEnvironmentVariableA swr_lx_getenv
#define LoadLibraryA swr_lx_loadlib
#define GetProcAddress swr_lx_getproc
#endif
