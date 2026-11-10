// A batch run into the EFB the way the hardware would (execute_batch): its textures and
// vertices uploaded, then its draws, copies and present in order, with the draw log and
// the switches that pick draws out.
#include "render_gl_internal.h"

namespace gx {

// The renderer's half of GCN_FRAMETIME (see xf.cpp): one line per batch with the time
// spent uploading textures, uploading the vertex buffer, and issuing the frame, plus how
// many times the full pixel state had to be re-applied. CPU time only: the GPU runs
// behind, so a long frame here is driver and submission cost, not fill.
bool frametime_on() {  // on first use: the app sets its environment after static init
    static const bool on = getenv_nonempty("GCN_FRAMETIME") != nullptr;
    return on;
}

// The vertex and index buffers a batch is drawn from, and the VAO holding both. There are
// several, used in turn, because refilling one the GPU is still drawing from makes the
// driver wait for it. At the headset's own eye size that wait is rare, but the eyes' GPU
// time is what decides it: at 1.4x with 4x MSAA, one set left the flat pass at 27 ms a
// race frame on a Quest 3 (p50), nearly all of it in glBufferData waiting for the last
// frame's eyes; three sets took it to 4.9 ms, the same as at the old size. The GPU runs
// at most a frame or two behind, so three is enough. g_vao and g_vbo are the set in use;
// execute_batch moves them on.
//
// Each set's vertex buffer holds the batch's packed vertices (Batch::packed), a region per
// texture coordinate count, so each set has a vertex array per count over the same two
// buffers: g_vaos[set][n] reads n coordinates at packed_stride(n). g_vao is the one bound.
GLuint g_vaos[kVertexRing][9], g_vbos[kVertexRing], g_ebos[kVertexRing];
static int g_vertex_set = 0;
GLuint g_vao, g_vbo;

// Binds the vertex layout for a draw with `ntex` texture coordinates.
static void bind_vertex_format(uint32_t ntex) {
    const GLuint vao = g_vaos[g_vertex_set][ntex];
    if (vao == g_vao) return;
    g_vao = vao;
    glBindVertexArray(vao);
}

// Without glDrawElementsBaseVertex (GL ES 3.0 without the extension), the base vertex is
// folded into each draw's indices when the batch is uploaded (upload_indices) and the draw
// is an ordinary one. A base vertex is often negative -- it is where the draw's vertices
// were packed less where they were first numbered -- so pointing the vertex arrays at it
// instead would not do.
bool g_base_vertex = true;

// What stalled a frame, for render_report_frame: programs compiled, and draws that were the
// first of their program and blend state (g_seen_states), timed.
StallCounts g_stall;
std::unordered_set<uint64_t> g_seen_states;
bool g_state_new;  // the state applied for the next draw has not been drawn with
// The state applied for the next draw has a program still being built in the background
// (render_set_async_shaders), so the draw is left out.
bool g_draw_skip;

// One draw command, from the packed region its texture coordinate count selects.
void draw_cmd(const Batch& b, const Cmd& c) {
    static const GLenum mode[3] = {GL_TRIANGLES, GL_LINES, GL_POINTS};
    if (g_draw_skip) {
        g_stall.waited++;
        return;
    }
    bind_vertex_format(draw_ntex(b.states[c.state]));
    const void* first = (const void*)(uintptr_t)(c.first * sizeof(uint32_t));
    const bool timed = g_state_new;
    const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (g_base_vertex) glDrawElementsBaseVertex(mode[c.prim], c.count, GL_UNSIGNED_INT, first, c.base_vertex);
    else glDrawElements(mode[c.prim], c.count, GL_UNSIGNED_INT, first);
    if (timed) {
        g_state_new = false;
        g_stall.new_states++;
        g_stall.new_state_ms += ms_since(t0);
    }
}

void render_report_frame(const char* what, double ms) {
    if (ms > 50.0)
        fprintf(stderr, "[stall] %s frame: %.0f ms; %d programs compiled (%.0f ms), %d new program/blend states drawn (%.0f ms), "
                "%d draws left out awaiting a program\n",
                what, ms, g_stall.compiled, g_stall.compile_ms, g_stall.new_states, g_stall.new_state_ms, g_stall.waited);
    g_stall = {};
}

// The batch's indices into the bound element buffer, each draw's base vertex added in
// where the GL cannot add it (see g_base_vertex).
static void upload_indices(const Batch& b) {
    if (g_base_vertex) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, b.indices.size() * sizeof(uint32_t), b.indices.data(), GL_STREAM_DRAW);
        return;
    }
    static std::vector<uint32_t> rebased;
    rebased = b.indices;
    for (const Cmd& c : b.cmds) {
        if (c.type != CmdType::Draw) continue;
        for (uint32_t i = c.first; i < c.first + c.count; i++) rebased[i] += (uint32_t)c.base_vertex;
    }
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, rebased.size() * sizeof(uint32_t), rebased.data(), GL_STREAM_DRAW);
}

// Batches drawn so far, the frame number the logs and the eye hook are given.
uint32_t g_render_frame;

// The plane of a draw's last triangle, as the flat path's vertex shader places it: the
// GX projection, then the viewport (less the scissor offset, and anchored like the draw),
// Y flipped for the EFB blit.
static bool zfreeze_plane(const Batch& b, const Cmd& c, float out[4]) {
    if (c.prim != 0 || c.count < 3) return false;
    const PixelState& st = b.states[c.state];
    const float* p = st.proj;
    const float* vp = st.viewport;
    const bool perspective = (int)p[6] == 0;
    float vpa[4];
    viewport_a(st, draw_anchor_map(st), vpa);
    const float vpb[2] = {2.0f * vp[5] / 16777215.0f - 1.0f, 2.0f * vp[2] / 16777215.0f};
    double X[3], Y[3], Z[3];
    for (int i = 0; i < 3; i++) {
        const float* v = b.verts[b.indices[c.first + c.count - 3 + i]].pos;
        float cx, cy, cz, cw;
        if (perspective) { cx = p[0] * v[0] + p[1] * v[2]; cy = p[2] * v[1] + p[3] * v[2]; cw = -v[2]; }
        else { cx = p[0] * v[0] + p[1]; cy = p[2] * v[1] + p[3]; cw = 1.0f; }
        cz = p[4] * v[2] + p[5];
        if (cw <= 0.0f) return false;
        const float px = vpa[0] * cw + vpa[1] * cx, py = vpa[2] * cw + vpa[3] * cy, pz = vpb[0] * cw + vpb[1] * cz;
        X[i] = (px / cw + 1.0) * 0.5 * EFB_W * g_scale;
        Y[i] = (-py / cw + 1.0) * 0.5 * EFB_H * g_scale;
        Z[i] = (pz / cw + 1.0) * 0.5;
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

// The box a draw covers on the EFB, after its anchor's remap, for the draw log: x0, x1, y0
// and y1 in pixels from the top left, the vertices taken through the projection and the
// viewport as the vertex shader does. False when no vertex is in front of the camera.
static bool draw_screen_box(const Batch& b, const Cmd& c, float box[4]) {
    const PixelState& st = b.states[c.state];
    const float* p = st.proj;
    const bool perspective = (int)p[6] == 0;
    float vpa[4];
    viewport_a(st, draw_anchor_map(st), vpa);
    bool any = false;
    for (uint32_t i = 0; i < c.count; i++) {
        const float* v = b.verts[b.indices[c.first + i]].pos;
        float cx, cy, cw;
        if (perspective) { cx = p[0] * v[0] + p[1] * v[2]; cy = p[2] * v[1] + p[3] * v[2]; cw = -v[2]; }
        else { cx = p[0] * v[0] + p[1]; cy = p[2] * v[1] + p[3]; cw = 1.0f; }
        if (cw <= 0.0f) continue;
        const float x = ((vpa[0] * cw + vpa[1] * cx) / cw + 1.0f) * 0.5f * EFB_W;
        const float y = ((vpa[2] * cw + vpa[3] * cy) / cw + 1.0f) * 0.5f * EFB_H;
        if (!any) {
            box[0] = box[1] = x;
            box[2] = box[3] = y;
            any = true;
        } else {
            box[0] = std::min(box[0], x); box[1] = std::max(box[1], x);
            box[2] = std::min(box[2], y); box[3] = std::max(box[3], y);
        }
    }
    return any;
}

// Runs a batch into the EFB the way the hardware would. With do_present false the final
// scanout is skipped but everything else -- including every render-to-texture copy -- still
// happens, which is how the stereo path obtains the textures its eye passes sample.
// `again` draws a batch this function has just drawn: the textures and vertices are
// already up, and it is the same frame to the texture cache.
bool execute_batch(Batch& b, bool do_present, const std::vector<uint8_t>* leave_out, bool again) {
    const auto t_start = std::chrono::steady_clock::now();
    double ms_tex = 0.0, ms_vbo = 0.0, ms_copy = 0.0;
    if (!again) {
        g_render_frame++;
        const size_t n_dead = g_dead_pending.size();
        evict_textures(b);
        const double ms_evict = frametime_on() ? ms_since(t_start) : 0.0;
        upload_textures(b);
        ms_tex = frametime_on() ? ms_since(t_start) : 0.0;
        if (frametime_on() && ms_tex > 5.0) {
            size_t bytes = 0;
            for (auto& t : b.new_textures)
                for (auto& l : t->levels) bytes += l.size() * 4;
            fprintf(stderr, "[rt-tex] f%u deleted %zu in %.2f ms, uploaded %zu (%zu KB) in %.2f ms\n", g_render_frame,
                    n_dead, ms_evict, b.new_textures.size(), bytes / 1024, ms_tex - ms_evict);
        }
        // A set the GPU has finished with; see g_vaos. The eye passes after this draw from
        // the same one, so a batch is still uploaded once however many views it is drawn
        // into.
        g_vertex_set = (g_vertex_set + 1) % kVertexRing;
        g_vao = g_vaos[g_vertex_set][0];
        g_vbo = g_vbos[g_vertex_set];
        glBindVertexArray(g_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glBufferData(GL_ARRAY_BUFFER, b.packed.size(), b.packed.data(), GL_STREAM_DRAW);
        upload_indices(b);
        ms_vbo = frametime_on() ? ms_since(t_start) - ms_tex : 0.0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
    glBindVertexArray(g_vao);
    uint32_t n_apply = 0;
    note_fullscreen_copies(b);
    static const bool batchlog = getenv_nonempty("GCN_EYELOG") != nullptr;
    if (batchlog) {
        int nd = 0, nc = 0, np = 0;
        for (auto& c : b.cmds) {
            nd += c.type == CmdType::Draw;
            nc += c.type == CmdType::EfbCopy;
            np += c.type == CmdType::Present;
        }
        fprintf(stderr, "[batch] f%u cmds=%zu draws=%d copies=%d present=%d verts=%zu indices=%zu\n",
                g_render_frame, b.cmds.size(), nd, nc, np, b.verts.size(), b.indices.size());
    }
    bool presented = false;
    uint32_t cur_state = UINT32_MAX;
    gl_state_invalidate();
    view_invalidate();   // the flat view, after an eye
    memset(g_zplane, 0, sizeof(g_zplane));  // none yet this frame
    int cur_prim = -1;
    // GCN_NO_COMP drops draws that sample a copy of the whole frame, in the flat path too.
    // The duplicate racer on the water is visible without any of the VR code, so this is
    // how to tell whether that draw is responsible for it.
    static const bool no_comp = getenv_nonempty("GCN_NO_COMP") != nullptr;
    int draw_index = 0;
    // GCN_DRAWLOG_COPY=WxH logs, like GCN_DRAWLOG, every batch holding a copy of that size:
    // a render to texture done once, at a frame that differs from run to run, is logged
    // without knowing the frame.
    static uint32_t dlc_w = 0, dlc_h = 0;
    static const bool dlc_parsed = [] {
        if (const char* e = getenv("GCN_DRAWLOG_COPY")) sscanf(e, "%ux%u", &dlc_w, &dlc_h);
        return true;
    }();
    (void)dlc_parsed;
    bool log_batch = false;
    if (dlc_w)
        for (auto& c : b.cmds)
            if (c.type == CmdType::EfbCopy && !c.copy.to_xfb && c.copy.dst_w == dlc_w && c.copy.dst_h == dlc_h) log_batch = true;
    if (log_batch) fprintf(stderr, "[drawlog] batch with a %ux%u copy, presented frame %u\n", dlc_w, dlc_h, g_present_count + 1);
    static const uint32_t drawlog_frame = getenv("GCN_DRAWLOG") ? (uint32_t)atoi(getenv("GCN_DRAWLOG")) : 0;
    // Copies are listed between the draws in either form of the log.
    const bool log_copies = log_batch || (drawlog_frame && g_present_count + 1 == drawlog_frame);
    for (size_t ci = 0; ci < b.cmds.size(); ci++) {
        Cmd& c = b.cmds[ci];
        switch (c.type) {
        case CmdType::Draw: {
            if (!g_hide.empty() && g_hide[ci]) break;
            // A draw the eyes will make themselves, whose only product here would be a
            // picture nobody sees; see flat_pass_trim.
            if (leave_out && (*leave_out)[ci]) break;
            const bool comp = no_comp && samples_fullscreen_copy(b.states[c.state]);
            // GCN_DRAWLOG=<frame> lists every draw in one frame with its index, so a
            // specific piece of geometry can be found and then skipped by index.
            // GCN_DRAW_SKIP=a-b drops a range of draw indices, to attribute a piece of the
            // image to the draws that made it.
            static int skip_lo = -1, skip_hi = -1;
            static bool skip_parsed = false;
            if (!skip_parsed) {
                skip_parsed = true;
                if (const char* s = getenv_nonempty("GCN_DRAW_SKIP")) {
                    skip_lo = atoi(s);
                    const char* dash = strchr(s, '-');
                    skip_hi = dash ? atoi(dash + 1) : skip_lo;
                }
            }
            if (log_batch || (drawlog_frame && g_present_count + 1 == drawlog_frame)) {
                const PixelState& st = b.states[c.state];
                // The projection type and the view-space depth are what decide a draw's
                // fate in an eye: a perspective batch is re-projected as world geometry,
                // anything else goes on the HUD frame. And a draw a few tens of units
                // from the camera is in front of the viewer's face either way.
                float zlo = 1e30f, zhi = -1e30f, xlo = 1e30f, xhi = -1e30f, ylo = 1e30f, yhi = -1e30f;
                for (uint32_t v = 0; v < c.count; v++) {
                    const float* q = b.verts[b.indices[c.first + v]].pos;
                    zlo = std::min(zlo, q[2]); zhi = std::max(zhi, q[2]);
                    xlo = std::min(xlo, q[0]); xhi = std::max(xhi, q[0]);
                    ylo = std::min(ylo, q[1]); yhi = std::max(yhi, q[1]);
                }
                // Vertices within a unit of the view-space origin sit on top of the camera,
                // and a polygon that uses one is drawn as a sliver radiating from the
                // screen's centre; counted so such draws can be found.
                uint32_t at_origin = 0;
                for (uint32_t v = 0; v < c.count; v++) {
                    const float* p = b.verts[b.indices[c.first + v]].pos;
                    if (fabsf(p[0]) < 1.0f && fabsf(p[1]) < 1.0f && fabsf(p[2]) < 1.0f) at_origin++;
                }
                // Sliver triangles: longer than 40 units in view space with a height under
                // a thousandth of their length. Geometry that is right never looks like
                // that; a vertex that is wrong makes exactly that, drawn as a hairline
                // across the screen. The first few are printed with their vertices.
                uint32_t slivers = 0;
                std::string sliver_text;
                if (c.prim == 0) {
                    for (uint32_t t = 0; t + 2 < c.count; t += 3) {
                        const float* a = b.verts[b.indices[c.first + t]].pos;
                        const float* bb = b.verts[b.indices[c.first + t + 1]].pos;
                        const float* cc = b.verts[b.indices[c.first + t + 2]].pos;
                        auto len = [](const float* p, const float* q) {
                            return sqrtf((p[0]-q[0])*(p[0]-q[0]) + (p[1]-q[1])*(p[1]-q[1]) + (p[2]-q[2])*(p[2]-q[2]));
                        };
                        const float e0 = len(a, bb), e1 = len(bb, cc), e2 = len(cc, a);
                        const float longest = std::max(e0, std::max(e1, e2));
                        if (longest < 40.0f) continue;
                        // Heron's formula for the area; height = 2A / longest.
                        const float sp = (e0 + e1 + e2) * 0.5f;
                        const float area2 = std::max(0.0f, sp * (sp - e0) * (sp - e1) * (sp - e2));
                        const float height = 2.0f * sqrtf(area2) / longest;
                        static const float ratio = getenv_nonempty("GCN_SLIVER_RATIO") ? (float)atof(getenv_nonempty("GCN_SLIVER_RATIO")) : 0.001f;
                        if (height < longest * ratio) {
                            if (slivers < 3) {
                                char buf[200];
                                const GpuVertex& va = b.verts[b.indices[c.first + t]];
                                const GpuVertex& vb = b.verts[b.indices[c.first + t + 1]];
                                const GpuVertex& vc = b.verts[b.indices[c.first + t + 2]];
                                snprintf(buf, sizeof(buf), "\n        sliver src d%u:%d d%u:%d d%u:%d (%.3f,%.3f,%.3f) (%.3f,%.3f,%.3f) (%.3f,%.3f,%.3f) len=%.0f",
                                         va.src_draw, (int)va.src_idx, vb.src_draw, (int)vb.src_idx, vc.src_draw, (int)vc.src_idx,
                                         a[0], a[1], a[2], bb[0], bb[1], bb[2], cc[0], cc[1], cc[2], longest);
                                sliver_text += buf;
                            }
                            slivers++;
                        }
                    }
                }
                // The viewport's depth band, and the view-space box, to two decimals: a
                // weapon held in front of the camera is a unit or two across.
                const float band_hi = st.viewport[5] / 16777215.0f;
                const float band_lo = band_hi - fabsf(st.viewport[2]) / 16777215.0f;
                // The projection's scales and offsets (XF 0x1020..0x1023) go with the box: a
                // box is inside or outside a frustum only against the projection it was
                // drawn with, and a frame may use several.
                fprintf(stderr, "[draw] %d verts=%u st=%u texgens=%u proj=%c,%.4f,%.4f,%.4f,%.4f,%.6g,%.6g z=%.2f..%.2f x=%.2f..%.2f y=%.2f..%.2f band=%.5f..%.5f%s origin=%u slivers=%u%s",
                        draw_index, c.count, c.state, st.num_texgens,
                        (int)st.proj[6] == 0 ? 'p' : 'o', st.proj[0], st.proj[1], st.proj[2], st.proj[3], st.proj[4], st.proj[5],
                        zlo, zhi, xlo, xhi, ylo, yhi, band_lo, band_hi, st.clip_disable ? " noclip" : "",
                        at_origin, slivers, sliver_text.c_str());
                if (st.tag) fprintf(stderr, " tag=%u", st.tag);   // set_draw_tag
                // The game's anchor (S for none, else the letter and the rect) and the draw's
                // box on the EFB after the remap, in pixels from the top left.
                fprintf(stderr, " anchor=%c", anchor_letter((Anchor)st.anchor));
                if (st.anchor) fprintf(stderr, "(%g..%g)", st.anchor_x0, st.anchor_x1);
                float scr[4];
                if (draw_screen_box(b, c, scr)) fprintf(stderr, " scr=%.1f..%.1f,%.1f..%.1f", scr[0], scr[1], scr[2], scr[3]);
                else fprintf(stderr, " scr=none");
                if (slivers) fprintf(stderr, "\n       ");
                // Each texture by id and, for a decoded one, by its content hash, which is
                // the same in every run (TexData::hash); an EFB copy is starred.
                for (int i = 0; i < 8; i++) {
                    if (!st.tex_id[i]) continue;
                    if (st.tex_is_efb[i]) fprintf(stderr, " t%d=%u*", i, st.tex_id[i]);
                    else fprintf(stderr, " t%d=%u[%016llx]", i, st.tex_id[i], (unsigned long long)render_texture_hash(st.tex_id[i]));
                }
                fprintf(stderr, "\n");
                // GCN_DRAWLOG_VERBOSE=1 adds the TEV setup -- stage count, each stage's
                // order, colour and alpha combiners and konst selectors, the colour
                // registers and konsts -- and the first few vertices, so a draw's shading
                // can be read off without a shader dump.
                static const bool verbose = getenv_nonempty("GCN_DRAWLOG_VERBOSE") != nullptr;
                if (verbose) {
                    const uint32_t* bp = st.bp;
                    const uint32_t nstages = ((bp[0] >> 10) & 15) + 1;
                    fprintf(stderr, "        genmode=%06X stages=%u colors=%u", bp[0], nstages, st.num_colors);
                    for (uint32_t i = 0; i < nstages; i++)
                        fprintf(stderr, "\n        st%u order=%06X cenv=%06X aenv=%06X ksel=%06X", i,
                                bp[0x28 + i / 2], bp[0xC0 + 2 * i], bp[0xC1 + 2 * i], bp[0xF6 + i / 2]);
                    fprintf(stderr, "\n        alphafunc=%06X zmode=%06X blend=%06X dstalpha=%06X pecntrl=%06X", bp[0xF3], bp[0x40], bp[0x41], bp[0x42], bp[0x43]);
                    // A Z texture's bias and its type and op (BP 0xF4-0xF5), when it is on.
                    if ((bp[0xF5] >> 2) & 3) fprintf(stderr, " ztex=%06X,%06X", bp[0xF4], bp[0xF5]);
                    // The indirect stages: the matrices (BP 0x06-0x0E), the coordinate
                    // scales (0x25-0x26), the stage references (0x27) and each TEV
                    // stage's indirect command (0x10-0x1F).
                    if (((bp[0] >> 16) & 7) != 0) {
                        fprintf(stderr, "\n        indmtx=");
                        for (int i = 0; i < 9; i++) fprintf(stderr, "%06X%s", bp[0x06 + i], i == 8 ? "" : ",");
                        fprintf(stderr, " indcs=%06X,%06X iref=%06X indcmd=", bp[0x25], bp[0x26], bp[0x27]);
                        for (uint32_t i = 0; i < nstages; i++) fprintf(stderr, "%06X%s", bp[0x10 + i], i + 1 == nstages ? "" : ",");
                    }
                    fprintf(stderr, "\n        viewport=%.1f,%.1f,%.1f %.1f,%.1f,%.1f scissor=%06X %06X offset=%06X", st.viewport[0],
                            st.viewport[1], st.viewport[2], st.viewport[3], st.viewport[4], st.viewport[5], bp[0x20], bp[0x21], bp[0x59]);
                    // Each bound map's mode register (wrap, filters, LOD bias): BP 0x80-0x83
                    // and 0xA0-0xA3 for maps 0-3 and 4-7.
                    fprintf(stderr, "\n       ");
                    for (int i = 0; i < 8; i++)
                        if (st.tex_id[i]) fprintf(stderr, " mode%d=%06X", i, bp[i < 4 ? 0x80 + i : 0xA0 + (i - 4)]);
                    // Each map's image format (BP 0x88-0x8B, 0xA8-0xAB), and for an EFB copy
                    // the format it was copied in and the palette it is read through.
                    for (int i = 0; i < 8; i++) {
                        if (!st.tex_id[i]) continue;
                        fprintf(stderr, " fmt%d=%u", i, (bp[i < 4 ? 0x88 + i : 0xA8 + (i - 4)] >> 20) & 15);
                        if (st.tex_is_efb[i])
                            fprintf(stderr, "(copy %02X%s)", st.tex_efb_fmt[i],
                                    st.tex_pal[i] ? (st.tex_pal_bits[i] == 8 ? " pal8" : " pal4") : "");
                    }
                    // Fog (BP 0xEE-0xF2): the type as GXSetFog takes it (2 linear, 4 exp, 5
                    // exp2, 6-7 the reverse ones; 8 on top for an orthographic one), the
                    // registers, and for a perspective fog the start and end distances they
                    // come from -- GXSetFog makes A = n*f / ((f - n)(end - start)) and
                    // C = start / (end - start), and n*f / (f - n) is minus the projection's
                    // z offset.
                    const uint32_t fogsel = ((bp[0xF1] >> 21) & 7) | (((bp[0xF1] >> 20) & 1) << 3);
                    if ((fogsel & 7) >= 2) {
                        const float fa = fog_float(bp[0xEE]), fc = fog_float(bp[0xF1]);
                        fprintf(stderr, "\n        fog type=%u a=%g bmag=%u bshift=%u c=%g color=%06X", fogsel, fa,
                                bp[0xEF] & 0xFFFFFF, bp[0xF0] & 0x1F, fc, bp[0xF2] & 0xFFFFFF);
                        if (!(fogsel & 8) && fa != 0.0f) {
                            const float range = -st.proj[5] / fa;
                            fprintf(stderr, " (start %g end %g)", fc * range, fc * range + range);
                        }
                    }
                    for (int r = 0; r < 4; r++)
                        fprintf(stderr, "\n        reg%d=%08X %08X  konst%d=%08X %08X", r, st.tev_reg[r][0], st.tev_reg[r][1],
                                r, st.tev_konst[r][0], st.tev_konst[r][1]);
                    for (uint32_t v = 0; v < c.count && v < 24; v++) {
                        const uint32_t ix = b.indices[c.first + v];
                        const GpuVertex& gv = b.verts[ix];
                        fprintf(stderr, "\n        i%u=%u pos=%.1f,%.1f,%.1f col0=%02X%02X%02X%02X col1=%02X%02X%02X%02X uv0=%.3f,%.3f,%.3f", v, ix,
                                gv.pos[0], gv.pos[1], gv.pos[2], gv.col[0][0], gv.col[0][1], gv.col[0][2], gv.col[0][3],
                                gv.col[1][0], gv.col[1][1], gv.col[1][2], gv.col[1][3], gv.tex[0][0], gv.tex[0][1], gv.tex[0][2]);
                    }
                    fprintf(stderr, "\n");
                }
            }
            const int this_draw = draw_index++;
            if (skip_lo >= 0 && this_draw >= skip_lo && this_draw <= skip_hi) break;
            // GCN_NO_EFBTEX drops draws that sample a partial EFB copy -- here, the copy of
            // the water surface that the game tints submerged geometry with. Unlike a draw
            // index this is stable from frame to frame, which matters because the game's
            // timebase is wall-clock driven and frame N is not the same moment twice.
            static const bool no_efbtex = getenv_nonempty("GCN_NO_EFBTEX") != nullptr;
            if (no_efbtex) {
                const PixelState& st = b.states[c.state];
                bool partial = false;
                for (int i = 0; i < 8; i++)
                    if (st.tex_is_efb[i] && st.tex_id[i]) partial = true;
                if (partial && !samples_fullscreen_copy(st)) break;
            }
            if (comp) break;
            // A triangle drawn without z freeze is the plane the next z freeze draws take,
            // culled or not: a game draws one it culls to set the plane.
            const bool zfreeze = (b.states[c.state].bp[0x00] >> 19) & 1;
            if (!zfreeze) zfreeze_plane(b, c, g_zplane);
            if (c.state != cur_state || c.prim != cur_prim) {
                apply_state(b.states[c.state], c.prim);
                cur_state = c.state;
                cur_prim = c.prim;
                n_apply++;
            } else if (zfreeze && g_applied.pr && g_applied.pr->u_zplane >= 0) {
                glUniform4fv(g_applied.pr->u_zplane, 1, g_zplane);
            }
            draw_cmd(b, c);
            // GCN_PIXEL=x,y[;x,y...] (EFB pixels) with GCN_DRAWLOG=<frame>: after every draw
            // of that frame, each listed pixel is read back and printed when it changed,
            // with the draw's index in the draw log. Which draws touch a pixel, and what
            // each leaves there, read off one frame.
            {
                static std::vector<std::pair<int, int>> probes = [] {
                    std::vector<std::pair<int, int>> v;
                    if (const char* e = getenv("GCN_PIXEL")) {
                        for (const char* q = e; *q; ) {
                            int x = atoi(q);
                            const char* comma = strchr(q, ',');
                            if (!comma) break;
                            v.push_back({x, atoi(comma + 1)});
                            const char* semi = strchr(comma, ';');
                            if (!semi) break;
                            q = semi + 1;
                        }
                    }
                    return v;
                }();
                static std::vector<uint32_t> last(probes.size(), 0xFFFFFFFFu), last_z(probes.size(), 0xFFFFFFFFu);
                if (!probes.empty() && (log_batch || (drawlog_frame && g_present_count + 1 == drawlog_frame))) {
                    glFinish();
                    for (size_t pi = 0; pi < probes.size(); pi++) {
                        uint8_t px[4] = {0, 0, 0, 0};
                        const int gx = probes[pi].first * g_scale, gy = (EFB_H - 1 - probes[pi].second) * g_scale;
                        glReadPixels(gx, gy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                        float depth = -1.0f;
                        glReadPixels(gx, gy, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
                        const uint32_t v = (px[0] << 24) | (px[1] << 16) | (px[2] << 8) | px[3];
                        const uint32_t z = (uint32_t)(depth * 16777215.0f + 0.5f);
                        if (v != last[pi] || z != last_z[pi]) {
                            fprintf(stderr, "[pix] %d,%d after draw %d: %08X z=%06X\n", probes[pi].first, probes[pi].second, this_draw, v, z);
                            last[pi] = v;
                            last_z[pi] = z;
                        }
                    }
                }
            }
            break;
        }
        case CmdType::EfbCopy: {
            // Timed on its own: a copy mid-frame ends the EFB's render pass on a tiled GPU
            // and the driver's share of that lands here, on the CPU.
            const auto t_copy = frametime_on() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            if (log_copies)
                fprintf(stderr, "[copy] after draw %d: %ux%u from %u,%u to %08X tex %u fmt %X%s\n", draw_index - 1, c.copy.dst_w, c.copy.dst_h,
                        c.copy.src_x, c.copy.src_y, c.copy.dest, c.copy.tex_id, c.copy.format, c.copy.to_xfb ? " (display)" : "");
            do_efb_copy(c.copy);
            if (frametime_on()) ms_copy += ms_since(t_copy);
            glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
            cur_state = UINT32_MAX;
            gl_state_invalidate();
            break;
        }
        case CmdType::Present:
            if (do_present) present(c.copy);
            presented = true;
            glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
            glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
            cur_state = UINT32_MAX;
            gl_state_invalidate();
            break;
        }
    }
    if (frametime_on())
        fprintf(stderr, "[rt] f%u render %6.2fms (tex %5.2f vbo %5.2f copies %5.2f)  applies %4u  programs %zu\n",
                g_render_frame, ms_since(t_start), ms_tex, ms_vbo, ms_copy, n_apply, g_programs.size());
    apply_stats_flush("flat", g_render_frame);
    return presented;
}

bool render_execute(Batch& b) {
    frame_observe(b, nullptr);
    panel_hide(b);
    return execute_batch(b, true);
}

void render_set_panel_band(float band) { g_panel_band = band; }

bool render_execute_stereo_pair(Batch& b, unsigned fbo_l, unsigned fbo_r, float eye_sep,
                                float panel_width) {
    frame_observe(b, nullptr);
    panel_hide(b);
    const GLuint out = g_output_fbo;
    g_flat_halfw = panel_width > 0.0f ? 0.5f * panel_width : 1.0f;
    g_flat_eye = -0.5f * eye_sep;
    g_output_fbo = (GLuint)fbo_l;
    g_dump_suffix = "_l";
    const bool presented = execute_batch(b, true);
    g_flat_eye = 0.5f * eye_sep;
    g_output_fbo = (GLuint)fbo_r;
    g_dump_suffix = "_r";
    g_pair_second = true;
    execute_batch(b, true, nullptr, true);
    g_pair_second = false;
    g_dump_suffix = "";
    g_flat_eye = 0.0f;
    g_output_fbo = out;
    return presented;
}

}  // namespace gx
