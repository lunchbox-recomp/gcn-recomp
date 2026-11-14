// The menu's header and the page it shows (tabs.h).
#include "overlay/tabs.h"
#include "overlay/fonts.h"
#include "overlay/motion.h"
#include "overlay/theme.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include "imgui.h"
#include "imgui_internal.h"  // ImGui::NavInitWindow

namespace {

int g_page;          // the page showing, 0 for Display
bool g_focus_first;  // the page's first item takes the focus at its next draw, after a change
bool g_moved;        // and that change was a shoulder's or a key's, which shows the focus as a move does

bool g_motion = true;
int g_drawn = -1;  // the page the header last drew, -1 to draw the next one in place with no motion
// The pill under the page showing: its left edge from the header's, and its width, in pixels.
Tween g_pill_x, g_pill_w;
// The page's contents as they come in: how far they're moved across, in the theme's pixels at
// 1080 lines, and their alpha.
Tween g_slide, g_fade;
// Where the page's contents are in the panel's draw list at the last frame, and the scale then.
ImDrawList* g_list;
int g_begin, g_end;
float g_scale = 1.0f;

ImU32 col(uint32_t c) { return IM_COL32(c >> 24, c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF); }

bool pressed(std::initializer_list<ImGuiKey> keys) {
    return std::any_of(keys.begin(), keys.end(), [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); });
}

// The page a shoulder or a key pressed this frame moves to from the one showing: back a page for
// L1, L2 or Q, on a page for R1, R2 or E, and one page however many of either are pressed at
// once. A GameCube pad's Z is R1 to SDL and its L and R are L2 and R2 (overlay_read_pads).
int shouldered() {
    return g_page + (pressed({ImGuiKey_GamepadR1, ImGuiKey_GamepadR2, ImGuiKey_E}) ? 1 : 0) -
           (pressed({ImGuiKey_GamepadL1, ImGuiKey_GamepadL2, ImGuiKey_Q}) ? 1 : 0);
}

// A change's length, none with motion off. A game's theme may give a time that's negative,
// infinite or not a number, which takes none.
float span(float ms) { return g_motion && std::isfinite(ms) ? std::max(ms, 0.0f) : 0.0f; }

// A tween at `to` from now on.
Tween at_rest(float to) {
    Tween t;
    t.from = t.to = to;
    return t;
}

// The pill under the page showing heads for `x` and `w`, and a new page's contents come in from
// the side it's on, after a change the header saw at `now`. A first draw, and a change of size
// with nothing moving, put it there at once.
void follow(float x, float w, uint64_t now) {
    const OverlayTheme& t = theme_now();
    if (g_drawn >= 0 && g_drawn != g_page) {
        const float ms = span(t.page_ms);
        g_pill_x = tween_to(g_pill_x, now, x, ms, Ease::out_quad);
        g_pill_w = tween_to(g_pill_w, now, w, ms, Ease::out_quad);
        const float side = g_page > g_drawn ? 1.0f : -1.0f;
        g_slide = tween_to(at_rest(side * t.page_slide), now, 0.0f, ms, Ease::out_cubic);
        g_fade = tween_to(at_rest(0.0f), now, 1.0f, ms, Ease::out_cubic);
    } else if (g_drawn < 0 || (g_pill_x.done(now) && (g_pill_x.to != x || g_pill_w.to != w))) {
        g_pill_x = at_rest(x);
        g_pill_w = at_rest(w);
    }
    g_drawn = g_page;
}

}  // namespace

void tabs_motion(bool on) { g_motion = on; }

void tabs_reset() {
    g_page = 0;
    g_drawn = -1;
    g_fade = at_rest(1.0f);
    g_slide = at_rest(0.0f);
}

int tabs_page() { return g_page; }

void tabs_show(int page) {
    g_page = page;
    g_drawn = -1;
}

void tabs_pick(int page, bool moved) {
    if (page == g_page) return;
    g_page = page;
    g_focus_first = true;
    g_moved = moved;
}

bool tabs_moving(uint64_t now) {
    return !g_pill_x.done(now) || !g_pill_w.done(now) || !g_slide.done(now) || !g_fade.done(now);
}

void tabs_header(const std::string& title, const std::vector<std::string>& pages, bool live, uint64_t now) {
    const OverlayTheme& t = theme_now();
    const float s = ImGui::GetStyle().FontScaleMain;  // the theme's scale at this window's height
    const int last = std::max((int)pages.size() - 1, 0);
    g_page = std::clamp(g_page, 0, last);
    // While an item is held or dragged a shoulder is its own: a slider's L1 and R1 step it finer
    // and coarser. There's no wrap at either end.
    if (live && !ImGui::IsAnyItemActive()) tabs_pick(std::clamp(shouldered(), 0, last), true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = t.item_min_height * s;  // a pill, and the row
    const float gap = t.item_spacing * s;

    ImGui::PushFont(fonts_role(FontRole::title), t.title_size);
    const ImVec2 named = ImGui::CalcTextSize(title.c_str());
    dl->AddText(ImVec2(at.x, at.y + (height - named.y) / 2.0f), col(t.text), title.c_str());
    ImGui::PopFont();

    // The shoulders' names are plain text until F6's glyphs draw each pad's own.
    ImGui::PushFont(fonts_role(FontRole::tabs), t.tab_size);
    const char* const kLeft = "L";
    const char* const kRight = "R";
    const float pad = t.padding / 2.0f * s;
    std::vector<float> left(pages.size()), wide(pages.size());
    float row = ImGui::CalcTextSize(kLeft).x + gap;
    for (size_t i = 0; i < pages.size(); i++) {
        wide[i] = ImGui::CalcTextSize(pages[i].c_str()).x + 2.0f * pad;
        left[i] = row;
        row += wide[i] + gap;
    }
    row += ImGui::CalcTextSize(kRight).x;
    // Centred in the panel, or as near as the title leaves room for.
    const float x = std::max(at.x + (width - row) / 2.0f, at.x + named.x + 2.0f * gap);
    const float text_y = at.y + (height - ImGui::GetFontSize()) / 2.0f;
    const ImU32 dim = col(t.dim), text = col(t.text);
    const ImGuiStyle& style = ImGui::GetStyle();
    if (!pages.empty()) follow(left[g_page], wide[g_page], now);
    const bool still = g_pill_x.done(now) && g_pill_w.done(now);
    const bool clicked = live && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    for (size_t i = 0; i < pages.size(); i++) {
        const ImVec2 lo(x + left[i], at.y), hi(x + left[i] + wide[i], at.y + height);
        if (!still || (int)i != g_page) dl->AddRectFilled(lo, hi, ImGui::GetColorU32(style.Colors[ImGuiCol_Tab]), height / 2.0f);
        if (clicked && ImGui::IsMouseHoveringRect(lo, hi)) tabs_pick((int)i, false);
    }
    if (!pages.empty()) {
        const float px = x + g_pill_x.at(now);
        dl->AddRectFilled(ImVec2(px, at.y), ImVec2(px + g_pill_w.at(now), at.y + height),
                          ImGui::GetColorU32(style.Colors[ImGuiCol_TabSelected]), height / 2.0f);
    }
    dl->AddText(ImVec2(x, text_y), dim, kLeft);
    for (size_t i = 0; i < pages.size(); i++) dl->AddText(ImVec2(x + left[i] + pad, text_y), text, pages[i].c_str());
    dl->AddText(ImVec2(x + row - ImGui::CalcTextSize(kRight).x, text_y), dim, kRight);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(std::min(width, named.x + 2.0f * gap + row), height));
}

void tabs_focus(bool open) {
    if (g_focus_first && open) {
        ImGui::SetWindowFocus();
        ImGui::NavInitWindow(ImGui::GetCurrentWindow(), true);
        ImGui::GetCurrentContext()->NavInitRequestFromMove = g_moved;
    }
    g_focus_first = false;
}

void tabs_contents_begin() {
    g_list = ImGui::GetWindowDrawList();
    g_begin = g_end = g_list->VtxBuffer.Size;
    g_scale = ImGui::GetStyle().FontScaleMain;
}

void tabs_contents_end() { g_end = g_list ? g_list->VtxBuffer.Size : 0; }

void tabs_no_contents() { g_list = nullptr; }

void tabs_apply(ImDrawData* d, uint64_t now) {
    if (!g_list || !d->CmdLists.contains(g_list)) return;
    const float dx = g_slide.at(now) * g_scale, alpha = std::clamp(g_fade.at(now), 0.0f, 1.0f);
    if (dx == 0.0f && alpha >= 1.0f) return;
    for (int i = g_begin; i < std::min(g_end, g_list->VtxBuffer.Size); i++) {
        ImDrawVert& v = g_list->VtxBuffer[i];
        v.pos.x += dx;
        const ImU32 a = (ImU32)std::lround((float)(v.col >> IM_COL32_A_SHIFT & 0xFF) * alpha);
        v.col = (v.col & ~IM_COL32_A_MASK) | a << IM_COL32_A_SHIFT;
    }
}
