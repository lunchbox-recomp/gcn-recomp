// See pad_glyph.h. The labels follow SDL 3.4.18's drivers: what each one sends a printed button
// as (SDL_hidapi_*.c, the GameCube adapter's mapping in SDL_gamepad.c) and SDL_gamepad.h's notes
// on misc1 and the paddles.
#include "pad_glyph.h"
#include <initializer_list>

PadFamily pad_family(int sdl_type) {
    // SDL_GamepadType's values (pad_numbers.cpp checks them): 2 and 3 Xbox 360 and One, 4 to 6
    // PS3 to PS5, 7 the Switch Pro, 8 and 9 a Joy-Con alone, 10 a pair of them, 11 GameCube, 12
    // Steam.
    static constexpr PadFamily kByType[] = {
        PadFamily::other, PadFamily::other, PadFamily::xbox360, PadFamily::xbox, PadFamily::ps3,
        PadFamily::ps4, PadFamily::ps5, PadFamily::nintendo, PadFamily::joycon_left, PadFamily::joycon_right,
        PadFamily::nintendo, PadFamily::gamecube, PadFamily::steam};
    return sdl_type >= 0 && sdl_type < (int)std::size(kByType) ? kByType[sdl_type] : PadFamily::other;
}

namespace {

struct Name {
    PadFamily f;
    const char* name;
};

// The name `f` gives a button, from those families that print one, or `plain` for the others.
// The Xbox 360 takes the Xbox One's where it lists none, and the PS3 and PS5 the PS4's.
const char* pick(PadFamily f, std::initializer_list<Name> names, const char* plain) {
    for (int pass = 0; pass < 2; pass++) {
        for (const Name& n : names)
            if (n.f == f) return n.name;
        if (f == PadFamily::xbox360) f = PadFamily::xbox;
        else if (f == PadFamily::ps3 || f == PadFamily::ps5) f = PadFamily::ps4;
        else break;
    }
    return plain;
}

using F = PadFamily;

std::string button(F f, int b, int face_label) {
    static const char* const kLabels[] = {nullptr, "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle"};
    static const char* const kFaces[] = {"South button", "East button", "West button", "North button"};
    switch (b) {
    case kPadSouth: case kPadEast: case kPadWest: case kPadNorth:
        return face_label > 0 && face_label < (int)std::size(kLabels) ? kLabels[face_label] : kFaces[b];
    case kPadBack:
        return pick(f, {{F::xbox360, "Back"}, {F::xbox, "View"}, {F::ps3, "Select"}, {F::ps4, "Share"}, {F::ps5, "Create"},
                        {F::nintendo, "-"}},
                    "Back");
    // A Joy-Con alone sends its - or + as Start, and the left one's Capture as Guide.
    case kPadGuide:
        return pick(f, {{F::xbox, "Xbox"}, {F::ps4, "PS"}, {F::nintendo, "Home"}, {F::joycon_left, "Capture"},
                        {F::joycon_right, "Home"}, {F::gamecube, "Home"}, {F::steam, "Steam"}},
                    "Guide");
    case kPadStart:
        return pick(f, {{F::xbox360, "Start"}, {F::xbox, "Menu"}, {F::ps3, "Start"}, {F::ps4, "Options"},
                        {F::nintendo, "+"}, {F::joycon_left, "-"}, {F::joycon_right, "+"}},
                    "Start");
    case kPadLeftStick:
        return pick(f, {{F::xbox, "LS"}, {F::ps4, "L3"}, {F::joycon_left, "Stick click"}, {F::joycon_right, "Stick click"}},
                    "Left stick click");
    case kPadRightStick: return pick(f, {{F::xbox, "RS"}, {F::ps4, "R3"}}, "Right stick click");
    // A Joy-Con alone, held sideways, sends SL and SR as its shoulders.
    case kPadLeftShoulder:
        return pick(f, {{F::xbox, "LB"}, {F::ps4, "L1"}, {F::nintendo, "L"}, {F::joycon_left, "SL"},
                        {F::joycon_right, "SL"}, {F::gamecube, "ZL"}},
                    "Left shoulder");
    case kPadRightShoulder:
        return pick(f, {{F::xbox, "RB"}, {F::ps4, "R1"}, {F::nintendo, "R"}, {F::joycon_left, "SR"},
                        {F::joycon_right, "SR"}, {F::gamecube, "Z"}},
                    "Right shoulder");
    case kPadUp: return "D-pad up";
    case kPadDown: return "D-pad down";
    case kPadLeft: return "D-pad left";
    case kPadRight: return "D-pad right";
    case 15:
        return pick(f, {{F::xbox360, "Misc 1"}, {F::xbox, "Share"}, {F::ps5, "Mute"}, {F::nintendo, "Capture"},
                        {F::gamecube, "Capture"}, {F::steam, "Quick access"}},
                    "Misc 1");
    // And its L or R and ZL or ZR as paddles.
    case 16: return pick(f, {{F::joycon_right, "R"}, {F::steam, "R4"}}, "Right paddle 1");
    case 17: return pick(f, {{F::joycon_left, "L"}, {F::steam, "L4"}}, "Left paddle 1");
    case 18: return pick(f, {{F::joycon_right, "ZR"}, {F::steam, "R5"}}, "Right paddle 2");
    case 19: return pick(f, {{F::joycon_left, "ZL"}, {F::steam, "L5"}}, "Left paddle 2");
    case 20: return "Touchpad";
    // A GameCube pad's C button (a Switch 2 one's) and its triggers' clicks.
    case 21: return pick(f, {{F::gamecube, "C"}}, "Misc 2");
    case 22: return pick(f, {{F::gamecube, "L click"}}, "Misc 3");
    case 23: return pick(f, {{F::gamecube, "R click"}}, "Misc 4");
    default: return "Misc " + std::to_string(b - 19);  // misc5 and misc6, SDL's 24 and 25
    }
}

}  // namespace

std::string pad_glyph(PadFamily f, const PadSource& s, int face_label) {
    if (!s.axis) return button(f, s.index, face_label);
    if (s.index == kPadLeftTrigger)
        return pick(f, {{F::xbox, "LT"}, {F::ps4, "L2"}, {F::nintendo, "ZL"}, {F::gamecube, "L"}}, "Left trigger");
    if (s.index == kPadRightTrigger)
        return pick(f, {{F::xbox, "RT"}, {F::ps4, "R2"}, {F::nintendo, "ZR"}, {F::gamecube, "R"}}, "Right trigger");
    const bool left = s.index == kPadLeftX || s.index == kPadLeftY;
    std::string name = left ? "Left stick" : "Right stick";
    if (f == F::gamecube) name = left ? "Control stick" : "C-stick";
    // A Joy-Con alone sends its one stick as the left.
    if (f == F::joycon_left || f == F::joycon_right) name = left ? "Stick" : "Right stick";
    if (s.half == 0) return name;
    const bool x = s.index == kPadLeftX || s.index == kPadRightX;
    // SDL's y is down from the centre.
    return name + (x ? (s.half > 0 ? " right" : " left") : (s.half > 0 ? " down" : " up"));
}
