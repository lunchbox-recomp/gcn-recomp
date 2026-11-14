// Gamepad glyph checks: the `test_pad_glyph` target. Runs with no device, SDL or game.
// runtime/pad_glyph.cpp is linked as it is, and each row of the table is one family, one
// source and the name a player sees for it.
//
// usage: test_pad_glyph
// Exits 0 only if every check passes.
#include "pad_glyph.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_checks, g_failed;

void check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) g_failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    fflush(stdout);
}

PadSource button(int b) { return {false, b, 0, false}; }
PadSource axis(int a, int half = 0) { return {true, a, half, false}; }

// SDL_GamepadButtonLabel's values.
constexpr int kLabelA = 1, kLabelB = 2, kCross = 5, kTriangle = 8;

struct Row {
    PadFamily family;
    PadSource source;
    int face_label;
    const char* name;
};

const Row kRows[] = {
    {PadFamily::xbox, button(kPadSouth), kLabelA, "A"},
    {PadFamily::nintendo, button(kPadSouth), kLabelB, "B"},
    {PadFamily::ps5, button(kPadSouth), kCross, "Cross"},
    {PadFamily::ps5, button(kPadNorth), kTriangle, "Triangle"},
    {PadFamily::other, button(kPadWest), 0, "West button"},
    {PadFamily::other, button(kPadEast), 99, "East button"},
    {PadFamily::xbox, button(kPadBack), 0, "View"},
    {PadFamily::xbox360, button(kPadBack), 0, "Back"},
    {PadFamily::xbox360, button(kPadStart), 0, "Start"},
    {PadFamily::xbox360, button(kPadRightShoulder), 0, "RB"},
    {PadFamily::xbox, button(15), 0, "Share"},
    {PadFamily::ps3, button(kPadBack), 0, "Select"},
    {PadFamily::ps3, button(kPadStart), 0, "Start"},
    {PadFamily::ps4, button(kPadBack), 0, "Share"},
    {PadFamily::ps5, button(kPadBack), 0, "Create"},
    {PadFamily::ps5, button(15), 0, "Mute"},
    {PadFamily::ps4, button(15), 0, "Misc 1"},
    {PadFamily::ps3, axis(kPadLeftTrigger), 0, "L2"},
    {PadFamily::joycon_left, button(kPadLeftShoulder), 0, "SL"},
    {PadFamily::joycon_right, button(kPadRightShoulder), 0, "SR"},
    {PadFamily::joycon_left, button(17), 0, "L"},
    {PadFamily::joycon_left, button(19), 0, "ZL"},
    {PadFamily::joycon_right, button(16), 0, "R"},
    {PadFamily::joycon_right, button(18), 0, "ZR"},
    {PadFamily::joycon_left, button(kPadStart), 0, "-"},
    {PadFamily::joycon_left, button(kPadGuide), 0, "Capture"},
    {PadFamily::joycon_right, button(kPadStart), 0, "+"},
    {PadFamily::joycon_right, axis(kPadLeftX, 1), 0, "Stick right"},
    // Held upright: L and ZL, or R and ZR, as the shoulder and the trigger, and SL and SR as paddles.
    {PadFamily::joycon_left_upright, button(kPadLeftShoulder), 0, "L"},
    {PadFamily::joycon_left_upright, axis(kPadLeftTrigger), 0, "ZL"},
    {PadFamily::joycon_left_upright, button(17), 0, "SL"},
    {PadFamily::joycon_left_upright, button(19), 0, "SR"},
    {PadFamily::joycon_left_upright, button(kPadBack), 0, "-"},
    {PadFamily::joycon_left_upright, button(15), 0, "Capture"},
    {PadFamily::joycon_left_upright, button(kPadLeftStick), 0, "Stick click"},
    {PadFamily::joycon_left_upright, axis(kPadLeftY, -1), 0, "Stick up"},
    {PadFamily::joycon_right_upright, button(kPadRightShoulder), 0, "R"},
    {PadFamily::joycon_right_upright, axis(kPadRightTrigger), 0, "ZR"},
    {PadFamily::joycon_right_upright, button(16), 0, "SR"},
    {PadFamily::joycon_right_upright, button(18), 0, "SL"},
    {PadFamily::joycon_right_upright, button(kPadStart), 0, "+"},
    {PadFamily::joycon_right_upright, button(kPadGuide), 0, "Home"},
    {PadFamily::joycon_right_upright, button(kPadRightStick), 0, "Stick click"},
    {PadFamily::joycon_right_upright, axis(kPadRightX, 1), 0, "Stick right"},
    {PadFamily::gamecube, button(15), 0, "Capture"},
    {PadFamily::gamecube, button(21), 0, "C"},
    {PadFamily::gamecube, button(22), 0, "L click"},
    {PadFamily::gamecube, button(23), 0, "R click"},
    {PadFamily::steam, button(kPadGuide), 0, "Steam"},
    {PadFamily::steam, button(15), 0, "Quick access"},
    {PadFamily::steam, button(17), 0, "L4"},
    {PadFamily::steam, button(18), 0, "R5"},
    // The first Steam Controller: its back grips as paddles, and no quick access button.
    {PadFamily::steam_grips, button(16), 0, "Right grip"},
    {PadFamily::steam_grips, button(17), 0, "Left grip"},
    {PadFamily::steam_grips, button(kPadGuide), 0, "Steam"},
    {PadFamily::steam_grips, button(15), 0, "Misc 1"},
    {PadFamily::ps5, button(kPadStart), 0, "Options"},
    {PadFamily::nintendo, button(kPadStart), 0, "+"},
    {PadFamily::nintendo, button(kPadGuide), 0, "Home"},
    {PadFamily::other, button(kPadGuide), 0, "Guide"},
    {PadFamily::xbox, button(kPadRightShoulder), 0, "RB"},
    {PadFamily::ps5, button(kPadLeftShoulder), 0, "L1"},
    {PadFamily::nintendo, button(kPadLeftShoulder), 0, "L"},
    {PadFamily::gamecube, button(kPadRightShoulder), 0, "Z"},
    {PadFamily::ps5, button(kPadLeftStick), 0, "L3"},
    {PadFamily::xbox, button(kPadRightStick), 0, "RS"},
    {PadFamily::other, button(kPadUp), 0, "D-pad up"},
    {PadFamily::other, button(kPadRight), 0, "D-pad right"},
    {PadFamily::nintendo, button(15), 0, "Capture"},
    {PadFamily::other, button(20), 0, "Touchpad"},
    {PadFamily::other, button(21), 0, "Misc 2"},
    {PadFamily::other, button(25), 0, "Misc 6"},
    {PadFamily::xbox, axis(kPadLeftTrigger), 0, "LT"},
    {PadFamily::ps5, axis(kPadRightTrigger), 0, "R2"},
    {PadFamily::nintendo, axis(kPadRightTrigger), 0, "ZR"},
    {PadFamily::gamecube, axis(kPadLeftTrigger), 0, "L"},
    {PadFamily::other, axis(kPadLeftTrigger), 0, "Left trigger"},
    {PadFamily::xbox, axis(kPadLeftX), 0, "Left stick"},
    {PadFamily::xbox, axis(kPadRightY), 0, "Right stick"},
    {PadFamily::other, axis(kPadLeftX, 1), 0, "Left stick right"},
    {PadFamily::other, axis(kPadLeftX, -1), 0, "Left stick left"},
    {PadFamily::other, axis(kPadLeftY, -1), 0, "Left stick up"},
    {PadFamily::other, axis(kPadRightY, 1), 0, "Right stick down"},
    {PadFamily::gamecube, axis(kPadRightX, 1), 0, "C-stick right"},
    {PadFamily::gamecube, axis(kPadLeftY), 0, "Control stick"},
};

}  // namespace

int main() {
    for (const Row& r : kRows) {
        const std::string got = pad_glyph(r.family, r.source, r.face_label);
        check(got == r.name, pad_source_name(r.source) + " is \"" + r.name + "\" (got \"" + got + "\")");
    }
    // SDL_GamepadType's values, 0 unknown to 12 Steam.
    const PadFamily kFamilies[] = {PadFamily::other,       PadFamily::other,        PadFamily::xbox360,
                                   PadFamily::xbox,        PadFamily::ps3,          PadFamily::ps4,
                                   PadFamily::ps5,         PadFamily::nintendo,     PadFamily::joycon_left,
                                   PadFamily::joycon_right, PadFamily::nintendo,    PadFamily::gamecube,
                                   PadFamily::steam,       PadFamily::other};
    bool families = true;
    for (int t = 0; t < (int)std::size(kFamilies); t++) families = families && pad_family(t, false, true) == kFamilies[t];
    check(families, "each of SDL's gamepad types has its family, a Joy-Con pair Nintendo's, and an unknown type other");
    check(pad_family(8, true, false) == PadFamily::joycon_left_upright &&
              pad_family(9, true, false) == PadFamily::joycon_right_upright &&
              pad_family(12, true, false) == PadFamily::steam_grips && pad_family(12, true, true) == PadFamily::steam &&
              pad_family(3, true, false) == PadFamily::xbox && pad_family(10, true, false) == PadFamily::nintendo,
          "a Joy-Con alone with a trigger is held upright, a Steam pad with no misc1 is the first Steam Controller, "
          "and the other types don't change with either");
    check(!strcmp(pad_type_name(3), "Xbox One") && !strcmp(pad_type_name(8), "Joy-Con (L)") &&
              !strcmp(pad_type_name(11), "GameCube") && !strcmp(pad_type_name(12), "Steam") &&
              !strcmp(pad_type_name(-1), "gamepad") && !strcmp(pad_type_name(13), "gamepad"),
          "each of SDL's types has the name the Controllers page gives it, and one with none is a gamepad");
    printf("test_pad_glyph: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
