// What overlay.cpp has for the overlay's other files (overlay.h): whether overlay_init has run,
// the window the menu is over, null in tests, which draw nothing, and the menu opened or closed.
#pragma once

struct SDL_Window;

bool overlay_ready();
SDL_Window* overlay_window();
void overlay_set_open(bool on);
