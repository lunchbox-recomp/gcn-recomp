// ---- The port each gamepad takes (gamepads.cpp): its player index's under --pads=1 and
// --pads=2, at launch and as it connects, and what pulling it out leaves. Read in by
// tools/test_ports.cpp after the frontend.
#include "test_ports_support.h"

namespace {

// ---- The rules, with --pads=1, the default.
void rules() {
    power_on(1);
    gamepads_init();
    frame();
    check(read(0).connected && at_rest(read(0)) && !read(1).connected && !read(2).connected && !read(3).connected,
          "--pads=1, no gamepad: port 1 plugged in at rest, ports 2 to 4 not");

    Virtual a{"virtual pad a"}, b{"virtual pad b"}, c{"virtual pad c"}, d{"virtual pad d"}, e{"virtual pad e"},
        f{"virtual pad f"}, g{"virtual pad g"}, h{"virtual pad h"}, k{"virtual pad k"};
    attach(a);
    frame();
    check(port(a) == 1 && !read(1).connected && !read(2).connected && !read(3).connected,
          "a lone gamepad takes port 1 with index 0, and ports 2 to 4 stay unplugged");

    attach(b);
    const bool sdl01 = player(a) == 0 && player(b) == 1;
    frame();
    check(sdl01 && port(a) == 1 && port(b) == 2,
          "two gamepads SDL gives player indexes 0 and 1 land on ports 1 and 2, with those indexes");

    if (SDL_Joystick* jb = SDL_GetJoystickFromID(b.id)) {
        SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, true);
        SDL_SetJoystickVirtualAxis(jb, SDL_GAMEPAD_AXIS_LEFTX, 32767);
        frame();
        const PadState p2 = read(1);
        check(p2.connected && p2.buttons == PAD_A && p2.stick_x == 228 && at_rest(read(0)) && !read(2).connected &&
                  !read(3).connected,
              "port 2's gamepad presses A and pushes the stick on port 2 alone");
        SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, false);
        SDL_SetJoystickVirtualAxis(jb, SDL_GAMEPAD_AXIS_LEFTX, 0);
        frame();
    } else {
        check(false, "port 2's gamepad presses A and pushes the stick on port 2 alone (no joystick)");
    }

    motor(1);
    check(b.rumbles == 1 && a.rumbles == 0, "port 2's rumble motor runs port 2's gamepad alone");

    // While the guest is held (pause.h), the motor stays off, and one running stops.
    g_guest_held = true;
    motor(1);
    check(b.rumbles == 1, "port 2's motor turned on while the guest is held runs nothing");
    g_guest_held = false;
    set_motor(1, true);
    frame();
    const int stops = b.stops;
    g_guest_held = true;
    frame();
    check(b.rumbles == 2 && b.stops == stops + 1, "a running motor is stopped once the guest is held");
    g_guest_held = false;
    set_motor(1, false);
    frame();

    unplug({&b});
    check(!read(1).connected && port(a) == 1,
          "--pads=1: port 2's gamepad pulled out: port 2 reads unplugged, port 1 keeps its gamepad");

    attach(c, 2);
    frame();
    check(port(c) == 3 && !read(1).connected, "a gamepad with player index 2 lands on port 3, free, though port 2 is free too");

    motor(2);
    check(c.rumbles == 1 && a.rumbles == 0, "port 3's rumble motor runs port 3's gamepad alone");

    attach(d, 2);
    frame();
    check(port(d) == 2 && port(c) == 3,
          "a gamepad with player index 2 while port 3 has one lands on the lowest free port, 2; port 3's keeps port and index");
    unplug({&d});

    attach(e, -1);
    const bool unset = player(e) == -1;
    frame();
    check(unset && port(e) == 2 && read(1).connected && player(e) == 1 && e.player == 1,
          "a gamepad with no player index takes the lowest free port, 2 (4 free too), plugs it in again, and gets index 1");

    unplug({&a});
    const PadState out1 = read(0);
    attach(f);
    const int f_sdl = player(f);
    frame();
    check(out1.connected && at_rest(out1) && f_sdl == 0 && port(f) == 1 && port(e) == 2 && port(c) == 3,
          "port 1's gamepad pulled out: port 1 (the keyboard's, so still plugged in, at rest) is free, and the next gamepad takes it");

    attach(g);
    frame();
    attach(h);
    const int h_sdl = player(h);
    frame();
    check(port(g) == 4 && h_sdl == 4 && port(h) == 0 && player(h) == -1 && h.player == -1 && port(f) == 1 && port(e) == 2 &&
              port(c) == 3,
          "a fifth gamepad gets no port and its player index is cleared; the four keep their ports");

    // Left out stays out: nothing places it when a port frees, so port 3 reads unplugged
    // (--pads=1). Plugged in again, it is a new connection and takes port 3, SDL's index 2.
    unplug({&c});
    const PadState out3 = read(2);
    const bool h_out = player(h) == -1;
    detach(h);
    frame();
    attach(h);
    const int h_sdl2 = player(h);
    frame();
    check(!out3.connected && h_out && h_sdl2 == 2 && port(h) == 3 && port(f) == 1 && port(e) == 2 && port(g) == 4,
          "port 3's gamepad pulled out: the fifth stays left out and port 3 reads unplugged; plugged in again, it takes port 3");

    // A fifth that arrives holding port 2's index, as Steam or XInput could give it: SDL
    // moves port 2's gamepad to the next free index, 4, before the frontend hears of it.
    attach(k, 1);
    const bool e_moved = player(e) == 4;
    frame();
    check(e_moved && port(f) == 1 && port(e) == 2 && port(h) == 3 && port(g) == 4 && player(k) == -1 && k.player == -1,
          "a fifth gamepad holding port 2's index is left out with none, and the four keep index = port");
    unplug({&e, &f, &g, &h, &k});

    // Ports 1 and 4 taken, and a newcomer holding port 4's index: SDL moves port 4's
    // gamepad to index 1, the newcomer's lowest free port, and when the newcomer is given
    // index 1 SDL moves port 4's again, to 2.
    Virtual l{"virtual pad l"}, m{"virtual pad m"}, o{"virtual pad o"};
    attach(l);
    frame();
    attach(m, 3);
    frame();
    const bool l1m4 = port(l) == 1 && port(m) == 4;
    attach(o, 3);
    const bool m_moved = player(m) != 3;
    frame();
    check(l1m4 && m_moved && port(o) == 2 && port(m) == 4 && port(l) == 1,
          "with ports 1 and 4 taken, a gamepad holding port 4's index lands on port 2, and port 4's keeps index 3");
    unplug({&l, &m, &o});
    gamepads_shutdown();
}

// ---- Gamepads connected before gamepads_init: the same rule, in SDL's order, each
// placed once although SDL announces them again on the first frame.
void launch(const std::string& dir) {
    power_on(1);
    Virtual s1{"virtual pad s1"}, s2{"virtual pad s2"}, s3{"virtual pad s3"};
    attach(s1);
    attach(s2);
    attach(s3, 3);
    const std::string log3 = dir + "/startup3.txt";
    capture(log3, [] {
        gamepads_init();
        frame();
    });
    check(lines_with(log3, "\" on port ") == 3 && port(s1) == 1 && port(s2) == 2 && port(s3) == 4 && !read(2).connected,
          "three gamepads at launch, the third with player index 3: ports 1, 2 and 4, each placed once");
    unplug({&s1, &s2, &s3});
    gamepads_shutdown();

    Virtual t1{"virtual pad t1"}, t2{"virtual pad t2"}, t3{"virtual pad t3"}, t4{"virtual pad t4"}, t5{"virtual pad t5"};
    for (Virtual* v : {&t1, &t2, &t3, &t4, &t5}) attach(*v);
    const std::string log5 = dir + "/startup5.txt";
    capture(log5, [] {
        gamepads_init();
        frame();
    });
    check(lines_with(log5, "\" on port ") == 4 && lines_with(log5, "left out") == 1 && port(t1) == 1 && port(t2) == 2 &&
              port(t3) == 3 && port(t4) == 4 && port(t5) == 0 && player(t5) == -1,
          "five gamepads at launch: four on ports 1 to 4 and one left out, each said once");
    unplug({&t1, &t2, &t3, &t4, &t5});
    gamepads_shutdown();

    // A first gamepad with no player index, as one is that a gamecontrollerdb.txt maps, and
    // the next two with indexes 0 and 1. They are placed together, so each index is read before
    // any placement moves another's. The second and third land on ports 2 and 3, in SDL's order.
    power_on(1);
    Virtual u1{"virtual pad u1"}, u2{"virtual pad u2"}, u3{"virtual pad u3"};
    attach(u1, -1);
    attach(u2);
    attach(u3);
    const bool indexes = player(u1) == -1 && player(u2) == 0 && player(u3) == 1;
    gamepads_init();
    frame();
    check(indexes && port(u1) == 1 && port(u2) == 2 && port(u3) == 3,
          "three gamepads at launch, the first with no player index and the others holding 0 and 1: ports 1, 2 and 3 in SDL's order");
    unplug({&u1, &u2, &u3});
    gamepads_shutdown();
}

// ---- --pads=2.
void pads2() {
    // Read first before any frame: what si_init plugged in at power-on, since the last
    // frame, under --pads=1, left port 2 unplugged.
    power_on(1);
    frame();
    const bool was_out = !read(1).connected;
    power_on(2);
    check(was_out && read(0).connected && read(1).connected && at_rest(read(1)) && !read(2).connected && !read(3).connected,
          "--pads=2, before the first frame: ports 1 and 2 plugged in at rest, 3 and 4 not");
    gamepads_init();
    frame();
    check(read(0).connected && read(1).connected && at_rest(read(1)) && !read(2).connected && !read(3).connected,
          "--pads=2, no gamepad: ports 1 and 2 plugged in at rest, 3 and 4 not");
    check(read(1, 1000).buttons == PAD_B && !read(2, 1000).connected && !read(2, 1000).buttons,
          "--pads=2, no gamepad: a scripted press on port 2 lands, one on port 3 does nothing");

    Virtual h1{"virtual pad h1"}, h2{"virtual pad h2"}, h3{"virtual pad h3"}, h4{"virtual pad h4"};
    attach(h1);
    attach(h2);
    frame();
    check(port(h1) == 1 && port(h2) == 2, "--pads=2: two gamepads take ports 1 and 2");

    press(h2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    const PadState both = read(1, 1000);
    press(h2, SDL_GAMEPAD_BUTTON_SOUTH, false);
    check(both.connected && both.buttons == (PAD_A | PAD_B), "--pads=2: port 2's gamepad and a scripted press on port 2 both reach the guest");

    attach(h3);
    frame();
    check(port(h3) == 3 && read(2).connected && read(2, 1000).buttons == PAD_B,
          "--pads=2: a third gamepad plugs port 3 in, and the scripted press on port 3 now lands");

    unplug({&h2});
    check(read(1).connected && at_rest(read(1)) && read(1, 1000).buttons == PAD_B && port(h1) == 1 && port(h3) == 3,
          "--pads=2: port 2's gamepad pulled out: port 2 stays plugged in at rest, and the script still presses it");
    unplug({&h3});
    check(!read(2).connected && !read(2, 1000).buttons,
          "--pads=2: port 3's gamepad pulled out: port 3 reads unplugged, and the script's press there does nothing again");
    attach(h4);
    frame();
    check(port(h4) == 2, "--pads=2: the next gamepad (SDL index 1, freed with port 2) takes port 2 and drives it");
    unplug({&h1, &h4});
    gamepads_shutdown();
}

// Two gamepads pulled out and plugged back in the other order swap ports: each takes the
// port of the index SDL gives it.
void swap_order() {
    power_on(1);
    gamepads_init();
    Virtual sa{"virtual pad swap a"}, sb{"virtual pad swap b"};
    plug({&sa, &sb});
    const bool before = port(sa) == 1 && port(sb) == 2;
    unplug({&sa, &sb});
    plug({&sb, &sa});
    check(before && port(sb) == 1 && port(sa) == 2,
          "two gamepads pulled out and plugged back in the other order swap ports");
    unplug({&sa, &sb});
    gamepads_shutdown();
}

}  // namespace
