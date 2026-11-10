// apply_state: a pixel state put into GL for the draws after it, sending only what differs
// from what GL already holds.
#include "render_gl_internal.h"
#include "blend.h"

namespace gx {

// How far an EyeFrameScreen draw's edge vertices go along the HUD frame's plane, in the
// frame's own half-sizes: far enough to be well past any eye's field.
static constexpr float kFrameScreenReach = 40.0f;

bool g_cull_swap = false;

static inline int32_t sx11(uint32_t v) { return (int32_t)(v << 21) >> 21; }

float fog_float(uint32_t v) {
    uint32_t mant = v & 0x7FF, exp = (v >> 11) & 0xFF, sign = (v >> 19) & 1;
    uint32_t bits = (sign << 31) | (exp << 23) | (mant << 12);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Dual-source blending's factors: core in desktop GL 3.3, and GL_EXT_blend_func_extended's
// (the same values) on ES, whose loader does not define them.
#ifndef GL_SRC1_ALPHA
#define GL_SRC1_ALPHA 0x8589
#endif
#ifndef GL_ONE_MINUS_SRC1_ALPHA
#define GL_ONE_MINUS_SRC1_ALPHA 0x88FB
#endif
static const GLenum kBlendSrc[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
static const GLenum kBlendDst[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
static const GLenum kDepthFunc[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};

// The origin that the scissor box and the viewport are both measured from, in EFB pixels.
//
// GX stores it halved and biased by 342, so GXSetScissorBoxOffset(0, 0) reads back as 342.
// This game sets (-2, -2) -- the register reads 340 -- and moves every viewport by the same
// (-2, -2), so on hardware the two cancel and the picture sits at the EFB's origin.
// Assuming 342 instead left every pass two pixels up and left of where the game believes
// it is. That shows wherever the game samples a copy of the EFB at screen coordinates it
// worked out itself: the water looked its refraction up two pixels out of register, which
// stood a water-tinted second copy of the racer beside the real one, and the last two
// columns and rows of every pass were never drawn.
void scissor_offset(const uint32_t* bp, int& xoff, int& yoff) {
    xoff = (int)(bp[0x59] & 0x3FF) * 2;
    yoff = (int)((bp[0x59] >> 10) & 0x3FF) * 2;
}

// What apply_state last put into GL, so that it can put in only what differs.
//
// A race frame applies around a thousand states, and measured over a million consecutive
// pairs, 94% of them differ in a texture -- 1.1 texmaps on average -- while the projection,
// viewport, scissor, fog, indirect matrices and point size are the same more than 99% of
// the time, the program 81%, the TEV registers 76%, blend and depth 90%. Re-issuing the
// forty-odd GL calls of a full application for every one of those was most of the
// renderer's CPU time; the typical application is now a texture bind and its size.
//
// This holds the bindings and the fixed-function state, which are global to the context.
// The uniforms are kept with their program instead (UniformShadow): a uniform belongs to
// its program and keeps its value across switches, so a program switched back to still
// holds what it was last given, and a group whose inputs match is left alone whether or not
// the program changed. Before that, every switch re-uploaded every group -- a dozen and more
// calls -- and a frame here switched programs one draw in six. The shadow holds within one
// pass: an EFB copy, a present or an eye's grab changes GL state behind it, and so does the
// start of a new pass, whose eye matrices differ -- each of those invalidates it.
AppliedState g_applied;
// What GL's face culling is set to: -1 unknown, 0 off, 1 front, 2 back, 3 both. Kept apart
// from g_applied because GL culls polygons only: a line or a point leaves it as it is, so a
// game alternating filled faces with outlines -- a 3D map, thousands of draws a frame -- need
// not turn culling off and on again around each outline. Those toggles were a quarter of
// the render thread's time on such a screen on a Quest 3.
int g_gl_cull = -1;
void gl_state_invalidate() {
    g_applied.valid = false;
    g_gl_cull = -1;
}

// A program's uniform record is current while its `gen` matches g_uniform_gen, and the
// groups that depend on the pass -- the eye's matrices, the morph, the eye target's size,
// whether this is the flat view at all -- while `view_gen` matches g_view_gen as well.
// Nothing inside the renderer touches a program's uniforms except apply_state, so the
// first counter moves only for the EFB scale (the point size is in EFB pixels), and the
// second at the start of every pass and whenever the eye is placed.
static uint32_t g_uniform_gen = 1, g_view_gen = 1;
void uniforms_invalidate() { g_uniform_gen++; }
void view_invalidate() { g_view_gen++; }

// GCN_APPLYSTATS=1: which groups of state apply_state actually had to send, summed over a
// pass, printed every 64th frame. What a game changes between draws decides what a state
// application costs, and this is how to see it.
static bool applystats_on() {  // on first use: the app sets its environment after static init
    static const bool on = getenv_nonempty("GCN_APPLYSTATS") != nullptr;
    return on;
}
static struct ApplyStats {
    uint32_t applies, program, proj, viewport, psize, tev, aref, ind, indcs, fog, screen, ripple,
        texbind, sampbind, tsz, blend, depth, cull, scissor;
} g_astat;
void apply_stats_flush(const char* pass, uint32_t frame) {
    if (!applystats_on()) return;
    if ((frame & 63) == 0) {
        const ApplyStats& a = g_astat;
        fprintf(stderr, "[apply] f%u %s applies %u: prog %u proj %u vp %u psz %u tev %u aref %u ind %u "
                "indcs %u fog %u scr %u rip %u tex %u samp %u tsz %u blend %u depth %u cull %u sciss %u\n",
                frame, pass, a.applies, a.program, a.proj, a.viewport, a.psize, a.tev, a.aref, a.ind,
                a.indcs, a.fog, a.screen, a.ripple, a.texbind, a.sampbind, a.tsz, a.blend, a.depth, a.cull,
                a.scissor);
    }
    memset(&g_astat, 0, sizeof(g_astat));
}

// The z freeze plane (see the pixel shader): the depth plane of the last triangle drawn
// without z freeze, in the EFB target's window coordinates, w = 1 once there is one. Set
// by the flat pass as it goes; an eye keeps its draws' own depth (w = 0).
float g_zplane[4];

// What a state's draws are placed by in the flat view: the game's anchor for them and the
// squeeze the output aspect asks of it, the identity at 4:3 and in an eye.
AnchorMap draw_anchor_map(const PixelState& st) {
    return anchor_map((Anchor)st.anchor, st.anchor_x0, st.anchor_x1, anchor_scale(output_aspect(), g_eye_frame));
}

// u_vp_a for a state, less the scissor offset, with the anchor's remap of its x terms.
void viewport_a(const PixelState& st, const AnchorMap& an, float vpa[4]) {
    int xoff, yoff;
    scissor_offset(st.bp, xoff, yoff);
    const float* vp = st.viewport;
    vpa[0] = 2.0f * (vp[3] - xoff) / EFB_W - 1.0f;
    vpa[1] = 2.0f * vp[0] / EFB_W;
    vpa[2] = 2.0f * (vp[4] - yoff) / EFB_H - 1.0f;
    vpa[3] = 2.0f * vp[1] / EFB_H;
    anchor_viewport(an.a, an.x0, an.x1, an.k, EFB_W, vpa);
}

// The texture offset a point's coordinates run across (SU_LPSIZE bits 19-21: 1/16 to 1),
// and which of a draw's coordinates have it enabled (bit 19 of each SU_SSIZE): the
// coordinates in the low byte, with bit 8 set so that a draw of points with none enabled
// still differs from 0, which is every other draw's.
static const float kPointTexOffset[8] = {0.0f, 1.0f / 16, 1.0f / 8, 1.0f / 4, 1.0f / 2, 1.0f, 1.0f, 1.0f};
static uint32_t point_tex_key(const PixelState& st) {
    if (kPointTexOffset[(st.bp[0x22] >> 19) & 7] == 0.0f) return 0;
    uint32_t mask = 0;
    for (uint32_t t = 0; t < st.num_texgens && t < 8; t++)
        if ((st.bp[0x30 + 2 * t] >> 19) & 1) mask |= 1u << t;
    return mask ? (mask | 0x100u | (((st.bp[0x22] >> 19) & 7) << 9)) : 0;
}

void apply_state(const PixelState& st, int prim) {
    int xoff, yoff;
    scissor_offset(st.bp, xoff, yoff);
    const uint32_t* bp = st.bp;
    const uint32_t* abp = g_applied.bp;
    ShaderKey key = make_shader_key(st);
    {
        // A draw an eye will give a foreground layer's game depth (u_vr 4, below) gets the
        // program that works that depth out per fragment; the shader still checks u_vr, so
        // a draw that ends up elsewhere keeps its own depth.
        static const bool game_depth = !(getenv_nonempty("GCN_EYE_FGDEPTH") && atoi(getenv_nonempty("GCN_EYE_FGDEPTH")) == 0);
        static const bool frag_depth = !(getenv_nonempty("GCN_EYE_FGFRAGDEPTH") && atoi(getenv_nonempty("GCN_EYE_FGFRAGDEPTH")) == 0);
        if (g_vr_active && game_depth && frag_depth && g_morph >= 1.0f && (int)st.proj[6] == 0 && g_fg_to > 0.0f &&
            st.viewport[5] / 16777215.0f <= g_fg_to && !g_cur_flat3d)
            key.fg_depth = 1;
    }
    g_astat.applies++;
    // `same` means the previous application is still in GL and the program is unchanged.
    const bool same = g_applied.valid && key == g_applied.key;
    const Program* ready = same ? g_applied.pr : get_program(key);
    // Its program is still being built: the draw is left out, and nothing applied, so
    // what GL holds is still what g_applied says.
    g_draw_skip = !ready;
    if (!ready) return;
    const Program& pr = *ready;
    if (!same) {
        g_astat.program++;
        glUseProgram(pr.prog);
        g_applied.key = key;
        g_applied.pr = &pr;
    }
    // What this program was last given; see UniformShadow. A group is uploaded when its
    // inputs differ from the record, or the record is not current.
    UniformShadow& u = pr.shadow;
    const bool held = u.gen == g_uniform_gen;
    const bool view_held = held && u.view_gen == g_view_gen;
    auto held_words = [&](const uint32_t* a, const uint32_t* b, size_t n) {
        return held && memcmp(a, b, n * sizeof(uint32_t)) == 0;
    };

    // Projection. In VR a perspective batch is world geometry and gets the eye's
    // projection instead of the game's; an orthographic one is a 2D element and goes on
    // the HUD frame -- and so does a perspective batch the game placed in view space
    // itself, which is 3D but is no more part of the course than the lap counter is.
    // That is the countdown light rig: left in the world it stands in the water between
    // the viewer and the racer, because the game means it to hang in front of the camera.
    float P[16] = {0};
    const float* p = st.proj;
    const bool perspective = (int)p[6] == 0;
    // Something the game placed in view space but drew with a perspective frustum is a 3D
    // object held in front of the camera, not a 2D overlay. Painting it on the HUD frame
    // flattens it -- that frame's Z column is zero, so every vertex lands on one plane --
    // which throws away the object's own depth. The countdown rig lost the occlusion that
    // hides the lamp behind its lens, and the lamps showed through as white squares.
    //
    // So it keeps the eye's projection and stays 3D. It takes g_vr_view rather than
    // g_vr_view_world: the world pitch is taken out of the *world* to level the sea, and
    // applying it to something attached to the camera is what tilted the rig by 23
    // degrees. Without the correction it hangs in front of the viewer the way the game
    // means it to, and the head still moves within that.
    //
    // GCN_EYE_VS3D=0 puts it back on the HUD frame, which is where it was.
    //
    // Unless the game says its view-space draws are billboards in the world
    // (render_set_view_space_world): a sprite the game turns to face its camera, placed at
    // a point in the scene. Those are world geometry like anything else -- their vertices
    // are in the camera's frame as every world vertex is -- and held before the camera
    // they stayed where the chase camera saw them while the game's own eye moved off.
    static const bool vs3d = !(getenv_nonempty("GCN_EYE_VS3D") && atoi(getenv_nonempty("GCN_EYE_VS3D")) == 0);
    const bool view_space = st.view_space && !g_view_space_world;
    // A 3D draw the game composes into its 2D picture (EyeFlat): a model in a window of the
    // HUD, drawn with a frustum of its own into a viewport no bigger than that window. Its
    // positions are in a little scene of its own, so in the world it floats at whatever
    // depth that scene happens to put it, which matches nothing the HUD frame shows. It goes
    // on the HUD frame instead, through its own projection and viewport -- exactly where the
    // flat frame has it -- and keeps the game's own depth to sort its parts by. A model
    // drawn orthographically is the same thing: on the HUD frame either way, but without
    // this it had no depth test there, and its parts lay in the order they were drawn.
    const bool flat3d = g_vr_active && g_cur_flat3d;
    // A draw the game asked to have across the eye (EyeScreen): on the HUD frame's path,
    // but mapped onto the eye's clip space directly. Orthographic, or a perspective effect
    // the game hangs just before its camera's lens, which the game's projection maps to its
    // frame just the same.
    const bool screen = g_vr_active && g_cur_screen == 1;
    // One on the HUD frame with its edges taken far out (EyeFrameScreen).
    const bool frame_screen = g_vr_active && g_cur_screen == 2;
    // A copy of part of the frame put back over itself with a depth test (EyeRestore):
    // across the eye too, but at the game's depth and with its depth test.
    const bool restore = g_vr_active && g_cur_restore && g_cur_restore_box && !perspective;
    // A panel the game moved about its picture by its projection (EyeFacing): on the HUD
    // frame, turned to face the viewer.
    const bool facing = g_vr_active && g_cur_facing && !perspective && !screen && !restore && !frame_screen;
    const bool view_space_3d = perspective && view_space && vs3d && !flat3d;
    const bool on_hud_frame = screen || frame_screen || flat3d || ((!perspective || view_space) && !view_space_3d);
    if (perspective) {
        P[0] = p[0]; P[8] = p[1]; P[5] = p[2]; P[9] = p[3]; P[10] = p[4]; P[14] = p[5]; P[11] = -1.0f;
    } else {
        P[0] = p[0]; P[12] = p[1]; P[5] = p[2]; P[13] = p[3]; P[10] = p[4]; P[14] = p[5]; P[15] = 1.0f;
    }
    // The game's own clip space bounds a flat3d draw to its viewport, which the eye's
    // target does not have: distance i of these planes is >= 0 inside it.
    float flat_crop[16] = {0};
    if (flat3d)
        for (int c = 0; c < 4; c++) {
            const float x = P[c * 4 + 0], y = P[c * 4 + 1], w = P[c * 4 + 3];
            flat_crop[c * 4 + 0] = w - x;
            flat_crop[c * 4 + 1] = w + x;
            flat_crop[c * 4 + 2] = w - y;
            flat_crop[c * 4 + 3] = w + y;
        }
    if (flat3d) {
        // The viewport, as a map from the draw's clip space into the 640x480 frame's that
        // the HUD frame is painted with (y up): x' = a*x + b*w, y' = c*y + d*w.
        int xoff, yoff;
        scissor_offset(st.bp, xoff, yoff);
        const float* vp = st.viewport;
        float V[16] = {0};
        V[0] = 2.0f * vp[0] / 640.0f;
        V[12] = 2.0f * (vp[3] - xoff) / 640.0f - 1.0f;
        V[5] = -2.0f * vp[1] / 480.0f;
        V[13] = 1.0f - 2.0f * (vp[4] - yoff) / 480.0f;
        V[10] = 1.0f;
        V[15] = 1.0f;
        float VP[16];
        mat4_mul(V, P, VP);
        memcpy(P, VP, sizeof(P));
    }
    // Depth layers. GX has no depth-range call, but a viewport carries a z range, and a game
    // can confine a draw to a band of the depth buffer with it -- a sky kept behind the
    // world whatever its geometry's real distance, a weapon kept in front of it. An eye
    // keeps those bands (see the vertex shader). What it cannot keep is the illusion: a sky
    // modelled a short way out around the camera has the disparity of something near, and
    // a weapon modelled large and far off reads as large in two eyes. So a game can name
    // its background and foreground bands (render_set_depth_layers): the background is
    // drawn at infinity, turned with the head and the same in both eyes, and the
    // foreground is scaled towards the camera. GCN_EYE_SKY=0 turns the background off.
    static const bool sky_at_infinity = !(getenv_nonempty("GCN_EYE_SKY") && atoi(getenv_nonempty("GCN_EYE_SKY")) == 0);
    const float band_lo = (st.viewport[5] - fabsf(st.viewport[2])) / 16777215.0f;
    const float band_hi = st.viewport[5] / 16777215.0f;
    const bool background = sky_at_infinity && g_bg_from > 0.0f && perspective && !view_space &&
                            band_lo >= g_bg_from;
    // The HUD layer takes precedence over the foreground, whose band it sits inside. A
    // draw the game's filter called EyeTrueScale -- something placed in the world, a
    // marker at a target, drawn in a near band only to be drawn over everything -- is in
    // neither: it stays where it was modelled, and keeps its band's depth.
    const bool true_scale = g_vr_active && g_cur_true_scale;
    const bool hud_layer = !true_scale && g_hud_to > 0.0f && g_hud_scale != 1.0f && perspective && band_hi <= g_hud_to;
    const bool foreground = !true_scale && !hud_layer && g_fg_to > 0.0f && g_fg_scale != 1.0f && perspective && band_hi <= g_fg_to;
    // A foreground layer also keeps the game's own depth within its band (u_vr 4 in the
    // vertex shader), whether or not it is scaled.
    const bool fg_layer = g_fg_to > 0.0f && perspective && band_hi <= g_fg_to;
    const float hud_push = g_vr_active && hud_layer ? g_cur_hud_push : 1.0f;
    const bool same_proj = view_held && memcmp(st.proj, u.proj, sizeof(st.proj)) == 0 &&
                           st.view_space == u.view_space && background == u.background &&
                           foreground == u.foreground && fg_layer == u.fg_layer && hud_layer == u.hud_layer &&
                           flat3d == u.flat3d && screen == u.screen && frame_screen == u.frame_screen && facing == u.facing && !restore && !u.restore &&
                           hud_push == u.hud_push && (!flat3d || memcmp(st.viewport, u.viewport, sizeof(st.viewport)) == 0);
    // Note: a draw sampling a copy of the whole frame (the water surface is one) must
    // stay in the world, however tempting its screen-space origin makes the overlay path
    // look. Sending the water through it put the water, and the racer baked into the
    // copy, on a flat panel hanging in front of the camera while the real racer went on
    // moving in the world. The seam at the billboard's edge is the lesser problem.
    if (!same_proj) {
        g_astat.proj++;
        if (g_vr_active && g_morph < 1.0f) {
            // Part way between theater and stereo: see morph_chain.
            float M[16], F[16], C[16];
            morph_chain(P, screen ? MorphKind::Screen : on_hud_frame ? MorphKind::Hud
                           : view_space_3d ? MorphKind::CameraHeld : MorphKind::World,
                        M, F, C);
            glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, M);
            glUniformMatrix4fv(pr.u_view, 1, GL_FALSE, F);
            glUniformMatrix4fv(pr.u_crop, 1, GL_FALSE, C);
            glUniform1i(pr.u_vr, 3);
            g_eye_layer_name = "morph";
        } else if (g_vr_active && !on_hud_frame) {
            // A camera-placed 3D object is viewed with the head transform but without the
            // world's pitch correction; see view_space_3d above.
            const float* view = background ? g_vr_view_sky
                              : view_space_3d ? (hud_layer ? g_vr_view_hud : foreground ? g_vr_view_fg : g_vr_view)
                              : (hud_layer ? g_vr_view_world_hud : foreground ? g_vr_view_world_fg : g_vr_view_world);
            if (g_cur_late_view) view = g_cur_late_view;
            // GCN_EYE_GAMEPROJ keeps the game's own frustum and applies only the head
            // transform, which tells apart "the eye sees less than it should" from "the game
            // never drew anything out there".
            static const bool game_proj = getenv_nonempty("GCN_EYE_GAMEPROJ") != nullptr;
            static bool logged = false;
            if (!logged && getenv_nonempty("GCN_EYELOG")) {
                logged = true;
                fprintf(stderr, "[eye] game fov: x=%.1fdeg y=%.1fdeg (p0=%f p2=%f)\n",
                        2.0f * atanf(1.0f / p[0]) * 57.2958f, 2.0f * atanf(1.0f / p[2]) * 57.2958f,
                        p[0], p[2]);
            }
            // A HUD element pushed out (render_set_hud_push): scaled about the camera in the
            // game's view space, before the layer's own scale.
            float pushed[16];
            if (hud_push != 1.0f) {
                const float K[16] = {hud_push, 0, 0, 0, 0, hud_push, 0, 0, 0, 0, hud_push, 0, 0, 0, 0, 1};
                mat4_mul(view, K, pushed);
                view = pushed;
            }
            glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, game_proj ? P : g_vr_proj);
            glUniformMatrix4fv(pr.u_view, 1, GL_FALSE, view);
            g_eye_layer_name = background ? "sky" : hud_push != 1.0f ? "hud-band(pushed)"
                             : hud_layer ? (view_space_3d ? "hud-band(view)" : "hud-band(world)")
                             : foreground ? (view_space_3d ? "foreground(view)" : "foreground(world)")
                             : view_space_3d ? "view-space-3d" : g_cur_late_view ? "late(at-panel)"
                             : true_scale ? "true-scale" : "world";
            static const bool game_depth = !(getenv_nonempty("GCN_EYE_FGDEPTH") && atoi(getenv_nonempty("GCN_EYE_FGDEPTH")) == 0);
            glUniform1i(pr.u_vr, fg_layer && game_depth ? 4 : 1);
        } else if (g_vr_active) {
            // A HUD element in an eye. The game's own projection already puts its frame in
            // [-1,1], so that is where the chain picks up -- for the perspective rig too,
            // since the frame's matrix carries clip w through and the divide lands it on
            // the plane just the same.
            //
            // GX's viewport transform is deliberately not in the chain: it places the frame
            // within the 640x528 EFB, and an eye's render target is not the EFB -- the same
            // reason the scissor rect is dropped below. Including it would map the EFB
            // rather than the 480 lines the game displays, leaving the HUD a few per cent
            // small and off centre.
            //
            // Every term is constant for the draw, so the whole chain folds into one matrix
            // here and the shader is left with a single multiply.
            float a[16], b[16], M[16];
            if (screen) {
                // The game's frame is [-1,1] after its own projection, and so is the eye's
                // field after the eye's: the one becomes the other, enlarged past the
                // field's edges, with no view between them -- fixed to the head. Depth is
                // the frame's own concern only (the HUD frame's draws are drawn without a
                // depth test), so it is left at the middle of the range.
                eye_screen_matrix(P, M);
            } else if (restore) {
                // The game puts its copy back over the rect it came from, which in the flat
                // view is where what it cuts away is. In an eye that is somewhere else, so
                // the quad is stretched from its box over the eye's whole field, past the
                // edges as EyeScreen's is: the grab it samples is what lies under every
                // fragment, so only what was drawn since the copy, nearer than the quad,
                // changes. Its depth is what the game gives it in its band -- the depth the
                // vertex shader writes for a foreground layer (u_vr 4), so the two compare
                // as they do on the screen: z' = b.x + b.y * z for the game's clip z, whose
                // w is 1 under an ortho projection.
                const float* r = g_cur_restore_box;
                const float sx = r[1] > r[0] ? 2.0f * kEyeScreenOverscan / (r[1] - r[0]) : 1.0f;
                const float sy = r[3] > r[2] ? 2.0f * kEyeScreenOverscan / (r[3] - r[2]) : 1.0f;
                const float cx = 0.5f * (r[0] + r[1]), cy = 0.5f * (r[2] + r[3]);
                const float bx = 2.0f * st.viewport[5] / 16777215.0f - 1.0f, by = 2.0f * st.viewport[2] / 16777215.0f;
                for (int c = 0; c < 4; c++) {
                    M[c * 4 + 0] = sx * (P[c * 4 + 0] - cx * P[c * 4 + 3]);
                    M[c * 4 + 1] = sy * (P[c * 4 + 1] - cy * P[c * 4 + 3]);
                    M[c * 4 + 2] = by * P[c * 4 + 2] + bx * P[c * 4 + 3];
                    M[c * 4 + 3] = P[c * 4 + 3];
                }
            } else {
                float faced[16];
                if (facing) facing_frame(P, faced);
                mat4_mul(facing ? faced : g_vr_hud_eff, P, a);   // the game's 2D frame, placed in view space
                mat4_mul(g_vr_view, a, b);  // that frame seen from this eye
                mat4_mul(g_vr_proj, b, M);
                if (frame_screen) {
                    // The shader works in the game's clip space, which the game's own
                    // projection gives it, taking a vertex on the picture's edge
                    // kFrameScreenReach times out on that axis; then the frame and the
                    // eye, without the projection. The reach goes in u_proj.
                    float A[16];
                    mat4_mul(g_vr_view, g_vr_hud_eff, a);
                    mat4_mul(g_vr_proj, a, A);
                    glUniformMatrix4fv(pr.u_view, 1, GL_FALSE, A);
                    glUniformMatrix4fv(pr.u_crop, 1, GL_FALSE, P);
                    memset(M, 0, sizeof(M));
                    M[0] = kFrameScreenReach;
                }
            }
            glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, M);
            if (flat3d) glUniformMatrix4fv(pr.u_crop, 1, GL_FALSE, flat_crop);
            glUniform1i(pr.u_vr, flat3d ? 5 : frame_screen ? 6 : 2);
            g_eye_layer_name = flat3d ? "hud-frame(3d)" : screen ? "eye-screen" : restore ? "eye-restore"
                             : facing ? "hud-frame(facing)" : frame_screen ? "hud-frame(screen)" : "hud-frame";
        } else if (g_flat_eye != 0.0f && perspective &&
                   !(g_panel_band > 0.0f && band_hi > band_lo && band_hi <= g_panel_band)) {
            // One of a stereo pair on the panel (a draw in the panel band falls through to
            // the plain projection, and so sits on the panel). The view is moved g_flat_eye to the side,
            // which shifts every point by -P00*e in clip x, and the frustum is sheared back
            // by e/halfw per unit of depth, which cancels that at the depth where the
            // frustum is as wide as the panel (D = P00 * halfw): a point there sits on the
            // panel in both eyes, and the disparity of one at depth z is e * (1 - D/z) of
            // the panel's half-width -- the eyes' own separation at infinity. In the
            // vertex shader clip x is P00*x + P[8]*z + P[12], so both terms are in P.
            //
            // The game's background band (render_set_depth_layers) is the exception: a sky
            // modelled some tens of units out -- a planet, the stars -- is at infinity, as
            // the eyes draw it. The shear alone gives every point the disparity of one at
            // infinity, so the move to the side is left out. With it, the planet of a
            // cinematic stood a few metres behind the panel while hiding nothing it should
            // not: its band kept its occlusion right and its depth wrong.
            float Q[16];
            memcpy(Q, P, sizeof(Q));
            Q[12] = background ? 0.0f : -P[0] * g_flat_eye;
            Q[8] -= g_flat_eye / g_flat_halfw;
            glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, Q);
            glUniform1i(pr.u_vr, 0);
        } else {
            glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, P);
            glUniform1i(pr.u_vr, 0);
        }
        // Fog runs off the depth GX would have written, which an eye does not write, so
        // the shader rebuilds it from the game's own projection -- the z row is all that
        // takes, plus how w is formed: -z under a frustum, 1 under an ortho batch.
        const float zp[4] = {p[4], p[5], perspective ? -1.0f : 0.0f, perspective ? 0.0f : 1.0f};
        glUniform4fv(pr.u_zproj, 1, zp);
        memcpy(u.proj, st.proj, sizeof(st.proj));
        u.view_space = st.view_space;
        u.background = background;
        u.foreground = foreground;
        u.fg_layer = fg_layer;
        u.hud_layer = hud_layer;
        u.flat3d = flat3d;
        u.screen = screen;
        u.frame_screen = frame_screen;
        u.restore = restore;
        u.facing = facing;
        u.hud_push = hud_push;
    }
    // Per state, whatever the projection: see g_frame_grad.
    if (frame_screen && g_cur_uvgrad && pr.u_uvgrad >= 0) glUniform4fv(pr.u_uvgrad, 8, g_cur_uvgrad);
    const float* vp = st.viewport;  // sx, sy, sz, ox, oy, oz
    // The anchor moves the viewport's x terms and the scissor's x edges, and with the viewport
    // the z freeze plane (zfreeze_plane), which is exact for a frozen draw under the anchor
    // of the triangle it came from. It moves nothing else.
    const AnchorMap an = draw_anchor_map(st);
    if (!(held && bp[0x59] == u.scissor_off && memcmp(vp, u.viewport, sizeof(st.viewport)) == 0 && an == u.anchor)) {
        g_astat.viewport++;
        float vpa[4];
        viewport_a(st, an, vpa);
        float vpb[4] = {2.0f * vp[5] / 16777215.0f - 1.0f, 2.0f * vp[2] / 16777215.0f, 0, 0};
        glUniform4fv(pr.u_vp_a, 1, vpa);
        glUniform4fv(pr.u_vp_b, 1, vpb);
        memcpy(u.viewport, vp, sizeof(st.viewport));
        u.anchor = an;
        u.scissor_off = bp[0x59];
    }
    // Only points read the point size, and a game that sets a line width per draw changes
    // this register with every line: so it is sent when a draw of points needs it, and a
    // record that is not current forgets what it held rather than keep a value it never sent.
    if (prim == 2) {
        if (!(held && bp[0x22] == u.psize_reg)) {
            g_astat.psize++;
            float psize = ((bp[0x22] >> 8) & 0xFF) / 6.0f * g_scale;
            glUniform1f(pr.u_point_size, psize < 1 ? 1 : psize);
            u.psize_reg = bp[0x22];
        }
    } else if (!held) {
        u.psize_reg = ~0u;
    }
    // A point's texture coordinates run across it, from the vertex's value to that plus the
    // point's texture offset, in each coordinate that has offsets enabled (GX's point
    // sprites: smoke, sparks and dust). GL gives each point one interpolated coordinate, so
    // the fragment shader spreads it with gl_PointCoord; every other draw sends 0.
    {
        const uint32_t key = prim == 2 ? point_tex_key(st) : 0;
        if (!(held && key == u.point_tex) && pr.u_points >= 0) {
            float off[8] = {};
            if (key) {
                const float range = kPointTexOffset[(bp[0x22] >> 19) & 7];
                for (int t = 0; t < 8; t++)
                    if ((key >> t) & 1) off[t] = range;
            }
            glUniform1i(pr.u_points, key != 0);
            if (pr.u_point_off >= 0) glUniform1fv(pr.u_point_off, 8, off);
            u.point_tex = key;
        }
    }

    // TEV registers
    if (!(held && memcmp(st.tev_reg, u.tev_reg, sizeof(st.tev_reg)) == 0 &&
          memcmp(st.tev_konst, u.tev_konst, sizeof(st.tev_konst)) == 0 && memcmp(bp + 0xF6, u.ksel, sizeof(u.ksel)) == 0)) {
        g_astat.tev++;
        GLint regs[16], kon[16];
        for (int r = 0; r < 4; r++) {
            uint32_t ra = st.tev_reg[r][0], bg = st.tev_reg[r][1];
            regs[r * 4 + 0] = sx11(ra); regs[r * 4 + 3] = sx11(ra >> 12);
            regs[r * 4 + 2] = sx11(bg); regs[r * 4 + 1] = sx11(bg >> 12);
            uint32_t ka = st.tev_konst[r][0], kb = st.tev_konst[r][1];
            kon[r * 4 + 0] = ka & 0xFF; kon[r * 4 + 3] = (ka >> 12) & 0xFF;
            kon[r * 4 + 2] = kb & 0xFF; kon[r * 4 + 1] = (kb >> 12) & 0xFF;
        }
        glUniform4iv(pr.u_reg, 4, regs);
        GLint kstage[64];
        stage_konsts(bp, kon, kstage);
        glUniform4iv(pr.u_kstage, 16, kstage);
        memcpy(u.tev_reg, st.tev_reg, sizeof(st.tev_reg));
        memcpy(u.tev_konst, st.tev_konst, sizeof(st.tev_konst));
        memcpy(u.ksel, bp + 0xF6, sizeof(u.ksel));
    }
    if (!(held && bp[0xF3] == u.aref)) {
        g_astat.aref++;
        GLint aref[2] = {(GLint)(bp[0xF3] & 0xFF), (GLint)((bp[0xF3] >> 8) & 0xFF)};
        glUniform2iv(pr.u_alpharef, 1, aref);
        u.aref = bp[0xF3];
    }
    // Only a shader with a Z texture has the bias, and few draws do: set every time.
    if (pr.u_zbias >= 0) glUniform1i(pr.u_zbias, (GLint)(bp[0xF4] & 0xFFFFFF));
    if (pr.u_dstalpha >= 0) glUniform1f(pr.u_dstalpha, (float)(bp[0x42] & 0xFF) / 255.0f);
    if (pr.u_zplane >= 0) glUniform4fv(pr.u_zplane, 1, g_zplane);
    // Indirect matrices
    if (!held_words(bp + 0x06, u.ind, 9)) {
        g_astat.ind++;
        GLint im[18];
        GLint isc[3];
        for (int m = 0; m < 3; m++) {
            uint32_t r0 = bp[0x06 + 3 * m], r1 = bp[0x07 + 3 * m], r2 = bp[0x08 + 3 * m];
            im[m * 6 + 0] = sx11(r0); im[m * 6 + 1] = sx11(r1); im[m * 6 + 2] = sx11(r2);
            im[m * 6 + 3] = sx11(r0 >> 11); im[m * 6 + 4] = sx11(r1 >> 11); im[m * 6 + 5] = sx11(r2 >> 11);
            isc[m] = (int)(((r0 >> 22) & 3) | (((r1 >> 22) & 3) << 2) | (((r2 >> 22) & 3) << 4)) - 17;
        }
        glUniform3iv(pr.u_indmtx, 6, im);
        glUniform1iv(pr.u_indscale, 3, isc);
        memcpy(u.ind, bp + 0x06, sizeof(u.ind));
    }
    if (!held_words(bp + 0x25, u.indcs, 2)) {
        g_astat.indcs++;
        float ics[8];
        for (int i = 0; i < 4; i++) {
            uint32_t ss = bp[0x25 + i / 2] >> ((i & 1) * 8);
            ics[i * 2] = 1.0f / (float)(1u << (ss & 15));
            ics[i * 2 + 1] = 1.0f / (float)(1u << ((ss >> 4) & 15));
        }
        glUniform2fv(pr.u_indcoordscale, 4, ics);
        memcpy(u.indcs, bp + 0x25, sizeof(u.indcs));
    }
    // Fog
    if (!held_words(bp + 0xEE, u.fog, 5)) {
        g_astat.fog++;
        float fog[4] = {fog_float(bp[0xEE]), fog_float(bp[0xF1]), (float)(bp[0xEF] & 0xFFFFFF), (float)(bp[0xF0] & 0x1F)};
        glUniform4fv(pr.u_fog, 1, fog);
        // GXSetFog's type: fsel (bits 21-23) in the low three bits, and the projection bit
        // (20, set for an orthographic one) on top. Until 2026-10-16 bits 20-23 were read as
        // the type, the projection bit as its lowest: exponential fog (4) came out as none,
        // linear (2) as exponential and exp2 (5) as an orthographic linear one.
        glUniform1i(pr.u_fogmode, (int)(((bp[0xF1] >> 21) & 7) | (((bp[0xF1] >> 20) & 1) << 3)));
        float fogc[3] = {((bp[0xF2] >> 16) & 0xFF) / 255.0f, ((bp[0xF2] >> 8) & 0xFF) / 255.0f, (bp[0xF2] & 0xFF) / 255.0f};
        glUniform3fv(pr.u_fogcolor, 1, fogc);
        memcpy(u.fog, bp + 0xEE, sizeof(u.fog));
    }

    // Which texgens feed a copy of the whole frame. In an eye those lookups are taken
    // over: the texture becomes the eye's own grab and the coordinate becomes the
    // fragment's own position, since the game's coordinate belongs to a view this eye is
    // not looking from. See grab_eye().
    uint32_t screen_uv = 0;
    if (g_vr_active) {
        const uint32_t nstg = ((bp[0x00] >> 10) & 15) + 1;
        for (uint32_t s = 0; s < nstg; s++) {
            const uint32_t order = bp[0x28 + s / 2] >> ((s & 1) * 12);
            const uint32_t map = order & 7;
            if ((order & 0x40) && st.tex_is_efb[map] && st.tex_id[map] &&
                is_eye_screen_tex(st.tex_id[map]))
                screen_uv |= 1u << ((order >> 3) & 7);
        }
    }
    if (!(view_held && screen_uv == u.screen_uv)) {
        g_astat.screen++;
        glUniform1i(pr.u_screen_uv, (GLint)screen_uv);
        glUniform2f(pr.u_screen_px, g_eye_w ? 1.0f / (float)g_eye_w : 0.0f,
                    g_eye_h ? 1.0f / (float)g_eye_h : 0.0f);
        u.screen_uv = screen_uv;
    }

    // The indirect offset that distorts a substituted lookup is an absolute displacement
    // in the copy's texels, and the copy's texels are EFB pixels. The eye grab's are not:
    // the eye sees a wider field across more pixels, so one of its pixels covers a
    // different angle. Both grids are pixels over an angle, so the conversion is the
    // ratio of their pixels per unit of frustum tangent -- the game's from its own
    // viewport and projection, the eye's from the target size and the headset's.
    //
    // Without it the droplets keep a displacement of a few pixels on a target several
    // times the EFB's width, which is a fraction of the distortion the game asked for:
    // each one degenerates into an almost exact copy of its own background and the spray
    // disappears rather than reading wrongly.
    //
    // The y term is negated because the two grids run opposite ways. An EFB copy's row 0
    // is the top of its source rect, so +t walks down the screen; the eye grab comes
    // straight off the render target, so its row 0 is the bottom and +t walks up -- which
    // is also the direction gl_FragCoord.y counts, and why the undistorted part of the
    // lookup needs no flip of its own.
    float ripple[2] = {1.0f, 1.0f};
    // GCN_EYE_RIPPLE=0 keeps the substitution but leaves the offset unconverted, which is
    // what the water surface shipped with and separates "the lookup is in the wrong
    // space" from "the distortion is the wrong size".
    static const bool no_ripple = getenv_nonempty("GCN_EYE_RIPPLE") && atoi(getenv_nonempty("GCN_EYE_RIPPLE")) == 0;
    if (screen_uv && perspective && !no_ripple) {
        const float gx = fabsf(vp[0]) * p[0], gy = fabsf(vp[1]) * p[2];
        if (gx > 0.0f) ripple[0] = 0.5f * (float)g_eye_w * g_vr_proj[0] / gx;
        if (gy > 0.0f) ripple[1] = -0.5f * (float)g_eye_h * g_vr_proj[5] / gy;
        // Mid-morph the scene is partly on the panel, where a unit of the game's frustum
        // tangent spans the panel's (half-width / distance) * p rather than one -- so the
        // eye's pixels cover a different share of it.
        if (g_morph < 1.0f) {
            const float D = -g_panel[14], k = g_morph;
            ripple[0] *= (1.0f - k) * p[0] * g_panel[0] / D + k;
            ripple[1] *= (1.0f - k) * p[2] * g_panel[5] / D + k;
        }
    }
    if (!(view_held && ripple[0] == u.ripple[0] && ripple[1] == u.ripple[1])) {
        g_astat.ripple++;
        glUniform2fv(pr.u_screen_ripple, 1, ripple);
        u.ripple[0] = ripple[0];
        u.ripple[1] = ripple[1];
    }

    // Textures. The bindings are global, so a unit already holding this texture under
    // this sampler is left alone whatever the program; the sizes are a uniform. The
    // texture and the sampler are bound separately: a frame here changed the texture on
    // a unit at nearly every draw and the sampler almost never.
    float tsz[16];
    // Depth copies are sampled nearest (GlTex::depth), and the shader snaps their
    // coordinate to the centre of the texel GX would read. A game reads such a copy back
    // with a quad whose coordinates run from texel centre to texel centre over exactly as
    // many pixels, which at internal scale 1 leaves the last row and column's coordinate
    // within 1/256 of a texel edge: the GPU's own subtexel rounding then reads the wrong
    // texel, a different depth, and a fog volume indexing its ramp with those bytes drew a
    // line along the edge of every chunk it was copied in. In texels the margin is clear
    // (half a texel over the chunk), so the snap is done there in float.
    uint32_t texsnap = 0;
    for (int m = 0; m < 8; m++) {
        tsz[m * 2] = tsz[m * 2 + 1] = 1.0f;
        uint32_t id = st.tex_id[m];
        const bool sub = screen_uv && st.tex_is_efb[m] && id && is_eye_screen_tex(id);
        GLuint sampler = 0;
        const GlTex* gt = nullptr;
        if (sub) {
            const EyeGrab& g = eye_grab_for(id);
            tsz[m * 2] = (float)g.w;
            tsz[m * 2 + 1] = (float)g.h;
        } else if (id) {
            auto it = g_textures.find(id);
            if (it != g_textures.end()) {
                gt = &it->second;
                uint32_t base = m < 4 ? 0x80 + m : 0xA0 + (m - 4);
                uint32_t img0 = bp[base + 8];
                tsz[m * 2] = (float)((img0 & 0x3FF) + 1);
                tsz[m * 2 + 1] = (float)(((img0 >> 10) & 0x3FF) + 1);
                const uint32_t mode0 = gt->depth ? bp[base] & ~0xF0u : bp[base];  // see GlTex::depth
                sampler = get_sampler(mode0, bp[base + 4], gt->efb ? 1 : gt->levels);
                if (gt->depth) texsnap |= 1u << m;
            } else {
                id = 0;
            }
        }
        const bool tex_held = g_applied.valid && g_applied.tex_id[m] == id && g_applied.tex_sub[m] == sub;
        const bool samp_held = g_applied.valid && g_applied.sampler[m] == sampler;
        if (tex_held && samp_held) continue;
        glActiveTexture(GL_TEXTURE0 + m);
        if (!tex_held) {
            g_astat.texbind++;
            if (sub) glBindTexture(GL_TEXTURE_2D, eye_grab_for(id).tex);
            else glBindTexture(GL_TEXTURE_2D, gt ? gt->tex : 0);
            g_applied.tex_id[m] = id;
            g_applied.tex_sub[m] = sub;
        }
        if (!samp_held) {
            // The grab carries its own filtering; a sampler object would override it, so
            // a substituted unit gets none (0), as does an empty one.
            g_astat.sampbind++;
            glBindSampler(m, sampler);
            g_applied.sampler[m] = sampler;
        }
    }
    // The palettes an EFB copy is read through (PixelState::tex_pal), fetched texel by
    // texel, so whatever sampler the unit has does not matter.
    for (int m = 0; m < 8; m++) {
        uint32_t id = st.tex_is_efb[m] ? st.tex_pal[m] : 0;
        GLuint tex = 0;
        if (id) {
            auto it = g_textures.find(id);
            if (it != g_textures.end()) {
                tex = it->second.tex;
            } else {
                id = 0;
            }
        }
        if (g_applied.valid && g_applied.pal_id[m] == id) continue;
        if (!id && !g_applied.valid) { g_applied.pal_id[m] = 0; continue; }
        glActiveTexture(GL_TEXTURE8 + m);
        glBindTexture(GL_TEXTURE_2D, tex);
        g_applied.pal_id[m] = id;
    }
    if (!(held && memcmp(tsz, u.tsz, sizeof(tsz)) == 0)) {
        g_astat.tsz++;
        glUniform2fv(pr.u_texsize, 8, tsz);
        memcpy(u.tsz, tsz, sizeof(tsz));
    }
    if (!(held && u.texsnap == texsnap)) {
        glUniform1i(pr.u_texsnap, (GLint)texsnap);
        u.texsnap = texsnap;
    }
    u.gen = g_uniform_gen;
    u.view_gen = g_view_gen;
    // Blend / logic op, colour mask
    const bool same_blend = g_applied.valid && bp[0x41] == abp[0x41] && bp[0x42] == abp[0x42] && bp[0x43] == abp[0x43];
    uint32_t bm = bp[0x41];
    if (!same_blend) {
        g_astat.blend++;
        bool blend = bm & 1, logic = (bm >> 1) & 1, sub = (bm >> 11) & 1;
        // A constant destination alpha (dst_alpha_mode) is the shader's alpha output, kept
        // as it is; the colour's source-alpha factors read the TEV's from source 1.
        const uint32_t dst_alpha = dst_alpha_mode(bp);
        if (sub) {
            glEnable(GL_BLEND);
            set_logic_op_off();
            if (dst_alpha) {
                glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
                glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
            } else {
                glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
                glBlendFunc(GL_ONE, GL_ONE);
            }
        } else if (blend) {
            glEnable(GL_BLEND);
            set_logic_op_off();
            glBlendEquation(GL_FUNC_ADD);
            const BlendFactors bf = blend_factors(bm, bp[0x43]);
            GLenum sf = kBlendSrc[bf.src], df = kBlendDst[bf.dst];
            if (dst_alpha) {
                auto src1 = [](GLenum f) {
                    return f == GL_SRC_ALPHA ? (GLenum)GL_SRC1_ALPHA
                         : f == GL_ONE_MINUS_SRC_ALPHA ? (GLenum)GL_ONE_MINUS_SRC1_ALPHA : f;
                };
                glBlendFuncSeparate(src1(sf), src1(df), GL_ONE, GL_ZERO);
            } else {
                glBlendFunc(sf, df);
            }
        } else if (logic) {
            glDisable(GL_BLEND);
            set_logic_op(bm >> 12);
        } else {
            glDisable(GL_BLEND);
            set_logic_op_off();
        }
        bool has_alpha = efb_has_alpha(bp[0x43]);
        GLboolean cw = (bm >> 3) & 1, aw = ((bm >> 4) & 1) && has_alpha;
        glColorMask(cw, cw, cw, aw);
    }
    if (!same || !same_blend) {
        const uint64_t state = (uint64_t)pr.prog << 32 ^ bp[0x41] ^ (uint64_t)(bp[0x43] & 7) << 16 ^
                               (uint64_t)dst_alpha_mode(bp) << 20;
        if (g_seen_states.insert(state).second) g_state_new = true;
    }
    // Depth. In an eye the HUD is a quad out in the world rather than something laid over
    // the finished image, so the game's depth state no longer places it: the scene it is
    // meant to sit over is mostly nearer than the frame, and every element of the HUD is
    // on one plane. It is submitted last, so submission order is the layering. A 3D draw
    // composed into the HUD (flat3d) keeps the game's depth state: the eye cleared its
    // depth before the first of them (render_execute_eye), so they sort only among
    // themselves, by the game's own depth. A cut-away (EyeRestore) keeps it too: the depth
    // test is the whole of what it does.
    const bool hud_depth = g_vr_active && on_hud_frame && !flat3d && !restore;
    if (!(g_applied.valid && bp[0x40] == abp[0x40] && hud_depth == g_applied.hud_depth)) {
        g_astat.depth++;
        uint32_t zm = bp[0x40];
        if (zm & 1) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(kDepthFunc[(zm >> 1) & 7]);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask((zm >> 4) & 1);
        if (hud_depth) {
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
        }
        g_applied.hud_depth = hud_depth;
    }
    // Cull
    uint32_t cull = (bp[0x00] >> 14) & 3;
    if (prim == 0) {
        // GX: 1 = cull front, 2 = cull back. GX front faces are clockwise with y down,
        // which is counter-clockwise after our y flip (GL's default front face).
        const int want = cull == 0 ? 0 : cull == 3 ? 3 : ((cull == 2) != g_cull_swap ? 2 : 1);
        if (want != g_gl_cull) {
            g_astat.cull++;
            if (!want) {
                glDisable(GL_CULL_FACE);
            } else {
                glEnable(GL_CULL_FACE);
                glCullFace(want == 3 ? GL_FRONT_AND_BACK : want == 2 ? GL_BACK : GL_FRONT);
            }
            g_gl_cull = want;
        }
    }
    // Scissor (EFB coords, y down), measured from the same origin as the viewport.
    const bool same_scissor = g_applied.valid && bp[0x20] == abp[0x20] && bp[0x21] == abp[0x21] && bp[0x59] == abp[0x59] &&
                              an == g_applied.anchor;
    if (!same_scissor) {
        g_astat.scissor++;
        int x0 = (int)(bp[0x20] >> 12 & 0x7FF) - xoff, y0 = (int)(bp[0x20] & 0x7FF) - yoff;
        int x1 = (int)(bp[0x21] >> 12 & 0x7FF) - xoff + 1, y1 = (int)(bp[0x21] & 0x7FF) - yoff + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > EFB_W) x1 = EFB_W;
        if (y1 > EFB_H) y1 = EFB_H;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        if (g_vr_active) {
            // The scissor rect is in EFB coordinates, which say nothing about an eye's
            // render target.
            //
            // GCN_EYE_SCISSORLOG reports the draws whose rect is not the whole frame, which
            // are the ones this is throwing away clipping for. Anything the game relies on
            // the scissor to hide is drawn in full in an eye.
            static const bool slog = getenv_nonempty("GCN_EYE_SCISSORLOG") != nullptr;
            if (slog && (x0 > 0 || y0 > 0 || x1 < EFB_W || y1 < EFB_H)) {
                static int shown;
                if (shown++ < 40)
                    fprintf(stderr, "[scissor] f%u rect=%d,%d..%d,%d (frame is 0,0..%d,%d)\n",
                            g_render_frame, x0, y0, x1, y1, EFB_W, EFB_H);
            }
            glDisable(GL_SCISSOR_TEST);
        } else {
            glEnable(GL_SCISSOR_TEST);
            int left = x0, right = x1;
            anchor_scissor(an.a, an.x0, an.x1, an.k, g_scale, &left, &right);
            glScissor(left, (EFB_H - y1) * g_scale, right - left, (y1 - y0) * g_scale);
        }
    }
    // Everything above has now been compared against bp, so it becomes the reference.
    memcpy(g_applied.bp, bp, sizeof(g_applied.bp));
    g_applied.anchor = an;
    g_applied.valid = true;
}

}  // namespace gx
