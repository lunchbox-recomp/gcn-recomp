// ---- The menu's pages turned from its header, over a window from SDL's dummy video driver: R1,
// L1, either trigger and E and Q each move exactly one page, and no further than the first or
// the last. A shoulder, a trigger or a key held as the menu opens moves nothing, and nothing
// moves while a slider is active, while the menu asks or during the press-A order. A trigger
// turns a page past the gamepad's own trigger click, lowered too, and not short of it. After a
// change the page's first item has the focus, so A presses it. Read in by tools/test_ports.cpp
// after the frontend, with GCN_OVERLAY.
#include "test_ports_support.h"

namespace {

int g_knob_presses;  // the Knobs page's button, pressed
float g_knob_level = 0.5f;

// A page whose first item is a button, then a slider, which A makes active on a gamepad.
void knobs_page(void*) {
    if (ImGui::Button("Press")) g_knob_presses++;
    ImGui::SliderFloat("##Level", &g_knob_level, 0.0f, 1.0f, "");
}

// A trigger pulled in to `travel` as SDL's gamepad reads it, 0 to 32767, all the way unless
// said, and let go to rest, a host frame each. SDL maps a virtual trigger's -32768 to 32767 onto
// that.
void pull(Guest& g, const Virtual& v, int axis, int travel = 32767) {
    set_axis(v, axis, (Sint16)std::clamp(2 * travel - 32767, -32768, 32767));
    host_frame(g);
    set_axis(v, axis, -32768);
    host_frame(g);
}

void tabs_turn(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    overlay_test_reset_pages();
    overlay_add_page("Knobs", knobs_page, nullptr);
    overlay_add_page("Extra", draw_added_page, &g_page_draws);
    OverlayPicture picture;
    picture.path = dir + "/tabs-settings.txt";
    picture.apply = record_apply;
    picture.now.menu_motion = false;
    overlay_init(window, "Test title", picture);
    gamepads_init();
    Virtual pad{"virtual pad tabs"};
    attach(pad);
    Guest guest;
    host_frame(guest);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    const int last = (int)overlay_test_pages().size() - 1;

    std::vector<int> seen;  // the page after each press
    const auto after = [&](const std::function<void()>& press) {
        press();
        seen.push_back(overlay_page());
    };
    after([&] { tap(guest, pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER); });
    after([&] { tap(guest, pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER); });
    after([&] { pull(guest, pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER); });
    after([&] { pull(guest, pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER); });
    after([&] { tap(guest, SDL_SCANCODE_E, SDLK_E); });
    after([&] { tap(guest, SDL_SCANCODE_Q, SDLK_Q); });
    check(overlay_open() && seen == std::vector<int>{1, 0, 1, 0, 1, 0},
          "R1, L1, the right and left triggers, E and Q each move exactly one page, on and back");
    // A GameCube pad's R and Z (SDL's R2 and R1) pressed together, as a hand might.
    set_button(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    set_axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    host_frame(guest);
    set_button(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
    set_axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    host_frame(guest);
    check(overlay_page() == 1, "R1 and the right trigger pressed together move one page, not two");
    // Each trigger in to just short of the gamepad's own click, 29000 at the default, and back
    // turns nothing, and in all the way it turns one page.
    PadBinding own;
    const bool has_own = gamepads_binding(pad.id, own) && own.trigger_click == kGamepadTriggerClick;
    pull(guest, pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, own.trigger_click - 1000);
    pull(guest, pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, own.trigger_click - 1000);
    const bool nothing_short = overlay_page() == 1;
    pull(guest, pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    check(has_own && nothing_short && overlay_page() == 2,
          "a trigger 29000 in and back, short of its click, turns nothing, and 32767 in turns one page");

    tap(guest, pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    tap(guest, SDL_SCANCODE_Q, SDLK_Q);
    const bool first = overlay_page() == 0;
    for (int i = 0; i < last; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    const bool reached = overlay_page() == last;
    pull(guest, pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    tap(guest, SDL_SCANCODE_E, SDLK_E);
    check(first && reached && overlay_page() == last && g_page_draws > 0,
          "no wrap: L1 and Q on the first page and R1, the trigger and E on the last move nothing");

    // Held as it opens: each stays out of the menu until it's let go, and its next press moves.
    const auto held_at_open = [&](const std::function<void(bool)>& hold) {
        tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
        hold(true);
        host_frame(guest);
        tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
        host_frame(guest);
        const bool still = overlay_open() && overlay_page() == last;
        hold(false);
        host_frame(guest);
        const bool let_go = overlay_page() == last;
        tap(guest, pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        const bool moves = overlay_page() == last - 1;
        tap(guest, pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        return still && let_go && moves;
    };
    check(held_at_open([&](bool down) { set_button(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, down); }),
          "L1 held as the menu opens moves nothing, nor does letting it go, and the next press moves one page");
    check(held_at_open([&](bool down) { set_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, down ? 32767 : 0); }),
          "the left trigger held as the menu opens moves nothing, nor does letting it go");
    check(held_at_open([&](bool down) { push_key(SDL_SCANCODE_Q, SDLK_Q, down); }),
          "Q held as the menu opens moves nothing, nor does letting it go");

    // After a change the page's first item has the focus: A presses Knobs' button.
    tap(guest, pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    const bool on_knobs = overlay_page() == page_at("Knobs");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(on_knobs && g_knob_presses == 1, "after a page change the page's first item has the focus, and A presses it");

    // A slider made active by A keeps the shoulders, which step it, until A lets it go.
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool active = ImGui::IsAnyItemActive();
    tap(guest, pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    pull(guest, pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    tap(guest, SDL_SCANCODE_E, SDLK_E);
    const bool kept = overlay_page() == page_at("Knobs");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool let_go = !ImGui::IsAnyItemActive();
    tap(guest, pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    check(active && kept && let_go && overlay_page() == page_at("Extra") && g_knob_presses == 1,
          "while a slider is active no shoulder, trigger or key moves a page, and once it's let go R1 does");

    // While the menu asks, nothing moves, and B goes back to the page it was on.
    bottom_row(guest, pad);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool asking = waiting(OverlayAsk::restart);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    pull(guest, pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    tap(guest, SDL_SCANCODE_Q, SDLK_Q);
    const bool still_asking = waiting(OverlayAsk::restart) && overlay_page() == page_at("Extra");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    check(asking && still_asking && waiting(OverlayAsk::none) && overlay_page() == page_at("Extra"),
          "while the menu asks no shoulder, trigger or key moves a page, and B goes back to the same page");

    // During the press-A order, nothing moves either.
    pick_page(guest, "Controllers");
    overlay_controllers_order();
    host_frame(guest);
    const bool ordering = overlay_controllers_asking() == 0;
    tap(guest, pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    pull(guest, pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    tap(guest, SDL_SCANCODE_E, SDLK_E);
    const bool order_kept = overlay_controllers_asking() == 0 && overlay_page() == page_at("Controllers");
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    check(ordering && order_kept && overlay_controllers_asking() == -1 && overlay_open(),
          "during the press-A order no shoulder, trigger or key moves a page");

    // A gamepad whose trigger click is lowered, as the Buttons view lowers it for a worn pad,
    // turns a page with a trigger past that click, as the game takes its L and R there.
    PadBinding lowered;
    const bool bound = gamepads_binding(pad.id, lowered);
    lowered.trigger_click = 20000;
    gamepads_bind(pad.id, lowered);
    const int from = overlay_page();
    pull(guest, pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 19000);
    const bool short_of = overlay_page() == from;
    pull(guest, pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 21000);
    check(bound && from > 0 && short_of && overlay_page() == from - 1,
          "with the gamepad's trigger click lowered to 20000, a trigger 21000 in turns a page and 19000 in doesn't");
    gamepads_reset_binding(pad.id);

    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    unplug({&pad});
    gamepads_shutdown();
    close_window(window);
}

// A page change with `motion` or not, on a host clock moved by hand: whether E changed the page,
// whether it was moving then, with the paused wait at 8 ms, and a millisecond short of the
// theme's page_ms, and whether it was still with the wait back at 16 ms at page_ms.
struct Turned {
    bool changed, moving, before_end, still_at_end;
};
Turned page_change(SDL_Window* window, const std::string& dir, bool motion) {
    overlay_test_reset_pages();
    overlay_add_page("Knobs", knobs_page, nullptr);
    OverlayPicture picture;
    picture.path = dir + "/page-motion-settings.txt";
    picture.apply = record_apply;
    picture.now.menu_motion = motion;
    overlay_init(window, "Test title", picture);
    const uint64_t ms = (uint64_t)overlay_theme_default().page_ms * kMs;
    Guest guest;
    g_host_ns += 10000 * kMs;  // past every tween an earlier check left
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    g_host_ns += 1000 * kMs;  // past the open's motion
    host_frame(guest);
    Turned t{};
    const uint64_t at = g_host_ns;
    key(guest, SDL_SCANCODE_E, SDLK_E, true);
    t.changed = overlay_page() == 1;
    t.moving = overlay_animating() && batch_wait_ms() == 8;
    g_host_ns = at + ms - kMs;
    host_frame(guest);
    t.before_end = overlay_animating();
    g_host_ns = at + ms;
    host_frame(guest);
    t.still_at_end = !overlay_animating() && batch_wait_ms() == 16;
    key(guest, SDL_SCANCODE_E, SDLK_E, false);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    g_host_ns += 1000 * kMs;  // past the close's fade
    host_frame(guest);
    return t;
}

void page_motion(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    const Turned on = page_change(window, dir, true);
    check(on.changed && on.moving && on.before_end && on.still_at_end,
          "a page change moves on the host clock for the theme's page_ms, the paused wait 8 ms meanwhile, then 16");
    const Turned off = page_change(window, dir, false);
    check(off.changed && !off.moving && !off.before_end && off.still_at_end,
          "with menu_motion off a page change is whole at once");
    close_window(window);
}

}  // namespace
