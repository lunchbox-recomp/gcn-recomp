// The menu's blurred backdrop: the held frame, blurred into a target a quarter of the window's
// size, drawn over the output ahead of the menu (render_overlay_blur in render_gl.h).
#include "render_gl_internal.h"

namespace gx {

// The menu's blurred backdrop (render_overlay_blur): two targets a quarter of the window's size,
// the blur ending in the first, and what it was made at, so a new hold, a resize, a new output
// aspect or a frame presented after the hold began makes it again. 4 MB of targets at 3840x2160,
// made at the warm-up (render_overlay_blur_warm) and kept until render_keep_last(false), which
// the desktop frontend never calls, so they're kept for the process and a hold's first frame
// allocates none. g_blur_tex_w by g_blur_tex_h is the size they were given.
static GLuint g_blur_tex[2], g_blur_fbo[2];
static int g_blur_tex_w, g_blur_tex_h;
static int g_blur_win_w, g_blur_win_h;
static float g_blur_aspect;
// The presented count it was made at. A frame the front end had queued still presents after the
// hold begins, since pause_gate holds the guest as a batch is read, ahead of its draw.
static uint32_t g_blur_present;
static bool g_blur_ready, g_blur_holding;

void blur_free() {
    if (g_blur_fbo[0]) glDeleteFramebuffers(2, g_blur_fbo);
    if (g_blur_tex[0]) glDeleteTextures(2, g_blur_tex);
    g_blur_fbo[0] = g_blur_fbo[1] = g_blur_tex[0] = g_blur_tex[1] = 0;
    g_blur_tex_w = g_blur_tex_h = 0;
    g_blur_ready = g_blur_holding = false;
}

// The two targets, made if there are none, and sized w by h if they aren't.
static void blur_targets(int w, int h) {
    if (!g_blur_tex[0]) {
        glGenTextures(2, g_blur_tex);
        glGenFramebuffers(2, g_blur_fbo);
    }
    if (w == g_blur_tex_w && h == g_blur_tex_h) return;
    for (int i = 0; i < 2; i++) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_blur_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, g_blur_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_blur_tex[i], 0);
    }
    g_blur_tex_w = w;
    g_blur_tex_h = h;
}

// The blur program in use, sampling `tex` on unit 0 with no sampler object.
static void blur_use(GLuint tex) {
    glUseProgram(g_blur_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, 0);
    glUniform1i(g_blur_u_src, 0);
}

static const float kWhole[] = {0, 0, 1, 1};  // a rect of the whole source

// A draw into one of the targets, `fbo`, w by h, with no blend, as the fit and the passes draw.
static void blur_target_state(GLuint fbo, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    set_logic_op_off();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    g_gl_cull = 0;
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, w, h);
}

// The held frame into the first target, w by h, fitted as the output is, each pixel the mean of
// four bilinear taps spread over the frame's texels it stands for, so a thin line can't fall
// between samples. Leaves the blur program in use and the copy VAO bound.
static void blur_fit_frame(int w, int h) {
    blur_target_state(g_blur_fbo[0], w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const OutputRect fit = fit_aspect(w, h, output_aspect());
    glViewport(fit.x, fit.y, fit.w, fit.h);
    blur_use(g_last_tex);
    glUniform1f(g_blur_u_alpha, 1.0f);
    // The kept copy is laid out like the EFB, guest row r at GL row h - 1 - r (blit_rect_to).
    const float tw = (float)g_last_tex_w, th = (float)g_last_tex_h;
    const float src[] = {g_last_rect[0] / tw, 1.0f - (g_last_rect[1] + g_last_rect[3]) / th,
                         (g_last_rect[0] + g_last_rect[2]) / tw, 1.0f - g_last_rect[1] / th};
    glUniform4fv(g_blur_u_rect, 1, src);
    // Each tap a quarter of the texels a target pixel spans out from its centre, each way, and at
    // least half a texel, so the four taps' bilinear footprints meet and cover them.
    const float span_x = (float)g_last_rect[2] * g_last_scale / std::max(fit.w, 1);
    const float span_y = (float)g_last_rect[3] * g_last_scale / std::max(fit.h, 1);
    glUniform2f(g_blur_u_step, std::max(span_x / 4, 0.5f) / (tw * g_last_scale),
                std::max(span_y / 4, 0.5f) / (th * g_last_scale));
    glBindVertexArray(g_copy_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// A pass of Kawase's series from target `pass` into the other, its taps `x` and `y` of the
// target out, with the blur program in use and the copy VAO bound.
static void blur_pass(int pass, float x, float y) {
    glBindFramebuffer(GL_FRAMEBUFFER, g_blur_fbo[1 - pass]);
    glBindTexture(GL_TEXTURE_2D, g_blur_tex[pass]);
    glUniform2f(g_blur_u_step, x, y);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// The two targets sized for the window, the held frame fitted into the first, then the first
// two passes of Kawase's series, each the mean of four bilinear taps on the diagonals, 0.5 and
// then 1.5 texels out, ending in the first. Wider steps leave gaps that ring around an edge.
static void blur_make() {
    const int w = std::max(1, (g_win_w + 3) / 4), h = std::max(1, (g_win_h + 3) / 4);
    blur_targets(w, h);
    blur_fit_frame(w, h);
    glViewport(0, 0, w, h);
    glUniform4fv(g_blur_u_rect, 1, kWhole);
    for (int pass = 0; pass < 2; pass++) {
        const float texels = pass ? 1.5f : 0.5f;
        blur_pass(pass, texels / w, texels / h);
    }
    glBindVertexArray(g_vao);
    g_blur_win_w = g_win_w;
    g_blur_win_h = g_win_h;
    g_blur_aspect = output_aspect();
    g_blur_present = g_present_count;
    g_blur_ready = true;
}

// The blur stretched over the output's first w by h pixels, the whole of it, bars too, as the
// held frames draw it, at `alpha` over what's there.
static void blur_draw(float alpha, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, g_output_fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    g_gl_cull = 0;
    set_logic_op_off();
    glColorMask(1, 1, 1, 1);
    glEnable(GL_BLEND);
    // The game's last batch may have left GX's subtract (apply_state), which would draw it black.
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    blur_use(g_blur_tex[0]);
    glUniform4fv(g_blur_u_rect, 1, kWhole);
    glUniform2f(g_blur_u_step, 0, 0);
    glUniform1f(g_blur_u_alpha, std::min(alpha, 1.0f));
    glBindVertexArray(g_copy_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(g_vao);
}

void render_overlay_blur_warm(int win_w, int win_h) {
    blur_targets(std::max(1, (win_w + 3) / 4), std::max(1, (win_h + 3) / 4));
    // A pass as blur_make draws one, blend off, from the first target into the second's corner pixel.
    blur_target_state(g_blur_fbo[1], 1, 1);
    blur_use(g_blur_tex[0]);
    glUniform1f(g_blur_u_alpha, 1.0f);
    glUniform4fv(g_blur_u_rect, 1, kWhole);
    glBindVertexArray(g_copy_vao);
    blur_pass(0, 0.5f, 0.5f);
    // The draw over the output, blend on, at alpha 0 over its corner pixel, which leaves it as it was.
    blur_draw(0.0f, 1, 1);
    gl_state_invalidate();
}

void render_overlay_blur(bool hold, float alpha) {
    if (!hold && alpha <= 0.0f) {
        g_blur_ready = g_blur_holding = false;  // the targets are kept for the next hold
        return;
    }
    if (hold && !g_blur_holding) g_blur_ready = false;  // each hold blurs its own frame
    g_blur_holding = hold;
    if (!g_have_last || g_win_w <= 0 || g_win_h <= 0) return;
    const bool remake = hold && (!g_blur_ready || g_blur_win_w != g_win_w || g_blur_win_h != g_win_h ||
                                 g_blur_aspect != output_aspect() || g_blur_present != g_present_count);
    if (remake) blur_make();
    if (g_blur_ready && alpha > 0.0f) blur_draw(alpha, g_win_w, g_win_h);
    if (remake || (g_blur_ready && alpha > 0.0f)) gl_state_invalidate();
}

}  // namespace gx
