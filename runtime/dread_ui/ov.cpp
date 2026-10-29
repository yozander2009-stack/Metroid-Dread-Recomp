// In-game menu: F1 (or the controller shortcut) opens it over the running game. Drawn with Dear ImGui's OpenGL3 backend on
// the GL thread just before the swap (glq.c). The game keeps running at normal speed; while the menu is open it gets neutral
// input, and after closing it gets input again only once every button is released (no stuck buttons).
// Threads: window thread -> ov_wndproc (keyboard, mouse); main thread -> ov_pad (controller, open/close, settings snapshot);
// GL thread -> ov_gl_frame (drawing). They meet in g_q under its mutex. The window side: ov_wndproc on Windows, the
// ov_input_* functions from the X11 backend on Linux (Steam Deck: touch arrives as the mouse).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include "ui.h"
extern "C" { int cheats_count(void); const char* cheats_name(int); int cheats_is_on(int); int cheats_is_master(int); void cheats_set(int, int); }   // runtime cheats.c

extern "C" {
void settings_queue(const char* k, const char* v);
const char* settings_get(const char* k, const char* def);
void backend_window_size(int* w, int* h);
void* backend_gl_proc(const char* n);
void gpu_gl_state_dirty(void);
void backend_release_keys(void);
void backend_request_quit(void);
int backend_is_fullscreen(void);
extern void (*g_backend_status)(const char*);
extern int g_snxvk_on;
extern int g_opt_ff;
extern int g_frames;
void logf_(int lvl, const char* f, ...);
}
using namespace ui;

namespace {
struct Ev { int type; int a; int b; float x, y; };   // 0 key(OVK_, down) 1 char 2 mouse pos 3 mouse button(btn, down) 4 wheel
// platform-neutral keys the window side reports
enum { OVK_UP = 1, OVK_DOWN, OVK_LEFT, OVK_RIGHT, OVK_ENTER, OVK_ESCAPE, OVK_TAB, OVK_PAGEUP, OVK_PAGEDOWN, OVK_SPACE, OVK_HOME, OVK_END };
double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct Shared {
    std::mutex m;
    std::vector<Ev> ev;
    bool want_toggle = false;                     // F1 from the window thread
    volatile bool open = false;                   // visible / taking input
    bool up = false, down = false, left = false, right = false, act[NAV_COUNT] = {};
    std::map<std::string, std::string> vals;      // snapshot of the settings (main thread), updated by the menu
    int bind[NAV_COUNT] = {1, 0, 6, 7, 10}; bool nav_on = true; PromptStyle prompts = PROMPT_AUTO;
    bool pad_used = false; std::string renderer;
} g_q;
struct Lock { Lock() { g_q.m.lock(); } ~Lock() { g_q.m.unlock(); } };

bool g_latch, g_open_seen;                        // main thread: block the game until all buttons are up
Nav g_nav; double g_hold_t0;                      // main thread
unsigned long long g_prev_phys;

// GL thread
bool g_ready; double g_t_open; int g_tab; bool g_tab_changed = true;
std::map<std::string, std::string> g_vals;        // the menu's copy
struct Revert { bool active = false; double t0 = 0; std::vector<std::pair<std::string, std::string>> old; } g_rev;
bool g_quit_confirm;

const char* const TABS[] = {"DISPLAY", "GRAPHICS", "GAMEPLAY", "AUDIO", "CONTROLS", "HELP"};
const int NTABS = 6;

std::string val(const char* k) { auto it = g_vals.find(k); if (it != g_vals.end()) return it->second; const Opt* o = find_option(k); return o ? o->def : ""; }
void change(const char* k, const std::string& v) {
    g_vals[k] = v; settings_queue(k, v.c_str());
    Lock l; g_q.vals[k] = v;
}
bool is_display_key(const char* k) { return !strcmp(k, "resolution") || !strcmp(k, "fullscreen") || !strcmp(k, "dynamic_resolution") || !strcmp(k, "dynamic_resolution_min"); }
void change_display(const char* k, const std::string& v) {
    if (!g_rev.active) { g_rev.old.clear(); for (const char* x : {"resolution", "fullscreen", "dynamic_resolution", "dynamic_resolution_min"}) g_rev.old.push_back({x, val(x)}); }
    g_rev.active = true; g_rev.t0 = ImGui::GetTime(); change(k, v);
}
void revert_display() { for (auto& p : g_rev.old) if (val(p.first.c_str()) != p.second) change(p.first.c_str(), p.second); g_rev.active = false; }

const char* live_tag(const Opt& o) {
    bool native = g_q.renderer == "vulkan_native";
    if (o.live == LIVE_RESTART || (o.live == LIVE_GL && native)) return "NEXT START";
    return nullptr;
}

void opt_row(const char* key) {
    const Opt* o = find_option(key); if (!o) return;
    std::string cur = val(key), id = std::string("##o_") + key; const char* tag = live_tag(*o);
    auto commit = [&](const std::string& v) { if (is_display_key(key)) change_display(key, v); else change(key, v); };
    switch (o->type) {
    case OPT_BOOL: { bool v = cur == "1"; if (row_bool(id.c_str(), o->label, &v, tag)) commit(v ? "1" : "0"); break; }
    case OPT_CHOICE: { std::vector<std::string> items; int idx = 0;
        for (int i = 0; i < (int)o->choices.size(); i++) { items.push_back(o->choices[i].label); if (cur == o->choices[i].value) idx = i; }
        if (row_choice(id.c_str(), o->label, &idx, items, tag)) commit(o->choices[idx].value); break; }
    default: {
        float v = (float)atof(cur.c_str());
        std::string t = o->type == OPT_INT ? fmt("%d", (int)lroundf(v)) : fmt("%.2f", v);
        if (!strcmp(key, "resolution")) { bool d = val("docked") == "1"; std::string a = val("aspect"); t = res_text(v, d, a.c_str()); }
        if (!strcmp(key, "mouse_sensitivity")) t = fmt("%d%%", (int)lroundf(v));
        if (!strcmp(key, "volume") || !strcmp(key, "rumble")) t = (int)v == 0 ? "Off" : fmt("%d%%", (int)lroundf(v));
        if (!strcmp(key, "stick_deadzone")) t = fmt("%d%%", (int)(v * 100 / 32767));
        if (row_slider(id.c_str(), o->label, &v, o->lo, o->hi, o->step, t.c_str(), tag)) commit(o->type == OPT_INT ? fmt("%d", (int)lroundf(v)) : fmt("%g", roundf(v * 100) / 100));
    } }
}

void tab_content(int t) {
    switch (t) {
    case 0: section("PICTURE"); opt_row("resolution"); opt_row("fullscreen"); opt_row("dynamic_resolution"); if (val("dynamic_resolution") == "1") opt_row("dynamic_resolution_min");
            opt_row("game_dynamic_resolution"); section("CONSOLE"); opt_row("docked"); opt_row("aspect"); opt_row("language"); break;
    case 1: section("QUALITY"); opt_row("antialiasing"); opt_row("sharpening"); opt_row("shadows"); opt_row("anisotropy"); section("RENDERER"); opt_row("renderer"); opt_row("hdr"); break;
    case 2: if (cheats_count() > 0) { section("CHEATS");
                for (int i = 0; i < cheats_count(); i++) { if (cheats_is_master(i)) continue;
                    bool on = cheats_is_on(i) != 0; if (row_bool(fmt("##ovch%d", i).c_str(), cheats_name(i), &on)) cheats_set(i, on ? 1 : 0); } }
            section("FRAME RATE"); opt_row("fps_limit"); opt_row("forced_vsync"); opt_row("show_fps"); section("SCENES"); opt_row("cutscenes_60fps"); opt_row("loading_60fps");
#ifndef SWR_NO_SKIPCUT
        opt_row("skip_cutscenes");
#endif
       
            section("LOADING"); opt_row("fast_loading"); opt_row("fast_forward"); if (val("fast_forward") == "1") opt_row("fast_forward_speed"); break;
    case 3: section("OUTPUT"); opt_row("volume"); section("SPEAKERS"); opt_row("audio_channels"); section("COMPATIBILITY"); opt_row("audio_surround_downmix"); break;
    case 4: section("MENUS"); opt_row("menu_buttons"); section("MOUSE"); opt_row("mouse_aim"); opt_row("mouse_sensitivity");
            section("CONTROLLER"); opt_row("button_icons"); opt_row("stick_deadzone"); opt_row("rumble"); opt_row("menu_shortcut"); opt_row("controller_api");
            help_text("Controller buttons, keyboard keys and menu navigation are set on the Play screen's Controls page."); break;
    default: {
        section("KEYS");
        const char* kb[][2] = {{"Menu", "F1"}, {"Screenshot", "F3 (F8: full resolution PNG)"}, {"Record video", "F7"}, {"Fullscreen", "F11 or Alt+Enter"},
                               {"Fast forward (if on)", "Tab, or L3 + R3"}, {"Quit", "Esc"}, {"Move / aim", "W A S D / I J K L"}, {"A B X Y", "X  Z  C  V"},
                               {"L R ZL ZR", "Q  E  1  3"}, {"+ / -", "Enter / Backspace"}};
        for (auto& r : kb) row_text(r[0], r[1]);
        section("ABOUT"); help_text("Metroid Dread recompiled for PC. SwitchRecomp by MZ Labs; Native Vulkan by ScarletNX; 60 FPS cutscenes by theboy181; Dear ImGui; SDL2. Full credits on the Play screen.");
    } }
}

void draw_menu(float W, float H, double t) {
    float k = (float)std::min(1.0, (t - g_t_open) / 0.16);   // open animation
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled({0, 0}, {W, H}, IM_COL32(1, 3, 8, (int)(195 * k)));
    float pw = std::min(S(1200), W - S(80)), ph = H - S(120), x0 = (W - pw) / 2, y0 = S(60) + (1 - k) * S(30);
    chamfer_rect(bg, {x0, y0}, {x0 + pw, y0 + ph}, S(18), IM_COL32(5, 10, 19, (int)(250 * k)), C_LINE, 1);
    bg->AddRectFilled({x0, y0}, {x0 + S(220), y0 + S(3)}, C_RED);
    ImGui::SetNextWindowPos({x0 + S(36), y0 + S(26)}); ImGui::SetNextWindowSize({pw - S(72), ph - S(40)});
    ImGui::SetNextWindowBgAlpha(0);
    ImGui::Begin("##ov", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 p = ImGui::GetCursorScreenPos();
    // header + tabs
    dl->AddText(ImGui::GetFont(), S(30), p, C_TEXT, "MENU");
    float tx = p.x + S(140);
    for (int i = 0; i < NTABS; i++) {
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(S(19), FLT_MAX, 0, TABS[i]); ImVec2 a(tx, p.y + S(6)), b(tx + ts.x + S(28), p.y + S(40));
        ImGui::SetCursorScreenPos(a); if (ImGui::InvisibleButton(fmt("##tab%d", i).c_str(), {b.x - a.x, b.y - a.y}, ImGuiButtonFlags_None)) { g_tab = i; g_tab_changed = true; }
        bool sel = g_tab == i;
        if (sel) chamfer_rect(dl, a, b, S(6), IM_COL32(226, 30, 44, 200), 0); else if (ImGui::IsItemHovered()) chamfer_rect(dl, a, b, S(6), IM_COL32(64, 214, 255, 30), 0);
        dl->AddText(ImGui::GetFont(), S(19), {a.x + S(14), a.y + S(7)}, sel ? IM_COL32(255, 255, 255, 255) : C_DIM, TABS[i]);
        tx = b.x + S(6);
    }
    ImGui::SetCursorScreenPos({p.x, p.y + S(60)});
    float bodyh = ImGui::GetContentRegionAvail().y - S(64);
    ImGui::BeginChild("##ovbody", {0, bodyh}, 0, ImGuiWindowFlags_NoBackground);
    if (g_tab_changed) { ImGui::SetScrollY(0); g_focus_first = true; g_tab_changed = false; }
    tab_content(g_tab);
    ImGui::EndChild();
    // footer: resume / quit and prompts
    ImVec2 f = ImGui::GetCursorScreenPos();
    if (big_button("##resume", "RESUME", {S(220), S(48)})) { Lock l; g_q.open = false; }
    ImGui::SameLine(0, S(14));
    if (big_button("##quit", g_quit_confirm ? "QUIT: SURE?" : "QUIT GAME", {S(220), S(48)}, false)) { if (g_quit_confirm) backend_request_quit(); g_quit_confirm = true; }
    if (g_q.pad_used) {
        PromptStyle st = prompt_resolve(g_q.prompts, "");
        prompt_bar({{g_q.bind[NAV_CONFIRM], "Select"}, {g_q.bind[NAV_BACK], "Close"}, {12, "Change"}, {g_q.bind[NAV_PREV], "Tab"}, {g_q.bind[NAV_NEXT], "Tab"}}, st, {f.x + S(480), f.y + S(10)});
    } else dl->AddText({f.x + S(480), f.y + S(14)}, C_DIM, "Arrows, Enter, Left/Right, Esc to close, mouse");
    ImGui::End();
    // keep-or-revert for display changes
    if (g_rev.active) {
        double left = 15 - (t - g_rev.t0);
        if (left <= 0) revert_display();
        else {
            float dw = S(620), dh = S(170); ImVec2 a((W - dw) / 2, H - dh - S(90));
            ImGui::SetNextWindowPos(a); ImGui::SetNextWindowSize({dw, dh}); ImGui::SetNextWindowBgAlpha(0);
            ImGui::Begin("##rev", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
            ImDrawList* d2 = ImGui::GetWindowDrawList(); chamfer_rect(d2, a, {a.x + dw, a.y + dh}, S(12), IM_COL32(8, 16, 30, 245), C_CYAN, 1.5f);
            d2->AddText({a.x + S(24), a.y + S(20)}, C_TEXT, "Keep these display settings?");
            d2->AddText({a.x + S(24), a.y + S(52)}, C_DIM, fmt("They change back in %d s unless you keep them.", (int)ceil(left)).c_str());
            ImGui::SetCursorScreenPos({a.x + S(24), a.y + S(96)});
            if (big_button("##keep", "KEEP", {S(180), S(48)})) g_rev.active = false;
            ImGui::SameLine(0, S(14));
            if (big_button("##revert", "REVERT", {S(180), S(48)}, false)) revert_display();
            ImGui::End();
        }
    }
}

void snapshot_settings() {   // main thread
    Lock l; g_q.vals.clear();
    for (auto& o : options()) g_q.vals[o.key] = settings_get(o.key, o.def);
    for (const char* k : {"launcher_bindings", "launcher_nav", "launcher_prompts", "renderer", "controller_map"}) g_q.vals[k] = settings_get(k, "");
    bindings_from_string(settings_get("launcher_bindings", "1,0,6,7,10"), g_q.bind);
    g_q.nav_on = strcmp(settings_get("launcher_nav", "1"), "0") != 0; g_q.prompts = prompt_style_from(settings_get("launcher_prompts", "auto"));
    g_q.renderer = settings_get("renderer", "opengl");
}
} // namespace

// ---------------------------------------------------------------- window thread
extern "C" int ov_is_open(void) { return g_q.open ? 1 : 0; }
/* the window side, any platform: F1 (toggle) and, while the menu is open, keys, characters and the mouse */
extern "C" void ov_input_toggle(void) {
    if (g_snxvk_on) { if (g_backend_status) g_backend_status("The in-game menu needs the OpenGL renderer for now (Play screen > Graphics)"); return; }
    Lock k; g_q.want_toggle = true;
}
extern "C" void ov_input_key(int ovk, int down) { if (!g_q.open || !ovk) return; Lock k; g_q.ev.push_back({0, ovk, down != 0}); }
extern "C" void ov_input_char(unsigned c) { if (!g_q.open) return; Lock k; g_q.ev.push_back({1, (int)c, 0}); }
extern "C" void ov_input_mouse(float x, float y) { if (!g_q.open) return; Lock k; g_q.ev.push_back({2, 0, 0, x, y}); }
extern "C" void ov_input_button(int b, int down) { if (!g_q.open) return; Lock k; g_q.ev.push_back({3, b, down != 0}); }
extern "C" void ov_input_wheel(float dy) { if (!g_q.open) return; Lock k; g_q.ev.push_back({4, 0, 0, 0, dy}); }
#ifdef _WIN32
static int ovk_from_vk(unsigned long long w) {
    switch (w) { case VK_UP: return OVK_UP; case VK_DOWN: return OVK_DOWN; case VK_LEFT: return OVK_LEFT; case VK_RIGHT: return OVK_RIGHT;
        case VK_RETURN: return OVK_ENTER; case VK_ESCAPE: return OVK_ESCAPE; case VK_TAB: return OVK_TAB; case VK_PRIOR: return OVK_PAGEUP;
        case VK_NEXT: return OVK_PAGEDOWN; case VK_SPACE: return OVK_SPACE; case VK_HOME: return OVK_HOME; case VK_END: return OVK_END; }
    return 0;
}
extern "C" int ov_wndproc(void* h, unsigned m, unsigned long long w, long long l) {
    (void)h;
    if (m == WM_KEYDOWN && w == VK_F1 && !(l & (1 << 30))) { ov_input_toggle(); return 1; }
    if (!g_q.open) return 0;
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: ov_input_key(ovk_from_vk(w), 1); return 1;
    case WM_KEYUP: case WM_SYSKEYUP: ov_input_key(ovk_from_vk(w), 0); return 1;
    case WM_CHAR: ov_input_char((unsigned)w); return 1;
    case WM_MOUSEMOVE: ov_input_mouse((float)(short)LOWORD(l), (float)(short)HIWORD(l)); return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: ov_input_button(0, m == WM_LBUTTONDOWN); return 1;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: ov_input_button(1, m == WM_RBUTTONDOWN); return 1;
    case WM_MOUSEWHEEL: ov_input_wheel((float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA); return 1;
    }
    return 0;
}
#endif

// ---------------------------------------------------------------- main thread (video_poll)
extern "C" int ov_pad(unsigned long long phys, int lx, int ly) {
    double now = now_s();
    bool toggle = false;
    { Lock k; if (g_q.want_toggle) { g_q.want_toggle = false; toggle = true; } }
    std::string sc = settings_get("menu_shortcut", "home");
    unsigned long long pressed = phys & ~g_prev_phys;
    if (sc == "home" && (pressed >> 30 & 1)) toggle = true;
    if (sc == "minus_plus" && (phys >> 10 & 1) && (phys >> 11 & 1) && ((pressed >> 10 & 1) || (pressed >> 11 & 1))) toggle = true;
    if (sc == "l3_r3") { bool both = (phys >> 4 & 1) && (phys >> 5 & 1); if (both) { if (!g_hold_t0) g_hold_t0 = now; else if (g_hold_t0 > 0 && now - g_hold_t0 >= 1.0) { toggle = true; g_hold_t0 = -1; } } else g_hold_t0 = 0; }
    g_prev_phys = phys;
    if (toggle && g_snxvk_on) { if (g_backend_status) g_backend_status("The in-game menu needs the OpenGL renderer for now (Play screen > Graphics)"); toggle = false; }
    bool open;
    { Lock k; if (toggle) { if (!g_q.open) { g_q.m.unlock(); snapshot_settings(); g_q.m.lock(); g_q.open = true; backend_release_keys(); logf_(1, "menu: opened at frame %d\n", g_frames); } else g_q.open = false; } open = g_q.open; }
    if (open) {
        g_latch = true; g_open_seen = true;
        { Lock k; memcpy(g_nav.bind, g_q.bind, sizeof g_nav.bind); g_nav.enabled = g_q.nav_on; }
        if (g_nav.reset_hold(now, phys)) settings_queue("launcher_bindings", "1,0,6,7,10");
        g_nav.update(phys & ~(1ull << 30), lx, ly, now);
        Lock k;   // edges accumulate until the GL thread takes them
        g_q.up |= g_nav.up; g_q.down |= g_nav.down; g_q.left |= g_nav.left; g_q.right |= g_nav.right;
        for (int i = 0; i < NAV_COUNT; i++) g_q.act[i] |= g_nav.act[i];
        if (g_nav.any_pad) g_q.pad_used = true;
        return 1;
    }
    g_nav.update(0, 0, 0, now);
    if (g_latch && g_open_seen) { g_open_seen = false; logf_(1, "menu: closed at frame %d\n", g_frames); }
    if (g_latch) { if (phys & ~(1ull << 30)) return 1; g_latch = false; logf_(1, "menu: controller back to the game at frame %d (all buttons released)\n", g_frames); }   // closed: wait for every button to be released
    return 0;
}

// ---------------------------------------------------------------- GL thread (glq.c, before the swap)
// test only: SWR_WIN_SHOT_FRAMES="F,F" + SWR_WIN_SHOT_PREFIX: the window's finished picture (after the present pass, so FXAA and
// the downscale are in it; menu closed) at those guest frames, as PPM
static void test_window_shot() {
    static const char* s = getenv("SWR_WIN_SHOT_FRAMES"); static const char* pre = getenv("SWR_WIN_SHOT_PREFIX"); static int last = -1;
    if (!s || !pre || g_frames == last) return;
    bool want = false; for (const char* p = s; *p;) { char* e; long v = strtol(p, &e, 10); if (v == g_frames) want = true; if (e == p) break; p = *e == ',' ? e + 1 : e; }
    if (!want) return; last = g_frames;
    typedef void (APIENTRY * RP)(int, int, int, int, unsigned, unsigned, void*); typedef void (APIENTRY * BindFB)(unsigned, unsigned);
    static RP rp = (RP)backend_gl_proc("glReadPixels"); static BindFB bind = (BindFB)backend_gl_proc("glBindFramebuffer");
    int w = 1, h = 1; backend_window_size(&w, &h); if (!rp || !bind) return;
    bind(0x8CA8, 0);   // GL_READ_FRAMEBUFFER = the window
    std::vector<unsigned char> px((size_t)w * h * 3); rp(0, 0, w, h, 0x1907, 0x1401, px.data());
    std::string path = fmt("%s-w%d.ppm", pre, g_frames); FILE* f = fopen(path.c_str(), "wb");
    if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); for (int y = h - 1; y >= 0; y--) fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f); fclose(f); }
    gpu_gl_state_dirty();
}

extern "C" void ov_gl_frame(void) {
    test_window_shot();
    bool open; std::vector<Ev> ev; bool up, down, left, right, act[NAV_COUNT];
    {
        Lock k; open = g_q.open; ev.swap(g_q.ev);
        up = g_q.up; down = g_q.down; left = g_q.left; right = g_q.right; memcpy(act, g_q.act, sizeof act);
        g_q.up = g_q.down = g_q.left = g_q.right = false; memset(g_q.act, 0, sizeof g_q.act);
        if (open && !g_ready) {}   // first open below
        if (open && g_vals.empty()) g_vals = g_q.vals;
    }
    static bool was_open;
    if (!open) { if (was_open) { was_open = false; g_vals.clear(); g_rev.active = false; g_quit_confirm = false; } return; }
    if (!was_open) { was_open = true; g_t_open = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0; g_tab_changed = true; { Lock k; g_vals = g_q.vals; } }
    if (!g_ready) {
        IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad; io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        load_fonts(io); apply_style(1.0f);
        if (!ImGui_ImplOpenGL3_Init(nullptr)) { ImGui::DestroyContext(); Lock k; g_q.open = false; return; }
        g_ready = true; g_t_open = ImGui::GetTime();
    }
    ImGuiIO& io = ImGui::GetIO();
    int w = 1, h = 1; backend_window_size(&w, &h);
    io.DisplaySize = ImVec2((float)w, (float)h);
    static double last; double now = now_s();
    io.DeltaTime = last > 0 ? std::max(1e-4f, (float)(now - last)) : 1.0f / 60; last = now;
    for (auto& e : ev) {
        switch (e.type) {
        case 0: {
            ImGuiKey key = ImGuiKey_None;
            switch (e.a) { case OVK_UP: key = ImGuiKey_UpArrow; break; case OVK_DOWN: key = ImGuiKey_DownArrow; break; case OVK_LEFT: key = ImGuiKey_LeftArrow; break;
                case OVK_RIGHT: key = ImGuiKey_RightArrow; break; case OVK_ENTER: key = ImGuiKey_Enter; break; case OVK_SPACE: key = ImGuiKey_Space; break;
                case OVK_ESCAPE: key = ImGuiKey_Escape; break; case OVK_TAB: key = ImGuiKey_Tab; break; case OVK_PAGEUP: key = ImGuiKey_PageUp; break; case OVK_PAGEDOWN: key = ImGuiKey_PageDown; break;
                case OVK_HOME: key = ImGuiKey_Home; break; case OVK_END: key = ImGuiKey_End; break; }
            if (key == ImGuiKey_Enter && g_rev.active) { if (e.b) act[NAV_CONFIRM] = true; break; }   // Enter = Keep while that question shows
            if (key != ImGuiKey_None) io.AddKeyEvent(key, e.b != 0); break; }
        case 1: io.AddInputCharacter((unsigned)e.a); break;
        case 2: io.AddMousePosEvent(e.x, e.y); break;
        case 3: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case 4: io.AddMouseWheelEvent(0, e.y); break;
        }
    }
    float sc = std::max(0.55f, std::min(2.2f, std::min(h / 1080.0f, w / 1700.0f))); if (fabsf(sc - g_scale) > 0.01f) apply_style(sc);
    ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
    nav_frame_begin();
    g_row.pad_left = left || ImGui::IsKeyPressed(ImGuiKey_LeftArrow); g_row.pad_right = right || ImGui::IsKeyPressed(ImGuiKey_RightArrow); g_row.confirm = act[NAV_CONFIRM];
    if (g_rev.active && act[NAV_CONFIRM]) { g_rev.active = false; g_row.confirm = false; }   // "Keep these display settings?": confirm keeps
    bool close = act[NAV_BACK] || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (close) { if (g_rev.active) revert_display(); else { Lock k; g_q.open = false; } }
    if (act[NAV_PREV] || ImGui::IsKeyPressed(ImGuiKey_PageUp)) { g_tab = (g_tab + NTABS - 1) % NTABS; g_tab_changed = true; }
    if (act[NAV_NEXT] || ImGui::IsKeyPressed(ImGuiKey_PageDown)) { g_tab = (g_tab + 1) % NTABS; g_tab_changed = true; }
    draw_menu((float)w, (float)h, ImGui::GetTime());
    if (!g_rev.active) nav_move(down ? 1 : up ? -1 : 0);   // controller up/down (keyboard arrows use ImGui's own nav)
    ImGui::Render();
    // draw into the window's framebuffer (with renderer=vulkan the runtime redirects framebuffer 0 to the image Vulkan presents)
    typedef void (APIENTRY * BindFB)(unsigned, unsigned); typedef void (APIENTRY * GetI)(unsigned, int*); typedef void (APIENTRY * Clip)(unsigned, unsigned);
    typedef void (APIENTRY * En)(unsigned); typedef unsigned char (APIENTRY * IsEn)(unsigned); typedef void (APIENTRY * VP)(int, int, int, int);
    static BindFB bind = (BindFB)backend_gl_proc("glBindFramebuffer"); static GetI geti = (GetI)backend_gl_proc("glGetIntegerv");
    static Clip clip = (Clip)backend_gl_proc("glClipControl"); static En en = (En)backend_gl_proc("glEnable"), dis = (En)backend_gl_proc("glDisable");
    static IsEn isen = (IsEn)backend_gl_proc("glIsEnabled"); static VP vp = (VP)backend_gl_proc("glViewport");
    int origin = 0x8CA1, depth = 0x935F; if (geti) { geti(0x935C, &origin); geti(0x935D, &depth); }   // GL_CLIP_ORIGIN, GL_CLIP_DEPTH_MODE
    bool srgb = isen && isen(0x8DB9);
    if (bind) bind(0x8D40, 0); if (clip) clip(0x8CA1, 0x935E); if (srgb && dis) dis(0x8DB9); if (vp) vp(0, 0, w, h);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (clip) clip((unsigned)origin, (unsigned)depth); if (srgb && en) en(0x8DB9);
    gpu_gl_state_dirty();
    { // test only: SWR_OV_SHOTS="N,N" (Nth menu frame) + SWR_OV_SHOT_PREFIX: the window with the menu, as PPM
      static const char* shots = getenv("SWR_OV_SHOTS"); static const char* pre = getenv("SWR_OV_SHOT_PREFIX"); static int n;
      n++;
      if (shots && pre) { bool want = false; for (const char* p = shots; *p;) { char* e; long v = strtol(p, &e, 10); if (v == n) want = true; if (e == p) break; p = *e == ',' ? e + 1 : e; }
        typedef void (APIENTRY * RP)(int, int, int, int, unsigned, unsigned, void*); static RP rp = (RP)backend_gl_proc("glReadPixels");
        if (want && rp) { std::vector<unsigned char> px((size_t)w * h * 3); rp(0, 0, w, h, 0x1907, 0x1401, px.data());
          std::string path = fmt("%s-m%d.ppm", pre, n); FILE* f = fopen(path.c_str(), "wb");
          if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); for (int y = h - 1; y >= 0; y--) fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f); fclose(f); } } } }
}
