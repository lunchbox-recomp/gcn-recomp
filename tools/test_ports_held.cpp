// ---- test_ports --held: the held frame's redraw as the frontend's loop runs it (next_batch,
// main.cpp), over a hidden GL window at 1920x1080 (gl_window, tools/test_ports_pages.cpp): the menu
// opened with F1 and the guest held, with GCN_FRAMETIME set at overlay_init and the host clock
// running on with the steady clock (g_host_steady_at), so each `[overlay] frame` line's interval is
// what a real wait took. The redraw says a frame is kept (g_kept), noting the host time it was
// asked (g_kept_at), and draws none, and the blur is a stub, so an interval here is the loop's
// wait, the menu's draw and the swap. The `held 1` lines' intervals are printed, p50, p99 and max,
// for all of them and for the frames whose wait began with nothing moving, with the loop's thread
// CPU time a frame.
//
// Their p99 is held to a display frame and a third of a ms, 17 ms at 60 Hz, where CI is unset, and
// printed without failing where it's set, as a CI runner's VM may hold the thread off its core for
// a frame at a time. A stall of the machine makes a run's intervals long, so a run that misses the
// bar is tried again, up to three runs, and only a machine stalled through all of them fails it.
//
// Then, still held, the waits are checked. With the loop's last wait ended 300 ms ahead, the next
// asks for the held frame's redraw once, no sooner than that and a period on the host clock, a
// bound a stall can only help. With it 300 ms ahead, a batch queued before the wait ends it within
// 150 ms with that batch and draws nothing, and so does each of ten submitted 20 ms into a wait.
// Where CI is unset, the median time from those submits to the wait's end is held to 0.3 ms, in
// one of three rounds of ten at most, so a batch has to end the wait, not the next 1 ms step.
// Where it's set, a runner may start the submitting thread late, so those waits' deadline is 30 s
// ahead, each must end before it with that batch and draw nothing, and the times are printed.
// Before any window, the display rates that give a 60 Hz frame (frame_at_hz) are checked, and
// over it, that overlay_init warms the blur once at the window's pixel size (render_overlay_blur_warm).
// Exit 77 with no GL window. Read in by tools/test_ports.cpp after tools/test_ports_pages.cpp,
// with GCN_OVERLAY.
#include "test_ports_support.h"

namespace {

constexpr int kHeldWaits = 300;  // the loop's waits in each run, about 5 s of held frames
constexpr int kBarRuns = 3;      // runs that may try for the bar
constexpr double kBarOverMs = 1.0 / 3.0;  // the bar's room over a display frame
constexpr int kSubmits = 10;              // batches submitted into a wait in each round
constexpr int kSubmitRounds = 3;          // rounds that may try for the median
constexpr double kSubmitBarMs = 0.3;      // the median's bar, from a submit to the wait's end
constexpr int64_t kMsNs = 1000000;

struct HeldWaits {
    bool late = false;      // the wait 300 ms ahead redrew once, no sooner than that and a period
    bool queued = false;    // the batch queued before the wait ended it within 150 ms, drawing nothing
    bool arriving = false;  // each batch submitted 20 ms into a wait ended it, drawing nothing, as the head says
    double submit_ms = -1;  // the last round's median from a submit to the wait's end
};

struct HeldRun {
    std::vector<double> held;     // each `held 1` line's interval in ms, the first's left out
    std::vector<double> settled;  // the same for the frames whose wait began with nothing moving
    size_t frames = 0;            // the frames the menu drew
    double cpu_ms = 0;            // the loop's thread CPU time over its waits, the draws' with it
    bool warmed = false;          // overlay_init warmed the blur once, at the window's pixel size
    HeldWaits waits;
};

std::chrono::steady_clock::time_point steady_at(int64_t ns) {
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns));
}

// The loop's waits with the guest held, the window `pw` by `ph` pixels, as the head of the file says.
HeldWaits held_waits(SDL_Window* window, int pw, int ph, bool ci) {
    HeldWaits w;
    long frames = overlay_menu_frames();
    const int64_t period = std::chrono::nanoseconds(held_period(window)).count();
    int64_t t0 = steady_ns();
    const uint64_t host0 = g_host_ns + (uint64_t)(t0 - g_host_steady_at);  // host_ns() at t0
    g_waited = steady_at(t0 + 300 * kMsNs);
    bool batch = next_batch(window, pw, ph) != nullptr;
    w.late = !batch && g_kept_at >= host0 + (uint64_t)(300 * kMsNs + period) && overlay_menu_frames() == frames + 1;

    frames = overlay_menu_frames();
    gx::submit_batch(std::make_unique<gx::Batch>());
    t0 = steady_ns();
    g_waited = steady_at(t0 + 300 * kMsNs);
    batch = next_batch(window, pw, ph) != nullptr;
    w.queued = batch && steady_ns() - t0 < 150 * kMsNs && overlay_menu_frames() == frames;

    // Where CI is set the deadline is 30 s ahead, so only the batch can end the wait before it
    // however late a runner starts the submitting thread, and the times are printed, not held.
    const int64_t deadline = (ci ? 30000 : 300) * kMsNs;
    bool each = true, quick = false;
    for (int r = 0; r < kSubmitRounds && each && !quick; r++) {
        std::vector<double> from_submit;
        double into_max = 0, wait_max = 0;
        for (int i = 0; i < kSubmits && each; i++) {
            std::atomic<int64_t> sent{0};
            std::atomic<gx::Batch*> sent_batch{nullptr};
            std::thread submit([&sent, &sent_batch] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                auto b = std::make_unique<gx::Batch>();
                sent_batch = b.get();
                sent = steady_ns();
                gx::submit_batch(std::move(b));
            });
            t0 = steady_ns();
            g_waited = steady_at(t0 + deadline);
            const std::unique_ptr<gx::Batch> got = next_batch(window, pw, ph);
            const int64_t back = steady_ns();
            submit.join();
            const bool drew = overlay_menu_frames() != frames;
            const double into = (double)(sent - t0) / 1e6, wait = (double)(back - t0) / 1e6;
            const bool ok = got && got.get() == sent_batch && !drew && back - t0 < (ci ? deadline : 150 * kMsNs);
            if (!ok || wait >= 150)
                printf("[held] submit %d of round %d came %.3f ms into its wait, which ended %.3f ms in, %s that "
                       "batch%s\n",
                       i + 1, r + 1, into, wait, got && got.get() == sent_batch ? "with" : "without",
                       drew ? ", and drew a frame" : "");
            each = each && ok;
            into_max = std::max(into_max, into);
            wait_max = std::max(wait_max, wait);
            from_submit.push_back((double)(back - sent) / 1e6);
        }
        if (!each) break;
        w.submit_ms = percentile(from_submit, 0.5);
        printf("[held] a batch submitted into the held wait ends it a median %.3f ms later, over %d, the latest "
               "submit %.3f ms into its wait and the longest wait %.3f ms\n",
               w.submit_ms, kSubmits, into_max, wait_max);
        quick = w.submit_ms <= kSubmitBarMs;
        if (ci) break;
    }
    while (gx::take_batch_until(std::chrono::steady_clock::now())) {
    }  // a batch that came after its wait ended
    w.arriving = each && (ci || quick);
    return w;
}

// The menu opened with F1 over the guest, held for kHeldWaits of the loop's waits, then with
// `waits` the waits checked (held_waits), then closed, its lines written to `said`.
HeldRun held_run(SDL_Window* window, const std::string& dir, const std::string& said, bool waits, bool ci) {
    g_window_id = SDL_GetWindowID(window);
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = dir + "/held-settings.txt";
    gx::g_dump_output = false;  // dumping turns the menu's motion off
    std::vector<bool> moving;   // for each frame the menu drew, whether it was moving as the wait began
    HeldRun run;
    capture(said, [&] {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        const int warms = g_blur_warms;
        setenv("GCN_FRAMETIME", "1", 1);
        overlay_init(window, "Test title", picture);
        unsetenv("GCN_FRAMETIME");
        run.warmed = g_blur_warms == warms + 1 && g_blur_warm_w == pw && g_blur_warm_h == ph;
        g_kept = true;
        g_host_steady_at = steady_ns();
        Guest guest;
        push_key(SDL_SCANCODE_F1, SDLK_F1, true);
        push_key(SDL_SCANCODE_F1, SDLK_F1, false);
        const double cpu0 = thread_cpu_ms();
        for (int i = 0; i < kHeldWaits; i++) {
            pump_events();
            update_pads(0);
            const bool was_moving = overlay_animating();
            const long frames = overlay_menu_frames();
            next_batch(window, pw, ph);
            if (overlay_menu_frames() != frames) moving.push_back(was_moving);
            guest_on(guest);
        }
        run.cpu_ms = thread_cpu_ms() - cpu0;
        if (waits) run.waits = held_waits(window, pw, ph, ci);
        overlay_close();
        g_host_ns += (uint64_t)(steady_ns() - g_host_steady_at);
        g_host_steady_at = 0;
        g_kept = false;
    });
    overlay_test_end();
    gx::g_dump_output = true;
    const std::vector<FrameLine> lines = frame_lines(said);
    run.frames = moving.size();
    for (size_t i = 1; i < lines.size() && i < moving.size(); i++) {
        if (lines[i].held != 1) continue;
        run.held.push_back(lines[i].interval);
        if (!moving[i]) run.settled.push_back(lines[i].interval);
    }
    return run;
}

void print_intervals(const char* what, const std::vector<double>& v) {
    printf("[held] %s: p50 %.2f ms, p99 %.2f ms, max %.2f ms over %zu frames\n", what, percentile(v, 0.5),
           percentile(v, 0.99), percentile(v, 1.0), v.size());
}

// Whether a display at `hz` gets a frame of `ms`, to 1 ns.
bool frame_is(double hz, double ms) {
    return std::llabs(std::chrono::nanoseconds(frame_at_hz(hz)).count() - std::llround(ms * 1e6)) <= 1;
}

int run_held(const std::string& dir) {
    if (!sdl_up()) return 1;
    no_controls(dir);
    power_on(1);
    gamepads_init();
    const double k60 = 1000.0 / 60.0;
    check(frame_is(0, k60) && frame_is(1, k60) && frame_is(22.9, k60) && frame_is(60, k60) && frame_is(120, k60) &&
              frame_is(23, 1000.0 / 23.0) && frame_is(50, 20.0) && frame_is(59.94, 1000.0 / 59.94),
          "a display at 23 Hz up to 60 gets a frame of its own, and one faster, under 23 Hz or of no rate a 60 Hz one");
    SDL_Window* window = gl_window(1920, 1080);
    if (!window) {
        printf("test_ports held: skipped\n");
        return kSkipped;
    }
    const std::string out = dir + "/held";
    plat_make_dirs(out);
    const double frame_ms = std::chrono::duration<double, std::milli>(display_frame(window)).count();
    const double bar_ms = frame_ms + kBarOverMs;
    printf("[held] the display says %.2f Hz, so a display frame is %.2f ms and the bar %.2f ms\n", display_hz(window),
           frame_ms, bar_ms);
    const bool ci = getenv_nonempty("CI") != nullptr;
    HeldWaits waits;
    bool in_bar = false, warmed = false;
    for (int r = 0; r < kBarRuns && !in_bar; r++) {
        const HeldRun run = held_run(window, out, out + "/held-stderr-" + std::to_string(r) + ".txt", r == 0, ci);
        if (r == 0) {
            waits = run.waits;
            warmed = run.warmed;
        }
        print_intervals("the held redraw's interval", run.held);
        print_intervals("the same once nothing moves", run.settled);
        const double frames = run.frames ? (double)run.frames : 1.0;
        printf("[held] the loop's thread CPU %.3f ms a frame, overlay_draw's with it\n", run.cpu_ms / frames);
        in_bar = run.held.size() >= (size_t)kHeldWaits / 2 && percentile(run.held, 0.99) <= bar_ms;
        if (ci) break;
    }
    if (ci) {
        printf("[held] the held redraw's p99 is %s the bar, not held where CI is set\n", in_bar ? "within" : "over");
        printf("[held] the submits' median is %s the %.1f ms bar, not held where CI is set\n",
               waits.submit_ms >= 0 && waits.submit_ms <= kSubmitBarMs ? "within" : "over", kSubmitBarMs);
    } else {
        check(in_bar, "the held redraw's interval, over a run of the loop's waits with the menu open, has a p99 of a "
                      "display frame and a third of a ms or less, in one of three runs at most");
    }
    check(warmed, "overlay_init over a GL window warms the blurred backdrop once, at the window's size in pixels");
    check(waits.late, "with the loop's last wait ended 300 ms ahead, the held wait asks for the held frame's redraw "
                      "once, no sooner than that and a period after");
    check(waits.queued, "with it ended 300 ms ahead, a batch queued before the held wait ends it within 150 ms with "
                        "that batch and draws nothing");
    check(waits.arriving, ci ? "each of ten batches submitted 20 ms into a held wait 30 s long ends it before then "
                               "with that batch and draws nothing"
                             : "each of ten batches submitted 20 ms into a held wait ends it within 150 ms with that "
                               "batch and draws nothing, a median 0.3 ms or less after its submit in one of three "
                               "rounds at most");
    gl_window_close(window);
    return finish("held");
}

}  // namespace
