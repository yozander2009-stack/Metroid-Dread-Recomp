/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shipped_pass.h"
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/shader/shipped.h"
/* SWR: the two RomFS readers this walk uses (nx/hle/filesystem/fs.h declares them; including it pulls in the game-dump
   driver, which this build has no use for). runtime/snxvk_glue.c provides them */
typedef int (*RomfsEach)(void* ctx, const char* name, u64 off, u64 size);
int romfs_files(RomfsEach each, void* ctx);
int romfs_peek(u64 off, void* dst, u64 size);
#include "nx/runtime/loader.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MAX  (512u << 20)    /* a file or a decompressed stream past this isn't read, by this build ever */
#define HELD_MAX  (1024ull << 20) /* the bytes the walk reads into at once, a file's and its streams' */
#define KEPT_MAX  (256ull << 20)  /* the hits' bytes kept from counting to translating; past this, read again */
#define TABLE_MAX (16u << 20)     /* a SARC's tables past this aren't read to see what it holds */

const char* ship_walk_version(void) { return SNX_SHIP_WALK; }

/* a file or segment to look in */
typedef struct {
    char name[96];
    u64 off, size; /* a file's place in the RomFS image; a segment's guest address */
    const u8* mem; /* a segment's bytes, NULL for a file */
} Source;
/* a segment from ship_snapshot: a writable one's bytes are a copy the walk owns (owned), a read-only one's in place */
typedef struct {
    Source src;
    u8* owned;
} Segment;
static struct {
    Segment* seg;
    u32 nseg;
} g_shipped_pass;

static int snapshot_seg(void* ctx, const char* name, const u8* bytes, u64 gva, u64 size, int writable) {
    Segment* seg = realloc(g_shipped_pass.seg, (g_shipped_pass.nseg + 1) * sizeof *seg);
    Segment* s;
    (void)ctx;
    if (!seg) return 1;
    g_shipped_pass.seg = seg;
    s = &seg[g_shipped_pass.nseg];
    *s = (Segment){0};
    if (writable) {
        s->owned = malloc(size ? (size_t)size : 1);
        if (!s->owned) return 0; /* left out: neither read this run nor marked read */
        memcpy(s->owned, bytes, (size_t)size);
    }
    snprintf(s->src.name, sizeof s->src.name, "%s", name);
    s->src.off = gva;
    s->src.size = size;
    s->src.mem = writable ? s->owned : bytes;
    g_shipped_pass.nseg++;
    return 0;
}
void ship_snapshot(void) { loader_segments(snapshot_seg, NULL); }
void ship_release(void) {
    for (u32 i = 0; i < g_shipped_pass.nseg; i++) free(g_shipped_pass.seg[i].owned);
    free(g_shipped_pass.seg);
    g_shipped_pass.seg = NULL;
    g_shipped_pass.nseg = 0;
}

/* the walk's state */
typedef struct {
    const ShipSink* sink;
    ShipProgress* progress;
    Source* src; /* the segments, then the RomFS's files */
    u32 nsrc, cap;
    /* SHIP_ALL's id: every source's name, place and size mixed, and none of their bytes, so that a later run tells
       without reading any. An image whose every name, place and size stayed while some file's bytes changed is
       taken as read: a game's update runs as a program of its own, with shader files of its own */
    u64 all;
    int counting, stop;
    int again;             /* translating the hits counting found: what counting counted is met a second time */
    u32 stages[ST_CS + 2]; /* the programs counted by stage, VertexA's last */
    u32 found, made, known, skipped, lone, failed, lacked, unread, big;
    u64 held, kept; /* bytes read into now, and the hits' bytes kept from counting */
} Walk;
/* a source counting found programs in, with its bytes when they were kept */
typedef struct {
    u32 src;
    int kind;
    u64 id;
    u8* bytes;
} Hit;
/* what reading a source gave: its bytes, nothing to read (no programs, or too large for this build ever), or a failure
   a later run may not have */
enum { BYTES_OK, BYTES_NONE, BYTES_UNREAD };

static u64 mix(u64 h, const void* p, size_t n) {
    const u8* b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
static int add_source(void* ctx, const char* name, u64 off, u64 size) {
    Walk* w = ctx;
    if (w->nsrc == w->cap) {
        u32 cap = w->cap ? w->cap * 2 : 1024;
        Source* grown = realloc(w->src, cap * sizeof *grown);
        if (!grown) return 1;
        w->src = grown;
        w->cap = cap;
    }
    snprintf(w->src[w->nsrc].name, sizeof w->src[w->nsrc].name, "%s", name);
    w->src[w->nsrc].off = off;
    w->src[w->nsrc].size = size;
    w->src[w->nsrc].mem = NULL;
    w->all = mix(mix(mix(w->all, name, strlen(name)), &off, sizeof off), &size, sizeof size);
    w->nsrc++;
    return 0;
}
static int stopped(Walk* w) {
    if (!w->stop) w->stop = w->sink->stopped(w->sink->ctx);
    return w->stop;
}
static void say(Walk* w, int shown, const char* fmt, va_list ap) {
    char line[512];
    vsnprintf(line, sizeof line, fmt, ap);
    w->sink->note(w->sink->ctx, line, shown);
}
/* a line for the log and the screen */
static void note(Walk* w, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void note(Walk* w, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    say(w, 1, fmt, ap);
    va_end(ap);
}
/* a line for the log alone */
static void detail(Walk* w, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void detail(Walk* w, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    say(w, 0, fmt, ap);
    va_end(ap);
}
static int hold(Walk* w, u64 n) { /* 0, or -1 when n more bytes would pass HELD_MAX */
    if (n > HELD_MAX - w->held) return -1;
    w->held += n;
    return 0;
}
static void unhold(Walk* w, u64 n) { w->held -= n; }

/* each program found: counted, or handed to the sink */
static int each_program(void* ctx, const u8* sph, size_t avail, int stage) {
    Walk* w = ctx;
    if (stopped(w)) return 1;
    w->found++;
    if (w->counting) {
        w->stages[stage == SHIPPED_VERTEX_A ? ST_CS + 1 : stage]++;
        return 0;
    }
    __atomic_add_fetch(&w->progress->programs_done, 1, __ATOMIC_RELAXED);
    switch (w->sink->program(w->sink->ctx, sph, avail, stage)) {
        case SHIP_NEW: w->made++; break;
        case SHIP_KNOWN: w->known++; break;
        case SHIP_SKIPPED:
            if (stage == SHIPPED_VERTEX_A)
                w->lone++;
            else
                w->skipped++;
            break;
        case SHIP_FAILED: w->failed++; break;
        case SHIP_LACKED: w->lacked++; break;
        default: w->stop = 1; return 1;
    }
    return 0;
}

/* whether the Yaz0 stream in[0, n), all of it or its start, holds shader programs: a SARC whose names say so, or a
   shader container itself. 1, 0, -1 when more of the stream is needed to tell, or -2 when out of memory to tell */
static int yaz0_wants(const u8* in, size_t n) {
    u8 head[0x20];
    size_t total = shipped_yaz0_size(in, n), need;
    if (total < sizeof head) return 0;
    if (shipped_yaz0(in, n, head, sizeof head) < sizeof head) return -1;
    if (memcmp(head, "SARC", 4)) return shipped_kind(head, sizeof head) == SHIPPED_SCAN;
    need = shipped_sarc_head(head, sizeof head);
    if (need < sizeof head || need > total || need > TABLE_MAX) return 0;
    u8* t = malloc(need);
    if (!t) return -2;
    int wants = shipped_yaz0(in, n, t, need) < need ? -1 : shipped_sarc_wants(t, need) == 1;
    free(t);
    return wants;
}
static int scan_bytes(Walk* w, const u8* d, size_t n, int depth);
/* a Yaz0 stream: decompressed and scanned when it holds shader programs. 0, or -1 when it couldn't be read this run.
   One past FILE_MAX, or whose decompressing would hold past HELD_MAX, is counted in big and left: the walk holds
   nothing of other sources when it reads one (the hits' kept bytes are KEPT_MAX's), so no run of this build reads it */
static int scan_yaz0(Walk* w, const u8* in, size_t n, int depth) {
    size_t total = shipped_yaz0_size(in, n);
    int rc = -1, wants = yaz0_wants(in, n);
    if (wants != 1) return wants == -2 ? -1 : 0;
    if (total > FILE_MAX || hold(w, total)) {
        w->big += !w->again; /* counted once: when counted first, a hit's streams are met again translating */
        return 0;
    }
    u8* out = malloc(total);
    if (out && shipped_yaz0(in, n, out, total) == total)
        rc = scan_bytes(w, out, total, depth);
    else if (out)
        rc = 0; /* a stream cut short: what it holds can't be read on any run */
    free(out);
    unhold(w, total);
    return rc;
}
/* the programs in d[0, n), and in the Yaz0 entries of a SARC there, which no scan of its bytes reaches */
static int scan_bytes(Walk* w, const u8* d, size_t n, int depth) {
    int count = shipped_sarc_count(d, n), rc = 0;
    shipped_programs(d, n, each_program, w);
    for (int i = 0; depth < 2 && i < count && !stopped(w); i++) {
        size_t off, size;
        const char* name;
        if (!shipped_sarc_entry(d, n, i, &off, &size, &name) && shipped_yaz0_size(d + off, size) &&
            scan_yaz0(w, d + off, size, depth + 1))
            rc = -1;
    }
    return rc;
}
/* whether a Yaz0 file of the RomFS at off holds programs, from its stream's start: 1, 0, or -1 when it couldn't be
   read this run */
static int yaz0_file_wants(u64 off, u64 size) {
    size_t part = size < (64u << 10) ? (size_t)size : 64u << 10;
    u8* t = malloc(part ? part : 1);
    int go = !t || romfs_peek(off, t, part) ? -2 : yaz0_wants(t, part);
    if (go == -1) { /* the tables need more of the stream: as much as they are, and an eighth more for its flags */
        u8 sarc[0x20];
        size_t want =
            shipped_yaz0(t, part, sarc, sizeof sarc) == sizeof sarc ? shipped_sarc_head(sarc, sizeof sarc) : 0;
        go = 0; /* tables past TABLE_MAX, or a whole file whose stream ends before them: none read on any run */
        if (want && want <= TABLE_MAX) {
            want += want / 8 + 64;
            if (want > size) want = (size_t)size;
            if (want > part) {
                u8* more = realloc(t, want);
                if (more) t = more;
                int wants = !more || romfs_peek(off, t, want) ? -2 : yaz0_wants(t, want);
                go = wants == -2 ? -2 : wants == 1;
            }
        }
    }
    free(t);
    return go == -2 ? -1 : go == 1;
}
/* a source's bytes into *d: a segment's in place, a file's read whole into a buffer held (*owned) */
static int source_bytes(Walk* w, const Source* s, int kind, const u8** d, u8** owned) {
    *owned = NULL;
    *d = s->mem;
    if (s->mem) return BYTES_OK;
    if (kind == SHIPPED_YAZ0) {
        int wants = yaz0_file_wants(s->off, s->size);
        if (wants <= 0) return wants ? BYTES_UNREAD : BYTES_NONE;
    }
    if (hold(w, s->size)) return BYTES_UNREAD;
    *owned = malloc(s->size ? (size_t)s->size : 1);
    if (!*owned || romfs_peek(s->off, *owned, s->size)) {
        free(*owned);
        *owned = NULL;
        unhold(w, s->size);
        return BYTES_UNREAD;
    }
    *d = *owned;
    return BYTES_OK;
}
static int scan_source(Walk* w, const u8* d, size_t n, int kind) {
    u32 lacked = w->lacked;
    int rc = kind == SHIPPED_YAZ0 ? scan_yaz0(w, d, n, 0) : scan_bytes(w, d, n, 0);
    return w->lacked > lacked ? -1 : rc; /* a program not handled for want of memory: the source isn't read */
}
/* a source's name, place and size mixed: a file's id in the SHIP_NONE records, which reading none of it tells */
static u64 place_of(const Source* s) {
    u64 h = 0xCBF29CE484222325ull;
    return mix(mix(mix(h, s->name, strlen(s->name)), &s->off, sizeof s->off), &s->size, sizeof s->size);
}
/* what a source holds, by its first bytes: -1 when they couldn't be read, or its kind and its id in the SHIP_FILE
   records (its place, place_of's, and first bytes) */
static int source_kind(const Source* s, u64 place, u64* id) {
    u8 head[SHIPPED_HEAD] = {0};
    size_t n = s->size < sizeof head ? (size_t)s->size : sizeof head;
    if (s->mem)
        memcpy(head, s->mem, n);
    else if (romfs_peek(s->off, head, n))
        return -1;
    *id = mix(place, head, n);
    return s->mem ? SHIPPED_SCAN : shipped_kind(head, n);
}
/* a source the walk translates nothing from, marked read: a file for every build (SHIP_NONE, by its place), a segment
   for this one (whose name, place and size an update of the game keeps more often than a file's) */
static void read_nothing(Walk* w, const Source* s, u64 id, u64 place) {
    w->sink->mark(w->sink->ctx, s->mem ? SHIP_FILE : SHIP_NONE, s->mem ? id : place);
}
/* a source's programs as counted (since: the stages' counts before it): how many on the screen, by stage in the log */
static void note_counted(Walk* w, const char* name, const u32* since) {
    char by[192] = "";
    size_t at = 0;
    u32 n = 0;
    for (int k = 0; k < ST_CS + 2 && at < sizeof by; k++)
        if (w->stages[k] > since[k]) {
            n += w->stages[k] - since[k];
            at += (size_t)snprintf(by + at, sizeof by - at, "%s%u %s", at ? ", " : "", w->stages[k] - since[k],
                                   shipped_stage_name(k == ST_CS + 1 ? SHIPPED_VERTEX_A : k));
        }
    note(w, "%s: %u shader%s found", name, n, n == 1 ? "" : "s");
    detail(w, "%s: %s", name, by);
}
/* a counts' line: new, already done and for later (left out or failed: made as the game draws them), the zeros left
   out but for a line of nothing but zeros ("0 new") */
static void counts(char* out, size_t size, u32 made, u32 known, u32 later) {
    size_t at = 0;
    *out = 0;
    if (made || (!known && !later)) at += (size_t)snprintf(out, size, "%u new", made);
    if (known && at < size) at += (size_t)snprintf(out + at, size - at, "%s%u already done", at ? ", " : "", known);
    if (later && at < size) snprintf(out + at, size - at, "%s%u for later", at ? ", " : "", later);
}
/* a source's translations: before holds made, known, skipped and lone, and failed before it */
static void note_translated(Walk* w, const char* name, const u32 before[4]) {
    char line[128];
    u32 made = w->made - before[0], known = w->known - before[1], unused = w->skipped + w->lone - before[2],
        failed = w->failed - before[3];
    counts(line, sizeof line, made, known, unused + failed);
    note(w, "%s: %s", name, line);
    detail(w, "%s: %u translated, %u already translated, %u left out, %u failed", name, made, known, unused, failed);
}
/* a source the counting found programs in, kept for translating, with its bytes when KEPT_MAX has room for them
   (else they're freed here and read again): 0, or -1 */
static int add_hit(Walk* w, Hit** hit, u32* nhit, u32 src, int kind, u64 id, u8** owned) {
    Hit* grown = realloc(*hit, (*nhit + 1) * sizeof *grown);
    u64 size = w->src[src].size;
    if (!grown) return -1;
    *hit = grown;
    grown[*nhit] = (Hit){src, kind, id, NULL};
    if (*owned && size <= KEPT_MAX - w->kept) { /* from the bytes read into to those kept */
        grown[*nhit].bytes = *owned;
        *owned = NULL;
        unhold(w, size);
        w->kept += size;
    }
    ++*nhit;
    return 0;
}

int ship_walk(const ShipSink* sink, ShipProgress* progress, int count) {
    Walk w = {0};
    Hit* hit = NULL;
    u32 nhit = 0, i;
    int romfs;
    w.sink = sink;
    w.progress = progress;
    w.all = 0xCBF29CE484222325ull;
    for (i = 0; i < g_shipped_pass.nseg; i++) { /* the segments, then the RomFS's files, listed once */
        const Source* seg = &g_shipped_pass.seg[i].src;
        if (add_source(&w, seg->name, seg->off, seg->size)) break;
        w.src[w.nsrc - 1].mem = seg->mem;
    }
    romfs = romfs_files(add_source, &w);
    if (i < g_shipped_pass.nseg || romfs > 0) { /* out of memory listing them */
        free(w.src);
        return -1;
    }
    if (!w.nsrc) return romfs == -1 ? -1 : 0;
    if (romfs == -2) /* the table read, its malformed entries left out as on every run */
        detail(&w, "the RomFS's file table has malformed entries, left out");
    if (romfs != -1 && sink->scanned(sink->ctx, SHIP_ALL, w.all)) {
        note(&w, "The game's files were already checked by an earlier run");
        free(w.src);
        return 0;
    }
    __atomic_store_n(&progress->files, w.nsrc, __ATOMIC_RELAXED);
    w.counting = count;
    for (i = 0; i < w.nsrc && !stopped(&w); i++) {
        const Source* s = &w.src[i];
        u32 found = w.found, since[ST_CS + 2], before[4] = {w.made, w.known, w.skipped + w.lone, w.failed};
        u64 id = 0, place = place_of(s);
        u8* owned;
        const u8* d;
        int kind, got, rc;
        __atomic_add_fetch(&progress->files_seen, 1, __ATOMIC_RELAXED);
        /* a file of a kind skipped by its name, or that an earlier run of this walk found nothing in: none of it read */
        if (!s->mem && (shipped_name_skips(s->name) || sink->scanned(sink->ctx, SHIP_NONE, place))) continue;
        kind = source_kind(s, place, &id);
        if (kind < 0) {
            w.unread++;
            continue;
        }
        if (kind == SHIPPED_SKIP) { /* no container its first bytes name */
            read_nothing(&w, s, id, place);
            continue;
        }
        if (sink->scanned(sink->ctx, SHIP_FILE, id)) continue;
        if (s->size > FILE_MAX) { /* marked read: no run of this walk reads it */
            w.big++;
            read_nothing(&w, s, id, place);
            continue;
        }
        memcpy(since, w.stages, sizeof since);
        got = source_bytes(&w, s, kind, &d, &owned);
        if (got != BYTES_OK) { /* a Yaz0 file whose start says it has no programs is marked read */
            if (got == BYTES_NONE)
                read_nothing(&w, s, id, place);
            else
                w.unread++;
            continue;
        }
        rc = scan_source(&w, d, (size_t)s->size, kind);
        if (w.found == found) {
            if (rc)
                w.unread++;
            else
                read_nothing(&w, s, id, place);
        } else if (w.counting) {
            if (add_hit(&w, &hit, &nhit, i, kind, id, &owned)) w.unread++;
            note_counted(&w, s->name, since);
        } else {
            note_translated(&w, s->name, before);
            if (rc)
                w.unread++;
            else if (!stopped(&w))
                sink->mark(sink->ctx, SHIP_FILE, id);
        }
        if (owned) {
            free(owned);
            unhold(&w, s->size);
        }
    }
    if (w.counting && !stopped(&w)) { /* each hit's programs translated, now that their number is known */
        __atomic_store_n(&progress->programs, w.found, __ATOMIC_RELAXED);
        w.counting = 0;
        w.again = 1;
        w.found = 0;
        for (i = 0; i < nhit && !stopped(&w); i++) {
            const Source* s = &w.src[hit[i].src];
            u32 before[4] = {w.made, w.known, w.skipped + w.lone, w.failed};
            u8* owned = NULL;
            const u8* d = hit[i].bytes;
            int rc;
            if (d) { /* its kept bytes held again while read, as when counting: a stream in it holds what it did */
                w.kept -= s->size;
                hold(&w, s->size);
            }
            rc = d || source_bytes(&w, s, hit[i].kind, &d, &owned) == BYTES_OK
                     ? scan_source(&w, d, (size_t)s->size, hit[i].kind)
                     : -1;
            note_translated(&w, s->name, before);
            if (rc)
                w.unread++;
            else if (!stopped(&w))
                sink->mark(sink->ctx, SHIP_FILE, hit[i].id);
            if (owned) {
                free(owned);
                unhold(&w, s->size);
            }
            if (hit[i].bytes) {
                free(hit[i].bytes);
                unhold(&w, s->size);
                hit[i].bytes = NULL;
            }
        }
    }
    for (i = 0; i < nhit; i++) free(hit[i].bytes);
    free(hit);
    /* every source read, or never to be by this build: a later run needn't look again. A program that failed to
       translate is no reason to look, as it fails the same way until the build or the driver changes, which starts
       the file over. One not handled for want of memory is (the source is unread) */
    if (!stopped(&w) && !w.unread && romfs != -1) sink->mark(sink->ctx, SHIP_ALL, w.all);
    {
        char line[128];
        counts(line, sizeof line, w.made, w.known, w.skipped + w.lone + w.failed);
        note(&w, "%u shader%s found: %s%s", w.found, w.found == 1 ? "" : "s", line,
             stopped(&w) ? ". The rest couldn't be saved and are made as the game draws them" : "");
        detail(&w, "%u programs: %u translated, %u already translated, %u left out, %u failed", w.found, w.made,
               w.known, w.skipped + w.lone, w.failed);
    }
    if (w.unread)
        note(&w, "%u file%s skipped this time, checked again next launch", w.unread, w.unread == 1 ? "" : "s");
    if (w.big == 1) note(&w, "1 very large file or archive skipped. Its shaders are made as the game draws them");
    if (w.big > 1)
        note(&w, "%u very large files or archives skipped. Their shaders are made as the game draws them", w.big);
    free(w.src);
    return stopped(&w) ? 1 : w.unread || romfs == -1 ? -1 : 0;
}
