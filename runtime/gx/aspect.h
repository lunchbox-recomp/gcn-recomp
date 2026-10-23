// The output aspect's arithmetic: the shape the window shows the picture at, and where the
// picture goes in a window of another shape. Free of GL and SDL, so tools/test_aspect.cpp
// checks it on its own.
#pragma once
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <numeric>

namespace gx {

// "W:H", two whole numbers from 1 to 9999 such as 16:9, into `w` and `h`. False for anything
// else, so a mistyped flag is refused rather than read as some other shape.
inline bool parse_aspect(const char* s, int* w, int* h) {
    int v[2];
    for (int i = 0; i < 2; i++) {
        if (*s < '0' || *s > '9') return false;
        char* end;
        const long n = strtol(s, &end, 10);
        if (end - s > 4 || n < 1) return false;
        v[i] = (int)n;
        s = end;
        if (*s != (i == 0 ? ':' : '\0')) return false;
        if (i == 0) s++;
    }
    *w = v[0];
    *h = v[1];
    return true;
}

// The width of a window `height` pixels high at aspect w:h, rounded up: 1280 at 960 high for
// 4:3, 1707 for 16:9. A landscape window this wide is filled by fit_aspect, with no bars.
// Doubling it for a display of two pixels to a point keeps that for the shapes anyone is
// likely to pick, but an odd ratio can lose a pixel: 15:11 doubles to 2620 by 1920 and the
// picture is 2618 wide. A portrait shape can leave a bar at one pixel to a point, as 9:21
// does with a 412 wide window and a picture 411 wide.
inline int aspect_window_width(int height, int w, int h) {
    return (int)(((long long)height * w + h - 1) / h);
}

struct WindowSize {
    int w, h;
};

// The size a window opens at: `height` high and as wide as aspect w:h makes that, or, when
// that wouldn't fit in a max_w by max_h area such as a display's usable bounds, the largest
// window of the same shape that does, so it stays filled. A limit of 0 or less is no limit.
// The smaller window's height is a whole number of the shape's steps (3 at 4:3, 9 at 16:9),
// so its width is exact and it stays filled at any number of pixels to a point; a width
// rounded up would leave a pixel of bar at two. A shape too wide to be even one pixel high
// inside max_w can't keep its shape, and is clipped to max_w at one pixel high.
inline WindowSize aspect_window_size(int height, int w, int h, int max_w, int max_h) {
    if (max_w <= 0) max_w = INT_MAX;
    if (max_h <= 0) max_h = INT_MAX;
    const long long widest_fit = (long long)max_w * h / w;  // the tallest max_w wide at this shape
    long long tall = std::min({(long long)height, (long long)max_h, widest_fit});
    const long long step = h / std::gcd(w, h);
    if (tall < height && tall >= step) tall -= tall % step;
    tall = std::max(1LL, tall);
    return {std::min(aspect_window_width((int)tall, w, h), max_w), (int)tall};
}

struct OutputRect {
    int x, y, w, h;
};

// Where a picture of aspect `a` (width over height) goes in a w by h target: as large as
// fits, centred, with bars on the two sides that need them. Rounded down to whole pixels;
// the thousandth of a pixel absorbs the float's error in `a`, so at 4/3 this is exactly the
// integer w * 3 / 4 and h * 4 / 3.
inline OutputRect fit_aspect(int w, int h, float a) {
    int vw = w, vh = (int)std::floor(w / (double)a + 1e-3);
    if (vh > h) {
        vh = h;
        vw = (int)std::floor(h * (double)a + 1e-3);
    }
    return {(w - vw) / 2, (h - vh) / 2, vw, vh};
}

}  // namespace gx
