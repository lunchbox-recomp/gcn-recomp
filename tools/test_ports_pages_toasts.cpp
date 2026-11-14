// ---- The notices' review dump and their cost with the menu closed, over the hidden GL window of
// --pages (tools/test_ports_pages.cpp, which calls toasts_review at each size). A
// GCN_OVERLAY_TOASTS that isn't a count of 1 or more is refused in one line. Under
// --dump-output, GCN_OVERLAY_TOASTS=2 writes no dump while one notice shows, and once two show it
// writes overlay_toasts.png at the window's size over GCN_OVERLAY_PLAIN's grey, after their frame
// has settled. Then, with menu_motion on and the menu closed, overlay_draw's CPU time is printed
// over host frames with no notice, which lay out no frame of notices, and with two showing, which
// lay out one each. The PNG is for a person to look at, never a golden. Read in by
// tools/test_ports.cpp after the pages, with GCN_OVERLAY.
#include "test_ports_support.h"

namespace {

constexpr int kCostFrames = 150;  // host frames 10 ms apart, which two toasts outlast

// The menu set up closed over `window`, its dumps and settings in `out`.
void closed_menu(SDL_Window* window, const std::string& out) {
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = out + "/toasts-settings.txt";
    picture.apply = record_apply;
    overlay_init(window, "Test title", picture);
}

// `frames` host frames of overlay_draw with the menu closed, the host clock `step_ms` on at each,
// each one's overlay_draw on the CPU added to `ms` if it's given.
void closed_frames(SDL_Window* window, int frames, uint64_t step_ms, std::vector<double>* ms = nullptr) {
    for (int f = 0; f < frames; f++) {
        pump_events();
        g_host_ns += step_ms * 1000000;
        const auto t0 = std::chrono::steady_clock::now();
        overlay_draw();
        const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms) ms->push_back(took);
        SDL_GL_SwapWindow(window);
    }
}

void toasts_review(SDL_Window* window, const std::string& out, const std::string& at) {
    static std::string dump_dir;
    dump_dir = out + "/toasts";
    plat_make_dirs(dump_dir);
    gx::g_dump_dir = dump_dir.c_str();
    const std::string refused = dump_dir + "/refused-stderr.txt";
    capture(refused, [&] {
        setenv("GCN_OVERLAY_TOASTS", "0", 1);
        closed_menu(window, dump_dir);
        unsetenv("GCN_OVERLAY_TOASTS");
        overlay_test_end();
    });
    check(lines_with(refused, "[overlay] GCN_OVERLAY_TOASTS=0 isn't a count of 1 or more, so the notices aren't dumped\n") == 1,
          (at + ": a GCN_OVERLAY_TOASTS that isn't a count of 1 or more says so once").c_str());
    setenv("GCN_OVERLAY_TOASTS", "2", 1);
    setenv("GCN_OVERLAY_PLAIN", "1", 1);
    closed_menu(window, dump_dir);
    unsetenv("GCN_OVERLAY_TOASTS");
    unsetenv("GCN_OVERLAY_PLAIN");
    const std::string png = dump_dir + "/overlay_toasts.png";
    overlay_pads_notice("Xbox Wireless Controller", " connected as player 1");
    closed_frames(window, 6, 0);
    const bool waited = !exists_path(png);
    overlay_pads_notice("Keyboard", " is now player 2");
    int dumped_at = -1;
    for (int f = 0; f < 20 && dumped_at < 0; f++) {
        closed_frames(window, 1, 0);
        if (exists_path(png)) dumped_at = f;
    }
    const Png p = png_at(png);
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    overlay_test_end();
    check(waited, (at + ": GCN_OVERLAY_TOASTS=2 writes no dump while one notice shows").c_str());
    check(dumped_at >= 2 && p.w == pw && p.h == ph && p.first[0] == 128 && p.first[1] == 128 && p.first[2] == 128,
          (at + ": with two showing, overlay_toasts.png is written once their frame has settled, at the window's "
                "size over plain grey with no backdrop")
              .c_str());

    // The cost with the menu closed, with motion as a player has it, once those two have gone.
    g_host_ns += 3000 * 1000000ull;
    gx::g_dump_output = false;
    closed_menu(window, dump_dir);
    std::vector<double> none, two;
    const long menu = overlay_menu_frames(), before = overlay_notice_frames();
    closed_frames(window, kCostFrames, 10, &none);
    const long idle = overlay_notice_frames() - before;
    overlay_pads_notice("Xbox Wireless Controller", " connected as player 1");
    overlay_pads_notice("Keyboard", " is now player 2");
    closed_frames(window, kCostFrames, 10, &two);
    const long shown = overlay_notice_frames() - before - idle;
    const long menus = overlay_menu_frames() - menu;
    overlay_test_end();
    gx::g_dump_output = true;
    check(idle == 0 && shown == kCostFrames && menus == 0,
          (at + ": with the menu closed, no notice lays out no frame of notices, two showing one a host frame, and "
                "neither a frame of the menu")
              .c_str());
    const std::vector<double> after(two.begin() + 1, two.end());  // past the first, which makes GL objects
    printf("[pages] %s toasts, menu closed: none, %ld notice frames, overlay_draw p50 %.3f ms, p99 %.3f ms; two "
           "showing, %ld notice frames, first %.3f ms, then p50 %.3f ms, p99 %.3f ms, max %.3f ms; over %d frames, "
           "%ld menu frames\n",
           at.c_str(), idle, percentile(none, 0.5), percentile(none, 0.99), shown, two.front(), percentile(after, 0.5),
           percentile(after, 0.99), percentile(after, 1.0), kCostFrames, menus);
}

}  // namespace
