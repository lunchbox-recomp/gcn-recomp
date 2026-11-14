// Gamepad glyph checks: the `test_pad_glyph` target. Runs with no device, SDL or game.
// runtime/pad_glyph.cpp is linked as it is, and each row of the table is one family, one
// source and the name a player sees for it.
//
// usage: test_pad_glyph
// Exits 0 only if every check passes.
#include "pad_glyph.h"
#include <cstdio>
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
    {PadFamily::playstation, button(kPadSouth), kCross, "Cross"},
    {PadFamily::playstation, button(kPadNorth), kTriangle, "Triangle"},
    {PadFamily::other, button(kPadWest), 0, "West button"},
    {PadFamily::other, button(kPadEast), 99, "East button"},
    {PadFamily::xbox, button(kPadBack), 0, "View"},
    {PadFamily::playstation, button(kPadStart), 0, "Options"},
    {PadFamily::nintendo, button(kPadStart), 0, "+"},
    {PadFamily::nintendo, button(kPadGuide), 0, "Home"},
    {PadFamily::other, button(kPadGuide), 0, "Guide"},
    {PadFamily::xbox, button(kPadRightShoulder), 0, "RB"},
    {PadFamily::playstation, button(kPadLeftShoulder), 0, "L1"},
    {PadFamily::nintendo, button(kPadLeftShoulder), 0, "L"},
    {PadFamily::gamecube, button(kPadRightShoulder), 0, "Z"},
    {PadFamily::playstation, button(kPadLeftStick), 0, "L3"},
    {PadFamily::xbox, button(kPadRightStick), 0, "RS"},
    {PadFamily::other, button(kPadUp), 0, "D-pad up"},
    {PadFamily::other, button(kPadRight), 0, "D-pad right"},
    {PadFamily::nintendo, button(15), 0, "Capture"},
    {PadFamily::other, button(20), 0, "Touchpad"},
    {PadFamily::other, button(21), 0, "Misc 2"},
    {PadFamily::other, button(25), 0, "Misc 6"},
    {PadFamily::xbox, axis(kPadLeftTrigger), 0, "LT"},
    {PadFamily::playstation, axis(kPadRightTrigger), 0, "R2"},
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
    const PadFamily kFamilies[] = {PadFamily::other, PadFamily::other, PadFamily::xbox, PadFamily::xbox,
                                   PadFamily::playstation, PadFamily::playstation, PadFamily::playstation,
                                   PadFamily::nintendo, PadFamily::nintendo, PadFamily::nintendo, PadFamily::nintendo,
                                   PadFamily::gamecube, PadFamily::other};
    bool families = true;
    for (int t = 0; t < (int)std::size(kFamilies); t++) families = families && pad_family(t) == kFamilies[t];
    check(families, "each of SDL's gamepad types has its family: Xbox, PlayStation, Nintendo, GameCube or other");
    printf("test_pad_glyph: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
