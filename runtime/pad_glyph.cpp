// See pad_glyph.h.
#include "pad_glyph.h"

PadFamily pad_family(int sdl_type) {
    // SDL_GamepadType's values: 2 and 3 Xbox 360 and One, 4 to 6 PS3 to PS5, 7 to 10 the Switch
    // Pro and Joy-Cons, 11 GameCube (pad_numbers.cpp checks them).
    if (sdl_type == 2 || sdl_type == 3) return PadFamily::xbox;
    if (sdl_type >= 4 && sdl_type <= 6) return PadFamily::playstation;
    if (sdl_type >= 7 && sdl_type <= 10) return PadFamily::nintendo;
    if (sdl_type == 11) return PadFamily::gamecube;
    return PadFamily::other;
}

namespace {

// For each family, other, xbox, playstation, nintendo and gamecube, in that order.
using Names = const char* const[5];

const char* pick(PadFamily f, Names& names) { return names[(int)f]; }

std::string button(PadFamily f, int b, int face_label) {
    static const char* const kLabels[] = {nullptr, "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle"};
    static const char* const kFaces[] = {"South button", "East button", "West button", "North button"};
    static Names kBack = {"Back", "View", "Share", "-", "Back"};
    static Names kGuide = {"Guide", "Xbox", "PS", "Home", "Home"};
    static Names kStart = {"Start", "Menu", "Options", "+", "Start"};
    static Names kLeftStick = {"Left stick click", "LS", "L3", "Left stick click", "Left stick click"};
    static Names kRightStick = {"Right stick click", "RS", "R3", "Right stick click", "Right stick click"};
    static Names kLeftShoulder = {"Left shoulder", "LB", "L1", "L", "ZL"};
    static Names kRightShoulder = {"Right shoulder", "RB", "R1", "R", "Z"};
    static Names kMisc1 = {"Misc 1", "Share", "Mute", "Capture", "Misc 1"};
    switch (b) {
    case kPadSouth: case kPadEast: case kPadWest: case kPadNorth:
        return face_label > 0 && face_label < (int)std::size(kLabels) ? kLabels[face_label] : kFaces[b];
    case kPadBack: return pick(f, kBack);
    case kPadGuide: return pick(f, kGuide);
    case kPadStart: return pick(f, kStart);
    case kPadLeftStick: return pick(f, kLeftStick);
    case kPadRightStick: return pick(f, kRightStick);
    case kPadLeftShoulder: return pick(f, kLeftShoulder);
    case kPadRightShoulder: return pick(f, kRightShoulder);
    case kPadUp: return "D-pad up";
    case kPadDown: return "D-pad down";
    case kPadLeft: return "D-pad left";
    case kPadRight: return "D-pad right";
    case 15: return pick(f, kMisc1);
    case 16: return "Right paddle 1";
    case 17: return "Left paddle 1";
    case 18: return "Right paddle 2";
    case 19: return "Left paddle 2";
    case 20: return "Touchpad";
    default: return "Misc " + std::to_string(b - 19);  // misc2 to misc6, SDL's 21 to 25
    }
}

}  // namespace

std::string pad_glyph(PadFamily f, const PadSource& s, int face_label) {
    if (!s.axis) return button(f, s.index, face_label);
    static Names kLeftTrigger = {"Left trigger", "LT", "L2", "ZL", "L"};
    static Names kRightTrigger = {"Right trigger", "RT", "R2", "ZR", "R"};
    if (s.index == kPadLeftTrigger) return pick(f, kLeftTrigger);
    if (s.index == kPadRightTrigger) return pick(f, kRightTrigger);
    const bool left = s.index == kPadLeftX || s.index == kPadLeftY;
    std::string name = left ? "Left stick" : "Right stick";
    if (f == PadFamily::gamecube) name = left ? "Control stick" : "C-stick";
    if (s.half == 0) return name;
    const bool x = s.index == kPadLeftX || s.index == kPadRightX;
    // SDL's y is down from the centre.
    return name + (x ? (s.half > 0 ? " right" : " left") : (s.half > 0 ? " down" : " up"));
}
