// The notices over the game about controllers (overlay.h).
#include "overlay/overlay.h"
#include "overlay/fonts.h"
#include "overlay/review.h"
#include "overlay/theme.h"
#include "overlay/toasts.h"
#include "env.h"
#include "gx/render_gl.h"  // the dump's flags
#include "runtime.h"       // host_ns
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

void overlay_imgui_context(SDL_Window* window, bool gl);  // overlay.cpp's
void overlay_style_fit(SDL_Window* window);              // overlay.cpp's

namespace {

struct Notice {
    std::string text;
    size_t lead;     // the controller's name, the text's first `lead` bytes
    uint64_t since;  // host_ns as it was queued
    Tween slot;      // its place in the stack, 0 the lowest (toasts.h)
    bool over;       // queued over another, so it rises no further than the gap (toasts.h)
};
std::vector<Notice> g_notices;
ImGuiContext* g_context;  // their own, null with no window
SDL_Window* g_window;     // the one they show over
bool g_gl;                // drawn, not only laid out
long g_frames;            // frames they've been laid out in
bool g_motion = true;
int g_review;             // GCN_OVERLAY_TOASTS's count, 0 for none or once they're dumped
bool g_plain;             // GCN_OVERLAY_PLAIN: dumped over plain grey, not the held frame
ReviewWatch g_watch;      // their frame settling for the dump
#ifdef GCN_OVERLAY_TEST
std::vector<OverlayTestToast> g_laid;  // each pill as the last frame laid it out
#endif

// Each one gone at exactly kToastNs, motion or not, and the ones above it moving down into its
// place.
void expire(uint64_t now) {
    std::erase_if(g_notices, [&](const Notice& n) { return now - n.since >= kToastNs; });
    for (size_t i = 0; i < g_notices.size(); i++) {
        Tween& slot = g_notices[i].slot;
        if (slot.to != (float)i) slot = toast_reflow(theme_now(), slot, now, (float)i, g_motion);
    }
}

// The theme's 0xRRGGBBAA colour `c` as Dear ImGui packs one, its alpha times `a`.
ImU32 faded(uint32_t c, float a) {
    return theme_colour((c & ~0xFFu) | (uint32_t)std::lround(std::clamp((float)(c & 0xFF) * a, 0.0f, 255.0f)));
}

// Each notice a pill in the window's draw list: the panel's colour with its edge, the body text
// in it, the controller's name in the text's colour and what happened in the dimmed one. The
// oldest sits lowest, the theme's margin above the window's bottom edge, each newer one above
// the last, all centred across. Each fades and rises as it ages, and moves down as the stack
// reflows, at `now` (toasts.h).
void pills(uint64_t now) {
    const OverlayTheme& t = theme_now();
    const float s = ImGui::GetStyle().FontScaleMain;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = t.item_min_height * s, pad = t.padding * s, gap = t.item_spacing * s;
    const float edge = ImGui::GetStyle().FrameBorderSize;
    const float bottom = vp->Pos.y + vp->Size.y - t.margin * s;
    for (const Notice& n : g_notices) {
        const ToastLook look = toast_look(t, now - n.since, g_motion, n.over);
        const char* text = n.text.c_str();
        const char* split = text + n.lead;
        const float lead_w = ImGui::CalcTextSize(text, split).x;
        const float w = lead_w + ImGui::CalcTextSize(split).x + 2.0f * pad;
        const ImVec2 lo(vp->GetCenter().x - w / 2.0f, bottom - n.slot.at(now) * (h + gap) - h + look.below * s);
        const ImVec2 hi(lo.x + w, lo.y + h);
        dl->AddRectFilled(lo, hi, faded(t.panel, look.alpha), h / 2.0f);
        if (edge > 0.0f) dl->AddRect(lo, hi, faded(t.panel_edge, look.alpha), h / 2.0f, 0, edge);
        const float y = lo.y + (h - ImGui::GetFontSize()) / 2.0f;
        dl->AddText(ImVec2(lo.x + pad, y), faded(t.text, look.alpha), text, split);
        dl->AddText(ImVec2(lo.x + pad + lead_w, y), faded(t.dim, look.alpha), split);
#ifdef GCN_OVERLAY_TEST
        g_laid.push_back({lo.y, h, look.alpha});
#endif
    }
}

// Under --dump-output with GCN_OVERLAY_TOASTS=<n>, the first frame with n or more showing that
// looks as the two before it did is written once as overlay_toasts.png. A frame with fewer starts
// the watch again.
void dump() {
    if (!g_review || !g_gl || !gx::g_dump_output || !gx::g_dump_dir) return;
    if (g_notices.size() < (size_t)g_review) {
        g_watch = {};
        return;
    }
    if (!review_settled(g_watch, ImGui::GetDrawData())) return;
    g_review = 0;
    review_write(g_window, ImGui::GetDrawData(), "overlay_toasts.png", g_plain);
}

}  // namespace

void overlay_pads_notice(const std::string& name, const std::string& what) {
    const std::string text = name + what;
    fprintf(stderr, "[pads] %s\n", text.c_str());
    // Its place moves as the one under it does, a place above it, so the two never close up as
    // the stack reflows.
    const bool over = !g_notices.empty();
    Tween slot;
    if (over) {
        slot = g_notices.back().slot;
        slot.from += 1.0f;
        slot.to += 1.0f;
    }
    g_notices.push_back({text, name.size(), host_ns(), slot, over});
}

void notices_motion(bool on) { g_motion = on; }

std::vector<std::string> overlay_notices() {
    expire(host_ns());
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
    const char* review = getenv_nonempty("GCN_OVERLAY_TOASTS");
    g_review = review ? review_parse_toasts(review) : 0;
    g_plain = getenv_nonempty("GCN_OVERLAY_PLAIN") != nullptr;
    g_watch = {};
}

// overlay.cpp's, after the menu, if one is showing: the pills over the whole window, in a window
// of their own that has no background or edge and takes no input or focus. With none showing
// there's no frame at all.
void notices_frame() {
    const uint64_t now = host_ns();
    expire(now);
#ifdef GCN_OVERLAY_TEST
    g_laid.clear();
#endif
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
    pills(now);
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::Render();
    if (g_gl) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    g_frames++;
    dump();
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

const std::vector<OverlayTestToast>& overlay_test_toasts() { return g_laid; }
#endif
