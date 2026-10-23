// The blend factors a draw uses, in GX's numbering, once the EFB's pixel format has had its
// say. Free of GL, so tools/test_blend.cpp checks it on its own.
#pragma once
#include <cstdint>

namespace gx {

// Whether PE control's pixel format (bp[0x43], bits 0-2) keeps alpha. Only RGBA6_Z24 (1)
// does: RGB8_Z24, RGB565_Z16 and the Z and YUV formats have no alpha channel at all.
inline bool efb_has_alpha(uint32_t pe_control) { return (pe_control & 7) == 1; }

struct BlendFactors {
    uint32_t src, dst;  // 0 ZERO, 1 ONE, ..., 6 DSTALPHA, 7 INVDSTALPHA, as both tables number them
};

// The blend mode's (bp[0x41]) source and destination factors. A format without alpha reads
// destination alpha as full, so there DSTALPHA is ONE and INVDSTALPHA ZERO. GL can't be
// left to read it: its framebuffer keeps an alpha channel, which holds whatever an earlier
// pass wrote, since alpha updates are masked on such a format.
inline BlendFactors blend_factors(uint32_t blend_mode, uint32_t pe_control) {
    uint32_t src = (blend_mode >> 8) & 7, dst = (blend_mode >> 5) & 7;
    if (!efb_has_alpha(pe_control)) {
        auto full = [](uint32_t f) { return f == 6 ? 1u : f == 7 ? 0u : f; };
        src = full(src);
        dst = full(dst);
    }
    return {src, dst};
}

}  // namespace gx
