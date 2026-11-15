// ---- test_ports --held: the held frame's redraw as the frontend's loop runs it (next_batch,
// main.cpp), over a hidden GL window at 1920x1080 (gl_window, tools/test_ports_pages.cpp): the menu
// opened with F1 and the guest held, with GCN_FRAMETIME set at overlay_init and the host clock
// running on with the steady clock (g_host_steady_at), so each `[overlay] frame` line's interval is
// what a real wait took. The redraw says a frame is kept (g_kept), noting the host time it was
// asked (g_kept_at), and draws none, and the blur is a stub, so an interval here is the loop's
// wait, the menu's draw and the swap. The `held 1` lines' intervals are printed, p50, p99 and max,
// for all of them, for the frames whose wait began with something moving and for those with
// nothing, and the waits' lateness the lines carry, with the first held interval and its fields and
// the loop's thread CPU time a frame, and for the first run each line's thread CPU and swap. After the runs, the held wait's own CPU time is printed.
//
// Their p99 is held to a display frame and a third of a ms, 17 ms at 60 Hz, where CI is unset, and
// printed without failing where it's set, as a CI runner's VM may hold the thread off its core for
// a frame at a time. So are the p50s, while something moves to within 0.5 ms of half a display
// frame and once nothing does of a whole one, the periods held_period (main.cpp) gives. A stall of
// the machine makes a run's intervals long, so a run that misses a bar is tried again, up to three
// runs, and only a machine stalled through all of them fails it.
//
// Then, still held, the waits are checked. With the loop's last wait ended 300 ms ahead, the next
// asks for the held frame's redraw once, no sooner than that and a period on the host clock, a
// bound a stall can only help. With it 300 ms ahead, a batch queued before the wait ends it within
// 150 ms with that batch and draws nothing, and so does each of ten submitted 20 ms into a wait.
// Where CI is unset, the median time from those submits to the wait's end is held to 0.3 ms, in
// one of three rounds of ten at most, so a batch has to end the wait, not the next 1 ms step.
// Where it's set, a runner may start the submitting thread late, so those waits' deadline is 30 s
// ahead, each must end before it with that batch and draw nothing, and the times are printed.
// Before any window, the display rates that give a 60 Hz frame (frame_at_hz) are checked, and that
// the loop's wait for a batch with the guest running ends as the guest holds, which no batch would
// end (wake_batch_wait, from pause_gate), and that with the hold lifted the next such wait runs to
// its timeout, the wake spent. Over the window, overlay_init is checked to warm the blur once at
// the window's pixel size (render_overlay_blur_warm), and the first run's lines their lateness: the
// first held line's over 0, as no wait ends the instant it could, and every held line's 0 or more.
// Each held line's thread CPU is 0 or more and no more than its present and blur (kPrintedMs, or
// kCiThreadRoomMs where CI is set), some line's over 0, and its swap 0 or more, summing to over 0.
// Then a redraw is made to wait on the gamepads' lock, which another thread holds for 50 ms from
// before the wait ends: its present takes that wait and its thread CPU stays under half of it, as a
// blocked thread runs nothing. A stall that leaves the lock let go under 10 ms after the wait has
// it tried again, up to three times. Then one redraw's swap is slowed 20 ms (g_swap_window), and
// the next redraw's line must say a swap of 20 ms or more with a present under that, as its swap is
// the frame before's and comes ahead of its wait. Those checks hold where CI is set too.
// Exit 77 with no GL window. Read in by tools/test_ports.cpp after tools/test_ports_pages.cpp,
// with GCN_OVERLAY.
#include "test_ports_support.h"

#include <algorithm>

namespace {

constexpr int kHeldWaits = 300;  // the loop's waits in each run, about 5 s of held frames
constexpr int kBarRuns = 3;      // runs that may try for the bar
constexpr double kBarOverMs = 1.0 / 3.0;  // the bar's room over a display frame
constexpr int kSubmits = 10;              // batches submitted into a wait in each round
constexpr int kSubmitRounds = 3;          // rounds that may try for the median
constexpr double kSubmitBarMs = 0.3;      // the median's bar, from a submit to the wait's end
constexpr double kNearMs = 0.5;           // how near half a frame and a frame the p50s must be
constexpr int64_t kMsNs = 1000000;
// The room a line's thread CPU has over its present and blur, which show_frame reads inside their
// span: the three are printed to 0.001 ms. Where CI is set it's 0.1 ms. A runner's VM read 0.025 ms
// of thread CPU over a span of 0.003 ms with the reads in the older order, which put the last CPU
// read's own time in it, and its thread clock may be coarser than a Mac's.
constexpr double kPrintedMs = 0.005;
constexpr double kCiThreadRoomMs = 0.1;

struct HeldWaits {
    bool late = false;      // the wait 300 ms ahead redrew once, no sooner than that and a period
    bool queued = false;    // the batch queued before the wait ended it within 150 ms, drawing nothing
    bool arriving = false;  // each batch submitted 20 ms into a wait ended it, drawing nothing, as the head says
    double submit_ms = -1;  // the last round's median from a submit to the wait's end
};

struct HeldRun {
    std::vector<double> held;     // each `held 1` line's interval in ms, the first's left out
    std::vector<double> moving;   // the same for the frames whose wait began with something moving
    std::vector<double> settled;  // and for those whose wait began with nothing moving
    std::vector<double> late;     // each of those lines' lateness, how late its wait ended
    std::vector<double> thread;   // each of those lines' thread CPU over its present and blur
    bool thread_within = true;    // each of those within 0 and its present and blur, to the printing
    bool swap_read = true;        // each of those lines' swap 0 or more
    double swap_total = 0;        // their swaps' sum, in ms
    FrameLine first;              // the first of them, the first held interval
    FrameLine blocked;            // with `waits`, the redraw the gamepads' lock held off (blocked_draw)
    double blocked_ms = 0;        // how long after its wait ended the lock was let go
    FrameLine swapped;            // with `waits`, the redraw after one whose swap was slowed (slow_swap_draws)
    size_t frames = 0;            // the frames the menu drew
    double cpu_ms = 0;            // the loop's thread CPU time over its waits, the draws' with it
    bool warmed = false;          // overlay_init warmed the blur once, at the window's pixel size
    HeldWaits waits;
};

std::chrono::steady_clock::time_point steady_at(int64_t ns) {
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns));
}

// Whether the loop's wait for a batch with the guest running (take_batch, kRunWaitMs in main.cpp)
// ends as the guest holds, 20 ms into it, with no batch, rather than at its timeout, here 2 s, or
// 30 s where CI is set. It must end before half that, which a stall can only make it miss by
// ending the wait later, not pass.
bool hold_ends_wait(bool ci) {
    while (gx::take_batch_until(std::chrono::steady_clock::now())) {
    }  // nothing queued, and no wake left from a hold before
    const int timeout_ms = ci ? 30000 : 2000;
    Guest guest;
    pause_request(true);
    std::thread boundary([&guest] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        {
            std::lock_guard<std::mutex> lk(guest.m);
            guest.asked = 1;
        }
        guest.cv.notify_all();
    });
    const bool held_before = paused();
    const int64_t t0 = steady_ns();
    const std::unique_ptr<gx::Batch> b = gx::take_batch(timeout_ms);
    const double took = (double)(steady_ns() - t0) / 1e6;
    const bool held = paused();
    boundary.join();
    printf("[held] the wait for a batch with the guest running ended %.3f ms in, %s held\n", took,
           held ? "with the guest" : "without the guest");
    return !held_before && !b && held && took < timeout_ms / 2;
}

// How long the loop's wait for a batch runs with nothing queued once the hold that ended the last
// one is lifted, in ms, or -1 if a batch ended it. That wait spent the hold's wake, so this one
// runs to its timeout, kLiftedWaitMs. A stall can only make it longer.
constexpr int kLiftedWaitMs = 50;
double lifted_wait_ms() {
    const int64_t t0 = steady_ns();
    const std::unique_ptr<gx::Batch> b = gx::take_batch(kLiftedWaitMs);
    const double took = (double)(steady_ns() - t0) / 1e6;
    printf("[held] with the hold lifted, the wait for a batch with nothing queued ended %.3f ms in\n", took);
    return b ? -1 : took;
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

// One held redraw with another thread holding the gamepads' lock, which show_frame takes while the
// menu is open, from before the loop's wait ends until kLockHoldMs later. The wait ends at once.
// Returns how long after it ended the lock was let go, in ms.
constexpr int kLockHoldMs = 50;
double blocked_draw(SDL_Window* window, int pw, int ph) {
    std::mutex m;
    std::condition_variable cv;
    bool locked = false;
    std::atomic<int64_t> released{0};
    std::thread holder([&] {
        std::lock_guard<std::recursive_mutex> pads(g_pads_mutex);
        {
            std::lock_guard<std::mutex> lk(m);
            locked = true;
        }
        cv.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(kLockHoldMs));
        released = steady_ns();
    });
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&locked] { return locked; });
    }
    g_waited = std::chrono::steady_clock::now() - held_period(window);
    next_batch(window, pw, ph);
    holder.join();
    return (double)(released - std::chrono::duration_cast<std::chrono::nanoseconds>(g_waited.time_since_epoch()).count()) /
           1e6;
}

// What show_frame swaps the window with in these runs (g_swap_window): SDL_GL_SwapWindow, after a
// sleep of g_swap_sleep_ms where that's set, which the swap spends.
constexpr int kSlowSwapMs = 20;
int g_swap_sleep_ms = 0;
bool slowed_swap(SDL_Window* window) {
    if (g_swap_sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(g_swap_sleep_ms));
    g_swap_sleep_ms = 0;
    return SDL_GL_SwapWindow(window);
}

// Two held redraws, each with the wait ended at once: the first's swap slowed by kSlowSwapMs, then
// the one whose line carries that swap. Returns the second's frame number.
long slow_swap_draws(SDL_Window* window, int pw, int ph) {
    g_swap_sleep_ms = kSlowSwapMs;
    g_waited = std::chrono::steady_clock::now() - held_period(window);
    next_batch(window, pw, ph);
    const long after = overlay_menu_frames();
    g_waited = std::chrono::steady_clock::now() - held_period(window);
    next_batch(window, pw, ph);
    return after;
}

// The menu opened with F1 over the guest, held for kHeldWaits of the loop's waits, then with
// `waits` a redraw held off by the gamepads' lock (blocked_draw, tried again up to kBarRuns times
// while a stall leaves the lock let go under kLockHoldMs / 5 after the wait), two with a slowed swap
// (slow_swap_draws) and the waits checked (held_waits), then closed, its lines written to `said`.
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
        g_swap_window = slowed_swap;
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
        if (waits) {
            for (int a = 0; a < kBarRuns && run.blocked_ms < kLockHoldMs / 5; a++) {
                run.blocked.f = overlay_menu_frames();
                run.blocked_ms = blocked_draw(window, pw, ph);
            }
            run.swapped.f = slow_swap_draws(window, pw, ph);
            run.waits = held_waits(window, pw, ph, ci);
        }
        overlay_close();
        g_host_ns += (uint64_t)(steady_ns() - g_host_steady_at);
        g_host_steady_at = 0;
        g_kept = false;
        g_swap_window = SDL_GL_SwapWindow;
    });
    overlay_test_end();
    gx::g_dump_output = true;
    const std::vector<FrameLine> lines = frame_lines(said);
    run.frames = moving.size();
    for (size_t i = 1; i < lines.size() && i < moving.size(); i++) {
        if (lines[i].held != 1) continue;
        if (run.held.empty()) run.first = lines[i];
        run.held.push_back(lines[i].interval);
        run.late.push_back(lines[i].late);
        run.thread.push_back(lines[i].thread);
        run.thread_within = run.thread_within && lines[i].thread >= 0 &&
                            lines[i].thread <= lines[i].present + lines[i].blur + (ci ? kCiThreadRoomMs : kPrintedMs);
        run.swap_read = run.swap_read && lines[i].swap >= 0;
        run.swap_total += lines[i].swap;
        (moving[i] ? run.moving : run.settled).push_back(lines[i].interval);
    }
    const long blocked_f = run.blocked.f, swapped_f = run.swapped.f;
    run.blocked = {};
    run.swapped = {};
    for (const FrameLine& l : lines) {
        if (waits && l.f == blocked_f) run.blocked = l;
        if (waits && l.f == swapped_f) run.swapped = l;
    }
    return run;
}

void print_intervals(const char* what, const std::vector<double>& v) {
    printf("[held] %s: p50 %.2f ms, p99 %.2f ms, max %.2f ms over %zu frames\n", what, percentile(v, 0.5),
           percentile(v, 0.99), percentile(v, 1.0), v.size());
}

// The held wait's own CPU time on the loop's thread, a mean over kWaitCpuWaits waits of a display
// frame each with nothing to end them, in ms a wait.
constexpr int kWaitCpuWaits = 30;
double wait_cpu_ms(SDL_Window* window) {
    const auto period = display_frame(window);
    const double cpu0 = thread_cpu_ms();
    for (int i = 0; i < kWaitCpuWaits; i++) gx::take_batch_until(std::chrono::steady_clock::now() + period);
    return (thread_cpu_ms() - cpu0) / kWaitCpuWaits;
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
    const bool ci = getenv_nonempty("CI") != nullptr;
    check(hold_ends_wait(ci), "with the guest running, the loop's wait for a batch ends as the guest holds, with no "
                              "batch, well before its timeout");
    check(lifted_wait_ms() >= kLiftedWaitMs - 5, "with that hold lifted, the next wait for a batch with nothing "
                                                 "queued runs to its 50 ms timeout, the hold's wake spent");
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
    HeldWaits waits;
    bool in_bar = false, half = false, whole = false, warmed = false, late_read = false;
    bool thread_read = false, swap_read = false, blocked_read = false, swap_pinned = false;
    const auto near = [](const std::vector<double>& v, double ms) {
        return !v.empty() && std::fabs(percentile(v, 0.5) - ms) <= kNearMs;
    };
    for (int r = 0; r < kBarRuns && !(in_bar && half && whole); r++) {
        const HeldRun run = held_run(window, out, out + "/held-stderr-" + std::to_string(r) + ".txt", r == 0, ci);
        if (r == 0) {
            waits = run.waits;
            warmed = run.warmed;
            late_read = !run.late.empty() && run.first.late > 0 &&
                        std::all_of(run.late.begin(), run.late.end(), [](double ms) { return ms >= 0; });
            thread_read = !run.thread.empty() && run.thread_within && percentile(run.thread, 1.0) > 0;
            swap_read = !run.held.empty() && run.swap_read && run.swap_total > 0;
            const FrameLine& b = run.blocked;
            blocked_read = b.held == 1 && run.blocked_ms >= kLockHoldMs / 5 &&
                           b.present >= run.blocked_ms - kPrintedMs && b.thread <= run.blocked_ms / 2;
            const FrameLine& s = run.swapped;
            swap_pinned = s.held == 1 && s.swap >= kSlowSwapMs && s.present < kSlowSwapMs;
            printf("[held] after a swap slowed by %d ms, the next redraw's line has swap %.3f ms and present %.3f ms\n",
                   kSlowSwapMs, s.swap, s.present);
            printf("[held] the loop's thread CPU over each held line's present and blur: p50 %.3f ms, p99 %.3f ms, "
                   "max %.3f ms; their swaps %.3f ms in all\n",
                   percentile(run.thread, 0.5), percentile(run.thread, 0.99), percentile(run.thread, 1.0),
                   run.swap_total);
            printf("[held] with the gamepads' lock let go %.3f ms after the wait ended, the redraw's line has present "
                   "%.3f ms and thread %.3f ms\n",
                   run.blocked_ms, b.present, b.thread);
        }
        print_intervals("the held redraw's interval", run.held);
        print_intervals("the same while something moves", run.moving);
        print_intervals("the same once nothing moves", run.settled);
        print_intervals("their waits' lateness", run.late);
        printf("[held] the first held interval %.2f ms, its wait %.3f ms late, %.3f ms from it to the blur and %.3f "
               "ms of blur\n",
               run.first.interval, run.first.late, run.first.present, run.first.blur);
        const double frames = run.frames ? (double)run.frames : 1.0;
        printf("[held] the loop's thread CPU %.3f ms a frame, overlay_draw's with it\n", run.cpu_ms / frames);
        in_bar = in_bar || (run.held.size() >= (size_t)kHeldWaits / 2 && percentile(run.held, 0.99) <= bar_ms);
        half = half || near(run.moving, frame_ms / 2);
        whole = whole || near(run.settled, frame_ms);
        if (ci) break;
    }
    printf("[held] the held wait alone takes %.3f ms of the loop's thread CPU a display frame\n", wait_cpu_ms(window));
    if (ci) {
        printf("[held] the held redraw's p99 is %s the bar, not held where CI is set\n", in_bar ? "within" : "over");
        printf("[held] the p50 while something moves is %s half a display frame, and once nothing moves %s a whole "
               "one, not held where CI is set\n",
               half ? "near" : "not near", whole ? "near" : "not near");
        printf("[held] the submits' median is %s the %.1f ms bar, not held where CI is set\n",
               waits.submit_ms >= 0 && waits.submit_ms <= kSubmitBarMs ? "within" : "over", kSubmitBarMs);
    } else {
        check(in_bar, "the held redraw's interval, over a run of the loop's waits with the menu open, has a p99 of a "
                      "display frame and a third of a ms or less, in one of three runs at most");
        check(half, "while the menu moves, the held redraw's p50 is within 0.5 ms of half a display frame, in one of "
                    "three runs at most");
        check(whole, "once nothing moves, its p50 is within 0.5 ms of a whole display frame, in one of three runs at "
                     "most");
    }
    check(late_read, "the first held interval's line says its wait ended late, and every held line's "
                     "lateness is 0 or more");
    check(thread_read, "each held line's thread CPU is 0 or more and no more than its present and blur, and some "
                       "line's is over 0");
    check(swap_read, "each held line's swap is 0 or more, and over the run they come to more than 0");
    check(swap_pinned, "a redraw after one whose swap was slowed 20 ms has a swap of 20 ms or more on its line, and "
                       "a present under that");
    check(blocked_read, "a held redraw that waits on the gamepads' lock another thread holds has that wait in its "
                        "present and not in its thread CPU, which is under half of it");
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
