// The overlay's style built from its theme (theme.h). Each build starts from Dear ImGui's own
// style, gives it the theme's colours and its sizes at 1080 lines, then scales those to the
// window, as Dolphin rebuilds its style on a scale change: scaling the last one again would
// round each size again each time.
#include "overlay/theme.h"
#include "imgui.h"

namespace {

OverlayTheme g_theme;

ImVec4 rgba(uint32_t c, float alpha = 1.0f) {
    return ImVec4((float)(c >> 24 & 0xFF) / 255.0f, (float)(c >> 16 & 0xFF) / 255.0f, (float)(c >> 8 & 0xFF) / 255.0f,
                  (float)(c & 0xFF) / 255.0f * alpha);
}

void colours(const OverlayTheme& t, ImVec4* c) {
    const ImVec4 none(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_Text] = rgba(t.text);
    c[ImGuiCol_TextDisabled] = rgba(t.dim);  // a note, as TextDisabled draws one
    c[ImGuiCol_WindowBg] = c[ImGuiCol_PopupBg] = rgba(t.panel);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_MenuBarBg] = rgba(t.panel);
    c[ImGuiCol_ChildBg] = c[ImGuiCol_BorderShadow] = c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ResizeGrip] = c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_ResizeGripActive] = none;
    c[ImGuiCol_Border] = rgba(t.border);
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = rgba(t.divider);
    c[ImGuiCol_FrameBg] = c[ImGuiCol_Button] = c[ImGuiCol_Header] = c[ImGuiCol_ScrollbarGrab] = rgba(t.item);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = c[ImGuiCol_CheckboxSelectedBg] = rgba(t.item);
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered] = rgba(t.focused);
    c[ImGuiCol_ScrollbarGrabHovered] = c[ImGuiCol_TabHovered] = rgba(t.focused);
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = rgba(t.pressed);
    c[ImGuiCol_ScrollbarGrabActive] = rgba(t.pressed);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = rgba(t.accent_dark);
    c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_TabDimmedSelectedOverline] = rgba(t.accent);
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = c[ImGuiCol_NavCursor] = rgba(t.accent);
    c[ImGuiCol_TextLink] = c[ImGuiCol_DragDropTarget] = c[ImGuiCol_NavWindowingHighlight] = rgba(t.accent);
    c[ImGuiCol_SliderGrabActive] = rgba(t.accent_light);
    c[ImGuiCol_TextSelectedBg] = rgba(t.accent, 0.35f);
    c[ImGuiCol_InputTextCursor] = rgba(t.text);
    c[ImGuiCol_NavWindowingDimBg] = c[ImGuiCol_ModalWindowDimBg] = rgba(t.backdrop);
}

}  // namespace

const OverlayTheme& theme_now() { return g_theme; }

void theme_set(const OverlayTheme& theme) { g_theme = theme; }

float theme_scale(int height) { return height > 0 ? (float)height / 1080.0f : 1.0f; }

void theme_style(const OverlayTheme& t, float scale) {
    ImGuiStyle s;
    colours(t, s.Colors);
    s.WindowPadding = ImVec2(t.padding, t.padding);
    s.WindowRounding = t.panel_radius;
    s.WindowBorderSize = s.PopupBorderSize = t.border_size;
    s.PopupRounding = s.FrameRounding = s.GrabRounding = s.TabRounding = t.item_radius;
    // A button or a box is an item's height with the body text in it.
    s.FramePadding = ImVec2(t.padding / 2.0f, (t.item_height - t.body_size) / 2.0f);
    s.ItemSpacing = s.ItemInnerSpacing = ImVec2(t.item_spacing, t.item_spacing);
    s.ScaleAllSizes(scale);
    // The base size stays at 1080 lines, so a page's own sizes are in the theme's terms too.
    s.FontSizeBase = t.body_size;
    s.FontScaleMain = scale;
    s.FontScaleDpi = 1.0f;
    ImGui::GetStyle() = s;
}

void theme_fit(int height) {
    const float scale = theme_scale(height);
    if (ImGui::GetStyle().FontScaleMain != scale) theme_style(g_theme, scale);
}
