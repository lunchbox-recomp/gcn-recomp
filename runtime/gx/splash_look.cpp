// See splash_look.h.
#include "gx/splash_look.h"
#include "banner.h"
#include "env.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gx {
namespace {

// `top` at alpha `a` (0 to 255) over `bottom`, a channel at a time, rounded.
uint32_t blend(uint32_t top, uint32_t bottom, uint32_t a) {
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        const uint32_t t = (top >> sh) & 0xFF, b = (bottom >> sh) & 0xFF;
        out |= ((t * a + b * (255 - a) + 127) / 255) << sh;
    }
    return out;
}

// White over a dark background, black over a light one, for the bar.
uint32_t ink(uint32_t bg) {
    const uint32_t luma = ((bg >> 16 & 0xFF) * 299 + (bg >> 8 & 0xFF) * 587 + (bg & 0xFF) * 114) / 1000;
    return luma > 140 ? 0x000000 : 0xFFFFFF;
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool parse_rrggbb(const char* s, uint32_t* rgb) {
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        const int d = hex(s[i]);  // a NUL stops it here, so s is never read past its end
        if (d < 0) return false;
        v = v << 4 | (uint32_t)d;
    }
    if (s[6]) return false;
    *rgb = v;
    return true;
}

bool take_splash_color(int argc, char** argv, uint32_t* rgb) {
    *rgb = kSplashColor;
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--splash-color=", 15) && !parse_rrggbb(argv[i] + 15, rgb)) {
            fprintf(stderr, "--splash-color: RRGGBB, six hex digits, not %s\n", argv[i] + 15);
            return false;
        }
    return true;
}

bool splash_wanted(bool on, bool hidden) {
    if (hidden) return false;
    if (const char* e = getenv_nonempty("GCN_SPLASH")) {
        if (!strcmp(e, "0") || !strcmp(e, "1")) return e[0] == '1';
        fprintf(stderr, "[splash] GCN_SPLASH=%s isn't 0 or 1, so the build's choice holds\n", e);
    }
    return on;
}

std::vector<SplashRect> banner_runs(const uint8_t* rgba, uint32_t bg) {
    std::vector<SplashRect> runs;
    for (int y = 0; y < kBannerH; y++) {
        bool open = false;
        for (int x = 0; x < kBannerW; x++) {
            const uint8_t* p = rgba + ((size_t)y * kBannerW + x) * 4;
            const uint32_t c = blend((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2], bg, p[3]);
            if (c == bg) open = false;
            else if (open && runs.back().rgb == c) runs.back().w++;
            else {
                runs.push_back({x, y, 1, 1, c});
                open = true;
            }
        }
    }
    return runs;
}

int splash_scale(int w, int h) { return std::max(1, std::min(w / (kBannerW * 3), h / (kBannerH * 3))); }

std::vector<SplashRect> splash_frame(const std::vector<SplashRect>& runs, uint32_t bg, int w, int h, double ms_up,
                                     int done, int total) {
    const int s = splash_scale(w, h);
    const int bw = kBannerW * s, x0 = (w - bw) / 2, y0 = (h - kBannerH * s) / 2;
    std::vector<SplashRect> out;
    out.reserve(runs.size() + 2);
    for (const SplashRect& r : runs) out.push_back({x0 + r.x * s, y0 + (kBannerH - r.y - r.h) * s, r.w * s, r.h * s, r.rgb});
    if (splash_bar_shown(ms_up)) {
        const int bar_h = std::max(2, s / 2), bar_y = y0 - 4 * s - bar_h;
        out.push_back({x0, bar_y, bw, bar_h, blend(ink(bg), bg, 40)});
        const int fill = total > 0 ? (int)((int64_t)bw * std::clamp(done, 0, total) / total) : 0;
        if (fill > 0) out.push_back({x0, bar_y, fill, bar_h, blend(ink(bg), bg, 190)});
    }
    return out;
}

}  // namespace gx
