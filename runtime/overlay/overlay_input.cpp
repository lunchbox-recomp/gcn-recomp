// The overlay's input (overlay.h): what opens and closes it, the gamepads the menu reads, and
// what it holds back from the game.
#include "overlay/overlay.h"
#include "overlay/footer.h"
#include "overlay/state.h"
#include "gamepads.h"
#include "input_map.h"
#include "pause.h"
#include "runtime.h"  // host_ns
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <vector>
#include "imgui.h"
#include "imgui_impl_sdl3.h"

// overlay_controllers.cpp's and overlay_bindings.cpp's.
bool controllers_event(const SDL_Event& e);
bool bindings_event(const SDL_Event& e);
void bindings_poll();

namespace {

uint16_t g_held_over[4];  // the buttons held as the overlay closed, kept from the game until let go
// The triggers whose travel is kept from the game since the overlay closed, until each is back
// at rest: kTravelL and kTravelR.
uint8_t g_travel_held[4];
constexpr uint8_t kTravelL = 1, kTravelR = 2;
PadState g_before[4];     // each port as the game last had it with the overlay closed

// A gamepad held for the menu (overlay_hold_pads), and its buttons held as the menu opened, kept
// from the menu until each is let go, a bit for each SDL_GamepadButton and for each trigger
// (kTriggerL and kTriggerR). On one with only the GameCube's buttons (cube_only), whose Z and
// trigger clicks are the chord's, also those of them down at the menu's last frame, those that
// have been down with another or while an item was active, and those let go this frame alone,
// which turn a page (chord_turns).
struct HeldPad {
    SDL_JoystickID id;
    SDL_Gamepad* pad;
    uint32_t latched;
    bool cube;
    uint32_t chord_down, spoiled, turns;
};
std::vector<HeldPad> g_pads;
SDL_JoystickID g_last_pad;

// The triggers, as the menu takes them: buttons past SDL's own, each down while it's clicked in
// (clicked).
constexpr int kTriggerL = SDL_GAMEPAD_BUTTON_COUNT, kTriggerR = SDL_GAMEPAD_BUTTON_COUNT + 1;
static_assert(kTriggerR < 32, "a HeldPad's latch has a bit for each button and trigger");
// The chord's buttons as the menu takes them: Z, SDL's R1 on a GameCube pad, and the two clicks.
constexpr uint32_t kChordKeys = 1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER | 1u << kTriggerL | 1u << kTriggerR;

uint32_t buttons_down(SDL_Gamepad* p) {
    uint32_t down = 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        if (SDL_GetGamepadButton(p, (SDL_GamepadButton)b)) down |= 1u << b;
    return down;
}

// Whether `p` has only the GameCube's own buttons, as SDL maps one on Nintendo's adapter: no Back
// and no Guide, so the chord opens and closes the overlay on it (overlay_poll).
bool cube_only(SDL_Gamepad* p) {
    return !SDL_GamepadHasButton(p, SDL_GAMEPAD_BUTTON_BACK) && !SDL_GamepadHasButton(p, SDL_GAMEPAD_BUTTON_GUIDE);
}

// Whether a trigger of `p` is clicked in. On a gamepad with only the GameCube's buttons it's
// SDL's `click` button for it where SDL maps one, as for Nintendo's adapter. Elsewhere it's the
// trigger's `axis` past the click the gamepad's binding has (PadBinding::trigger_click), which
// the Buttons view moves and gamepads_merge clicks the game's L and R at, since another
// gamepad's MISC3 and MISC4 may be other buttons.
bool clicked(SDL_Gamepad* p, SDL_GamepadButton click, SDL_GamepadAxis axis) {
    if (cube_only(p) && SDL_GamepadHasButton(p, click)) return SDL_GetGamepadButton(p, click);
    PadBinding b;
    const int at = gamepads_binding(SDL_GetGamepadID(p), b) ? b.trigger_click : kGamepadTriggerClick;
    return SDL_GetGamepadAxis(p, axis) > at;
}

bool is_down(SDL_Gamepad* p, int b) {
    if (b == kTriggerL) return clicked(p, SDL_GAMEPAD_BUTTON_MISC3, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    if (b == kTriggerR) return clicked(p, SDL_GAMEPAD_BUTTON_MISC4, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    return SDL_GetGamepadButton(p, (SDL_GamepadButton)b);
}

// What the menu latches: the buttons and the triggers down.
uint32_t latch_of(SDL_Gamepad* p) {
    return buttons_down(p) | (uint32_t)is_down(p, kTriggerL) << kTriggerL | (uint32_t)is_down(p, kTriggerR) << kTriggerR;
}

// The chord's buttons on `h` that turn a page at this frame, into h.turns. Each turns one as
// it's let go, if it went down and up with no other of them down and no item active, so that
// the chord turns none on its way in or out. `active` is whether an item was at the last frame,
// and a latched button isn't down to the menu.
void chord_turns(HeldPad& h, bool active) {
    const uint32_t down = latch_of(h.pad) & ~h.latched & kChordKeys;
    if (active || (down & (down - 1))) h.spoiled |= down;
    h.turns = h.chord_down & ~down & ~h.spoiled;
    h.spoiled &= down;
    h.chord_down = down;
}

// Whether the press `b` is of its gamepad's B, the face labeled B (gamepads_faces).
bool is_b(const SDL_GamepadButtonEvent& b) {
    SDL_Gamepad* p = SDL_GetGamepadFromID(b.which);
    return p && b.button == gamepads_faces(p)[1];
}

// Nintendo's Switch 2 GameCube controller, by its USB IDs. SDL maps no Back on it, but it has
// ZL, a button the GameCube's pad lacks, and Home: SDL's left shoulder and Guide.
constexpr Uint16 kNintendo = 0x057e, kSwitch2GameCube = 0x2073;

// Whether the press `b` opens or closes the overlay: Back wherever SDL maps it, Guide (Home) on
// a gamepad SDL maps no Back on, as a Switch 2 GameCube controller or a single Switch 2 Joy-Con,
// and ZL on a Switch 2 GameCube controller.
bool opens(const SDL_GamepadButtonEvent& b) {
    if (b.button == SDL_GAMEPAD_BUTTON_BACK) return true;
    SDL_Gamepad* p = SDL_GetGamepadFromID(b.which);
    if (!p) return false;
    if (b.button == SDL_GAMEPAD_BUTTON_GUIDE) return !SDL_GamepadHasButton(p, SDL_GAMEPAD_BUTTON_BACK);
    return b.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER && SDL_GetGamepadVendor(p) == kNintendo &&
           SDL_GetGamepadProduct(p) == kSwitch2GameCube;
}

// The chord on a gamepad with only the GameCube's own buttons, which SDL maps no Back or Guide
// on: Z and both trigger clicks (clicked) held with nothing else for kChordNs. It opens or
// closes the overlay once a hold, and any other button stops it until the hold is let go.
constexpr uint64_t kChordNs = 1000000000;
struct ChordHold {
    SDL_JoystickID id;
    uint64_t since;  // host_ns as the hold began
    bool spent;      // it's opened or closed the overlay, or another button stopped it
};
std::vector<ChordHold> g_holds;

// Whether `p`'s chord has been held its second by `now`, the once a hold that it acts.
bool chord(SDL_JoystickID id, SDL_Gamepad* p, uint64_t now) {
    const auto at = std::find_if(g_holds.begin(), g_holds.end(), [&](const ChordHold& h) { return h.id == id; });
    const bool held = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) &&
                      clicked(p, SDL_GAMEPAD_BUTTON_MISC3, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) &&
                      clicked(p, SDL_GAMEPAD_BUTTON_MISC4, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    if (!held) {
        if (at != g_holds.end()) g_holds.erase(at);
        return false;
    }
    ChordHold& h = at != g_holds.end() ? *at : g_holds.emplace_back(ChordHold{id, now, false});
    constexpr uint32_t kChord = 1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER | 1u << SDL_GAMEPAD_BUTTON_MISC3 |
                                1u << SDL_GAMEPAD_BUTTON_MISC4;
    if (buttons_down(p) & ~kChord) h.spent = true;
    if (h.spent || now - h.since < kChordNs) return false;
    h.spent = true;
    return true;
}

bool is_input(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL: case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return true;
    default:
        return false;
    }
}

}  // namespace

void overlay_latch_pads() {
    for (HeldPad& h : g_pads) {
        h.latched = latch_of(h.pad);
        h.chord_down = h.spoiled = h.turns = 0;
    }
}

// Every gamepad SDL has, held open, so that the menu reads each one and each one's Back opens
// it, on a port or not. A device controls.txt binds is the file's alone, as it is for the ports
// (gamepads.cpp), so it's left out. At the start and again as a gamepad comes or goes, when one
// held already keeps its latch, a gamepad gone is let go, and one that comes while the menu is
// open is latched as at the open.
void overlay_hold_pads() {
    std::vector<HeldPad> held;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n; i++) {
        if (input_map_binds(ids[i])) continue;
        const auto was = std::find_if(g_pads.begin(), g_pads.end(), [&](const HeldPad& h) { return h.id == ids[i]; });
        if (was != g_pads.end()) {
            held.push_back(*was);
            was->pad = nullptr;  // still held, so not let go below
        } else if (SDL_Gamepad* p = SDL_OpenGamepad(ids[i])) {
            held.push_back({ids[i], p, overlay_open() ? latch_of(p) : 0, cube_only(p), 0, 0, 0});
        }
    }
    SDL_free(ids);
    for (const HeldPad& h : g_pads)
        if (h.pad) SDL_CloseGamepad(h.pad);
    g_pads = std::move(held);
}

// What Dear ImGui's navigation reads of the held gamepads, for the menu's frame, merged over
// them all as its SDL backend merges them but with each one's faces by their labels, as the
// game takes them (gamepads_faces). The backend takes faces by position, so it's given no
// gamepad (overlay_init). A latched button is up to the menu, and once it's let go it's no
// longer latched. The triggers in past their click are L2 and R2, which turn the menu's pages
// with L1 and R1 (tabs.h), since a GameCube pad has no L1 and its Z is SDL's R1. On a gamepad
// with only the GameCube's buttons, Z and the clicks are the chord's too, so each is down to
// the menu only for the frame it's let go alone (chord_turns), which turns a page then, or
// while an item is active, as a slider takes R1.
void overlay_read_pads() {
    if (g_pads.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const bool active = ImGui::IsAnyItemActive();
    for (HeldPad& h : g_pads) {
        h.latched &= latch_of(h.pad);
        if (h.cube) chord_turns(h, active);
    }
    const auto down = [active](const HeldPad& h, int b) {
        if (h.cube && !active && (kChordKeys >> b & 1)) return (h.turns >> b & 1) != 0;
        return is_down(h.pad, b) && !(h.latched >> b & 1);
    };
    static constexpr ImGuiKey kFaces[4] = {ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight,
                                           ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadFaceUp};
    bool faces[4] = {};
    for (const HeldPad& h : g_pads) {
        const std::array<int, 4> f = gamepads_faces(h.pad);
        for (int k = 0; k < 4; k++) faces[k] = faces[k] || down(h, f[k]);
    }
    for (int k = 0; k < 4; k++) io.AddKeyEvent(kFaces[k], faces[k]);
    static constexpr struct {
        ImGuiKey key;
        int button;
    } kButtons[] = {
        {ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},
        {ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {ImGuiKey_GamepadL2, kTriggerL},
        {ImGuiKey_GamepadR2, kTriggerR},
    };
    for (const auto& b : kButtons) {
        bool held = false;
        for (const HeldPad& h : g_pads) held = held || down(h, b.button);
        io.AddKeyEvent(b.key, held);
    }
    // How far past the backend's dead zone the left stick is pushed each way, 0 to 1.
    constexpr int kDeadZone = 8000;
    static constexpr struct {
        ImGuiKey key;
        SDL_GamepadAxis axis;
        int sign;
    } kStick[] = {
        {ImGuiKey_GamepadLStickLeft, SDL_GAMEPAD_AXIS_LEFTX, -1},
        {ImGuiKey_GamepadLStickRight, SDL_GAMEPAD_AXIS_LEFTX, 1},
        {ImGuiKey_GamepadLStickUp, SDL_GAMEPAD_AXIS_LEFTY, -1},
        {ImGuiKey_GamepadLStickDown, SDL_GAMEPAD_AXIS_LEFTY, 1},
    };
    for (const auto& s : kStick) {
        float v = 0.0f;
        for (const HeldPad& h : g_pads)
            v = std::max(v, std::clamp((s.sign * SDL_GetGamepadAxis(h.pad, s.axis) - kDeadZone) / float(32767 - kDeadZone),
                                       0.0f, 1.0f));
        io.AddKeyAnalogEvent(s.key, v > 0.1f, v);
    }
}

bool overlay_event(const SDL_Event& e) {
    if (!overlay_ready()) return false;
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) g_last_pad = 0;
    else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = e.gbutton.which;
    else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
             std::abs(e.gaxis.value) > 16000) g_last_pad = e.gaxis.which;
    if (overlay_open() && (controllers_event(e) || bindings_event(e))) return true;
    const SDL_Scancode key = e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat ? e.key.scancode : SDL_SCANCODE_UNKNOWN;
    const bool pressed = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    const bool cancel = overlay_open() && (key == SDL_SCANCODE_ESCAPE || (pressed && is_b(e.gbutton)));
    // While the menu asks, B or Escape goes back to it instead of closing it.
    if (cancel && overlay_asking() != OverlayAsk::none) {
        footer_back();
        return true;
    }
    if (key == SDL_SCANCODE_F1 || (pressed && opens(e.gbutton)) || cancel) {
        overlay_set_open(!overlay_open());
        return true;
    }
    if (overlay_window() && (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED)) overlay_hold_pads();
    else if (overlay_window() && overlay_open() && !(e.type == SDL_EVENT_KEY_DOWN && e.key.repeat))
        ImGui_ImplSDL3_ProcessEvent(&e);
    return overlay_open() && is_input(e);
}

SDL_Gamepad* overlay_last_pad() { return g_last_pad ? SDL_GetGamepadFromID(g_last_pad) : nullptr; }

void overlay_poll() {
    if (!overlay_ready()) return;
    if (overlay_open()) bindings_poll();
    const uint64_t now = host_ns();
    bool toggle = false;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n; i++) {
        SDL_Gamepad* p = SDL_GetGamepadFromID(ids[i]);
        if (!p || input_map_binds(ids[i]) || !cube_only(p)) continue;
        if (chord(ids[i], p, now)) toggle = true;
    }
    SDL_free(ids);
    if (toggle) overlay_set_open(!overlay_open());
}

void overlay_gate(int port, PadState& p) {
    if (port < 0 || port > 3) return;
    if (overlay_open()) {
        g_held_over[port] = 0xFFFF;
        g_travel_held[port] = kTravelL | kTravelR;
        const bool connected = p.connected;
        // Until the guest is held at its frame boundary it may poll again, and a port at rest
        // would be a press let go, which a game may act on.
        p = paused() ? PadState{} : g_before[port];
        p.connected = connected;
        return;
    }
    g_held_over[port] &= p.buttons;
    p.buttons &= ~g_held_over[port];
    // A trigger in as it closed, as the chord's are, keeps its travel out until it's back at rest
    // (gamepads_merge's dead zone), though its click lets go on the way.
    if (!p.trig_l) g_travel_held[port] &= ~kTravelL;
    if (!p.trig_r) g_travel_held[port] &= ~kTravelR;
    if (g_travel_held[port] & kTravelL) p.trig_l = 0;
    if (g_travel_held[port] & kTravelR) p.trig_r = 0;
    g_before[port] = p;
}
