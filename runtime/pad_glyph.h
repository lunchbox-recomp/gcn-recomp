// What a player sees for a gamepad's button or axis: the label printed on it where the pad's
// family has one, as `LB`, `L1` or `ZL`, and the label SDL gives a face button
// (SDL_GetGamepadButtonLabel), as `A` or `Cross`. Text only, in ASCII, which the overlay's font has.
//
// This file needs no SDL. The caller asks SDL for the gamepad's type and a face's label, and
// pad_glyph turns them into the name.
#pragma once
#include "pad_bindings.h"
#include <string>

enum class PadFamily {
    other, xbox360, xbox, ps3, ps4, ps5, nintendo, joycon_left, joycon_right, joycon_left_upright,
    joycon_right_upright, gamecube, steam, steam_grips
};

// The family of a gamepad of SDL's type `sdl_type` (an SDL_GamepadType value). Two types hold
// two families each, told apart by what the gamepad's mapping has: `trigger`, either trigger
// axis, and `misc1`, the misc1 button. A Joy-Con alone held upright, as SDL sets it up under
// SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, sends ZL or ZR as a trigger, and one held
// sideways has no trigger. A Steam Deck or a later Steam Controller has misc1, its quick access
// button, and the first Steam Controller has none and sends its back grips as paddles.
PadFamily pad_family(int sdl_type, bool trigger, bool misc1);

// The name of `s` on a gamepad of family `f`. `face_label` is the SDL_GamepadButtonLabel value
// SDL gives `s` when it's a face button (south, east, west or north), 0 for none, in which case
// the face is named by where it is. A whole stick axis is named by its stick alone.
std::string pad_glyph(PadFamily f, const PadSource& s, int face_label);

// What the Controllers page calls a gamepad of SDL's type `sdl_type`: "Xbox One", "Joy-Con (L)",
// or "gamepad" for a type with no name of its own.
const char* pad_type_name(int sdl_type);
