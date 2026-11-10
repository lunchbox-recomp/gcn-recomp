// GL back end for the GX pixel pipeline. Runs on the main thread.
//
// This file sets it up: the EFB and the GL objects every pass uses, at an internal scale
// that can change between frames. The other render_gl_*.cpp files each hold one job, and
// share what they must through render_gl_internal.h.
#include "render_gl_internal.h"

namespace gx {

// Samples kept per hardware pixel, in each axis. See render_set_internal_scale.
int g_scale = 2;

GLuint g_efb_fbo, g_efb_color, g_efb_depth;
GLuint g_copy_prog, g_copy_vao;
GLint g_copy_u_src, g_copy_u_rect, g_copy_u_mode, g_copy_u_depth, g_copy_u_alpha;
GLuint g_blit_prog;
GLint g_blit_u_src, g_blit_u_rect;
GLuint g_copy_fbo;
// The morph's background: see draw_morph_background.
GLuint g_bg_prog;
GLint g_bg_u_mvp, g_bg_u_crop, g_bg_u_quad, g_bg_u_col;
GLuint g_vs;

static GLuint compile(GLenum type, const std::string& src) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        fprintf(stderr, "%s\n", src.c_str());
        fatal("shader compile failed: %s", log);
    }
    return sh;
}

static GLuint link(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fatal("program link failed: %s", log);
    }
    return p;
}

// Without a version line: glsl_header_with_clip() goes in front. Clip distances 4 and 5
// are enabled for every program on desktop GL (see render_init), so this writes them.
static const char* kCopyVS = R"(
out vec2 v_uv;
uniform vec4 u_rect;  // source rect in normalized EFB coords (x0,y0,x1,y1), y down
void main() {
    vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    v_uv = mix(u_rect.xy, u_rect.zw, p);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
#ifdef GCN_CLIP
    gl_ClipDistance[4] = 1.0;
    gl_ClipDistance[5] = 1.0;
#endif
}
)";

// Converts EFB color (or depth) to the requested GX copy format. Output row 0 = top.
static const char* kCopyFS = GCN_GLSL_VERSION R"(
in vec2 v_uv;
uniform sampler2D u_src;
uniform sampler2D u_depth;
uniform int u_mode;   // copy texture format (+16 intensity, +32 depth)
uniform int u_alpha;  // 0 when the EFB's pixel format has no alpha, which a copy reads as 1
out vec4 o;
void main() {
    vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);
    vec4 c = texture(u_src, uv);
    // The GL framebuffer keeps an alpha channel whatever the format, holding what an earlier
    // pass left there.
    if (u_alpha == 0) c.a = 1.0;
    if ((u_mode & 32) != 0) {
        float z = texture(u_depth, uv).r;
        // Clamped: at the far plane z * 16777215 + 0.5 rounds up to 16777216 in single
        // precision, whose top byte is 0, so a cleared depth buffer copied as Z8 read as
        // the nearest depth instead of the farthest.
        uint zi = min(uint(z * 16777215.0 + 0.5), 16777215u);
        c = vec4(float((zi >> 16) & 255u), float((zi >> 8) & 255u), float(zi & 255u), 255.0) / 255.0;
        c = vec4(c.r, c.g, c.b, c.r);
        // The two-byte depth formats store the lower of their two bytes first: Z16 is
        // (middle, top) and Z16L (low, middle), the reverse of the colour order the
        // swizzles below assume. Read back as IA8 that puts the top byte in the
        // intensity and the middle in the alpha, and an indirect lookup that takes its
        // column from the alpha and its row from the intensity indexes a ramp whose rows
        // are the coarse axis. With the bytes the other way round the lookup is
        // transposed, and the ramp's steep last rows light up wherever the middle byte
        // nears 255, in lines across the fog.
        int dfmt = u_mode & 15;
        if (dfmt == 11) c = vec4(c.g, c.r, c.b, c.a);
        else if (dfmt == 12) c = vec4(c.r, c.b, c.g, c.a);
    }
    int fmt = u_mode & 15;
    bool intensity = (u_mode & 16) != 0;
    if (intensity) {
        float y = clamp(dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0, 0.0, 1.0);
        c.rgb = vec3(y);
        // I4 and I8 hold the intensity alone, which a texture of either format reads back
        // in all four components, the alpha too; IA4 and IA8 keep the frame's alpha. I4
        // keeps the top four bits of the 8-bit value: a game that reads the copy back as
        // CI4 indexes its palette with exactly those.
        if (fmt == 0) c = vec4(floor(floor(y * 255.0 + 0.5) / 16.0) / 15.0);
        else if (fmt == 1) c = vec4(y);
    }
    // Single-channel formats replicate into all components like the texture decoder does.
    // R4 (0) is the red channel at 4 bits, read back as I4: a projected shadow draws its
    // caster white into a corner of the frame and copies that as its mask, whose alpha
    // is then the red, not whatever the frame's alpha held there.
    if (fmt == 7) c = vec4(c.a);              // A8
    else if (fmt == 0 && !intensity) c = vec4(floor(c.r * 15.0 + 0.5) / 15.0);   // R4
    else if (fmt == 8) c = vec4(c.r);         // R8
    else if (fmt == 9) c = vec4(c.g);         // G8
    else if (fmt == 10) c = vec4(c.b);        // B8
    else if (fmt == 1 && !intensity) c = vec4(c.r);
    // The two-channel formats are read back as IA8: the first byte is the alpha and the
    // second the intensity. (A 16-bit depth copy is RG8 with its bytes swapped above.)
    else if (fmt == 11) c = vec4(c.g, c.g, c.g, c.r);   // RG8
    else if (fmt == 12) c = vec4(c.b, c.b, c.b, c.g);   // GB8
    if (fmt == 4) c.a = 1.0;                  // RGB565
    o = c;
}
)";

static const char* kBlitFS = GCN_GLSL_VERSION R"(
in vec2 v_uv;
uniform sampler2D u_src;
out vec4 o;
void main() { o = vec4(texture(u_src, vec2(v_uv.x, 1.0 - v_uv.y)).rgb, 1.0); }
)";

void render_init(int internal_scale) {
    g_scale = internal_scale < 1 ? 1
                                 : (internal_scale > kMaxInternalScale ? kMaxInternalScale
                                                                       : internal_scale);
    const std::string vs_src = gen_vertex_shader();
#ifdef GCN_GL_ES
    {
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n && !g_clip_ok; i++) {
            const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            g_clip_ok = e && !strcmp(e, "GL_EXT_clip_cull_distance");
        }
        if (!g_clip_ok)
            fprintf(stderr, "no GL_EXT_clip_cull_distance: the theater/stereo morph is uncropped\n");
        for (GLint i = 0; i < n && !g_dual_source; i++) {
            const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            g_dual_source = e && !strcmp(e, "GL_EXT_blend_func_extended");
        }
        if (!g_dual_source)
            fprintf(stderr, "no GL_EXT_blend_func_extended: a blended draw stores its own alpha, not GXSetDstAlpha's\n");
        g_base_vertex = glDrawElementsBaseVertex != nullptr;
        if (!g_base_vertex)
            fprintf(stderr, "no glDrawElementsBaseVertex: base vertices are added into the indices instead\n");
    }
#else
    g_clip_ok = true;
    g_dual_source = true;
    // GX clips depth in clip space and clamps what the viewport makes of it, so a viewport
    // may map depth well past the buffer's range; the vertex shader clips at GX's planes
    // through clip distances 4 and 5, which every program here writes. ES has no depth
    // clamp in core, and keeps GL's clipping at the viewport's depth.
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_CLIP_DISTANCE0 + 4);
    glEnable(GL_CLIP_DISTANCE0 + 5);
#endif
    g_renderer_writes_back = true;  // see set_copy_writeback in gx.h
    g_vs_hash = hash_str(vs_src.c_str());
    g_vs = compile(GL_VERTEX_SHADER, vs_src);
#if GCN_HAVE_PROGRAM_BINARY
    {
        GLint n = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &n);
        g_binaries_supported = n > 0;
    }
#endif
    {
        // GL_COMPLETION_STATUS_KHR is the same enum in the ARB extension. Neither says how
        // many threads build programs; ANGLE uses as many as it likes unless told otherwise.
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        bool parallel = false;
        for (GLint i = 0; i < n && !parallel; i++) {
            const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            parallel = e && (!strcmp(e, "GL_KHR_parallel_shader_compile") || !strcmp(e, "GL_ARB_parallel_shader_compile"));
        }
        bool want = g_async_wanted;
        if (const char* v = getenv_nonempty("GCN_ASYNC_SHADERS")) want = atoi(v) != 0;
        g_parallel_compile = want && parallel;
        if (want)
            fprintf(stderr, parallel ? "[shaders] built in the background; a draw waiting for its program is left out\n"
                                     : "[shaders] no parallel shader compilation: a new program stalls the frame it is built in\n");
    }
    shader_cache_load();

    glGenFramebuffers(1, &g_efb_fbo);
    glGenTextures(1, &g_efb_color);
    glBindTexture(GL_TEXTURE_2D, g_efb_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_W * g_scale, EFB_H * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &g_efb_depth);
    glBindTexture(GL_TEXTURE_2D, g_efb_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, EFB_W * g_scale, EFB_H * g_scale, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_efb_color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_efb_depth, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("EFB framebuffer incomplete");
    glClearColor(0, 0, 0, 1);
    clear_depth(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glGenVertexArrays(kVertexRing * 9, &g_vaos[0][0]);
    glGenBuffers(kVertexRing, g_vbos);
    glGenBuffers(kVertexRing, g_ebos);
    for (int r = 0; r < kVertexRing; r++)
        for (uint32_t n = 0; n < 9; n++) {
            glBindVertexArray(g_vaos[r][n]);
            glBindBuffer(GL_ARRAY_BUFFER, g_vbos[r]);
            // The element buffer binding is part of the VAO's state, so binding it once here
            // keeps it bound whenever this VAO is.
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ebos[r]);
            const GLsizei stride = (GLsizei)packed_stride(n);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(GpuVertex, pos));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(GpuVertex, col[0]));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(GpuVertex, col[1]));
            for (uint32_t i = 0; i < n; i++) {
                glEnableVertexAttribArray(3 + i);
                glVertexAttribPointer(3 + i, 3, GL_FLOAT, GL_FALSE, stride, (void*)(offsetof(GpuVertex, tex) + i * 12));
            }
        }
    // A coordinate past a draw's count reads the attribute's constant value instead, which
    // is what the transform writes into a GpuVertex's unused ones.
    for (int i = 0; i < 8; i++) glVertexAttrib4f(3 + i, 0.0f, 0.0f, 1.0f, 1.0f);
    g_vao = g_vaos[0][0];
    g_vbo = g_vbos[0];
    glBindVertexArray(g_vao);

    GLuint cvs = compile(GL_VERTEX_SHADER, glsl_header_with_clip() + kCopyVS);
    g_copy_prog = link(cvs, compile(GL_FRAGMENT_SHADER, kCopyFS));
    g_copy_u_src = glGetUniformLocation(g_copy_prog, "u_src");
    g_copy_u_depth = glGetUniformLocation(g_copy_prog, "u_depth");
    g_copy_u_rect = glGetUniformLocation(g_copy_prog, "u_rect");
    g_copy_u_mode = glGetUniformLocation(g_copy_prog, "u_mode");
    g_copy_u_alpha = glGetUniformLocation(g_copy_prog, "u_alpha");
    g_blit_prog = link(cvs, compile(GL_FRAGMENT_SHADER, kBlitFS));
    g_blit_u_src = glGetUniformLocation(g_blit_prog, "u_src");
    g_blit_u_rect = glGetUniformLocation(g_blit_prog, "u_rect");
    glGenVertexArrays(1, &g_copy_vao);
    glGenFramebuffers(1, &g_copy_fbo);
    {
        const std::string bvs = glsl_header_with_clip() + R"(
uniform mat4 u_mvp;
uniform mat4 u_crop;
uniform vec4 u_quad;   // half-width, half-height, view-space z, unused
void main() {
    vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0 - 1.0;
    vec4 a = vec4(c * u_quad.xy, u_quad.z, 1.0);
    gl_Position = u_mvp * a;
#ifdef GCN_CLIP
    vec4 cd = u_crop * a;
    gl_ClipDistance[0] = cd.x;
    gl_ClipDistance[1] = cd.y;
    gl_ClipDistance[2] = cd.z;
    gl_ClipDistance[3] = cd.w;
    gl_ClipDistance[4] = 1.0;
    gl_ClipDistance[5] = 1.0;
#endif
}
)";
        const std::string bfs = std::string(GCN_GLSL_VERSION) + R"(
uniform vec3 u_col;
out vec4 o;
void main() { o = vec4(u_col, 1.0); }
)";
        g_bg_prog = link(compile(GL_VERTEX_SHADER, bvs), compile(GL_FRAGMENT_SHADER, bfs));
        g_bg_u_mvp = glGetUniformLocation(g_bg_prog, "u_mvp");
        g_bg_u_crop = glGetUniformLocation(g_bg_prog, "u_crop");
        g_bg_u_quad = glGetUniformLocation(g_bg_prog, "u_quad");
        g_bg_u_col = glGetUniformLocation(g_bg_prog, "u_col");
    }
#ifndef GCN_GL_ES
    // Desktop core profile needs this to honour gl_PointSize; ES always does.
    glEnable(GL_PROGRAM_POINT_SIZE);
#endif
}

// Change how many samples the EFB keeps per hardware pixel, between frames.
//
// The two VR views want different answers. Theater shows the EFB itself, blown up to fill
// a panel wider than the frame was ever drawn for, so every extra sample is detail the
// viewer sees. Stereo never shows it: there it holds only what the eye passes sample out
// of it -- the water reflection, the sheet the spray is cut from -- and the pass that
// fills it is a whole extra scene render on top of the two eyes. So the scale follows the
// view, which means changing it while the game runs.
//
// Nothing in the EFB has to survive the change: it is cleared and redrawn every frame.
// The textures EFB copies land in do not get off so lightly -- they are allocated at
// dst * scale but matched for reuse by their logical size alone, so a survivor would be
// reused at the old resolution and quietly sample wrong. They go with the EFB, and so do
// the XFBs display copies are assembled in, which are sized and found again the same way.
void render_set_internal_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > kMaxInternalScale) scale = kMaxInternalScale;
    if (scale == g_scale) return;
    const int old_scale = g_scale;
    g_scale = scale;
    gl_state_invalidate();
    uniforms_invalidate();
    // The EFB keeps what it held, scaled. A frame on the console does not begin with a
    // clear -- the previous frame's display copy cleared the EFB -- so the first frame drawn
    // after a change of scale would otherwise start from undefined colour and depth, and
    // depth that fails every test leaves the frame empty. A frontend changes the scale as it
    // moves between its views, and the first pass of the new view showed it: the left image
    // of a stereo pair came out black for a frame while the right, drawn after the left's
    // display copy had cleared the EFB, was whole.
    GLuint color = 0, depth = 0;
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_W * g_scale, EFB_H * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &depth);
    glBindTexture(GL_TEXTURE_2D, depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, EFB_W * g_scale, EFB_H * g_scale, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
    // A blit is cut by the scissor and the write masks; the state cache was just
    // invalidated, so the next draw sets them again.
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    const GLint ow = EFB_W * old_scale, oh = EFB_H * old_scale, nw = EFB_W * g_scale, nh = EFB_H * g_scale;
    glBlitFramebuffer(0, 0, ow, oh, 0, 0, nw, nh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBlitFramebuffer(0, 0, ow, oh, 0, 0, nw, nh, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &g_efb_color);
    glDeleteTextures(1, &g_efb_depth);
    g_efb_color = color;
    g_efb_depth = depth;
    for (auto it = g_textures.begin(); it != g_textures.end();) {
        if (it->second.efb) {
            glDeleteTextures(1, &it->second.tex);
            it = g_textures.erase(it);
        } else {
            ++it;
        }
    }
    xfbs_free();
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fatal("EFB framebuffer incomplete at internal scale %d", g_scale);
}

}  // namespace gx
