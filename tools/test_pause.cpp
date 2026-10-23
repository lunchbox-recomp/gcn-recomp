// Pause checks: the `test_pause` target. Runs with no window, GL, disc or game.
//
// runtime/pause.cpp is compiled into this file over a fake host clock, and a std::thread
// stands in for the guest thread at a frame boundary. clock_hold sums what it's given, so a
// check can see that a hold's host time went to the clock. GCN_PAUSE_AT is set before the
// first boundary, which is when pause.cpp reads it.
//
// usage: test_pause
// Exits 0 only if every check passes. A guest thread still held after 2 s is a failure, and
// the run then ends without waiting for it.
#include "pause.cpp"
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

std::atomic<uint64_t> g_host{0};    // the fake host clock, in ns
std::atomic<uint64_t> g_handed{0};  // what clock_hold has been given, all told
int g_failed;
bool g_stuck;  // a guest thread never got past its boundary, so it can't be joined

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

// Whether `cond` holds within 2 s.
template <class F> bool soon(F cond) {
    for (int i = 0; i < 2000 && !cond(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return cond();
}

// The guest thread reaching the boundary at `frame` and going on past it. Made with new: one
// that never gets past is left running, and must not outlive what it writes to.
struct Guest {
    std::atomic<bool> past{false};
    std::thread t;
    explicit Guest(uint32_t frame) : t([this, frame] { pause_gate(frame); past = true; }) {}
    // Whether it got past within 2 s. Joined and freed if it did.
    bool finish() {
        if (!soon([this] { return past.load(); })) {
            t.detach();
            g_stuck = true;
            return false;
        }
        t.join();
        delete this;
        return true;
    }
};

// Whether the guest stays held for a while, as it should while a pause is asked for.
bool stays_held(const Guest& g) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return paused() && !g.past;
}

void check_parse(const char* s, uint32_t frame, long host_frames) {
    const TestPause t = parse_pause_at(s);
    char what[128];
    if (host_frames) snprintf(what, sizeof(what), "GCN_PAUSE_AT=%s is frame %u for %ld host frames", s, frame, host_frames);
    else snprintf(what, sizeof(what), "GCN_PAUSE_AT=%s is refused", s);
    check(t.frame == frame && t.host_frames == host_frames, what);
}

}  // namespace

uint64_t host_ns() { return g_host.load(); }
void clock_hold(uint64_t ns) { g_handed += ns; }
uint64_t audio_frames_out() { return 0; }

int main() {
    setenv("GCN_PAUSE_AT", "50:3", 1);

    pause_request(true);
    Guest* g = new Guest(1);
    check(soon([] { return paused(); }), "a pause asked for holds the guest at its next frame boundary");
    check(stays_held(*g), "the guest stays held while the pause is asked for");
    g_host += 40000000;
    pause_request(false);
    check(g->finish(), "lifting the pause lets the guest go on");
    check(!paused(), "the guest isn't held once it has gone on");
    check(g_handed == 40000000, "the 40 ms the guest was held went to the clock");

    pause_request(true);
    pause_request(false);
    g = new Guest(2);
    check(g->finish() && g_handed == 40000000, "a pause lifted before the boundary doesn't hold the guest");

    pause_request(true);
    g = new Guest(3);
    check(soon([] { return paused(); }), "a pause asked for from the frontend holds the guest");
    for (int i = 0; i < 5; i++) pause_host_frame();
    check(stays_held(*g), "the frontend's host frames don't lift a pause GCN_PAUSE_AT didn't ask for");
    pause_request(false);
    check(g->finish(), "lifting it lets the guest go on");

    g = new Guest(49);
    check(g->finish() && !pause_requested(), "GCN_PAUSE_AT=50:3 doesn't pause at frame 49");
    g = new Guest(51);
    check(soon([] { return paused(); }), "GCN_PAUSE_AT=50:3 pauses at the first boundary from frame 50 on");
    pause_host_frame();
    pause_host_frame();
    check(stays_held(*g), "it holds the guest through 2 of its 3 host frames");
    pause_host_frame();
    check(g->finish(), "it lets the guest go on after the third");
    g = new Guest(52);
    check(g->finish() && !pause_requested(), "GCN_PAUSE_AT pauses only once");

    check_parse("1000:300", 1000, 300);
    check_parse("0:5", 0, 5);
    check_parse("4294967295:2", 4294967295u, 2);
    check_parse("1000:0", 0, 0);
    check_parse("1000:300x", 0, 0);
    check_parse("1000:", 0, 0);
    check_parse(":5", 0, 0);
    check_parse("x", 0, 0);
    check_parse("-1:5", 0, 0);
    check_parse("5:-1", 0, 0);
    check_parse("4294967296:5", 0, 0);
    check_parse("99999999999999999999:5", 0, 0);
    check_parse("5:99999999999999999999", 0, 0);

    printf("%s\n", g_failed ? "FAILED" : "all passed");
    fflush(stdout);
    if (g_stuck) std::_Exit(1);
    return g_failed ? 1 : 0;
}
