// The present path: display copies assembled into the XFBs, the finished frame blitted to
// the output and dumped, and the last one kept for a redraw while the game is paused.
#include "render_gl_internal.h"

namespace gx {

static int g_win_w, g_win_h;
void render_set_window_size(int w, int h) { g_win_w = w; g_win_h = h; }

const char* g_dump_dir = nullptr;
int g_dump_every = 0;
bool g_dump_output = false;
// The second pass of a stereo pair (render_execute_stereo_pair) presents the same frame
// again: it is not counted twice, and its dump is told apart by a suffix.
bool g_pair_second = false;
const char* g_dump_suffix = "";

// Writes the w by h rect at (x, y) of `fbo`, whose rows GL reads bottom up, as an opaque
// PNG named for `kind` and the presented frame.
static void dump_png(GLuint fbo, int x, int y, int w, int h, const char* kind) {
    std::vector<uint8_t> px((size_t)w * h * 4), flipped((size_t)w * h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int r = 0; r < h; r++) {
        memcpy(&flipped[(size_t)r * w * 4], &px[(size_t)(h - 1 - r) * w * 4], (size_t)w * 4);
        for (int i = 0; i < w; i++) flipped[((size_t)r * w + i) * 4 + 3] = 255;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%05u%s.png", g_dump_dir, kind, g_present_count, g_dump_suffix);
    write_png(path, flipped.data(), w, h);
}

static void dump_efb(const EfbCopyCmd& c) {
    dump_png(g_efb_fbo, c.src_x * g_scale, (EFB_H - c.src_y - c.src_h) * g_scale, c.src_w * g_scale,
             c.src_h * g_scale, "frame");
}

// Where the finished frame is blitted. 0 is the window's framebuffer; a VR frontend
// points this at one of its swapchain images instead.
GLuint g_output_fbo = 0;
// The most recent presented frame, so the output can be drawn again without the game
// having produced a new one (render_present_last): a copy of the texture it was shown from,
// and the rect of it that was shown, in EFB pixels. The EFB can't be read again later, as
// the display copy that presented it clears it. Every presented frame is kept, so the copy
// is always the newest one, but only for a frontend that asks (render_keep_last): it costs
// a texture of the frame's size at the internal scale, 5.4 MB for the whole EFB at scale 2
// and 86 MB at scale 8, and a blit into it at each present.
static GLuint g_last_tex, g_last_fbo;
static uint32_t g_last_tex_w, g_last_tex_h, g_last_rect[4];
static int g_last_scale;
static bool g_keep_last, g_have_last;
static void keep_last(GLuint src_fbo, uint32_t tex_w, uint32_t tex_h, uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh) {
    const GLint w = (GLint)(tex_w * g_scale), h = (GLint)(tex_h * g_scale);
    if (!g_last_fbo) {
        glGenTextures(1, &g_last_tex);
        glGenFramebuffers(1, &g_last_fbo);
    }
    if (tex_w != g_last_tex_w || tex_h != g_last_tex_h || g_scale != g_last_scale) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_last_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, g_last_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_last_tex, 0);
        g_last_tex_w = tex_w;
        g_last_tex_h = tex_h;
        g_last_scale = g_scale;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_last_fbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g_last_rect[0] = rx;
    g_last_rect[1] = ry;
    g_last_rect[2] = rw;
    g_last_rect[3] = rh;
    g_have_last = true;
}
// Blit a rect of a texture laid out like the EFB (guest row r at GL row h - 1 - r) to a
// w by h target, stretched to the output aspect and fitted with bars where the target's
// shape differs (fit_aspect).
static void blit_rect_to(GLuint fbo, int w, int h, GLuint src_tex, uint32_t tex_w, uint32_t tex_h, uint32_t rx,
                         uint32_t ry, uint32_t rw, uint32_t rh) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    set_logic_op_off();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    g_gl_cull = 0;
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const OutputRect fit = fit_aspect(w, h, output_aspect());
    glViewport(fit.x, fit.y, fit.w, fit.h);
    glUseProgram(g_blit_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glBindSampler(0, 0);
    glUniform1i(g_blit_u_src, 0);
    float rect[4] = {(float)rx / tex_w, (float)(ry + rh) / tex_h, (float)(rx + rw) / tex_w, (float)ry / tex_h};
    glUniform4fv(g_blit_u_rect, 1, rect);
    glBindVertexArray(g_copy_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(g_vao);
}

// --dump-output: what the window shows, drawn again into a target of the window's size and
// read back from there, since a hidden window's own framebuffer has nothing to read.
static void dump_output(GLuint src_tex, uint32_t tex_w, uint32_t tex_h, uint32_t rx, uint32_t ry, uint32_t rw,
                        uint32_t rh) {
    static GLuint tex, fbo;
    static int tw, th;
    if (g_win_w <= 0 || g_win_h <= 0) return;
    if (tw != g_win_w || th != g_win_h) {
        if (!fbo) {
            glGenTextures(1, &tex);
            glGenFramebuffers(1, &fbo);
        }
        tw = g_win_w;
        th = g_win_h;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }
    blit_rect_to(fbo, tw, th, src_tex, tex_w, tex_h, rx, ry, rw, rh);
    dump_png(fbo, 0, 0, tw, th, "output");
}

// The frame to the output, and to its own dump too when this frame is dumped.
static void blit_rect_to_output(GLuint src_tex, uint32_t tex_w, uint32_t tex_h, uint32_t rx, uint32_t ry,
                                uint32_t rw, uint32_t rh, bool dump) {
    blit_rect_to(g_output_fbo, g_win_w, g_win_h, src_tex, tex_w, tex_h, rx, ry, rw, rh);
    if (dump && g_dump_output) dump_output(src_tex, tex_w, tex_h, rx, ry, rw, rh);
}

// The external framebuffers. A game whose EFB cannot hold a whole frame (anti-aliased,
// the EFB is 640x264) renders it in halves and copies each to its place in the XFB, and
// may copy strips elsewhere besides; presenting each display copy as a frame showed the
// last strip. So display copies are assembled here into an image per XFB, keyed by the
// address range the copies land in, and the image is presented when every one of its
// rows has been written since it was last shown. The VI's timing registers say how many
// rows a frame has; a copy that covers them all presents at once, as before.
struct Xfb {
    uint32_t base, stride, h;
    GLuint tex, fbo;
    std::vector<uint8_t> written;
    uint32_t written_rows;
};
static std::vector<Xfb> g_xfbs;

static Xfb& xfb_for(uint32_t dest, uint32_t w, uint32_t stride, uint32_t h) {
    for (auto& x : g_xfbs)
        if (x.stride == stride && x.h == h && dest >= x.base && dest < x.base + stride * h) return x;
    Xfb x{};
    x.base = dest;
    x.stride = stride;
    x.h = h;
    glGenTextures(1, &x.tex);
    glBindTexture(GL_TEXTURE_2D, x.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)(w * g_scale), (GLsizei)(h * g_scale), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &x.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, x.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, x.tex, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    x.written.assign(h, 0);
    g_xfbs.push_back(x);
    if (g_xfbs.size() > 8) g_xfbs.erase(g_xfbs.begin());
    return g_xfbs.back();
}

// Each is sized at the scale it was made at, so a change of scale frees them all
// (render_set_internal_scale) and the next display copies make them again.
void xfbs_free() {
    for (const Xfb& x : g_xfbs) {
        glDeleteFramebuffers(1, &x.fbo);
        glDeleteTextures(1, &x.tex);
    }
    g_xfbs.clear();
}

static void dump_xfb(const Xfb& x, uint32_t w) { dump_png(x.fbo, 0, 0, w * g_scale, x.h * g_scale, "frame"); }

// A display copy into its XFB. Returns the XFB when the copy completed a frame of it.
static Xfb* xfb_write(const EfbCopyCmd& c) {
    uint32_t h = vi_display_height();
    if (!h || c.dst_h >= h) return nullptr;  // the copy is the whole frame: present it as such
    uint32_t stride = c.dst_w * 2;
    Xfb& x = xfb_for(c.dest, c.dst_w, stride, h);
    uint32_t row0 = (c.dest - x.base) / stride;
    uint32_t rows = std::min(c.dst_h, h - row0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, x.fbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(c.src_x * g_scale, (EFB_H - c.src_y - c.src_h) * g_scale, (c.src_x + c.src_w) * g_scale, (EFB_H - c.src_y) * g_scale,
                      0, (h - row0 - rows) * g_scale, c.dst_w * g_scale, (h - row0) * g_scale, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    for (uint32_t r = row0; r < row0 + rows; r++)
        if (!x.written[r]) { x.written[r] = 1; x.written_rows++; }
    if (x.written_rows < h) return nullptr;
    std::fill(x.written.begin(), x.written.end(), 0);
    x.written_rows = 0;
    return &x;
}

void present(const EfbCopyCmd& c) {
    // A copy that is part of a frame: into the XFB, and nothing to show unless it was the
    // last part.
    Xfb* xfb = nullptr;
    if (!g_pair_second) {
        uint32_t h = vi_display_height();
        if (h && c.dst_h < h) {
            xfb = xfb_write(c);
            if (!xfb) return;
        }
    }
    if (!g_pair_second) g_present_count++;
    // GCN_DUMP_RANGE=a-b restricts dumping to a window of presented frames, so a short
    // stretch can be captured every single frame. Flicker is only visible frame by frame.
    static int range_lo = -1, range_hi = -1;
    static bool range_parsed = false;
    if (!range_parsed) {
        range_parsed = true;
        if (const char* s = getenv_nonempty("GCN_DUMP_RANGE")) {
            range_lo = atoi(s);
            const char* dash = strchr(s, '-');
            range_hi = dash ? atoi(dash + 1) : range_lo;
        }
    }
    const bool in_range = range_lo < 0 || ((int)g_present_count >= range_lo &&
                                           (int)g_present_count <= range_hi);
    const bool dump = g_dump_dir && g_dump_every && in_range && g_present_count % g_dump_every == 0;
    if (xfb) {
        if (dump) dump_xfb(*xfb, c.dst_w);
        if (g_keep_last) keep_last(xfb->fbo, c.dst_w, xfb->h, 0, 0, c.dst_w, xfb->h);
        blit_rect_to_output(xfb->tex, c.dst_w, xfb->h, 0, 0, c.dst_w, xfb->h, dump);
        return;
    }
    if (dump) dump_efb(c);
    if (g_keep_last) keep_last(g_efb_fbo, EFB_W, EFB_H, c.src_x, c.src_y, c.src_w, c.src_h);
    blit_rect_to_output(g_efb_color, EFB_W, EFB_H, c.src_x, c.src_y, c.src_w, c.src_h, dump);
}

void render_keep_last(bool on) {
    g_keep_last = on;
    if (on) return;
    g_have_last = false;
    if (g_last_fbo) {
        glDeleteFramebuffers(1, &g_last_fbo);
        glDeleteTextures(1, &g_last_tex);
        g_last_fbo = g_last_tex = 0;
        g_last_tex_w = g_last_tex_h = 0;  // so that keeping again sizes a new texture
    }
}

// Through the same aspect fit as present(), at the window's size now, and never dumped.
bool render_present_last() {
    if (!g_have_last) return false;
    blit_rect_to_output(g_last_tex, g_last_tex_w, g_last_tex_h, g_last_rect[0], g_last_rect[1], g_last_rect[2],
                        g_last_rect[3], false);
    gl_state_invalidate();
    return true;
}

void render_set_output_fbo(unsigned fbo) { g_output_fbo = (GLuint)fbo; }
uint32_t present_count() { return g_present_count; }

}  // namespace gx
