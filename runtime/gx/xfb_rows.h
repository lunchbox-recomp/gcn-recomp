// Whether a display copy finishes a frame. A game whose EFB cannot hold a whole frame renders
// it in parts and copies each to its place in the XFB (render_gl.cpp assembles them, see Xfb
// there); ending a batch at every part handed a frontend several batches per game frame, and
// one that takes a batch per display frame ran the game at a fraction of its speed, while an
// eye drew each part as a frame of its own. So the rows each XFB has had written are tracked
// here as the renderer tracks them, and only the copy that completes one, or a copy of the
// whole frame, ends a frame. Copies to an address that never fills up (strips to a scratch
// buffer) then simply ride along in the next frame.
//
// A frame's batch ends there (xf.cpp), and a pause may hold the guest there (pause.h). The
// command parser and the guest thread's skim each keep their own (cmd.cpp), since with the
// front end on its own thread they see the same copies at different times.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace gx {

struct XfbRows {
    struct Xfb {
        uint32_t base, stride, h, n;
        std::vector<uint8_t> written;
    };
    std::vector<Xfb> xfbs;

    // A display copy `w` pixels wide and `h` rows high to `dest`, with the VI showing
    // `display_h` rows.
    bool ends_frame(uint32_t dest, uint32_t w, uint32_t h, uint32_t display_h) {
        if (!display_h || h >= display_h) return true;
        const uint32_t stride = w * 2;
        Xfb* x = nullptr;
        for (auto& r : xfbs)
            if (r.stride == stride && r.h == display_h && dest >= r.base && dest < r.base + stride * display_h) x = &r;
        if (!x) {
            xfbs.push_back({dest, stride, display_h, 0, std::vector<uint8_t>(display_h, 0)});
            if (xfbs.size() > 8) xfbs.erase(xfbs.begin());
            x = &xfbs.back();
        }
        const uint32_t row0 = (dest - x->base) / stride;
        const uint32_t rows = std::min(h, display_h - row0);
        for (uint32_t r = row0; r < row0 + rows; r++)
            if (!x->written[r]) { x->written[r] = 1; x->n++; }
        if (x->n < display_h) return false;
        std::fill(x->written.begin(), x->written.end(), 0);
        x->n = 0;
        return true;
    }

    // The display copy the BP registers `bp` describe, read as renderer_efb_copy reads them:
    // its size in 0x4A, halved by 0x52's bit 9, and its address in 0x4B.
    bool copy_ends_frame(const uint32_t* bp, uint32_t display_h) {
        const bool half = (bp[0x52] >> 9) & 1;
        const uint32_t w = (bp[0x4A] & 0x3FF) + 1, h = ((bp[0x4A] >> 10) & 0x3FF) + 1;
        return ends_frame((bp[0x4B] & 0x1FFFFF) << 5, half ? w / 2 : w, half ? h / 2 : h, display_h);
    }
};

}  // namespace gx
