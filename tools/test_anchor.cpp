// Draw anchor checks: the `test_anchor` target. Runs with no window, GL, disc or game.
//
// runtime/gx/anchor.h is the arithmetic behind gx::set_draw_anchor: where an x goes when a
// game marks a draw left, center or right, and the state the front end keeps for the mark.
// At 4:3, and in an eye of a headset, every remap must be the identity down to the bit, so
// that nothing the game draws moves until the output is wider. Otherwise a left anchor holds
// the rect's left edge, a right anchor its right edge and a centered one its middle, whatever
// the squeeze. The viewport uniform the vertex shader takes must land a point where the
// remapped x does, and so must the scissor's edges, or a draw would be cut off short of its
// own picture. After the picture is stretched to the output aspect, an element must sit at
// its 4:3 distance from its edge.
//
// usage: test_anchor
// Exits 0 only if every check passes.
#include "gx/anchor.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

using gx::Anchor;

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

bool same_bits(float a, float b) { return memcmp(&a, &b, sizeof(float)) == 0; }
bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

const Anchor kAnchors[] = {Anchor::Left, Anchor::Center, Anchor::Right};

struct Rect {
    float x0, x1;
};
// The whole frame, the halves and quarters a split view's HUD is anchored to, and an odd one.
const Rect kRects[] = {{0, 640}, {0, 320}, {320, 640}, {0, 160}, {160, 320}, {480, 640}, {100, 540}};

// 4:3 over 16:9, 16:10, 21:9 and 32:9, and one above 1 for a window narrower than 4:3.
const float kSqueezes[] = {0.75f, 0.8333333f, 0.5714286f, 0.375f, 1.25f};

}  // namespace

int main() {
    // k.
    const float wide = 16.0f / 9.0f;
    check(gx::anchor_scale(4.0f / 3.0f, false) == 1.0f && gx::anchor_scale((float)4 / (float)3, false) == 1.0f &&
              gx::anchor_scale((float)8 / (float)6, false) == 1.0f,
          "k is exactly 1 at 4:3, however the ratio is written");
    check(near(gx::anchor_scale(wide, false), 0.75f, 1e-6f) && near(gx::anchor_scale(21.0f / 9.0f, false), 4.0f / 7.0f, 1e-6f) &&
              near(gx::anchor_scale(32.0f / 9.0f, false), 0.375f, 1e-6f),
          "k is (4/3) over the aspect: 0.75 at 16:9, 4/7 at 21:9, 0.375 at 32:9");
    check(gx::anchor_scale(wide, true) == 1.0f && gx::anchor_scale(32.0f / 9.0f, true) == 1.0f,
          "k is 1 in an eye at any output aspect");
    check(gx::anchor_scale(0.0f, false) == 1.0f && gx::anchor_scale(-1.0f, false) == 1.0f &&
              gx::anchor_scale(NAN, false) == 1.0f,
          "an output aspect that is not a shape gives k = 1");

    // Identity at k = 1: bit for bit, for x, the viewport, the scissor and the map.
    long moved = 0, n = 0;
    for (Anchor a : kAnchors)
        for (const Rect& r : kRects)
            for (float x = -50.0f; x < 700.0f; x += 0.37f) {
                n++;
                if (!same_bits(gx::anchor_x(a, x, r.x0, r.x1, 1.0f), x)) moved++;
            }
    printf("  %ld x values, %ld moved\n", n, moved);
    check(moved == 0, "at k = 1 every anchor returns x itself, bit for bit");
    const float vps[][4] = {{0.0f, 1.0f, -0.25f, 0.5f}, {-0.5f, 0.5f, 0.0f, -1.0f}, {0.1f, 0.3333333f, 0.7f, 0.2f}};
    long vp_moved = 0;
    for (Anchor a : kAnchors)
        for (const auto& v : vps) {
            float vpa[4] = {v[0], v[1], v[2], v[3]};
            gx::anchor_viewport(a, 0.0f, 320.0f, 1.0f, 640.0f, vpa);
            if (memcmp(vpa, v, sizeof(vpa)) != 0) vp_moved++;
        }
    check(vp_moved == 0, "at k = 1 the viewport uniform is untouched, bit for bit");
    long cut = 0;
    for (Anchor a : kAnchors)
        for (int scale = 1; scale <= 4; scale++)
            for (int l = 0; l <= 640; l += 7) {
                int left = l, right = 640 - l / 2;
                gx::anchor_scissor(a, 0.0f, 640.0f, 1.0f, scale, &left, &right);
                if (left != l * scale || right != (640 - l / 2) * scale) cut++;
            }
    check(cut == 0, "at k = 1 the scissor edges are exactly their EFB pixels times the scale");
    check(gx::anchor_map(Anchor::Left, 0.0f, 320.0f, 1.0f) == gx::AnchorMap{} &&
              gx::anchor_map(Anchor::Right, 320.0f, 640.0f, 1.0f) == gx::AnchorMap{} &&
              gx::anchor_map(Anchor::Stretch, 0.0f, 640.0f, 0.5f) == gx::AnchorMap{} &&
              gx::anchor_map(Anchor::Left, 0.0f, 320.0f, gx::anchor_scale(wide, true)) == gx::AnchorMap{},
          "Stretch, k = 1 and an eye all map to the one identity");
    check(!(gx::anchor_map(Anchor::Left, 0.0f, 320.0f, 0.75f) == gx::AnchorMap{}) &&
              !(gx::anchor_map(Anchor::Left, 0.0f, 320.0f, 0.75f) == gx::anchor_map(Anchor::Right, 0.0f, 320.0f, 0.75f)) &&
              !(gx::anchor_map(Anchor::Left, 0.0f, 320.0f, 0.75f) == gx::anchor_map(Anchor::Left, 0.0f, 640.0f, 0.75f)) &&
              !(gx::anchor_map(Anchor::Left, 0.0f, 320.0f, 0.75f) == gx::anchor_map(Anchor::Left, 0.0f, 320.0f, 0.5f)),
          "maps that differ in anchor, rect or k compare unequal");

    // The rules, with figures that are exact in a float: a rect of 100 to 540 squeezed by a half.
    check(gx::anchor_x(Anchor::Left, 300.0f, 100.0f, 540.0f, 0.5f) == 200.0f &&
              gx::anchor_x(Anchor::Right, 300.0f, 100.0f, 540.0f, 0.5f) == 420.0f &&
              gx::anchor_x(Anchor::Center, 300.0f, 100.0f, 540.0f, 0.5f) == 310.0f &&
              gx::anchor_x(Anchor::Stretch, 300.0f, 100.0f, 540.0f, 0.5f) == 300.0f,
          "a left anchor gives x0 + (x - x0)k, a right x1 - (x1 - x)k and a centered one scales about the middle");

    // Edges held.
    long lost = 0, crossed = 0;
    for (const Rect& r : kRects)
        for (float k : kSqueezes) {
            const float mid = 0.5f * (r.x0 + r.x1);
            if (gx::anchor_x(Anchor::Left, r.x0, r.x0, r.x1, k) != r.x0) lost++;
            if (gx::anchor_x(Anchor::Right, r.x1, r.x0, r.x1, k) != r.x1) lost++;
            if (gx::anchor_x(Anchor::Center, mid, r.x0, r.x1, k) != mid) lost++;
            // Squeezing (k below 1) pulls the far edge of the rect toward the held one.
            if (k < 1.0f) {
                if (!(gx::anchor_x(Anchor::Left, r.x1, r.x0, r.x1, k) < r.x1)) crossed++;
                if (!(gx::anchor_x(Anchor::Right, r.x0, r.x0, r.x1, k) > r.x0)) crossed++;
                if (!(gx::anchor_x(Anchor::Center, r.x0, r.x0, r.x1, k) > r.x0 &&
                      gx::anchor_x(Anchor::Center, r.x1, r.x0, r.x1, k) < r.x1))
                    crossed++;
            }
        }
    check(lost == 0, "a left anchor keeps x0, a right keeps x1 and a centered one keeps the middle, exactly");
    check(crossed == 0, "a squeeze moves the rect's other edges toward the one that is held");

    // A split view's rect: each quarter or half holds its own edge, not the screen's.
    check(gx::anchor_x(Anchor::Left, 320.0f, 0.0f, 320.0f, 0.75f) == 240.0f &&
              gx::anchor_x(Anchor::Right, 320.0f, 320.0f, 640.0f, 0.75f) == 400.0f &&
              gx::anchor_x(Anchor::Left, 320.0f, 320.0f, 640.0f, 0.75f) == 320.0f &&
              gx::anchor_x(Anchor::Left, 640.0f, 320.0f, 640.0f, 0.75f) == 560.0f &&
              gx::anchor_x(Anchor::Right, 640.0f, 320.0f, 640.0f, 0.75f) == 640.0f,
          "in a 4P view's rect (320 to 640) a right anchor holds 640 and takes 320 to 400, a left one holds 320");

    // The shape: stretched to the aspect, a draw is where it was at 4:3. An element x pixels
    // from the left edge of the picture is x / 640 of the way across, so its distance in units
    // of the window's height is x / 640 * aspect once stretched, and x / 640 * 4/3 at 4:3.
    long wrong = 0;
    const float aspects[] = {16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f, 32.0f / 9.0f, 1.0f};
    for (float aspect : aspects) {
        const float k = gx::anchor_scale(aspect, false);
        for (float x = 0.0f; x <= 640.0f; x += 5.0f) {
            const float left = gx::anchor_x(Anchor::Left, x, 0.0f, 640.0f, k);
            const float right = gx::anchor_x(Anchor::Right, x, 0.0f, 640.0f, k);
            const float center = gx::anchor_x(Anchor::Center, x, 0.0f, 640.0f, k);
            if (!near(left * aspect, x * 4.0f / 3.0f, 1e-2f)) wrong++;
            if (!near((640.0f - right) * aspect, (640.0f - x) * 4.0f / 3.0f, 1e-2f)) wrong++;
            if (!near((center - 320.0f) * aspect, (x - 320.0f) * 4.0f / 3.0f, 1e-2f)) wrong++;
        }
    }
    check(wrong == 0, "stretched to 16:9, 16:10, 21:9, 32:9 or 1:1, an anchored element sits at its 4:3 distance from its edge");

    // The scissor.
    int l = 0, r = 640;
    gx::anchor_scissor(Anchor::Right, 0.0f, 640.0f, 0.75f, 1, &l, &r);
    const bool right1 = l == 160 && r == 640;
    l = 0, r = 640;
    gx::anchor_scissor(Anchor::Left, 0.0f, 640.0f, 0.75f, 2, &l, &r);
    const bool left2 = l == 0 && r == 960;
    l = 0, r = 640;
    gx::anchor_scissor(Anchor::Center, 0.0f, 640.0f, 0.75f, 2, &l, &r);
    const bool center2 = l == 160 && r == 1120;
    l = 101, r = 103;
    gx::anchor_scissor(Anchor::Left, 0.0f, 640.0f, 0.75f, 3, &l, &r);
    const bool rounded = l == 227 && r == 232;  // 75.75 * 3 and 77.25 * 3 are 227.25 and 231.75
    l = 320, r = 640;
    gx::anchor_scissor(Anchor::Right, 320.0f, 640.0f, 0.75f, 1, &l, &r);
    const bool view = l == 400 && r == 640;
    check(right1 && left2 && center2 && rounded && view,
          "a full-frame scissor at k = 0.75 is 160 to 640 right, 0 to 960 left at two samples, 160 to 1120 centered, and a 4P view's stays in its view");
    long off = 0, backwards = 0;
    for (Anchor a : kAnchors)
        for (const Rect& rc : kRects)
            for (float k : kSqueezes)
                for (int scale = 1; scale <= 4; scale++)
                    for (int e = 0; e <= 640; e += 3) {
                        int left = e, right = e + 5;
                        gx::anchor_scissor(a, rc.x0, rc.x1, k, scale, &left, &right);
                        const long want_l = std::lround(gx::anchor_x(a, (float)e, rc.x0, rc.x1, k) * (float)scale);
                        const long want_r = std::lround(gx::anchor_x(a, (float)(e + 5), rc.x0, rc.x1, k) * (float)scale);
                        if (left != want_l || right != want_r) off++;
                        if (right < left) backwards++;
                    }
    check(off == 0 && backwards == 0,
          "a scissor edge is the remapped x times the scale rounded to a sample, and an edge never passes the other");

    // The viewport uniform: window x = vpa[0] + vpa[1] * (clip x / w), -1 to 1 across 640.
    // Remapped, it must put every point where anchor_x puts the pixel, which is what remapping
    // the two corners and everything between gives.
    const float width = 640.0f;
    float worst = 0.0f;
    long touched = 0;
    for (Anchor a : kAnchors)
        for (const Rect& rc : kRects)
            for (float k : kSqueezes)
                for (const auto& v : vps) {
                    float vpa[4] = {v[0], v[1], v[2], v[3]};
                    gx::anchor_viewport(a, rc.x0, rc.x1, k, width, vpa);
                    if (vpa[2] != v[2] || vpa[3] != v[3]) touched++;
                    for (float u = -1.0f; u <= 1.0f; u += 0.25f) {
                        const float before = (v[0] + v[1] * u + 1.0f) * 0.5f * width;
                        const float after = (vpa[0] + vpa[1] * u + 1.0f) * 0.5f * width;
                        worst = std::fmax(worst, std::fabs(after - gx::anchor_x(a, before, rc.x0, rc.x1, k)));
                    }
                }
    printf("  worst viewport error %.5f pixels\n", worst);
    check(worst < 0.01f, "the remapped viewport uniform lands every point where the remapped x does, corners included");
    check(touched == 0, "the viewport remap leaves y alone");

    // The front end's state.
    gx::AnchorMark m;
    Anchor a = Anchor::Right;
    float x0 = 1.0f, x1 = 1.0f;
    m.in_force(&a, &x0, &x1);
    check(a == Anchor::Stretch && x0 == 0.0f && x1 == 0.0f, "no mark is Stretch with a zero rect");
    check(m.set(Anchor::Left, 0.0f, 320.0f) && !m.set(Anchor::Left, 0.0f, 320.0f) && m.set(Anchor::Left, 0.0f, 160.0f) &&
              m.set(Anchor::Right, 0.0f, 160.0f),
          "a mark that changes the anchor or the rect says so, and the same one again does not");
    m.in_force(&a, &x0, &x1);
    check(a == Anchor::Right && x0 == 0.0f && x1 == 160.0f, "the mark in force is the last one set");
    check(m.set(Anchor::Stretch, 12.0f, 34.0f) && !m.set(Anchor::Stretch, 56.0f, 78.0f), "Stretch ends the mark, whatever rect it comes with");
    m.in_force(&a, &x0, &x1);
    check(a == Anchor::Stretch && x0 == 0.0f && x1 == 0.0f, "an ended mark leaves a zero rect, so states without one compare equal");
    gx::AnchorMark all(Anchor::Center);
    all.set(Anchor::Left, 0.0f, 320.0f);
    all.in_force(&a, &x0, &x1);
    check(a == Anchor::Center && x0 == 0.0f && x1 == 640.0f, "GCN_ANCHOR_ALL takes over the whole 640-pixel frame from the game's mark");

    // A copy made under an anchor.
    uint64_t count = 0;
    gx::AnchorMark c;
    check(!c.copy_made(5, &count) && count == 0, "a copy made with no anchor set is not counted");
    c.set(Anchor::Left, 0.0f, 640.0f);
    check(c.copy_made(10, &count) && count == 1, "the first copy made under an anchor is reported");
    const bool same_frame = !c.copy_made(10, &count) && count == 2;
    const bool next_frame = !c.copy_made(11, &count) && count == 3;
    const bool after_gap = c.copy_made(13, &count) && count == 4;
    const bool continued = !c.copy_made(14, &count) && count == 5;
    check(same_frame && next_frame, "more of them in that frame and the next belong to the same scene and are counted quietly");
    check(after_gap && continued, "one after a frame with none starts a new scene, and the frame after it continues it");
    c.set(Anchor::Stretch, 0.0f, 0.0f);
    check(!c.copy_made(15, &count) && count == 5, "a copy after the anchor ends is not counted");
    gx::AnchorMark all_copy(Anchor::Right);
    check(all_copy.copy_made(0, &count) && count == 1, "GCN_ANCHOR_ALL counts every copy as made under an anchor, frame 0 included");

    // GCN_ANCHOR_ALL and the log.
    Anchor parsed = Anchor::Stretch;
    check(gx::parse_anchor("L", &parsed) && parsed == Anchor::Left && gx::parse_anchor("c", &parsed) && parsed == Anchor::Center &&
              gx::parse_anchor("R", &parsed) && parsed == Anchor::Right,
          "GCN_ANCHOR_ALL reads L, C or R in either case");
    int accepted = 0;
    for (const char* s : {"", "LR", "S", "x", "1", "left", " L"})
        accepted += gx::parse_anchor(s, &parsed);
    check(accepted == 0 && !gx::parse_anchor(nullptr, &parsed), "anything else is refused");
    check(gx::anchor_letter(Anchor::Stretch) == 'S' && gx::anchor_letter(Anchor::Left) == 'L' &&
              gx::anchor_letter(Anchor::Center) == 'C' && gx::anchor_letter(Anchor::Right) == 'R',
          "the draw log names the anchors S, L, C and R");

    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
