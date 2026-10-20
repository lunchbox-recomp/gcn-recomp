// Gamepads on the GameCube's four ports.
//
// A gamepad is a controller SDL has a mapping for (SDL_IsGamepad); any other joystick, and
// any device controls.txt names, is controls.txt's (input_map.h). Each gamepad is given a
// port as it connects: the port it had before if that is free, or else the first free
// one, so that gamepads fill ports 1 to 4 in the order they arrive. Its SDL player index is
// set to the port (0 for port 1), which lights the matching player LED on a controller that
// has them.
//
// The ports are saved with each gamepad's SDL GUID and serial number, one line a port
// (`<port> <guid> <serial or ->`), in the file gamepads_init is given: saves/gamepads.txt
// on the desktop. At the next launch the same controller goes back to the same port, and
// those already connected at startup are placed by the file before the rest take what is
// left. A GUID that changed with the same serial, vendor and product (a controller moved
// from USB to Bluetooth) still counts as the same controller. Two controllers of one model
// that report no serial number share a GUID, so only the order they arrive in tells them
// apart.
//
// A gamepad pulled out mid-game leaves its port plugged in, at rest, and the port is held
// for it until it is plugged back in, when it goes back there. A game sees a controller
// that vanishes as one pulled out, and one that comes back on another port as another
// player. Another gamepad takes a held port only when the other ports all have one: a
// player who has picked up a different controller. One more than that is left out.
//
// The design, a port per SDL player index saved by GUID and serial, is that of aurora's
// lib/gamepad.cpp (github.com/encounter/aurora, MIT); this is written afresh for the
// runtime.
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
// After SDL_Init with SDL_INIT_GAMEPAD. Reads the saved ports from `save_path` and writes
// them back there as they change (null: none are kept), and takes the gamepads already
// connected.
void gamepads_init(const char* save_path);
// Gamepads coming and going. Every SDL event may be passed.
void gamepads_event(const SDL_Event& e);
// The gamepad on `port` (0 is port 1) merged into `p`: buttons are added, and a stick or
// trigger is taken over only while it is pushed. `p.connected` is set while a gamepad is
// on the port or the port is held for one.
void gamepads_merge(int port, PadState& p);
// The pad's rumble motor on the gamepad on `port`, renewed while it runs.
void gamepads_rumble(int port, bool motor);
// Stops any rumble and closes the gamepads.
void gamepads_shutdown();
