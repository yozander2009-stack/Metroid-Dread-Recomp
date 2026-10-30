// Front end, Linux side of fe.h (fe_sys.cpp is the Windows one): files, the setup process, save backups, mods,
// controllers (SDL2, loaded at run time like the game does) and hardware detection. Folder pickers are zenity or
// kdialog, "open" is xdg-open. Updates, the Steam shortcut and moving an older installation over are Windows-only.
#include "fe.h"
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <cerrno>
#include <algorithm>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>

extern char** environ;

namespace fe {

Paths P;

// ---------------------------------------------------------------- strings / files
std::string utf8(const std::wstring& w) { std::string s; for (wchar_t c : w) s += c < 0x80 ? (char)c : '?'; return s; }
std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b; std::string r = a.back() == '/' ? a + b : a + "/" + b;
    for (auto& c : r) if (c == '\\') c = '/';   // the shared screens write Windows separators in a few places
    return r;
}
bool file_exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
bool dir_exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
long long file_size(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (long long)st.st_size : -1; }
bool make_dirs(const std::string& p) {
    if (p.empty() || dir_exists(p)) return true;
    size_t s = p.find_last_of('/'); if (s != std::string::npos && s > 0) make_dirs(p.substr(0, s));
    return mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
}
std::string read_text(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb"); if (!f) return {};
    std::string s; char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s;
}
bool write_text(const std::string& p, const std::string& s) {
    std::string tmp = p + ".tmp"; FILE* f = fopen(tmp.c_str(), "wb"); if (!f) return false;
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size(); ok = fclose(f) == 0 && ok;
    if (!ok) { unlink(tmp.c_str()); return false; }
    return rename(tmp.c_str(), p.c_str()) == 0;
}
std::string now_stamp() { time_t t = time(nullptr); tm l; localtime_r(&t, &l); char b[32]; strftime(b, sizeof b, "%Y%m%d-%H%M%S", &l); return b; }

static std::string data_home() {
    const char* x = getenv("XDG_DATA_HOME"); if (x && *x) return x;
    const char* h = getenv("HOME"); return std::string(h ? h : ".") + "/.local/share";
}
void Paths::init(const char* dir, const char* name) {
    exename = name;
    // absolute: setup runs in the recompiler's folder, and gets this folder as its --out
    char b[4096]; const char* d = dir && *dir ? dir : ".";
    exedir = realpath(d, b) ? b : d;
    exe = join(exedir, exename); ini = join(exedir, exename + ".ini"); data = join(exedir, exename + "_data");
    code = join(exedir, exename + "_code.so"); build = join(exedir, exename + "_build");
    // the recompiler (Python scripts, patches, GCC): the AppImage passes its own copy, a build from source has it next to the game
    const char* r = getenv("SWR_RECOMPILER"); recompiler = r && *r ? r : join(exedir, "recompiler");
    setup_ok = join(data, "setup.ok");
    appdata = join(data_home(), "MetroidDreadRecomp");
}

// ---------------------------------------------------------------- INI (the same line-preserving editor as fe_sys.cpp)
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
bool IniFile::save() const { std::string s; for (auto& l : lines) { s += l; s += "\n"; } return write_text(path, s); }
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

// ---------------------------------------------------------------- processes
static bool on_path(const char* tool) {
    const char* p = getenv("PATH"); if (!p) return false; std::string s = p; size_t i = 0;
    while (i <= s.size()) { size_t e = s.find(':', i); if (e == std::string::npos) e = s.size();
        std::string c = s.substr(i, e - i) + "/" + tool; if (access(c.c_str(), X_OK) == 0) return true; i = e + 1; }
    return false;
}
// runs argv, waits, returns its stdout (trailing newline removed); *code gets the exit status
static std::string run_capture(const std::vector<std::string>& argv, int* code) {
    int fd[2]; if (pipe(fd)) { if (code) *code = -1; return {}; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1); close(fd[0]); close(fd[1]);
        std::vector<char*> a; for (auto& s : argv) a.push_back((char*)s.c_str()); a.push_back(nullptr);
        execvp(a[0], a.data()); _exit(127);
    }
    close(fd[1]); std::string out; char b[4096]; ssize_t n;
    while ((n = read(fd[0], b, sizeof b)) > 0) out.append(b, (size_t)n);
    close(fd[0]); int st = 0; if (pid > 0) waitpid(pid, &st, 0);
    if (code) *code = pid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
// starts argv and forgets it (a viewer, the browser): double fork, so nothing is left to reap
static void run_detached(const std::vector<std::string>& argv) {
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid(); int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
            std::vector<char*> a; for (auto& s : argv) a.push_back((char*)s.c_str()); a.push_back(nullptr);
            execvp(a[0], a.data()); _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);
}

// ---------------------------------------------------------------- dialogs / shell
static std::string pick(const char* title, bool folder, const char* pattern) {
    int code = 0; std::string r;
    if (on_path("zenity")) {
        std::vector<std::string> a = {"zenity", "--file-selection", std::string("--title=") + title};
        if (folder) a.push_back("--directory"); else if (pattern) a.push_back(std::string("--file-filter=") + pattern);
        r = run_capture(a, &code);
    } else if (on_path("kdialog")) {
        std::vector<std::string> a = {"kdialog", "--title", title};
        if (folder) { a.push_back("--getexistingdirectory"); a.push_back(getenv("HOME") ? getenv("HOME") : "/"); }
        else { a.push_back("--getopenfilename"); a.push_back(getenv("HOME") ? getenv("HOME") : "/"); if (pattern) a.push_back(pattern); }
        r = run_capture(a, &code);
    } else {
        fprintf(stderr, "front end: no file chooser (install zenity or kdialog)\n");
        return {};
    }
    return code == 0 ? r : std::string();
}
std::string pick_folder(void*, const char* title) { return pick(title, true, nullptr); }
std::string pick_file(void*, const char* title, const wchar_t*, const wchar_t* fspec) { std::string p = utf8(fspec ? fspec : L""); return pick(title, false, p.empty() ? nullptr : p.c_str()); }
void open_in_explorer(const std::string& p) { run_detached({"xdg-open", p}); }
void open_in_notepad(const std::string& p) { run_detached({"xdg-open", p}); }

template <class F> static void walk(const std::string& root, const std::string& rel, F&& f) {
    DIR* d = opendir(join(root, rel).c_str()); if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name; if (n == "." || n == "..") continue;
        std::string r = rel.empty() ? n : rel + "/" + n;
        struct stat st; if (lstat(join(root, r).c_str(), &st)) continue;
        if (S_ISLNK(st.st_mode)) continue;   // never follow links
        if (S_ISDIR(st.st_mode)) { f(r, true, 0LL); walk(root, r, f); }
        else if (S_ISREG(st.st_mode)) f(r, false, (long long)st.st_size);
    }
    closedir(d);
}
static bool remove_tree(const std::string& p) {
    struct stat st; if (lstat(p.c_str(), &st)) return errno == ENOENT;
    if (S_ISDIR(st.st_mode)) {
        std::vector<std::string> kids; DIR* d = opendir(p.c_str());
        if (d) { while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n != "." && n != "..") kids.push_back(join(p, n)); } closedir(d); }
        for (auto& k : kids) remove_tree(k);
        return rmdir(p.c_str()) == 0;
    }
    return unlink(p.c_str()) == 0;
}
bool recycle(const std::string& p) {
    int code = -1; if (on_path("gio")) run_capture({"gio", "trash", "--", p}, &code);   // to the desktop's trash when there is one
    return code == 0 || remove_tree(p);
}
long long dir_size(const std::string& p, int* files) {
    long long t = 0; int n = 0; walk(p, "", [&](const std::string&, bool dir, long long sz) { if (!dir) { t += sz; n++; } });
    if (files) *files = n; return t;
}
static bool copy_file(const std::string& s, const std::string& d) {
    FILE* in = fopen(s.c_str(), "rb"); if (!in) return false;
    std::string tmp = d + ".part"; FILE* out = fopen(tmp.c_str(), "wb"); if (!out) { fclose(in); return false; }
    char b[1 << 16]; size_t n; bool ok = true;
    while ((n = fread(b, 1, sizeof b, in)) > 0) if (fwrite(b, 1, n, out) != n) { ok = false; break; }
    fclose(in); ok = fclose(out) == 0 && ok;
    if (!ok) { unlink(tmp.c_str()); return false; }
    return rename(tmp.c_str(), d.c_str()) == 0;
}
bool copy_tree(const std::string& src, const std::string& dst, std::string* err, bool overwrite) {
    if (!make_dirs(dst)) { if (err) *err = "cannot create " + dst; return false; }
    bool ok = true;
    walk(src, "", [&](const std::string& r, bool dir, long long) {
        if (!ok) return; std::string d = join(dst, r);
        if (dir) { if (!make_dirs(d)) { ok = false; if (err) *err = "cannot create " + d; } return; }
        if (!overwrite && file_exists(d)) return;
        if (!copy_file(join(src, r), d)) { ok = false; if (err) *err = "cannot copy " + r; }
    });
    return ok;
}
bool trees_equal(const std::string& a, const std::string& b) {
    bool eq = true;
    walk(a, "", [&](const std::string& r, bool dir, long long) {
        if (!eq || dir) return;
        if (!file_exists(join(b, r)) || read_text(join(a, r)) != read_text(join(b, r))) eq = false; });
    return eq;
}

// ---------------------------------------------------------------- installed game
GameCheck check_installed() {
    GameCheck g;
    g.code_ok = file_exists(P.code) && file_size(P.code) > 50 * 1024 * 1024;
    g.exefs_ok = file_exists(join(P.data, "exefs/main")) && file_exists(join(P.data, "exefs/main.npdm"));
    g.romfs_ok = dir_exists(join(P.data, "romfs/system")) && dir_exists(join(P.data, "romfs/packs"));
    g.data_ok = g.exefs_ok && g.romfs_ok;
    g.setup_ok = file_exists(P.setup_ok);
    if (!g.code_ok) g.summary = "The game has not been compiled yet.";
    else if (!g.data_ok) g.summary = "Game data is missing.";
    else g.summary = "Ready.";
    return g;
}

// ---------------------------------------------------------------- setup process: dread_setup.py, read line by line
static std::mutex g_mx;
void setup_lock() { g_mx.lock(); }
void setup_unlock() { g_mx.unlock(); }
// the bundled Python (python-build-standalone in the AppImage), else the system's
std::string recompiler_python() { std::string p = join(P.recompiler, "python/bin/python3"); return file_exists(p) ? p : "python3"; }

static void reader(SetupJob* jp, int rd, pid_t pid) {
    SetupJob& j = *jp; std::string buf; char b[4096]; ssize_t n;
    FILE* lf = fopen(join(P.exedir, "setup.log").c_str(), "ab");
    while ((n = read(rd, b, sizeof b)) > 0) {
        buf.append(b, (size_t)n); size_t e;
        while ((e = buf.find('\n')) != std::string::npos) {
            std::string l = buf.substr(0, e); buf.erase(0, e + 1); if (!l.empty() && l.back() == '\r') l.pop_back();
            if (lf) { fwrite(l.data(), 1, l.size(), lf); fputc('\n', lf); fflush(lf); }
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
    close(rd);
    int st = 0; waitpid(pid, &st, 0); int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    setup_lock();
    if (!j.done && !j.failed) { j.failed = true; if (j.error.empty()) j.error = j.cancelled ? "Cancelled." : "The recompiler stopped unexpectedly (exit code " + std::to_string(code) + ")."; }
    if (code != 0 && !j.cancelled && j.error.empty()) j.error = "The compiler stopped with an error (see the log).";
    j.running = false; j.proc = nullptr;
    setup_unlock();
    if (lf) fclose(lf);
}

bool setup_start(SetupJob& j) {
    std::string py = recompiler_python(), script = join(P.recompiler, "dread_setup.py");
    if (!file_exists(script)) script = join(P.recompiler, "dread/dread_setup.py");   // SWR_RECOMPILER pointing at the repository
    if (!file_exists(script)) { j.failed = true; j.error = "The recompiler is missing: expected " + script + "."; return false; }
    if (py == "python3" && !on_path("python3")) { j.failed = true; j.error = "Python 3 was not found: install python3, or use the AppImage (it brings its own)."; return false; }
    std::vector<std::string> argv = {py, "-I", "-u", script, "--out", P.exedir, "--name", P.exename, "--base", j.base};
    if (!j.update.empty()) { argv.push_back("--update"); argv.push_back(j.update); }
    int fd[2]; if (pipe(fd)) return false;
    j.log.clear(); j.error.clear(); j.pct = 0; j.done = j.failed = j.cancelled = false; j.running = true; j.stage = "Starting"; j.message = "";
    { FILE* f = fopen(join(P.exedir, "setup.log").c_str(), "ab"); if (f) { fprintf(f, "\n===== setup %s =====\n", now_stamp().c_str()); fclose(f); } }
    pid_t pid = fork();
    if (pid == 0) {
        // its own process group, so cancel ends python and every gcc it started; below normal priority, as on Windows
        setpgid(0, 0); setpriority(PRIO_PROCESS, 0, 5);
        int nul = open("/dev/null", O_RDONLY); if (nul >= 0) dup2(nul, 0);
        dup2(fd[1], 1); dup2(fd[1], 2); close(fd[0]); close(fd[1]);
        if (chdir(P.recompiler.c_str())) {}
        std::vector<char*> a; for (auto& s : argv) a.push_back((char*)s.c_str()); a.push_back(nullptr);
        execvp(a[0], a.data()); fprintf(stderr, "@@E Could not start %s: %s\n", a[0], strerror(errno)); _exit(127);
    }
    close(fd[1]);
    if (pid < 0) { close(fd[0]); j.running = false; j.failed = true; j.error = "Could not start the recompiler."; return false; }
    setpgid(pid, pid);
    j.proc = (void*)(intptr_t)pid;
    std::thread(reader, &j, fd[0], pid).detach();
    return true;
}
void setup_cancel(SetupJob& j) { setup_lock(); if (j.running && j.proc) { j.cancelled = true; kill(-(pid_t)(intptr_t)j.proc, SIGKILL); } setup_unlock(); }

// ---------------------------------------------------------------- moving an older installation over: Windows-only (the Python launcher)
OldInstall describe_old(const std::string& exe) { OldInstall o; o.exe = exe; return o; }
std::vector<OldInstall> find_old_installs() { return {}; }
MigrateResult migrate(const OldInstall&, bool, bool, bool, bool) { return {}; }

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
    walk(src, "", [&](const std::string& r, bool dir, long long) { if (dir) return; ZipEntry e; e.name = "save/" + r; e.data = read_text(join(src, r)); es.push_back(e); });
    if (es.empty()) { if (err) *err = "The save folder is empty."; return false; }
    if (dir_size(src) != before) { if (err) *err = "Saves changed during the backup. Close the game and try again."; return false; }
    std::string z = stored_zip(es);
    make_dirs(backups_dir());
    std::string path = join(backups_dir(), "Metroid-Dread-saves-" + now_stamp() + ".zip");
    if (!write_text(path, z)) { if (err) *err = "Could not write " + path; return false; }
    std::string back = read_text(path);   // verify: same bytes, every entry's CRC still matches
    if (back != z) { if (err) *err = "Backup verification failed."; unlink(path.c_str()); return false; }
    for (auto& e : es) if (crc32(back.substr(e.off + 30 + e.name.size(), e.data.size())) != e.crc) { if (err) *err = "Backup verification failed."; return false; }
    if (out) *out = path; return true;
}

// ---------------------------------------------------------------- problem reports
std::string cpu_name() {
    std::string s = read_text("/proc/cpuinfo"); size_t p = s.find("model name");
    if (p == std::string::npos) return "";
    p = s.find(':', p); if (p == std::string::npos) return "";
    std::string n = s.substr(p + 1, s.find('\n', p) - p - 1);
    while (!n.empty() && n[0] == ' ') n.erase(0, 1); while (!n.empty() && n.back() == ' ') n.pop_back(); return n;
}
std::string reports_dir() { return join(P.exedir, "reports"); }
bool make_report(const std::string& info, std::string* out, std::string* err) {
    std::vector<ZipEntry> es; ZipEntry i; i.name = "system.txt"; i.data = info; es.push_back(i);
    const std::string n = P.exename;
    const std::string files[] = {n + ".log", n + ".previous.log", "performance.txt", "threads.txt", "setup.log", n + ".ini"};
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
    if (DIR* dd = opendir(d.c_str())) { while (dirent* e = readdir(dd)) { std::string n = e->d_name; if (n != "." && n != ".." && dir_exists(join(d, n))) present.push_back(n); } closedir(dd); }
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
    make_dirs(mods_dir()); std::string s = "# Mod load order: first line = lowest priority. '-' disables a mod.\n";
    for (auto& x : m) s += (x.enabled ? "" : "-") + x.name + "\n";
    write_text(join(mods_dir(), "load_order.txt"), s);
}
static std::string find_romfs(const std::string& root) {
    const char* id = "010093801237C000";
    std::string c[] = {join(root, "romfs"), join(root, std::string("atmosphere/contents/") + id + "/romfs"), join(root, std::string("atmosphere/titles/") + id + "/romfs"),
                       join(root, std::string("contents/") + id + "/romfs"), join(root, std::string(id) + "/romfs")};
    for (auto& x : c) if (dir_exists(x)) return x;
    std::string found; walk(root, "", [&](const std::string& r, bool dir, long long) { if (found.empty() && dir) { size_t s = r.find_last_of('/'); std::string leaf = s == std::string::npos ? r : r.substr(s + 1); if (leaf == "romfs") found = join(root, r); } });
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
bool mod_import_zip(const std::string& zip, std::string* err) {
    const char* t = getenv("TMPDIR"); std::string dir = join(t && *t ? t : "/tmp", "dreadmod_" + now_stamp()); make_dirs(dir);
    // Python's zipfile (the bundled Python, or the system's): it refuses names that leave the target folder
    int code = -1; run_capture({recompiler_python(), "-I", "-m", "zipfile", "-e", zip, dir}, &code);
    bool ok = code == 0;
    std::string name = zip.substr(zip.find_last_of('/') + 1); name = name.substr(0, name.find_last_of('.'));
    if (!ok) { if (err) *err = "The zip could not be extracted."; }
    else ok = mod_import_folder(dir, name, err);
    remove_tree(dir);
    return ok;
}
// Brazilian Portuguese fan translation (JumpmanClub Brasil, GitHub issue #15) and cheats: as fe_sys.cpp
const char* const PT_PACK_NAME = "Portuguese (Brazil) - JumpmanClub Brasil";
const char* const PT_PACK_URL = "https://www.jumpmanclubbrasil.com.br/post/metroid-dread";
bool pt_pack_installed() { return file_exists(join(join(join(mods_dir(), PT_PACK_NAME), "romfs"), "system/localization/us_english.txt")); }
bool pt_pack_import(const std::string& src, std::string* err) {
    std::string low = src; for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
    bool zip = low.size() > 4 && low.compare(low.size() - 4, 4, ".zip") == 0, ok;
    if (zip) {
        const char* t = getenv("TMPDIR"); std::string dir = join(t && *t ? t : "/tmp", "dreadpt_" + now_stamp()); make_dirs(dir);
        int code = -1; run_capture({recompiler_python(), "-I", "-m", "zipfile", "-e", src, dir}, &code);
        ok = code == 0; if (!ok && err) *err = "The zip could not be extracted.";
        if (ok) ok = mod_import_folder(dir, PT_PACK_NAME, err);
        remove_tree(dir);
    } else ok = mod_import_folder(src, PT_PACK_NAME, err);
    if (!ok) return false;
    if (!pt_pack_installed()) { if (err) *err = "That isn't the Metroid Dread translation: no system/localization/us_english.txt inside."; recycle(join(mods_dir(), PT_PACK_NAME)); return false; }
    return true;
}
std::string cheats_dir() { return join(P.data, "cheats"); }
std::string game_build_id() {
    FILE* f = fopen(join(P.data, "exefs/main").c_str(), "rb"); if (!f) return "";
    unsigned char b[0x48]; size_t n = fread(b, 1, sizeof b, f); fclose(f); if (n < 0x48 || memcmp(b, "NSO0", 4)) return "";
    char s[17]; for (int i = 0; i < 8; i++) snprintf(s + i * 2, 3, "%02X", b[0x40 + i]); return s;
}
static bool is_bid_file(const std::string& n) {
    if (n.size() != 20) return false; for (int i = 0; i < 16; i++) if (!isxdigit((unsigned char)n[i])) return false;
    return strcasecmp(n.c_str() + 16, ".txt") == 0;
}
std::vector<CheatEntry> cheats_list() {
    std::vector<CheatEntry> out; std::string d = cheats_dir(), bid = game_build_id(); if (!dir_exists(d)) return out;
    std::vector<std::string> files;
    if (DIR* dd = opendir(d.c_str())) { while (dirent* e = readdir(dd)) { std::string n = e->d_name;
        if (n.size() > 4 && !strcasecmp(n.c_str() + n.size() - 4, ".txt") && strcasecmp(n.c_str(), "enabled.txt") && file_exists(join(d, n))) files.push_back(n); } closedir(dd); }
    std::sort(files.begin(), files.end());
    std::string en = read_text(join(d, "enabled.txt"));
    std::vector<std::string> on; { size_t i = 0; while (i < en.size()) { size_t e = en.find('\n', i); if (e == std::string::npos) e = en.size(); std::string l = en.substr(i, e - i); i = e + 1;
        while (!l.empty() && (l.back() == '\r')) l.pop_back(); if (!l.empty()) on.push_back(l); } }
    for (auto& fn : files) {
        if (is_bid_file(fn) && !bid.empty() && strncasecmp(fn.c_str(), bid.c_str(), 16)) continue;   // another version of the game
        std::string t = read_text(join(d, fn)); size_t i = 0;
        while (i < t.size()) { size_t e = t.find('\n', i); if (e == std::string::npos) e = t.size(); std::string l = t.substr(i, e - i); i = e + 1;
            size_t a = l.find_first_not_of(" \t\xEF\xBB\xBF"); if (a == std::string::npos) continue;
            char o = l[a], c = o == '[' ? ']' : o == '{' ? '}' : 0; if (!c) continue; size_t z = l.find(c, a + 1); if (z == std::string::npos) continue;
            std::string name = l.substr(a + 1, z - a - 1); if (name.size() > 95) name.resize(95);
            bool master = o == '{'; out.push_back({name, fn, master || std::find(on.begin(), on.end(), name) != on.end(), master}); }
    }
    return out;
}
void cheats_write(const std::vector<CheatEntry>& c) {
    std::string s; for (auto& e : c) if (e.on && !e.master) s += e.name + "\n";
    make_dirs(cheats_dir()); write_text(join(cheats_dir(), "enabled.txt"), s);
}
bool cheat_import(const std::string& txt, std::string* err) {
    std::string name = txt.substr(txt.find_last_of('/') + 1);
    if (name.size() < 5 || strcasecmp(name.c_str() + name.size() - 4, ".txt")) { *err = "Pick a cheat .txt file"; return false; }
    if (!strcasecmp(name.c_str(), "enabled.txt")) { *err = "That name is used by the game: rename the file first"; return false; }
    std::string bid = game_build_id();
    if (is_bid_file(name) && !bid.empty() && strncasecmp(name.c_str(), bid.c_str(), 16)) { *err = "These cheats are for another version of the game (yours is " + bid + ", version 2.1.0)"; return false; }
    make_dirs(cheats_dir());
    if (!copy_file(txt, join(cheats_dir(), name))) { *err = "Could not copy the file"; return false; }
    return true;
}
const IconPack ICON_PACKS[3] = {
    {"Switch to XBOX Layout", "Xbox", "Krydar", "https://gamebanana.com/dl/679191"},
    {"Switch to PS4 Layout", "PlayStation 4", "Nova77", "https://gamebanana.com/dl/680684"},
    {"Switch to PS5 Layout", "PlayStation 5", "xSeRosiSx", "https://gamebanana.com/dl/681751"},
};
bool icon_pack_installed(int i) { return dir_exists(join(join(mods_dir(), ICON_PACKS[i].name), "romfs")); }
bool icon_pack_get(int i, std::string* err) {   // some packs are .rar: the Linux build has no extractor for them yet
    if (err) *err = std::string("Download it from ") + ICON_PACKS[i].url + " and add it with Mods > Add.";
    open_in_explorer(ICON_PACKS[i].url); return false;
}
bool mod_remove(const std::string& name) {
    bool ok = recycle(join(mods_dir(), name));
    auto m = mods_list(); mods_write(m); return ok;
}
int steam_add_shortcut(const std::string&, const std::string&, std::string& msg) { msg = "Not available on Linux yet: add the AppImage to Steam with Add a Non-Steam Game."; return -1; }

// ---------------------------------------------------------------- updates (fe_update.cpp): Windows-only for now
std::string installed_version() { return ""; }
void update_check_async() {}
void update_install_async() {}
UpdateState update_state() { return {}; }
std::string update_notes_url() { return ""; }
bool update_rollback(std::string* msg, bool*, bool*) { if (msg) *msg = "Not available on Linux."; return false; }
bool update_restart() { return false; }

// ---------------------------------------------------------------- controllers: SDL2 from the system, as the game loads it (backend_x11.c)
typedef void* GC;
static int (*s_init)(unsigned); static void (*s_quit)(void); static int (*s_hint)(const char*, const char*); static int (*s_num)(void);
static int (*s_isgc)(int); static GC (*s_open)(int); static void (*s_close)(GC); static const char* (*s_name_i)(int);
static unsigned char (*s_btn)(GC, int); static short (*s_axis)(GC, int); static void (*s_update)(void); static void (*s_pump)(void);
static int (*s_rumble)(GC, unsigned short, unsigned short, unsigned); static int (*s_attached)(GC);
static void* s_mod; static bool s_ok; static GC s_pad[8]; static int s_n; static long long s_scan;
static long long now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
void pads_init() {
    s_mod = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_LOCAL); if (!s_mod) s_mod = dlopen("libSDL2.so", RTLD_NOW | RTLD_LOCAL);
    if (!s_mod) return;
#define L(v, n) *(void**)&v = dlsym(s_mod, n); if (!v) return;
    L(s_init, "SDL_Init") L(s_quit, "SDL_Quit") L(s_hint, "SDL_SetHint") L(s_num, "SDL_NumJoysticks") L(s_isgc, "SDL_IsGameController") L(s_open, "SDL_GameControllerOpen")
    L(s_close, "SDL_GameControllerClose") L(s_name_i, "SDL_GameControllerNameForIndex") L(s_btn, "SDL_GameControllerGetButton") L(s_axis, "SDL_GameControllerGetAxis")
    L(s_update, "SDL_GameControllerUpdate") L(s_pump, "SDL_PumpEvents") L(s_attached, "SDL_GameControllerGetAttached")
#undef L
    *(void**)&s_rumble = dlsym(s_mod, "SDL_GameControllerRumble");
    s_hint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0"); s_hint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");   // the hints the game sets
    s_ok = s_init(0x00002000u | 0x00000200u) == 0;
}
void pads_shutdown() {
    pads_rumble_off();
    if (s_ok) { for (int i = 0; i < s_n; i++) s_close(s_pad[i]); s_n = 0; s_quit(); s_ok = false; }   // the game starts SDL again with its own state
}
bool pads_sdl_loaded() { return s_ok; }
static void sdl_rescan() {
    if (!s_ok) return; long long now = now_ms(); if (now - s_scan < 1000 && s_scan) return; s_scan = now; s_pump();
    for (int i = 0; i < s_n;) if (!s_attached(s_pad[i])) { s_close(s_pad[i]); s_pad[i] = s_pad[--s_n]; } else i++;
    int n = s_num();
    for (int j = 0; j < n && s_n < 8; j++) { if (!s_isgc(j)) continue; GC g = s_open(j); if (!g) continue; bool have = false; for (int i = 0; i < s_n; i++) if (s_pad[i] == g) have = true; if (have) s_close(g); else s_pad[s_n++] = g; }
}
std::vector<PadInfo> pads_list() {
    std::vector<PadInfo> r;
    if (s_ok) { s_pump(); int n = s_num(); int k = 0; for (int j = 0; j < n; j++) if (s_isgc(j)) { const char* nm = s_name_i(j); r.push_back({++k, nm ? nm : "Controller"}); } }
    return r;
}
PadState pads_read(int, int index) {
    PadState st;
    if (s_ok) {
        sdl_rescan(); s_update();
        int first = index > 0 ? index - 1 : 0, last = index > 0 ? index : s_n;
        for (int i = first; i < last && i < s_n; i++) { GC g = s_pad[i]; unsigned long long b = 0;
            static const int map[16] = {1, 0, 3, 2, 7, 8, 9, 10, -1, -1, 6, 4, 13, 11, 14, 12};
            for (int k = 0; k < 16; k++) if (map[k] >= 0 && s_btn(g, map[k])) b |= 1ull << k;
            int lt = s_axis(g, 4), rt = s_axis(g, 5); if (lt > 8000) b |= 1 << 8; if (rt > 8000) b |= 1 << 9;
            if (s_btn(g, 5)) b |= 1ull << 30;
            if (b || !st.phys) { st.phys |= b; st.lx = s_axis(g, 0); st.ly = -s_axis(g, 1); st.rx = s_axis(g, 2); st.ry = -s_axis(g, 3); st.lt = lt; st.rt = rt; st.sdl = true; }
        }
        if (st.sdl && s_n) { int i = index > 0 ? index - 1 : 0; if (i < s_n) { int n = s_num(); for (int j = 0, k = 0; j < n; j++) if (s_isgc(j)) { if (k++ == i) { const char* nm = s_name_i(j); st.name = nm ? nm : "Controller"; } } } }
    }
    auto dz = [](int& v) { if (v > -7000 && v < 7000) v = 0; }; dz(st.lx); dz(st.ly); dz(st.rx); dz(st.ry);
    return st;
}
void pads_rumble(int pct) {
    unsigned short v = (unsigned short)(65535 * std::min(1.0, std::max(0.0, pct / 100.0 * 0.5)));
    if (s_ok && s_rumble) for (int i = 0; i < s_n; i++) s_rumble(s_pad[i], v, v, 250);
}
void pads_rumble_off() { if (s_ok && s_rumble) for (int i = 0; i < s_n; i++) s_rumble(s_pad[i], 0, 0, 0); }

// ---------------------------------------------------------------- hardware: screen and GPU come from the front end's window (fe.cpp fe_x11_*)
extern void fe_x11_screen(int* w, int* h, int* hz);
extern std::string fe_x11_gpu(int* vram_mb, bool* integrated);
Hardware detect_hardware() {
    Hardware h;
    const char* fake = getenv("SWR_FE_TEST") ? getenv("SWR_FE_FAKE_HW") : nullptr;   // tests: "gpu name|w|h|hz|cores|vram MB|ram GB"
    std::string gpu; int vram = 0; bool igpu = false;
    if (fake) {
        std::string s = fake; std::vector<std::string> v; size_t p = 0; for (;;) { size_t e = s.find('|', p); v.push_back(s.substr(p, e == std::string::npos ? std::string::npos : e - p)); if (e == std::string::npos) break; p = e + 1; }
        if (v.size() == 7) { h.w = atoi(v[1].c_str()); h.h = atoi(v[2].c_str()); h.hz = atoi(v[3].c_str()); h.cores = atoi(v[4].c_str()); gpu = v[0]; vram = atoi(v[5].c_str()); h.ram_gb = atoi(v[6].c_str()); }
    } else {
        h.cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
        fe_x11_screen(&h.w, &h.h, &h.hz);
        gpu = fe_x11_gpu(&vram, &igpu);
        long long pages = sysconf(_SC_PHYS_PAGES), ps = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && ps > 0) h.ram_gb = (int)(((long long)pages * ps + (1ll << 30) - 1) >> 30);
    }
    // the same ranking as fe_sys.cpp consider_gpu; Mesa says when the GPU shares system memory (an APU, the Steam Deck)
    std::string u = gpu; for (auto& c : u) c = (char)toupper((unsigned char)c);
    auto has = [&](const char* s) { return u.find(s) != std::string::npos; };
    igpu = igpu || (vram && vram < 1024) || has("780M") || has("760M") || has("680M") || has("890M") || has("880M") || has("VANGOGH") || has("RENOIR") || has("CEZANNE") || has("REMBRANDT") || has("PHOENIX")
           || has("RADEON(TM) GRAPHICS") || has("RADEON GRAPHICS") || has("INTEL(R) UHD") || has("INTEL(R) IRIS") || has("MESA INTEL") || (has("VEGA") && !has("RX VEGA"));
    int rank = igpu ? 1
             : (has("RX 64") || has("RX 65")) ? 2
             : (has("RTX 50") || has("RTX 40") || has("RTX 30") || has("RX 9") || has("RX 7") || has("RX 6")) ? 3
             : (has("RTX 20") || has("GTX 16") || has("GTX 10") || has("RX 5") || has("ARC")) ? 2
             : (has("NVIDIA") || has("RADEON RX")) ? 2 : 1;
    size_t paren = gpu.find(" ("); if (paren != std::string::npos) gpu = gpu.substr(0, paren);   // Mesa adds "(radeonsi, renoir, ACO, DRM ...)"
    h.gpu = gpu.empty() ? "unknown graphics card" : gpu; h.tier = rank == 3 ? "high" : rank == 2 ? "mid" : "low";
    h.rtx = has("RTX") && has("NVIDIA"); h.vram_mb = vram; h.integrated = igpu;
    return h;
}

} // namespace fe
