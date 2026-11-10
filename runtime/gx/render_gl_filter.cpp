// What the game's eye hooks and draw filter say about a batch, kept as lists per command
// and per state for the eye pass and apply_state: the draws an eye hides, draws last, or
// places apart from the world.
#include "render_gl_internal.h"

namespace gx {

static EyeFilter g_eye_filter = nullptr;

// Per command of the batch being drawn: 1 for a draw the game's eye leaves out (the
// player's own model, say). Empty when nothing is hidden.
std::vector<uint8_t> g_hide;
// Per command: 1 for a draw of an object the game's eye filter asked for last
// (EyeLateObject), which g_hide also leaves out of its place. Empty when there is none.
std::vector<uint8_t> g_late;
// Per pixel state of the batch being drawn: 1 for one the game's eye filter called a 3D
// draw composed into the 2D picture (EyeFlat), which an eye paints on the HUD frame where
// the flat frame puts it. Empty when there is none. g_cur_flat3d is the state apply_state
// is applying, in an eye.
std::vector<uint8_t> g_flat3d;
bool g_cur_flat3d = false;
// Likewise for an orthographic draw over the whole picture (EyeScreen), which an eye draws
// across its whole field, fixed to the head: the game's frame mapped onto the eye's own clip
// space, enlarged by kEyeScreenOverscan so that its edges fall outside what the eye can see.
// 1 per such state; 2 for one drawn on the HUD frame with its edges taken out past the
// field (EyeFrameScreen).
std::vector<uint8_t> g_screen;
uint8_t g_cur_screen = 0;
// Per EyeFrameScreen state, eight texture coordinates' gradients over the game's clip x
// and y (ds/dx, ds/dy, dt/dx, dt/dy), fitted to the state's own vertices: what the shader
// carries a coordinate out by with its vertex. Zero for a coordinate that is not linear in
// position over the draw, which then stays as it is. g_cur_uvgrad is the current draw's.
std::vector<float> g_frame_grad;
const float* g_cur_uvgrad = nullptr;
// And for a copy of part of the frame put back over itself with a depth test (EyeRestore):
// per state, 1 for such a draw, with the box its draws cover in the game's clip space
// (four floats a state), which an eye stretches over its whole field. g_restore_tex holds
// the copies those draws sample, at which the eye takes its own grab (g_restore_grab).
std::vector<uint8_t> g_restore;
std::vector<float> g_restore_box;
std::vector<uint32_t> g_restore_tex;
bool g_cur_restore = false;
const float* g_cur_restore_box = nullptr;
// And for an orthographic panel the game moves about its picture by its projection's
// translation (EyeFacing): 1 per such state, drawn on the HUD frame turned to face it.
std::vector<uint8_t> g_facing;
bool g_cur_facing = false;
// And for a perspective draw in a scaled layer's band that is drawn at its modelled
// distance all the same (EyeTrueScale).
std::vector<uint8_t> g_true_scale;
bool g_cur_true_scale = false;
// What the frame's facing panels (EyeFacing) cover together in the game's clip space, for
// the late objects inside it (late_panel_view); g_has_facing_box false when there is none.
// g_cur_late_view is the view apply_state draws such an object with, null otherwise.
float g_facing_box[4];
bool g_has_facing_box = false;
const float* g_cur_late_view = nullptr;

static FrameHook g_frame_hook = nullptr;
void render_set_frame_hook(FrameHook hook) { g_frame_hook = hook; }
// The game's look at a batch about to be drawn (render_set_frame_hook). `skip` is the
// off-screen passes if the caller has them already.
void frame_observe(const Batch& b, const std::vector<uint8_t>* skip) {
    if (!g_frame_hook) return;
    static std::vector<uint8_t> own;
    if (!skip) {
        mark_offscreen_passes(b, own);
        skip = &own;
    }
    g_frame_hook(b, *skip);
}
void render_set_eye_filter(EyeFilter filter) { g_eye_filter = filter; }

// What an eye filter is told about a draw; see EyeFilter.
static EyeDrawFacts draw_facts(const PixelState& st) {
    EyeDrawFacts f{};
    f.ortho = (int)st.proj[6] != 0;
    f.indirect = ((st.bp[0x00] >> 16) & 7) != 0;
    f.band_hi = st.viewport[5] / 16777215.0f;
    f.band_lo = (st.viewport[5] - fabsf(st.viewport[2])) / 16777215.0f;
    int xoff, yoff;
    scissor_offset(st.bp, xoff, yoff);
    f.vp_w = 2.0f * fabsf(st.viewport[0]);
    f.vp_h = 2.0f * fabsf(st.viewport[1]);
    f.vp_x = st.viewport[3] - xoff - 0.5f * f.vp_w;
    f.vp_y = st.viewport[4] - yoff - 0.5f * f.vp_h;
    for (int i = 0; i < 8; i++) {
        if (!st.tex_id[i]) continue;
        if (st.tex_is_efb[i]) {
            f.samples_copy = true;
            if (is_fullscreen_tex(st.tex_id[i])) f.samples_fullscreen_copy = true;
        } else {
            f.tex_hash[i] = render_texture_hash(st.tex_id[i]);
        }
    }
    return f;
}

// The game's draw filter's answer for each state the batch's draws use (-1 for a state no
// draw outside `skip` uses), and what each orthographic state's draws cover together, in its
// clip space (x = p0*x + p1, y = p2*y + p3 for GX's orthographic projection), four floats a
// state, for EyeDrawFacts' box. A draw's facts are the state's, so each state is asked once
// and its answer stands for its draws.
static void filter_answers(const Batch& b, const std::vector<uint8_t>* skip, std::vector<int8_t>& answer,
                           std::vector<float>& box) {
    note_fullscreen_copies(b);
    box.assign(b.states.size() * 4, 0.0f);
    std::vector<float> area(b.states.size(), 0.0f);
    std::vector<uint8_t> boxed(b.states.size(), 0);
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const Cmd& c = b.cmds[i];
        if (c.type != CmdType::Draw || (skip && (*skip)[i])) continue;
        const PixelState& st = b.states[c.state];
        if ((int)st.proj[6] == 0) continue;
        float* r = &box[c.state * 4];
        for (uint32_t v = 0; v < c.count; v++) {
            const float* pos = b.verts[b.indices[c.first + v]].pos;
            const float x = st.proj[0] * pos[0] + st.proj[1], y = st.proj[2] * pos[1] + st.proj[3];
            if (!boxed[c.state]) { r[0] = r[1] = x; r[2] = r[3] = y; boxed[c.state] = 1; }
            r[0] = std::min(r[0], x); r[1] = std::max(r[1], x);
            r[2] = std::min(r[2], y); r[3] = std::max(r[3], y);
        }
        if (c.prim != 0) continue;  // lines and points cover nothing
        auto clip = [&](uint32_t v, float& x, float& y) {
            const float* pos = b.verts[b.indices[c.first + v]].pos;
            x = st.proj[0] * pos[0] + st.proj[1];
            y = st.proj[2] * pos[1] + st.proj[3];
        };
        for (uint32_t t = 0; t + 2 < c.count; t += 3) {
            float x0, y0, x1, y1, x2, y2;
            clip(t, x0, y0); clip(t + 1, x1, y1); clip(t + 2, x2, y2);
            area[c.state] += 0.5f * fabsf((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0));
        }
    }
    answer.assign(b.states.size(), -1);
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const Cmd& c = b.cmds[i];
        if (c.type != CmdType::Draw || (skip && (*skip)[i])) continue;
        int8_t& a = answer[c.state];
        if (a >= 0) continue;
        EyeDrawFacts f = draw_facts(b.states[c.state]);
        const float* r = &box[c.state * 4];
        f.clip_x0 = r[0]; f.clip_x1 = r[1]; f.clip_y0 = r[2]; f.clip_y1 = r[3];
        f.clip_area = area[c.state];
        a = (int8_t)g_eye_filter(b.states[c.state], f);
    }
}

// An EyeFrameScreen state's texture coordinates as functions of the game's clip x and y,
// where the shader takes them out: only a vertex on the picture's edge moves, so only the
// triangles with one matter. Each gives its gradients exactly (three points make a plane),
// and they are kept where all of those triangles agree. A whole-draw fit failed on a
// filter that draws a full-screen quad and a window's rect in one draw with different
// mappings: the rect never moves, and its mapping is nothing to the quad's.
static void fit_uv_gradients(const Batch& b, uint32_t state, float out[32]) {
    memset(out, 0, 32 * sizeof(float));
    const PixelState& st = b.states[state];
    float grad[32];
    bool have = false, agree[8][2];
    for (int i = 0; i < 8; i++) agree[i][0] = agree[i][1] = true;
    for (const Cmd& c : b.cmds) {
        if (c.type != CmdType::Draw || c.state != state || c.prim != 0) continue;
        for (uint32_t t = 0; t + 2 < c.count; t += 3) {
            const GpuVertex* v[3];
            float x[3], y[3];
            bool edge = false;
            for (int j = 0; j < 3; j++) {
                v[j] = &b.verts[b.indices[c.first + t + j]];
                x[j] = st.proj[0] * v[j]->pos[0] + st.proj[1];
                y[j] = st.proj[2] * v[j]->pos[1] + st.proj[3];
                edge |= fabsf(x[j]) >= 0.98f || fabsf(y[j]) >= 0.98f;
            }
            if (!edge) continue;
            const float dx1 = x[1] - x[0], dy1 = y[1] - y[0], dx2 = x[2] - x[0], dy2 = y[2] - y[0];
            const float det = dx1 * dy2 - dx2 * dy1;
            if (fabsf(det) < 1e-8f) continue;
            float g[32];
            for (int i = 0; i < 8; i++)
                for (int k = 0; k < 2; k++) {
                    const float d1 = v[1]->tex[i][k] - v[0]->tex[i][k], d2 = v[2]->tex[i][k] - v[0]->tex[i][k];
                    g[i * 4 + k * 2] = (d1 * dy2 - d2 * dy1) / det;
                    g[i * 4 + k * 2 + 1] = (dx1 * d2 - dx2 * d1) / det;
                }
            if (!have) {
                memcpy(grad, g, sizeof(grad));
                have = true;
                continue;
            }
            for (int i = 0; i < 8; i++)
                for (int k = 0; k < 2; k++)
                    for (int e = 0; e < 2; e++) {
                        const float a0 = grad[i * 4 + k * 2 + e], a1 = g[i * 4 + k * 2 + e];
                        if (fabsf(a0 - a1) > 1e-3f * (1.0f + fabsf(a0))) agree[i][k] = false;
                    }
        }
    }
    if (!have) return;
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 2; k++)
            if (agree[i][k]) {
                out[i * 4 + k * 2] = grad[i * 4 + k * 2];
                out[i * 4 + k * 2 + 1] = grad[i * 4 + k * 2 + 1];
            }
    // GCN_EYELOG: what was fitted, per coordinate the state uses.
    static const bool log = getenv_nonempty("GCN_EYELOG") != nullptr;
    if (log)
        for (int i = 0; i < st.num_texgens && i < 8; i++)
            fprintf(stderr, "[eye] frame-screen state %u coord %d: s %s %.4f %.4f, t %s %.4f %.4f\n", state, i,
                    agree[i][0] ? "kept" : "dropped", grad[i * 4], grad[i * 4 + 1], agree[i][1] ? "kept" : "dropped",
                    grad[i * 4 + 2], grad[i * 4 + 3]);
}

// Where the eye stands and what it leaves out, for the batch about to be drawn: the chase
// camera with its pitch taken out (as the game's pitch hook says, if it has one), unless
// first person is on and the game's eye hook has somewhere better to put it
// (render_set_eye_hook). `skip` marks the off-screen passes, as
// mark_offscreen_passes leaves it.
void eye_prepare(const Batch& b, const std::vector<uint8_t>& skip) {
    g_hide.clear();
    g_late.clear();
    g_flat3d.clear();
    g_screen.clear();
    g_frame_grad.clear();
    g_restore.clear();
    g_restore_tex.clear();
    g_facing.clear();
    g_has_facing_box = false;
    g_true_scale.clear();
    g_hud_push.clear();
    if (g_hud_push_below > 0.0f && g_hud_to > 0.0f) {
        // Each HUD-layer state's nearest vertex over the frame, in view space.
        std::vector<float> nearest(b.states.size(), 1e30f);
        bool any = false;
        for (size_t i = 0; i < b.cmds.size(); i++) {
            const Cmd& c = b.cmds[i];
            if (c.type != CmdType::Draw || skip[i]) continue;
            const PixelState& st = b.states[c.state];
            if ((int)st.proj[6] != 0 || st.viewport[5] / 16777215.0f > g_hud_to) continue;
            float& n = nearest[c.state];
            for (uint32_t v = 0; v < c.count; v++) {
                const float d = -b.verts[b.indices[c.first + v]].pos[2];
                if (d > 0.0f && d < n) n = d;
            }
            any = true;
        }
        if (any) {
            g_hud_push.assign(b.states.size(), 1.0f);
            for (size_t s = 0; s < b.states.size(); s++)
                if (nearest[s] < g_hud_push_below) g_hud_push[s] = g_hud_push_to / nearest[s];
        }
    }
    g_eye_overridden = false;
    hud_upright();
    EyeOverride o;
    if (g_fp_on && g_eye_hook && g_eye_hook(b, skip, g_render_frame, o)) {
        memcpy(g_world_xform, o.view_to_eye, sizeof(g_world_xform));
        memcpy(g_hud_xform, o.hud_to_eye, sizeof(g_hud_xform));
        g_hide.swap(o.hide);
        g_eye_overridden = true;
    } else {
        if (g_pitch_hook) g_pitch_hook(b, &g_world_pitch);
        world_pitch_matrix(g_world_xform);
    }
    // The game's draw filter, on top of whatever the eye hook hid.
    if (!g_eye_filter) return;
    std::vector<int8_t> answer;
    std::vector<float> box;
    filter_answers(b, &skip, answer, box);
    std::vector<uint8_t> hidden_mtx;   // EyeHideObject: the position matrices to clear of draws
    std::vector<uint8_t> late_mtx;     // EyeLateObject: those whose draws go last
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const Cmd& c = b.cmds[i];
        if (c.type != CmdType::Draw || skip[i]) continue;
        const int8_t a = answer[c.state];
        if (a == EyeKeep || a < 0) continue;
        if (a == EyeFlat) {
            if (g_flat3d.empty()) g_flat3d.assign(b.states.size(), 0);
            g_flat3d[c.state] = 1;
            continue;
        }
        if (a == EyeScreen || a == EyeFrameScreen) {
            if (g_screen.empty()) g_screen.assign(b.states.size(), 0);
            if (a == EyeFrameScreen && g_screen[c.state] != 2) {
                if (g_frame_grad.empty()) g_frame_grad.assign(b.states.size() * 32, 0.0f);
                fit_uv_gradients(b, c.state, &g_frame_grad[c.state * 32]);
            }
            g_screen[c.state] = a == EyeScreen ? 1 : 2;
            continue;
        }
        if (a == EyeFacing) {
            if (g_facing.empty()) g_facing.assign(b.states.size(), 0);
            g_facing[c.state] = 1;
            const float* r = &box[c.state * 4];
            if (!g_has_facing_box) {
                memcpy(g_facing_box, r, sizeof(g_facing_box));
                g_has_facing_box = true;
            } else {
                g_facing_box[0] = std::min(g_facing_box[0], r[0]);
                g_facing_box[1] = std::max(g_facing_box[1], r[1]);
                g_facing_box[2] = std::min(g_facing_box[2], r[2]);
                g_facing_box[3] = std::max(g_facing_box[3], r[3]);
            }
            continue;
        }
        if (a == EyeTrueScale) {
            if (g_true_scale.empty()) g_true_scale.assign(b.states.size(), 0);
            g_true_scale[c.state] = 1;
            continue;
        }
        if (a == EyeRestore) {
            if (g_restore.empty()) {
                g_restore.assign(b.states.size(), 0);
                g_restore_box.assign(b.states.size() * 4, 0.0f);
            }
            if (!g_restore[c.state]) {
                g_restore[c.state] = 1;
                memcpy(&g_restore_box[c.state * 4], &box[c.state * 4], 4 * sizeof(float));
                const PixelState& st = b.states[c.state];
                for (int m = 0; m < 8; m++)
                    if (st.tex_is_efb[m] && st.tex_id[m]) g_restore_tex.push_back(st.tex_id[m]);
            }
            continue;
        }
        if (a == EyeLateObject) {
            if (late_mtx.size() <= c.mtx) late_mtx.resize(c.mtx + 1, 0);
            late_mtx[c.mtx] = 1;
            continue;
        }
        if (g_hide.empty()) g_hide.assign(b.cmds.size(), 0);
        g_hide[i] = 1;
        if (a == EyeHideObject) {
            if (hidden_mtx.size() <= c.mtx) hidden_mtx.resize(c.mtx + 1, 0);
            hidden_mtx[c.mtx] = 1;
        }
    }
    if (hidden_mtx.empty() && late_mtx.empty()) return;
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const Cmd& c = b.cmds[i];
        if (c.type != CmdType::Draw || skip[i]) continue;
        const bool hidden = c.mtx < hidden_mtx.size() && hidden_mtx[c.mtx];
        const bool late = !hidden && c.mtx < late_mtx.size() && late_mtx[c.mtx];
        if (!hidden && !late) continue;
        if (g_hide.empty()) g_hide.assign(b.cmds.size(), 0);
        g_hide[i] = 1;
        if (!late) continue;
        if (g_late.empty()) g_late.assign(b.cmds.size(), 0);
        g_late[i] = 1;
    }
}

// The flat view shows everything the eyes leave out, but what the game's filter says to
// leave out everywhere (EyeHideEverywhere).
void panel_hide(const Batch& b) {
    g_hide.clear();
    if (!g_eye_filter) return;
    std::vector<int8_t> answer;
    std::vector<float> box;
    filter_answers(b, nullptr, answer, box);
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const Cmd& c = b.cmds[i];
        if (c.type != CmdType::Draw || answer[c.state] != EyeHideEverywhere) continue;
        if (g_hide.empty()) g_hide.assign(b.cmds.size(), 0);
        g_hide[i] = 1;
    }
}

}  // namespace gx
