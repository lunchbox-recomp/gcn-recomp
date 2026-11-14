// The overlay's theme checks: the `test_overlay_theme` target. Runs with no window, GL, disc or
// game, on a Dear ImGui context of its own.
//
// The default theme (runtime/overlay/theme.h) must be readable from a couch over any game:
// every text colour of the style built from it at least 4.5:1 against every fill text sits on,
// a question's confirm's included, and the focus ring and its pulse, an item's edge, a check
// mark and a slider's grab at least 3:1 against what's around them, by WCAG 2's contrast ratio.
// The panel is translucent, so it's taken over the backdrop over black and over white,
// whichever is worse. At 1080 lines the body text is at least 22 px and nothing to press is
// under 48 px. Each kind of text has its size and, on macOS, its chain of the Mac's own fonts.
// The style built from it
// (runtime/overlay/theme.cpp) carries its colours, has its sizes at 1080 lines, and is built
// afresh at another height rather than scaled again, so a height and back gives the same sizes.
//
// usage: test_overlay_theme
// Exits 0 only if every check passes.
#include "overlay/theme.h"
#include "overlay/glyph.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "imgui.h"

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

// A style colour as 0xRRGGBBAA.
uint32_t packed(const ImVec4& c) {
    const auto byte = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    return byte(c.x) << 24 | byte(c.y) << 16 | byte(c.z) << 8 | byte(c.w);
}

// Every colour slot Dear ImGui names as text: Text, TextDisabled and TextLink, not the
// background TextSelectedBg.
std::vector<ImGuiCol> text_slots() {
    std::vector<ImGuiCol> slots;
    for (ImGuiCol c = 0; c < ImGuiCol_COUNT; c++) {
        const char* name = ImGui::GetStyleColorName(c);
        if (!strncmp(name, "Text", 4) && !strstr(name, "Bg")) slots.push_back(c);
    }
    return slots;
}

// Each text slot of the style built from `t`, over each fill of the style that text sits on:
// the panel, and an item at rest, focused and pressed. The selected tab's fill is under its
// label alone, which is plain text.
void contrasts(const OverlayTheme& t) {
    ImGui::CreateContext();
    theme_style(t, 1.0f);
    const ImVec4* c = ImGui::GetStyle().Colors;
    const ImGuiCol kFills[] = {ImGuiCol_WindowBg, ImGuiCol_PopupBg, ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered,
                               ImGuiCol_FrameBgActive, ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive,
                               ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive, ImGuiCol_Tab,
                               ImGuiCol_TabHovered, ImGuiCol_TabDimmed};
    const std::vector<ImGuiCol> texts = text_slots();
    check(texts.size() == 3, "the style has three text slots: Text, TextDisabled and TextLink");
    for (ImGuiCol text : texts) {
        for (ImGuiCol fill : kFills) {
            // The panel's own colour is under it as the fill, so it's taken as no fill on it.
            const uint32_t on = fill == ImGuiCol_WindowBg ? 0 : packed(c[fill]);
            const double r = worst(t, packed(c[text]), on);
            char what[160];
            snprintf(what, sizeof(what), "%s on %s is %.2f:1, at least 4.5:1", ImGui::GetStyleColorName(text),
                     ImGui::GetStyleColorName(fill), r);
            check(r >= 4.5, what);
        }
    }
    for (ImGuiCol tab : {ImGuiCol_TabSelected, ImGuiCol_TabDimmedSelected}) {
        const double r = worst(t, packed(c[ImGuiCol_Text]), packed(c[tab]));
        char what[160];
        snprintf(what, sizeof(what), "Text on %s is %.2f:1, at least 4.5:1", ImGui::GetStyleColorName(tab), r);
        check(r >= 4.5, what);
    }
    ImGui::DestroyContext();
    // A question's confirm (runtime/overlay/footer.cpp): the panel's colour, whole, on danger.
    const double confirm = worst(t, t.panel | 0xFF, t.danger);
    char said[160];
    snprintf(said, sizeof(said), "the confirm's text, the panel's colour, on danger is %.2f:1, at least 4.5:1", confirm);
    check(confirm >= 4.5, said);
    for (const auto& ring : {std::pair<const char*, uint32_t>{"the focus ring", t.accent}, {"its pulse", t.accent_light}}) {
        const double r = worst(t, ring.second, 0);
        char what[160];
        snprintf(what, sizeof(what), "%s on the panel is %.2f:1, at least 3:1", ring.first, r);
        check(r >= 3.0, what);
    }
}

void glyph_contrasts(const OverlayTheme& t) {
    const struct { PadFamily family; int face; uint32_t fill; const char* label; } cases[] = {
        {PadFamily::xbox, 1, t.xbox.a, "A"}, {PadFamily::xbox, 2, t.xbox.b, "B"},
        {PadFamily::xbox, 3, t.xbox.x, "X"}, {PadFamily::xbox, 4, t.xbox.y, "Y"},
        {PadFamily::ps5, 5, t.playstation.a, "Cross"}, {PadFamily::ps5, 6, t.playstation.b, "Circle"},
        {PadFamily::ps5, 7, t.playstation.x, "Square"}, {PadFamily::ps5, 8, t.playstation.y, "Triangle"},
        {PadFamily::nintendo, 1, t.item, "A"}, {PadFamily::other, 1, t.item, "A"},
        {PadFamily::gamecube, 1, t.gamecube.a, "A"}, {PadFamily::gamecube, 2, t.gamecube.b, "B"},
        {PadFamily::gamecube, 3, t.gamecube.x, "X"}, {PadFamily::gamecube, 4, t.gamecube.y, "Y"},
    };
    for (const auto& c : cases) {
        const OverlayGlyph g = overlay_pad_glyph(t, c.family, {false, kPadSouth}, c.face);
        char what[160];
        snprintf(what, sizeof(what), "%s face label and colour", c.label);
        check(g.label == c.label && g.fill == c.fill && g.shape == OverlayGlyphShape::circle, what);
        snprintf(what, sizeof(what), "%s glyph ink has 4.5:1 contrast", c.label);
        check(contrast(g.ink, g.fill) >= 4.5, what);
        if (c.family == PadFamily::nintendo || c.family == PadFamily::other)
            check(g.ink == t.text, "Nintendo and unknown faces use the theme's text on its item colour");
    }
    const OverlayGlyph z = overlay_pad_glyph(t, PadFamily::gamecube, {false, kPadRightShoulder}, 0);
    check(z.label == "Z" && z.fill == t.gamecube_z && z.shape == OverlayGlyphShape::pill && contrast(z.ink, z.fill) >= 4.5,
          "GameCube Z uses the violet shoulder pill with readable ink");
    const OverlayGlyph key = overlay_key_glyph(t, "Esc");
    check(key.label == "Esc" && key.shape == OverlayGlyphShape::keycap && contrast(key.ink, key.fill) >= 4.5,
          "Escape has a readable keycap");
}

// What isn't text but has to be seen, at least 3:1 by WCAG 2.1's 1.4.11, read from the style
// built from `t`: an item's edge on the panel, which is all an unchecked box, a radio button
// not picked or a slider's track shows of itself there, at every height; a check mark on its
// box; and a slider's grab on its track, at rest, focused and held.
void edges(const OverlayTheme& t) {
    ImGui::CreateContext();
    const ImGuiStyle& s = ImGui::GetStyle();
    bool drawn = true;
    for (int height : {480, 720, 1080, 2160}) {
        theme_style(t, theme_scale(height));
        drawn = drawn && s.FrameBorderSize >= 1.0f;
    }
    check(drawn, "an item has an edge at least 1 px wide at 480, 720, 1080 and 2160 lines");
    const ImVec4* c = s.Colors;
    const struct {
        ImGuiCol mark, on;
    } kMarks[] = {{ImGuiCol_Border, ImGuiCol_WindowBg},          {ImGuiCol_CheckMark, ImGuiCol_CheckboxSelectedBg},
                  {ImGuiCol_CheckMark, ImGuiCol_FrameBg},        {ImGuiCol_CheckMark, ImGuiCol_FrameBgHovered},
                  {ImGuiCol_CheckMark, ImGuiCol_FrameBgActive},  {ImGuiCol_SliderGrab, ImGuiCol_FrameBg},
                  {ImGuiCol_SliderGrab, ImGuiCol_FrameBgHovered}, {ImGuiCol_SliderGrabActive, ImGuiCol_FrameBgActive}};
    for (const auto& m : kMarks) {
        const uint32_t on = m.on == ImGuiCol_WindowBg ? 0 : packed(c[m.on]);
        const double r = worst(t, packed(c[m.mark]), on);
        char what[160];
        snprintf(what, sizeof(what), "%s on %s is %.2f:1, at least 3:1", ImGui::GetStyleColorName(m.mark),
                 ImGui::GetStyleColorName(m.on), r);
        check(r >= 3.0, what);
    }
    ImGui::DestroyContext();
}

// Whether `a` and `b` have exactly the same sizes, of those the theme sets and some it doesn't.
bool same_sizes(const ImGuiStyle& a, const ImGuiStyle& b) {
    const auto eq = [](ImVec2 x, ImVec2 y) { return x.x == y.x && x.y == y.y; };
    return eq(a.WindowPadding, b.WindowPadding) && eq(a.FramePadding, b.FramePadding) &&
           eq(a.ItemSpacing, b.ItemSpacing) && eq(a.ItemInnerSpacing, b.ItemInnerSpacing) &&
           a.WindowRounding == b.WindowRounding && a.FrameRounding == b.FrameRounding &&
           a.WindowBorderSize == b.WindowBorderSize && a.FrameBorderSize == b.FrameBorderSize &&
           a.IndentSpacing == b.IndentSpacing && a.ScrollbarSize == b.ScrollbarSize && a.GrabMinSize == b.GrabMinSize &&
           a.FontScaleMain == b.FontScaleMain;
}

void couch(const OverlayTheme& t) {
    check(t.body_size >= 22.0f, "body text is at least 22 px at 1080 lines");
    check(t.item_min_height >= 48.0f && t.item_height >= t.item_min_height,
          "nothing to press is under 48 px at 1080 lines, items included");
}

// Each kind of text has the design's size at 1080 lines. On macOS its chain is the Mac's own Avenir
// Next, then Helvetica Neue, then Helvetica, read from /System/Library/Fonts where they're
// installed; elsewhere it's empty, for Dear ImGui's own font, until a platform lists its own.
void fonts(const OverlayTheme& t) {
    check(t.title_size == 36.0f && t.tab_size == 26.0f && t.item_text_size == 24.0f && t.body_size == 22.0f &&
              t.footer_size == 18.0f,
          "the title, tabs, items, body and footer are 36, 26, 24, 22 and 18 px at 1080 lines");
    const struct {
        const char* role;
        const std::vector<OverlayFace>& chain;
        const char *avenir, *neue, *helvetica;
    } roles[] = {{"title", t.fonts.title, "Avenir Next Heavy", "Helvetica Neue Bold", "Helvetica Bold"},
                 {"tabs", t.fonts.tabs, "Avenir Next Demi Bold", "Helvetica Neue Medium", "Helvetica Bold"},
                 {"items", t.fonts.items, "Avenir Next Medium", "Helvetica Neue Medium", "Helvetica"},
                 {"body", t.fonts.body, "Avenir Next Medium", "Helvetica Neue", "Helvetica"},
                 {"footer", t.fonts.footer, "Avenir Next Demi Bold", "Helvetica Neue Medium", "Helvetica Bold"}};
    for (const auto& r : roles) {
        char what[160];
#ifdef __APPLE__
        const bool ok = r.chain.size() == 3 && r.chain[0].path == "/System/Library/Fonts/Avenir Next.ttc" &&
                        r.chain[0].name == r.avenir && r.chain[1].path == "/System/Library/Fonts/HelveticaNeue.ttc" &&
                        r.chain[1].name == r.neue && r.chain[2].path == "/System/Library/Fonts/Helvetica.ttc" &&
                        r.chain[2].name == r.helvetica;
        snprintf(what, sizeof(what), "on macOS the text of the %s takes %s, then %s, then %s", r.role, r.avenir, r.neue,
                 r.helvetica);
#else
        const bool ok = r.chain.empty();
        snprintf(what, sizeof(what), "off macOS the text of the %s lists no font, for Dear ImGui's own", r.role);
#endif
        check(ok, what);
    }
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
              same(s.Colors[ImGuiCol_TextDisabled], t.dim) && same(s.Colors[ImGuiCol_Border], t.border) &&
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
    // Fitted from 1080 lines to 1000 and back, as a window resized and resized back, each size
    // is what a build at that height has, not one rounded again from the last.
    theme_style(t, theme_scale(1000));
    const ImGuiStyle at1000 = s;
    theme_style(t, theme_scale(1080));
    const ImGuiStyle at1080 = s;
    theme_fit(1000);
    const bool down = same_sizes(s, at1000);
    theme_fit(1080);
    check(down && same_sizes(s, at1080), "fitted to 1000 lines and back to 1080, the sizes are a fresh build's at each");
    check(near(theme_scale(720), 720.0f / 1080.0f) && near(theme_scale(0), 1.0f),
          "the scale is the window's height over 1080, and 1 with no height");
    OverlayTheme pink = t;
    pink.accent = 0xFF6FB5FF;
    theme_style(pink, 1.0f);
    check(same(s.Colors[ImGuiCol_CheckMark], pink.accent), "a theme's own accent reaches the style");
    // An edge stays a pixel wide in a small window, and a theme with none has none there.
    theme_style(t, theme_scale(540));
    const bool pixel = near(s.FrameBorderSize, 1.0f) && near(s.WindowBorderSize, 1.0f);
    OverlayTheme bare = t;
    bare.border_size = 0.0f;
    theme_style(bare, theme_scale(540));
    check(pixel && s.WindowBorderSize == 0.0f && s.PopupBorderSize == 0.0f && s.FrameBorderSize == 0.0f,
          "at 540 lines an edge is still a pixel wide, and a theme with no edge has none");
    ImGui::DestroyContext();
}

}  // namespace

int main() {
    const OverlayTheme t = overlay_theme_default();
    contrasts(t);
    glyph_contrasts(t);
    edges(t);
    couch(t);
    fonts(t);
    style(t);
    printf("test_overlay_theme: %s\n", g_failed ? "FAILED" : "passed");
    return g_failed ? 1 : 0;
}
