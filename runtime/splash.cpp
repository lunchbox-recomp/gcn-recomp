// See splash.h.
#include "splash.h"
#include "runtime.h"
#include "banner.h"
#include "ready_file.h"
#include "gx/clear_draw.h"
#include "gx/gl.h"
#include "gx/render_gl.h"
#include "gx/splash_look.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <vector>

namespace {

// The cache's step between frames: about 30 frames a second while the bar moves, and the
// window's events taken as often.
constexpr double kStepMs = 30.0;

uint32_t g_bg = gx::kSplashColor;
std::vector<gx::SplashRect> g_runs;  // the banner's, worked out once
std::vector<gx::SplashRect> g_shown;  // what the window has up
int g_shown_w = -1, g_shown_h = -1;
std::chrono::steady_clock::time_point g_t0;

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// The bound framebuffer cleared to the background, then each rect, leaving the scissor off
// and the clear colour as the renderer sets it.
void draw_rects(const std::vector<gx::SplashRect>& rects, int w, int h) {
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gx::clear_rect(0, 0, w, h, g_bg);  // the scissor is off, so all of it
    glEnable(GL_SCISSOR_TEST);
    for (const gx::SplashRect& r : rects) gx::clear_rect(r.x, r.y, r.w, r.h, r.rgb);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
}

// Draws and swaps when the picture differs from what the window has up. True when it drew.
bool frame(SDL_Window* win, int done, int total) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    std::vector<gx::SplashRect> rects = gx::splash_frame(g_runs, g_bg, w, h, ms_since(g_t0), done, total);
    if (w == g_shown_w && h == g_shown_h && rects == g_shown) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    draw_rects(rects, w, h);
    SDL_GL_SwapWindow(win);
    ready_mark("window");
    g_shown = std::move(rects);
    g_shown_w = w;
    g_shown_h = h;
    return true;
}

}  // namespace

void splash_begin(SDL_Window* win, uint32_t rgb, const char* iso) {
    di_init(iso);
    const auto t = std::chrono::steady_clock::now();
    std::vector<uint8_t> rgba(kBannerW * kBannerH * 4);
    const bool ok = banner_read([](uint64_t off, void* dst, uint32_t n) { return iso_read(off, dst, n); }, rgba.data());
    if (ok) fprintf(stderr, "[splash] the disc's banner, read in %.1f ms\n", ms_since(t));
    else fprintf(stderr, "[splash] the disc has no banner to show\n");
    splash_show(win, rgb, ok ? rgba.data() : nullptr);
}

void splash_show(SDL_Window* win, uint32_t rgb, const uint8_t* banner_rgba) {
    g_t0 = std::chrono::steady_clock::now();
    g_bg = rgb;
    g_runs = banner_rgba ? gx::banner_runs(banner_rgba, rgb) : std::vector<gx::SplashRect>{};
    gx::render_set_shader_cache_deferred(true);
    frame(win, 0, 0);
}

int splash_load(SDL_Window* win, bool (*pump)()) {
    int frames = 1, done = 0, total = 0;
    bool stopped = false;
    for (bool finished = false; !finished;) {
        if (!stopped && !pump()) stopped = true;
        // Asked to stop, it builds the rest at once and draws nothing more.
        finished = gx::render_shader_cache_step(stopped ? 1e30 : kStepMs, &done, &total);
        if (!stopped && frame(win, done, total)) frames++;
    }
    if (stopped) {
        SDL_Event quit;
        SDL_zero(quit);
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }
    fprintf(stderr, "[splash] %d frames in %.0f ms\n", frames, ms_since(g_t0));
    return frames;
}

void splash_draw(int w, int h, double ms_up, int done, int total) {
    draw_rects(gx::splash_frame(g_runs, g_bg, w, h, ms_up, done, total), w, h);
}
