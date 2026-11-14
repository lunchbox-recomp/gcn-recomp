// How the menu comes and goes (appear.h).
#include "overlay/appear.h"
#include "overlay/motion.h"
#include <algorithm>
#include <cmath>
#include "imgui.h"

namespace {

bool g_motion = true;
bool g_open;  // opened last, not closed
// From 0, gone, to 1, whole: the panel's alpha, its contents' and the backdrop's share of the
// theme's. And the panel's scale.
Tween g_panel, g_contents, g_backdrop, g_scale;

// A change's length, none with motion off.
float span(float ms) { return g_motion ? std::max(ms, 0.0f) : 0.0f; }

}  // namespace

void appear_motion(bool on) { g_motion = on; }

void appear_open(const OverlayTheme& t, uint64_t now) {
    const bool gone = g_panel.at(now) <= 0.0f;
    g_open = true;
    const float delay = std::clamp(t.contents_delay_ms, 0.0f, t.open_ms);
    g_panel = tween_to(g_panel, now, 1.0f, span(t.open_ms), Ease::out_back, 0.0f, t.open_overshoot);
    g_scale = tween_to(g_scale, now, 1.0f, span(t.open_ms), Ease::out_back, 0.0f, t.open_overshoot);
    if (gone) g_scale.from = t.open_scale;
    g_contents = tween_to(g_contents, now, 1.0f, span(t.open_ms - delay), Ease::out_quad, span(delay));
    g_backdrop = tween_to(g_backdrop, now, 1.0f, span(t.backdrop_ms), Ease::out_quad);
}

void appear_close(const OverlayTheme& t, uint64_t now) {
    g_open = false;
    const float ms = span(t.close_ms);
    g_panel = tween_to(g_panel, now, 0.0f, ms, Ease::in_quad);
    g_scale = tween_to(g_scale, now, t.close_scale, ms, Ease::in_quad);
    g_contents = tween_to(g_contents, now, 0.0f, ms, Ease::in_quad);
    g_backdrop = tween_to(g_backdrop, now, 0.0f, ms, Ease::in_quad);
}

bool appear_closing(uint64_t now) { return !g_open && appear_moving(now); }

bool appear_moving(uint64_t now) {
    return !g_panel.done(now) || !g_scale.done(now) || !g_contents.done(now) || !g_backdrop.done(now);
}

bool appear_shown(uint64_t now) { return g_open && g_panel.done(now) && g_scale.done(now) && g_contents.done(now); }

float appear_backdrop(uint64_t now) { return std::clamp(g_backdrop.at(now), 0.0f, 1.0f); }

float appear_panel_alpha(uint64_t now) { return std::clamp(g_panel.at(now), 0.0f, 1.0f); }

// Never more than the panel under them.
float appear_contents_alpha(uint64_t now) {
    return std::min(std::clamp(g_contents.at(now), 0.0f, 1.0f), appear_panel_alpha(now));
}

float appear_scale(uint64_t now) { return g_scale.at(now); }

void appear_apply(ImDrawData* d, const ImDrawList* skip, const ImDrawList* panel, int panel_vtx, float cx, float cy,
                  float scale, float panel_alpha, float contents_alpha) {
    if (scale == 1.0f && panel_alpha >= 1.0f && contents_alpha >= 1.0f) return;
    const auto fade = [](ImU32 c, float a) {
        const ImU32 alpha = (ImU32)std::lround((float)(c >> IM_COL32_A_SHIFT & 0xFF) * a);
        return (c & ~IM_COL32_A_MASK) | alpha << IM_COL32_A_SHIFT;
    };
    const auto x = [&](float v) { return cx + (v - cx) * scale; };
    const auto y = [&](float v) { return cy + (v - cy) * scale; };
    for (ImDrawList* l : d->CmdLists) {
        if (l == skip) continue;
        for (int i = 0; i < l->VtxBuffer.Size; i++) {
            ImDrawVert& v = l->VtxBuffer[i];
            v.pos = ImVec2(x(v.pos.x), y(v.pos.y));
            v.col = fade(v.col, l == panel && i < panel_vtx ? panel_alpha : contents_alpha);
        }
        for (ImDrawCmd& c : l->CmdBuffer) c.ClipRect = ImVec4(x(c.ClipRect.x), y(c.ClipRect.y), x(c.ClipRect.z), y(c.ClipRect.w));
    }
}
