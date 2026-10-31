// The overlay's Display page: the aspect, fullscreen and the internal scale (overlay.h).
#include "overlay/overlay.h"
#include "gx/render_gl.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include "imgui.h"

namespace {
OverlayPicture g_picture;
}

void display_init(const OverlayPicture& picture) { g_picture = picture; }

void overlay_display_change(const Settings& next) {
    Settings& now = g_picture.now;
    Settings& saved = g_picture.saved;
    if (next.aspect == now.aspect && next.fullscreen == now.fullscreen && next.scale == now.scale) return;
    if (next.aspect != now.aspect) saved.aspect = next.aspect;
    if (next.fullscreen != now.fullscreen) saved.fullscreen = next.fullscreen;
    if (next.scale != now.scale) saved.scale = next.scale;
    now = next;
    if (g_picture.apply) g_picture.apply(now);
    settings_save(g_picture.path, saved);
}

void display_page(SDL_Window* window) {
    // F11 or the window's own control may have changed it since the last draw.
    g_picture.now.fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    Settings next = g_picture.now;
    static const char* const kAspects[][2] = {
        {"4:3", "4:3"}, {"16:9", "16:9"}, {"16:10", "16:10"}, {"21:9", "21:9"}, {"window", "Fill window"}};
    ImGui::SeparatorText("Aspect ratio");
    for (const auto& a : kAspects) {
        if (ImGui::RadioButton(a[1], next.aspect == a[0])) next.aspect = a[0];
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Separator();
    ImGui::Checkbox("Fullscreen", &next.fullscreen);
    ImGui::SeparatorText("Scale");
    for (int s = 1; s <= gx::kMaxInternalScale; s++) {
        char label[8];
        snprintf(label, sizeof(label), "%dx", s);
        if (ImGui::RadioButton(label, std::min(next.scale, gx::kMaxInternalScale) == s)) next.scale = s;
        if (s < gx::kMaxInternalScale) ImGui::SameLine();
    }
    overlay_display_change(next);
}
