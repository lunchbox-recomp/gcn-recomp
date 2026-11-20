// The flat path's viewport: where GX's viewport puts a vertex on the EFB, as the terms the
// vertex shader takes (u_vp_a), with the console's pixel centre. Free of GL, so
// tools/test_viewport.cpp checks it on its own.
#pragma once
#include "anchor.h"

namespace gx {

// The console's rasterizer takes a pixel as covered when a point 7/12 of the way across it
// and 7/12 of the way down it is inside the primitive, with no antialiasing. GL takes the
// point at 1/2. Moving every vertex 1/12 of a pixel left and up makes GL's sample see what
// the console's does, so an edge that isn't on a whole pixel covers the console's pixels.
//
// It is 1/12 of a pixel of the target drawn into: at an internal scale of N, of an EFB pixel
// drawn N times as wide. Dolphin corrects by the same amount at every scale. 1/12 of a
// 640-wide EFB pixel would put a whole-frame quad's right edge on or before the last sample's
// centre from 6 times up, and leave that column undrawn.
constexpr float kPixelCentreShift = 7.0f / 12.0f - 0.5f;

// u_vp_a for a viewport (sx, sy, sz, ox, oy, oz as XF holds them) on an EFB `width` by
// `height` drawn at `scale` samples to a pixel, less the scissor offset (`xoff`, `yoff`):
// window x = vpa[0] * w + vpa[1] * clip x and y = vpa[2] * w + vpa[3] * clip y, both from -1
// to 1 and y pointing down as GX's does. The anchor remaps the x terms first; the pixel
// centre comes after it, since the console samples the pixels the draw lands on.
inline void flat_viewport(const float vp[6], int xoff, int yoff, const AnchorMap& an, int width, int height,
                          int scale, float vpa[4]) {
    vpa[0] = 2.0f * (vp[3] - (float)xoff) / (float)width - 1.0f;
    vpa[1] = 2.0f * vp[0] / (float)width;
    vpa[2] = 2.0f * (vp[4] - (float)yoff) / (float)height - 1.0f;
    vpa[3] = 2.0f * vp[1] / (float)height;
    anchor_viewport(an.a, an.x0, an.x1, an.k, (float)width, vpa);
    vpa[0] -= 2.0f * kPixelCentreShift / (float)(width * scale);
    vpa[2] -= 2.0f * kPixelCentreShift / (float)(height * scale);
}

}  // namespace gx
