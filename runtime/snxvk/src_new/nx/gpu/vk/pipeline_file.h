/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_PIPELINE_FILE_H
#define SNX_VK_PIPELINE_FILE_H
/* Not in vk.h: the lock below brings host.h, which includes <windows.h> on Windows, whose macros (far, near) break
   the renderer's other files */
#include "common/cpu/aarch64/cpu.h"
#include "common/filesystem/host.h" /* PMutex */
#include "nx/runtime/platform/plat.h" /* PFileMap */
#include "nx/gpu/shader/shader.h"
#include <stdio.h>

/* pipeline_file.c: the translated shaders and the pipelines made, kept across runs in shader_cache/<game>.vkshaders,
   so the next run makes them before the game asks (pipeline.c), and the shaders of the programs the game ships, in
   shader_cache/<game>.vkshipped, read as they are used (pf_open_mapped). The file is a header (PF_MAGIC, PF_VERSION,
   then the key: the build and the device, whatever changes the SPIR-V or the meaning of a pipeline key) and records
   appended as they are made, each {u32 type, u32 payload bytes, u64 check of the payload} and its payload, a multiple
   of 8 bytes. Reading stops at the first record that doesn't check out, and a file of another key is started over.
   Modules are known to the file by a u64 of their handle, and records refer to shaders by their number in the file
   and their check, never by handle.

   The main file's key starts with what its PF_SOURCE and PF_PIPE records depend on (pf_open's guest part: the device,
   its features, the layout of a pipeline key), and the rest is the build and the driver, which only the SPIR-V and
   ShaderInfo of a PF_SHADER record depend on. A file whose guest part is the same is rebuilt rather than started
   over: each shader translated again from the program its PF_SOURCE record keeps, and each pipeline made again, into
   a file of the new key. After an update of the runtime or the driver every pipeline would otherwise be new again,
   made as the game first draws with it, with the frame waiting (a second on NVIDIA for one game's largest) */
#define PF_SHADER  1
#define PF_PIPE    2
#define PF_SCAN    3 /* the shipped programs' walk (gpu/shipped_pass.c) read a file, or all of them: two u64s */
#define PF_SHIPREF 4 /* a shader of the shipped file, by where its record starts there and the record's check */
#define PF_SOURCE  5 /* what the PF_SHADER record right after it was translated from (PfSource) */
#define PF_MAX     (256u << 20) /* past this size nothing more is appended */
#define PF_REC     16           /* a record's header: type, payload bytes, check */
/* a shader record: this, then ncbr constant buffer reads ({idx, off, ok, v}, VkCbRead's words) and nspv SPIR-V
   words. Everything vk_shader_get keys a shader by and checks a found one with */
#define PF_SHIPPED 1u /* a PfShader's flags: a program the game ships, translated before a draw asked for it */
typedef struct {
    u64 hash, qh, qh_a;
    u32 stage, clen, clen_a, ncbr, nspv, flags;
    u32 mask[20];
    ShaderInfo si;
} PfShader;
/* a source record: this, then ncbr constant buffer reads ({idx, off, ok, v}), ncode bytes of the program as the
   translator read it (both parts merged, for VertexA and VertexB) and, with PF_SRC_XFB, the ShXfb it was translated
   with. Of a build-independent layout: no ShaderInfo, whose layout the build decides */
#define PF_SRC_XFB 1u
typedef struct {
    u64 hash, qh, qh_a;
    u32 stage, clen, clen_a, ncbr, ncode, flags;
    u32 mask[20];
} PfSource;
/* a pipeline record: this, then keylen bytes of the PipeKey with its module handles zeroed. sh: the shaders' record
   numbers (from 1; 0 for none) in the order vs, tcs, tes, gs, fs, and check: those records' checks */
typedef struct {
    u64 check[5];
    u32 sh[5];
    u32 keylen;
} PfPipe;
typedef struct {
    u64 module, check; /* module 0: a free slot */
    u32 rec, pad;
} PfMod;
typedef struct {
    PMutex lock;
    FILE* f; /* appended to; NULL once nothing more is (an error, or the file reached max), and pf_open_mapped's file
                but between pf_unseal and pf_seal */
    char path[1400]; /* f's: what a failed write names, and what pf_seal maps */
    u64 size; /* the file's bytes */
    u64 max;  /* past this size nothing more is appended: PF_MAX for 0 */
    int kept; /* pf_open_mapped's file that is there but couldn't be mapped or rewritten: never appended to */
    PFileMap view; /* pf_open_mapped's: the file as it was mapped, its records read from there */
    /* pf_open's files only: the shader records in it, and each module with a record, its number and check, by open
       addressing on the handle (cap a power of two), for pf_pipe */
    u32 nsh;
    PfMod* map;
    u32 cap, nmap;
} PFile;
/* What reading a file hands back. shader: a record, with its reads and SPIR-V, made into a module; the module's u64
   (its handle's bits), or 0 when it couldn't be made. pipe: a record whose shaders were all made, with their modules'
   u64s (0 for none) and the key's bytes. shipref: a PF_SHIPREF record's place and check, made into a module as a
   shader record is (its u64, 0 when it couldn't be). source: in a file being rebuilt (pf_open), a PF_SOURCE record,
   its reads, program and transform feedback layout (NULL without PF_SRC_XFB) translated again and kept in the new
   file: the module's u64, which stands for the PF_SHADER record after it, or 0. rebuild is set while a file is */
typedef struct {
    u64 (*shader)(void* ctx, const PfShader* s, const u32* cbr, const u32* spv);
    void (*pipe)(void* ctx, const u64 modules[5], const void* key, u32 keylen);
    u64 (*shipref)(void* ctx, u64 at, u64 check);
    u64 (*source)(void* ctx, const PfSource* s, const u32* cbr, const u8* code, const ShXfb* xfb);
    void* ctx;
    int rebuild;
} PfLoad;
/* opens path: the records written there with the same key go to load in their order, and what follows is appended.
   A missing file, or one with another key, is started over, except that one whose key's first guest bytes are the
   same is rebuilt: started over, with each PF_SOURCE record handed to load->source (which records the shader in the
   new file), and each pipeline whose shaders were made to load->pipe. The counts of shader and pipeline records
   handed on go in loaded[0] and [1] */
void pf_open(PFile* pf, const char* path, const char* key, size_t guest, PfLoad* load, u32 loaded[2]);
/* a shader record for module (made from spv; 0 for none), whose number pf_pipe then refers to; skipped once the file
   is full. With src (its reads src_cbr, its program code, xfb with PF_SRC_XFB), a PF_SOURCE record right before it.
   Where the shader record starts in the file, 0 when it wasn't written */
u64 pf_shader(PFile* pf, u64 module, const PfShader* s, const u32* cbr, const u32* spv, const PfSource* src,
              const u8* code, const ShXfb* xfb);
/* a pipeline record with modules (vs, tcs, tes, gs, fs; 0 for none): skipped when one of them has no record */
void pf_pipe(PFile* pf, const u64 modules[5], const void* key, u32 keylen);
/* a PF_SCAN record: what was read (shipped_pass.h's SHIP_FILE or SHIP_ALL) and its id */
void pf_scan(PFile* pf, u64 what, u64 id);
/* a PF_SHIPREF record for module, which pf_pipe then refers to as it does a shader record */
void pf_shipref(PFile* pf, u64 module, u64 at, u64 check);

/* A file whose records are read when they are used (the shaders of the programs a game ships, pipeline.c): its
   records handed to each from a view of it, by their type, their payload in the view and where each starts,
   unchecked. A missing file, or one of another key, is started over; one whose last record was cut short, or that
   ends in a record of no known type, is rewritten without what follows the last good record; one that is there but
   can't be mapped is left as it is, and nothing is appended to it. It stays mapped for pf_payload until pf_unseal
   unmaps it (a Windows view doesn't share writing) and opens it for appending, and pf_seal ends the appending and
   maps it again. pf_payload: the payload of the record that starts at at, and its check, NULL when it isn't there or
   doesn't check out */
typedef void (*PfEach)(void* ctx, u32 type, const u8* payload, u32 len, u64 at);
void pf_open_mapped(PFile* pf, const char* path, const char* key, PfEach each, void* ctx);
void pf_unseal(PFile* pf);
/* the appending ended and the view unmapped, for a file nothing reads afterwards */
void pf_close(PFile* pf);
/* the process's id, for a temporary file of its own: two runs of a game started together would write the same one */
unsigned pf_pid(void);
void pf_seal(PFile* pf);
const u8* pf_payload(const PFile* pf, u64 at, u32* len, u64* check);

#endif
