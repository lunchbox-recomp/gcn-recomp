// What the overlay's input (overlay_input.cpp) has for its other files.
#pragma once

struct SDL_Gamepad;

// The gamepads held for the menu, and the buttons each has down kept out of the menu until each
// is let go.
void overlay_hold_pads();
void overlay_latch_pads();
// overlay_menu.cpp's: the held gamepads read for the menu's frame.
void overlay_read_pads();
// footer.cpp's: the gamepad whose glyphs the footer draws, null for keycaps.
SDL_Gamepad* overlay_last_pad();
