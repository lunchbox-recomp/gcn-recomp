// The overlay's set-up, opening and closing, and the input it holds back; see overlay.h. The
// Display page is overlay_display.cpp, the Controllers page overlay_controllers.cpp and the
// notices notices.cpp.
#include "overlay/overlay.h"
#include "overlay/appear.h"
#include "frontend.h"
#include "gamepads.h"
#include "input_log.h"
#include "input_map.h"
#include "gx/render_gl.h"
#include "pause.h"
#include "runtime.h"  // host_ns
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h"  // ImGui::FocusItem

// review.cpp's. GCN_OVERLAY_AT=<frame>:<host frames>, both whole numbers, the second over 0,
// into `frame` and `left`, which a value it refuses leaves alone. GCN_OVERLAY_PAGE=<n>, a page
// from 0 (Display) to `last`, or -1 for a value it refuses. Then the watch on the menu's frame
// as it settles, and its dump.
bool review_parse_at(const char* e, long long& frame, long& left);
int review_parse_page(const char* e, int last);
void review_watch_reset();
bool review_settled(const ImDrawData* d);
void review_dump(SDL_Window* window, ImDrawData* d, int page, bool plain);
// overlay_display.cpp's.
void display_init(const OverlayPicture& picture);
void display_page(SDL_Window* window);
// overlay_controllers.cpp's and notices.cpp's.
void controllers_page();
bool controllers_event(const SDL_Event& e);
void controllers_stop();
// overlay_bindings.cpp's.
void bindings_page();
bool bindings_event(const SDL_Event& e);
void bindings_poll();
void bindings_stop();
void notices_init(SDL_Window* window, bool gl);
void notices_frame();
// Below, for the Controllers page too: the buttons each held gamepad has down stay out of the
// menu until each is let go.
void overlay_latch_pads();
// Below, for the notices too: a Dear ImGui context over `window`, made current, with the GL
// backend where `gl` says the window has a GL context and none otherwise, so it's laid out and
// never drawn. It writes no file, opens no link, offers no item picker and reads no gamepad
// itself. The menu and the notices each have one, so a frame of notices with the menu closed
// leaves the menu's focus as it was. Its style is the theme's at the window's height.
void overlay_imgui_context(SDL_Window* window, bool gl);
#ifdef GCN_OVERLAY_TEST
// Below, for the notices too: the current context's backends, GL's where `gl` says it has one,
// shut down and the context destroyed. And notices.cpp's, which does that to theirs.
void overlay_imgui_context_end(bool gl);
void notices_test_end();
#endif
// Below, for the notices too: the current context's style built again from the theme if the
// window's height has changed since it was last built, before a frame.
void overlay_style_fit(SDL_Window* window);
// theme.cpp's: the theme, and the current context's style built from it at a scale, the scale
// for a window's height in Dear ImGui's units, and the style built again where that has changed.
const OverlayTheme& theme_now();
void theme_set(const OverlayTheme& theme);
void theme_style(const OverlayTheme& theme, float scale);
float theme_scale(int height);
void theme_fit(int height);

namespace {

bool g_ready, g_open;
bool g_view_left;  // it closed from a view in place of the tabs, drawn as it fades (set_open)
SDL_Window* g_window;  // null in tests, which draw nothing
bool g_gl;             // the window has a GL context to draw the menu with, which tests' don't
std::string g_title;
struct AddedPage {
    std::string title;
    void (*draw)(void* user);
    void* user;
};
std::vector<AddedPage> g_pages;
OverlayAsk g_ask;         // what the menu is asking before it does it
OverlayAsk g_asked;       // the button that asked, which takes the focus back at the next draw
bool g_focus_cancel;      // Cancel takes the focus at the next draw, as the menu starts asking
uint16_t g_held_over[4];  // the buttons held as the overlay closed, kept from the game until let go
// The triggers whose travel is kept from the game since the overlay closed, until each is back
// at rest: kTravelL and kTravelR.
uint8_t g_travel_held[4];
constexpr uint8_t kTravelL = 1, kTravelR = 2;
PadState g_before[4];     // each port as the game last had it with the overlay closed
bool g_focus;             // the menu takes the keyboard's and the gamepads' focus at its next draw
bool g_dumped;            // since it opened, under --dump-output
long long g_at_frame = -1;  // GCN_OVERLAY_AT's, -1 for none or once it's opened the overlay
long g_at_left;             // draws over the held frame left before GCN_OVERLAY_AT's closes it
int g_page = -1;            // GCN_OVERLAY_PAGE's, 0 for Display, -1 for none
int g_select = -1;          // the page the next draw selects, as the menu opens on g_page
bool g_plain;               // GCN_OVERLAY_PLAIN: dumps over plain grey, not the held frame
uint64_t g_opened_ns;       // host_ns as it last opened
bool g_said_shown;          // its `shown after` line, since it opened
long g_menu_frames;         // frames that have laid out the menu
ImDrawList* g_panel_list;   // the menu window's draw list at its last frame
int g_panel_vtx;            // and how many of its vertices were the panel's own, before its contents

// A gamepad held for the menu (hold_pads), and its buttons held as the menu opened, kept from
// the menu until each is let go, a bit for each SDL_GamepadButton.
struct HeldPad {
    SDL_JoystickID id;
    SDL_Gamepad* pad;
    uint32_t latched;
};
std::vector<HeldPad> g_pads;

uint32_t buttons_down(SDL_Gamepad* p) {
    uint32_t down = 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        if (SDL_GetGamepadButton(p, (SDL_GamepadButton)b)) down |= 1u << b;
    return down;
}

// What showed in place of the tabs ends with nothing done: Restart? or Quit?, the press-A order,
// and the Buttons view with any press-to-bind.
void end_view() {
    g_view_left = false;
    g_ask = g_asked = OverlayAsk::none;
    controllers_stop();
    bindings_stop();
}

void set_open(bool on) {
    if (on == g_open) return;
    g_open = on;
    pause_request(on);
    const uint64_t now = host_ns();
    if (on) appear_open(theme_now(), now);
    else appear_close(theme_now(), now);
    fprintf(stderr, "[overlay] %s\n", on ? "open" : "closed");
    // A view up as it closes is drawn as it was while the menu fades, with no input reaching it,
    // and ends once it's gone (menu_lays_out), or at once with no fade. Opened again before then,
    // the menu is back on its tabs.
    if (on) {
        if (g_view_left) end_view();
    } else if (g_window && appear_closing(now)) {
        g_view_left = true;
    } else {
        end_view();
    }
    if (!g_window) return;
    if (!on) {
        // As it fades out the menu is laid out with no input fed to it (overlay_event, menu_frame),
        // and with none held, so nothing moves its focus, which Dear ImGui keeps for the next
        // open. A button picked for the focus to go to (actions, asking, controllers_page, the
        // Buttons view's rows) isn't taken there either. Its NoInputs window flag would lose that
        // focus. A key queued ahead of this close in the same pump is dropped too, or the fade's
        // first frame, or with no fade the next open's, would press it. While it's closed no
        // event is fed to it (overlay_event): the SDL backend's NewFrame queues only the mouse's
        // position, where it can read it from the OS, and each fade frame uses up its own.
        ImGui::GetIO().ClearEventsQueue();
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        return;
    }
    // Keys and buttons let go while it was closed never reached the menu. One held as it opens
    // stays out of it until it's let go, though Dear ImGui keeps the focus from the last time,
    // so that it presses nothing: a key's repeats never reach the menu (overlay_event), and a
    // gamepad's buttons are latched here.
    ImGui::GetIO().ClearInputKeys();
    overlay_latch_pads();
    g_focus = true;
    g_dumped = false;
    g_select = g_page;
    g_opened_ns = now;
    g_said_shown = false;
    review_watch_reset();
}

// Every gamepad SDL has, held open, so that the menu reads each one and each one's Back opens
// it, on a port or not. A device controls.txt binds is the file's alone, as it is for the ports
// (gamepads.cpp), so it's left out. At the start and again as a gamepad comes or goes, when one
// held already keeps its latch, a gamepad gone is let go, and one that comes while the menu is
// open is latched as at the open.
void hold_pads() {
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
            held.push_back({ids[i], p, g_open ? buttons_down(p) : 0});
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
// longer latched.
void read_pads() {
    if (g_pads.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    for (HeldPad& h : g_pads) h.latched &= buttons_down(h.pad);
    const auto down = [](const HeldPad& h, int b) {
        return SDL_GetGamepadButton(h.pad, (SDL_GamepadButton)b) && !(h.latched >> b & 1);
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
        SDL_GamepadButton button;
    } kButtons[] = {
        {ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},
        {ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
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
// on: Z and both trigger clicks held with nothing else for kChordNs. The clicks are SDL's MISC3
// and MISC4 where it maps them, as for Nintendo's adapter, and the triggers' axes past their
// click (kGamepadTriggerClick) where it doesn't. It opens or closes the overlay once a hold, and
// any other button stops it until the hold is let go.
constexpr uint64_t kChordNs = 1000000000;
struct ChordHold {
    SDL_JoystickID id;
    uint64_t since;  // host_ns as the hold began
    bool spent;      // it's opened or closed the overlay, or another button stopped it
};
std::vector<ChordHold> g_holds;

bool clicked(SDL_Gamepad* p, SDL_GamepadButton click, SDL_GamepadAxis axis) {
    return SDL_GamepadHasButton(p, click) ? SDL_GetGamepadButton(p, click)
                                          : SDL_GetGamepadAxis(p, axis) > kGamepadTriggerClick;
}

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

// GCN_OVERLAY_AT's frame has been presented, so overlay_draw opens the menu.
bool at_frame_due() { return g_at_frame >= 0 && gx::present_count() >= g_at_frame; }

// A page's tab, selected by the draw after the menu opens where GCN_OVERLAY_PAGE gave it.
bool page_tab(const char* title, int index) {
    return ImGui::BeginTabItem(title, nullptr, g_select == index ? ImGuiTabItemFlags_SetSelected : 0);
}

// The menu asks before it restarts or quits, with Cancel focused, and goes back to the button
// that asked. FocusItem moves the focus without pressing what it lands on, which
// SetKeyboardFocusHere does.
void ask(OverlayAsk what) {
    g_ask = what;
    g_focus_cancel = true;
}

void back() {
    g_asked = g_ask;
    g_ask = OverlayAsk::none;
}

// Closes the overlay and asks SDL to quit, as the window's close button does, so the frontend's
// loop stops and the run ends the way it does for an Escape.
void quit() {
    set_open(false);
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

// Below the pages, so on every one. A replay has no Restart game, even once its log runs out:
// the log can't hold one, and a relaunch would replay the log from its start (frontend_restart).
void actions() {
    ImGui::Separator();
    if (!input_replay_loaded()) {
        if (ImGui::Button("Restart game")) ask(OverlayAsk::restart);
        if (g_open && g_asked == OverlayAsk::restart) ImGui::FocusItem();
        ImGui::SameLine();
    }
    if (ImGui::Button("Quit game")) ask(OverlayAsk::quit);
    if (g_open && g_asked == OverlayAsk::quit) ImGui::FocusItem();
    g_asked = OverlayAsk::none;
}

void asking() {
    const bool restart = g_ask == OverlayAsk::restart;
    ImGui::TextUnformatted(restart ? "Restart the game? Anything it hasn't saved is lost."
                                   : "Quit the game? Anything it hasn't saved is lost.");
    if (ImGui::Button("Cancel")) back();
    if (g_open && g_focus_cancel) ImGui::FocusItem();
    g_focus_cancel = false;
    ImGui::SameLine();
    if (ImGui::Button(restart ? "Restart" : "Quit")) {
        if (restart) frontend_restart();
        else quit();
    }
}

void draw_menu() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (g_focus) ImGui::SetNextWindowFocus();
    g_focus = false;
    // Begin draws the panel's edge, and the rule under its title that a frame border turns on, in
    // the Border colour, an item's edge. They take the theme's fainter panel edge instead.
    const uint32_t e = theme_now().panel_edge;
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(e >> 24, e >> 16 & 0xFF, e >> 8 & 0xFF, e & 0xFF));
    ImGui::Begin(g_title.c_str(), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    g_panel_list = ImGui::GetWindowDrawList();
    g_panel_vtx = g_panel_list->VtxBuffer.Size;  // the panel itself, drawn by Begin
    if (g_ask != OverlayAsk::none) {
        asking();
    } else if (overlay_controllers_asking() >= 0) {
        controllers_page();  // the press-A order alone, with nothing to focus
    } else if (overlay_bindings_shown()) {
        bindings_page();
    } else {
        if (ImGui::BeginTabBar("pages")) {
            if (page_tab("Display", 0)) {
                display_page(g_window);
                ImGui::EndTabItem();
            }
            if (page_tab("Controllers", 1)) {
                controllers_page();
                ImGui::EndTabItem();
            }
            for (size_t i = 0; i < g_pages.size(); i++) {
                if (page_tab(g_pages[i].title.c_str(), 2 + (int)i)) {
                    g_pages[i].draw(g_pages[i].user);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
            g_select = -1;
        }
        actions();
    }
    ImGui::End();
}

// The game dimmed behind the menu, in the background draw list, which the panel's motion leaves
// alone (appear_apply).
void backdrop(uint64_t now) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const uint32_t c = theme_now().backdrop;
    const int alpha = (int)((float)(c & 0xFF) * appear_backdrop(now) + 0.5f);
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  IM_COL32(c >> 24, c >> 16 & 0xFF, c >> 8 & 0xFF, alpha));
}

// Once the menu has opened, its panel drawn whole for the first time.
void say_shown(uint64_t now) {
    if (g_said_shown || !g_open || !appear_shown(now) || !ImGui::GetDrawData()->CmdLists.contains(g_panel_list)) return;
    g_said_shown = true;
    fprintf(stderr, "[overlay] shown after %llu ms\n", (unsigned long long)((now - g_opened_ns) / 1000000));
}

// The menu's frame: its input taken while it's open and its windows laid out, ready to draw,
// moved as it opens or closes.
void menu_frame() {
    const uint64_t now = host_ns();
    overlay_style_fit(g_window);
    ImGui_ImplSDL3_NewFrame();
    if (g_open) read_pads();
    ImGui::NewFrame();
    draw_menu();
    backdrop(now);
    ImGui::Render();
    const ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    appear_apply(ImGui::GetDrawData(), ImGui::GetBackgroundDrawList(), g_panel_list, g_panel_vtx, c.x, c.y,
                 appear_scale(now), appear_panel_alpha(now), appear_contents_alpha(now));
    g_menu_frames++;
    say_shown(now);
}

// The menu is open or still fading out over a window.
bool menu_showing() { return g_open || (g_window && appear_closing(host_ns())); }

// Whether a frame lays out the menu. Both frames ask it, so a view the menu closed from ends
// once its fade is over (set_open) on either.
bool menu_lays_out() {
    if (g_view_left && !menu_showing()) end_view();
    return menu_showing();
}

}  // namespace

void overlay_latch_pads() {
    for (HeldPad& h : g_pads) h.latched = buttons_down(h.pad);
}

void overlay_add_page(const char* title, void (*draw)(void* user), void* user) {
    if (g_ready) { fprintf(stderr, "[overlay] page registration is closed after init\n"); return; }
    if (!title || !*title) { fprintf(stderr, "[overlay] page title is empty\n"); return; }
    if (!strcmp(title, "Display") || !strcmp(title, "Controllers") ||
        std::any_of(g_pages.begin(), g_pages.end(), [&](const AddedPage& page) { return page.title == title; })) {
        fprintf(stderr, "[overlay] page title %s is already in use\n", title);
        return;
    }
    if (!draw) { fprintf(stderr, "[overlay] page %s has no callback\n", title); return; }
    g_pages.push_back({title, draw, user});
}

void overlay_set_theme(const OverlayTheme& theme) {
    if (g_ready) { fprintf(stderr, "[overlay] the theme is fixed after init, so this one is refused\n"); return; }
    theme_set(theme);
}

#ifdef GCN_OVERLAY_TEST
void overlay_test_reset_pages() {
    g_pages.clear();
    theme_set(overlay_theme_default());
    g_ready = false;
}

void overlay_test_end() {
    if (!g_window) return;
    notices_test_end();
    overlay_imgui_context_end(g_gl);
    g_window = nullptr;
}
#endif

void overlay_style_fit(SDL_Window* window) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    theme_fit(h);
}

void overlay_init(SDL_Window* window, const char* title, const OverlayPicture& picture) {
    g_ready = true;
    g_window = window;
    g_title = title;
    display_init(picture);
    appear_motion(picture.now.menu_motion && !gx::g_dump_output);
    if (const char* e = getenv_nonempty("GCN_OVERLAY_AT")) review_parse_at(e, g_at_frame, g_at_left);
    const char* page = getenv_nonempty("GCN_OVERLAY_PAGE");
    g_page = page ? review_parse_page(page, 1 + (int)g_pages.size()) : -1;
    g_plain = getenv_nonempty("GCN_OVERLAY_PLAIN") != nullptr;
    if (!window) return;
    g_gl = SDL_GL_GetCurrentContext() != nullptr;
    notices_init(window, g_gl);
    overlay_imgui_context(window, g_gl);  // the menu's, current from here on
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    hold_pads();
}

void overlay_imgui_context(SDL_Window* window, bool gl) {
    ImGui::SetCurrentContext(ImGui::CreateContext());
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written to the game's directory
    io.ConfigDebugHighlightIdConflictsShowItemPicker = false;
    io.LogFilename = nullptr;
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    theme_style(theme_now(), theme_scale(h));
    if (gl) {
        ImGui_ImplSDL3_InitForOpenGL(window, SDL_GL_GetCurrentContext());
        ImGui_ImplOpenGL3_Init();
    } else {
        // Nothing draws it, so the texture its font is baked into is never made.
        ImGui_ImplSDL3_InitForOther(window);
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    }
    // The SDL3 backend sets this to SDL_OpenURL and Dear ImGui's duplicate-ID tooltip
    // carries a link, so clear it after both backend inits.
    ImGui::GetPlatformIO().Platform_OpenInShellFn = nullptr;
    // The backend reads no gamepad, so read_pads can read them all by their labels.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
}

#ifdef GCN_OVERLAY_TEST
void overlay_imgui_context_end(bool gl) {
    if (gl) ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}
#endif

bool overlay_open() { return g_open; }

bool overlay_menu_due() { return menu_showing() || at_frame_due(); }

bool overlay_animating() { return g_window && appear_moving(host_ns()); }

long overlay_menu_frames() { return g_menu_frames; }

void overlay_close() { set_open(false); }

OverlayAsk overlay_asking() { return g_ask; }

bool overlay_event(const SDL_Event& e) {
    if (!g_ready) return false;
    if (g_open && (controllers_event(e) || bindings_event(e))) return true;
    const SDL_Scancode key = e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat ? e.key.scancode : SDL_SCANCODE_UNKNOWN;
    const bool pressed = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    const bool cancel = g_open && (key == SDL_SCANCODE_ESCAPE || (pressed && is_b(e.gbutton)));
    // While the menu asks, B or Escape goes back to it instead of closing it.
    if (cancel && g_ask != OverlayAsk::none) {
        back();
        return true;
    }
    if (key == SDL_SCANCODE_F1 || (pressed && opens(e.gbutton)) || cancel) {
        set_open(!g_open);
        return true;
    }
    if (g_window && (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED)) hold_pads();
    else if (g_window && g_open && !(e.type == SDL_EVENT_KEY_DOWN && e.key.repeat)) ImGui_ImplSDL3_ProcessEvent(&e);
    return g_open && is_input(e);
}

void overlay_poll() {
    if (!g_ready) return;
    if (g_open) bindings_poll();
    const uint64_t now = host_ns();
    bool toggle = false;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n; i++) {
        SDL_Gamepad* p = SDL_GetGamepadFromID(ids[i]);
        if (!p || input_map_binds(ids[i]) || SDL_GamepadHasButton(p, SDL_GAMEPAD_BUTTON_BACK) ||
            SDL_GamepadHasButton(p, SDL_GAMEPAD_BUTTON_GUIDE))
            continue;
        if (chord(ids[i], p, now)) toggle = true;
    }
    SDL_free(ids);
    if (toggle) set_open(!g_open);
}

void overlay_gate(int port, PadState& p) {
    if (port < 0 || port > 3) return;
    if (g_open) {
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

void overlay_menu_frame() {
    if (!g_window) return;
    if (menu_lays_out()) menu_frame();
    notices_frame();
}

void overlay_draw() {
    if (!g_window || !g_gl) return;
    // Opening ends a view left from the last close itself (set_open).
    if (at_frame_due()) {
        g_at_frame = -1;
        set_open(true);
    }
    if (menu_lays_out()) {
        ImGui_ImplOpenGL3_NewFrame();
        menu_frame();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    notices_frame();
    if (!g_open || !paused()) return;
    // The dump waits for a frame that looks as the ones before it did (review.cpp).
    const bool dumps = gx::g_dump_output && gx::g_dump_dir;
    if (!g_dumped && dumps && review_settled(ImGui::GetDrawData())) {
        g_dumped = true;
        review_dump(g_window, ImGui::GetDrawData(), g_page, g_plain);
    }
    if (g_at_left > 0 && --g_at_left == 0) {
        if (dumps && !g_dumped) fprintf(stderr, "[overlay] the menu closed before its frame settled, so there's no dump\n");
        set_open(false);
    }
}
