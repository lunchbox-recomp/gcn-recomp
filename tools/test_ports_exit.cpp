// ---- How a run ends. In the port checks, the window loop's end waiting for a save (RunEnd)
// and GCN_QUIT_AT. Then the modes that end in a process of their own: --interrupt, --reset,
// --shutdown, --exit-held, --restart, --quit and --restart-replay. Each but --reset ends in an
// at-exit function, which checks what the end did and prints the mode's last line. Read in by
// tools/test_ports.cpp after the frontend.
#include "test_ports_support.h"
#include <unistd.h>

namespace {

// ---- The window loop's end waits for a save the game may be part way through (RunEnd), here
// on a fake clock with a fake card's answers: none written lately ends at once, a recent write
// waits for the card's quiet period, and 3 s is the most it waits.
void run_end(const std::string& dir) {
    using Clock = RunEnd::Clock;
    using std::chrono::milliseconds;
    const Clock::time_point t0 = Clock::time_point{} + std::chrono::hours(1);
    const std::string out = dir + "/run-end-stderr.txt";
    bool at_once = false, waits = false, saved = false, most = false, scripted = false;
    capture(out, [&] {
        RunEnd script;
        script.scripted();
        scripted = script.may_end(t0, true);
        RunEnd idle;
        at_once = idle.may_end(t0, false);
        RunEnd saving;
        waits = !saving.may_end(t0, true) && !saving.may_end(t0 + milliseconds(1500), true);
        saved = saving.may_end(t0 + milliseconds(1600), false);
        RunEnd busy;
        most = !busy.may_end(t0, true) && !busy.may_end(t0 + milliseconds(2999), true) &&
               busy.may_end(t0 + milliseconds(3000), true);
    });
    check(at_once, "a run whose memory card has no write unsaved, or no card, ends at the first ask");
    check(waits && saved, "one whose card has a write unsaved goes on until the card's saver has taken it");
    check(most, "and 3 s from the first ask is the most it goes on");
    check(scripted, "--exit-after's end is at its frame, whatever the card has unsaved");
    check(lines_with(out, "[quit] ") == 2, "each run that has to wait says so once, and --exit-after's says nothing");
}

// ---- GCN_QUIT_AT=N asks SDL to quit at presented frame N, as the window's close button does,
// so the window loop stops (pump_events) and the run ends through RunEnd's wait. It asks once.
void quit_at_frame(const std::string& dir) {
    const std::string out = dir + "/quit-at-stderr.txt";
    bool before = false, at = false, once = false, bad = false, empty = false;
    capture(out, [&] {
        frame();
        setenv("GCN_QUIT_AT", "3", 1);
        quit_at_load();
        quit_at_presented(2);
        before = pump_events();
        quit_at_presented(3);
        at = !pump_events();
        quit_at_presented(4);
        once = pump_events();
        setenv("GCN_QUIT_AT", "3x", 1);
        quit_at_load();
        quit_at_presented(3);
        bad = pump_events();
        setenv("GCN_QUIT_AT", "", 1);
        quit_at_load();
        quit_at_presented(3);
        empty = g_quit_at < 0 && pump_events();
        unsetenv("GCN_QUIT_AT");
        quit_at_load();
    });
    check(before && at, "GCN_QUIT_AT=3 stops the window loop at presented frame 3, as the close button does");
    check(once && lines_with(out, "[quit] GCN_QUIT_AT=3 ends the run at presented frame 3") == 1,
          "and only then, which it says once");
    check(bad && lines_with(out, "[quit] GCN_QUIT_AT=3x isn't a frame number, so it ends nothing") == 1,
          "a GCN_QUIT_AT that isn't a frame number ends nothing, and says so");
    check(empty && lines_with(out, "[quit] GCN_QUIT_AT= ") == 0,
          "an empty GCN_QUIT_AT ends nothing and says nothing, as an unset one does");
}

Virtual* g_rumbling;  // the gamepad rumbling as the process ends, for the at-exit check
int g_stderr = -1;    // the test's stderr, while the exit's goes to a file

// `v` plugged in, and port 1's motor on from here, which rumbles it.
void rumbling(Virtual& v) {
    attach(v);
    frame();
    set_motor(0, true);
    frame();
}

// ---- An interrupt runs on a thread of its own (plat_install_crash_handlers), so it stops
// the gamepads' rumble while the main loop may still be running frames.

// The interrupt's plat_exit_now runs this last, after the gamepads have been shut down.
void after_interrupt() {
    check(g_rumbling->stops > 0, "an interrupt on its own thread stops a rumbling gamepad while frames run");
    finish_now("--interrupt");
}

// An event watch, which SDL calls as it queues an event: for each gamepad press, whether
// another thread found the gamepads' lock free then.
int g_presses_queued, g_queued_unlocked;
bool SDLCALL lock_probe(void*, SDL_Event* e) {
    if (e->type != SDL_EVENT_GAMEPAD_BUTTON_DOWN) return true;
    bool unlocked = false;
    std::thread([&] {
        unlocked = g_pads_mutex.try_lock();
        if (unlocked) g_pads_mutex.unlock();
    }).join();
    g_presses_queued++;
    g_queued_unlocked += unlocked;
    return true;
}

int run_interrupt(const std::string& dir) {
    if (!sdl_up()) return 1;
    no_controls(dir);
    power_on(1);
    gamepads_init();
    Virtual a{"virtual pad a"};
    rumbling(a);
    check(a.rumbles == 1 && a.stops == 0, "port 1's motor rumbles its gamepad");
    SDL_AddEventWatch(lock_probe, nullptr);
    press(a, SDL_GAMEPAD_BUTTON_SOUTH, true);
    press(a, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_RemoveEventWatch(lock_probe, nullptr);
    check(g_presses_queued == 1 && g_queued_unlocked == 1,
          "SDL queues the window's events with the gamepads' lock free, so a loop SDL runs in there keeps no interrupt waiting");
    g_rumbling = &a;
    plat_at_exit(after_interrupt);
    std::thread(on_interrupt).detach();
    // The interrupt ends the process, or its 2 s watchdog does.
    for (const Uint64 give_up = SDL_GetTicks() + 5000; SDL_GetTicks() < give_up;) frame();
    check(false, "the interrupt ends the process");
    return finish("--interrupt");
}

// ---- The game resetting the console ends the process (guest_reset), so the frontend's
// reset handler stops a rumbling gamepad and has it say what it sent first. Under a replay
// the handler doesn't start the game again, which leaves this process here to check.
int run_reset(const std::string& dir) {
    if (!sdl_up()) return 1;
    no_controls(dir);
    write_log(dir + "/log", "# gcn-recomp input log v5\n# game TEST00  started 2026-10-28T12:00:00\n"
                            "# epoch 1792000000\n0 0 0 1 0000 80 80 80 80 00 00\n# end 4 0\n");
    std::string card, sram;
    if (!input_replay_load(dir + "/log", card, sram)) return 1;
    power_on(1);
    gamepads_init();
    Virtual a{"virtual pad reset"};
    rumbling(a);
    const std::string out = dir + "/reset-stderr.txt";
    // Without the replay the handler would start this program again.
    if (input_replay_active()) capture(out, [] { on_reset(0, 0); });
    check(a.rumbles == 1 && a.stops == 1 && lines_with(out, "gamepads: port 1 rumble on 1 time, 0 of 2 calls failed\n") == 1,
          "the game resetting the console stops a rumbling gamepad, which says what it sent");
    detach(a);
    return finish("--reset");
}

// ---- The game shutting the console down ends the process with no reset handler
// (guest_reset), so what stops a rumbling gamepad and has it say what it sent is the
// shutdown open_controllers leaves for the exit. --exit-held exits the same way with the
// gamepads' lock held on the exiting thread, as show_frame holds it while the menu is drawn,
// where the Display page's change of scale can be fatal: the shutdown takes the lock again
// there. A hang ends at a 2 s watchdog, with 2.
bool g_held;  // --exit-held

// The exit runs this last, after open_controllers' shutdown.
void after_shutdown() {
    stderr_back(g_stderr, "shutdown-stderr.txt");
    check(g_rumbling->stops == 1 && lines_with("shutdown-stderr.txt", "gamepads: port 1 rumble on 1 time, 0 of 2 calls failed\n") == 1,
          g_held ? "an exit with the gamepads' lock held on its thread stops a rumbling gamepad, which says what it sent"
                 : "the game shutting the console down stops a rumbling gamepad, which says what it sent");
    finish_now(g_held ? "--exit-held" : "--shutdown");
}

int run_shutdown(const std::string& dir, bool held) {
    // main()'s saves/ports.txt is relative to where it runs.
    if (chdir(dir.c_str()) != 0 || !sdl_up()) return 1;
    open_controllers("no-controls.txt");
    power_on(1);
    Virtual a{"virtual pad shutdown"};
    rumbling(a);
    check(a.rumbles == 1 && a.stops == 0, "port 1's motor rumbles its gamepad");
    g_rumbling = &a;
    g_stderr = stderr_to("shutdown-stderr.txt");
    plat_at_exit(after_shutdown);
    g_held = held;
    if (held) {
        plat_watchdog(2, 2);
        g_pads_mutex.lock();
    }
    // What guest_reset does for a shutdown, and fatal before it aborts: no reset handler, only
    // the exit. Its code isn't 0, so the run passes only by after_shutdown's own exit.
    plat_exit_now(3);
}

// ---- A restart asked for (frontend_restart, which the overlay's Restart calls) stops the
// frontend's loop as a quit does, and the window path's end (end_run) ends the run as for a
// quit: the input log gets its end, and the exit's at-exit functions run. Then it prints a
// [reset] line, as a console reset does, and relaunches. This process isn't started again:
// the at-exit function added here runs last, and ends it, before the relaunch would. No memory
// card is made here (exi_init), so none is written.
//
// The loop claims the run's end as it stops (stop_loop), so the game resetting the console
// after that (guest_reset, on another thread, with main()'s reset handler) says nothing, ends
// nothing and waits, as the loop waits for a save and as end_run runs. A restart's own [reset]
// line is the run's one, and --quit, the same with a quit, has none and no relaunch.
std::string g_restart_dir;

// Where the run's stderr goes as it ends, from stderr_to on.
std::string end_said() { return g_restart_dir + "/restart-stderr.txt"; }

// The input log's `# end` lines, and the [reset] lines the run printed.
int log_ends() {
    const std::string ends = lines_starting(g_restart_dir + "/log/inputs.txt", "# end ");
    return (int)std::count(ends.begin(), ends.end(), '\n');
}
std::string resets() { return lines_starting(end_said(), "[reset] "); }

void after_restart() {
    stderr_back(g_stderr, end_said());
    check(g_relaunching == 1, "the run ends through the exit's at-exit functions on the way to one relaunch");
    check(log_ends() == 1, "the input log has one end");
    check(resets() == "[reset] the overlay's Restart game starts the game again\n",
          "one [reset] line, Restart's: the game's resets in the wait and in end_run say nothing");
    finish_now("--restart");
}

void after_quit() {
    stderr_back(g_stderr, end_said());
    check(!g_relaunching && log_ends() == 1,
          "a quit ends the run with no relaunch, though the game resets the console in the wait and in end_run");
    check(resets().empty(), "and no [reset] line");
    finish_now("--quit");
}

// The game resetting the console on the guest's thread as the run waits for a save, and again
// as end_run runs (record::finish's stub).
void resets_as_it_ends() {
    std::thread(guest_reset, 0u, 0u).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    g_reset_in_finish = true;
}

int run_restart(const std::string& dir) {
    if (!sdl_up()) return 1;
    g_restart_dir = dir;
    if (!input_log_start(dir + "/log", dir + "/no-card.raw", dir + "/no-sram.raw")) return 1;
    set_reset_handler(on_reset);
    frontend_restart();
    check(!pump_events() && !g_relaunching, "after a restart is asked for, the frontend's loop stops, as for a quit");
    bool running = true;
    stop_loop(running);
    // The loop asks again at each turn of the wait, as pump_events keeps saying it should stop.
    stop_loop(running);
    g_stderr = stderr_to(end_said());
    plat_at_exit(after_restart);
    resets_as_it_ends();
    end_run();
}

int run_quit(const std::string& dir) {
    if (!sdl_up()) return 1;
    g_restart_dir = dir;
    if (!input_log_start(dir + "/log", dir + "/no-card.raw", dir + "/no-sram.raw")) return 1;
    set_reset_handler(on_reset);
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
    check(!pump_events() && !g_restart, "a quit stops the frontend's loop");
    bool running = true;
    stop_loop(running);
    g_stderr = stderr_to(end_said());
    plat_at_exit(after_quit);
    resets_as_it_ends();
    end_run();
}

// ---- In a replay the menu has no Restart game, as a relaunch would replay the log from its
// start, which the log doesn't hold. That holds after the log runs out too. A restart asked
// for all the same only ends the run, as a console reset does in a replay, with no relaunch
// and no [reset] line.
void after_restart_replay() {
    stderr_back(g_stderr, end_said());
    check(!g_relaunching && resets().empty(),
          "in a replay a restart asked for ends the run with no relaunch and no [reset] line");
    finish_now("--restart-replay");
}

#ifdef GCN_OVERLAY
// The bottom row of the page, over a window as in overlay_buttons, from the keyboard.
void replay_menu(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    OverlayPicture picture;
    picture.path = dir + "/settings.txt";
    overlay_init(window, "Test title", picture);
    {
        Guest guest;
        host_frame(guest);
        tap(guest, SDL_SCANCODE_F1, SDLK_F1);
        bottom_row(guest);
        tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
        check(waiting(OverlayAsk::quit), "in a replay the bottom row of the page is Quit game alone");
        tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
        tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    }
    close_window(window);
}
#endif

int run_restart_replay(const std::string& dir) {
    if (!sdl_up()) return 1;
    g_restart_dir = dir;
    write_log(dir + "/log", "# gcn-recomp input log v5\n# game TEST00  started 2026-10-31T12:00:00\n"
                            "# epoch 1792000000\n0 0 0 1 0000 80 80 80 80 00 00\n# end 4 0\n");
    std::string card, sram;
    if (!input_replay_load(dir + "/log", card, sram) || !input_replay_active()) return 1;
    // Polled past the log's end, which hands the ports to live input. The run is a replay's
    // still, and a relaunch would replay it again.
    const std::string finished = dir + "/finished-stderr.txt";
    capture(finished, [] {
        for (int i = 0; i < 8; i++) read(0);
    });
    check(lines_with(finished, "[input] replay finished at poll ") == 1 && !input_replay_active(),
          "the replay runs out, and live input takes over");
#ifdef GCN_OVERLAY
    replay_menu(dir);
#endif
    frontend_restart();
    g_stderr = stderr_to(end_said());
    plat_at_exit(after_restart_replay);
    end_run();
}

}  // namespace
