// The notices over the game about controllers (overlay.h).
#include "overlay/overlay.h"
#include "runtime.h"  // host_ns
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

void overlay_imgui_context(SDL_Window* window, bool gl);  // overlay.cpp's
void overlay_style_fit(SDL_Window* window);              // overlay.cpp's

namespace {

constexpr uint64_t kShownNs = 2000000000;  // how long each one shows, in host time
struct Notice {
    std::string text;
    uint64_t since;  // host_ns as it was queued
};
std::vector<Notice> g_notices;
ImGuiContext* g_context;  // their own, null with no window
SDL_Window* g_window;     // the one they show over
bool g_gl;                // drawn, not only laid out
long g_frames;            // frames they've been laid out in

void expire() {
    const uint64_t now = host_ns();
    std::erase_if(g_notices, [&](const Notice& n) { return now - n.since >= kShownNs; });
}

}  // namespace

void overlay_pads_notice(const std::string& text) {
    fprintf(stderr, "[pads] %s\n", text.c_str());
    g_notices.push_back({text, host_ns()});
}

std::vector<std::string> overlay_notices() {
    expire();
    std::vector<std::string> out;
    for (const Notice& n : g_notices) out.push_back(n.text);
    return out;
}

long overlay_notice_frames() { return g_frames; }

// overlay.cpp's, before the menu's context is made. The game's cursor is left as it is, and so
// is the mouse's capture: this context gets no events, so its backend would let go of the one
// the menu's took, at each frame of notices.
void notices_init(SDL_Window* window, bool gl) {
    overlay_imgui_context(window, gl);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplSDL3_SetMouseCaptureMode(ImGui_ImplSDL3_MouseCaptureMode_Disabled);
    g_context = ImGui::GetCurrentContext();
    g_window = window;
    g_gl = gl;
}

// overlay.cpp's, after the menu, if one is showing: centred at the bottom of the window, oldest
// first, in a window that takes no input or focus.
void notices_frame() {
    expire();
    if (!g_context || g_notices.empty()) return;
    ImGuiContext* menu = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_context);
    overlay_style_fit(g_window);
    if (g_gl) ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->WorkPos.y + vp->WorkSize.y - ImGui::GetTextLineHeight()),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::Begin("##pads notices", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings);
    for (const Notice& n : g_notices) ImGui::TextUnformatted(n.text.c_str());
    ImGui::End();
    ImGui::Render();
    if (g_gl) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    g_frames++;
    ImGui::SetCurrentContext(menu);
}
