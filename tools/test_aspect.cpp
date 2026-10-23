// Output aspect checks: the `test_aspect` target. Runs with no window, GL, disc or game.
//
// runtime/gx/aspect.h is the arithmetic behind --aspect: reading W:H, the width a window
// opens at, and where the picture goes in a target of any shape. At 4:3 the fit must be the
// integer letterbox (w * 3 / 4, else h * 4 / 3) in every window up to 3000 pixels a side, so
// the default picture never moves. A window opened at a landscape aspect must be filled
// with no bar at every height, and the shapes a player picks must be filled at two pixels
// to a point too, or a pixel of black would show down one side of every frame.
//
// usage: test_aspect
// Exits 0 only if every check passes.
#include "gx/aspect.h"
#include <cstdio>

namespace {

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

bool parses(const char* s, int w, int h) {
    int pw = 0, ph = 0;
    return gx::parse_aspect(s, &pw, &ph) && pw == w && ph == h;
}

bool same(const gx::OutputRect& a, const gx::OutputRect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

gx::OutputRect letterbox_4x3(int w, int h) {
    int vw = w, vh = w * 3 / 4;
    if (vh > h) {
        vh = h;
        vw = h * 4 / 3;
    }
    return {(w - vw) / 2, (h - vh) / 2, vw, vh};
}

}  // namespace

int main() {
    check(parses("16:9", 16, 9) && parses("16:10", 16, 10) && parses("21:9", 21, 9) && parses("4:3", 4, 3) &&
              parses("9999:1", 9999, 1) && parses("1:9999", 1, 9999),
          "W:H reads as two whole numbers");
    const char* bad[] = {"", "16", "16:", ":9", "0:9", "16:0", "16:9x", "-16:9", "+16:9", " 16:9",
                         "16:9:1", "10000:1", "16:10000", "16.5:9", "window"};
    int accepted = 0;
    for (const char* s : bad) {
        int w, h;
        if (gx::parse_aspect(s, &w, &h)) {
            printf("  accepted \"%s\"\n", s);
            accepted++;
        }
    }
    check(accepted == 0, "anything else is refused");

    check(gx::aspect_window_width(960, 4, 3) == 1280 && gx::aspect_window_width(960, 16, 9) == 1707 &&
              gx::aspect_window_width(960, 16, 10) == 1536 && gx::aspect_window_width(960, 21, 9) == 2240,
          "a window 960 high opens 1280, 1707, 1536 and 2240 wide at 4:3, 16:9, 16:10 and 21:9");

    long moved = 0;
    for (int w = 1; w <= 3000; w++)
        for (int h = 1; h <= 3000; h++)
            if (!same(gx::fit_aspect(w, h, 4.0f / 3.0f), letterbox_4x3(w, h))) moved++;
    if (moved) printf("  %ld windows differ\n", moved);
    check(moved == 0, "at 4:3 the fit is the integer letterbox in every window up to 3000 by 3000");

    const int landscape[][2] = {{4, 3}, {16, 9}, {16, 10}, {21, 9}, {32, 9}, {5, 4}, {15, 11}, {1, 1}};
    long barred = 0;
    for (const auto& s : landscape)
        for (int h = 1; h <= 2160; h++) {
            const int w = gx::aspect_window_width(h, s[0], s[1]);
            if (!same(gx::fit_aspect(w, h, (float)s[0] / (float)s[1]), {0, 0, w, h})) barred++;
        }
    check(barred == 0, "a window opened at a landscape aspect is filled, at every height to 2160");

    const int picked[][2] = {{4, 3}, {16, 9}, {16, 10}, {21, 9}};
    barred = 0;
    for (const auto& s : picked)
        for (int px = 1; px <= 2; px++) {
            const int w = gx::aspect_window_width(960, s[0], s[1]) * px, h = 960 * px;
            if (!same(gx::fit_aspect(w, h, (float)s[0] / (float)s[1]), {0, 0, w, h})) barred++;
        }
    check(barred == 0, "4:3, 16:9, 16:10 and 21:9 fill the window they open, at one and two pixels to a point");

    barred = 0;
    for (int w = 1; w <= 1000; w++)
        for (int h = 1; h <= 1000; h++)
            if (!same(gx::fit_aspect(w, h, (float)w / (float)h), {0, 0, w, h})) barred++;
    check(barred == 0, "--aspect=window fills every window up to 1000 by 1000");

    check(same(gx::fit_aspect(1280, 960, 16.0f / 9.0f), {0, 120, 1280, 720}) &&
              same(gx::fit_aspect(1920, 1080, 4.0f / 3.0f), {240, 0, 1440, 1080}) &&
              same(gx::fit_aspect(3440, 1440, 16.0f / 9.0f), {440, 0, 2560, 1440}),
          "a picture of another shape is centred with bars on the sides it doesn't reach");

    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
