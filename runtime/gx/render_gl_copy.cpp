// EFB copies: the EFB copied into a texture in its GX format by the copy shader, written
// back to RAM when the game asks, and the clear that goes with a copy.
#include "render_gl_internal.h"

namespace gx {

// A copy the spray composites from. It neither scans out nor clears -- a clearing copy
// ends an off-screen pass, and a grab deliberately leaves the scene it lifted from intact
// -- and it is small: at speed the spray takes 32x32 and 64x64 rects, while the other
// partial copies in this game (the water reflection, the rect the submerged tint samples)
// are far larger, which is what keeps them out of this. See is_grab_tex().
static constexpr uint32_t kGrabMax = 64;
bool is_grab_copy(const EfbCopyCmd& c) {
    return !c.to_xfb && !c.clear && c.dst_w <= kGrabMax && c.dst_h <= kGrabMax;
}

// Presented frames so far; the number the frame dumps carry. Defined here because the
// copy dump below keys on it.
uint32_t g_present_count;
// A texture copy written to RAM (EfbCopyCmd::writeback), in GX's tiled layout: read back
// from the copy's texture, which the copy fbo still holds, one sample from the middle of
// each hardware pixel's block, and encoded as the format the game asked for. The hardware
// writes whole tiles; rows and columns past the rect repeat its edge here. The bytes go to
// copy_writeback_done, for the guest thread to put in RAM; a format this does not encode is
// handed over empty, so that nothing waits on it.
//
// The copy shader has already written each texel as the game will read it (kCopyFS), so a
// format is only a matter of packing: the one-byte formats (I8, A8, R8, G8, B8, and the
// depth bytes Z8, Z8M, Z8L, which GX copies by the same nibbles) hold their byte in every
// channel, in 8x4 tiles; the two-byte ones (IA8, RG8, GB8, and Z16 and Z16L as the last
// two) are read back as IA8, alpha then intensity, in 4x4 tiles. A depth copy into the
// colour formats is not encoded.
static void copy_writeback(const EfbCopyCmd& c) {
    const uint32_t fmt = c.format & 0xF, w = c.dst_w, h = c.dst_h, s = (uint32_t)g_scale;
    const bool one_byte = fmt == 1 || (fmt >= 7 && fmt <= 10);
    const bool two_byte = fmt == 3 || fmt == 11 || fmt == 12;
    const bool colour = fmt == 4 || fmt == 5 || fmt == 6;
    const uint32_t tw = one_byte ? 8 : 4;
    const uint32_t bw = (w + tw - 1) / tw, bh = (h + 3) / 4;
    const uint32_t bytes = fmt == 6 ? bw * bh * 64 : bw * bh * 32;
    const bool known = one_byte || two_byte || (colour && !c.depth);
    if (!known || (c.dest & 0x01FFFFFF) + bytes > RAM_SIZE) {
        static bool warned;
        if (!warned) { warned = true; fprintf(stderr, "[copy] write-back of format %X%s not done\n", c.format, c.depth ? " (depth)" : ""); }
        copy_writeback_done(c.writeback_ticket, {});
        return;
    }
    std::vector<uint8_t> px((size_t)w * s * h * s * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, (GLsizei)(w * s), (GLsizei)(h * s), GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    auto at = [&](uint32_t x, uint32_t y) {
        x = std::min(x, w - 1);
        y = std::min(y, h - 1);
        return &px[(((size_t)y * s + s / 2) * w * s + (size_t)x * s + s / 2) * 4];
    };
    std::vector<uint8_t> encoded(bytes);
    uint8_t* out = encoded.data();
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t* blk = out + ((size_t)by * bw + bx) * (fmt == 6 ? 64 : 32);
            if (one_byte) {
                for (uint32_t i = 0; i < 32; i++) blk[i] = at(bx * 8 + i % 8, by * 4 + i / 8)[0];
                continue;
            }
            for (uint32_t i = 0; i < 16; i++) {
                const uint8_t* p = at(bx * 4 + i % 4, by * 4 + i / 4);
                uint16_t v;
                if (two_byte) {
                    blk[2 * i] = p[3]; blk[2 * i + 1] = p[0];
                    continue;
                }
                if (fmt == 6) {  // RGBA8: AR pairs in the first half of the tile, GB in the second
                    blk[2 * i] = p[3]; blk[2 * i + 1] = p[0];
                    blk[32 + 2 * i] = p[1]; blk[32 + 2 * i + 1] = p[2];
                    continue;
                }
                if (fmt == 4) v = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
                else if (p[3] >= 0xE0) v = (uint16_t)(0x8000 | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3));
                else v = (uint16_t)(((p[3] >> 5) << 12) | ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4));
                blk[2 * i] = (uint8_t)(v >> 8);
                blk[2 * i + 1] = (uint8_t)v;
            }
        }
    copy_writeback_done(c.writeback_ticket, std::move(encoded));
}

void do_efb_copy(const EfbCopyCmd& c) {
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    set_logic_op_off();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    g_gl_cull = 0;
    glColorMask(1, 1, 1, 1);
    if (!c.to_xfb && c.tex_id) {
        // The same target redrawn each frame keeps its id, so reuse the texture it
        // already has rather than allocating and freeing one per copy per frame.
        GlTex t{};
        auto old = g_textures.find(c.tex_id);
        const bool reuse = old != g_textures.end() && old->second.efb &&
                           old->second.w == c.dst_w && old->second.h == c.dst_h;
        if (reuse) {
            t = old->second;
            glBindTexture(GL_TEXTURE_2D, t.tex);
        } else {
            glGenTextures(1, &t.tex);
            t.w = c.dst_w; t.h = c.dst_h; t.levels = 1; t.efb = true;
            glBindTexture(GL_TEXTURE_2D, t.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c.dst_w * g_scale, c.dst_h * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }
        // Set after the reuse path, which carries the previous copy's flags in.
        t.grab = is_grab_copy(c);
        t.depth = c.depth;
        glBindFramebuffer(GL_FRAMEBUFFER, g_copy_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        glViewport(0, 0, c.dst_w * g_scale, c.dst_h * g_scale);
        glUseProgram(g_copy_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_efb_color);
        glBindSampler(0, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_efb_depth);
        glBindSampler(1, 0);
        glUniform1i(g_copy_u_src, 0);
        glUniform1i(g_copy_u_depth, 1);
        // Destination texel row 0 = top of the source rect.
        float rect[4] = {(float)c.src_x / EFB_W, (float)c.src_y / EFB_H,
                         (float)(c.src_x + c.src_w) / EFB_W, (float)(c.src_y + c.src_h) / EFB_H};
        glUniform4fv(g_copy_u_rect, 1, rect);
        glUniform1i(g_copy_u_mode, (int)c.format | (c.depth ? 32 : 0));
        glUniform1i(g_copy_u_alpha, c.efb_alpha ? 1 : 0);
        glBindVertexArray(g_copy_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (c.writeback) copy_writeback(c);
        // GCN_COPYDUMP=<dir> with GCN_DRAWLOG=<frame>: every texture copy made in that
        // frame is written out as it was produced, twice -- its colour, and its alpha
        // as grey -- named by the frame, the copy's order in it and its texture id.
        {
            static const char* copydump = getenv_nonempty("GCN_COPYDUMP");
            static const uint32_t copydump_frame = getenv("GCN_DRAWLOG") ? (uint32_t)atoi(getenv("GCN_DRAWLOG")) : 0;
            static uint32_t copydump_n = 0;
            // GCN_COPYDUMP_SIZE=WxH dumps every copy of that size, in whatever frame: a copy
            // made once, at a moment that differs from run to run, is caught without a frame.
            static uint32_t size_w = 0, size_h = 0;
            static const bool size_parsed = [] {
                if (const char* e = getenv("GCN_COPYDUMP_SIZE")) sscanf(e, "%ux%u", &size_w, &size_h);
                return true;
            }();
            (void)size_parsed;
            const bool by_size = size_w && c.dst_w == size_w && c.dst_h == size_h;
            if (copydump && (by_size || (copydump_frame && g_present_count + 1 == copydump_frame))) {
                const int w = c.dst_w * g_scale, h = c.dst_h * g_scale;
                std::vector<uint8_t> px((size_t)w * h * 4), out((size_t)w * h * 4);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                char path[512];
                for (int pass = 0; pass < 2; pass++) {
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++) {
                            const uint8_t* q = &px[((size_t)y * w + x) * 4];
                            uint8_t* o = &out[((size_t)y * w + x) * 4];
                            if (pass == 0) { o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; }
                            else { o[0] = o[1] = o[2] = q[3]; }
                            o[3] = 255;
                        }
                    snprintf(path, sizeof(path), "%s/copy_%05u_%03u_tex%u_%s.png", copydump, g_present_count + 1, copydump_n,
                             c.tex_id, pass == 0 ? "rgb" : "alpha");
                    write_png(path, out.data(), w, h);
                }
                copydump_n++;
            }
        }
        if (!reuse && old != g_textures.end()) glDeleteTextures(1, &old->second.tex);
        g_textures[c.tex_id] = t;
        glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
        glBindVertexArray(g_vao);
    }
    if (c.clear) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
        glEnable(GL_SCISSOR_TEST);
        glScissor(c.src_x * g_scale, (EFB_H - c.src_y - c.src_h) * g_scale, c.src_w * g_scale, c.src_h * g_scale);
        GLbitfield bits = 0;
        // Alpha is cleared only on a format that has it, as on the console.
        const bool clear_alpha = c.clear_alpha && c.efb_alpha;
        glColorMask(c.clear_color, c.clear_color, c.clear_color, clear_alpha);
        if (c.clear_color || clear_alpha) bits |= GL_COLOR_BUFFER_BIT;
        if (c.clear_z) { bits |= GL_DEPTH_BUFFER_BIT; glDepthMask(GL_TRUE); }
        glClearColor(((c.clear_rgba >> 24) & 0xFF) / 255.0f, ((c.clear_rgba >> 16) & 0xFF) / 255.0f,
                     ((c.clear_rgba >> 8) & 0xFF) / 255.0f, (c.clear_rgba & 0xFF) / 255.0f);
        clear_depth(c.clear_z_value / 16777215.0f);
        if (bits) glClear(bits);
        glColorMask(1, 1, 1, 1);
    }
}

}  // namespace gx
