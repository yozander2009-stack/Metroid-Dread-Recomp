// Shared theme, widgets and settings table for the front end (fe.cpp) and the in-game menu (ov.cpp).
#include "ui.h"
#include "imgui_internal.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <strings.h>
#include <unistd.h>
#define _stricmp strcasecmp
#endif
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <algorithm>

namespace ui {

const ImU32 C_BG0 = IM_COL32(4, 7, 13, 255), C_BG1 = IM_COL32(10, 17, 30, 255), C_PANEL = IM_COL32(9, 15, 27, 215),
            C_PANEL_HI = IM_COL32(18, 32, 52, 235), C_LINE = IM_COL32(52, 96, 128, 140), C_CYAN = IM_COL32(64, 214, 255, 255),
            C_CYAN_DIM = IM_COL32(64, 214, 255, 70), C_RED = IM_COL32(226, 30, 44, 255), C_RED_HI = IM_COL32(240, 52, 60, 255),
            C_TEXT = IM_COL32(232, 238, 246, 255), C_DIM = IM_COL32(140, 156, 180, 255), C_OK = IM_COL32(80, 220, 150, 255),
            C_WARN = IM_COL32(250, 196, 80, 255), C_BAD = IM_COL32(255, 84, 84, 255);
Fonts g_fonts; float g_scale = 1.0f; RowCtx g_row; bool g_focus_first;

float S(float px) { return px * g_scale; }

std::string fmt(const char* f, ...) {
    char b[2048]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); return b;
}

void load_fonts(ImGuiIO& io) {
    ImFontConfig cfg; cfg.OversampleH = 2;
#ifdef _WIN32
    char win[MAX_PATH]; GetWindowsDirectoryA(win, MAX_PATH);
    std::string dir = std::string(win) + "\\Fonts\\";
    const char* names[] = {"bahnschrift.ttf", "segoeui.ttf", "arial.ttf"};
    auto exists = [](const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
#else   /* the system's fonts (SteamOS has Noto and DejaVu) */
    std::string dir;
    const char* names[] = {"/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/NotoSans-Regular.ttf",
                           "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf"};
    auto exists = [](const std::string& p) { return access(p.c_str(), R_OK) == 0; };
#endif
    for (const char* n : names) {
        std::string p = dir + n;
        if (!exists(p)) continue;
        g_fonts.body = io.Fonts->AddFontFromFileTTF(p.c_str(), 20.0f, &cfg);
        if (g_fonts.body) break;
    }
    if (!g_fonts.body) g_fonts.body = io.Fonts->AddFontDefault();
#ifdef _WIN32
    // a symbol font for arrows and prompt glyphs, merged into the body font
    std::string sym = dir + "seguisym.ttf";
    if (exists(sym)) { ImFontConfig m; m.MergeMode = true; io.Fonts->AddFontFromFileTTF(sym.c_str(), 20.0f, &m); }
#else   // DejaVu has the arrows and shapes Noto Sans lacks (the Play screen's "CHANGE ▸")
    for (const char* sym : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf"})
        if (exists(sym)) { ImFontConfig m; m.MergeMode = true; io.Fonts->AddFontFromFileTTF(sym, 20.0f, &m); break; }
#endif
    g_fonts.head = g_fonts.body;
}

void apply_style(float scale) {
    g_scale = scale;
    ImGuiStyle st;
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0; st.FrameRounding = 2; st.GrabRounding = 2; st.ScrollbarRounding = 2; st.ChildRounding = 0;
    st.WindowBorderSize = 0; st.FrameBorderSize = 0; st.ScrollbarSize = 10;
    st.ItemSpacing = ImVec2(10, 6); st.WindowPadding = ImVec2(0, 0); st.FramePadding = ImVec2(10, 6);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(C_TEXT); c[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(C_DIM);
    c[ImGuiCol_FrameBg] = ImVec4(0.06f, 0.11f, 0.19f, 0.9f); c[ImGuiCol_FrameBgHovered] = ImVec4(0.09f, 0.17f, 0.28f, 1);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.84f, 1, 0.25f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.25f, 0.84f, 1, 0.5f); c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.25f, 0.84f, 1, 0.7f);
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);   // our rows draw their own focus
    c[ImGuiCol_Button] = ImVec4(0.07f, 0.13f, 0.22f, 1); c[ImGuiCol_ButtonHovered] = ImVec4(0.12f, 0.22f, 0.36f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.886f, 0.118f, 0.173f, 1); c[ImGuiCol_PopupBg] = ImVec4(0.03f, 0.06f, 0.1f, 0.97f);
    c[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(C_LINE); c[ImGuiCol_InputTextCursor] = ImGui::ColorConvertU32ToFloat4(C_CYAN);
    st.ScaleAllSizes(scale);
    st.FontScaleDpi = scale;
    ImGui::GetStyle() = st;
}

// ---------------------------------------------------------------- navigation
const char* const NAV_NAMES[NAV_COUNT] = {"Confirm", "Back", "Previous page", "Next page", "Play / home"};
const int NAV_DEFAULT[NAV_COUNT] = {1, 0, 6, 7, 10};
const char* const POSITION_NAMES[16] = {"Right face button", "Bottom face button", "Top face button", "Left face button",
    "Left stick click", "Right stick click", "Left shoulder", "Right shoulder", "Left trigger", "Right trigger",
    "Start / +", "Back / -", "D-pad left", "D-pad up", "D-pad right", "D-pad down"};

bool Nav::valid() const {
    for (int i = 0; i < NAV_COUNT; i++) {
        if (bind[i] < 0 || bind[i] > 11) return false;
        for (int j = 0; j < i; j++) if (bind[i] == bind[j]) return false;
    }
    return true;
}

void Nav::update(unsigned long long phys, int lx, int ly, double now) {
    int d = 0;
    if ((phys >> 13 & 1) || ly > 18000) d = 1;
    else if ((phys >> 15 & 1) || ly < -18000) d = 2;
    else if ((phys >> 12 & 1) || lx < -18000) d = 3;
    else if ((phys >> 14 & 1) || lx > 18000) d = 4;
    up = down = left = right = false;
    bool fire = false;
    if (d != pdir) { pdir = d; if (d) { fire = true; next_rep = now + 0.38; } }
    else if (d && now >= next_rep) { fire = true; next_rep = now + 0.09; }
    if (fire) { up = d == 1; down = d == 2; left = d == 3; right = d == 4; }
    unsigned long long pressed = phys & ~prev, released = prev & ~phys; prev = phys;
    // + and - (positions 10, 11) act when released, and never when both were held: holding them together is the reset gesture,
    // which must work even when Confirm or Back is bound to one of them
    const unsigned long long PM = (1ull << 10) | (1ull << 11);
    if ((phys & PM) == PM) pm_chord = true;
    unsigned long long trig = (pressed & ~PM) | (pm_chord ? 0 : (released & PM));
    if (!(phys & PM)) pm_chord = false;
    for (int i = 0; i < NAV_COUNT; i++) act[i] = enabled && bind[i] >= 0 && (trig >> bind[i] & 1);
    if (!enabled) up = down = left = right = false;
    if (phys || lx > 18000 || lx < -18000 || ly > 18000 || ly < -18000) any_pad = true;
}

void Nav::feed_imgui(ImGuiIO& io) {
    // ImGui's own gamepad nav moves focus; we send it a short press for each step (repeat is ours)
    (void)io;   // up/down go through nav_move (ImGui's own gamepad nav is not used)
}

bool Nav::reset_hold(double now, unsigned long long phys) {
    bool both = (phys >> 10 & 1) && (phys >> 11 & 1);
    if (!both) { reset_t0 = 0; return false; }
    if (reset_t0 == 0) reset_t0 = now;
    if (now - reset_t0 >= 3.0) { for (int i = 0; i < NAV_COUNT; i++) bind[i] = NAV_DEFAULT[i]; enabled = true; reset_t0 = now + 1e9; return true; }
    return false;
}

std::string bindings_to_string(const int b[NAV_COUNT]) {
    std::string s; for (int i = 0; i < NAV_COUNT; i++) { if (i) s += ","; s += std::to_string(b[i]); } return s;
}
bool bindings_from_string(const char* s, int b[NAV_COUNT]) {
    int v[NAV_COUNT]; const char* p = s;
    for (int i = 0; i < NAV_COUNT; i++) {
        char* e; long x = strtol(p, &e, 10); if (e == p || x < 0 || x > 11) return false; v[i] = (int)x;
        p = e; if (i < NAV_COUNT - 1) { if (*p != ',') return false; p++; }
    }
    for (int i = 0; i < NAV_COUNT; i++) for (int j = 0; j < i; j++) if (v[i] == v[j]) return false;
    memcpy(b, v, sizeof v); return true;
}

PromptStyle prompt_style_from(const char* s) {
    if (!s) return PROMPT_AUTO;
    if (!_stricmp(s, "xbox")) return PROMPT_XBOX; if (!_stricmp(s, "playstation")) return PROMPT_PS;
    if (!_stricmp(s, "nintendo")) return PROMPT_NINTENDO; if (!_stricmp(s, "positions")) return PROMPT_POSITIONS;
    return PROMPT_AUTO;
}
const char* prompt_style_key(PromptStyle p) {
    switch (p) { case PROMPT_XBOX: return "xbox"; case PROMPT_PS: return "playstation"; case PROMPT_NINTENDO: return "nintendo";
                 case PROMPT_POSITIONS: return "positions"; default: return "auto"; }
}
PromptStyle prompt_resolve(PromptStyle p, const char* name) {
    if (p != PROMPT_AUTO) return p;
    std::string n = name ? name : ""; for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
    auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
    if (has("dualsense") || has("dualshock") || has("playstation") || has("ps4") || has("ps5") || has("ps3")) return PROMPT_PS;
    if (has("nintendo") || has("switch") || has("joy-con") || has("pro controller")) return PROMPT_NINTENDO;
    return PROMPT_XBOX;   // XInput pads and unknown ones
}

void draw_prompt(ImDrawList* dl, ImVec2 c, float r, int pos, PromptStyle style) {
    static const char* XB[16] = {"B", "A", "Y", "X", "LS", "RS", "LB", "RB", "LT", "RT", "Menu", "View", "<", "^", ">", "v"};
    static const char* NT[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "+", "-", "<", "^", ">", "v"};
    static const ImU32 XC[4] = {IM_COL32(230, 70, 70, 255), IM_COL32(110, 200, 80, 255), IM_COL32(240, 200, 60, 255), IM_COL32(70, 140, 240, 255)};
    if (pos < 4 && style == PROMPT_PS) {
        dl->AddCircleFilled(c, r, IM_COL32(20, 26, 40, 255)); dl->AddCircle(c, r, IM_COL32(200, 210, 225, 255), 0, 1.5f);
        float s = r * 0.48f; ImU32 col;
        switch (pos) {
        case 0: col = IM_COL32(240, 90, 90, 255); dl->AddCircle(c, s, col, 0, 2); break;   // circle
        case 1: col = IM_COL32(110, 160, 240, 255); dl->AddLine({c.x - s, c.y - s}, {c.x + s, c.y + s}, col, 2); dl->AddLine({c.x - s, c.y + s}, {c.x + s, c.y - s}, col, 2); break;
        case 2: col = IM_COL32(80, 210, 170, 255); dl->AddTriangle({c.x, c.y - s}, {c.x + s, c.y + s * 0.8f}, {c.x - s, c.y + s * 0.8f}, col, 2); break;
        default: col = IM_COL32(230, 120, 200, 255); dl->AddRect({c.x - s * 0.8f, c.y - s * 0.8f}, {c.x + s * 0.8f, c.y + s * 0.8f}, col, 0, 0, 2); break;
        }
        return;
    }
    const char* t = style == PROMPT_NINTENDO ? NT[pos] : style == PROMPT_PS ? (pos == 6 ? "L1" : pos == 7 ? "R1" : pos == 8 ? "L2" : pos == 9 ? "R2" : pos == 10 ? "Opt" : pos == 11 ? "Share" : pos == 4 ? "L3" : pos == 5 ? "R3" : NT[pos])
                  : style == PROMPT_POSITIONS ? (pos == 0 ? "E" : pos == 1 ? "S" : pos == 2 ? "N" : pos == 3 ? "W" : XB[pos]) : XB[pos];
    bool face = pos < 4;
    ImVec2 ts = ImGui::CalcTextSize(t);
    float w = std::max(r * 2, ts.x * 0.8f + r);
    ImU32 fg = face && style == PROMPT_XBOX ? XC[pos] : IM_COL32(225, 232, 242, 255);
    if (face) { dl->AddCircleFilled(c, r, IM_COL32(20, 26, 40, 255)); dl->AddCircle(c, r, fg, 0, 1.5f); }
    else dl->AddRectFilled({c.x - w / 2, c.y - r * 0.8f}, {c.x + w / 2, c.y + r * 0.8f}, IM_COL32(30, 40, 60, 255), r * 0.35f);
    float fs = ImGui::GetFontSize() * (face ? 0.8f : 0.7f);
    ImVec2 t2 = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, t);
    dl->AddText(ImGui::GetFont(), fs, {c.x - t2.x / 2, c.y - t2.y / 2}, fg, t);
}

float prompt_bar(const std::vector<std::pair<int, const char*>>& items, PromptStyle style, ImVec2 p) {
    ImDrawList* dl = ImGui::GetWindowDrawList(); float x = p.x, r = S(13);
    for (auto& it : items) {
        draw_prompt(dl, {x + r, p.y + r}, r, it.first, style); x += r * 2 + S(8);
        dl->AddText({x, p.y + r - ImGui::GetFontSize() / 2}, C_TEXT, it.second);
        x += ImGui::CalcTextSize(it.second).x + S(26);
    }
    return x - p.x;
}

// ---------------------------------------------------------------- drawing
void chamfer_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut, ImU32 fill, ImU32 border, float thick) {
    ImVec2 pts[6] = {{a.x + cut, a.y}, {b.x, a.y}, {b.x, b.y - cut}, {b.x - cut, b.y}, {a.x, b.y}, {a.x, a.y + cut}};
    if (fill) dl->AddConvexPolyFilled(pts, 6, fill);
    if (border) dl->AddPolyline(pts, 6, border, ImDrawFlags_Closed, thick);
}

void background(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, bool translucent) {
    float w = b.x - a.x, h = b.y - a.y;
    if (translucent) dl->AddRectFilled(a, b, IM_COL32(2, 5, 10, 205));
    else dl->AddRectFilledMultiColor(a, b, C_BG1, C_BG0, C_BG0, C_BG1);
    // a faint hexagonal lattice
    float hs = S(46), hx = hs * 1.732f;
    for (int row = -1; row * hs * 1.5f < h + hs; row++)
        for (int col = -1; col * hx < w + hx; col++) {
            ImVec2 c(a.x + col * hx + (row & 1) * hx / 2, a.y + row * hs * 1.5f);
            ImVec2 pts[6];
            for (int k = 0; k < 6; k++) { float an = 3.14159f / 3 * k + 3.14159f / 6; pts[k] = {c.x + cosf(an) * hs, c.y + sinf(an) * hs}; }
            float dx = (c.x - a.x) / w; int alpha = (int)(10 + 14 * dx);
            dl->AddPolyline(pts, 6, IM_COL32(64, 214, 255, alpha), ImDrawFlags_Closed, 1.0f);
        }
    // scanner rings on the right, slowly turning
    ImVec2 rc(a.x + w * 0.80f, a.y + h * 0.42f); float R = h * 0.42f;
    for (int i = 0; i < 3; i++) {
        float r = R * (0.55f + 0.22f * i), s = t * (0.10f + 0.05f * i) * (i & 1 ? -1 : 1);
        dl->PathArcTo(rc, r, s, s + 4.2f, 64); dl->PathStroke(IM_COL32(64, 214, 255, 26 + 8 * i), 0, S(1.5f));
        dl->PathArcTo(rc, r, s + 4.6f, s + 5.6f, 24); dl->PathStroke(IM_COL32(226, 30, 44, 46), 0, S(2.0f));
    }
    // a light streak and a vignette
    dl->AddRectFilledMultiColor({a.x, a.y + h * 0.70f}, {b.x, a.y + h * 0.70f + S(2)}, IM_COL32(226, 30, 44, 0), IM_COL32(226, 30, 44, 60),
                                IM_COL32(226, 30, 44, 60), IM_COL32(226, 30, 44, 0));
    dl->AddRectFilledMultiColor(a, {b.x, a.y + h * 0.18f}, IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor({a.x, b.y - h * 0.2f}, b, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 170));
}

static void spaced_text(ImDrawList* dl, ImVec2 p, float size, ImU32 col, const char* s, float spacing, bool bold) {
    ImFont* f = ImGui::GetFont();
    for (const char* c = s; *c; c++) {
        char ch[2] = {*c, 0};
        dl->AddText(f, size, p, col, ch);
        if (bold) dl->AddText(f, size, {p.x + size * 0.03f, p.y}, col, ch);
        p.x += f->CalcTextSizeA(size, FLT_MAX, 0, ch).x + spacing;
    }
}

void logo(ImDrawList* dl, ImVec2 pos, float size, bool tag) {
    spaced_text(dl, pos, size, C_TEXT, "METROID", size * 0.10f, true);
    spaced_text(dl, {pos.x, pos.y + size * 0.95f}, size * 1.25f, C_RED, "DREAD", size * 0.14f, true);
    if (tag) spaced_text(dl, {pos.x + size * 0.04f, pos.y + size * 2.35f}, size * 0.30f, C_CYAN, "RECOMPILED FOR PC", size * 0.08f, false);
}

void page_title(const char* title, const char* sub) {
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = S(40);
    spaced_text(dl, p, fs, C_TEXT, title, S(3), true);
    dl->AddRectFilled({p.x, p.y + fs + S(8)}, {p.x + S(60), p.y + fs + S(11)}, C_RED);
    dl->AddLine({p.x + S(66), p.y + fs + S(9.5f)}, {p.x + S(420), p.y + fs + S(9.5f)}, C_CYAN_DIM, S(1));
    ImGui::Dummy({1, fs + S(18)});
    if (sub && *sub) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C_DIM)); ImGui::TextWrapped("%s", sub); ImGui::PopStyleColor(); }
    ImGui::Dummy({1, S(10)});
}

void section(const char* title) {
    ImGui::Dummy({1, S(10)});
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled({p.x, p.y + S(4)}, {p.x + S(4), p.y + S(22)}, C_CYAN);
    spaced_text(dl, {p.x + S(14), p.y}, S(19), C_CYAN, title, S(2), false);
    dl->AddLine({p.x, p.y + S(30)}, {p.x + w, p.y + S(30)}, C_LINE, 1);
    ImGui::Dummy({1, S(36)});
}

void help_text(const char* s) {
    ImGui::Indent(S(18)); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C_DIM));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - S(18));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.86f); ImGui::TextUnformatted(s); ImGui::PopFont();
    ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); ImGui::Unindent(S(18)); ImGui::Dummy({1, S(4)});
}

// ---------------------------------------------------------------- our own up/down navigation
struct NavItem { ImGuiID id; ImRect r; ImGuiWindow* w; };
static std::vector<NavItem> g_nav_items;
void nav_frame_begin() { g_nav_items.clear(); }
void nav_item() { g_nav_items.push_back({ImGui::GetItemID(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), ImGui::GetCurrentWindow()}); }
void nav_move(int dir) {
    if (!dir || g_nav_items.empty()) return;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const NavItem* cur = nullptr; for (auto& it : g_nav_items) if (it.id == g.NavId) cur = &it;
    const NavItem* best = nullptr;
    if (!cur) best = &g_nav_items[dir > 0 ? 0 : g_nav_items.size() - 1];
    else {
        float cx = (cur->r.Min.x + cur->r.Max.x) / 2, best_score = 1e30f;
        for (auto& it : g_nav_items) {
            if (it.id == cur->id) continue;
            float gap = dir > 0 ? it.r.Min.y - cur->r.Max.y : cur->r.Min.y - it.r.Max.y;   // rows strictly below / above
            if (gap < -2) continue;
            float dx = fabsf((it.r.Min.x + it.r.Max.x) / 2 - cx);
            float score = (gap < 0 ? 0 : gap) * 4 + dx;   // nearest row first, then the one most in line
            if (score < best_score) { best_score = score; best = &it; }
        }
    }
    if (!best) return;
    ImGui::FocusWindow(best->w); ImGui::SetFocusID(best->id, best->w);
    best->w->NavRectRel[0] = ImGui::WindowRectAbsToRel(best->w, best->r);
    g.NavCursorVisible = true;
    // keep the focused row in view, with some of the rows around it
    ImRect v = best->r; v.Min.y -= S(70); v.Max.y += S(70);
    ImGui::ScrollToRect(best->w, v, ImGuiScrollFlags_KeepVisibleEdgeY);
}

// a page or tab just opened: its first row (or big button) takes the focus, so the controller starts there
static void take_focus_if_first() {
    if (!g_focus_first) return;
    g_focus_first = false;
    ImGuiWindow* w = ImGui::GetCurrentWindow(); ImGui::FocusWindow(w); ImGui::SetFocusID(ImGui::GetItemID(), w);
    ImGui::GetCurrentContext()->NavCursorVisible = true;
}

// one option row: returns its rect; focus drawn here
static bool row_begin(const char* id, const char* label, const char* tag, ImRect& r, bool& focused, bool& clicked, float h = 0) {
    float w = ImGui::GetContentRegionAvail().x; if (h <= 0) h = S(48);
    ImVec2 p = ImGui::GetCursorScreenPos();
    clicked = ImGui::InvisibleButton(id, {w, h});
    take_focus_if_first(); nav_item();
    focused = ImGui::IsItemFocused(); bool hovered = ImGui::IsItemHovered();
    r = ImRect(p, {p.x + w, p.y + h});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (focused || hovered) {
        dl->AddRectFilledMultiColor(r.Min, r.Max, IM_COL32(64, 214, 255, focused ? 46 : 22), IM_COL32(64, 214, 255, 6),
                                    IM_COL32(64, 214, 255, 6), IM_COL32(64, 214, 255, focused ? 46 : 22));
        if (focused) { dl->AddRectFilled(r.Min, {r.Min.x + S(4), r.Max.y}, C_RED); dl->AddRect(r.Min, r.Max, C_CYAN_DIM, 0, 0, 1); }
    } else dl->AddLine({r.Min.x, r.Max.y - 1}, {r.Max.x, r.Max.y - 1}, IM_COL32(52, 96, 128, 50), 1);
    float ty = r.Min.y + (h - ImGui::GetFontSize()) / 2;
    dl->AddText({r.Min.x + S(18), ty}, focused ? IM_COL32(255, 255, 255, 255) : C_TEXT, label);
    if (tag && *tag) {
        float lw = ImGui::CalcTextSize(label).x; float fs = ImGui::GetFontSize() * 0.68f;
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, tag);
        ImVec2 tp(r.Min.x + S(30) + lw, r.Min.y + (h - ts.y) / 2 - S(2));
        dl->AddRect({tp.x - S(5), tp.y - S(2)}, {tp.x + ts.x + S(5), tp.y + ts.y + S(2)}, C_WARN, S(3), 0, 1);
        dl->AddText(ImGui::GetFont(), fs, tp, C_WARN, tag);
    }
    return true;
}

static void arrows_value(ImDrawList* dl, const ImRect& r, const char* value, bool focused, float wv) {
    float h = r.GetHeight(); float x1 = r.Max.x - S(16), x0 = x1 - wv;
    ImU32 ac = focused ? C_RED : C_DIM;
    float cy = r.Min.y + h / 2, a = S(6);
    dl->AddTriangleFilled({x0, cy}, {x0 + a, cy - a}, {x0 + a, cy + a}, ac);
    dl->AddTriangleFilled({x1, cy}, {x1 - a, cy - a}, {x1 - a, cy + a}, ac);
    ImVec2 ts = ImGui::CalcTextSize(value);
    dl->AddText({(x0 + x1) / 2 - ts.x / 2, cy - ts.y / 2}, focused ? C_CYAN : C_TEXT, value);
}

bool row_bool(const char* id, const char* label, bool* v, const char* tag) {
    ImRect r; bool f, c; row_begin(id, label, tag, r, f, c);
    bool ch = false;
    if (c || (f && (g_row.pad_left || g_row.pad_right || g_row.confirm))) { *v = !*v; ch = true; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = r.GetHeight(), sw = S(52), sh = S(24); ImVec2 a(r.Max.x - S(16) - sw, r.Min.y + (h - sh) / 2);
    dl->AddRectFilled(a, {a.x + sw, a.y + sh}, *v ? IM_COL32(226, 30, 44, 200) : IM_COL32(30, 44, 66, 255), sh / 2);
    dl->AddRect(a, {a.x + sw, a.y + sh}, *v ? C_RED_HI : C_LINE, sh / 2, 0, 1);
    float kx = *v ? a.x + sw - sh / 2 : a.x + sh / 2;
    dl->AddCircleFilled({kx, a.y + sh / 2}, sh / 2 - S(3), *v ? IM_COL32(255, 255, 255, 255) : C_DIM);
    const char* t = *v ? "ON" : "OFF"; ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText({a.x - ts.x - S(12), r.Min.y + (h - ts.y) / 2}, *v ? C_CYAN : C_DIM, t);
    return ch;
}

bool row_choice(const char* id, const char* label, int* idx, const std::vector<std::string>& items, const char* tag) {
    ImRect r; bool f, c; row_begin(id, label, tag, r, f, c);
    int n = (int)items.size(); bool ch = false; if (n == 0) return false;
    float wv = S(300);
    if (f && g_row.pad_left) { *idx = (*idx - 1 + n) % n; ch = true; }
    if (f && (g_row.pad_right || g_row.confirm)) { *idx = (*idx + 1) % n; ch = true; }
    if (c) { float mx = ImGui::GetIO().MousePos.x; float x0 = r.Max.x - S(16) - wv;
             if (mx < x0 + wv / 2) *idx = (*idx - 1 + n) % n; else *idx = (*idx + 1) % n; ch = true; }
    if (*idx < 0 || *idx >= n) *idx = 0;
    arrows_value(ImGui::GetWindowDrawList(), r, items[*idx].c_str(), f, wv);
    return ch;
}

bool row_slider(const char* id, const char* label, float* v, float lo, float hi, float step, const char* text, const char* tag) {
    ImRect r; bool f, c; row_begin(id, label, tag, r, f, c, S(56));
    bool ch = false; float old = *v;
    if (f && g_row.pad_left) *v -= step;
    if (f && g_row.pad_right) *v += step;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = r.GetHeight(), bw = S(380), bh = S(30);
    ImVec2 a(r.Max.x - S(16) - bw, r.Min.y + (h - bh) / 2), b(a.x + bw, a.y + bh);
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
        float mx = ImGui::GetIO().MousePos.x;
        if (mx >= a.x - S(10)) { float t = (mx - a.x) / bw; t = t < 0 ? 0 : t > 1 ? 1 : t; *v = lo + t * (hi - lo); *v = roundf(*v / step) * step; }
    }
    if (*v < lo) *v = lo; if (*v > hi) *v = hi;
    if (*v != old) ch = true;
    float t = (*v - lo) / (hi - lo);
    chamfer_rect(dl, a, b, S(6), IM_COL32(12, 22, 38, 255), f ? C_CYAN : C_LINE, 1);
    ImVec2 fb(a.x + (b.x - a.x) * t, b.y);
    dl->PushClipRect(a, fb, true); chamfer_rect(dl, a, b, S(6), f ? IM_COL32(226, 30, 44, 190) : IM_COL32(226, 30, 44, 120), 0); dl->PopClipRect();
    dl->AddRectFilled({fb.x - S(1.5f), a.y - S(3)}, {fb.x + S(1.5f), b.y + S(3)}, IM_COL32(255, 255, 255, 230));
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText({(a.x + b.x) / 2 - ts.x / 2 + 1, (a.y + b.y) / 2 - ts.y / 2 + 1}, IM_COL32(0, 0, 0, 200), text);
    dl->AddText({(a.x + b.x) / 2 - ts.x / 2, (a.y + b.y) / 2 - ts.y / 2}, IM_COL32(255, 255, 255, 255), text);
    return ch;
}

bool row_button(const char* id, const char* label, const char* value, bool primary) {
    ImRect r; bool f, c; row_begin(id, label, nullptr, r, f, c);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* t = value ? value : "SELECT"; ImVec2 ts = ImGui::CalcTextSize(t);
    ImVec2 a(r.Max.x - S(16) - ts.x - S(28), r.Min.y + S(9)), b(r.Max.x - S(16), r.Max.y - S(9));
    chamfer_rect(dl, a, b, S(5), primary ? IM_COL32(226, 30, 44, f ? 255 : 200) : IM_COL32(20, 36, 58, 255), f ? C_CYAN : C_LINE, 1);
    dl->AddText({a.x + S(14), (a.y + b.y) / 2 - ts.y / 2}, IM_COL32(255, 255, 255, 255), t);
    return c || (f && g_row.confirm);
}

void row_text(const char* label, const char* value, ImU32 col) {
    float w = ImGui::GetContentRegionAvail().x, h = S(40); ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy({w, h});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText({p.x + S(18), p.y + (h - ImGui::GetFontSize()) / 2}, C_DIM, label);
    ImVec2 ts = ImGui::CalcTextSize(value);
    dl->AddText({p.x + w - S(16) - ts.x, p.y + (h - ts.y) / 2}, col ? col : C_TEXT, value);
}

bool big_button(const char* id, const char* label, ImVec2 size, bool primary) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool c = ImGui::InvisibleButton(id, size); take_focus_if_first(); nav_item(); bool f = ImGui::IsItemFocused(), h = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 b(p.x + size.x, p.y + size.y);
    ImU32 fill = primary ? (f || h ? C_RED_HI : C_RED) : (f || h ? IM_COL32(28, 52, 84, 255) : IM_COL32(16, 30, 50, 255));
    chamfer_rect(dl, p, b, size.y * 0.28f, fill, f ? IM_COL32(255, 255, 255, 255) : (primary ? 0 : C_LINE), f ? S(2) : 1);
    if (f) chamfer_rect(dl, {p.x - S(4), p.y - S(4)}, {b.x + S(4), b.y + S(4)}, size.y * 0.3f, 0, C_CYAN_DIM, S(1.5f));
    float fs = std::min(size.y * 0.42f, S(30)); ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, label);
    dl->AddText(ImGui::GetFont(), fs, {p.x + size.x / 2 - ts.x / 2, p.y + size.y / 2 - ts.y / 2}, IM_COL32(255, 255, 255, 255), label);
    return c || (f && g_row.confirm);
}

void progress_bar(float frac, ImVec2 size, const char* text) {
    ImVec2 p = ImGui::GetCursorScreenPos(); ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 b(p.x + size.x, p.y + size.y);
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    chamfer_rect(dl, p, b, S(8), IM_COL32(8, 16, 28, 255), C_LINE, 1);
    ImVec2 fb(p.x + size.x * frac, b.y);
    dl->PushClipRect(p, fb, true);
    dl->AddRectFilledMultiColor(p, b, IM_COL32(226, 30, 44, 255), IM_COL32(240, 52, 60, 255), IM_COL32(240, 52, 60, 255), IM_COL32(226, 30, 44, 255));
    float t = (float)ImGui::GetTime();
    for (float x = p.x - size.y + fmodf(t * 60, size.y * 2); x < fb.x; x += size.y * 2)   // moving stripes
        dl->AddQuadFilled({x, b.y}, {x + size.y, p.y}, {x + size.y * 1.6f, p.y}, {x + size.y * 0.6f, b.y}, IM_COL32(255, 255, 255, 28));
    dl->PopClipRect();
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText({p.x + size.x / 2 - ts.x / 2 + 1, p.y + size.y / 2 - ts.y / 2 + 1}, IM_COL32(0, 0, 0, 220), text);
    dl->AddText({p.x + size.x / 2 - ts.x / 2, p.y + size.y / 2 - ts.y / 2}, IM_COL32(255, 255, 255, 255), text);
}

// ---------------------------------------------------------------- resolution text
float game_base_w(bool docked, const char* aspect) {
    if (!docked) return 1280; if (aspect && !strcmp(aspect, "21:9")) return 1680; if (aspect && !strcmp(aspect, "16:10")) return 1440; return 1600;
}
float game_base_h(bool docked, const char* aspect) {
    if (!docked) return 720; if (aspect && !strcmp(aspect, "21:9")) return 720; return 900;
}
std::string res_text(float s, bool docked, const char* aspect) {
    int w = (int)(game_base_w(docked, aspect) * s), h = (int)(game_base_h(docked, aspect) * s);
    return fmt("%.2f\xC3\x97 (%d \xC3\x97 %d)", s, w, h);
}

// ---------------------------------------------------------------- settings table
static std::vector<Opt> build_options() {
    std::vector<Opt> o;
    auto B = [&](const char* k, const char* sec, const char* page, const char* label, OptLive live, const char* def, const char* help, bool menu = true) {
        Opt x; x.key = k; x.section = sec; x.page = page; x.label = label; x.help = help; x.type = OPT_BOOL; x.live = live; x.def = def; x.menu = menu; o.push_back(x); };
    auto C = [&](const char* k, const char* sec, const char* page, const char* label, OptLive live, const char* def, std::vector<Choice> ch, const char* help, bool menu = true) {
        Opt x; x.key = k; x.section = sec; x.page = page; x.label = label; x.help = help; x.type = OPT_CHOICE; x.live = live; x.def = def; x.choices = ch; x.menu = menu; o.push_back(x); };
    auto F = [&](const char* k, const char* sec, const char* page, const char* label, OptLive live, const char* def, float lo, float hi, float st, const char* help, bool menu = true) {
        Opt x; x.key = k; x.section = sec; x.page = page; x.label = label; x.help = help; x.type = OPT_FLOAT; x.live = live; x.def = def; x.lo = lo; x.hi = hi; x.step = st; x.menu = menu; o.push_back(x); };
    auto I = [&](const char* k, const char* sec, const char* page, const char* label, OptLive live, const char* def, float lo, float hi, float st, const char* help, bool menu = true) {
        Opt x; x.key = k; x.section = sec; x.page = page; x.label = label; x.help = help; x.type = OPT_INT; x.live = live; x.def = def; x.lo = lo; x.hi = hi; x.step = st; x.menu = menu; o.push_back(x); };
    // Display
    B("docked", "Display", "Display", "Docked (TV) mode", LIVE_RESTART, "1", "Docked: the game renders 1600\xC3\x97" "900. Handheld: 1280\xC3\x97" "720.");
    B("fullscreen", "Display", "Display", "Fullscreen", LIVE_YES, "1", "F11 or Alt+Enter also switches it.");
    C("aspect", "Display", "Display", "Picture shape", LIVE_RESTART, "16:9", {{"16:9", "16:9"}, {"21:9", "21:9 ultrawide (experimental)"}, {"32:9", "32:9 super-ultrawide (experimental)"}, {"16:10", "16:10"}},
      "21:9 is theboy181's patch (commissioned by Nikorasu081); 16:10 uses the same spot for 1440\xC3\x97" "900 (Steam Deck, 1920\xC3\x97" "1200). "
      "They need Docked mode and may cut off a little in videos and some menus. 21:9 and 32:9 are experimental: the game "
      "only draws the room you are in, so the extra width can show black past a room's edge.");
    C("language", "Display", "Display", "Language", LIVE_RESTART, "1", {{"0", "Japanese"}, {"1", "English (US)"}, {"2", "French"}, {"3", "German"},
      {"4", "Italian"}, {"5", "Spanish"}, {"6", "Chinese"}, {"7", "Korean"}, {"8", "Dutch"}, {"9", "Portuguese (Brazil): fan translation"}, {"10", "Russian"},
      {"11", "Taiwanese"}, {"12", "English (UK)"}, {"13", "French (Canada)"}, {"14", "Spanish (Latin America)"}}, "");
    // Graphics
    F("resolution", "Graphics", "Graphics", "Internal resolution", LIVE_GL, "1", 0.25f, 4.0f, 0.05f,
      "The game's 3D resolution. Above your screen's size it is averaged down (smoother edges, no shimmer).");
    C("renderer", "Graphics", "Graphics", "Renderer", LIVE_RESTART, "opengl", {{"opengl", "OpenGL"}, {"vulkan", "Vulkan output"}, {"vulkan_native", "Native Vulkan (experimental)"}},
      "OpenGL is the most tested. Vulkan output: OpenGL draws, Vulkan presents. Native Vulkan: ScarletNX's Vulkan renderer draws everything.");
    B("antialiasing", "Graphics", "Graphics", "Anti-aliasing (FXAA)", LIVE_YES, "0", "Smooths jagged edges on the final picture. Not needed above 1\xC3\x97 resolution.");
    C("sharpening", "Graphics", "Graphics", "Sharpening", LIVE_YES, "0", {{"0", "Off"}, {"25", "Low"}, {"50", "Medium"}, {"75", "High"}, {"100", "Maximum"}},
      "Sharpens the final picture. Helps when the game looks soft, most at 1\xC3\x97 resolution.");
    C("anisotropy", "Graphics", "Graphics", "Anisotropic filtering", LIVE_RESTART, "0", {{"0", "Game default"}, {"2", "2\xC3\x97"}, {"4", "4\xC3\x97"}, {"8", "8\xC3\x97"}, {"16", "16\xC3\x97"}},
      "Sharper floors and walls at an angle.");
    B("shadows", "Graphics", "Graphics", "Shadows", LIVE_YES, "1", "");
    B("dynamic_resolution", "Graphics", "Graphics", "Dynamic resolution", LIVE_YES, "0", "Lowers the resolution when the graphics card can't keep up.");
    F("dynamic_resolution_min", "Graphics", "Graphics", "Lowest dynamic resolution", LIVE_YES, "0.6", 0.25f, 1.0f, 0.05f, "");
    B("game_dynamic_resolution", "Graphics", "Graphics", "Game's own dynamic resolution", LIVE_YES, "0", "Let the game lower its own resolution like on the console.");
    B("hdr", "Graphics", "Graphics", "HDR output (experimental)", LIVE_RESTART, "0", "Native Vulkan only, with \"Use HDR\" on in Windows display settings.");
    I("hdr_paper_white", "Graphics", "Graphics", "HDR paper white (nits)", LIVE_RESTART, "200", 80, 500, 10, "");
#ifndef SWR_RELEASE
    B("dlss", "Graphics", "Graphics", "NVIDIA DLAA", LIVE_RESTART, "0", "OpenGL or Native Vulkan, RTX cards, nvngx_dlss.dll next to the game.", false);
#endif
    // Gameplay
    C("fps_limit", "Performance", "Gameplay", "Frame rate", LIVE_YES, "1", {{"1", "60 FPS (console)"}, {"90", "90 FPS"}, {"120", "120 FPS"}, {"144", "144 FPS"}, {"240", "240 FPS"}, {"0", "Unlimited"}},
      "Dread's engine uses real elapsed time, so the game keeps its speed above 60 when your PC holds the cap steadily. Slow-motion moments run at 60.");
    B("forced_vsync", "Performance", "Gameplay", "Force VSync", LIVE_YES, "0", "Can lower FPS a lot on slow PCs.");
    B("show_fps", "Graphics", "Gameplay", "FPS counter", LIVE_YES, "0", "Shows the frames per second that reach the screen, in the top-left corner.");
    B("loading_60fps", "Launcher", "Gameplay", "60 FPS elevators and loading screens", LIVE_YES, "1", "The game draws them at 30.");
#ifndef SWR_NO_SKIPCUT
    B("skip_cutscenes", "Gameplay", "Gameplay", "Skip any cutscene", LIVE_YES, "0", "Hold - to skip a cutscene, even the first time you see it. A prompt shows in the top-left corner while you can.");
#endif
    B("cutscenes_60fps", "Launcher", "Gameplay", "60 FPS cutscenes (theboy181)", LIVE_YES, "0", "Port of theboy181's 60fps patch: cinematics show a new picture every 1/60 s.");
    B("fast_loading", "Performance", "Gameplay", "Fast loading", LIVE_YES, "0", "Shortens the game's fixed 60 ms pauses during loads (about 1 s faster first load).");
    B("fast_forward", "Performance", "Gameplay", "Fast-forward toggle (Tab or L3+R3)", LIVE_YES, "0", "For elevator and train rides. Don't leave it on during play.");
    I("fast_forward_speed", "Performance", "Gameplay", "Fast-forward speed", LIVE_YES, "3", 2, 8, 1, "");
    // Audio
    I("volume", "Audio", "Audio", "Master volume", LIVE_YES, "100", 0, 200, 5, "");
    B("audio_surround_downmix", "Performance", "Audio", "Mix all six surround channels into stereo", LIVE_YES, "0", "");
    // Controls
    B("mouse_aim", "Input", "Controls", "Mouse free aim", LIVE_YES, "1",
      "Hold the right mouse button and move the mouse to aim in any direction. Left click shoots, middle click arms missiles.");
    I("mouse_sensitivity", "Input", "Controls", "Mouse aim sensitivity", LIVE_YES, "100", 25, 400, 5, "Percent: higher turns the aim with less mouse movement.");
    C("button_icons", "Input", "Controls", "Button icons", LIVE_RESTART, "auto",
      {{"auto", "Automatic"}, {"switch", "Switch (original)"}, {"xbox", "Xbox"}, {"ps4", "PlayStation 4"}, {"ps5", "PlayStation 5"}},
      "Automatic: from the controller connected when the game starts. Xbox controllers and the Steam Deck get Xbox icons, DualShock 4 PS4 icons, DualSense PS5 icons, Switch controllers the originals.");
    C("audio_channels", "Audio", "Audio", "Speakers", LIVE_RESTART, "stereo", {{"stereo", "Stereo"}, {"5.1", "5.1 surround"}},
      "5.1: the game's own surround mix (front, centre, subwoofer, rear) goes to your 5.1 speakers or receiver. On stereo speakers Windows mixes it down.");
    C("menu_buttons", "Input", "Controls", "Confirm / back in menus", LIVE_YES, "game", {{"game", "As in the game (A right, B bottom)"}, {"swap", "Swapped (confirm on the bottom button)"}},
      "Swapped: in the game's menus (pause, map, options, dialogs, title) A and B change places, so the bottom button confirms like on Xbox and PlayStation. Gameplay buttons stay the same.");
    C("controller_api", "Input", "Controls", "Controller support", LIVE_RESTART, "auto", {{"auto", "Automatic"}, {"sdl", "SDL2"}, {"xinput", "XInput"}},
      "Automatic: SDL2 (Switch Pro, Joy-Con, DualShock 4, DualSense, 8BitDo, Xbox) when SDL2.dll is next to the game, else XInput.");
    I("controller_index", "Input", "Controls", "Player 1 controller", LIVE_YES, "0", 0, 8, 1, "0 = any connected controller.", false);
    I("stick_deadzone", "Input", "Controls", "Stick dead zone", LIVE_YES, "7000", 0, 20000, 500, "");
    I("rumble", "Input", "Controls", "Rumble strength", LIVE_YES, "100", 0, 200, 10, "0 = off. Up to 200% boosts soft effects.");
    // Advanced
    B("wait_for_shaders", "Launcher", "Advanced", "Wait for new shaders", LIVE_RESTART, "1", "On: no missing objects, a short stutter the first time an effect appears. Off: no stutter, objects can pop in.");
    B("prebuild_shaders", "Graphics", "Advanced", "Build all shaders before playing", LIVE_RESTART, "1",
      "OpenGL: the first start after an update or driver change takes up to about 30 s. Native Vulkan: the game's shaders are prepared in the background while the title screen plays, so the first session stutters much less.");
    B("low_priority_shader_compiler", "Performance", "Advanced", "Compile shaders at low priority", LIVE_RESTART, "1", "");
    B("sleep_waits", "Performance", "Advanced", "Sleep instead of spinning while waiting for the driver", LIVE_YES, "1", "Frees CPU time on 2-4 core PCs.");
    C("texture_threads", "Performance", "Advanced", "Texture unpacking threads", LIVE_RESTART, "0", {{"0", "Automatic"}, {"1", "1"}, {"2", "2"}, {"4", "4"}, {"8", "8"}}, "");
    C("memory_gb", "Performance", "Advanced", "Memory the game sees", LIVE_RESTART, "0", {{"0", "Automatic"}, {"4", "4 GB"}, {"6", "6 GB"}, {"8", "8 GB"}}, "Larger values only help heavily modded games.");
    B("smooth_recording", "Recording", "Advanced", "Smooth F7 recordings", LIVE_YES, "1", "Slow motion while recording if the PC can't keep up; the video plays at normal speed.");
    C("recording_fps", "Recording", "Advanced", "Recording FPS", LIVE_YES, "60", {{"30", "30"}, {"60", "60"}}, "");
    C("log", "Debug", "Advanced", "Log detail", LIVE_YES, "1", {{"1", "Normal"}, {"2", "Everything (slow)"}}, "");
    B("diag", "Debug", "Advanced", "Automatic diagnostic dumps", LIVE_YES, "0", "");
    B("profile", "Debug", "Advanced", "CPU profiler (profile.txt every 10 s)", LIVE_RESTART, "0", "");
    C("menu_shortcut", "Launcher", "Controls", "In-game menu button", LIVE_YES, "home", {{"home", "Home / Guide / PS"}, {"minus_plus", "- and + together"}, {"l3_r3", "L3 + R3 held 1 s"}},
      "F1 on the keyboard always opens the menu.");
    B("skip_launcher", "Launcher", "Advanced", "Skip the Play screen at start", LIVE_YES, "0", "Hold Shift while starting to see it again.", false);
    return o;
}
const std::vector<Opt>& options() { static std::vector<Opt> o = build_options(); return o; }
const Opt* find_option(const char* key) { for (auto& o : options()) if (!strcmp(o.key, key)) return &o; return nullptr; }

} // namespace ui
