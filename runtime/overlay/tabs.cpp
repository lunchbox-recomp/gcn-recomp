// The menu's header and the page it shows (tabs.h).
#include "overlay/tabs.h"
#include "overlay/fonts.h"
#include "overlay/theme.h"
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include "imgui.h"
#include "imgui_internal.h"  // ImGui::NavInitWindow

namespace {

int g_page;          // the page showing, 0 for Display
bool g_focus_first;  // the page's first item takes the focus at its next draw, after a change
bool g_moved;        // and that change was a shoulder's or a key's, which shows the focus as a move does

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

}  // namespace

void tabs_reset() { g_page = 0; }

int tabs_page() { return g_page; }

void tabs_show(int page) { g_page = page; }

void tabs_pick(int page, bool moved) {
    if (page == g_page) return;
    g_page = page;
    g_focus_first = true;
    g_moved = moved;
}

void tabs_focus(bool open) {
    if (g_focus_first && open) {
        ImGui::SetWindowFocus();
        ImGui::NavInitWindow(ImGui::GetCurrentWindow(), true);
        ImGui::GetCurrentContext()->NavInitRequestFromMove = g_moved;
    }
    g_focus_first = false;
}

void tabs_header(const std::string& title, const std::vector<std::string>& pages, bool live) {
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
    std::vector<float> wide(pages.size());
    float row = ImGui::CalcTextSize(kLeft).x + ImGui::CalcTextSize(kRight).x + 2.0f * gap;
    for (size_t i = 0; i < pages.size(); i++) {
        wide[i] = ImGui::CalcTextSize(pages[i].c_str()).x + 2.0f * pad;
        row += wide[i] + (i ? gap : 0.0f);
    }
    // Centred in the panel, or as near as the title leaves room for.
    float x = std::max(at.x + (width - row) / 2.0f, at.x + named.x + 2.0f * gap);
    const float text_y = at.y + (height - ImGui::GetFontSize()) / 2.0f;
    const ImU32 dim = col(t.dim), text = col(t.text);
    dl->AddText(ImVec2(x, text_y), dim, kLeft);
    x += ImGui::CalcTextSize(kLeft).x + gap;
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool clicked = live && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    for (size_t i = 0; i < pages.size(); i++) {
        const ImVec2 lo(x, at.y), hi(x + wide[i], at.y + height);
        const bool showing = (int)i == g_page;
        dl->AddRectFilled(lo, hi, ImGui::GetColorU32(showing ? style.Colors[ImGuiCol_TabSelected] : style.Colors[ImGuiCol_Tab]),
                          height / 2.0f);
        dl->AddText(ImVec2(x + pad, text_y), text, pages[i].c_str());
        if (clicked && ImGui::IsMouseHoveringRect(lo, hi)) tabs_pick((int)i, false);
        x += wide[i] + gap;
    }
    dl->AddText(ImVec2(x, text_y), dim, kRight);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(std::min(width, named.x + 2.0f * gap + row), height));
}
