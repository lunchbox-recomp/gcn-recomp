// The in-game overlay: a menu drawn over the game with Dear ImGui (github.com/ocornut/imgui),
// in the desktop frontend (main.cpp) when the build has GCN_OVERLAY on. Its Display page sets
// the picture.
//
// F1 or a gamepad's Back opens it, and the guest is paused (pause.h) at its next frame
// boundary. F1, Back or B closes it and the guest goes on. B is the right face button, which
// Dear ImGui's menus take as cancel. Escape closes it too, and the frontend quits on an Escape
// while it's closed.
//
// While it's open nothing reaches the game. Each port stays plugged in or not, at rest, and a
// button still held as it closes stays out of the game until it's let go, so the press that
// closed it isn't the game's as well. Any gamepad, the keyboard or the mouse drives the menu.
//
// The Display page sets the aspect (4:3, 16:9, 16:10, 21:9 or the window's own shape),
// fullscreen and the internal scale. A change applies at once, and settings.txt (settings.h)
// gets the key that changed, so a flag that set another key for this run isn't written back.
//
// GCN_OVERLAY_AT=<frame>:<host frames> opens it once <frame> frames have been presented and
// closes it after it's been drawn over the held frame <host frames> times, for a run nobody is
// at. With --dump-output its first draw over the held frame goes to overlay_NNNNN.png in the
// dump directory too, drawn again into a target of the window's size as output dumps are.
#pragma once
#include "hw/pad.h"
#include "settings.h"
#include <string>

struct SDL_Window;
union SDL_Event;

// The picture the Display page shows and changes.
struct OverlayPicture {
    Settings now;      // the picture as it is, flags included
    Settings saved;    // what settings.txt holds
    std::string path;  // settings.txt
    // The frontend's, which applies `now` at once.
    void (*apply)(const Settings& now) = nullptr;
};

#ifdef GCN_OVERLAY
// Once the window's GL context is current, with the menu's title. A null window draws no menu,
// for tests: the overlay still opens, closes and holds input back.
void overlay_init(SDL_Window* window, const char* title, const OverlayPicture& picture);
// Each of the window's events, before the frontend's own. True for one the overlay takes, which
// nothing else should see: F1, Back, B or Escape opening or closing it, and every key, mouse and
// gamepad button or axis event while it's open.
bool overlay_event(const SDL_Event& e);
bool overlay_open();
// Port `port`'s state (0 is port 1) as the game gets it, after everything that drives the port
// is merged into `p`.
void overlay_gate(int port, PadState& p);
// Draws the menu over the window's framebuffer while it's open, after the frame and before the
// swap.
void overlay_draw();
// What the Display page does with `next`, the picture with its change made: the frontend applies
// it, and each key where it differs from the picture now is saved.
void overlay_display_change(const Settings& next);
#else
inline void overlay_init(SDL_Window*, const char*, const OverlayPicture&) {}
inline bool overlay_event(const SDL_Event&) { return false; }
inline bool overlay_open() { return false; }
inline void overlay_gate(int, PadState&) {}
inline void overlay_draw() {}
#endif
