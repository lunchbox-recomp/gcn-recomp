// ---- The menu's motion over a window from SDL's dummy video driver, on a host clock moved by
// hand (g_host_ns): opening still asks for the pause at once, the paused wait is 8 ms while it
// moves and 16 ms once it's still, and an `[overlay] shown after N ms` line comes once it's
// whole, while the backdrop dims the game from clear. As it closes the guest goes on at once
// while the menu fades with no input of its own, from a key or a gamepad, and once it's gone 600 closed frames lay out no menu frame and no notice. With menu_motion off,
// and under --dump-output, nothing moves. Read in by tools/test_ports.cpp after the frontend,
// with GCN_OVERLAY.
#include "test_ports_support.h"

namespace {

constexpr uint64_t kMs = 1000000;  // host_ns in a millisecond

bool enter_reaches_game() {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = g_window_id;
    e.key.scancode = SDL_SCANCODE_RETURN;
    e.key.key = SDLK_RETURN;
    e.key.down = true;
    return !overlay_event(e);
}

// The backdrop's alpha in the menu's frame last laid out, 0 with none drawn.
int backdrop_alpha() {
    const ImDrawList* list = ImGui::GetBackgroundDrawList();
    return list->VtxBuffer.Size ? (int)(list->VtxBuffer[0].col >> IM_COL32_A_SHIFT & 0xFF) : 0;
}

// The alpha of the menu's panel, its window's first vertex, in the frame last laid out. -1 with
// the window not drawn.
int panel_alpha() {
    const ImGuiWindow* w = ImGui::FindWindowByName("Test title");
    const ImDrawData* d = ImGui::GetDrawData();
    if (!w || !d || !d->CmdLists.contains(w->DrawList) || !w->DrawList->VtxBuffer.Size) return -1;
    return (int)(w->DrawList->VtxBuffer[0].col >> IM_COL32_A_SHIFT & 0xFF);
}

// The menu opened with F1 and closed again, with `motion` and under --dump-output or not: whether
// it moved as it opened and whether its menu was still due as it closed.
struct Still {
    bool opened_moving, closed_due;
};
Still still_with(SDL_Window* window, const std::string& dir, bool motion, bool dump_output) {
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = dir + "/motion-settings.txt";
    picture.apply = record_apply;
    picture.now.menu_motion = motion;
    gx::g_dump_output = dump_output;
    overlay_init(window, "Test title", picture);
    gx::g_dump_output = false;
    Guest guest;
    Still s{};
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    s.opened_moving = overlay_animating();
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    s.closed_due = overlay_menu_due();
    return s;
}

void overlay_motion(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = dir + "/motion-settings.txt";
    picture.apply = record_apply;
    overlay_init(window, "Test title", picture);
    const OverlayTheme theme = overlay_theme_default();
    gamepads_init();
    Virtual pad{"virtual pad motion"};
    attach(pad);
    Guest guest;
    host_frame(guest);
    g_host_ns += 10000 * kMs;  // past every tween an earlier check left, whose clock stood still
    const uint64_t opened = g_host_ns;

    const std::string said = dir + "/motion-stderr.txt";
    bool asked = false, moving = false, fast = false, early = false;
    int dim_first = -1, panel_early = -1, panel_whole = -1;
    capture(said, [&] {
        key(guest, SDL_SCANCODE_F1, SDLK_F1, true);
        asked = overlay_open() && pause_requested();
        host_frame(guest);
        moving = overlay_animating() && paused();
        dim_first = backdrop_alpha();
        g_host_ns = opened + 20 * kMs;
        host_frame(guest);
        panel_early = panel_alpha();
        fast = batch_wait_ms() == 8;
        g_host_ns = opened + (uint64_t)theme.open_ms * kMs - kMs;
        host_frame(guest);
        early = lines_with(said, "shown after") == 0;
        g_host_ns = opened + (uint64_t)theme.open_ms * kMs;
        host_frame(guest);
        panel_whole = panel_alpha();
        host_frame(guest);
    });
    key(guest, SDL_SCANCODE_F1, SDLK_F1, false);
    check(asked, "with the menu moving, F1 opens it and asks for the pause at once");
    check(moving && fast, "while it moves over the held game, the paused wait is 8 ms");
    char line[96];
    snprintf(line, sizeof(line), "[overlay] shown after %d ms\n", (int)theme.open_ms);
    check(panel_early > 0 && panel_early < panel_whole && panel_whole == (int)(theme.panel & 0xFF),
          "the panel fades in, whole at the theme's alpha once it's shown");
    check(early && lines_with(said, line) == 1, "it says once that it's shown when the panel is whole, not a ms before");
    g_host_ns = opened + (uint64_t)theme.backdrop_ms * kMs;
    host_frame(guest);
    check(!overlay_animating() && batch_wait_ms() == 16, "once the backdrop has settled nothing moves, and the wait is 16 ms");
    check(dim_first == 0 && backdrop_alpha() == (int)(theme.backdrop & 0xFF),
          "the backdrop dims the game from clear to the theme's alpha as it opens");

    const long menu_frames = overlay_menu_frames();
    key(guest, SDL_SCANCODE_DOWN, SDLK_DOWN, true);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    const bool held_down = ImGui::IsKeyDown(ImGuiKey_DownArrow);
    key(guest, SDL_SCANCODE_DOWN, SDLK_DOWN, false);
    const uint64_t closed = g_host_ns;
    set_button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    host_frame(guest);
    const bool pad_down = ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown);
    set_button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    check(!held_down, "a key held as it closes is let go for the menu, so nothing moves its focus as it fades");
    check(!pad_down, "a gamepad's press as it fades doesn't reach the menu");
    check(!overlay_open() && !pause_requested() && !paused() && guest.passed == guest.asked,
          "F1 closes it and the guest goes on at once, the menu still fading");
    check(overlay_menu_due() && overlay_animating() && overlay_menu_frames() > menu_frames && enter_reaches_game(),
          "as it fades the menu is still due and laid out, and a key reaches the game, not the menu");
    g_host_ns = closed + (uint64_t)theme.close_ms * kMs - kMs;
    host_frame(guest);
    const bool fading = overlay_menu_due();
    g_host_ns = closed + (uint64_t)theme.close_ms * kMs;
    check(fading && !overlay_menu_due() && !overlay_animating(), "it's gone at the theme's close time and not before");
    const long gone = overlay_menu_frames(), notices = overlay_notice_frames();
    const std::string closed_said = dir + "/motion-closed-stderr.txt";
    capture(closed_said, [&] {
        for (int i = 0; i < 600; i++) host_frame(guest);
    });
    check(overlay_menu_frames() == gone && overlay_notice_frames() == notices && read_file(closed_said).empty(),
          "600 frames with it closed and no notice lay out no menu frame and no notice, and say nothing");
    unplug({&pad});
    gamepads_shutdown();

    const Still off = still_with(window, dir, false, false);
    check(!off.opened_moving && !off.closed_due, "with menu_motion off it opens whole and closes at once");
    const Still dumping = still_with(window, dir, true, true);
    check(!dumping.opened_moving && !dumping.closed_due, "under --dump-output it doesn't move either");
    close_window(window);
}

}  // namespace
