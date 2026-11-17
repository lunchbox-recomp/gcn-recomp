// What the tools that draw on this machine's GL with no disc or game share: the stubs of the
// runtime the renderer reaches, which they link none of, and a hidden window with a GL context.
// tools/bench_blur.cpp, tools/bench_splash.cpp and tools/test_ports.cpp each read it once, as it
// defines the stubs.
#pragma once
#include "runtime.h"  // fatal's declaration
#include "gx/gl.h"
#include "gx/render.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
namespace gx {
std::atomic<bool> g_renderer_writes_back{false};
}

// A hidden window titled `title` with a GL core context of `major`.`minor` or later current. With
// `pixels` it's `w` by `h` pixels whatever the display's pixel density, and without, `w` by `h` in
// the display's units. Null if SDL can't make one, and SDL_GetError says why.
inline SDL_Window* gl_hidden_window(const char* title, int w, int h, int major, int minor, bool pixels) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    const SDL_WindowFlags density = pixels ? SDL_WINDOW_HIGH_PIXEL_DENSITY : 0;
    SDL_Window* window = SDL_CreateWindow(title, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | density);
    const float scale = window && pixels ? SDL_GetWindowPixelDensity(window) : 1.0f;
    if (scale > 1.0f) SDL_SetWindowSize(window, (int)(w / scale), (int)(h / scale));
    SDL_GLContext ctx = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!ctx) {
        if (window) SDL_DestroyWindow(window);
        return nullptr;
    }
    SDL_GL_MakeCurrent(window, ctx);
    return window;
}

// A benchmark's window: SDL's video started, a GL 4.1 context in a window of `w` by `h` in the
// display's units, swaps that don't wait for the display, and GL's functions loaded. Null if any
// of that fails, and SDL_GetError may say why.
inline SDL_Window* gl_bench_window(const char* title, int w, int h) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return nullptr;
    SDL_Window* window = gl_hidden_window(title, w, h, 4, 1, false);
    if (!window) return nullptr;
    SDL_GL_SetSwapInterval(0);
    const GLLoadProc proc = [](const char* name) { return (void*)SDL_GL_GetProcAddress(name); };
    if (gl_load_with(proc) < GCN_GL_VERSION_MIN) return nullptr;
    return window;
}
