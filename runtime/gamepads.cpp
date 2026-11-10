// See gamepads.h.
#include "gamepads.h"
#include "input_map.h"
#include "overlay/overlay.h"  // the notices
#include "platform.h"
#include "port_memory.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
// After the standard headers: SDL_endian.h trips over clang's intrinsics otherwise.
#include <SDL3/SDL.h>

namespace {

struct Port {
    SDL_Gamepad* pad = nullptr;
    SDL_JoystickID which = 0;  // the pad's instance ID
    bool rumbling = false;
    std::array<SDL_GamepadButton, 4> face{};  // the GameCube's A, B, X and Y on this gamepad (faces)
    // What the gamepad sent while on the port (gamepads_merge) and how its rumble went
    // (gamepads_rumble), said when it leaves the port (say_sent).
    bool read = false;  // merged at least once, so that lo and hi hold values
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> down{};
    std::array<int, SDL_GAMEPAD_BUTTON_COUNT> presses{};
    std::array<int, SDL_GAMEPAD_AXIS_COUNT> lo{}, hi{};  // each axis's extents, raw
    int rumbles = 0;              // times the rumble was turned on
    int calls = 0, failed = 0;    // SDL_RumbleGamepad calls, and those that failed
    std::vector<std::string> reasons_said;  // SDL's errors for them said since the pad landed
};

Port g_ports[4];
std::vector<SDL_JoystickID> g_left_out;  // gamepads connected while no port was free

// The device's own name, as controls.txt and --list-controls give it: the gamepad's, from
// its mapping, can be one model's name shared by several.
const char* pad_name(SDL_Gamepad* g) {
    const char* n = SDL_GetJoystickName(SDL_GetGamepadJoystick(g));
    return n ? n : "?";
}

int port_of(SDL_JoystickID which) {
    for (int i = 0; i < 4; i++)
        if (g_ports[i].pad && g_ports[i].which == which) return i;
    return -1;
}

const char* label_name(SDL_GamepadButtonLabel l) {
    static const char* const kNames[] = {"unknown", "A", "B", "X", "Y", "cross", "circle", "square", "triangle"};
    return l >= 0 && l < (int)std::size(kNames) ? kNames[l] : "unknown";
}

// The SDL buttons that are the GameCube's A, B, X and Y on `g`: its face buttons labeled A,
// B, X and Y, wherever the gamepad has them (south B and east A on Nintendo's own), by the
// labels SDL reports for its type. A gamepad whose faces carry no letters, as PlayStation's
// don't, has them by Xbox position.
std::array<SDL_GamepadButton, 4> faces(SDL_Gamepad* g) {
    static constexpr std::array<SDL_GamepadButton, 4> kXbox = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                               SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
    static constexpr SDL_GamepadButtonLabel kLetters[] = {SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_B,
                                                          SDL_GAMEPAD_BUTTON_LABEL_X, SDL_GAMEPAD_BUTTON_LABEL_Y};
    std::array<SDL_GamepadButton, 4> got{};
    int found = 0;
    for (int k = 0; k < 4; k++)
        for (const SDL_GamepadButton f : kXbox)
            if (SDL_GetGamepadButtonLabel(g, f) == kLetters[k]) {
                got[k] = f;
                found++;
                break;
            }
    return found == 4 ? got : kXbox;
}

// What SDL knows of the gamepad on port `i`, said as it lands there: its type, how it's
// connected, its USB IDs, whether it has rumble, and the labels on its south and east
// buttons, which say whose layout it has.
void say_landed(int i) {
    SDL_Gamepad* g = g_ports[i].pad;
    const char* type = SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
    const SDL_JoystickConnectionState c = SDL_GetGamepadConnectionState(g);
    const bool rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    fprintf(stderr, "gamepads: port %d is %s, %s, %04x:%04x, rumble %s, south label %s, east label %s\n", i + 1,
            type ? type : "unknown",
            c == SDL_JOYSTICK_CONNECTION_WIRED ? "wired" : c == SDL_JOYSTICK_CONNECTION_WIRELESS ? "wireless" : "unknown",
            SDL_GetGamepadVendor(g), SDL_GetGamepadProduct(g), rumble ? "yes" : "no",
            label_name(SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_SOUTH)),
            label_name(SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_EAST)));
}

// What the gamepad on port `i` sent while there, said as it leaves: each SDL button it
// pressed and how many times, each axis's lowest and highest raw value, then how often its
// rumble was turned on and how many calls for it failed.
void say_sent(int i) {
    static const char* const kButtons[] = {
        "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick",
        "leftshoulder", "rightshoulder", "up", "down", "left", "right", "misc1", "rightpaddle1",
        "leftpaddle1", "rightpaddle2", "leftpaddle2", "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6"};
    static_assert(std::size(kButtons) == SDL_GAMEPAD_BUTTON_COUNT, "a name for each of SDL's buttons");
    const Port& g = g_ports[i];
    if (!g.read) {
        fprintf(stderr, "gamepads: port %d was never read\n", i + 1);
    } else {
        std::string s;
        for (int k = 0; k < SDL_GAMEPAD_BUTTON_COUNT; k++)
            if (g.presses[k]) s += std::string(" ") + kButtons[k] + " " + std::to_string(g.presses[k]) + ",";
        fprintf(stderr,
                "gamepads: port %d sent%s left stick x %d to %d y %d to %d, right stick x %d to %d y %d to %d, "
                "triggers left %d to %d right %d to %d\n",
                i + 1, s.c_str(), g.lo[SDL_GAMEPAD_AXIS_LEFTX], g.hi[SDL_GAMEPAD_AXIS_LEFTX], g.lo[SDL_GAMEPAD_AXIS_LEFTY],
                g.hi[SDL_GAMEPAD_AXIS_LEFTY], g.lo[SDL_GAMEPAD_AXIS_RIGHTX], g.hi[SDL_GAMEPAD_AXIS_RIGHTX],
                g.lo[SDL_GAMEPAD_AXIS_RIGHTY], g.hi[SDL_GAMEPAD_AXIS_RIGHTY], g.lo[SDL_GAMEPAD_AXIS_LEFT_TRIGGER],
                g.hi[SDL_GAMEPAD_AXIS_LEFT_TRIGGER], g.lo[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER], g.hi[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER]);
    }
    fprintf(stderr, "gamepads: port %d rumble on %d time%s, %d of %d call%s failed\n", i + 1, g.rumbles,
            g.rumbles == 1 ? "" : "s", g.failed, g.calls, g.calls == 1 ? "" : "s");
}

// A notice over the game and its [pads] line (overlay.h), as `name` `what` player `port` + 1.
void notice(const char* name, const char* what, int port) {
    overlay_pads_notice(name + std::string(what) + std::to_string(port + 1));
}

int g_keyboard_port = 0;    // the keyboard's port, which only gamepads_assign moves
std::string g_memory_path;  // where the saved ports are kept, empty for nowhere
SavedPorts g_saved;         // what the file said, and what has been put on a port since
bool g_saved_loaded = false;
bool g_may_save = true;  // false once the file turned out to exist and not be readable

// The saved ports, read when a gamepad or a move first needs them and not before, so that a
// launch with no gamepad reads nothing.
void load_saved() {
    if (g_memory_path.empty() || g_saved_loaded) return;
    g_saved = ports_load(g_memory_path, g_may_save);
    g_saved_loaded = true;
}

// What tells the gamepad from another across launches.
PortDevice device_of(SDL_Gamepad* g) {
    PortDevice d;
    char guid[33];
    SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(g)), guid, sizeof(guid));
    d.guid = guid;
    if (const char* serial = SDL_GetGamepadSerial(g)) d.serial = serial;
    d.vendor = SDL_GetGamepadVendor(g);
    d.product = SDL_GetGamepadProduct(g);
    return d;
}

// Each placed gamepad's player index set to its port, a new one's included. A joystick
// that claims an index another holds has SDL move the holder to the lowest free index,
// which need not be the holder's port. So a device that connects holding a placed
// gamepad's index moves it, whatever becomes of the newcomer here.
void set_indexes() {
    for (int i = 0; i < 4; i++)
        if (g_ports[i].pad && SDL_GetGamepadPlayerIndex(g_ports[i].pad) != i) SDL_SetGamepadPlayerIndex(g_ports[i].pad, i);
}

// The gamepads with these instance IDs, onto ports together, so that every saved port
// gets its gamepad before another takes it.
void add(const std::vector<SDL_JoystickID>& ids) {
    struct Arrival {
        SDL_Gamepad* pad;
        SDL_JoystickID which;
    };
    std::vector<Arrival> arrivals;
    bool disturbed = false;  // a joystick SDL gave an index, whether or not it gets a port
    for (const SDL_JoystickID which : ids) {
        // SDL also announces the gamepads gamepads_init took.
        if (port_of(which) >= 0 || std::find(g_left_out.begin(), g_left_out.end(), which) != g_left_out.end()) continue;
        // A device controls.txt binds is the file's alone.
        if (input_map_binds(which)) {
            disturbed = true;
            continue;
        }
        SDL_Gamepad* g = SDL_OpenGamepad(which);
        if (!g) {
            const char* n = SDL_GetJoystickNameForID(which);
            fprintf(stderr, "gamepads: cannot open \"%s\": %s\n", n ? n : "?", SDL_GetError());
            disturbed = true;
            continue;
        }
        arrivals.push_back({g, which});
    }
    if (!arrivals.empty()) {
        load_saved();
        std::vector<PresentDevice> present;
        for (int i = 0; i < 4; i++)
            if (g_ports[i].pad) present.push_back({device_of(g_ports[i].pad), SDL_GetGamepadPlayerIndex(g_ports[i].pad), i});
        const size_t first = present.size();
        for (const Arrival& a : arrivals) present.push_back({device_of(a.pad), SDL_GetGamepadPlayerIndex(a.pad), -1});
        std::array<bool, 4> pinned;
        for (int i = 0; i < 4; i++) pinned[i] = input_map_pins(i);
        const std::vector<int> got = match_ports(g_saved, present, pinned);

        bool moved = false;  // a gamepad landed on a port the saved ports didn't have it on
        for (size_t k = 0; k < arrivals.size(); k++) {
            const Arrival& a = arrivals[k];
            const int i = got[first + k];
            if (i < 0) {
                fprintf(stderr, "gamepads: \"%s\" left out: no port is free\n", pad_name(a.pad));
                overlay_pads_notice(pad_name(a.pad) + std::string(" has no player: all four are taken. "
                                                                  "The menu's Controllers page can give it one."));
                SDL_SetGamepadPlayerIndex(a.pad, -1);
                SDL_CloseGamepad(a.pad);
                g_left_out.push_back(a.which);
                continue;
            }
            g_ports[i] = {a.pad, a.which, false, faces(a.pad)};
            if (!g_memory_path.empty() && ports_assign(g_saved, i, present[first + k].id)) moved = true;
            fprintf(stderr, "gamepads: \"%s\" on port %d\n", pad_name(a.pad), i + 1);
            say_landed(i);
            notice(pad_name(a.pad), " connected as player ", i);
        }
        if (moved && g_may_save) ports_save(g_memory_path, g_saved);
    }
    if (!arrivals.empty() || disturbed) set_indexes();
}

uint8_t axis_to_u8(int v, bool invert) {
    float f = v / 32767.0f;
    if (invert) f = -f;
    int r = 128 + (int)(f * 100.0f);
    return (uint8_t)std::clamp(r, 0, 255);
}

// Whether the gamepad `g` is asked for rumble: one SDL says has none isn't, unless SDL types
// it as GameCube, as it does for several adapters and pads across its drivers. Of those only
// Nintendo's adapter keeps a stale flag, saying no rumble for a pad that landed before the
// adapter's second cable was in. Asking one of the others costs a refused call at each on
// and one line (gamepads_rumble).
bool may_rumble(SDL_Gamepad* g) {
    return SDL_GetGamepadType(g) == SDL_GAMEPAD_TYPE_GAMECUBE ||
           SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
}

// ---- The list and the moves (gamepads.h)

bool changeable(int port) { return port >= 0 && port < 4 && !input_map_pins(port); }

const char* type_name(SDL_GamepadType t) {
    switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One";
    case SDL_GAMEPAD_TYPE_PS3: return "PS3";
    case SDL_GAMEPAD_TYPE_PS4: return "PS4";
    case SDL_GAMEPAD_TYPE_PS5: return "PS5";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Joy-Con (L)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Joy-Con (R)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con pair";
    case SDL_GAMEPAD_TYPE_GAMECUBE: return "GameCube";
    default: return "gamepad";
    }
}

// Whether a button of `g` is down, or a stick or trigger is pushed past a quarter of its travel.
bool pushed(SDL_Gamepad* g) {
    for (int k = 0; k < SDL_GAMEPAD_BUTTON_COUNT; k++)
        if (SDL_GetGamepadButton(g, (SDL_GamepadButton)k)) return true;
    for (int k = 0; k < SDL_GAMEPAD_AXIS_COUNT; k++) {
        const int v = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)k);
        if ((k >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? v : abs(v)) > 8192) return true;
    }
    return false;
}

// The gamepad `which` as gamepads_list has it, on `port` or on none (-1). Its light reads the
// overlay's own handle when this file has let it go.
PadDevice entry(SDL_JoystickID which, int port) {
    PadDevice d{which, port};
    const char* n = SDL_GetJoystickNameForID(which);
    d.name = n ? n : "?";
    const SDL_GamepadType type = SDL_GetGamepadTypeForID(which), real = SDL_GetRealGamepadTypeForID(which);
    d.type = type_name(type);
    if (real != type) d.type = type_name(real) + std::string(", mapped as ") + d.type;
    if (SDL_Gamepad* g = SDL_GetGamepadFromID(which)) d.active = pushed(g);
    return d;
}

// The gamepad on port `i` taken off it, with no port until it's given one or plugged in again.
void leave(int i) {
    Port& g = g_ports[i];
    fprintf(stderr, "gamepads: \"%s\" taken off port %d\n", pad_name(g.pad), i + 1);
    say_sent(i);
    notice(pad_name(g.pad), " is no longer player ", i);
    if (g.rumbling) gamepads_rumble(i, false);
    SDL_SetGamepadPlayerIndex(g.pad, -1);
    SDL_CloseGamepad(g.pad);
    g_left_out.push_back(g.which);
    g = Port();
}

// The gamepad on each port, by its instance ID, 0 for none: what a move is checked against.
std::array<SDL_JoystickID, 4> occupants() {
    std::array<SDL_JoystickID, 4> on;
    for (int i = 0; i < 4; i++) on[i] = g_ports[i].which;
    return on;
}

// After a move: each placed gamepad's light shows its port again, and each port whose gamepad
// isn't the one it had `before` is saved as what's on it now. A port the move left as it was
// keeps its line, which may name a gamepad that's pulled out.
void settle(const std::array<SDL_JoystickID, 4>& before) {
    set_indexes();
    if (g_memory_path.empty()) return;
    load_saved();
    bool changed = false;
    for (int i = 0; i < 4; i++) {
        if (g_ports[i].which == before[i]) continue;
        std::optional<PortDevice> now;
        if (g_ports[i].pad) now = device_of(g_ports[i].pad);
        changed = changed || g_saved[i] != now;
        g_saved[i] = now;
    }
    if (changed && g_may_save) ports_save(g_memory_path, g_saved);
}

}  // namespace

void gamepads_load_mappings() {
    const char* base = SDL_GetBasePath();
    if (!base) return;
    const std::string file = std::string(base) + "gamecontrollerdb.txt";
    if (!plat_readable(file.c_str())) return;
    // SDL takes only the lines for this platform (their `platform:` field), and counts only
    // the mappings it did not have: one that replaces its own is used but not counted.
    const int n = SDL_AddGamepadMappingsFromFile(file.c_str());
    if (n < 0) fprintf(stderr, "gamepads: cannot read mappings from %s: %s\n", file.c_str(), SDL_GetError());
    else fprintf(stderr, "gamepads: %d new mapping%s from %s\n", n, n == 1 ? "" : "s", file.c_str());
}

void gamepads_remember(const char* path) {
    g_memory_path = path ? path : "";
    g_saved = SavedPorts{};
    g_saved_loaded = false;
    g_may_save = true;
}

void gamepads_init() {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    add(std::vector<SDL_JoystickID>(ids, ids + (ids ? n : 0)));
    SDL_free(ids);
}

void gamepads_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
        add({e.gdevice.which});
    } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
        g_left_out.erase(std::remove(g_left_out.begin(), g_left_out.end(), e.gdevice.which), g_left_out.end());
        const int i = port_of(e.gdevice.which);
        if (i < 0) return;
        fprintf(stderr, "gamepads: \"%s\" pulled out of port %d\n", pad_name(g_ports[i].pad), i + 1);
        say_sent(i);
        notice(pad_name(g_ports[i].pad), " disconnected from player ", i);
        SDL_CloseGamepad(g_ports[i].pad);
        g_ports[i] = Port();
    }
}

void gamepads_merge(int port, PadState& p) {
    Port& g = g_ports[port];
    SDL_Gamepad* c = g.pad;
    if (!c) return;
    // Every button, the frontend's or not, and every axis before any dead zone.
    for (int k = 0; k < SDL_GAMEPAD_BUTTON_COUNT; k++) {
        const bool down = SDL_GetGamepadButton(c, (SDL_GamepadButton)k);
        if (down && !g.down[k]) g.presses[k]++;
        g.down[k] = down;
    }
    for (int k = 0; k < SDL_GAMEPAD_AXIS_COUNT; k++) {
        const int v = SDL_GetGamepadAxis(c, (SDL_GamepadAxis)k);
        g.lo[k] = g.read ? std::min(g.lo[k], v) : v;
        g.hi[k] = g.read ? std::max(g.hi[k], v) : v;
    }
    g.read = true;
    p.connected = true;
    auto b = [&](SDL_GamepadButton btn) { return SDL_GetGamepadButton(c, btn); };
    if (b(g.face[0])) p.buttons |= PAD_A;
    if (b(g.face[1])) p.buttons |= PAD_B;
    if (b(g.face[2])) p.buttons |= PAD_X;
    if (b(g.face[3])) p.buttons |= PAD_Y;
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
    if (lt > 1000) { p.trig_l = std::max(p.trig_l, (uint8_t)(lt * 255 / 32767)); if (lt > kGamepadTriggerClick) p.buttons |= PAD_L; }
    if (rt > 1000) { p.trig_r = std::max(p.trig_r, (uint8_t)(rt * 255 / 32767)); if (rt > kGamepadTriggerClick) p.buttons |= PAD_R; }
}

void gamepads_rumble(int port, bool motor) {
    Port& g = g_ports[port];
    if (g.pad && motor && !g.rumbling) g.rumbles++;
    if (g.pad && (motor || g.rumbling) && may_rumble(g.pad)) {
        g.calls++;
        if (!SDL_RumbleGamepad(g.pad, motor ? 0x6000 : 0, motor ? 0xA000 : 0, motor ? 200 : 0)) {
            g.failed++;
            // Each reason once while the pad is on the port, not at every call or every on.
            const std::string why = SDL_GetError();
            if (std::find(g.reasons_said.begin(), g.reasons_said.end(), why) == g.reasons_said.end()) {
                fprintf(stderr, "gamepads: port %d rumble failed: %s\n", port + 1, why.c_str());
                g.reasons_said.push_back(why);
            }
        }
    }
    g.rumbling = motor;
}

void gamepads_shutdown() {
    for (int i = 0; i < 4; i++) {
        if (g_ports[i].pad) {
            if (g_ports[i].rumbling) gamepads_rumble(i, false);
            say_sent(i);
            SDL_CloseGamepad(g_ports[i].pad);
        }
        g_ports[i] = Port();
    }
    g_left_out.clear();
    g_keyboard_port = 0;
    // The next launch in this process reads the file again.
    g_saved = SavedPorts{};
    g_saved_loaded = false;
    g_may_save = true;
}

std::array<int, 4> gamepads_faces(SDL_Gamepad* g) {
    const std::array<SDL_GamepadButton, 4> f = faces(g);
    return {f[0], f[1], f[2], f[3]};
}

PadList gamepads_list() {
    PadList list;
    for (int i = 0; i < 4; i++) list.pinned[i] = input_map_pins(i);
    int keys = 0;
    const bool* k = SDL_GetKeyboardState(&keys);
    list.devices.push_back({kKeyboard, g_keyboard_port, "Keyboard", "", std::any_of(k, k + keys, [](bool d) { return d; })});
    for (int i = 0; i < 4; i++)
        if (g_ports[i].pad) list.devices.push_back(entry(g_ports[i].which, i));
    for (const SDL_JoystickID which : g_left_out) list.devices.push_back(entry(which, -1));
    return list;
}

bool gamepads_swap(int a, int b) {
    if (!changeable(a) || !changeable(b)) return false;
    const std::array<SDL_JoystickID, 4> before = occupants();
    std::swap(g_ports[a], g_ports[b]);
    for (const int i : {a, b})
        if (g_ports[i].pad && a != b) notice(pad_name(g_ports[i].pad), " is now player ", i);
    settle(before);
    return true;
}

bool gamepads_unassign(int port) {
    if (!changeable(port) || !g_ports[port].pad) return false;
    const std::array<SDL_JoystickID, 4> before = occupants();
    leave(port);
    settle(before);
    return true;
}

bool gamepads_assign(uint32_t device, int port) {
    if (!changeable(port)) return false;
    if (device == kKeyboard) {
        if (g_keyboard_port != port) overlay_pads_notice("Keyboard is now player " + std::to_string(port + 1));
        g_keyboard_port = port;
        return true;
    }
    if (port_of(device) >= 0) return gamepads_swap(port_of(device), port);
    const auto out = std::find(g_left_out.begin(), g_left_out.end(), device);
    SDL_Gamepad* g = out != g_left_out.end() ? SDL_OpenGamepad(device) : nullptr;
    if (!g) return false;
    const std::array<SDL_JoystickID, 4> before = occupants();
    g_left_out.erase(out);
    if (g_ports[port].pad) leave(port);
    g_ports[port] = {g, device, false, faces(g)};
    fprintf(stderr, "gamepads: \"%s\" on port %d\n", pad_name(g), port + 1);
    say_landed(port);
    notice(pad_name(g), " is now player ", port);
    settle(before);
    return true;
}

int gamepads_keyboard_port() { return g_keyboard_port; }
