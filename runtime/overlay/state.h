// What overlay.cpp has for the overlay's other files (overlay.h): whether overlay_init has run,
// the window the menu is over, null in tests, which draw nothing, and the menu opened or closed.
#pragma once

struct SDL_Window;

bool overlay_ready();
SDL_Window* overlay_window();
void overlay_set_open(bool on);
// For the menu and the notices: a Dear ImGui context over `window`, made current, with the GL
// backend where `gl` says the window has a GL context and none otherwise, so it's laid out and
// never drawn. It writes no file, opens no link, offers no item picker and reads no gamepad
// itself. The menu and the notices each have one, so a frame of notices with the menu closed
// leaves the menu's focus as it was. Its style is the theme's at the window's height.
void overlay_imgui_context(SDL_Window* window, bool gl);
#ifdef GCN_OVERLAY_TEST
// The current context's backends, GL's where `gl` says it has one, shut down and the context
// destroyed.
void overlay_imgui_context_end(bool gl);
#endif
// The current context's style built again from the theme if the window's height has changed
// since it was last built, before a frame.
void overlay_style_fit(SDL_Window* window);
