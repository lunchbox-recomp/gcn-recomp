// ---- The menu's panel height, read in its header's y over a window from SDL's dummy video
// driver. The panel is never shorter than a page has had it since the menu opened, so the
// header stays across pages. A view is left out of that, and the menu opening or the window's
// height changing starts it again from the page showing, not from the frame before, which Dear
// ImGui sized the panel by. Read in by tools/test_ports.cpp after the frontend, with GCN_OVERLAY.
#include "test_ports_support.h"
#include "overlay/tabs.h"  // where the header laid out each pill

namespace {

// A page as tall as ten buttons, taller than the others.
void tall_page(void*) {
    for (int i = 0; i < 10; i++) ImGui::Button(("Row " + std::to_string(i)).c_str());
}

// The header's y: the middle of its first pill, two frames on, as Dear ImGui sizes a window by
// what it held at the frame before. -1 with no pill.
float header_at(Guest& guest) {
    host_frame(guest);
    host_frame(guest);
    float x = 0.0f, y = 0.0f;
    return tabs_test_pill(0, x, y) ? y : -1.0f;
}

// The header stays where it is across pages: the panel is never shorter than it's been since
// the menu opened, so only a page taller than any yet moves it, up, and once each has shown it's
// at the same height on every page. Opened again, the panel starts from the page it opens on.
void header_still(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    add_extra_page();
    overlay_add_page("Tall", tall_page, nullptr);
    OverlayPicture picture;
    picture.path = dir + "/header-settings.txt";
    picture.apply = record_apply;
    picture.now.menu_motion = false;
    overlay_init(window, "Test title", picture);
    Guest guest;
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    // The header's y with page `page` showing.
    const auto header_y = [&](int page) {
        overlay_test_pick_page(page);
        return header_at(guest);
    };
    const int extra = page_at("Extra"), tall = page_at("Tall");
    const float display = header_y(0), controllers = header_y(1), tallest = header_y(tall), shorter = header_y(extra);
    bool same = true;
    for (int page : {0, 1, extra, tall}) same = same && header_y(page) == tallest;
    check(display >= 0.0f && controllers <= display && tallest < controllers && shorter == tallest && same,
          "the header moves only up, for a page taller than any yet, and once each page has shown it's at one y on all");
    header_y(extra);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    host_frame(guest);
    float x = 0.0f, y = 0.0f;
    check(overlay_open() && tabs_test_pill(0, x, y) && y > tallest,
          "opened again on a shorter page, the panel starts from that page's height");
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    close_window(window);
}

// The window made smaller while the menu is open, after a taller page, and the menu opened on
// a page other than the one its last frame before closing showed: each time the header is
// where closing the menu and opening it again on that page puts it.
void header_reset(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    add_extra_page();
    overlay_add_page("Tall", tall_page, nullptr);
    OverlayPicture picture;
    picture.path = dir + "/header-reset-settings.txt";
    picture.apply = record_apply;
    picture.now.menu_motion = false;
    overlay_init(window, "Test title", picture);
    Guest guest;
    const int extra = page_at("Extra"), tall = page_at("Tall");
    // The header's y once the menu is closed and opened again on the page showing.
    const auto reopened = [&] {
        tap(guest, SDL_SCANCODE_F1, SDLK_F1);
        tap(guest, SDL_SCANCODE_F1, SDLK_F1);
        return header_at(guest);
    };
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    overlay_test_pick_page(tall);
    header_at(guest);
    overlay_test_pick_page(extra);
    header_at(guest);
    SDL_SetWindowSize(window, 640, 480);
    const float shrunk = header_at(guest);
    check(shrunk >= 0.0f && shrunk == reopened(),
          "the window made smaller while the menu is open, after a taller page, puts the header where opening "
          "the menu again does");
    SDL_SetWindowSize(window, 1280, 960);
    overlay_test_pick_page(tall);
    header_at(guest);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    overlay_test_pick_page(extra);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    const float opened = header_at(guest);
    check(opened >= 0.0f && opened == reopened(),
          "closed on a taller page and opened on a shorter one, the panel starts from the shorter page");
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    close_window(window);
}

// The Buttons view, taller than the Controllers page, grows the panel, and backing out of it
// puts the header back where the page had it. Closed from the view, whose fade still lays the
// view out, the menu opens again on the page in a panel the page's size.
void header_view(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = dir + "/header-view-settings.txt";
    picture.apply = record_apply;
    overlay_init(window, "Test title", picture);
    gamepads_init();
    Virtual pad{"virtual pad header view"};
    attach(pad);
    Guest guest;
    // The header's y past any motion.
    const auto settle = [&] {
        g_host_ns += 1000 * kMs;
        return header_at(guest);
    };
    g_host_ns += 10000 * kMs;  // past every tween an earlier check left
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    overlay_test_pick_page(page_at("Controllers"));
    const float page = settle();
    const bool shown = overlay_bindings_open(pad.id);
    const float view = settle();
    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    const float back = settle();
    check(page >= 0.0f && shown && view < page && !overlay_bindings_shown() && back == page,
          "the Buttons view grows the panel past the Controllers page's, and B out of it puts the header back");
    overlay_bindings_open(pad.id);
    settle();
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    settle();
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    const float reopened = settle();
    check(overlay_open() && !overlay_bindings_shown() && reopened == page,
          "closed from the Buttons view and opened again, the Controllers page is in a panel its own size");
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    settle();
    unplug({&pad});
    gamepads_shutdown();
    close_window(window);
}

}  // namespace
