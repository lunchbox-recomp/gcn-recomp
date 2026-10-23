// GCN_SESSION=<dir>: one switch for a play session with every recording and counter on that
// explains its sound and its pacing afterwards, all of it in one directory. See "A play
// session" in docs/diagnostics.md.
#pragma once
#include <string>

// Reads GCN_SESSION. When it's set, makes the directory and sets the switches a session
// turns on, leaving alone any the caller set, and returns the directory the input log goes
// to. Returns "" without a session. Call it first in main, before anything reads a switch.
std::string session_begin(int argc, char** argv);
// Writes <dir>/session.json once the audio device is open, or before the guest boots in a
// run with no window or device: the start time, the host, the engine's commit, the device and
// the command line. `refresh_hz` is the display's, 0 when unknown, and `engine_sha` "" when
// unknown. Without it a relaunch can't tell the directory is used.
void session_write(double refresh_hz, const char* engine_sha);
// Takes back the switches this process set, before it relaunches itself, so the new process
// sets its own.
void session_relaunching();
