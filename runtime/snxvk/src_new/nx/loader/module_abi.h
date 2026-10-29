/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The descriptor a game module hands the runtime: the modules it was recompiled from, where its code is and how
   its main thread starts. The recompiler writes it (module_llvm.c, module_arm.c) and the runtime
   reads it (runtime/main.c, runtime/loader.c, runtime/arm.c), so its layout is part of the ABI: a change to it needs
   a new SNX_ABI (cpu.h), or SNX_ARM_ABI for the 32-bit one */
#ifndef SNX_NX_MODULE_ABI_H
#define SNX_NX_MODULE_ABI_H
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/arm/arm.h"
#include <stddef.h>

/* One module as it was recompiled. The runtime maps it at base and checks the file it loads is this one */
typedef struct SnxModInfo {
    const char* name;
    u64 base, size;
    u8 build_id[32];
    u32 text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size;
    u32 is_nro;
} SnxModInfo;

/* What snx_module() returns. abi comes first, so the runtime can refuse a module of another version before it reads
   anything else */
typedef struct SnxModule {
    u32 abi;
    u32 nmods;
    const SnxModInfo* mods;
    u32 nentries;
    const u64* entry_addr; /* the dispatch entries' guest addresses, and their functions */
    const GuestFn* entry_fn;
    GuestFn interp; /* runs code nothing was compiled for */
    u64 code_lo, code_hi;
    const char* title;
    u64 title_id;
    u32 main_stack, main_prio, main_core;
    /* the module's functions (SNX_ABI 14), for code the guest patches: fn[i] was compiled from guest code in
       [fn_lo[i], fn_lo[i] + fn_len[i]), and runs its entry through dispatch instead once stale[i] isn't 0, which the
       runtime sets (relaxed: the function reads it with a relaxed load) when a line in that range changed */
    u32 nfns;
    const GuestFn* fn;
    const u64* fn_lo;
    const u32* fn_len;
    u8* stale;
} SnxModule;

/* What a 32-bit program's module returns from snx_module_arm() instead. Its code is AArch32 blocks
   (common/cpu/arm/), each running to its next control transfer, which runtime/arm.c dispatches one after another.
   Being another symbol with a version of its own, it leaves SNX_ABI and 64-bit modules alone. abi comes first here
   too */
#define SNX_ARM_MODULE_ABI (0x41333200u | SNX_ARM_ABI) /* "A32" and the SnxArmCPU layout's version */
typedef struct SnxArmModule {
    u32 abi;
    u32 nmods;
    const SnxModInfo* mods;
    u32 nentries;
    const u32* entry_addr; /* sorted block addresses, bit 0 set for Thumb ones */
    const SnxArmBlock* entry_fn;
    const char* title;
    u64 title_id;
    u32 main_stack, main_prio, main_core;
    u32 aspace; /* the NPDM's address space type */
} SnxArmModule;

/* The layouts existing modules were built with, on the 64-bit hosts modules are made for: a change that moves a byte
   fails to compile here */
#define SNX_ABI_CHECK(name, cond) typedef char snx_abi_check_##name[(sizeof(void*) != 8 || (cond)) ? 1 : -1]
SNX_ABI_CHECK(modinfo_size, sizeof(SnxModInfo) == 88);
SNX_ABI_CHECK(modinfo_base, offsetof(SnxModInfo, base) == 8 && offsetof(SnxModInfo, build_id) == 24);
SNX_ABI_CHECK(modinfo_text, offsetof(SnxModInfo, text_off) == 56 && offsetof(SnxModInfo, bss_size) == 80);
SNX_ABI_CHECK(modinfo_nro, offsetof(SnxModInfo, is_nro) == 84);
SNX_ABI_CHECK(module_size, sizeof(SnxModule) == 128);
SNX_ABI_CHECK(module_mods, offsetof(SnxModule, mods) == 8 && offsetof(SnxModule, nentries) == 16);
SNX_ABI_CHECK(module_entries, offsetof(SnxModule, entry_addr) == 24 && offsetof(SnxModule, entry_fn) == 32);
SNX_ABI_CHECK(module_code, offsetof(SnxModule, interp) == 40 && offsetof(SnxModule, code_lo) == 48 &&
                               offsetof(SnxModule, code_hi) == 56);
SNX_ABI_CHECK(module_title, offsetof(SnxModule, title) == 64 && offsetof(SnxModule, title_id) == 72);
SNX_ABI_CHECK(module_main, offsetof(SnxModule, main_stack) == 80 && offsetof(SnxModule, main_core) == 88);
SNX_ABI_CHECK(module_fns, offsetof(SnxModule, nfns) == 92 && offsetof(SnxModule, fn) == 96 &&
                              offsetof(SnxModule, fn_lo) == 104 && offsetof(SnxModule, fn_len) == 112 &&
                              offsetof(SnxModule, stale) == 120);
SNX_ABI_CHECK(arm_size, sizeof(SnxArmModule) == 72);
SNX_ABI_CHECK(arm_entries, offsetof(SnxArmModule, entry_addr) == 24 && offsetof(SnxArmModule, entry_fn) == 32);
SNX_ABI_CHECK(arm_title, offsetof(SnxArmModule, title) == 40 && offsetof(SnxArmModule, title_id) == 48);
SNX_ABI_CHECK(arm_main, offsetof(SnxArmModule, main_stack) == 56 && offsetof(SnxArmModule, aspace) == 68);
#undef SNX_ABI_CHECK

#endif
