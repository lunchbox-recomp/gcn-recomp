// The bot link. Another program drives the controllers, one game frame at a time, over a Unix
// socket. The bot listens on the socket and the engine connects to it before the guest runs
// (--bot=PATH or GCN_BOT=PATH on the desktop). See "Bots" in docs/diagnostics.md.
//
// At the first pad poll of each new submitted frame (gx::g_frames_submitted, the count
// input_script.h keys on) the engine sends `frame N` and waits. The bot may send any number of
// `read ADDR LEN` lines, ADDR in hex where the game sees main RAM (80000000 to 817FFFFF) and
// LEN 1 to 256, and the engine answers each with `data HEX`, the bytes as the guest has them.
// Then it sends one `pad PORT BUTTONS SX SY CX CY L R` line for each port it drives (PORT 1 to
// the ports it was given, BUTTONS the PADStatus bits in hex, the rest 0 to 255 in decimal) and
// `done`. Every poll of that frame, on every port, reads those states, so a frame the game
// polls several times is one exchange, and a frame with no poll is none, so `frame` numbers can
// jump.
//
// The guest waits for `done`, and the host time it waited goes to the clock as a pause's does
// (clock_hold), so the pacer makes no catch-up jump for the wait. A paced run still jumps when
// the host falls behind for other reasons, and two paced runs of a bot part at the first jump
// that differs. So a run is the same every time for the same bot unpaced, or paced under
// GCN_CATCHUP=0.
//
// The states go in below the input log (input_log.h), which records them as it records any
// controller's, so a replay needs no bot. The link only reads memory.
//
// A line it doesn't take, a read outside RAM, a `done` without a port the bot drives and the bot
// hanging up each stop the game. The reason goes to stderr with the line quoted, the input log is
// ended there (input_log_end) and the process exits 1. Watching the time is the bot's job.
#pragma once
#include "hw/pad.h"

// Connects to the bot listening on the Unix socket at `path`, to drive ports 1 to `ports`
// (1 to 4) from the first poll. It's false, saying why on stderr, if the connect fails.
//
// The desktop frontend calls it last, after boot_load has read and checked the disc and just
// before the guest starts (threads_start_boot). Under --vr it's called before the headset comes
// up. It's never called with a replay, which already decides the pads.
bool bot_link_start(const char* path, int ports);

// Called by the SI layer (pad_get in hw/si.cpp) at every pad poll, on the guest thread, before
// a script's presses go on and before the input log sees the state. With a bot linked, steps
// the bot at the first poll of a new frame and puts the bot's state for channel `chan` (0 is
// port 1) in `p`; a port the bot doesn't drive reads as unplugged. Does nothing without a bot.
void bot_link_apply(PadState& p, int chan);
