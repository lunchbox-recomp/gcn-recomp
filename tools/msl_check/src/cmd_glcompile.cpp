// The glcompile mode: the baseline for Metal's compile times.
#include "commands.h"
#include "gl_ref.h"
#include "spike_util.h"
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <cstdlib>
#include <string>
#include <vector>

using namespace gx;

// The baseline: the same keys through Apple's GL, compile, link and a first draw (the
// driver may finish a program only when it is first used).
int cmd_glcompile(const char* keys_path) {
    std::vector<ShaderKey> keys = load_keys(keys_path);
    if (!gl_init()) return 1;
    const std::string vsrc = gen_vertex_shader();
    GLuint fbo, color, vao;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glViewport(0, 0, 4, 4);
    std::vector<double> ms;
    for (size_t n = 0; n < keys.size(); n++) {
        // A nonce in the code, as the Metal side has, so that no compile comes from a cache
        // (Apple's GL runs on Metal and may share its shader cache).
        std::string fsrc = gen_pixel_shader(keys[n]);
        if (!getenv("NONONCE"))
            fsrc.insert(fsrc.rfind('}'), "  if (u_points == " + std::to_string((arc4random() & 0x3FFFFFFF) | 1) + ") o_color.r = 0.123;\n");
        const double t0 = now_ms();
        GLuint p = glCreateProgram();
        GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
        const char* vp = vsrc.c_str();
        const char* fp = fsrc.c_str();
        glShaderSource(v, 1, &vp, nullptr);
        glShaderSource(f, 1, &fp, nullptr);
        glCompileShader(v);
        glCompileShader(f);
        glAttachShader(p, v);
        glAttachShader(p, f);
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        glUseProgram(p);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        ms.push_back(now_ms() - t0);
        glDeleteShader(v);
        glDeleteShader(f);
        glDeleteProgram(p);
    }
    stats("GL compile + link + first draw", ms);
    return 0;
}
