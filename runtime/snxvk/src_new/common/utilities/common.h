/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TOOL_COMMON_H
#define SNX_TOOL_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* C99 implementation; fixed-width types come from <stdint.h>. */
typedef uint32_t SnxU32;
typedef uint64_t SnxU64;
typedef struct {
    const char* repo; /* the data folder: games/, work/, the keys */
    const char* build;
    const char* executable;
    const char* source; /* the checkout the build came from (snx_checkout), to build tools in; NULL without one */
    const char* keys; /* optional prod.keys path supplied with --keys */
    const char* title_keys; /* optional title.keys path supplied with --title-keys */
} SnxPaths;

void* snx_alloc(size_t size);
void* snx_realloc(void* ptr, size_t size);
char* snx_strdup(const char* s);
char* snx_format(const char* fmt, ...);
/* 1 when text ends with ending, ASCII letters of either case alike (a file's extension) */
int snx_ends_with_nocase(const char* text, const char* ending);
/* a shared library (a DLL on Windows, its UTF-8 path widened) loaded with its symbols bound now and kept to itself
   (RTLD_NOW | RTLD_LOCAL); NULL with "cannot load PATH: reason" in error when it can't be */
void* snx_library_open(const char* path, char* error, size_t error_size);
/* a symbol of a library snx_library_open loaded, NULL when it has none of that name */
void* snx_library_symbol(void* library, const char* name);
void snx_library_close(void* library);
/* a path's / turned into \ on Windows, the separator its system shows (C:\Users\...); nothing elsewhere */
void snx_path_native(char* path);
/* the room of a path made in a caller's buffer: Linux's PATH_MAX, past what Windows takes without its long paths */
#define SNX_PATH_SIZE 4096
/* fmt's path into out, of size bytes, its separators the system's (snx_path_native). 0; -1 when it does not fit,
   said with snx_error, out empty */
int snx_path_format(char* out, size_t size, const char* fmt, ...);
/* says a line of error, "scarletnx: " and the line on stderr, or to the sink given to snx_error_sink. Returns 1 */
int snx_error(const char* fmt, ...);
/* an option given by the name an older version had, old (a literal), said once a run with its name now */
void snx_option_renamed(const char* old, const char* now);
/* where snx_error's lines go from now on instead of stderr (the runtime's log, which a game's process on Android has
   no other way to keep them in), NULL for stderr again. Set before the threads that say errors start */
void snx_error_sink(void (*sink)(const char* line));
/* the system's reason for the file or folder call that just failed, into out: strerror(errno), or on Windows the
   text of GetLastError, which its calls set instead of errno */
char* snx_os_error(char* out, size_t size);
/* a, then b, a separator between: one path in the system's separators (snx_path_native) */
char* snx_path_join(const char* a, const char* b);
char* snx_dirname(const char* path);
char* snx_absolute(const char* path);
/* the last part of a path, after its last '/' or '\\' (either, on any host: names from other systems' paths too) */
const char* snx_path_leaf(const char* path);
/* the full path of a game's folder: outdir/title when outdir is given, else title's folder under recompiled/ in repo
   or, without one, in the data folder beside the program argv0 names (snx_data_dir); made first when make is set.
   NULL when it cannot be resolved (or made), the reason in errno (snx_os_error). The caller's to free */
char* snx_game_folder(const char* repo, const char* outdir, const char* argv0, const char* title, int make);
/* a run length in seconds from text ("90", "2.5"): above zero, at most 1e9, nothing after the number. 0 otherwise */
int snx_parse_seconds(const char* text, double* seconds);
/* whether path is a full one, from a root (a drive's or a share's on Windows), not one relative to a working folder */
int snx_path_is_full(const char* path);
char* snx_executable(const char* argv0);
int snx_is_file(const char* path);
int snx_is_dir(const char* path);
int snx_is_link(const char* path);
/* Any existing directory entry, including dangling symlinks and special files. */
int snx_path_exists(const char* path);
int snx_mkdirs(const char* path);
int snx_remove_tree(const char* path);
/* the folder at path taken away when it is empty, and only then (0); nonzero, and nothing touched, when it is not */
int snx_remove_empty_dir(const char* path);
int snx_copy_file(const char* src, const char* dst);
/* size bytes of src from offset into a new dst, in pieces: never the whole range in memory */
int snx_copy_file_range(const char* src, SnxU64 offset, SnxU64 size, const char* dst);
int snx_copy_tree(const char* src, const char* dst);
/* dst a second name of the file src, on the same disk: nonzero when dst is there already, or the disk or system takes
   none (another volume, FAT), for the caller to copy instead */
int snx_link_file(const char* src, const char* dst);
/* link a symbolic link to target, which it names as given: nonzero when it cannot be made (on Windows, without the
   right to make one: developer mode or an administrator) */
int snx_symlink(const char* target, const char* link);
/* on Apple, the Vulkan driver a program's own folders carry, for SDL to load instead of the system's loader: the
   MoltenVK beside it (a game's runtime copy), the app bundle's Frameworks, or the framework in an iOS app. NULL
   elsewhere, and when its folders have none (the system's loader, which the app's own bundle finds by itself) */
char* snx_vulkan_driver(const char* program);
/* put the Vulkan driver program carries in folder, beside the program a game runs there: its copy of the runtime is
   outside the app's bundle, where neither SDL's search nor a system's loader reaches the library. 0, or 1 with the
   reason said. Nothing to do, and 0, where the system's loader serves or the program carries none */
int snx_deploy_vulkan_driver(const char* program, const char* folder);
int snx_path_within(const char* path, const char* parent);
int snx_replace(const char* src, const char* dst);
int snx_list_dir(const char* path, char*** names, size_t* count);
void snx_free_strings(char** strings, size_t count);
unsigned char* snx_read_file(const char* path, size_t* size);
/* Whether the file at path holds a record of size bytes, its first magic_size bytes magic, that accept takes: it is
   given each such record in the file, at any offset, until it takes one. What a compiled module was compiled for,
   read from its descriptor in the file before the module is loaded */
int snx_file_has_record(const char* path, const void* magic, size_t magic_size, size_t size,
                        int (*accept)(const unsigned char* record, const void* context), const void* context);
int snx_write_file(const char* path, const void* data, size_t size);
int snx_write_changed(const char* path, const void* data, size_t size);
char* snx_temp_dir(const char* parent);
/* the system's folder for temporary files: TMPDIR, else /tmp; on Windows GetTempPath's */
char* snx_temp_root(void);
/* Extract to tmpfs: where a game's extraction goes, <temp root>/scarletnx/work/<title ID as snx_title_text writes it>,
   and its RomFS is read from when the game's data folder has none. NULL for a title ID that is no game's own */
char* snx_tmpfs_work(SnxU64 title);
char* snx_user_data_dir(void);
/* Systems that follow the XDG Base Directory spec, where settings, state and caches have folders of their own */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
#define SNX_XDG 1
#endif
enum { SNX_USER_DATA, SNX_USER_CONFIG, SNX_USER_STATE, SNX_USER_CACHE };
/* A portable copy's data folder: this folder beside the AppImage or the program */
#define SNX_PORTABLE_DIR ".scarletnx"
/* iOS: the app (dist/apple), which starts no other process and keeps its data in its Documents folder */
#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define SNX_IOS 1
#endif
#endif
/* An older version's data folder on Android, the shared storage's: a game there still runs as one of a data folder */
#define SNX_ANDROID_DIR "/sdcard/ScarletNX"
/* The user's folder for one kind of file: $XDG_CONFIG_HOME/scarletnx (~/.config), $XDG_STATE_HOME/scarletnx
   (~/.local/state) or $XDG_CACHE_HOME/scarletnx (~/.cache), a relative path ignored. Without SNX_XDG config/, state/
   and cache/ of the data folder. NULL when the environment names no home */
char* snx_user_dir(int kind);
/* 1 for a data folder that keeps each kind of file apart: the user's, a portable copy's (SNX_PORTABLE_DIR), Android's.
   Another folder's games (--repo, --outdir) keep their files in their folders */
int snx_app_dir(const char* folder);
/* Such a data folder's folder for one kind of file: for SNX_USER_STATE and SNX_USER_CACHE the one its config folder's
   app.ini names, [folders] state = and cache = (Settings > App > Folders' Logs and Caches), a full path; else
   snx_app_kind_default's. One app.ini names wrongly is said once */
char* snx_app_kind_dir(const char* app, int kind);
/* Its folder for one kind of file before Folders picks one: the user's as snx_user_dir says, another's config/, state/
   and cache/ in it; the data folder itself for SNX_USER_DATA */
char* snx_app_kind_default(const char* app, int kind);
/* A path in a data folder (snx_app_dir), moved to the same place in its folder of kind: games/X/X.ini of the data
   folder is games/X/X.ini of the config folder. Any other path (another --repo, an --outdir) is returned as it is */
char* snx_user_path(const char* path, int kind);
/* The folder of kind of the launcher's own files (settings.ini and gamecontrollerdb.txt in config, recent.txt in
   state) for a program in folder: the data folder's (snx_app_dir) that folder is in, else the user's. The launcher
   names its data folder, a game its own folder, so a game finds the files of the launcher whose games/ it is in */
char* snx_menu_dir(const char* folder, int kind);
/* The same before Folders picks one: snx_app_kind_default's */
char* snx_menu_kind_default(const char* folder, int kind);
/* 1 while what an older build left at old is still there, for the caller to say each run where it is and that this
   build keeps it at now. Nothing is moved: the player does that */
int snx_left_behind(const char* old, const char* now);
/* 1 for a name the program has as the launcher, not as a game's copy: scarletnx, scarletnx-switch, the
   AppImage's (ScarletNX-x86_64), in any case, without .exe or .AppImage. Started bare under another name it is a
   game's copy, which says what is missing and never opens the menu */
int snx_launcher_name(const char* name);
/* A game's own files in its folder, where an older build kept them, and the kind of folder this one keeps each in: its
   log and threads.txt, its settings, its pipeline cache. A name starting with '.' follows the game's name; the others
   the folder has once, for all its games. The runtime says so for a game it runs, the menu for every game */
#define SNX_GAME_FILES 4
typedef struct {
    const char* name;
    int kind;
} SnxGameFile;
extern const SnxGameFile snx_game_files[SNX_GAME_FILES];
/* work/ of a data folder, the drivers' extractions: the user's is in its cache folder. An older build's
   work/ in the data folder stays, a line on stdout saying so */
char* snx_work_dir(const char* data_dir);
/* recompiled/ of a data folder, where the drivers make its games. An older version made them in games/, which stays
   and is still run from until the player moves it (docs/DEPRECATIONS.md): snx_recompiled_game is game name's folder,
   recompiled/<name>, else games/<name> when only that one is there; snx_recompiled_left says on stderr that games/ is
   still there */
char* snx_recompiled_dir(const char* data_dir);
char* snx_recompiled_game(const char* data_dir, const char* name);
void snx_recompiled_left(const char* data_dir);
/* The folder work/, games/ and the keys are in when a run names none (--repo), and the menu's games/: SNX_PORTABLE_DIR
   beside the AppImage or the program, when it is there (a portable copy); else the user's data folder. NULL when none
   can be named */
char* snx_data_dir(const char* program_dir);
/* SNX_PORTABLE_DIR beside the AppImage or the program in program_dir, there or not; NULL when none can be named */
char* snx_portable_dir(const char* program_dir);
/* The checkout a program run from its build tree (build; NULL: any program) was built from, for what only a checkout
   has: its sources, and what an older version kept in it. NULL for any other program */
char* snx_checkout(const char* checkout, const char* build, const char* program_dir);
/* What an older version kept in the checkout of a build tree's program, its data folder then: games/, work/, the
   keys and scarletnx-gnuabi's sysroot, linux/. snx_checkout_now names where this one keeps each for the data folder data; snx_checkout_left says so on
   stderr for each still there. Nothing is moved: the player does that (docs/DEPRECATIONS.md) */
#define SNX_CHECKOUT_FILES 5
extern const char* const snx_checkout_files[SNX_CHECKOUT_FILES];
char* snx_checkout_now(const char* data, int file);
void snx_checkout_left(const char* checkout, const char* data);
/* The name of a dump's extraction in work/: its file's name without the extension, kept to letters, digits, UTF-8,
   spaces, '-' and '_', trailing spaces cut; "Game" when nothing is left */
char* snx_dump_name(const char* input);
/* Why folder cannot take what the drivers write there, and with run the games run from it: "is not there", "is not a
   folder", "cannot be written" or "is on a disk that runs no programs (noexec)"; NULL when it can */
const char* snx_folder_unfit(const char* folder, int run);
SnxU64 snx_free_space(const char* path);
/* The disk path is on: its bytes free to this user and its size. 0, or -1 with the system's reason (snx_os_error) */
int snx_disk_room(const char* path, SnxU64* free_bytes, SnxU64* total);
/* 1 when a and b are on one disk (one file system), each a folder or one still to be made under the nearest folder
   there: what goes into one takes the other's free space */
int snx_same_disk(const char* a, const char* b);
/* a and b name the same file, through links: 1, else 0 (one of them missing too) */
int snx_same_file(const char* a, const char* b);
SnxU64 snx_file_size(const char* path);
/* path's last modification in seconds since 1970; 0 when there is no file to give one */
SnxU64 snx_file_time(const char* path);
/* path's last modification and access set to seconds since 1970: 0, or nonzero with the system's reason
   (snx_os_error) */
int snx_set_file_time(const char* path, SnxU64 seconds);
int snx_make_executable(const char* path);
unsigned snx_cpu_count(void);
/* seconds on a monotonic clock, for durations */
double snx_seconds(void);
/* Address space for code the process writes and then runs: reserved without access, then given access in whole pages.
   snx_vm_protect commits the pages it makes accessible and decommits them with SNX_VM_NONE on Windows. 0 or -1 */
enum { SNX_VM_NONE, SNX_VM_READ, SNX_VM_READ_WRITE, SNX_VM_READ_EXECUTE };
size_t snx_vm_page_size(void);
/* A run that cannot be killed (the driver on a thread of the app, where an app may start no process) is stopped by
   asking: the player's abort sets this, and the long loops (the recompiler's units) end early once it is set */
void snx_abort_request(int on);
int snx_abort_requested(void);
/* the memory this process is charged for now, in bytes: its footprint on macOS and iOS, its resident set on Linux,
   Android and FreeBSD, its working set on Windows; 0 where the system does not tell */
SnxU64 snx_footprint(void);
/* the processor time this process has used, every thread's together, in ns; 0 where the system does not tell */
SnxU64 snx_process_cpu_ns(void);
/* the memory the system can still give this process, in bytes, into *bytes: what iOS lets the app take before it ends
   it, the memory available on Linux and Android (MemAvailable), on macOS (free, inactive and purgeable pages) and on
   Windows (available physical memory); whether the system told it */
int snx_memory_available(SnxU64* bytes);
void* snx_vm_reserve(size_t size); /* NULL when the system refuses */
int snx_vm_protect(void* p, size_t size, int access);
void snx_vm_release(void* p, size_t size);
/* Makes instructions written to [p, p + size) visible to instruction fetch, once the pages are executable. Another
   core needs nothing more when the pages were never executable before: it cannot hold old instructions for them */
void snx_vm_flush_code(void* p, size_t size);
void snx_tool_console(void);
/* 1 when stdin and stdout are a terminal's, which a person types into and reads */
int snx_terminal(void);
/* The program name names: a path, or a file of that name in a folder of PATH (with .exe on Windows). Its folder is
   resolved and its name kept, as a link's name can say what the program does; NULL when there is none */
char* snx_find_tool(const char* name);
/* argv is NULL terminated. No shell. Captured output is heap-owned by caller.
   home isolates HOME/USERPROFILE and removes XDG_CONFIG_HOME in the child. */
int snx_run(const char* const* argv, const char* cwd, const char* home, char** output);
/* snx_run calling tick(data) about once a second while the child runs: a long tool can say how far it is */
int snx_run_ticking(const char* const* argv, const char* cwd, const char* home, char** output, void (*tick)(void*),
                    void* data);
/* Foreground workflow stage with inherited stdio, interruption forwarding and diagnostics.
   A stopped POSIX child is aborted so a rerun can restart that stage. Not thread safe. */
int snx_run_stage(const char* const* argv, const char* cwd, const char* stage);
/* iOS starts no process: a stage then calls run(argc, argv) in this one, argv[0] being the program a child would be.
   Set once by the driver, whose own main it is */
void snx_set_stage_runner(int (*run)(int argc, char** argv));
/* As above, but intentional debugger stops do not abort the game. */
int snx_run_debug_stage(const char* const* argv, const char* cwd, const char* stage);
FILE* snx_fopen(const char* path, const char* mode);
/* The launcher's logs, in its state folder (docs/USAGE.md#folders): what it says, a file a day named after this one
   (gui/log.h's gui_log_console_name; this name the versions before wrote, kept), and a fatal error of it or of a game
   in the other one too */
#define SNX_CONSOLE_LOG "scarletnx-console.log"
#define SNX_ERROR_LOG   "scarletnx-error.log"
/* text appended to the file at path, each of its lines after the local time and this process's id
   ("2026-10-25 11:30:02 [4242] "), in one write: two programs appending at once keep each line whole and named, and a
   text without an ending newline gets one. 0, or -1 with errno set */
int snx_log_lines(const char* path, const char* text);
/* An exclusive lock on the file at path, made when it is not there and left as it is, held until snx_unlock_file: 0
   with *lock set, 1 when another holder has it (another program, or another lock of this one), -1 with errno. The
   file's bytes stay free to be read and written by anyone: POSIX's flock, which only other locks see, and on Windows a
   lock of one byte past any end the file reaches */
int snx_lock_file(const char* path, intptr_t* lock);
void snx_unlock_file(intptr_t lock);
/* f to offset from its start, past 2 GiB too (fseek takes a long, 32 bits on Windows); 0, or nonzero */
int snx_seek(FILE* f, SnxU64 offset);

/* The game's name in a control.nacp: American English's, else the first language's that has one, in UTF-8 without
   control characters. 1 when it has one, else 0 and "" */
int snx_nacp_name(const unsigned char* nacp, size_t size, char* name, size_t name_size);
/* The title ID of a program without an NPDM (an NRO, an ExeFS without main.npdm): none of its own */
#define SNX_NPDM_DEFAULT_TITLE 0x0100000000001000ull
/* The title ID a main.npdm's ACI0 gives its program, as the loader reads it, for one not loaded; SNX_NPDM_DEFAULT_TITLE
   for none */
SnxU64 snx_npdm_title(const unsigned char* npdm, size_t size);
/* id as a game's folder and files are named, 16 hex digits into out (17 bytes): 1, or 0 for an ID that is none of the
   game's own (0, SNX_NPDM_DEFAULT_TITLE) */
int snx_title_text(SnxU64 id, char* out);
/* text as snx_title_text writes a title ID, 16 hex digits in capitals and nothing more, into *id: 1, else 0 */
int snx_title_parse(const char* text, SnxU64* id);
/* An add-on of a game, an update or a DLC (add-on content), is made in the game's folder beside it and named
   <base>_<its entry>, base the game's title ID: its game is the folder's, recorded as the add-on's content meta named
   it, never worked out from the add-on's own ID. Its entry is its title ID as snx_title_text writes it, and for an
   update v and its version (its content meta's), as every version of a game's update has the same title ID:
   0100ABCD12340800v65536. The game's addons= lists entries. 1 when name is such an add-on of base, its title ID in
   *id when id is not NULL */
int snx_addon_name(const char* base, const char* name, SnxU64* id);
/* 1 when entry is an add-on entry of the game, its title ID in *id when id is not NULL */
int snx_addon_entry_id(SnxU64 game, const char* entry, SnxU64* id);
/* no version known: an update made before versions were kept, or from a dump without a content meta */
#define SNX_NO_VERSION (~(SnxU64)0)
/* the entry of the game's add-on id, an update's of version (SNX_NO_VERSION for none) */
void snx_addon_entry(SnxU64 game, SnxU64 id, SnxU64 version, char entry[32]);
/* the version an update's entry names, SNX_NO_VERSION for none */
SnxU64 snx_addon_version(const char* entry);
enum { SNX_ADDON_NONE, SNX_ADDON_UPDATE, SNX_ADDON_DLC };
/* what the title id is to the game: SNX_ADDON_UPDATE for its patch (its ID with 0x800, as the console makes every
   patch's), SNX_ADDON_DLC for any other, SNX_ADDON_NONE for the game's own (or 0) */
int snx_addon_kind(SnxU64 game, SnxU64 id);
/* the game an add-on's ID would belong to by the usual numbering, for a dump without a content meta to name it: a
   patch's ID less 0x800, add-on content's below its 0x1000 */
SnxU64 snx_addon_guess(SnxU64 id);
/* the ID a game's add-on content is numbered from, its control.nacp's AddOnContentBaseId (aoc:u's base ID): a DLC's
   data ID is it plus the DLC's index, 1 to 2047. The SDK's default for application when nacp is NULL or sets none */
SnxU64 snx_nacp_aoc_base(const unsigned char* nacp, size_t size, SnxU64 application);
/* the display version in a control.nacp ("1.2.0"), printable ASCII into version: 1, or 0 for none */
int snx_nacp_version(const unsigned char* nacp, size_t size, char* version, size_t version_size);
#endif
