// What the desktop frontend (main.cpp) does for the rest of the runtime.
#pragma once

// A restart, which the overlay's Restart game calls (overlay.h), as a game's own page may: the
// overlay closes and the guest goes on, the run ends as a quit ends it, memory card and all, and
// the game starts again as it does when it resets the console. In a replay that ends the run, as
// there.
void frontend_restart();
