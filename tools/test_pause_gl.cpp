// The paused redraw's checks, which tools/test_pause.cpp runs: render_gl.cpp's present()
// and render_present_last() over a fake GL, and render_set_internal_scale(), which the
// overlay's Display page calls while the game is paused.
//
// render_gl.cpp is compiled into this file as the GL ES build of it, whose entry points are
// glad's pointers, and the ones those functions reach are pointed at fakes. The fake keeps
// one number per texture for what it holds, a frame's id, and the size it was given, and
// the window has its own number. A blit copies the number, a clear makes it 0, and a draw
// with the blit program copies the bound texture's number to the target, with the rect and
// viewport recorded. The XFB and the EFB hold the same number once a frame is assembled, so
// a blit into the kept copy also records the framebuffer it read. The linker drops the rest
// of the renderer, which nothing here reaches.
#define GCN_GL_ES
#include "gx/render_gl.cpp"
#include <cstdio>
#include <cstdarg>
#include <map>
#include <set>
#include <utility>

bool write_png(const char*, const uint8_t*, int, int) { return true; }
void fatal(const char* fmt, ...) {  // render_set_internal_scale's, for an incomplete EFB
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
namespace gx {
float output_aspect() { return 4.0f / 3.0f; }
}

namespace {

std::map<GLuint, int> g_tex;       // what each texture holds
std::map<GLuint, std::pair<int, int>> g_size;  // and the size it was last given
std::map<GLuint, GLuint> g_attach;  // each framebuffer's texture
int g_window;                       // what the window holds
GLuint g_next = 1, g_read, g_draw, g_bound_tex, g_program;
GLuint g_kept_from;                 // the framebuffer the last blit into the kept copy read
std::set<GLuint> g_deleted;         // textures and framebuffers deleted (gen numbers both)
float g_rect[4];
GLint g_view[4];
int g_failed;

int& holds(GLuint fbo) { return fbo ? g_tex[g_attach[fbo]] : g_window; }

void GLAD_API_PTR gen(GLsizei n, GLuint* ids) {
    for (GLsizei i = 0; i < n; i++) ids[i] = g_next++;
}
void GLAD_API_PTR delete_ids(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; i++) g_deleted.insert(ids[i]);
}
void GLAD_API_PTR bind_framebuffer(GLenum target, GLuint fbo) {
    if (target != GL_DRAW_FRAMEBUFFER) g_read = fbo;
    if (target != GL_READ_FRAMEBUFFER) g_draw = fbo;
}
void GLAD_API_PTR bind_texture(GLenum, GLuint tex) { g_bound_tex = tex; }
void GLAD_API_PTR tex_image(GLenum, GLint, GLint, GLsizei w, GLsizei h, GLint, GLenum, GLenum, const void*) {
    g_tex[g_bound_tex] = 0;
    g_size[g_bound_tex] = {w, h};
}
void GLAD_API_PTR attach(GLenum, GLenum, GLenum, GLuint tex, GLint) { g_attach[g_draw] = tex; }
void GLAD_API_PTR blit(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) {
    holds(g_draw) = holds(g_read);
    if (g_draw && g_draw == gx::g_last_fbo) g_kept_from = g_read;
}
void GLAD_API_PTR clear(GLbitfield mask) {
    if (mask & GL_COLOR_BUFFER_BIT) holds(g_draw) = 0;
}
void GLAD_API_PTR use_program(GLuint p) { g_program = p; }
void GLAD_API_PTR uniform4fv(GLint, GLsizei, const GLfloat* v) { memcpy(g_rect, v, sizeof(g_rect)); }
void GLAD_API_PTR viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    g_view[0] = x;
    g_view[1] = y;
    g_view[2] = w;
    g_view[3] = h;
}
void GLAD_API_PTR draw_arrays(GLenum, GLint, GLsizei) {
    if (g_program == gx::g_blit_prog) holds(g_draw) = g_tex[g_bound_tex];
}
void GLAD_API_PTR nothing_e(GLenum) {}
void GLAD_API_PTR nothing_eei(GLenum, GLenum, GLint) {}
void GLAD_API_PTR nothing_uu(GLuint, GLuint) {}
void GLAD_API_PTR nothing_u(GLuint) {}
void GLAD_API_PTR nothing_ii(GLint, GLint) {}
void GLAD_API_PTR nothing_ffff(GLfloat, GLfloat, GLfloat, GLfloat) {}
void GLAD_API_PTR nothing_bbbb(GLboolean, GLboolean, GLboolean, GLboolean) {}
void GLAD_API_PTR nothing_b(GLboolean) {}
GLenum GLAD_API_PTR complete(GLenum) { return GL_FRAMEBUFFER_COMPLETE; }

void install() {
    glad_glGenTextures = gen;
    glad_glGenFramebuffers = gen;
    glad_glDeleteTextures = delete_ids;
    glad_glDeleteFramebuffers = delete_ids;
    glad_glBindFramebuffer = bind_framebuffer;
    glad_glBindTexture = bind_texture;
    glad_glTexImage2D = tex_image;
    glad_glFramebufferTexture2D = attach;
    glad_glBlitFramebuffer = blit;
    glad_glClear = clear;
    glad_glUseProgram = use_program;
    glad_glUniform4fv = uniform4fv;
    glad_glViewport = viewport;
    glad_glDrawArrays = draw_arrays;
    glad_glDisable = nothing_e;
    glad_glActiveTexture = nothing_e;
    glad_glTexParameteri = nothing_eei;
    glad_glBindSampler = nothing_uu;
    glad_glBindVertexArray = nothing_u;
    glad_glUniform1i = nothing_ii;
    glad_glClearColor = nothing_ffff;
    glad_glColorMask = nothing_bbbb;
    glad_glDepthMask = nothing_b;
    glad_glCheckFramebufferStatus = complete;
}

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

// A display copy of `rows` rows of a 640-pixel frame to `dest`, from a little way into the
// EFB, so that the rect drawn from tells x from y.
gx::EfbCopyCmd display_copy(uint32_t dest, uint32_t rows) {
    gx::EfbCopyCmd c{};
    c.src_x = 4;
    c.src_y = 12;
    c.src_w = c.dst_w = 640;
    c.src_h = c.dst_h = rows;
    c.to_xfb = true;
    c.dest = dest;
    return c;
}

// The EFB holding frame `id` presented through `c`, after which the copy clears the EFB and
// something is drawn over the window.
void present_frame(int id, const gx::EfbCopyCmd& c) {
    holds(gx::g_efb_fbo) = id;
    gx::present(c);
    holds(gx::g_efb_fbo) = 0;
}

}  // namespace

// The checks; returns how many failed.
int present_last_checks() {
    install();
    glGenTextures(1, &gx::g_efb_color);
    glGenFramebuffers(1, &gx::g_efb_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gx::g_efb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gx::g_efb_color, 0);
    gx::g_blit_prog = 1000;
    gx::render_set_window_size(1280, 960);
    const gx::EfbCopyCmd whole = display_copy(0x00400000, 480);
    const gx::EfbCopyCmd top = display_copy(0x00400000, 240), bottom = display_copy(0x00400000 + 240 * 1280, 240);

    present_frame(11, whole);
    g_window = -1;
    check(!gx::render_present_last() && g_window == -1, "with no frames kept, render_present_last draws nothing");

    gx::render_keep_last(true);
    present_frame(21, whole);
    const bool shown = g_window == 21;
    float rect[4];
    GLint view[4];
    memcpy(rect, g_rect, sizeof(rect));
    memcpy(view, g_view, sizeof(view));
    g_window = -1;
    check(shown && gx::render_present_last() && g_window == 21,
          "render_present_last draws the presented frame again, though the EFB it came from was cleared");
    check(!memcmp(rect, g_rect, sizeof(rect)) && !memcmp(view, g_view, sizeof(view)),
          "it draws it through the same rect and fit as when it was presented");

    present_frame(22, whole);
    g_window = -1;
    check(gx::render_present_last() && g_window == 22, "it draws the newest presented frame, not an older one");

    present_frame(31, top);
    present_frame(32, bottom);
    const bool assembled = g_window == 32;
    const GLuint kept_from = g_kept_from;
    present_frame(41, top);
    g_window = -1;
    check(assembled && gx::render_present_last() && g_window == 32,
          "a frame copied in halves is drawn as presented, though its XFB has since taken half the next one");
    check(gx::g_xfbs.size() == 1 && kept_from == gx::g_xfbs[0].fbo,
          "it was kept from the assembled XFB, not from the EFB that held only its last half");

    const GLuint kept_tex = gx::g_last_tex, kept_fbo = gx::g_last_fbo;
    gx::render_keep_last(false);
    g_window = -1;
    check(!gx::render_present_last() && g_window == -1, "once keeping stops, render_present_last draws nothing");
    check(kept_tex && kept_fbo && g_deleted.count(kept_tex) && g_deleted.count(kept_fbo),
          "and the kept copy's texture and framebuffer are freed");

    // The XFB has frame 41's top half, so this presents a frame the size of the one freed.
    gx::render_keep_last(true);
    present_frame(52, bottom);
    g_window = -1;
    check(gx::render_present_last() && g_window == 52,
          "keeping again keeps the next frame presented, in a new texture though it's the same size");

    // The overlay's Display page changes the scale between frames. An XFB kept at the old
    // scale would show a corner of the next frame scaling up, and old rows scaling down.
    const GLuint xfb_tex = gx::g_xfbs[0].tex, xfb_fbo = gx::g_xfbs[0].fbo;
    gx::render_set_internal_scale(3);
    check(gx::g_xfbs.empty() && g_deleted.count(xfb_tex) && g_deleted.count(xfb_fbo),
          "a change of internal scale frees the XFB sized at the old one");
    present_frame(61, top);
    present_frame(62, bottom);
    check(g_window == 62 && gx::g_xfbs.size() == 1 && g_size[gx::g_xfbs[0].tex] == std::make_pair(640 * 3, 480 * 3),
          "the next frame copied in halves is assembled in an XFB sized at the new scale");
    return g_failed;
}
