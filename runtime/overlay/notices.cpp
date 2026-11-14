// The notices over the game about controllers (overlay.h).
#include "overlay/overlay.h"
#include "overlay/fonts.h"
#include "overlay/theme.h"
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
    size_t lead;     // the controller's name, the text's first `lead` bytes
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

// Each notice a pill in the window's draw list: the panel's colour with its edge, the body text
// in it, the controller's name in the text's colour and what happened in the dimmed one. The
// oldest sits lowest, the theme's margin above the window's bottom edge, each newer one above
// the last, all centred across.
void pills() {
    const OverlayTheme& t = theme_now();
    const float s = ImGui::GetStyle().FontScaleMain;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = t.item_min_height * s, pad = t.padding * s, gap = t.item_spacing * s;
    const float edge = ImGui::GetStyle().FrameBorderSize;
    const float bottom = vp->Pos.y + vp->Size.y - t.margin * s;
    for (size_t i = 0; i < g_notices.size(); i++) {
        const Notice& n = g_notices[i];
        const char* text = n.text.c_str();
        const char* split = text + n.lead;
        const float lead_w = ImGui::CalcTextSize(text, split).x;
        const float w = lead_w + ImGui::CalcTextSize(split).x + 2.0f * pad;
        const ImVec2 lo(vp->GetCenter().x - w / 2.0f, bottom - (float)i * (h + gap) - h);
        const ImVec2 hi(lo.x + w, lo.y + h);
        dl->AddRectFilled(lo, hi, theme_colour(t.panel), h / 2.0f);
        if (edge > 0.0f) dl->AddRect(lo, hi, theme_colour(t.panel_edge), h / 2.0f, 0, edge);
        const float y = lo.y + (h - ImGui::GetFontSize()) / 2.0f;
        dl->AddText(ImVec2(lo.x + pad, y), theme_colour(t.text), text, split);
        dl->AddText(ImVec2(lo.x + pad + lead_w, y), theme_colour(t.dim), split);
    }
}

}  // namespace

void overlay_pads_notice(const std::string& name, const std::string& what) {
    const std::string text = name + what;
    fprintf(stderr, "[pads] %s\n", text.c_str());
    g_notices.push_back({text, name.size(), host_ns()});
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

// overlay.cpp's, after the menu, if one is showing: the pills over the whole window, in a window
// of their own that has no background or edge and takes no input or focus. With none showing
// there's no frame at all.
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
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##pads notices", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(fonts_role(FontRole::body), theme_now().body_size);
    pills();
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::Render();
    if (g_gl) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    g_frames++;
    ImGui::SetCurrentContext(menu);
}

#ifdef GCN_OVERLAY_TEST
void overlay_imgui_context_end(bool gl);  // overlay.cpp's

// overlay.cpp's, for overlay_test_end.
void notices_test_end() {
    if (!g_context) return;
    ImGuiContext* menu = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_context);
    overlay_imgui_context_end(g_gl);
    g_context = nullptr;
    ImGui::SetCurrentContext(menu);
}

ImGuiContext* overlay_test_notices_context() { return g_context; }
#endif
