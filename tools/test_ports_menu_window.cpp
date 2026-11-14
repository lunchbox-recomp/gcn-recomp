// ---- The overlay's menu over a window from SDL's dummy video driver: its pages, Restart game
// and Quit game, the Display page's aspect and what a gamepad held or plugged in as it opens
// presses. The window has no GL context, so the menu is driven but not drawn
// (overlay_menu_frame). First an A and an Enter held as the menu opens press nothing. Then a
// gamepad alone goes through Restart and the keyboard alone through Quit. Neither ends this
// process: each stops pump_events, the frontend's loop, and --restart follows a restart from
// there. Between the two a Switch Pro pad asks and goes back with its faces by their labels, as
// the game reads them, so its east presses and its south goes back. The Controllers page's
// checks (tools/test_ports_controllers.cpp) then run over the same window. Read in by
// tools/test_ports.cpp after the frontend, with GCN_OVERLAY.
#include "test_ports_support.h"

namespace {

void overlay_buttons(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    OverlayPicture picture;
    picture.path = dir + "/buttons-settings.txt";
    picture.apply = record_apply;
    const int applies = g_applies;
    add_extra_page();
    const std::string page_errors = dir + "/page-errors.txt";
    capture(page_errors, [] {
        overlay_add_page("", draw_added_page, &g_page_draws);
        overlay_add_page("Extra", draw_added_page, &g_page_draws);
        overlay_add_page("Display", draw_added_page, &g_page_draws);
        overlay_add_page("Controllers", draw_added_page, &g_page_draws);
    });
    check(lines_with(page_errors, "[overlay] page title is empty\n") == 1 &&
              lines_with(page_errors, "[overlay] page title Extra is already in use\n") == 1 &&
              lines_with(page_errors, "[overlay] page title Display is already in use\n") == 1 &&
              lines_with(page_errors, "[overlay] page title Controllers is already in use\n") == 1,
          "empty and duplicate page titles each print one refusal");
    OverlayTheme theme = overlay_theme_default();
    theme.panel = 0x102030F0;
    overlay_set_theme(theme);
    overlay_init(window, "Test title", picture);
    const ImVec4 panel = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    check(std::abs(panel.x - 0x10 / 255.0f) < 1e-3f && std::abs(panel.y - 0x20 / 255.0f) < 1e-3f &&
              std::abs(panel.z - 0x30 / 255.0f) < 1e-3f && std::abs(panel.w - 0xF0 / 255.0f) < 1e-3f,
          "a theme set before init colours the menu's panel");
    capture(page_errors, [] {
        OverlayTheme late = overlay_theme_default();
        late.panel = 0xFF0000FF;
        overlay_set_theme(late);
    });
    check(lines_with(page_errors, "[overlay] the theme is fixed after init, so this one is refused\n") == 1 &&
              ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x == panel.x,
          "a theme set after init prints one refusal and changes nothing");
    check(ImGui::GetPlatformIO().Platform_OpenInShellFn == nullptr,
          "the menu leaves Dear ImGui's link opener disabled after init");
    check(!ImGui::GetIO().ConfigDebugHighlightIdConflictsShowItemPicker,
          "the menu leaves Dear ImGui's duplicate-ID item picker disabled after init");
    capture(page_errors, [] { overlay_add_page("Late", draw_added_page, &g_page_draws); });
    check(lines_with(page_errors, "[overlay] page registration is closed after init\n") == 1,
          "a page registered after init prints one refusal");
    gamepads_init();
    Virtual pad{"virtual pad buttons"};
    attach(pad);
    // A south and B east by its labels, as by position, where a Switch Pro has them the other
    // way round. Both are in throughout, so each press goes by its own pad's labels.
    Virtual nintendo{"virtual switch pro buttons"};
    nintendo.vendor = 0x057e;
    nintendo.product = 0x2009;
    attach(nintendo);
    Guest guest;
    host_frame(guest);

    // A face or a key held as the menu opens stays out of it until it's let go, though the
    // focus is where the menu left it, here on Restart game (below).
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    const TabAt display = find_tab("Display"), controllers = find_tab("Controllers"), extra = find_tab("Extra");
    const bool order = display.bar && controllers.bar == display.bar && extra.bar == display.bar &&
                       controllers.index > display.index && extra.index > controllers.index && display.bar->Tabs.Size == 3;
    pick_tab(guest, "Extra");
    check(order, "Controllers follows Display, the added page's tab follows them, and refused pages add no tabs");
    check(g_page_draws > 0, "selecting the added page calls its callback in the dummy window");
    pick_tab(guest, "Display");
    bottom_row(guest, pad);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    host_frame(guest);
    const bool held_a = waiting(OverlayAsk::none);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    host_frame(guest);
    const bool let_go = waiting(OverlayAsk::none);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(held_a && let_go && waiting(OverlayAsk::restart),
          "an A held as Back opens the menu presses nothing, nor does letting it go, and its next press does");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    key(guest, SDL_SCANCODE_RETURN, SDLK_RETURN, true);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    key(guest, SDL_SCANCODE_RETURN, SDLK_RETURN, true, true);
    host_frame(guest);
    const bool repeat_presses_nothing = waiting(OverlayAsk::none);
    key(guest, SDL_SCANCODE_RETURN, SDLK_RETURN, false);
    tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
    check(repeat_presses_nothing && waiting(OverlayAsk::restart),
          "a held Enter's repeat after F1 opens the menu presses nothing, and its next press does");
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    // A key let go while the menu was closed is up to it as it opens again, so its next press
    // reaches it: here Left, held as the menu closed, onto Restart game from Quit game.
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    tap(guest, SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    key(guest, SDL_SCANCODE_LEFT, SDLK_LEFT, true);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    key(guest, SDL_SCANCODE_LEFT, SDLK_LEFT, false);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    tap(guest, SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    tap(guest, SDL_SCANCODE_LEFT, SDLK_LEFT);
    tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
    check(waiting(OverlayAsk::restart), "a key let go while the menu was closed moves its focus the next time it's open");
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);

    // From the top of the page down past every row and left past every column is Restart game.
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    bottom_row(guest, pad);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(waiting(OverlayAsk::restart), "on a gamepad, Restart game is at the bottom of the page, and pressing it asks first");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(waiting(OverlayAsk::none) && host_frame(guest), "Cancel is focused as it asks, and pressing it goes back with nothing done");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool again = waiting(OverlayAsk::restart);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    check(again && waiting(OverlayAsk::none) && host_frame(guest),
          "Restart game is focused again after, and B goes back with nothing done, the overlay still open");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(waiting(OverlayAsk::restart), "Restart game is focused again after B too");
    // Dear ImGui sizes a window to what it held the frame before, so the question is laid out
    // where it stays a frame after it's asked, and a move from Cancel starts from there.
    host_frame(guest);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    const bool closed = !overlay_open() && !pause_requested() && !paused() && guest.passed == guest.asked;
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    check(closed && g_restart && !host_frame(guest),
          "confirming Restart closes the overlay and lets the guest go, then the frontend's loop stops to restart");
    g_restart = false;  // as the relaunched process starts

    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_BACK);
    bottom_row(guest, nintendo);
    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_EAST);
    SDL_Gamepad* switch_pro = SDL_GetGamepadFromID(nintendo.id);
    check(switch_pro && SDL_GetGamepadButtonLabel(switch_pro, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A &&
              waiting(OverlayAsk::restart),
          "on a Switch Pro pad, the button labeled A, east, presses Restart game");
    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_EAST);
    check(waiting(OverlayAsk::none) && host_frame(guest), "its A presses Cancel too, going back with nothing done");
    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_EAST);
    const bool asks_again = waiting(OverlayAsk::restart);
    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_SOUTH);
    check(asks_again && waiting(OverlayAsk::none) && host_frame(guest),
          "its B, south, goes back from the question with nothing done, the overlay still open");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool other_presses = waiting(OverlayAsk::restart);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    const bool open = waiting(OverlayAsk::none);
    check(other_presses && open, "the other pad's south is still its A and its east its B");
    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_SOUTH);
    check(open && !overlay_open() && !paused() && guest.passed == guest.asked,
          "the Switch Pro pad's B closes the overlay, and the guest goes on");

    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    bottom_row(guest);
    tap(guest, SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
    check(waiting(OverlayAsk::quit), "on the keyboard, Quit game is right of Restart game, and Enter on it asks first");
    tap(guest, SDL_SCANCODE_SPACE, SDLK_SPACE);
    check(waiting(OverlayAsk::none) && host_frame(guest), "Cancel is focused as it asks, and Space on it goes back with nothing done");
    tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
    const bool asks = waiting(OverlayAsk::quit);
    const bool stays = tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    check(asks && stays && waiting(OverlayAsk::none) && host_frame(guest),
          "Quit game is focused again after, and Escape goes back with nothing done, without quitting");
    tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
    host_frame(guest);  // the question laid out where it stays, as above
    tap(guest, SDL_SCANCODE_RIGHT, SDLK_RIGHT);
    const bool running = tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
    check(!running && !g_restart && !overlay_open() && !pause_requested(),
          "confirming Quit closes the overlay, and the frontend's loop stops as for a quit, not a restart");
    check(g_applies == applies, "no press on the way to either button changed the picture");

    // Up from Restart game past the scale and Fullscreen is the aspect's row, and the second in
    // it is 16:9. With the aspect held, as --eye and GCN_THEATER_STEREO hold it, the row is
    // there but can't be picked, so the same keys pick nothing.
    const auto pick_16_9 = [&](bool held) {
        OverlayPicture p;
        p.path = dir + (held ? "/held-settings.txt" : "/free-settings.txt");
        p.apply = record_apply;
        p.aspect_held = held;
        display_init(p);
        const int before = g_applies;
        tap(guest, SDL_SCANCODE_F1, SDLK_F1);
        bottom_row(guest);
        for (int i = 0; i < 3; i++) tap(guest, SDL_SCANCODE_UP, SDLK_UP);
        for (int i = 0; i < 5; i++) tap(guest, SDL_SCANCODE_LEFT, SDLK_LEFT);
        tap(guest, SDL_SCANCODE_RIGHT, SDLK_RIGHT);
        tap(guest, SDL_SCANCODE_SPACE, SDLK_SPACE);
        tap(guest, SDL_SCANCODE_F1, SDLK_F1);
        return g_applies == before + 1 && g_applied.aspect == "16:9";
    };
    check(pick_16_9(false), "up from Restart game past the scale and Fullscreen, the keys pick the Display page's 16:9");
    check(!pick_16_9(true), "with the aspect held, the same keys pick nothing");

    // The frontend's apply_picture, as the page calls it: the aspect and the scale reach the
    // renderer, and with the aspect held only the scale does.
    g_window = window;
    Settings s;
    s.aspect = "16:9";
    s.scale = 4;
    apply_picture(s);
    const bool applied = std::abs(g_output_aspect - 16.0f / 9.0f) < 1e-4f && g_internal_scale == 4 && !g_aspect_window;
    g_aspect_held = true;
    s.aspect = "21:9";
    s.scale = 2;
    apply_picture(s);
    check(applied && std::abs(g_output_aspect - 16.0f / 9.0f) < 1e-4f && g_internal_scale == 2,
          "the frontend gives the renderer the page's aspect and scale, and with the aspect held the scale alone");
    g_aspect_held = false;
    g_window = nullptr;

    // A gamepad plugged in while the menu is open, holding A, has the A kept from the menu as one
    // held at the open is. A gamepad already in keeps its own latch: an A it held through the
    // open still presses nothing, and a D-pad it pressed since stays down for the menu. The
    // focus is on Restart game first, the bottom row, where down moves nothing.
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    bottom_row(guest, pad);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    set_button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    host_frame(guest);
    const bool dpad_down = ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown);
    const bool held_a_out = waiting(OverlayAsk::none);
    Virtual late{"late pad buttons"};
    attach(late);
    // Opened here, so that its A is down before the frontend hears of it.
    SDL_Joystick* late_joystick = SDL_OpenJoystick(late.id);
    SDL_SetJoystickVirtualButton(late_joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    host_frame(guest);
    const bool dpad_kept = ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown);
    const bool arrived = waiting(OverlayAsk::none);
    set_button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    SDL_SetJoystickVirtualButton(late_joystick, SDL_GAMEPAD_BUTTON_SOUTH, false);
    host_frame(guest);
    const bool late_let_go = waiting(OverlayAsk::none);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    host_frame(guest);
    const bool held_let_go = waiting(OverlayAsk::none);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool held_presses = waiting(OverlayAsk::restart);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    tap(guest, late, SDL_GAMEPAD_BUTTON_SOUTH);
    check(dpad_down && dpad_kept, "a gamepad plugged in while the menu is open leaves another's held D-pad down for it");
    check(held_a_out && arrived && held_let_go && held_presses,
          "an A held through the open still presses nothing as another gamepad comes, until it's let go and pressed");
    check(arrived && late_let_go && waiting(OverlayAsk::restart),
          "an A the new gamepad holds as it comes presses nothing, nor does letting it go, and its next press does");
    tap(guest, late, SDL_GAMEPAD_BUTTON_EAST);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    SDL_CloseJoystick(late_joystick);
    detach(late);
    host_frame(guest);

    // A device controls.txt binds is the file's alone, so its Back opens nothing.
    load_controls(dir + "/buttons-controls.txt", "A = \"virtual pad bound overlay\" button 0\n");
    Virtual bound{"virtual pad bound overlay"};
    attach(bound);
    host_frame(guest);
    tap(guest, bound, SDL_GAMEPAD_BUTTON_BACK);
    const bool unopened = !overlay_open() && !paused();
    set_button(bound, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    const bool a_by_file = read(0).buttons & PAD_A;
    set_button(bound, SDL_GAMEPAD_BUTTON_SOUTH, false);
    host_frame(guest);
    check(unopened && a_by_file, "a gamepad controls.txt binds opens no menu with its Back, and its button 0 is A on port 1 by the file");
    detach(bound);
    host_frame(guest);
    no_controls(dir);

    // The menu's sizes follow the window's height, built at 960 lines and again at 1440.
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool at_960 = std::abs(style.FontScaleMain - 960.0f / 1080.0f) < 1e-4f && style.WindowPadding.x == 28.0f;
    SDL_SetWindowSize(window, 1920, 1440);
    host_frame(guest);
    const bool at_1440 = std::abs(style.FontScaleMain - 1440.0f / 1080.0f) < 1e-4f && style.WindowPadding.x == 42.0f;
    SDL_SetWindowSize(window, 1280, 960);
    host_frame(guest);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    check(at_960 && at_1440, "the menu's style is built for the window's height, and again when that changes");

    unplug({&pad, &nintendo});
    gamepads_shutdown();
    port_changes(guest, dir);
    controllers_order(guest, dir);
    controllers_buttons(guest, dir);
    order_left_out(guest, dir);
    bindings_page_checks(guest, dir);
    close_window(window);
}

}  // namespace
