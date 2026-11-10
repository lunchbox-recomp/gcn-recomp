// One eye's view of a batch (render_execute_eye): the flat pass that makes the frame's
// copies, then the batch re-projected into the eye's target, with the eye's own grabs
// standing in for the game's copies of the frame.
#include "render_gl_internal.h"

namespace gx {

// What the eye has drawn so far, standing in for the game's copy of the finished frame.
// There are two because the two users want different moments: the water refracts the
// scene as it stood *before* the water was drawn, and the spray refracts it after, water
// included. One texture serving both would quietly re-point the water's lookup -- and the
// game's own final composite, which samples the same whole-frame copy -- at whatever the
// spray grabbed later. See grab_eye().
// A third, g_restore_grab, is taken where the game copies part of the frame that an
// EyeRestore draw later puts back.
EyeGrab g_eye_grab, g_spray_grab, g_restore_grab;
int g_eye_w, g_eye_h;

// The water surface refracts by looking the finished frame up at the screen position the
// game computed for each of its vertices. In an eye those positions are the flat view's,
// and the copy is the flat view's too, so water the eye can see beyond the game's own
// 60-degree frustum samples off the edge of the copy and clamps -- the scene smeared down
// the sea in streaks, the racer among it.
//
// Nothing about that is fixable by moving the lookup around, because the data is not in
// the copy. So the eye grabs what it has drawn itself, which covers exactly what the eye
// can see, and the fragment samples it at its own position rather than at the flat view's.
// The indirect stage that ripples the lookup still applies on top, so the water keeps its
// wobble. Costs one full-target copy per eye, taken only on a frame that has such a
// draw -- and the spray wants the same thing at a later moment, so a frame throwing
// spray pays for two. See is_grab_tex().
static void grab_eye(EyeGrab& g, int w, int h) {
    // The copy must keep the target's encoding: GL ES refuses glCopyTexSubImage2D between
    // an sRGB attachment and a linear texture or the other way round, and a headset's eye
    // is an sRGB swapchain image (openxr/xr_frontend.cpp, create_swapchains). An sRGB grab
    // is then read without decoding (EXT_texture_sRGB_decode), so the shader sees the
    // same values it would from a linear one: the game's own, never converted.
    GLint encoding = GL_LINEAR, target = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
    if (target)  // the default framebuffer has no GL_COLOR_ATTACHMENT0 to ask about
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
    const bool srgb = encoding == GL_SRGB;
    if (!g.tex || g.w != w || g.h != h || g.srgb != srgb) {
        if (!g.tex) glGenTextures(1, &g.tex);
        glBindTexture(GL_TEXTURE_2D, g.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        if (srgb) {
            constexpr GLenum kTextureSrgbDecodeExt = 0x8A48, kSkipDecodeExt = 0x8A4A;
            glTexParameteri(GL_TEXTURE_2D, kTextureSrgbDecodeExt, kSkipDecodeExt);
        }
        g.srgb = srgb;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g.w = w;
        g.h = h;
    } else {
        glBindTexture(GL_TEXTURE_2D, g.tex);
    }
#ifndef GCN_GL_ES
    // A desktop eye target is an ordinary multisampled framebuffer, which cannot be copied
    // from, only resolved by a blit. (On GL ES the samples are the render-to-texture
    // extension's and the copy resolves them itself.) The blit honours the scissor; the
    // caller invalidates the state cache after a grab, which puts it back.
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLE_BUFFERS, &samples);
    if (samples > 0) {
        static GLuint resolve_fbo;
        if (!resolve_fbo) glGenFramebuffers(1, &resolve_fbo);
        GLint target = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.tex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)target);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)target);
        return;
    }
#endif
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
}

// The spray is composited from screen-space grabs -- the same mechanism as the water
// surface, one size down. At speed it lifts some fifty small rects out of the live scene
// and redraws each as a billboard sampling its own rect, so on hardware a droplet
// refracts exactly the pixels it covers and its edges cannot be seen. An eye re-projects
// the billboard but not the rect, which belongs to the flat view, so the patch agrees
// with nothing behind it and reads as a square with a piece of scene in it.
//
// So these lookups are taken over the way the water's are: the texture becomes the eye's
// own grab and the coordinate becomes the fragment's own position, which is the one place
// the pixels the droplet wants actually are. What survives of the game's own lookup is
// the indirect offset that distorts it -- the droplet's whole visible character, since
// the undistorted part is by construction the background it sits on.
//
// Unlike the whole-frame copies these go to addresses that rotate, so a fresh texture id
// appears for every sprite of every frame; a set of ids would grow without bound and be
// scanned per draw. The flag rides on the texture instead.
bool is_grab_tex(uint32_t id) {
    // GCN_EYE_SPRAY=0 restores the stale lookup, which is the only way to see from inside
    // the headset what this changed.
    static const bool off = getenv_nonempty("GCN_EYE_SPRAY") && atoi(getenv_nonempty("GCN_EYE_SPRAY")) == 0;
    if (off || !id) return false;
    auto it = g_textures.find(id);
    return it != g_textures.end() && it->second.grab;
}

// A copy an EyeRestore draw puts back, while an eye is drawing one (g_cur_restore): only
// that draw's lookups are taken over, at the grab taken where the copy was made.
static bool is_restore_tex(uint32_t id) {
    if (!g_cur_restore || !g_restore_grab.tex || !id) return false;
    for (uint32_t t : g_restore_tex)
        if (t == id) return true;
    return false;
}

// Whether an eye takes this texture's lookups over at all: a copy of the whole frame
// (the water surface samples one), one of the spray's grabs, or a copy put back by an
// EyeRestore draw.
bool is_eye_screen_tex(uint32_t id) {
    return (g_eye_grab.tex && is_fullscreen_tex(id)) || (g_spray_grab.tex && is_grab_tex(id)) ||
           is_restore_tex(id);
}
const EyeGrab& eye_grab_for(uint32_t id) {
    return is_restore_tex(id) ? g_restore_grab : is_grab_tex(id) ? g_spray_grab : g_eye_grab;
}

// The same plane as an eye sees the triangle: the eye's projection and its view of the
// world, the depth mapped into the draw's band as the vertex shader maps it, in the eye
// target's window coordinates. A sky drawn with z freeze after a triangle culled at the
// far end of the frustum otherwise keeps its own depth in an eye, a hundred units out
// around the camera, and hides everything beyond it. A triangle placed by the camera (its
// positions in view space) is seen through the eye's view of the camera, as apply_state
// draws it. A 2D draw sets none (w = 0), and the draws that follow keep their own depth.
static bool zfreeze_plane_eye(const Batch& b, const Cmd& c, float out[4]) {
    out[3] = 0.0f;
    if (c.prim != 0 || c.count < 3) return false;
    const PixelState& st = b.states[c.state];
    if ((int)st.proj[6] != 0) return false;
    float M[16];
    mat4_mul(g_vr_proj, st.view_space ? g_vr_view : g_vr_view_world, M);
    const float band_lo = (st.viewport[5] - fabsf(st.viewport[2])) / 16777215.0f;
    const float band_hi = st.viewport[5] / 16777215.0f;
    double X[3], Y[3], Z[3];
    for (int i = 0; i < 3; i++) {
        const float* v = b.verts[b.indices[c.first + c.count - 3 + i]].pos;
        float clip[4];
        for (int r = 0; r < 4; r++) clip[r] = M[r] * v[0] + M[4 + r] * v[1] + M[8 + r] * v[2] + M[12 + r];
        if (clip[3] <= 0.0f) return false;
        const double zn = (band_hi + band_lo - 1.0) + (band_hi - band_lo) * (clip[2] / clip[3]);
        X[i] = (clip[0] / clip[3] + 1.0) * 0.5 * g_eye_w;
        Y[i] = (clip[1] / clip[3] + 1.0) * 0.5 * g_eye_h;
        Z[i] = (zn + 1.0) * 0.5;
    }
    const double ux = X[1] - X[0], uy = Y[1] - Y[0], uz = Z[1] - Z[0];
    const double wx = X[2] - X[0], wy = Y[2] - Y[0], wz = Z[2] - Z[0];
    const double det = ux * wy - uy * wx;
    if (fabs(det) < 1e-9) return false;
    const double a = (uz * wy - uy * wz) / det, bb = (ux * wz - uz * wx) / det;
    out[0] = (float)a;
    out[1] = (float)bb;
    out[2] = (float)(Z[0] - a * X[0] - bb * Y[0]);
    out[3] = 1.0f;
    return true;
}

// A late object (EyeLateObject) the game draws where its window covers the screen: on the
// screen the window magnifies it, and it reads as part of the window. In an eye the window
// is a panel near the viewer (EyeFacing, on the HUD frame) and the object stands in the
// world behind it, metres further off; seen through the panel's outline, at the panel's
// place on the screen but far behind it, it is hard on the eyes. So an object whose origin
// the game projects inside the facing panels' box is scaled about the viewer onto the
// panel's plane: the same direction and the same angular size, at the panel's depth.
// Returns false, leaving `out` alone, for an object outside the box, one nearer than the
// panel, or a frame with no facing panel.
static bool late_panel_view(const Batch& b, const Cmd& c, float out[16]) {
    const PixelState& st = b.states[c.state];
    if (!g_has_facing_box || (int)st.proj[6] != 0) return false;
    // The object's origin in the game's view space: its position matrix's translation,
    // or, for one drawn with the identity, the middle of its vertices.
    float p[3] = {0, 0, 0};
    if ((size_t)c.mtx * 12 + 12 <= b.mtxs.size()) {
        const float* m = &b.mtxs[(size_t)c.mtx * 12];
        p[0] = m[3]; p[1] = m[7]; p[2] = m[11];
    }
    if (p[0] * p[0] + p[1] * p[1] + p[2] * p[2] < 1e-6f && c.vcount) {
        for (uint32_t v = 0; v < c.vcount; v++)
            for (int k = 0; k < 3; k++) p[k] += b.verts[c.vfirst + v].pos[k];
        for (int k = 0; k < 3; k++) p[k] /= (float)c.vcount;
    }
    if (p[2] >= -1e-3f) return false;
    // Where the game's perspective puts it: x' = p0*x + p1*z, y' = p2*y + p3*z, w = -z.
    const float x = (st.proj[0] * p[0] + st.proj[1] * p[2]) / -p[2];
    const float y = (st.proj[2] * p[1] + st.proj[3] * p[2]) / -p[2];
    const float* r = g_facing_box;
    if (x < r[0] || x > r[1] || y < r[2] || y > r[3]) return false;
    // The panel's plane, as facing_frame places it: square on to the direction of the
    // box's centre on the HUD frame, as far out as the frame's centre.
    const float* F = g_vr_hud_eff;
    const float cx = 0.5f * (r[0] + r[1]), cy = 0.5f * (r[2] + r[3]);
    float n[3] = {F[0] * cx + F[4] * cy + F[12], F[1] * cx + F[5] * cy + F[13], F[2] * cx + F[6] * cy + F[14]};
    const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    const float d = sqrtf(F[12] * F[12] + F[13] * F[13] + F[14] * F[14]);
    if (nl <= 0.0f || d <= 0.0f) return false;
    for (float& v : n) v /= nl;
    // The origin where the eyes see it (g_world_xform takes the game's view space there),
    // and the scale about the viewer that puts it on the plane.
    const float* W = g_world_xform;
    float q[3];
    for (int k = 0; k < 3; k++) q[k] = W[k] * p[0] + W[4 + k] * p[1] + W[8 + k] * p[2] + W[12 + k];
    const float along = n[0] * q[0] + n[1] * q[1] + n[2] * q[2];
    if (along <= d) return false;
    const float s = d / along;
    const float S[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1};
    float SW[16];
    mat4_mul(S, g_world_xform, SW);
    mat4_mul(g_vr_view, SW, out);
    return true;
}

// Draw one eye's view of a batch into `fbo`.
//
// The vertex buffer, the CPU-side transform in xf.cpp and any render-to-texture results
// are all shared between the eyes -- only the uniforms and the draw calls are repeated,
// which is what makes stereo affordable here. Pass do_copies for the first eye only;
// the second reuses what it produced.
bool render_execute_eye(Batch& b, unsigned fbo, int w, int h, bool do_copies) {
    // The scene samples textures the game produces by copying them back out of the EFB:
    // the water reflection, the sprite sheet the spray uses. An eye pass never draws into
    // the EFB, so on its own it would copy out an empty one -- which once left a model
    // untextured and put a black quad on the water. So the first eye runs the whole frame
    // flat into the EFB exactly as the hardware would, minus the scanout, and the eyes
    // then re-project only the main scene on top of correct textures.
    static std::vector<uint8_t> skip, trim;
    g_eye_frame = true;
    apply_view_turn(b);
    mark_offscreen_passes(b, skip);
    const auto t_start = std::chrono::steady_clock::now();
    double ms_flat = 0.0;
    size_t n_trimmed = 0;
    if (do_copies) {
        frame_observe(b, &skip);
        // Where the eye stands and what it leaves out are read off the batch, so they
        // are settled before anything is drawn from it -- the flat pass included, which
        // leaves the hidden draws out of the EFB too.
        eye_prepare(b, skip);
        compose_world_view();
        g_vr_active = false;
        flat_pass_trim(b, skip, trim);
        for (uint8_t t : trim) n_trimmed += t;
        execute_batch(b, false, trim.empty() ? nullptr : &trim);  // which also notes this batch's whole-frame copies
        if (frametime_on()) ms_flat = ms_since(t_start);
    }
    // GCN_EYELOG=1 reports how a frame was split, which is the only way to tell a scene
    // rendered at the wrong field of view from a composite quad standing in for one.
    static const bool eyelog = getenv_nonempty("GCN_EYELOG") != nullptr;
    int n_drawn = 0, n_skipped = 0, n_spray = 0;
    if (eyelog && do_copies) {
        uint32_t dw, dh;
        display_size(b, dw, dh);
        fprintf(stderr, "[eye] f%u cmds=%zu display=%ux%u\n", g_render_frame, b.cmds.size(),
                dw, dh);
        size_t pass_start = 0;
        for (size_t i = 0; i < b.cmds.size(); i++) {
            if (b.cmds[i].type != CmdType::EfbCopy) continue;
            int nd = 0;
            for (size_t j = pass_start; j < i; j++) nd += b.cmds[j].type == CmdType::Draw;
            const EfbCopyCmd& cc = b.cmds[i].copy;
            fprintf(stderr, "[eye]   copy %ux%u xfb=%d tex=%u fmt=%02X%s full=%d draws=%d clr=%d%d src=%u,%u+%ux%u %s\n",
                    cc.dst_w, cc.dst_h, (int)cc.to_xfb, cc.tex_id, cc.format, cc.depth ? " depth" : "",
                    (int)is_fullscreen_copy(cc, dw, dh), nd, (int)cc.clear,
                    (int)cc.clear_color, cc.src_x, cc.src_y, cc.src_w, cc.src_h,
                    skip[i] ? "SKIP" : "replay");
            pass_start = i + 1;
        }
    }

    g_vr_active = true;
    g_eye_w = w;
    g_eye_h = h;
    memset(g_zplane, 0, sizeof(g_zplane));  // the flat pass's planes are the EFB's
    view_invalidate();   // this eye's matrices and target
    bool grabbed = false, grabbed_spray = false, grabbed_restore = false;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(GL_TRUE);
    // The eye's background has to be the EFB's. Where the scene draws nothing behind the
    // water -- open sea, past where the course gives the sea a bottom -- the water
    // refracts whatever the buffer was cleared to, and an eye cleared to black turned
    // that into a black sea in stereo while the flat view, cleared to the game's own
    // colour, showed open water. So take the clear the game set for the main scene: the
    // last copy to clear the colour buffer ahead of the first draw an eye replays.
    float bg[3] = {0, 0, 0};
    for (size_t i = 0; i < b.cmds.size(); i++) {
        if (b.cmds[i].type == CmdType::Draw) {
            if (!skip[i]) break;
            continue;
        }
        if (b.cmds[i].type != CmdType::EfbCopy) continue;
        const EfbCopyCmd& c = b.cmds[i].copy;
        if (c.clear && c.clear_color) {
            bg[0] = ((c.clear_rgba >> 24) & 0xFF) / 255.0f;
            bg[1] = ((c.clear_rgba >> 16) & 0xFF) / 255.0f;
            bg[2] = ((c.clear_rgba >> 8) & 0xFF) / 255.0f;
        }
    }
    if (eyelog && do_copies)
        fprintf(stderr, "[eye]   background %.3f %.3f %.3f\n", bg[0], bg[1], bg[2]);
    // Mid-morph, theater's black surround comes up to the scene's clear colour as the
    // window opens, so the edge of the background quad is gone by the time it would show.
    const bool morph = g_morph < 1.0f;
    const float surround = morph ? g_morph * g_morph : 1.0f;
    glClearColor(bg[0] * surround, bg[1] * surround, bg[2] * surround, 1);
    clear_depth(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    // GL_CLIP_DISTANCE0, which ES knows only by its extension's name.
    constexpr GLenum kClipDistance0 = 0x3000;
    if (morph) {
        // The main camera's projection, from the first world draw an eye replays: the
        // background is a piece of its frustum. A frame with no world in it -- a menu --
        // gets a stand-in the shape of the panel, since only where it lands matters.
        float P[16] = {0};
        P[0] = 2.5f / 1.6f; P[5] = 2.5f / 1.2f; P[10] = -1.0f; P[11] = -1.0f; P[14] = -1.0f;
        for (size_t i = 0; i < b.cmds.size(); i++) {
            if (b.cmds[i].type != CmdType::Draw || skip[i]) continue;
            const PixelState& st = b.states[b.cmds[i].state];
            const float* p = st.proj;
            if ((int)p[6] != 0 || st.view_space) continue;
            memset(P, 0, sizeof(P));
            P[0] = p[0]; P[8] = p[1]; P[5] = p[2]; P[9] = p[3]; P[10] = p[4]; P[14] = p[5];
            P[11] = -1.0f;
            break;
        }
        if (g_clip_ok)
            for (GLenum i = 0; i < 4; i++) glEnable(kClipDistance0 + i);
        draw_morph_background(P, bg);
    }
    // The same four planes keep a 3D draw composed into the HUD (EyeFlat) inside its
    // viewport; every other draw outside the morph writes distances of 1.
    const bool clip4 = g_clip_ok && (morph || !g_flat3d.empty());
    if (clip4 && !morph)
        for (GLenum i = 0; i < 4; i++) glEnable(kClipDistance0 + i);

    uint32_t cur_state = UINT32_MAX;
    gl_state_invalidate();
    int cur_prim = -1;
    static int eye_skip_lo = -1, eye_skip_hi = -1;
    static bool eye_skip_parsed = false;
    if (!eye_skip_parsed) {
        eye_skip_parsed = true;
        if (const char* s = getenv_nonempty("GCN_DRAW_SKIP")) {
            eye_skip_lo = atoi(s);
            const char* dash = strchr(s, '-');
            eye_skip_hi = dash ? atoi(dash + 1) : eye_skip_lo;
        }
    }
    static const uint32_t eye_drawlog_frame = getenv("GCN_DRAWLOG") ? (uint32_t)atoi(getenv("GCN_DRAWLOG")) : 0;
    // Keyed on the presented frame, the number the dumps carry; a frame may take several batches.
    const bool eye_drawlog = eye_drawlog_frame && g_present_count + 1 == eye_drawlog_frame;
    int eye_draw_index = 0;   // the flat draw log's numbering: every draw command, in order
    bool flat3d_cleared = false;
    const size_t eye_end = first_part_end(b, skip);
    uint32_t disp_w, disp_h;
    display_size(b, disp_w, disp_h);
    for (size_t i = 0; i < eye_end; i++) {
        Cmd& c = b.cmds[i];
        // Copies and the present are the first eye's business, done against the EFB. A
        // copy that an EyeRestore draw puts back is taken from the eye as well, at the
        // same moment: what the eye has drawn so far is what the game's copy holds.
        if (c.type != CmdType::Draw) {
            if (c.type == CmdType::EfbCopy && !morph && c.copy.tex_id &&
                std::find(g_restore_tex.begin(), g_restore_tex.end(), c.copy.tex_id) != g_restore_tex.end())
            {
                grab_eye(g_restore_grab, w, h);
                grabbed_restore = true;
            }
            // A second copy of the whole frame in one frame is a later state of it: a
            // pass that copies the frame, draws over it, and copies it again to post-
            // process what it drew (a false-colour filter, then the hot things over it
            // and a second filter on those). The first grab is of the frame before the
            // second pass drew anything, so the next draw that samples the copy grabs
            // again.
            if (c.type == CmdType::EfbCopy && grabbed && c.copy.tex_id && is_fullscreen_copy(c.copy, disp_w, disp_h))
                grabbed = false;
            cur_state = UINT32_MAX;
            gl_state_invalidate();
            continue;
        }
        // Mid-morph the foreground's depth is the eye's, not the game's (u_vr 3), so the
        // depth a cut-away is drawn at means nothing against it: it is left out, and what it
        // would have cut away stays for the length of the morph.
        const bool restore_draw = !g_restore.empty() && g_restore[c.state];
        if (restore_draw && (morph || !grabbed_restore)) { eye_draw_index++; continue; }
        if (skip[i]) { n_skipped++; eye_draw_index++; continue; }
        if (!g_hide.empty() && g_hide[i]) { eye_draw_index++; continue; }
        // GCN_EYE_SKIPCOMP drops the screen-space passes entirely, for comparing against
        // drawing them flat across the eye. Dropping the water one leaves bare seabed.
        static const bool skipcomp = getenv_nonempty("GCN_EYE_SKIPCOMP") != nullptr;
        if (skipcomp && samples_fullscreen_copy(b.states[c.state])) { n_skipped++; continue; }
        // The first draw that wants the finished frame is the moment to take it: the
        // scene behind the water is in the target by now and the water is not yet.
        if (!grabbed && samples_fullscreen_copy(b.states[c.state])) {
            grabbed = true;
            grab_eye(g_eye_grab, w, h);
            cur_state = UINT32_MAX;   // the grab left its own texture bound
            gl_state_invalidate();
        }
        // The spray composites itself over the finished scene, the water included, so the
        // grab above -- taken deliberately before the water, which is what the water
        // itself needs -- is a layer short by the time the droplets draw. Re-take it at
        // the first of them, once, and every droplet in the frame then refracts the scene
        // as it actually stands. Costs a second full-target copy per eye on any frame
        // that throws spray, which at speed is every frame.
        if (samples_grab_copy(b.states[c.state])) {
            // How a droplet actually addresses its grab, which decides what the
            // substitution has to carry over: the indirect matrix id per stage (1..3 is
            // a static offset in texels, 5..11 scales the coordinate itself) and the
            // wrap, which folds the coordinate into the copy's own size.
            if (eyelog && do_copies && !grabbed_spray) {
                const PixelState& st = b.states[c.state];
                const uint32_t nstg = ((st.bp[0x00] >> 10) & 15) + 1;
                fprintf(stderr, "[eye]   spray draw: stages=%u nind=%u", nstg,
                        (st.bp[0x00] >> 16) & 7);
                for (uint32_t t = 0; t < nstg; t++) {
                    const uint32_t ic = st.bp[0x10 + t];
                    fprintf(stderr, " s%u[mid=%u sw=%u tw=%u bt=%u]", t, (ic >> 9) & 15,
                            (ic >> 13) & 7, (ic >> 16) & 7, ic & 3);
                }
                fprintf(stderr, "\n");
            }
            if (!grabbed_spray) {
                grabbed_spray = true;
                grab_eye(g_spray_grab, w, h);
                cur_state = UINT32_MAX;
                gl_state_invalidate();
            }
            n_spray++;
        }
        n_drawn++;
        // GCN_DRAW_SKIP=a-b leaves those draws out of the eye too, by the index the flat
        // pass's draw log gives them, and GCN_DRAWLOG=<frame> says which layer the eye
        // gave each draw (see eye_layer_name), beside its band: how a draw that lands
        // somewhere surprising in an eye is told from one the game put there.
        if (eye_skip_lo >= 0 && eye_draw_index >= eye_skip_lo && eye_draw_index <= eye_skip_hi) { eye_draw_index++; continue; }
        // The z freeze plane as this eye sees it (zfreeze_plane_eye).
        const bool zfreeze = (b.states[c.state].bp[0x00] >> 19) & 1;
        if (!zfreeze) zfreeze_plane_eye(b, c, g_zplane);
        if (c.state != cur_state || c.prim != cur_prim) {
            g_cur_flat3d = !g_flat3d.empty() && g_flat3d[c.state];
            // The 3D draws composed into the HUD sort among themselves by the game's
            // depth, and the world's depth in this eye means nothing to them: they are
            // on the HUD frame, in front of everything. The first of them clears it.
            if (g_cur_flat3d && !flat3d_cleared) {
                flat3d_cleared = true;
                glDepthMask(GL_TRUE);
                glClear(GL_DEPTH_BUFFER_BIT);
                gl_state_invalidate();
            }
            g_cur_screen = g_screen.empty() ? 0 : g_screen[c.state];
            g_cur_uvgrad = g_cur_screen == 2 && !g_frame_grad.empty() ? &g_frame_grad[c.state * 32] : nullptr;
            g_cur_facing = !g_facing.empty() && g_facing[c.state];
            g_cur_true_scale = !g_true_scale.empty() && g_true_scale[c.state];
            g_cur_hud_push = g_hud_push.empty() ? 1.0f : g_hud_push[c.state];
            g_cur_restore = restore_draw;
            g_cur_restore_box = restore_draw ? &g_restore_box[c.state * 4] : nullptr;
            apply_state(b.states[c.state], c.prim);
            g_cur_flat3d = false;
            g_cur_screen = 0;
            g_cur_uvgrad = nullptr;
            g_cur_facing = false;
            g_cur_true_scale = false;
            g_cur_hud_push = 1.0f;
            g_cur_restore = false;
            g_cur_restore_box = nullptr;
            // apply_state binds the EFB's scissor and viewport expectations; the eye
            // target overrides both.
            glViewport(0, 0, w, h);
            glDisable(GL_SCISSOR_TEST);
            cur_state = c.state;
            cur_prim = c.prim;
        } else if (zfreeze && g_applied.pr && g_applied.pr->u_zplane >= 0) {
            glUniform4fv(g_applied.pr->u_zplane, 1, g_zplane);
        }
        if (eye_drawlog && do_copies) {
            const PixelState& st = b.states[c.state];
            fprintf(stderr, "[eyedraw] %d st=%u %s band=%.5f..%.5f view_space=%d\n", eye_draw_index, c.state, g_eye_layer_name,
                    (st.viewport[5] - fabsf(st.viewport[2])) / 16777215.0f, st.viewport[5] / 16777215.0f, (int)st.view_space);
        }
        eye_draw_index++;
        draw_cmd(b, c);
    }
    // The objects the game asked for last (EyeLateObject), over everything the eye drew,
    // with their own depth state against the depth the eye has written: a marker in the
    // world stays hidden behind what is in front of it there, and shows over the 2D
    // window the game painted on top of where it stands. The flat pass left them out, so
    // a copy the window magnifies does not hold them a second time. One inside the window
    // is drawn on the window's plane (late_panel_view).
    float late_view[16], cur_late_view[16];
    bool cur_at_panel = false;
    for (size_t i = 0; i < eye_end && !g_late.empty(); i++) {
        if (!g_late[i] || skip[i]) continue;
        Cmd& c = b.cmds[i];
        const bool zfreeze = (b.states[c.state].bp[0x00] >> 19) & 1;
        if (!zfreeze) zfreeze_plane_eye(b, c, g_zplane);
        const bool at_panel = late_panel_view(b, c, late_view);
        const bool view_changed = at_panel != cur_at_panel ||
                                  (at_panel && memcmp(late_view, cur_late_view, sizeof(late_view)) != 0);
        if (view_changed) {
            view_invalidate();
            cur_at_panel = at_panel;
            if (at_panel) memcpy(cur_late_view, late_view, sizeof(late_view));
        }
        if (view_changed || c.state != cur_state || c.prim != cur_prim) {
            g_cur_hud_push = g_hud_push.empty() ? 1.0f : g_hud_push[c.state];
            // Otherwise at its modelled distance, as EyeTrueScale: a late object marks
            // something in the world, and a scaled layer's band (one a reticle round the
            // marked thing is drawn in, to be drawn over everything) would take it off what
            // it marks. The band's depth is kept for the depth test.
            g_cur_true_scale = true;
            g_cur_late_view = at_panel ? cur_late_view : nullptr;
            apply_state(b.states[c.state], c.prim);
            g_cur_hud_push = 1.0f;
            g_cur_true_scale = false;
            g_cur_late_view = nullptr;
            glViewport(0, 0, w, h);
            glDisable(GL_SCISSOR_TEST);
            cur_state = c.state;
            cur_prim = c.prim;
        } else if (zfreeze && g_applied.pr && g_applied.pr->u_zplane >= 0) {
            glUniform4fv(g_applied.pr->u_zplane, 1, g_zplane);
        }
        if (eye_drawlog && do_copies) fprintf(stderr, "[eyedraw] late cmd %zu st=%u %s\n", i, c.state, g_eye_layer_name);
        n_drawn++;
        draw_cmd(b, c);
    }
    if (cur_at_panel) view_invalidate();  // nothing after keeps the panel's view
    if (eyelog && do_copies)
        fprintf(stderr, "[eye]   drawn=%d skipped_composite=%d spray=%d flat_trimmed=%zu\n", n_drawn,
                n_skipped, n_spray, n_trimmed);
    // The renderer's half of GCN_FRAMETIME for an eye: the flat pass (first eye only) and
    // this eye's own draws, CPU time.
    if (frametime_on())
        fprintf(stderr, "[eye-rt] f%u %s flat %6.2fms eye %6.2fms  draws %d\n", g_render_frame,
                do_copies ? "first" : "second", ms_flat, ms_since(t_start) - ms_flat, n_drawn);
    apply_stats_flush(do_copies ? "eye0" : "eye1", g_render_frame);
    if (clip4)
        for (GLenum i = 0; i < 4; i++) glDisable(kClipDistance0 + i);
#ifdef GCN_GL_ES
    // The eye's depth buffer is finished with: nothing reads it, and the next pass clears
    // it. On a tiled GPU, which every standalone headset has, a depth attachment left
    // valid at the end of a pass is written out of tile memory to RAM tile by tile, and
    // at the eye's size with multisampling that is a store of several tens of megabytes
    // per eye per frame, for nothing. Saying so lets the driver drop it. Desktop GL 3.3
    // has no glInvalidateFramebuffer, and an immediate-mode GPU would gain nothing.
    // GCN_EYE_KEEPDEPTH=1 leaves it in, for comparing.
    static const bool keep_depth = getenv_nonempty("GCN_EYE_KEEPDEPTH") != nullptr;
    if (!keep_depth) {
        const GLenum attachment = GL_DEPTH_ATTACHMENT;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &attachment);
    }
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_vr_active = false;
    g_eye_frame = false;
    // An eye frame presents nothing through the EFB, so the presented count -- which the
    // draw log, the copy dump and the stereo logs key on -- is stepped here instead, once
    // per frame: the frontends number their eye dumps the same way, so GCN_DRAWLOG=<n>
    // names the frame eye_<n>.png shows.
    if (do_copies) g_present_count++;
    return true;
}

}  // namespace gx
