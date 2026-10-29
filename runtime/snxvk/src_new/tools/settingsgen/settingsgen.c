/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* settingsgen: generates the settings from src/nx/runtime/settings.txt (its head describes the format): snx_settings.h,
   the struct Settings with a member for every setting (its name in snake_case) and g_settings, the tables of
   nx/runtime/ini.h that read and write them, and the code that hands LLVM's settings to LLVM. An llvm table is
   filled with what the LLVM being built against has: every pass of the pipelines its opt prints for -O0 to -Oz (a
   file of "<level> <pipeline>" lines, --pipelines), on unless turned off, and each option its C API sets on the pass
   builder (LLVMPassBuilderOptionsSet*) and the target machine (LLVMSetTargetMachine*) taking a bool or an integer,
   read from its headers (--llvm-include), LLVM's own value until it is set. It is strict C99 and depends on nothing,
   so a cross build can build it for the machine it runs on.

   Usage: settingsgen --out DIR [--pipelines FILE] [--llvm-include DIR] SETTINGS
   It writes DIR/snx_settings.h, settings.c (g_settings), a settings_<table>.c for each runtime-scope table (whose set
   functions the runtime defines) and settings_runtime.c (the .ini's sections), settings_launch.c (the launcher's and the recompiler's tables
   and sections) and settings_llvm.c (LLVM's pipelines, which pass each toggle turns off, and the options set), each
   only rewritten when it changes. Without --pipelines or --llvm-include, the llvm table has no passes or no options
   of LLVM's. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SETTINGS 1024
#define MAX_TABLES   64
#define MAX_LISTS    8
#define MAX_ITEMS    64
#define MAX_SECTIONS 32
#define MAX_INCLUDES 16
#define DEF_IFS      4 /* a setting's default-if lines */
#define LEVELS       "0123sz"

enum { T_INT, T_FLOAT, T_TEXT };
enum { S_RUNTIME, S_LAUNCHER, S_RECOMPILE, S_LLVM };
/* where an LLVM setting goes: none, a pass of the pipelines, an option of the pass builder or of the target machine */
enum { L_NONE, L_PASS, L_BUILDER, L_MACHINE };

typedef struct {
    char *name, *member, *key, *label, *help, *words, *set, *lo, *hi, *def;
    char *def_if[DEF_IFS], *def_if_value[DEF_IFS]; /* default-if's, the first that holds taken */
    int def_ifs;
    int type, size, once, startup, in_game, recompile, step, shift, list, repeat, block, table, line;
    /* LLVM's: its kind, its ID (a pass's name, a setter's suffix), the pipelines' levels a pass is in (bit per LEVELS
       character), the setter's argument type (LLVMBool, int or unsigned) */
    int llvm, levels;
    char *llvm_id, *llvm_type;
} Setting;

typedef struct {
    char* name;
    int scope;
} Table;

typedef struct {
    char* name;
    char *id[MAX_ITEMS], *label[MAX_ITEMS];
    int n;
} List;

typedef struct {
    char* name;
    char* table[3];
    char* page[3];
    int ntables, backend, launcher;
} Section;

/* an @ID override of an llvm table: its name, key and help for the setting LLVM's ID makes */
typedef struct {
    char *id, *name, *key, *help, *label;
    int table, line;
} Override;

static Setting settings[MAX_SETTINGS];
static int nsettings;
static Override overrides[MAX_SETTINGS];
static int noverrides;
static Table tables[MAX_TABLES];
static int ntables;
static List lists[MAX_LISTS];
static int nlists;
static Section sections[MAX_SECTIONS];
static int nsections;
static char* includes[MAX_INCLUDES];
static int nincludes;
static char* pipelines[6]; /* by LEVELS */
static const char* spec_path;

static char* obuf;
static size_t olen, ocap;

static void fail(int line, const char* fmt, ...) {
    va_list ap;
    if (line)
        fprintf(stderr, "%s:%d: ", spec_path, line);
    else
        fprintf(stderr, "settingsgen: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void* alloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) fail(0, "out of memory");
    return p;
}
static char* dup_n(const char* s, size_t n) {
    char* d = (char*)alloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
static char* dup(const char* s) { return dup_n(s, strlen(s)); }
static char* cat3(const char* a, const char* b, const char* c) {
    size_t na = strlen(a), nb = strlen(b), nc = strlen(c);
    char* d = (char*)alloc(na + nb + nc + 1);
    memcpy(d, a, na);
    memcpy(d + na, b, nb);
    memcpy(d + na + nb, c, nc + 1);
    return d;
}

/* room for n more characters and the terminator */
static void reserve(size_t n) {
    if (olen + n + 1 > ocap) {
        ocap = (olen + n + 1) * 2;
        obuf = (char*)realloc(obuf, ocap);
        if (!obuf) fail(0, "out of memory");
    }
}
static void outs_n(const char* s, size_t n) {
    reserve(n);
    memcpy(obuf + olen, s, n);
    olen += n;
    obuf[olen] = 0;
}
static void outs(const char* s) { outs_n(s, strlen(s)); }
/* fmt, as printf formats it, onto the output */
static void out(const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) fail(0, "out: cannot format %s", fmt);
    reserve((size_t)n);
    va_start(ap, fmt);
    vsnprintf(obuf + olen, (size_t)n + 1, fmt, ap);
    va_end(ap);
    olen += (size_t)n;
}
/* s as a C string literal, NULL as NULL */
static void out_string(const char* s) {
    char c[8];
    if (!s) {
        outs("NULL");
        return;
    }
    outs("\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            c[0] = '\\';
            c[1] = *s;
            c[2] = 0;
        } else if (*s == '\n') {
            strcpy(c, "\\n");
        } else {
            c[0] = *s;
            c[1] = 0;
        }
        outs(c);
    }
    outs("\"");
}

/* written only when it changes, so what includes it recompiles only then (Ninja's restat) */
static void flush(const char* dir, const char* name) {
    char* path = cat3(dir, "/", name);
    FILE* f = fopen(path, "rb");
    if (f) {
        char* old = (char*)alloc(olen + 1);
        size_t got = fread(old, 1, olen + 1, f);
        int same = got == olen && !memcmp(old, obuf, olen);
        free(old);
        fclose(f);
        if (same) {
            olen = 0;
            free(path);
            return;
        }
    }
    f = fopen(path, "wb");
    if (!f || fwrite(obuf, 1, olen, f) != olen || fclose(f)) fail(0, "cannot write %s", path);
    olen = 0;
    free(path);
}

static char* read_file(const char* path, int must) {
    FILE* f = fopen(path, "rb");
    char* text;
    long n = 0;
    if (!f) {
        if (must) fail(0, "cannot read %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) fail(0, "cannot read %s", path);
    text = (char*)alloc((size_t)n + 1);
    if (fread(text, 1, (size_t)n, f) != (size_t)n) fail(0, "cannot read %s", path);
    text[n] = 0;
    fclose(f);
    return text;
}

/* ============================================================ names */

/* NAME in snake_case: letters and digits lowercased, every other run an underscore, none at either end; a camel-case
   word split where its case changes (LoopVectorization, SLPVectorization: loop_vectorization, slp_vectorization) */
static char* snake(const char* name) {
    size_t n = strlen(name), i, o = 0;
    char* s = (char*)alloc(2 * n + 2);
    int gap = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c)) {
            gap = 1;
            continue;
        }
        if (o && isupper(c) && i > 0 &&
            (islower((unsigned char)name[i - 1]) || isdigit((unsigned char)name[i - 1]) ||
             (isupper((unsigned char)name[i - 1]) && i + 1 < n && islower((unsigned char)name[i + 1]))))
            gap = 1;
        if (gap && o) s[o++] = '_';
        gap = 0;
        s[o++] = (char)tolower(c);
    }
    s[o] = 0;
    if (!o) fail(0, "\"%s\" has no letter or digit to name a member by", name);
    if (isdigit((unsigned char)s[0])) {
        memmove(s + 1, s, o + 1);
        s[0] = '_';
    }
    return s;
}
/* a camel-case ID in words, lowercased but its acronyms (LoopVectorization: loop vectorization, SLPVectorization: SLP
   vectorization) */
static char* words_of(const char* id) {
    size_t n = strlen(id), i, o = 0, start;
    char* s = (char*)alloc(2 * n + 1);
    for (i = 0; i < n;) {
        int acronym;
        start = i;
        i++;
        while (i < n && !(isupper((unsigned char)id[i]) &&
                          (islower((unsigned char)id[i - 1]) ||
                           (isupper((unsigned char)id[i - 1]) && i + 1 < n && islower((unsigned char)id[i + 1])))))
            i++;
        acronym = i - start > 1 && isupper((unsigned char)id[start + 1]);
        if (o) s[o++] = ' ';
        for (; start < i; start++) s[o++] = acronym ? id[start] : (char)tolower((unsigned char)id[start]);
    }
    s[o] = 0;
    return s;
}

/* ============================================================ settings.txt */

static char* trim(char* s) {
    char* e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}
/* the first word of s, cut off: the rest, its spaces left out */
static char* split(char* s, char** rest) {
    char* e = s;
    while (*e && *e != ' ' && *e != '\t') e++;
    if (*e) *e++ = 0;
    *rest = trim(e);
    return s;
}
static int find_table(const char* name) {
    int i;
    for (i = 0; i < ntables; i++)
        if (!strcmp(tables[i].name, name)) return i;
    return -1;
}
static int find_list(const char* name) {
    int i;
    for (i = 0; i < nlists; i++)
        if (!strcmp(lists[i].name, name)) return i;
    return -1;
}

static void parse_list(char* rest, int line) {
    List* l;
    char *item, *eq, *p;
    if (nlists == MAX_LISTS) fail(line, "too many lists");
    l = &lists[nlists++];
    l->name = dup(split(rest, &rest));
    while (*rest) {
        item = split(rest, &rest);
        eq = strchr(item, '=');
        if (!eq || eq == item || !eq[1]) fail(line, "a list's instance is id=Label");
        if (l->n == MAX_ITEMS) fail(line, "too many instances in a list");
        l->id[l->n] = dup_n(item, (size_t)(eq - item));
        l->label[l->n] = dup(eq + 1);
        for (p = l->label[l->n]; *p; p++)
            if (*p == '_') *p = ' ';
        l->n++;
    }
    if (!l->n) fail(line, "list %s has no instance", l->name);
}

/* is s's last word w: then cut off */
static int cut_last(char* s, const char* w) {
    size_t n = strlen(s), m = strlen(w);
    if (n <= m || strcmp(s + n - m, w) || (s[n - m - 1] != ' ' && s[n - m - 1] != '\t')) return 0;
    s[n - m] = 0;
    trim(s);
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return 1;
}
static void parse_section(char* rest, int line) {
    Section* s;
    char *colon = strchr(rest, ':'), *p, *item, *slash;
    int last = 0;
    if (!colon) fail(line, "section NAME: TABLE, ...");
    if (nsections == MAX_SECTIONS) fail(line, "too many sections");
    s = &sections[nsections++];
    *colon = 0;
    s->name = dup(trim(rest));
    p = trim(colon + 1);
    for (;;) { /* the flags after the last table */
        if (cut_last(p, "backend"))
            s->backend = 1;
        else if (cut_last(p, "launcher"))
            s->launcher = 1;
        else
            break;
    }
    while (!last) {
        char* end = p + strcspn(p, ",");
        last = !*end;
        *end = 0;
        item = trim(p);
        if (s->ntables == 3) fail(line, "a section has three tables at most");
        slash = strchr(item, '/');
        if (slash) {
            *slash = 0;
            s->page[s->ntables] = dup(trim(slash + 1));
        }
        s->table[s->ntables] = dup(trim(item));
        if (find_table(s->table[s->ntables]) < 0) fail(line, "no table %s", s->table[s->ntables]);
        s->ntables++;
        p = end + 1;
    }
}

static int scope_of(const char* word, int line) {
    if (!*word || !strcmp(word, "runtime")) return S_RUNTIME;
    if (!strcmp(word, "launcher")) return S_LAUNCHER;
    if (!strcmp(word, "recompile")) return S_RECOMPILE;
    if (!strcmp(word, "llvm")) return S_LLVM;
    fail(line, "unknown table scope %s", word);
    return 0;
}

static void add_help(char** help, const char* text) {
    char* joined;
    if (!*help) {
        *help = dup(text);
        return;
    }
    joined = cat3(*help, "\n", text);
    free(*help);
    *help = joined;
}

static void read_spec(const char* path) {
    char* text = read_file(path, 1);
    char *line, *next, *rest, *word;
    int number = 0, table = -1, repeat = 0, in_repeat = 0, blocks = 0, i;
    Setting* s = NULL;
    Override* o = NULL;
    for (line = text; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        number++;
        line = trim(line);
        if (!*line || *line == '#') continue;
        word = split(line, &rest);
        if (!strcmp(word, "include")) {
            if (nincludes == MAX_INCLUDES) fail(number, "too many includes");
            includes[nincludes++] = dup(rest);
            s = NULL;
            o = NULL;
        } else if (!strcmp(word, "list")) {
            parse_list(rest, number);
            s = NULL;
            o = NULL;
        } else if (!strcmp(word, "table")) {
            char* name = split(rest, &rest);
            if (ntables == MAX_TABLES) fail(number, "too many tables");
            if (find_table(name) >= 0) fail(number, "table %s is declared twice", name);
            tables[ntables].name = dup(name);
            tables[ntables].scope = scope_of(rest, number);
            table = ntables++;
            s = NULL;
            o = NULL;
        } else if (!strcmp(word, "repeat")) {
            if (in_repeat) fail(number, "a repeat inside a repeat");
            repeat = atoi(rest);
            if (repeat < 1 || repeat > 64) fail(number, "repeat N takes 1 to 64");
            in_repeat = 1;
            blocks++;
            s = NULL;
            o = NULL;
        } else if (!strcmp(word, "end")) {
            if (!in_repeat) fail(number, "end without a repeat");
            in_repeat = 0;
            repeat = 0;
            s = NULL;
            o = NULL;
        } else if (!strcmp(word, "section")) {
            parse_section(rest, number);
            s = NULL;
            o = NULL;
        } else if (!strcmp(word, "setting")) {
            if (table < 0) fail(number, "a setting before its table");
            if (rest[0] == '@') {
                if (tables[table].scope != S_LLVM) fail(number, "setting @ID is an llvm table's");
                if (noverrides == MAX_SETTINGS) fail(number, "too many @ID settings");
                o = &overrides[noverrides++];
                o->id = dup(rest + 1);
                o->table = table;
                o->line = number;
                s = NULL;
                continue;
            }
            if (nsettings == MAX_SETTINGS) fail(number, "too many settings");
            s = &settings[nsettings++];
            s->name = dup(rest);
            s->member = snake(rest);
            s->table = table;
            s->repeat = repeat;
            s->block = repeat ? blocks : 0;
            s->list = -1;
            s->type = -1;
            s->line = number;
            o = NULL;
        } else if (o) {
            if (!strcmp(word, "name"))
                o->name = dup(rest);
            else if (!strcmp(word, "key"))
                o->key = dup(rest);
            else if (!strcmp(word, "help"))
                add_help(&o->help, rest);
            else if (!strcmp(word, "label"))
                o->label = dup(rest);
            else
                fail(number, "an @ID setting takes name, key, label and help");
        } else if (!s) {
            fail(number, "unknown line %s", word);
        } else if (!strcmp(word, "key")) {
            s->key = dup(rest);
        } else if (!strcmp(word, "int") || !strcmp(word, "float")) {
            char* lo = split(rest, &rest);
            if (!*lo || !*rest) fail(number, "%s LO HI", word);
            s->type = word[0] == 'i' ? T_INT : T_FLOAT;
            s->lo = dup(lo);
            s->hi = dup(rest);
        } else if (!strcmp(word, "text")) {
            s->type = T_TEXT;
            s->size = atoi(rest);
            if (s->size < 2) fail(number, "text SIZE takes 2 or more");
        } else if (!strcmp(word, "default")) {
            s->def = dup(strcmp(rest, "\"\"") ? rest : "");
        } else if (!strcmp(word, "default-if")) {
            char* macro = split(rest, &rest);
            if (!*macro || !*rest) fail(number, "default-if MACRO VALUE");
            if (s->def_ifs == DEF_IFS) fail(number, "a setting takes at most %d default-if", DEF_IFS);
            s->def_if[s->def_ifs] = dup(macro);
            s->def_if_value[s->def_ifs++] = dup(strcmp(rest, "\"\"") ? rest : "");
        } else if (!strcmp(word, "once")) {
            s->once = 1;
        } else if (!strcmp(word, "words")) {
            s->words = dup(rest);
        } else if (!strcmp(word, "step")) {
            s->step = atoi(rest);
        } else if (!strcmp(word, "shift")) {
            s->shift = atoi(rest);
        } else if (!strcmp(word, "label")) {
            s->label = dup(rest);
        } else if (!strcmp(word, "startup")) {
            s->startup = 1;
        } else if (!strcmp(word, "in-game")) {
            s->in_game = 1;
        } else if (!strcmp(word, "recompile")) {
            s->recompile = 1;
        } else if (!strcmp(word, "set")) {
            s->set = dup(rest);
        } else if (!strcmp(word, "each")) {
            s->list = find_list(rest);
            if (s->list < 0) fail(number, "no list %s", rest);
            if (s->repeat) fail(number, "each inside a repeat");
        } else if (!strcmp(word, "help")) {
            add_help(&s->help, rest);
        } else
            fail(number, "unknown line %s", word);
    }
    if (in_repeat) fail(number, "a repeat without its end");
    free(text);
    for (i = 0; i < nsettings; i++) {
        s = &settings[i];
        if (!s->key) fail(s->line, "%s has no key", s->name);
        if (s->type < 0) fail(s->line, "%s has no type (int, float or text)", s->name);
        if (!s->def && s->type == T_TEXT) s->def = dup(""); /* empty text */
        if (!s->def) fail(s->line, "%s has no default", s->name);
        if (s->repeat && !strstr(s->key, "<N>")) fail(s->line, "a repeated setting's key has <N>");
        if (s->list >= 0 && !strstr(s->key, "<ID>")) fail(s->line, "an each setting's key has <ID>");
        if (s->recompile && (tables[s->table].scope != S_RUNTIME || s->repeat || s->list >= 0 || s->def_ifs || s->set))
            fail(s->line, "%s: recompile takes a game's setting of one value, without default-if or set", s->name);
    }
}

/* ============================================================ what LLVM has */

static int level_index(char c) {
    const char* p = strchr(LEVELS, c);
    return c && p ? (int)(p - LEVELS) : -1;
}

/* the passes of a pipeline: each name with no pipeline of its own in parentheses, but require<> and invalidate<> */
static void pipeline_passes(const char* text, int level, char*** names, int** levels, int* n) {
    const char* p = text;
    while (*p) {
        const char* start;
        const char* end;
        int depth;
        while (*p == ',' || *p == '(' || *p == ')' || *p == ' ') p++;
        if (!*p) break;
        start = p;
        while (*p && *p != ',' && *p != '(' && *p != ')' && *p != '<') p++;
        end = p;
        if (*p == '<') { /* its parameters, which may nest */
            depth = 0;
            for (; *p; p++) {
                if (*p == '<') depth++;
                if (*p == '>' && !--depth) {
                    p++;
                    break;
                }
            }
        }
        if (*p != '(' && end > start && !(end - start == 7 && !strncmp(start, "require", 7)) &&
            !(end - start == 10 && !strncmp(start, "invalidate", 10))) {
            int i;
            for (i = 0; i < *n; i++)
                if (strlen((*names)[i]) == (size_t)(end - start) && !strncmp((*names)[i], start, (size_t)(end - start)))
                    break;
            if (i == *n) {
                *names = (char**)realloc(*names, (size_t)(*n + 1) * sizeof **names);
                *levels = (int*)realloc(*levels, (size_t)(*n + 1) * sizeof **levels);
                if (!*names || !*levels) fail(0, "out of memory");
                (*names)[i] = dup_n(start, (size_t)(end - start));
                (*levels)[i] = 0;
                (*n)++;
            }
            (*levels)[i] |= 1 << level;
        }
    }
}

static const Override* override_of(int table, const char* id) {
    int i;
    for (i = 0; i < noverrides; i++)
        if (overrides[i].table == table && !strcmp(overrides[i].id, id)) return &overrides[i];
    return NULL;
}

static Setting* new_llvm(int table, int kind, const char* id, const char* auto_name, const char* key) {
    Setting* s;
    const Override* o = override_of(table, id);
    if (nsettings == MAX_SETTINGS) fail(0, "too many settings with LLVM's");
    s = &settings[nsettings++];
    s->name = dup(o && o->name ? o->name : auto_name);
    s->member = snake(s->name);
    s->label = o && o->label ? dup(o->label) : NULL;
    s->help = o && o->help ? dup(o->help) : NULL;
    s->key = dup(o && o->key ? o->key : key);
    s->table = table;
    s->list = -1;
    s->llvm = kind;
    s->llvm_id = dup(id);
    return s;
}

static void fill_passes(int table) {
    char** names = NULL;
    int *levels = NULL, n = 0, i, l;
    for (l = 0; l < 6; l++)
        if (pipelines[l]) pipeline_passes(pipelines[l], l, &names, &levels, &n);
    for (i = 0; i < n; i++) {
        char* auto_name = cat3("LLVM pass ", names[i], "");
        char* member = snake(names[i]);
        char* key = cat3("llvm_pass_", member, "");
        Setting* s = new_llvm(table, L_PASS, names[i], auto_name, key);
        s->type = T_INT;
        s->lo = dup("0");
        s->hi = dup("1");
        s->def = dup("1");
        s->words = dup("Off|On");
        s->levels = levels[i];
        if (!s->help) {
            char at[64]; /* " -O0, -O1, -O2, -O3, -Os, -Oz" at most */
            char* head;
            int first = 1;
            at[0] = 0;
            for (l = 0; l < 6; l++)
                if (levels[i] & 1 << l) {
                    sprintf(at + strlen(at), "%s -O%c", first ? "" : ",", LEVELS[l]);
                    first = 0;
                }
            head = cat3("LLVM's ", names[i], " pass, in the pipelines of");
            s->help = cat3(head, at, "");
            free(head);
        }
        free(auto_name);
        free(member);
        free(key);
        free(names[i]);
    }
    free(names);
    free(levels);
}

/* each "void <prefix><Name>(<ref> <arg>, <type> <arg>);" of header that takes a bool or an integer. The void and the
   name are on one line or two: clang-format breaks a long declaration after its return type (LLVM 20's headers, with
   LLVM_C_ABI before it) */
static void fill_options(int table, const char* header, const char* prefix, const char* ref, int kind,
                         const char* setter) {
    char* text = read_file(header, 0);
    const char* p;
    size_t np = strlen(prefix);
    if (!text) return;
    for (p = text; (p = strstr(p, prefix)) != NULL; p += np) {
        const char *q = p + np, *name, *type, *r = p;
        size_t nname, ntype;
        char *id, *auto_name, *key, *spaced, *sn;
        Setting* s;
        while (r > text && isspace((unsigned char)r[-1])) r--;
        if (r == p || r < text + 4 || strncmp(r - 4, "void", 4) ||
            (r - 4 > text && (isalnum((unsigned char)r[-5]) || r[-5] == '_')))
            continue;
        name = q;
        while (isalnum((unsigned char)*q)) q++;
        nname = (size_t)(q - name);
        if (!nname || *q != '(') continue;
        q++;
        while (isspace((unsigned char)*q)) q++;
        if (strncmp(q, ref, strlen(ref))) continue;
        q = strchr(q, ',');
        if (!q) continue;
        q++;
        while (isspace((unsigned char)*q)) q++;
        type = q;
        while (isalnum((unsigned char)*q) || *q == '_') q++;
        ntype = (size_t)(q - type);
        if (!((ntype == 8 && !strncmp(type, "LLVMBool", 8)) || (ntype == 3 && !strncmp(type, "int", 3)) ||
              (ntype == 8 && !strncmp(type, "unsigned", 8))))
            continue;
        while (isspace((unsigned char)*q)) q++;
        while (isalnum((unsigned char)*q) || *q == '_') q++; /* the argument's name */
        while (isspace((unsigned char)*q)) q++;
        if (*q != ')') continue; /* more arguments */
        id = dup_n(name, nname);
        spaced = words_of(id);
        auto_name = cat3("LLVM ", spaced, "");
        sn = snake(id);
        key = cat3("llvm_", sn, "");
        s = new_llvm(table, kind, id, auto_name, key);
        s->type = T_INT;
        s->llvm_type = dup_n(type, ntype);
        if (type[0] == 'L') {
            s->lo = dup("0");
            s->hi = dup("1");
            s->words = dup("Off|On");
        } else if (type[0] == 'i') {
            s->lo = dup("INT_MIN + 1");
            s->hi = dup("INT_MAX");
        } else {
            s->lo = dup("0");
            s->hi = dup("INT_MAX");
        }
        s->def = NULL; /* LLVM's own until it is set */
        if (!s->help) s->help = cat3(setter, id, ", LLVM's own value until it is set");
        free(id);
        free(auto_name);
        free(key);
        free(spaced);
        free(sn);
    }
    free(text);
}

static void read_pipelines(const char* path) {
    char* text = read_file(path, 1);
    char *line, *next;
    for (line = text; line && *line; line = next) {
        int l;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        line = trim(line);
        if (!*line) continue;
        l = level_index(line[0]);
        if (l < 0 || (line[1] != ' ' && line[1] != '\t')) fail(0, "%s: a line is \"<level> <pipeline>\"", path);
        pipelines[l] = dup(trim(line + 2));
        if (!*pipelines[l]) pipelines[l] = NULL;
    }
    free(text);
}

/* the llvm tables filled, after what they declare; then every member and key checked unique */
static void fill_llvm(const char* include_dir) {
    int t, i, j;
    for (t = 0; t < ntables; t++) {
        if (tables[t].scope != S_LLVM) continue;
        fill_passes(t);
        if (include_dir) {
            char* builder = cat3(include_dir, "/llvm-c/Transforms/PassBuilder.h", "");
            char* machine = cat3(include_dir, "/llvm-c/TargetMachine.h", "");
            fill_options(t, builder, "LLVMPassBuilderOptionsSet", "LLVMPassBuilderOptionsRef", L_BUILDER,
                         "LLVMPassBuilderOptionsSet");
            fill_options(t, machine, "LLVMSetTargetMachine", "LLVMTargetMachineRef", L_MACHINE,
                         "LLVMSetTargetMachine");
            free(builder);
            free(machine);
        }
    }
    /* an @ID that is no pass of the pipelines and no option of these headers names no setting: said, since another
       LLVM may have it, and a typo would otherwise go unseen */
    for (i = 0; i < noverrides; i++) {
        for (j = 0; j < nsettings; j++)
            if (settings[j].table == overrides[i].table && settings[j].llvm_id &&
                !strcmp(settings[j].llvm_id, overrides[i].id))
                break;
        if (j == nsettings)
            fprintf(stderr, "%s:%d: @%s is no pass of the pipelines nor an option of this LLVM's headers: left out\n",
                    spec_path, overrides[i].line, overrides[i].id);
    }
    for (i = 0; i < nsettings; i++)
        for (j = i + 1; j < nsettings; j++) {
            if (!strcmp(settings[i].member, settings[j].member))
                fail(settings[j].line, "\"%s\" and \"%s\" are both the member %s: give one another name",
                     settings[i].name, settings[j].name, settings[j].member);
            if (!strcmp(settings[i].key, settings[j].key))
                fail(settings[j].line, "%s and %s have the same key %s", settings[i].name, settings[j].name,
                     settings[j].key);
        }
}

/* ============================================================ the generated files */

static const char* c_type(const Setting* s) { return s->type == T_FLOAT ? "float" : s->type == T_TEXT ? "char" : "int"; }
static int count_of(const Setting* s) { return s->repeat ? s->repeat : s->list >= 0 ? lists[s->list].n : 0; }
static int scope_of_setting(const Setting* s) { return tables[s->table].scope; }
static int is_runtime(const Setting* s) { return scope_of_setting(s) == S_RUNTIME; }
static char* upper(const char* s) {
    char* u = dup(s);
    char* p;
    for (p = u; *p; p++) *p = (char)toupper((unsigned char)*p);
    return u;
}

static void header(void) {
    outs("/* Generated by tools/settingsgen from src/nx/runtime/settings.txt: edit that file, not this one */\n");
}

static void write_h(const char* dir) {
    int i, t;
    header();
    outs("#ifndef SNX_SETTINGS_H\n#define SNX_SETTINGS_H\n#include \"nx/runtime/ini.h\"\n#include <limits.h>\n\n");
    outs("/* an LLVM option's value while LLVM's own is kept */\n#define SETTINGS_UNSET INT_MIN\n\n");
    for (i = 0; i < nlists; i++) {
        char* u = upper(lists[i].name);
        out("#define SETTINGS_%s_COUNT %d\n", u, lists[i].n);
        free(u);
    }
    outs("\n/* every setting's value, by its name in snake_case */\ntypedef struct {\n");
    for (i = 0; i < nsettings; i++) {
        const Setting* s = &settings[i];
        int n = count_of(s);
        out("    %s %s", c_type(s), s->member);
        if (n) out("[%d]", n);
        if (s->type == T_TEXT) out("[%d]", s->size);
        out("; /* %s */\n", s->key);
    }
    outs("} Settings;\nextern Settings g_settings;\n\n");
    for (t = 0; t < ntables; t++) out("extern const IniOpt %s[];\n", tables[t].name);
    outs("/* the game's settings the recompile takes too (recompile), in the recompiler's file */\n"
         "extern const IniOpt g_recompile_game_ini[];\n");
    outs("/* the game's .ini's sections, in its order */\nextern const IniSection g_ini_sections[];\n"
         "extern const int g_ini_sections_count;\n");
    outs("/* the launcher's Settings' own groups: its rows and the recompiler's */\n"
         "extern const IniSection g_launch_sections[];\nextern const int g_launch_sections_count;\n\n");
    outs("/* the set functions the tables call, each defined by its module */\n");
    for (i = 0; i < nsettings; i++) {
        int j, seen = 0;
        if (!settings[i].set) continue;
        for (j = 0; j < i; j++)
            if (settings[j].set && !strcmp(settings[j].set, settings[i].set)) seen = 1;
        if (!seen) out("void %s(const IniOpt* o, const char* v);\n", settings[i].set);
    }
    outs("\n#endif\n");
    flush(dir, "snx_settings.h");
}

/* a setting's default as C: the text quoted, a number as it is, LLVM's own SETTINGS_UNSET */
static void out_value(const Setting* s, const char* v) {
    if (!v)
        outs("SETTINGS_UNSET");
    else if (s->type == T_TEXT)
        out_string(v);
    else
        outs(v);
}
static void out_init(const Setting* s, const char* v) {
    int n = count_of(s), i;
    if (!n) {
        out_value(s, v);
        return;
    }
    outs("{");
    for (i = 0; i < n; i++) {
        if (i) outs(", ");
        out_value(s, v);
    }
    outs("}");
}

static void write_c(const char* dir) {
    int i;
    header();
    outs("#include \"snx_settings.h\"\n\n");
    for (i = 0; i < nsettings; i++) {
        const Setting* s = &settings[i];
        if (!s->def_ifs) continue;
        for (int k = 0; k < s->def_ifs; k++) {
            out("#%s defined(%s)\n#define SETTING_INIT_%s ", k ? "elif" : "if", s->def_if[k], s->member);
            out_init(s, s->def_if_value[k]);
            outs("\n");
        }
        out("#else\n#define SETTING_INIT_%s ", s->member);
        out_init(s, s->def);
        outs("\n#endif\n");
    }
    outs("Settings g_settings = {\n");
    for (i = 0; i < nsettings; i++) {
        const Setting* s = &settings[i];
        outs("    ");
        if (s->def_ifs)
            out("SETTING_INIT_%s", s->member);
        else
            out_init(s, s->def);
        out("%s /* %s */\n", i + 1 < nsettings ? "," : "", s->member);
    }
    outs("};\n");
    flush(dir, "settings.c");
}

/* key with <N> or <ID> put in */
static char* key_of(const Setting* s, const char* what, const char* value) {
    const char* at = strstr(s->key, what);
    char* head;
    char* k;
    if (!at) return dup(s->key);
    head = dup_n(s->key, (size_t)(at - s->key));
    k = cat3(head, value, at + strlen(what));
    free(head);
    return k;
}

static void out_row(const Setting* s, int index, const char* key, const char* label) {
    const char* def = s->once ? NULL : s->def;
    out("    {");
    out_string(key);
    outs(", ");
    if (s->def_ifs && def) {
        outs("SETTING_DEF_");
        outs(s->member);
    } else
        out_string(def);
    outs(", ");
    out_string(index ? NULL : s->help);
    out(", %s, %s%s%s, ", s->type == T_INT ? "INI_INT" : s->type == T_FLOAT ? "INI_FLOAT" : "INI_TEXT",
        s->startup ? "INI_STARTUP" : "0", s->in_game ? " | INI_IN_GAME" : "", "");
    if (count_of(s)) {
        if (s->type == T_TEXT)
            out("g_settings.%s[%d]", s->member, index);
        else
            out("&g_settings.%s[%d]", s->member, index);
    } else if (s->type == T_TEXT)
        out("g_settings.%s", s->member);
    else
        out("&g_settings.%s", s->member);
    out(", %s, %s, %s, ", s->type == T_TEXT ? "0" : s->lo, s->type == T_TEXT ? "0" : s->hi, s->set ? s->set : "NULL");
    out_string(label);
    outs(", ");
    out_string(s->words);
    out(", %d, %d, ", s->step, s->shift);
    if (s->type == T_TEXT) {
        if (count_of(s))
            out("sizeof g_settings.%s[0]", s->member);
        else
            out("sizeof g_settings.%s", s->member);
    } else
        outs("0");
    outs("},\n");
}

static void write_table(int t) {
    int i, k, n = 0, first;
    out("const IniOpt %s[] = {\n", tables[t].name);
    for (i = 0; i < nsettings; i = first) {
        const Setting* s = &settings[i];
        first = i + 1;
        if (s->table != t) continue;
        if (s->repeat) { /* the run of settings in this repeat, interleaved */
            int end = i, r;
            while (end < nsettings && settings[end].block == s->block) end++;
            for (r = 0; r < s->repeat; r++)
                for (k = i; k < end; k++) {
                    char num[16], *key;
                    sprintf(num, "%d", r + 1);
                    key = key_of(&settings[k], "<N>", num);
                    out_row(&settings[k], r, key, settings[k].label ? settings[k].label : settings[k].name);
                    free(key);
                    n++;
                }
            first = end;
        } else if (s->list >= 0) {
            const List* l = &lists[s->list];
            for (k = 0; k < l->n; k++) {
                char* key = key_of(s, "<ID>", l->id[k]);
                out_row(s, k, key, l->label[k]);
                free(key);
                n++;
            }
        } else {
            out_row(s, 0, s->key, s->label ? s->label : s->name);
            n++;
        }
    }
    outs("    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0}};\n");
    (void)n;
}

/* the rows of the game's settings the recompile takes too, beside the recompiler's tables: the driver links those, not
   the game's */
static void write_recompile_game(void) {
    int i;
    outs("const IniOpt g_recompile_game_ini[] = {\n");
    for (i = 0; i < nsettings; i++)
        if (settings[i].recompile)
            out_row(&settings[i], 0, settings[i].key, settings[i].label ? settings[i].label : settings[i].name);
    outs("    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0}};\n");
}

static void write_sections(const char* name, int launcher) {
    int i, j, n = 0;
    out("const IniSection %s[] = {\n", name);
    for (i = 0; i < nsections; i++) {
        const Section* s = &sections[i];
        if (s->launcher != launcher) continue;
        outs("    {");
        out_string(s->name);
        out(", %d, {", !s->backend);
        for (j = 0; j < 3; j++) out("%s%s", j ? ", " : "", j < s->ntables ? s->table[j] : "NULL");
        outs("}, {");
        for (j = 0; j < 3; j++) {
            if (j) outs(", ");
            out_string(j < s->ntables ? s->page[j] : NULL);
        }
        outs("}},\n");
        n++;
    }
    if (!n) outs("    {NULL, 0, {NULL, NULL, NULL}, {NULL, NULL, NULL}}\n");
    outs("};\n");
    out("const int %s_count = %d;\n", name, n);
}

static void write_defs(int runtime) {
    int i;
    for (i = 0; i < nsettings; i++) {
        const Setting* s = &settings[i];
        if (!s->def_ifs || s->once || is_runtime(s) != runtime) continue;
        for (int k = 0; k < s->def_ifs; k++) {
            out("#%s defined(%s)\n#define SETTING_DEF_%s ", k ? "elif" : "if", s->def_if[k], s->member);
            out_string(s->def_if_value[k]);
            outs("\n");
        }
        out("#else\n#define SETTING_DEF_%s ", s->member);
        out_string(s->def);
        outs("\n#endif\n");
    }
}

static void table_head(int runtime) {
    int i;
    header();
    outs("#include \"snx_settings.h\"\n");
    for (i = 0; i < nincludes; i++) out("#include %s\n", includes[i]);
    outs("#include <stddef.h>\n\n");
    write_defs(runtime);
}
/* a runtime table's file: settings_<NAME without g_ and _ini>.c, one each, so a program (a test) links only the tables
   it uses, and their set functions */
static char* table_file(const char* name) {
    char* base = dup(name + (strncmp(name, "g_", 2) ? 0 : 2));
    size_t n = strlen(base);
    char* file;
    if (n > 4 && !strcmp(base + n - 4, "_ini")) base[n - 4] = 0;
    file = cat3("settings_", base, ".c");
    free(base);
    return file;
}
static void write_tables(const char* dir, int runtime) {
    int t;
    if (runtime) {
        for (t = 0; t < ntables; t++) {
            char* file;
            if (tables[t].scope != S_RUNTIME) continue;
            table_head(1);
            write_table(t);
            file = table_file(tables[t].name);
            flush(dir, file);
            free(file);
        }
        table_head(1);
        write_sections("g_ini_sections", 0);
        flush(dir, "settings_runtime.c");
        return;
    }
    table_head(0);
    for (t = 0; t < ntables; t++)
        if (tables[t].scope != S_RUNTIME) write_table(t);
    write_recompile_game();
    write_sections("g_launch_sections", 1);
    flush(dir, "settings_launch.c");
}

static void write_llvm(const char* dir) {
    int i, l, n = 0;
    header();
    outs("/* LLVM's settings: its pipelines for -O0 to -Oz as its opt printed them, the pass each toggle turns off, and "
         "the\n   options set on the pass builder and the target machine (nx/runtime/settings_llvm.h) */\n");
    outs("#include \"snx_settings.h\"\n#include \"nx/runtime/settings_llvm.h\"\n#include <llvm-c/TargetMachine.h>\n"
         "#include <llvm-c/Transforms/PassBuilder.h>\n#include <stddef.h>\n#include <string.h>\n\n");
    outs("static const char* const pipelines[6] = {\n");
    for (l = 0; l < 6; l++) {
        outs("    ");
        out_string(pipelines[l]);
        outs(l < 5 ? ",\n" : "\n");
    }
    outs("};\n");
    out("const char* settings_llvm_pipeline(char level) {\n    const char* p = strchr(\"%s\", level);\n"
        "    return level && p ? pipelines[p - \"%s\"] : NULL;\n}\n",
        LEVELS, LEVELS);
    outs("typedef struct {\n    const char* name;\n    size_t at;\n} Pass;\nstatic const Pass passes[] = {\n");
    for (i = 0; i < nsettings; i++)
        if (settings[i].llvm == L_PASS) {
            outs("    {");
            out_string(settings[i].llvm_id);
            out(", offsetof(Settings, %s)},\n", settings[i].member);
            n++;
        }
    if (!n) outs("    {NULL, 0}\n");
    outs("};\n");
    out("int settings_llvm_pass(const Settings* s, const char* name, size_t length) {\n    size_t i;\n"
        "    for (i = 0; i < %d; i++)\n"
        "        if (strlen(passes[i].name) == length && !memcmp(passes[i].name, name, length))\n"
        "            return *(const int*)((const char*)s + passes[i].at) != 0;\n    return -1;\n}\n",
        n);
    outs("void settings_llvm_builder(const Settings* s, LLVMPassBuilderOptionsRef options) {\n");
    for (i = 0; i < nsettings; i++)
        if (settings[i].llvm == L_BUILDER)
            out("    if (s->%s != SETTINGS_UNSET) LLVMPassBuilderOptionsSet%s(options, (%s)s->%s);\n", settings[i].member,
                settings[i].llvm_id, settings[i].llvm_type, settings[i].member);
    outs("    (void)s;\n    (void)options;\n}\n");
    outs("void settings_llvm_machine(const Settings* s, LLVMTargetMachineRef machine) {\n");
    for (i = 0; i < nsettings; i++)
        if (settings[i].llvm == L_MACHINE)
            out("    if (s->%s != SETTINGS_UNSET) LLVMSetTargetMachine%s(machine, (%s)s->%s);\n", settings[i].member,
                settings[i].llvm_id, settings[i].llvm_type, settings[i].member);
    outs("    (void)s;\n    (void)machine;\n}\n");
    flush(dir, "settings_llvm.c");
}

int main(int argc, char** argv) {
    const char *dir = NULL, *pipes = NULL, *include_dir = NULL;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "--pipelines") && i + 1 < argc)
            pipes = argv[++i];
        else if (!strcmp(argv[i], "--llvm-include") && i + 1 < argc)
            include_dir = argv[++i];
        else if (argv[i][0] == '-' || spec_path)
            fail(0, "usage: settingsgen --out DIR [--pipelines FILE] [--llvm-include DIR] SETTINGS");
        else
            spec_path = argv[i];
    }
    if (!dir || !spec_path) fail(0, "usage: settingsgen --out DIR [--pipelines FILE] [--llvm-include DIR] SETTINGS");
    read_spec(spec_path);
    if (pipes) read_pipelines(pipes);
    fill_llvm(include_dir);
    write_h(dir);
    write_c(dir);
    write_tables(dir, 1);
    write_tables(dir, 0);
    write_llvm(dir);
    return 0;
}
