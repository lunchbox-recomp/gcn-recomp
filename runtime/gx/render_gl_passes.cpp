// What a batch's copies say about its passes: the size of the frame it displays, the
// off-screen passes an eye leaves out, the copies of the whole frame, and what the flat
// pass can leave out when it runs only to feed the eyes.
#include "render_gl_internal.h"

namespace gx {

// The size of the frame the game actually scans out, which is the display copy's. It is
// not the EFB's size: the EFB is 640x528 and the game displays 640x480 of it. A frame
// rendered in parts (see Xfb) is as wide as its copies and as tall as the VI's frame;
// its strips and parts are smaller than the frame they belong to.
void display_size(const Batch& b, uint32_t& w, uint32_t& h) {
    w = EFB_W;
    h = EFB_H;
    bool any = false;
    for (auto& c : b.cmds) {
        if (c.type != CmdType::EfbCopy || !c.copy.to_xfb) continue;
        if (!any || c.copy.dst_h > h) { w = c.copy.dst_w; h = c.copy.dst_h; }
        any = true;
    }
    const uint32_t vh = vi_display_height();
    if (any && vh > h) h = vh;
}

// Where the first part of a frame rendered in parts ends: the first partial display copy
// after a draw of the main scene (`skip` marks the off-screen passes). Each part draws the
// same scene through its own window of the frame, and an eye has no window, so it draws
// the first part's commands and leaves the rest out; the flat pass still runs them all.
// Strips copied before the first part, left over from the previous frame, have no draws in
// front of them. Returns the command count when the frame is not in parts.
size_t first_part_end(const Batch& b, const std::vector<uint8_t>& skip) {
    const uint32_t vh = vi_display_height();
    bool drew = false;
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const Cmd& c = b.cmds[i];
        if (c.type == CmdType::Draw && !skip[i]) drew = true;
        if (drew && c.type == CmdType::EfbCopy && c.copy.to_xfb && vh && c.copy.dst_h < vh) return i;
    }
    return b.cmds.size();
}

// A copy that takes the whole displayed frame is the game compositing its finished
// image: a post pass reads the EFB out and draws it straight back as a screen-filling
// quad. Smaller copies are real scene content -- the water reflection, the sprite sheet
// the spray uses -- and the eye pass needs them.
//
// A copy of depth is never that, whatever its size: an eye's grab is of colour and cannot
// stand in for it. A shadow map rendered from a light at the screen's size and copied out
// as depth is a render target like any other, and the eyes must sample the flat pass's;
// taken for the frame, its pass was replayed from each eye and the eye's own colour grab
// sampled in its place, which lit every surface it fell on a solid colour.
bool is_fullscreen_copy(const EfbCopyCmd& c, uint32_t dw, uint32_t dh) {
    return !c.to_xfb && !c.depth && c.dst_w >= dw && c.dst_h >= dh;
}

// Marks the commands an eye must not replay.
//
// An off-screen pass draws into a target and then copies it out -- a reflection, the
// sheet the spray uses. Its draws are in that target's space, so re-aiming them at an
// eye is meaningless, and the first eye has already produced all of them against the
// EFB. Everything else -- the main scene, the composite over it, the HUD -- is replayed.
//
// What marks such a pass is that its copy *clears* the EFB, since the next pass needs it
// empty. A copy that does not clear is a grab: the game lifting a piece of the live
// scene to texture with, which is how the spray is done -- at speed it takes some fifty
// 32x32 and 64x64 rects from scattered screen positions, each preceded by no draws of
// its own. Treating every copy as ending a pass instead throws away whatever draws
// happen to sit in front of the first grab, and the water surface is among them: the
// ocean disappeared the moment the racer was fast enough to throw spray.
void mark_offscreen_passes(const Batch& b, std::vector<uint8_t>& skip) {
    uint32_t dw, dh;
    display_size(b, dw, dh);
    skip.assign(b.cmds.size(), 0);
    size_t pass_start = 0;
    for (size_t i = 0; i < b.cmds.size(); i++) {
        if (b.cmds[i].type != CmdType::EfbCopy) continue;
        const EfbCopyCmd& c = b.cmds[i].copy;
        if (!c.to_xfb && c.clear && !is_fullscreen_copy(c, dw, dh))
            for (size_t j = pass_start; j <= i; j++) skip[j] = 1;
        pass_start = i + 1;
    }
}

// Texture ids holding a copy of a whole frame.
//
// A draw sampling one is screen-space, so in an eye it is a flat billboard rather than
// something in the world -- it leaves a faint rectangular seam where its edges fall. It
// is tempting to drop those draws, but in this game the water surface is one of them:
// dropping it leaves the seabed showing through bare sand instead of blue-green water,
// which is far worse than the seam. GCN_EYE_SKIPCOMP drops them anyway, for comparing.
static std::vector<uint32_t> g_fullscreen_tex;

// These ids are stable across frames -- an EFB copy keeps the id its destination address
// was registered under -- so the set only ever needs adding to.
void note_fullscreen_copies(const Batch& b) {
    uint32_t dw, dh;
    display_size(b, dw, dh);
    for (auto& c : b.cmds) {
        if (c.type != CmdType::EfbCopy || !is_fullscreen_copy(c.copy, dw, dh) || !c.copy.tex_id)
            continue;
        bool known = false;
        for (uint32_t id : g_fullscreen_tex) known |= (id == c.copy.tex_id);
        if (!known) g_fullscreen_tex.push_back(c.copy.tex_id);
    }
}

bool is_fullscreen_tex(uint32_t id) {
    for (uint32_t t : g_fullscreen_tex)
        if (t == id) return true;
    return false;
}

bool samples_fullscreen_copy(const PixelState& st) {
    for (int i = 0; i < 8; i++)
        if (st.tex_is_efb[i] && st.tex_id[i] && is_fullscreen_tex(st.tex_id[i])) return true;
    return false;
}

bool samples_grab_copy(const PixelState& st) {
    for (int i = 0; i < 8; i++)
        if (st.tex_is_efb[i] && st.tex_id[i] && is_grab_tex(st.tex_id[i])) return true;
    return false;
}

// Which draws the flat pass can leave out when it runs only to feed the eyes.
//
// In stereo the flat pass exists for the render-to-texture copies: the eyes sample them,
// and an eye never draws into the EFB. The picture it paints is never shown. So it need not
// draw past the last copy whose contents something will read -- the main scene after that
// point is drawn by each eye anyway, and here it was a thousand draw calls a frame, a third
// of the render thread, spent on pixels that went straight to the next clear.
//
// What is read: every copy an off-screen pass samples (those draws run only here), and
// every copy an eye samples as copied. An eye takes over the lookups of a whole-frame copy
// and of the small grabs the spray uses, once it has a grab of its own to substitute
// (is_eye_screen_tex), and those need no flat scene behind them. A copy with no reader in
// this frame is kept all the same: a game can copy the scene once and sample it for the
// frames that follow, so an unread copy is not an unneeded one. Only a whole-frame copy or a
// grab the eye would substitute for is taken as unread. GCN_EYE_FULLFLAT=1 draws the whole
// flat pass as before.
//
// `skip` is mark_offscreen_passes' answer; `out` gets one byte per command, 1 to leave out.
// Empty when nothing is to be left out.
void flat_pass_trim(const Batch& b, const std::vector<uint8_t>& skip, std::vector<uint8_t>& out) {
    out.clear();
    static const bool full = getenv_nonempty("GCN_EYE_FULLFLAT") != nullptr;
    if (full) return;
    uint32_t dw, dh;
    display_size(b, dw, dh);
    // Copy textures read as copied this frame. A frame has a handful of copies, so a list.
    std::vector<uint32_t> read;
    auto note_read = [&](uint32_t id) {
        for (uint32_t r : read) if (r == id) return;
        read.push_back(id);
    };
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const Cmd& c = b.cmds[i];
        if (c.type != CmdType::Draw) continue;
        const PixelState& st = b.states[c.state];
        // Whether an eye substitutes its grab in this draw: the same test apply_state makes
        // for screen_uv, which then takes over every eye-screen texture the draw binds.
        bool subst = false;
        if (!skip[i]) {
            const uint32_t nstg = ((st.bp[0x00] >> 10) & 15) + 1;
            for (uint32_t s = 0; s < nstg && !subst; s++) {
                const uint32_t order = st.bp[0x28 + s / 2] >> ((s & 1) * 12);
                const uint32_t map = order & 7;
                subst = (order & 0x40) && st.tex_is_efb[map] && st.tex_id[map] &&
                        is_eye_screen_tex(st.tex_id[map]);
            }
        }
        for (int m = 0; m < 8; m++) {
            if (!st.tex_is_efb[m] || !st.tex_id[m]) continue;
            if (subst && is_eye_screen_tex(st.tex_id[m])) continue;
            note_read(st.tex_id[m]);
        }
    }
    size_t last = 0;
    bool any = false;
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const Cmd& c = b.cmds[i];
        if (c.type != CmdType::EfbCopy || c.copy.to_xfb) continue;
        const EfbCopyCmd& cc = c.copy;
        bool needed = true;
        if (cc.tex_id && !cc.depth &&
            ((g_eye_grab.tex && is_fullscreen_copy(cc, dw, dh)) || (g_spray_grab.tex && is_grab_copy(cc)))) {
            needed = false;
            for (uint32_t r : read) needed |= r == cc.tex_id;
        }
        if (needed) { last = i; any = true; }
    }
    const size_t from = any ? last + 1 : 0;
    bool trimmed = false;
    for (size_t i = from; i < b.cmds.size(); i++)
        if (b.cmds[i].type == CmdType::Draw && !skip[i]) trimmed = true;
    if (!trimmed) return;
    out.assign(b.cmds.size(), 0);
    for (size_t i = from; i < b.cmds.size(); i++)
        if (b.cmds[i].type == CmdType::Draw && !skip[i]) out[i] = 1;
}

}  // namespace gx
