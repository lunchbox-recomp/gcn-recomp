// ---- The held frame's redraw as the frontend's loop runs it (next_batch, main.cpp), over a hidden
// GL window at 1920x1080 (gl_window, tools/test_ports_pages.cpp): the menu opened with F1 and the
// guest held, with GCN_FRAMETIME set at overlay_init and the host clock running on with the steady
// clock (g_host_steady_at), so each `[overlay] frame` line's interval is what a real wait took.
// The redraw says a frame is kept (g_kept) and draws none, and the blur is a stub, so an interval
// here is the loop's wait, the menu's draw and the swap. The `held 1` lines' intervals are printed,
// p50, p99 and max, for all of them and for the frames whose wait began with nothing moving.
// Read in by tools/test_ports.cpp after tools/test_ports_pages.cpp, with GCN_OVERLAY.
#include "test_ports_support.h"

namespace {

constexpr int kHeldWaits = 300;  // the loop's waits in each run, about 5 s of held frames

struct HeldRun {
    std::vector<double> held;     // each `held 1` line's interval in ms, the first's left out
    std::vector<double> settled;  // the same for the frames whose wait began with nothing moving
};

// The menu opened with F1 over the guest, held for kHeldWaits of the loop's waits, then closed,
// its lines written to `said`.
HeldRun held_run(SDL_Window* window, const std::string& dir, const std::string& said) {
    g_window_id = SDL_GetWindowID(window);
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = dir + "/held-settings.txt";
    gx::g_dump_output = false;  // dumping turns the menu's motion off
    std::vector<bool> moving;   // for each frame the menu drew, whether it was moving as the wait began
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
        for (int i = 0; i < kHeldWaits; i++) {
            pump_events();
            update_pads(0);
            const bool was_moving = overlay_animating();
            const long frames = overlay_menu_frames();
            next_batch(window, pw, ph);
            if (overlay_menu_frames() != frames) moving.push_back(was_moving);
            guest_on(guest);
        }
        overlay_close();
        g_host_ns += (uint64_t)(steady_ns() - g_host_steady_at);
        g_host_steady_at = 0;
        g_kept = false;
    });
    overlay_test_end();
    gx::g_dump_output = true;
    HeldRun run;
    const std::vector<FrameLine> lines = frame_lines(said);
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
    const HeldRun run = held_run(window, out, out + "/held-stderr.txt");
    print_intervals("the held redraw's interval", run.held);
    print_intervals("the same once nothing moves", run.settled);
    gl_window_close(window);
}

}  // namespace
