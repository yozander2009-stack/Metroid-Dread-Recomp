/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Runs with nobody at the screen (unattended.c), the same for the windowed and the headless backend: a time or frame
   limit, screenshots, scripted button presses. The options are seconds, frames, shot, shot_every, press_len and
   script, given as --key=value or in the .ini's [Other] section, and never written back. Also the
   stick handling the backends share. */
#ifndef SNX_UNATTENDED_H
#define SNX_UNATTENDED_H
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

/* Takes k = v when k is one of the options above: 1, else 0 */
int unattended_ini(const char* k, const char* v);
/* 1 when k is one of the options, changing nothing */
int unattended_known(const char* k);
/* the moment seconds and the millisecond presses of script count from */
void unattended_start(void);
u64 unattended_ms(void); /* milliseconds since then */
/* 1 when seconds or frames limits the run */
int unattended_active(void);
/* The buttons the options press now, as HidNpadButton bits, plus bits 40 to 47 for stick directions (see
   input_key_sticks) */
u64 unattended_buttons(void);
/* a scripted press of this bit is Home: it opens the game's menu and closes it, and while it is open the scripted
   buttons move it as a pad's do (so a run can test the menu) */
#define SNX_SCRIPT_HOME (1ull << 48)
const char* unattended_shot(void); /* the shot path without its extension, or NULL */
/* 1 when shot_every asks for a numbered screenshot now, its path (no extension) written into path */
int unattended_periodic_shot(char* path, size_t n);
int unattended_done(void); /* 1 once the frame count or the time is reached */
/* Ends the run: prints the frame count, the last image's hash and its size (w and h 0 when not known) on stderr, saves
   what a clean exit saves, and exits with 0 */
__attribute__((noreturn)) void unattended_exit(u64 hash, int w, int h);

/* Stick directions held like keys, bits 40 to 47 of k (left, up, right, down of the left stick, then of the right),
   as full deflections in st = {lx, ly, rx, ry}; opposite directions cancel */
void input_key_sticks(u64 k, i32 st[4]);
/* Limits a stick to what a console reports: each axis in -32767..32767, and the point inside the circle of radius
   32767, pulled in along its direction */
void input_stick_clamp(i32* x, i32* y);

#endif
