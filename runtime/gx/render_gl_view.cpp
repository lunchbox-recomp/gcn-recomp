// Where an eye looks from: the eye's matrices from the frontend, the game's hooks and depth
// layers, the HUD frame, and the morph between theater and stereo.
#include "render_gl_internal.h"

namespace gx {

// VR eye state. When active, perspective batches are re-projected for the eye and the
// orthographic ones (the 2D HUD) are painted on a frame standing out in front of the
// game's camera -- see render_hud_frame.
bool g_vr_active = false;
// An eye's frame is being drawn, the flat pass that feeds its copies included, which runs
// before g_vr_active is set. A headset shows the picture at the shape the game drew it, so
// draw anchors and the output aspect do not apply (anchor_scale).
bool g_eye_frame = false;
float g_vr_proj[16], g_vr_view[16];
// The eye's view as the frontend gave it (render_set_vr_eye). g_vr_view is this times the
// batch's view turn (Batch::view_turn, set_view_turn), from the camera the game turned to
// the one the viewer stands at, so that the turn leaves the world where it was.
static float g_vr_view_head[16];
// The eye's view with the world's pitch taken out, for world geometry. The HUD frame uses
// g_vr_view untouched -- it is placed in the headset's space, not the game's.
float g_vr_view_world[16];
// Depth layers in stereo (render_set_depth_layers). Both are off unless a game turns them
// on, because where a game puts its layers in the depth buffer -- if it uses them at all --
// is the game's own convention.
//
// Background: a perspective draw whose viewport confines it to depths from g_bg_from to
// 1 is drawn at infinity, through g_vr_view_sky, g_vr_view_world with its translation
// taken out. 0 is off.
float g_vr_view_sky[16];
float g_bg_from = 0.0f;
// Foreground: a perspective draw whose viewport confines it to depths no further than
// g_fg_to is scaled towards the camera by g_fg_scale -- the same angular size in each eye,
// but that much nearer and that much smaller. 0 is off.
float g_fg_to = 0.0f;
float g_fg_scale = 1.0f;
float g_vr_view_fg[16], g_vr_view_world_fg[16];
// A HUD layer: the same, for a perspective draw confined to depths no further than
// g_hud_to, with its own scale -- a game that models its HUD as geometry a long way out
// in view space, in a band of its own nearer than the weapon's, wants it nearer than the
// weapon's scale would put it. Takes precedence over the foreground. 0 is off.
float g_hud_to = 0.0f;
float g_hud_scale = 1.0f;
// render_set_hud_push: below which distance a HUD-layer draw is pushed out, and to where.
// g_hud_push holds each state's factor for the frame (1 for none), g_cur_hud_push the
// current draw's, read by apply_state.
float g_hud_push_below = 0.0f, g_hud_push_to = 0.0f;
std::vector<float> g_hud_push;
float g_cur_hud_push = 1.0f;
float g_vr_view_hud[16], g_vr_view_world_hud[16];
// Whether a perspective draw placed in view space is a billboard in the world rather than
// something held before the camera (render_set_view_space_world). Off unless a game says.
bool g_view_space_world = false;
float g_world_pitch = 0.0f;
static float g_vr_hud[16];
// What the HUD frame goes through before the eye's view: the identity, unless the game's
// eye hook put something there -- an eye that tilts with a vehicle wants the HUD turned
// back to its level frame. g_vr_hud_eff is this times g_vr_hud, and is what the HUD is
// drawn with.
float g_hud_xform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
float g_vr_hud_eff[16];
// What world geometry goes through before the eye's own view: the chase camera's pitch
// taken back out (world_pitch_matrix), or whatever the game's eye hook put there.
// g_vr_view_world is g_vr_view times this.
float g_world_xform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
// First person (render_set_first_person): while on, the game's eye hook is asked per frame
// where the eye stands, and g_eye_overridden says it answered for the frame being drawn.
EyeHook g_eye_hook = nullptr;
WorldPitchHook g_pitch_hook = nullptr;
// Which placement apply_state gave the last state in an eye, for the eye's draw log.
const char* g_eye_layer_name = "-";
bool g_fp_on = false;
bool g_eye_overridden = false;
// The morph between theater and stereo; see render_set_vr_morph. 1 is plain stereo.
float g_morph = 1.0f;
// A flat frame drawn as one of a stereo pair (render_execute_stereo_pair): where the eye
// stands beside the game's camera, 0 for the plain flat frame, and half the width of the
// panel the pair is shown on, both in game units.
float g_flat_eye = 0.0f, g_flat_halfw = 1.0f;
float g_panel_band = 0.0f;   // render_set_panel_band
float g_panel[16];
// Whether the vertex shader can write clip distances, which is what crops the morph.
bool g_clip_ok = false;
// An EyeScreen draw's matrix: the game's frame mapped onto the eye's own clip space,
// enlarged past the field's edges, with no view between them.
void eye_screen_matrix(const float P[16], float M[16]) {
    for (int c = 0; c < 4; c++) {
        M[c * 4 + 0] = kEyeScreenOverscan * P[c * 4 + 0];
        M[c * 4 + 1] = kEyeScreenOverscan * P[c * 4 + 1];
        M[c * 4 + 2] = 0.0f;
        M[c * 4 + 3] = P[c * 4 + 3];
    }
}

// c = a * b, column-major, element (row r, column k) at m[k * 4 + r].
void mat4_mul(const float* a, const float* b, float* c) {
    for (int k = 0; k < 4; k++)
        for (int r = 0; r < 4; r++) {
            float sum = 0;
            for (int i = 0; i < 4; i++) sum += a[i * 4 + r] * b[k * 4 + i];
            c[k * 4 + r] = sum;
        }
}

// The turn yaw(y) then pitch(x), column-major: one that takes -z, the way ahead, to the
// direction `yaw` left and `pitch` up of it.
static void yaw_pitch_matrix(float yaw, float pitch, float m[16]) {
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    const float R[16] = {cy, 0, -sy, 0, sy * sp, cp, cy * sp, 0, sy * cp, -sp, cy * cp, 0, 0, 0, 0, 1};
    memcpy(m, R, sizeof(R));
}

// An EyeFacing draw's frame. The game centred its panel on the point (cx, cy) of its clip
// space by translating its orthographic projection, so on the HUD frame (g_vr_hud_eff,
// clip to view space) the panel lies off the frame's centre and is seen at a slant. Here
// the panel goes back to the centre, and the frame is turned about the camera -- yaw, then
// pitch, no roll -- from the direction of its centre to that of the point: the panel is
// where it was, as far away as the frame's centre, square on to the camera.
void facing_frame(const float P[16], float out[16]) {
    const float* F = g_vr_hud_eff;
    const float cx = P[12], cy = P[13];   // the panel's origin in clip space, w being 1
    const float C[3] = {F[12], F[13], F[14]};
    const float Q[3] = {F[0] * cx + F[4] * cy + F[12], F[1] * cx + F[5] * cy + F[13], F[2] * cx + F[6] * cy + F[14]};
    auto yaw = [](const float* v) { return atan2f(-v[0], -v[2]); };
    auto pitch = [](const float* v) { return atan2f(v[1], sqrtf(v[0] * v[0] + v[2] * v[2])); };
    float to[16], from[16], back[16], R[16], RF[16];
    yaw_pitch_matrix(yaw(Q), pitch(Q), to);
    yaw_pitch_matrix(yaw(C), pitch(C), from);
    for (int k = 0; k < 4; k++)
        for (int r = 0; r < 4; r++) back[k * 4 + r] = from[r * 4 + k];   // a rotation's inverse
    mat4_mul(to, back, R);
    mat4_mul(R, F, RF);
    const float T[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -cx, -cy, 0, 1};
    mat4_mul(RF, T, out);
}

// The camera looks down by `pitch`, so the world's up arrives at (0, cos, sin) in the
// vertices' own frame. Rotating about X by -pitch takes it back to (0, 1, 0), which is the
// headset's up, and the sea with it.
void world_pitch_matrix(float R[16]) {
    const float c = cosf(g_world_pitch), s = sinf(g_world_pitch);
    memset(R, 0, 16 * sizeof(float));
    R[0] = 1.0f;
    R[5] = c;  R[6] = -s;
    R[9] = s;  R[10] = c;
    R[15] = 1.0f;
}

void compose_world_view() {
    view_invalidate();
    mat4_mul(g_vr_view, g_world_xform, g_vr_view_world);
    memcpy(g_vr_view_sky, g_vr_view_world, sizeof(g_vr_view_sky));
    g_vr_view_sky[12] = g_vr_view_sky[13] = g_vr_view_sky[14] = 0.0f;
    const float S[16] = {g_fg_scale, 0, 0, 0, 0, g_fg_scale, 0, 0, 0, 0, g_fg_scale, 0, 0, 0, 0, 1};
    mat4_mul(g_vr_view, S, g_vr_view_fg);
    mat4_mul(g_vr_view_world, S, g_vr_view_world_fg);
    const float H[16] = {g_hud_scale, 0, 0, 0, 0, g_hud_scale, 0, 0, 0, 0, g_hud_scale, 0, 0, 0, 0, 1};
    mat4_mul(g_vr_view, H, g_vr_view_hud);
    mat4_mul(g_vr_view_world, H, g_vr_view_world_hud);
    mat4_mul(g_hud_xform, g_vr_hud, g_vr_hud_eff);
}

void hud_upright() {
    memset(g_hud_xform, 0, sizeof(g_hud_xform));
    g_hud_xform[0] = g_hud_xform[5] = g_hud_xform[10] = g_hud_xform[15] = 1.0f;
}

void render_set_world_pitch(float pitch_rad) {
    g_world_pitch = pitch_rad;
    if (!g_eye_overridden) world_pitch_matrix(g_world_xform);
    compose_world_view();
}

void render_set_depth_layers(float background_from, float foreground_to, float foreground_scale,
                             float hud_to, float hud_scale) {
    g_bg_from = background_from;
    g_fg_to = foreground_to;
    g_fg_scale = foreground_scale > 0.0f ? foreground_scale : 1.0f;
    g_hud_to = hud_to;
    g_hud_scale = hud_scale > 0.0f ? hud_scale : 1.0f;
    compose_world_view();
}

void render_set_view_space_world(bool on) { g_view_space_world = on; }
void render_set_hud_push(float below, float to) {
    g_hud_push_below = below > 0.0f && to > 0.0f ? below : 0.0f;
    g_hud_push_to = to;
}

void render_set_eye_hook(EyeHook hook) { g_eye_hook = hook; }
void render_set_world_pitch_hook(WorldPitchHook hook) { g_pitch_hook = hook; }

bool render_first_person() { return g_fp_on; }

void render_set_first_person(bool on) {
    g_fp_on = on;
    if (!on) {
        g_eye_overridden = false;
        hud_upright();
        world_pitch_matrix(g_world_xform);
        compose_world_view();
    }
}

void render_set_vr_eye(const float proj[16], const float view[16], const float hud[16]) {
    memcpy(g_vr_proj, proj, sizeof(g_vr_proj));
    memcpy(g_vr_view_head, view, sizeof(g_vr_view_head));
    memcpy(g_vr_view, view, sizeof(g_vr_view));
    memcpy(g_vr_hud, hud, sizeof(g_vr_hud));
    compose_world_view();
}

// g_vr_view for `b`: the head's view times the batch's view turn, a rotation taking what
// the game placed about its turned camera back to where the viewer's frame has it.
void apply_view_turn(const Batch& b) {
    const float x = b.view_turn[0], y = b.view_turn[1], z = b.view_turn[2], w = b.view_turn[3];
    const float R[16] = {
        1 - 2 * (y * y + z * z), 2 * (x * y + w * z),     2 * (x * z - w * y),     0,
        2 * (x * y - w * z),     1 - 2 * (x * x + z * z), 2 * (y * z + w * x),     0,
        2 * (x * z + w * y),     2 * (y * z - w * x),     1 - 2 * (x * x + y * y), 0,
        0,                       0,                       0,                       1,
    };
    mat4_mul(g_vr_view_head, R, g_vr_view);
    compose_world_view();
}

void render_set_vr_morph(float t, const float panel[16]) {
    // Exactly 0 puts every vertex on the panel's plane, where the depth test has nothing
    // to sort by: the sea drew over the racer. Any step off it restores the order, since
    // depth along each line of sight stays monotonic -- 0.002 already matches the flat
    // frame on the device, and puts the scene within a few millimetres of the panel.
    constexpr float kFloor = 0.002f;
    g_morph = t >= 1.0f ? 1.0f : kFloor + (1.0f - kFloor) * (t < 0.0f ? 0.0f : t);
    if (panel) memcpy(g_panel, panel, sizeof(g_panel));
    view_invalidate();
}

// One draw's matrices part way between theater and stereo.
//
// Theater is the game's frame on a panel: a vertex goes through the game's projection P and
// lands on the panel at the frame position that gives -- g_panel * P, the same thing the
// HUD path does with its own frame. Stereo is wherever the eye path would put it. The two
// are blended as homogeneous points, which keeps the whole chain one matrix per draw and
// keeps the GPU's clipping correct all the way through: a vertex behind the game's camera
// still has w < 0 on the panel side and is still cut, which a per-vertex divide would lose.
//
// Blending homogeneous points weights each by its w. The panel side's w is the vertex's
// depth from the game's camera; the stereo side's is 1, so it is scaled by the panel's own
// distance D. The weights then come out as (1-k)*z and k*D, which is exactly interpolation
// in 1/depth: every vertex's disparity grows linearly with k, rather than the far scenery
// sitting on the panel until the last moment and then leaving all at once. When the stereo
// side was itself built from P (the HUD frame), the two share a w and no scaling is wanted.
//
// For the same reason, a vertex slides along a line between two points that the game's
// camera sees in nearly the same direction; from where that camera stands the picture
// barely moves, and only depth comes in. It is not exact, because the panel is not exactly
// the game's field of view and the world is pitched in stereo but not on the panel.
//
// `crop` gives four clip distances that cut the scene to a window of the game's frustum
// widened by s in each axis: s = 1 is the frame the panel shows, and s grows without bound
// as k reaches 1. In stereo the eye sees far more than the game's frustum, and on the panel
// none of that may show -- it would spill round the edges of the frame.
void morph_chain(const float P[16], MorphKind kind, float chain[16], float fog[16],
                 float crop[16]) {
    const float k = g_morph;
    const float D = -g_panel[14];
    float T[16], S[16], sigma = D;
    mat4_mul(g_panel, P, T);
    memset(fog, 0, 16 * sizeof(float));
    fog[0] = fog[5] = fog[10] = fog[15] = 1.0f;
    if (kind == MorphKind::Screen) {
        // An EyeScreen draw ends across the whole eye, in the eye's clip space, so the two
        // ends are blended there: the panel's chain, whose w is the panel's distance, and
        // the screen's, whose w is 1 and is scaled by that distance to match. It leaves the
        // panel for the eye's edges rather than for the HUD frame, where a film with dark
        // borders showed them as bands across the HUD for the whole transition.
        float VT[16], PVT[16], Ms[16];
        mat4_mul(g_vr_view, T, VT);
        mat4_mul(g_vr_proj, VT, PVT);
        eye_screen_matrix(P, Ms);
        for (int i = 0; i < 16; i++) chain[i] = (1.0f - k) * PVT[i] + k * D * Ms[i];
        memset(crop, 0, 16 * sizeof(float));
        for (int i = 0; i < 4; i++) crop[12 + i] = 1.0f;
        return;
    }
    switch (kind) {
    case MorphKind::World:
        memcpy(S, g_world_xform, sizeof(S));
        for (int i = 0; i < 16; i++) fog[i] = (1.0f - k) * fog[i] + k * g_vr_view_world[i];
        break;
    case MorphKind::CameraHeld:
        memset(S, 0, sizeof(S));
        S[0] = S[5] = S[10] = S[15] = 1.0f;
        for (int i = 0; i < 16; i++) fog[i] = (1.0f - k) * fog[i] + k * g_vr_view[i];
        break;
    case MorphKind::Hud:
        mat4_mul(g_vr_hud_eff, P, S);
        sigma = 1.0f;
        break;
    }
    float M[16], VM[16];
    for (int i = 0; i < 16; i++) M[i] = (1.0f - k) * T[i] + k * sigma * S[i];
    mat4_mul(g_vr_view, M, VM);
    mat4_mul(g_vr_proj, VM, chain);

    memset(crop, 0, 16 * sizeof(float));
    if (kind == MorphKind::Hud) {
        // The HUD maps the game's frame onto its own at both ends; there is nothing outside
        // it to hide.
        for (int i = 0; i < 4; i++) crop[12 + i] = 1.0f;
        return;
    }
    const float s = 1.0f / fmaxf(1.0f - k, 1e-4f);
    // Row r of P is (P[r], P[4+r], P[8+r], P[12+r]); distance i is crop's row i.
    for (int c = 0; c < 4; c++) {
        const float w = s * P[c * 4 + 3], x = P[c * 4 + 0], y = P[c * 4 + 1];
        crop[c * 4 + 0] = w - x;
        crop[c * 4 + 1] = w + x;
        crop[c * 4 + 2] = w - y;
        crop[c * 4 + 3] = w + y;
    }
    // However wide s opens them, planes through the game's camera still hide everything
    // behind its image plane -- and that camera looks down, so with the head turned the
    // sky off to the side is behind it, and stayed black until the morph finished and it
    // popped in. So the planes also back away from the camera, by an amount that is
    // nothing at the start and passes the far end of the world well before the end.
    const float back = D * (s * s - 1.0f);
    for (int i = 0; i < 4; i++) crop[12 + i] += back;
}

// Paints what the panel shows where the game drew nothing: the EFB's clear colour, inside
// the window the crop leaves open and black outside it, as theater is black around its
// panel. A quad well out in the main camera's frustum, morphed like the world -- so it is
// the panel's own rectangle at the start and opens out with the crop. Drawn first, with no
// depth, so the scene simply covers it.
void draw_morph_background(const float P[16], const float rgb[3]) {
    float chain[16], fog[16], crop[16];
    morph_chain(P, MorphKind::World, chain, fog, crop);
    const float s = 1.0f / fmaxf(1.0f - g_morph, 1e-4f);
    const float z = 4000.0f;   // game units; anywhere inside both frustums does
    glUseProgram(g_bg_prog);
    glUniformMatrix4fv(g_bg_u_mvp, 1, GL_FALSE, chain);
    glUniformMatrix4fv(g_bg_u_crop, 1, GL_FALSE, crop);
    // Half again past the crop on each side, so the crop and not the quad is the edge.
    glUniform4f(g_bg_u_quad, 1.5f * s * z / P[0], 1.5f * s * z / P[5], -z, 0.0f);
    glUniform3f(g_bg_u_col, rgb[0], rgb[1], rgb[2]);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    set_logic_op_off();
    glDisable(GL_CULL_FACE);
    g_gl_cull = 0;
    glColorMask(1, 1, 1, 1);
    glBindVertexArray(g_copy_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(g_vao);
    glDepthMask(GL_TRUE);
}

// The frame the HUD is painted on in stereo: a quad `dist` game units ahead of the game's
// camera, `scale` of the vertical field of view tall and 4:3 wide, with the game's own 2D
// frame mapped onto it corner to corner. It is anchored to the camera rather than to the
// head, so it frames the race while the viewer looks forward and stays where it is when
// they turn to look at something else.
//
// `height` lifts its centre off the forward axis and `pitch_rad` leans the top away, both
// because where a panel wants to hang is a question about a person and not about geometry.
// At 0 and 0 the frame stands vertical and centred on the axis, which is where the eye was
// at the moment the runtime fixed its LOCAL space.
//
// Writing NDC straight into the eye -- what this replaced -- cannot work in stereo. The
// headset's per-eye frustums are asymmetric, so one NDC position is a different direction
// in each eye; there is no depth at which the two images agree, and they never fuse.
void render_hud_frame(float dist, float tan_half_fovy, float scale, float height,
                      float pitch_rad, float out[16]) {
    const float half_h = dist * tan_half_fovy * scale;
    const float half_w = half_h * 4.0f / 3.0f;   // the game's frame is 4:3
    const float c = cosf(pitch_rad), s = sinf(pitch_rad);
    memset(out, 0, 16 * sizeof(float));
    // X: the frame's own right, which the tilt leaves alone.
    out[0] = half_w;
    // Y: its up, leaned back by the pitch. GX clip space and view space both point Y up.
    out[5] = half_h * c;
    out[6] = -half_h * s;
    // W: the centre, `height` up from the forward axis and `dist` down it (-Z forward).
    out[13] = height;
    out[14] = -dist;
    out[15] = 1.0f;
    // The Z column stays zero: every element lands on the plane of the frame. What that
    // costs is the overlay's own depth ordering, which is why the eye path drops the
    // depth test for these draws and lets submission order do the layering.
}

}  // namespace gx
