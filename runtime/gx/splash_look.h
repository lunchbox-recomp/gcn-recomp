// What the boot splash (splash.h) draws, worked out with no GL: its colour, where the banner and
// the progress bar go, and the banner as runs of one colour, so a frame is a list of rects to
// clear (clear_draw.h).
#pragma once
#include <cstdint>
#include <vector>

namespace gx {

// --splash-color's default, a neutral dark grey.
constexpr uint32_t kSplashColor = 0x18191C;
// The bar shows only once the splash has been up this long, so a quick start shows none.
constexpr double kSplashBarAfterMs = 300.0;

// Exactly six hex digits, either case, as 0xRRGGBB. False, with `rgb` as it was, for anything else.
bool parse_rrggbb(const char* s, uint32_t* rgb);

inline bool splash_bar_shown(double ms_up) { return ms_up >= kSplashBarAfterMs; }

// x, y, w, h and a colour: in window pixels with y up, as glScissor takes them, or in the banner's
// own pixels with y down (banner_runs).
struct SplashRect {
    int x, y, w, h;
    uint32_t rgb;
    bool operator==(const SplashRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h && rgb == o.rgb; }
};

// The banner (banner.h's kBannerW x kBannerH straight-alpha RGBA) blended over `bg`, as runs of
// one colour along each row, one pixel high. Runs that come out as `bg` are left out.
std::vector<SplashRect> banner_runs(const uint8_t* rgba, uint32_t bg);

// The banner's whole-number scale in a `w` x `h` window: as large as fits a third of each, and
// at least 1.
int splash_scale(int w, int h);

// The rects of one frame over a window cleared to `bg`, in order: the banner's runs (empty for no
// banner) scaled and centred, then once the splash has been up `ms_up` long the bar's track and
// what `done` of `total` fills, below the banner's place, which a disc with none leaves empty.
std::vector<SplashRect> splash_frame(const std::vector<SplashRect>& runs, uint32_t bg, int w, int h, double ms_up,
                                     int done, int total);

}  // namespace gx
