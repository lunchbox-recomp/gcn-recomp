// The overlay's set-up, opening and closing, and the input it holds back; see overlay.h. The
// Display page is overlay_display.cpp.
#include "overlay/overlay.h"
#include "gamepads.h"
#include "input_log.h"
#include "input_map.h"
#include "gx/gl.h"
#include "gx/render_gl.h"
#include "pause.h"
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

bool write_png(const char* path, const uint8_t* rgba, int w, int h);
// overlay_display.cpp's.
void display_init(const OverlayPicture& picture);
void display_page(SDL_Window* window);

namespace {

bool g_ready, g_open;
SDL_Window* g_window;  // null in tests, which draw nothing
bool g_gl;             // the window has a GL context to draw the menu with, which tests' don't
std::string g_title;
OverlayAsk g_ask;         // what the menu is asking before it does it
OverlayAsk g_asked;       // the button that asked, which takes the focus back at the next draw
bool g_focus_cancel;      // Cancel takes the focus at the next draw, as the menu starts asking
uint16_t g_held_over[4];  // the buttons held as the overlay closed, kept from the game until let go
PadState g_before[4];     // each port as the game last had it with the overlay closed
bool g_focus;             // the menu takes the keyboard's and the gamepads' focus at its next draw
bool g_dumped;            // since it opened, under --dump-output
long long g_at_frame = -1;  // GCN_OVERLAY_AT's, -1 for none or once it's opened the overlay
long g_at_left;             // draws over the held frame left before GCN_OVERLAY_AT's closes it

// A gamepad held for the menu (hold_pads), and its buttons held as the menu opened, kept from
// the menu until each is let go, a bit for each SDL_GamepadButton.
struct HeldPad {
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

void set_open(bool on) {
    if (on == g_open) return;
    g_open = on;
    g_ask = g_asked = OverlayAsk::none;
    pause_request(on);
    fprintf(stderr, "[overlay] %s\n", on ? "open" : "closed");
    if (!on || !g_window) return;
    // Keys and buttons let go while it was closed never reached the menu. One held as it opens
    // stays out of it until it's let go, though Dear ImGui keeps the focus from the last time,
    // so that it presses nothing: a key's repeats never reach the menu (overlay_event), and a
    // gamepad's buttons are latched here.
    ImGui::GetIO().ClearInputKeys();
    for (HeldPad& h : g_pads) h.latched = buttons_down(h.pad);
    g_focus = true;
    g_dumped = false;
}

// Every gamepad SDL has, held open after letting go of those held before, so that the menu
// reads each one and each one's Back opens it, on a port or not. A device controls.txt binds
// is the file's alone, as it is for the ports (gamepads.cpp), so it's left out. At the start
// and again as a gamepad comes or goes. While the menu is open, what each one holds is latched
// as at the open.
void hold_pads() {
    for (const HeldPad& h : g_pads) SDL_CloseGamepad(h.pad);
    g_pads.clear();
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n; i++) {
        if (input_map_binds(ids[i])) continue;
        if (SDL_Gamepad* p = SDL_OpenGamepad(ids[i])) g_pads.push_back({p, g_open ? buttons_down(p) : 0});
    }
    SDL_free(ids);
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

// GCN_OVERLAY_AT=<frame>:<host frames>, both whole numbers, the second over 0.
void parse_at(const char* e) {
    char* end = nullptr;
    const unsigned long frame = strtoul(e, &end, 10);
    if (end != e && *end == ':' && end[1] >= '0' && end[1] <= '9') {
        const long left = strtol(end + 1, &end, 10);
        if (!*end && left > 0 && frame <= UINT32_MAX) {
            g_at_frame = (long long)frame;
            g_at_left = left;
            return;
        }
    }
    fprintf(stderr, "[overlay] GCN_OVERLAY_AT=%s isn't <frame>:<host frames>, so it doesn't open\n", e);
}

// --dump-output: the menu over the held frame in a target of the window's size, read back from
// there, since a hidden window's own framebuffer has nothing to read.
void dump() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    if (w <= 0 || h <= 0) return;
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    gx::render_set_output_fbo(fbo);
    const bool drawn = gx::render_present_last();
    gx::render_set_output_fbo(0);
    if (drawn) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        std::vector<uint8_t> px((size_t)w * h * 4), rows(px.size());
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        const size_t stride = (size_t)w * 4;
        for (int y = 0; y < h; y++) memcpy(&rows[y * stride], &px[(size_t)(h - 1 - y) * stride], stride);
        for (size_t i = 3; i < rows.size(); i += 4) rows[i] = 255;
        char path[512];
        snprintf(path, sizeof(path), "%s/overlay_%05u.png", gx::g_dump_dir, gx::present_count());
        write_png(path, rows.data(), w, h);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
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

// Below the pages, so on every one. A replay has no Restart game: the log can't hold one, and
// a relaunch would replay the log from its start (frontend_restart).
void actions() {
    ImGui::Separator();
    if (!input_replay_active()) {
        if (ImGui::Button("Restart game")) ask(OverlayAsk::restart);
        if (g_asked == OverlayAsk::restart) ImGui::FocusItem();
        ImGui::SameLine();
    }
    if (ImGui::Button("Quit game")) ask(OverlayAsk::quit);
    if (g_asked == OverlayAsk::quit) ImGui::FocusItem();
    g_asked = OverlayAsk::none;
}

void asking() {
    const bool restart = g_ask == OverlayAsk::restart;
    ImGui::TextUnformatted(restart ? "Restart the game? Anything it hasn't saved is lost."
                                   : "Quit the game? Anything it hasn't saved is lost.");
    if (ImGui::Button("Cancel")) back();
    if (g_focus_cancel) ImGui::FocusItem();
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
    ImGui::Begin(g_title.c_str(), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    if (g_ask != OverlayAsk::none) {
        asking();
    } else {
        if (ImGui::BeginTabBar("pages")) {
            if (ImGui::BeginTabItem("Display")) {
                display_page(g_window);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        actions();
    }
    ImGui::End();
}

// The menu's frame: its input taken and its windows laid out, ready to draw.
void menu_frame() {
    ImGui_ImplSDL3_NewFrame();
    read_pads();
    ImGui::NewFrame();
    draw_menu();
    ImGui::Render();
}

}  // namespace

void overlay_init(SDL_Window* window, const char* title, const OverlayPicture& picture) {
    g_ready = true;
    g_window = window;
    g_title = title;
    display_init(picture);
    if (const char* e = getenv("GCN_OVERLAY_AT")) parse_at(e);
    if (!window) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written to the game's directory
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    // Twice Dear ImGui's own size, to be read from a couch, times the display's content scale.
    const float content = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window));
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontScaleMain = 2.0f;
    style.FontScaleDpi = content > 0.0f ? content : 1.0f;
    style.ScaleAllSizes(style.FontScaleMain * style.FontScaleDpi);
    g_gl = SDL_GL_GetCurrentContext() != nullptr;
    if (g_gl) {
        ImGui_ImplSDL3_InitForOpenGL(window, SDL_GL_GetCurrentContext());
        ImGui_ImplOpenGL3_Init();
    } else {
        // Nothing draws the menu, so the texture its font is baked into is never made.
        ImGui_ImplSDL3_InitForOther(window);
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    }
    // The backend reads no gamepad, so read_pads can read them all by their labels.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
    hold_pads();
}

bool overlay_open() { return g_open; }

void overlay_close() { set_open(false); }

OverlayAsk overlay_asking() { return g_ask; }

bool overlay_event(const SDL_Event& e) {
    if (!g_ready) return false;
    const SDL_Scancode key = e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat ? e.key.scancode : SDL_SCANCODE_UNKNOWN;
    const bool pressed = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    const bool cancel = g_open && (key == SDL_SCANCODE_ESCAPE || (pressed && is_b(e.gbutton)));
    // While the menu asks, B or Escape goes back to it instead of closing it.
    if (cancel && g_ask != OverlayAsk::none) {
        back();
        return true;
    }
    if (key == SDL_SCANCODE_F1 || (pressed && e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) || cancel) {
        set_open(!g_open);
        return true;
    }
    if (g_window && (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED)) hold_pads();
    else if (g_window && g_open && !(e.type == SDL_EVENT_KEY_DOWN && e.key.repeat)) ImGui_ImplSDL3_ProcessEvent(&e);
    return g_open && is_input(e);
}

void overlay_gate(int port, PadState& p) {
    if (port < 0 || port > 3) return;
    if (g_open) {
        g_held_over[port] = 0xFFFF;
        const bool connected = p.connected;
        // Until the guest is held at its frame boundary it may poll again, and a port at rest
        // would be a press let go, which a game may act on.
        p = paused() ? PadState{} : g_before[port];
        p.connected = connected;
        return;
    }
    g_held_over[port] &= p.buttons;
    p.buttons &= ~g_held_over[port];
    g_before[port] = p;
}

void overlay_menu_frame() {
    if (g_window && g_open) menu_frame();
}

void overlay_draw() {
    if (!g_window || !g_gl) return;
    if (g_at_frame >= 0 && gx::present_count() >= g_at_frame) {
        g_at_frame = -1;
        set_open(true);
    }
    if (!g_open) return;
    ImGui_ImplOpenGL3_NewFrame();
    menu_frame();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (!paused()) return;
    // A menu's first frame only lays it out and draws nothing, so the dump waits for one that drew.
    if (!g_dumped && gx::g_dump_output && gx::g_dump_dir && ImGui::GetDrawData()->TotalVtxCount > 0) {
        g_dumped = true;
        dump();
    }
    if (g_at_left > 0 && --g_at_left == 0) set_open(false);
}
