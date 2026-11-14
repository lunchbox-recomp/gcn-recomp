// ---- The menu's theme over a window from SDL's dummy video driver, as the window's height
// changes: the menu's style and the notices' are built again from the theme given before init,
// at the new height, and one given after init, which is refused, has no part in it. Read in by
// tools/test_ports.cpp after the frontend, with GCN_OVERLAY.
#include "test_ports_support.h"

namespace {

// Whether the current context's style is at `scale` with `rgba`'s panel.
bool styled(float scale, uint32_t rgba) {
    const ImGuiStyle& s = ImGui::GetStyle();
    const ImVec4 p = s.Colors[ImGuiCol_WindowBg];
    const auto near = [](float v, uint32_t c) { return std::abs(v - (float)(c & 0xFF) / 255.0f) < 1e-3f; };
    return std::abs(s.FontScaleMain - scale) < 1e-6f && near(p.x, rgba >> 24) && near(p.y, rgba >> 16) &&
           near(p.z, rgba >> 8) && near(p.w, rgba);
}

void overlay_theme_fit(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    overlay_test_reset_pages();
    OverlayTheme theme = overlay_theme_default();
    theme.panel = 0x102030F0;
    overlay_set_theme(theme);
    OverlayPicture picture;
    picture.path = dir + "/theme-settings.txt";
    picture.apply = record_apply;
    overlay_init(window, "Test title", picture);
    const std::string said = dir + "/theme-stderr.txt";
    capture(said, [] {
        OverlayTheme late = overlay_theme_default();
        late.panel = 0xFF0000FF;
        overlay_set_theme(late);
    });
    SDL_SetWindowSize(window, w, 720);
    Guest guest;
    overlay_pads_notice("A notice laid out at the new height");
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    const float scale = 720.0f / 1080.0f;
    const bool menu = styled(scale, theme.panel);
    ImGuiContext* was = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(overlay_test_notices_context());
    const bool notices = styled(scale, theme.panel);
    ImGui::SetCurrentContext(was);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    g_host_ns += 3000000000;  // past the close's fade and the notice's time
    host_frame(guest);
    check(lines_with(said, "[overlay] the theme is fixed after init, so this one is refused\n") == 1 && menu,
          "at a new height the menu's style is built again from the theme given before init, not the refused one");
    check(notices, "and so is the notices' style, at the frame that lays them out");
    close_window(window);
}

// The panel's own edge and the rule Dear ImGui draws under its title take the Border colour as
// Begin draws them, which the menu gives the theme's panel edge there, under an item's edge. So
// the panel, open and still, has the panel edge's colour among its own vertices and never the
// item edge's, which its items draw after it.
void overlay_panel_edge(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = dir + "/edge-settings.txt";
    picture.apply = record_apply;
    overlay_init(window, "Test title", picture);
    const OverlayTheme t = overlay_theme_default();
    const auto u32 = [](uint32_t c) { return IM_COL32(c >> 24, c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF); };
    Guest guest;
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    g_host_ns += 1000000000;  // past the open's motion
    host_frame(guest);
    const ImGuiWindow* panel = ImGui::FindWindowByName("Test title");
    int last_edge = -1, first_item = -1;
    for (int i = 0; panel && i < panel->DrawList->VtxBuffer.Size; i++) {
        const ImU32 col = panel->DrawList->VtxBuffer[i].col;
        if (col == u32(t.panel_edge)) last_edge = i;
        if (col == u32(t.border) && first_item < 0) first_item = i;
    }
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    g_host_ns += 1000000000;
    host_frame(guest);
    check(last_edge >= 0 && first_item > last_edge && (t.panel_edge & 0xFF) < (t.border & 0xFF),
          "the panel's edge and its title's rule are drawn in the panel edge, fainter than an item's edge");
    close_window(window);
}

}  // namespace
