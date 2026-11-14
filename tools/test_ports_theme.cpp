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

}  // namespace
