// The in-game overlay: a menu drawn over the game with Dear ImGui (github.com/ocornut/imgui),
// in the desktop frontend (main.cpp) when the build has GCN_OVERLAY on. Its Display page sets
// the picture.
//
// F1 or a gamepad's Back opens it, and the guest is paused (pause.h) at its next frame
// boundary. F1, Back or B closes it and the guest goes on. Escape closes it too, and the
// frontend quits on an Escape while it's closed.
//
// A gamepad's A presses what's focused and its B goes back, and they're the faces labeled A and
// B, the game's own A and B (gamepads_faces in gamepads.h). So a Switch Pro's east button
// presses and its south one goes back, the other way round from an Xbox pad's, and with both
// in each press goes by its own pad. X and Y are the faces labeled so too, and a pad whose
// faces carry no letters has all four by Xbox position.
//
// Below the pages, on every one of them, are Restart game and Quit game. Each asks first, with
// Cancel focused, and Cancel, B or Escape goes back to the page with nothing done. Quit closes
// the overlay and quits the way an Escape with it closed does, and Restart is frontend_restart.
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
// at. With --dump-output the first draw over the held frame that shows the menu goes to
// overlay_NNNNN.png in the dump directory too, drawn again into a target of the window's size as
// output dumps are.
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

// What the menu is asking before it does it, if anything.
enum class OverlayAsk { none, restart, quit };

// The frontend's (main.cpp), which the menu's Restart calls, as a game's own page may: the overlay
// closes and the guest goes on, the run ends as a quit ends it, memory card and all, and the game
// starts again as it does when it resets the console. In a replay that ends the run, as there.
void frontend_restart();

#ifdef GCN_OVERLAY
// Once the window's GL context is current, with the menu's title. A null window draws no menu,
// for tests: the overlay still opens, closes and holds input back. A window with no GL context,
// which tests make with SDL's dummy video driver, has a menu that's driven but never drawn
// (overlay_menu_frame).
void overlay_init(SDL_Window* window, const char* title, const OverlayPicture& picture);
// Each of the window's events, before the frontend's own. True for one the overlay takes, which
// nothing else should see: F1, Back, B or Escape opening or closing it or going back from a
// question, and every key, mouse and gamepad button or axis event while it's open.
bool overlay_event(const SDL_Event& e);
bool overlay_open();
// Closes it if it's open, and the guest goes on.
void overlay_close();
// What the menu is asking, none when it isn't.
OverlayAsk overlay_asking();
// Port `port`'s state (0 is port 1) as the game gets it, after everything that drives the port
// is merged into `p`.
void overlay_gate(int port, PadState& p);
// Draws the menu over the window's framebuffer while it's open, after the frame and before the
// swap.
void overlay_draw();
// What overlay_draw does short of drawing, for a window with no GL context: while it's open, the
// menu takes its input and lays out its frame.
void overlay_menu_frame();
// What the Display page does with `next`, the picture with its change made: the frontend applies
// it, and each key where it differs from the picture now is saved.
void overlay_display_change(const Settings& next);
#else
inline void overlay_init(SDL_Window*, const char*, const OverlayPicture&) {}
inline bool overlay_event(const SDL_Event&) { return false; }
inline bool overlay_open() { return false; }
inline void overlay_close() {}
inline void overlay_gate(int, PadState&) {}
inline void overlay_draw() {}
#endif
