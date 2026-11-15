// ---- The held frame's redraw as the frontend's loop runs it (next_batch, main.cpp), over a hidden
// GL window at 1920x1080 (gl_window, tools/test_ports_pages.cpp): the menu opened with F1 and the
// guest held, with GCN_FRAMETIME set at overlay_init and the host clock running on with the steady
// clock (g_host_steady_at), so each `[overlay] frame` line's interval is what a real wait took.
// The redraw says a frame is kept (g_kept) and draws none, and the blur is a stub, so an interval
// here is the loop's wait, the menu's draw and the swap. The `held 1` lines' intervals are printed,
// p50, p99 and max, for all of them and for the frames whose wait began with nothing moving, with
// the loop's thread CPU time a frame.
//
// Their p99 is held to a display frame and a third of a ms, 17 ms at 60 Hz, where CI is unset, and
// printed without failing where it's set, as a CI runner's VM may hold the thread off its core for
// a frame at a time. A stall of the machine makes a run's intervals long, so a run that misses the
// bar is tried again, up to three runs, and only a machine stalled through all of them fails it.
// Then, still held, the waits are checked against times far apart enough that no stall can make
// them pass or fail: with the loop's last wait ended 300 ms ahead, the next draws the held frame
// once and no sooner than that and a period; and with it 2 s ahead, a batch queued before the wait,
// and one submitted 20 ms into it, each end the wait at once with that batch and draw nothing.
// Read in by tools/test_ports.cpp after tools/test_ports_pages.cpp, with GCN_OVERLAY.
#include "test_ports_support.h"

namespace {

constexpr int kHeldWaits = 300;  // the loop's waits in each run, about 5 s of held frames
constexpr int kBarRuns = 3;      // runs that may try for the bar
constexpr double kBarOverMs = 1.0 / 3.0;  // the bar's room over a display frame

struct HeldWaits {
    bool late = false;      // the wait 300 ms ahead drew once, no sooner than that and a period
    bool queued = false;    // the batch queued before the wait ended it at once, drawing nothing
    bool arriving = false;  // the batch submitted 20 ms into the wait ended it then, drawing nothing
};

struct HeldRun {
    std::vector<double> held;     // each `held 1` line's interval in ms, the first's left out
    std::vector<double> settled;  // the same for the frames whose wait began with nothing moving
    size_t frames = 0;            // the frames the menu drew
    double cpu_ms = 0;            // the loop's thread CPU time over its waits, the draws' with it
    HeldWaits waits;
};

// The loop's waits with the guest held, the window `pw` by `ph` pixels, as the head of the file says.
HeldWaits held_waits(SDL_Window* window, int pw, int ph) {
    using clock = std::chrono::steady_clock;
    using std::chrono::milliseconds;
    HeldWaits w;
    long frames = overlay_menu_frames();
    const clock::duration period = held_period(window);
    clock::time_point t0 = clock::now();
    g_waited = t0 + milliseconds(300);
    bool batch = next_batch(window, pw, ph) != nullptr;
    w.late = !batch && clock::now() - t0 >= milliseconds(300) + period && overlay_menu_frames() == frames + 1;

    frames = overlay_menu_frames();
    gx::submit_batch(std::make_unique<gx::Batch>());
    t0 = clock::now();
    g_waited = t0 + milliseconds(2000);
    batch = next_batch(window, pw, ph) != nullptr;
    w.queued = batch && clock::now() - t0 < milliseconds(1000) && overlay_menu_frames() == frames;

    std::thread submit([] {
        std::this_thread::sleep_for(milliseconds(20));
        gx::submit_batch(std::make_unique<gx::Batch>());
    });
    t0 = clock::now();
    g_waited = t0 + milliseconds(2000);
    batch = next_batch(window, pw, ph) != nullptr;
    const clock::duration took = clock::now() - t0;
    submit.join();
    w.arriving = batch && took >= milliseconds(20) && took < milliseconds(1000) && overlay_menu_frames() == frames;
    return w;
}

// The menu opened with F1 over the guest, held for kHeldWaits of the loop's waits, then with
// `waits` the waits checked (held_waits), then closed, its lines written to `said`.
HeldRun held_run(SDL_Window* window, const std::string& dir, const std::string& said, bool waits) {
    g_window_id = SDL_GetWindowID(window);
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = dir + "/held-settings.txt";
    gx::g_dump_output = false;  // dumping turns the menu's motion off
    std::vector<bool> moving;   // for each frame the menu drew, whether it was moving as the wait began
    HeldRun run;
    capture(said, [&] {
        setenv("GCN_FRAMETIME", "1", 1);
        overlay_init(window, "Test title", picture);
        unsetenv("GCN_FRAMETIME");
        g_kept = true;
        g_host_steady_at = steady_ns();
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
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
        if (waits) run.waits = held_waits(window, pw, ph);
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

void held_pace(const std::string& dir) {
    SDL_Window* window = gl_window(1920, 1080);
    if (!window) return;
    const std::string out = dir + "/held";
    plat_make_dirs(out);
    const double frame_ms = std::chrono::duration<double, std::milli>(display_frame(window)).count();
    const double bar_ms = frame_ms + kBarOverMs;
    printf("[held] the display says %.2f Hz, so a display frame is %.2f ms and the bar %.2f ms\n", display_hz(window),
           frame_ms, bar_ms);
    const bool ci = getenv_nonempty("CI") != nullptr;
    HeldWaits waits;
    bool in_bar = false;
    for (int r = 0; r < kBarRuns && !in_bar; r++) {
        const HeldRun run = held_run(window, out, out + "/held-stderr-" + std::to_string(r) + ".txt", r == 0);
        if (r == 0) waits = run.waits;
        print_intervals("the held redraw's interval", run.held);
        print_intervals("the same once nothing moves", run.settled);
        const double frames = run.frames ? (double)run.frames : 1.0;
        printf("[held] the loop's thread CPU %.3f ms a frame, overlay_draw's with it\n", run.cpu_ms / frames);
        in_bar = run.held.size() >= (size_t)kHeldWaits / 2 && percentile(run.held, 0.99) <= bar_ms;
        if (ci) break;
    }
    if (ci)
        printf("[held] the held redraw's p99 is %s the bar, not held where CI is set\n", in_bar ? "within" : "over");
    else
        check(in_bar, "the held redraw's interval, over a run of the loop's waits with the menu open, has a p99 of a "
                      "display frame and a third of a ms or less, in one of three runs at most");
    check(waits.late, "with the loop's last wait ended 300 ms ahead, the held wait draws the held frame once, no sooner "
                      "than that and a period after");
    check(waits.queued && waits.arriving,
          "with it ended 2 s ahead, a batch queued before the held wait, or submitted 20 ms into it, ends the wait at "
          "once with that batch and draws nothing");
    gl_window_close(window);
}

}  // namespace
