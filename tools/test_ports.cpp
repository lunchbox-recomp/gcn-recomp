// Gamepad port checks on SDL's virtual joysticks: the `test_ports` target, run by
// tools/test_ports.sh.
//
// runtime/main.cpp is compiled into this file with its main() renamed, so a frame's input
// is the frontend's own pump_events / update_pads / merge_port, and runtime/hw/si.cpp too,
// so each port is read back the way the guest reads it: pad_get, with scripted presses
// (input_script.cpp) applied and the input log (input_log.cpp) consulted. gamepads.cpp,
// port_memory.cpp, input_map.cpp, input_defaults.cpp, input_script.cpp, input_log.cpp,
// bot_link.cpp (with no bot), platform.cpp, hw/mmio.cpp, vr_config.cpp (the headset's
// default gamepad layout) and settings.cpp are linked as they are. Stubbed: force feedback
// (ffb_configure, ffb_attach, ffb_update, ffb_shutdown; ffb_update keeps the motor it was
// given), the RTC (rtc_set_base), the event queue and interrupt request (event_schedule_in,
// irq_request), and the frame counter (gx::g_frames_submitted, gx_frames_submitted), which a
// check sets by hand. The renderer's aspect and internal scale (gx::render_set_output_aspect,
// gx::render_set_internal_scale) keep what they were last given. The interrupt handler's
// thread dump (debug_dump_threads) and guest memory (g_mem) are stubbed too, and fatal, which
// says why and exits 1, and the session's note that the game is starting again
// (session_relaunching), which notes it was called, and the clock's hold for a bot (host_ns,
// clock_hold), which no check reaches. So are the end of a movie (record::finish) and the
// guest threads' quit (threads_request_quit), which only --restart's and --quit's ends of the
// run reach, and where the first resets the console on another thread for them
// (g_reset_in_finish). reset.cpp is linked, for the game's reset (guest_reset). The disc
// speed the drive takes at boot (g_fast_disc, hw/di.cpp's) is defined here, fast as there,
// and a check sets it as settings.txt or --disc would and reads back what a replay left.
// With GCN_OVERLAY, overlay/*.cpp and pause.cpp are linked as well, and pause.cpp's clock
// (host_ns and clock_hold above, and audio_frames_out) is stubbed.
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
//        test_ports --restart <empty work dir>   a restart asked for (frontend_restart), to
//                                                the relaunch, with the game resetting the
//                                                console as the run ends
//        test_ports --quit <empty work dir>   the same with a quit
//        test_ports --restart-replay <empty work dir>   the menu, then a restart asked for,
//                                                       in a replay past its log's end
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
#include "vr_game.h"
#include <climits>
#include <condition_variable>
#include <functional>
#include <initializer_list>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static float g_output_aspect;  // what the frontend last gave the renderer, 0 before any
static int g_internal_scale;
namespace gx {
std::atomic<uint32_t> g_frames_submitted{0};
void render_set_output_aspect(float aspect) { g_output_aspect = aspect; }
void render_set_internal_scale(int scale) { g_internal_scale = scale; }
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
uint64_t host_ns() { return 0; }
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
void display_init(const OverlayPicture& picture);  // overlay.cpp's: the Display page's picture
#endif

namespace {

int g_checks, g_failed;

void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}

int finish(const char* what) {
    SDL_Quit();
    printf("test_ports %s: %d of %d checks passed\n", what, g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}

const int kSdlIndex = INT_MIN;  // attach(): keep the player index SDL gives

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

struct Virtual {
    const char* name;
    bool gamepad = true;
    SDL_JoystickID id = 0;
    int rumbles = 0;  // rumbles started on it
    int stops = 0;    // rumbles stopped on it
    int player = -1;  // the player index SDL last told the device
    bool can_rumble = true;  // false: it has no rumble, as SDL sees it
    bool refuses = false;    // a rumble asked of it fails, saying why
    const char* why = "virtual pad refuses rumble";
    Uint16 vendor = 0, product = 0;  // its USB IDs, from which SDL takes its type and labels
    Uint32 button_mask = 0;  // a gamepad's buttons, a bit for each SDL_GamepadButton; 0 is all
};

bool SDLCALL on_rumble(void* u, Uint16 low, Uint16 high) {
    Virtual* v = static_cast<Virtual*>(u);
    if (low || high) v->rumbles++;
    else v->stops++;
    return v->refuses ? SDL_SetError("%s", v->why) : true;
}

void SDLCALL on_player(void* u, int index) { static_cast<Virtual*>(u)->player = index; }

// The player index SDL has for `v`.
int player(const Virtual& v) { return SDL_GetJoystickPlayerIndexForID(v.id); }

// Plugs `v` in: a gamepad with every axis SDL has and every button, or those in its
// button_mask, in SDL's order, or a plain joystick with two axes and four buttons, which SDL
// has no mapping for. With `index`, its player index is set to that before the frontend hears
// of it, as Steam Input's slot or an XInput user index would be.
void attach(Virtual& v, int index = kSdlIndex) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = v.gamepad ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
    d.naxes = v.gamepad ? SDL_GAMEPAD_AXIS_COUNT : 2;
    d.nbuttons = !v.gamepad ? 4 : v.button_mask ? __builtin_popcount(v.button_mask) : SDL_GAMEPAD_BUTTON_COUNT;
    d.button_mask = v.button_mask;
    d.name = v.name;
    d.vendor_id = v.vendor;
    d.product_id = v.product;
    d.userdata = &v;
    d.Rumble = v.can_rumble ? on_rumble : nullptr;
    d.SetPlayerIndex = on_player;
    v.id = SDL_AttachVirtualJoystick(&d);
    if (!v.id) fprintf(stderr, "SDL_AttachVirtualJoystick: %s\n", SDL_GetError());
    if (index != kSdlIndex && v.id) {
        SDL_Joystick* j = SDL_OpenJoystick(v.id);
        SDL_SetJoystickPlayerIndex(j, index);
        SDL_CloseJoystick(j);
    }
}

void detach(Virtual& v) {
    if (!v.id) return;
    if (!SDL_DetachVirtualJoystick(v.id)) fprintf(stderr, "SDL_DetachVirtualJoystick: %s\n", SDL_GetError());
    v.id = 0;
}

// One frame of the desktop frontend: the window's events, then all four ports.
void frame() {
    pump_events();
    update_pads(0);
}

void unplug(std::initializer_list<Virtual*> vs) {
    for (Virtual* v : vs) detach(*v);
    frame();
}

// Port `i` (0 is port 1) as the guest reads it at presented frame `at`.
PadState read(int i, uint32_t at = 0) {
    gx::g_frames_submitted = at;
    const PadState p = pad_get(i);
    gx::g_frames_submitted = 0;
    return p;
}

bool at_rest(const PadState& p) {
    return !p.buttons && p.stick_x == 0x80 && p.stick_y == 0x80 && p.cstick_x == 0x80 && p.cstick_y == 0x80 &&
           !p.trig_l && !p.trig_r;
}

void press(const Virtual& v, int button, bool down) {
    if (SDL_Joystick* j = v.id ? SDL_GetJoystickFromID(v.id) : nullptr) SDL_SetJoystickVirtualButton(j, button, down);
    frame();
}

void push(const Virtual& v, int axis, Sint16 value) {
    if (SDL_Joystick* j = v.id ? SDL_GetJoystickFromID(v.id) : nullptr) SDL_SetJoystickVirtualAxis(j, axis, value);
    frame();
}

// The port `v` drives, 1 to 4, found by pressing its A and reading all four ports. 0 for
// none; -1 if more than one port saw it, or if the player index SDL has for the device,
// or the one it last told the device, is not that port's.
int port(const Virtual& v) {
    if (!v.id || !SDL_GetJoystickFromID(v.id)) return 0;
    press(v, SDL_GAMEPAD_BUTTON_SOUTH, true);
    int found = 0, n = 0;
    for (int i = 0; i < 4; i++)
        if (read(i).buttons & PAD_A) { found = i + 1; n++; }
    press(v, SDL_GAMEPAD_BUTTON_SOUTH, false);
    if (n > 1) return -1;
    if (!found) return 0;
    return player(v) == found - 1 && v.player == found - 1 ? found : -1;
}

// The game's poll command for port `i` with the rumble motor on, for one frame.
void motor(int i) {
    g_outbuf[i] = 0x00400001;
    frame();
    g_outbuf[i] = 0x00400000;
    frame();
}

// What main() does with --pads=N, and what the guest's power-on does with it (si_init):
// a copy of main()'s lines, not main() itself.
void power_on(int pads) {
    pad_set_count(pads);
    g_port_count = std::max(pads, input_map_port_count());
    pad_set_count(g_port_count);
    si_init();
}

bool write_file(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    fputs(text.c_str(), f);
    return fclose(f) == 0;
}

std::string read_file(const std::string& path) {
    std::string out;
    if (FILE* f = fopen(path.c_str(), "r")) {
        char line[512];
        while (fgets(line, sizeof(line), f)) out += line;
        fclose(f);
    }
    return out;
}

// Runs `f` with stderr going to `path`, then prints what it wrote.
void capture(const std::string& path, const std::function<void()>& f) {
    fflush(stderr);
    const int saved = dup(2);
    FILE* out = fopen(path.c_str(), "w");
    if (out) dup2(fileno(out), 2);
    f();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    if (out) fclose(out);
    printf("---- stderr\n%s----\n", read_file(path).c_str());
}

int lines_with(const std::string& path, const char* what) {
    int n = 0;
    const std::string text = read_file(path);
    for (size_t at = 0; (at = text.find(what, at)) != std::string::npos; at++) n++;
    return n;
}

bool sdl_up() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    printf("SDL %d.%d.%d (%s), platform \"%s\"\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
           SDL_GetRevision(), SDL_GetPlatform());
    for (int i = 0; i < 20; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    int found = 0;
    SDL_free(SDL_GetJoysticks(&found));
    if (found > 0) {
        printf("needs the ports to itself: disconnect the %d joystick(s) first\n", found);
        SDL_Quit();
        return false;
    }
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    return true;
}

std::string guid_of(const Virtual& v) {
    char guid[33];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(v.id), guid, sizeof(guid));
    return guid;
}

// How many mappings SDL has.
int mapping_count() {
    int n = 0;
    SDL_free(SDL_GetGamepadMappings(&n));
    return n;
}

// ---- The rules, with --pads=1, the default.
void rules() {
    power_on(1);
    gamepads_init();
    frame();
    check(read(0).connected && at_rest(read(0)) && !read(1).connected && !read(2).connected && !read(3).connected,
          "--pads=1, no gamepad: port 1 plugged in at rest, ports 2 to 4 not");

    Virtual a{"virtual pad a"}, b{"virtual pad b"}, c{"virtual pad c"}, d{"virtual pad d"}, e{"virtual pad e"},
        f{"virtual pad f"}, g{"virtual pad g"}, h{"virtual pad h"}, k{"virtual pad k"};
    attach(a);
    frame();
    check(port(a) == 1 && !read(1).connected && !read(2).connected && !read(3).connected,
          "a lone gamepad takes port 1 with index 0, and ports 2 to 4 stay unplugged");

    attach(b);
    const bool sdl01 = player(a) == 0 && player(b) == 1;
    frame();
    check(sdl01 && port(a) == 1 && port(b) == 2,
          "two gamepads SDL gives player indexes 0 and 1 land on ports 1 and 2, with those indexes");

    if (SDL_Joystick* jb = SDL_GetJoystickFromID(b.id)) {
        SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, true);
        SDL_SetJoystickVirtualAxis(jb, SDL_GAMEPAD_AXIS_LEFTX, 32767);
        frame();
        const PadState p2 = read(1);
        check(p2.connected && p2.buttons == PAD_A && p2.stick_x == 228 && at_rest(read(0)) && !read(2).connected &&
                  !read(3).connected,
              "port 2's gamepad presses A and pushes the stick on port 2 alone");
        SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, false);
        SDL_SetJoystickVirtualAxis(jb, SDL_GAMEPAD_AXIS_LEFTX, 0);
        frame();
    } else {
        check(false, "port 2's gamepad presses A and pushes the stick on port 2 alone (no joystick)");
    }

    motor(1);
    check(b.rumbles == 1 && a.rumbles == 0, "port 2's rumble motor runs port 2's gamepad alone");

    // While the guest is held (pause.h), the motor stays off, and one running stops.
    g_guest_held = true;
    motor(1);
    check(b.rumbles == 1, "port 2's motor turned on while the guest is held runs nothing");
    g_guest_held = false;
    g_outbuf[1] = 0x00400001;
    frame();
    const int stops = b.stops;
    g_guest_held = true;
    frame();
    check(b.rumbles == 2 && b.stops == stops + 1, "a running motor is stopped once the guest is held");
    g_guest_held = false;
    g_outbuf[1] = 0x00400000;
    frame();

    unplug({&b});
    check(!read(1).connected && port(a) == 1,
          "--pads=1: port 2's gamepad pulled out: port 2 reads unplugged, port 1 keeps its gamepad");

    attach(c, 2);
    frame();
    check(port(c) == 3 && !read(1).connected, "a gamepad with player index 2 lands on port 3, free, though port 2 is free too");

    motor(2);
    check(c.rumbles == 1 && a.rumbles == 0, "port 3's rumble motor runs port 3's gamepad alone");

    attach(d, 2);
    frame();
    check(port(d) == 2 && port(c) == 3,
          "a gamepad with player index 2 while port 3 has one lands on the lowest free port, 2; port 3's keeps port and index");
    unplug({&d});

    attach(e, -1);
    const bool unset = player(e) == -1;
    frame();
    check(unset && port(e) == 2 && read(1).connected && player(e) == 1 && e.player == 1,
          "a gamepad with no player index takes the lowest free port, 2 (4 free too), plugs it in again, and gets index 1");

    unplug({&a});
    const PadState out1 = read(0);
    attach(f);
    const int f_sdl = player(f);
    frame();
    check(out1.connected && at_rest(out1) && f_sdl == 0 && port(f) == 1 && port(e) == 2 && port(c) == 3,
          "port 1's gamepad pulled out: port 1 (the keyboard's, so still plugged in, at rest) is free, and the next gamepad takes it");

    attach(g);
    frame();
    attach(h);
    const int h_sdl = player(h);
    frame();
    check(port(g) == 4 && h_sdl == 4 && port(h) == 0 && player(h) == -1 && h.player == -1 && port(f) == 1 && port(e) == 2 &&
              port(c) == 3,
          "a fifth gamepad gets no port and its player index is cleared; the four keep their ports");

    // Left out stays out: nothing places it when a port frees, so port 3 reads unplugged
    // (--pads=1). Plugged in again, it is a new connection and takes port 3, SDL's index 2.
    unplug({&c});
    const PadState out3 = read(2);
    const bool h_out = player(h) == -1;
    detach(h);
    frame();
    attach(h);
    const int h_sdl2 = player(h);
    frame();
    check(!out3.connected && h_out && h_sdl2 == 2 && port(h) == 3 && port(f) == 1 && port(e) == 2 && port(g) == 4,
          "port 3's gamepad pulled out: the fifth stays left out and port 3 reads unplugged; plugged in again, it takes port 3");

    // A fifth that arrives holding port 2's index, as Steam or XInput could give it: SDL
    // moves port 2's gamepad to the next free index, 4, before the frontend hears of it.
    attach(k, 1);
    const bool e_moved = player(e) == 4;
    frame();
    check(e_moved && port(f) == 1 && port(e) == 2 && port(h) == 3 && port(g) == 4 && player(k) == -1 && k.player == -1,
          "a fifth gamepad holding port 2's index is left out with none, and the four keep index = port");
    unplug({&e, &f, &g, &h, &k});

    // Ports 1 and 4 taken, and a newcomer holding port 4's index: SDL moves port 4's
    // gamepad to index 1, the newcomer's lowest free port, and when the newcomer is given
    // index 1 SDL moves port 4's again, to 2.
    Virtual l{"virtual pad l"}, m{"virtual pad m"}, o{"virtual pad o"};
    attach(l);
    frame();
    attach(m, 3);
    frame();
    const bool l1m4 = port(l) == 1 && port(m) == 4;
    attach(o, 3);
    const bool m_moved = player(m) != 3;
    frame();
    check(l1m4 && m_moved && port(o) == 2 && port(m) == 4 && port(l) == 1,
          "with ports 1 and 4 taken, a gamepad holding port 4's index lands on port 2, and port 4's keeps index 3");
    unplug({&l, &m, &o});
    gamepads_shutdown();
}

// ---- Gamepads connected before gamepads_init: the same rule, in SDL's order, each
// placed once although SDL announces them again on the first frame.
void launch(const std::string& dir) {
    power_on(1);
    Virtual s1{"virtual pad s1"}, s2{"virtual pad s2"}, s3{"virtual pad s3"};
    attach(s1);
    attach(s2);
    attach(s3, 3);
    const std::string log3 = dir + "/startup3.txt";
    capture(log3, [] {
        gamepads_init();
        frame();
    });
    check(lines_with(log3, "\" on port ") == 3 && port(s1) == 1 && port(s2) == 2 && port(s3) == 4 && !read(2).connected,
          "three gamepads at launch, the third with player index 3: ports 1, 2 and 4, each placed once");
    unplug({&s1, &s2, &s3});
    gamepads_shutdown();

    Virtual t1{"virtual pad t1"}, t2{"virtual pad t2"}, t3{"virtual pad t3"}, t4{"virtual pad t4"}, t5{"virtual pad t5"};
    for (Virtual* v : {&t1, &t2, &t3, &t4, &t5}) attach(*v);
    const std::string log5 = dir + "/startup5.txt";
    capture(log5, [] {
        gamepads_init();
        frame();
    });
    check(lines_with(log5, "\" on port ") == 4 && lines_with(log5, "left out") == 1 && port(t1) == 1 && port(t2) == 2 &&
              port(t3) == 3 && port(t4) == 4 && port(t5) == 0 && player(t5) == -1,
          "five gamepads at launch: four on ports 1 to 4 and one left out, each said once");
    unplug({&t1, &t2, &t3, &t4, &t5});
    gamepads_shutdown();

    // A first gamepad with no player index, as one is that a gamecontrollerdb.txt maps, and
    // the next two with indexes 0 and 1. They are placed together, so each index is read before
    // any placement moves another's. The second and third land on ports 2 and 3, in SDL's order.
    power_on(1);
    Virtual u1{"virtual pad u1"}, u2{"virtual pad u2"}, u3{"virtual pad u3"};
    attach(u1, -1);
    attach(u2);
    attach(u3);
    const bool indexes = player(u1) == -1 && player(u2) == 0 && player(u3) == 1;
    gamepads_init();
    frame();
    check(indexes && port(u1) == 1 && port(u2) == 2 && port(u3) == 3,
          "three gamepads at launch, the first with no player index and the others holding 0 and 1: ports 1, 2 and 3 in SDL's order");
    unplug({&u1, &u2, &u3});
    gamepads_shutdown();
}

// ---- --pads=2.
void pads2() {
    // Read first before any frame: what si_init plugged in at power-on, since the last
    // frame, under --pads=1, left port 2 unplugged.
    power_on(1);
    frame();
    const bool was_out = !read(1).connected;
    power_on(2);
    check(was_out && read(0).connected && read(1).connected && at_rest(read(1)) && !read(2).connected && !read(3).connected,
          "--pads=2, before the first frame: ports 1 and 2 plugged in at rest, 3 and 4 not");
    gamepads_init();
    frame();
    check(read(0).connected && read(1).connected && at_rest(read(1)) && !read(2).connected && !read(3).connected,
          "--pads=2, no gamepad: ports 1 and 2 plugged in at rest, 3 and 4 not");
    check(read(1, 1000).buttons == PAD_B && !read(2, 1000).connected && !read(2, 1000).buttons,
          "--pads=2, no gamepad: a scripted press on port 2 lands, one on port 3 does nothing");

    Virtual h1{"virtual pad h1"}, h2{"virtual pad h2"}, h3{"virtual pad h3"}, h4{"virtual pad h4"};
    attach(h1);
    attach(h2);
    frame();
    check(port(h1) == 1 && port(h2) == 2, "--pads=2: two gamepads take ports 1 and 2");

    press(h2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    const PadState both = read(1, 1000);
    press(h2, SDL_GAMEPAD_BUTTON_SOUTH, false);
    check(both.connected && both.buttons == (PAD_A | PAD_B), "--pads=2: port 2's gamepad and a scripted press on port 2 both reach the guest");

    attach(h3);
    frame();
    check(port(h3) == 3 && read(2).connected && read(2, 1000).buttons == PAD_B,
          "--pads=2: a third gamepad plugs port 3 in, and the scripted press on port 3 now lands");

    unplug({&h2});
    check(read(1).connected && at_rest(read(1)) && read(1, 1000).buttons == PAD_B && port(h1) == 1 && port(h3) == 3,
          "--pads=2: port 2's gamepad pulled out: port 2 stays plugged in at rest, and the script still presses it");
    unplug({&h3});
    check(!read(2).connected && !read(2, 1000).buttons,
          "--pads=2: port 3's gamepad pulled out: port 3 reads unplugged, and the script's press there does nothing again");
    attach(h4);
    frame();
    check(port(h4) == 2, "--pads=2: the next gamepad (SDL index 1, freed with port 2) takes port 2 and drives it");
    unplug({&h1, &h4});
    gamepads_shutdown();
}

// ---- controls.txt naming a gamepad.
void named(const std::string& dir) {
    const std::string controls = dir + "/controls.txt";
    Virtual bound{"virtual pad bound"};

    // One it names connects holding port 1's gamepad's index: SDL moves the gamepad's, and
    // it gets index 0 back although the newcomer takes no port.
    write_file(controls, "A = \"virtual pad named\" button 0\n");
    input_map_load(controls.c_str());
    power_on(1);
    gamepads_init();
    Virtual pa{"virtual pad pa"}, named{"virtual pad named"};
    attach(pa);
    frame();
    attach(named, 0);
    const bool pa_moved = player(pa) != 0;
    frame();
    check(pa_moved && port(pa) == 1,
          "a device controls.txt names connects holding port 1's gamepad's index: port 1's gets index 0 back");
    unplug({&pa, &named});
    gamepads_shutdown();

    write_file(controls, "A = \"virtual pad bound\" button 0\n");
    input_map_load(controls.c_str());
    power_on(1);
    gamepads_init();
    attach(bound);
    frame();
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, true);
    const PadState own = read(0);
    const bool own_alone = !read(1).buttons && !read(2).buttons && !read(3).buttons;
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, false);
    press(bound, SDL_GAMEPAD_BUTTON_WEST, true);
    bool mapped = false;
    for (int i = 0; i < 4; i++) mapped = mapped || read(i).buttons;
    press(bound, SDL_GAMEPAD_BUTTON_WEST, false);
    check(own.buttons == PAD_A && own_alone && !mapped,
          "a gamepad controls.txt names gets no port: its button 0 is A on port 1 by the file, and its B reaches no port");
    unplug({&bound});
    gamepads_shutdown();
    input_map_load((dir + "/no-controls.txt").c_str());
}

// ---- A `port <n>` line pins its device to the port. A device line with port 3, and a
// key line with port 5, which is refused.
void pins(const std::string& dir) {
    const std::string controls = dir + "/controls.txt";
    Virtual bound{"virtual pad bound"};
    power_on(1);  // port 3 starts out unplugged, whatever the section before left
    frame();
    write_file(controls, "A = \"virtual pad bound\" button 0 port 3\nB = key X port 5\n"
                         "stick_x = \"virtual pad bound\" axis 0 port 3\n");
    input_map_load(controls.c_str());
    const bool port3_was_out = !read(2).connected;
    power_on(1);
    const bool before_frame = read(1).connected && read(2).connected && !read(3).connected;
    gamepads_init();
    frame();
    check(port3_was_out && before_frame && input_map_port_count() == 3 && read(1).connected && read(2).connected &&
              !read(3).connected,
          "controls.txt pins a device to port 3 (a port 5 line refused): ports 1 to 3 plugged in, before the first frame too");
    Virtual x{"virtual pad x"}, y{"virtual pad y"};
    attach(bound);
    const int bound_sdl = player(bound);
    frame();
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, true);
    const PadState p3 = read(2);
    const bool alone = !read(0).buttons && !read(1).buttons && !read(3).buttons;
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, false);
    check(bound_sdl == 0 && p3.buttons == PAD_A && alone && player(bound) == 2 && bound.player == 2,
          "a gamepad pinned to port 3, given index 0 by SDL, drives port 3 alone, and its player index follows the pin to 2");
    attach(x);
    frame();
    check(port(x) == 1, "the next gamepad (SDL index 0) takes port 1");
    attach(y, 2);
    frame();
    check(port(y) == 2 && player(bound) == 2 && bound.player == 2,
          "a gamepad with player index 2 does not take pinned port 3: it lands on port 2, and the pinned device keeps index 2");
    g_outbuf[2] = 0x00400001;
    frame();
    const int ffb_on3 = g_ffb_motor;
    g_outbuf[2] = 0x00400000;
    g_outbuf[0] = 0x00400001;
    frame();
    const int ffb_on1 = g_ffb_motor;
    g_outbuf[0] = 0x00400000;
    frame();
    check(ffb_on3 == 1 && ffb_on1 == 0 && x.rumbles == 1 && y.rumbles == 0,
          "the wheel's force feedback follows port 3's motor, the steering line's port, and port 1's motor runs port 1's gamepad");

    // Ports 1, 2 and 4 taken and port 3 pinned: a gamepad holding index 2 is left out, and
    // only the pin's re-set on another device's arrival gives the pinned device index 2 back.
    Virtual q4{"virtual pad q4"}, q5{"virtual pad q5"};
    attach(q4);
    frame();
    const bool q4_on4 = port(q4) == 4;
    attach(q5, 2);
    const bool bound_moved = player(bound) != 2;
    frame();
    check(q4_on4 && bound_moved && player(q5) == -1 && player(bound) == 2 && bound.player == 2 && port(x) == 1 &&
              port(y) == 2 && port(q4) == 4,
          "ports 1, 2 and 4 taken and port 3 pinned: a gamepad holding index 2 is left out, and the pinned device gets 2 back");
    unplug({&bound, &x, &y, &q4, &q5});
    gamepads_shutdown();
}

// ---- The other pin scenarios, each with its own controls.txt and pads.
void pins_more(const std::string& dir) {
    const std::string controls = dir + "/controls.txt";
    Virtual bound{"virtual pad bound"}, x{"virtual pad x"}, y{"virtual pad y"}, z{"virtual pad z"};

    // At launch, the pinned device first in SDL's list, and a gamepad whose index is the
    // pinned port's.
    write_file(controls, "A = \"virtual pad bound\" button 0 port 2\n");
    input_map_load(controls.c_str());
    power_on(1);
    attach(bound);
    attach(z);
    const bool sdl_order = player(bound) == 0 && player(z) == 1;
    gamepads_init();
    frame();
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, true);
    const PadState p2 = read(1);
    const bool alone2 = !read(0).buttons && !read(2).buttons && !read(3).buttons;
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, false);
    check(sdl_order && port(z) == 1 && p2.buttons == PAD_A && alone2 && player(bound) == 1 && bound.player == 1,
          "at launch, a gamepad with the pinned port's index 1 takes port 1 instead; the device pinned to port 2 drives it with index 1");
    unplug({&bound, &z});
    gamepads_shutdown();

    // `port 1` pins port 1 too: a line that says it keeps gamepads off port 1.
    write_file(controls, "A = \"virtual pad bound\" button 0 port 1\n");
    input_map_load(controls.c_str());
    power_on(1);
    gamepads_init();
    attach(bound);
    frame();
    attach(x);
    const bool x_sdl = player(x) == 1;
    frame();
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, true);
    const PadState pin1 = read(0);
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, false);
    attach(y, 0);
    frame();
    check(x_sdl && pin1.buttons == PAD_A && port(x) == 2 && port(y) == 3 && player(bound) == 0 && bound.player == 0,
          "`port 1` pins port 1: the pinned device drives it with index 0, and gamepads, even one holding index 0, take ports 2 and 3");
    unplug({&bound, &x, &y});
    gamepads_shutdown();

    // Two plain joysticks pinned to port 2, as a wheel and its pedals would be: the first
    // shows index 1 and the second none, so no slot is taken that a gamepad needs.
    write_file(controls, "A = \"virtual stick w\" button 0 port 2\nB = \"virtual stick p\" button 1 port 2\n");
    input_map_load(controls.c_str());
    power_on(1);
    gamepads_init();
    Virtual sw{"virtual stick w", false}, sp{"virtual stick p", false}, gp{"virtual pad gp"};
    attach(sw);
    frame();
    attach(sp);
    frame();
    const bool first_shows = player(sw) == 1 && sw.player == 1 && player(sp) == -1;
    attach(gp);
    frame();
    check(first_shows && port(gp) == 1 && player(sw) == 1 && player(sp) == -1,
          "two plain joysticks pinned to port 2: the first has index 1, the second none, and a gamepad then takes port 1");
    unplug({&sw, &sp, &gp});
    gamepads_shutdown();

    // An unpinned plain joystick first, then a gamepad: the joystick is no gamepad, so it
    // does not push the lone gamepad off port 1. SDL gives a plain joystick no player index
    // (-1) and the gamepad index 0, which is why: the joystick holds no index to take.
    input_map_load((dir + "/no-controls.txt").c_str());
    power_on(1);
    gamepads_init();
    Virtual loose{"virtual stick loose", false}, lone{"virtual pad lone"};
    attach(loose);
    frame();
    attach(lone);
    const int loose_index = player(loose);
    const int lone_index = player(lone);
    frame();
    check(loose_index == -1 && lone_index == 0 && port(lone) == 1,
          "an unpinned plain joystick attached first does not push a lone gamepad off port 1");
    unplug({&loose, &lone});
    gamepads_shutdown();

    // A port number past int range must not wrap into 1 to 4: the whole line is refused.
    write_file(controls, "A = \"virtual pad bound\" button 0 port 4294967299\n");
    input_map_load(controls.c_str());
    power_on(1);
    gamepads_init();
    attach(bound);
    frame();
    check(input_map_port_count() == 1 && !input_map_pins(0) && !input_map_pins(1) && !input_map_pins(2) &&
              !input_map_pins(3) && port(bound) == 1 && player(bound) == 0,
          "a controls.txt line with port 4294967299 is refused: no port is pinned and none is plugged in");
    unplug({&bound});
    gamepads_shutdown();

    // A device number past int range must not wrap into a real one: the name matches no device.
    write_file(controls, "A = \"virtual pad bound #4294967297\" button 0\n");
    input_map_load(controls.c_str());
    power_on(1);
    gamepads_init();
    attach(bound);
    frame();
    check(!input_map_binds(bound.id) && port(bound) == 1,
          "a controls.txt device named \"#4294967297\" names no device, so the first one is not bound");
    unplug({&bound});
    gamepads_shutdown();

    // A key line's port plugs the port in but pins nothing: a gamepad holding its index
    // still takes it.
    write_file(controls, "B = key X port 3\n");
    input_map_load(controls.c_str());
    power_on(1);
    const bool key_plugged = read(1).connected && read(2).connected && !read(3).connected;
    gamepads_init();
    Virtual k1{"virtual pad k1"}, k2{"virtual pad k2"}, k3{"virtual pad k3"};
    attach(k1, 0);
    attach(k2, 1);
    attach(k3, 2);
    frame();
    check(key_plugged && port(k1) == 1 && port(k2) == 2 && port(k3) == 3,
          "a key line with port 3 plugs ports 1 to 3 in before the first frame, but pins nothing: a gamepad with index 2 takes port 3");
    unplug({&k1, &k2, &k3});
    gamepads_shutdown();
    input_map_load((dir + "/no-controls.txt").c_str());
}

// ---- gamecontrollerdb.txt beside the executable, the one mappings file the frontend
// loads (SDL_GAMECONTROLLERCONFIG_FILE is SDL's own, read at SDL_Init).
void mappings(const std::string& dir) {
    power_on(1);
    const char* base = SDL_GetBasePath();
    const std::string beside = std::string(base ? base : "./") + "gamecontrollerdb.txt";
    if (plat_readable(beside.c_str())) {
        printf("%s exists: not touching it\n", beside.c_str());
        check(false, (beside + " is there already: the mappings checks did not run; move it away and run again").c_str());
        return;
    }
    Virtual stick{"virtual stick", false};
    attach(stick);
    frame();
    const std::string guid = guid_of(stick);
    const std::string platform = SDL_GetPlatform();
    const int before = mapping_count();

    const std::string quiet = dir + "/mappings-none.txt";
    capture(quiet, [] { gamepads_load_mappings(); });
    check(mapping_count() == before && !SDL_IsGamepad(stick.id) && read_file(quiet).empty(),
          "no gamecontrollerdb.txt beside the executable: nothing loaded, nothing said");

    write_file(beside, "");
    gamepads_load_mappings();
    check(mapping_count() == before && !SDL_IsGamepad(stick.id), "an empty gamecontrollerdb.txt changes nothing");

    write_file(beside, "this is not a mapping\n"
                       "platform:" + platform + ",\n" +
                       guid + "platform:" + platform + ",\n" +
                       guid + ",virtual stick,a:b0,b:b1,leftx:a0,lefty:a1,\n" +
                       guid + ",virtual stick,a:b0,b:b1,leftx:a0,lefty:a1,platform:Windows,\n" +
                       guid + ",virtual stick,a:b0,b:b1,leftx:a0,lefty:a1,platform:" + platform);
    gamepads_load_mappings();
    check(mapping_count() == before && !SDL_IsGamepad(stick.id),
          "a malformed gamecontrollerdb.txt (no fields, no platform, another platform, a cut-off line) changes nothing");

    // A line for this platform, then one for Windows with the same GUID, which SDL must skip
    // (it would replace the first, and its mapping has no b:b1).
    write_file(beside, "# made up for this test\n" + guid + ",virtual stick,a:b0,b:b1,x:b2,y:b3,leftx:a0,lefty:a1,platform:" +
                           platform + ",\n" + guid + ",virtual stick,a:b0,platform:Windows,\n");
    gamepads_load_mappings();
    char* mapping = SDL_GetGamepadMappingForGUID(SDL_GetJoystickGUIDForID(stick.id));
    const bool this_platform = mapping && std::string(mapping).find("b:b1") != std::string::npos;
    SDL_free(mapping);
    gamepads_init();
    frame();
    check(mapping_count() == before + 1 && this_platform && SDL_IsGamepad(stick.id) && port(stick) == 1,
          "a joystick SDL has no mapping for, mapped by gamecontrollerdb.txt (its line for this platform alone) before the "
          "gamepads are taken, gets port 1");
    // Copied, then removed: a rename fails when the work dir is on another filesystem.
    const bool kept = write_file(dir + "/gamecontrollerdb.beside.txt", read_file(beside));
    check(kept && std::remove(beside.c_str()) == 0,
          "the test's gamecontrollerdb.txt is copied to the work dir and removed from beside the executable");
    unplug({&stick});
    gamepads_shutdown();
}

// Two gamepads pulled out and plugged back in the other order swap ports: each takes the
// port of the index SDL gives it.
void swap_order() {
    power_on(1);
    gamepads_init();
    Virtual sa{"virtual pad swap a"}, sb{"virtual pad swap b"};
    attach(sa);
    frame();
    attach(sb);
    frame();
    const bool before = port(sa) == 1 && port(sb) == 2;
    unplug({&sa, &sb});
    attach(sb);
    frame();
    attach(sa);
    frame();
    check(before && port(sb) == 1 && port(sa) == 2,
          "two gamepads pulled out and plugged back in the other order swap ports");
    unplug({&sa, &sb});
    gamepads_shutdown();
}

std::string describe(const PadState& p) {
    char s[64];
    snprintf(s, sizeof(s), "%d %04X %02X %02X %02X %02X %02X %02X\n", p.connected ? 1 : 0, p.buttons, p.stick_x, p.stick_y,
             p.cstick_x, p.cstick_y, p.trig_l, p.trig_r);
    return s;
}

// ---- Remembered ports. SDL's virtual joysticks have a GUID made from their name and USB IDs
// of 0. They have no serial, so what a serial adds is checked on its own in test_port_memory.

// The file's identity. A write renames a new file over the old one, so its inode changes
// with every write and with nothing else. 0 for no file.
uint64_t file_id(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (uint64_t)st.st_ino : 0;
}

bool exists_path(const std::string& path) { return file_id(path) != 0; }

bool has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

// The line the file has for `v` on `port` (1 to 4). The virtual pads have no serial and no USB IDs.
std::string line_for(int port, const Virtual& v) { return "\n" + std::to_string(port) + " " + guid_of(v) + " - 0000:0000\n"; }

// A launch from nothing. The file is gone, and it's read again at the first gamepad.
void fresh_file(const std::string& file) {
    remove(file.c_str());
    gamepads_remember(file.c_str());
}

// A new launch. The gamepads are let go, and the file is read again at the first gamepad.
void relaunch() {
    gamepads_shutdown();
    power_on(1);
}

// Gamepads pulled out and plugged in the other order get their saved ports back. Pulling
// them out writes nothing, and neither does plugging them back in.
void memory_order(const std::string& file) {
    fresh_file(file);
    power_on(1);
    gamepads_init();
    Virtual a{"virtual pad ma"}, b{"virtual pad mb"};
    attach(a);
    frame();
    attach(b);
    frame();
    const bool first = port(a) == 1 && port(b) == 2;
    const std::string text = read_file(file);
    check(first && has(text, line_for(1, a)) && has(text, line_for(2, b)) && has(text, "\n3 none\n"),
          "two gamepads take ports 1 and 2, and ports.txt names them there");
    const uint64_t written = file_id(file);

    unplug({&a, &b});
    check(file_id(file) == written, "pulling both out writes nothing");
    attach(b);
    frame();
    attach(a);
    frame();
    check(port(b) == 2 && port(a) == 1 && file_id(file) == written,
          "plugged back in the other order, each gets its saved port, and nothing is written");
    unplug({&a, &b});

    relaunch();
    attach(b);
    attach(a);
    gamepads_init();
    frame();
    check(port(b) == 2 && port(a) == 1 && file_id(file) == written,
          "at the next launch, listed in the other order, they get their saved ports, and nothing is written");
    unplug({&a, &b});
    gamepads_shutdown();
}

// Four gamepads connecting in the reverse order, which asks every player index to move.
void memory_reversed(const std::string& file) {
    fresh_file(file);
    power_on(1);
    gamepads_init();
    Virtual p1{"virtual pad r1"}, p2{"virtual pad r2"}, p3{"virtual pad r3"}, p4{"virtual pad r4"};
    for (Virtual* v : {&p1, &p2, &p3, &p4}) {
        attach(*v);
        frame();
    }
    const bool in_order = port(p1) == 1 && port(p2) == 2 && port(p3) == 3 && port(p4) == 4;
    unplug({&p1, &p2, &p3, &p4});
    relaunch();
    for (Virtual* v : {&p4, &p3, &p2, &p1}) attach(*v);
    gamepads_init();
    frame();
    check(in_order && port(p1) == 1 && port(p2) == 2 && port(p3) == 3 && port(p4) == 4,
          "four gamepads that connect in the reverse order at launch each get their saved port and player index");
    unplug({&p1, &p2, &p3, &p4});
    relaunch();
    for (Virtual* v : {&p3, &p1, &p4, &p2}) {
        attach(*v);
        frame();
    }
    check(port(p1) == 1 && port(p2) == 2 && port(p3) == 3 && port(p4) == 4,
          "and plugged in one at a time in a shuffled order, the same");
    unplug({&p1, &p2, &p3, &p4});
    gamepads_shutdown();
}

// Two gamepads of one model have the same GUID and no serial, so they can't be told apart.
// They fill their ports in the order they connect.
void memory_twins(const std::string& file) {
    fresh_file(file);
    power_on(1);
    gamepads_init();
    Virtual t1{"virtual pad twin"}, t2{"virtual pad twin"};
    attach(t1);
    frame();
    attach(t2);
    frame();
    const uint64_t written = file_id(file);
    check(port(t1) == 1 && port(t2) == 2 && has(read_file(file), line_for(1, t1)) && has(read_file(file), line_for(2, t2)),
          "two gamepads with the same GUID take ports 1 and 2");
    unplug({&t1, &t2});
    relaunch();
    attach(t2);
    frame();
    attach(t1);
    frame();
    check(port(t2) == 1 && port(t1) == 2 && file_id(file) == written,
          "and when they come back, the first to connect gets port 1: the file can't tell them apart, and says the same");
    unplug({&t1, &t2});
    gamepads_shutdown();
}

// A saved port isn't a reservation. Its gamepad is away, and another takes it. The
// gamepad that comes back holding port 1's player index doesn't move the one on port 1.
void memory_missing(const std::string& file) {
    fresh_file(file);
    power_on(1);
    gamepads_init();
    Virtual a{"virtual pad away"};
    attach(a);
    frame();
    const bool on_one = port(a) == 1;
    const std::string away = guid_of(a);
    unplug({&a});
    relaunch();
    Virtual c{"virtual pad cover"};
    attach(c);
    frame();
    const std::string text = read_file(file);
    check(on_one && port(c) == 1 && has(text, line_for(1, c)) && !has(text, away),
          "port 1's saved gamepad is away: another takes port 1, and the file now names that one there");
    attach(a, 0);
    frame();
    check(port(c) == 1 && port(a) == 2,
          "the first comes back holding port 1's player index and finds port 1 taken: it takes port 2, and the one on port 1 stays");
    unplug({&a, &c});
    gamepads_shutdown();
}

// A port controls.txt pins beats the file. The gamepad saved there takes the lowest free
// port, not the port of its player index.
void memory_pinned(const std::string& dir, const std::string& file) {
    fresh_file(file);
    power_on(1);
    gamepads_init();
    Virtual d{"virtual pad pinned away"};
    attach(d, 2);
    frame();
    const bool on_three = port(d) == 3;
    unplug({&d});
    relaunch();

    const std::string controls = dir + "/controls.txt";
    write_file(controls, "A = \"virtual pad pinning\" button 0 port 3\n");
    input_map_load(controls.c_str());
    power_on(1);
    gamepads_init();
    attach(d, 1);
    frame();
    const bool one = port(d) == 1;
    check(on_three && one && has(read_file(file), line_for(1, d)),
          "port 3 saved for a gamepad and pinned by controls.txt: it takes the lowest free port, 1, though its player index says 2");
    unplug({&d});
    gamepads_shutdown();
    input_map_load((dir + "/no-controls.txt").c_str());
}

// A gamepad on a port keeps it, though the file also names it on a lower port that's free
// by now. The gamepad landed on port 2 because controls.txt pinned port 1 then, and the pin
// is gone when the next gamepad asks for port 2.
void memory_held(const std::string& dir, const std::string& file) {
    const std::string controls = dir + "/controls.txt";
    fresh_file(file);
    write_file(controls, "A = \"virtual pad nobody\" button 0 port 1\n");
    input_map_load(controls.c_str());
    power_on(1);
    Virtual held{"virtual pad held"}, next{"virtual pad next"};
    attach(held);
    write_file(file, "1 " + guid_of(held) + " - 0000:0000\n");
    gamepads_init();
    frame();
    const bool on_two = port(held) == 2 && has(read_file(file), line_for(2, held));
    input_map_load((dir + "/no-controls.txt").c_str());
    attach(next, 1);
    frame();
    check(on_two && port(held) == 2 && port(next) == 1,
          "a gamepad saved on port 1 but put on port 2 by a pin keeps port 2, and a newcomer holding index 1 takes port 1");
    unplug({&held, &next});
    gamepads_shutdown();
}

// A gamepad left out for want of a port isn't saved. A file of lines that aren't ports is
// replaced when a gamepad lands.
void memory_file(const std::string& dir, const std::string& file) {
    fresh_file(file);
    power_on(1);
    gamepads_init();
    Virtual g[5] = {{"virtual pad f1"}, {"virtual pad f2"}, {"virtual pad f3"}, {"virtual pad f4"}, {"virtual pad f5"}};
    for (Virtual& v : g) {
        attach(v);
        frame();
    }
    const std::string text = read_file(file);
    check(port(g[4]) == 0 && has(text, line_for(4, g[3])) && !has(text, guid_of(g[4])),
          "a fifth gamepad is left out and ports.txt doesn't name it");
    unplug({&g[0], &g[1], &g[2], &g[3], &g[4]});
    gamepads_shutdown();

    write_file(file, "this is not a port\n9 none\n1 " + std::string(32, 'a') + " - 0000:0000 more\n");
    gamepads_remember(file.c_str());
    power_on(1);
    gamepads_init();
    const std::string said = dir + "/memory-bad-stderr.txt";
    Virtual v{"virtual pad after the bad file"};
    capture(said, [&] {
        attach(v);
        frame();
    });
    check(lines_with(said, "so the line is skipped") == 3 && port(v) == 1 && has(read_file(file), line_for(1, v)),
          "a file of lines that aren't ports is read as empty with each line said, the gamepad takes port 1, and the file is written whole");
    unplug({&v});
    gamepads_shutdown();
}

// A launch with no gamepad connected reads nothing and writes nothing. Each port reads as
// it does with no memory at all.
void memory_no_pads(const std::string& dir, const std::string& file) {
    // The ports as the guest reads them over a few frames.
    auto ports_seen = [] {
        std::string out;
        for (int s = 0; s < 4; s++) {
            frame();
            for (int i = 0; i < 4; i++) out += describe(read(i));
        }
        return out;
    };

    gamepads_remember(nullptr);
    power_on(1);
    gamepads_init();
    const std::string plain = ports_seen();
    gamepads_shutdown();

    // With no file, nothing is made. That includes its directory.
    fresh_file(file);
    rmdir((dir + "/saves").c_str());
    const std::string quiet = dir + "/memory-none-stderr.txt";
    std::string with_none;
    capture(quiet, [&] {
        power_on(1);
        gamepads_init();
        with_none = ports_seen();
    });
    check(with_none == plain && !exists_path(file) && !exists_path(dir + "/saves") && read_file(quiet).empty(),
          "no gamepad and no ports.txt: the ports read as with no memory, and no file or directory is made");
    gamepads_shutdown();

    // A file with ports in it that can't even be opened. A launch that tried to read it would say so.
    plat_make_dirs(dir + "/saves");
    write_file(file, "1 " + std::string(32, 'a') + " - 0000:0000\n");
    const uint64_t before = file_id(file);
    const std::string kept = read_file(file);
    gamepads_remember(file.c_str());
    const bool locked = chmod(file.c_str(), 0) == 0 && access(file.c_str(), R_OK) != 0;
    const std::string loud = dir + "/memory-locked-stderr.txt";
    std::string with_file;
    capture(loud, [&] {
        power_on(1);
        gamepads_init();
        with_file = ports_seen();
    });
    chmod(file.c_str(), 0644);
    if (!locked) printf("this account reads files whatever their mode: the unreadable-file check is weaker here\n");
    check(with_file == plain && read_file(loud).empty() && file_id(file) == before && read_file(file) == kept,
          "no gamepad and a ports.txt that can't be read: it isn't opened, nothing is said, and the file is as it was");
    gamepads_shutdown();

    // The same with the file readable.
    gamepads_remember(file.c_str());
    power_on(1);
    gamepads_init();
    const std::string readable = ports_seen();
    check(readable == plain && file_id(file) == before, "with a readable ports.txt and no gamepad, the file isn't rewritten");
    gamepads_shutdown();
}

// A ports.txt that exists and can't be read isn't saved over. The gamepads still land, with
// nothing remembered, and the file is as it was when they have.
void memory_unreadable(const std::string& dir, const std::string& file) {
    plat_make_dirs(dir + "/saves");
    write_file(file, "1 " + std::string(32, 'a') + " - 0000:0000\n");
    const uint64_t before = file_id(file);
    const std::string kept = read_file(file);
    if (chmod(file.c_str(), 0) != 0 || access(file.c_str(), R_OK) == 0) {
        chmod(file.c_str(), 0644);
        printf("this account reads files whatever their mode, so the unreadable ports.txt check didn't run\n");
        return;
    }
    gamepads_remember(file.c_str());
    power_on(1);
    gamepads_init();
    Virtual a{"virtual pad locked a"}, b{"virtual pad locked b"};
    const std::string said = dir + "/memory-unreadable-stderr.txt";
    capture(said, [&] {
        attach(a);
        frame();
        attach(b);
        frame();
    });
    chmod(file.c_str(), 0644);
    check(port(a) == 1 && port(b) == 2 && lines_with(said, "can't read it") == 1 && file_id(file) == before &&
              read_file(file) == kept && !exists_path(file + ".tmp"),
          "a ports.txt that can't be read is said once, the gamepads still take ports 1 and 2, and nothing is written");
    unplug({&a, &b});
    gamepads_shutdown();
    remove(file.c_str());
}

// ---- What the run's output says of each gamepad: what SDL knows of it as it lands on a
// port, what it sent while there when it leaves (pulled out, or still there at shutdown),
// and why its rumble failed, each reason once while it's there.
void said(const std::string& dir) {
    power_on(1);
    gamepads_init();
    Virtual a{"virtual pad said"}, quiet{"virtual pad quiet"};
    quiet.can_rumble = false;
    const std::string landed = dir + "/said-landed.txt";
    capture(landed, [&] {
        attach(a);
        frame();
        attach(quiet);
        frame();
    });
    check(lines_with(landed, "gamepads: \"virtual pad said\" on port 1\n"
                             "gamepads: port 1 is standard, unknown, 0000:0000, rumble yes, south label A, east label B\n") == 1 &&
              lines_with(landed, "gamepads: \"virtual pad quiet\" on port 2\n"
                                 "gamepads: port 2 is standard, unknown, 0000:0000, rumble no, south label A, east label B\n") == 1,
          "a gamepad that lands on a port is said, after its port line, with its type, connection, USB IDs, rumble and face labels");

    // Three presses of south and one each of east and misc6, which no port takes, each held
    // for two frames so that a count of frames held would be twice the presses; the left
    // stick to both ends, the right stick down a little and the left trigger all the way.
    for (int k = 0; k < 3; k++) {
        press(a, SDL_GAMEPAD_BUTTON_SOUTH, true);
        frame();
        press(a, SDL_GAMEPAD_BUTTON_SOUTH, false);
    }
    for (const int b : {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_MISC6}) {
        press(a, b, true);
        frame();
        press(a, b, false);
    }
    for (const Sint16 x : {-32768, 32767, 0}) push(a, SDL_GAMEPAD_AXIS_LEFTX, x);
    push(a, SDL_GAMEPAD_AXIS_RIGHTY, 1000);
    push(a, SDL_GAMEPAD_AXIS_RIGHTY, 0);
    // A virtual trigger's axis runs from -32768, released, to 32767, as the gamepad's 0 to 32767.
    push(a, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    push(a, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    motor(0);
    motor(0);
    const std::string out = dir + "/said-out.txt";
    capture(out, [&] { unplug({&a}); });
    check(lines_with(out, "gamepads: \"virtual pad said\" pulled out of port 1\n"
                          "gamepads: port 1 sent south 3, east 1, misc6 1, left stick x -32768 to 32767 y 0 to 0, right stick "
                          "x 0 to 0 y 0 to 1000, triggers left 0 to 32767 right 0 to 0\n"
                          "gamepads: port 1 rumble on 2 times, 0 of 4 calls failed\n") == 1,
          "a gamepad pulled out says, after its pulled-out line, each button it pressed and how often, each axis's extents and "
          "its rumble");

    // A rumble that fails says each of its reasons once while the gamepad is on its port, not
    // at each call or each on: on for five frames and off, on for three and off, then on for
    // two, accepted, and off, refused for another reason.
    Virtual f{"virtual pad refusing"};
    f.refuses = true;
    attach(f);
    frame();
    const std::string failing = dir + "/said-failing.txt";
    capture(failing, [&] {
        for (const int on : {5, 3}) {
            g_outbuf[0] = 0x00400001;
            for (int k = 0; k < on; k++) frame();
            g_outbuf[0] = 0x00400000;
            frame();
        }
        f.refuses = false;
        g_outbuf[0] = 0x00400001;
        frame();
        frame();
        f.refuses = true;
        f.why = "virtual pad refuses to stop";
        g_outbuf[0] = 0x00400000;
        frame();
        frame();
    });
    capture(dir + "/said-failing-out.txt", [&] { unplug({&f}); });
    check(lines_with(failing, "gamepads: port 1 rumble failed: virtual pad refuses rumble\n") == 1 &&
              lines_with(failing, "gamepads: port 1 rumble failed: virtual pad refuses to stop\n") == 1 &&
              lines_with(failing, "rumble failed") == 2 &&
              lines_with(dir + "/said-failing-out.txt", "gamepads: port 1 rumble on 3 times, 9 of 13 calls failed\n") == 1,
          "a rumble that fails says each reason once while the gamepad is on its port, and the pull-out line counts the "
          "failed calls");
    // Plugged in again, it lands afresh, so a reason it gave before is said again.
    f.why = "virtual pad refuses rumble";
    attach(f);
    frame();
    const std::string again = dir + "/said-failing-again.txt";
    capture(again, [] { motor(0); });
    capture(dir + "/said-failing-again-out.txt", [&] { unplug({&f}); });
    check(lines_with(again, "gamepads: port 1 rumble failed: virtual pad refuses rumble\n") == 1,
          "a gamepad plugged in again says why its rumble fails again");

    // The game's motor on port 2 on and off three times, where the gamepad SDL says has no
    // rumble is: it's never asked for one.
    const std::string none = dir + "/said-no-rumble.txt";
    capture(none, [] {
        for (int k = 0; k < 3; k++) motor(1);
    });

    // At shutdown each gamepad still on a port says what it sent: one with a press, one with none.
    Virtual b{"virtual pad shut"};
    attach(b);
    frame();
    press(b, SDL_GAMEPAD_BUTTON_WEST, true);
    frame();
    press(b, SDL_GAMEPAD_BUTTON_WEST, false);
    const std::string shut = dir + "/said-shutdown.txt";
    capture(shut, [] { gamepads_shutdown(); });
    const char* const still = "left stick x 0 to 0 y 0 to 0, right stick x 0 to 0 y 0 to 0, triggers left 0 to 0 right 0 to 0\n";
    check(lines_with(shut, (std::string("gamepads: port 1 sent west 1, ") + still).c_str()) == 1 &&
              lines_with(shut, (std::string("gamepads: port 2 sent ") + still).c_str()) == 1 &&
              lines_with(shut, "gamepads: port 1 rumble on 0 times, 0 of 0 calls failed\n") == 1,
          "at shutdown each gamepad still on a port says what it sent, a press or none, and its rumble");
    check(lines_with(none, "rumble failed") == 0 &&
              lines_with(shut, "gamepads: port 2 rumble on 3 times, 0 of 0 calls failed\n") == 1,
          "a gamepad SDL says has no rumble makes no rumble call and says no failure, and its line counts the game's ons");
    unplug({&b, &quiet});
}

// ---- A GameCube pad is asked for rumble whatever SDL says of it. SDL's GameCube driver
// says a pad has none if it lands before the adapter's second cable is plugged in, and never
// says otherwise, though it rumbles the pad once the cable is in.
void gamecube_rumble(const std::string& dir) {
    power_on(1);
    gamepads_init();
    Virtual v{"virtual gamecube adapter"};
    v.vendor = 0x057e;
    v.product = 0x0337;
    attach(v);
    // The test's own hold on the device, so the capability it clears is still there when the
    // frontend opens it.
    SDL_Joystick* j = SDL_OpenJoystick(v.id);
    SDL_SetBooleanProperty(SDL_GetJoystickProperties(j), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
    const std::string landed = dir + "/gamecube-landed.txt", out = dir + "/gamecube-out.txt";
    capture(landed, [] { frame(); });
    SDL_CloseJoystick(j);
    capture(out, [] {
        for (int k = 0; k < 3; k++) motor(0);
        gamepads_shutdown();
    });
    check(lines_with(landed, "gamepads: port 1 is gamecube, unknown, 057e:0337, rumble no, south label A, east label X\n") == 1 &&
              v.rumbles == 3 && v.stops == 3 && lines_with(out, "gamepads: port 1 rumble on 3 times, 0 of 6 calls failed\n") == 1,
          "a GameCube pad SDL says has no rumble is still asked for it, and rumbles");
    detach(v);

    // One with no rumble at all, as a WaveBird or an input-only pad SDL types as GameCube is,
    // is refused at each on, and says so once. SDL takes each off as done without asking the
    // driver, since the refused on left its rumble at 0 (SDL_RumbleJoystick).
    power_on(1);
    gamepads_init();
    Virtual w{"virtual gamecube no rumble"};
    w.vendor = 0x057e;
    w.product = 0x0337;
    w.can_rumble = false;
    attach(w);
    frame();
    const std::string refused = dir + "/gamecube-refused.txt";
    capture(refused, [] {
        for (int k = 0; k < 3; k++) motor(0);
        gamepads_shutdown();
    });
    check(lines_with(refused, "rumble failed") == 1 &&
              lines_with(refused, "gamepads: port 1 rumble failed: That operation is not supported\n") == 1 &&
              lines_with(refused, "gamepads: port 1 rumble on 3 times, 3 of 6 calls failed\n") == 1,
          "a GameCube pad with no rumble says SDL's refusal once, and its line counts each refused call");
    detach(w);
}

// ---- GameCube A, B, X and Y are the face buttons labeled A, B, X and Y, as Nintendo's own
// GameCube games on its consoles have them, and a pad whose faces carry no letters has them
// by Xbox position. Each pad lands on port 1, and each of its face buttons is pressed alone.
void labels(const std::string& dir) {
    struct Case {
        const char* name;
        Uint16 vendor, product;
        const char* line;            // the placement line
        uint16_t south, east, west, north;  // what each face button is to the game
    };
    static const Case kCases[] = {
        {"virtual switch pro", 0x057e, 0x2009, "gamepads: port 1 is switchpro, unknown, 057e:2009, rumble yes, south label B, east label A\n",
         PAD_B, PAD_A, PAD_Y, PAD_X},
        {"virtual xbox one", 0x045e, 0x02ea, "gamepads: port 1 is xboxone, unknown, 045e:02ea, rumble yes, south label A, east label B\n",
         PAD_A, PAD_B, PAD_X, PAD_Y},
        {"virtual dualsense", 0x054c, 0x0ce6, "gamepads: port 1 is ps5, unknown, 054c:0ce6, rumble yes, south label cross, east label circle\n",
         PAD_A, PAD_B, PAD_X, PAD_Y},
    };
    for (const Case& c : kCases) {
        power_on(1);
        gamepads_init();
        Virtual v{c.name};
        v.vendor = c.vendor;
        v.product = c.product;
        const std::string landed = dir + "/labels-landed.txt";
        capture(landed, [&] {
            attach(v);
            frame();
        });
        // What port 1 reads while `button` alone is held.
        auto sends = [&](int button) {
            press(v, button, true);
            const uint16_t got = read(0).buttons;
            press(v, button, false);
            return got;
        };
        const bool faces = sends(SDL_GAMEPAD_BUTTON_SOUTH) == c.south && sends(SDL_GAMEPAD_BUTTON_EAST) == c.east &&
                           sends(SDL_GAMEPAD_BUTTON_WEST) == c.west && sends(SDL_GAMEPAD_BUTTON_NORTH) == c.north;
        check(lines_with(landed, c.line) == 1 && faces,
              (std::string(c.name) + ": its face buttons reach the game by their labels, or by Xbox position for "
                                     "symbols, as its placement line's labels say").c_str());
        unplug({&v});
        gamepads_shutdown();
    }
}

// ---- The headset's default layout (vr::default_gamepad_layout) takes GameController's
// buttons, A south, B east, X west and Y north, to the GameCube's A, B, X and Y, as the
// desktop's gamepads have them by their labels.
void headset_layout() {
    auto sends = [](uint32_t button) {
        vr::Gamepad g;
        g.buttons = button;
        PadState p;
        vr::default_gamepad_layout(g, p);
        return p.buttons;
    };
    check(sends(vr::Gamepad::South) == PAD_A && sends(vr::Gamepad::East) == PAD_B &&
              sends(vr::Gamepad::West) == PAD_X && sends(vr::Gamepad::North) == PAD_Y,
          "the headset's default layout: south is GameCube A, east B, west X and north Y");
}

// ---- main() takes the disc speed from settings.txt, and --disc anywhere on the command
// line beats it for the run (take_disc_speed). Each check starts g_fast_disc at the other
// speed, so one that's left alone shows.
void disc_speed(const std::string& dir) {
    Settings console, fast;
    console.fast_disc = false;
    char game[] = "game", image[] = "game.iso", hidden[] = "--hidden", overlay[] = "--disc-overlay=overlay.json";
    char to_fast[] = "--disc=fast", to_console[] = "--disc=console", slow[] = "--disc=slow";
    char* none[] = {game, image, nullptr};
    char* last[] = {game, image, to_fast, nullptr};
    char* first[] = {game, to_fast, hidden, image, nullptr};
    char* both[] = {game, to_fast, to_console, nullptr};
    char* bad[] = {game, slow, image, nullptr};
    char* other[] = {game, overlay, image, nullptr};

    g_fast_disc = true;
    check(take_disc_speed(console, 2, none) && !g_fast_disc, "settings.txt's disc console with no --disc reads at console");
    g_fast_disc = false;
    check(take_disc_speed(console, 3, last) && g_fast_disc, "--disc=fast beats settings.txt's disc console");
    g_fast_disc = true;
    check(take_disc_speed(fast, 3, both) && !g_fast_disc,
          "--disc=console beats settings.txt's disc fast, and the last --disc wins");
    g_fast_disc = false;
    check(take_disc_speed(console, 4, first) && g_fast_disc, "a --disc before other arguments still counts");
    g_fast_disc = false;
    check(take_disc_speed(fast, 3, other) && g_fast_disc, "--disc-overlay= isn't taken for --disc=");
    const std::string said = dir + "/disc-stderr.txt";
    bool took = true;
    capture(said, [&] { took = take_disc_speed(fast, 3, bad); });
    check(!took && lines_with(said, "--disc: fast or console, not slow\n") == 1,
          "--disc=slow is refused with a line that says what --disc takes");
}

// ---- main() takes the bot from --bot=PATH or GCN_BOT (take_bot), which turns the gamepads
// off. Each check starts with the gamepads on, so one the bot turned off shows.
void bot_flags(const std::string& dir) {
    char game[] = "game", image[] = "game.iso", to_a[] = "--bot=a.sock", to_b[] = "--bot=b.sock";
    char bare[] = "--bot", empty[] = "--bot=", other[] = "--botany=x";
    char* none[] = {game, image, nullptr};
    char* both[] = {game, to_a, image, to_b, nullptr};
    char* lone[] = {game, bare, image, nullptr};
    char* blank[] = {game, empty, nullptr};
    char* near[] = {game, other, nullptr};
    char* one[] = {game, to_a, nullptr};
    const std::string said = dir + "/bot-stderr.txt";
    std::string bot = "left";
    bool took = false;
    // Takes argv with `replaying` and `opening`, saying on stderr into `said`, from the
    // gamepads on and `bot` set to something it must replace.
    auto take = [&](int argc, char** argv, bool replaying = false, bool opening = false) {
        g_gamepads = true;
        bot = "left";
        capture(said, [&] { took = take_bot(argc, argv, replaying, opening, bot); });
        return took;
    };

    unsetenv("GCN_BOT");
    check(take(2, none) && bot.empty() && g_gamepads, "with no --bot and no GCN_BOT there's no bot");
    check(take(4, both) && bot == "b.sock" && !g_gamepads,
          "--bot=PATH is the bot, the last one winning, and turns the gamepads off");
    check(take(2, near) && bot.empty() && g_gamepads, "--botany= isn't taken for --bot=");
    check(take(2, none, true, true) && bot.empty() && g_gamepads,
          "with no bot, a replay and --list-controls go ahead");
    setenv("GCN_BOT", "env.sock", 1);
    check(take(2, none) && bot == "env.sock" && !g_gamepads, "GCN_BOT is the bot with no --bot");
    check(take(2, one) && bot == "a.sock", "--bot=PATH beats GCN_BOT");
    check(!take(2, none, true) &&
              lines_with(said, "GCN_BOT can't go with --replay: a replay already decides the pads\n") == 1,
          "GCN_BOT with a replay is refused, naming GCN_BOT");
    check(!take(2, none, false, true) &&
              lines_with(said, "GCN_BOT opens no controller, so it can't go with --list-controls or --test-ffb\n") == 1,
          "GCN_BOT with --list-controls or --test-ffb is refused, naming GCN_BOT and not --no-gamepads");
    setenv("GCN_BOT", "", 1);
    check(take(2, none) && bot.empty() && g_gamepads, "an empty GCN_BOT is no bot");
    unsetenv("GCN_BOT");
    check(!take(2, one, true) &&
              lines_with(said, "--bot can't go with --replay: a replay already decides the pads\n") == 1,
          "--bot with a replay is refused, naming --bot");
    check(!take(2, one, false, true) &&
              lines_with(said, "--bot opens no controller, so it can't go with --list-controls or --test-ffb\n") == 1,
          "--bot with --list-controls or --test-ffb is refused, naming --bot");
    check(!take(3, lone) && lines_with(said, "--bot: the socket's path goes after an =, as in --bot=PATH\n") == 1,
          "a bare --bot is refused, so the argument after it isn't taken for the disc image");
    check(!take(2, blank) &&
              lines_with(said, "--bot: the path of the socket the bot listens on, not nothing\n") == 1,
          "--bot= with no path is refused");
    g_gamepads = true;
}

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
    bool before = false, at = false, once = false, bad = false;
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
        unsetenv("GCN_QUIT_AT");
        quit_at_load();
    });
    check(before && at, "GCN_QUIT_AT=3 stops the window loop at presented frame 3, as the close button does");
    check(once && lines_with(out, "[quit] GCN_QUIT_AT=3 ends the run at presented frame 3") == 1,
          "and only then, which it says once");
    check(bad && lines_with(out, "[quit] GCN_QUIT_AT=3x isn't a frame number, so it ends nothing") == 1,
          "a GCN_QUIT_AT that isn't a frame number ends nothing, and says so");
}

#ifdef GCN_OVERLAY
// ---- The overlay (overlay/overlay.h) with no window, so no menu is drawn. A thread stands in
// for the guest. Each host frame lets it reach one more frame boundary (pause_gate), after the
// frontend has taken its events and set the ports.
struct Guest {
    std::mutex m;
    std::condition_variable cv;
    uint32_t asked = 0;               // the boundaries it may reach
    std::atomic<uint32_t> passed{0};  // the boundaries it's gone past
    bool quit = false;
    std::thread t{[this] { run(); }};
    void run() {
        for (uint32_t n = 0;; n++) {
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return quit || asked > n; });
                if (quit) return;
            }
            pause_gate(n);
            passed = n + 1;
        }
    }
    ~Guest() {
        pause_request(false);
        {
            std::lock_guard<std::mutex> lk(m);
            quit = true;
        }
        cv.notify_all();
        t.join();
    }
};

// One host frame, the menu's included where there's a window, then the guest on past its next
// boundary, or held there. pump_events' answer.
bool host_frame(Guest& g) {
    const bool running = pump_events();
    update_pads(0);
    overlay_menu_frame();
    uint32_t want;
    {
        std::lock_guard<std::mutex> lk(g.m);
        want = ++g.asked;
    }
    g.cv.notify_all();
    for (int i = 0; i < 2000 && g.passed < want && !(pause_requested() && paused()); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return running;
}

void key_down(SDL_Scancode key, bool repeat = false) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = key;
    e.key.down = true;
    e.key.repeat = repeat;
    SDL_PushEvent(&e);
}

// SDL's gamepad button `button` on `v`, which a virtual gamepad numbers among the buttons it
// has, in SDL's order.
void set_button(const Virtual& v, int button, bool down) {
    const int index = v.button_mask ? __builtin_popcount(v.button_mask & ((1u << button) - 1)) : button;
    if (SDL_Joystick* j = SDL_GetJoystickFromID(v.id)) SDL_SetJoystickVirtualButton(j, index, down);
}

void set_axis(const Virtual& v, int axis, Sint16 value) {
    if (SDL_Joystick* j = SDL_GetJoystickFromID(v.id)) SDL_SetJoystickVirtualAxis(j, axis, value);
}

Settings g_applied;  // what the Display page last had the frontend apply
int g_applies;
void record_apply(const Settings& s) {
    g_applied = s;
    g_applies++;
}

void overlay(const std::string& dir) {
    const std::string path = dir + "/settings.txt";
    write_file(path, "# The picture\naspect 4:3\nvsync on\nfullscreen 0\n\nscale 2\ndisc console\n# the end\n");
    OverlayPicture picture;
    picture.saved = picture.now = settings_load(path);
    picture.now.scale = 3;  // as --scale=3 makes it for this run
    picture.path = path;
    picture.apply = record_apply;
    overlay_init(nullptr, "Test title", picture);
    power_on(1);
    gamepads_init();
    Virtual pad{"virtual pad overlay"};
    attach(pad);
    Guest guest;
    host_frame(guest);
    check(!overlay_open() && !paused() && guest.passed == 1, "with the overlay closed, the guest goes past its frame boundary");

    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, true);
    int frames = 0;
    while (frames < 4 && !(overlay_open() && paused())) {
        host_frame(guest);
        frames++;
    }
    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, false);
    host_frame(guest);
    check(frames <= 2 && overlay_open() && paused(), "a gamepad's Back opens the overlay, and the guest is held within 2 host frames");

    // Each pushed all the way, and back to where it rests: a virtual pad's triggers rest at the
    // bottom of their axis.
    const struct {
        int axis;
        Sint16 rest;
    } kPushed[] = {{SDL_GAMEPAD_AXIS_LEFTX, 0}, {SDL_GAMEPAD_AXIS_RIGHTX, 0},
                   {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768}, {SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768}};
    for (const auto& a : kPushed) set_axis(pad, a.axis, 32767);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    const PadState gated = read(0);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    for (const auto& a : kPushed) set_axis(pad, a.axis, a.rest);
    host_frame(guest);
    check(gated.connected && at_rest(gated) && overlay_open(),
          "while it's open, the gamepad's A, sticks and triggers don't reach port 1, which stays plugged in");

    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, true);
    host_frame(guest);
    const bool closed = !overlay_open() && !pause_requested();
    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, false);
    host_frame(guest);
    check(closed && !paused() && guest.passed == guest.asked, "Back again closes it, and the guest goes on");

    key_down(SDL_SCANCODE_F1);
    host_frame(guest);
    const bool f1_opens = overlay_open() && paused();
    key_down(SDL_SCANCODE_F1);
    host_frame(guest);
    check(f1_opens && !overlay_open() && !paused(), "F1 opens it and F1 closes it");

    key_down(SDL_SCANCODE_F1);
    host_frame(guest);
    key_down(SDL_SCANCODE_F1, true);
    host_frame(guest);
    const bool f1_repeat_stays = overlay_open();
    key_down(SDL_SCANCODE_ESCAPE, true);
    host_frame(guest);
    const bool escape_repeat_stays = overlay_open();
    key_down(SDL_SCANCODE_F1);
    host_frame(guest);
    check(f1_repeat_stays && escape_repeat_stays && !overlay_open(), "a held F1's or Escape's repeat doesn't close it");

    key_down(SDL_SCANCODE_F1);
    host_frame(guest);
    key_down(SDL_SCANCODE_ESCAPE);
    const bool stays = host_frame(guest);
    const bool escape_closes = stays && !overlay_open() && !paused();
    key_down(SDL_SCANCODE_ESCAPE, true);
    const bool repeat_stays = host_frame(guest);
    key_down(SDL_SCANCODE_ESCAPE);
    check(escape_closes && repeat_stays && !host_frame(guest) && !overlay_open(),
          "Escape closes it without quitting; with it closed, Escape quits, though a held Escape's repeat doesn't");

    key_down(SDL_SCANCODE_F1);
    host_frame(guest);
    set_button(pad, SDL_GAMEPAD_BUTTON_EAST, true);
    host_frame(guest);
    const bool b_closes = !overlay_open() && !paused();
    const PadState held = read(0);
    set_button(pad, SDL_GAMEPAD_BUTTON_EAST, false);
    host_frame(guest);
    set_button(pad, SDL_GAMEPAD_BUTTON_EAST, true);
    host_frame(guest);
    const PadState again = read(0);
    set_button(pad, SDL_GAMEPAD_BUTTON_EAST, false);
    host_frame(guest);
    check(b_closes && !held.buttons && again.buttons == PAD_B && !overlay_open(),
          "B closes it and stays out of the game until it's let go; pressed with it closed, it's the game's B and opens nothing");

    // Until the guest is held, port 1 keeps what it had as the overlay opened, so the game
    // never sees the A let go, and then it's at rest.
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    const bool a_in = read(0).buttons & PAD_A;
    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, true);
    host_frame(guest);
    const PadState opening = read(0);
    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, false);
    host_frame(guest);
    const PadState stopped = read(0);
    check(a_in && (opening.buttons & PAD_A) && overlay_open() && paused() && stopped.connected && at_rest(stopped),
          "an A held across the open stays in port 1 until the guest is held, then the port is at rest");
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, true);
    host_frame(guest);
    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, false);
    host_frame(guest);

    // A trigger in as it closes stays out of the game with its travel until it's back at rest,
    // though it's let out past its click (kGamepadTriggerClick) on the way.
    const auto back = [&] {
        set_button(pad, SDL_GAMEPAD_BUTTON_BACK, true);
        host_frame(guest);
        set_button(pad, SDL_GAMEPAD_BUTTON_BACK, false);
        host_frame(guest);
    };
    back();
    set_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    set_axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    host_frame(guest);
    back();
    const bool shut = !overlay_open();
    set_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 20000);
    host_frame(guest);
    const PadState eased = read(0);
    set_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    set_axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    host_frame(guest);
    set_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 20000);
    host_frame(guest);
    const PadState rested = read(0);
    set_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    host_frame(guest);
    check(shut && eased.connected && !eased.trig_l && !eased.trig_r && !(eased.buttons & (PAD_L | PAD_R)),
          "a trigger in as it closes stays out of the game with its travel, eased out past its click too");
    check(rested.trig_l > 0 && !(rested.buttons & PAD_L), "once it's been back at rest, its travel is the game's again");

    // A gamepad with no Back, as SDL maps GameCube controllers, opens and closes it with L and
    // R clicked in and Start, all held, once each time they're held together.
    // SDL keeps a virtual gamepad's mapping under the first nine characters of its name, so
    // this name can't start as "virtual pad" does, or it would get that pad's Back.
    Virtual cube{"no-back pad"};
    cube.button_mask = ((1u << SDL_GAMEPAD_BUTTON_COUNT) - 1) & ~(1u << SDL_GAMEPAD_BUTTON_BACK);
    attach(cube);
    host_frame(guest);
    SDL_Gamepad* cube_pad = SDL_GetGamepadFromID(cube.id);
    const bool no_back = cube_pad && !SDL_GamepadHasButton(cube_pad, SDL_GAMEPAD_BUTTON_BACK);
    const auto hold = [&](const Virtual& v, bool l, bool r, bool start) {
        set_axis(v, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, l ? 32767 : -32768);
        set_axis(v, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, r ? 32767 : -32768);
        set_button(v, SDL_GAMEPAD_BUTTON_START, start);
        host_frame(guest);
    };
    hold(cube, true, false, true);
    const bool two_of_three = !overlay_open();
    hold(cube, true, true, true);
    const bool chord_opens = overlay_open() && paused();
    // Still held, the chord sees the stick move, and that closes nothing.
    set_axis(cube, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    hold(cube, true, true, true);
    const bool held_stays = overlay_open();
    set_axis(cube, SDL_GAMEPAD_AXIS_LEFTX, 0);
    hold(cube, false, false, false);
    hold(cube, true, true, true);
    const bool chord_closes = !overlay_open() && !paused();
    const PadState closing = read(1);
    hold(cube, false, false, false);
    hold(pad, true, true, true);
    const bool back_pad = !overlay_open();
    hold(pad, false, false, false);
    check(no_back && chord_opens && held_stays && chord_closes,
          "on a gamepad with no Back, L, R and Start held together open it, once, and then close it");
    check(two_of_three && back_pad, "two of the three don't, and on a gamepad with a Back all three don't");
    check(closing.connected && at_rest(closing),
          "the chord that closed it stays out of port 2 until it's let go, the triggers' travel too");
    unplug({&cube});

    Settings next = picture.now;
    next.aspect = "16:9";
    overlay_display_change(next);
    check(read_file(path) == "# The picture\naspect 16:9\nvsync on\nfullscreen 0\n\nscale 2\ndisc console\n# the end\n" &&
              g_applied.aspect == "16:9" && g_applied.scale == 3,
          "the Display page's aspect applies at once and is saved, every other line as it was and the flag's scale left out");
    next.fullscreen = true;
    overlay_display_change(next);
    check(read_file(path) == "# The picture\naspect 16:9\nvsync on\nfullscreen 1\n\nscale 2\ndisc console\n# the end\n" &&
              g_applied.fullscreen,
          "its fullscreen applies at once and is saved, every other line as it was");
    next.scale = 5;
    overlay_display_change(next);
    overlay_display_change(next);
    check(read_file(path) == "# The picture\naspect 16:9\nvsync on\nfullscreen 1\n\nscale 5\ndisc console\n# the end\n" &&
              g_applied.scale == 5 && g_applies == 3,
          "its scale applies at once and is saved, every other line as it was, and a picture with no change does nothing");
    unplug({&pad});
    gamepads_shutdown();
}

// ---- The overlay's Restart game and Quit game, over a window from SDL's dummy video driver.
// It has no GL context, so the menu is driven but not drawn (overlay_menu_frame). First an A
// and an Enter held as the menu opens press nothing. Then a gamepad alone goes through Restart
// and the keyboard alone through Quit. Neither ends this process: each stops pump_events, the
// frontend's loop, and --restart follows a restart from there.
// Between the two a Switch Pro pad asks and goes back with its faces by their labels, as the
// game reads them, so its east presses and its south goes back.
SDL_WindowID g_window_id;  // the test window's, which the menu takes keys from

// A key pressed, or let go, over the window, or a held one's repeat, then a host frame. False
// if pump_events was.
bool key(Guest& g, SDL_Scancode scancode, SDL_Keycode keycode, bool down, bool repeat = false) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = g_window_id;
    e.key.scancode = scancode;
    e.key.key = keycode;
    e.key.down = down;
    e.key.repeat = repeat;
    SDL_PushEvent(&e);
    return host_frame(g);
}

// A key pressed and let go over the window, a host frame each. False if pump_events was.
bool tap(Guest& g, SDL_Scancode scancode, SDL_Keycode keycode) {
    const bool pressed = key(g, scancode, keycode, true);
    return key(g, scancode, keycode, false) && pressed;
}

// A gamepad's button pressed and let go, a host frame each.
void tap(Guest& g, const Virtual& v, int button) {
    set_button(v, button, true);
    host_frame(g);
    set_button(v, button, false);
    host_frame(g);
}

// The menu open over the held game, asking `asking`, and no restart on its way.
bool waiting(OverlayAsk asking) { return overlay_open() && paused() && overlay_asking() == asking && !g_restart; }

void overlay_buttons(const std::string& dir) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_Window* window = SDL_InitSubSystem(SDL_INIT_VIDEO) ? SDL_CreateWindow("test_ports", 1280, 960, SDL_WINDOW_HIDDEN) : nullptr;
    check(window != nullptr, "SDL's dummy video driver makes a window for the menu");
    if (!window) return;
    g_window_id = SDL_GetWindowID(window);
    OverlayPicture picture;
    picture.path = dir + "/buttons-settings.txt";
    picture.apply = record_apply;
    const int applies = g_applies;
    overlay_init(window, "Test title", picture);
    gamepads_init();
    Virtual pad{"virtual pad buttons"};
    attach(pad);
    // A south and B east by its labels, as by position, where a Switch Pro has them the other
    // way round. Both are in throughout, so each press goes by its own pad's labels.
    Virtual nintendo{"virtual switch pro buttons"};
    nintendo.vendor = 0x057e;
    nintendo.product = 0x2009;
    attach(nintendo);
    Guest guest;
    host_frame(guest);

    // A face or a key held as the menu opens stays out of it until it's let go, though the
    // focus is where the menu left it, here on Restart game (below).
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    for (int i = 0; i < 8; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    for (int i = 0; i < 2; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    host_frame(guest);
    const bool held_a = waiting(OverlayAsk::none);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    host_frame(guest);
    const bool let_go = waiting(OverlayAsk::none);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(held_a && let_go && waiting(OverlayAsk::restart),
          "an A held as Back opens the menu presses nothing, nor does letting it go, and its next press does");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    key(guest, SDL_SCANCODE_RETURN, SDLK_RETURN, true);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    key(guest, SDL_SCANCODE_RETURN, SDLK_RETURN, true, true);
    host_frame(guest);
    const bool repeat_presses_nothing = waiting(OverlayAsk::none);
    key(guest, SDL_SCANCODE_RETURN, SDLK_RETURN, false);
    tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
    check(repeat_presses_nothing && waiting(OverlayAsk::restart),
          "a held Enter's repeat after F1 opens the menu presses nothing, and its next press does");
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    // A key let go while the menu was closed is up to it as it opens again, so its next press
    // reaches it: here Left, held as the menu closed, onto Restart game from Quit game.
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    tap(guest, SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    key(guest, SDL_SCANCODE_LEFT, SDLK_LEFT, true);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    key(guest, SDL_SCANCODE_LEFT, SDLK_LEFT, false);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    tap(guest, SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    tap(guest, SDL_SCANCODE_LEFT, SDLK_LEFT);
    tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
    check(waiting(OverlayAsk::restart), "a key let go while the menu was closed moves its focus the next time it's open");
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);

    // From the top of the page down past every row and left past every column is Restart game.
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    for (int i = 0; i < 8; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    for (int i = 0; i < 2; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(waiting(OverlayAsk::restart), "on a gamepad, Restart game is at the bottom of the page, and pressing it asks first");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(waiting(OverlayAsk::none) && host_frame(guest), "Cancel is focused as it asks, and pressing it goes back with nothing done");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool again = waiting(OverlayAsk::restart);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    check(again && waiting(OverlayAsk::none) && host_frame(guest),
          "Restart game is focused again after, and B goes back with nothing done, the overlay still open");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(waiting(OverlayAsk::restart), "Restart game is focused again after B too");
    // Dear ImGui sizes a window to what it held the frame before, so the question is laid out
    // where it stays a frame after it's asked, and a move from Cancel starts from there.
    host_frame(guest);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    const bool closed = !overlay_open() && !pause_requested() && !paused() && guest.passed == guest.asked;
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    check(closed && g_restart && !host_frame(guest),
          "confirming Restart closes the overlay and lets the guest go, then the frontend's loop stops to restart");
    g_restart = false;  // as the relaunched process starts

    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_BACK);
    for (int i = 0; i < 8; i++) tap(guest, nintendo, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    for (int i = 0; i < 2; i++) tap(guest, nintendo, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_EAST);
    SDL_Gamepad* switch_pro = SDL_GetGamepadFromID(nintendo.id);
    check(switch_pro && SDL_GetGamepadButtonLabel(switch_pro, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A &&
              waiting(OverlayAsk::restart),
          "on a Switch Pro pad, the button labeled A, east, presses Restart game");
    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_EAST);
    check(waiting(OverlayAsk::none) && host_frame(guest), "its A presses Cancel too, going back with nothing done");
    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_EAST);
    const bool asks_again = waiting(OverlayAsk::restart);
    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_SOUTH);
    check(asks_again && waiting(OverlayAsk::none) && host_frame(guest),
          "its B, south, goes back from the question with nothing done, the overlay still open");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool other_presses = waiting(OverlayAsk::restart);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    const bool open = waiting(OverlayAsk::none);
    check(other_presses && open, "the other pad's south is still its A and its east its B");
    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_SOUTH);
    check(open && !overlay_open() && !paused() && guest.passed == guest.asked,
          "the Switch Pro pad's B closes the overlay, and the guest goes on");

    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    for (int i = 0; i < 8; i++) tap(guest, SDL_SCANCODE_DOWN, SDLK_DOWN);
    for (int i = 0; i < 2; i++) tap(guest, SDL_SCANCODE_LEFT, SDLK_LEFT);
    tap(guest, SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
    check(waiting(OverlayAsk::quit), "on the keyboard, Quit game is right of Restart game, and Enter on it asks first");
    tap(guest, SDL_SCANCODE_SPACE, SDLK_SPACE);
    check(waiting(OverlayAsk::none) && host_frame(guest), "Cancel is focused as it asks, and Space on it goes back with nothing done");
    tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
    const bool asks = waiting(OverlayAsk::quit);
    const bool stays = tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    check(asks && stays && waiting(OverlayAsk::none) && host_frame(guest),
          "Quit game is focused again after, and Escape goes back with nothing done, without quitting");
    tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
    host_frame(guest);  // the question laid out where it stays, as above
    tap(guest, SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    const bool running = tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
    check(!running && !g_restart && !overlay_open() && !pause_requested(),
          "confirming Quit closes the overlay, and the frontend's loop stops as for a quit, not a restart");
    check(g_applies == applies, "no press on the way to either button changed the picture");

    // Up from Restart game past the scale and Fullscreen is the aspect's row, and the second in
    // it is 16:9. With the aspect held, as --eye and GCN_THEATER_STEREO hold it, the row is
    // there but can't be picked, so the same keys pick nothing.
    const auto pick_16_9 = [&](bool held) {
        OverlayPicture p;
        p.path = dir + (held ? "/held-settings.txt" : "/free-settings.txt");
        p.apply = record_apply;
        p.aspect_held = held;
        display_init(p);
        const int before = g_applies;
        tap(guest, SDL_SCANCODE_F1, SDLK_F1);
        for (int i = 0; i < 8; i++) tap(guest, SDL_SCANCODE_DOWN, SDLK_DOWN);
        for (int i = 0; i < 2; i++) tap(guest, SDL_SCANCODE_LEFT, SDLK_LEFT);
        for (int i = 0; i < 3; i++) tap(guest, SDL_SCANCODE_UP, SDLK_UP);
        for (int i = 0; i < 5; i++) tap(guest, SDL_SCANCODE_LEFT, SDLK_LEFT);
        tap(guest, SDL_SCANCODE_RIGHT, SDLK_RIGHT);
        tap(guest, SDL_SCANCODE_SPACE, SDLK_SPACE);
        tap(guest, SDL_SCANCODE_F1, SDLK_F1);
        return g_applies == before + 1 && g_applied.aspect == "16:9";
    };
    check(pick_16_9(false), "up from Restart game past the scale and Fullscreen, the keys pick the Display page's 16:9");
    check(!pick_16_9(true), "with the aspect held, the same keys pick nothing");

    // The frontend's apply_picture, as the page calls it: the aspect and the scale reach the
    // renderer, and with the aspect held only the scale does.
    g_window = window;
    Settings s;
    s.aspect = "16:9";
    s.scale = 4;
    apply_picture(s);
    const bool applied = std::abs(g_output_aspect - 16.0f / 9.0f) < 1e-4f && g_internal_scale == 4 && !g_aspect_window;
    g_aspect_held = true;
    s.aspect = "21:9";
    s.scale = 2;
    apply_picture(s);
    check(applied && std::abs(g_output_aspect - 16.0f / 9.0f) < 1e-4f && g_internal_scale == 2,
          "the frontend gives the renderer the page's aspect and scale, and with the aspect held the scale alone");
    g_aspect_held = false;
    g_window = nullptr;

    // A device controls.txt binds is the file's alone, so its Back opens nothing.
    const std::string controls = dir + "/buttons-controls.txt";
    write_file(controls, "A = \"virtual pad bound overlay\" button 0\n");
    input_map_load(controls.c_str());
    Virtual bound{"virtual pad bound overlay"};
    attach(bound);
    host_frame(guest);
    tap(guest, bound, SDL_GAMEPAD_BUTTON_BACK);
    const bool unopened = !overlay_open() && !paused();
    set_button(bound, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    const bool a_by_file = read(0).buttons & PAD_A;
    set_button(bound, SDL_GAMEPAD_BUTTON_SOUTH, false);
    host_frame(guest);
    check(unopened && a_by_file, "a gamepad controls.txt binds opens no menu with its Back, and its button 0 is A on port 1 by the file");
    detach(bound);
    host_frame(guest);
    input_map_load((dir + "/no-controls.txt").c_str());

    unplug({&pad, &nintendo});
    gamepads_shutdown();
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
#endif

int run_ports(const std::string& dir) {
    drivers_stay_off();
    if (!sdl_up()) return 1;
    input_map_load((dir + "/no-controls.txt").c_str());
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
    memory_order(file);
    memory_reversed(file);
    memory_twins(file);
    memory_missing(file);
    memory_pinned(dir, file);
    memory_held(dir, file);
    memory_file(dir, file);
    memory_no_pads(dir, file);
    memory_unreadable(dir, file);
    gamepads_remember(nullptr);
    said(dir);
    gamecube_rumble(dir);
    labels(dir);
    headset_layout();
    disc_speed(dir);
    bot_flags(dir);
    run_end(dir);
    quit_at_frame(dir);
#ifdef GCN_OVERLAY
    overlay(dir);
    overlay_buttons(dir);
#endif
    return finish("ports");
}

// ---- Recording and replay: the same reads in both runs. Each step sets what the live
// gamepads do (when there are any) and the game's rumble motors, runs a frame and reads the
// four ports once, at presented frame 100 + step.
const int kSteps = 6;

// The motor lines a log of those steps has: port 1's motor on at step 1 and still on at step
// 2, off at step 3, and port 2's on from step 4 to the end. Each is written at the poll that
// reads the port (the step's polls are 4 * step to 4 * step + 3), at the step's frame.
const char* const kMotorLines = "motor 4 101 0 1\nmotor 12 103 0 0\nmotor 17 104 1 1\n";

// The lines of the file at `path` that start with `prefix`.
std::string lines_starting(const std::string& path, const char* prefix) {
    std::string out;
    const std::string text = read_file(path);
    for (size_t at = 0; at < text.size();) {
        const size_t next = std::min(text.find('\n', at), text.size() - 1) + 1;
        if (text.compare(at, strlen(prefix), prefix) == 0) out += text.substr(at, next - at);
        at = next;
    }
    return out;
}

std::string steps(Virtual* a, Virtual* b, std::vector<PadState>& seen) {
    std::string out;
    for (int s = 0; s < kSteps; s++) {
        SDL_Joystick* ja = a ? SDL_GetJoystickFromID(a->id) : nullptr;
        SDL_Joystick* jb = b ? SDL_GetJoystickFromID(b->id) : nullptr;
        if (jb) SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, s == 1 || s == 2);
        if (ja) SDL_SetJoystickVirtualButton(ja, SDL_GAMEPAD_BUTTON_EAST, s == 3);
        if (jb) SDL_SetJoystickVirtualAxis(jb, SDL_GAMEPAD_AXIS_LEFTX, s == 4 ? 32767 : 0);
        g_outbuf[0] = s == 1 || s == 2 ? 0x00400001 : 0x00400000;
        g_outbuf[1] = s >= 4 ? 0x00400001 : 0x00400000;
        frame();
        for (int i = 0; i < 4; i++) {
            seen.push_back(read(i, 100 + s));
            out += describe(seen.back());
        }
    }
    return out;
}

int run_record(const std::string& dir) {
    if (!sdl_up()) return 1;
    input_map_load((dir + "/no-controls.txt").c_str());
    input_script_init("");
    power_on(1);
    gamepads_init();
    Virtual a{"virtual pad a"}, b{"virtual pad b"};
    attach(a);
    attach(b);
    frame();
    const std::string sram = "the SRAM the run started with\n";
    write_file(dir + "/sram.raw", sram);
    if (!input_log_start(dir + "/log", dir + "/no-card.raw", dir + "/sram.raw")) return 1;
    check(read_file(dir + "/log/sram.raw") == sram, "the log keeps a snapshot of the SRAM");
    std::vector<PadState> seen;
    const std::string text = steps(&a, &b, seen);
    input_log_end();
    write_file(dir + "/expected.txt", text);
    bool two = true, rest = true;
    for (int s = 0; s < kSteps; s++) {
        two = two && seen[s * 4 + 1].connected;
        rest = rest && !seen[s * 4 + 2].connected && !seen[s * 4 + 3].connected;
    }
    check(two && rest && seen[1 * 4 + 1].buttons == PAD_A && seen[2 * 4 + 1].buttons == PAD_A && seen[3 * 4].buttons == PAD_B &&
              seen[4 * 4 + 1].stick_x == 228 && at_rest(seen[5 * 4]) && at_rest(seen[5 * 4 + 1]),
          "recording with --pads=1: port 2's gamepad (A, then the stick) and port 1's (east button, labeled B) reach the guest; ports 3 and 4 unplugged");
    const std::string log = dir + "/log/inputs.txt";
    check(read_file(log).rfind("# gcn-recomp input log v5\n", 0) == 0 &&
              lines_starting(log, "# motor ") ==
                  "# motor <poll> <frame> <chan> <0|1>: the game turned that channel's rumble motor on or off there (decimal)\n",
          "the log is version 5, and its header gives the motor line");
    check(lines_starting(log, "# disc ") == "# disc fast\n",
          "the log's header says the run read the disc at disc fast, the speed the drive is left at");
    check(lines_starting(log, "motor ") == kMotorLines,
          "the log has a motor line for each change of a port's rumble motor, at the poll that read it, and none where it stayed");
    unplug({&a, &b});
    gamepads_shutdown();
    return finish("--record");
}

int run_replay(const std::string& dir) {
    if (!sdl_up()) return 1;
    input_map_load((dir + "/no-controls.txt").c_str());
    input_script_init("");
    std::string card, sram;
    bool loaded = false;
    const std::string said = dir + "/replay-stderr.txt";
    g_fast_disc = false;  // as settings.txt's disc console or --disc=console leave it
    capture(said, [&] { loaded = input_replay_load(dir + "/log", card, sram); });
    if (!loaded) return 1;
    check(sram == dir + "/log/sram_replay.raw" && read_file(sram) == "the SRAM the run started with\n",
          "a replay gets a scratch copy of the logged SRAM");
    check(lines_with(said, "(with its SRAM snapshot)") == 1, "and its summary line says the log had an SRAM snapshot");
    check(g_fast_disc && lines_with(said, "[input] the log was recorded at disc fast, so the replay reads at that and not "
                                          "at disc console\n") == 1,
          "a log recorded at disc fast replays at disc fast with disc console asked for, and says so");
    // Logged as the frontend logs a replay, into a log of its own.
    if (!input_log_start(dir + "/replay-log", dir + "/no-card.raw", sram)) return 1;
    check(lines_starting(dir + "/replay-log/inputs.txt", "# disc ") == "# disc fast\n",
          "and the replay's own log says disc fast");
    power_on(1);
    gamepads_init();
    frame();
    std::vector<PadState> seen;
    const std::string text = steps(nullptr, nullptr, seen);
    check(text == read_file(dir + "/expected.txt"),
          "replayed with no gamepad and --pads=1: every read of all four ports is the recording's, its motor lines skipped");
    check(lines_starting(dir + "/replay-log/inputs.txt", "motor ") == kMotorLines,
          "the replay's own log has the recording's motor lines, with no gamepad to rumble");
    read(0);  // the log's end marker: the replay holds through this read
    const PadState after2 = read(1);
    check(read(0).connected && after2.connected && at_rest(after2) && !read(2).connected && !read(3).connected,
          "past the log's end: port 2, plugged in when the log ended, stays plugged in at rest with no gamepad on it");
    Virtual n{"virtual pad n"};
    attach(n);
    frame();
    check(port(n) == 1, "after the replay a gamepad that connects takes port 1 and drives it live");
    unplug({&n});
    gamepads_shutdown();
    return finish("--replay");
}

// The recording's log with its SRAM snapshot taken away, after --replay has left a scratch
// copy beside it. A log with no snapshot replays from the default SRAM, so the copy an
// earlier replay left must be cleared, not loaded in its place.
int run_replay_without_sram(const std::string& dir) {
    remove((dir + "/log/sram.raw").c_str());
    write_file(dir + "/log/sram_replay.raw", "left by an earlier replay\n");
    std::string card, sram;
    bool loaded = false;
    const std::string said = dir + "/replay-without-sram-stderr.txt";
    capture(said, [&] { loaded = input_replay_load(dir + "/log", card, sram); });
    if (!loaded) return 1;
    check(sram == dir + "/log/sram_replay.raw" && !plat_readable(sram.c_str()),
          "a log with no SRAM snapshot replays from the default SRAM: the scratch copy an earlier replay left is cleared");
    check(lines_with(said, "SRAM snapshot") == 0, "and its summary line doesn't claim a snapshot");
    check(g_fast_disc && lines_with(said, "so the replay reads at") == 0,
          "a replay asked for the disc speed its log was recorded at reads at it and says nothing of it");
    return finish("--replay-no-sram");
}

// ---- A version 4 log, from before the motor lines, replays as it always did: three steps
// of reads of the four ports, each port at its logged state from its line's poll on. It has
// no disc line either, so it replays at disc console.
int run_replay_v4(const std::string& dir) {
    plat_make_dirs(dir + "/log");
    write_file(dir + "/log/inputs.txt", "# gcn-recomp input log v4\n# game TEST00  started 2026-10-28T12:00:00\n"
                                        "# epoch 1792000000\n"
                                        "0 0 0 1 0000 80 80 80 80 00 00\n0 0 1 0 0000 80 80 80 80 00 00\n"
                                        "0 0 2 0 0000 80 80 80 80 00 00\n0 0 3 0 0000 80 80 80 80 00 00\n"
                                        "5 0 1 1 0100 E4 80 80 80 00 00\n8 0 0 1 1000 80 80 80 80 00 00\n# end 12 0\n");
    std::string card, sram;
    bool loaded = false;
    const std::string said = dir + "/replay-stderr.txt";
    capture(said, [&] { loaded = input_replay_load(dir + "/log", card, sram); });
    if (!loaded) return 1;
    input_script_init("");
    power_on(1);
    std::string text;
    for (int s = 0; s < 3; s++)
        for (int i = 0; i < 4; i++) text += describe(read(i));
    const std::string rest = "1 0000 80 80 80 80 00 00\n", out = "0 0000 80 80 80 80 00 00\n";
    const std::string with_a = "1 0100 E4 80 80 80 00 00\n", start = "1 1000 80 80 80 80 00 00\n";
    check(text == rest + out + out + out + rest + with_a + out + out + start + with_a + out + out,
          "a version 4 log replays: every read of all four ports is its logged state");
    check(!g_fast_disc && lines_with(said, "[input] the log has no disc line, so the replay reads at disc console and "
                                           "not at disc fast\n") == 1,
          "a log with no disc line replays at disc console with disc fast asked for, and says so");
    return finish("--replay-v4");
}

// ---- A log recorded at disc console, written by hand, replays at disc console with disc
// fast asked for, and the replay's own log says disc console. One whose disc line names a
// speed the runtime doesn't know replays at disc console too, and says that rather than
// that the line is missing.
int run_replay_console(const std::string& dir) {
    plat_make_dirs(dir + "/log");
    write_file(dir + "/log/inputs.txt", "# gcn-recomp input log v5\n# game TEST00  started 2026-10-28T12:00:00\n"
                                        "# epoch 1792000000\n# disc console\n0 0 0 1 0000 80 80 80 80 00 00\n"
                                        "# end 4 0\n");
    std::string card, sram;
    bool loaded = false;
    const std::string said = dir + "/replay-stderr.txt";
    capture(said, [&] { loaded = input_replay_load(dir + "/log", card, sram); });
    if (!loaded) return 1;
    check(!g_fast_disc && lines_with(said, "[input] the log was recorded at disc console, so the replay reads at that "
                                           "and not at disc fast\n") == 1,
          "a log recorded at disc console replays at disc console with disc fast asked for, and says so");
    if (!input_log_start(dir + "/replay-log", dir + "/no-card.raw", sram)) return 1;
    check(lines_starting(dir + "/replay-log/inputs.txt", "# disc ") == "# disc console\n",
          "and the replay's own log says disc console");

    // A log whose disc line names a speed this runtime doesn't know. Its changes join the
    // first log's, which nothing reads from here on.
    plat_make_dirs(dir + "/turbo");
    write_file(dir + "/turbo/inputs.txt", "# gcn-recomp input log v5\n# game TEST00  started 2026-10-28T12:00:00\n"
                                          "# epoch 1792000000\n# disc turbo\n0 0 0 1 0000 80 80 80 80 00 00\n"
                                          "# end 4 0\n");
    g_fast_disc = true;  // as settings.txt's disc fast or --disc=fast leave it
    const std::string said_turbo = dir + "/turbo-stderr.txt";
    capture(said_turbo, [&] { loaded = input_replay_load(dir + "/turbo", card, sram); });
    if (!loaded) return 1;
    check(!g_fast_disc &&
              lines_with(said_turbo, "[input] the log's disc line names neither fast nor console, so the replay reads at "
                                     "disc console and not at disc fast\n") == 1 &&
              lines_with(said_turbo, "no disc line") == 0,
          "a log whose disc line names neither speed replays at disc console with disc fast asked for, and says that");
    return finish("--replay-console");
}

// ---- An interrupt runs on a thread of its own (plat_install_crash_handlers), so it stops
// the gamepads' rumble while the main loop may still be running frames.
Virtual* g_rumbling;

// The interrupt's plat_exit_now runs this last, after the gamepads have been shut down.
void after_interrupt() {
    check(g_rumbling->stops > 0, "an interrupt on its own thread stops a rumbling gamepad while frames run");
    printf("test_ports --interrupt: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    fflush(stdout);
    _exit(g_failed ? 1 : 0);
}

int run_interrupt(const std::string& dir) {
    if (!sdl_up()) return 1;
    input_map_load((dir + "/no-controls.txt").c_str());
    power_on(1);
    gamepads_init();
    Virtual a{"virtual pad a"};
    attach(a);
    frame();
    g_outbuf[0] = 0x00400001;  // port 1's motor on from here
    frame();
    check(a.rumbles == 1 && a.stops == 0, "port 1's motor rumbles its gamepad");
    g_rumbling = &a;
    plat_at_exit(after_interrupt);
    std::thread(on_interrupt).detach();
    // The interrupt ends the process, or its 2 s watchdog does.
    for (const Uint64 give_up = SDL_GetTicks() + 5000; SDL_GetTicks() < give_up;) frame();
    check(false, "the interrupt ends the process");
    return finish("--interrupt");
}

// ---- --no-gamepads: a gamepad connected before the frontend starts is never opened, gets
// no port and isn't saved, and the game's rumble motor reaches nothing.
int run_no_gamepads(const std::string& dir) {
    // main()'s saves/ports.txt is relative to where it runs.
    if (chdir(dir.c_str()) != 0) return 1;
    g_gamepads = false;
    open_controllers("no-controls.txt");
    const SDL_InitFlags controllers = SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC;
    check(!(kSdlInit & controllers) && !SDL_WasInit(controllers),
          "--no-gamepads: neither main()'s SDL_Init nor open_controllers starts a joystick, gamepad or haptic subsystem");
    gamepads_shutdown();
    // From here SDL is up for the virtual gamepad, as another program's SDL would be: what's
    // checked is that the frontend leaves it alone.
    if (!sdl_up()) return 1;
    power_on(1);
    Virtual v{"virtual pad unseen"};
    attach(v, 1);
    capture("no-gamepads-stderr.txt", [] {
        open_controllers("no-controls.txt");
        frame();
    });
    const bool unopened = !SDL_GetGamepadFromID(v.id) && !SDL_GetJoystickFromID(v.id);
    // Opened here, by the test, to press its buttons: the frontend hears nothing of it.
    SDL_Joystick* j = SDL_OpenJoystick(v.id);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, true);
    frame();
    const bool unread = !read(0).buttons && !read(1).connected;
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, false);
    motor(1);
    check(unopened && lines_with("no-gamepads-stderr.txt", " on port ") == 0 && unread && v.rumbles == 0 &&
              !exists_path("saves/ports.txt") && !exists_path("saves"),
          "--no-gamepads: a gamepad connected before start is never opened, plugs no port in, rumbles for no motor, "
          "and nothing is saved");
    SDL_CloseJoystick(j);
    detach(v);
    gamepads_shutdown();
    return finish("--no-gamepads");
}

// ---- The game resetting the console ends the process (guest_reset), so the frontend's
// reset handler stops a rumbling gamepad and has it say what it sent first. Under a replay
// the handler doesn't start the game again, which leaves this process here to check.
int run_reset(const std::string& dir) {
    if (!sdl_up()) return 1;
    input_map_load((dir + "/no-controls.txt").c_str());
    plat_make_dirs(dir + "/log");
    write_file(dir + "/log/inputs.txt", "# gcn-recomp input log v5\n# game TEST00  started 2026-10-28T12:00:00\n"
                                        "# epoch 1792000000\n0 0 0 1 0000 80 80 80 80 00 00\n# end 4 0\n");
    std::string card, sram;
    if (!input_replay_load(dir + "/log", card, sram)) return 1;
    power_on(1);
    gamepads_init();
    Virtual a{"virtual pad reset"};
    attach(a);
    frame();
    g_outbuf[0] = 0x00400001;  // port 1's motor on from here
    frame();
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
// shutdown open_controllers leaves for the exit.
Virtual* g_shut;
int g_stderr = -1;  // the test's stderr, while the exit's goes to a file

// The exit runs this last, after open_controllers' shutdown.
void after_shutdown() {
    fflush(stderr);
    dup2(g_stderr, 2);
    printf("---- stderr\n%s----\n", read_file("shutdown-stderr.txt").c_str());
    check(g_shut->stops == 1 && lines_with("shutdown-stderr.txt", "gamepads: port 1 rumble on 1 time, 0 of 2 calls failed\n") == 1,
          "the game shutting the console down stops a rumbling gamepad, which says what it sent");
    printf("test_ports --shutdown: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    fflush(stdout);
    _exit(g_failed ? 1 : 0);
}

int run_shutdown(const std::string& dir) {
    // main()'s saves/ports.txt is relative to where it runs.
    if (chdir(dir.c_str()) != 0 || !sdl_up()) return 1;
    open_controllers("no-controls.txt");
    power_on(1);
    Virtual a{"virtual pad shutdown"};
    attach(a);
    frame();
    g_outbuf[0] = 0x00400001;  // port 1's motor on from here
    frame();
    check(a.rumbles == 1 && a.stops == 0, "port 1's motor rumbles its gamepad");
    g_shut = &a;
    fflush(stderr);
    g_stderr = dup(2);
    if (FILE* f = fopen("shutdown-stderr.txt", "w")) {
        dup2(fileno(f), 2);
        fclose(f);
    }
    plat_at_exit(after_shutdown);
    // What guest_reset does for a shutdown: no reset handler, only the exit.
    plat_exit_now(0);
}

// ---- A restart asked for (frontend_restart, which the overlay's Restart calls) stops the
// frontend's loop as a quit does, and the window path's end (end_run) ends the run as for a
// quit: the input log gets its end, and the exit's at-exit functions run. Then it prints a
// [reset] line, as a console reset does, and relaunches. This process isn't started again:
// the at-exit function added here runs last, and ends it, before the relaunch would. No memory
// card is made here (exi_init), so none is written.
//
// The loop claims the run's end as it stops (claim_end), so the game resetting the console
// after that (guest_reset, on another thread, with main()'s reset handler) says nothing, ends
// nothing and waits, as the loop waits for a save and as end_run runs. A restart's own [reset]
// line is the run's one, and --quit, the same with a quit, has none and no relaunch.
std::string g_restart_dir;

// The input log's `# end` lines, and the [reset] lines the run printed.
int log_ends() {
    const std::string ends = lines_starting(g_restart_dir + "/log/inputs.txt", "# end ");
    return (int)std::count(ends.begin(), ends.end(), '\n');
}
std::string resets() { return lines_starting(g_restart_dir + "/restart-stderr.txt", "[reset] "); }

void after_restart() {
    fflush(stderr);
    dup2(g_stderr, 2);
    printf("---- stderr\n%s----\n", read_file(g_restart_dir + "/restart-stderr.txt").c_str());
    check(g_relaunching == 1, "the run ends through the exit's at-exit functions on the way to one relaunch");
    check(log_ends() == 1, "the input log has one end");
    check(resets() == "[reset] the overlay's Restart game starts the game again\n",
          "one [reset] line, Restart's: the game's resets in the wait and in end_run say nothing");
    printf("test_ports --restart: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    fflush(stdout);
    _exit(g_failed ? 1 : 0);
}

void after_quit() {
    fflush(stderr);
    dup2(g_stderr, 2);
    printf("---- stderr\n%s----\n", read_file(g_restart_dir + "/restart-stderr.txt").c_str());
    check(!g_relaunching && log_ends() == 1,
          "a quit ends the run with no relaunch, though the game resets the console in the wait and in end_run");
    check(resets().empty(), "and no [reset] line");
    printf("test_ports --quit: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    fflush(stdout);
    _exit(g_failed ? 1 : 0);
}

// The game resetting the console on the guest's thread as the run waits for a save, and again
// as end_run runs (record::finish's stub).
void resets_as_it_ends() {
    std::thread(guest_reset, 0u, 0u).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    g_reset_in_finish = true;
}

// From here stderr goes to <dir>/restart-stderr.txt, and g_stderr is the test's.
void stderr_to_file(const std::string& dir) {
    fflush(stderr);
    g_stderr = dup(2);
    if (FILE* f = fopen((dir + "/restart-stderr.txt").c_str(), "w")) {
        dup2(fileno(f), 2);
        fclose(f);
    }
}

int run_restart(const std::string& dir) {
    if (!sdl_up()) return 1;
    g_restart_dir = dir;
    if (!input_log_start(dir + "/log", dir + "/no-card.raw", dir + "/no-sram.raw")) return 1;
    set_reset_handler(on_reset);
    frontend_restart();
    check(!pump_events() && !g_relaunching, "after a restart is asked for, the frontend's loop stops, as for a quit");
    claim_end();
    stderr_to_file(dir);
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
    claim_end();
    stderr_to_file(dir);
    plat_at_exit(after_quit);
    resets_as_it_ends();
    end_run();
}

// ---- In a replay the menu has no Restart game, as a relaunch would replay the log from its
// start, which the log doesn't hold. That holds after the log runs out too. A restart asked
// for all the same only ends the run, as a console reset does in a replay, with no relaunch
// and no [reset] line.
void after_restart_replay() {
    fflush(stderr);
    dup2(g_stderr, 2);
    const std::string out = g_restart_dir + "/restart-stderr.txt";
    printf("---- stderr\n%s----\n", read_file(out).c_str());
    check(!g_relaunching && lines_starting(out, "[reset] ").empty(),
          "in a replay a restart asked for ends the run with no relaunch and no [reset] line");
    printf("test_ports --restart-replay: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    fflush(stdout);
    _exit(g_failed ? 1 : 0);
}

#ifdef GCN_OVERLAY
// The bottom row of the page, over a window as in overlay_buttons, from the keyboard.
void replay_menu(const std::string& dir) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_Window* window = SDL_InitSubSystem(SDL_INIT_VIDEO) ? SDL_CreateWindow("test_ports", 1280, 960, SDL_WINDOW_HIDDEN) : nullptr;
    check(window != nullptr, "SDL's dummy video driver makes a window for the menu");
    if (!window) return;
    g_window_id = SDL_GetWindowID(window);
    OverlayPicture picture;
    picture.path = dir + "/settings.txt";
    overlay_init(window, "Test title", picture);
    {
        Guest guest;
        host_frame(guest);
        tap(guest, SDL_SCANCODE_F1, SDLK_F1);
        for (int i = 0; i < 8; i++) tap(guest, SDL_SCANCODE_DOWN, SDLK_DOWN);
        for (int i = 0; i < 2; i++) tap(guest, SDL_SCANCODE_LEFT, SDLK_LEFT);
        tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
        check(waiting(OverlayAsk::quit), "in a replay the bottom row of the page is Quit game alone");
        tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
        tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    }
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
#endif

int run_restart_replay(const std::string& dir) {
    if (!sdl_up()) return 1;
    g_restart_dir = dir;
    plat_make_dirs(dir + "/log");
    write_file(dir + "/log/inputs.txt", "# gcn-recomp input log v5\n# game TEST00  started 2026-10-31T12:00:00\n"
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
    stderr_to_file(dir);
    plat_at_exit(after_restart_replay);
    end_run();
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
    if (argc == 3 && !strcmp(argv[1], "--shutdown")) return run_shutdown(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--restart")) return run_restart(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--quit")) return run_quit(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--restart-replay")) return run_restart_replay(argv[2]);
    fprintf(stderr, "usage: test_ports [--record|--replay|--replay-no-sram|--replay-v4|--replay-console|--interrupt|"
                    "--no-gamepads|--reset|--shutdown|--restart|--quit|--restart-replay] <work dir>\n");
    return 2;
}
