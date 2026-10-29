/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* xxHash 0.8.3's XXH3 (Yann Collet, BSD-2-Clause, cmake/dependencies.cmake) for vk_hash (cmd.c), the one way each
   architecture's compiler takes, decided by its own predefined macros rather than by the build's idea of the target
   (a cross build's CMAKE_SYSTEM_PROCESSOR may name the host). On x86 the dispatcher runs the AVX-512, AVX2 or SSE2
   loop the CPU has, from a build for any x86. Elsewhere xxhash.c picks its loop from the compiler's macros: NEON on
   arm64, VSX on POWER8 and later, scalar on the rest (riscv64, loongarch64, big-endian ppc64), and it reads its input
   little-endian on every host, so every one gives the same hashes */
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include "xxh_x86dispatch.c"
#else
#include "xxhash.c"
#endif
