// The overlay's Buttons view, opened from the Controllers page: each gamepad's binding
// (pad_bindings.h), changed by press-to-bind, with its dead zones, its trigger click, a rumble
// test and a way back to the map (overlay.h).
#include "overlay/overlay.h"
#include "gamepads.h"
#include "input_map.h"
#include "pad_glyph.h"
#include "runtime.h"  // host_ns
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include "imgui.h"
#include "imgui_internal.h"  // ImGui::FocusItem

void overlay_latch_pads();  // overlay.cpp's

namespace {

bool g_shown;                       // the view is up in place of the tabs
uint32_t g_device;                  // the gamepad it shows
std::optional<BindListen> g_listen;  // press-to-bind, while it waits
std::string g_said;                 // what the last change did
int g_focus = -1;                   // the row that takes the focus at the next draw, -1 for none

const char* const kRows[kPadControls] = {
    "A", "B", "X", "Y", "Z", "Start", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "L", "R",
    "Control stick left and right", "Control stick up and down", "C-stick left and right", "C-stick up and down"};

std::string name_of(uint32_t device) {
    const char* n = SDL_GetJoystickNameForID(device);
    return n ? n : "?";
}

// What `s` is called on the gamepad `g`, as pad_glyph names it, and whether a stick axis is
// turned round from the way the map has it.
std::string glyph(SDL_Gamepad* g, PadControl c, const PadSource& s) {
    const int label = !s.axis && s.index <= kPadNorth ? SDL_GetGamepadButtonLabel(g, (SDL_GamepadButton)s.index) : 0;
    const bool trigger =
        SDL_GamepadHasAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) || SDL_GamepadHasAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    const PadFamily f = pad_family(SDL_GetGamepadType(g), trigger, SDL_GamepadHasButton(g, SDL_GAMEPAD_BUTTON_MISC1));
    std::string name = pad_glyph(f, s, label);
    const bool y = c == PadControl::StickY || c == PadControl::CStickY;
    if (pad_control_is_stick(c) && s.invert != y) name += " (reversed)";
    return name;
}

// The press-to-bind ends: what it did is said, and a button held for it stays out of the view
// until it's let go, so that it presses nothing there.
void end_listen(const std::string& said) {
    g_focus = (int)g_listen->control;
    g_listen.reset();
    g_said = said;
    overlay_latch_pads();
}

std::string refused() { return name_of(g_device) + " can't be set up here any more."; }

// What a change that went in adds to what's said: the other players whose gamepads pads.txt
// can't tell from this one, which took it too, and whether it lasts past this session.
std::string kept(Bound r) {
    std::string said;
    int own = -1;
    for (const PadDevice& d : gamepads_list().devices)
        if (d.id == g_device) own = d.port;
    std::vector<int> others = gamepads_sharing(g_device);
    others.erase(std::remove(others.begin(), others.end(), own), others.end());
    for (size_t i = 0; i < others.size(); i++)
        said += (i == 0 ? (others.size() > 1 ? " Players " : " Player ") : i + 1 == others.size() ? " and " : ", ") +
                std::to_string(others[i] + 1);
    if (!others.empty()) said += others.size() > 1 ? "'s controllers have it too." : "'s controller has it too.";
    if (r == Bound::unsaved) said += " pads.txt isn't saved, so this lasts until the game quits.";
    return said;
}

void hear(SDL_Gamepad* g) {
    PadSource got;
    const PadControl c = g_listen->control;
    PadBinding b;
    if (!gamepads_binding(g_device, b)) {
        end_listen(refused());
        return;
    }
    const std::string row = kRows[(int)c];
    const std::string still = row + " is still " + glyph(g, c, b.source[(int)c]) + ".";
    switch (bind_hear(*g_listen, gamepads_read(g), host_ns(), got)) {
    case BindHeard::waiting: return;
    case BindHeard::cancelled: end_listen(still); return;
    case BindHeard::timed_out:
        end_listen("Nothing was pressed in " + std::to_string(kBindListenNs / 1000000000) + " seconds, so " + still);
        return;
    case BindHeard::bound: break;
    }
    const PadBinding was = b;
    pad_binding_set(b, c, got);
    const Bound r = gamepads_bind(g_device, b);
    if (r == Bound::refused) return end_listen(refused());
    if (r == Bound::unchanged) return end_listen(still);
    std::string said = row + " is now " + glyph(g, c, got) + ".";
    for (int d = 0; d < kPadControls; d++)
        if (d != (int)c && b.source[d] != was.source[d])
            said += std::string(" ") + kRows[d] + " is now " + glyph(g, (PadControl)d, b.source[d]) + ".";
    // One way of a stick axis and all of it don't swap, since neither control can take what the
    // other had (pad_binding_set), so that stick drives both.
    for (int d = 0; d < kPadControls; d++) {
        const PadSource& t = b.source[d];
        if (d == (int)c || !got.axis || !t.axis || t.index != got.index || (t.half == 0) == (got.half == 0)) continue;
        if (pad_control_is_stick((PadControl)d)) said += std::string(" ") + kRows[d] + " still moves with it.";
        else said += " " + glyph(g, (PadControl)d, t) + " still presses " + kRows[d] + " too.";
    }
    end_listen(said + kept(r));
}

// The gamepads the view can show, on a player or not, and the ones controls.txt binds, which it
// shows locked.
void choose(const PadList& list) {
    ImGui::SeparatorText("Controller");
    for (const PadDevice& d : list.devices) {
        if (d.id == kKeyboard) continue;
        const std::string label = d.port >= 0 ? "Player " + std::to_string(d.port + 1) + ": " + d.name : d.name + " (no player)";
        if (ImGui::RadioButton((label + "##" + std::to_string(d.id)).c_str(), d.id == g_device)) {
            g_device = d.id;
            g_said.clear();
        }
    }
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n; i++)
        if (input_map_binds(ids[i]))
            ImGui::TextDisabled("%s: set by controls.txt, so its buttons can't be changed here.", name_of(ids[i]).c_str());
    SDL_free(ids);
}

// A dead zone or the trigger click, as a whole percentage of the travel, saved once it's let go.
// Its value is beside the slider with its name, not in it, where the grab, in the accent, would
// be under it at 1.57:1 (theme.h).
void setting(const char* label, int& raw, int most, PadBinding& b) {
    int pct = (int)std::lround(raw * 100.0 / 32767.0);
    if (ImGui::SliderInt((std::string("##") + label).c_str(), &pct, 0, most, "", ImGuiSliderFlags_AlwaysClamp))
        raw = (int)std::lround(pct * 32767.0 / 100.0);
    const bool let_go = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    ImGui::Text("%s %d%%", label, pct);
    if (!let_go) return;
    const Bound r = gamepads_bind(g_device, b);
    if (r == Bound::refused) g_said = refused();
    else if (r != Bound::unchanged) g_said = std::string(label) + " is now " + std::to_string(pct) + "%." + kept(r);
}

void rows(SDL_Gamepad* g, PadBinding& b) {
    ImGui::SeparatorText("Buttons");
    ImGui::TextUnformatted("Pick a control, then press the button or push the stick for it.");
    const float column = ImGui::CalcTextSize(kRows[(int)PadControl::CStickX]).x + ImGui::GetStyle().ItemSpacing.x * 2;
    for (int c = 0; c < kPadControls; c++) {
        ImGui::PushID(c);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kRows[c]);
        ImGui::SameLine(column);
        if (ImGui::Button(glyph(g, (PadControl)c, b.source[c]).c_str())) overlay_bindings_listen((PadControl)c);
        if (g_focus == c && overlay_open()) ImGui::FocusItem();  // the fade moves no focus
        ImGui::PopID();
    }
    g_focus = -1;
    ImGui::SeparatorText("Sticks and triggers");
    static PadBinding s_edit;  // the settings while one is being dragged
    if (!ImGui::IsAnyItemActive()) s_edit = b;
    setting("Control stick dead zone", s_edit.stick_deadzone, 50, s_edit);
    setting("C-stick dead zone", s_edit.cstick_deadzone, 50, s_edit);
    setting("Trigger dead zone", s_edit.trigger_deadzone, 50, s_edit);
    setting("Trigger click", s_edit.trigger_click, 99, s_edit);
}

}  // namespace

bool overlay_bindings_open(uint32_t device) {
    PadBinding b;
    if (!gamepads_binding(device, b)) return false;
    g_shown = true;
    g_device = device;
    g_said.clear();
    return true;
}

bool overlay_bindings_shown() { return g_shown; }
bool overlay_bindings_listening() { return g_listen.has_value(); }
const std::string& overlay_bindings_said() { return g_said; }

bool overlay_bindings_listen(PadControl c) {
    SDL_Gamepad* g = g_shown ? SDL_GetGamepadFromID(g_device) : nullptr;
    if (!g) return false;
    g_listen = bind_listen(c, gamepads_read(g), host_ns());
    g_said.clear();
    return true;
}

// overlay.cpp's, as the menu closes: the view and any press-to-bind end, with nothing changed.
void bindings_stop() {
    g_listen.reset();
    g_shown = false;
    g_focus = -1;
}

// overlay.cpp's, once a host frame while the menu is open: press-to-bind reads its gamepad.
void bindings_poll() {
    if (!g_listen) return;
    SDL_Gamepad* g = SDL_GetGamepadFromID(g_device);
    if (!g) end_listen(name_of(g_device) + " was disconnected, so nothing changed.");
    else hear(g);
}

// While press-to-bind waits, every key and gamepad event but F1, which closes the menu, is its
// own: Escape, or Back or Guide on any gamepad, cancels (its own gamepad's are read by
// bindings_poll). With the view up, B or Escape goes back to the Controllers page. True for each
// event it takes.
bool bindings_event(const SDL_Event& e) {
    if (!g_shown || (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F1)) return false;
    const bool escape = e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_ESCAPE;
    const bool button = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && !input_map_binds(e.gbutton.which);
    if (g_listen) {
        SDL_Gamepad* g = SDL_GetGamepadFromID(g_device);
        const bool other_cancel = button && e.gbutton.which != g_device &&
                                  (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK || e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE);
        if ((escape || other_cancel) && g) {
            const PadControl c = g_listen->control;
            PadBinding b;
            gamepads_binding(g_device, b);
            end_listen(std::string(kRows[(int)c]) + " is still " + glyph(g, c, b.source[(int)c]) + ".");
        }
        switch (e.type) {
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            return true;
        default:
            return false;
        }
    }
    SDL_Gamepad* p = button ? SDL_GetGamepadFromID(e.gbutton.which) : nullptr;
    if (escape || (p && e.gbutton.button == gamepads_faces(p)[1])) {
        g_shown = false;
        return true;
    }
    return false;
}

// The view. While press-to-bind waits, it has nothing to focus, so no press for it presses a
// button too.
void bindings_page() {
    SDL_Gamepad* g = SDL_GetGamepadFromID(g_device);
    PadBinding b;
    if (g_listen && g) {
        const PadControl c = g_listen->control;
        const uint64_t now = host_ns();
        const int left = (int)((g_listen->until - std::min(now, g_listen->until) + 999999999) / 1000000000);
        ImGui::Text(pad_control_is_stick(c) ? "Push a stick on %s for %s." : "Press a button on %s for %s.",
                    name_of(g_device).c_str(), kRows[(int)c]);
        ImGui::Text("Back or Guide on any controller, or Escape, cancels. %d s left.", left);
        return;
    }
    choose(gamepads_list());
    if (!g || !gamepads_binding(g_device, b)) {
        ImGui::TextDisabled("Pick a controller above.");
    } else {
        rows(g, b);
        if (ImGui::Button("Test rumble")) {
            // Sent is all that's known: a pad SDL drives through HIDAPI may still not shake (gamepads.h).
            if (!gamepads_may_rumble(g))
                g_said = name_of(g_device) + " has no rumble.";
            else if (!SDL_RumbleGamepad(g, 0x6000, 0xA000, 1000))
                g_said = std::string("Rumble failed: ") + SDL_GetError();
            else
                g_said = "Rumble sent to " + name_of(g_device) + " for 1 second.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset to default")) {
            const Bound r = gamepads_reset_binding(g_device);
            const std::string defaults = "default buttons, dead zones and trigger click";
            if (r == Bound::refused) g_said = refused();
            else if (r == Bound::unchanged) g_said = name_of(g_device) + " already has its " + defaults + ".";
            else g_said = name_of(g_device) + " has its " + defaults + " again." + kept(r);
        }
    }
    if (!g_said.empty()) ImGui::TextUnformatted(g_said.c_str());
    ImGui::Separator();
    if (ImGui::Button("Done")) g_shown = false;
}
