// ---- --init-order: a theme, a page and a telemetry default given before main, from the static
// initializer of a file linked ahead of the overlay's and telemetry.cpp
// (tools/test_overlay_early.cpp), as a game's are. The storage of each, built after it, must not
// take them back: all three are there at main, and the theme and the page at overlay_init and at
// the menu's first frame. Read in by tools/test_ports.cpp after the frontend, with GCN_OVERLAY.
#include "test_ports_support.h"

extern const uint32_t kEarlyPanel;  // tools/test_overlay_early.cpp's
extern const float kEarlyGain;
extern int g_early_draws;

namespace {

// Whether the current context's panel colour is `rgba`'s.
bool panel_is(uint32_t rgba) {
    const ImVec4 p = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const auto near = [](float v, uint32_t c) { return std::abs(v - (float)(c & 0xFF) / 255.0f) < 1e-3f; };
    return near(p.x, rgba >> 24) && near(p.y, rgba >> 16) && near(p.z, rgba >> 8) && near(p.w, rgba);
}

int run_init_order(const std::string& dir) {
    const std::vector<std::string> pages = {"Display", "Controllers", "Early"};
    check(overlay_test_pages() == pages, "a page added before main is the menu's third at main");
    check(theme_now().panel == kEarlyPanel, "a theme set before main is the menu's at main");
    check(telemetry::get("early_gain", 0.0f) == kEarlyGain, "a telemetry default set before main is there at main");
    if (!sdl_up()) return 1;
    no_controls(dir);
    power_on(1);
    gamepads_init();
    SDL_Window* window = open_window();
    if (!window) return finish("--init-order");
    OverlayPicture picture;
    picture.path = dir + "/settings.txt";
    picture.apply = record_apply;
    setenv("GCN_OVERLAY_PAGE", "2", 1);
    overlay_init(window, "Test title", picture);
    check(panel_is(kEarlyPanel), "the theme set before main colours the menu's panel at overlay_init");
    Guest guest;
    key(guest, SDL_SCANCODE_F1, SDLK_F1, true);
    check(overlay_open() && overlay_menu_frames() == 1 && overlay_page() == 2 && g_early_draws == 1 &&
              overlay_test_pages() == pages,
          "GCN_OVERLAY_PAGE=2 opens the menu on the page added before main, drawn at its first frame");
    check(panel_is(kEarlyPanel), "and the menu's first frame has the theme set before main");
    key(guest, SDL_SCANCODE_F1, SDLK_F1, false);
    close_window(window);
    return finish("--init-order");
}

}  // namespace
