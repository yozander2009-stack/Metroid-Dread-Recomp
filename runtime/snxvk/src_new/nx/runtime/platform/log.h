/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's log and its fatal error, both in plat.c */
#ifndef SNX_LOG_H
#define SNX_LOG_H
#include <stdio.h>

/* the .ini's log key: 1 normal, 2 also the LOGV lines (and every line on stderr) */
#include "snx_settings.h" /* g_settings.log_level, the .ini's log */

/* lvl 0 is a warning, 1 a normal line, 2 a verbose one. Checked like printf */
void logf_(int lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#define WARN(...) logf_(0, __VA_ARGS__)
#define LOG(...) logf_(1, __VA_ARGS__)
#define LOGV(...)                                                                                                      \
    do {                                                                                                               \
        if (g_settings.log_level >= 2) logf_(2, __VA_ARGS__);                                                                     \
    } while (0)

/* Say something at most n times per call site, for a message a game could otherwise trigger every frame. Each site
   counts in its own static int (relaxed: only the count matters), and stops adding once past the limit */
#define SNX_LOG_AT_MOST_(said, n, lvl, ...)                                                                            \
    do {                                                                                                               \
        static int said;                                                                                               \
        if (__atomic_load_n(&said, __ATOMIC_RELAXED) < (n) && __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < (n))    \
            logf_(lvl, __VA_ARGS__);                                                                                   \
    } while (0)
#define LOG_LIMIT(n, ...) SNX_LOG_AT_MOST_(log_limit_said_, n, 1, __VA_ARGS__)
#define WARN_LIMIT(n, ...) SNX_LOG_AT_MOST_(warn_limit_said_, n, 0, __VA_ARGS__)

/* snx_error's sink (common.h) for the runtime: the line as a warning, in the log and on stderr */
void log_error_line(const char* line);

/* Logs "FATAL: ..." and ends the process, after the error box where there is one */
void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* While set, the calling thread's lines go to this file and nowhere else (the main loop's threads.txt dump). It is
   per thread so the others keep writing the log during the dump */
extern __thread FILE* tls_log_redirect;

/* log_max in MiB once, after the log first went past it and was cut (the oldest lines after the run's start gone), for
   the frontend to say over the game; 0 before that and after it was taken */
int log_full_take(void);

/* Called by the crash handler before its report: from now on lines bypass the log's lock and stdio buffering, which
   the crashed code may hold, and this thread's redirect is dropped */
void log_crash_mode(void);

#endif
