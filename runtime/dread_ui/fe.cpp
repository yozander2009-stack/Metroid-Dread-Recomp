// Metroid Dread front end: first-run setup and the Play / settings screens, drawn with Dear ImGui + Direct3D 11
// in the game's own window before the game starts (backend_win32.c). Direct3D leaves no pixel format on the window,
// so OpenGL or Vulkan can take it over afterwards: one process, one window.
// Linux (backend_x11.c): the same screens in their own X11 window with OpenGL 3 (the host at the end of this file); it
// closes on Play and the game opens its window. The system side is fe_sys_linux.cpp.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <objbase.h>
#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <GL/glx.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <cmath>
#include <algorithm>
#include "imgui.h"
#include "imgui_internal.h"
#ifdef _WIN32
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#else
#include "imgui_impl_opengl3.h"
#endif
#include "ui.h"
#include "fe.h"

#ifdef _WIN32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern "C" void backend_fe_fullscreen(int on);
extern "C" int backend_is_fullscreen(void);
#else
extern char** environ;
#endif

using namespace ui;
namespace fe {

// ---------------------------------------------------------------- state
#ifdef _WIN32
static HWND g_hwnd; static ID3D11Device* g_dev; static ID3D11DeviceContext* g_ctx; static IDXGISwapChain* g_sc; static ID3D11RenderTargetView* g_rtv;
#else
static void* g_hwnd;   // the pickers' owner window: none on Linux (zenity / kdialog open their own)
static void backend_fe_fullscreen(int on); static int backend_is_fullscreen(void);   // the front end's own window (host below)
static bool g_focused = true; static double g_rumble_off;   // window focus; when the PULSE test rumble stops
#endif
static bool g_quit, g_play, g_resize;
static IniFile g_ini; static bool g_ini_dirty;
enum Page { PG_SETUP, PG_PLAY, PG_DISPLAY, PG_GRAPHICS, PG_GAMEPLAY, PG_AUDIO, PG_CONTROLS, PG_MODS, PG_ADVANCED, PG_ABOUT, PG_COUNT };
static const char* const PAGE_NAMES[PG_COUNT] = {"SETUP", "PLAY", "DISPLAY", "GRAPHICS", "GAMEPLAY", "AUDIO", "CONTROLS", "MODS", "ADVANCED", "CREDITS"};
static Page g_page = PG_PLAY; static bool g_page_changed = true;
static Nav g_nav; static PadState g_pad; static PromptStyle g_prompts = PROMPT_AUTO;
static std::string g_flash; static double g_flash_t;
static SetupJob g_job; enum SetupStep { SS_WELCOME, SS_RUNNING, SS_FAILED, SS_DONE } ; static SetupStep g_ss = SS_WELCOME;
static bool g_restart_after_setup, g_do_restart;   // updates: start the new EXE after the rebuild / now
static bool g_first_install;   // the game was not compiled when this screen opened: settings are picked after setup
static std::vector<OldInstall> g_old; static bool g_mig_settings = true, g_mig_saves = true, g_mig_mods = true, g_mig_caches = true;
static std::string g_mig_report;
static Hardware g_hw; static bool g_hw_done;
static std::vector<Mod> g_mods; static int g_mod_sel = -1;
static int g_old_bind[NAV_COUNT];
static int g_frame_no; static bool g_test; static unsigned long long test_input();   // test hooks, below

static void flash(const std::string& s) { g_flash = s; g_flash_t = ImGui::GetTime(); }
static void go(Page p) { if (p != g_page) { g_page = p; g_page_changed = true; } }

// ---------------------------------------------------------------- settings access
static std::string get(const char* k) { const Opt* o = find_option(k); return g_ini.get(k, o ? o->def : ""); }
static void set(const char* k, const std::string& v) { const Opt* o = find_option(k); g_ini.set(o ? o->section : "Launcher", k, v); g_ini_dirty = true; }
static bool getb(const char* k) { return get(k) == "1"; }

static bool draw_opt(const char* key, const char* tag = nullptr) {
    const Opt* o = find_option(key); if (!o) return false;
    std::string cur = get(key); bool ch = false;
    std::string id = std::string("##") + key;
    switch (o->type) {
    case OPT_BOOL: { bool v = cur == "1"; if (row_bool(id.c_str(), o->label, &v, tag)) { set(key, v ? "1" : "0"); ch = true; } break; }
    case OPT_CHOICE: {
        std::vector<std::string> items; int idx = 0;
        for (int i = 0; i < (int)o->choices.size(); i++) { items.push_back(o->choices[i].label); if (cur == o->choices[i].value) idx = i; }
        if (row_choice(id.c_str(), o->label, &idx, items, tag)) { set(key, o->choices[idx].value); ch = true; } break; }
    case OPT_FLOAT: case OPT_INT: {
        float v = (float)atof(cur.c_str()); std::string t = o->type == OPT_INT ? fmt("%d", (int)lroundf(v)) : fmt("%.2f", v);
        if (!strcmp(key, "volume") || !strcmp(key, "rumble")) t = (int)v == 0 ? "Off" : fmt("%d%%", (int)lroundf(v));
        if (!strcmp(key, "stick_deadzone")) t = fmt("%d%%", (int)(v * 100 / 32767));
        if (row_slider(id.c_str(), o->label, &v, o->lo, o->hi, o->step, t.c_str(), tag)) { set(key, o->type == OPT_INT ? fmt("%d", (int)lroundf(v)) : fmt("%g", roundf(v * 100) / 100)); ch = true; }
        break; }
    }
    if (o->help && *o->help) help_text(o->help);
    return ch;
}

// ---------------------------------------------------------------- environment for the game (same switches the Python launcher set)
#ifdef _WIN32
static void env(const char* name, bool on) {
    SetEnvironmentVariableA(name, on ? "1" : nullptr); char b[160]; snprintf(b, sizeof b, "%s=%s", name, on ? "1" : ""); _putenv(b);
}
#else
static void env(const char* name, bool on) { if (on) setenv(name, "1", 1); else unsetenv(name); }
#endif
static void apply_env(const IniFile& ini) {
    // never inherit stray test / diagnostic switches
    std::vector<std::string> drop;
#ifdef _WIN32
    LPWCH all = GetEnvironmentStringsW();
    for (LPWCH p = all; *p; p += wcslen(p) + 1) { std::string s = utf8(p); if (_strnicmp(s.c_str(), "SWR_", 4) == 0) drop.push_back(s.substr(0, s.find('='))); }
    FreeEnvironmentStringsW(all);
#else
    // SWR_RECOMPILER is the AppImage's, not a switch
    for (char** p = environ; *p; p++) if (!strncmp(*p, "SWR_", 4) && strncmp(*p, "SWR_RECOMPILER=", 15)) { std::string s = *p; drop.push_back(s.substr(0, s.find('='))); }
#endif
    if (!getenv("SWR_FE_TEST")) for (auto& d : drop) env(d.c_str(), false);
    env("SWR_SYNC_SHADERS", ini.get("wait_for_shaders", "1") == "1");
    env("SWR_LOADING_60", ini.get("loading_60fps", "1") == "1");
    env("SWR_CUTSCENES_60FPS", ini.get("cutscenes_60fps", "0") == "1");
    env("SWR_SKIP_CUTSCENES", ini.get("skip_cutscenes", "0") == "1");
}

// ---------------------------------------------------------------- D3D11
#ifdef _WIN32
static void make_rtv() { ID3D11Texture2D* bb; g_sc->GetBuffer(0, IID_PPV_ARGS(&bb)); g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
static bool d3d_init() {
    DXGI_SWAP_CHAIN_DESC sd = {}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl; const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) return false;
    make_rtv(); return true;
}
static void d3d_shutdown() {
    if (g_ctx) { g_ctx->ClearState(); g_ctx->Flush(); }
    if (g_rtv) g_rtv->Release(); if (g_sc) g_sc->Release(); if (g_ctx) g_ctx->Release(); if (g_dev) g_dev->Release();
    g_rtv = nullptr; g_sc = nullptr; g_ctx = nullptr; g_dev = nullptr;
}
#endif

// ---------------------------------------------------------------- pieces
#ifndef _WIN32
void fe_x11_screen(int* w, int* h, int* hz);   // host below
#endif
static float native_scale() {
    bool docked = getb("docked"); std::string asp = get("aspect");
#ifdef _WIN32
    RECT r; GetWindowRect(GetDesktopWindow(), &r); float sh = (float)(r.bottom - r.top);
#else
    int sw = 1920, shi = 1080, hz = 60; fe_x11_screen(&sw, &shi, &hz); float sh = (float)shi;
#endif
    float s = sh / game_base_h(docked, asp.c_str()); s = roundf(s * 20) / 20; return std::max(0.25f, std::min(4.0f, s));
}

static void resolution_rows(bool live_tag) {
    bool docked = getb("docked"); std::string asp = get("aspect");
    float v = (float)atof(get("resolution").c_str()); if (v <= 0) v = 1;
    std::string t = res_text(v, docked, asp.c_str());
    if (row_slider("##res", "Internal resolution", &v, 0.25f, 4.0f, 0.05f, t.c_str(), live_tag ? "LIVE" : nullptr)) set("resolution", fmt("%g", roundf(v * 100) / 100));
    static const float pre[] = {0, 0.5f, 0.75f, 1, 1.5f, 2, 3, 4};
    std::vector<std::string> items = {"Native for this screen"}; for (int i = 1; i < 8; i++) items.push_back(res_text(pre[i], docked, asp.c_str()));
    int idx = -1; for (int i = 1; i < 8; i++) if (fabsf(pre[i] - v) < 0.001f) idx = i;
    int shown = idx < 0 ? 0 : idx;
    if (row_choice("##respre", "Quick pick", &shown, items)) set("resolution", fmt("%g", shown == 0 ? native_scale() : pre[shown]));
    help_text(fmt("Native for this screen is %s. Above your screen it is averaged down (smoother edges, no shimmer).", res_text(native_scale(), docked, asp.c_str()).c_str()).c_str());
}

static void controller_status_card() {
    section("CONTROLLER");
    std::string n = g_pad.name.empty() ? "No controller detected (keyboard and mouse work)" : g_pad.name + " \xE2\x80\x94 connected";
    row_text("Player 1", n.c_str(), g_pad.name.empty() ? C_DIM : C_OK);
    if (row_button("##ctlcfg", "Controller settings", "CONFIGURE")) go(PG_CONTROLS);
}

// ---------------------------------------------------------------- settings picked for this PC (Play screen AUTO-SET, Advanced,
// and once after first-time setup when nothing was brought over from an older installation)
struct Rec { float res = 1; std::string fps, af, asp, sharp, renderer, summary, note; };
static Rec recommend() {
    if (!g_hw_done) { g_hw = detect_hardware(); g_hw_done = true; }
    Rec r; float ratio = (float)g_hw.w / std::max(1, g_hw.h);
    r.asp = ratio >= 2.2f ? "21:9" : fabsf(ratio - 1.6f) < 0.02f ? "16:10" : "16:9";
    float base = game_base_h(true, r.asp.c_str()), native = roundf(g_hw.h / base * 20) / 20;
    r.res = g_hw.tier == "high" ? (g_hw.h >= 2000 ? native : std::max(native, 2.0f)) : g_hw.tier == "mid" ? std::min(native, 1.5f) : 1.0f;
    r.fps = g_hw.hz >= 230 && g_hw.tier == "high" && g_hw.cores >= 8 ? "240" : g_hw.hz >= 140 && g_hw.tier != "low" ? "144" : g_hw.hz >= 115 && g_hw.tier != "low" ? "120" : "1";
    r.af = g_hw.tier != "low" ? "16" : "4";
    r.sharp = base * r.res < g_hw.h - 1 ? "25" : "0";   // the picture is stretched to the screen: a light sharpen
    // renderer: above 60 FPS on a discrete NVIDIA / AMD card native Vulkan is clearly smoother (2026-10-23, RTX 5070, 240 cap:
    // seconds under 200 FPS 2 vs 33-35 with OpenGL, whose driver-call thread is the limit in heavy rooms). At 60 both hold 60, and
    // OpenGL is the most tested; integrated graphics stay on OpenGL (native Vulkan untested there). It falls back to OpenGL by itself.
    // 2026-10-23 owner: OpenGL stays the pick for now (native Vulkan is still experimental). When it becomes the default:
    //   r.renderer = !g_hw.integrated && (NVIDIA or AMD/Radeon in g_hw.gpu) && r.fps != "1" ? "vulkan_native" : "opengl";
    r.renderer = "opengl";
    r.summary = fmt("Resolution %s, %s, picture %s, anisotropic filtering %s\xC3\x97%s, %s, fullscreen.",
                    res_text(r.res, true, r.asp.c_str()).c_str(), r.fps == "1" ? "60 FPS" : (r.fps + " FPS").c_str(), r.asp.c_str(), r.af.c_str(),
                    r.sharp != "0" ? ", light sharpening" : "", r.renderer == "vulkan_native" ? "Native Vulkan (smoothest at high frame rates)" : "OpenGL");
    if (g_hw.ram_gb && g_hw.ram_gb < 8) r.note = fmt("This PC has %d GB of memory; the game needs about 4 GB, so 8 GB is recommended. Close other programs before playing.", g_hw.ram_gb);
#ifndef SWR_RELEASE   // the public release has no DLSS
    else if (g_hw.rtx) r.note = "Optional: Graphics > Native Vulkan + NVIDIA DLAA gives the cleanest edges on RTX cards (experimental renderer).";
#endif
    return r;
}
static void apply_rec(const Rec& r) {
    set("docked", "1"); set("fullscreen", "1"); set("resolution", fmt("%g", r.res)); set("fps_limit", r.fps); set("renderer", r.renderer); set("aspect", r.asp);
    set("anisotropy", r.af); set("sharpening", r.sharp); set("sleep_waits", "1"); set("texture_threads", "0"); set("prebuild_shaders", "1"); set("loading_60fps", "1");
    set("auto_set", "1");
}
static std::string hw_line() {
    if (!g_hw_done) { g_hw = detect_hardware(); g_hw_done = true; }
    return fmt("%s%s  \xC2\xB7  %d\xC3\x97%d at %d Hz  \xC2\xB7  %d CPU threads%s", g_hw.gpu.c_str(), g_hw.integrated ? " (integrated)" : "", g_hw.w, g_hw.h, g_hw.hz, g_hw.cores,
               g_hw.ram_gb ? fmt("  \xC2\xB7  %d GB memory", g_hw.ram_gb).c_str() : "");
}

// ---------------------------------------------------------------- setup screens
static std::string g_base, g_update;
static void setup_screen(ImVec2 a, ImVec2 b) {
    ImGui::SetNextWindowPos(a); ImGui::SetNextWindowSize({b.x - a.x, b.y - a.y});
    ImGui::Begin("##setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float W = b.x - a.x, colw = std::min(S(1100), W - S(120)), x0 = a.x + (W - colw) / 2;
    logo(dl, {x0, a.y + S(50)}, S(54), true);
    ImGui::SetCursorScreenPos({x0, a.y + S(230)});
    ImGui::BeginChild("##sbody", {colw, b.y - a.y - S(330)}, 0, ImGuiWindowFlags_NoBackground);
    if (g_ss == SS_WELCOME) {
        page_title("FIRST-TIME SETUP", "This turns your own copy of Metroid Dread (version 2.1.0) into a native PC game. "
                   "Nothing from Nintendo is included: you provide the game files, and they are compiled on this PC with the bundled GCC. "
                   "It takes a few minutes and needs about 7 GB of free space here.");
        section("YOUR GAME FILES");
        std::string bl = g_base.empty() ? "CHOOSE" : g_base; if (bl.size() > 48) bl = "..." + bl.substr(bl.size() - 45);
        if (row_button("##base", "Game folder (with exefs and romfs)", bl.c_str())) { std::string p = pick_folder(g_hwnd, "Choose your extracted Metroid Dread folder"); if (!p.empty()) { g_base = p; set("setup_base", p); } }
        help_text("Dump the game and its 2.1.0 update from your own Switch and extract them with the update applied (or use an emulator's Extract RomFS / Extract ExeFS with the update installed). "
                  "That gives the 2.1.0 ExeFS and an already merged RomFS. You can also pick a folder that has exefs and romfs directly.");
        std::string ul = g_update.empty() ? "OPTIONAL" : g_update; if (ul.size() > 48) ul = "..." + ul.substr(ul.size() - 45);
        if (row_button("##upd", "Update folder (if extracted separately)", ul.c_str())) { std::string p = pick_folder(g_hwnd, "Choose the extracted 2.1.0 update folder"); g_update = p; set("setup_update", p); }
        help_text("Only if your base game and the 2.1.0 update were extracted to separate folders. The update's ExeFS is used, and its RomFS files are "
                  "layered over the base game's. Every file is then checked against the 2.1.0 file list, so a partial or unmerged update is caught.");
        if (!g_old.empty()) {
            section("BRING OVER FROM YOUR EXISTING INSTALLATION");
            const OldInstall& o = g_old[0];
            row_text("Found", o.exe.c_str());
            row_bool("##ms", "Settings (graphics, controls, launcher choices)", &g_mig_settings);
            if (!o.saves.empty()) row_bool("##msv", fmt("Saves (%.1f KB, copied: the originals are only read)", o.save_bytes / 1024.0).c_str(), &g_mig_saves);
            if (!o.mods.empty()) row_bool("##mm", fmt("Mods and texture packs (%.0f MB)", o.mods_bytes / 1048576.0).c_str(), &g_mig_mods);
            if (o.cache_bytes) row_bool("##mc", fmt("Shader caches (%.0f MB, fewer first-time stutters)", o.cache_bytes / 1048576.0).c_str(), &g_mig_caches);
        }
        ImGui::Dummy({1, S(16)});
        ImGui::BeginDisabled(g_base.empty());
        if (big_button("##go", "START SETUP", {S(360), S(68)})) {
            g_job = SetupJob(); g_job.base = g_base; g_job.update = g_update; g_job.t0 = ImGui::GetTime();
            if (setup_start(g_job)) g_ss = SS_RUNNING; else g_ss = SS_FAILED;
        }
        ImGui::EndDisabled();
    } else if (g_ss == SS_RUNNING || g_ss == SS_FAILED || g_ss == SS_DONE) {
        setup_lock();
        float pct = g_job.pct; std::string stage = g_job.stage, msg = g_job.message, err = g_job.error; bool running = g_job.running, done = g_job.done, failed = g_job.failed;
        std::vector<std::string> tail(g_job.log.end() - std::min<size_t>(g_job.log.size(), 400), g_job.log.end());
        setup_unlock();
        if (done && !running && g_ss == SS_RUNNING) {
            g_ss = SS_DONE; g_focus_first = true;
            if (!g_old.empty()) { MigrateResult m = migrate(g_old[0], g_mig_settings, g_mig_saves, g_mig_mods, g_mig_caches); g_mig_report = m.text; g_ini.load(P.ini); }
            if (g_first_install && !g_restart_after_setup && (g_old.empty() || !g_mig_settings) && g_ini.get("auto_set", "").empty()) {
                Rec r = recommend(); apply_rec(r); g_ini.save(); g_ini_dirty = false;   // CONTINUE reloads the .ini from disk
                g_mig_report += (g_mig_report.empty() ? "" : "\n") + std::string("Settings picked for this PC: ") + r.summary + " Change them any time, or use AUTO-SET on the Play screen.";
            }
        }
        if (failed && !running && g_ss == SS_RUNNING) { g_ss = SS_FAILED; g_focus_first = true; }
        page_title(g_ss == SS_DONE ? "SETUP COMPLETE" : g_ss == SS_FAILED ? "SETUP STOPPED" : "BUILDING METROID DREAD",
                   g_ss == SS_RUNNING ? "You can leave this running. Everything is kept if it stops: Retry continues where it left off." : "");
        double el = ImGui::GetTime() - g_job.t0;
        std::string bar = fmt("%.0f%%  \xE2\x80\x94  %s", pct, stage.c_str());
        progress_bar(g_ss == SS_DONE ? 1.0f : pct / 100.0f, {ImGui::GetContentRegionAvail().x, S(54)}, g_ss == SS_DONE ? "100%  \xE2\x80\x94  Done" : bar.c_str());
        std::string eta = pct > 3 && running ? fmt("   \xC2\xB7   about %d min left", (int)ceil(el * (100 - pct) / pct / 60)) : "";
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C_DIM));
        ImGui::Text("%s   \xC2\xB7   %d:%02d elapsed%s", msg.c_str(), (int)el / 60, (int)el % 60, eta.c_str()); ImGui::PopStyleColor();
        if (g_ss == SS_FAILED) {
            ImGui::Dummy({1, S(8)});
            ImVec2 p = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - S(30));
            ImVec2 ts = ImGui::CalcTextSize(err.c_str(), nullptr, false, w - S(30));
            chamfer_rect(ImGui::GetWindowDrawList(), p, {p.x + w, p.y + ts.y + S(28)}, S(8), IM_COL32(60, 12, 12, 200), C_BAD);
            ImGui::SetCursorScreenPos({p.x + S(14), p.y + S(14)}); ImGui::TextUnformatted(err.c_str()); ImGui::PopTextWrapPos();
            ImGui::SetCursorScreenPos({p.x, p.y + ts.y + S(36)});
            if (row_button("##retry", "Try again (continues where it stopped)", "RETRY", true)) { g_job.t0 = ImGui::GetTime(); if (setup_start(g_job)) g_ss = SS_RUNNING; }
            if (row_button("##other", "Choose other game files", "BACK")) { g_ss = SS_WELCOME; g_page_changed = true; }
            if (row_button("##log", "Open the full log (setup.log)", "OPEN")) open_in_notepad(join(P.exedir, "setup.log"));
        } else if (g_ss == SS_DONE) {
            ImGui::Dummy({1, S(8)});
            if (!g_mig_report.empty()) help_text(g_mig_report.c_str());
            if (big_button("##cont", g_restart_after_setup ? "RESTART" : "CONTINUE", {S(320), S(64)})) {
                if (g_restart_after_setup) { g_do_restart = true; g_quit = true; } else { g_ini.load(P.ini); go(PG_PLAY); } }
        } else {
            if (row_button("##cancel", "Stop setup (you can continue it later)", "CANCEL")) setup_cancel(g_job);
        }
        section("LOG");
        ImGui::BeginChild("##log", {0, std::max(S(160), ImGui::GetContentRegionAvail().y - S(10))}, ImGuiChildFlags_Borders);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.78f);
        for (auto& l : tail) ImGui::TextUnformatted(l.c_str());
        if (running) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
        ImGui::EndChild();
        if (g_page_changed) { g_page_changed = false; }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- updates (fe_update.cpp)
// the game code is rebuilt from what setup already installed (the _data folder has the verified exefs and romfs)
static void start_rebuild(bool restart_after) {
    g_job = SetupJob(); g_job.base = P.data; g_job.t0 = ImGui::GetTime(); g_restart_after_setup = restart_after;
    g_ss = setup_start(g_job) ? SS_RUNNING : SS_FAILED; go(PG_SETUP);
}
// banner: only when there is something to do; otherwise the full UPDATES section on the Play page
static void update_rows(bool banner) {
    UpdateState u = update_state();
    std::string have = u.installed.empty() ? std::string("an older version") : u.installed;
    switch (u.stage) {
    case UP_AVAILABLE:
        if (row_button("##upd", fmt("Version %s is available (you have %s)", u.latest.c_str(), have.c_str()).c_str(), "UPDATE", true)) update_install_async();
        if (row_button("##updn", "What's new in it", "OPEN")) open_in_explorer(update_notes_url());
        if (!banner) help_text("Only the files that changed are downloaded, each one is checked before anything is replaced, and the "
                               "replaced files are kept so you can go back. Your saves and settings are not touched.");
        break;
    case UP_DOWNLOADING:
        progress_bar(u.pct / 100.0f, {ImGui::GetContentRegionAvail().x, S(40)}, fmt("%s  —  %.0f%%", u.what.c_str(), u.pct).c_str());
        break;
    case UP_INSTALLED:
        if (u.restart) { if (row_button("##updr", fmt("Version %s is installed. Restart to use it", u.installed.c_str()).c_str(), "RESTART", true)) { g_do_restart = true; g_quit = true; } }
        else if (!banner) row_text("Update", fmt("Version %s is installed (%d files)", u.installed.c_str(), u.files).c_str(), C_OK);
        break;
    case UP_REBUILD:
        if (row_button("##updb", fmt("Version %s is installed. The game code must be rebuilt (a few minutes)", u.installed.c_str()).c_str(), "REBUILD", true)) start_rebuild(u.restart);
        break;
    case UP_ERROR: if (!banner) row_text("Update", u.error.c_str(), C_WARN); break;
    case UP_CHECKING: if (!banner) row_text("Update", "Checking..."); break;
    case UP_CURRENT: if (!banner) row_text("Version", fmt("%s (the newest)", u.installed.c_str()).c_str(), C_OK); break;
    default: if (!banner) row_text("Version", have.c_str()); break;
    }
}

// ---------------------------------------------------------------- pages
static std::string url_enc(const std::string& s) {
    std::string o; for (unsigned char c : s) { if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == '_' || c == '~') o += (char)c; else o += fmt("%%%02X", c); } return o;
}
// Problem report: pack the logs, show the zip, and open the GitHub bug form with what we know filled in
static std::string g_report_url;   // tests (SWR_FE_REPORT): the page that would open
static void report_problem() {
    if (!g_hw_done) { g_hw = detect_hardware(); g_hw_done = true; }
    if (g_ini_dirty) { g_ini.save(); g_ini_dirty = false; }
    std::string ver = installed_version(), cpu = cpu_name(), rn = get("renderer");
    std::string info = fmt("version %s\r\nrenderer %s\r\ngpu %s\r\ncpu %s\r\n%s\r\n", ver.c_str(), rn.c_str(), g_hw.gpu.c_str(), cpu.c_str(), hw_line().c_str());
    std::string zip, err;
    if (!make_report(info, &zip, &err)) { flash(err); return; }
    if (!g_test) open_in_explorer(reports_dir());
    std::string url = "https://github.com/yozander2009-stack/Metroid-Dread-Recomp/issues/new?template=bug_report.yml&version=" + url_enc(ver) +
                      "&gpu=" + url_enc(g_hw.gpu) + "&cpu=" + url_enc(cpu) + (g_hw.ram_gb ? "&ram=" + url_enc(fmt("%d GB", g_hw.ram_gb)) : std::string());
    if (g_test) g_report_url = url; else open_in_explorer(url);
    flash("Logs packed: " + zip + ". Drag it into the bug report.");
}

static void page_play() {
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 p = ImGui::GetCursorScreenPos();
    logo(dl, {p.x, p.y + S(10)}, S(70), true);
    ImGui::Dummy({1, S(250)});
    GameCheck g = check_installed();
    bool ready = g.code_ok && g.data_ok;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ready ? C_OK : C_WARN));
    ImGui::TextUnformatted(ready ? "READY" : g.summary.c_str()); ImGui::PopStyleColor();
    ImGui::Dummy({1, S(8)});
    if (big_button("##play", "PLAY", {S(420), S(86)})) { if (ready) g_play = true; else go(PG_SETUP); }
    ImGui::Dummy({1, S(14)});
#ifdef _WIN32
    { UpStage st = update_state().stage; if (st == UP_AVAILABLE || st == UP_DOWNLOADING || st == UP_REBUILD || (st == UP_INSTALLED && update_state().restart)) { update_rows(true); ImGui::Dummy({1, S(10)}); } }
#endif
    // three tiles
    float w = ImGui::GetContentRegionAvail().x, tw = (w - S(24)) / 3, th = S(108);
    std::string rn = get("renderer"); const char* rl = rn == "vulkan" ? "Vulkan output" : rn == "vulkan_native" ? "Native Vulkan" : "OpenGL";
    std::string rr = rl; if (rn != "vulkan" && getb("dlss")) rr += " + DLAA";
    float rs = (float)atof(get("resolution").c_str()); if (rs <= 0) rs = 1;
    std::string fl = get("fps_limit"); std::string ft = fl == "0" ? "Unlimited" : (fl == "1" || fl.empty()) ? "60 FPS" : fl + " FPS";
    struct T { const char* id; const char* title; std::string value; Page page; } tiles[3] = {
        {"##t1", "RENDERER", rr, PG_GRAPHICS}, {"##t2", "RESOLUTION", res_text(rs, getb("docked"), get("aspect").c_str()), PG_GRAPHICS}, {"##t3", "FPS TARGET", ft, PG_GAMEPLAY}};
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine(0, S(12));
        ImVec2 q = ImGui::GetCursorScreenPos();
        bool c = ImGui::InvisibleButton(tiles[i].id, {tw, th}); nav_item(); bool f = ImGui::IsItemFocused(), h = ImGui::IsItemHovered();
        chamfer_rect(dl, q, {q.x + tw, q.y + th}, S(10), f || h ? C_PANEL_HI : C_PANEL, f ? C_CYAN : C_LINE, f ? S(2) : 1);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.75f, {q.x + S(16), q.y + S(14)}, C_DIM, tiles[i].title);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.15f, {q.x + S(16), q.y + S(44)}, C_TEXT, tiles[i].value.c_str());
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.7f, {q.x + S(16), q.y + th - S(26)}, f ? C_RED : C_CYAN_DIM, "CHANGE  \xE2\x96\xB8");
        if (c || (f && g_row.confirm)) go(tiles[i].page);
    }
    section("BEST SETTINGS FOR THIS PC");
    {
        Rec r = recommend();
        row_text("Detected", hw_line().c_str());
        if (row_button("##auto", "Not sure what to pick? Choose the best settings for this PC", "AUTO-SET", true)) { apply_rec(r); flash("Settings picked for this PC: " + r.summary); }
        help_text((r.summary + " Anything can still be changed afterwards.").c_str());
        if (!r.note.empty()) help_text(r.note.c_str());
    }
    controller_status_card();
    section("SAVES");
    bool have = dir_exists(join(P.data, "save"));
    row_text("Save data", have ? "Every slot is backed up together. Close the game first." : "No saves yet.");
    if (row_button("##bk", "Back up saves now", "BACK UP")) { std::string z, e; if (backup_saves(&z, &e)) flash("Saves backed up: " + z); else flash("Backup failed: " + e); }
    if (row_button("##ob", "Open backups folder", "OPEN")) { make_dirs(backups_dir()); open_in_explorer(backups_dir()); }
    if (row_button("##os", "Open saves folder", "OPEN")) { if (have) open_in_explorer(join(P.data, "save")); else flash("No saves yet."); }
#ifdef _WIN32   // Linux: the Steam shortcut and updates from the releases page aren't there yet
    section("STEAM");
    if (row_button("##steam", "Add Metroid Dread to Steam as a non-Steam game", "ADD")) {
        std::string m; steam_add_shortcut(P.exe, P.exedir, m); flash(m); }
    help_text("Uses this game's icon. Steam is closed and started again if it's running.");
    section("UPDATES");
    update_rows(false);
    { UpStage st = update_state().stage; if (st != UP_CHECKING && st != UP_DOWNLOADING && st != UP_AVAILABLE && row_button("##updc", "Check for a new version now", "CHECK")) update_check_async(); }
    { bool v = get("check_updates") != "0"; if (row_bool("##updauto", "Check for a new version when this screen opens", &v)) set("check_updates", v ? "1" : "0"); }
    if (dir_exists(join(P.exedir, "update_backup")) && row_button("##updu", "Go back to the version before the last update", "UNDO")) {
        std::string m; bool rb = false, rs = false;
        if (update_rollback(&m, &rb, &rs)) { flash(m); if (rb) start_rebuild(rs); else if (rs) { g_do_restart = true; g_quit = true; } } else flash(m); }
    help_text("Updates come from this project's GitHub releases page and only replace this game's own files.");
#endif
    section("PROBLEMS");
    if (row_button("##report", "Report a problem: pack the logs and open the bug report page", "REPORT")) report_problem();
#ifdef _WIN32
    help_text("The zip has this game's logs and settings, nothing else (no saves, no game files). Logs show your Windows user name in folder paths. Drag the zip into the report.");
#else
    help_text("The zip has this game's logs and settings, nothing else (no saves, no game files). Logs show your user name in folder paths. Drag the zip into the report.");
#endif
    section("FOLDERS");
    if (row_button("##gf", "Game folder", "OPEN")) open_in_explorer(P.exedir);
    if (row_button("##sc", "Screenshots and recordings", "OPEN")) { make_dirs(join(P.exedir, "captures")); open_in_explorer(join(P.exedir, "captures")); }
}

static void page_display() {
    page_title("DISPLAY", "Console mode, window and picture shape. These apply when the game starts.");
    section("CONSOLE MODE"); draw_opt("docked");
    section("WINDOW");
    bool fs = getb("fullscreen");
    if (draw_opt("fullscreen")) backend_fe_fullscreen(getb("fullscreen") ? 1 : 0); (void)fs;
    section("PICTURE SHAPE");
    if (draw_opt("aspect")) {
        set("ultrawide", get("aspect") == "21:9" ? "1" : "0");   // the Python launcher writes both; keep them in step
        if (get("aspect") != "16:9" && !getb("docked")) { set("docked", "1"); flash("Docked mode switched on: the picture shape only applies there"); }
    }
    section("LANGUAGE"); draw_opt("language");
    if (get("language") == "9") {   // the fan translation, installed by the player (svc_fs.c applies it)
        std::string err;
        if (pt_pack_installed()) row_text("Portuguese text", "Installed: the JumpmanClub Brasil fan translation", C_OK);
        else row_text("Portuguese text", "Not installed yet: the game shows English until you add it", C_WARN);
        if (row_button("##ptweb", "Download the translation from JumpmanClub Brasil's website", "OPEN"))
            ShellExecuteW(nullptr, L"open", wide(PT_PACK_URL).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (row_button("##ptzip", "Add the downloaded .zip", "CHOOSE")) { std::string z = pick_file(g_hwnd, "Choose the translation (.zip)", L"Zip archive", L"*.zip");
            if (!z.empty()) { flash("Installing..."); if (pt_pack_import(z, &err)) flash("Portuguese translation installed"); else flash(err); } }
        if (row_button("##ptdir", "Add the extracted folder", "CHOOSE")) { std::string d = pick_folder(g_hwnd, "Choose the extracted translation folder");
            if (!d.empty()) { if (pt_pack_import(d, &err)) flash("Portuguese translation installed"); else flash(err); } }
        help_text("Metroid Dread has no Portuguese of its own: this is the fan translation by JumpmanClub Brasil (MrVtR, CaninosVermelhos, "
                  "Davi Prado, hhgo cb, EsaanAlencar). It applies at the next game start. With it on, button symbols inside text "
                  "stay Switch-style: the translation and the icon packs change the same font texture.");
    }
}

static void page_graphics() {
    page_title("GRAPHICS", "Resolution, renderer and image quality.");
    section("RESOLUTION"); resolution_rows(false);
    section("RENDERER"); draw_opt("renderer", get("renderer") == "vulkan_native" ? "EXPERIMENTAL" : nullptr);
#ifndef SWR_RELEASE   // the public release's renderer has no DLSS
    if (get("renderer") != "vulkan" && file_exists(join(P.exedir, "nvngx_dlss.dll"))) {   // OpenGL and Native Vulkan
        bool v = getb("dlss"); if (row_bool("##dlss", "NVIDIA DLAA anti-aliasing", &v)) set("dlss", v ? "1" : "0");
        help_text("RTX cards. Uses nvngx_dlss.dll from the game folder (not part of this release). Applies at the next start.");
    }
#endif
    section("QUALITY");
    draw_opt("antialiasing"); draw_opt("sharpening"); draw_opt("anisotropy"); draw_opt("shadows");
    draw_opt("dynamic_resolution"); if (getb("dynamic_resolution")) draw_opt("dynamic_resolution_min");
    draw_opt("game_dynamic_resolution");
    section("HDR");
    draw_opt("hdr", "EXPERIMENTAL"); if (getb("hdr")) draw_opt("hdr_paper_white");
}

static void page_gameplay() {
    page_title("GAMEPLAY", "Frame rate, cinematics and loading.");
    section("FRAME RATE"); draw_opt("fps_limit"); draw_opt("forced_vsync"); draw_opt("show_fps");
    section("SCENES"); draw_opt("loading_60fps"); draw_opt("cutscenes_60fps");
#ifndef SWR_NO_SKIPCUT
    draw_opt("skip_cutscenes");
#endif
   
    section("LOADING AND RIDES"); draw_opt("fast_loading"); draw_opt("fast_forward"); if (getb("fast_forward")) draw_opt("fast_forward_speed");
    section("CHEATS");
    help_text(fmt("Atmosphere cheat files (.txt) for version 2.1.0 (build id %s). Cheats that change values (energy, missiles, items) work; "
                  "ones that patch the game's code can't, because the code is recompiled. You can also switch them in the in-game menu (F1).", game_build_id().c_str()).c_str());
    { static std::vector<CheatEntry> ch; if (g_focus_first || ch.empty()) ch = cheats_list(); bool changed = false;
      if (ch.empty()) row_text("Cheats", "None yet: add a cheat file below.", C_DIM);
      for (size_t i = 0; i < ch.size(); i++) {
          if (ch[i].master) { row_text(ch[i].name.c_str(), "Always on (master code)", C_DIM); continue; }
          bool on = ch[i].on; if (row_bool(fmt("##ch%d", (int)i).c_str(), ch[i].name.c_str(), &on)) { ch[i].on = on; changed = true; } }
      if (changed) { cheats_write(ch); flash("Cheats saved"); }
      if (row_button("##chadd", "Add a cheat file (.txt)", "CHOOSE")) {
          std::string f = pick_file(g_hwnd, "Choose a cheat file", L"Cheats", L"*.txt");
          if (!f.empty()) { std::string err; if (cheat_import(f, &err)) { ch = cheats_list(); flash("Cheat file added"); } else flash(err); } }
      if (row_button("##chdir", "Cheats folder", "OPEN")) { make_dirs(cheats_dir()); open_in_explorer(cheats_dir()); } }
}

static void page_audio() {
    page_title("AUDIO", "");
    section("OUTPUT"); draw_opt("volume");
    section("SPEAKERS"); draw_opt("audio_channels");
    section("COMPATIBILITY"); draw_opt("audio_surround_downmix");
}

static const char* const GAME_BUTTONS[17] = {"A", "B", "X", "Y", "Left stick click", "Right stick click", "L", "R", "ZL", "ZR", "+ (pause)", "- (map)",
    "D-pad left", "D-pad up", "D-pad right", "D-pad down", "Disabled"};
#ifdef _WIN32   // Windows key codes (the X11 backend has its own fixed keys)
// keyboard_map (main.c, backend_win32.c keymap): 24 actions, each up to two Windows key codes
static const char KB_DEFAULT[] = "88+32,90,67,86,16,70,81,69,49,51,13,8,37,38,39,40,65,87,68,83,74,73,76,75";
static const char* const KB_ACTIONS[24] = {"A", "B (jump)", "X", "Y (shoot)", "Left stick click", "Right stick click", "L (free aim)", "R (missiles)", "ZL", "ZR",
    "+ (pause)", "- (map)", "D-pad left", "D-pad up", "D-pad right", "D-pad down", "Move left", "Move up", "Move right", "Move down",
    "Right stick left", "Right stick up", "Right stick right", "Right stick down"};
static int g_kb_capture = -1;   // the action waiting for a key press, -1 = none
static void kb_parse(const std::string& m, int k[24][2]) {
    for (int pass = 0; pass < 2; pass++) {
        const char* p = pass ? KB_DEFAULT : m.c_str(); if (!pass && !*p) continue;
        bool ok = true; memset(k, 0, sizeof(int) * 48);
        for (int i = 0; i < 24 && ok; i++) {
            for (int j = 0;; j++) { char* e; long v = strtol(p, &e, 10); if (e == p || v < 0 || v > 255 || j > 1) { ok = false; break; }
                k[i][j] = (int)v; p = e; if (*p == '+') { p++; continue; } break; }
            if (ok && i < 23) { if (*p != ',') ok = false; else p++; }
        }
        if (ok && !*p) return;
    }
}
static std::string kb_string(int k[24][2]) {
    std::string v; for (int i = 0; i < 24; i++) { if (i) v += ","; v += std::to_string(k[i][0]); if (k[i][1]) v += "+" + std::to_string(k[i][1]); }
    return v;
}
static std::string key_name(int vk) {
    switch (vk) { case 0: return "-"; case VK_SPACE: return "Space"; case VK_RETURN: return "Enter"; case VK_BACK: return "Backspace";
        case VK_LEFT: return "Left arrow"; case VK_RIGHT: return "Right arrow"; case VK_UP: return "Up arrow"; case VK_DOWN: return "Down arrow";
        case VK_SHIFT: return "Shift"; case VK_CONTROL: return "Ctrl"; case VK_MENU: return "Alt"; }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC); LONG lp = (LONG)(sc << 16);
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_DIVIDE) lp |= 1 << 24;
    char b[64]; if (sc && GetKeyNameTextA(lp, b, sizeof b) > 0) return b;
    return fmt("Key %d", vk);
}
static bool kb_reserved(int vk) { return vk == VK_ESCAPE || vk == VK_TAB || (vk >= VK_F1 && vk <= VK_F24) || vk == VK_LWIN || vk == VK_RWIN; }
static void kb_assign(int vk) {
    int k[24][2]; kb_parse(get("keyboard_map"), k); std::string moved;
    for (int i = 0; i < 24; i++) for (int j = 0; j < 2; j++) if (i != g_kb_capture && k[i][j] == vk) {   // a key does one thing
        k[i][j] = 0; if (j == 0) { k[i][0] = k[i][1]; k[i][1] = 0; } moved = KB_ACTIONS[i]; }
    k[g_kb_capture][0] = vk; k[g_kb_capture][1] = 0;
    std::string v = kb_string(k); set("keyboard_map", v == KB_DEFAULT ? "" : v);
    flash(moved.empty() ? fmt("%s = %s", KB_ACTIONS[g_kb_capture], key_name(vk).c_str())
                        : fmt("%s = %s (taken from %s)", KB_ACTIONS[g_kb_capture], key_name(vk).c_str(), moved.c_str()));
    g_kb_capture = -1;
}
#endif

static void page_controls() {
#ifdef _WIN32
    page_title("CONTROLS", "Pair Bluetooth controllers in Windows Settings first. With SDL2 the game supports Switch Pro, Joy-Con, DualShock 4, "
               "DualSense, 8BitDo and Xbox controllers over USB or Bluetooth.");
    section("CONTROLLER SUPPORT");
    draw_opt("controller_api");
    bool sdl = file_exists(join(P.exedir, "SDL2.dll"));
    row_text("SDL2", sdl ? "Installed next to the game" : "Not installed: only Xbox (XInput) controllers work", sdl ? C_OK : C_WARN);
    if (row_button("##sdl", "Use another SDL2.dll", "CHOOSE")) {
        std::string f = pick_file(g_hwnd, "Choose SDL2.dll", L"SDL2", L"SDL2.dll");
        if (!f.empty()) { if (CopyFileW(wide(f).c_str(), wide(join(P.exedir, "SDL2.dll")).c_str(), FALSE)) flash("SDL2.dll installed: restart the launcher to use it"); else flash("Copy failed"); }
    }
    section("MOUSE"); draw_opt("mouse_aim"); if (getb("mouse_aim")) draw_opt("mouse_sensitivity");
#else   // controllers come from the system's SDL2 (backend_x11.c); the game mouse is Windows-only
    page_title("CONTROLS", "Pair Bluetooth controllers in your desktop's settings first. With SDL2 the game supports Switch Pro, Joy-Con, DualShock 4, "
               "DualSense, 8BitDo, Xbox and Steam Deck controls over USB or Bluetooth.");
    section("CONTROLLER SUPPORT");
    row_text("SDL2", pads_sdl_loaded() ? "Found on this system" : "Not found: install SDL2 (libSDL2-2.0.so.0) for controllers", pads_sdl_loaded() ? C_OK : C_WARN);
#endif
    section("BUTTON ICONS");
    draw_opt("button_icons");
    for (int i = 0; i < 3; i++) {
        const IconPack& k = ICON_PACKS[i];
        bool have = icon_pack_installed(i);
        if (have) row_text(fmt("%s icons", k.label).c_str(), fmt("Installed  (by %s)", k.author).c_str(), C_OK);
        else if (row_button(fmt("##ip%d", i).c_str(), fmt("%s icons: not installed  (by %s, from GameBanana)", k.label, k.author).c_str(), "GET")) {
            flash("Downloading..."); std::string err;
            if (icon_pack_get(i, &err)) flash(fmt("%s icons installed", k.label)); else flash(err);
        }
    }
    help_text("Icon packs are community mods from gamebanana.com, downloaded only when you press GET. Applies at the next game start.");
    section("PLAYER 1");
    int cur = atoi(get("controller_index").c_str());
    auto pads = pads_list(); int k = 0;
    if (row_button("##any", "Any connected controller", cur == 0 ? "SELECTED" : "USE")) set("controller_index", "0");
    for (auto& pd : pads) { if (pd.index < 0) continue; k++;
        std::string id = fmt("##pad%d", pd.index);
        if (row_button(id.c_str(), fmt("%d. %s", pd.index, pd.name.c_str()).c_str(), cur == pd.index ? "SELECTED" : "USE")) set("controller_index", std::to_string(pd.index)); }
    for (auto& pd : pads) if (pd.index < 0) row_text("XInput", pd.name.c_str());
    if (pads.empty()) row_text("Connected", "No controller found. Connect or pair one.", C_DIM);
    // live tester
    section("LIVE TEST");
    { ImVec2 q = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, h = S(170); ImGui::Dummy({w, h});
      ImDrawList* dl = ImGui::GetWindowDrawList(); chamfer_rect(dl, q, {q.x + w, q.y + h}, S(10), C_PANEL, C_LINE);
      PadState s = g_pad;
      for (int i = 0; i < 2; i++) { ImVec2 c(q.x + S(90) + i * S(150), q.y + S(80)); float R = S(50);
          dl->AddCircle(c, R, C_DIM, 0, S(2)); float x = (i ? s.rx : s.lx) / 32767.0f, y = -(i ? s.ry : s.ly) / 32767.0f;
          dl->AddCircleFilled({c.x + x * R * 0.85f, c.y + y * R * 0.85f}, S(10), C_RED);
          dl->AddText({c.x - S(20), q.y + h - S(30)}, C_DIM, i ? "R stick" : "L stick"); }
      for (int i = 0; i < 2; i++) { float v = (i ? s.rt : s.lt) / 32767.0f; ImVec2 b0(q.x + S(330) + i * S(46), q.y + S(30));
          dl->AddRect(b0, {b0.x + S(30), b0.y + S(100)}, C_DIM); dl->AddRectFilled({b0.x, b0.y + S(100) * (1 - v)}, {b0.x + S(30), b0.y + S(100)}, C_RED);
          dl->AddText({b0.x + S(2), q.y + h - S(30)}, C_DIM, i ? "ZR" : "ZL"); }
      PromptStyle st = prompt_resolve(g_prompts, s.name.c_str()); float x = q.x + S(450), y = q.y + S(36);
      for (int b : {0, 1, 2, 3, 6, 7, 4, 5, 10, 11, 12, 13, 14, 15}) { bool on = (s.phys >> b) & 1;
          if (on) dl->AddCircleFilled({x, y}, S(19), IM_COL32(226, 30, 44, 120));
          draw_prompt(dl, {x, y}, S(15), b, st); x += S(50); if (x > q.x + w - S(40)) { x = q.x + S(450); y += S(50); } }
      if (s.phys >> 30 & 1) dl->AddText({q.x + S(450), q.y + h - S(30)}, C_CYAN, "HOME / GUIDE pressed");
    }
    section("FEEL");
    draw_opt("stick_deadzone"); draw_opt("rumble");
#ifdef _WIN32
    if (row_button("##rt", "Test rumble", "PULSE")) { pads_rumble(atoi(get("rumble").c_str())); SetTimer(g_hwnd, 77, 300, nullptr); }
#else
    if (row_button("##rt", "Test rumble", "PULSE")) { pads_rumble(atoi(get("rumble").c_str())); g_rumble_off = ImGui::GetTime() + 0.3; }
#endif
    draw_opt("menu_shortcut");
    section("GAME BUTTONS");
    draw_opt("menu_buttons");
    help_text("Physical position on your controller \xE2\x86\x92 the button Metroid Dread receives. Applies in the game right away. Keyboard and sticks are unchanged.");
    {
        std::string m = get("controller_map"); int map[16]; for (int i = 0; i < 16; i++) map[i] = i;
        if (!m.empty()) { const char* s = m.c_str(); for (int i = 0; i < 16; i++) { char* e; long v = strtol(s, &e, 10); if (e == s) break; map[i] = (int)v; s = *e == ',' ? e + 1 : e; } }
        std::vector<std::string> items(GAME_BUTTONS, GAME_BUTTONS + 17); bool ch = false;
        for (int i = 0; i < 16; i++) { int idx = map[i] < 0 ? 16 : map[i]; if (row_choice(fmt("##gm%d", i).c_str(), POSITION_NAMES[i], &idx, items)) { map[i] = idx == 16 ? -1 : idx; ch = true; } }
        if (row_button("##gmd", "Default (Switch positions)", "APPLY")) { for (int i = 0; i < 16; i++) map[i] = i; ch = true; }
        if (row_button("##gmx", "Match Xbox face letters (A bottom, B right)", "APPLY")) { int o[16] = {1, 0, 3, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}; memcpy(map, o, sizeof o); ch = true; }
        if (ch) { bool id = true; for (int i = 0; i < 16; i++) if (map[i] != i) id = false;
            std::string v; if (!id) for (int i = 0; i < 16; i++) { if (i) v += ","; v += std::to_string(map[i]); } set("controller_map", v); }
    }
    section("MENU NAVIGATION");
    { bool en = get("launcher_nav") != "0"; if (row_bool("##nav", "Navigate menus with a controller", &en)) { set("launcher_nav", en ? "1" : "0"); g_nav.enabled = en; } }
    { std::vector<std::string> st = {"Automatic", "Xbox", "PlayStation", "Nintendo", "Positions"}; int idx = (int)prompt_style_from(get("launcher_prompts").c_str());
      if (row_choice("##ps", "Button prompts", &idx, st)) { set("launcher_prompts", prompt_style_key((PromptStyle)idx)); g_prompts = (PromptStyle)idx; } }
    std::vector<std::string> pos(POSITION_NAMES, POSITION_NAMES + 12);
    int b[NAV_COUNT]; memcpy(b, g_nav.bind, sizeof b); bool ch = false;
    for (int i = 0; i < NAV_COUNT; i++) if (row_choice(fmt("##nb%d", i).c_str(), NAV_NAMES[i], &b[i], pos)) ch = true;
    if (ch) { Nav t; memcpy(t.bind, b, sizeof b); if (t.valid()) { memcpy(g_nav.bind, b, sizeof b); set("launcher_bindings", bindings_to_string(b)); }
              else { memcpy(g_nav.bind, b, sizeof b); flash("Two actions share a button: pick a different one (not saved until each action has its own)"); } }
    if (row_button("##nr", "Reset menu buttons", "RESET")) { for (int i = 0; i < NAV_COUNT; i++) g_nav.bind[i] = NAV_DEFAULT[i]; set("launcher_bindings", bindings_to_string(g_nav.bind)); flash("Menu buttons reset"); }
    help_text("Locked out? Hold + and - together for 3 seconds on any screen, or use the keyboard (arrows, Enter, Esc) or the mouse.");
#ifdef _WIN32
    section("KEYBOARD");
    help_text("Press SET, then the key. A key does one thing: giving it to another action takes it from the old one. Esc cancels. Esc, Tab and the F keys keep their jobs.");
    { int k[24][2]; kb_parse(get("keyboard_map"), k);
      for (int i = 0; i < 24; i++) {
          std::string keys = g_kb_capture == i ? "press a key..." : key_name(k[i][0]) + (k[i][1] ? " or " + key_name(k[i][1]) : "");
          if (row_button(fmt("##kb%d", i).c_str(), fmt("%s:  %s", KB_ACTIONS[i], keys.c_str()).c_str(), g_kb_capture == i ? "CANCEL" : "SET"))
              g_kb_capture = g_kb_capture == i ? -1 : i;
      }
      if (row_button("##kbr", "Default keys (WASD, X/Space, Z, C, V, Shift, F, arrows)", "RESET")) { set("keyboard_map", ""); g_kb_capture = -1; flash("Keyboard reset to the default keys"); } }
#else
    section("KEYBOARD");
#endif
    const char* kb[][2] = {{"Menu", "F1"}, {"Screenshot", "F3 (F8 full resolution)"},
                           {"Record video", "F7"}, {"Fullscreen", "F11 or Alt+Enter"}, {"Fast forward", "Tab"}, {"Quit", "Esc"},
                           {"Free aim (L)", "Hold right mouse button, move the mouse"}, {"Shoot (Y)", "Left mouse button"}, {"Missiles (R)", "Middle mouse button"}};
    for (auto& r : kb) row_text(r[0], r[1]);
}

static void page_mods() {
    page_title("MODS & TEXTURE PACKS", "Mods replace game files (RomFS): texture packs, models, text, sounds. Mods lower in the list win when two change the same file. "
               "Changes apply on the next game start.");
    if (g_focus_first) g_mods = mods_list();   // the page just opened
    section("INSTALLED");
    if (g_mods.empty()) row_text("Mods", "None installed yet.", C_DIM);
    bool ch = false;
    for (int i = 0; i < (int)g_mods.size(); i++) {
        Mod& m = g_mods[i]; bool en = m.enabled;
        bool icons = false; for (int k = 0; k < 3; k++) if (m.name == ICON_PACKS[k].name) icons = true;
        if (icons) { row_text(m.name.c_str(), "Button icons: chosen under Controls", C_DIM); continue; }
        if (m.name == PT_PACK_NAME) { row_text(m.name.c_str(), "Portuguese text: applies with Display > Language > Portuguese", C_DIM); continue; }
        std::string label = fmt("%s   (%d files, %.1f MB)", m.name.c_str(), m.files, m.bytes / 1048576.0);
        if (row_bool(fmt("##mod%d", i).c_str(), label.c_str(), &en)) { m.enabled = en; ch = true; }
        if (ImGui::IsItemFocused()) g_mod_sel = i;
    }
    if (g_mod_sel >= 0 && g_mod_sel < (int)g_mods.size()) {
        section(("SELECTED: " + g_mods[g_mod_sel].name).c_str());
        if (row_button("##mu", "Move up (lower priority)", "UP") && g_mod_sel > 0) { std::swap(g_mods[g_mod_sel], g_mods[g_mod_sel - 1]); g_mod_sel--; ch = true; }
        if (row_button("##md", "Move down (higher priority)", "DOWN") && g_mod_sel + 1 < (int)g_mods.size()) { std::swap(g_mods[g_mod_sel], g_mods[g_mod_sel + 1]); g_mod_sel++; ch = true; }
        if (row_button("##mr", "Remove (to the Recycle Bin)", "REMOVE")) { mod_remove(g_mods[g_mod_sel].name); g_mods = mods_list(); g_mod_sel = -1; flash("Mod moved to the Recycle Bin"); }
    }
    if (ch) mods_write(g_mods);
    section("ADD");
    std::string err;
    if (row_button("##az", "Add a .zip", "CHOOSE")) { std::string z = pick_file(g_hwnd, "Choose a mod or texture pack (.zip)", L"Zip archive", L"*.zip");
        if (!z.empty()) { flash("Extracting..."); if (mod_import_zip(z, &err)) flash("Mod installed"); else flash(err); g_mods = mods_list(); } }
    if (row_button("##af", "Add a folder", "CHOOSE")) { std::string d = pick_folder(g_hwnd, "Choose a mod folder (with romfs inside)");
        if (!d.empty()) { std::string n = d.substr(d.find_last_of("\\/") + 1); if (mod_import_folder(d, n, &err)) flash("Mod installed"); else flash(err); g_mods = mods_list(); } }
    if (row_button("##om", "Open the mods folder", "OPEN")) { make_dirs(mods_dir()); open_in_explorer(mods_dir()); }
    help_text("A mod is a folder with a 'romfs' inside, laid out like the game's own RomFS. Atmosphere-style zips (atmosphere/contents/010093801237C000/romfs) are recognised.");
}

static void page_advanced() {
    page_title("ADVANCED", "Shaders, CPU, memory, diagnostics and recommendations.");
    section("RECOMMENDED FOR THIS PC");
    {
        Rec r = recommend();
        row_text("Detected", hw_line().c_str());
        help_text((r.summary + (r.renderer == "vulkan_native" ? " Also: docked, shaders saved as you play, 60 FPS loading screens."
                                                              : " Also: docked, shaders built before playing, 60 FPS loading screens.")).c_str());
        if (row_button("##rec", "Apply the recommended settings", "APPLY", true)) { apply_rec(r); flash("Recommended settings applied"); }
        if (!r.note.empty()) help_text(r.note.c_str());
    }
    section("SHADERS"); draw_opt("wait_for_shaders"); draw_opt("prebuild_shaders"); draw_opt("low_priority_shader_compiler");
    section("CPU"); draw_opt("sleep_waits"); draw_opt("texture_threads");
    section("MEMORY"); draw_opt("memory_gb");
#ifdef _WIN32   // the F7 recorder is Media Foundation
    section("RECORDING (F7)"); draw_opt("smooth_recording"); draw_opt("recording_fps");
#endif
    section("DIAGNOSTICS"); draw_opt("log"); draw_opt("diag"); draw_opt("profile");
    section("START-UP");
    { bool v = getb("skip_launcher"); if (row_bool("##skip", "Start the game directly (skip this screen)", &v)) set("skip_launcher", v ? "1" : "0");
#ifdef _WIN32
      help_text("Hold Shift while starting the game to see this screen again."); }
#else
      help_text("Start the game with --launcher to see this screen again."); }
#endif
    section("FILES");
#ifdef _WIN32
    if (row_button("##ini", "Open the settings file in Notepad", "OPEN"))
#else
    if (row_button("##ini", "Open the settings file in a text editor", "OPEN"))
#endif
        { if (g_ini_dirty) { g_ini.save(); g_ini_dirty = false; } open_in_notepad(P.ini); }
    if (row_button("##slog", "Open the setup log", "OPEN")) open_in_notepad(join(P.exedir, "setup.log"));
    if (row_button("##resetup", "Check game files and rebuild", "SETUP")) { g_base = get("setup_base"); g_update = get("setup_update"); g_ss = SS_WELCOME; go(PG_SETUP); }
}

static void page_about() {
    page_title("CREDITS", "Metroid Dread recompiled for PC. Metroid Dread is \xC2\xA9 Nintendo; this program contains none of its files and needs your own copy.");
    struct C { const char* who; const char* what; } cs[] = {
        {"yozander2009-stack", "Project lead: this Metroid Dread PC version, its testing and its direction."},
        {"Claude (Anthropic)", "AI pair programmer: runtime fixes and performance work, this front end and in-game menu, setup, the Linux build, testing and documentation."},
        {"SwitchRecomp by MZ Labs", "The static recompiler and runtime this build is made from (Apache-2.0). \"SwitchRecomp by MZ Labs\" credit and NOTICE kept."},
        {"ScarletNX contributors", "Native Vulkan renderer: GPU front end, Maxwell to SPIR-V shader translation (GPL-3.0)."},
        {"Krydar, Nova77, xSeRosiSx", "Button-icon mods: Switch to XBOX Layout (Krydar), Switch to PS4 Layout (Nova77), Switch to PS5 Layout (xSeRosiSx), downloaded from gamebanana.com when you choose them."},
        {"Xelu's FREE Controller Prompts", "The PlayStation button art in those icon mods."},
        {"Smashicons - Flaticon", "The program icon: Metroid icon created by Smashicons, flaticon.com/free-icons/metroid (Flaticon licence)."},
        {"JumpmanClub Brasil", "Brazilian Portuguese fan translation (jumpmanclubbrasil.com.br), which you add under Display > Language: MrVtR (Vitor Ribeiro), translator, romhacker and proofreader; CaninosVermelhos (Philipe Rodrigues), Davi Prado and hhgo cb, translators; EsaanAlencar, proofreader."},
        {"theboy181", "60 FPS cutscenes, 21:9 ultrawide and other Dread patches (github.com/theboy181/switch-ptchtxt-mods); 21:9 commissioned by Nikorasu081."},
        {"Dear ImGui by Omar Cornut", "This interface (MIT)."},
        {"SDL2 (Sam Lantinga and contributors)", "Controller support (zlib licence)."},
        {"zlib (Jean-loup Gailly, Mark Adler)", "Fast game-data decompression (zlib licence)."},
        {"libvpx (WebM project)", "VP9 video playback (BSD)."},
        {"OpenH264 (Cisco)", "H.264 video decoding (BSD)."},
        {"GCC, binutils, MinGW-w64, Python", "The bundled compiler used by setup (GPL / LGPL with runtime exception, ZPL, PSF)."},
        {"Ryujinx and yuzu projects", "Public documentation of the Switch audio renderer and services, used as reference."},
    };
    for (auto& c : cs) { section(c.who); help_text(c.what); }
    section("LICENCES"); help_text("Full licence texts are in the licenses folder next to the game. The runtime is GPL-3.0 when built with the Native Vulkan renderer; its source is published with the release.");
    if (row_button("##lic", "Open the licenses folder", "OPEN")) open_in_explorer(join(P.exedir, "licenses"));
}

// ---------------------------------------------------------------- frame
static void sidebar(ImVec2 a, ImVec2 b) {
    ImGui::SetNextWindowPos(a); ImGui::SetNextWindowSize({b.x - a.x, b.y - a.y});
    ImGui::Begin("##side", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, IM_COL32(3, 6, 12, 200)); dl->AddLine({b.x - 1, a.y}, {b.x - 1, b.y}, C_LINE);
    logo(dl, {a.x + S(30), a.y + S(36)}, S(30), false);
    float y = a.y + S(160);
    for (int p = PG_PLAY; p < PG_COUNT; p++) {
        ImVec2 r0(a.x, y), r1(b.x, y + S(52));
        ImGui::SetCursorScreenPos(r0); bool c = ImGui::InvisibleButton(fmt("##nav%d", p).c_str(), {b.x - a.x, S(52)}); bool h = ImGui::IsItemHovered();
        bool sel = g_page == p;
        if (sel) { dl->AddRectFilledMultiColor(r0, r1, IM_COL32(226, 30, 44, 70), IM_COL32(226, 30, 44, 0), IM_COL32(226, 30, 44, 0), IM_COL32(226, 30, 44, 70)); dl->AddRectFilled(r0, {r0.x + S(5), r1.y}, C_RED); }
        else if (h) dl->AddRectFilled(r0, r1, IM_COL32(64, 214, 255, 18));
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.95f, {a.x + S(34), y + S(14)}, sel ? IM_COL32(255, 255, 255, 255) : C_DIM, PAGE_NAMES[p]);
        if (c) go((Page)p);
        y += S(54);
    }
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.62f, {a.x + S(30), b.y - S(56)}, C_DIM, "SwitchRecomp by MZ Labs  \xC2\xB7  ImGui");
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.62f, {a.x + S(30), b.y - S(36)}, C_DIM, "Game version 2.1.0");
    ImGui::End();
}

static void frame() {
    ImGuiIO& io = ImGui::GetIO(); float W = io.DisplaySize.x, H = io.DisplaySize.y;
    float sc = std::max(0.55f, std::min(2.2f, std::min(H / 1080.0f, W / 1700.0f)));
    if (fabsf(sc - g_scale) > 0.01f) apply_style(sc);
    ImDrawList* bg = ImGui::GetBackgroundDrawList(); background(bg, {0, 0}, {W, H}, (float)ImGui::GetTime(), false);
    if (g_page == PG_SETUP) { if (g_page_changed) { g_focus_first = true; g_page_changed = false; } setup_screen({0, 0}, {W, H - S(70)}); }
    else {
        float sw = S(300);
        sidebar({0, 0}, {sw, H - S(70)});
        ImGui::SetNextWindowPos({sw, 0}); ImGui::SetNextWindowSize({W - sw, H - S(70)});
        ImGui::Begin("##content", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        float cw = std::min(S(1150), W - sw - S(120));
        ImGui::SetCursorPos({S(60), S(40)});
        ImGui::BeginChild("##pg", {cw, H - S(70) - S(60)}, 0, ImGuiWindowFlags_NoBackground);
        if (g_page_changed) { ImGui::SetScrollY(0); g_focus_first = true; g_page_changed = false; }
        switch (g_page) {
        case PG_PLAY: page_play(); break; case PG_DISPLAY: page_display(); break; case PG_GRAPHICS: page_graphics(); break;
        case PG_GAMEPLAY: page_gameplay(); break; case PG_AUDIO: page_audio(); break; case PG_CONTROLS: page_controls(); break;
        case PG_MODS: page_mods(); break; case PG_ADVANCED: page_advanced(); break; case PG_ABOUT: page_about(); break; default: break;
        }
        ImGui::EndChild();
        ImGui::End();
    }
    // footer: prompts and messages
    ImGui::SetNextWindowPos({0, H - S(70)}); ImGui::SetNextWindowSize({W, S(70)});
    ImGui::Begin("##foot", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList(); dl->AddRectFilled({0, H - S(70)}, {W, H}, IM_COL32(2, 4, 9, 230)); dl->AddLine({0, H - S(70)}, {W, H - S(70)}, C_LINE);
    float x = S(30);
    if (g_nav.any_pad && g_nav.enabled) {
        PromptStyle st = prompt_resolve(g_prompts, g_pad.name.c_str());
        std::vector<std::pair<int, const char*>> it = {{g_nav.bind[NAV_CONFIRM], "Select"}, {g_nav.bind[NAV_BACK], "Back"}, {12, "Change"}};
        if (g_page != PG_SETUP) { it.push_back({g_nav.bind[NAV_PREV], "Page"}); it.push_back({g_nav.bind[NAV_NEXT], "Page"}); it.push_back({g_nav.bind[NAV_PLAY], "Play"}); }
        x += prompt_bar(it, st, {x, H - S(48)});
    } else dl->AddText({x, H - S(46)}, C_DIM, "Arrows: move   Enter: select   Left / Right: change   Esc: back   F11: fullscreen");
    if (!g_flash.empty() && ImGui::GetTime() - g_flash_t < 6) {
        ImVec2 ts = ImGui::CalcTextSize(g_flash.c_str()); dl->AddText({W - ts.x - S(30), H - S(46)}, C_CYAN, g_flash.c_str());
    }
    ImGui::End();
}

static void handle_nav() {
    ImGuiIO& io = ImGui::GetIO();
    double now = ImGui::GetTime();
    int api = get("controller_api") == "sdl" ? 1 : get("controller_api") == "xinput" ? 2 : 0;
#ifdef _WIN32
    bool fg = GetForegroundWindow() == g_hwnd;
#else
    bool fg = g_focused;
#endif
    g_pad = pads_read(api, atoi(get("controller_index").c_str()));
    unsigned long long phys = (fg ? g_pad.phys : 0) | test_input();
    if (g_test && test_input()) g_pad.name = g_pad.name.empty() ? "Test pad" : g_pad.name;
    if (g_nav.reset_hold(now, phys)) { set("launcher_bindings", bindings_to_string(g_nav.bind)); set("launcher_nav", "1"); flash("Menu buttons reset to the defaults"); }
    g_nav.update(phys, fg ? g_pad.lx : 0, fg ? g_pad.ly : 0, now);
    g_nav.feed_imgui(io);
    g_row.pad_left = g_nav.left || ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    g_row.pad_right = g_nav.right || ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    g_row.confirm = g_nav.act[NAV_CONFIRM];
    bool back = g_nav.act[NAV_BACK] || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (g_page != PG_SETUP) {
        if (g_nav.act[NAV_PREV] || ImGui::IsKeyPressed(ImGuiKey_PageUp)) go((Page)(g_page <= PG_PLAY ? PG_ABOUT : g_page - 1));
        if (g_nav.act[NAV_NEXT] || ImGui::IsKeyPressed(ImGuiKey_PageDown)) go((Page)(g_page >= PG_ABOUT ? PG_PLAY : g_page + 1));
        if (back && g_page != PG_PLAY) go(PG_PLAY);
        if (g_nav.act[NAV_PLAY]) { if (g_page == PG_PLAY) { GameCheck c = check_installed(); if (c.code_ok && c.data_ok) g_play = true; } else go(PG_PLAY); }
    } else if (back && g_ss == SS_FAILED) { g_ss = SS_WELCOME; g_page_changed = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_F11)) { bool fs = !backend_is_fullscreen(); backend_fe_fullscreen(fs); set("fullscreen", fs ? "1" : "0"); }
}

// ---------------------------------------------------------------- test hooks (SWR_FE_TEST=1 only; inert otherwise)
// SWR_FE_INPUT="FRAME:HEXBUTTONS,..." physical pad buttons from that front-end frame on (held until the next entry)
// SWR_FE_SHOTS="FRAME,..." + SWR_FE_SHOT_PREFIX=path: the window as PPM at those frames
// SWR_FE_AUTOSETUP="base|update": start setup at once; SWR_FE_PLAY_FRAME=N: press Play at frame N (also right after a
// finished setup); SWR_FE_EXIT_FRAME=N: quit; SWR_FE_RESULT=path: a line per second (frame, page, setup state, percent)
static unsigned long long test_input() {
    static const char* s = getenv("SWR_FE_INPUT"); if (!s) return 0;
    unsigned long long v = 0; const char* p = s;
    while (*p) { char* e; long f = strtol(p, &e, 10); if (*e != ':') break; unsigned long long b = strtoull(e + 1, &e, 16); if (f > g_frame_no) break; v = b; p = *e == ',' ? e + 1 : e; }
    return v;
}
static void test_shot() {
    static const char* s = getenv("SWR_FE_SHOTS"); static const char* pre = getenv("SWR_FE_SHOT_PREFIX"); if (!s || !pre) return;
    bool want = false; for (const char* p = s; *p;) { char* e; long f = strtol(p, &e, 10); if (f == g_frame_no) want = true; if (e == p) break; p = *e == ',' ? e + 1 : e; }
    if (!want) return;
#ifdef _WIN32
    ID3D11Texture2D* bb; g_sc->GetBuffer(0, IID_PPV_ARGS(&bb)); D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st; if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) { bb->Release(); return; }
    g_ctx->CopyResource(st, bb); D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        std::string path = fmt("%s-f%d.ppm", pre, g_frame_no); FILE* f = fopen(path.c_str(), "wb");
        if (f) { fprintf(f, "P6\n%u %u\n255\n", d.Width, d.Height);
            for (UINT y = 0; y < d.Height; y++) { const unsigned char* r = (const unsigned char*)m.pData + y * m.RowPitch; for (UINT x = 0; x < d.Width; x++) fwrite(r + x * 4, 1, 3, f); }
            fclose(f); }
        g_ctx->Unmap(st, 0);
    }
    st->Release(); bb->Release();
#else   // the back buffer of the front end's window, bottom row first in GL
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp); int w = vp[2], h = vp[3]; std::vector<unsigned char> px((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadBuffer(GL_BACK); glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    std::string path = fmt("%s-f%d.ppm", pre, g_frame_no); FILE* f = fopen(path.c_str(), "wb");
    if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); for (int y = h - 1; y >= 0; y--) fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f); fclose(f); }
#endif
}
static void test_tick() {
    if (!g_test) return;
    static int autostarted; static const char* as = getenv("SWR_FE_AUTOSETUP");
    if (as && !autostarted && g_page == PG_SETUP && g_ss == SS_WELCOME) { autostarted = 1;
        std::string v = as; size_t bar = v.find('|'); g_base = v.substr(0, bar); g_update = bar == std::string::npos ? "" : v.substr(bar + 1);
        g_job = SetupJob(); g_job.base = g_base; g_job.update = g_update; g_job.t0 = ImGui::GetTime(); g_ss = setup_start(g_job) ? SS_RUNNING : SS_FAILED; }
    static const char* pf = getenv("SWR_FE_PLAY_FRAME"); static const char* ef = getenv("SWR_FE_EXIT_FRAME");
    if (pf && g_frame_no >= atoi(pf) && g_page != PG_SETUP) g_play = true;
    if (pf && g_ss == SS_DONE) { g_ini.load(P.ini); g_page = PG_PLAY; g_play = true; }
    if (as && g_ss == SS_FAILED && !g_job.running && getenv("SWR_FE_QUIT_ON_FAIL")) g_quit = true;
    if (ef && g_frame_no >= atoi(ef)) g_quit = true;
    if (getenv("SWR_FE_AUTOSET") && g_frame_no == 30) {   // presses AUTO-SET, writes what was detected and picked, then quits
        Rec r = recommend(); apply_rec(r); g_ini.save(); g_ini_dirty = false;
        const char* rp = getenv("SWR_FE_RESULT"); FILE* f = rp ? fopen(rp, "a") : nullptr;
        if (f) { fprintf(f, "autoset tier=%s detected=%s | picked=%s | note=%s\n", g_hw.tier.c_str(), hw_line().c_str(), r.summary.c_str(), r.note.c_str()); fclose(f); }
        g_quit = true;
    }
    if (getenv("SWR_FE_REPORT") && g_frame_no == 30) {   // presses REPORT, writes the zip and the page it would open, then quits
        report_problem(); const char* rp = getenv("SWR_FE_RESULT"); FILE* f = rp ? fopen(rp, "a") : nullptr;
        if (f) { fprintf(f, "report msg=%s url=%s\n", g_flash.c_str(), g_report_url.c_str()); fclose(f); }
        g_quit = true;
    }
    if (getenv("SWR_FE_PT_IMPORT") && g_frame_no == 30) {   // installs the Portuguese translation from that path, writes the result, quits
        std::string err; bool ok = pt_pack_import(getenv("SWR_FE_PT_IMPORT"), &err);
        const char* rp = getenv("SWR_FE_RESULT"); FILE* f = rp ? fopen(rp, "a") : nullptr;
        if (f) { fprintf(f, "pt_import ok=%d installed=%d err=%s\n", ok, pt_pack_installed(), err.c_str()); fclose(f); }
        g_quit = true;
    }
    if (getenv("SWR_FE_UNDO")) {   // presses UNDO once, writes what it said to the result file, then quits
        static int done; if (!done) { done = 1; std::string m; bool rb = false, rs = false; bool ok = update_rollback(&m, &rb, &rs);
            const char* rp = getenv("SWR_FE_RESULT"); FILE* f = rp ? fopen(rp, "a") : nullptr;
            if (f) { fprintf(f, "undo ok=%d rebuild=%d restart=%d installed=%s msg=%s\n", ok, rb, rs, installed_version().c_str(), m.c_str()); fclose(f); }
            g_quit = true; } }
    if (getenv("SWR_FE_AUTOUPDATE")) {   // presses UPDATE, then REBUILD / RESTART, as a player would
        UpdateState u = update_state();
        if (u.stage == UP_AVAILABLE) update_install_async();
        else if (u.stage == UP_REBUILD && g_ss != SS_RUNNING && g_ss != SS_DONE && g_ss != SS_FAILED) start_rebuild(u.restart);
        else if (u.stage == UP_INSTALLED && u.restart) { g_do_restart = true; g_quit = true; }
        if (g_ss == SS_DONE && g_restart_after_setup) { g_do_restart = true; g_quit = true; }
        if ((u.stage == UP_CURRENT || u.stage == UP_ERROR || g_ss == SS_FAILED) && !g_job.running) g_quit = true;   // test over
    }
    static const char* rp = getenv("SWR_FE_RESULT"); static double last;
    static unsigned last_nav;
    ImGuiContext& gx = *ImGui::GetCurrentContext();
    bool nav_changed = gx.NavId != last_nav; last_nav = gx.NavId;
    if (rp && (ImGui::GetTime() - last >= 1 || g_quit || g_play || nav_changed)) { last = ImGui::GetTime(); FILE* f = fopen(rp, "a");
        if (f) { setup_lock(); fprintf(f, "frame=%d page=%s setup=%d pct=%.1f stage=%s nav=%08x navwin=%s active=%08x padflags=%d/%d down=%d update=%d/%s/%.0f error=%s%s\n", g_frame_no, PAGE_NAMES[g_page], (int)g_ss, (float)g_job.pct, g_job.stage.c_str(),
                     gx.NavId, gx.NavWindow ? gx.NavWindow->Name : "-", gx.ActiveId, (gx.IO.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0, (gx.IO.BackendFlags & ImGuiBackendFlags_HasGamepad) != 0,
                     (int)ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown), (int)update_state().stage, update_state().installed.c_str(), update_state().pct, g_job.error.c_str(), update_state().error.c_str()); setup_unlock(); fclose(f); } }
}

// the settings, the page to open and the menu buttons, before the window shows (fe_run on both systems)
static void fe_prepare() {
    g_ini.load(P.ini); if (g_ini.path.empty()) g_ini.path = P.ini;
    g_old = find_old_installs();
    if (g_test && getenv("SWR_FE_NO_OLD")) g_old.clear();   // tests: a PC with no earlier installation
    g_base = g_ini.get("setup_base", ""); g_update = g_ini.get("setup_update", "");
    // Metroid Dread has no Portuguese (9, pt-BR 17): the game is given English then (svc_core.c game_lang), and the menu shows that
    { std::string l = g_ini.get("language", ""); if (l == "9" || l == "17") { set("language", "1"); g_ini.save(); g_ini_dirty = false; } }
    bindings_from_string(g_ini.get("launcher_bindings", "1,0,6,7,10").c_str(), g_nav.bind); memcpy(g_old_bind, g_nav.bind, sizeof g_old_bind);
    g_nav.enabled = g_ini.get("launcher_nav", "1") != "0"; g_prompts = prompt_style_from(g_ini.get("launcher_prompts", "auto").c_str());
    GameCheck gc = check_installed(); g_page = (gc.code_ok && gc.data_ok) ? PG_PLAY : PG_SETUP; g_first_install = !gc.code_ok;
    if (g_test && getenv("SWR_FE_PAGE") && g_page != PG_SETUP) { int pg = atoi(getenv("SWR_FE_PAGE")); if (pg > PG_SETUP && pg < PG_COUNT) g_page = (Page)pg; }   // screenshots
}

} // namespace fe

using namespace fe;

// ---------------------------------------------------------------- entry points (backend_win32.c)
#ifdef _WIN32
extern "C" int fe_should_show(int argc, char** argv, const char* exedir, const char* exename) {
    P.init(exedir, exename);
    IniFile ini; ini.load(P.ini);
    bool play = false; for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--play")) play = true;
    bool skip = (play || ini.get("skip_launcher", "0") == "1") && !(GetAsyncKeyState(VK_SHIFT) & 0x8000);
    GameCheck g = check_installed();
    if (!g.code_ok || !g.data_ok) return 1;   // setup first, whatever was asked
    if (skip) { apply_env(ini); return 0; }
    return 1;
}

extern "C" int fe_wndproc(void* h, unsigned m, unsigned long long w, long long l, long long* result) {
    if (g_kb_capture >= 0 && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !(l & (1LL << 30))) {   // Controls > Keyboard: SET waits for a key
        int vk = (int)w;
        if (vk == VK_ESCAPE) { g_kb_capture = -1; flash("Not changed"); }
        else if (kb_reserved(vk)) flash(fmt("%s is reserved: pick another key", key_name(vk).c_str()));
        else if (vk > 0 && vk < 256) kb_assign(vk);
        *result = 0; return 1;
    }
    if (g_kb_capture >= 0 && (m == WM_KEYUP || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SYSCHAR)) { *result = 0; return 1; }
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler((HWND)h, m, (WPARAM)w, (LPARAM)l)) { *result = 1; return 1; }
    switch (m) {
    case WM_SIZE: if (w != SIZE_MINIMIZED) g_resize = true; *result = 0; return 1;
    case WM_CLOSE: if (g_job.running) { if (MessageBoxW((HWND)h, L"Setup is still running. Stop it? (You can continue it later.)", L"Metroid Dread", MB_YESNO | MB_ICONQUESTION) != IDYES) { *result = 0; return 1; } setup_cancel(g_job); }
        g_quit = true; *result = 0; return 1;
    case WM_TIMER: if (w == 77) { pads_rumble_off(); KillTimer((HWND)h, 77); } *result = 0; return 1;
    case WM_ERASEBKGND: *result = 1; return 1;
    case WM_SYSCOMMAND: if ((w & 0xfff0) == SC_KEYMENU) { *result = 0; return 1; } break;
    }
    return 0;
}

extern "C" int fe_run(void* hwnd) {
    g_hwnd = (HWND)hwnd; g_test = getenv("SWR_FE_TEST") != nullptr;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
<<<<<<< HEAD
    g_ini.load(P.ini); if (g_ini.path.empty()) g_ini.path = P.ini;
    g_old = find_old_installs();
    if (g_test && getenv("SWR_FE_NO_OLD")) g_old.clear();   // tests: a PC with no earlier installation
    g_base = g_ini.get("setup_base", ""); g_update = g_ini.get("setup_update", "");
    // pt-BR (17) is the menu's Portuguese (9): the fan translation applies with either (svc_fs.c), the list shows 9
    { std::string l = g_ini.get("language", ""); if (l == "17") { set("language", "9"); g_ini.save(); g_ini_dirty = false; } }
    bindings_from_string(g_ini.get("launcher_bindings", "1,0,6,7,10").c_str(), g_nav.bind); memcpy(g_old_bind, g_nav.bind, sizeof g_old_bind);
    g_nav.enabled = g_ini.get("launcher_nav", "1") != "0"; g_prompts = prompt_style_from(g_ini.get("launcher_prompts", "auto").c_str());
    GameCheck gc = check_installed(); g_page = (gc.code_ok && gc.data_ok) ? PG_PLAY : PG_SETUP; g_first_install = !gc.code_ok;
    if (g_test && getenv("SWR_FE_PAGE") && g_page != PG_SETUP) { int pg = atoi(getenv("SWR_FE_PAGE")); if (pg > PG_SETUP && pg < PG_COUNT) g_page = (Page)pg; }   // screenshots
=======
    fe_prepare();
>>>>>>> pr22
    // a 16:10 window on the primary screen, centred; fullscreen if the game will be
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int ww = std::min(1440, (int)((wa.right - wa.left) * 0.85f)), wh = std::min((int)(ww * 0.625f), (int)((wa.bottom - wa.top) * 0.9f));
    RECT r = {0, 0, ww, wh}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(g_hwnd, nullptr, wa.left + ((wa.right - wa.left) - (r.right - r.left)) / 2, wa.top + ((wa.bottom - wa.top) - (r.bottom - r.top)) / 2, r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
    SetWindowTextW(g_hwnd, L"Metroid Dread");
    ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd);
    if (g_ini.get("fullscreen", "1") == "1") backend_fe_fullscreen(1);
    if (!d3d_init()) { MessageBoxW(g_hwnd, L"Direct3D 11 is not available: the Play screen can't open. The game will start directly.", L"Metroid Dread", MB_OK | MB_ICONWARNING); apply_env(g_ini); return 0; }
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad; io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    load_fonts(io); apply_style(1.0f);
    ImGui_ImplWin32_Init(g_hwnd); ImGui_ImplDX11_Init(g_dev, g_ctx);
    pads_init();
    if (g_ini.get("check_updates", "1") != "0" && (!g_test || getenv("SWR_UPDATE_URL"))) update_check_async();
    while (!g_quit && !g_play) {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (g_quit || g_play) break;
        if (IsIconic(g_hwnd)) { Sleep(30); continue; }
        if (g_resize) { g_resize = false; if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } g_sc->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0); make_rtv(); }
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        nav_frame_begin();
        handle_nav();
        frame();
        nav_move(g_nav.down ? 1 : g_nav.up ? -1 : 0);
        ImGui::Render();
        const float clear[4] = {0.015f, 0.027f, 0.05f, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr); g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        test_shot(); g_frame_no++; test_tick();
        g_sc->Present(1, 0);
        if (g_ini_dirty) { g_ini.save(); g_ini_dirty = false; }
    }
    if (g_ini_dirty) g_ini.save();
    pads_shutdown();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    d3d_shutdown();
    if (g_do_restart) update_restart();   // an update replaced the EXE: the new one takes over
    if (g_quit) return 1;
    apply_env(g_ini);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    return 0;
}
#endif

#ifndef _WIN32
// ---------------------------------------------------------------- Linux host (backend_x11.c backend_frontend)
// An X11 window with an OpenGL 3 context for ImGui's OpenGL3 renderer, input fed to ImGui here. On Play it closes and
// fe_run returns: the game then opens its own window and GL context (video_init), as on Windows after Direct3D.
namespace fe {
static Display* x_dpy; static Window x_win; static GLXContext x_ctx; static Atom x_wm_delete, x_state, x_fs; static int x_fs_on, x_w = 1280, x_h = 800;

static void backend_fe_fullscreen(int on) {
    if (!x_dpy || (on != 0) == (x_fs_on != 0)) return;
    XEvent e; memset(&e, 0, sizeof e); e.xclient.type = ClientMessage; e.xclient.window = x_win; e.xclient.message_type = x_state; e.xclient.format = 32;
    e.xclient.data.l[0] = on ? 1 : 0; e.xclient.data.l[1] = (long)x_fs; e.xclient.data.l[3] = 1;
    XSendEvent(x_dpy, DefaultRootWindow(x_dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e); XFlush(x_dpy); x_fs_on = on;
}
static int backend_is_fullscreen(void) { return x_fs_on; }

// the primary monitor (XRandR 1.5 monitors, libXrandr loaded at run time), else the whole X screen at 60 Hz
void fe_x11_screen(int* w, int* h, int* hz) {
    Display* d = x_dpy ? x_dpy : XOpenDisplay(nullptr); if (!d) return;
    *w = DisplayWidth(d, DefaultScreen(d)); *h = DisplayHeight(d, DefaultScreen(d)); *hz = 60;
    static void* xr = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL);
    struct Mon { Atom name; Bool primary, automatic; int noutput, x, y, width, height, mwidth, mheight; unsigned long* outputs; };
    typedef Mon* (*GetMonitors)(Display*, Window, Bool, int*); typedef void (*FreeMonitors)(Mon*);
    typedef void* (*GetInfo)(Display*, Window); typedef short (*Rate)(void*); typedef void (*FreeInfo)(void*);
    if (xr) {
        GetMonitors gm = (GetMonitors)dlsym(xr, "XRRGetMonitors"); FreeMonitors fm = (FreeMonitors)dlsym(xr, "XRRFreeMonitors");
        int n = 0; Mon* m = gm ? gm(d, DefaultRootWindow(d), True, &n) : nullptr;
        if (m && n > 0) { int k = 0; for (int i = 0; i < n; i++) if (m[i].primary) k = i; *w = m[k].width; *h = m[k].height; }
        if (m && fm) fm(m);
        GetInfo gi = (GetInfo)dlsym(xr, "XRRGetScreenInfo"); Rate rt = (Rate)dlsym(xr, "XRRConfigCurrentRate"); FreeInfo fi = (FreeInfo)dlsym(xr, "XRRFreeScreenConfigInfo");
        void* c = gi && rt ? gi(d, DefaultRootWindow(d)) : nullptr; if (c) { short r = rt(c); if (r > 0) *hz = r; if (fi) fi(c); }
    }
    if (d != x_dpy) XCloseDisplay(d);
}
// what the front end's GL context runs on; Mesa also tells the video memory and whether it is the system's (an APU)
std::string fe_x11_gpu(int* vram_mb, bool* integrated) {
    if (!x_ctx) return {};
    const char* r = (const char*)glGetString(GL_RENDERER); std::string n = r ? r : "";
    typedef Bool (*QRI)(int, unsigned*); QRI q = (QRI)glXGetProcAddressARB((const GLubyte*)"glXQueryCurrentRendererIntegerMESA");
    unsigned v[1] = {0};
    if (q && q(0x8187 /* GLX_RENDERER_VIDEO_MEMORY_MESA */, v)) *vram_mb = (int)v[0];
    if (q && q(0x8188 /* GLX_RENDERER_UNIFIED_MEMORY_ARCHITECTURE_MESA */, v)) *integrated = v[0] != 0;
    if (!*vram_mb) { GLint kb = 0; while (glGetError()) {} glGetIntegerv(0x9047 /* GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX */, &kb); if (!glGetError() && kb > 0) *vram_mb = kb / 1024; }
    return n;
}

static ImGuiKey x_key(KeySym k) {
    if (k >= XK_a && k <= XK_z) return (ImGuiKey)(ImGuiKey_A + (k - XK_a));
    if (k >= XK_0 && k <= XK_9) return (ImGuiKey)(ImGuiKey_0 + (k - XK_0));
    switch (k) {
    case XK_Tab: case XK_ISO_Left_Tab: return ImGuiKey_Tab; case XK_Left: return ImGuiKey_LeftArrow; case XK_Right: return ImGuiKey_RightArrow;
    case XK_Up: return ImGuiKey_UpArrow; case XK_Down: return ImGuiKey_DownArrow; case XK_Prior: return ImGuiKey_PageUp; case XK_Next: return ImGuiKey_PageDown;
    case XK_Home: return ImGuiKey_Home; case XK_End: return ImGuiKey_End; case XK_Insert: return ImGuiKey_Insert; case XK_Delete: return ImGuiKey_Delete;
    case XK_BackSpace: return ImGuiKey_Backspace; case XK_space: return ImGuiKey_Space; case XK_Return: return ImGuiKey_Enter; case XK_KP_Enter: return ImGuiKey_KeypadEnter;
    case XK_Escape: return ImGuiKey_Escape; case XK_F11: return ImGuiKey_F11;
    case XK_Shift_L: return ImGuiKey_LeftShift; case XK_Shift_R: return ImGuiKey_RightShift; case XK_Control_L: return ImGuiKey_LeftCtrl; case XK_Control_R: return ImGuiKey_RightCtrl;
    case XK_Alt_L: return ImGuiKey_LeftAlt; case XK_Alt_R: return ImGuiKey_RightAlt;
    }
    return ImGuiKey_None;
}
static void x_events() {
    ImGuiIO& io = ImGui::GetIO();
    while (XPending(x_dpy)) {
        XEvent e; XNextEvent(x_dpy, &e);
        switch (e.type) {
        case KeyPress: case KeyRelease: {
            bool down = e.type == KeyPress; unsigned st = e.xkey.state;
            io.AddKeyEvent(ImGuiMod_Ctrl, (st & ControlMask) != 0); io.AddKeyEvent(ImGuiMod_Shift, (st & ShiftMask) != 0); io.AddKeyEvent(ImGuiMod_Alt, (st & Mod1Mask) != 0);
            KeySym k = XLookupKeysym(&e.xkey, 0);
            if (down && (k == XK_Return || k == XK_KP_Enter) && (st & Mod1Mask)) { bool fs = !x_fs_on; backend_fe_fullscreen(fs); set("fullscreen", fs ? "1" : "0"); break; }   // Alt+Enter, as in the game
            ImGuiKey ik = x_key(k); if (ik != ImGuiKey_None) io.AddKeyEvent(ik, down);
            if (down) { char c[16]; KeySym ks; int n = XLookupString(&e.xkey, c, sizeof c - 1, &ks, nullptr); c[n > 0 ? n : 0] = 0;
                if (n == 1 && (unsigned char)c[0] >= 32 && c[0] != 127) io.AddInputCharacter((unsigned char)c[0]); }
            break; }
        case MotionNotify: io.AddMousePosEvent((float)e.xmotion.x, (float)e.xmotion.y); break;
        case ButtonPress: case ButtonRelease: {
            unsigned b = e.xbutton.button; io.AddMousePosEvent((float)e.xbutton.x, (float)e.xbutton.y);
            if (b == 1 || b == 2 || b == 3) io.AddMouseButtonEvent(b == 1 ? 0 : b == 3 ? 1 : 2, e.type == ButtonPress);
            else if (e.type == ButtonPress && (b == 4 || b == 5)) io.AddMouseWheelEvent(0, b == 4 ? 1.0f : -1.0f);
            break; }
        case LeaveNotify: io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); break;
        case FocusIn: g_focused = true; io.AddFocusEvent(true); break;
        case FocusOut: g_focused = false; io.AddFocusEvent(false); break;
        case ConfigureNotify: x_w = std::max(1, e.xconfigure.width); x_h = std::max(1, e.xconfigure.height); break;
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == x_wm_delete) { if (g_job.running) setup_cancel(g_job); g_quit = true; }   // setup continues from where it stopped next time
            break;
        }
    }
}
static int x_err_ignore(Display*, XErrorEvent*) { return 0; }
static bool x_open() {
    x_dpy = XOpenDisplay(nullptr); if (!x_dpy) return false;
    int scr = DefaultScreen(x_dpy);
    static int att[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_DOUBLEBUFFER, True, None};
    int n = 0; GLXFBConfig* fc = glXChooseFBConfig(x_dpy, scr, att, &n); if (!fc || !n) return false;
    XVisualInfo* vi = glXGetVisualFromFBConfig(x_dpy, fc[0]); if (!vi) return false;
    // a 16:10 window on the primary screen, centred; fullscreen if the game will be (as on Windows)
    int sw = 1920, sh = 1080, hz = 60; fe_x11_screen(&sw, &sh, &hz);
    x_w = std::min(1440, (int)(sw * 0.85f)); x_h = std::min((int)(x_w * 0.625f), (int)(sh * 0.9f));
    XSetWindowAttributes swa; memset(&swa, 0, sizeof swa); swa.colormap = XCreateColormap(x_dpy, RootWindow(x_dpy, vi->screen), vi->visual, AllocNone);
    swa.background_pixel = BlackPixel(x_dpy, scr);
    swa.event_mask = KeyPressMask | KeyReleaseMask | StructureNotifyMask | FocusChangeMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask;
    x_win = XCreateWindow(x_dpy, RootWindow(x_dpy, vi->screen), (sw - x_w) / 2, (sh - x_h) / 2, x_w, x_h, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask | CWBackPixel, &swa);
    XFree(vi);
    XStoreName(x_dpy, x_win, "Metroid Dread");
    { XClassHint ch = {(char*)"metroid-dread", (char*)"MetroidDread"}; XSetClassHint(x_dpy, x_win, &ch); }
    x_wm_delete = XInternAtom(x_dpy, "WM_DELETE_WINDOW", False); XSetWMProtocols(x_dpy, x_win, &x_wm_delete, 1);
    x_state = XInternAtom(x_dpy, "_NET_WM_STATE", False); x_fs = XInternAtom(x_dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XkbSetDetectableAutoRepeat(x_dpy, True, nullptr);
    if (g_ini.get("fullscreen", "1") == "1") { XChangeProperty(x_dpy, x_win, x_state, XA_ATOM, 32, PropModeReplace, (unsigned char*)&x_fs, 1); x_fs_on = 1; }   // read when mapped
    XMapWindow(x_dpy, x_win);
    typedef GLXContext (*CCA)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
    CCA cca = (CCA)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    int (*old)(Display*, XErrorEvent*) = XSetErrorHandler(x_err_ignore);
    if (cca) { int ca[] = {0x2091, 3, 0x2092, 3, 0x9126, 0x00000001, None}; x_ctx = cca(x_dpy, fc[0], nullptr, True, ca); XSync(x_dpy, False); }   // 3.3 core
    if (!x_ctx) x_ctx = glXCreateNewContext(x_dpy, fc[0], GLX_RGBA_TYPE, nullptr, True);
    XSetErrorHandler(old); XFree(fc);
    if (!x_ctx || !glXMakeCurrent(x_dpy, x_win, x_ctx)) return false;
    typedef void (*SIE)(Display*, GLXDrawable, int); typedef int (*SIM)(unsigned);
    SIE sie = (SIE)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT"); SIM sim = (SIM)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalMESA");
    if (sie) sie(x_dpy, x_win, 1); else if (sim) sim(1);
    return true;
}
static void x_close() {
    if (!x_dpy) return;
    if (x_ctx) { glXMakeCurrent(x_dpy, None, nullptr); glXDestroyContext(x_dpy, x_ctx); x_ctx = nullptr; }
    if (x_win) { XDestroyWindow(x_dpy, x_win); x_win = 0; }
    XCloseDisplay(x_dpy); x_dpy = nullptr;
}
static double x_now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
} // namespace fe

extern "C" int fe_should_show(int argc, char** argv, const char* exedir, const char* exename) {
    P.init(exedir, exename);
    IniFile ini; ini.load(P.ini);
    bool play = false, launcher = false;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--play")) play = true; if (!strcmp(argv[i], "--launcher")) launcher = true; }
    bool skip = (play || ini.get("skip_launcher", "0") == "1") && !launcher;   // --launcher: the Play screen even when it is skipped
    GameCheck g = check_installed();
    if (!g.code_ok || !g.data_ok) return 1;   // setup first, whatever was asked
    if (skip) { apply_env(ini); return 0; }
    return 1;
}

extern "C" int fe_run(void*) {
    g_test = getenv("SWR_FE_TEST") != nullptr;
    fe_prepare();
    if (!x_open()) { fprintf(stderr, "front end: no X11 display with OpenGL 3: the game starts directly\n"); x_close(); apply_env(g_ini); return check_installed().code_ok ? 0 : 1; }
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr; io.BackendPlatformName = "fe_x11";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad; io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    load_fonts(io); apply_style(1.0f);
    ImGui_ImplOpenGL3_Init("#version 150");
    pads_init();
    double last = x_now();
    while (!g_quit && !g_play) {
        x_events();
        if (g_quit || g_play) break;
        double now = x_now(); io.DeltaTime = (float)std::max(1e-4, now - last); last = now;
        io.DisplaySize = ImVec2((float)x_w, (float)x_h); io.DisplayFramebufferScale = ImVec2(1, 1);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        nav_frame_begin();
        handle_nav();
        frame();
        nav_move(g_nav.down ? 1 : g_nav.up ? -1 : 0);
        ImGui::Render();
        glViewport(0, 0, x_w, x_h); glClearColor(0.015f, 0.027f, 0.05f, 1); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        test_shot(); g_frame_no++; test_tick();
        glXSwapBuffers(x_dpy, x_win);
        if (g_rumble_off && ImGui::GetTime() >= g_rumble_off) { pads_rumble_off(); g_rumble_off = 0; }
        if (g_ini_dirty) { g_ini.save(); g_ini_dirty = false; }
    }
    if (g_ini_dirty) g_ini.save();
    pads_shutdown();
    ImGui_ImplOpenGL3_Shutdown(); ImGui::DestroyContext();
    x_close();
    if (g_quit) return 1;
    apply_env(g_ini);
    return 0;
}
#endif
