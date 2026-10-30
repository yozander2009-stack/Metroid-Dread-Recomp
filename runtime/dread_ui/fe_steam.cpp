// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
// any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
// Play screen > Add to Steam: a non-Steam game shortcut in Steam's userdata/<account>/config/shortcuts.vdf (binary
// KeyValues: 0x00 map, 0x01 string, 0x02 int32, 0x07 uint64, 0x08 end of map). The file is parsed and written back
// whole, unknown entries kept as they are; shortcuts.vdf.bak holds the previous file. Steam rewrites the file when it
// exits, so a running Steam is shut down first and started again afterwards. The file format is the same on Linux;
// finding Steam, its account and stopping / starting it differ (below, per system).
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cstring>
#include <cstdlib>
#define _stricmp strcasecmp
#endif
#include <string>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstdint>

namespace {
struct Node {
    int type = 0;                          // 0 map, 1 string, 2 int32, 7 uint64, others kept raw
    std::string key, str;
    uint32_t i32 = 0; uint64_t u64 = 0;
    std::vector<std::unique_ptr<Node>> kids;
};
bool read_str(const std::string& d, size_t& p, std::string& out) {
    size_t e = d.find('\0', p); if (e == std::string::npos) return false;
    out = d.substr(p, e - p); p = e + 1; return true; }
bool parse_map(const std::string& d, size_t& p, Node& m) {   // children until 0x08
    while (p < d.size()) {
        int t = (unsigned char)d[p++];
        if (t == 8) return true;
        auto n = std::make_unique<Node>(); n->type = t;
        if (!read_str(d, p, n->key)) return false;
        if (t == 0) { if (!parse_map(d, p, *n)) return false; }
        else if (t == 1) { if (!read_str(d, p, n->str)) return false; }
        else if (t == 2) { if (p + 4 > d.size()) return false; memcpy(&n->i32, d.data() + p, 4); p += 4; }
        else if (t == 7) { if (p + 8 > d.size()) return false; memcpy(&n->u64, d.data() + p, 8); p += 8; }
        else return false;                 // a type we don't know: refuse rather than corrupt the file
        m.kids.push_back(std::move(n));
    }
    return false; }
void write_map(std::string& o, const Node& m) {
    for (auto& n : m.kids) {
        o += (char)n->type; o += n->key; o += '\0';
        if (n->type == 0) write_map(o, *n);
        else if (n->type == 1) { o += n->str; o += '\0'; }
        else if (n->type == 2) o.append((const char*)&n->i32, 4);
        else o.append((const char*)&n->u64, 8);
    }
    o += (char)8; }
Node* child(Node& m, const char* key) { for (auto& n : m.kids) if (_stricmp(n->key.c_str(), key) == 0) return n.get(); return nullptr; }
void add_str(Node& m, const char* k, const std::string& v) { auto n = std::make_unique<Node>(); n->type = 1; n->key = k; n->str = v; m.kids.push_back(std::move(n)); }
void add_int(Node& m, const char* k, uint32_t v) { auto n = std::make_unique<Node>(); n->type = 2; n->key = k; n->i32 = v; m.kids.push_back(std::move(n)); }
uint32_t crc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) { c ^= b; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c; }
}   // namespace

namespace fe {
// adds the shortcut to one shortcuts.vdf (created if missing). 1 added, 0 already there, -1 error (msg says why)
int steam_vdf_add(const std::string& path, const std::string& name, const std::string& exe, const std::string& dir, std::string& msg,
                  const std::string& icon) {
    std::string d;
    if (FILE* f = fopen(path.c_str(), "rb")) { char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.append(buf, n); fclose(f); }
    Node root;
    if (!d.empty()) { size_t p = 0; if (!parse_map(d, p, root)) { msg = "Steam's shortcut list has a format this version doesn't know; nothing was changed."; return -1; } }
    Node* sc = child(root, "shortcuts");
    if (!sc) { auto n = std::make_unique<Node>(); n->type = 0; n->key = "shortcuts"; sc = n.get(); root.kids.push_back(std::move(n)); }
    std::string qexe = "\"" + exe + "\"";
    for (auto& e : sc->kids) if (e->type == 0) { Node* x = child(*e, "Exe"); if (x && _stricmp(x->str.c_str(), qexe.c_str()) == 0) { msg = "Metroid Dread is already in Steam."; return 0; } }
    int idx = 0; for (auto& e : sc->kids) { int v = atoi(e->key.c_str()); if (v + 1 > idx) idx = v + 1; }
    auto e = std::make_unique<Node>(); e->type = 0; e->key = std::to_string(idx);
    add_int(*e, "appid", crc32(qexe + name) | 0x80000000u);
    add_str(*e, "AppName", name); add_str(*e, "Exe", qexe); add_str(*e, "StartDir", "\"" + dir + "\"");
    add_str(*e, "icon", icon.empty() ? exe : icon); add_str(*e, "ShortcutPath", ""); add_str(*e, "LaunchOptions", "");
    add_int(*e, "IsHidden", 0); add_int(*e, "AllowDesktopConfig", 1); add_int(*e, "AllowOverlay", 1);
    add_int(*e, "OpenVR", 0); add_int(*e, "Devkit", 0); add_str(*e, "DevkitGameID", ""); add_int(*e, "DevkitOverrideAppID", 0);
    add_int(*e, "LastPlayTime", 0); add_str(*e, "FlatpakAppID", "");
    { auto t = std::make_unique<Node>(); t->type = 0; t->key = "tags"; e->kids.push_back(std::move(t)); }
    sc->kids.push_back(std::move(e));
    std::string o; write_map(o, root);
    if (!d.empty()) { std::string bak = path + ".bak"; if (FILE* b = fopen(bak.c_str(), "wb")) { fwrite(d.data(), 1, d.size(), b); fclose(b); } }
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb"); if (!f) { msg = "Could not write Steam's shortcut list."; return -1; }
    bool ok = fwrite(o.data(), 1, o.size(), f) == o.size(); ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp.c_str()); msg = "Could not write Steam's shortcut list."; return -1; }
#else
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) { remove(tmp.c_str()); msg = "Could not write Steam's shortcut list."; return -1; }
#endif
    msg = "Added to Steam."; return 1; }

#ifdef _WIN32

static bool steam_running(DWORD* pid) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (s == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe; pe.dwSize = sizeof pe; bool found = false;
    for (BOOL ok = Process32First(s, &pe); ok; ok = Process32Next(s, &pe)) if (_stricmp(pe.szExeFile, "steam.exe") == 0) { found = true; if (pid) *pid = pe.th32ProcessID; break; }
    CloseHandle(s); return found; }
static std::string reg_str(HKEY root, const char* key, const char* val) {
    char buf[1024]; DWORD n = sizeof buf; if (RegGetValueA(root, key, val, RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS) return buf; return ""; }
static DWORD reg_dword(HKEY root, const char* key, const char* val) {
    DWORD v = 0, n = sizeof v; if (RegGetValueA(root, key, val, RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS) return v; return 0; }

// the whole button: finds Steam and the account, closes and restarts Steam around the change
int steam_add_shortcut(const std::string& exe, const std::string& dir, std::string& msg) {
    std::string steam = reg_str(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath");
    if (steam.empty()) { msg = "Steam isn't installed for this Windows user."; return -1; }
    for (auto& ch : steam) if (ch == '/') ch = '\\';
    // the account: the one signed in now, else the userdata folder used most recently
    std::string acct; DWORD active = reg_dword(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", "ActiveUser");
    if (active) acct = std::to_string(active);
    if (acct.empty()) {
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((steam + "\\userdata\\*").c_str(), &fd); FILETIME best = {};
        if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] < '1' || fd.cFileName[0] > '9') continue;
                WIN32_FILE_ATTRIBUTE_DATA a; std::string lc = steam + "\\userdata\\" + fd.cFileName + "\\config\\localconfig.vdf";
                if (GetFileAttributesExA(lc.c_str(), GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &best) > 0) { best = a.ftLastWriteTime; acct = fd.cFileName; }
            } while (FindNextFileA(h, &fd)); FindClose(h); }
    }
    if (acct.empty()) { msg = "Sign in to Steam once, then try again."; return -1; }
    std::string cfg = steam + "\\userdata\\" + acct + "\\config";
    CreateDirectoryA(cfg.c_str(), nullptr);
    DWORD pid = 0; bool was = steam_running(&pid);
    if (was) {   // Steam keeps its own copy and writes it back on exit: close it cleanly first
        ShellExecuteA(nullptr, "open", (steam + "\\steam.exe").c_str(), "-shutdown", nullptr, SW_HIDE);
        HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, pid);
        DWORD w = ph ? WaitForSingleObject(ph, 30000) : WAIT_FAILED; if (ph) CloseHandle(ph);
        if (w != WAIT_OBJECT_0 && steam_running(nullptr)) { msg = "Steam didn't close. Close Steam yourself, then press Add to Steam again."; return -1; }
        Sleep(1000);
    }
    int r = steam_vdf_add(cfg + "\\shortcuts.vdf", "Metroid Dread", exe, dir, msg, "");
    if (was) { ShellExecuteA(nullptr, "open", (steam + "\\steam.exe").c_str(), "-silent", nullptr, SW_SHOWMINNOACTIVE); if (r == 1) msg += " Steam was restarted to pick it up."; }
    return r; }
#else
// ---------------------------------------------------------------- Linux (Steam Deck Desktop Mode, any distribution)
static bool is_dir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static bool is_file(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
static std::string home() { const char* h = getenv("HOME"); return h && *h ? h : "."; }
// Steam's folder: the Deck's and most distributions', the ~/.steam links, Flatpak's
static std::string steam_root() {
    const char* c[] = {"/.local/share/Steam", "/.steam/steam", "/.steam/root", "/.var/app/com.valvesoftware.Steam/.local/share/Steam"};
    for (auto s : c) { std::string p = home() + s; char b[4096]; if (realpath(p.c_str(), b) && is_dir(std::string(b) + "/userdata")) return b; }
    return ""; }
static std::string slurp(const std::string& p) {
    std::string d; if (FILE* f = fopen(p.c_str(), "rb")) { char b[8192]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) d.append(b, n); fclose(f); }
    return d; }
// the account: loginusers.vdf's MostRecent one (SteamID64 - 76561197960265728), else the userdata folder used last
static std::string steam_account(const std::string& steam) {
    std::string t = slurp(steam + "/config/loginusers.vdf"), best;
    for (size_t p = 0; (p = t.find("\"7656", p)) != std::string::npos;) {
        size_t e = t.find('"', p + 1); if (e == std::string::npos) break;
        size_t blk = t.find('}', e); std::string body = t.substr(e, blk == std::string::npos ? std::string::npos : blk - e);
        size_t m = body.find("\"MostRecent\""), one = m == std::string::npos ? m : body.find("\"1\"", m);
        bool recent = one != std::string::npos && one < m + 24;
        unsigned long long v = strtoull(t.substr(p + 1, e - p - 1).c_str(), nullptr, 10);
        if (v > 76561197960265728ull && (recent || best.empty())) { best = std::to_string(v - 76561197960265728ull); if (recent) break; }
        p = e;
    }
    if (!best.empty() && is_dir(steam + "/userdata/" + best)) return best;
    best.clear(); time_t bt = 0;
    if (DIR* d = opendir((steam + "/userdata").c_str())) { while (dirent* e = readdir(d)) { if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
            struct stat st; std::string lc = steam + "/userdata/" + e->d_name + "/config/localconfig.vdf";
            if (stat(lc.c_str(), &st) == 0 && st.st_mtime > bt) { bt = st.st_mtime; best = e->d_name; } } closedir(d); }
    return best; }
static std::vector<int> steam_pids() {   // Steam's client processes (named "steam")
    std::vector<int> out;
    if (DIR* d = opendir("/proc")) { while (dirent* e = readdir(d)) { int pid = atoi(e->d_name); if (pid <= 0) continue;
            std::string c = slurp(std::string("/proc/") + e->d_name + "/comm"); while (!c.empty() && c.back() == '\n') c.pop_back();
            if (c == "steam") out.push_back(pid); } closedir(d); }
    return out; }
static void spawn(const std::vector<std::string>& argv) {   // detached (its own session), no output, not waited for
    pid_t p = fork();
    if (p == 0) { setsid(); int n = open("/dev/null", O_RDWR); if (n >= 0) { dup2(n, 0); dup2(n, 1); dup2(n, 2); }
        std::vector<char*> a; for (auto& s : argv) a.push_back((char*)s.c_str()); a.push_back(nullptr);
        if (fork() == 0) { execvp(a[0], a.data()); _exit(127); } _exit(0); }
    if (p > 0) waitpid(p, nullptr, 0); }
static std::string steam_cmd(const std::string& steam) {
    if (is_file("/usr/bin/steam")) return "/usr/bin/steam";
    if (is_file(steam + "/steam.sh")) return steam + "/steam.sh";
    return "steam"; }
// The whole button. The shortcut starts the AppImage when the game runs from one ($APPIMAGE), else run.sh next to the
// program (the Steam Deck package), else the program. SWR_STEAM_VDF=file (tests): only that file is changed, Steam is
// neither looked for nor stopped
int steam_add_shortcut(const std::string& exe, const std::string& dir, std::string& msg) {
    const char* ai = getenv("APPIMAGE");
    std::string target = ai && *ai ? ai : is_file(dir + "/run.sh") ? dir + "/run.sh" : exe;
    std::string tdir = target.substr(0, target.find_last_of('/'));
    std::string icon = is_file(dir + "/metroid-dread.png") ? dir + "/metroid-dread.png" : "";   // AppRun puts it there
    const char* tv = getenv("SWR_STEAM_VDF");
    if (tv && *tv) {   // tests: what would be used, and only that file changed
        std::string st = steam_root(), ac = st.empty() ? "" : steam_account(st);
        int r = steam_vdf_add(tv, "Metroid Dread", target, tdir, msg, icon);
        msg += " [test: steam=" + st + " account=" + ac + " running=" + std::to_string(!steam_pids().empty()) + " target=" + target + " icon=" + icon + "]";
        return r;
    }
    std::string steam = steam_root();
    if (steam.empty()) { msg = "Steam isn't installed for this user."; return -1; }
    std::string acct = steam_account(steam);
    if (acct.empty()) { msg = "Sign in to Steam once, then try again."; return -1; }
    std::string cfg = steam + "/userdata/" + acct + "/config"; mkdir(cfg.c_str(), 0755);
    const char* xd = getenv("XDG_CURRENT_DESKTOP");
    bool was = !steam_pids().empty();
    if (was && xd && !strcasecmp(xd, "gamescope")) { msg = "Steam can't be restarted in Game Mode: add the game from Desktop Mode."; return -1; }
    if (was) {   // Steam keeps its own copy and writes it back on exit: close it cleanly first
        spawn({steam_cmd(steam), "-shutdown"});
        for (int i = 0; i < 300 && !steam_pids().empty(); i++) usleep(100000);
        if (!steam_pids().empty()) { msg = "Steam didn't close. Close Steam yourself, then press Add to Steam again."; return -1; }
        sleep(1);
    }
    int r = steam_vdf_add(cfg + "/shortcuts.vdf", "Metroid Dread", target, tdir, msg, icon);
    if (was) { spawn({steam_cmd(steam), "-silent"}); if (r == 1) msg += " Steam was restarted to pick it up."; }
    return r; }
#endif
}   // namespace fe
