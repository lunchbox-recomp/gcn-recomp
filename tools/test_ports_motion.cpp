// ---- The menu's motion over a window from SDL's dummy video driver, on a host clock moved by
// hand (g_host_ns): opening still asks for the pause at once, the paused wait is 8 ms while it
// moves and 16 ms once it's still, and an `[overlay] shown after N ms` line comes once it's
// whole, while the backdrop dims the game from clear. As it closes the guest goes on at once
// while the menu fades with no input of its own, from a key, the mouse or a gamepad, and once
// it's gone 600 closed frames lay out no menu frame and no notice. With menu_motion off, and
// under --dump-output, nothing moves, and it says it's shown at the frame that first draws it.
// Keys queued in the pump of the F1 that closes it press nothing in its fade or at its next
// open. Quit?, the press-A order and the Buttons view stay drawn as
// they were through the fade and end after it. Read in by tools/test_ports.cpp after the
// frontend, with GCN_OVERLAY.
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

// The mouse moved to (x, y) over the window and its left button pressed or let go there, then a
// host frame.
void mouse(Guest& guest, float x, float y, bool down) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = g_window_id;
    e.motion.x = x;
    e.motion.y = y;
    SDL_PushEvent(&e);
    e = SDL_Event{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = g_window_id;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = down;
    e.button.clicks = 1;
    e.button.x = x;
    e.button.y = y;
    SDL_PushEvent(&e);
    host_frame(guest);
}

// With menu_motion off the menu is whole as it opens, but Dear ImGui lays out a new window once
// before it draws it. Whether `[overlay] shown` waited for the frame that drew it.
bool shown_when_drawn(SDL_Window* window, const std::string& dir) {
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = dir + "/shown-settings.txt";
    picture.apply = record_apply;
    picture.now.menu_motion = false;
    overlay_init(window, "Test title", picture);
    Guest guest;
    const std::string said = dir + "/shown-stderr.txt";
    bool quiet = false;
    capture(said, [&] {
        key(guest, SDL_SCANCODE_F1, SDLK_F1, true);
        quiet = panel_alpha() < 0 && lines_with(said, "shown after") == 0;
        key(guest, SDL_SCANCODE_F1, SDLK_F1, false);
    });
    const bool once = panel_alpha() > 0 && lines_with(said, "[overlay] shown after 0 ms\n") == 1;
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    return quiet && once;
}

// Down, then Space, then F1, all in one host frame's pump, then each let go.
void down_space_f1(Guest& guest) {
    for (bool down : {true, false}) {
        push_key(SDL_SCANCODE_DOWN, SDLK_DOWN, down);
        push_key(SDL_SCANCODE_SPACE, SDLK_SPACE, down);
        push_key(SDL_SCANCODE_F1, SDLK_F1, down);
        host_frame(guest);
    }
}

// The Display page open on 16:9, so that Down then Space on its first choice, 4:3, would change
// it: Down, Space and F1 in one pump close the menu, with `motion` or not, and it fades out and
// opens again. Then they open it from closed, and it closes again. Whether no setting was
// applied or saved: the presses ahead of F1 in its pump reach neither the menu's fade nor its
// next open's first frame, and none reach the menu while it's closed.
bool nothing_queued(SDL_Window* window, const std::string& dir, bool motion) {
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = dir + "/queued-settings-" + (motion ? "motion" : "still") + ".txt";
    picture.apply = record_apply;
    picture.now.aspect = picture.saved.aspect = "16:9";
    picture.now.menu_motion = motion;
    overlay_init(window, "Test title", picture);
    const int applies = g_applies;
    Guest guest;
    const auto settle = [&] {
        g_host_ns += 1000 * kMs;
        host_frame(guest);
        host_frame(guest);
    };
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    settle();
    tap(guest, SDL_SCANCODE_DOWN, SDLK_DOWN);  // the focus onto the page's first choice
    down_space_f1(guest);  // closes it
    host_frame(guest);
    settle();
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    settle();
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    settle();
    down_space_f1(guest);  // opens it
    settle();
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    settle();
    return g_applies == applies && !exists_path(picture.path);
}

// The menu's window as last laid out: its size and how many vertices it drew.
struct Look {
    ImVec2 size;
    int vtx = -1;
};
Look look() {
    const ImGuiWindow* w = ImGui::FindWindowByName("Test title");
    return w ? Look{w->SizeFull, w->DrawList->VtxBuffer.Size} : Look{};
}

// F1 closes the menu from a view shown in place of its tabs, which `showing` says is up. Whether
// the fade's first frame drew the view as the last open frame did, no larger or smaller, with
// it still up, and whether it had ended once the fade was over.
bool kept_through_fade(Guest& guest, bool (*showing)()) {
    const Look open = look();
    key(guest, SDL_SCANCODE_F1, SDLK_F1, true);
    const Look first = look();
    const bool fading = !overlay_open() && overlay_animating() && showing();
    key(guest, SDL_SCANCODE_F1, SDLK_F1, false);
    g_host_ns += 1000 * kMs;
    host_frame(guest);
    return fading && open.vtx > 0 && first.vtx == open.vtx && first.size.x == open.size.x &&
           first.size.y == open.size.y && !showing();
}

// Quit?, the press-A order and the Buttons view each stay up through the close's fade, drawn as
// they were, and end once it's over. One closed and opened again mid-fade has ended, and with
// menu_motion off one ends as the menu closes.
void views_kept(SDL_Window* window, const std::string& dir) {
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = dir + "/views-settings.txt";
    picture.apply = record_apply;
    overlay_init(window, "Test title", picture);
    gamepads_init();
    Virtual pad{"virtual pad views"};
    attach(pad);
    Guest guest;
    host_frame(guest);
    const auto open = [&] {
        tap(guest, SDL_SCANCODE_F1, SDLK_F1);
        g_host_ns += 1000 * kMs;
        host_frame(guest);
        host_frame(guest);
    };
    const auto ask_quit = [&] {
        bottom_row(guest);
        tap(guest, SDL_SCANCODE_RIGHT, SDLK_RIGHT);
        tap(guest, SDL_SCANCODE_RETURN, SDLK_RETURN);
        host_frame(guest);
        return waiting(OverlayAsk::quit);
    };
    const auto quit_asked = [] { return overlay_asking() == OverlayAsk::quit; };
    open();
    check(ask_quit() && kept_through_fade(guest, quit_asked),
          "closed from Quit?, the fade draws Quit? as it showed, and Quit? ends once the fade is over");

    open();
    overlay_controllers_order();
    host_frame(guest);
    host_frame(guest);
    const bool ordering = overlay_controllers_asking() == 0;
    check(ordering && kept_through_fade(guest, [] { return overlay_controllers_asking() >= 0; }),
          "closed from the press-A order, the fade draws the order, and it ends once the fade is over");

    open();
    const bool shown = overlay_bindings_open(pad.id) && overlay_bindings_listen(PadControl::Z);
    host_frame(guest);
    host_frame(guest);
    key(guest, SDL_SCANCODE_F1, SDLK_F1, true);
    set_button(pad, SDL_GAMEPAD_BUTTON_NORTH, true);
    host_frame(guest);
    const bool listening = overlay_bindings_shown() && overlay_bindings_listening();
    set_button(pad, SDL_GAMEPAD_BUTTON_NORTH, false);
    key(guest, SDL_SCANCODE_F1, SDLK_F1, false);
    g_host_ns += 1000 * kMs;
    host_frame(guest);
    check(shown && listening && !overlay_bindings_shown() && !overlay_bindings_listening() &&
              !has(overlay_bindings_said(), "is now"),
          "closed from the Buttons view as it listens, the fade draws it listening, a press then binds "
          "nothing, and the view ends once the fade is over");
    open();
    overlay_bindings_open(pad.id);
    host_frame(guest);
    host_frame(guest);
    check(kept_through_fade(guest, overlay_bindings_shown), "closed from the Buttons view, the fade draws the view");

    open();
    const bool asked = ask_quit();
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    check(asked && overlay_open() && overlay_animating() && overlay_asking() == OverlayAsk::none,
          "closed from Quit? and opened again before the fade is over, it's back on its tabs");
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    g_host_ns += 1000 * kMs;
    host_frame(guest);
    unplug({&pad});
    gamepads_shutdown();

    overlay_test_reset_pages();
    picture.now.menu_motion = false;
    overlay_init(window, "Test title", picture);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    const bool still_asked = ask_quit();
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    check(still_asked && overlay_asking() == OverlayAsk::none, "with menu_motion off, Quit? ends as the menu closes");
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
    const ImGuiWindow* panel = ImGui::FindWindowByName("Test title");
    mouse(guest, panel->Pos.x + 8.0f, panel->Pos.y + 8.0f, true);  // held on the title bar
    key(guest, SDL_SCANCODE_DOWN, SDLK_DOWN, true);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    const bool held_down = ImGui::IsKeyDown(ImGuiKey_DownArrow);
    const bool held_mouse = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    key(guest, SDL_SCANCODE_DOWN, SDLK_DOWN, false);
    mouse(guest, panel->Pos.x + 8.0f, panel->Pos.y + 8.0f, false);
    const uint64_t closed = g_host_ns;
    set_button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    host_frame(guest);
    const bool pad_down = ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown);
    set_button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    check(!held_down && !held_mouse,
          "a key or a mouse button held as it closes is let go for the menu, so nothing moves its focus as it fades");
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
    check(nothing_queued(window, dir, true),
          "Down and Space ahead of the F1 that closes it, in one pump, press nothing as it fades or as it opens again");
    check(nothing_queued(window, dir, false),
          "with menu_motion off they press nothing as it opens again, and none ahead of an F1 that opens it do");
    views_kept(window, dir);
    check(shown_when_drawn(window, dir),
          "with menu_motion off it says it's shown at the frame that first draws it, not before");
    close_window(window);
}

}  // namespace
