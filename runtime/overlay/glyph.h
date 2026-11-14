#pragma once
#include "overlay/theme.h"
#include "pad_glyph.h"
#include "imgui.h"
#include <string>

enum class OverlayGlyphShape { circle, pill, keycap };
struct OverlayGlyph {
    std::string label;
    uint32_t fill;
    uint32_t ink;
    OverlayGlyphShape shape;
};

OverlayGlyph overlay_pad_glyph(const OverlayTheme& theme, PadFamily family, const PadSource& source, int face_label);
OverlayGlyph overlay_key_glyph(const OverlayTheme& theme, const char* label);
uint32_t overlay_glyph_ink(uint32_t fill);
float overlay_glyph_width(const OverlayGlyph& glyph, float height);
void overlay_draw_glyph(ImDrawList* draw, ImVec2 at, float height, const OverlayGlyph& glyph);
