// The focus ring (ring.h).
#include "overlay/ring.h"
#include "overlay/theme.h"
#include <algorithm>
#include <cmath>
#include "imgui.h"
#include "imgui_internal.h"  // the item with the focus, and its rect

namespace {

// The ring around `item`, in the foreground's draw list, which the panel's motion scales and fades
// with the menu's contents (appear_apply). Where the item is cut off by `clip`, its window's, the
// ring goes around what shows of it, as Dear ImGui's own highlight does.
void draw(const ImRect& item, const ImRect& clip) {
    const OverlayTheme& t = theme_now();
    const float s = ImGui::GetStyle().FontScaleMain;
    // Whole pixels, at least one, as an item's edge is (theme.cpp).
    const float width = std::max(1.0f, std::round(t.focus_width * s));
    const float out = std::max(0.0f, std::round(t.focus_gap * s)) + width / 2.0f;
    ImRect ring = item;
    ring.Expand(out);
    ImRect shown = item;
    shown.ClipWith(clip);
    shown.Expand(out + width / 2.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(shown.Min, shown.Max);
    dl->AddRect(ring.Min, ring.Max, theme_colour(t.accent), t.item_radius * s + out, 0, width);
    dl->PopClipRect();
}

}  // namespace

void ring_draw() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* w = g.NavWindow;
    // NavIdIsAlive: the item with the focus was laid out at this frame, so its rect is this frame's.
    if (!w || w->RootWindow != ImGui::GetCurrentWindow()->RootWindow || !g.NavCursorVisible || !g.NavIdIsAlive)
        return;
    draw(ImGui::WindowRectRelToAbs(w, w->NavRectRel[g.NavLayer]), w->InnerClipRect);
}
