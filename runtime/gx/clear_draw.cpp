// See clear_draw.h.
#include "gx/clear_draw.h"
#include "gx/gl.h"
#include <cstdint>
#include <cstring>

namespace gx {
namespace {

struct Glyph {
    char c;
    uint8_t rows[7];  // top first; bit 4 is the leftmost column
};
const Glyph kGlyphs[] = {
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'a', {0b00000, 0b00000, 0b01110, 0b00001, 0b01111, 0b10001, 0b01111}},
    {'d', {0b00001, 0b00001, 0b01101, 0b10011, 0b10001, 0b10001, 0b01111}},
    {'e', {0b00000, 0b00000, 0b01110, 0b10001, 0b11111, 0b10000, 0b01110}},
    {'f', {0b00110, 0b01001, 0b01000, 0b11100, 0b01000, 0b01000, 0b01000}},
    {'g', {0b00000, 0b01111, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110}},
    {'h', {0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001}},
    {'i', {0b00100, 0b00000, 0b01100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'l', {0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'m', {0b00000, 0b00000, 0b11010, 0b10101, 0b10101, 0b10001, 0b10001}},
    {'n', {0b00000, 0b00000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001}},
    {'o', {0b00000, 0b00000, 0b01110, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'p', {0b00000, 0b00000, 0b11110, 0b10001, 0b11110, 0b10000, 0b10000}},
    {'r', {0b00000, 0b00000, 0b10110, 0b11001, 0b10000, 0b10000, 0b10000}},
    {'s', {0b00000, 0b00000, 0b01110, 0b10000, 0b01110, 0b00001, 0b11110}},
    {'t', {0b01000, 0b01000, 0b11100, 0b01000, 0b01000, 0b01001, 0b00110}},
    {'u', {0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b10011, 0b01101}},
    {'y', {0b00000, 0b00000, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110}},
    {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}},
    {'(', {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010}},
    {')', {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000}},
};

}  // namespace

void clear_text(const char* s, int cx, int bottom, int px) {
    const int n = (int)strlen(s);
    const int x0 = cx - n * 6 * px / 2;
    for (int i = 0; i < n; i++) {
        const Glyph* g = nullptr;
        for (const Glyph& gl : kGlyphs)
            if (gl.c == s[i]) g = &gl;
        if (!g) continue;
        for (int r = 0; r < 7; r++) {
            const int y = bottom + (6 - r) * px;
            for (int c = 0; c < 5;) {
                if (!((g->rows[r] >> (4 - c)) & 1)) { c++; continue; }
                int e = c;
                while (e < 5 && ((g->rows[r] >> (4 - e)) & 1)) e++;
                glScissor(x0 + (i * 6 + c) * px, y, (e - c) * px, px);
                glClear(GL_COLOR_BUFFER_BIT);
                c = e;
            }
        }
    }
}

void clear_rect(int x, int y, int w, int h, uint32_t rgb) {
    glClearColor((float)(rgb >> 16 & 0xFF) / 255.0f, (float)(rgb >> 8 & 0xFF) / 255.0f, (float)(rgb & 0xFF) / 255.0f, 1.0f);
    glScissor(x, y, w, h);
    glClear(GL_COLOR_BUFFER_BIT);
}

}  // namespace gx
