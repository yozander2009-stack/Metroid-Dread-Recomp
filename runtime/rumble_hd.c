/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* HD rumble: the game's own vibration (svc_hid.c: per side {low amplitude, low Hz, high amplitude, high Hz}) played
   with its frequencies on controllers that can do that, instead of SDL's two fixed-frequency motor strengths:
   - Switch Pro Controller / Joy-Con: rumble-only output report 0x10 (packet counter, 4 bytes left, 4 bytes right),
     encoded as the Switch reverse-engineering notes document (dekuNukem, Nintendo_Switch_Reverse_Engineering,
     rumble_data_table.md: frequency 32*log2(f/10), the piecewise log amplitude curve).
   - DualSense (PS5): its voice coils are driven as sound, two sine waves per side. Over USB through the controller's
     4-channel 48 kHz audio device (channels 3 and 4); over Bluetooth through output report 0x32 (32 stereo frames of
     signed 8-bit samples at 3 kHz per report, CRC32 seeded with 0xA2; layout as documented by egormanga's SAxense).
   - Steam Deck, with [Input] deck_trackpad_rumble=1 (off by default): its trackpad actuators play tones on command
     (feature report 0xEA, MsgTriggerHaptic "tone", per side; SDL's steam/controller_structs.h), the stronger band of
     each side, on top of the grip motors, which keep SDL's normal rumble.
   Pacing, peak merging, the "crisp notes" accent and the DualSense synthesis (x^0.5 curve, 0.8 ceiling, 4 ms
   smoothing, 256-frame slots) follow the measurements and design of RenHDrum by Three-Dola
   (https://github.com/trzz1214/RenHDrum, research notes CC BY 4.0); this is our own C, no RenHDrum code is copied.
   Everything runs on one thread of its own (devices looked for every 5 s, so a controller plugged in later is used).
   Other pads keep SDL's rumble; hd_rumble_covers tells the backends which ones this plays. [Input] hd_rumble=0 = off. */
#include <math.h>
#include <string.h>
#include <stdio.h>
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
static int (*hdr_feature)(void*, const unsigned char*, size_t);
static void (*hdr_close)(void*);
typedef struct { int freq; u16 format; u8 channels, silence; u16 samples, padding; u32 size;
                 void (*callback)(void*, u8*, int); void* userdata; } SdlSpec;   /* SDL_AudioSpec */
static int (*sa_init)(u32);
static int (*sa_count)(int);
static const char* (*sa_name)(int, int);
static u32 (*sa_open)(const char*, int, const SdlSpec*, SdlSpec*, int);
static void (*sa_pause)(u32, int);

int g_opt_hd_rumble = 1;   /* ini [Input] hd_rumble */
int g_opt_deck_pads = 0;   /* ini [Input] deck_trackpad_rumble: Steam Deck trackpads on top of the grip motors */
typedef struct { float al, fl, ah, fh; } Side;
enum { K_SWITCH, K_DS_BT, K_DS_USB, K_DECK };
#define HD_MAX 6
typedef struct {
    void* h; int kind, bt, len; unsigned short pid; char* path;
    u8 counter, last[8]; u64 sent_ns, tok_ns; float tokens; int sounding, crisp[2]; Side pend[2];   /* Switch */
    u64 next_ns, quiet_ns; u8 seq, cnt;                                                              /* DualSense BT */
    u64 deck_ns; int deck_f[2], deck_db[2];                                                          /* Steam Deck */
} Dev;
static Dev g_dev[HD_MAX];
static int g_ndev;
static volatile int g_cover;   /* 1 Nintendo, 2 DualSense, 4 Steam Deck: SDL rumble skipped on those pads */
static void* (*g_sym)(const char*);
static volatile int g_started;
static u64 g_sent;   /* reports / commands that went out (the log says it every 5 s while it changes) */

/* ------------------------------------------------------------------ Switch encoding */
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

static int side_silent(const Side* s) { return !(s->al > 0.001f) && !(s->ah > 0.001f); }
static float side_level(const Side* s) { return s->al + s->ah; }

/* crisp notes (RenHDrum): a note's start is played +3 dB for 10 ms, then -3 dB for `dip` 5 ms slots, then as the game
   sent it; an actuator at rest is slow to start, so this keeps short notes and clicks distinct. n = slots since the start */
static float crisp_gain(int n, int dip) { return n < 0 ? 1.f : n < 2 ? 1.41f : n < 2 + dip ? 0.71f : 1.f; }
static Side crisp_apply(const Side* s, float g) {
    Side r = *s; if (g == 1.f) return r;
    r.al = s->al * g > 1.f ? 1.f : s->al * g; r.ah = s->ah * g > 1.f ? 1.f : s->ah * g;
    if (g > 1.f) { float lim = s->al + s->ah > 1.f ? s->al + s->ah : 1.f, sum = r.al + r.ah; if (sum > lim) { r.al *= lim / sum; r.ah *= lim / sum; } }
    return r;
}
static int g_crisp = 1;

static int sw_write(Dev* d, const u8* data) {
    u8 r[64]; memset(r, 0, sizeof r);
    r[0] = 0x10; r[1] = (u8)(d->counter++ & 0x0F); g_sent++;
    if (d->pid == 0x2007) { memcpy(r + 2, data + 4, 4); memcpy(r + 6, data + 4, 4); }       /* right Joy-Con: the right side */
    else if (d->pid == 0x2006) { memcpy(r + 2, data, 4); memcpy(r + 6, data, 4); }          /* left Joy-Con: the left side */
    else memcpy(r + 2, data, 8);                                                             /* Pro Controller: both */
    if (!d->len) {   /* Bluetooth takes 49-byte reports, USB 64: whichever the device accepts */
        if (hdr_write(d->h, r, 49) >= 0) { d->len = 49; return 1; }
        if (hdr_write(d->h, r, 64) >= 0) { d->len = 64; return 1; }
        return 0;
    }
    return hdr_write(d->h, r, (size_t)d->len) >= 0;
}

/* pacing (RenHDrum's measurements): USB takes a report every 15 ms; refreshed every 50 ms while something plays.
   Bluetooth is half duplex, and a report per game frame crowds out the controller's input reports (input lag,
   disconnects): at most a 3-report burst refilled every 45 ms, 20 ms apart, a steady effect refreshed every 150 ms.
   A stop goes out at once. Between reports the strongest value per side is kept, so a short pulse is not lost */
static int sw_tick(Dev* d, const Side cur[2], u64 now) {
    for (int s = 0; s < 2; s++) if (side_level(&cur[s]) > side_level(&d->pend[s])) d->pend[s] = cur[s];
    u64 since = now - d->sent_ns;
    if (d->bt) { d->tokens += (float)(now - d->tok_ns) / 45e6f; if (d->tokens > 3.f) d->tokens = 3.f; d->tok_ns = now; }
    if (side_silent(&cur[0]) && side_silent(&cur[1]) && side_silent(&d->pend[0]) && side_silent(&d->pend[1])) {
        if (d->sounding == 1 || (d->sounding == 2 && since > 60000000ull)) {   /* neutral, and once more 60 ms later */
            static const u8 z[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
            if (!sw_write(d, z)) return 0;
            memcpy(d->last, z, 8); d->sent_ns = now; d->sounding = d->sounding == 1 ? 2 : 0; d->crisp[0] = d->crisp[1] = -1;
        }
        return 1;
    }
    u8 data[8]; Side out[2];
    for (int s = 0; s < 2; s++) {
        int n = side_silent(&d->pend[s]) ? -1 : d->crisp[s] < 0 ? 0 : d->crisp[s];
        out[s] = g_crisp ? crisp_apply(&d->pend[s], crisp_gain(n < 0 ? -1 : n * 3, d->bt ? 0 : 6)) : d->pend[s];   /* a report = 3 slots */
        hd_encode(&out[s].al, data + 4 * s);
    }
    int changed = memcmp(data, d->last, 8) != 0 || d->sounding != 1;
    int due = changed ? since >= (d->bt ? 20000000ull : 15000000ull) && (!d->bt || d->tokens >= 1.f)
                      : since >= (d->bt ? 150000000ull : 50000000ull);
    if (!due) return 1;
    if (!sw_write(d, data)) return 0;
    if (d->bt && changed) d->tokens -= 1.f;
    memcpy(d->last, data, 8); d->sent_ns = now; d->sounding = 1;
    for (int s = 0; s < 2; s++) { d->crisp[s] = side_silent(&d->pend[s]) ? -1 : d->crisp[s] < 0 ? 1 : d->crisp[s] + (d->crisp[s] < 9); d->pend[s] = cur[s]; }
    return 1;
}

/* ------------------------------------------------------------------ DualSense synthesis */
typedef struct { double ph[2]; float a[2], f[2]; } Voice;
typedef struct { Voice v[2]; Side slot[2]; int crisp[2], left; } Dsp;
#define DS_RATE 48000
static const float k_ds_ceiling = 0.8f, k_ds_smooth = 0.0051948f;   /* 1 - exp(-1000 / (48000 * 4 ms)) */
static Side ds_shape(Side s) {   /* the game's amplitudes -> drive levels: x^0.5, both bands together at most 1 */
    s.al = s.al > 0 ? sqrtf(s.al > 1 ? 1 : s.al) : 0; s.ah = s.ah > 0 ? sqrtf(s.ah > 1 ? 1 : s.ah) : 0;
    if (s.al + s.ah > 1.f) { float k = 1.f / (s.al + s.ah); s.al *= k; s.ah *= k; }
    s.fl = s.fl > 0 ? (s.fl < 20 ? 20 : s.fl > 1000 ? 1000 : s.fl) : 160.f;
    s.fh = s.fh > 0 ? (s.fh < 20 ? 20 : s.fh > 1000 ? 1000 : s.fh) : 320.f;
    return s;
}
static void dsp_init(Dsp* d) { memset(d, 0, sizeof *d); for (int s = 0; s < 2; s++) { d->v[s].f[0] = 160; d->v[s].f[1] = 320; d->crisp[s] = -1; } }
/* frames of 48 kHz into out[frame*stride + ch[side]]; the targets are taken once per 256-frame slot (5.3 ms) */
static void dsp_fill(Dsp* d, const volatile Side* tgt, float* out, int frames, int stride, const int ch[2]) {
    for (int i = 0; i < frames; i++) {
        if (!d->left) {
            for (int s = 0; s < 2; s++) {
                Side raw = {tgt[s].al, tgt[s].fl, tgt[s].ah, tgt[s].fh};
                if (side_silent(&raw)) d->crisp[s] = -1;
                else { if (d->crisp[s] < 0) d->crisp[s] = 0;
                       if (g_crisp) raw = crisp_apply(&raw, crisp_gain(d->crisp[s], 6));
                       if (d->crisp[s] < 8) d->crisp[s]++; }
                d->slot[s] = ds_shape(raw);
            }
            d->left = 256;
        }
        d->left--;
        for (int s = 0; s < 2; s++) {
            Voice* v = &d->v[s]; const Side* t = &d->slot[s];
            v->a[0] += (t->al - v->a[0]) * k_ds_smooth; v->a[1] += (t->ah - v->a[1]) * k_ds_smooth;
            v->f[0] += (t->fl - v->f[0]) * k_ds_smooth; v->f[1] += (t->fh - v->f[1]) * k_ds_smooth;
            v->ph[0] += v->f[0] / (double)DS_RATE; if (v->ph[0] >= 1) v->ph[0] -= 1;
            v->ph[1] += v->f[1] / (double)DS_RATE; if (v->ph[1] >= 1) v->ph[1] -= 1;
            double x = v->a[0] * sin(6.283185307179586 * v->ph[0]) + v->a[1] * sin(6.283185307179586 * v->ph[1]);
            if (x > 1) x = 1; if (x < -1) x = -1;
            out[i * stride + ch[s]] = (float)(x * k_ds_ceiling);
        }
    }
}
static volatile Side g_ds_tgt[2];
static Dsp g_ds_bt_dsp, g_ds_usb_dsp;

static u32 ds_crc(const u8* p, size_t n) {   /* CRC32 (0xEDB88320) seeded with the 0xA2 HID output header */
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i <= n; i++) { c ^= i == 0 ? 0xA2u : p[i - 1]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
/* one Bluetooth haptics report 0x32 (142 bytes): 10.667 ms of sound. 1 = not silent */
static int ds_bt_build(Dev* d, u8* r) {
    static float pcm[512 * 2]; static const int ch[2] = {0, 1};
    dsp_fill(&g_ds_bt_dsp, g_ds_tgt, pcm, 512, 2, ch);
    memset(r, 0, 142);
    r[0] = 0x32; r[1] = (u8)(d->seq << 4);
    r[2] = 0x11 | 0x80; r[3] = 7; r[4] = 0xFE; r[9] = 0xFF; r[10] = d->cnt;   /* packet 0x11: microphone off, buffer length */
    r[11] = 0x12 | 0x80; r[12] = 64;                                          /* packet 0x12: 32 stereo frames */
    int sig = 0;
    for (int f = 0; f < 32; f++) for (int s = 0; s < 2; s++) {
        float m = 0; for (int k = 0; k < 16; k++) m += pcm[(f * 16 + k) * 2 + s];
        long v = lrintf(m / 16.f * 127.f); if (v > 127) v = 127; if (v < -128) v = -128;
        r[13 + f * 2 + s] = (u8)(int8_t)v; sig |= v != 0;
    }
    u32 c = ds_crc(r, 138); r[138] = (u8)c; r[139] = (u8)(c >> 8); r[140] = (u8)(c >> 16); r[141] = (u8)(c >> 24);
    d->seq = (u8)((d->seq + 1) & 0x0F); d->cnt++;
    return sig;
}
static int ds_bt_tick(Dev* d, u64 now) {
    int on = !side_silent((const Side*)&g_ds_tgt[0]) || !side_silent((const Side*)&g_ds_tgt[1]);
    if (on) d->quiet_ns = now;
    else if (now - d->quiet_ns > 200000000ull) { d->next_ns = 0; return 1; }   /* 200 ms after the last sound: nothing sent */
    if (!d->next_ns || (now > d->next_ns && now - d->next_ns > 50000000ull)) d->next_ns = now;   /* start, or fell behind */
    if (now < d->next_ns) return 1;
    u8 r[142]; ds_bt_build(d, r);
    d->next_ns += 10666667ull; g_sent++;
    return hdr_write(d->h, r, sizeof r) >= 0;
}
static void ds_usb_cb(void* u, u8* stream, int len) {
    (void)u; static const int ch[2] = {2, 3};
    float* o = (float*)stream; int frames = len / 16;
    for (int i = 0; i < frames * 4; i++) o[i] = 0;   /* channels 1-2: the speaker / headphones, silent */
    dsp_fill(&g_ds_usb_dsp, g_ds_tgt, o, frames, 4, ch);
}
static u32 g_ds_audio;
static int ds_usb_open(void) {   /* the controller's own audio device: 4 channels, voice coils on 3 and 4 */
    static int tried; if (g_ds_audio) return 1; if (tried) return 0; tried = 1;
    *(void**)&sa_init = g_sym("SDL_InitSubSystem"); *(void**)&sa_count = g_sym("SDL_GetNumAudioDevices");
    *(void**)&sa_name = g_sym("SDL_GetAudioDeviceName"); *(void**)&sa_open = g_sym("SDL_OpenAudioDevice");
    *(void**)&sa_pause = g_sym("SDL_PauseAudioDevice");
    if (!sa_init || !sa_count || !sa_name || !sa_open || !sa_pause || sa_init(0x10) != 0) return 0;
    int n = sa_count(0);
    for (int i = 0; i < n; i++) {
        const char* nm = sa_name(i, 0);
        if (!nm || (!strstr(nm, "DualSense") && !strstr(nm, "Wireless Controller"))) continue;
        SdlSpec want, have; memset(&want, 0, sizeof want);
        want.freq = DS_RATE; want.format = 0x8120; want.channels = 4; want.samples = 256; want.callback = ds_usb_cb;   /* AUDIO_F32 */
        dsp_init(&g_ds_usb_dsp);
        u32 id = sa_open(nm, 0, &want, &have, 0);
        if (!id) continue;
        sa_pause(id, 0); g_ds_audio = id;
        LOG("input: DualSense haptics through its audio device \"%s\"\n", nm);
        return 1;
    }
    LOG("input: a DualSense is connected by USB, but its audio device was not found: plain rumble\n");
    return 0;
}

/* ------------------------------------------------------------------ Steam Deck trackpads */
static int deck_cmd(Dev* d, int side, int on, int freq, int db, int ms) {
    u8 r[65]; memset(r, 0, sizeof r);
    r[1] = 0xEA; r[2] = 19;   /* ID_TRIGGER_HAPTIC_CMD, sizeof MsgTriggerHaptic */
    r[3] = (u8)side; r[4] = on ? 3 : 0; r[5] = 0; r[6] = (u8)(int8_t)db;   /* side 1 L / 2 R, cmd 3 = tone, intensity, dB */
    r[7] = (u8)freq; r[8] = (u8)(freq >> 8); r[9] = (u8)ms; r[10] = (u8)(ms >> 8);
    g_sent++;
    return hdr_feature(d->h, r, sizeof r) >= 0;
}
/* each side plays its stronger band as a tone: sent when it changes (10 ms apart at most), refreshed every 100 ms with
   a 250 ms length, so it ends by itself if we stop; switched off when the game stops */
static int deck_tick(Dev* d, const Side cur[2], u64 now) {
    if (now - d->deck_ns < 10000000ull) return 1;
    for (int s = 0; s < 2; s++) {
        const Side* v = &cur[s]; float pl = v->al > 0 ? sqrtf(v->al) : 0, ph = v->ah > 0 ? sqrtf(v->ah) : 0;
        float p = pl > ph ? pl : ph, f = pl > ph ? (v->fl > 0 ? v->fl : 160) : (v->fh > 0 ? v->fh : 320);
        int fq = 0, db = 0;
        if (p > 0.03f) { fq = (int)(f < 40 ? 40 : f > 1000 ? 1000 : f); db = (int)lrintf(20.f * log10f(p > 1 ? 1 : p)); if (db < -30) db = -30; }
        if (fq == d->deck_f[s] && db == d->deck_db[s] && (!fq || now - d->deck_ns < 100000000ull)) continue;
        if (!deck_cmd(d, s + 1, fq != 0, fq, db, 250)) return 0;
        d->deck_f[s] = fq; d->deck_db[s] = db; d->deck_ns = now;
    }
    return 1;
}

/* ------------------------------------------------------------------ devices and the thread */
static int path_bt(const HidInfo* i) {
#ifdef _WIN32
    return i->path && (strstr(i->path, "00001124-0000-1000-8000-00805f9b34fb") || strstr(i->path, "00001124-0000-1000-8000-00805F9B34FB"));
#else
    const char* b = i->path ? strrchr(i->path, '/') : NULL; if (!b) return i->interface_number < 0;
    char p[128]; snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/uevent", b + 1);
    FILE* f = fopen(p, "r"); if (!f) return 0;
    char line[160]; int bt = 0; while (fgets(line, sizeof line, f)) if (!strncmp(line, "HID_ID=0005:", 12)) bt = 1;
    fclose(f); return bt;
#endif
}
static int known(const char* path) { for (int i = 0; i < g_ndev; i++) if (g_dev[i].path && !strcmp(g_dev[i].path, path)) return 1; return 0; }
static void cover_update(void) {
    int c = 0; for (int i = 0; i < g_ndev; i++) c |= g_dev[i].kind == K_SWITCH ? 1 : g_dev[i].kind == K_DECK ? 4 : 2;
    g_cover = c;
}
static void scan(void) {
    HidInfo* list = hdr_enum(0, 0);
    for (HidInfo* i = list; i && g_ndev < HD_MAX; i = i->next) {
        int kind = -1; unsigned short v = i->vendor_id, p = i->product_id;
        if (v == 0x057E && (p == 0x2009 || p == 0x2006 || p == 0x2007)) kind = K_SWITCH;            /* Pro Controller, Joy-Con L / R */
        else if (v == 0x054C && (p == 0x0CE6 || p == 0x0DF2)) kind = path_bt(i) ? K_DS_BT : K_DS_USB;   /* DualSense, DualSense Edge */
        else if (v == 0x28DE && p == 0x1205 && i->interface_number == 2 && hdr_feature && g_opt_deck_pads) kind = K_DECK;  /* Steam Deck */
        if (kind < 0 || !i->path || known(i->path)) continue;
        if (kind == K_DS_USB && !ds_usb_open()) continue;
        void* h = kind == K_DS_USB ? NULL : hdr_open(i->path, 0);
        if (kind != K_DS_USB && !h) continue;
        Dev* d = &g_dev[g_ndev++]; memset(d, 0, sizeof *d);
        d->h = h; d->kind = kind; d->pid = p; d->bt = kind == K_DS_BT || (kind == K_SWITCH && path_bt(i)); d->path = strdup(i->path);
        d->crisp[0] = d->crisp[1] = -1; d->tok_ns = plat_ns(); d->tokens = 3.f;
        if (kind == K_DS_BT) dsp_init(&g_ds_bt_dsp);
        LOG("input: HD rumble on %s%s\n", kind == K_SWITCH ? (p == 0x2009 ? "a Switch Pro Controller" : p == 0x2006 ? "a Joy-Con (L)" : "a Joy-Con (R)")
            : kind == K_DECK ? "the Steam Deck's trackpads" : "a DualSense", d->bt ? " (Bluetooth)" : kind == K_DECK ? "" : " (USB)");
    }
    if (list) hdr_free(list);
    cover_update();
}
static void drop(int i) {
    LOG("input: HD rumble: a controller is gone or does not take it\n");
    if (g_dev[i].h) hdr_close(g_dev[i].h);
    free(g_dev[i].path);
    g_dev[i] = g_dev[--g_ndev]; cover_update();
}
static void hd_thread(void* a) {
    (void)a; plat_diag_role("hd-rumble");
    u64 last_scan = 0;
    for (;;) {
        u64 now = plat_ns();
        if (!last_scan || now - last_scan > 5000000000ull) { scan(); last_scan = now;
            static u64 said; if (g_sent != said) { LOG("hd rumble: %llu reports sent so far\n", (unsigned long long)g_sent); said = g_sent; } }
        extern int hid_rumble_hd(float v[2][4]);
        float v[2][4]; hid_rumble_hd(v);
        Side cur[2]; for (int s = 0; s < 2; s++) { cur[s].al = v[s][0]; cur[s].fl = v[s][1]; cur[s].ah = v[s][2]; cur[s].fh = v[s][3]; }
        for (int s = 0; s < 2; s++) g_ds_tgt[s] = cur[s];
        for (int i = 0; i < g_ndev; i++) {
            Dev* d = &g_dev[i]; int ok = 1;
            if (d->kind == K_SWITCH) ok = sw_tick(d, cur, now);
            else if (d->kind == K_DS_BT) ok = ds_bt_tick(d, now);
            else if (d->kind == K_DECK) ok = deck_tick(d, cur, now);
            if (!ok) { drop(i); i--; }
        }
        plat_sleep_ns(g_ndev ? 1000000 : 20000000);
    }
}

/* the backend's SDL symbol lookup (SDL 2.0.18+ for SDL_hid_*); starts the thread once */
int hd_rumble_open(void* (*sym)(const char*)) {
    if (g_started) return g_cover != 0;
    g_started = 1;
    if (!g_opt_hd_rumble || getenv("SWR_NO_HD_RUMBLE")) return 0;
    if (getenv("SWR_HD_NO_CRISP")) g_crisp = 0;
    g_sym = sym;
    *(void**)&hdr_enum = sym("SDL_hid_enumerate"); *(void**)&hdr_free = sym("SDL_hid_free_enumeration");
    *(void**)&hdr_open = sym("SDL_hid_open_path"); *(void**)&hdr_write = sym("SDL_hid_write"); *(void**)&hdr_close = sym("SDL_hid_close");
    *(void**)&hdr_feature = sym("SDL_hid_send_feature_report");
    if (!hdr_enum || !hdr_free || !hdr_open || !hdr_write || !hdr_close) return 0;   /* SDL older than 2.0.18 */
    plat_thread(hd_thread, NULL, 256 * 1024);
    return 0;
}
void hd_rumble_send(void) {}   /* the thread does it */
int hd_rumble_active(void) { return g_cover != 0; }
/* 1 = HD rumble plays this SDL pad (SDL_GameControllerType), so the backend sends it no SDL rumble. The Steam Deck's
   grip motors always keep SDL's rumble: its trackpads only add to it */
int hd_rumble_covers(int type, const char* name) {
    (void)name; int c = g_cover;
    if ((c & 1) && (type == 5 || type == 11 || type == 12 || type == 13)) return 1;
    if ((c & 2) && type == 7) return 1;
    return 0;
}

/* test: SWR_TEST_HD_RUMBLE=1 logs a few encodings and DualSense reports (no controller needed) */
void hd_rumble_selftest(void) {
    if (!getenv("SWR_TEST_HD_RUMBLE")) return;
    static const float t[5][4] = {{0, 160, 0, 320}, {1, 160, 1, 320}, {0.5f, 80, 0.3f, 640}, {0.1f, 41, 0.05f, 1252}, {0.8f, 626, 0, 82}};
    for (int i = 0; i < 5; i++) { u8 o[4]; hd_encode(t[i], o);
        LOG("hd rumble test: low %.2f @ %.0f Hz, high %.2f @ %.0f Hz -> %02x %02x %02x %02x\n", t[i][0], t[i][1], t[i][2], t[i][3], o[0], o[1], o[2], o[3]); }
    Dev d; memset(&d, 0, sizeof d); u8 r[142];
    Side z = {0, 160, 0, 320}, a = {1, 160, 0, 320}, b = {0, 160, 0.5f, 320};
    dsp_init(&g_ds_bt_dsp); g_ds_tgt[0] = z; g_ds_tgt[1] = z;
    ds_bt_build(&d, r);
    LOG("hd rumble test: DualSense silent report crc %02x%02x%02x%02x (expect 26e20c96)\n", r[141], r[140], r[139], r[138]);
    g_ds_tgt[0] = a; g_ds_tgt[1] = b;
    int peak = 0; for (int k = 0; k < 20; k++) { ds_bt_build(&d, r); for (int j = 13; j < 77; j++) { int s = (int8_t)r[j]; if (s < 0) s = -s; if (s > peak) peak = s; } }
    LOG("hd rumble test: DualSense loud reports peak %d (ceiling 102), seq %d\n", peak, r[1] >> 4);
    g_ds_tgt[0] = z; g_ds_tgt[1] = z;
}
