// The focus ring (ring.h).
#include "overlay/ring.h"
#include "overlay/tabs.h"
#include "overlay/theme.h"
#include <algorithm>
#include <cmath>
#include "imgui.h"
#include "imgui_internal.h"  // the item with the focus, and its rect

namespace {

bool g_motion = true;
bool g_snap = true;     // the next frame puts it on its item at once
bool g_shown;           // it showed at the last frame
bool g_moving;          // and wasn't on its item yet
ImRect g_at, g_item;    // where it was then, as the rect it goes around, and its item's rect
uint64_t g_last;        // host_ns then

// Whether `at` is more than half the panel's width or height from `to`, centre to centre.
bool far(const ImRect& at, const ImRect& to, const ImVec2& panel) {
    const ImVec2 a = at.GetCenter(), b = to.GetCenter();
    return std::fabs(a.x - b.x) > panel.x / 2.0f || std::fabs(a.y - b.y) > panel.y / 2.0f;
}

// `at` moved `k` of the way to `to`, or onto it within half a pixel.
ImRect toward(const ImRect& at, const ImRect& to, float k) {
    const ImRect r(ImLerp(at.Min, to.Min, k), ImLerp(at.Max, to.Max, k));
    const bool there = std::fabs(r.Min.x - to.Min.x) <= 0.5f && std::fabs(r.Min.y - to.Min.y) <= 0.5f &&
                       std::fabs(r.Max.x - to.Max.x) <= 0.5f && std::fabs(r.Max.y - to.Max.y) <= 0.5f;
    return there ? to : r;
}

// The ring around `at`, moved `dx` across and faded by `alpha`, in the foreground's draw list,
// which the panel's motion scales and fades with the menu's contents (appear_apply). Where the
// item is cut off by `clip`, its window's, the ring goes around what shows of it, as Dear ImGui's
// own highlight does.
void draw(const ImRect& at, const ImRect& clip, float dx, float alpha) {
    const OverlayTheme& t = theme_now();
    const float s = ImGui::GetStyle().FontScaleMain;
    // Whole pixels, at least one, as an item's edge is (theme.cpp).
    const float width = std::max(1.0f, std::round(t.focus_width * s));
    const float out = std::max(0.0f, std::round(t.focus_gap * s)) + width / 2.0f;
    ImRect ring = at;
    ring.Expand(out);
    ImRect shown = at;
    shown.ClipWith(clip);
    shown.Expand(out + width / 2.0f);
    ring.Translate(ImVec2(dx, 0.0f));
    shown.Translate(ImVec2(dx, 0.0f));
    const uint32_t a = (uint32_t)std::lround((float)(t.accent & 0xFF) * alpha);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(shown.Min, shown.Max);
    dl->AddRect(ring.Min, ring.Max, theme_colour((t.accent & ~0xFFu) | a), t.item_radius * s + out, 0, width);
    dl->PopClipRect();
}

}  // namespace

void ring_motion(bool on) { g_motion = on; }

void ring_snap() { g_snap = true; }

bool ring_moving() { return g_moving; }

#ifdef GCN_OVERLAY_TEST
bool ring_test(float at[4], float item[4]) {
    const float a[4] = {g_at.Min.x, g_at.Min.y, g_at.Max.x, g_at.Max.y};
    const float b[4] = {g_item.Min.x, g_item.Min.y, g_item.Max.x, g_item.Max.y};
    std::copy(a, a + 4, at);
    std::copy(b, b + 4, item);
    return g_shown;
}
#endif

void ring_draw(uint64_t now) {
    const double dt_ms = now > g_last ? (double)(now - g_last) / 1e6 : 0.0;
    g_last = now;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* w = g.NavWindow;
    ImGuiWindow* panel = ImGui::GetCurrentWindow()->RootWindow;
    // NavIdIsAlive: the item with the focus was laid out at this frame, so its rect is this frame's.
    const bool was_shown = g_shown;
    g_shown = w && w->RootWindow == panel && g.NavCursorVisible && g.NavIdIsAlive;
    g_moving = false;
    if (!g_shown) return;
    g_item = ImGui::WindowRectRelToAbs(w, w->NavRectRel[g.NavLayer]);
    const float ms = theme_now().focus_ms;
    // A game's theme may give a time that's negative, infinite or not a number, which takes none.
    const bool eases = g_motion && std::isfinite(ms) && ms > 0.0f;
    // A page change, and a question or a view in place of the page or back from one, leave a frame
    // with no item that has the focus laid out: the change clears the focus (tabs_focus) and the
    // button that asked, or the question, isn't there. So the ring isn't shown then, and
    // !was_shown puts it on the next item at once.
    if (!eases || g_snap || !was_shown || far(g_at, g_item, panel->Size)) {
        g_at = g_item;
    } else {
        g_at = toward(g_at, g_item, (float)(1.0 - std::exp(-dt_ms / ms)));
    }
    g_snap = false;
    g_moving = g_at.Min.x != g_item.Min.x || g_at.Min.y != g_item.Min.y || g_at.Max.x != g_item.Max.x ||
               g_at.Max.y != g_item.Max.y;
    // Around an item in the footer or a question it stays still and whole as the page comes in.
    float dx = 0.0f, alpha = 1.0f;
    if (tabs_contents_focused()) tabs_contents_moved(now, dx, alpha);
    draw(g_at, w->InnerClipRect, dx, alpha);
}
