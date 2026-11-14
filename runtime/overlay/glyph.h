// The menu's glyphs for a gamepad's buttons and the keyboard's keys, drawn with Dear ImGui's
// draw list in the font current at the call: a face button as a disc in its family's colour
// (theme.h), a shoulder, a trigger or any other button as a pill, and a key as a keycap. The
// footer and the Buttons view draw them.
#pragma once
#include "overlay/theme.h"
#include "pad_glyph.h"
#include "imgui.h"
#include <string>

// A disc whose label is wider than it fits is drawn as a pill of the same height.
enum class OverlayGlyphShape { circle, pill, keycap };
// The shape PlayStation prints on a face, drawn on the disc in place of its label.
enum class OverlayGlyphMark { none, cross, circle, square, triangle };
struct OverlayGlyph {
    std::string label;  // its name, as pad_glyph gives it or a key's
    uint32_t fill;      // each colour 0xRRGGBBAA, as the theme's
    uint32_t ink;       // the label's or the mark's
    uint32_t edge;      // its outline, 0 for none
    OverlayGlyphShape shape;
    OverlayGlyphMark mark;
};

// The glyph for `source` on a gamepad of family `family`, coloured from `theme`. `face_label`
// is the SDL_GamepadButtonLabel SDL gives a face button, 0 for none (pad_glyph).
OverlayGlyph overlay_pad_glyph(const OverlayTheme& theme, PadFamily family, const PadSource& source, int face_label);
// A keycap with `label` on it.
OverlayGlyph overlay_key_glyph(const OverlayTheme& theme, const char* label);
// How wide `glyph` is drawn `height` tall in the current font.
float overlay_glyph_width(const OverlayGlyph& glyph, float height);
// `glyph` drawn `height` tall in the current font, its top left at `at`.
void overlay_draw_glyph(ImDrawList* draw, ImVec2 at, float height, const OverlayGlyph& glyph);
