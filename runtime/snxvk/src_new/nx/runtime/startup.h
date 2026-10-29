/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the switches read before the game's code loads (startup.c) */
#ifndef SNX_STARTUP_H
#define SNX_STARTUP_H

int snx_startup_options(int argc, char** argv); /* shared startup flags, before any window/error dialog */
extern int g_opt_headless;                      /* startup only, never changed by an INI reload */
extern int g_opt_gdb, g_opt_gdb_stop;
extern int g_opt_tracy_capture;                 /* --tracy-capture: tracy-capture records this run */
extern int g_opt_tracy_client;                  /* --tracy: start the client for a GUI connection */
extern int g_cli_fullscreen; /* -f or --fullscreen[=1]: 1, --fullscreen=0: 0, neither: -1 (the .ini decides) */

#endif
