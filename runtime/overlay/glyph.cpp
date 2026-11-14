// The menu's button and key glyphs (glyph.h).
#include "overlay/glyph.h"
#include "overlay/theme.h"
#include <algorithm>
#include <cmath>

namespace {

// WCAG 2's relative luminance of an opaque 0xRRGGBBAA colour.
double luminance(uint32_t rgba) {
    double sum = 0;
    for (int i = 0; i < 3; i++) {
        double c = ((rgba >> (24 - 8 * i)) & 255) / 255.0;
        c = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        sum += c * (i == 0 ? 0.2126 : i == 1 ? 0.7152 : 0.0722);
    }
    return sum;
}

// Black or white, whichever has more contrast on `fill`.
uint32_t ink_on(uint32_t fill) { return luminance(fill) > 0.179 ? 0x000000FF : 0xFFFFFFFF; }

// Each by the glyph's height: how wide a label a disc takes, a pill's and a keycap's corners, and
// the room either side of a label on either.
constexpr float kDiscLabel = 0.62f, kPillRadius = 0.42f, kKeyRadius = 0.18f, kSides = 0.4f;

// Whether `g` is drawn as a disc `height` across: a face with a mark, or a label that fits it.
bool disc(const OverlayGlyph& g, float height) {
    return g.shape == OverlayGlyphShape::circle &&
           (g.mark != OverlayGlyphMark::none || ImGui::CalcTextSize(g.label.c_str()).x <= height * kDiscLabel);
}

// `g`'s mark in its ink, on its disc of radius `r` about `c`.
void draw_mark(ImDrawList* dl, ImVec2 c, float r, const OverlayGlyph& g) {
    const ImU32 ink = theme_colour(g.ink);
    const float w = std::max(1.0f, r * 0.16f);
    const float a = r * 0.42f;
    switch (g.mark) {
    case OverlayGlyphMark::cross:
        dl->AddLine(ImVec2(c.x - a, c.y - a), ImVec2(c.x + a, c.y + a), ink, w);
        dl->AddLine(ImVec2(c.x - a, c.y + a), ImVec2(c.x + a, c.y - a), ink, w);
        break;
    case OverlayGlyphMark::circle: dl->AddCircle(c, r * 0.48f, ink, 0, w); break;
    case OverlayGlyphMark::square: dl->AddRect(ImVec2(c.x - a, c.y - a), ImVec2(c.x + a, c.y + a), ink, 0.0f, 0, w); break;
    case OverlayGlyphMark::triangle: {
        // Its corners `t` from a point a quarter of that below the disc's centre, which centres it
        // top to bottom.
        const float t = r * 0.58f;
        const ImVec2 m(c.x, c.y + t * 0.25f);
        dl->AddTriangle(ImVec2(m.x, m.y - t), ImVec2(m.x + t * 0.866f, m.y + t * 0.5f),
                        ImVec2(m.x - t * 0.866f, m.y + t * 0.5f), ink, w);
        break;
    }
    case OverlayGlyphMark::none: break;
    }
}

}  // namespace

OverlayGlyph overlay_pad_glyph(const OverlayTheme& t, PadFamily f, const PadSource& s, int face_label) {
    const bool face = !s.axis && s.index >= kPadSouth && s.index <= kPadNorth;
    const OverlayTheme::Faces* colours = nullptr;
    if (f == PadFamily::xbox || f == PadFamily::xbox360) colours = &t.xbox;
    if (f == PadFamily::ps3 || f == PadFamily::ps4 || f == PadFamily::ps5) colours = &t.playstation;
    if (f == PadFamily::gamecube) colours = &t.gamecube;
    OverlayGlyph g{pad_glyph(f, s, face_label), t.item, t.text, t.border,
                   face ? OverlayGlyphShape::circle : OverlayGlyphShape::pill, OverlayGlyphMark::none};
    // A face takes its colour by the label printed on it, A or Cross first (SDL's labels 1 to 8),
    // or with none by its place, south first.
    bool coloured = false;
    if (face && colours) {
        const uint32_t entries[] = {colours->a, colours->b, colours->x, colours->y};
        g.fill = entries[face_label >= 1 && face_label <= 8 ? (face_label - 1) % 4 : s.index];
        coloured = true;
    } else if (f == PadFamily::gamecube && !s.axis && s.index == kPadRightShoulder) {
        g.fill = t.gamecube_z;
        coloured = true;
    }
    // One in a colour has black or white on it and no edge; one on the item's colour has the
    // text's and the item's edge, as a button has.
    if (coloured) {
        g.ink = ink_on(g.fill);
        g.edge = 0;
    }
    if (face && face_label >= 5 && face_label <= 8) g.mark = (OverlayGlyphMark)(face_label - 4);
    return g;
}

OverlayGlyph overlay_key_glyph(const OverlayTheme& t, const char* label) {
    return {label, t.item, t.text, t.text, OverlayGlyphShape::keycap, OverlayGlyphMark::none};
}

float overlay_glyph_width(const OverlayGlyph& g, float height) {
    return disc(g, height) ? height : ImGui::CalcTextSize(g.label.c_str()).x + 2.0f * kSides * height;
}

void overlay_draw_glyph(ImDrawList* dl, ImVec2 at, float height, const OverlayGlyph& g) {
    const float width = overlay_glyph_width(g, height);
    const ImVec2 end(at.x + width, at.y + height), centre(at.x + width * 0.5f, at.y + height * 0.5f);
    const float edge = std::max(1.0f, ImGui::GetStyle().FrameBorderSize);
    if (disc(g, height)) {
        dl->AddCircleFilled(centre, height * 0.5f, theme_colour(g.fill));
        if (g.edge & 0xFF) dl->AddCircle(centre, height * 0.5f - edge * 0.5f, theme_colour(g.edge), 0, edge);
        if (g.mark != OverlayGlyphMark::none) return draw_mark(dl, centre, height * 0.5f, g);
    } else {
        const float radius = height * (g.shape == OverlayGlyphShape::keycap ? kKeyRadius : kPillRadius);
        dl->AddRectFilled(at, end, theme_colour(g.fill), radius);
        if (g.edge & 0xFF) dl->AddRect(at, end, theme_colour(g.edge), radius, 0, edge);
    }
    const ImVec2 size = ImGui::CalcTextSize(g.label.c_str());
    dl->AddText(ImVec2(centre.x - size.x * 0.5f, centre.y - size.y * 0.5f), theme_colour(g.ink), g.label.c_str());
}
