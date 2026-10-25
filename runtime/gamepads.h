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
#pragma once
#include "hw/pad.h"

union SDL_Event;

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
// nothing. Call it before gamepads_init.
void gamepads_remember(const char* path);
// After SDL_Init with SDL_INIT_GAMEPAD. Takes the gamepads already connected. Saved ports
// go to their gamepads first, and the others follow in SDL's order.
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
