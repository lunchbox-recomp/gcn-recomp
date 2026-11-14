// ---- The notices as toasts over a window from SDL's dummy video driver, with the menu closed, on
// a host clock moved by hand (g_host_ns). With menu_motion on, a notice's pill is clear 16 px
// below its place as it's queued, rises and fades in to whole at 200 ms, and fades out from
// 1820 ms; with it off, and under --dump-output, it's whole in its place from its first frame.
// Either way it goes at exactly 2 s, and as it goes the one above moves down into its place, over
// 160 ms with motion and at once without. Each host frame with one showing lays out one frame of
// notices and none of the menu, and once the last has gone none is laid out. Read in by
// tools/test_ports.cpp after the frontend, with GCN_OVERLAY.
#include "test_ports_support.h"

namespace {

constexpr uint64_t kToastMsNs = 1000000;  // host_ns in a millisecond

// The top of the pill in place `slot` of the stack, 0 the lowest, at rest in `window`: the theme's
// margin above its bottom edge, the pills each item_min_height tall and item_spacing apart, all
// scaled by its height over 1080.
float rest_top(SDL_Window* window, int slot) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    const OverlayTheme t = overlay_theme_default();
    const float s = (float)h / 1080.0f, pill = t.item_min_height * s;
    return (float)h - t.margin * s - pill - (float)slot * (pill + t.item_spacing * s);
}

// The alpha of the first vertex the notices' window drew, the lowest pill's fill, in their last
// frame; -1 with none.
int first_alpha() {
    ImGuiContext* menu = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(overlay_test_notices_context());
    const ImGuiWindow* w = ImGui::FindWindowByName("##pads notices");
    const int a = w && w->DrawList->VtxBuffer.Size ? (int)(w->DrawList->VtxBuffer[0].col >> IM_COL32_A_SHIFT & 0xFF) : -1;
    ImGui::SetCurrentContext(menu);
    return a;
}

bool near_px(float a, float b) { return std::fabs(a - b) < 0.01f; }

// The menu set up over `window` with menu_motion `motion`, under --dump-output or not, and a host
// clock past anything an earlier check left showing.
void toasts_init(SDL_Window* window, const std::string& dir, bool motion, bool dump_output, Guest& guest) {
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = dir + "/toasts-settings.txt";
    picture.apply = record_apply;
    picture.now.menu_motion = motion;
    gx::g_dump_output = dump_output;
    overlay_init(window, "Test title", picture);
    gx::g_dump_output = false;
    g_host_ns += 10000 * kToastMsNs;
    host_frame(guest);
}

// One notice through its life, then two, the older going first, as `what` describes: the menu
// set up with `motion` and `dump_output`, and `moves` whether a toast should move.
void toast_life(SDL_Window* window, const std::string& dir, bool motion, bool dump_output, const char* what) {
    Guest guest;
    toasts_init(window, dir, motion, dump_output, guest);
    const bool moves = motion && !dump_output;
    int width = 0, height = 0;
    SDL_GetWindowSize(window, &width, &height);
    const float s = (float)height / 1080.0f;  // the theme's sizes in the window's pixels
    const long menu_frames = overlay_menu_frames(), notice_frames = overlay_notice_frames();
    const uint64_t queued = g_host_ns;
    overlay_pads_notice("Pad one", " connected as player 1");
    const uint64_t kAt[] = {0, 100, 200, 1820, 1910};
    const float kAlpha[] = {0.0f, 0.875f, 1.0f, 1.0f, 0.75f};
    const float kBelow[] = {16.0f, 2.0f, 0.0f, 0.0f, 0.0f};
    bool aged = true, drawn = true;
    for (size_t i = 0; i < std::size(kAt); i++) {
        g_host_ns = queued + kAt[i] * kToastMsNs;
        host_frame(guest);
        const std::vector<OverlayTestToast>& laid = overlay_test_toasts();
        const float alpha = moves ? kAlpha[i] : 1.0f, below = moves ? kBelow[i] * s : 0.0f;
        aged = aged && laid.size() == 1 && std::fabs(laid[0].alpha - alpha) < 1e-4f &&
               near_px(laid[0].top, rest_top(window, 0) + below);
        // A clear pill draws nothing at all, as Dear ImGui skips what has no alpha.
        const int fill = (int)std::lround((float)(overlay_theme_default().panel & 0xFF) * alpha);
        drawn = drawn && first_alpha() == (fill ? fill : -1);
    }
    g_host_ns = queued + 2000 * kToastMsNs - 1;
    host_frame(guest);
    const bool kept = overlay_notices().size() == 1 && overlay_test_toasts().size() == 1;
    const long showing = overlay_notice_frames() - notice_frames;
    g_host_ns = queued + 2000 * kToastMsNs;
    host_frame(guest);
    const bool gone = overlay_notices().empty() && overlay_test_toasts().empty() &&
                      overlay_notice_frames() - notice_frames == showing;
    char said[256];
    snprintf(said, sizeof(said), "%s: a toast's alpha and place at 0, 100, 200, 1820 and 1910 ms are %s", what,
             moves ? "0 16 px below, 0.875 2 px below, then whole in place to 1820 ms, 0.75 at 1910 ms"
                   : "whole in its place");
    check(aged, said);
    snprintf(said, sizeof(said), "%s: its fill is drawn at the panel's alpha times the toast's, and not at all when clear", what);
    check(drawn, said);
    snprintf(said, sizeof(said), "%s: it's there a nanosecond before 2 s and gone at exactly 2 s, with no frame after", what);
    check(kept && gone, said);
    snprintf(said, sizeof(said), "%s: each of the %ld host frames it showed in laid out one frame of notices and none of the menu",
             what, showing);
    check(showing == (long)std::size(kAt) + 1 && overlay_menu_frames() == menu_frames, said);

    // Two, the second queued 500 ms after the first: as the first goes, the second moves down.
    const uint64_t first = g_host_ns;
    overlay_pads_notice("Pad one", " connected as player 1");
    g_host_ns = first + 500 * kToastMsNs;
    overlay_pads_notice("Pad two", " connected as player 2");
    g_host_ns = first + 700 * kToastMsNs;  // the second whole in its place
    host_frame(guest);
    const std::vector<OverlayTestToast> both = overlay_test_toasts();
    const bool stacked = both.size() == 2 && near_px(both[0].top, rest_top(window, 0)) &&
                         near_px(both[1].top, rest_top(window, 1));
    std::vector<float> tops;
    for (uint64_t ms : {2000, 2080, 2160}) {
        g_host_ns = first + ms * kToastMsNs;
        host_frame(guest);
        const std::vector<OverlayTestToast>& laid = overlay_test_toasts();
        tops.push_back(laid.size() == 1 ? laid[0].top : -1.0f);
    }
    const float up = rest_top(window, 1), down = rest_top(window, 0);
    const bool reflowed = moves ? near_px(tops[0], up) && near_px(tops[1], up + (down - up) * 0.875f) && near_px(tops[2], down)
                                : near_px(tops[0], down) && near_px(tops[1], down) && near_px(tops[2], down);
    snprintf(said, sizeof(said), "%s: two stack, the older lowest", what);
    check(stacked, said);
    snprintf(said, sizeof(said), "%s: as the older goes at 2 s the other moves down into its place %s", what,
             moves ? "over 160 ms, out-cubic" : "at once");
    check(reflowed, said);
    g_host_ns = first + 2500 * kToastMsNs;
    host_frame(guest);
    const long none = overlay_notice_frames();
    for (int i = 0; i < 600; i++) host_frame(guest);
    snprintf(said, sizeof(said), "%s: once the last has gone 600 closed frames lay out no notice and no menu", what);
    check(overlay_notices().empty() && overlay_notice_frames() == none && overlay_menu_frames() == menu_frames, said);
}

void toasts(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    toast_life(window, dir, true, false, "with menu_motion on");
    toast_life(window, dir, false, false, "with menu_motion off");
    toast_life(window, dir, true, true, "under --dump-output");
    close_window(window);
}

}  // namespace
