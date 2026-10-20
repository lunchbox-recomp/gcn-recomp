// See gamepads.h.
#include "gamepads.h"
#include "input_map.h"
#include "platform.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
// After the standard headers: SDL_endian.h trips over clang's intrinsics otherwise.
#include <SDL3/SDL.h>

namespace {

struct Identity {
    std::string guid;    // SDL_GUIDToString's; empty while no gamepad has had the port
    std::string serial;  // lower case, without ':' or '-'; empty when the device gives none
};

struct Port {
    SDL_Gamepad* pad = nullptr;
    SDL_JoystickID which = 0;  // the pad's instance ID
    Identity id;               // the gamepad on the port, or the last one that was
    bool held = false;         // its gamepad was pulled out and has not come back
    bool rumbling = false;
};

Port g_ports[4];
std::string g_save;  // empty: the ports are not kept

std::string normal_serial(const char* s) {
    std::string out;
    for (; s && *s; s++)
        if (*s != ':' && *s != '-') out += (char)tolower((unsigned char)*s);
    return out;
}

Identity identity(SDL_Gamepad* g) {
    char guid[33];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(SDL_GetGamepadID(g)), guid, sizeof(guid));
    return {guid, normal_serial(SDL_GetGamepadSerial(g))};
}

// The device's own name, as controls.txt and --list-controls give it: the gamepad's, from
// its mapping, can be one model's name shared by several.
const char* pad_name(SDL_Gamepad* g) {
    const char* n = SDL_GetJoystickName(SDL_GetGamepadJoystick(g));
    return n ? n : "?";
}

// Whether `now` is the gamepad `saved` was: the same GUID, and the same serial number if
// one was saved; or else the same serial number on the same vendor and product, the GUID
// having changed with the way it is connected.
enum class Match { None, Fallback, Exact };
Match match(const Identity& saved, const Identity& now) {
    if (saved.guid.empty()) return Match::None;
    const bool serial = !saved.serial.empty() && saved.serial == now.serial;
    if (saved.guid == now.guid) return saved.serial.empty() || serial ? Match::Exact : Match::None;
    if (!serial) return Match::None;
    Uint16 v0 = 0, p0 = 0, v1 = 0, p1 = 0;
    SDL_GetJoystickGUIDInfo(SDL_StringToGUID(saved.guid.c_str()), &v0, &p0, nullptr, nullptr);
    SDL_GetJoystickGUIDInfo(SDL_StringToGUID(now.guid.c_str()), &v1, &p1, nullptr, nullptr);
    return v0 && v0 == v1 && p0 && p0 == p1 ? Match::Fallback : Match::None;
}

// The ports a gamepad may take, best first. The last is a port held for another gamepad,
// taken when nothing else is left: a player who has picked up a different controller.
enum class Pick { HeldExact, HeldFallback, SavedExact, SavedFallback, Free, Held };

bool fits(const Port& p, Pick k, const Identity& id) {
    switch (k) {
    case Pick::HeldExact: return p.held && match(p.id, id) == Match::Exact;
    case Pick::HeldFallback: return p.held && match(p.id, id) == Match::Fallback;
    case Pick::SavedExact: return !p.held && match(p.id, id) == Match::Exact;
    case Pick::SavedFallback: return !p.held && match(p.id, id) == Match::Fallback;
    case Pick::Free: return !p.held;
    case Pick::Held: return p.held;
    }
    return false;
}

// The port for `id` among those without a gamepad, by the picks up to `last`; -1 if none.
int choose(const Identity& id, Pick last) {
    for (int k = 0; k <= (int)last; k++)
        for (int i = 0; i < 4; i++)
            if (!g_ports[i].pad && fits(g_ports[i], (Pick)k, id)) return i;
    return -1;
}

int port_of(SDL_JoystickID which) {
    for (int i = 0; i < 4; i++)
        if (g_ports[i].pad && g_ports[i].which == which) return i;
    return -1;
}

void load() {
    FILE* f = g_save.empty() ? nullptr : fopen(g_save.c_str(), "r");
    if (!f) return;
    char line[512], guid[64], serial[400];
    while (fgets(line, sizeof(line), f)) {
        int port = 0;
        if (sscanf(line, "%d %63s %399[^\r\n]", &port, guid, serial) != 3 || port < 1 || port > 4) continue;
        g_ports[port - 1].id = {guid, strcmp(serial, "-") ? serial : ""};
    }
    fclose(f);
}

void save() {
    if (g_save.empty()) return;
    const size_t slash = g_save.find_last_of("/\\");
    if (slash != std::string::npos) plat_make_dirs(g_save.substr(0, slash));
    const std::string tmp = g_save + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    bool ok = f != nullptr;
    if (f) {
        fprintf(f, "# port, SDL GUID, serial number (- for none): the gamepad last on each port\n");
        for (int i = 0; i < 4; i++) {
            const Identity& id = g_ports[i].id;
            if (!id.guid.empty()) fprintf(f, "%d %s %s\n", i + 1, id.guid.c_str(), id.serial.empty() ? "-" : id.serial.c_str());
        }
        ok = fclose(f) == 0 && plat_replace_file(tmp.c_str(), g_save.c_str());
    }
    if (!ok) fprintf(stderr, "gamepads: cannot write %s\n", g_save.c_str());
}

// `g` onto a port by the picks up to `last`. False, and `g` left as it was, if none fits.
bool place(SDL_Gamepad* g, Pick last) {
    const Identity id = identity(g);
    const int i = choose(id, last);
    if (i < 0) return false;
    Port& p = g_ports[i];
    const bool back = p.held && match(p.id, id) != Match::None;
    const bool changed = p.id.guid != id.guid || p.id.serial != id.serial;
    p.pad = g;
    p.which = SDL_GetGamepadID(g);
    p.id = id;
    p.held = false;
    p.rumbling = false;
    SDL_SetGamepadPlayerIndex(g, i);
    fprintf(stderr, "gamepads: \"%s\" %son port %d\n", pad_name(g), back ? "back " : "", i + 1);
    if (changed) save();
    return true;
}

void leave_out(SDL_Gamepad* g) {
    fprintf(stderr, "gamepads: \"%s\" left out: all four ports are taken\n", pad_name(g));
    SDL_SetGamepadPlayerIndex(g, -1);
    SDL_CloseGamepad(g);
}

SDL_Gamepad* open_pad(SDL_JoystickID which) {
    // A device controls.txt binds is the file's alone.
    if (input_map_binds(which)) return nullptr;
    SDL_Gamepad* g = SDL_OpenGamepad(which);
    if (!g) {
        const char* n = SDL_GetJoystickNameForID(which);
        fprintf(stderr, "gamepads: cannot open \"%s\": %s\n", n ? n : "?", SDL_GetError());
    }
    return g;
}

uint8_t axis_to_u8(int v, bool invert) {
    float f = v / 32767.0f;
    if (invert) f = -f;
    int r = 128 + (int)(f * 100.0f);
    return (uint8_t)std::clamp(r, 0, 255);
}

}  // namespace

void gamepads_load_mappings(const char* path) {
    std::string file = path ? path : "";
    if (!path) {
        const char* base = SDL_GetBasePath();
        if (!base) return;
        file = std::string(base) + "gamecontrollerdb.txt";
        if (!plat_readable(file.c_str())) return;
    }
    // SDL takes only the lines for this platform (their `platform:` field), and counts only
    // the mappings it did not have: one that replaces its own is used but not counted.
    const int n = SDL_AddGamepadMappingsFromFile(file.c_str());
    if (n < 0) fprintf(stderr, "gamepads: cannot read mappings from %s: %s\n", file.c_str(), SDL_GetError());
    else fprintf(stderr, "gamepads: %d new mapping%s from %s\n", n, n == 1 ? "" : "s", file.c_str());
}

void gamepads_init(const char* save_path) {
    g_save = save_path ? save_path : "";
    load();
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    std::vector<SDL_Gamepad*> found;
    for (int i = 0; ids && i < n; i++)
        if (SDL_Gamepad* g = open_pad(ids[i])) found.push_back(g);
    SDL_free(ids);
    // Those the file knows first, the surest first, so that a gamepad met earlier in SDL's
    // list cannot take a port another had.
    for (Pick last : {Pick::SavedExact, Pick::SavedFallback, Pick::Held})
        for (SDL_Gamepad*& g : found)
            if (g && place(g, last)) g = nullptr;
    for (SDL_Gamepad* g : found)
        if (g) leave_out(g);
}

void gamepads_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
        // SDL also announces the gamepads gamepads_init found.
        if (port_of(e.gdevice.which) >= 0) return;
        if (SDL_Gamepad* g = open_pad(e.gdevice.which))
            if (!place(g, Pick::Held)) leave_out(g);
    } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
        const int i = port_of(e.gdevice.which);
        if (i < 0) return;
        Port& p = g_ports[i];
        fprintf(stderr, "gamepads: \"%s\" pulled out of port %d, which stays plugged in for it\n", pad_name(p.pad), i + 1);
        SDL_CloseGamepad(p.pad);
        p.pad = nullptr;
        p.which = 0;
        p.held = true;
        p.rumbling = false;
    }
}

void gamepads_merge(int port, PadState& p) {
    const Port& g = g_ports[port];
    if (g.held) p.connected = true;
    SDL_Gamepad* c = g.pad;
    if (!c) return;
    p.connected = true;
    auto b = [&](SDL_GamepadButton btn) { return SDL_GetGamepadButton(c, btn); };
    if (b(SDL_GAMEPAD_BUTTON_SOUTH)) p.buttons |= PAD_A;
    if (b(SDL_GAMEPAD_BUTTON_WEST)) p.buttons |= PAD_B;
    if (b(SDL_GAMEPAD_BUTTON_EAST)) p.buttons |= PAD_X;
    if (b(SDL_GAMEPAD_BUTTON_NORTH)) p.buttons |= PAD_Y;
    if (b(SDL_GAMEPAD_BUTTON_START)) p.buttons |= PAD_START;
    if (b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) p.buttons |= PAD_Z;
    if (b(SDL_GAMEPAD_BUTTON_DPAD_UP)) p.buttons |= PAD_UP;
    if (b(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) p.buttons |= PAD_DOWN;
    if (b(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) p.buttons |= PAD_LEFT;
    if (b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) p.buttons |= PAD_RIGHT;
    int lx = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTX);
    int ly = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTY);
    if (abs(lx) > 4000 || abs(ly) > 4000) { p.stick_x = axis_to_u8(lx, false); p.stick_y = axis_to_u8(ly, true); }
    int rx = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTX);
    int ry = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTY);
    if (abs(rx) > 1000 || abs(ry) > 1000) { p.cstick_x = axis_to_u8(rx, false); p.cstick_y = axis_to_u8(ry, true); }
    int lt = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    int rt = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    if (lt > 1000) { p.trig_l = std::max(p.trig_l, (uint8_t)(lt * 255 / 32767)); if (lt > 30000) p.buttons |= PAD_L; }
    if (rt > 1000) { p.trig_r = std::max(p.trig_r, (uint8_t)(rt * 255 / 32767)); if (rt > 30000) p.buttons |= PAD_R; }
}

void gamepads_rumble(int port, bool motor) {
    Port& g = g_ports[port];
    if (g.pad && (motor || g.rumbling)) SDL_RumbleGamepad(g.pad, motor ? 0x6000 : 0, motor ? 0xA000 : 0, motor ? 200 : 0);
    g.rumbling = motor;
}

void gamepads_shutdown() {
    for (Port& g : g_ports) {
        if (g.pad) {
            if (g.rumbling) SDL_RumbleGamepad(g.pad, 0, 0, 0);
            SDL_CloseGamepad(g.pad);
        }
        g = Port();
    }
}

// ---------------------------------------------------------------------------
// --test-ports

namespace {

// One of SDL's virtual joysticks. It has no serial number, and no USB IDs: its GUID comes
// from its name.
struct Virtual {
    const char* name;
    SDL_JoystickID id = 0;
    int rumbles = 0;  // rumbles started on it
};

bool SDLCALL count_rumble(void* userdata, Uint16 low, Uint16 high) {
    if (low || high) static_cast<Virtual*>(userdata)->rumbles++;
    return true;
}

// Plugs `v` in: a gamepad with every button and axis SDL has, its button and axis numbers
// SDL's own, or a plain joystick with two axes and four buttons, which SDL has no mapping for.
void attach(Virtual& v, bool gamepad = true) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = gamepad ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
    d.naxes = gamepad ? SDL_GAMEPAD_AXIS_COUNT : 2;
    d.nbuttons = gamepad ? SDL_GAMEPAD_BUTTON_COUNT : 4;
    d.name = v.name;
    d.userdata = &v;
    d.Rumble = count_rumble;
    v.id = SDL_AttachVirtualJoystick(&d);
    if (!v.id) fprintf(stderr, "SDL_AttachVirtualJoystick: %s\n", SDL_GetError());
}

void detach(Virtual& v) {
    SDL_DetachVirtualJoystick(v.id);
    v.id = 0;
}

// The events gamepads_event would see, but for controllers of the host's that turn up
// halfway: the test's own come and go, and a removal is of nothing on a port otherwise.
void pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type != SDL_EVENT_GAMEPAD_ADDED || SDL_IsJoystickVirtual(e.gdevice.which)) gamepads_event(e);
}

// The port `v` is on, 1 to 4, and 0 for none; -1 if its SDL player index says otherwise.
int port(const Virtual& v) {
    const int i = v.id ? port_of(v.id) : -1;
    if (i < 0) return 0;
    return SDL_GetGamepadPlayerIndex(g_ports[i].pad) == i ? i + 1 : -1;
}

PadState merged(int i) {
    PadState p;
    gamepads_merge(i, p);
    return p;
}

bool at_rest(const PadState& p) {
    return !p.buttons && p.stick_x == 0x80 && p.stick_y == 0x80 && p.cstick_x == 0x80 && p.cstick_y == 0x80 &&
           !p.trig_l && !p.trig_r;
}

int g_checks, g_failed;

void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}

}  // namespace

int gamepads_test() {
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int real = 0;
    SDL_free(SDL_GetGamepads(&real));
    if (real) {
        printf("--test-ports needs the ports to itself: disconnect the %d gamepad%s first.\n", real, real == 1 ? "" : "s");
        SDL_Quit();
        return 1;
    }
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
    const std::string save = (tmp / "gcn-test-ports.txt").string();
    const std::string mappings = (tmp / "gcn-test-ports-mappings.txt").string();
    std::filesystem::remove(save, ec);
    printf("Virtual gamepads on the four ports, with the ports saved in %s.\n", save.c_str());
    fflush(stdout);
    gamepads_init(save.c_str());

    Virtual pads[] = {{"virtual pad 1"}, {"virtual pad 2"}, {"virtual pad 3"}, {"virtual pad 4"},
                      {"virtual pad 5"}, {"virtual pad 6"}};
    Virtual &a = pads[0], &b = pads[1], &c = pads[2], &d = pads[3], &e = pads[4], &f = pads[5];
    for (int i = 0; i < 4; i++) {
        attach(pads[i]);
        pump();
    }
    check(port(a) == 1 && port(b) == 2 && port(c) == 3 && port(d) == 4,
          "four gamepads land on ports 1 to 4 in the order they connect, each with that player index");

    SDL_Joystick* jb = SDL_GetJoystickFromID(b.id);
    SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualAxis(jb, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    pump();
    const PadState p2 = merged(1);
    check(p2.buttons == PAD_A && p2.stick_x == 228 && at_rest(merged(0)) && at_rest(merged(2)) && at_rest(merged(3)),
          "port 2's gamepad presses A and pushes the stick on port 2 alone");
    SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualAxis(jb, SDL_GAMEPAD_AXIS_LEFTX, 0);
    pump();

    gamepads_rumble(2, true);
    check(c.rumbles == 1 && a.rumbles + b.rumbles + d.rumbles == 0, "port 3's rumble motor runs port 3's gamepad alone");
    gamepads_rumble(2, false);

    detach(b);
    pump();
    const PadState out = merged(1);
    gamepads_rumble(1, true);  // the game's motor on the empty port: nothing to run
    gamepads_rumble(1, false);
    check(out.connected && at_rest(out) && port(a) == 1 && port(c) == 3 && port(d) == 4,
          "port 2's gamepad pulled out: port 2 stays plugged in, at rest, and the others stay put");

    attach(b);
    pump();
    check(port(b) == 2, "plugged back in, it is on port 2 again");

    attach(e);
    pump();
    check(port(e) == 0 && port(a) == 1 && port(b) == 2 && port(c) == 3 && port(d) == 4,
          "a fifth gamepad is left out, and the four keep their ports");

    detach(e);
    detach(c);
    pump();
    attach(e);
    pump();
    attach(c);
    pump();
    check(port(e) == 3 && port(c) == 0,
          "with port 3's gamepad gone and the rest taken, another takes port 3, and the first is then left out");

    // The next launch: shut down with every gamepad pulled out, then start again with only
    // the ones on ports 4 and 2 connected, and a new one, in that order. Their arrivals
    // are still queued when gamepads_init looks, as at a program's start.
    for (Virtual& v : pads)
        if (v.id) detach(v);
    pump();
    gamepads_shutdown();
    attach(d);
    attach(b);
    attach(f);
    gamepads_init(save.c_str());
    pump();
    check(port(d) == 4 && port(b) == 2 && port(f) == 1,
          "next launch: ports 4 and 2 again, from the file, and the new gamepad on port 1, the first free");

    Virtual stick{"virtual stick"};
    attach(stick, false);
    pump();
    const bool plain = !SDL_IsGamepad(stick.id) && port(stick) == 0;
    char guid[33];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(stick.id), guid, sizeof(guid));
    if (FILE* m = fopen(mappings.c_str(), "w")) {
        fprintf(m, "# For --test-ports, in SDL_GameControllerDB's format\n");
        fprintf(m, "%s,virtual stick,a:b0,b:b1,x:b2,y:b3,leftx:a0,lefty:a1,platform:%s,\n", guid, SDL_GetPlatform());
        fclose(m);
    }
    gamepads_load_mappings(mappings.c_str());
    pump();
    check(plain && SDL_IsGamepad(stick.id) && port(stick) == 3,
          "a joystick SDL has no mapping for is a gamepad once a mappings file names it, and takes port 3");

    gamepads_shutdown();
    for (Virtual& v : pads)
        if (v.id) detach(v);
    detach(stick);
    std::filesystem::remove(save, ec);
    std::filesystem::remove(mappings, ec);
    SDL_Quit();
    printf("--test-ports: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
