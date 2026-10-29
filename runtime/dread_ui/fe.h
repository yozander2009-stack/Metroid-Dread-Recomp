// Front end internals shared by fe.cpp (the screens) and fe_sys.cpp (files, processes, controllers).
#pragma once
#include <string>
#include <vector>
#include <map>

namespace fe {

// ---------------------------------------------------------------- line-preserving INI editor (keeps comments and unknown keys)
struct IniFile {
    std::string path; std::vector<std::string> lines;
    bool load(const std::string& p);
    bool save() const;
    std::string get(const char* key, const char* def = "") const;   // the runtime finds keys in any section
    bool has(const char* key) const;
    void set(const char* section, const char* key, const std::string& value);
};

// ---------------------------------------------------------------- paths
struct Paths {
    std::string exedir, exename, exe, ini, data, code, build, recompiler, setup_ok, appdata;
    void init(const char* dir, const char* name);
};
extern Paths P;

// ---------------------------------------------------------------- files
bool file_exists(const std::string& p);
bool dir_exists(const std::string& p);
long long file_size(const std::string& p);
bool make_dirs(const std::string& p);
std::string read_text(const std::string& p);
bool write_text(const std::string& p, const std::string& s);
std::string join(const std::string& a, const std::string& b);
std::string utf8(const std::wstring& w);
std::wstring wide(const std::string& s);
std::string pick_folder(void* owner, const char* title);
std::string pick_file(void* owner, const char* title, const wchar_t* filter_name, const wchar_t* filter_spec);
void open_in_explorer(const std::string& p);
void open_in_notepad(const std::string& p);
long long dir_size(const std::string& p, int* files = nullptr);
bool copy_tree(const std::string& src, const std::string& dst, std::string* err, bool overwrite);
bool trees_equal(const std::string& a, const std::string& b);
bool recycle(const std::string& p);
std::string now_stamp();

// ---------------------------------------------------------------- game files
struct GameCheck {
    bool code_ok = false, data_ok = false, exefs_ok = false, romfs_ok = false, setup_ok = false;
    std::string summary;
};
GameCheck check_installed();

// ---------------------------------------------------------------- setup (compile) child process
struct SetupJob {
    std::string base, update;                 // folders chosen by the player
    bool import_from_old = false; std::string old_exe;
    // progress, written by the reader thread
    volatile float pct = 0; volatile bool running = false, done = false, failed = false, cancelled = false;
    std::string stage, message, error; std::vector<std::string> log; double t0 = 0;
    void* proc = nullptr;
};
bool setup_start(SetupJob& j);
void setup_cancel(SetupJob& j);
void setup_lock(); void setup_unlock();
std::string recompiler_python();

// ---------------------------------------------------------------- migration from an existing installation
struct OldInstall {
    std::string exe, ini, saves, mods, shader_cache, vk_cache, sdl, json;   // what exists
    long long save_bytes = 0, mods_bytes = 0, cache_bytes = 0;
};
std::vector<OldInstall> find_old_installs();
OldInstall describe_old(const std::string& exe);
struct MigrateResult { bool ok = true; std::string text; };
MigrateResult migrate(const OldInstall& o, bool settings, bool saves, bool mods, bool caches);

// ---------------------------------------------------------------- saves
bool backup_saves(std::string* out_zip, std::string* err);
std::string cpu_name();
std::string reports_dir();
bool make_report(const std::string& info, std::string* out_zip, std::string* err);   // reports\\Metroid-Dread-report-<time>.zip: logs + settings
std::string backups_dir();

// ---------------------------------------------------------------- mods (LayeredFS folders + load_order.txt, same format as the Python launcher)
struct Mod { std::string name; bool enabled; int files; long long bytes; };
std::vector<Mod> mods_list();
void mods_write(const std::vector<Mod>& m);
bool mod_import_folder(const std::string& src, const std::string& name, std::string* err);
bool mod_import_zip(const std::string& zip, std::string* err);
// the Brazilian Portuguese fan translation (JumpmanClub Brasil): installed as the mod svc_fs.c applies with language 9
extern const char* const PT_PACK_NAME;
extern const char* const PT_PACK_URL;
bool pt_pack_installed();
bool pt_pack_import(const std::string& folder_or_zip, std::string* err);   // the download as it comes (Yuzu layout is fine)
bool mod_remove(const std::string& name);
// cheats (runtime cheats.c): <data>/cheats/*.txt in Atmosphere format; which run is cheats/enabled.txt
struct CheatEntry { std::string name, file; bool on, master; };
std::string cheats_dir();
std::string game_build_id();   // exefs/main's build id, 16 hex digits (Atmosphere's cheat file name)
std::vector<CheatEntry> cheats_list();
void cheats_write(const std::vector<CheatEntry>& c);
bool cheat_import(const std::string& txt, std::string* err);
// button-icon packs (Controls > Button icons): fixed folder names the runtime picks from (svc_fs.c)
struct IconPack { const char* name; const char* label; const char* author; const char* url; };
extern const IconPack ICON_PACKS[3];
bool icon_pack_installed(int i);
bool icon_pack_get(int i, std::string* err);   // downloads it from GameBanana and installs it
int steam_add_shortcut(const std::string& exe, const std::string& dir, std::string& msg);   // fe_steam.cpp: 1 added, 0 already there, -1 error
std::string mods_dir();

// ---------------------------------------------------------------- updates from the GitHub releases (fe_update.cpp)
enum UpStage { UP_IDLE, UP_CHECKING, UP_CURRENT, UP_AVAILABLE, UP_DOWNLOADING, UP_INSTALLED, UP_REBUILD, UP_ERROR };
struct UpdateState {
    UpStage stage = UP_IDLE; std::string installed, latest, what, error; float pct = 0; int files = 0;
    bool rebuild = false;   // a recompiler file changed: the game code must be rebuilt (setup with the game's own _data)
    bool restart = false;   // the EXE changed: start it again to use it
};
std::string installed_version();
void update_check_async();
void update_install_async();
UpdateState update_state();
std::string update_notes_url();
bool update_rollback(std::string* msg, bool* rebuild, bool* restart);
bool update_restart();   // starts the game's EXE again; the caller then exits

// ---------------------------------------------------------------- controllers (front end only; the game has its own)
struct PadInfo { int index; std::string name; };
struct PadState { unsigned long long phys = 0; int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0; std::string name; bool sdl = false; };
void pads_init();
void pads_shutdown();
bool pads_sdl_loaded();
std::vector<PadInfo> pads_list();
PadState pads_read(int api, int index);      // api 0 auto, 1 sdl, 2 xinput; index 0 = any
void pads_rumble(int percent);
void pads_rumble_off();

// ---------------------------------------------------------------- hardware recommendation
struct Hardware {
    std::string gpu; int w = 1920, h = 1080, hz = 60, cores = 4; std::string tier; bool rtx = false;
    int vram_mb = 0, ram_gb = 0; bool integrated = false;   // integrated: under 1 GB of dedicated video memory
};
Hardware detect_hardware();

} // namespace fe
