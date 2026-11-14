// The in-game overlay: a menu drawn over the game with Dear ImGui (github.com/ocornut/imgui),
// in the desktop frontend (main.cpp) when the build has GCN_OVERLAY on. Its Display page sets
// the picture, and its Controllers page which controller is which player.
//
// F1 or a gamepad's Back opens it, and the guest is paused (pause.h) at its next frame
// boundary. F1, Back or B closes it and the guest goes on. Escape closes it too, and the
// frontend quits on an Escape while it's closed. A gamepad SDL maps no Back on opens and closes
// it with Guide (Home) instead, as a Switch 2 GameCube controller or a single Switch 2 Joy-Con,
// and that GameCube controller's ZL, which the GameCube's own pad lacks, does too. A gamepad
// with neither Back nor Guide, as SDL maps a GameCube controller on Nintendo's adapter, opens
// and closes it with Z and both triggers clicked in, held for a second with nothing else, once
// a hold (overlay_poll). The clicks are SDL's MISC3 and MISC4 where SDL maps them, and where it
// doesn't the triggers past the gamepad's trigger click (PadBinding::trigger_click, which the
// Buttons view sets), as gamepads_merge takes them. The menu takes the triggers' clicks for its
// page turns the same way. A device
// controls.txt binds is the file's alone (input_map.h), so the menu never reads it.
//
// The panel is the theme's share of the window's width (theme.h) and as tall as its page, but
// never shorter than a page has had it since the menu opened, centred: a page no taller than one
// shown since leaves the header where it was, and a taller one grows the panel each way. A view
// or a question taller than that grows it too, and backing out puts it back. Its
// header has the menu's title on the left and a pill for each page centred between the
// shoulders' names, the page showing on the accent. A click on a pill shows its page, and so
// does a shoulder: L1, L2 or Q goes back a page and R1, R2 or E on one, no further than the first
// or the last. L2 and R2 are the triggers pressed in past their click, so a GameCube pad turns
// pages with its L and R, and with its Z, SDL's R1. On a gamepad with only the GameCube's
// buttons, where those three are the chord, each turns a page as it's let go, and only if it
// went down and up alone, so the chord turns none and closes the menu on the page it showed.
// Every other gamepad turns a page as the button goes down. The shoulders and keys move nothing
// while the menu asks, while the press-A order or the Buttons view is up, or while an item is
// active, as a slider whose L1 and R1 step it is, and a shoulder held as the menu opens moves
// nothing until it's let go. After a change the page's first item has the focus.
//
// A gamepad's A presses what's focused and its B goes back, and they're the faces labeled A and
// B, the game's own A and B (gamepads_faces in gamepads.h). So a Switch Pro's east button
// presses and its south one goes back, the other way round from an Xbox pad's, and with both
// in each press goes by its own pad. X and Y are the faces labeled so too, and a pad whose
// faces carry no letters has all four by Xbox position.
//
// The footer, below every page, has hints on the left, each a key's name and what it does, only
// the way out while the Buttons view listens for a press to bind, and
// Restart game and Quit game, small, on the right. Each asks first, in a card centred in place of
// the page with its confirm on the theme's danger, with Cancel focused, and Cancel, B or Escape
// goes back to the page with nothing done. Quit closes the overlay and quits the way an Escape
// with it closed does, and Restart is frontend_restart (frontend.h). A replay has no Restart
// game, since its log can't hold one (input_log.h).
//
// While it's open nothing reaches the game. Each port keeps what it had as it opened until the
// guest is held, so the game sees nothing let go before it stops. Then each port stays plugged
// in or not, at rest, and a button still held as it closes stays out of the game until it's
// let go, so the press that closed it isn't the game's as well. A trigger in as it closes keeps
// its travel out until it's back at rest, though its click lets go on the way. Any gamepad, the
// keyboard or the mouse drives the menu. A key or a gamepad's button held as it opens stays out
// of the menu the same way, so it presses nothing there, and a held key's repeats never reach
// the menu. So does a button held on a gamepad plugged in while it's open, and a gamepad coming
// or going leaves what the others hold as it was.
//
// It moves on the host clock (appear.h, motion.h) with menu_motion on in settings.txt, the
// default. As it opens, the pause is asked for at once while a backdrop dims the game and the
// panel fades in and grows, and an `[overlay] shown after N ms` line says when the panel is
// whole. As it closes, the guest goes on at once while the menu fades out and shrinks over the
// game with no input of its own, still showing what it showed, Quit? or the Buttons view, say,
// which ends once it's gone. Then nothing more is laid out until it opens again. A page change
// slides the pill to the new page and brings its contents in from that side (tabs.h). Its times
// and sizes are the theme's (theme.h). With menu_motion off, and under --dump-output, each is
// whole at once.
//
// The Display page sets the aspect (4:3, 16:9, 16:10, 21:9 or the window's own shape),
// fullscreen and the internal scale. A change applies at once, and settings.txt (settings.h)
// gets the key that changed, so a flag that set another key for this run isn't written back.
// With the aspect held at 4:3, as --eye and GCN_THEATER_STEREO hold it, its choices are shown
// disabled with a note saying so.
//
// The Controllers page, after Display, lists each player's controllers with a light that's lit
// while one is pressed or pushed, and the controllers on no player (gamepads_list in
// gamepads.h). Each player has Swap with the next one, which takes the keyboard along and is off
// while neither has a gamepad, and Remove, off while it has none. A port controls.txt pins says
// so and has neither. Set player order asks for player 1's controller, then player 2's
// and so on past any pinned port: the first device whose A goes down (the face labeled A, as
// the menu takes it, or X, the keyboard's A) takes that player, and one already placed is
// passed over. It ends once every gamepad has been placed or no port is left, and Back, B or
// Escape ends it sooner. Each press moves its device at once (gamepads_assign), and the page
// then has nothing to focus, so no press reaches a button.
//
// Change buttons, right of Set player order and off while there's no gamepad, puts up the
// Buttons view (overlay_bindings.cpp) in place of a page, on the first gamepad listed. It
// picks a gamepad, on a player or not, and lists the ones controls.txt binds as locked. Each
// control's button, named as the gamepad labels it (pad_glyph.h), starts press-to-bind
// (pad_bindings.h), which takes the next press on that gamepad within 5 s. Its prompt has
// nothing to focus. Escape, or Back or Guide on any gamepad, cancels it, F1 closes the menu, and
// every other key and gamepad event while it waits is its own. It ends saying what changed. The
// view also sets the dead zones and the trigger click, saved as each slider is let go, and has
// Test rumble (1 s), Reset to default and Done. B or Escape goes back to the Controllers page.
// The menu's own A and B stay the faces labeled so, whatever the game's binding says.
//
// A notice over the bottom of the window says for 2 s of host time that a controller
// connected, disconnected or moved, and logs the same as a `[pads]` line. It shows with the
// menu closed too, never pauses the game and takes no input. Frame and output dumps never
// include it.
//
// GCN_OVERLAY_AT=<frame>:<host frames> opens it once <frame> frames have been presented and
// closes it after it's been drawn over the held frame <host frames> times, for a run nobody is
// at. GCN_OVERLAY_PAGE=<n> opens it on page n, 0 for Display, 1 for Controllers and 2 on for the
// pages added. With --dump-output the first draw over the held frame that looks as the two
// before it did goes to overlay_NNNNN.png in the dump directory too, or overlay_page<n>.png with
// GCN_OVERLAY_PAGE, drawn again into a target of the window's size as output dumps are, and over
// plain grey instead of the held frame with GCN_OVERLAY_PLAIN set.
#pragma once
#include "hw/pad.h"
#include "overlay/theme.h"
#include "pad_bindings.h"
#include "settings.h"
#include <string>
#include <vector>

struct SDL_Window;
union SDL_Event;

// The picture the Display page shows and changes.
struct OverlayPicture {
    Settings now;      // the picture as it is, flags included
    Settings saved;    // what settings.txt holds
    std::string path;  // settings.txt
    // The frontend's, which applies `now` at once.
    void (*apply)(const Settings& now) = nullptr;
    // The aspect stays 4:3 whatever `now` says, as --eye and GCN_THEATER_STEREO keep it, so the
    // page shows its choices but none can be picked.
    bool aspect_held = false;
};

// What the menu is asking before it does it, if anything.
enum class OverlayAsk { none, restart, quit };

#ifdef GCN_OVERLAY
// Add a page after Display and Controllers before overlay_init. The callback draws that page
// with ImGui. A page added after overlay_init, with an empty or taken title or with no callback,
// prints one line on stderr and is refused. A slider's value is drawn on its grab, the accent,
// at 1.57:1, so a page's slider has an empty format and its value as text beside it, as the
// Buttons view's do.
void overlay_add_page(const char* title, void (*draw)(void* user), void* user);
// Once the window's GL context is current, with the menu's title. A null window draws no menu,
// for tests: the overlay still opens, closes and holds input back. A window with no GL
// context, which tests make with SDL's dummy video driver, has a menu that's driven but
// never drawn (overlay_menu_frame).
// The menu asks the OS to open no link or file. Its duplicate-ID tooltip offers no item picker.
void overlay_init(SDL_Window* window, const char* title, const OverlayPicture& picture);
#ifdef GCN_OVERLAY_TEST
// Clear prior registrations so another test can register before its overlay_init.
void overlay_test_reset_pages();
// The menu's and the notices' Dear ImGui contexts shut down with their backends and destroyed,
// with the window's GL context still current, so a test can init again without leaking them.
void overlay_test_end();
// The notices' context, null with none.
struct ImGuiContext* overlay_test_notices_context();
// Every page's title in the header's order, and the page `page` shown as a click on its pill
// shows it.
const std::vector<std::string>& overlay_test_pages();
void overlay_test_pick_page(int page);
// Whether the menu's last frame showed a page, not a question or a view in its place.
bool overlay_test_page_shown();
// The page showing, 0 for Display, 1 for Controllers and 2 on for the pages added.
int overlay_page();
#endif
// Each of the window's events, before the frontend's own. True for one the overlay takes, which
// nothing else should see: F1, Back (Guide on a gamepad with no Back, and ZL on a Switch 2
// GameCube controller), B or Escape opening or closing it or going back from a question, and
// every key, mouse and gamepad button or axis event while it's open.
bool overlay_event(const SDL_Event& e);
// Once a host frame, after the window's events: the GameCube chord opens or closes it once it's
// been held its second, which no event marks.
void overlay_poll();
bool overlay_open();
// Whether overlay_draw lays out the menu this time: it's open or still fading out, or
// GCN_OVERLAY_AT's frame has been presented, so that overlay_draw opens it first.
bool overlay_menu_due();
// Whether the menu is still opening or closing, so the frontend draws a held frame again sooner.
bool overlay_animating();
// Closes it if it's open, and the guest goes on.
void overlay_close();
// What the menu is asking, none when it isn't. For the tests (tools/test_ports.cpp).
OverlayAsk overlay_asking();
// Port `port`'s state (0 is port 1) as the game gets it, after everything that drives the port
// is merged into `p`.
void overlay_gate(int port, PadState& p);
// Draws the menu over the window's framebuffer while it's open, after the frame and before the
// swap.
void overlay_draw();
// What overlay_draw does short of drawing, for a window with no GL context: while it's open, the
// menu takes its input and lays out its frame. With no window, there's no ImGui frame.
void overlay_menu_frame();
// What the Display page does with `next`, the picture with its change made: the frontend applies
// it, and each key where it differs from the picture now is saved.
void overlay_display_change(const Settings& next);
// A notice about a controller (notices.cpp), and its `[pads]` line.
void overlay_pads_notice(const std::string& text);
// The notices still showing, oldest first, and how many frames have shown them. For the tests.
std::vector<std::string> overlay_notices();
long overlay_notice_frames();
// How many frames have laid out the menu. For the tests.
long overlay_menu_frames();
// What the Controllers page's Set player order does. For the tests.
void overlay_controllers_order();
// The port the press-A order asks for next (0 is port 1), or -1 when it isn't asking.
int overlay_controllers_asking();
// The Buttons view (overlay_bindings.cpp) up, showing gamepad `device`, as Change buttons puts
// it. False for a device gamepads_binding refuses. For the tests too.
bool overlay_bindings_open(uint32_t device);
bool overlay_bindings_shown();
// Press-to-bind for `c` on the gamepad the view shows. False if the view isn't up or its gamepad
// is gone. For the tests.
bool overlay_bindings_listen(PadControl c);
bool overlay_bindings_listening();
// What the view says the last change did. For the tests.
const std::string& overlay_bindings_said();
#else
inline void overlay_pads_notice(const std::string&) {}
inline void overlay_add_page(const char*, void (*)(void*), void*) {}
inline void overlay_init(SDL_Window*, const char*, const OverlayPicture&) {}
inline bool overlay_event(const SDL_Event&) { return false; }
inline void overlay_poll() {}
inline bool overlay_open() { return false; }
inline bool overlay_menu_due() { return false; }
inline bool overlay_animating() { return false; }
inline void overlay_close() {}
inline void overlay_gate(int, PadState&) {}
inline void overlay_draw() {}
#endif
