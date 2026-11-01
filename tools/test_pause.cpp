// Pause checks: the `test_pause` target. Runs with no window, GL, disc or game.
//
// runtime/pause.cpp is compiled into this file over a fake host clock, and a std::thread
// stands in for the guest thread at a frame boundary. clock_hold and exi_card_held each sum
// what they're given, so a check can see that a hold's host time went to the clock and to the
// memory card. GCN_PAUSE_AT is set before the first boundary, which is when pause.cpp reads it.
//
// runtime/gx/cmd.cpp is compiled in too, so the guest thread can reach its boundaries the way
// a game's command stream takes it there: through process() with the front end inline, and
// through skim() with it on its own thread. The renderer's side of a copy is a stub that
// counts the frames as xf.cpp does inline and records whether the batch would end there;
// the skim's sync counts them as fifo.cpp does. The VI shows 480 rows.
//
// tools/test_pause_gl.cpp has the paused redraw's checks, render_gl.cpp on a fake GL.
//
// usage: test_pause
// Exits 0 only if every check passes. A guest thread still held after 2 s is a failure, and
// the run then ends without waiting for it.
#include "pause.cpp"
#include "gx/cmd.cpp"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>

uint8_t* g_mem;
bool g_log_enabled[LOG_COUNT];
void log_msg(LogCat, const char*, ...) {}
void pe_signal_token(uint16_t, bool) {}
void pe_signal_finish() {}
uint32_t vi_display_height() { return 480; }

namespace {
std::vector<bool> g_ends;  // what each display copy told the renderer, in order
}

namespace gx {
uint32_t g_frame_counter;
std::atomic<uint32_t> g_frames_submitted{0};
void renderer_draw(const DrawCall&) {}
void renderer_bp_write(uint32_t, uint32_t) {}
void renderer_efb_copy(uint32_t, bool ends_frame) {
    if (!((g_state.bp[0x52] >> 14) & 1)) return;
    g_ends.push_back(ends_frame);
    if (!g_fe_threaded) g_frames_submitted++;
}
}  // namespace gx

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
bool held_soon() {
    return soon([] { return paused(); });
}

// The guest thread running `run`, which takes it to a boundary and on past it. Made with new:
// one that never gets past is left running, and must not outlive what it writes to.
struct Guest {
    std::atomic<bool> past{false};
    std::thread t;
    explicit Guest(std::function<void()> run) : t([this, run] { run(); past = true; }) {}
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

Guest* at_boundary(uint32_t frame) {
    return new Guest([frame] { pause_gate(frame); });
}

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

// A BP register write as the command stream carries it.
void bp(std::vector<uint8_t>& s, uint32_t reg, uint32_t v) {
    const uint32_t w = reg << 24 | (v & 0xFFFFFF);
    s.insert(s.end(), {0x61, (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w});
}

// A display copy of `rows` rows of a 640-pixel frame to `dest`, as the XFB's rows go there.
// A halved one (0x52's bit 9) writes half the rows, each half as wide.
constexpr uint32_t kXfb = 0x00400000, kStride = 640 * 2;
std::vector<uint8_t> display_copy(uint32_t dest, uint32_t rows, bool halved = false) {
    std::vector<uint8_t> s;
    bp(s, 0x49, 0);
    bp(s, 0x4A, 639 | (rows - 1) << 10);
    bp(s, 0x4B, dest >> 5);
    bp(s, 0x52, 1 << 14 | (halved ? 1 << 9 : 0));
    return s;
}

void skim_sync(gx::SkimSync kind, uint32_t, std::vector<uint8_t>&) {
    if (kind == gx::SkimSync::Frame) gx::g_frames_submitted++;
}

// The guest thread issuing `s`: inline through process(), or through the skim.
Guest* issue(std::vector<uint8_t> s, bool inline_fe) {
    return new Guest([s, inline_fe] {
        if (inline_fe) {
            gx::process(s.data(), (uint32_t)s.size(), true);
        } else {
            std::vector<uint8_t> out;
            gx::skim(s.data(), (uint32_t)s.size(), true, out, skim_sync);
        }
    });
}

// A pause asked for before each copy: a whole frame, then a frame in two halves. The redraw
// can show a held frame only if its copy went on before the guest stopped: inline to the
// renderer, which ends the batch there, and threaded to the front end through the skim's
// sync. Either counts the frame, so the count when held says whether it had gone.
void check_frame_ends(bool inline_fe) {
    const char* how = inline_fe ? "inline" : "threaded";
    char what[160];
    gx::g_fe_threaded = !inline_fe;
    g_ends.clear();
    const uint32_t before = gx::g_frames_submitted;
    // Whether the guest is held with `copies` copies gone on, the last of them ending a frame.
    auto held_after = [&](uint32_t copies) {
        return held_soon() && gx::g_frames_submitted == before + copies &&
               (!inline_fe || (g_ends.size() == copies && g_ends.back()));
    };
    pause_request(true);
    Guest* g = issue(display_copy(kXfb, 480), inline_fe);
    snprintf(what, sizeof(what), "%s, a pause holds the guest at a display copy of the whole frame", how);
    check(held_soon(), what);
    snprintf(what, sizeof(what), "%s, it holds after the copy has gone on", how);
    check(held_after(1), what);
    pause_request(false);
    snprintf(what, sizeof(what), "%s, lifting it lets the guest go on", how);
    check(g->finish(), what);

    pause_request(true);
    g = issue(display_copy(kXfb, 240), inline_fe);
    snprintf(what, sizeof(what), "%s, a pause doesn't hold the guest at the first half of a frame", how);
    check(g->finish() && !paused(), what);
    g = issue(display_copy(kXfb + 240 * kStride, 240), inline_fe);
    snprintf(what, sizeof(what), "%s, it holds the guest at the second half", how);
    check(held_soon(), what);
    snprintf(what, sizeof(what), "%s, after the second half has gone on", how);
    check(held_after(3), what);
    pause_request(false);
    snprintf(what, sizeof(what), "%s, lifting it lets the guest go on", how);
    check(g->finish(), what);

    // Halved, a copy of all 480 rows writes 240 rows of a 320-pixel XFB, so it takes two.
    constexpr uint32_t kHalvedXfb = 0x00600000, kHalvedStride = 320 * 2;
    pause_request(true);
    g = issue(display_copy(kHalvedXfb, 480, true), inline_fe);
    snprintf(what, sizeof(what), "%s, a pause doesn't hold the guest at a halved copy of all 480 rows", how);
    check(g->finish() && !paused(), what);
    g = issue(display_copy(kHalvedXfb + 240 * kHalvedStride, 480, true), inline_fe);
    snprintf(what, sizeof(what), "%s, it holds the guest at the second halved copy, which fills the XFB", how);
    check(held_after(5), what);
    pause_request(false);
    snprintf(what, sizeof(what), "%s, lifting it lets the guest go on", how);
    check(g->finish(), what);

    if (inline_fe) {
        check(g_ends == std::vector<bool>({true, false, true, false, true}),
              "inline, the batch ends at the whole frame and at each copy that fills the XFB, not before");
    }
}

}  // namespace

uint64_t host_ns() { return g_host.load(); }
void clock_hold(uint64_t ns) { g_handed += ns; }
static std::atomic<uint64_t> g_card_held{0};  // what exi_card_held has been given, all told
void exi_card_held(uint64_t ns) { g_card_held += ns; }
uint64_t audio_frames_out() { return 0; }
int present_last_checks();  // tools/test_pause_gl.cpp

int main() {
    setenv("GCN_PAUSE_AT", "50:3", 1);

    pause_request(true);
    Guest* g = at_boundary(1);
    check(held_soon(), "a pause asked for holds the guest at its next frame boundary");
    check(stays_held(*g), "the guest stays held while the pause is asked for");
    g_host += 40000000;
    pause_request(false);
    check(g->finish(), "lifting the pause lets the guest go on");
    check(!paused(), "the guest isn't held once it has gone on");
    check(g_handed == 40000000, "the 40 ms the guest was held went to the clock");
    check(g_card_held == 40000000, "and to the memory card, whose quiet period leaves it out");

    pause_request(true);
    pause_request(false);
    g = at_boundary(2);
    check(g->finish() && g_handed == 40000000, "a pause lifted before the boundary doesn't hold the guest");

    pause_request(true);
    g = at_boundary(3);
    check(held_soon(), "a pause asked for from the frontend holds the guest");
    for (int i = 0; i < 5; i++) pause_host_frame();
    check(stays_held(*g), "the frontend's host frames don't lift a pause GCN_PAUSE_AT didn't ask for");
    pause_request(false);
    check(g->finish(), "lifting it lets the guest go on");

    g = at_boundary(49);
    check(g->finish() && !pause_requested(), "GCN_PAUSE_AT=50:3 doesn't pause at frame 49");
    g = at_boundary(51);
    check(held_soon(), "GCN_PAUSE_AT=50:3 pauses at the first boundary from frame 50 on");
    pause_host_frame();
    pause_host_frame();
    check(stays_held(*g), "it holds the guest through 2 of its 3 host frames");
    pause_host_frame();
    check(g->finish(), "it lets the guest go on after the third");
    g = at_boundary(52);
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

    check_frame_ends(true);
    check_frame_ends(false);

    g_failed += present_last_checks();

    printf("%s\n", g_failed ? "FAILED" : "all passed");
    fflush(stdout);
    if (g_stuck) std::_Exit(1);
    return g_failed ? 1 : 0;
}
