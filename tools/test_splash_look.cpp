// The boot splash's look, worked out with no GL: the `test_splash_look` target. Runs with no
// window, GL, disc or game.
//
// runtime/gx/splash_look.cpp parses --splash-color, decides when the bar shows, blends the
// banner over the colour into runs and places them and the bar in a window. The banners here
// are made up by this file.
//
// usage: test_splash_look
// Exits 0 only if every check passes.
#include "gx/splash_look.h"
#include "banner.h"
#include <cstdio>
#include <vector>

using namespace gx;

namespace {

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

void set(std::vector<uint8_t>& rgba, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t* p = &rgba[((size_t)y * kBannerW + x) * 4];
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

}  // namespace

int main() {
    uint32_t rgb = 7;
    check(parse_rrggbb("1A2b3C", &rgb) && rgb == 0x1A2B3C, "RRGGBB in either case is 0xRRGGBB");
    check(parse_rrggbb("000000", &rgb) && rgb == 0, "000000 is black");
    rgb = 7;
    bool refused = true;
    for (const char* s : {"", "12345", "1234567", "12345g", "#123456", " 123456", "0x1234"})
        if (parse_rrggbb(s, &rgb)) refused = false;
    check(refused && rgb == 7, "anything but six hex digits is refused, and leaves the colour as it was");

    check(!splash_bar_shown(0) && !splash_bar_shown(299.9), "no bar in the first 300 ms");
    check(splash_bar_shown(300) && splash_bar_shown(5000), "and a bar from then on");

    // A row of three opaque reds, a half-clear white, a clear pixel and a pixel the colour of
    // the background; a red on the next row.
    std::vector<uint8_t> rgba(kBannerW * kBannerH * 4, 0);
    for (int x = 0; x < 3; x++) set(rgba, x, 0, 255, 0, 0, 255);
    set(rgba, 3, 0, 255, 255, 255, 128);
    set(rgba, 4, 0, 255, 255, 255, 0);
    set(rgba, 5, 0, 0x20, 0x40, 0x60, 255);
    set(rgba, 95, 31, 255, 0, 0, 255);
    const std::vector<SplashRect> black = banner_runs(rgba.data(), 0x000000);
    check(black.size() == 4 && black[0] == (SplashRect{0, 0, 3, 1, 0xFF0000}), "a row's pixels of one colour are one run");
    check(black.size() == 4 && black[1] == (SplashRect{3, 0, 1, 1, 0x808080}), "alpha 128 over black is half the colour");
    check(black.size() == 4 && black[2] == (SplashRect{5, 0, 1, 1, 0x204060}) && black[3] == (SplashRect{95, 31, 1, 1, 0xFF0000}),
          "a clear pixel is left out, and rows go top to bottom");
    const std::vector<SplashRect> blue = banner_runs(rgba.data(), 0x204060);
    bool halfway = blue.size() == 3;
    for (int sh = 0; halfway && sh < 24; sh += 8) {
        const double got = (blue[1].rgb >> sh) & 0xFF, mid = (((0x204060 >> sh) & 0xFF) + 255) / 2.0;
        if (got < mid - 1 || got > mid + 1) halfway = false;
    }
    check(halfway, "half-clear white over a colour is halfway between them");
    check(blue.size() == 3 && blue[2] == (SplashRect{95, 31, 1, 1, 0xFF0000}), "and a pixel the colour of the background is left out");
    std::vector<uint8_t> clear(kBannerW * kBannerH * 4, 0);
    check(banner_runs(clear.data(), 0x18191C).empty(), "a clear banner is no runs");

    check(splash_scale(2560, 1920) == 8 && splash_scale(1280, 960) == 4, "the banner is a third of the window at most");
    check(splash_scale(2560, 600) == 6, "of its height too");
    check(splash_scale(100, 50) == 1 && splash_scale(0, 0) == 1, "and 1 when the window is smaller");

    // 1280 x 960 at scale 4: the banner is 384 x 128 from (448, 416), y up, so its top row is at
    // y 540 and every rect is a whole number of its pixels.
    const std::vector<SplashRect> early = splash_frame(black, 0, 1280, 960, 100, 1, 4);
    check(early.size() == black.size() && early[0] == (SplashRect{448, 540, 12, 4, 0xFF0000}),
          "the banner's runs are scaled and centred, the top row highest");
    check(early.size() == black.size() && early[3] == (SplashRect{448 + 95 * 4, 416, 4, 4, 0xFF0000}),
          "its bottom right pixel at the box's corner");
    const std::vector<SplashRect> late = splash_frame(black, 0, 1280, 960, 300, 1, 4);
    check(late.size() == black.size() + 2, "after 300 ms the bar's track and fill follow the banner");
    if (late.size() == black.size() + 2) {
        const SplashRect& track = late[late.size() - 2];
        const SplashRect& fill = late.back();
        check(track.x == 448 && track.w == 384 && track.y + track.h <= 416, "the track is as wide as the banner, below it");
        check(fill.x == 448 && fill.y == track.y && fill.h == track.h && fill.w == 96, "a quarter done fills a quarter");
        check(track.rgb != 0 && fill.rgb != track.rgb, "the track and the fill stand out from the background and each other");
    }
    check(splash_frame(black, 0, 1280, 960, 400, 0, 4).size() == black.size() + 1, "nothing done is the track alone");
    const std::vector<SplashRect> full = splash_frame({}, 0, 1280, 960, 400, 4, 4);
    check(full.size() == 2 && full[1].w == full[0].w, "with no banner the bar is where it would be, and all done fills it");
    const std::vector<SplashRect> light = splash_frame({}, 0xFFFFFF, 1280, 960, 400, 2, 4);
    check(light.size() == 2 && light[0].rgb != 0xFFFFFF && light[1].rgb < light[0].rgb,
          "on a white background the bar is drawn darker, not lost in it");

    return g_failed ? 1 : 0;
}
