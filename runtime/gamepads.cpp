// See gamepads.h.
#include "gamepads.h"
#include "input_map.h"
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

std::string g_memory_path;  // where the saved ports are kept, empty for nowhere
SavedPorts g_saved;         // what the file said, and what has been put on a port since
bool g_saved_loaded = false;
bool g_may_save = true;  // false once the file turned out to exist and not be readable

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
        // The file is read here and not before, so that a launch with no gamepad reads nothing.
        if (!g_memory_path.empty() && !g_saved_loaded) {
            g_saved = ports_load(g_memory_path, g_may_save);
            g_saved_loaded = true;
        }
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
                SDL_SetGamepadPlayerIndex(a.pad, -1);
                SDL_CloseGamepad(a.pad);
                g_left_out.push_back(a.which);
                continue;
            }
            g_ports[i] = {a.pad, a.which, false};
            if (!g_memory_path.empty() && ports_assign(g_saved, i, present[first + k].id)) moved = true;
            fprintf(stderr, "gamepads: \"%s\" on port %d\n", pad_name(a.pad), i + 1);
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
        SDL_CloseGamepad(g_ports[i].pad);
        g_ports[i] = Port();
    }
}

void gamepads_merge(int port, PadState& p) {
    SDL_Gamepad* c = g_ports[port].pad;
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
    g_left_out.clear();
    // The next launch in this process reads the file again.
    g_saved = SavedPorts{};
    g_saved_loaded = false;
    g_may_save = true;
}
