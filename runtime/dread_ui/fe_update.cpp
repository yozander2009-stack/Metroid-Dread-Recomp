// Front end: in-place updates from the project's GitHub releases.
//
// A release carries update.txt (a list of every file the release owns, except the bundled compiler and Python, with
// SHA-256 and size) and update-<version>.zip (those files). The installed copy's own update.txt says what is installed.
// Updating downloads the manifest, then the zip only if a file differs; every file is checked against its hash before
// anything is replaced. Replaced files go to update_backup\ (Undo puts them back). Downloads use WinHTTP over HTTPS: the
// files are written by this program, so they carry no "downloaded from the internet" mark, and nothing is started from
// them except this game's own EXE after a restart. No scripts, installers or administrator rights.
// If a file under recompiler\ changed (setup, translator, the verified patches), the game code is rebuilt from the
// files setup already installed (fe.cpp starts setup with the game's own _data folder).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#include "fe.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <functional>

namespace fe {

static const wchar_t* UA = L"MetroidDreadRecomp-updater";
static const char* REPO = "yozander2009-stack/Metroid-Dread-Recomp";

struct ManFile { std::string sha, path; long long size = 0; };
// extra / xfile (0.5.10+): the bundled Clang, a package of its own fetched only where it is missing (updates before it
// left recompiler\clang out). Older updaters skip both kinds of line
struct Manifest { std::string version, tag, pkg, pkg_sha, xpkg, xpkg_sha; long long pkg_size = 0, xpkg_size = 0;
                  std::vector<ManFile> files, xfiles; bool ok = false; };

static CRITICAL_SECTION g_ucs; static bool g_ucs_init;
static UpdateState g_us; static Manifest g_latest; static std::string g_latest_text;
static void ulock() { if (!g_ucs_init) { InitializeCriticalSection(&g_ucs); g_ucs_init = true; } EnterCriticalSection(&g_ucs); }
static void uunlock() { LeaveCriticalSection(&g_ucs); }
UpdateState update_state() { ulock(); UpdateState s = g_us; uunlock(); return s; }
static void set_error(const std::string& e) { ulock(); g_us.stage = UP_ERROR; g_us.error = e; uunlock(); }

// ---------------------------------------------------------------- manifest
static Manifest parse_manifest(const std::string& t) {
    Manifest m; size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p); std::string l = t.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? t.size() : e + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.empty() || l[0] == '#') continue;
        size_t s1 = l.find(' '); if (s1 == std::string::npos) continue;
        std::string k = l.substr(0, s1), v = l.substr(s1 + 1);
        if (k == "version") m.version = v;
        else if (k == "tag") m.tag = v;
        else if (k == "package" || k == "file" || k == "extra" || k == "xfile") {   // package|extra <name> <sha> <size> | file|xfile <sha> <size> <path with spaces>
            size_t a = v.find(' '), b = a == std::string::npos ? a : v.find(' ', a + 1); if (b == std::string::npos) continue;
            if (k == "package") { m.pkg = v.substr(0, a); m.pkg_sha = v.substr(a + 1, b - a - 1); m.pkg_size = atoll(v.c_str() + b + 1); }
            else if (k == "extra") { m.xpkg = v.substr(0, a); m.xpkg_sha = v.substr(a + 1, b - a - 1); m.xpkg_size = atoll(v.c_str() + b + 1); }
            else { ManFile f; f.sha = v.substr(0, a); f.size = atoll(v.substr(a + 1, b - a - 1).c_str()); f.path = v.substr(b + 1); (k == "file" ? m.files : m.xfiles).push_back(f); }
        }
    }
    m.ok = !m.version.empty() && !m.pkg.empty() && m.pkg_sha.size() == 64 && !m.files.empty();
    for (auto* fs : {&m.files, &m.xfiles})   // never outside the game folder
        for (auto& f : *fs)
            if (f.sha.size() != 64 || f.path.empty() || f.path.find("..") != std::string::npos || f.path[0] == '/' || f.path[0] == '\\' || f.path.find(':') != std::string::npos) m.ok = false;
    if (!m.xfiles.empty() && (m.xpkg.empty() || m.xpkg_sha.size() != 64)) m.xfiles.clear();   // a list without its package: ignored
    return m;
}
std::string installed_version() { Manifest m = parse_manifest(read_text(join(P.exedir, "update.txt"))); return m.version; }

static int vcmp(const std::string& a, const std::string& b) {   // dotted numbers: 0.2.10 > 0.2.9
    const char *x = a.c_str(), *y = b.c_str();
    while (*x || *y) { long p = strtol(x, (char**)&x, 10), q = strtol(y, (char**)&y, 10); if (p != q) return p < q ? -1 : 1; if (*x == '.') x++; if (*y == '.') y++;
        if (*x && !isdigit((unsigned char)*x)) break; if (*y && !isdigit((unsigned char)*y)) break; }
    return 0;
}

// ---------------------------------------------------------------- HTTPS
static std::string base_url() { const char* e = getenv("SWR_UPDATE_URL"); return e && *e ? e : ""; }   // tests: a local server
static std::string manifest_url() { std::string b = base_url(); return b.empty() ? std::string("https://github.com/") + REPO + "/releases/latest/download/update.txt" : b + "/update.txt"; }
static std::string package_url(const Manifest& m, const std::string& name) { std::string b = base_url(); return b.empty() ? std::string("https://github.com/") + REPO + "/releases/download/" + m.tag + "/" + name : b + "/" + name; }
std::string update_notes_url() { ulock(); std::string t = g_latest.tag; uunlock(); return std::string("https://github.com/") + REPO + "/releases/tag/" + t; }

// GET url into *body (small) or into the file `path` (large); progress into g_us.pct. Redirects (github.com -> its
// download host) are followed by WinHTTP; https only, except http://127.0.0.1 for tests.
static bool http_get(const std::string& url, std::string* body, const std::string& path, long long expect, std::string* err) {
    std::wstring w = wide(url); URL_COMPONENTS uc = {sizeof uc}; wchar_t host[256], up[2048]; uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = up; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) { *err = "bad address " + url; return false; }
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!https && wcscmp(host, L"127.0.0.1") != 0) { *err = "refusing a non-HTTPS address"; return false; }
    HINTERNET s = WinHttpOpen(UA, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(UA, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { *err = "network unavailable (WinHTTP " + std::to_string(GetLastError()) + ")"; return false; }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", up, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr);
    DWORD code = 0, cl = sizeof code;
    if (ok) WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &cl, WINHTTP_NO_HEADER_INDEX);
    if (!ok) *err = "could not reach the update server (error " + std::to_string(GetLastError()) + ")";
    else if (code != 200) { *err = code == 404 ? "no update information on the server yet" : "the update server answered " + std::to_string(code); ok = false; }
    FILE* f = nullptr;
    if (ok && body == nullptr) { f = _wfopen(wide(path).c_str(), L"wb"); if (!f) { *err = "cannot write " + path; ok = false; } }
    long long got = 0;
    while (ok) {
        DWORD avail = 0; if (!WinHttpQueryDataAvailable(r, &avail)) { *err = "download interrupted"; ok = false; break; }
        if (!avail) break;
        std::vector<char> b(avail); DWORD n = 0; if (!WinHttpReadData(r, b.data(), avail, &n)) { *err = "download interrupted"; ok = false; break; }
        if (body) { body->append(b.data(), n); if (body->size() > (1u << 20)) { *err = "update information too large"; ok = false; } }
        else if (fwrite(b.data(), 1, n, f) != n) { *err = "disk full?"; ok = false; }
        got += n; if (expect > 0) { ulock(); g_us.pct = (float)(100.0 * got / expect); uunlock(); }
    }
    if (f) fclose(f);
    if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return ok;
}

// ---------------------------------------------------------------- SHA-256 (CNG)
std::string sha256_file(const std::string& path) {
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; unsigned char d[32]; std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return out;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        FILE* f = _wfopen(wide(path).c_str(), L"rb");
        if (f) { std::vector<unsigned char> b(1 << 20); size_t n; bool ok = true;
            while ((n = fread(b.data(), 1, b.size(), f)) > 0) if (BCryptHashData(h, b.data(), (ULONG)n, 0) != 0) { ok = false; break; }
            fclose(f);
            if (ok && BCryptFinishHash(h, d, 32, 0) == 0) { char x[65]; for (int i = 0; i < 32; i++) sprintf(x + 2 * i, "%02x", d[i]); out = x; } }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

bool code_patches_stale() {
    static std::string last; static bool stale = false;
    std::string ok = read_text(P.setup_ok);
    if (ok == last) return stale;   // checked again only when setup.ok changes (after a setup)
    last = ok;
    std::string pj = join(join(P.recompiler, "dread"), "dread_v210_patches.json");
    if (ok.empty() || !file_exists(P.code) || !file_exists(pj)) return stale = false;
    std::string h = sha256_file(pj);
    return stale = !h.empty() && ok.find(h) == std::string::npos;
}

static std::string local_path(const std::string& rel) { std::string p = rel; for (auto& c : p) if (c == '/') c = '\\'; return join(P.exedir, p); }
static bool differs(const ManFile& f) { std::string p = local_path(f.path); return !file_exists(p) || file_size(p) != f.size || sha256_file(p) != f.sha; }
// the compiler's files: present and of the listed size (hashing 300 MB at every check would be slow; the download is
// checked file by file before anything is installed)
static bool xdiffers(const ManFile& f) { std::string p = local_path(f.path); return !file_exists(p) || file_size(p) != f.size; }
static long long xmissing(const Manifest& m) { long long n = 0; for (auto& f : m.xfiles) if (xdiffers(f)) n++; return n; }

static void remove_tree(const std::string& dir) {   // our own update_tmp only
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(wide(join(dir, "*")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { std::wstring n = fd.cFileName; if (n == L"." || n == L"..") continue; std::string p = join(dir, utf8(n));
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p); else { SetFileAttributesW(wide(p).c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(wide(p).c_str()); } } while (FindNextFileW(h, &fd)); FindClose(h); }
    RemoveDirectoryW(wide(dir).c_str());
}
static void remove_empty_dirs(const std::string& dir) {   // a folder whose files were all moved out (files are kept)
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(wide(join(dir, "*")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { std::wstring n = fd.cFileName; if (n != L"." && n != L".." && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) remove_empty_dirs(join(dir, utf8(n))); } while (FindNextFileW(h, &fd)); FindClose(h); }
    RemoveDirectoryW(wide(dir).c_str());
}
static void parent_dirs(const std::string& p) { size_t s = p.find_last_of('\\'); if (s != std::string::npos) make_dirs(p.substr(0, s)); }

// ---------------------------------------------------------------- check
static DWORD WINAPI check_thread(void*) {
    std::string t, err;
    if (!http_get(manifest_url(), &t, "", 0, &err)) { set_error(err); return 0; }
    Manifest m = parse_manifest(t);
    if (!m.ok) { set_error("the update information could not be read"); return 0; }
    std::string have = installed_version();
    bool newer = have.empty() || vcmp(have, m.version) < 0, compiler = !newer && vcmp(have, m.version) == 0 && xmissing(m) > 0;
    ulock(); g_latest = m; g_latest_text = t; g_us.latest = m.version; g_us.installed = have;
    g_us.compiler = compiler; g_us.extra_mb = (m.xpkg_size + (1 << 20) - 1) >> 20;
    g_us.stage = newer || compiler ? UP_AVAILABLE : UP_CURRENT; uunlock();
    return 0;
}
void update_check_async() {
    ulock(); if (g_us.stage == UP_CHECKING || g_us.stage == UP_DOWNLOADING) { uunlock(); return; } g_us = UpdateState(); g_us.stage = UP_CHECKING; g_us.installed = installed_version(); uunlock();
    CloseHandle(CreateThread(nullptr, 0, check_thread, nullptr, 0, nullptr));
}

// ---------------------------------------------------------------- install
// one package: downloaded, unpacked, every listed file checked against its hash, then moved in (the replaced ones to bk)
static bool install_pkg(const Manifest& m, const std::string& pkg, const std::string& sha, long long size,
                        const std::vector<ManFile>& changed, const std::string& tmp, const std::string& bk,
                        bool* rebuild, bool* exe, std::string* err) {
    std::string zip = join(tmp, pkg);
    ulock(); g_us.what = "Downloading " + pkg; g_us.pct = 0; uunlock();
    if (!http_get(package_url(m, pkg), nullptr, zip, size, err)) return false;
    if (file_size(zip) != size || sha256_file(zip) != sha) { *err = "the download is damaged (checksum mismatch): nothing was changed"; return false; }
    ulock(); g_us.what = "Unpacking"; uunlock();
    std::string x = join(tmp, "x-" + pkg); make_dirs(x);
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);   // Windows' own tar (10 1803+), never one from PATH
    std::wstring tar = std::wstring(sys) + L"\\tar.exe", cmd = L"\"" + tar + L"\" -xf \"" + wide(zip) + L"\" -C \"" + wide(x) + L"\"";
    std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
    STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi; DWORD code = 1;
    if (!CreateProcessW(tar.c_str(), c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { *err = "could not unpack (tar.exe is missing)"; return false; }
    WaitForSingleObject(pi.hProcess, 600000); GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    for (auto& f : changed) {   // every new file checked before anything is replaced
        std::string rel = f.path; for (auto& ch : rel) if (ch == '/') ch = '\\';
        if (sha256_file(join(x, rel)) != f.sha) { *err = "the update package does not match its file list (" + f.path + "): nothing was changed"; return false; }
    }
    ulock(); g_us.what = "Installing"; uunlock();
    std::vector<std::pair<std::string, std::string>> done;   // (installed path, backup path or "") for undo on failure
    for (auto& f : changed) {
        std::string rel = f.path; for (auto& ch : rel) if (ch == '/') ch = '\\';
        std::string dst = local_path(f.path), src = join(x, rel), b;
        parent_dirs(dst);
        bool fail = false;
        if (file_exists(dst)) { b = join(bk, rel); parent_dirs(b);   // a running EXE or loaded DLL can be renamed, not overwritten
            if (!MoveFileExW(wide(dst).c_str(), wide(b).c_str(), MOVEFILE_REPLACE_EXISTING)) { *err = "could not move " + f.path + " aside (error " + std::to_string(GetLastError()) + ")"; fail = true; } }
        if (!fail && !MoveFileExW(wide(src).c_str(), wide(dst).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            *err = "could not install " + f.path + " (error " + std::to_string(GetLastError()) + ")"; if (!b.empty()) MoveFileExW(wide(b).c_str(), wide(dst).c_str(), MOVEFILE_REPLACE_EXISTING); fail = true; }
        if (fail) {   // put back what was already replaced
            for (auto it = done.rbegin(); it != done.rend(); ++it) { if (!it->second.empty()) MoveFileExW(wide(it->second).c_str(), wide(it->first).c_str(), MOVEFILE_REPLACE_EXISTING); else DeleteFileW(wide(it->first).c_str()); }
            *err += ": the previous version was kept"; return false;
        }
        done.push_back({dst, b});
        if (f.path.rfind("recompiler/", 0) == 0) *rebuild = true;
        if (_stricmp(f.path.c_str(), (P.exename + ".exe").c_str()) == 0) *exe = true;
    }
    return true;
}
static DWORD WINAPI install_thread(void*) {
    ulock(); Manifest m = g_latest; std::string mtext = g_latest_text; g_us.stage = UP_DOWNLOADING; g_us.pct = 0; g_us.what = "Checking files"; uunlock();
    std::vector<ManFile> changed, xchanged;
    for (auto& f : m.files) if (differs(f)) changed.push_back(f);
    for (auto& f : m.xfiles) if (xdiffers(f)) xchanged.push_back(f);
    std::string tmp = join(P.exedir, "update_tmp"), err; remove_tree(tmp); make_dirs(tmp);
    bool rebuild = false, exe = false;
    std::string have = installed_version(); std::string bk = join(join(P.exedir, "update_backup"), (have.empty() ? std::string("before") : have) + "-" + now_stamp());
    if (!changed.empty()) {
        if (!install_pkg(m, m.pkg, m.pkg_sha, m.pkg_size, changed, tmp, bk, &rebuild, &exe, &err)) { remove_tree(tmp); set_error(err); return 0; }
        std::string old = join(P.exedir, "update.txt"); if (file_exists(old)) { std::string b = join(bk, "update.txt"); parent_dirs(b); CopyFileW(wide(old).c_str(), wide(b).c_str(), FALSE); }
    }
    write_text(join(P.exedir, "update.txt"), mtext);   // the release is in: a failed compiler download below leaves it so
    if (!xchanged.empty()) {   // the bundled Clang (recompiler\clang): new files only, so nothing to back up
        if (!install_pkg(m, m.xpkg, m.xpkg_sha, m.xpkg_size, xchanged, tmp, bk, &rebuild, &exe, &err)) { remove_tree(tmp); set_error("the compiler download failed: " + err); return 0; }
    }
    remove_tree(tmp);
    ulock(); g_us.stage = rebuild ? UP_REBUILD : UP_INSTALLED; g_us.rebuild = rebuild; g_us.restart = exe || g_us.restart; g_us.files = (int)(changed.size() + xchanged.size()); g_us.installed = m.version; g_us.compiler = false; g_us.pct = 100; uunlock();
    return 0;
}
void update_install_async() {
    ulock(); bool ok = g_us.stage == UP_AVAILABLE; uunlock(); if (!ok) return;
    CloseHandle(CreateThread(nullptr, 0, install_thread, nullptr, 0, nullptr));
}

// ---------------------------------------------------------------- undo (the newest update_backup folder back in place)
// only the folders installs made (<version>-<date>); "undone-*" holds what an undo replaced and is never restored
bool update_rollback(std::string* msg, bool* rebuild, bool* restart) {
    std::string root = join(P.exedir, "update_backup"), dir; std::vector<std::string> rels;
    std::function<void(const std::string&)> walk = [&](const std::string& r) {
        WIN32_FIND_DATAW f; HANDLE k = FindFirstFileW(wide(join(join(dir, r), "*")).c_str(), &f); if (k == INVALID_HANDLE_VALUE) return;
        do { std::wstring n = f.cFileName; if (n == L"." || n == L"..") continue; std::string rr = r.empty() ? utf8(n) : join(r, utf8(n));
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk(rr); else rels.push_back(rr); } while (FindNextFileW(k, &f)); FindClose(k); };
    for (;;) {
        WIN32_FIND_DATAW fd; std::string newest; FILETIME nt = {};
        HANDLE h = FindFirstFileW(wide(join(root, "*")).c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { do { std::wstring n = fd.cFileName; if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n.rfind(L"undone-", 0) == 0) continue;
            if (newest.empty() || CompareFileTime(&fd.ftCreationTime, &nt) > 0) { newest = utf8(n); nt = fd.ftCreationTime; } } while (FindNextFileW(h, &fd)); FindClose(h); }
        if (newest.empty()) { *msg = "No earlier version to go back to."; return false; }
        dir = join(root, newest); rels.clear(); walk("");
        if (!rels.empty()) break;
        remove_empty_dirs(dir);   // an emptied leftover: try the one before it
    }
    std::string aside = join(root, "undone-" + now_stamp());
    *rebuild = false; *restart = false; int n = 0;
    for (auto& r : rels) {
        std::string cur = join(P.exedir, r), b = join(dir, r);
        if (file_exists(cur)) { std::string a = join(aside, r); parent_dirs(a); MoveFileExW(wide(cur).c_str(), wide(a).c_str(), MOVEFILE_REPLACE_EXISTING); }
        parent_dirs(cur);
        if (MoveFileExW(wide(b).c_str(), wide(cur).c_str(), MOVEFILE_REPLACE_EXISTING)) n++;
        if (_strnicmp(r.c_str(), "recompiler\\", 11) == 0) *rebuild = true;
        if (_stricmp(r.c_str(), (P.exename + ".exe").c_str()) == 0) *restart = true;
    }
    remove_empty_dirs(dir);
    *msg = "Went back to the previous version (" + std::to_string(n) + " files). The newer files are in update_backup\\" + aside.substr(root.size() + 1) + ".";
    ulock(); g_us = UpdateState(); g_us.installed = installed_version(); uunlock();
    return true;
}

// ---------------------------------------------------------------- restart into the new EXE
bool update_restart() {
    STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi;
    std::wstring cmd = L"\"" + wide(P.exe) + L"\""; std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
    if (!CreateProcessW(wide(P.exe).c_str(), c.data(), nullptr, nullptr, FALSE, 0, nullptr, wide(P.exedir).c_str(), &si, &pi)) return false;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return true;
}

} // namespace fe
