// Gamepad port checks on SDL's virtual joysticks: the `test_ports` target, run by
// tools/test_ports.sh.
//
// runtime/main.cpp is compiled into this file with its main() renamed, so a frame's input
// is the frontend's own pump_events / update_pads / merge_port, and runtime/hw/si.cpp too,
// so each port is read back the way the guest reads it: pad_get, with scripted presses
// (input_script.cpp) applied and the input log (input_log.cpp) consulted. gamepads.cpp,
// input_map.cpp, input_defaults.cpp, input_script.cpp, input_log.cpp, platform.cpp and
// hw/mmio.cpp are linked as they are. Stubbed: force feedback (ffb_configure, ffb_attach,
// ffb_update, ffb_shutdown; ffb_update keeps the motor it was given), the RTC
// (rtc_set_base), the event queue and interrupt request (event_schedule_in, irq_request),
// and the frame counter (gx::g_frames_submitted, gx_frames_submitted), which a check sets
// by hand.
//
// usage: test_ports <empty work dir>            the port checks
//        test_ports --record <empty work dir>   two gamepads, played and logged
//        test_ports --replay <that work dir>    that log replayed with none
// Files are written into the work dir and left there.
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
    int player = -1;  // the player index SDL last told the device
};

bool SDLCALL on_rumble(void* u, Uint16 low, Uint16 high) {
    if (low || high) static_cast<Virtual*>(u)->rumbles++;
    return true;
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
    d.userdata = &v;
    d.Rumble = on_rumble;
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
    std::rename(beside.c_str(), (dir + "/gamecontrollerdb.beside.txt").c_str());
    unplug({&stick});
    gamepads_shutdown();
}

int run_ports(const std::string& dir) {
    if (!sdl_up()) return 1;
    input_map_load((dir + "/no-controls.txt").c_str());
    // Port 2 and port 3 pressed B by script from frame 1000 to 1099.
    input_script_init("2/1000:B:100,3/1000:B:100");
    rules();
    launch(dir);
    pads2();
    named(dir);
    pins(dir);
    pins_more(dir);
    mappings(dir);
    return finish("ports");
}

// ---- Recording and replay: the same reads in both runs. Each step sets what the live
// gamepads do (when there are any), runs a frame and reads the four ports once.
const int kSteps = 6;

std::string describe(const PadState& p) {
    char s[64];
    snprintf(s, sizeof(s), "%d %04X %02X %02X %02X %02X %02X %02X\n", p.connected ? 1 : 0, p.buttons, p.stick_x, p.stick_y,
             p.cstick_x, p.cstick_y, p.trig_l, p.trig_r);
    return s;
}

std::string steps(Virtual* a, Virtual* b, std::vector<PadState>& seen) {
    std::string out;
    for (int s = 0; s < kSteps; s++) {
        SDL_Joystick* ja = a ? SDL_GetJoystickFromID(a->id) : nullptr;
        SDL_Joystick* jb = b ? SDL_GetJoystickFromID(b->id) : nullptr;
        if (jb) SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, s == 1 || s == 2);
        if (ja) SDL_SetJoystickVirtualButton(ja, SDL_GAMEPAD_BUTTON_EAST, s == 3);
        if (jb) SDL_SetJoystickVirtualAxis(jb, SDL_GAMEPAD_AXIS_LEFTX, s == 4 ? 32767 : 0);
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
    if (!input_log_start(dir + "/log", dir + "/no-card.raw")) return 1;
    std::vector<PadState> seen;
    const std::string text = steps(&a, &b, seen);
    input_log_end();
    write_file(dir + "/expected.txt", text);
    bool two = true, rest = true;
    for (int s = 0; s < kSteps; s++) {
        two = two && seen[s * 4 + 1].connected;
        rest = rest && !seen[s * 4 + 2].connected && !seen[s * 4 + 3].connected;
    }
    check(two && rest && seen[1 * 4 + 1].buttons == PAD_A && seen[2 * 4 + 1].buttons == PAD_A && seen[3 * 4].buttons == PAD_X &&
              seen[4 * 4 + 1].stick_x == 228 && at_rest(seen[5 * 4]) && at_rest(seen[5 * 4 + 1]),
          "recording with --pads=1: port 2's gamepad (A, then the stick) and port 1's (east button, as X) reach the guest; ports 3 and 4 unplugged");
    unplug({&a, &b});
    gamepads_shutdown();
    return finish("--record");
}

int run_replay(const std::string& dir) {
    if (!sdl_up()) return 1;
    input_map_load((dir + "/no-controls.txt").c_str());
    input_script_init("");
    std::string card;
    if (!input_replay_load(dir + "/log", card)) return 1;
    power_on(1);
    gamepads_init();
    frame();
    std::vector<PadState> seen;
    const std::string text = steps(nullptr, nullptr, seen);
    check(text == read_file(dir + "/expected.txt"),
          "replayed with no gamepad and --pads=1: every read of all four ports is the recording's");
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

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) return run_ports(argv[1]);
    if (argc == 3 && !strcmp(argv[1], "--record")) return run_record(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--replay")) return run_replay(argv[2]);
    fprintf(stderr, "usage: test_ports [--record|--replay] <work dir>\n");
    return 2;
}
