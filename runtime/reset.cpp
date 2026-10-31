// The game resetting the console, and the end of a run that it and a frontend may both reach
// for (runtime.h).
#include "runtime.h"
#include "input_log.h"
#include "platform.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

static std::atomic<bool> g_end_claimed{false};

bool claim_run_end() { return !g_end_claimed.exchange(true); }

static ResetHandler g_reset_handler;
void set_reset_handler(ResetHandler fn) { g_reset_handler = fn; }

void guest_reset(uint32_t kind, uint32_t code) {
    // The frontend is ending the run already, and says itself whether the game starts again.
    if (!claim_run_end())
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    static const char* const kinds[] = {"restart", "hot reset", "shutdown"};
    fprintf(stderr, "[reset] the game reset the console (%s, code %u)\n", kind < 3 ? kinds[kind] : "unknown", code);
    input_log_end();
    fflush(nullptr);
    if (g_reset_handler && kind != 2) g_reset_handler(kind, code);
    plat_exit_now(0);
}
