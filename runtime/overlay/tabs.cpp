// The menu's header and the page it shows (tabs.h).
#include "overlay/tabs.h"
#include "overlay/fonts.h"
#include "overlay/theme.h"
#include <algorithm>
#include <cstdint>
#include "imgui.h"

namespace {

int g_page;  // the page showing, 0 for Display

ImU32 col(uint32_t c) { return IM_COL32(c >> 24, c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF); }

}  // namespace

void tabs_reset() { g_page = 0; }

int tabs_page() { return g_page; }

void tabs_show(int page) { g_page = page; }

void tabs_header(const std::string& title, const std::vector<std::string>& pages, bool live) {
    const OverlayTheme& t = theme_now();
    const float s = ImGui::GetStyle().FontScaleMain;  // the theme's scale at this window's height
    const int last = (int)pages.size() - 1;
    g_page = std::clamp(g_page, 0, std::max(last, 0));
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
        if (clicked && ImGui::IsMouseHoveringRect(lo, hi)) g_page = (int)i;
        x += wide[i] + gap;
    }
    dl->AddText(ImVec2(x, text_y), dim, kRight);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(std::min(width, named.x + 2.0f * gap + row), height));
}
