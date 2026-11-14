// The overlay's theme checks: the `test_overlay_theme` target. Runs with no window, GL, disc or
// game, on a Dear ImGui context of its own.
//
// The default theme (runtime/overlay/theme.h) must be readable from a couch over any game:
// text and dimmed text at least 4.5:1 against every fill they sit on, and the focus ring and its
// pulse at least 3:1 against the panel, by WCAG 2's contrast ratio. The panel is translucent, so
// it's taken over the backdrop over black and over white, whichever is worse. At 1080 lines the
// body text is at least 22 px and nothing to press is under 48 px. The style built from it
// (runtime/overlay/theme.cpp) carries its colours, has its sizes at 1080 lines, and is built
// afresh at another height rather than scaled again.
//
// usage: test_overlay_theme
// Exits 0 only if every check passes.
#include "overlay/theme.h"
#include <cmath>
#include <cstdio>
#include "imgui.h"

void theme_style(const OverlayTheme& theme, float scale);
float theme_scale(int height);

namespace {

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

float channel(uint32_t rgba, int shift) { return (float)(rgba >> shift & 0xFF) / 255.0f; }

// `fg` over `bg`, which is opaque, by `fg`'s alpha.
uint32_t over(uint32_t fg, uint32_t bg) {
    const float a = channel(fg, 0);
    uint32_t out = 0xFF;
    for (int shift = 8; shift <= 24; shift += 8) {
        const float c = channel(fg, shift) * a + channel(bg, shift) * (1.0f - a);
        out |= (uint32_t)std::lround(c * 255.0f) << shift;
    }
    return out;
}

// WCAG 2's relative luminance of an opaque colour.
double luminance(uint32_t rgba) {
    double l = 0.0;
    const double weight[3] = {0.2126, 0.7152, 0.0722};
    for (int i = 0; i < 3; i++) {
        const double c = channel(rgba, 24 - 8 * i);
        l += weight[i] * (c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
    }
    return l;
}

double contrast(uint32_t a, uint32_t b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// The least contrast `fg` has over `fill` on the panel, the panel over the backdrop over black
// or white.
double worst(const OverlayTheme& t, uint32_t fg, uint32_t fill) {
    double least = 1e9;
    for (uint32_t game : {0x000000FFu, 0xFFFFFFFFu}) {
        const uint32_t seen = over(fill, over(t.panel, over(t.backdrop, game)));
        least = std::min(least, contrast(over(fg, seen), seen));
    }
    return least;
}

bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

bool same(const ImVec4& c, uint32_t rgba) {
    return near(c.x, channel(rgba, 24)) && near(c.y, channel(rgba, 16)) && near(c.z, channel(rgba, 8)) &&
           near(c.w, channel(rgba, 0));
}

void contrasts(const OverlayTheme& t) {
    // The selected tab's label is text, never dimmed.
    const struct {
        const char* name;
        uint32_t fill;
        bool dimmed;
    } kFills[] = {{"the panel", 0, true}, {"an item", t.item, true}, {"a focused item", t.focused, true},
                  {"a pressed item", t.pressed, true}, {"the selected tab", t.accent_dark, false}};
    const struct {
        const char* name;
        uint32_t colour;
        bool dimmed;
    } kTexts[] = {{"text", t.text, false}, {"dimmed text", t.dim, true}};
    for (const auto& text : kTexts) {
        for (const auto& fill : kFills) {
            if (text.dimmed && !fill.dimmed) continue;
            const double r = worst(t, text.colour, fill.fill);
            char what[160];
            snprintf(what, sizeof(what), "%s on %s is %.2f:1, at least 4.5:1", text.name, fill.name, r);
            check(r >= 4.5, what);
        }
    }
    for (const auto& ring : {std::pair<const char*, uint32_t>{"the focus ring", t.accent}, {"its pulse", t.accent_light}}) {
        const double r = worst(t, ring.second, 0);
        char what[160];
        snprintf(what, sizeof(what), "%s on the panel is %.2f:1, at least 3:1", ring.first, r);
        check(r >= 3.0, what);
    }
}

void couch(const OverlayTheme& t) {
    check(t.body_size >= 22.0f, "body text is at least 22 px at 1080 lines");
    check(t.item_min_height >= 48.0f && t.item_height >= t.item_min_height,
          "nothing to press is under 48 px at 1080 lines, items included");
    check(t.fonts.empty(), "the default theme names no font, so Dear ImGui's own is used");
}

void style(const OverlayTheme& t) {
    ImGui::CreateContext();
    ImGuiStyle& s = ImGui::GetStyle();
    theme_style(t, theme_scale(1080));
    const float indent = s.IndentSpacing;  // Dear ImGui's own, which the theme doesn't name
    const float frame = s.FontSizeBase * s.FontScaleMain * s.FontScaleDpi + 2.0f * s.FramePadding.y;
    check(near(s.FontSizeBase * s.FontScaleMain * s.FontScaleDpi, t.body_size) && near(frame, t.item_height),
          "at 1080 lines the text is the body size, and a button or a box is an item's height");
    check(near(s.WindowPadding.x, t.padding) && near(s.WindowRounding, t.panel_radius) &&
              near(s.FrameRounding, t.item_radius) && near(s.ItemSpacing.y, t.item_spacing) &&
              near(s.WindowBorderSize, t.border_size),
          "at 1080 lines the panel's padding and corners and the items' corners and spacing are the theme's");
    check(same(s.Colors[ImGuiCol_WindowBg], t.panel) && same(s.Colors[ImGuiCol_Text], t.text) &&
              same(s.Colors[ImGuiCol_Button], t.item) && same(s.Colors[ImGuiCol_ButtonHovered], t.focused) &&
              same(s.Colors[ImGuiCol_ButtonActive], t.pressed) && same(s.Colors[ImGuiCol_TabSelected], t.accent_dark) &&
              same(s.Colors[ImGuiCol_CheckMark], t.accent) && same(s.Colors[ImGuiCol_NavCursor], t.accent) &&
              same(s.Colors[ImGuiCol_TextDisabled], t.disabled) && same(s.Colors[ImGuiCol_Border], t.border) &&
              same(s.Colors[ImGuiCol_Separator], t.divider),
          "the style's colours are the theme's");
    // At 2160 lines, twice over, every size is twice its 1080 one, not four times.
    theme_style(t, theme_scale(2160));
    theme_style(t, theme_scale(2160));
    check(near(s.FontSizeBase * s.FontScaleMain * s.FontScaleDpi, 2.0f * t.body_size) &&
              near(s.WindowPadding.x, 2.0f * t.padding) && near(s.WindowRounding, 2.0f * t.panel_radius) &&
              near(s.FrameRounding, 2.0f * t.item_radius) && near(s.IndentSpacing, 2.0f * indent),
          "built again at 2160 lines, twice, the sizes are twice the 1080 ones, from Dear ImGui's own style each time");
    check(near(s.FontSizeBase, t.body_size), "the base font size stays the theme's at 1080 lines, for a page's own sizes");
    check(near(theme_scale(720), 720.0f / 1080.0f) && near(theme_scale(0), 1.0f),
          "the scale is the window's height over 1080, and 1 with no height");
    OverlayTheme pink = t;
    pink.accent = 0xFF6FB5FF;
    theme_style(pink, 1.0f);
    check(same(s.Colors[ImGuiCol_CheckMark], pink.accent), "a theme's own accent reaches the style");
    ImGui::DestroyContext();
}

}  // namespace

int main() {
    const OverlayTheme t = overlay_theme_default();
    contrasts(t);
    couch(t);
    style(t);
    printf("test_overlay_theme: %s\n", g_failed ? "FAILED" : "passed");
    return g_failed ? 1 : 0;
}
