// Gamepad port checks on SDL's virtual joysticks: the `test_ports` target, run by
// tools/test_ports.sh.
//
// runtime/main.cpp is compiled into this file with its main() renamed, so a frame's input
// is the frontend's own pump_events / update_pads / merge_port, and runtime/hw/si.cpp too,
// so each port is read back the way the guest reads it: pad_get, with scripted presses
// (input_script.cpp) applied and the input log (input_log.cpp) consulted. gamepads.cpp,
// port_memory.cpp, input_map.cpp, input_defaults.cpp, input_script.cpp, input_log.cpp,
// bot_link.cpp (with no bot), platform.cpp, hw/mmio.cpp, vr_config.cpp (the headset's
// default gamepad layout), settings.cpp and ready_file.cpp (a reset's ready_relaunching) are
// linked as they are. Stubbed: force feedback
// (ffb_configure, ffb_attach, ffb_update, ffb_shutdown; ffb_update keeps the motor it was
// given), the RTC (rtc_set_base), the event queue and interrupt request (event_schedule_in,
// irq_request), and the frame counters (gx::g_frames_submitted, gx_frames_submitted, and the
// presented frames, gx::present_count), which a check sets by hand. The renderer's aspect and internal scale (gx::render_set_output_aspect,
// gx::render_set_internal_scale) keep what they were last given. The interrupt handler's
// thread dump (debug_dump_threads) and guest memory (g_mem) are stubbed too, and fatal, which
// says why and exits 1, and the session's note that the game is starting again
// (session_relaunching), which notes it was called, and the clock's hold for a bot
// (clock_hold), which no check reaches. So are the end of a movie (record::finish) and the
// guest threads' quit (threads_request_quit), which only --restart's and --quit's ends of the
// run reach, and where the first resets the console on another thread for them
// (g_reset_in_finish), and the host clock (host_ns), which reads g_host_ns, moved by hand for
// the overlay's chord. reset.cpp is linked, for the game's reset (guest_reset). The disc
// speed the drive takes at boot (g_fast_disc, hw/di.cpp's) is defined here, fast as there,
// and a check sets it as settings.txt or --disc would and reads back what a replay left.
// With GCN_OVERLAY, overlay/*.cpp and pause.cpp are linked as well, and the clock they read
// (host_ns and clock_hold above, and audio_frames_out) is stubbed, as is the memory card's
// note of a hold (exi_card_held).
//
// The checks are in a file for each area, read in below after the frontend, as main.cpp is,
// so that they see its statics: none of them is compiled on its own. What more than one area
// uses is in tools/test_ports_support.h and .cpp.
//
// usage: test_ports <empty work dir>            the port checks
//        test_ports --record <empty work dir>   two gamepads, played and logged
//        test_ports --replay <that work dir>    that log replayed with none
//        test_ports --replay-no-sram <that work dir>   that log again, its SRAM snapshot gone
//        test_ports --replay-v4 <empty work dir>   a version 4 log written by hand, replayed
//        test_ports --replay-console <empty work dir>   logs recorded at disc console and at
//                                                       a speed it doesn't know, replayed
//                                                       with disc fast asked for
//        test_ports --interrupt <empty work dir>   the interrupt handler run beside the
//                                                  main loop, which ends the process
//        test_ports --no-gamepads <empty work dir>   the frontend under --no-gamepads, with
//                                                    a gamepad connected before it starts
//        test_ports --reset <empty work dir>   the game resetting the console during a
//                                              replay, with a gamepad rumbling
//        test_ports --shutdown <empty work dir>   the game shutting the console down,
//                                                 with a gamepad rumbling
//        test_ports --exit-held <empty work dir>   the same exit with the gamepads' lock
//                                                  held, as the menu's draw holds it
//        test_ports --restart <empty work dir>   a restart asked for (frontend_restart), to
//                                                the relaunch, with the game resetting the
//                                                console as the run ends
//        test_ports --quit <empty work dir>   the same with a quit
//        test_ports --restart-replay <empty work dir>   the menu, then a restart asked for,
//                                                       in a replay past its log's end
//        test_ports --pages <empty work dir>   each page of the menu dumped over a hidden GL
//                                              window, with its frame time; exit 77 with no
//                                              GL window or no overlay in the build
// Files are written into the work dir and left there.
//
// SDL's virtual joysticks stand in for controllers. Before anything starts SDL, main() turns
// off SDL's drivers that find real ones on macOS and Linux (kRealDriversOff) and clears
// SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD, which SDL reads ahead of the allow list
// (kSteamVirtualGamepads), to keep a controller connected beside the run from being opened,
// rumbled or read.
#define main gcn_frontend_main
#include "main.cpp"
#undef main
#include "hw/si.cpp"
#include "ffb.h"
#include "gx/render.h"
#include "test_ports_support.h"

static float g_output_aspect;  // what the frontend last gave the renderer, 0 before any
static int g_internal_scale;
static uint32_t g_presented;  // what gx::present_count says
namespace gx {
std::atomic<uint32_t> g_frames_submitted{0};
void render_set_output_aspect(float aspect) { g_output_aspect = aspect; }
void render_set_internal_scale(int scale) { g_internal_scale = scale; }
uint32_t present_count() { return g_presented; }
}
uint32_t gx_frames_submitted() { return gx::g_frames_submitted.load(); }
void rtc_set_base(time_t) {}
void event_schedule_in(uint64_t, EventFn) {}  // si_init's poll tick, which nothing here runs
void irq_request() {}  // what mmio.cpp's pi_update calls, which that tick would reach
static int g_ffb_motor = -1;  // the motor ffb_update was last given; -1 before any
void ffb_configure(const FfbSettings&) {}
void ffb_attach(SDL_Joystick*, int, float) {}
void ffb_update(bool motor) { g_ffb_motor = motor ? 1 : 0; }
void ffb_shutdown() {}
uint8_t* g_mem;  // on_interrupt's GCN_RAM_DUMP, which nothing here sets
void debug_dump_threads() {}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
static std::atomic<int> g_relaunching{0};  // how many times session_relaunching has been called
void session_relaunching() { g_relaunching++; }
static std::atomic<uint64_t> g_host_ns{0};  // the host clock, which a check moves by hand
uint64_t host_ns() { return g_host_ns; }
void clock_hold(uint64_t) {}
// Set, the end of a movie resets the console on another thread first, as a game may while
// end_run runs, and gives that reset time to act.
static std::atomic<bool> g_reset_in_finish{false};
namespace record {
void finish() {
    if (!g_reset_in_finish) return;
    std::thread(guest_reset, 0u, 0u).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}
}
void threads_request_quit() {}
bool g_fast_disc = true;
#ifdef GCN_OVERLAY
uint64_t audio_frames_out() { return 0; }
void exi_card_held(uint64_t) {}
void display_init(const OverlayPicture& picture);  // overlay.cpp's: the Display page's picture
// The menu's dumps (--pages). No frame is kept here, so a dump has only GCN_OVERLAY_PLAIN's grey
// to draw the menu over.
namespace gx {
const char* g_dump_dir;
bool g_dump_output;
bool render_present_last() { return false; }
void render_set_output_fbo(unsigned) {}
}
#endif

#include "test_ports_support.cpp"
#include "test_ports_placement.cpp"  // the port each gamepad takes: player indexes, --pads, launch
#include "test_ports_controls.cpp"   // controls.txt's devices and pins, and gamecontrollerdb.txt
#include "test_ports_memory.cpp"     // the ports remembered in saves/ports.txt
#include "test_ports_bindings.cpp"   // each gamepad's binding in saves/pads.txt
#include "test_ports_gamepads.cpp"   // each gamepad's lines in the output, its rumble and labels
#include "test_ports_flags.cpp"      // main()'s flags and variables, and --no-gamepads
#ifdef GCN_OVERLAY
#include "test_ports_menu_no_window.cpp"  // the menu with no window: what opens it, what it holds
// The Controllers checks come before the menu file, whose checks call port_changes,
// controllers_order, controllers_buttons and order_left_out.
#include "test_ports_controllers.cpp"     // the Controllers page and its notices
#include "test_ports_bindings_page.cpp"   // the Buttons view and its press-to-bind
#include "test_ports_menu_window.cpp"     // the menu over a window, then the Controllers checks
#include "test_ports_motion.cpp"          // the menu fading and growing in and out on the host clock
#include "test_ports_theme.cpp"           // the menu's theme and its style at a new window height
#include "test_ports_tabs.cpp"            // the menu's pages turned by shoulders, triggers and keys
#include "test_ports_pages.cpp"           // each page dumped over a GL window, and its frame time
#endif
#include "test_ports_replay.cpp"  // --record and the replays
#include "test_ports_exit.cpp"    // how a run ends: RunEnd, GCN_QUIT_AT and the modes that end it

namespace {

// SDL's drivers that find real controllers on macOS and Linux, off. HIDAPI, its use of
// libusb, IOKit and MFi have switches. Linux's evdev driver has none, so the gamepad allow
// list names only a USB vendor that's reserved (FFFF), which turns away every device a
// driver that consults it finds; SDL's virtual driver doesn't. main() sets these before
// anything starts SDL, at override priority, so the environment's values for them lose.
const struct {
    const char* hint;
    const char* value;
} kRealDriversOff[] = {
    {SDL_HINT_JOYSTICK_HIDAPI, "0"},
    {SDL_HINT_HIDAPI_LIBUSB, "0"},
    {SDL_HINT_JOYSTICK_IOKIT, "0"},
    {SDL_HINT_JOYSTICK_MFI, "0"},
    {SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xFFFF/0xFFFF"},
};

// Set, it lets Steam's virtual gamepads past the allow list. SDL reads it from the process
// environment rather than as a hint, ahead of the allow list, so main() clears it before
// anything starts SDL. tools/test_ports.sh sets it, so that drivers_stay_off sees it cleared.
const char* const kSteamVirtualGamepads = "SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD";

// ---- The hints main() sets win over the environment, where SDL_JOYSTICK_HIDAPI=1 or the
// like would otherwise turn a driver for real controllers back on, and the Steam variable
// tools/test_ports.sh sets is gone from where SDL reads it. No virtual gamepad can show the
// variable's effect: SDL's virtual driver never asks whether to ignore a device.
void drivers_stay_off() {
    SDL_Environment* env = SDL_GetEnvironment();
    bool off = true;
    for (const auto& h : kRealDriversOff) {
        SDL_SetEnvironmentVariable(env, h.hint, "1", true);
        const char* v = SDL_GetHint(h.hint);
        off = off && v && !strcmp(v, h.value);
        SDL_UnsetEnvironmentVariable(env, h.hint);
    }
    check(off, "the hints that turn off the drivers for real controllers beat the environment");
    check(!SDL_getenv_unsafe(kSteamVirtualGamepads),
          "the variable that lets Steam's virtual gamepads past the allow list is cleared before SDL starts");
}

int run_ports(const std::string& dir) {
    drivers_stay_off();
    if (!sdl_up()) return 1;
    no_controls(dir);
    // Port 2 and port 3 pressed B by script from frame 1000 to 1099.
    input_script_init("2/1000:B:100,3/1000:B:100");
    // Every section but the one on the order runs with remembered ports and a file of its
    // own to start from, so what it checks is what a first launch does.
    const std::string file = dir + "/saves/ports.txt";
    fresh_file(file);
    rules();
    gamepads_remember(nullptr);
    swap_order();
    fresh_file(file);
    launch(dir);
    fresh_file(file);
    pads2();
    fresh_file(file);
    named(dir);
    fresh_file(file);
    pins(dir);
    fresh_file(file);
    pins_more(dir);
    fresh_file(file);
    mappings(dir);
    fresh_file(file);
    gamepad_sources(dir);
    memory_order(file);
    memory_reversed(file);
    memory_twins(file);
    memory_missing(file);
    memory_pinned(dir, file);
    memory_held(dir, file);
    memory_file(dir, file);
    memory_no_pads(dir, file);
    memory_unreadable(dir, file);
    bindings_merged(dir);
    gamepads_remember(nullptr);
    said(dir);
    gamecube_rumble(dir);
    labels(dir);
    headset_layout();
    disc_speed(dir);
    disc_overlay_flags(dir);
    disc_overlay_missing(dir);
    bot_flags(dir);
    ram_patch_flags(dir);
    ram_patches_missing(dir);
    telemetry_flags();
    run_end(dir);
    quit_at_frame(dir);
    input_log_switch(dir);
#ifdef GCN_OVERLAY
    overlay(dir);
    overlay_buttons(dir);
    overlay_motion(dir);
    overlay_theme_fit(dir);
    overlay_panel_edge(dir);
    tabs_turn(dir);
    tabs_chord(dir);
    page_motion(dir);
    pill_click(dir);
#endif
    return finish("ports");
}

}  // namespace

int main(int argc, char** argv) {
    // Before anything starts SDL: its drivers that find real controllers off, and Steam's
    // virtual gamepads kept to the allow list.
    for (const auto& h : kRealDriversOff) SDL_SetHintWithPriority(h.hint, h.value, SDL_HINT_OVERRIDE);
    unsetenv(kSteamVirtualGamepads);
    if (argc == 2) return run_ports(argv[1]);
    if (argc == 3 && !strcmp(argv[1], "--record")) return run_record(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--replay")) return run_replay(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--replay-no-sram")) return run_replay_without_sram(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--replay-v4")) return run_replay_v4(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--replay-console")) return run_replay_console(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--interrupt")) return run_interrupt(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--no-gamepads")) return run_no_gamepads(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--reset")) return run_reset(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--shutdown")) return run_shutdown(argv[2], false);
    if (argc == 3 && !strcmp(argv[1], "--exit-held")) return run_shutdown(argv[2], true);
    if (argc == 3 && !strcmp(argv[1], "--restart")) return run_restart(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--quit")) return run_quit(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--restart-replay")) return run_restart_replay(argv[2]);
#ifdef GCN_OVERLAY
    if (argc == 3 && !strcmp(argv[1], "--pages")) return run_pages(argv[2]);
#else
    if (argc == 3 && !strcmp(argv[1], "--pages")) {
        printf("test_ports pages: skipped, the build has no overlay\n");
        return 77;
    }
#endif
    fprintf(stderr, "usage: test_ports [--record|--replay|--replay-no-sram|--replay-v4|--replay-console|--interrupt|"
                    "--no-gamepads|--reset|--shutdown|--exit-held|--restart|--quit|--restart-replay|--pages] <work dir>\n");
    return 2;
}
