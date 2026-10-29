/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Reconcile a shader's mapped output with guest memory. The host may have written the guest range while the GPU was
   running: shader-changed bytes land only where the guest still holds the value the shader originally read. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"

/* Nested because a result can flush an overlapping render target while landing. */
static _Thread_local unsigned t_result_depth;
void vk_result_begin(void) { t_result_depth++; }
void vk_result_end(void) { t_result_depth--; }
int vk_result_landing(void) { return t_result_depth != 0; }

/* whether [p, p + n) overlaps the guest's address space, where pages the GPU holds fault when read */
static int in_guest(const u8* p, u32 n) { return p < g_mem + AS_SIZE && p + n > g_mem; }

void vk_write_changed(u64 addr, const u8* res, const u8* org, u32 size) {
    /* res and org are host copies: the shader's output read back into host-cached memory, and the bytes it started
       from, snapshotted when it was bound (buffer.c, compute.c, surface.c). Comparing them reads no guest page, so it
       can't fault on one the GPU holds. Only cur below reads guest memory, after vk_rt_before_write */
    if (!size) return;
    if (in_guest(res, size) || in_guest(org, size)) fatal("vulkan: a storage result compared in guest memory");
    if (!memcmp(res, org, size)) return; /* nothing written: no guest read, allocation or flush */
    vk_result_begin();
    vk_rt_before_write(addr, size);
    u8* dst = gmem_ptr(addr, size);
    u8* tmp = NULL;
    const u8* cur = dst;
    int changed = 0;
    if (!cur) {
        tmp = malloc(size);
        if (!tmp) fatal("Out of memory landing a shader storage buffer");
        gmem_read(addr, tmp, size);
        cur = tmp;
    }
    for (u32 i = 0; i < size;) {
        u32 n = size - i < 1024 ? size - i : 1024, end = i + n;
        /* Large storage bindings often change only a few bytes: the host's vectorized comparison of the two host
           copies skips their unchanged ranges, and guest memory is read only where the shader changed a byte */
        if (!memcmp(res + i, org + i, n)) {
            i = end;
            continue;
        }
        do {
            if (end - i >= 8 && !memcmp(res + i, org + i, 8)) {
                i += 8;
                continue;
            }
            if (res[i] == org[i] || cur[i] != org[i]) {
                i++;
                continue;
            }
            u32 start = i;
            while (i < size && res[i] != org[i] && cur[i] == org[i]) i++;
            if (dst)
                memcpy(dst + start, res + start, i - start);
            else
                gmem_write(addr + start, res + start, i - start);
            changed = 1;
        } while (i < end);
    }
    free(tmp);
    if (changed) gpu_mem_written_shader(addr, size); /* other GPU virtual addresses can alias these guest pages */
    vk_result_end();
}
