// Flat viewport checks: the `test_viewport` target. Runs with no window, GL, disc or game.
//
// runtime/gx/viewport.h gives the terms the flat path's vertex shader places a vertex on the
// EFB with (u_vp_a). The console takes a pixel as covered when the point 7/12 of the way
// across and down it is inside, and GL takes the point at 1/2, so every vertex is drawn 1/12
// of a sample left and up of where GX's viewport puts it. These checks pin that offset: its
// size and direction at 1x, that it is 1/12 of a sample and not of a 1x pixel at a scaled
// internal resolution, that an anchor's squeeze at a wider output doesn't scale it, that a
// mirrored viewport moves the same way, and that GL's 1/2 sample then covers the console's
// pixels and a whole-frame quad still covers every sample.
//
// usage: test_viewport
// Exits 0 only if every check passes.
#include "gx/viewport.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {

using gx::Anchor;
using gx::AnchorMap;

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

constexpr int kW = 640, kH = 528;
// GX's scissor offset and the viewport origin both carry 342.
constexpr int kOff = 342;
constexpr double kTwelfth = 1.0 / 12.0;

// A viewport of the whole 640x480 frame, as a game sets one: sx, sy, sz, ox, oy, oz.
void whole_frame(float vp[6], bool mirrored = false) {
    vp[0] = mirrored ? -320.0f : 320.0f;
    vp[1] = -240.0f;
    vp[2] = 16777215.0f;
    vp[3] = 320.0f + kOff;
    vp[4] = 240.0f + kOff;
    vp[5] = 16777215.0f;
}

// Where a vertex at clip (cx, cy, w = 1) lands, in samples from the target's top left, as GL
// places it from the shader's terms: y negated into GL's and back, so y points down.
void window_xy(const float vpa[4], float cx, float cy, int scale, double* x, double* y) {
    const double nx = (double)(vpa[0] + vpa[1] * cx);
    const double ny = (double)(vpa[2] + vpa[3] * cy);
    *x = (nx + 1.0) * 0.5 * kW * scale;
    *y = (ny + 1.0) * 0.5 * kH * scale;
}

// Where GX puts the same vertex, in 1x EFB pixels from the top left, with no correction.
void gx_xy(const float vp[6], float cx, float cy, double* x, double* y) {
    *x = (double)vp[3] - kOff + (double)vp[0] * cx;
    *y = (double)vp[4] - kOff + (double)vp[1] * cy;
}

bool near(double a, double b) { return std::fabs(a - b) <= 1e-3; }

// The first sample a left edge at `edge` covers: the first whose point, at `at` of the way
// across, isn't before it.
long first_covered(double edge, double at) { return (long)std::ceil(edge - at); }

}  // namespace

int main() {
    float vp[6];
    whole_frame(vp);
    const AnchorMap none{};
    const float cxs[] = {-1.0f, -0.37f, 0.0f, 0.5f, 1.0f};

    // 1x, 4:3: 1/12 of a pixel left and up of GX's place.
    {
        float vpa[4];
        gx::flat_viewport(vp, kOff, kOff, none, kW, kH, 1, vpa);
        bool ok = true;
        for (float c : cxs) {
            double x, y, gx_x, gx_y;
            window_xy(vpa, c, c, 1, &x, &y);
            gx_xy(vp, c, c, &gx_x, &gx_y);
            ok = ok && near(x, gx_x - kTwelfth) && near(y, gx_y - kTwelfth);
        }
        check(ok, "at 1x a vertex lands 1/12 of a pixel left of and above GX's place");
    }

    // Scaled: 1/12 of a sample, whatever the scale.
    for (int scale : {2, 3, 4, 8}) {
        float vpa[4];
        gx::flat_viewport(vp, kOff, kOff, none, kW, kH, scale, vpa);
        bool ok = true;
        for (float c : cxs) {
            double x, y, gx_x, gx_y;
            window_xy(vpa, c, c, scale, &x, &y);
            gx_xy(vp, c, c, &gx_x, &gx_y);
            ok = ok && near(x, gx_x * scale - kTwelfth) && near(y, gx_y * scale - kTwelfth);
        }
        char what[96];
        snprintf(what, sizeof(what), "at %dx a vertex lands 1/12 of a sample left of and above GX's place", scale);
        check(ok, what);
    }

    // GL's sample at 1/2 then covers what the console's at 7/12 covers, for edges all across a
    // pixel. Edges within a hair of either rule's sample point are left out, since which side
    // of the point they fall is a rounding.
    for (int scale : {1, 2, 8}) {
        bool ok = true;
        int compared = 0;
        for (int i = 0; i <= 96; i++) {
            const float cx = -0.5f + (float)i / (96.0f * 320.0f);  // an edge near x = 160, in 1/96ths
            float vpa[4];
            gx::flat_viewport(vp, kOff, kOff, none, kW, kH, scale, vpa);
            double x, y, gx_x, gx_y;
            window_xy(vpa, cx, 0.0f, scale, &x, &y);
            gx_xy(vp, cx, 0.0f, &gx_x, &gx_y);
            const double console_edge = gx_x * scale;
            const double frac = console_edge - std::floor(console_edge);
            if (std::fabs(frac - 7.0 / 12.0) < 1e-3 || std::fabs(x - std::floor(x) - 0.5) < 1e-3) continue;
            compared++;
            ok = ok && first_covered(x, 0.5) == first_covered(console_edge, 7.0 / 12.0);
        }
        char what[96];
        snprintf(what, sizeof(what), "at %dx GL's 1/2 sample covers the console's 7/12 pixels (%d edges)", scale, compared);
        check(ok && compared > 80, what);
    }

    // A quad over the whole frame covers every sample, at every scale up to the renderer's
    // cap: its right and bottom edges stay past the last sample's 1/2 point.
    {
        bool ok = true;
        for (int scale = 1; scale <= 8; scale++) {
            float vpa[4];
            gx::flat_viewport(vp, kOff, kOff, none, kW, kH, scale, vpa);
            double l, t, r, b;
            window_xy(vpa, -1.0f, 1.0f, scale, &l, &t);
            window_xy(vpa, 1.0f, -1.0f, scale, &r, &b);
            ok = ok && first_covered(l, 0.5) == 0 && first_covered(t, 0.5) == 0 &&
                 first_covered(r, 0.5) == 640L * scale && first_covered(b, 0.5) == 480L * scale;
        }
        check(ok, "a whole-frame quad covers every sample from 1x to 8x");
    }

    // A wider output squeezes anchored draws toward their edge, in EFB pixels, before they're
    // sampled: the offset comes after the squeeze and isn't scaled by it.
    {
        const float k = (4.0f / 3.0f) / (16.0f / 9.0f);
        bool ok = true;
        for (Anchor a : {Anchor::Left, Anchor::Center, Anchor::Right}) {
            const AnchorMap an = gx::anchor_map(a, 0.0f, 640.0f, k);
            for (int scale : {1, 2}) {
                float vpa[4];
                gx::flat_viewport(vp, kOff, kOff, an, kW, kH, scale, vpa);
                for (float c : cxs) {
                    double x, y, gx_x, gx_y;
                    window_xy(vpa, c, c, scale, &x, &y);
                    gx_xy(vp, c, c, &gx_x, &gx_y);
                    const double want = (double)gx::anchor_x(a, (float)gx_x, 0.0f, 640.0f, k) * scale - kTwelfth;
                    ok = ok && near(x, want) && near(y, gx_y * scale - kTwelfth);
                }
            }
        }
        check(ok, "at 16:9 an anchored vertex lands 1/12 of a sample left of its squeezed place");
    }

    // The console's sample point is fixed on the screen, so a mirrored viewport moves left too.
    {
        float mvp[6];
        whole_frame(mvp, true);
        float vpa[4];
        gx::flat_viewport(mvp, kOff, kOff, none, kW, kH, 1, vpa);
        bool ok = true;
        for (float c : cxs) {
            double x, y, gx_x, gx_y;
            window_xy(vpa, c, c, 1, &x, &y);
            gx_xy(mvp, c, c, &gx_x, &gx_y);
            ok = ok && near(x, gx_x - kTwelfth);
        }
        check(ok, "a mirrored viewport's vertex lands 1/12 of a pixel left of GX's place too");
    }

    // The scissor offset still comes off: a game that sets it to 340 and moves its viewport by
    // the same keeps its picture at the EFB's origin.
    {
        float svp[6];
        whole_frame(svp);
        svp[3] -= 2.0f;
        svp[4] -= 2.0f;
        float vpa[4];
        gx::flat_viewport(svp, kOff - 2, kOff - 2, none, kW, kH, 1, vpa);
        double x, y;
        window_xy(vpa, -1.0f, 1.0f, 1, &x, &y);
        check(near(x, -kTwelfth) && near(y, -kTwelfth), "the scissor offset comes off before the pixel centre");
    }

    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
