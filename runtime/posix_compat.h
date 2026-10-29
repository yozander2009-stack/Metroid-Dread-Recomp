/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* macOS build of the game code (and any POSIX build the setup compiles): force-included (-include) into every
   generated file, so the patched sources compile unchanged. The patches declare the few Windows calls they make
   themselves: GetEnvironmentVariableA (the SWR_* toggles) and LoadLibraryA/GetProcAddress (zlib1.dll). Here they are
   getenv and dlopen/dlsym, and zlib1.dll is the system's zlib. The same as linux/lx_compat.h */
#ifndef SWR_POSIX_COMPAT_H
#define SWR_POSIX_COMPAT_H
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define __declspec(x)
#define __stdcall
#if defined(__APPLE__)
#define SWR_SYSTEM_ZLIB "libz.1.dylib"
#else
#define SWR_SYSTEM_ZLIB "libz.so.1"
#endif
static unsigned long swr_px_getenv(const char* n, char* b, unsigned long sz) {
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
static void* swr_px_loadlib(const char* n) {
    if (n && !strcmp(n, "zlib1.dll")) return dlopen(SWR_SYSTEM_ZLIB, RTLD_NOW | RTLD_LOCAL);
    return dlopen(n, RTLD_NOW | RTLD_LOCAL);
}
static void* swr_px_getproc(void* h, const char* n) { return h ? dlsym(h, n) : NULL; }
#define GetEnvironmentVariableA swr_px_getenv
#define LoadLibraryA swr_px_loadlib
#define GetProcAddress swr_px_getproc
#endif
