/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The walk over a game's own files for the shader programs it ships (gpu/shader/shipped.h finds them in bytes): its
   RomFS (hle/filesystem/fs.h) and its executables' segments (runtime/loader.h). It runs on the renderer's shader file
   thread, and hands each program to the renderer, which translates and keeps it (gpu/vk/pipeline.c), through a
   ShipSink. What was read is kept by the sink as SHIP_FILE records (a file or segment, by its name, place, size and
   first bytes) and a SHIP_ALL record (every one, by the RomFS's table and the segments), so a later run reads only
   what it hasn't. Those are of one build and driver, whose translations the sink keeps with them. A RomFS file the
   walk finds no programs in is a SHIP_NONE record, by its name, place and size alone, which the sink keeps across
   builds and drivers under the walk's own version (ship_walk_version): after an update, the walk reads again only
   the files that hold programs, instead of every file (one game's 51,000 of them took 26 s, read one at a time) */
#ifndef SNX_SHIPPED_PASS_H
#define SNX_SHIPPED_PASS_H
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

enum { SHIP_FILE = 1, SHIP_ALL = 2, SHIP_NONE = 3 }; /* what a scan record says was read */
/* what the sink did with a program */
enum { SHIP_NEW, SHIP_KNOWN, SHIP_SKIPPED, SHIP_FAILED, SHIP_LACKED, SHIP_STOP };
typedef struct {
    void* ctx;
    int (*scanned)(void* ctx, u32 what, u64 id); /* read by an earlier run */
    void (*mark)(void* ctx, u32 what, u64 id);   /* read now, for the next run */
    /* a program: its SPH, the bytes from there to the end of what holds it, and its stage (shipped_sph_stage's,
       SHIPPED_VERTEX_A included). SHIP_NEW, SHIP_KNOWN (translated before), SHIP_SKIPPED (no draw would find it),
       SHIP_FAILED (its translation failed, as it will on every run of this build and driver), SHIP_LACKED (not
       handled for want of memory, for a later run to try again), or SHIP_STOP once the walk is to end */
    int (*program)(void* ctx, const u8* sph, size_t avail, int stage);
    int (*stopped)(void* ctx);
    /* a line of the walk, for the log, and for the screen when shown (the screen's in a player's words, the log's
       with the stages and the failures) */
    void (*note)(void* ctx, const char* line, int shown);
} ShipSink;
/* how far the walk is (atomics): the files and segments, those it looked at, the programs found (once counted) and
   those handed to the sink */
typedef struct {
    u32 files, files_seen, programs, programs_done;
} ShipProgress;

/* what the walk finds in a file's bytes, as a version: the hash of its sources (shipped.c, shipped_pass.c and their
   headers), which the build hands it. The key of the SHIP_NONE records, which a change of what it finds starts over */
const char* ship_walk_version(void);
/* the executables' writable segments copied, on the main thread before the game's first thread runs, so the walk
   reads them as they were loaded (the read-only ones it reads in place) */
void ship_snapshot(void);
/* the snapshot's copies freed, once the walk is done with them (or won't run) */
void ship_release(void);
/* the walk: counted first when count, so programs is known for the progress; else each program handed on as it is
   found. 0 when everything was read, 1 when the sink said stop, -1 when something couldn't be read */
int ship_walk(const ShipSink* sink, ShipProgress* progress, int count);
#endif
