#include "gl_ref.h"
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace gx;

static GLuint gl_shader(GLenum type, const std::string& src) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "GL compile failed:\n%s\n", log);
        exit(1);
    }
    return s;
}

bool gl_init() {
    CGLPixelFormatAttribute attrs[] = {kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core, kCGLPFAAccelerated,
                                       kCGLPFAAllowOfflineRenderers, (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pf = nullptr;
    GLint npix = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &npix) != kCGLNoError || !pf) return false;
    CGLContextObj ctx = nullptr;
    if (CGLCreateContext(pf, nullptr, &ctx) != kCGLNoError) return false;
    CGLDestroyPixelFormat(pf);
    CGLSetCurrentContext(ctx);
    printf("GL: %s, %s\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    return true;
}

static GLuint gl_texture(int w, int h, const uint8_t* data) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, g_linear && h > 1 ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, g_linear && h > 1 ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return t;
}

Image gl_draw(const ShaderKey& k, const Inputs& in, int logic_op) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, gl_shader(GL_VERTEX_SHADER, gen_vertex_shader()));
    glAttachShader(prog, gl_shader(GL_FRAGMENT_SHADER, gen_pixel_shader(k)));
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "GL link failed:\n%s\n", log);
        exit(1);
    }
    glUseProgram(prog);
    auto L = [&](const char* n) { return glGetUniformLocation(prog, n); };
    const blk::FsBlock& f = in.fs;
    glUniform4iv(L("u_reg"), 4, (const GLint*)f.u_reg);
    glUniform4iv(L("u_kstage"), 16, (const GLint*)f.u_kstage);
    glUniform2fv(L("u_texsize"), 8, (const GLfloat*)f.u_texsize);
    GLint indmtx[18];
    for (int i = 0; i < 6; i++)
        for (int c = 0; c < 3; c++) indmtx[i * 3 + c] = f.u_indmtx[i][c];
    glUniform3iv(L("u_indmtx"), 6, indmtx);
    glUniform1iv(L("u_indscale"), 3, f.u_indscale);
    glUniform2iv(L("u_alpharef"), 1, (const GLint*)&f.u_alpharef);
    glUniform4fv(L("u_fog"), 1, (const GLfloat*)&f.u_fog);
    glUniform1i(L("u_fogmode"), f.u_fogmode);
    glUniform3fv(L("u_fogcolor"), 1, (const GLfloat*)&f.u_fogcolor);
    glUniform1i(L("u_zbias"), f.u_zbias);
    glUniform4fv(L("u_zplane"), 1, (const GLfloat*)&f.u_zplane);
    glUniform1f(L("u_dstalpha"), f.u_dstalpha);
    glUniform2fv(L("u_indcoordscale"), 4, (const GLfloat*)f.u_indcoordscale);
    glUniform1i(L("u_points"), f.u_points);
    glUniform1fv(L("u_point_off"), 8, f.u_point_off);
    glUniform1i(L("u_texsnap"), f.u_texsnap);
    glUniform1i(L("u_screen_uv"), f.u_screen_uv);
    glUniform2fv(L("u_screen_px"), 1, (const GLfloat*)&f.u_screen_px);
    glUniform2fv(L("u_screen_ripple"), 1, (const GLfloat*)&f.u_screen_ripple);
    // u_vr and u_vp_b exist in both stages under the same names (a fg_depth key's).
    const blk::VsBlock& v = in.vs;
    glUniformMatrix4fv(L("u_proj"), 1, GL_FALSE, (const GLfloat*)&v.u_proj);
    glUniform4fv(L("u_vp_a"), 1, (const GLfloat*)&v.u_vp_a);
    glUniform4fv(L("u_vp_b"), 1, (const GLfloat*)&v.u_vp_b);
    glUniform1f(L("u_point_size"), v.u_point_size);
    glUniform1i(L("u_vr"), v.u_vr);
    glUniform4fv(L("u_uvgrad"), 8, (const GLfloat*)v.u_uvgrad);
    glUniformMatrix4fv(L("u_view"), 1, GL_FALSE, (const GLfloat*)&v.u_view);
    glUniformMatrix4fv(L("u_crop"), 1, GL_FALSE, (const GLfloat*)&v.u_crop);
    glUniform4fv(L("u_zproj"), 1, (const GLfloat*)&v.u_zproj);
    GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7}, pal_units[8] = {8, 9, 10, 11, 12, 13, 14, 15};
    glUniform1iv(L("u_tex"), 8, units);
    glUniform1iv(L("u_pal"), 8, pal_units);
    GLuint tex[16];
    for (int m = 0; m < 8; m++) {
        glActiveTexture(GL_TEXTURE0 + m);
        tex[m] = gl_texture(TS, TS, in.tex[m]);
        glActiveTexture(GL_TEXTURE8 + m);
        tex[8 + m] = gl_texture(256, 1, in.pal[m]);
    }
    // target
    GLuint fbo, color, depth;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &color);
    glActiveTexture(GL_TEXTURE0 + 20);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, in.dst);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glGenTextures(1, &depth);
    glBindTexture(GL_TEXTURE_2D, depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, W, H, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "GL framebuffer incomplete\n");
        exit(1);
    }
    glViewport(0, 0, W, H);
    glClearDepth(1.0);
    if (logic_op < 0) {
        glClearColor(1.0f, 0.0f, 1.0f, 128.0f / 255.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    } else {
        glClear(GL_DEPTH_BUFFER_BIT);  // the colour keeps in.dst
    }
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_CLIP_DISTANCE0 + 4);
    glEnable(GL_CLIP_DISTANCE0 + 5);
    glDisable(GL_BLEND);
    if (g_cull_back) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);  // GL's default, which render_gl.cpp keeps
    } else {
        glDisable(GL_CULL_FACE);
    }
    static const GLenum kLogicOp[16] = {GL_CLEAR, GL_AND, GL_AND_REVERSE, GL_COPY, GL_AND_INVERTED, GL_NOOP, GL_XOR, GL_OR,
                                        GL_NOR, GL_EQUIV, GL_INVERT, GL_OR_REVERSE, GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND, GL_SET};
    if (logic_op >= 0) {
        glEnable(GL_COLOR_LOGIC_OP);
        glLogicOp(kLogicOp[logic_op & 15]);
    } else {
        glDisable(GL_COLOR_LOGIC_OP);
    }
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(in.verts), in.verts, GL_STATIC_DRAW);
    int off = 0;
    for (int a = 0; a < 11; a++) {
        const int n = (a == 1 || a == 2) ? 4 : 3;
        glEnableVertexAttribArray(a);
        glVertexAttribPointer(a, n, GL_FLOAT, GL_FALSE, kFloats * 4, (const void*)(intptr_t)off);
        off += n * 4;
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    Image img;
    img.rgba.resize(W * H * 4);
    img.depth.resize(W * H);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
    glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, img.depth.data());
    if (GLenum e = glGetError()) fprintf(stderr, "GL error %04x\n", e);
    glDisable(GL_COLOR_LOGIC_OP);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &color);
    glDeleteTextures(1, &depth);
    glDeleteTextures(16, tex);
    glDeleteProgram(prog);
    return img;
}
