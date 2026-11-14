// The overlay's motion checks: the `test_overlay_motion` target. Runs with no window, GL, disc
// or game, on a Dear ImGui context of its own, with the host clock given by hand.
//
// runtime/overlay/motion.h's five easings start at 0 and end at 1, and all but out_back stay
// within that on the way. A tween waits out its delay, follows its easing on host time and holds
// its end, a tween of no length is at its end at once, and one turned mid-way starts from the
// value showing. runtime/overlay/appear.h opens the menu over the theme's times, the contents
// behind the panel and the backdrop last, and is whole when the panel and the contents are,
// within 200 ms, or at once with a negative open time. It closes over the theme's close time
// and can open again from wherever it was. With motion off each change is whole at once. Its
// transform of a frame's draw data scales every list but the backdrop's about the centre, clip
// rects too, and fades the panel's vertices and the contents' each by their own alpha. The
// standard library checks its preconditions as these run (CMakeLists.txt).
//
// usage: test_overlay_motion
// Exits 0 only if every check passes.
#include "overlay/appear.h"
#include "overlay/motion.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include "imgui.h"

namespace {

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

bool near(float a, float b, float within = 1e-4f) { return std::fabs(a - b) <= within; }

constexpr uint64_t kMs = 1000000;  // host_ns in a millisecond

const Ease kAll[] = {Ease::out_quad, Ease::out_cubic, Ease::in_quad, Ease::out_circ, Ease::out_back};

void easings() {
    bool ends = true;
    for (Ease e : kAll) ends = ends && near(ease(e, 0.0f), 0.0f) && near(ease(e, 1.0f), 1.0f) &&
                               near(ease(e, -0.5f), 0.0f) && near(ease(e, 1.5f), 1.0f);
    check(ends, "every easing is 0 at the start and 1 at the end, before and after them too");
    check(near(ease(Ease::out_quad, 0.5f), 0.75f) && near(ease(Ease::out_cubic, 0.5f), 0.875f) &&
              near(ease(Ease::in_quad, 0.5f), 0.25f) && near(ease(Ease::out_circ, 0.5f), std::sqrt(0.75f)) &&
              near(ease(Ease::out_back, 0.5f), 1.025f),
          "half way, out-quad is 0.75, out-cubic 0.875, in-quad 0.25, out-circ 0.866 and out-back at 1.2 is 1.025");
    bool within = true, rising = true;
    for (Ease e : kAll) {
        if (e == Ease::out_back) continue;
        float last = 0.0f;
        for (int i = 0; i <= 100; i++) {
            const float v = ease(e, i / 100.0f);
            within = within && v >= 0.0f && v <= 1.0f;
            rising = rising && v >= last;
            last = v;
        }
    }
    check(within && rising, "out-quad, out-cubic, in-quad and out-circ rise from 0 to 1 and never leave it");
    float peak = 0.0f;
    for (int i = 0; i <= 100; i++) peak = std::max(peak, ease(Ease::out_back, i / 100.0f));
    check(peak > 1.0f && peak < 1.1f && ease(Ease::out_back, 0.5f, 2.0f) > ease(Ease::out_back, 0.5f, 1.2f),
          "out-back passes 1 by under a tenth before it settles, and by more with more overshoot");
}

void tweens() {
    const uint64_t t0 = 1000 * kMs;
    Tween t;
    t.from = 2.0f;
    t.to = 4.0f;
    t.start = t0;
    t.ms = 100.0f;
    t.delay_ms = 20.0f;
    t.e = Ease::in_quad;
    check(near(t.at(t0), 2.0f) && near(t.at(t0 + 20 * kMs), 2.0f) && near(t.at(t0 - 5 * kMs), 2.0f),
          "a tween is at its start through its delay, and before it starts");
    check(near(t.at(t0 + 70 * kMs), 2.5f) && !t.done(t0 + 119 * kMs),
          "half way through its time, past the delay, an in-quad tween from 2 to 4 is at 2.5");
    check(near(t.at(t0 + 120 * kMs), 4.0f) && near(t.at(t0 + 5000 * kMs), 4.0f) && t.done(t0 + 120 * kMs),
          "it ends at its end on time and holds it");
    Tween now;
    now.from = 0.0f;
    now.to = 1.0f;
    now.start = t0;
    check(near(now.at(t0), 1.0f) && now.done(t0), "a tween of no length is at its end at once");
    const Tween turned = tween_to(t, t0 + 70 * kMs, 0.0f, 50.0f, Ease::out_quad);
    check(near(turned.at(t0 + 70 * kMs), 2.5f) && near(turned.at(t0 + 120 * kMs), 0.0f),
          "a tween turned mid-way starts from the value showing and ends at its new end");
}

void opens_and_closes() {
    const OverlayTheme th = overlay_theme_default();
    const uint64_t t0 = 5000 * kMs;
    appear_motion(true);
    appear_open(th, t0);
    check(near(appear_panel_alpha(t0), 0.0f) && near(appear_scale(t0), th.open_scale) &&
              near(appear_backdrop(t0), 0.0f) && appear_moving(t0) && !appear_shown(t0) && !appear_closing(t0),
          "as it opens the panel is clear at the theme's open scale and the backdrop clear");
    check(appear_panel_alpha(t0 + 40 * kMs) > 0.0f && near(appear_contents_alpha(t0 + 40 * kMs), 0.0f),
          "the contents stay clear the theme's delay behind the panel");
    const uint64_t whole = t0 + (uint64_t)th.open_ms * kMs;
    check(near(appear_panel_alpha(whole), 1.0f) && near(appear_contents_alpha(whole), 1.0f) &&
              near(appear_scale(whole), 1.0f) && appear_shown(whole) && !appear_shown(whole - kMs),
          "the panel and its contents are whole at the theme's open time and not before");
    const uint64_t settled = t0 + (uint64_t)th.backdrop_ms * kMs;
    check(appear_moving(settled - kMs) && !appear_moving(settled) && near(appear_backdrop(settled), 1.0f) &&
              th.backdrop_ms <= 200.0f && th.open_ms <= 200.0f,
          "the backdrop is the last to settle, at the theme's backdrop time, all of it within 200 ms");

    const uint64_t t1 = 9000 * kMs;
    appear_close(th, t1);
    const float half = appear_panel_alpha(t1 + 60 * kMs);
    check(appear_closing(t1) && appear_closing(t1 + 60 * kMs) && half > 0.0f && half < 1.0f &&
              appear_scale(t1 + 60 * kMs) < 1.0f && appear_scale(t1 + 60 * kMs) > th.close_scale,
          "as it closes it fades and shrinks toward the theme's close scale");
    const uint64_t gone = t1 + (uint64_t)th.close_ms * kMs;
    check(!appear_closing(gone) && !appear_moving(gone) && near(appear_panel_alpha(gone), 0.0f) &&
              near(appear_backdrop(gone), 0.0f) && appear_closing(gone - kMs),
          "it's gone at the theme's close time and not before");
    appear_close(th, t1);
    const float shrunk = appear_scale(t1 + 60 * kMs);
    appear_open(th, t1 + 60 * kMs);
    check(near(appear_panel_alpha(t1 + 60 * kMs), half) && near(appear_scale(t1 + 60 * kMs), shrunk) &&
              !appear_closing(t1 + 60 * kMs),
          "opened again mid-way through a close, it starts from where it was, its size too");
    appear_close(th, t1 + 1000 * kMs);
    appear_open(th, t1 + 2000 * kMs);
    check(near(appear_scale(t1 + 2000 * kMs), th.open_scale),
          "opened again once a close is over, it grows from the theme's open scale, not the close's");

    OverlayTheme backwards = th;
    backwards.open_ms = -50.0f;
    const uint64_t t3 = 15000 * kMs;
    appear_close(th, t3 - 1000 * kMs);
    appear_open(backwards, t3);
    check(appear_shown(t3) && near(appear_panel_alpha(t3), 1.0f) && near(appear_contents_alpha(t3), 1.0f),
          "a game's theme with a negative open time opens whole at once, its contents' delay held to none");

    appear_motion(false);
    const uint64_t t2 = 20000 * kMs;
    appear_open(th, t2);
    check(near(appear_panel_alpha(t2), 1.0f) && near(appear_contents_alpha(t2), 1.0f) && near(appear_scale(t2), 1.0f) &&
              near(appear_backdrop(t2), 1.0f) && appear_shown(t2) && !appear_moving(t2),
          "with motion off it's whole as it opens");
    appear_close(th, t2 + kMs);
    check(!appear_closing(t2 + kMs) && !appear_moving(t2 + kMs) && near(appear_panel_alpha(t2 + kMs), 0.0f),
          "and gone as it closes");
}

struct Vtx {
    ImVec2 pos;
    ImU32 col;
};

std::vector<Vtx> copy(const ImDrawList* l) {
    std::vector<Vtx> out;
    for (const ImDrawVert& v : l->VtxBuffer) out.push_back({v.pos, v.col});
    return out;
}

int alpha(ImU32 c) { return (int)(c >> IM_COL32_A_SHIFT & 0xFF); }

void transform() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000.0f, 800.0f);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;
    // A window's first frame is laid out and not drawn, so the second is the one looked at.
    ImDrawList* panel = nullptr;
    int panel_vtx = 0;
    for (int frame = 0; frame < 2; frame++) {
        if (frame) ImGui::Render();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(300.0f, 200.0f));
        ImGui::Begin("panel");
        panel = ImGui::GetWindowDrawList();
        panel_vtx = panel->VtxBuffer.Size;
        ImGui::TextUnformatted("contents");
        ImGui::End();
    }
    ImDrawList* backdrop = ImGui::GetBackgroundDrawList();
    backdrop->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(10, 20, 30, 140));
    ImGui::Render();
    ImDrawData* d = ImGui::GetDrawData();
    const std::vector<Vtx> before = copy(panel), behind = copy(backdrop);
    const ImVec4 clip = panel->CmdBuffer[0].ClipRect;
    const float cx = 500.0f, cy = 400.0f, s = 0.5f;
    appear_apply(d, backdrop, panel, panel_vtx, cx, cy, s, 0.5f, 0.25f);
    bool moved = panel_vtx > 0 && panel->VtxBuffer.Size > panel_vtx, panel_faded = true, contents_faded = true;
    for (int i = 0; i < panel->VtxBuffer.Size; i++) {
        const ImDrawVert& v = panel->VtxBuffer[i];
        moved = moved && near(v.pos.x, cx + (before[i].pos.x - cx) * s, 0.01f) &&
                near(v.pos.y, cy + (before[i].pos.y - cy) * s, 0.01f);
        const int want = (int)std::lround(alpha(before[i].col) * (i < panel_vtx ? 0.5f : 0.25f));
        (i < panel_vtx ? panel_faded : contents_faded) &= std::abs(alpha(v.col) - want) <= 1;
    }
    check(moved, "every vertex of the panel's list is scaled about the centre");
    check(panel_faded && contents_faded, "the panel's own vertices fade by its alpha and the contents' by theirs");
    const ImVec4 now = panel->CmdBuffer[0].ClipRect;
    check(near(now.x, cx + (clip.x - cx) * s, 0.01f) && near(now.y, cy + (clip.y - cy) * s, 0.01f) &&
              near(now.z, cx + (clip.z - cx) * s, 0.01f) && near(now.w, cy + (clip.w - cy) * s, 0.01f),
          "its clip rects are scaled about the centre too");
    bool kept = true;
    for (int i = 0; i < backdrop->VtxBuffer.Size; i++)
        kept = kept && backdrop->VtxBuffer[i].pos.x == behind[i].pos.x && backdrop->VtxBuffer[i].col == behind[i].col;
    check(kept && !behind.empty(), "the backdrop's list is left as it was drawn");
    ImGui::DestroyContext();
}

}  // namespace

int main() {
    easings();
    tweens();
    opens_and_closes();
    transform();
    printf("test_overlay_motion: %s\n", g_failed ? "FAILED" : "passed");
    return g_failed ? 1 : 0;
}
