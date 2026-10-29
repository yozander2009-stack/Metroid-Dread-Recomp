/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's clock (clock.c): the host's monotonic time, counted from the run's start, less the time paused */
#ifndef SNX_CLOCK_H
#define SNX_CLOCK_H
#include "common/cpu/aarch64/cpu.h"

void clock_start(void); /* the run's start: guest time 0 */
u64 g_t0_ns(void);      /* plat_ns() at guest time 0: clock_start's, moved on by the pauses since */
u64 guest_ns(void);     /* nanoseconds since clock_start, less the time paused */
/* a game pause (pause.c): guest time stops at clock_pause, and goes on from there at clock_resume, so the game sees no
   gap. The frontend's thread calls both */
void clock_pause(void);
void clock_resume(void);
void clock_set(u64 ns); /* a save state's load: guest time ns from here */
u64 guest_ticks(void);  /* the same time as the console's 19.2 MHz system tick (svcGetSystemTick, CNTPCT_EL0) */

#endif
