// Gamepads on the GameCube's four ports.
//
// A gamepad is a controller SDL has a mapping for (SDL_IsGamepad). Any other joystick, and
// any device controls.txt names, is controls.txt's (input_map.h).
//
// Each gamepad is given a port as it connects. One the saved ports name (port_memory.h)
// takes its saved port if that's free. Any other takes the port of its SDL player index if
// that index is 0 to 3 and the port is free (Steam Input's order, or the XInput slot on
// Windows), else the lowest free port.
//
// A gamepad connected while no port is free gets none. Its player index is then set to the
// port (0 for port 1), which lights the matching player LED on a controller whose lights
// SDL drives. An XInput controller's ring shows the slot Windows gave it.
//
// Setting one gamepad's index makes SDL move any other that holds it to a free index. So
// every placed gamepad's is set back to its port after each connection, whatever becomes
// of that connection.
//
// A gamepad pulled out frees its port, and the saved ports keep what they said. A saved
// port is only a preference. A gamepad that finds its port taken goes to the lowest free
// port, and none already on a port is moved.
//
// A port reads as plugged in only while a gamepad is on it. The keyboard (port 1), `--pads`
// or a port controls.txt names keeps it plugged in (main.cpp).
//
// A port a controls.txt line names with `port <n>`, `port 1` too, is pinned to the device
// on that line, and no gamepad takes it (input_map.h). That beats a saved port.
//
// The GameCube's A, B, X and Y are the face buttons labeled A, B, X and Y, as Nintendo's own
// GameCube games have them on its consoles: east A and south B on a Switch Pro, south A and
// east B on an Xbox pad. The labels are the ones SDL reports for the pad's type
// (SDL_GetGamepadButtonLabel), not what's printed on it: a pad that passes for another, as an
// 8BitDo does in its X-input mode as an Xbox 360 pad, gets that pad's. A gamepad whose faces
// carry no letters (PlayStation's symbols) has them where an Xbox pad does. Which is which
// is worked out as the gamepad lands on its port. Z is the right shoulder, and L and R are
// the triggers.
//
// What each gamepad did goes to stderr as `gamepads: ` lines. After `"<name>" on port N`,
// what SDL knows of it: `port N is <type>, <wired|wireless|unknown>, <vendor>:<product>,
// rumble <yes|no>, south label <label>, east label <label>`. After `"<name>" pulled out of
// port N`, and at gamepads_shutdown for each gamepad still on a port, what it sent while
// there: `port N sent <button> <presses>, ..., left stick x <lowest> to <highest> y ...,
// right stick x ... y ..., triggers left ... right ...`, naming only the buttons pressed,
// raw values before any dead zone, or `port N was never read` for one that left before a
// frame read it. Then `port N rumble on <ons> times, <failed> of <calls> calls failed`
// (`time` and `call` for 1).
//
// A gamepad SDL says has no rumble (SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, asked at each call)
// is never asked for one, so its line counts the game's ons and no calls. One SDL types as
// GameCube is asked anyway, since SDL never updates what it says of a pad on Nintendo's
// adapter that landed before the adapter's second cable was in. A rumble that fails says
// why once while the gamepad is on its port, each of SDL's reasons once, as `port N rumble
// failed: <SDL's error>`, and the rumble line counts every failed call. So a GameCube pad
// with no rumble, such as a WaveBird or a PowerA-style input-only pad, says it once. On a
// pad SDL drives through HIDAPI a call SDL accepts has only been queued, and a write that
// fails after that isn't reported, so `0 of N calls failed` doesn't mean the pad shook.
#pragma once
#include "hw/pad.h"
#include <array>

union SDL_Event;
struct SDL_Gamepad;

// Controller mappings in SDL_GameControllerDB's format (one `<guid>,<name>,<mapping>,
// platform:<platform>,` line a controller, as github.com/mdqinc/SDL_GameControllerDB keeps
// them; SDL skips a line without a platform or for another one) from gamecontrollerdb.txt
// beside the executable (SDL_GetBasePath: Contents/Resources in a macOS app bundle), if
// there is one. They make gamepads of controllers SDL has no mapping of its own for, or
// change the mapping it has. A file elsewhere is read by SDL itself, from the path in the
// SDL_GAMECONTROLLERCONFIG_FILE environment variable. After SDL_Init and before
// gamepads_init, so that the controllers mapped are gamepads from the start.
void gamepads_load_mappings();
// Where the saved ports are kept (port_memory.h), or nowhere for null or empty, which is
// how a program starts. The file is read when a gamepad first needs a port and written when
// one lands on a port the file doesn't have it on, so a launch with no gamepad touches
// nothing. A file that exists and can't be read is never written over. Call it before
// gamepads_init.
void gamepads_remember(const char* path);
// After SDL_Init with SDL_INIT_GAMEPAD. Takes the gamepads already connected. Saved ports
// go to their gamepads first, and the others follow in SDL's order.
void gamepads_init();
// Gamepads coming and going. Every SDL event may be passed.
void gamepads_event(const SDL_Event& e);
// The gamepad on `port` (0 is port 1) merged into `p`: buttons are added, and a stick or
// trigger is taken over only while it is pushed. `p.connected` is set while a gamepad is
// on the port. Each of SDL's buttons and axes is also counted here, for the line said when
// the gamepad leaves.
void gamepads_merge(int port, PadState& p);
// How far a trigger's axis (0 to 32767) goes before gamepads_merge takes it for the GameCube's
// digital L or R, the click at the bottom of its travel. The overlay's chord takes them the same
// way on a gamepad SDL maps no click buttons on (overlay.h).
constexpr int kGamepadTriggerClick = 30000;
// The pad's rumble motor on the gamepad on `port`, renewed while it runs.
void gamepads_rumble(int port, bool motor);
// Stops any rumble, says what each gamepad on a port sent, and closes the gamepads.
void gamepads_shutdown();
// The SDL buttons (SDL_GamepadButton's values) that are the GameCube's A, B, X and Y on the
// open gamepad `g`, by its labels as above, the ones gamepads_merge reads. The overlay's menu
// takes a gamepad's faces from here too, on a port or not (overlay.h).
std::array<int, 4> gamepads_faces(SDL_Gamepad* g);
