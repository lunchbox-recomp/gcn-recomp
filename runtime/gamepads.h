// Gamepads on the GameCube's four ports.
//
// A gamepad is a controller SDL has a mapping for (SDL_IsGamepad); any other joystick, and
// any device controls.txt names, is controls.txt's (input_map.h). Each gamepad is given a
// port as it connects: the port of its SDL player index if that index is 0 to 3 and the
// port is free (Steam Input's order, or the XInput slot on Windows), else the lowest free
// port. A gamepad connected while no port is free gets none. Its player index is then set
// to the port (0 for port 1), which lights the matching player LED on a controller whose
// lights SDL drives; an XInput controller's ring shows the slot Windows gave it. Setting
// one gamepad's index makes SDL move any other that holds it to a free index, so every
// placed gamepad's is set back to its port after each connection, whatever becomes of
// that connection.
//
// A gamepad pulled out frees its port, and nothing is kept from one launch to the next.
// A port reads as plugged in only while a gamepad is on it, or the keyboard (port 1),
// `--pads` or a port controls.txt names keeps it plugged in (main.cpp).
#pragma once
#include "hw/pad.h"

union SDL_Event;

// Controller mappings in SDL_GameControllerDB's format (one `<guid>,<name>,<mapping>,
// platform:<platform>,` line a controller, as github.com/mdqinc/SDL_GameControllerDB keeps
// them; SDL skips a line without a platform or for another one), which make gamepads
// of controllers SDL has no mapping of its own for, or change the mapping it has. From
// `path` (--gamepad-mappings=<file>), or else from gamecontrollerdb.txt beside the
// executable (SDL_GetBasePath: Contents/Resources in a macOS app bundle) if there is
// one. After SDL_Init and before gamepads_init, so that the controllers mapped are
// gamepads from the start.
void gamepads_load_mappings(const char* path);
// After SDL_Init with SDL_INIT_GAMEPAD. Takes the gamepads already connected, in SDL's
// order.
void gamepads_init();
// Gamepads coming and going. Every SDL event may be passed.
void gamepads_event(const SDL_Event& e);
// The gamepad on `port` (0 is port 1) merged into `p`: buttons are added, and a stick or
// trigger is taken over only while it is pushed. `p.connected` is set while a gamepad is
// on the port.
void gamepads_merge(int port, PadState& p);
// The pad's rumble motor on the gamepad on `port`, renewed while it runs.
void gamepads_rumble(int port, bool motor);
// Stops any rumble and closes the gamepads.
void gamepads_shutdown();
