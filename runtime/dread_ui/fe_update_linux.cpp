// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
// any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
// Front end, Linux: updates from the project's GitHub releases for the AppImage (fe_update.cpp is the Windows one).
//
// On Linux a release is one file, Metroid_Dread-x86_64.AppImage (runtime + recompiler + GCC + Python), and the game
// it compiles lives outside it (AppRun: ~/.local/share/MetroidDreadRecomp). The release also carries update-linux.txt:
//   version <x.y.z> / tag <tag> / package <AppImage name> <sha256> <size> / recompiler <sha256 of the recompiler>
// and the AppImage has the same lines but the package one inside (AppRun copies them to update.txt next to the game).
// Updating downloads the newer AppImage next to the running one (curl, HTTPS only), checks size and SHA-256, keeps a
// copy of the old one in update_backup/<version>-<date>/ (Undo puts it back) and renames the new one over it, then the
// game restarts from it. When the recompiler changed, the game code is rebuilt after the restart (rebuild_pending):
// the new recompiler is only inside the new AppImage. A copy that isn't an AppImage (built from source, the Steam Deck
// package) says so and changes nothing.
#include "fe.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace fe {

static const char* REPO = "yozander2009-stack/Metroid-Dread-Recomp";
static const char* MANIFEST = "update-linux.txt";

struct LxManifest { std::string version, tag, pkg, pkg_sha, recompiler; long long pkg_size = 0; bool ok = false; };

static std::mutex g_um;
static UpdateState g_us; static LxManifest g_latest; static std::string g_latest_text;
UpdateState update_state() {
    std::lock_guard<std::mutex> l(g_um);
    // after a restart into an update whose recompiler changed: the game code is rebuilt with the new one
    if (g_us.stage == UP_IDLE || g_us.stage == UP_CURRENT || g_us.stage == UP_ERROR || g_us.stage == UP_REBUILD) {
        std::string mk = join(P.exedir, "rebuild_pending"); struct stat a, b;
        if (stat(mk.c_str(), &a) == 0) {
            if (stat(P.code.c_str(), &b) == 0 && b.st_mtime > a.st_mtime) {   // rebuilt since: done
                remove(mk.c_str());
                if (g_us.stage == UP_REBUILD) { g_us.stage = UP_CURRENT; g_us.rebuild = false; }
            } else { g_us.stage = UP_REBUILD; g_us.rebuild = true; g_us.restart = false; }
        }
    }
    return g_us;
}
static void set_error(const std::string& e) { std::lock_guard<std::mutex> l(g_um); g_us.stage = UP_ERROR; g_us.error = e; }
static void set_what(const std::string& w, float pct) { std::lock_guard<std::mutex> l(g_um); g_us.what = w; g_us.pct = pct; }

static LxManifest parse(const std::string& t) {
    LxManifest m; size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p); std::string l = t.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? t.size() : e + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t s1 = l.find(' '); if (l.empty() || l[0] == '#' || s1 == std::string::npos) continue;
        std::string k = l.substr(0, s1), v = l.substr(s1 + 1);
        if (k == "version") m.version = v; else if (k == "tag") m.tag = v; else if (k == "recompiler") m.recompiler = v;
        else if (k == "package") { size_t a = v.find(' '), b = a == std::string::npos ? a : v.find(' ', a + 1); if (b == std::string::npos) continue;
            m.pkg = v.substr(0, a); m.pkg_sha = v.substr(a + 1, b - a - 1); m.pkg_size = atoll(v.c_str() + b + 1); }
    }
    m.ok = !m.version.empty() && !m.pkg.empty() && m.pkg_sha.size() == 64 && m.pkg_size > 0 && m.pkg.find('/') == std::string::npos;
    return m;
}
static LxManifest installed() { return parse(read_text(join(P.exedir, "update.txt"))); }
std::string installed_version() { return installed().version; }

static int vcmp(const std::string& a, const std::string& b) {   // dotted numbers: 0.2.10 > 0.2.9
    const char *x = a.c_str(), *y = b.c_str();
    while (*x || *y) { long p = strtol(x, (char**)&x, 10), q = strtol(y, (char**)&y, 10); if (p != q) return p < q ? -1 : 1; if (*x == '.') x++; if (*y == '.') y++;
        if (*x && !isdigit((unsigned char)*x)) break; if (*y && !isdigit((unsigned char)*y)) break; }
    return 0;
}

static std::string base_url() { const char* e = getenv("SWR_UPDATE_URL"); return e && *e ? e : ""; }   // tests: a local server
static std::string manifest_url() { std::string b = base_url(); return b.empty() ? std::string("https://github.com/") + REPO + "/releases/latest/download/" + MANIFEST : b + "/" + MANIFEST; }
static std::string package_url(const LxManifest& m) { std::string b = base_url(); return b.empty() ? std::string("https://github.com/") + REPO + "/releases/download/" + m.tag + "/" + m.pkg : b + "/" + m.pkg; }
std::string update_notes_url() { std::lock_guard<std::mutex> l(g_um); return std::string("https://github.com/") + REPO + "/releases/tag/" + g_latest.tag; }

// runs argv (no shell), its output into *out; the exit code
static int run(const std::vector<std::string>& argv, std::string* out) {
    int fd[2]; if (pipe(fd)) return -1;
    pid_t p = fork();
    if (p == 0) { close(fd[0]); dup2(fd[1], 1); int n = open("/dev/null", O_RDWR); if (n >= 0) { dup2(n, 0); dup2(n, 2); }
        std::vector<char*> a; for (auto& s : argv) a.push_back((char*)s.c_str()); a.push_back(nullptr); execvp(a[0], a.data()); _exit(127); }
    close(fd[1]); char b[4096]; ssize_t n;
    while ((n = read(fd[0], b, sizeof b)) > 0) if (out && out->size() < (4u << 20)) out->append(b, (size_t)n);
    close(fd[0]); int st = 0; if (p > 0) waitpid(p, &st, 0); return p > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static bool url_ok(const std::string& u) { return u.rfind("https://", 0) == 0 || u.rfind("http://127.0.0.1", 0) == 0; }
// GET url into *body (small) or into the file path (large; progress from its size)
static bool http_get(const std::string& url, std::string* body, const std::string& path, long long expect, std::string* err) {
    if (!url_ok(url)) { *err = "refusing a non-HTTPS address"; return false; }
    std::string out; int code;
    if (body) code = run({"curl", "-fsSL", "--retry", "2", "--connect-timeout", "10", "--max-filesize", "1048576", url}, &out);
    else {
        bool done = false; std::thread prog([&] { while (!done) { struct stat st; if (expect > 0 && stat(path.c_str(), &st) == 0) set_what("Downloading", (float)(100.0 * st.st_size / expect)); usleep(200000); } });
        code = run({"curl", "-fsSL", "--retry", "2", "--connect-timeout", "10", "-o", path, url}, nullptr);
        done = true; prog.join();
    }
    if (code == 127) { *err = "curl is not installed"; return false; }
    if (code == 22) { *err = "no update information on the server yet"; return false; }
    if (code != 0) { *err = "could not reach the update server (curl " + std::to_string(code) + ")"; return false; }
    if (body) *body = out;
    return true;
}
static std::string sha256_file(const std::string& path) {
    std::string out; if (run({"sha256sum", "--", path}, &out) != 0 || out.size() < 64) return "";
    return out.substr(0, 64);
}
static bool copy_file_to(const std::string& s, const std::string& d) {
    FILE* a = fopen(s.c_str(), "rb"); if (!a) return false; FILE* b = fopen(d.c_str(), "wb"); if (!b) { fclose(a); return false; }
    char buf[1 << 16]; size_t n; bool ok = true; while ((n = fread(buf, 1, sizeof buf, a)) > 0) if (fwrite(buf, 1, n, b) != n) { ok = false; break; }
    fclose(a); ok = fclose(b) == 0 && ok; return ok;
}
static std::string appimage() { const char* a = getenv("APPIMAGE"); return a && *a ? a : ""; }

// ---------------------------------------------------------------- check
void update_check_async() {
    { std::lock_guard<std::mutex> l(g_um); if (g_us.stage == UP_CHECKING || g_us.stage == UP_DOWNLOADING) return; g_us = UpdateState(); g_us.stage = UP_CHECKING; }
    std::thread([] {
        std::string have = installed_version();
        { std::lock_guard<std::mutex> l(g_um); g_us.installed = have; }
        if (appimage().empty()) { set_error("Updates come with the AppImage. This copy was built from the source or unpacked: update it the same way."); return; }
        std::string t, err;
        if (!http_get(manifest_url(), &t, "", 0, &err)) { set_error(err); return; }
        LxManifest m = parse(t);
        if (!m.ok) { set_error("the update information could not be read"); return; }
        std::lock_guard<std::mutex> l(g_um);
        g_latest = m; g_latest_text = t; g_us.latest = m.version;
        g_us.stage = (have.empty() || vcmp(have, m.version) < 0) ? UP_AVAILABLE : UP_CURRENT;
    }).detach();
}

// ---------------------------------------------------------------- install: the new AppImage over the running one
void update_install_async() {
    { std::lock_guard<std::mutex> l(g_um); if (g_us.stage != UP_AVAILABLE) return; g_us.stage = UP_DOWNLOADING; g_us.pct = 0; g_us.what = "Downloading"; }
    std::thread([] {
        LxManifest m, have = installed(); std::string mtext;
        { std::lock_guard<std::mutex> l(g_um); m = g_latest; mtext = g_latest_text; }
        std::string ai = appimage(), err;
        std::string dir = ai.substr(0, ai.find_last_of('/')), part = join(dir, "." + m.pkg + ".update");
        remove(part.c_str());
        if (!http_get(package_url(m), nullptr, part, m.pkg_size, &err)) { remove(part.c_str()); set_error(err); return; }
        set_what("Checking the download", 100);
        if (file_size(part) != m.pkg_size || sha256_file(part) != m.pkg_sha) { remove(part.c_str()); set_error("the download is damaged (checksum mismatch): nothing was changed"); return; }
        chmod(part.c_str(), 0755);
        set_what("Installing", 100);
        // the old AppImage kept for Undo, with what it said it was
        std::string bk = join(join(P.exedir, "update_backup"), (have.version.empty() ? std::string("before") : have.version) + "-" + now_stamp());
        make_dirs(bk);
        std::string keep = join(bk, ai.substr(ai.find_last_of('/') + 1));
        if (!copy_file_to(ai, keep)) { remove(part.c_str()); set_error("could not keep a copy of the current version (disk full?): nothing was changed"); return; }
        chmod(keep.c_str(), 0755);
        write_text(join(bk, "appimage_path"), ai);
        if (file_exists(join(P.exedir, "update.txt"))) copy_file_to(join(P.exedir, "update.txt"), join(bk, "update.txt"));
        if (rename(part.c_str(), ai.c_str()) != 0) { remove(part.c_str()); set_error("could not replace " + ai + " (" + strerror(errno) + "): the previous version was kept"); return; }
        bool rebuild = !m.recompiler.empty() && m.recompiler != have.recompiler;
        if (rebuild) write_text(join(P.exedir, "rebuild_pending"), m.version + "\n");   // after the restart, with the new recompiler
        write_text(join(P.exedir, "update.txt"), mtext);
        std::lock_guard<std::mutex> l(g_um);
        g_us.stage = UP_INSTALLED; g_us.restart = true; g_us.rebuild = false; g_us.files = 1; g_us.installed = m.version; g_us.pct = 100;
    }).detach();
}

// ---------------------------------------------------------------- undo: the newest kept AppImage back in place
bool update_rollback(std::string* msg, bool* rebuild, bool* restart) {
    *rebuild = false; *restart = false;
    std::string root = join(P.exedir, "update_backup"), newest; time_t nt = 0;
    if (DIR* d = opendir(root.c_str())) { while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n == "." || n == ".." || n.rfind("undone-", 0) == 0) continue;
            struct stat st; std::string p = join(root, n); if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_mtime >= nt && file_exists(join(p, "appimage_path"))) { nt = st.st_mtime; newest = p; } } closedir(d); }
    if (newest.empty()) { *msg = "No earlier version to go back to."; return false; }
    std::string ai = read_text(join(newest, "appimage_path")); while (!ai.empty() && (ai.back() == '\n' || ai.back() == '\r')) ai.pop_back();
    std::string cur = appimage(); if (!cur.empty()) ai = cur;   // where this one runs from now
    std::string kept = join(newest, ai.substr(ai.find_last_of('/') + 1));
    if (!file_exists(kept)) { *msg = "The kept version is missing from " + newest + "."; return false; }
    std::string tmp = join(ai.substr(0, ai.find_last_of('/')), ".undo.part");
    if (!copy_file_to(kept, tmp) || chmod(tmp.c_str(), 0755) != 0 || rename(tmp.c_str(), ai.c_str()) != 0) { remove(tmp.c_str()); *msg = "Could not put the earlier version back."; return false; }
    LxManifest now = installed(), was = parse(read_text(join(newest, "update.txt")));
    if (file_exists(join(newest, "update.txt"))) copy_file_to(join(newest, "update.txt"), join(P.exedir, "update.txt"));
    if (!was.recompiler.empty() && was.recompiler != now.recompiler) write_text(join(P.exedir, "rebuild_pending"), was.version + "\n");
    std::string aside = join(root, "undone-" + now_stamp()); rename(newest.c_str(), aside.c_str());
    *restart = true;
    *msg = "Went back to the previous version" + (was.version.empty() ? std::string() : " (" + was.version + ")") + ". The game restarts with it.";
    std::lock_guard<std::mutex> l(g_um); g_us = UpdateState(); g_us.installed = was.version;
    return true;
}

// ---------------------------------------------------------------- restart: the AppImage again (it is the new one now)
bool update_restart() {
    std::string ai = appimage(); std::string exe = ai.empty() ? P.exe : ai;
    pid_t p = fork();
    if (p == 0) { setsid(); if (fork() == 0) { execl(exe.c_str(), exe.c_str(), (char*)nullptr); _exit(127); } _exit(0); }
    if (p < 0) return false;
    waitpid(p, nullptr, 0); return true;
}

} // namespace fe
