// What a player sees for a gamepad's button or axis: the label printed on it where the pad's
// family has one, as `LB`, `L1` or `ZL`, and the label SDL gives a face button
// (SDL_GetGamepadButtonLabel), as `A` or `Cross`. Text only, in ASCII, which the overlay's font has.
//
// This file needs no SDL. The caller asks SDL for the gamepad's type and a face's label, and
// pad_glyph turns them into the name.
#pragma once
#include "pad_bindings.h"
#include <string>

enum class PadFamily { other, xbox, playstation, nintendo, gamecube };

// The family of a gamepad of SDL's type `sdl_type` (an SDL_GamepadType value).
PadFamily pad_family(int sdl_type);

// The name of `s` on a gamepad of family `f`. `face_label` is the SDL_GamepadButtonLabel value
// SDL gives `s` when it's a face button (south, east, west or north), 0 for none, in which case
// the face is named by where it is. A whole stick axis is named by its stick alone.
std::string pad_glyph(PadFamily f, const PadSource& s, int face_label);
