// Draw anchors, the front end's half: the mark the command stream has reached, which the
// pixel states taken from there on carry (PixelState::anchor), and the log line for a texture
// copy made while one is set. The state and its rules are AnchorMark's (anchor.h), and the
// renderer applies the remap.
#include "anchor.h"
#include "gx.h"
#include "render.h"
#include <cstdio>
#include <cstdlib>

namespace gx {

namespace {

// GCN_ANCHOR_ALL=L, C or R anchors every draw over the whole frame, so the remap shows
// without a game hook. Read on first use, because a frontend may set its environment after
// static initialization.
AnchorMark& mark() {
    static AnchorMark m = [] {
        Anchor all = Anchor::Stretch;
        if (const char* s = getenv("GCN_ANCHOR_ALL"))
            if (!parse_anchor(s, &all)) fprintf(stderr, "GCN_ANCHOR_ALL is L, C or R, not \"%s\"\n", s);
        return AnchorMark(all);
    }();
    return m;
}

}  // namespace

void draw_anchor_reached(Anchor a, float x0, float x1) {
    if (mark().set(a, x0, x1)) g_state.pixel_dirty = true;
}

void anchor_stamp(PixelState& st) {
    Anchor a;
    mark().in_force(&a, &st.anchor_x0, &st.anchor_x1);
    st.anchor = (uint8_t)a;
}

void anchor_copy_made(uint32_t frame) {
    uint64_t n;
    if (mark().copy_made(frame, &n)) fprintf(stderr, "[anchor] copy %llu made under an anchor\n", (unsigned long long)n);
}

}  // namespace gx
