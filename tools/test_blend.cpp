// Blend factor checks: the `test_blend` target. Runs with no window, GL, disc or game.
//
// runtime/gx/blend.h picks the factors render_gl.cpp hands glBlendFunc. GX reads destination
// alpha as full when the EFB's pixel format has no alpha channel, so on any format but
// RGBA6_Z24 a DSTALPHA factor must be ONE and INVDSTALPHA ZERO, as a source factor and as a
// destination one. The GL framebuffer keeps an alpha channel whatever the format, and on
// those formats the renderer masks writes to it, so it holds whatever an earlier pass left.
// A draw that blended by it multiplied the frame by an alpha the game never set, and a
// full-screen ZERO, DSTALPHA pass on RGB8_Z24 turned the whole picture black.
//
// usage: test_blend
// Exits 0 only if every check passes.
#include "gx/blend.h"
#include <cstdio>

namespace {

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

uint32_t mode(uint32_t src, uint32_t dst) { return 1u | src << 8 | dst << 5; }

// What a factor becomes on a format with no alpha: DSTALPHA (6) is ONE (1), INVDSTALPHA (7)
// is ZERO (0), and the rest are as given.
uint32_t without_dst_alpha(uint32_t f) { return f == 6 ? 1 : f == 7 ? 0 : f; }

// How many of the 64 source and destination pairs come out wrong at PE control `pe`, whose
// format has alpha or hasn't as `has_alpha` says.
int wrong_pairs(uint32_t pe, bool has_alpha) {
    int wrong = 0;
    for (uint32_t s = 0; s < 8; s++)
        for (uint32_t d = 0; d < 8; d++) {
            const gx::BlendFactors f = gx::blend_factors(mode(s, d), pe);
            const uint32_t ws = has_alpha ? s : without_dst_alpha(s), wd = has_alpha ? d : without_dst_alpha(d);
            if (f.src != ws || f.dst != wd) wrong++;
        }
    return wrong;
}

}  // namespace

int main() {
    // PE control's pixel format is bits 0-2. Bits 3-5 are the z format and bit 6 the z compare
    // location, so each format is tried with all 16 settings of those four bits.
    int lost = 0, gained = 0;
    for (uint32_t fmt = 0; fmt < 8; fmt++)
        for (uint32_t other = 0; other < 0x80; other += 8) {
            const bool has = gx::efb_has_alpha(other | fmt);
            if (fmt == 1 && !has) lost++;
            if (fmt != 1 && has) gained++;
        }
    check(lost == 0, "RGBA6_Z24 has alpha, whatever PE control's other bits");
    check(gained == 0, "and no other of the eight pixel formats does");

    const int kept = wrong_pairs(1, true) + wrong_pairs(0x41, true);
    if (kept) printf("  %d pairs changed\n", kept);
    check(kept == 0, "on RGBA6_Z24 all 64 source and destination pairs are as the blend mode gives them");

    int dropped = 0;
    for (uint32_t fmt = 0; fmt < 8; fmt++) {
        if (fmt == 1) continue;
        const int wrong = wrong_pairs(fmt, false) + wrong_pairs(0x40 | fmt, false);
        if (wrong) printf("  format %u: %d pairs wrong\n", fmt, wrong);
        dropped += wrong;
    }
    check(dropped == 0,
          "on the seven formats without alpha DSTALPHA is ONE and INVDSTALPHA ZERO, as source or destination, "
          "and every other factor is kept");

    // A full-screen pass of ZERO, DSTALPHA with colour and alpha updates (0x18), on RGB8_Z24
    // with z compare before texture (PE control 0x40).
    const gx::BlendFactors pass = gx::blend_factors(mode(0, 6) | 0x18, 0x40);
    check(pass.src == 0 && pass.dst == 1,
          "ZERO, DSTALPHA on RGB8_Z24 is ZERO, ONE, which leaves the frame as it was");
    const gx::BlendFactors inv = gx::blend_factors(1u | 7u << 8 | 7u << 5, 0x000042);
    check(inv.src == 0 && inv.dst == 0, "INVDSTALPHA, INVDSTALPHA on RGB565_Z16 is ZERO, ZERO");

    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
