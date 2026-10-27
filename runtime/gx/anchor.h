// Draw anchors: the arithmetic behind gx::set_draw_anchor. A game marks the draws of a HUD
// element left, center or right, and the renderer squeezes each one toward its edge so that,
// once the picture is stretched to the output aspect, the element keeps its 4:3 shape at its
// 4:3 distance from that edge. Free of GL and SDL, so tools/test_anchor.cpp checks it on its
// own, and a renderer other than GL's takes the same remap from here.
#pragma once
#include <cmath>
#include <cstdint>

namespace gx {

// How a draw is placed. Stretch is the default and leaves the draw where the game put it, so
// it stretches with the rest of the picture.
enum class Anchor : uint8_t { Stretch, Left, Center, Right };

// How much a marked draw is squeezed toward its anchor: (4/3) over the output aspect, so
// that it is 1 at 4:3 and every remap below is then the identity. The picture is stretched
// by the aspect over 4/3 on its way to the window, and this takes that back out of the draw.
// An eye of a headset shows the picture at the shape the game drew it, so it is always 1.
inline float anchor_scale(float output_aspect, bool eye) {
    if (eye || !(output_aspect > 0.0f)) return 1.0f;
    return (4.0f / 3.0f) / output_aspect;
}

// Where x goes for an anchor whose rect runs from x0 to x1, all in EFB pixels. A left anchor
// holds x0, a right anchor holds x1 and a centered one holds the middle of the rect. At k = 1
// it returns x itself, bit for bit, because x0 + (x - x0) can differ from x by a rounding.
inline float anchor_x(Anchor a, float x, float x0, float x1, float k) {
    if (k == 1.0f) return x;
    switch (a) {
    case Anchor::Left:
        return x0 + (x - x0) * k;
    case Anchor::Right:
        return x1 - (x1 - x) * k;
    case Anchor::Center: {
        const float mid = 0.5f * (x0 + x1);
        return mid + (x - mid) * k;
    }
    case Anchor::Stretch:
        break;
    }
    return x;
}

// A draw's viewport as the vertex shader takes it (u_vp_a): window x = vpa[0] * w + vpa[1] *
// clip x, on a target `width` pixels across, from -1 to 1. Remapping the two terms moves
// every point the way anchor_x does, since the remap is a scale and a shift. Only the first
// two of the four floats are touched.
inline void anchor_viewport(Anchor a, float x0, float x1, float k, float width, float vpa[]) {
    if (a == Anchor::Stretch || k == 1.0f) return;
    const float px = (vpa[0] + 1.0f) * 0.5f * width;
    vpa[0] = 2.0f * anchor_x(a, px, x0, x1, k) / width - 1.0f;
    vpa[1] *= k;
}

// A scissor rect's left and right edges (`left` inclusive, `right` exclusive, in EFB pixels)
// remapped, then taken to `scale` samples to a pixel and rounded the way GL picks samples. A
// sample is drawn when its center, i + 0.5, lies from the left edge up to but not at the right
// one, so an edge on a half sample rounds down: ceil(edge - 0.5). With no remap they come out
// as exactly left * scale and right * scale.
inline void anchor_scissor(Anchor a, float x0, float x1, float k, int scale, int* left, int* right) {
    const bool remap = a != Anchor::Stretch && k != 1.0f;
    const float l = remap ? anchor_x(a, (float)*left, x0, x1, k) : (float)*left;
    const float r = remap ? anchor_x(a, (float)*right, x0, x1, k) : (float)*right;
    *left = (int)std::ceil(l * (float)scale - 0.5f);
    *right = (int)std::ceil(r * (float)scale - 0.5f);
}

// What a draw is placed by, as a renderer keeps it: the anchor, its rect and k. Stretch, or
// a k of 1, is the one identity, which anchor_map gives with a zero rect so that two
// identities compare equal.
struct AnchorMap {
    Anchor a = Anchor::Stretch;
    float x0 = 0.0f, x1 = 0.0f, k = 1.0f;
    bool operator==(const AnchorMap& o) const { return a == o.a && x0 == o.x0 && x1 == o.x1 && k == o.k; }
};
inline AnchorMap anchor_map(Anchor a, float x0, float x1, float k) {
    if (a == Anchor::Stretch || k == 1.0f) return {};
    return {a, x0, x1, k};
}

// The front end's anchor state: the game's mark as the command stream has reached it, and
// GCN_ANCHOR_ALL's anchor, which takes the place of the mark over the whole frame when it is
// set. The front end is one thread, so this takes no lock.
class AnchorMark {
public:
    // The EFB is 640 pixels across.
    static constexpr float kFrameWidth = 640.0f;

    explicit AnchorMark(Anchor all = Anchor::Stretch) : all_(all) {}

    // The game's mark. The rect is kept as zero for Stretch, so that states with no anchor
    // compare equal whatever came before. False when the mark was already this one.
    bool set(Anchor a, float x0, float x1) {
        if (a == Anchor::Stretch) x0 = x1 = 0.0f;
        if (a == a_ && x0 == x0_ && x1 == x1_) return false;
        a_ = a;
        x0_ = x0;
        x1_ = x1;
        return true;
    }

    // The mark a draw taken now carries.
    void in_force(Anchor* a, float* x0, float* x1) const {
        if (all_ != Anchor::Stretch) {
            *a = all_;
            *x0 = 0.0f;
            *x1 = kFrameWidth;
        } else {
            *a = a_;
            *x0 = x0_;
            *x1 = x1_;
        }
    }

    // A texture copy made in `frame`. The EFB it reads holds anchored draws already squeezed,
    // so it is counted when a mark is in force, and `*n` is the count so far. It returns true
    // for the first copy of a scene, which is a run of frames that each make one, so a game
    // that does it every frame is told once.
    bool copy_made(uint32_t frame, uint64_t* n) {
        if (a_ == Anchor::Stretch && all_ == Anchor::Stretch) return false;
        *n = ++copies_;
        const bool starts = last_frame_ == kNone || frame > last_frame_ + 1;
        last_frame_ = frame;
        return starts;
    }

private:
    static constexpr uint32_t kNone = UINT32_MAX;
    Anchor all_;
    Anchor a_ = Anchor::Stretch;
    float x0_ = 0.0f, x1_ = 0.0f;
    uint64_t copies_ = 0;
    uint32_t last_frame_ = kNone;
};

// The anchor's letter in the draw log: S, L, C or R.
inline char anchor_letter(Anchor a) {
    static const char kLetters[] = {'S', 'L', 'C', 'R'};
    return kLetters[(unsigned)a & 3];
}

// "L", "C" or "R" (either case) for GCN_ANCHOR_ALL. False for anything else, so a mistyped
// setting is refused rather than read as some other anchor.
inline bool parse_anchor(const char* s, Anchor* a) {
    if (!s || !s[0] || s[1]) return false;
    switch (s[0]) {
    case 'L': case 'l': *a = Anchor::Left; return true;
    case 'C': case 'c': *a = Anchor::Center; return true;
    case 'R': case 'r': *a = Anchor::Right; return true;
    }
    return false;
}

}  // namespace gx
