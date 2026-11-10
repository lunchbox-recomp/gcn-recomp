// What the renderer's files share: the helpers that hide the GL profiles' differences, the
// records of GL objects and state, and what each file defines for the others, grouped
// below under the file that defines it.
//
// One source serves desktop OpenGL 3.3 core and OpenGL ES 3.2; where the two profiles
// differ, the difference is confined to the small block of helpers below.
#pragma once
#include "../runtime.h"
#include "render.h"
#include "render_gl.h"
#include "aspect.h"
#include "shadergen.h"
#include "gl.h"
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

bool write_png(const char* path, const uint8_t* rgba, int w, int h);

namespace gx {

static const int EFB_W = 640, EFB_H = 528;

static inline double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// Logic-op blending does not exist in GL ES: there is no GL_COLOR_LOGIC_OP, no
// glLogicOp, and none of the GL_CLEAR..GL_SET enums. The game uses GX logic ops for
// only a few effects, so on ES they are skipped -- the draw writes through with
// blending off, which matches the common GX_LO_COPY case. Reproducing the rest would
// mean reading the framebuffer in the generated TEV shader via
// GL_EXT_shader_framebuffer_fetch.
#ifdef GCN_GL_ES
static inline void set_logic_op_off() {}
static inline void set_logic_op(uint32_t) {}
// ES spells this with the float suffix; desktop GL 3.3 core only has the double form.
static inline void clear_depth(float d) { glClearDepthf(d); }
// ES has no sampler LOD bias. GX uses it to nudge mip selection, so skipping it can
// pick a slightly different mip level than hardware would.
static inline void set_lod_bias(GLuint, float) {}
#else
static const GLenum kLogicOp[16] = {GL_CLEAR, GL_AND, GL_AND_REVERSE, GL_COPY, GL_AND_INVERTED, GL_NOOP, GL_XOR, GL_OR,
                                    GL_NOR, GL_EQUIV, GL_INVERT, GL_OR_REVERSE, GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND, GL_SET};
static inline void set_logic_op_off() { glDisable(GL_COLOR_LOGIC_OP); }
static inline void set_logic_op(uint32_t mode) {
    glEnable(GL_COLOR_LOGIC_OP);
    glLogicOp(kLogicOp[mode & 15]);
}
static inline void clear_depth(float d) { glClearDepth(d); }
static inline void set_lod_bias(GLuint s, float bias) {
    glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS, bias);
}
#endif

#if defined(GCN_GL_ES) || defined(__APPLE__)
#define GCN_HAVE_PROGRAM_BINARY 1
#else
// The desktop glad loader stops at 3.3; program binaries are 4.1.
#define GCN_HAVE_PROGRAM_BINARY 0
#endif

// The inputs a program's uniforms were last computed from; see AppliedState. `gen` and
// `view_gen` say whether the record is current at all (g_uniform_gen, g_view_gen).
struct UniformShadow {
    uint32_t gen = 0, view_gen = 0;
    float proj[7];
    uint8_t view_space, background, foreground, fg_layer, hud_layer, flat3d, screen, frame_screen, restore, facing;
    float hud_push;
    float viewport[6];
    AnchorMap anchor;          // what the viewport's x terms were remapped by
    uint32_t scissor_off;      // bp[0x59]
    uint32_t psize_reg;        // bp[0x22]
    uint32_t point_tex;        // point_tex_key(): what u_points and u_point_off were sent for
    uint32_t tev_reg[4][2], tev_konst[4][2];
    uint32_t ksel[8];          // bp[0xF6..0xFD]
    uint32_t aref;             // bp[0xF3]
    uint32_t ind[9];           // bp[0x06..0x0E]
    uint32_t indcs[2];         // bp[0x25..0x26]
    uint32_t fog[5];           // bp[0xEE..0xF2]
    uint32_t screen_uv;
    float ripple[2];
    float tsz[16];
    uint32_t texsnap;
};

struct Program {
    GLuint prog;
    GLint u_proj, u_vp_a, u_vp_b, u_point_size, u_tex, u_reg, u_kstage, u_texsize, u_indmtx, u_indscale,
        u_alpharef, u_fog, u_fogmode, u_fogcolor, u_indcoordscale, u_texsnap, u_zbias, u_zplane, u_dstalpha;
    GLint u_vr, u_view, u_crop, u_zproj, u_screen_uv, u_screen_px, u_screen_ripple, u_uvgrad;
    GLint u_points, u_point_off;
    mutable UniformShadow shadow;
};

// What apply_state last put into GL; see g_applied.
struct AppliedState {
    bool valid = false;
    ShaderKey key;
    const Program* pr = nullptr;
    uint32_t bp[256];
    uint32_t tex_id[8];
    uint8_t tex_sub[8];       // the unit holds an eye grab, not the game's texture
    GLuint sampler[8];
    uint32_t pal_id[8];       // unit 8+m: the palette map m's EFB copy is read through
    bool hud_depth;
    AnchorMap anchor;         // what the scissor's x edges were remapped by
};

// `grab` marks one of the spray's screen-space grabs; see is_grab_copy().
// `depth`: an EFB copy of the depth buffer, whose texels are a depth packed into bytes. A
// filter would average neighbouring depths a byte at a time, which at an edge is nonsense
// -- a fog volume showed it as bright specks along a waterline -- so it is always sampled
// nearest. (At the console's own resolution a full-screen quad lands on texel centres and
// a filter changes nothing; at a higher internal scale it does not.)
struct GlTex { GLuint tex; uint32_t w, h, levels; bool efb; bool grab; bool depth; uint64_t hash; };

// One of the eye's grabs of what it has drawn; see g_eye_grab.
struct EyeGrab { GLuint tex; int w, h; bool srgb; };

// What a draw is to the morph: where it ends up in stereo decides what it is folded from.
enum class MorphKind { World, CameraHeld, Hud, Screen };

// How much an EyeScreen draw is enlarged past the eye's field, so that its edges fall
// outside what the eye can see.
static constexpr float kEyeScreenOverscan = 1.1f;

// How many sets of vertex buffers execute_batch takes in turn; see g_vaos.
static constexpr int kVertexRing = 3;

// What stalled a frame; see g_stall.
struct StallCounts {
    int compiled = 0, new_states = 0, waited = 0;
    double compile_ms = 0.0, new_state_ms = 0.0;
};

// render_gl.cpp: setup, and the GL objects every pass uses.
extern int g_scale;
extern GLuint g_efb_fbo, g_efb_color, g_efb_depth;
extern GLuint g_copy_prog, g_copy_vao;
extern GLint g_copy_u_src, g_copy_u_rect, g_copy_u_mode, g_copy_u_depth, g_copy_u_alpha;
extern GLuint g_blit_prog;
extern GLint g_blit_u_src, g_blit_u_rect;
extern GLuint g_copy_fbo;
extern GLuint g_bg_prog;
extern GLint g_bg_u_mvp, g_bg_u_crop, g_bg_u_quad, g_bg_u_col;
extern GLuint g_vs;

// render_gl_programs.cpp: the pixel shader programs and their cache.
extern std::unordered_map<ShaderKey, Program, ShaderKeyHash> g_programs;
extern bool g_binaries_supported;
uint64_t hash_str(const char* s, uint64_t h = 1469598103934665603ull);
extern uint64_t g_vs_hash;
void shader_cache_load();
extern bool g_async_wanted;
extern bool g_parallel_compile;
const Program* get_program(const ShaderKey& k);

// render_gl_textures.cpp: the game's textures in GL.
extern std::unordered_map<uint32_t, GlTex> g_textures;
GLuint get_sampler(uint32_t mode0, uint32_t mode1, uint32_t levels);
extern std::vector<uint32_t> g_dead_pending;
void evict_textures(const Batch& b);
void upload_textures(const Batch& b);

// render_gl_state.cpp: apply_state and what it keeps of GL's state.
float fog_float(uint32_t v);
void scissor_offset(const uint32_t* bp, int& xoff, int& yoff);
extern AppliedState g_applied;
extern int g_gl_cull;
void gl_state_invalidate();
void uniforms_invalidate();
void view_invalidate();
void apply_stats_flush(const char* pass, uint32_t frame);
extern float g_zplane[4];
AnchorMap draw_anchor_map(const PixelState& st);
void viewport_a(const PixelState& st, const AnchorMap& an, float vpa[4]);
void apply_state(const PixelState& st, int prim);

// render_gl_copy.cpp: EFB copies.
bool is_grab_copy(const EfbCopyCmd& c);
extern uint32_t g_present_count;
void do_efb_copy(const EfbCopyCmd& c);

// render_gl_present.cpp: the present path.
extern bool g_pair_second;
extern const char* g_dump_suffix;
extern GLuint g_output_fbo;
void xfbs_free();
void present(const EfbCopyCmd& c);

// render_gl_view.cpp: where an eye looks from.
extern bool g_vr_active;
extern bool g_eye_frame;
extern float g_vr_proj[16], g_vr_view[16];
extern float g_vr_view_world[16];
extern float g_vr_view_sky[16];
extern float g_bg_from;
extern float g_fg_to;
extern float g_fg_scale;
extern float g_vr_view_fg[16], g_vr_view_world_fg[16];
extern float g_hud_to;
extern float g_hud_scale;
extern float g_hud_push_below, g_hud_push_to;
extern std::vector<float> g_hud_push;
extern float g_cur_hud_push;
extern float g_vr_view_hud[16], g_vr_view_world_hud[16];
extern bool g_view_space_world;
extern float g_world_pitch;
extern float g_hud_xform[16];
extern float g_vr_hud_eff[16];
extern float g_world_xform[16];
extern EyeHook g_eye_hook;
extern WorldPitchHook g_pitch_hook;
extern const char* g_eye_layer_name;
extern bool g_fp_on;
extern bool g_eye_overridden;
extern float g_morph;
extern float g_flat_eye, g_flat_halfw;
extern float g_panel_band;
extern float g_panel[16];
extern bool g_clip_ok;
void eye_screen_matrix(const float P[16], float M[16]);
void mat4_mul(const float* a, const float* b, float* c);
void facing_frame(const float P[16], float out[16]);
void world_pitch_matrix(float R[16]);
void compose_world_view();
void hud_upright();
void apply_view_turn(const Batch& b);
void morph_chain(const float P[16], MorphKind kind, float chain[16], float fog[16],
                 float crop[16]);
void draw_morph_background(const float P[16], const float rgb[3]);

// render_gl_filter.cpp: what the game's eye hooks and filter say about a batch.
extern std::vector<uint8_t> g_hide;
extern std::vector<uint8_t> g_late;
extern std::vector<uint8_t> g_flat3d;
extern bool g_cur_flat3d;
extern std::vector<uint8_t> g_screen;
extern uint8_t g_cur_screen;
extern std::vector<float> g_frame_grad;
extern const float* g_cur_uvgrad;
extern std::vector<uint8_t> g_restore;
extern std::vector<float> g_restore_box;
extern std::vector<uint32_t> g_restore_tex;
extern bool g_cur_restore;
extern const float* g_cur_restore_box;
extern std::vector<uint8_t> g_facing;
extern bool g_cur_facing;
extern std::vector<uint8_t> g_true_scale;
extern bool g_cur_true_scale;
extern float g_facing_box[4];
extern bool g_has_facing_box;
extern const float* g_cur_late_view;
void frame_observe(const Batch& b, const std::vector<uint8_t>* skip);
void eye_prepare(const Batch& b, const std::vector<uint8_t>& skip);
void panel_hide(const Batch& b);

// render_gl_passes.cpp: what a batch's copies say about its passes.
void display_size(const Batch& b, uint32_t& w, uint32_t& h);
size_t first_part_end(const Batch& b, const std::vector<uint8_t>& skip);
bool is_fullscreen_copy(const EfbCopyCmd& c, uint32_t dw, uint32_t dh);
void mark_offscreen_passes(const Batch& b, std::vector<uint8_t>& skip);
void note_fullscreen_copies(const Batch& b);
bool is_fullscreen_tex(uint32_t id);
bool samples_fullscreen_copy(const PixelState& st);
bool samples_grab_copy(const PixelState& st);
void flat_pass_trim(const Batch& b, const std::vector<uint8_t>& skip, std::vector<uint8_t>& out);

// render_gl_eye.cpp: one eye's view of a batch.
extern EyeGrab g_eye_grab, g_spray_grab, g_restore_grab;
extern int g_eye_w, g_eye_h;
bool is_grab_tex(uint32_t id);
bool is_eye_screen_tex(uint32_t id);
const EyeGrab& eye_grab_for(uint32_t id);

// render_gl_batch.cpp: a batch run into the EFB.
bool frametime_on();
extern GLuint g_vaos[kVertexRing][9], g_vbos[kVertexRing], g_ebos[kVertexRing];
extern GLuint g_vao, g_vbo;
extern bool g_base_vertex;
extern StallCounts g_stall;
extern std::unordered_set<uint64_t> g_seen_states;
extern bool g_state_new;
extern bool g_draw_skip;
extern uint32_t g_render_frame;
void draw_cmd(const Batch& b, const Cmd& c);
bool execute_batch(Batch& b, bool do_present, const std::vector<uint8_t>* leave_out = nullptr,
                   bool again = false);

}  // namespace gx
