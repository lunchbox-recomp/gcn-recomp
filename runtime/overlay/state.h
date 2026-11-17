// What the overlay's files have for each other, behind overlay.h.
#pragma once

#include <cstdint>

struct OverlayPicture;
struct SDL_Gamepad;
struct SDL_Window;
union SDL_Event;

// overlay.cpp: whether overlay_init has run, the window the menu is over, null in tests, which
// draw nothing, and the menu opened or closed.
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

// The menu (overlay_menu.cpp): set up with its title on its first page, how many pages a game
// added, the menu opened on a page (-1 for where it was) at a time, and its frame.
void menu_init(const char* title);
int menu_added_pages();
void menu_opened(int page, uint64_t now);
void menu_frame();
#ifdef GCN_OVERLAY_TEST
void menu_clear_pages();
#endif

// The notices (notices.cpp): their own context over `window`, made before the menu's, with GL
// where `gl` says.
void notices_init(SDL_Window* window, bool gl);
void notices_motion(bool on);
// Whether any notice is rising or fading at `now`, or moving down as the stack reflows.
bool notices_moving(uint64_t now);
// After the menu, if one is showing: the pills over the whole window.
void notices_frame();
#ifdef GCN_OVERLAY_TEST
// Their context shut down and destroyed.
void notices_test_end();
#endif

// The Buttons view (overlay_bindings.cpp): the view and any press-to-bind ended with nothing
// changed, whether it went back to the page since the last call, the gamepad press-to-bind
// listens to (null while it isn't listening), its gamepad read once a host frame, an event it
// takes (true if it took it), and the view.
void bindings_stop();
bool bindings_back();
SDL_Gamepad* bindings_pad();
void bindings_poll();
bool bindings_event(const SDL_Event& e);
void bindings_page();

// The Controllers page (overlay_controllers.cpp): the press-A order ended, an event it takes
// (true if it took it), and the page.
void controllers_stop();
bool controllers_event(const SDL_Event& e);
void controllers_page();

// The Display page (overlay_display.cpp): the picture it shows and changes, and the page.
void display_init(const OverlayPicture& picture);
void display_page(SDL_Window* window);

// The input (overlay_input.cpp): the gamepads held for the menu, and the buttons each has down
// kept out of the menu until each is let go; the held gamepads read for the menu's frame; and
// the gamepad whose glyphs the footer draws, null for keycaps.
void overlay_hold_pads();
void overlay_latch_pads();
void overlay_read_pads();
SDL_Gamepad* overlay_last_pad();
