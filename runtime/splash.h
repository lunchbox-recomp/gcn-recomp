// The desktop window's boot splash: from the window's first frame until the shader cache is
// built, a background colour with the disc's own banner (banner.h) and, once it has been up
// 300 ms, a thin bar of the cache's progress. It's drawn with scissored clears alone
// (gx/clear_draw.h), since no shader program is built yet, and swapped straight to the window:
// it never goes through the renderer's present, so present_count, frame dumps, GCN_QUIT_AT and
// the movie don't see it. The last frame stays up while the disc boots, until the game's first.
//
// The cache is built in steps (render_set_shader_cache_deferred) with the window's events
// taken between them, where without the splash render_init builds it all at once.
#pragma once
#include <cstdint>

struct SDL_Window;

// Before render_init: opens the disc for its banner (di_init), puts the cache in steps and
// shows the first frame. splash_show does the same with a banner already decoded, or none.
void splash_begin(SDL_Window* win, uint32_t rgb, const char* iso);
void splash_show(SDL_Window* win, uint32_t rgb, const uint8_t* banner_rgba);

// After render_init: builds the cache a step at a time, drawing a frame whenever the picture
// changes, with `pump` taking the window's events between. A `pump` that says to stop (false)
// isn't called again, and its stop is passed on to the next event loop as SDL_EVENT_QUIT.
// Returns the frames drawn, the first one included.
int splash_load(SDL_Window* win, bool (*pump)());

// One frame into the bound framebuffer, `ms_up` into the splash, with no swap. For the
// benchmark; splash_load draws its own.
void splash_draw(int w, int h, double ms_up, int done, int total);
