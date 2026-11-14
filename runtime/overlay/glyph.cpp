#include "overlay/glyph.h"
#include "overlay/theme.h"
#include <algorithm>
#include <cmath>

namespace {
double luminance(uint32_t rgba) {
    double sum = 0;
    for (int i = 0; i < 3; i++) {
        double c = ((rgba >> (24 - 8 * i)) & 255) / 255.0;
        c = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        sum += c * (i == 0 ? 0.2126 : i == 1 ? 0.7152 : 0.0722);
    }
    return sum;
}
}

uint32_t overlay_glyph_ink(uint32_t fill) {
    return luminance(fill) > 0.179 ? 0x000000FF : 0xFFFFFFFF;
}

OverlayGlyph overlay_pad_glyph(const OverlayTheme& t, PadFamily f, const PadSource& s, int face_label) {
    const std::string label = pad_glyph(f, s, face_label);
    const bool face = !s.axis && s.index >= kPadSouth && s.index <= kPadNorth;
    uint32_t fill = t.item;
    if (face) {
        const OverlayTheme::Faces* colours = nullptr;
        if (f == PadFamily::xbox || f == PadFamily::xbox360) colours = &t.xbox;
        if (f == PadFamily::ps3 || f == PadFamily::ps4 || f == PadFamily::ps5) colours = &t.playstation;
        if (f == PadFamily::gamecube) colours = &t.gamecube;
        if (colours) {
            const uint32_t entries[] = {colours->a, colours->b, colours->x, colours->y};
            int n = s.index;
            if (face_label == 1 || face_label == 5) n = 0;
            else if (face_label == 2 || face_label == 6) n = 1;
            else if (face_label == 3 || face_label == 7) n = 2;
            else if (face_label == 4 || face_label == 8) n = 3;
            fill = entries[n];
        }
    } else if (f == PadFamily::gamecube && !s.axis && s.index == kPadRightShoulder) fill = t.gamecube_z;
    const bool mono = f == PadFamily::nintendo || f == PadFamily::joycon_left || f == PadFamily::joycon_right ||
                      f == PadFamily::joycon_left_upright || f == PadFamily::joycon_right_upright || f == PadFamily::other;
    return {label, fill, mono ? t.text : overlay_glyph_ink(fill), face ? OverlayGlyphShape::circle : OverlayGlyphShape::pill};
}

OverlayGlyph overlay_key_glyph(const OverlayTheme& t, const char* label) {
    return {label, t.item, overlay_glyph_ink(t.item), OverlayGlyphShape::keycap};
}

float overlay_glyph_width(const OverlayGlyph& g, float height) {
    const float text = ImGui::CalcTextSize(g.label.c_str()).x;
    return g.shape == OverlayGlyphShape::circle ? std::max(height, text + height * 0.38f)
                                                 : text + height * 0.8f;
}

void overlay_draw_glyph(ImDrawList* dl, ImVec2 at, float height, const OverlayGlyph& g) {
    const float width = overlay_glyph_width(g, height);
    const ImVec2 end(at.x + width, at.y + height);
    const float radius = g.shape == OverlayGlyphShape::circle ? height * 0.5f
                         : g.shape == OverlayGlyphShape::pill ? height * 0.42f : height * 0.18f;
    if (g.shape == OverlayGlyphShape::circle)
        dl->AddCircleFilled(ImVec2(at.x + width * 0.5f, at.y + height * 0.5f), width * 0.5f, theme_colour(g.fill));
    else dl->AddRectFilled(at, end, theme_colour(g.fill), radius);
    if (g.shape == OverlayGlyphShape::keycap) dl->AddRect(at, end, theme_colour(g.ink), radius);
    const ImVec2 size = ImGui::CalcTextSize(g.label.c_str());
    dl->AddText(ImVec2(at.x + (width - size.x) * 0.5f, at.y + (height - size.y) * 0.5f), theme_colour(g.ink),
                g.label.c_str());
}
