// Checks, at compile time, that the numbers pad_bindings.h and pad_glyph.cpp use for a
// gamepad's buttons, axes and types are SDL's. Those files need no SDL, and gamepads.cpp,
// input_map.cpp and the overlay cast between the two, so a new SDL that renumbered any of them
// fails to build here. It holds no code.
#include "pad_bindings.h"
// After pad_bindings.h's standard headers: SDL_endian.h trips over clang's intrinsics otherwise.
#include <SDL3/SDL.h>

static_assert((int)kPadSouth == SDL_GAMEPAD_BUTTON_SOUTH && (int)kPadEast == SDL_GAMEPAD_BUTTON_EAST &&
              (int)kPadWest == SDL_GAMEPAD_BUTTON_WEST && (int)kPadNorth == SDL_GAMEPAD_BUTTON_NORTH &&
              (int)kPadBack == SDL_GAMEPAD_BUTTON_BACK && (int)kPadGuide == SDL_GAMEPAD_BUTTON_GUIDE &&
              (int)kPadStart == SDL_GAMEPAD_BUTTON_START && (int)kPadLeftStick == SDL_GAMEPAD_BUTTON_LEFT_STICK &&
              (int)kPadRightStick == SDL_GAMEPAD_BUTTON_RIGHT_STICK && (int)kPadLeftShoulder == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER &&
              (int)kPadRightShoulder == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER && (int)kPadUp == SDL_GAMEPAD_BUTTON_DPAD_UP &&
              (int)kPadDown == SDL_GAMEPAD_BUTTON_DPAD_DOWN && (int)kPadLeft == SDL_GAMEPAD_BUTTON_DPAD_LEFT &&
              (int)kPadRight == SDL_GAMEPAD_BUTTON_DPAD_RIGHT && (int)kPadButtons == SDL_GAMEPAD_BUTTON_COUNT);
// pad_bindings.cpp names the rest by number, as pad_glyph.cpp does: misc1, the four paddles,
// the touchpad, then misc2 to misc6.
static_assert(SDL_GAMEPAD_BUTTON_MISC1 == 15 && SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1 == 16 &&
              SDL_GAMEPAD_BUTTON_LEFT_PADDLE1 == 17 && SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2 == 18 &&
              SDL_GAMEPAD_BUTTON_LEFT_PADDLE2 == 19 && SDL_GAMEPAD_BUTTON_TOUCHPAD == 20 &&
              SDL_GAMEPAD_BUTTON_MISC2 == 21 && SDL_GAMEPAD_BUTTON_MISC3 == 22 && SDL_GAMEPAD_BUTTON_MISC4 == 23 &&
              SDL_GAMEPAD_BUTTON_MISC5 == 24 && SDL_GAMEPAD_BUTTON_MISC6 == 25);
static_assert((int)kPadLeftX == SDL_GAMEPAD_AXIS_LEFTX && (int)kPadLeftY == SDL_GAMEPAD_AXIS_LEFTY &&
              (int)kPadRightX == SDL_GAMEPAD_AXIS_RIGHTX && (int)kPadRightY == SDL_GAMEPAD_AXIS_RIGHTY &&
              (int)kPadLeftTrigger == SDL_GAMEPAD_AXIS_LEFT_TRIGGER && (int)kPadRightTrigger == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER &&
              (int)kPadAxes == SDL_GAMEPAD_AXIS_COUNT);
// pad_glyph.cpp's pad_family, from SDL_GamepadType.
static_assert(SDL_GAMEPAD_TYPE_UNKNOWN == 0 && SDL_GAMEPAD_TYPE_STANDARD == 1 && SDL_GAMEPAD_TYPE_XBOX360 == 2 &&
              SDL_GAMEPAD_TYPE_XBOXONE == 3 && SDL_GAMEPAD_TYPE_PS3 == 4 && SDL_GAMEPAD_TYPE_PS4 == 5 &&
              SDL_GAMEPAD_TYPE_PS5 == 6 && SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO == 7 &&
              SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT == 8 && SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT == 9 &&
              SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR == 10 && SDL_GAMEPAD_TYPE_GAMECUBE == 11 &&
              SDL_GAMEPAD_TYPE_STEAM == 12 && SDL_GAMEPAD_TYPE_COUNT == 13);
