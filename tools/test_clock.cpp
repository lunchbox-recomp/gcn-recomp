// Virtual clock checks: the `test_clock` target. Runs with no device, disc or game.
//
// runtime/cpu.cpp is compiled into this file, and its clock runs as a replay's would: the
// guest's running moves the back-edge count on, and clock_pace makes the recorded catch-up
// jumps. The AI DMA (hw/dsp.cpp) leaves those jumps out of the time it gives the guest to
// queue its next buffer, taking them from clock_jumped_ticks(). So that total has to be just
// the virtual time the jumps added, at whatever rate the clock runs (clock_set_cpu_scale):
// the clock less the total is then the guest's own running.
//
// usage: test_clock
// Exits 0 only if every check passes.
#include "cpu.cpp"

// What cpu.cpp's clock calls elsewhere in the runtime. The input log is a replay with one
// jump pending at a time.
std::atomic<uint64_t> g_dec_deadline{UINT64_MAX};
uint64_t g_aid_frames_delivered;
uint32_t gx_frames_submitted() { return 0; }
static uint64_t g_replay_at = UINT64_MAX, g_replay_edges;
bool input_replay_active() { return true; }
void input_log_jump(uint64_t, uint64_t) {}
uint64_t input_replay_next_jump_at() { return g_replay_at; }
uint64_t input_replay_take_jump(uint64_t count) {
    if (count < g_replay_at) return 0;
    g_replay_at = UINT64_MAX;
    return g_replay_edges;
}

namespace {

int g_failed;
uint64_t g_ran;  // the guest's own running, in ticks

void check(bool ok, const char* what, double scale) {
    printf("%s: %s, at CPU scale %g\n", ok ? "ok" : "FAIL", what, scale);
    if (!ok) g_failed++;
}

// The guest runs `edges` loop iterations.
void run(uint64_t edges) {
    const uint64_t t = now_ticks();
    g_vcount += edges;
    g_ran += now_ticks() - t;
}

}  // namespace

int main() {
    clock_set_scale(0);  // unpaced, as a --fast replay runs: only the recorded jumps
    for (double scale : {1.0, 2.0, 0.5}) {
        clock_set_cpu_scale(scale);
        run(1000);
        g_replay_at = g_vcount;
        g_replay_edges = 500;
        const uint64_t t = now_ticks(), jumped = clock_jumped_ticks();
        clock_pace();
        const uint64_t added = now_ticks() - t;
        check(added > 0 && clock_jumped_ticks() - jumped == added, "a jump adds to the jumped total what it adds to the clock", scale);
        run(300);
        check(now_ticks() - clock_jumped_ticks() == g_ran, "the clock less the jumped total is the guest's own running", scale);
    }
    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
