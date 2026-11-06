/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* HD rumble on Nintendo controllers (Switch Pro Controller, Joy-Con L/R) connected to the PC. The game sends, per side,
   {low-band amplitude, low frequency, high-band amplitude, high frequency} (svc_hid.c). SDL's rumble call only takes
   two motor strengths and plays them at fixed frequencies; these controllers can play the game's own frequencies,
   which is what HD rumble is. So when SDL (2.0.18 or later: its SDL_hid_* functions) finds one, the game's values go
   to it directly as rumble-only output reports (report 0x10: packet counter, then 4 bytes left, 4 bytes right),
   encoded the way the controller expects (frequency: 32*log2(f/10); amplitude: the piecewise log curve), as documented
   by the Switch reverse-engineering notes (dekuNukem, Nintendo_Switch_Reverse_Engineering, rumble_data_table.md) and
   used by SDL itself. Other pads keep SDL's two-motor rumble. [Input] hd_rumble=0 turns it off. */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <wchar.h>
#include "rt.h"

typedef struct HidInfo {   /* SDL_hid_device_info (SDL 2.0.18+) */
    char* path; unsigned short vendor_id, product_id; wchar_t* serial_number; unsigned short release_number;
    wchar_t* manufacturer_string; wchar_t* product_string; unsigned short usage_page, usage;
    int interface_number, interface_class, interface_subclass, interface_protocol; struct HidInfo* next;
} HidInfo;
static HidInfo* (*hdr_enum)(unsigned short, unsigned short);
static void (*hdr_free)(HidInfo*);
static void* (*hdr_open)(const char*, int);
static int (*hdr_write)(void*, const unsigned char*, size_t);
static void (*hdr_close)(void*);
#define HD_MAX 4
static struct { void* dev[HD_MAX]; unsigned short pid[HD_MAX]; int n, tried, len[HD_MAX]; u8 counter; u8 last[8]; u64 sent_ns; } g_hd;
int g_opt_hd_rumble = 1;   /* ini [Input] hd_rumble */

/* one band pair (one side) -> 4 bytes. amp 0..1, frequencies in Hz */
static void hd_encode(const float* v, u8* o) {
    float al = v[0], fl = v[1], ah = v[2], fh = v[3];
    if (!(al > 0.001f) && !(ah > 0.001f)) { o[0] = 0x00; o[1] = 0x01; o[2] = 0x40; o[3] = 0x40; return; }   /* neutral */
    if (!(fl > 0)) fl = 160.f;
    if (!(fh > 0)) fh = 320.f;
    if (fl < 41.f) fl = 41.f; if (fl > 626.f) fl = 626.f;
    if (fh < 82.f) fh = 82.f; if (fh > 1252.f) fh = 1252.f;
    int el = (int)lrintf(log2f(fl / 10.f) * 32.f), eh = (int)lrintf(log2f(fh / 10.f) * 32.f);
    unsigned hf = (unsigned)(eh - 0x60) * 4u;   /* 9 bits */
    unsigned lf = (unsigned)(el - 0x40) & 0x7F; /* 7 bits */
    int a[2]; float in[2] = {ah, al};
    for (int k = 0; k < 2; k++) {
        float x = in[k] > 1.f ? 1.f : in[k];
        int e;
        if (!(x > 0.001f)) e = 0;
        else if (x > 0.23f) e = (int)lrintf(log2f(x * 8.7f) * 32.f);
        else if (x > 0.12f) e = (int)lrintf(log2f(x * 17.f) * 16.f);
        else e = (int)lrintf(4.f * log2f(x / 0.0085f));   /* the table's low end: code 1 ~ 0.010, code 16 ~ 0.117 */
        if (e < (x > 0.001f ? 1 : 0)) e = x > 0.001f ? 1 : 0;   /* a weak effect stays felt */
        if (e > 100) e = 100;
        a[k] = e;
    }
    unsigned hf_amp = (unsigned)a[0] * 2u;              /* high band amplitude: 7 bits, shifted */
    unsigned lf_amp = (unsigned)a[1] / 2u + 0x40u;      /* low band amplitude: its low bit goes to bit 7 of byte 2 */
    o[0] = (u8)(hf & 0xFF);
    o[1] = (u8)(hf_amp | ((hf >> 8) & 1));
    o[2] = (u8)(lf | ((a[1] & 1) ? 0x80 : 0));
    o[3] = (u8)(lf_amp & 0xFF);
}

/* the backend's SDL symbol lookup; 1 = at least one Nintendo controller takes HD rumble */
int hd_rumble_open(void* (*sym)(const char*)) {
    if (g_hd.tried) return g_hd.n > 0;
    g_hd.tried = 1;
    if (!g_opt_hd_rumble || getenv("SWR_NO_HD_RUMBLE")) return 0;
    *(void**)&hdr_enum = sym("SDL_hid_enumerate"); *(void**)&hdr_free = sym("SDL_hid_free_enumeration");
    *(void**)&hdr_open = sym("SDL_hid_open_path"); *(void**)&hdr_write = sym("SDL_hid_write"); *(void**)&hdr_close = sym("SDL_hid_close");
    if (!hdr_enum || !hdr_free || !hdr_open || !hdr_write || !hdr_close) return 0;   /* SDL older than 2.0.18 */
    HidInfo* list = hdr_enum(0x057E, 0);
    for (HidInfo* d = list; d && g_hd.n < HD_MAX; d = d->next) {
        unsigned short p = d->product_id;
        if (p != 0x2009 && p != 0x2006 && p != 0x2007) continue;   /* Pro Controller, Joy-Con L, Joy-Con R */
        void* h = hdr_open(d->path, 0);
        if (!h) continue;
        g_hd.dev[g_hd.n] = h; g_hd.pid[g_hd.n] = p; g_hd.len[g_hd.n] = 0; g_hd.n++;
        LOG("input: HD rumble on %s (the game's own frequencies)\n", p == 0x2009 ? "a Switch Pro Controller" : p == 0x2006 ? "a Joy-Con (L)" : "a Joy-Con (R)");
    }
    if (list) hdr_free(list);
    return g_hd.n > 0;
}

/* each input poll: the game's per-side values (svc_hid.c hid_rumble_hd) to every Nintendo controller found. Sent when
   they change, else every 30 ms while something plays (as SDL refreshes), then once neutral */
void hd_rumble_send(void) {
    if (g_hd.n <= 0) return;
    extern int hid_rumble_hd(float v[2][4]);
    float v[2][4]; int on = hid_rumble_hd(v);
    u8 data[8]; hd_encode(v[0], data); hd_encode(v[1], data + 4);
    u64 now = plat_ns();
    int changed = memcmp(data, g_hd.last, 8) != 0;
    if (!changed && !(on && now - g_hd.sent_ns > 30000000ull)) return;
    memcpy(g_hd.last, data, 8); g_hd.sent_ns = now;
    for (int i = 0; i < g_hd.n; i++) {
        u8 r[64]; memset(r, 0, sizeof r);
        r[0] = 0x10; r[1] = (u8)(g_hd.counter++ & 0x0F);
        if (g_hd.pid[i] == 0x2007) { memcpy(r + 2, data + 4, 4); memcpy(r + 6, data + 4, 4); }       /* right Joy-Con: the right side */
        else if (g_hd.pid[i] == 0x2006) { memcpy(r + 2, data, 4); memcpy(r + 6, data, 4); }          /* left Joy-Con: the left side */
        else memcpy(r + 2, data, 8);                                                                     /* Pro Controller: both */
        if (!g_hd.len[i]) {   /* Bluetooth takes 49-byte reports, USB 64: whichever the device accepts */
            if (hdr_write(g_hd.dev[i], r, 49) >= 0) g_hd.len[i] = 49;
            else if (hdr_write(g_hd.dev[i], r, 64) >= 0) g_hd.len[i] = 64;
            else { LOG("input: HD rumble write failed: back to SDL rumble\n"); hdr_close(g_hd.dev[i]); g_hd.dev[i] = g_hd.dev[--g_hd.n]; i--; }
            continue;
        }
        hdr_write(g_hd.dev[i], r, (size_t)g_hd.len[i]);
    }
}
int hd_rumble_active(void) { return g_hd.n > 0; }

/* test: SWR_TEST_HD_RUMBLE=1 logs a few encodings (no controller needed) */
void hd_rumble_selftest(void) {
    if (!getenv("SWR_TEST_HD_RUMBLE")) return;
    static const float t[5][4] = {{0, 160, 0, 320}, {1, 160, 1, 320}, {0.5f, 80, 0.3f, 640}, {0.1f, 41, 0.05f, 1252}, {0.8f, 626, 0, 82}};
    for (int i = 0; i < 5; i++) { u8 o[4]; hd_encode(t[i], o);
        LOG("hd rumble test: low %.2f @ %.0f Hz, high %.2f @ %.0f Hz -> %02x %02x %02x %02x\n", t[i][0], t[i][1], t[i][2], t[i][3], o[0], o[1], o[2], o[3]); }
}
