// The menu's blurred backdrop's benchmark: the `bench_blur` target of a desktop build, on this
// machine's GL in a hidden window, with no disc or game.
//
//   bench_blur W H    a frame presented and kept, then the frontend's held draws before the menu's
//                     (render_present_last, then render_overlay_blur) for two holds of 30 frames
//                     each, with a close between them that fades the blur out, the window W by H
//                     pixels
//
// Each draw's CPU time to the call's return is printed, which is what the held loop's interval
// carries (main.cpp's show_frame stamps the menu's frame after it), and the time to glFinish
// after it beside it. The process's first hold is the one a game's first menu open has: run it
// as a fresh process each time.
#include "runtime.h"
#include "gx/gl.h"
#include "gx/render_gl.h"
#include "gx/render_gl_internal.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// What the renderer reaches of the rest of the runtime, which this links none of.
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
uint32_t vi_display_height() { return 0; }  // each display copy a whole frame
namespace gx {
std::atomic<bool> g_renderer_writes_back{false};
static float g_aspect = 4.0f / 3.0f;  // xf.cpp's, which would bring the whole front end
void render_set_output_aspect(float aspect) { g_aspect = aspect; }
float output_aspect() { return g_aspect; }
}

namespace {

void* gl_proc(const char* name) { return (void*)SDL_GL_GetProcAddress(name); }

SDL_Window* open_window() {
    if (!SDL_Init(SDL_INIT_VIDEO)) return nullptr;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* win = SDL_CreateWindow("bench_blur", 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win || !SDL_GL_CreateContext(win)) return nullptr;
    SDL_GL_SetSwapInterval(0);
    if (gl_load_with(gl_proc) < GCN_GL_VERSION_MIN) return nullptr;
    return win;
}

double ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// One held draw before the menu's: the kept frame drawn again and the blur over it at `alpha`.
void held_draw(const char* what, bool hold, float alpha, bool say) {
    const auto t0 = std::chrono::steady_clock::now();
    gx::render_present_last();
    const auto t1 = std::chrono::steady_clock::now();
    gx::render_overlay_blur(hold, alpha);
    const auto t2 = std::chrono::steady_clock::now();
    glFinish();
    const auto t3 = std::chrono::steady_clock::now();
    if (say) printf("%-26s present %6.3f ms  blur %6.3f ms  to glFinish %6.3f ms\n", what, ms(t0, t1), ms(t1, t2), ms(t2, t3));
}

// The steady draws of a hold after its first, as a median of the blur's CPU time.
void steady(const char* what, int n) {
    double sum_present = 0, sum_blur = 0;
    for (int i = 0; i < n; i++) {
        const auto t0 = std::chrono::steady_clock::now();
        gx::render_present_last();
        const auto t1 = std::chrono::steady_clock::now();
        gx::render_overlay_blur(true, 1.0f);
        const auto t2 = std::chrono::steady_clock::now();
        glFinish();
        sum_present += ms(t0, t1);
        sum_blur += ms(t1, t2);
    }
    printf("%-26s present %6.3f ms  blur %6.3f ms  a frame, the mean of %d\n", what, sum_present / n, sum_blur / n, n);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 || atoi(argv[1]) <= 0 || atoi(argv[2]) <= 0) {
        fprintf(stderr, "usage: bench_blur W H\n");
        return 2;
    }
    SDL_Window* win = open_window();
    if (!win) {
        fprintf(stderr, "bench_blur: no GL window (%s)\n", SDL_GetError());
        return 1;
    }
    const int w = atoi(argv[1]), h = atoi(argv[2]);
    gx::render_init(2);
    gx::render_keep_last(true);
    gx::render_set_window_size(w, h);
    gx::render_set_output_aspect(16.0f / 9.0f);
    gx::EfbCopyCmd c{};
    c.src_w = c.dst_w = 640;
    c.src_h = c.dst_h = 480;
    c.to_xfb = true;
    gx::present(c);
    glFinish();
    printf("bench_blur at %dx%d, the blur's targets %dx%d\n", w, h, (w + 3) / 4, (h + 3) / 4);
    for (int hold = 1; hold <= 2; hold++) {
        char what[64];
        snprintf(what, sizeof(what), "hold %d, its first frame", hold);
        held_draw(what, true, 0.1f, true);
        steady("  then", 29);
        for (int i = 0; i < 10; i++) held_draw("", false, 1.0f - 0.1f * (float)i, false);
        held_draw("", false, 0.0f, false);
    }
    return 0;
}
