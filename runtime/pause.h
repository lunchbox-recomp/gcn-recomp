// Pausing the guest.
//
// A pause holds the guest thread at its next frame boundary, and never in the middle of a
// frame. The boundary is a display copy that finishes a frame: a copy of the whole frame, or
// the last part of one the game copies to the XFB in parts (gx/xfb_rows.h). The guest frame
// hook (gx::set_guest_frame_hook) has run there, where one runs; with the front end inline
// (GCN_GX_SYNC=1) it doesn't. The hold can come as late as the guest's longest gap between
// frames, a scene load say, and until then pause_requested() is true and paused() false.
// Every other guest thread waits on the held one, so the whole guest stops. Guest time stops
// with it: the virtual clock only counts what the guest runs, and the host time the hold
// took is handed to the clock (clock_hold), so the pacer doesn't make it up afterwards. The
// guest resumes where it stopped, with nothing to catch up. Meanwhile the frontend draws the
// newest presented frame again (gx::render_present_last), which is the one the guest is held
// after, so the window can still be redrawn, resized or drawn over. It also stops the pads'
// rumble motors until the resume.
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

// The guest thread, at each frame boundary (gx/cmd.cpp), with the frames submitted so far.
// Those count display copies, so a game that copies its frames in two parts ends them two
// apart. Holds the thread there while a pause is asked for. GCN_PAUSE_AT=<frame>:<host
// frames> asks for one here, once, at the first boundary where the count has reached
// <frame> (so 0 is the first boundary), and lifts it after the frontend has shown <host
// frames> frames while held (pause_host_frame), for tests.
void pause_gate(uint32_t frame);
// The frontend, once for each frame it shows while the guest is held.
void pause_host_frame();
// When the last pause ended, or the clock's epoch if none has. For timing the guest's
// frames, which shouldn't count the time it was held.
std::chrono::steady_clock::time_point pause_resumed_at();
