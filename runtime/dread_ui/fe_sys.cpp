// Front end: files, the setup process, migration, save backups, mods, controllers and hardware detection.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <urlmon.h>   // URLDownloadToFileW: the button-icon packs
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <dxgi.h>
#include "fe.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace fe {

Paths P;

// ---------------------------------------------------------------- strings / files
std::string utf8(const std::wstring& w) {
    if (w.empty()) return {}; int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return {}; int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}
std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b; char c = a.back(); return (c == '\\' || c == '/') ? a + b : a + "\\" + b;
}
bool file_exists(const std::string& p) { DWORD a = GetFileAttributesW(wide(p).c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
bool dir_exists(const std::string& p) { DWORD a = GetFileAttributesW(wide(p).c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
long long file_size(const std::string& p) {
    WIN32_FILE_ATTRIBUTE_DATA d; if (!GetFileAttributesExW(wide(p).c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}
/* a drive path past the classic 260-character limit, with the \\?\ prefix that lifts it (mods nest deep: the Portuguese
   translation's Yuzu layout is 11 folders down, and a download folder adds its own); other paths as they are */
static std::wstring lp(const std::string& s) {
    std::wstring w = wide(s);
    if (w.size() < 240 || w.size() < 3 || w[1] != L':') return w;
    for (auto& c : w) if (c == L'/') c = L'\\';
    return L"\\\\?\\" + w;
}
bool make_dirs(const std::string& p) {
    if (p.size() < 240) { int r = SHCreateDirectoryExW(nullptr, wide(p).c_str(), nullptr); return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS; }
    /* SHCreateDirectoryExW takes no long paths: one level at a time */
    for (size_t i = 3; i <= p.size(); i++)
        if (i == p.size() || p[i] == '\\' || p[i] == '/') {
            std::wstring w = lp(p.substr(0, i));
            if (!CreateDirectoryW(w.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
    return true;
}
std::string read_text(const std::string& p) {
    FILE* f = _wfopen(wide(p).c_str(), L"rb"); if (!f) return {};
    std::string s; char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s;
}
bool write_text(const std::string& p, const std::string& s) {
    std::string tmp = p + ".tmp"; FILE* f = _wfopen(wide(tmp).c_str(), L"wb"); if (!f) return false;
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size(); ok = fclose(f) == 0 && ok;
    if (!ok) { DeleteFileW(wide(tmp).c_str()); return false; }
    return MoveFileExW(wide(tmp).c_str(), wide(p).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
std::string now_stamp() { time_t t = time(nullptr); tm l; localtime_s(&l, &t); char b[32]; strftime(b, sizeof b, "%Y%m%d-%H%M%S", &l); return b; }

void Paths::init(const char* dir, const char* name) {
    exedir = dir; exename = name;
    for (auto& c : exedir) if (c == '/') c = '\\';
    exe = join(exedir, exename + ".exe"); ini = join(exedir, exename + ".ini"); data = join(exedir, exename + "_data");
    code = join(exedir, exename + "_code.dll"); build = join(exedir, exename + "_build"); recompiler = join(exedir, "recompiler");
    setup_ok = join(data, "setup.ok");
    wchar_t* ad = nullptr; if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &ad) == S_OK) { appdata = join(utf8(ad), "MetroidDreadRecomp"); CoTaskMemFree(ad); }
}

// ---------------------------------------------------------------- INI
static bool ini_kv(const std::string& ln, std::string& k, std::string& v) {
    size_t a = ln.find_first_not_of(" \t"); if (a == std::string::npos) return false;
    char c = ln[a]; if (c == ';' || c == '#' || c == '[') return false;
    size_t e = ln.find('=', a); if (e == std::string::npos) return false;
    k = ln.substr(a, e - a); while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
    v = ln.substr(e + 1); size_t b = v.find_first_not_of(" \t"); v = b == std::string::npos ? "" : v.substr(b);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
    return true;
}
bool IniFile::load(const std::string& p) {
    path = p; lines.clear(); std::string s = read_text(p); if (s.empty() && !file_exists(p)) return false;
    size_t i = 0; while (i <= s.size()) { size_t e = s.find('\n', i); if (e == std::string::npos) e = s.size();
        std::string l = s.substr(i, e - i); if (!l.empty() && l.back() == '\r') l.pop_back(); if (!(e == s.size() && l.empty())) lines.push_back(l); i = e + 1; }
    return true;
}
bool IniFile::save() const { std::string s; for (auto& l : lines) { s += l; s += "\r\n"; } return write_text(path, s); }
std::string IniFile::get(const char* key, const char* def) const {
    std::string k, v; for (auto& l : lines) if (ini_kv(l, k, v) && k == key) return v; return def;
}
bool IniFile::has(const char* key) const { std::string k, v; for (auto& l : lines) if (ini_kv(l, k, v) && k == key) return true; return false; }
void IniFile::set(const char* section, const char* key, const std::string& value) {
    std::string k, v;
    for (auto& l : lines) if (ini_kv(l, k, v) && k == key) { l = std::string(key) + "=" + value; return; }
    int sec_end = -1; bool in = false;
    for (int i = 0; i < (int)lines.size(); i++) {
        const std::string& l = lines[i]; size_t a = l.find_first_not_of(" \t");
        if (a != std::string::npos && l[a] == '[') { in = l.substr(a + 1, l.find(']') - a - 1) == section; if (in) sec_end = i + 1; continue; }
        if (in && a != std::string::npos) sec_end = i + 1;
    }
    if (sec_end >= 0) lines.insert(lines.begin() + sec_end, std::string(key) + "=" + value);
    else { lines.push_back(""); lines.push_back(std::string("[") + section + "]"); lines.push_back(std::string(key) + "=" + value); }
}

// ---------------------------------------------------------------- dialogs / shell
std::string pick_folder(void* owner, const char* title) {
    std::string out; IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD o; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM); d->SetTitle(wide(title).c_str());
    if (SUCCEEDED(d->Show((HWND)owner))) { IShellItem* it; if (SUCCEEDED(d->GetResult(&it))) { PWSTR p; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = utf8(p); CoTaskMemFree(p); } it->Release(); } }
    d->Release(); return out;
}
std::string pick_file(void* owner, const char* title, const wchar_t* fname, const wchar_t* fspec) {
    std::string out; IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    COMDLG_FILTERSPEC f = {fname, fspec}; d->SetFileTypes(1, &f); d->SetTitle(wide(title).c_str());
    if (SUCCEEDED(d->Show((HWND)owner))) { IShellItem* it; if (SUCCEEDED(d->GetResult(&it))) { PWSTR p; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = utf8(p); CoTaskMemFree(p); } it->Release(); } }
    d->Release(); return out;
}
void open_in_explorer(const std::string& p) { ShellExecuteW(nullptr, L"open", wide(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void open_in_notepad(const std::string& p) { ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + wide(p) + L"\"").c_str(), nullptr, SW_SHOWNORMAL); }
bool recycle(const std::string& p) {
    std::wstring w = wide(p); w.push_back(0); w.push_back(0);
    SHFILEOPSTRUCTW op = {}; op.wFunc = FO_DELETE; op.pFrom = w.c_str(); op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0;
}

template <class F> static void walk(const std::string& root, const std::string& rel, F&& f) {
    WIN32_FIND_DATAW d; HANDLE h = FindFirstFileW(lp(join(join(root, rel), "*")).c_str(), &d); if (h == INVALID_HANDLE_VALUE) return;
    do { std::string n = utf8(d.cFileName); if (n == "." || n == "..") continue;
         std::string r = rel.empty() ? n : rel + "\\" + n;
         if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // never follow links
         if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { f(r, true, 0LL); walk(root, r, f); }
         else f(r, false, ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    } while (FindNextFileW(h, &d)); FindClose(h);
}
long long dir_size(const std::string& p, int* files) {
    long long t = 0; int n = 0; walk(p, "", [&](const std::string&, bool dir, long long sz) { if (!dir) { t += sz; n++; } });
    if (files) *files = n; return t;
}
bool copy_tree(const std::string& src, const std::string& dst, std::string* err, bool overwrite) {
    if (!make_dirs(dst)) { if (err) *err = "cannot create " + dst; return false; }
    bool ok = true;
    walk(src, "", [&](const std::string& r, bool dir, long long) {
        if (!ok) return; std::string d = join(dst, r);
        if (dir) { if (!make_dirs(d)) { ok = false; if (err) *err = "cannot create " + d; } return; }
        if (!overwrite && file_exists(d)) return;
        if (!CopyFileW(lp(join(src, r)).c_str(), lp(d).c_str(), FALSE)) { ok = false; if (err) *err = "cannot copy " + r; }
    });
    return ok;
}
bool trees_equal(const std::string& a, const std::string& b) {
    bool eq = true;
    walk(a, "", [&](const std::string& r, bool dir, long long) {
        if (!eq || dir) return;
        std::string x = read_text(join(a, r)), y = read_text(join(b, r)); if (x != y || !file_exists(join(b, r))) eq = false; });
    return eq;
}

// ---------------------------------------------------------------- installed game
GameCheck check_installed() {
    GameCheck g;
    g.code_ok = file_exists(P.code) && file_size(P.code) > 50 * 1024 * 1024;
    g.exefs_ok = file_exists(join(P.data, "exefs\\main")) && file_exists(join(P.data, "exefs\\main.npdm"));
    g.romfs_ok = dir_exists(join(P.data, "romfs\\system")) && dir_exists(join(P.data, "romfs\\packs"));
    g.data_ok = g.exefs_ok && g.romfs_ok;
    g.setup_ok = file_exists(P.setup_ok);
    if (!g.code_ok) g.summary = "The game has not been compiled yet.";
    else if (!g.data_ok) g.summary = "Game data is missing.";
    else g.summary = "Ready.";
    return g;
}

// ---------------------------------------------------------------- setup process
static CRITICAL_SECTION g_cs; static bool g_cs_init;
void setup_lock() { if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = true; } EnterCriticalSection(&g_cs); }
void setup_unlock() { LeaveCriticalSection(&g_cs); }
std::string recompiler_python() { return join(P.recompiler, "python\\python.exe"); }

struct ReaderArg { SetupJob* j; HANDLE rd; HANDLE proc; HANDLE job; };
static DWORD WINAPI reader(void* p) {
    ReaderArg* a = (ReaderArg*)p; SetupJob& j = *a->j; std::string buf; char b[4096]; DWORD n;
    std::string logpath = join(P.exedir, "setup.log");
    FILE* lf = _wfopen(wide(logpath).c_str(), L"ab");
    while (ReadFile(a->rd, b, sizeof b, &n, nullptr) && n) {
        buf.append(b, n); size_t e;
        while ((e = buf.find('\n')) != std::string::npos) {
            std::string l = buf.substr(0, e); buf.erase(0, e + 1); if (!l.empty() && l.back() == '\r') l.pop_back();
            if (lf) { fwrite(l.data(), 1, l.size(), lf); fwrite("\r\n", 1, 2, lf); fflush(lf); }
            setup_lock();
            if (l.rfind("@@P ", 0) == 0) {   // @@P <percent> <stage>|<message>
                float pc = (float)atof(l.c_str() + 4); size_t s = l.find(' ', 4), bar = l.find('|');
                j.pct = pc; if (s != std::string::npos) { j.stage = l.substr(s + 1, bar == std::string::npos ? std::string::npos : bar - s - 1); if (bar != std::string::npos) j.message = l.substr(bar + 1); }
            } else if (l.rfind("@@E ", 0) == 0) { j.error = l.substr(4); j.failed = true; }
            else if (l == "@@D") { j.done = true; }
            else { j.log.push_back(l); if (j.log.size() > 5000) j.log.erase(j.log.begin(), j.log.begin() + 1000); }
            setup_unlock();
        }
    }
    WaitForSingleObject(a->proc, INFINITE); DWORD code = 1; GetExitCodeProcess(a->proc, &code);
    setup_lock();
    if (!j.done && !j.failed) { j.failed = true; if (j.error.empty()) j.error = j.cancelled ? "Cancelled." : "The recompiler stopped unexpectedly (exit code " + std::to_string(code) + ")."; }
    if (code != 0 && !j.cancelled && j.error.empty()) j.error = "The compiler stopped with an error (see the log).";
    j.running = false; j.proc = nullptr;
    setup_unlock();
    if (lf) fclose(lf);
    CloseHandle(a->rd); CloseHandle(a->proc); CloseHandle(a->job); delete a; return 0;
}

bool setup_start(SetupJob& j) {
    std::string py = recompiler_python(), script = join(P.recompiler, "dread_setup.py");
    if (!file_exists(py) || !file_exists(script)) { j.failed = true; j.error = "The recompiler is missing: expected " + P.recompiler + " (python\\python.exe and dread_setup.py). Re-extract the release."; return false; }
    // command-line quoting: a backslash before the closing quote must be doubled ("C:\dir\\")
    auto q = [](const std::string& v) { std::wstring w = wide(v); size_t n = 0; while (n < w.size() && w[w.size() - 1 - n] == L'\\') n++; w.append(n, L'\\'); return L"\"" + w + L"\""; };
    std::wstring cmd = q(py) + L" -I -u " + q(script) + L" --out " + q(P.exedir) + L" --name " + q(P.exename) + L" --base " + q(j.base);
    if (!j.update.empty()) cmd += L" --update " + q(j.update);
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE}; HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false; SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof si}; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);   // cancel ends python and every gcc it started
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {}; li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
    j.log.clear(); j.error.clear(); j.pct = 0; j.done = j.failed = j.cancelled = false; j.running = true; j.stage = "Starting"; j.message = "";
    { std::string lp = join(P.exedir, "setup.log"); FILE* f = _wfopen(wide(lp).c_str(), L"ab"); if (f) { fprintf(f, "\r\n===== setup %s =====\r\n", now_stamp().c_str()); fclose(f); } }
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr, wide(P.recompiler).c_str(), &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); CloseHandle(job); j.running = false; j.failed = true; j.error = "Could not start the recompiler (error " + std::to_string(GetLastError()) + ")."; return false;
    }
    AssignProcessToJobObject(job, pi.hProcess); ResumeThread(pi.hThread); CloseHandle(pi.hThread); CloseHandle(wr);
    j.proc = job;
    ReaderArg* a = new ReaderArg{&j, rd, pi.hProcess, job};
    CloseHandle(CreateThread(nullptr, 0, reader, a, 0, nullptr));
    return true;
}
void setup_cancel(SetupJob& j) { setup_lock(); if (j.running && j.proc) { j.cancelled = true; TerminateJobObject((HANDLE)j.proc, 1); } setup_unlock(); }

// ---------------------------------------------------------------- migration
static std::string json_str(const std::string& s, const char* key) {
    std::string k = std::string("\"") + key + "\""; size_t p = s.find(k); if (p == std::string::npos) return {};
    p = s.find(':', p + k.size()); if (p == std::string::npos) return {}; p++;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
    if (p < s.size() && s[p] == '"') { std::string out; for (size_t i = p + 1; i < s.size() && s[i] != '"'; i++) { if (s[i] == '\\' && i + 1 < s.size()) { i++; out += s[i] == '\\' ? '\\' : s[i]; } else out += s[i]; } return out; }
    size_t e = s.find_first_of(",}\r\n", p); return s.substr(p, e - p);
}
static std::string old_json_path() {
    wchar_t* ad = nullptr; std::string r; if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &ad) == S_OK) { r = join(utf8(ad), "DreadLauncher\\config.json"); CoTaskMemFree(ad); } return r;
}
OldInstall describe_old(const std::string& exe) {
    OldInstall o; o.exe = exe; std::string base = exe.substr(0, exe.size() - 4), dir = exe.substr(0, exe.find_last_of("\\/"));
    if (file_exists(base + ".ini")) o.ini = base + ".ini";
    if (dir_exists(base + "_data\\save")) { o.saves = base + "_data\\save"; o.save_bytes = dir_size(o.saves); }
    if (dir_exists(base + "_data\\mods")) { o.mods = base + "_data\\mods"; o.mods_bytes = dir_size(o.mods); }
    if (dir_exists(join(dir, "shader_cache"))) { o.shader_cache = join(dir, "shader_cache"); o.cache_bytes += dir_size(o.shader_cache); }
    if (dir_exists(join(dir, "vulkan_native"))) { o.vk_cache = join(dir, "vulkan_native"); o.cache_bytes += dir_size(o.vk_cache); }
    if (file_exists(join(dir, "SDL2.dll"))) o.sdl = join(dir, "SDL2.dll");
    std::string jp = old_json_path(); if (file_exists(jp)) o.json = jp;
    return o;
}
std::vector<OldInstall> find_old_installs() {
    std::vector<OldInstall> r; std::string jp = old_json_path();
    if (file_exists(jp)) {
        std::string exe = json_str(read_text(jp), "exe");
        if (!exe.empty() && file_exists(exe) && _stricmp(exe.c_str(), P.exe.c_str())) r.push_back(describe_old(exe));
    }
    return r;
}
MigrateResult migrate(const OldInstall& o, bool settings, bool saves, bool mods, bool caches) {
    MigrateResult m; std::string err;
    if (settings && !o.ini.empty()) {
        IniFile src, dst; src.load(o.ini); dst.load(P.ini); if (dst.path.empty()) dst.path = P.ini;
        std::string sec = "Other"; int n = 0;
        for (auto& l : src.lines) {
            size_t a = l.find_first_not_of(" \t"); if (a == std::string::npos) continue;
            if (l[a] == '[') { sec = l.substr(a + 1, l.find(']') - a - 1); continue; }
            std::string k, v; if (!ini_kv(l, k, v)) continue;
            dst.set(sec.c_str(), k.c_str(), v); n++;
        }
        if (!o.json.empty()) {   // the Python launcher's own preferences
            std::string js = read_text(o.json);
            auto flag = [&](const char* jk, const char* ik) { std::string v = json_str(js, jk); if (v == "true" || v == "false") { dst.set("Launcher", ik, v == "true" ? "1" : "0"); n++; } };
            flag("sync_shaders", "wait_for_shaders"); flag("loading_60fps", "loading_60fps"); flag("cutscenes_60fps", "cutscenes_60fps"); flag("controller_navigation", "launcher_nav");
            std::string pr = json_str(js, "controller_prompts");
            if (!pr.empty()) { for (auto& ch : pr) ch = (char)tolower((unsigned char)ch); dst.set("Launcher", "launcher_prompts", pr == "automatic" ? "auto" : pr); }
            size_t lb = js.find("\"launcher_bindings\"");
            if (lb != std::string::npos) {
                std::string obj = js.substr(lb, js.find('}', lb) - lb + 1);
                const char* names[5] = {"Confirm", "Back", "Previous page", "Next page", "Play / home"}; std::string v; bool ok = true;
                for (int i = 0; i < 5; i++) { std::string x = json_str(obj, names[i]); if (x.empty()) { ok = false; break; } if (i) v += ","; v += x; }
                if (ok) dst.set("Launcher", "launcher_bindings", v);
            }
        }
        dst.set("Launcher", "migrated_from", o.exe);
        if (!dst.save()) { m.ok = false; m.text += "Settings could not be written.\n"; } else m.text += "Settings: " + std::to_string(n) + " values imported.\n";
    }
    if (saves && !o.saves.empty()) {
        std::string dst = join(P.data, "save");
        if (dir_exists(dst) && dir_size(dst) > 0) {   // keep what is there: back it up first
            std::string zip, e2; if (!backup_saves(&zip, &e2)) { m.ok = false; m.text += "Existing saves could not be backed up (" + e2 + "); saves not imported.\n"; goto after_saves; }
            m.text += "Saves already here were backed up to " + zip + ".\n";
        }
        if (!copy_tree(o.saves, dst, &err, true) || !trees_equal(o.saves, dst)) { m.ok = false; m.text += "Saves: copy failed (" + err + ").\n"; }
        else m.text += "Saves: copied and verified (the originals were only read).\n";
    }
after_saves:
    if (mods && !o.mods.empty()) { if (copy_tree(o.mods, mods_dir(), &err, false)) m.text += "Mods copied.\n"; else { m.ok = false; m.text += "Mods: " + err + "\n"; } }
    if (caches) {
        if (!o.shader_cache.empty()) copy_tree(o.shader_cache, join(P.exedir, "shader_cache"), &err, false);
        if (!o.vk_cache.empty()) copy_tree(o.vk_cache, join(P.exedir, "vulkan_native"), &err, false);
        if (!o.shader_cache.empty() || !o.vk_cache.empty()) m.text += "Shader caches copied.\n";
    }
    if (!o.sdl.empty() && !file_exists(join(P.exedir, "SDL2.dll"))) CopyFileW(wide(o.sdl).c_str(), wide(join(P.exedir, "SDL2.dll")).c_str(), TRUE);
    return m;
}

// ---------------------------------------------------------------- save backups: a stored (uncompressed) zip, verified by reading it back
static unsigned crc_table[256];
static unsigned crc32(const std::string& d) {
    if (!crc_table[1]) for (unsigned i = 0; i < 256; i++) { unsigned c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[i] = c; }
    unsigned c = 0xFFFFFFFFu; for (unsigned char b : d) c = crc_table[(c ^ b) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFu;
}
struct ZipEntry { std::string name, data; unsigned crc; unsigned off; };
static std::string stored_zip(std::vector<ZipEntry>& es) {
    std::string z; auto u16 = [&](unsigned v) { z += (char)(v & 255); z += (char)(v >> 8 & 255); };
    auto u32 = [&](unsigned v) { u16(v & 0xFFFF); u16(v >> 16); };
    for (auto& e : es) { e.crc = crc32(e.data); e.off = (unsigned)z.size(); u32(0x04034b50); u16(20); u16(0x0800); u16(0); u16(0); u16(0); u32(e.crc); u32((unsigned)e.data.size()); u32((unsigned)e.data.size()); u16((unsigned)e.name.size()); u16(0); z += e.name; z += e.data; }
    unsigned cd = (unsigned)z.size();
    for (auto& e : es) { u32(0x02014b50); u16(20); u16(20); u16(0x0800); u16(0); u16(0); u16(0); u32(e.crc); u32((unsigned)e.data.size()); u32((unsigned)e.data.size()); u16((unsigned)e.name.size()); u16(0); u16(0); u16(0); u16(0); u32(0); u32(e.off); z += e.name; }
    unsigned cdlen = (unsigned)z.size() - cd; u32(0x06054b50); u16(0); u16(0); u16((unsigned)es.size()); u16((unsigned)es.size()); u32(cdlen); u32(cd); u16(0);
    return z;
}
std::string backups_dir() { return join(P.appdata, "save-backups"); }
bool backup_saves(std::string* out, std::string* err) {
    std::string src = join(P.data, "save");
    if (!dir_exists(src)) { if (err) *err = "No saves yet."; return false; }
    std::vector<ZipEntry> es; long long before = dir_size(src);
    walk(src, "", [&](const std::string& r, bool dir, long long) { if (dir) return; ZipEntry e; e.name = "save/" + r; for (auto& c : e.name) if (c == '\\') c = '/'; e.data = read_text(join(src, r)); es.push_back(e); });
    if (es.empty()) { if (err) *err = "The save folder is empty."; return false; }
    if (dir_size(src) != before) { if (err) *err = "Saves changed during the backup. Close the game and try again."; return false; }
    std::string z = stored_zip(es);
    make_dirs(backups_dir());
    std::string path = join(backups_dir(), "Metroid-Dread-saves-" + now_stamp() + ".zip");
    if (!write_text(path, z)) { if (err) *err = "Could not write " + path; return false; }
    std::string back = read_text(path);   // verify: same bytes, every entry's CRC still matches
    if (back != z) { if (err) *err = "Backup verification failed."; DeleteFileW(wide(path).c_str()); return false; }
    for (auto& e : es) if (crc32(back.substr(e.off + 30 + e.name.size(), e.data.size())) != e.crc) { if (err) *err = "Backup verification failed."; return false; }
    if (out) *out = path; return true;
}

// ---------------------------------------------------------------- problem reports: this game's logs and settings in one zip (no saves, no game files)
std::string cpu_name() {
    wchar_t b[256] = {}; DWORD n = sizeof b;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, b, &n) != ERROR_SUCCESS) return "";
    std::string s = utf8(b); while (!s.empty() && s.back() == ' ') s.pop_back(); while (!s.empty() && s[0] == ' ') s.erase(0, 1); return s;
}
std::string reports_dir() { return join(P.exedir, "reports"); }
bool make_report(const std::string& info, std::string* out, std::string* err) {
    std::vector<ZipEntry> es; ZipEntry i; i.name = "system.txt"; i.data = info; es.push_back(i);
    const std::string n = P.exename;
    const std::string files[] = {n + ".log", n + ".previous.log", "performance.txt", "threads.txt", "setup.log", n + ".ini", "update.txt"};
    for (auto& f : files) { std::string p = join(P.exedir, f); if (!file_exists(p)) continue; ZipEntry e; e.name = f; e.data = read_text(p); es.push_back(e); }
    if (es.size() < 2) { if (err) *err = "There are no logs yet. Start the game once, then try again."; return false; }
    make_dirs(reports_dir());
    std::string path = join(reports_dir(), "Metroid-Dread-report-" + now_stamp() + ".zip");
    if (!write_text(path, stored_zip(es))) { if (err) *err = "Could not write " + path; return false; }
    if (out) *out = path; return true;
}

// ---------------------------------------------------------------- mods
std::string mods_dir() { return join(P.data, "mods"); }
std::vector<Mod> mods_list() {
    std::vector<Mod> out; std::string d = mods_dir(); if (!dir_exists(d)) return out;
    std::vector<std::string> present;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(wide(join(d, "*")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { std::string n = utf8(fd.cFileName); if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n != "." && n != "..") present.push_back(n); } while (FindNextFileW(h, &fd)); FindClose(h); }
    std::sort(present.begin(), present.end());
    std::string lo = read_text(join(d, "load_order.txt")); size_t i = 0;
    while (i < lo.size()) { size_t e = lo.find('\n', i); if (e == std::string::npos) e = lo.size(); std::string l = lo.substr(i, e - i); i = e + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back(); if (l.empty() || l[0] == '#') continue;
        bool en = l[0] != '-'; std::string n = en ? l : l.substr(1); while (!n.empty() && n[0] == ' ') n.erase(0, 1);
        if (std::find(present.begin(), present.end(), n) != present.end() && std::none_of(out.begin(), out.end(), [&](const Mod& m) { return m.name == n; })) out.push_back({n, en, 0, 0}); }
    for (auto& n : present) if (std::none_of(out.begin(), out.end(), [&](const Mod& m) { return m.name == n; })) out.push_back({n, true, 0, 0});
    for (auto& m : out) m.bytes = dir_size(join(join(d, m.name), "romfs"), &m.files);
    return out;
}
void mods_write(const std::vector<Mod>& m) {
    make_dirs(mods_dir()); std::string s = "# Mod load order: first line = lowest priority. '-' disables a mod.\r\n";
    for (auto& x : m) s += (x.enabled ? "" : "-") + x.name + "\r\n";
    write_text(join(mods_dir(), "load_order.txt"), s);
}
static std::string find_romfs(const std::string& root) {
    const char* id = "010093801237C000";
    std::string c[] = {join(root, "romfs"), join(root, std::string("atmosphere\\contents\\") + id + "\\romfs"), join(root, std::string("atmosphere\\titles\\") + id + "\\romfs"),
                       join(root, std::string("contents\\") + id + "\\romfs"), join(root, std::string(id) + "\\romfs")};
    for (auto& x : c) if (dir_exists(x)) return x;
    std::string found; walk(root, "", [&](const std::string& r, bool dir, long long) { if (found.empty() && dir) { size_t s = r.find_last_of('\\'); std::string leaf = s == std::string::npos ? r : r.substr(s + 1); if (leaf == "romfs") found = join(root, r); } });
    if (!found.empty()) return found;
    for (const char* d : {"packs", "textures", "system", "gui", "sounds", "actors"}) if (dir_exists(join(root, d))) return root;
    return {};
}
bool mod_import_folder(const std::string& src, const std::string& name, std::string* err) {
    std::string rf = find_romfs(src);
    if (rf.empty()) { if (err) *err = "No 'romfs' folder in this mod. Only RomFS (file replacement) mods and texture packs work with the recompiled game; exefs/code patches can't apply."; return false; }
    std::string dst = join(join(mods_dir(), name), "romfs");
    if (!copy_tree(rf, dst, err, true)) return false;
    auto m = mods_list(); mods_write(m); return true;
}
static bool zip_into(const std::string& zip, const std::string& name, std::string* err);
bool mod_import_zip(const std::string& zip, std::string* err) {
    std::string name = zip.substr(zip.find_last_of("\\/") + 1); name = name.substr(0, name.find_last_of('.'));
    return zip_into(zip, name, err);
}
static bool zip_into(const std::string& zip, const std::string& name, std::string* err) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::string dir = join(utf8(tmp), "dreadmod_" + now_stamp()); make_dirs(dir);
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + wide(zip) + L"\" -C \"" + wide(dir) + L"\"";   // Windows' bsdtar: refuses paths that leave -C
    STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi; std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { if (err) *err = "tar.exe could not start"; return false; }
    WaitForSingleObject(pi.hProcess, INFINITE); DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    bool ok = code == 0;
    if (!ok) { if (err) *err = "The zip could not be extracted."; }
    else ok = mod_import_folder(dir, name, err);
    recycle(dir);
    return ok;
}
// the community's button-prompt mods on GameBanana (each a romfs with the two prompt textures)
const IconPack ICON_PACKS[3] = {
    {"Switch to XBOX Layout", "Xbox", "Krydar", "https://gamebanana.com/dl/679191"},
    {"Switch to PS4 Layout", "PlayStation 4", "Nova77", "https://gamebanana.com/dl/680684"},
    {"Switch to PS5 Layout", "PlayStation 5", "xSeRosiSx", "https://gamebanana.com/dl/681751"},
};
bool icon_pack_installed(int i) { return dir_exists(join(join(mods_dir(), ICON_PACKS[i].name), "romfs")); }
bool icon_pack_get(int i, std::string* err) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::string dir = join(utf8(tmp), "dreadicons_" + now_stamp()); make_dirs(dir);
    std::string arc = join(dir, "pack.bin");
    if (URLDownloadToFileW(nullptr, wide(ICON_PACKS[i].url).c_str(), wide(arc).c_str(), 0, nullptr) != S_OK) {
        if (err) *err = "Download failed: check the internet connection."; recycle(dir); return false; }
    // zip or rar: Windows' bsdtar reads both, and refuses paths that leave -C
    std::string out = join(dir, "x"); make_dirs(out);
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + wide(arc) + L"\" -C \"" + wide(out) + L"\"";
    STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi; std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { if (err) *err = "tar.exe could not start"; recycle(dir); return false; }
    WaitForSingleObject(pi.hProcess, INFINITE); DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    bool ok = code == 0 && mod_import_folder(out, ICON_PACKS[i].name, err);
    if (code != 0 && err) *err = "The download could not be extracted.";
    recycle(dir);
    return ok;
}
// Brazilian Portuguese fan translation by JumpmanClub Brasil (GitHub issue #15); its name must match svc_fs.c PT_PACK.
// Not shipped with the game (its textures and font are edits of the game's own files): players add their download
const char* const PT_PACK_NAME = "Portuguese (Brazil) - JumpmanClub Brasil";
const char* const PT_PACK_URL = "https://www.jumpmanclubbrasil.com.br/post/metroid-dread";
bool pt_pack_installed() { return file_exists(join(join(join(mods_dir(), PT_PACK_NAME), "romfs"), "system\\localization\\us_english.txt")); }
bool pt_pack_import(const std::string& src, std::string* err) {
    std::string low = src; for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
    bool zip = low.size() > 4 && low.compare(low.size() - 4, 4, ".zip") == 0;
    if (!(zip ? zip_into(src, PT_PACK_NAME, err) : mod_import_folder(src, PT_PACK_NAME, err))) return false;
    if (!pt_pack_installed()) { if (err) *err = "That isn't the Metroid Dread translation: no system/localization/us_english.txt inside."; recycle(join(mods_dir(), PT_PACK_NAME)); return false; }
    return true;
}
bool mod_remove(const std::string& name) {
    bool ok = recycle(join(mods_dir(), name));   // to the Recycle Bin, not deleted
    auto m = mods_list(); mods_write(m); return ok;
}

// ---------------------------------------------------------------- controllers
typedef struct { WORD b; BYTE lt, rt; SHORT lx, ly, rx, ry; } XGP;
typedef struct { DWORD pkt; XGP g; } XST;
typedef DWORD(WINAPI* XGet)(DWORD, XST*); typedef DWORD(WINAPI* XSet)(DWORD, void*);
static XGet xget, xget_ex; static XSet xset; static int x_last = -1;
typedef void* GC;
static int (*s_init)(unsigned); static void (*s_quit)(void); static int (*s_hint)(const char*, const char*); static int (*s_num)(void);
static int (*s_isgc)(int); static GC (*s_open)(int); static void (*s_close)(GC); static const char* (*s_name_i)(int);
static unsigned char (*s_btn)(GC, int); static short (*s_axis)(GC, int); static void (*s_update)(void); static void (*s_pump)(void);
static int (*s_rumble)(GC, unsigned short, unsigned short, unsigned); static int (*s_attached)(GC);
static HMODULE s_mod; static bool s_ok; static GC s_pad[8]; static int s_n; static DWORD s_scan;
void pads_init() {
    const char* xs[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !xget; i++) { HMODULE h = LoadLibraryA(xs[i]); if (h) { xget = (XGet)GetProcAddress(h, "XInputGetState"); xset = (XSet)GetProcAddress(h, "XInputSetState"); if (i < 2) xget_ex = (XGet)GetProcAddress(h, (LPCSTR)(size_t)100); } }
    s_mod = LoadLibraryW(wide(join(P.exedir, "SDL2.dll")).c_str());
    if (!s_mod) return;
#define L(v, n) *(FARPROC*)&v = GetProcAddress(s_mod, n); if (!v) return;
    L(s_init, "SDL_Init") L(s_quit, "SDL_Quit") L(s_hint, "SDL_SetHint") L(s_num, "SDL_NumJoysticks") L(s_isgc, "SDL_IsGameController") L(s_open, "SDL_GameControllerOpen")
    L(s_close, "SDL_GameControllerClose") L(s_name_i, "SDL_GameControllerNameForIndex") L(s_btn, "SDL_GameControllerGetButton") L(s_axis, "SDL_GameControllerGetAxis")
    L(s_update, "SDL_GameControllerUpdate") L(s_pump, "SDL_PumpEvents") L(s_attached, "SDL_GameControllerGetAttached")
#undef L
    *(FARPROC*)&s_rumble = GetProcAddress(s_mod, "SDL_GameControllerRumble");
    // the same hints the game uses (backend_win32.c sdl_load), so SDL behaves the same in both
    const char* hints[][2] = {{"SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0"}, {"SDL_JOYSTICK_HIDAPI_PS4", "1"}, {"SDL_JOYSTICK_HIDAPI_PS5", "1"},
        {"SDL_JOYSTICK_HIDAPI_SWITCH", "1"}, {"SDL_JOYSTICK_HIDAPI_JOY_CONS", "1"}, {"SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS", "1"},
        {"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1"}, {"SDL_JOYSTICK_RAWINPUT", "0"}, {"SDL_JOYSTICK_THREAD", "1"}};
    for (auto& h : hints) s_hint(h[0], h[1]);
    s_ok = s_init(0x00002000u | 0x00000200u) == 0;
}
void pads_shutdown() {
    pads_rumble_off();
    if (s_ok) { for (int i = 0; i < s_n; i++) s_close(s_pad[i]); s_n = 0; s_quit(); s_ok = false; }   // the game starts SDL again with its own state
}
bool pads_sdl_loaded() { return s_ok; }
static void sdl_rescan() {
    if (!s_ok) return; DWORD now = GetTickCount(); if (now - s_scan < 1000 && s_scan) return; s_scan = now; s_pump();
    for (int i = 0; i < s_n;) if (!s_attached(s_pad[i])) { s_close(s_pad[i]); s_pad[i] = s_pad[--s_n]; } else i++;
    int n = s_num();
    for (int j = 0; j < n && s_n < 8; j++) { if (!s_isgc(j)) continue; GC g = s_open(j); if (!g) continue; bool have = false; for (int i = 0; i < s_n; i++) if (s_pad[i] == g) have = true; if (have) s_close(g); else s_pad[s_n++] = g; }
}
std::vector<PadInfo> pads_list() {
    std::vector<PadInfo> r;
    if (s_ok) { s_pump(); int n = s_num(); int k = 0; for (int j = 0; j < n; j++) if (s_isgc(j)) { const char* nm = s_name_i(j); r.push_back({++k, nm ? nm : "Controller"}); } }
    if (xget) for (DWORD i = 0; i < 4; i++) { XST s; if (xget(i, &s) == 0) r.push_back({-(int)i - 1, "Xbox controller (XInput " + std::to_string(i + 1) + ")"}); }
    return r;
}
PadState pads_read(int api, int index) {
    PadState st;
    if (api != 2 && s_ok) {
        sdl_rescan(); s_update();
        int first = index > 0 ? index - 1 : 0, last = index > 0 ? index : s_n;
        for (int i = first; i < last && i < s_n; i++) { GC g = s_pad[i]; unsigned long long b = 0;
            static const int map[16] = {1, 0, 3, 2, 7, 8, 9, 10, -1, -1, 6, 4, 13, 11, 14, 12};
            for (int k = 0; k < 16; k++) if (map[k] >= 0 && s_btn(g, map[k])) b |= 1ull << k;
            int lt = s_axis(g, 4), rt = s_axis(g, 5); if (lt > 8000) b |= 1 << 8; if (rt > 8000) b |= 1 << 9;
            if (s_btn(g, 5)) b |= 1ull << 30;
            if (b || !st.phys) { st.phys |= b; st.lx = s_axis(g, 0); st.ly = -s_axis(g, 1); st.rx = s_axis(g, 2); st.ry = -s_axis(g, 3); st.lt = lt; st.rt = rt; st.sdl = true; }
        }
    }
    if (api != 1 && xget) {
        for (DWORD i = 0; i < 4; i++) { XST s; if ((xget_ex ? xget_ex : xget)(i, &s)) continue; WORD x = s.g.b; unsigned long long b = 0;
            static const WORD m[16] = {0x2000, 0x1000, 0x8000, 0x4000, 0x40, 0x80, 0x100, 0x200, 0, 0, 0x10, 0x20, 4, 1, 8, 2};
            for (int k = 0; k < 16; k++) if (m[k] && (x & m[k])) b |= 1ull << k;
            if (s.g.lt > 125) b |= 1 << 8; if (s.g.rt > 125) b |= 1 << 9; if (x & 0x400) b |= 1ull << 30;
            st.phys |= b; if (!st.lx && !st.ly) { st.lx = s.g.lx; st.ly = s.g.ly; st.rx = s.g.rx; st.ry = s.g.ry; st.lt = s.g.lt * 128; st.rt = s.g.rt * 128; }
            if (st.name.empty()) st.name = "Xbox controller"; x_last = (int)i; break; }
    }
    if (st.sdl && s_n) { int i = index > 0 ? index - 1 : 0; if (i < s_n) { int n = s_num(); for (int j = 0, k = 0; j < n; j++) if (s_isgc(j)) { if (k++ == i) { const char* nm = s_name_i(j); st.name = nm ? nm : "Controller"; } } } }
    auto dz = [](int& v) { if (v > -7000 && v < 7000) v = 0; }; dz(st.lx); dz(st.ly); dz(st.rx); dz(st.ry);
    return st;
}
void pads_rumble(int pct) {
    unsigned short v = (unsigned short)(65535 * std::min(1.0, std::max(0.0, pct / 100.0 * 0.5)));
    if (s_ok && s_rumble) for (int i = 0; i < s_n; i++) s_rumble(s_pad[i], v, v, 250);
    if (xset) for (DWORD i = 0; i < 4; i++) { WORD m[2] = {v, v}; xset(i, m); }
}
void pads_rumble_off() { if (xset) for (DWORD i = 0; i < 4; i++) { WORD m[2] = {0, 0}; xset(i, m); } if (s_ok && s_rumble) for (int i = 0; i < s_n; i++) s_rumble(s_pad[i], 0, 0, 0); }

// ---------------------------------------------------------------- hardware
// one graphics adapter: keeps it in h if it ranks above what was seen so far (the best card of the PC is what plays)
static void consider_gpu(Hardware& h, int& best, const std::string& n, int vram) {
    std::string u = n; for (auto& c : u) c = (char)toupper((unsigned char)c);
    auto has = [&](const char* s) { return u.find(s) != std::string::npos; };
    // integrated graphics (Intel UHD / Iris / Arc in a CPU, AMD Radeon 780M and other APUs) share system memory
    bool igpu = vram < 1024 || has("780M") || has("760M") || has("680M") || has("890M") || has("880M") || has("RADEON(TM) GRAPHICS") || (has("VEGA") && !has("RX VEGA"));
    int rank = igpu ? 1
             : (has("RX 64") || has("RX 65")) ? 2   // entry-level RDNA2 cards
             : (has("RTX 50") || has("RTX 40") || has("RTX 30") || has("RX 9") || has("RX 7") || has("RX 6")) ? 3
             : (has("RTX 20") || has("GTX 16") || has("GTX 10") || has("RX 5") || has("ARC")) ? 2
             : (has("NVIDIA") || has("RADEON RX")) ? 2 : 1;
    if (rank > best) { best = rank; h.gpu = n; h.tier = rank == 3 ? "high" : rank == 2 ? "mid" : "low"; h.rtx = has("RTX") && has("NVIDIA"); h.vram_mb = vram; h.integrated = igpu; }
}
Hardware detect_hardware() {
    Hardware h; int best = -1;
    const char* fake = getenv("SWR_FE_TEST") ? getenv("SWR_FE_FAKE_HW") : nullptr;   // tests: "gpu name|w|h|hz|cores|vram MB|ram GB"
    if (fake) {
        std::string s = fake; std::vector<std::string> v; size_t p = 0; for (;;) { size_t e = s.find('|', p); v.push_back(s.substr(p, e == std::string::npos ? std::string::npos : e - p)); if (e == std::string::npos) break; p = e + 1; }
        if (v.size() == 7) { h.w = atoi(v[1].c_str()); h.h = atoi(v[2].c_str()); h.hz = atoi(v[3].c_str()); h.cores = atoi(v[4].c_str()); consider_gpu(h, best, v[0], atoi(v[5].c_str())); h.ram_gb = atoi(v[6].c_str()); return h; }
    }
    h.cores = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    DEVMODEW dm = {}; dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) { h.w = (int)dm.dmPelsWidth; h.h = (int)dm.dmPelsHeight; h.hz = (int)dm.dmDisplayFrequency; }
    IDXGIFactory1* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) {
        IDXGIAdapter1* a; for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d); a->Release(); if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            consider_gpu(h, best, utf8(d.Description), (int)(d.DedicatedVideoMemory >> 20));
        }
        f->Release();
    }
    if (h.gpu.empty()) { h.gpu = "unknown graphics card"; h.tier = "low"; }
    MEMORYSTATUSEX ms = {sizeof ms}; if (GlobalMemoryStatusEx(&ms)) h.ram_gb = (int)((ms.ullTotalPhys + (1ull << 30) - 1) >> 30);   // Windows reports a little less than is fitted: round up
    return h;
}

} // namespace fe
