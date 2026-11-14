// The clear-drawn font's checks: the `test_clear_draw` target. Runs with no window, GL, disc
// or game.
//
// runtime/gx/clear_draw.cpp is compiled here as its GL ES build, whose entry points are
// glad's pointers, and glScissor and glClear are pointed at a fake that records each rect a
// clear covers. The checks are where those rects land for a known string.
//
// usage: test_clear_draw
// Exits 0 only if every check passes.
#define GCN_GL_ES
#include "gx/clear_draw.cpp"
#include <cstdio>
#include <vector>

namespace {

struct Rect {
    int x, y, w, h;
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};
Rect g_scissor;
std::vector<Rect> g_cleared;

void GLAD_API_PTR scissor(GLint x, GLint y, GLsizei w, GLsizei h) { g_scissor = {x, y, w, h}; }
void GLAD_API_PTR clear(GLbitfield mask) {
    if (mask & GL_COLOR_BUFFER_BIT) g_cleared.push_back(g_scissor);
}

std::vector<Rect> draw(const char* s, int cx, int bottom, int px) {
    g_cleared.clear();
    gx::clear_text(s, cx, bottom, px);
    return g_cleared;
}

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

}  // namespace

int main() {
    glad_glScissor = scissor;
    glad_glClear = clear;

    // "L" alone at 2 px: 6 columns wide with its space, so it starts 6 px left of the centre.
    // Rows 0 to 5 are its stem, one pixel each, and row 6 (the bottom) one run of five.
    std::vector<Rect> want;
    for (int r = 0; r < 6; r++) want.push_back({94, 10 + (6 - r) * 2, 2, 2});
    want.push_back({94, 10, 10, 2});
    check(draw("L", 100, 10, 2) == want, "an L is its stem's pixels and one run along its bottom");

    // A character with no glyph draws nothing and still takes its place: "?L" is 12 columns,
    // so the L starts at the centre.
    const std::vector<Rect> moved = draw("?L", 100, 10, 2);
    check(moved.size() == 7 && moved[0].x == 100 && moved[6] == (Rect{100, 10, 10, 2}),
          "an unknown character is a space as wide as a glyph");

    // m's row 2 (0b11010, row 0 the top) is two runs, its row 4 (0b10101) three of one pixel.
    const std::vector<Rect> m = draw("m", 0, 0, 1);
    int row2 = 0, row4 = 0;
    for (const Rect& r : m) {
        if (r.y == 6 - 2) row2++;
        if (r.y == 6 - 4 && r.w == 1) row4++;
    }
    check(m.size() == 2 + 3 + 3 + 2 + 2 && row2 == 2 && row4 == 3, "each run of lit pixels is one clear");

    // Every character of the headset's two messages has a glyph, but the spaces.
    int missing = 0;
    for (const char* msg : {"Compiling shaders... (first time startup only)", "Loading shaders..."})
        for (const char* p = msg; *p; p++) {
            const char one[2] = {*p, 0};
            if (*p != ' ' && draw(one, 0, 0, 1).empty()) missing++;
        }
    check(missing == 0, "the loading screen's messages have every glyph they use");
    check(draw(" ", 0, 0, 1).empty(), "a space draws nothing");

    return g_failed ? 1 : 0;
}
