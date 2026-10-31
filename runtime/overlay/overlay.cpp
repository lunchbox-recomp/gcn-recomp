// The overlay's set-up, opening and closing, and the input it holds back; see overlay.h. The
// Display page is overlay_display.cpp.
#include "overlay/overlay.h"
#include "gx/gl.h"
#include "gx/render_gl.h"
#include "pause.h"
#include <SDL3/SDL.h>
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
bool g_focus;             // the menu takes the keyboard's and the gamepads' focus at its next draw
bool g_dumped;            // since it opened, under --dump-output
long long g_at_frame = -1;  // GCN_OVERLAY_AT's, -1 for none or once it's opened the overlay
long g_at_left;             // draws over the held frame left before GCN_OVERLAY_AT's closes it

void set_open(bool on) {
    if (on == g_open) return;
    g_open = on;
    g_ask = g_asked = OverlayAsk::none;
    pause_request(on);
    fprintf(stderr, "[overlay] %s\n", on ? "open" : "closed");
    if (!on || !g_window) return;
    // Keys and buttons let go while it was closed never reached the menu.
    ImGui::GetIO().ClearInputKeys();
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    g_focus = true;
    g_dumped = false;
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

// Below the pages, so on every one.
void actions() {
    ImGui::Separator();
    if (ImGui::Button("Restart game")) ask(OverlayAsk::restart);
    if (g_asked == OverlayAsk::restart) ImGui::FocusItem();
    ImGui::SameLine();
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
        return;
    }
    // Nothing draws the menu, so the texture its font is baked into is never made.
    ImGui_ImplSDL3_InitForOther(window);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
}

bool overlay_open() { return g_open; }

void overlay_close() { set_open(false); }

OverlayAsk overlay_asking() { return g_ask; }

bool overlay_event(const SDL_Event& e) {
    if (!g_ready) return false;
    const SDL_Scancode key = e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat ? e.key.scancode : SDL_SCANCODE_UNKNOWN;
    const int button = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e.gbutton.button : -1;
    const bool cancel = key == SDL_SCANCODE_ESCAPE || button == SDL_GAMEPAD_BUTTON_EAST;
    // While the menu asks, B or Escape goes back to it instead of closing it.
    if (g_open && g_ask != OverlayAsk::none && cancel) {
        back();
        return true;
    }
    if (key == SDL_SCANCODE_F1 || button == SDL_GAMEPAD_BUTTON_BACK || (g_open && cancel)) {
        set_open(!g_open);
        return true;
    }
    // Gamepads coming and going reach the menu while it's closed too, so it knows them all.
    if (g_window && (g_open || e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED))
        ImGui_ImplSDL3_ProcessEvent(&e);
    return g_open && is_input(e);
}

void overlay_gate(int port, PadState& p) {
    if (port < 0 || port > 3) return;
    if (g_open) {
        g_held_over[port] = 0xFFFF;
        const bool connected = p.connected;
        p = PadState{};
        p.connected = connected;
        return;
    }
    g_held_over[port] &= p.buttons;
    p.buttons &= ~g_held_over[port];
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
