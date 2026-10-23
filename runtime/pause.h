// Pausing the guest.
//
// A pause holds the guest thread at its next frame boundary, where the guest frame hook
// runs (gx::set_guest_frame_hook), and never in the middle of a frame. Every other guest
// thread waits on that one, so the whole guest stops. Guest time stops with it: the virtual
// clock only counts what the guest runs, and the host time the hold took is handed to the
// clock (clock_hold), so the pacer doesn't make it up afterwards. The guest resumes where it
// stopped, with nothing to catch up. Meanwhile the frontend draws the last frame again
// (gx::render_present_last), so the window can still be redrawn, resized or drawn over.
//
// The sound ring isn't told. Nothing new reaches it while the guest is held, so it plays
// out what it has, fades as it runs dry and fades back in once the cushion has refilled
// after the resume (audio.cpp), as after any stall.
#pragma once
#include <chrono>
#include <cstdint>

// Asks for a pause, or lifts it. Any thread.
void pause_request(bool on);
// Whether a pause is asked for, whether or not the guest has reached its boundary yet.
bool pause_requested();
// Whether the guest is held at a frame boundary now.
bool paused();

// The guest thread, at each frame boundary, with the frames submitted so far. Holds the
// thread there while a pause is asked for. GCN_PAUSE_AT=<frame>:<host frames> asks for one
// here, once, when the count reaches <frame>, and lifts it after the frontend has shown
// <host frames> frames while held (pause_host_frame), for tests.
void pause_gate(uint32_t frame);
// The frontend, once for each frame it shows while the guest is held.
void pause_host_frame();
// When the last pause ended, or the clock's epoch if none has. For timing the guest's
// frames, which shouldn't count the time it was held.
std::chrono::steady_clock::time_point pause_resumed_at();
