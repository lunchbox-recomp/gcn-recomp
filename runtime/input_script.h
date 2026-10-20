// Scripted controller input, for driving the game with no human at the controls.
//
// Shared by every frontend -- the SDL one, the headless EGL renderer and the
// benchmark -- since anything past the title screen needs button presses, and the
// headless targets have no other way to produce them. A frontend only calls
// input_script_init; the SI layer applies the presses where the guest reads a pad, so a
// press lands at the same instant of the game on every run, at any host speed.
#pragma once
#include "hw/pad.h"

// Parses "[port/]frame:BUTTON[+BUTTON]:duration,..." where frame is the presented-frame
// count at which to press, e.g. "900:START:10,1200:A:10". The port, 1 to 4, is the
// controller that presses: "2/900:A:10" is A on port 2. Without one it is port 1.
// Buttons: A B X Y Z L R START UP DOWN LEFT RIGHT; stick: SL SR SU SD.
// GCN_INPUT_LOG=1 logs each press once, at the frame it first reaches the game.
//
// `spec` null or empty falls back to the GCN_INPUT environment variable, which an
// Android app has no way to set -- it passes the script in directly instead.
void input_script_init(const char* spec = nullptr);

// Overlays any press for channel `chan` (0 is port 1) that is active at the current
// presented-frame count. Called by the SI layer (hw/si.cpp), on the guest thread, each
// time the guest reads a port that is plugged in; a press on any other port does nothing.
void input_script_apply(PadState& p, int chan);
