// What the notices (notices.cpp) have for overlay.cpp.
#pragma once

#include <cstdint>

struct SDL_Window;

// Before the menu's context is made, their own over `window`, with GL where `gl` says.
void notices_init(SDL_Window* window, bool gl);
void notices_motion(bool on);
// Whether any notice is rising or fading at `now`, or moving down as the stack reflows.
bool notices_moving(uint64_t now);
// After the menu, if one is showing: the pills over the whole window.
void notices_frame();
#ifdef GCN_OVERLAY_TEST
// Their context shut down and destroyed, for overlay_test_end.
void notices_test_end();
#endif
