// Gamepad port checks on SDL's virtual joysticks: the `test_ports` target, run by
// tools/test_ports.sh.
//
// runtime/main.cpp is compiled into this file with its main() renamed, so a frame's input
// is the frontend's own pump_events / update_pads / merge_port, and runtime/hw/si.cpp too,
// so each port is read back the way the guest reads it: pad_get, with scripted presses
// (input_script.cpp) applied and the input log (input_log.cpp) consulted. gamepads.cpp,
// port_memory.cpp, input_map.cpp, input_defaults.cpp, input_script.cpp, input_log.cpp,
// platform.cpp and hw/mmio.cpp are linked as they are. Stubbed: force feedback (ffb_configure, ffb_attach,
// ffb_update, ffb_shutdown; ffb_update keeps the motor it was given), the RTC
// (rtc_set_base), the event queue and interrupt request (event_schedule_in, irq_request),
// and the frame counter (gx::g_frames_submitted, gx_frames_submitted), which a check sets
// by hand. The interrupt handler's thread dump (debug_dump_threads) and guest memory
// (g_mem) are stubbed too, and fatal, which says why and exits 1.
//
// usage: test_ports <empty work dir>            the port checks
//        test_ports --record <empty work dir>   two gamepads, played and logged
//        test_ports --replay <that work dir>    that log replayed with none
//        test_ports --replay-no-sram <that work dir>   that log again, its SRAM snapshot gone
//        test_ports --replay-v4 <empty work dir>   a version 4 log written by hand, replayed
//        test_ports --interrupt <empty work dir>   the interrupt handler run beside the
//                                                  main loop, which ends the process
//        test_ports --no-gamepads <empty work dir>   the frontend under --no-gamepads, with
//                                                    a gamepad connected before it starts
// Files are written into the work dir and left there.
//
// Only SDL's virtual joysticks are seen: main() turns off every driver that finds real
// controllers before anything starts SDL, so a controller connected beside the run is never
// opened, rumbled or read.
#define main gcn_frontend_main
#include "main.cpp"
#undef main
#include "hw/si.cpp"
#include "ffb.h"
#include "gx/render.h"
#include <climits>
#include <functional>
#include <initializer_list>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace gx {
std::atomic<uint32_t> g_frames_submitted{0};
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

struct Virtual {
    const char* name;
    bool gamepad = true;
    SDL_JoystickID id = 0;
    int rumbles = 0;  // rumbles started on it
    int stops = 0;    // rumbles stopped on it
    int player = -1;  // the player index SDL last told the device
    bool can_rumble = true;  // false: it has no rumble, as SDL sees it
    bool refuses = false;    // a rumble asked of it fails, saying why
    Uint16 vendor = 0, product = 0;  // its USB IDs, from which SDL takes its type and labels
};

bool SDLCALL on_rumble(void* u, Uint16 low, Uint16 high) {
    Virtual* v = static_cast<Virtual*>(u);
    if (low || high) v->rumbles++;
    else v->stops++;
    return v->refuses ? SDL_SetError("virtual pad refuses rumble") : true;
}

void SDLCALL on_player(void* u, int index) { static_cast<Virtual*>(u)->player = index; }

// The player index SDL has for `v`.
int player(const Virtual& v) { return SDL_GetJoystickPlayerIndexForID(v.id); }

// Plugs `v` in: a gamepad with every button and axis SDL has, in SDL's order, or a plain
// joystick with two axes and four buttons, which SDL has no mapping for. With `index`, its
// player index is set to that before the frontend hears of it, as Steam Input's slot or
// an XInput user index would be.
void attach(Virtual& v, int index = kSdlIndex) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = v.gamepad ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
    d.naxes = v.gamepad ? SDL_GAMEPAD_AXIS_COUNT : 2;
    d.nbuttons = v.gamepad ? SDL_GAMEPAD_BUTTON_COUNT : 4;
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
// and why its rumble failed, once after each on or off.
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

    // Three presses of south and one each of east and misc6, which no port takes; the left
    // stick to both ends, the right stick down a little and the left trigger all the way.
    for (int k = 0; k < 3; k++) {
        press(a, SDL_GAMEPAD_BUTTON_SOUTH, true);
        press(a, SDL_GAMEPAD_BUTTON_SOUTH, false);
    }
    for (const int b : {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_MISC6}) {
        press(a, b, true);
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

    // A rumble that fails is said once after each on or off, not for each call: on for five
    // frames and off, on for three and off, then on for two, accepted, and off, refused.
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
        g_outbuf[0] = 0x00400000;
        frame();
        frame();
    });
    capture(dir + "/said-failing-out.txt", [&] { unplug({&f}); });
    check(lines_with(failing, "gamepads: port 1 rumble failed: virtual pad refuses rumble\n") == 3 &&
              lines_with(failing, "rumble failed") == 3 &&
              lines_with(dir + "/said-failing-out.txt", "gamepads: port 1 rumble on 3 times, 9 of 13 calls failed\n") == 1,
          "a rumble that fails says why once after each on or off, and the pull-out line counts the failed calls");

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

int run_ports(const std::string& dir) {
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
    labels(dir);
    return finish("ports");
}

// ---- Recording and replay: the same reads in both runs. Each step sets what the live
// gamepads do (when there are any) and the game's rumble motors, runs a frame and reads the
// four ports once.
const int kSteps = 6;

// The motor lines a log of those steps has: port 1's motor on at step 1 and still on at step
// 2, off at step 3, and port 2's on from step 4 to the end. Each is written at the poll that
// reads the port (the step's polls are 4 * step to 4 * step + 3), at frame 0.
const char* const kMotorLines = "motor 4 0 0 1\nmotor 12 0 0 0\nmotor 17 0 1 1\n";

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
            seen.push_back(read(i));
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
    capture(said, [&] { loaded = input_replay_load(dir + "/log", card, sram); });
    if (!loaded) return 1;
    check(sram == dir + "/log/sram_replay.raw" && read_file(sram) == "the SRAM the run started with\n",
          "a replay gets a scratch copy of the logged SRAM");
    check(lines_with(said, "(with its SRAM snapshot)") == 1, "and its summary line says the log had an SRAM snapshot");
    // Logged as the frontend logs a replay, into a log of its own.
    if (!input_log_start(dir + "/replay-log", dir + "/no-card.raw", sram)) return 1;
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
    return finish("--replay-no-sram");
}

// ---- A version 4 log, from before the motor lines, replays as it always did: three steps
// of reads of the four ports, each port at its logged state from its line's poll on.
int run_replay_v4(const std::string& dir) {
    plat_make_dirs(dir + "/log");
    write_file(dir + "/log/inputs.txt", "# gcn-recomp input log v4\n# game TEST00  started 2026-10-28T12:00:00\n"
                                        "# epoch 1792000000\n"
                                        "0 0 0 1 0000 80 80 80 80 00 00\n0 0 1 0 0000 80 80 80 80 00 00\n"
                                        "0 0 2 0 0000 80 80 80 80 00 00\n0 0 3 0 0000 80 80 80 80 00 00\n"
                                        "5 0 1 1 0100 E4 80 80 80 00 00\n8 0 0 1 1000 80 80 80 80 00 00\n# end 12 0\n");
    std::string card, sram;
    if (!input_replay_load(dir + "/log", card, sram)) return 1;
    input_script_init("");
    power_on(1);
    std::string text;
    for (int s = 0; s < 3; s++)
        for (int i = 0; i < 4; i++) text += describe(read(i));
    const std::string rest = "1 0000 80 80 80 80 00 00\n", out = "0 0000 80 80 80 80 00 00\n";
    const std::string with_a = "1 0100 E4 80 80 80 00 00\n", start = "1 1000 80 80 80 80 00 00\n";
    check(text == rest + out + out + out + rest + with_a + out + out + start + with_a + out + out,
          "a version 4 log replays: every read of all four ports is its logged state");
    return finish("--replay-v4");
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
    check(!SDL_WasInit(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC),
          "--no-gamepads starts no joystick, gamepad or haptic subsystem");
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

}  // namespace

int main(int argc, char** argv) {
    // Before anything starts SDL: no driver that finds real controllers, so only the virtual
    // joysticks are seen. HIDAPI, IOKit and MFi have switches. Linux's evdev driver has none,
    // so the gamepad allow list names only a USB vendor that's reserved (FFFF), which turns
    // away every device a driver finds; SDL's virtual driver doesn't consult it.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_IOKIT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xFFFF/0xFFFF");
    if (argc == 2) return run_ports(argv[1]);
    if (argc == 3 && !strcmp(argv[1], "--record")) return run_record(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--replay")) return run_replay(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--replay-no-sram")) return run_replay_without_sram(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--replay-v4")) return run_replay_v4(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--interrupt")) return run_interrupt(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--no-gamepads")) return run_no_gamepads(argv[2]);
    fprintf(stderr, "usage: test_ports [--record|--replay|--replay-no-sram|--replay-v4|--interrupt|--no-gamepads] <work dir>\n");
    return 2;
}
