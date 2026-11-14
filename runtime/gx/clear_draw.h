// Drawing with scissored glClears alone, for a screen shown before any shader program can be
// assumed: the headset's loading screen (openxr/xr_frontend.cpp) and the desktop's boot splash
// (splash.cpp). Each call clears its rects of the bound framebuffer with GL_SCISSOR_TEST on,
// which the caller enables first and turns off after, or with it off, which clears the whole
// target whatever the rect (the headset's first clear_rect).
#pragma once
#include <cstdint>

namespace gx {

// `s` centred on x = cx with its bottom at y = bottom (GL's, up), in a 5x7 font of pixels `px`
// square, a column of space after each character, in the current clear colour. A character
// without a glyph is a space. One clear for each run of lit pixels in a glyph's row.
void clear_text(const char* s, int cx, int bottom, int px);

// The rect x, y, w, h (GL's, y up) cleared to the colour 0xRRGGBB, opaque. Leaves that as the
// clear colour.
void clear_rect(int x, int y, int w, int h, uint32_t rgb);

}  // namespace gx
