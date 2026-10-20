// See gamepads.h.
#include "gamepads.h"
#include "input_map.h"
#include "platform.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
