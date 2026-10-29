/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The functions a save state may hold a pointer to, by a name that holds across builds (state_fns.c): the service
   objects' dispatchers and destructors, and the calls queued for the GPU's thread. A state keeps the name, and a load
   finds the function of that name in whichever build runs it. A function missing from these tables can't be saved:
   the save says so. Each file whose static functions a state may hold lists them in a table of its own, declared in
   its header and listed in state_fns.c */
#ifndef SNX_STATE_FNS_H
#define SNX_STATE_FNS_H
#include "common/cpu/aarch64/cpu.h"

typedef void (*StateFnPtr)(void); /* any function, converted back to its own type before it is called */
typedef struct {
    const char* name;
    StateFnPtr fn;
} StateFn;
typedef struct {
    const StateFn* fn;
    u32 n;
} StateFnTable;
/* a table's entry: the function by the file's name and its own, "fs.file_destroy" */
#define STATE_FN(file, f) {file "." #f, (StateFnPtr)(f)}
/* the table name of a file, of the entries given */
#define STATE_FN_TABLE(name, ...)                                                                                      \
    static const StateFn name##_list[] = {__VA_ARGS__};                                                                \
    const StateFnTable name = {name##_list, (u32)(sizeof name##_list / sizeof *name##_list)}

/* swipc's: every interface's dispatcher by the interface's name (generated, swipc_fns.c) */
extern const StateFnTable g_swipc_fns;

/* fn's name, NULL when no table has it */
const char* state_fn_name(StateFnPtr fn);
/* the function named name, NULL when this build has none of that name */
StateFnPtr state_fn_find(const char* name);

#endif
