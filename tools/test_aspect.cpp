// Output aspect checks: the `test_aspect` target. Runs with no window, GL, disc or game.
//
// runtime/gx/aspect.h is the arithmetic behind --aspect: reading W:H, the width a window
// opens at, and where the picture goes in a target of any shape. At 4:3, 16:9, 16:10, 21:9
// and 32:9 the fit must be the integer letterbox (w * H / W, else h * W / H) in every window
// up to 3000 pixels a side, so the default picture never moves and no other shape's is a
// pixel short. A window opened at a landscape aspect must be filled with no bar at every
// height, and the shapes a player picks must be filled at two pixels to a point too, or a
// pixel of black would show down one side of every frame. A window that wouldn't fit the
// display's usable area is scaled down in its own shape, and never opens past that area.
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

// The exact fit of a picture aw:ah in a w by h target, by integer division: as large as fits,
// centred, rounded down.
gx::OutputRect letterbox(int w, int h, int aw, int ah) {
    long long vw = w, vh = (long long)w * ah / aw;
    if (vh > h) {
        vh = h;
        vw = (long long)h * aw / ah;
    }
    return {(int)((w - vw) / 2), (int)((h - vh) / 2), (int)vw, (int)vh};
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

    const int exact[][2] = {{4, 3}, {16, 9}, {16, 10}, {21, 9}, {32, 9}};
    long moved = 0;
    for (const auto& s : exact) {
        long off = 0;
        const float a = (float)s[0] / (float)s[1];
        for (int w = 1; w <= 3000; w++)
            for (int h = 1; h <= 3000; h++)
                if (!same(gx::fit_aspect(w, h, a), letterbox(w, h, s[0], s[1]))) off++;
        if (off) printf("  at %d:%d %ld windows differ\n", s[0], s[1], off);
        moved += off;
    }
    check(moved == 0, "at 4:3, 16:9, 16:10, 21:9 and 32:9 the fit is the integer letterbox in every window up to 3000 by 3000");

    const int landscape[][2] = {{4, 3}, {16, 9}, {16, 10}, {21, 9}, {32, 9}, {5, 4}, {15, 11}, {1, 1}};
    long barred = 0;
    for (const auto& s : landscape)
        for (int h = 1; h <= 2160; h++) {
            const int w = gx::aspect_window_width(h, s[0], s[1]);
            if (!same(gx::fit_aspect(w, h, (float)s[0] / (float)s[1]), {0, 0, w, h})) barred++;
        }
    check(barred == 0, "a window opened at a landscape aspect is filled, at every height to 2160");
    // The opening size inside a display. With room, or with no limit given, it is the size above.
    // Without room it is scaled down in its own shape: 21:9 on a 1728 by 1050 area is 1727 by
    // 740, and 4:3 on a 1366 by 700 area is 934 by 700. A shape too wide for even one pixel of
    // height is clipped to the area's width at one pixel high, where it was 9,599,040 wide.
    auto sized = [](int h, int w, int ah, int mw, int mh) {
        const gx::WindowSize s = gx::aspect_window_size(h, w, ah, mw, mh);
        return s.w * 100000LL + s.h;
    };
    check(sized(960, 4, 3, 0, 0) == 1280 * 100000LL + 960 && sized(960, 16, 9, 3000, 2000) == 1707 * 100000LL + 960 &&
              sized(960, 21, 9, 0, 1200) == 2240 * 100000LL + 960 && sized(960, 4, 3, 1280, 960) == 1280 * 100000LL + 960,
          "a window with room opens at the size above, and so does one with no limit given");
    check(sized(960, 21, 9, 1728, 1050) == 1727 * 100000LL + 740 && sized(960, 4, 3, 1366, 700) == 934 * 100000LL + 700 &&
              sized(960, 9999, 1, 3456, 2234) == 3456 * 100000LL + 1,
          "a window that won't fit is scaled down in its own shape, 21:9 to 1727 by 740 on 1728 by 1050");
    long over = 0, unfilled = 0, samples = 0;
    for (const auto& s : landscape)
        for (int mw = 100; mw <= 4000; mw += 37)
            for (int mh = 100; mh <= 2400; mh += 41) {
                const gx::WindowSize win = gx::aspect_window_size(960, s[0], s[1], mw, mh);
                samples++;
                if (win.w > mw || win.h > mh || win.h > 960) over++;
                else if (win.h > 1 && !same(gx::fit_aspect(win.w, win.h, (float)s[0] / (float)s[1]), {0, 0, win.w, win.h})) unfilled++;
            }
    printf("  %ld sizes, %ld over their area, %ld not filled\n", samples, over, unfilled);
    check(over == 0 && unfilled == 0, "a landscape window never opens past its area and stays filled, at 8 shapes in a grid of areas");


    const int picked[][2] = {{4, 3}, {16, 9}, {16, 10}, {21, 9}, {32, 9}, {5, 4}, {3, 2}, {2, 1},
                             {64, 27}, {43, 18}, {12, 5}, {1, 1}, {9, 16}, {3, 4}};
    barred = 0;
    for (const auto& s : picked)
        for (int px = 1; px <= 2; px++) {
            const int w = gx::aspect_window_width(960, s[0], s[1]) * px, h = 960 * px;
            if (!same(gx::fit_aspect(w, h, (float)s[0] / (float)s[1]), {0, 0, w, h})) {
                printf("  %d:%d at %dx leaves a bar\n", s[0], s[1], px);
                barred++;
            }
        }
    check(barred == 0, "14 named shapes, two of them portrait, fill the window they open at one and two pixels to a point");

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
