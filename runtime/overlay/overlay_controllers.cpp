// The overlay's Controllers page: each player's controllers, the press-A order, swaps and
// removals (overlay.h).
#include "overlay/overlay.h"
#include "gamepads.h"
#include "input_map.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include "imgui.h"
#include "imgui_internal.h"  // ImGui::FocusItem

void overlay_latch_pads();  // overlay.cpp's

namespace {

int g_asking = -1;              // the port the order asks for next, -1 while it isn't asking
std::vector<uint32_t> g_placed;  // the devices the order has placed
bool g_focus_order;             // Set player order takes the focus at the next draw

// The first port from `from` on that the order can give, or -1.
int next_port(int from) {
    while (from < 4 && input_map_pins(from)) from++;
    return from < 4 ? from : -1;
}

// The order ends. A button held for it stays out of the page until it's let go, so the press
// that ended it doesn't press the button that comes back under it.
void stop() {
    if (g_asking < 0) return;
    g_asking = -1;
    g_placed.clear();
    g_focus_order = true;
    overlay_latch_pads();
}

void place(uint32_t device) {
    if (std::find(g_placed.begin(), g_placed.end(), device) != g_placed.end() || !gamepads_assign(device, g_asking))
        return;
    g_placed.push_back(device);
    const int next = next_port(g_asking + 1);
    const PadList list = gamepads_list();
    const bool all = std::all_of(list.devices.begin(), list.devices.end(), [](const PadDevice& d) {
        return d.id == kKeyboard || std::find(g_placed.begin(), g_placed.end(), d.id) != g_placed.end();
    });
    if (next < 0 || all) stop();
    else g_asking = next;
}

// A light, lit while the device is pressed or pushed.
void light(bool lit) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + h * 0.3f, at.y + h * 0.5f), h * 0.3f,
                                                lit ? IM_COL32(90, 220, 120, 255) : IM_COL32(90, 90, 90, 255));
    ImGui::Dummy(ImVec2(h * 0.6f, h));
    ImGui::SameLine();
}

// Players `p` and `p` + 1 trade their gamepads, and the keyboard goes along, as the page lists it
// with them. gamepads_swap itself leaves it, so the order's own swaps don't move it.
void swap_with_next(int p) {
    gamepads_swap(p, p + 1);
    const int keys = gamepads_keyboard_port();
    if (keys == p || keys == p + 1) gamepads_assign(kKeyboard, keys == p ? p + 1 : p);
}

// Each player and what's on it, then the controllers on none. With `buttons`, each player
// that isn't pinned has Swap with the next one, off while neither has a gamepad, and Remove,
// off while it has none.
void players(const PadList& list, bool buttons) {
    std::array<bool, 4> listed{}, gamepad{};
    for (const PadDevice& d : list.devices) {
        if (d.port < 0) continue;
        listed[d.port] = true;
        gamepad[d.port] = gamepad[d.port] || d.id != kKeyboard;
    }
    for (int p = 0; p < 4; p++) {
        ImGui::PushID(p);
        ImGui::SeparatorText(("Player " + std::to_string(p + 1)).c_str());
        for (const PadDevice& d : list.devices) {
            if (d.port != p) continue;
            light(d.active);
            ImGui::Text(d.type.empty() ? "%s" : "%s (%s)", d.name.c_str(), d.type.c_str());
        }
        if (list.pinned[p]) ImGui::TextDisabled("Set by controls.txt, so it can't be changed here.");
        else if (!listed[p]) ImGui::TextDisabled("No controller");
        if (buttons && !list.pinned[p]) {
            if (p < 3 && !list.pinned[p + 1]) {
                ImGui::BeginDisabled(!gamepad[p] && !gamepad[p + 1]);
                if (ImGui::Button(("Swap with player " + std::to_string(p + 2)).c_str())) swap_with_next(p);
                ImGui::EndDisabled();
                ImGui::SameLine();
            }
            ImGui::BeginDisabled(!gamepad[p]);
            if (ImGui::Button("Remove")) gamepads_unassign(p);
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    if (std::none_of(list.devices.begin(), list.devices.end(), [](const PadDevice& d) { return d.port < 0; })) return;
    ImGui::SeparatorText("No player");
    for (const PadDevice& d : list.devices) {
        if (d.port >= 0) continue;
        light(d.active);
        ImGui::Text("%s (%s)", d.name.c_str(), d.type.c_str());
    }
}

}  // namespace

void overlay_controllers_order() {
    g_placed.clear();
    g_asking = next_port(0);
}

int overlay_controllers_asking() { return g_asking; }

// overlay.cpp's, as the menu closes.
void controllers_stop() { stop(); }

// While the order asks: a gamepad's A (the face labeled A, gamepads_faces) or the keyboard's X,
// the GameCube's A, places that device. Back, B or Escape ends it. True for each of those,
// which the menu then doesn't take.
bool controllers_event(const SDL_Event& e) {
    if (g_asking < 0) return false;
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        if (e.key.scancode == SDL_SCANCODE_X) place(kKeyboard);
        else if (e.key.scancode == SDL_SCANCODE_ESCAPE) stop();
        else return false;
        return true;
    }
    SDL_Gamepad* p = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && !input_map_binds(e.gbutton.which)
                         ? SDL_GetGamepadFromID(e.gbutton.which)
                         : nullptr;
    if (!p) return false;
    const std::array<int, 4> faces = gamepads_faces(p);
    if (e.gbutton.button == faces[0]) place(e.gbutton.which);
    else if (e.gbutton.button == faces[1] || e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) stop();
    else return false;
    return true;
}

// The page. While the order asks, it has nothing to focus, so no A pressed for it presses a
// button too.
void controllers_page() {
    const PadList list = gamepads_list();
    if (g_asking >= 0) {
        ImGui::Text("Press A on player %d's controller. On the keyboard, A is X.", g_asking + 1);
        ImGui::TextUnformatted("Back, B or Escape stops here, and the players set so far keep their controllers.");
        players(list, false);
        return;
    }
    if (ImGui::Button("Set player order")) overlay_controllers_order();
    if (g_focus_order) ImGui::FocusItem();
    g_focus_order = false;
    players(list, true);
}
