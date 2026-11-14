// ---- The overlay (overlay/overlay.h) with no window, so no menu is drawn: GCN_OVERLAY_AT,
// what opens and closes it on each kind of gamepad and the keyboard, what the game gets while
// it's open and as it closes, and the Display page's saves. Read in by tools/test_ports.cpp
// after the frontend, with GCN_OVERLAY.
#include "test_ports_support.h"

namespace {

void key_down(SDL_Scancode key, bool repeat = false) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = key;
    e.key.down = true;
    e.key.repeat = repeat;
    SDL_PushEvent(&e);
}

void set_axis(const Virtual& v, int axis, Sint16 value) {
    if (SDL_Joystick* j = SDL_GetJoystickFromID(v.id)) SDL_SetJoystickVirtualAxis(j, axis, value);
}

void overlay(const std::string& dir) {
    add_extra_page();
    const std::string path = dir + "/settings.txt";
    write_file(path, "# The picture\naspect 4:3\nvsync on\nfullscreen 0\n\nscale 2\ndisc console\n# the end\n");
    OverlayPicture picture;
    picture.saved = picture.now = settings_load(path);
    picture.now.scale = 3;  // as --scale=3 makes it for this run
    picture.path = path;
    picture.apply = record_apply;
    const std::string at_said = dir + "/overlay-at-stderr.txt";
    setenv("GCN_OVERLAY_AT", "", 1);
    capture(at_said, [&] { overlay_init(nullptr, "Test title", picture); });
    unsetenv("GCN_OVERLAY_AT");
    check(lines_with(at_said, "GCN_OVERLAY_AT") == 0,
          "an empty GCN_OVERLAY_AT opens nothing and says nothing, as an unset one does");
    // Once GCN_OVERLAY_AT's frame is presented the menu is due before overlay_draw opens it, so
    // show_frame takes the gamepads' lock for that first frame of it too.
    setenv("GCN_OVERLAY_AT", "5:1", 1);
    overlay_init(nullptr, "Test title", picture);
    unsetenv("GCN_OVERLAY_AT");
    g_presented = 4;
    const bool early = !overlay_menu_due();
    g_presented = 5;
    check(early && overlay_menu_due() && !overlay_open(),
          "the menu is due once GCN_OVERLAY_AT's frame is presented, before it opens, and not a frame sooner");
    g_presented = 0;
    power_on(1);
    gamepads_init();
    Virtual pad{"virtual pad overlay"};
    attach(pad);
    Guest guest;
    host_frame(guest);
    check(!overlay_open() && !paused() && guest.passed == 1, "with the overlay closed, the guest goes past its frame boundary");

    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, true);
    int frames = 0;
    while (frames < 4 && !(overlay_open() && paused())) {
        host_frame(guest);
        frames++;
    }
    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, false);
    host_frame(guest);
    check(frames <= 2 && overlay_open() && paused(), "a gamepad's Back opens the overlay, and the guest is held within 2 host frames");
    host_frame(guest);
    check(g_page_draws == 0, "a null window never calls an added page without an ImGui context");

    // Each pushed all the way, and back to where it rests: a virtual pad's triggers rest at the
    // bottom of their axis.
    const struct {
        int axis;
        Sint16 rest;
    } kPushed[] = {{SDL_GAMEPAD_AXIS_LEFTX, 0}, {SDL_GAMEPAD_AXIS_RIGHTX, 0},
                   {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768}, {SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768}};
    for (const auto& a : kPushed) set_axis(pad, a.axis, 32767);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    const PadState gated = read(0);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    for (const auto& a : kPushed) set_axis(pad, a.axis, a.rest);
    host_frame(guest);
    check(gated.connected && at_rest(gated) && overlay_open(),
          "while it's open, the gamepad's A, sticks and triggers don't reach port 1, which stays plugged in");

    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, true);
    host_frame(guest);
    const bool closed = !overlay_open() && !pause_requested();
    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, false);
    host_frame(guest);
    check(closed && !paused() && guest.passed == guest.asked, "Back again closes it, and the guest goes on");

    key_down(SDL_SCANCODE_F1);
    host_frame(guest);
    const bool f1_opens = overlay_open() && paused();
    key_down(SDL_SCANCODE_F1);
    host_frame(guest);
    check(f1_opens && !overlay_open() && !paused(), "F1 opens it and F1 closes it");

    key_down(SDL_SCANCODE_F1);
    host_frame(guest);
    key_down(SDL_SCANCODE_F1, true);
    host_frame(guest);
    const bool f1_repeat_stays = overlay_open();
    key_down(SDL_SCANCODE_ESCAPE, true);
    host_frame(guest);
    const bool escape_repeat_stays = overlay_open();
    key_down(SDL_SCANCODE_F1);
    host_frame(guest);
    check(f1_repeat_stays && escape_repeat_stays && !overlay_open(), "a held F1's or Escape's repeat doesn't close it");

    key_down(SDL_SCANCODE_F1);
    host_frame(guest);
    key_down(SDL_SCANCODE_ESCAPE);
    const bool stays = host_frame(guest);
    const bool escape_closes = stays && !overlay_open() && !paused();
    key_down(SDL_SCANCODE_ESCAPE, true);
    const bool repeat_stays = host_frame(guest);
    key_down(SDL_SCANCODE_ESCAPE);
    check(escape_closes && repeat_stays && !host_frame(guest) && !overlay_open(),
          "Escape closes it without quitting; with it closed, Escape quits, though a held Escape's repeat doesn't");

    key_down(SDL_SCANCODE_F1);
    host_frame(guest);
    set_button(pad, SDL_GAMEPAD_BUTTON_EAST, true);
    host_frame(guest);
    const bool b_closes = !overlay_open() && !paused();
    const PadState held = read(0);
    // Back opens it again with that B still down, and the game doesn't get the B on the way to
    // the hold.
    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, true);
    host_frame(guest);
    const PadState reopening = read(0);
    const bool reopened = overlay_open() && paused();
    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, false);
    host_frame(guest);
    set_button(pad, SDL_GAMEPAD_BUTTON_EAST, false);
    host_frame(guest);
    const bool b_up_stays = overlay_open();
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    check(reopened && reopening.connected && !(reopening.buttons & PAD_B) && b_up_stays && !overlay_open(),
          "Back opens it again with that B still down, and the game doesn't get the B as the guest is held");
    set_button(pad, SDL_GAMEPAD_BUTTON_EAST, true);
    host_frame(guest);
    const PadState again = read(0);
    set_button(pad, SDL_GAMEPAD_BUTTON_EAST, false);
    host_frame(guest);
    check(b_closes && !held.buttons && again.buttons == PAD_B && !overlay_open(),
          "B closes it and stays out of the game until it's let go; pressed with it closed, it's the game's B and opens nothing");

    // Until the guest is held, port 1 keeps what it had as the overlay opened, so the game
    // never sees the A let go, and then it's at rest.
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    const bool a_in = read(0).buttons & PAD_A;
    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, true);
    host_frame(guest);
    const PadState opening = read(0);
    set_button(pad, SDL_GAMEPAD_BUTTON_BACK, false);
    host_frame(guest);
    const PadState stopped = read(0);
    check(a_in && (opening.buttons & PAD_A) && overlay_open() && paused() && stopped.connected && at_rest(stopped),
          "an A held across the open stays in port 1 until the guest is held, then the port is at rest");
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);

    // A trigger in as it closes stays out of the game with its travel until it's back at rest,
    // though it's let out past its click (kGamepadTriggerClick) on the way.
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    set_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    set_axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    host_frame(guest);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    const bool shut = !overlay_open();
    set_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 20000);
    host_frame(guest);
    const PadState eased = read(0);
    set_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    set_axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    host_frame(guest);
    set_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 20000);
    host_frame(guest);
    const PadState rested = read(0);
    set_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    host_frame(guest);
    check(shut && eased.connected && !eased.trig_l && !eased.trig_r && !(eased.buttons & (PAD_L | PAD_R)),
          "a trigger in as it closes stays out of the game with its travel, eased out past its click too");
    check(rested.trig_l > 0 && !(rested.buttons & PAD_L), "once it's been back at rest, its travel is the game's again");

    // A gamepad with only the GameCube's buttons, as SDL maps one on an adapter, opens and closes
    // it with Z and both trigger clicks held for 1 s with nothing else, once a hold. The clicks
    // are SDL's MISC3 and MISC4 where it maps them, as for the adapter, and the triggers' axes
    // past their click (kGamepadTriggerClick) where it doesn't. The second is on the host clock
    // (host_ns), which g_host_ns sets here. SDL keeps a virtual gamepad's mapping under the first
    // nine characters of its name, so each name here starts differently.
    Virtual cube{"gamecube pad overlay"};
    cube.button_mask = kCubeButtons | kClicks;
    Virtual bare{"bare cube pad overlay"};  // one whose mapping has no click buttons
    bare.button_mask = kCubeButtons;
    attach(cube);
    attach(bare);
    host_frame(guest);
    // The host clock `ms` on, then a host frame.
    const auto after = [&](uint64_t ms) {
        g_host_ns += ms * 1000000;
        host_frame(guest);
    };
    // `v`'s Z, and its trigger clicks: its click buttons where it has them, or else both triggers
    // pushed to `travel`, then a host frame.
    const auto chord = [&](const Virtual& v, bool z, bool clicks, Sint16 travel = 32767) {
        set_button(v, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, z);
        if (v.button_mask & kClicks) {
            set_button(v, SDL_GAMEPAD_BUTTON_MISC3, clicks);
            set_button(v, SDL_GAMEPAD_BUTTON_MISC4, clicks);
        } else {
            set_axis(v, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, clicks ? travel : -32768);
            set_axis(v, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, clicks ? travel : -32768);
        }
        after(0);
    };
    // The chord held on `v` for `ms`, then let go. Whether that opened or closed the overlay.
    const auto held_for = [&](const Virtual& v, uint64_t ms) {
        const bool was = overlay_open();
        chord(v, true, true);
        after(ms);
        const bool toggled = overlay_open() != was;
        chord(v, false, false);
        return toggled;
    };
    chord(cube, true, true);
    after(999);
    const bool not_yet = !overlay_open();
    after(1);
    const bool opens = overlay_open() && paused();
    after(1500);
    const bool once = overlay_open();
    chord(cube, false, false);
    const bool closes = held_for(cube, 1000) && !overlay_open() && !paused();
    check(not_yet && opens && once && closes,
          "on a gamepad with only the GameCube's buttons, Z and both clicks held 1 s open it, once a hold, and close it");
    const bool first_short = !held_for(cube, 900);
    check(first_short && !held_for(cube, 900) && !overlay_open(),
          "let go before 1 s they don't, and the next hold is timed from its own start");

    // Any other button stops it: A or Start held from the start, or A pressed and let go partway.
    set_button(cube, SDL_GAMEPAD_BUTTON_SOUTH, true);
    const bool with_a = !held_for(cube, 1500);
    set_button(cube, SDL_GAMEPAD_BUTTON_SOUTH, false);
    set_button(cube, SDL_GAMEPAD_BUTTON_START, true);
    const bool with_start = !held_for(cube, 1500);
    set_button(cube, SDL_GAMEPAD_BUTTON_START, false);
    chord(cube, true, true);
    after(500);
    set_button(cube, SDL_GAMEPAD_BUTTON_SOUTH, true);
    after(0);
    set_button(cube, SDL_GAMEPAD_BUTTON_SOUTH, false);
    after(1500);
    const bool a_stops = !overlay_open();
    chord(cube, false, false);
    check(with_a && with_start && a_stops && !overlay_open(),
          "with A or Start held too they don't, nor with A pressed and let go during the hold");

    chord(cube, false, true);
    after(1500);
    const bool no_z = !overlay_open();
    chord(cube, false, false);
    set_button(bare, SDL_GAMEPAD_BUTTON_START, true);
    chord(bare, false, true);
    after(1500);
    const bool no_start_chord = !overlay_open();
    set_button(bare, SDL_GAMEPAD_BUTTON_START, false);
    chord(bare, false, false);
    check(no_z && no_start_chord, "the clicks without Z don't open it, nor do L and R clicked in with Start");

    chord(bare, true, true, 20000);
    after(1500);
    const bool partway = !overlay_open();
    chord(bare, false, false);
    const bool bare_opens = held_for(bare, 1000) && overlay_open();
    chord(bare, true, true);
    after(1000);
    const bool bare_closes = !overlay_open() && !paused();
    const PadState closing = read(2);
    chord(bare, false, false);
    check(partway && bare_opens && bare_closes,
          "where SDL maps no click buttons the triggers past their click stand in, though partway in they don't");
    check(closing.connected && at_rest(closing),
          "the hold that closed it stays out of port 3 until it's let go, the triggers' travel too");
    // With the gamepad's trigger click lowered, as the Buttons view lowers it for a worn pad, the
    // triggers past it are the chord's clicks, as they're the game's L and R.
    PadBinding lowered;
    const bool bound = gamepads_binding(bare.id, lowered);
    lowered.trigger_click = 25000;
    gamepads_bind(bare.id, lowered);
    chord(bare, true, true, 20000);
    after(1000);
    const bool lowered_opens = overlay_open();
    chord(bare, false, false);
    const bool lowered_closes = held_for(bare, 1000) && !overlay_open();
    gamepads_reset_binding(bare.id);
    check(bound && lowered_opens && lowered_closes,
          "with the gamepad's trigger click lowered, the triggers partway in past it open and close it");

    // A Switch 2 GameCube controller has no Back. Its ZL, a button the GameCube's pad lacks, opens
    // and closes it, and so does its Home, SDL's left shoulder and Guide on it. It has Guide, so
    // the GameCube's chord isn't its, though it has the clicks.
    Virtual nso{"switch 2 gamecube overlay"};
    nso.vendor = 0x057e;
    nso.product = 0x2073;
    nso.button_mask = kCubeButtons | kClicks | 1u << SDL_GAMEPAD_BUTTON_GUIDE | 1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER |
                      1u << SDL_GAMEPAD_BUTTON_MISC1 | 1u << SDL_GAMEPAD_BUTTON_MISC2;
    attach(nso);
    host_frame(guest);
    // `button` pressed and let go on `v`, a host frame each. Whether the overlay's open after.
    // SDL holds back a Guide let go within 250 ms of its press (SDL_gamepad.c's
    // SDL_MINIMUM_GUIDE_BUTTON_DELAY_MS) and drops the next press until it's sent, so a Guide is
    // held that long first.
    const auto pressed = [&](const Virtual& v, int button) {
        set_button(v, button, true);
        host_frame(guest);
        if (button == SDL_GAMEPAD_BUTTON_GUIDE) std::this_thread::sleep_for(std::chrono::milliseconds(260));
        set_button(v, button, false);
        host_frame(guest);
        return overlay_open();
    };
    const bool zl = pressed(nso, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) && !pressed(nso, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    const bool home = pressed(nso, SDL_GAMEPAD_BUTTON_GUIDE) && !pressed(nso, SDL_GAMEPAD_BUTTON_GUIDE);
    check(zl && home && !held_for(nso, 1500),
          "on a Switch 2 GameCube controller ZL opens it and closes it, and so does Home, but the chord doesn't");
    const bool guide = !pressed(pad, SDL_GAMEPAD_BUTTON_GUIDE);
    const bool shoulder = !pressed(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    check(guide && shoulder && !held_for(pad, 1500),
          "on a gamepad with a Back, Guide, the left shoulder and the GameCube's chord open nothing");
    unplug({&cube, &bare, &nso});

    // Guide opens and closes it on any gamepad SDL maps with Guide and no Back, as it maps a
    // single Switch 2 Joy-Con. ZL is the Switch 2 GameCube controller's alone. This one is made
    // up, with neither's USB IDs, and it takes a port the others left.
    Virtual home_only{"home only pad overlay"};
    home_only.button_mask = kCubeButtons | 1u << SDL_GAMEPAD_BUTTON_GUIDE | 1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    attach(home_only);
    host_frame(guest);
    const bool home_only_opens =
        pressed(home_only, SDL_GAMEPAD_BUTTON_GUIDE) && !pressed(home_only, SDL_GAMEPAD_BUTTON_GUIDE);
    check(home_only_opens && !pressed(home_only, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER),
          "on another gamepad with Guide and no Back, Guide opens it and closes it, and its left shoulder doesn't");
    unplug({&home_only});

    Settings next = picture.now;
    next.aspect = "16:9";
    overlay_display_change(next);
    check(read_file(path) == "# The picture\naspect 16:9\nvsync on\nfullscreen 0\n\nscale 2\ndisc console\n# the end\n" &&
              g_applied.aspect == "16:9" && g_applied.scale == 3,
          "the Display page's aspect applies at once and is saved, every other line as it was and the flag's scale left out");
    next.fullscreen = true;
    overlay_display_change(next);
    check(read_file(path) == "# The picture\naspect 16:9\nvsync on\nfullscreen 1\n\nscale 2\ndisc console\n# the end\n" &&
              g_applied.fullscreen,
          "its fullscreen applies at once and is saved, every other line as it was");
    next.scale = 5;
    overlay_display_change(next);
    overlay_display_change(next);
    check(read_file(path) == "# The picture\naspect 16:9\nvsync on\nfullscreen 1\n\nscale 5\ndisc console\n# the end\n" &&
              g_applied.scale == 5 && g_applies == 3,
          "its scale applies at once and is saved, every other line as it was, and a picture with no change does nothing");
    unplug({&pad});
    gamepads_shutdown();
}

}  // namespace
