/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The file of translated shaders and pipelines made (vk.h's pipeline_file.c). Records are appended as they are made
   and flushed, so a run that crashes keeps what it made, and read once at startup. It calls no Vulkan function:
   pipeline.c makes the modules and pipelines from what it hands back */
#include <stdlib.h>
#include <string.h>
#include "common/utilities/common.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/pipeline_file.h"
#if !defined(_WIN32)
#include <unistd.h> /* getpid */
#endif

#define PF_MAGIC   "SNXVKSH\n"
#define PF_VERSION 2 /* 2: PF_SOURCE records, and a key whose guest part comes first */

/* the check of a payload: words of it after its type and size (FNV-1a over 32-bit words, with a last mix) */
static u64 pf_mix(u64 h, const void* p, size_t n) {
    const u8* b = (const u8*)p;
    size_t i;
    for (i = 0; i + 4 <= n; i += 4) {
        u32 w;
        memcpy(&w, b + i, 4);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}
static u64 pf_start(u32 type, u32 size) {
    u64 h = 0xCBF29CE484222325ull;
    h = (h ^ type) * 0x100000001B3ull;
    return (h ^ size) * 0x100000001B3ull;
}
static u64 pf_end(u64 h) {
    h ^= h >> 31;
    h *= 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}
static u32 pf_get32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}
static u64 pf_get64(const u8* p) {
    u64 v;
    memcpy(&v, p, 8);
    return v;
}

/* the module's slot in the map: its own, or the free one where it would go */
static PfMod* pf_slot(PfMod* map, u32 cap, u64 module) {
    u32 i = (u32)((module * 0x9E3779B97F4A7C15ull) >> 32) & (cap - 1);
    while (map[i].module && map[i].module != module) i = (i + 1) & (cap - 1);
    return &map[i];
}
static void pf_map_add(PFile* pf, u64 module, u32 rec, u64 check) {
    PfMod* m;
    if (!module) return;
    if ((pf->nmap + 1) * 2 > pf->cap) { /* at most half full */
        u32 cap = pf->cap ? pf->cap * 2 : 1024, i;
        PfMod* grown = (PfMod*)calloc(cap, sizeof *grown);
        if (!grown) return; /* pipelines with this module are not recorded */
        for (i = 0; i < pf->cap; i++)
            if (pf->map[i].module) *pf_slot(grown, cap, pf->map[i].module) = pf->map[i];
        free(pf->map);
        pf->map = grown;
        pf->cap = cap;
    }
    m = pf_slot(pf->map, pf->cap, module);
    if (!m->module) pf->nmap++;
    m->module = module;
    m->check = check;
    m->rec = rec;
}

/* the header for key, malloc'd, its size a multiple of 8 */
static u8* pf_header(const char* key, size_t* size) {
    size_t n = strlen(key), total = (16 + n + 7) & ~(size_t)7;
    u8* h = (u8*)calloc(1, total);
    u32 v = PF_VERSION, len = (u32)n;
    if (!h) return NULL;
    memcpy(h, PF_MAGIC, 8);
    memcpy(h + 8, &v, 4);
    memcpy(h + 12, &len, 4);
    memcpy(h + 16, key, n);
    *size = total;
    return h;
}

/* appends a record of pieces (each a multiple of 4 bytes), padded to 8: 0, or -1 when it can't be (the file is full or
   a write failed; nothing more is appended then). Called with pf->lock */
static int pf_append(PFile* pf, u32 type, const void* const* piece, const size_t* len, int pieces, u64* check) {
    static const u8 zero[8] = {0};
    size_t payload = 0, pad;
    u8 head[PF_REC];
    u64 h, max = pf->max ? pf->max : PF_MAX;
    u32 size;
    int i, ok;
    if (!pf->f) return -1;
    for (i = 0; i < pieces; i++) payload += len[i];
    pad = (8 - payload % 8) % 8;
    if (payload + pad > 0xFFFFFFF0u || pf->size + PF_REC + payload + pad > max) {
        LOG("vulkan: %s reached %llu MiB: what is made from now on is not kept for the next run\n", pf->path,
            (unsigned long long)(max >> 20));
        fclose(pf->f);
        pf->f = NULL;
        return -1;
    }
    size = (u32)(payload + pad);
    h = pf_start(type, size);
    for (i = 0; i < pieces; i++) h = pf_mix(h, piece[i], len[i]);
    h = pf_end(pf_mix(h, zero, pad));
    memcpy(head, &type, 4);
    memcpy(head + 4, &size, 4);
    memcpy(head + 8, &h, 8);
    ok = fwrite(head, 1, PF_REC, pf->f) == PF_REC;
    for (i = 0; i < pieces && ok; i++) ok = fwrite(piece[i], 1, len[i], pf->f) == len[i];
    if (ok && pad) ok = fwrite(zero, 1, pad, pf->f) == pad;
    if (ok) ok = fflush(pf->f) == 0;
    if (!ok) { /* a record cut short ends what the next run reads, which is all it can tell */
        char why[256];
        WARN("vulkan: cannot write %s (%s): what is made from now on is not kept\n", pf->path,
             snx_os_error(why, sizeof why));
        fclose(pf->f);
        pf->f = NULL;
        return -1;
    }
    pf->size += PF_REC + size;
    *check = h;
    return 0;
}

u64 pf_shader(PFile* pf, u64 module, const PfShader* s, const u32* cbr, const u32* spv, const PfSource* src,
              const u8* code, const ShXfb* xfb) {
    const void* piece[4];
    size_t len[4];
    u64 check, at = 0;
    int bad = 0;
    pm_lock(&pf->lock);
    if (src) { /* the source first, in the same hold of the lock: the shader record that follows is what it made */
        piece[0] = src;
        len[0] = sizeof *src;
        piece[1] = cbr; /* the shader's reads */
        len[1] = (size_t)src->ncbr * 16;
        piece[2] = code;
        len[2] = src->ncode;
        piece[3] = xfb;
        len[3] = src->flags & PF_SRC_XFB ? sizeof *xfb : 0;
        bad = pf_append(pf, PF_SOURCE, piece, len, 4, &check);
    }
    piece[0] = s;
    len[0] = sizeof *s;
    piece[1] = cbr;
    len[1] = (size_t)s->ncbr * 16;
    piece[2] = spv;
    len[2] = (size_t)s->nspv * 4;
    at = pf->size;
    if (!bad && !pf_append(pf, PF_SHADER, piece, len, 3, &check)) {
        pf->nsh++;
        pf_map_add(pf, module, pf->nsh, check);
    } else
        at = 0;
    pm_unlock(&pf->lock);
    return at;
}

void pf_pipe(PFile* pf, const u64 modules[5], const void* key, u32 keylen) {
    const void* piece[2];
    size_t len[2];
    PfPipe p;
    u64 check;
    int i;
    memset(&p, 0, sizeof p);
    p.keylen = keylen;
    piece[0] = &p;
    len[0] = sizeof p;
    piece[1] = key;
    len[1] = keylen;
    pm_lock(&pf->lock);
    for (i = 0; i < 5; i++) {
        const PfMod* m;
        if (!modules[i]) continue;
        m = pf->cap ? pf_slot(pf->map, pf->cap, modules[i]) : NULL;
        if (!m || !m->module) { /* not a shader of the file (a clear's), or its record failed */
            pm_unlock(&pf->lock);
            return;
        }
        p.sh[i] = m->rec;
        p.check[i] = m->check;
    }
    pf_append(pf, PF_PIPE, piece, len, 2, &check);
    pm_unlock(&pf->lock);
}

void pf_shipref(PFile* pf, u64 module, u64 at, u64 check) {
    u64 words[2], c;
    const void* piece[1];
    size_t len[1];
    words[0] = at;
    words[1] = check;
    piece[0] = words;
    len[0] = sizeof words;
    pm_lock(&pf->lock);
    if (!pf_append(pf, PF_SHIPREF, piece, len, 1, &c)) {
        pf->nsh++;
        pf_map_add(pf, module, pf->nsh, c);
    }
    pm_unlock(&pf->lock);
}
void pf_scan(PFile* pf, u64 what, u64 id) {
    u64 words[2], check;
    const void* piece[1];
    size_t len[1];
    words[0] = what;
    words[1] = id;
    piece[0] = words;
    len[0] = sizeof words;
    pm_lock(&pf->lock);
    pf_append(pf, PF_SCAN, piece, len, 1, &check);
    pm_unlock(&pf->lock);
}

/* the records in buf[at, size) in order, to load: how far they go while they check out */
static size_t pf_read(PFile* pf, const u8* buf, size_t at, size_t size, const PfLoad* load, u32 loaded[2]) {
    /* by shader record number (from 1, a PF_SHADER or a PF_SHIPREF): its check and module, for the pipeline records
       after it */
    struct {
        u64 check, module;
    }* rec = NULL;
    u32 cap = 0, n = 0;
    while (size - at >= PF_REC) {
        u32 type = pf_get32(buf + at), len = pf_get32(buf + at + 4);
        const u8* p = buf + at + PF_REC;
        u64 h;
        if (len % 8 || len > size - at - PF_REC) break;
        h = pf_end(pf_mix(pf_start(type, len), p, len));
        if (h != pf_get64(buf + at + 8)) break;
        if (type == PF_SHADER || (type == PF_SHIPREF && len == 16)) {
            PfShader s;
            u64 made;
            if (type == PF_SHADER) {
                u64 need;
                if (len < sizeof s) break;
                memcpy(&s, p, sizeof s);
                need = sizeof s + (u64)s.ncbr * 16 + (u64)s.nspv * 4;
                if (need > len || len - need >= 8 || s.stage > ST_FS || s.nspv < 5) break;
            }
            if (n + 1 >= cap) {
                u32 c = cap ? cap * 2 : 1024;
                void* grown = realloc(rec, c * sizeof *rec);
                if (!grown) break;
                rec = grown;
                cap = c;
            }
            /* the reads and the words are 8-byte aligned in buf (malloc'd), past the fixed part's multiple of 8. A
               PF_SHIPREF names a shader record of the shipped file, by where it starts there and its check */
            made = type == PF_SHADER ? load->shader(load->ctx, &s, (const u32*)(const void*)(p + sizeof s),
                                                    (const u32*)(const void*)(p + sizeof s + (size_t)s.ncbr * 16))
                                     : load->shipref(load->ctx, pf_get64(p), pf_get64(p + 8));
            n++;
            rec[n].check = h;
            rec[n].module = made;
            if (made) {
                pf_map_add(pf, made, n, h);
                loaded[0]++;
            }
        } else if (type == PF_PIPE) {
            PfPipe q;
            u64 mods[5];
            int i, ok = 1;
            if (len < sizeof q) break;
            memcpy(&q, p, sizeof q);
            if (q.keylen > len - sizeof q || len - sizeof q - q.keylen >= 8) break;
            for (i = 0; i < 5; i++) {
                mods[i] = 0;
                if (!q.sh[i]) continue;
                /* a shader record before it, the one it names (by its check), that was made */
                if (q.sh[i] > n || rec[q.sh[i]].check != q.check[i] || !rec[q.sh[i]].module)
                    ok = 0;
                else
                    mods[i] = rec[q.sh[i]].module;
            }
            if (ok) {
                load->pipe(load->ctx, mods, p + sizeof q, q.keylen);
                loaded[1]++;
            }
        } else if (type != PF_SOURCE) /* a source matters only to a rebuild (pf_rebuild) */
            break;
        at += PF_REC + len;
    }
    pf->nsh = n;
    free(rec);
    return at;
}

/* pf_read's for a file being rebuilt into a new one of another key, which load->source and load->pipe write to: a
   PF_SHADER record stands for the module the PF_SOURCE record before it was translated into again, as its layout
   (ShaderInfo's) is the old build's; a PF_SHIPREF names a record of a shipped file that was started over. Called
   without pf->lock, which the callbacks' writing takes */
static void pf_rebuild(const u8* buf, size_t at, size_t size, const PfLoad* load, u32 loaded[2]) {
    struct {
        u64 check, module;
    }* rec = NULL;
    u32 cap = 0, n = 0;
    u64 source = 0; /* the module of the last PF_SOURCE record, for the PF_SHADER record after it */
    while (size - at >= PF_REC) {
        u32 type = pf_get32(buf + at), len = pf_get32(buf + at + 4);
        const u8* p = buf + at + PF_REC;
        u64 h;
        if (len % 8 || len > size - at - PF_REC) break;
        h = pf_end(pf_mix(pf_start(type, len), p, len));
        if (h != pf_get64(buf + at + 8)) break;
        if (type == PF_SOURCE) {
            PfSource s;
            ShXfb xfb;
            u64 need;
            if (len < sizeof s) break;
            memcpy(&s, p, sizeof s);
            need = sizeof s + (u64)s.ncbr * 16 + s.ncode + (s.flags & PF_SRC_XFB ? sizeof xfb : 0);
            if (need > len || len - need >= 8 || s.stage > ST_FS || s.ncode % 8) break;
            if (s.flags & PF_SRC_XFB) memcpy(&xfb, p + sizeof s + (size_t)s.ncbr * 16 + s.ncode, sizeof xfb);
            source = load->source(load->ctx, &s, (const u32*)(const void*)(p + sizeof s),
                                  p + sizeof s + (size_t)s.ncbr * 16, s.flags & PF_SRC_XFB ? &xfb : NULL);
            if (source) loaded[0]++;
        } else if (type == PF_SHADER || type == PF_SHIPREF) {
            if (n + 1 >= cap) {
                u32 c = cap ? cap * 2 : 1024;
                void* grown = realloc(rec, c * sizeof *rec);
                if (!grown) break;
                rec = grown;
                cap = c;
            }
            n++;
            rec[n].check = h;
            rec[n].module = type == PF_SHADER ? source : 0;
            source = 0;
        } else if (type == PF_PIPE) {
            PfPipe q;
            u64 mods[5];
            int i, ok = 1;
            if (len < sizeof q) break;
            memcpy(&q, p, sizeof q);
            if (q.keylen > len - sizeof q || len - sizeof q - q.keylen >= 8) break;
            for (i = 0; i < 5; i++) {
                mods[i] = 0;
                if (!q.sh[i]) continue;
                if (q.sh[i] > n || rec[q.sh[i]].check != q.check[i] || !rec[q.sh[i]].module)
                    ok = 0;
                else
                    mods[i] = rec[q.sh[i]].module;
            }
            if (ok) {
                load->pipe(load->ctx, mods, p + sizeof q, q.keylen);
                loaded[1]++;
            }
        } else
            break;
        at += PF_REC + len;
    }
    free(rec);
}

/* whether buf (size bytes) is a file of this format whose key's first guest bytes are key's: the size of its header,
   else 0 */
static size_t pf_same_guest(const u8* buf, size_t size, const char* key, size_t guest) {
    u32 v, len;
    if (!guest || size < 16 || memcmp(buf, PF_MAGIC, 8)) return 0;
    v = pf_get32(buf + 8);
    len = pf_get32(buf + 12);
    if (v != PF_VERSION || len < guest || len > size - 16 || memcmp(buf + 16, key, guest)) return 0;
    return ((size_t)16 + len + 7) & ~(size_t)7;
}

void pf_open(PFile* pf, const char* path, const char* key, size_t guest, PfLoad* load, u32 loaded[2]) {
    size_t hsize = 0, size = 0, used = 0, old_head = 0;
    u8 *head = pf_header(key, &hsize), *buf = NULL;
    FILE* f;
    long length = -1;
    char why[256] = "";
    loaded[0] = loaded[1] = 0;
    pm_lock(&pf->lock);
    snprintf(pf->path, sizeof pf->path, "%s", path);
    if (!head) {
        pm_unlock(&pf->lock);
        return;
    }
    f = fopen(path, "rb");
    if (f && !fseek(f, 0, SEEK_END)) length = ftell(f);
    /* a file of another key, or of more than a full one and a record, is of no use */
    if (f && length >= (long)hsize && (unsigned long)length <= 2ul * PF_MAX && !fseek(f, 0, SEEK_SET)) {
        size = (size_t)length;
        buf = (u8*)malloc(size);
        SNX_ZONE_V(z, "shader cache read", size);
        if (buf && fread(buf, 1, size, f) != size) {
            free(buf);
            buf = NULL;
        }
    }
    if (f) fclose(f);
    if (buf && !memcmp(buf, head, hsize)) {
        used = pf_read(pf, buf, hsize, size, load, loaded);
        if (used < size)
            LOG("vulkan: %s: the records past %lu of its %lu bytes were cut short or changed, and are dropped\n", path,
                (unsigned long)used, (unsigned long)size);
    } else if (buf && (old_head = pf_same_guest(buf, size, key, guest)) != 0)
        LOG("vulkan: %s is of another build or driver: its shaders are translated again and its pipelines made "
            "again\n",
            path);
    else if (length > 0)
        LOG("vulkan: %s is of another build, GPU or driver, or unreadable: started over\n", path);
    if (used && used == size) {
        pf->f = fopen(path, "ab");
        if (!pf->f) snx_os_error(why, sizeof why);
    } else { /* started over, or rewritten with the records that checked out (what a crash cut short goes) */
        int ok;
        pf->f = fopen(path, "wb");
        ok = pf->f && fwrite(used ? buf : head, 1, used ? used : hsize, pf->f) == (used ? used : hsize) &&
             !fflush(pf->f);
        if (!ok) snx_os_error(why, sizeof why); /* before fclose's */
        if (!ok && pf->f) {
            fclose(pf->f);
            pf->f = NULL;
        }
        if (!used) {
            pf->nsh = 0;
            used = hsize;
        }
    }
    pf->size = used;
    if (!pf->f)
        WARN("vulkan: cannot write %s (%s): shaders and pipelines made are not kept for the next run\n", path, why);
    free(head);
    pm_unlock(&pf->lock);
    if (old_head) { /* into the file just started over, which the callbacks write to */
        load->rebuild = 1;
        pf_rebuild(buf, old_head, size, load, loaded);
        load->rebuild = 0;
    }
    free(buf);
}

unsigned pf_pid(void) {
#if defined(_WIN32)
    return (unsigned)GetCurrentProcessId();
#else
    return (unsigned)getpid();
#endif
}
void pf_open_mapped(PFile* pf, const char* path, const char* key, PfEach each, void* ctx) {
    size_t hsize = 0;
    u8* head = pf_header(key, &hsize);
    u64 at = 0, size = 0;
    char why[256] = "";
    int mapped;
    FILE* probe;
    pm_lock(&pf->lock);
    snprintf(pf->path, sizeof pf->path, "%s", path);
    if (!head) {
        pm_unlock(&pf->lock);
        return;
    }
    mapped = !plat_file_map(&pf->view, path);
    if (!mapped) snx_os_error(why, sizeof why); /* the map's reason, before the probe's calls */
    if (mapped && pf->view.data) size = pf->view.size;
    if (!mapped && (probe = fopen(path, "rb")) != NULL) { /* there, but not mapped: kept as it is, for a later run */
        fclose(probe);
        WARN("vulkan: cannot map %s (%s): its shaders aren't used this run, nor added to\n", path, why);
        pf->kept = 1;
        free(head);
        pm_unlock(&pf->lock);
        return;
    }
    if (size >= hsize && !memcmp(pf->view.data, head, hsize)) {
        const u8* d = pf->view.data;
        for (at = hsize; size - at >= PF_REC;) {
            u32 type = pf_get32(d + at), len = pf_get32(d + at + 4);
            /* a tail of no known type (zeros a lost write left) ends the records as one cut short does */
            if (len % 8 || len > size - at - PF_REC || type < PF_SHADER || type > PF_SHIPREF) break;
            each(ctx, type, d + at + PF_REC, len, at);
            at += PF_REC + len;
        }
    } else if (size)
        LOG("vulkan: %s is of another build, GPU or driver: started over\n", path);
    if (!at || at < size) {
        /* started over (its header alone), or rewritten without what a crash cut short: written beside it and moved
           over it, so another run that has it mapped keeps reading the old one */
        char* tmp = snx_format("%s.%u.tmp", path, pf_pid());
        FILE* t = tmp ? fopen(tmp, "wb") : NULL;
        const u8* keep = at ? pf->view.data : head;
        size_t n = at ? (size_t)at : hsize;
        int ok = t && fwrite(keep, 1, n, t) == n;
        if (!ok) snx_os_error(why, sizeof why); /* each failure's reason where it happened, before the cleanup's */
        if (t && fclose(t) && ok) {
            snx_os_error(why, sizeof why);
            ok = 0;
        }
        plat_file_unmap(&pf->view);
        if (ok && plat_replace_file(tmp, path)) {
            snx_os_error(why, sizeof why);
            ok = 0;
        }
        if (!ok && tmp) remove(tmp);
        free(tmp);
        if (!at) at = hsize;
        if (!ok) { /* as it was: nothing is appended after what it holds */
            WARN("vulkan: cannot write %s (%s): what is made is not kept for the next run\n", path, why);
            pf->kept = 1;
        }
        if (plat_file_map(&pf->view, path)) memset(&pf->view, 0, sizeof pf->view);
    }
    pf->size = at;
    free(head);
    pm_unlock(&pf->lock);
}
const u8* pf_payload(const PFile* pf, u64 at, u32* len, u64* check) {
    const u8* d = pf->view.data;
    u32 type, n;
    if (!d || at > pf->view.size || pf->view.size - at < PF_REC) return NULL;
    type = pf_get32(d + at);
    n = pf_get32(d + at + 4);
    if (n > pf->view.size - at - PF_REC ||
        pf_end(pf_mix(pf_start(type, n), d + at + PF_REC, n)) != pf_get64(d + at + 8))
        return NULL;
    *len = n;
    *check = pf_get64(d + at + 8);
    return d + at + PF_REC;
}
void pf_unseal(PFile* pf) {
    pm_lock(&pf->lock);
    plat_file_unmap(&pf->view);
    if (!pf->kept && !pf->f) {
        char why[256];
        pf->f = fopen(pf->path, "ab");
        if (!pf->f)
            WARN("vulkan: cannot write %s (%s): what is made is not kept for the next run\n", pf->path,
                 snx_os_error(why, sizeof why));
    }
    pm_unlock(&pf->lock);
}
void pf_close(PFile* pf) {
    pm_lock(&pf->lock);
    if (pf->f) {
        fclose(pf->f);
        pf->f = NULL;
    }
    plat_file_unmap(&pf->view);
    pm_unlock(&pf->lock);
}
void pf_seal(PFile* pf) {
    pm_lock(&pf->lock);
    if (pf->f) {
        fclose(pf->f);
        pf->f = NULL;
    }
    plat_file_unmap(&pf->view);
    if (plat_file_map(&pf->view, pf->path)) memset(&pf->view, 0, sizeof pf->view);
    pm_unlock(&pf->lock);
}
