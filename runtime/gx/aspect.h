// The output aspect's arithmetic: the shape the window shows the picture at, and where the
// picture goes in a window of another shape. Free of GL and SDL, so tools/test_aspect.cpp
// checks it on its own.
#pragma once
#include <cmath>
#include <cstdlib>

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

// The width of a window `height` pixels high at aspect w:h, rounded up, so that fit_aspect
// fills it: 1280 at 960 high for 4:3, 1707 for 16:9.
inline int aspect_window_width(int height, int w, int h) {
    return (int)(((long long)height * w + h - 1) / h);
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
