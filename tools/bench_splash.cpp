// The boot splash's benchmark: the `bench_splash` target of a desktop build, on this machine's
// GL in a hidden window, with no disc or game.
//
//   bench_splash frames            each splash frame's cost, drawn offscreen and finished, at
//                                  2560 x 1920 with the bar up, for a banner of one run per pixel
//                                  (the most a banner can be) and a shaded shape
//   bench_splash shot OUT.png      one frame of the shaded shape, the bar half full, as a PNG
//   bench_splash cache N PATH      writes a shader cache of N made-up programs to PATH
//   bench_splash load PATH         builds PATH's programs at once, as render_init does with no splash
//   bench_splash steps PATH        builds them in the splash's steps with nothing drawn
//   bench_splash splash PATH       builds them in steps under the splash, as the window does
//
// A load prints the renderer's own `[shaders] ... in N ms` line, the time the two are compared
// by, and the splash its `[splash]` line. The cache is rewritten by each load, as a run does.
#include "splash.h"
#include "banner.h"
#include "runtime.h"
#include "gx/gl.h"
#include "gx/render_gl.h"
#include "gx/shadergen.h"
#include "gx/splash_look.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

// What the renderer reaches of the rest of the runtime, which this links none of.
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
bool write_png(const char* path, const uint8_t* rgba, int w, int h);  // runtime/png.cpp
namespace gx {
std::atomic<bool> g_renderer_writes_back{false};
}

namespace {

void* gl_proc(const char* name) { return (void*)SDL_GL_GetProcAddress(name); }

SDL_Window* open_window(int w, int h) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return nullptr;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* win = SDL_CreateWindow("bench_splash", w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win || !SDL_GL_CreateContext(win)) return nullptr;
    SDL_GL_SetSwapInterval(0);
    if (gl_load_with(gl_proc) < GCN_GL_VERSION_MIN) return nullptr;
    return win;
}

bool pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {}
    return true;
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// Every pixel its own colour, opaque, so one run each, the most a banner can be; or a wide shape
// with a soft edge, shaded across in steps of two pixels, as a busy logo is.
std::vector<uint8_t> made_up_banner(bool every_pixel) {
    std::vector<uint8_t> rgba(kBannerW * kBannerH * 4, 0);
    for (int y = 0; y < kBannerH; y++)
        for (int x = 0; x < kBannerW; x++) {
            uint8_t* p = &rgba[((size_t)y * kBannerW + x) * 4];
            if (every_pixel) {
                p[0] = (uint8_t)(x * 2);
                p[1] = (uint8_t)(y * 8);
                p[2] = (uint8_t)((x + y) & 1 ? 255 : 0);
                p[3] = 255;
            } else {
                const int dx = x - 48, dy = (y - 16) * 3;
                const int d = dx * dx + dy * dy;
                p[0] = 240;
                p[1] = (uint8_t)(120 + (x / 2) * 2);  // a new colour every other pixel
                p[2] = 60;
                p[3] = d < 1600 ? 255 : d < 1900 ? 128 : 0;
            }
        }
    return rgba;
}

// A framebuffer of its own at 2560 x 1920 (a 1280 x 960 window at two pixels a point), bound,
// so every rect is rasterized whether or not the hidden window's own is. 0 if it can't be made.
constexpr int kW = 2560, kH = 1920;
GLuint offscreen() {
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE ? fbo : 0;
}

// Each frame drawn offscreen and finished.
int frames(SDL_Window* win) {
    const int w = kW, h = kH;
    const GLuint fbo = offscreen();
    if (!fbo) return 1;
    for (bool every : {true, false}) {
        const std::vector<uint8_t> banner = made_up_banner(every);
        splash_show(win, 0x18191C, banner.data());
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        std::vector<double> ms;
        for (int i = 0; i < 300; i++) {
            const auto t = std::chrono::steady_clock::now();
            splash_draw(w, h, 400.0, i, 300);
            glFinish();
            ms.push_back(ms_since(t));
        }
        const double first = ms[0];
        ms.erase(ms.begin());
        std::sort(ms.begin(), ms.end());
        printf("%s, %zu runs: the first %.3f ms, then median %.3f ms, 95th %.3f ms, most %.3f ms a frame\n",
               every ? "one run a pixel" : "a shaded shape", gx::banner_runs(banner.data(), 0x18191C).size(), first,
               ms[ms.size() / 2], ms[ms.size() * 95 / 100], ms.back());
    }
    return 0;
}

// One frame of the shaded shape with the bar half full, offscreen, as a PNG to look at.
int shot(SDL_Window* win, const char* path) {
    const GLuint fbo = offscreen();
    if (!fbo) return 1;
    const std::vector<uint8_t> banner = made_up_banner(false);
    splash_show(win, 0x18191C, banner.data());
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    splash_draw(kW, kH, 400.0, 1, 2);
    std::vector<uint8_t> up((size_t)kW * kH * 4), down(up.size());
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, up.data());
    for (int y = 0; y < kH; y++) memcpy(&down[(size_t)y * kW * 4], &up[(size_t)(kH - 1 - y) * kW * 4], (size_t)kW * 4);
    return write_png(path, down.data(), kW, kH) ? 0 : 1;
}

// N keys whose programs differ: one to four stages, each stage's colour and alpha
// combiners varied, kept only when its source is new.
int cache(int n, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return 1;
    const uint32_t magic = 0x43535257, ver = gx::kShaderKeyFormat;  // render_gl_programs.cpp's
    const uint64_t drv = 0;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver, 4, 1, f);
    fwrite(&drv, 8, 1, f);
    std::set<std::string> seen;
    for (uint32_t i = 0; (int)seen.size() < n && i < 100000; i++) {
        gx::ShaderKey k;
        memset(&k, 0, sizeof(k));
        const uint32_t stages = i % 4;
        k.genmode = stages << 10;
        for (uint32_t s = 0; s <= stages; s++) {
            k.cenv[s] = (i * 2654435761u >> (s * 3)) & 0xFFFFFF;
            k.aenv[s] = (i * 40503u >> s) & 0xFFFFF0;
        }
        gx::canonicalize_shader_key(k);
        if (!seen.insert(gx::gen_pixel_shader(k)).second) continue;
        const uint64_t hash = 0;
        const uint32_t fmt = 0, len = 0;
        fwrite(&k, sizeof(k), 1, f);
        fwrite(&hash, 8, 1, f);
        fwrite(&fmt, 4, 1, f);
        fwrite(&len, 4, 1, f);
    }
    fclose(f);
    printf("%zu programs in %s\n", seen.size(), path);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "cache") && argc == 4) return cache(atoi(argv[2]), argv[3]);
    SDL_Window* win = open_window(1280, 960);
    if (!win) {
        fprintf(stderr, "bench_splash: no GL window (%s)\n", SDL_GetError());
        return 1;
    }
    if (!strcmp(mode, "frames")) return frames(win);
    if (!strcmp(mode, "shot") && argc == 3) return shot(win, argv[2]);
    if ((!strcmp(mode, "load") || !strcmp(mode, "steps") || !strcmp(mode, "splash")) && argc == 3) {
        gx::render_set_shader_cache(argv[2]);
        const bool splash = !strcmp(mode, "splash"), steps = !strcmp(mode, "steps");
        const std::vector<uint8_t> banner = made_up_banner(false);
        const auto t = std::chrono::steady_clock::now();
        if (splash) splash_show(win, 0x18191C, banner.data());
        if (steps) gx::render_set_shader_cache_deferred(true);
        gx::render_init(1);
        if (splash) splash_load(win, pump);
        while (steps && !gx::render_shader_cache_step(30.0, nullptr, nullptr)) pump();
        printf("%s: %.0f ms to the end of the load\n", mode, ms_since(t));
        return 0;
    }
    fprintf(stderr, "usage: bench_splash frames | shot OUT.png | cache N PATH | load PATH | steps PATH | splash PATH\n");
    return 2;
}
