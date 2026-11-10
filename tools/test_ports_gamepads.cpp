// ---- What each gamepad is to the run: the lines the run's output has for it, its rumble, and
// its face buttons by their labels, on the desktop and in the headset's default layout. Read in
// by tools/test_ports.cpp after the frontend.
#include "test_ports_support.h"
#include "vr_game.h"

namespace {

// `v`'s `axis` pushed to `value`, then a frame.
void push(const Virtual& v, int axis, Sint16 value) {
    if (SDL_Joystick* j = v.id ? SDL_GetJoystickFromID(v.id) : nullptr) SDL_SetJoystickVirtualAxis(j, axis, value);
    frame();
}

// ---- What the run's output says of each gamepad: what SDL knows of it as it lands on a
// port, what it sent while there when it leaves (pulled out, or still there at shutdown),
// and why its rumble failed, each reason once while it's there.
void said(const std::string& dir) {
    power_on(1);
    gamepads_init();
    Virtual a{"virtual pad said"}, quiet{"virtual pad quiet"};
    quiet.can_rumble = false;
    const std::string landed = dir + "/said-landed.txt";
    capture(landed, [&] { plug({&a, &quiet}); });
    check(lines_with(landed, "gamepads: \"virtual pad said\" on port 1\n"
                             "gamepads: port 1 is standard, unknown, 0000:0000, rumble yes, south label A, east label B\n") == 1 &&
              lines_with(landed, "gamepads: \"virtual pad quiet\" on port 2\n"
                                 "gamepads: port 2 is standard, unknown, 0000:0000, rumble no, south label A, east label B\n") == 1,
          "a gamepad that lands on a port is said, after its port line, with its type, connection, USB IDs, rumble and face labels");

    // Three presses of south and one each of east and misc6, which no port takes, each held
    // for two frames so that a count of frames held would be twice the presses; the left
    // stick to both ends, the right stick down a little and the left trigger all the way.
    for (int k = 0; k < 3; k++) {
        press(a, SDL_GAMEPAD_BUTTON_SOUTH, true);
        frame();
        press(a, SDL_GAMEPAD_BUTTON_SOUTH, false);
    }
    for (const int b : {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_MISC6}) {
        press(a, b, true);
        frame();
        press(a, b, false);
    }
    for (const Sint16 x : {-32768, 32767, 0}) push(a, SDL_GAMEPAD_AXIS_LEFTX, x);
    push(a, SDL_GAMEPAD_AXIS_RIGHTY, 1000);
    push(a, SDL_GAMEPAD_AXIS_RIGHTY, 0);
    // A virtual trigger's axis runs from -32768, released, to 32767, as the gamepad's 0 to 32767.
    push(a, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    push(a, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    motor(0);
    motor(0);
    const std::string out = dir + "/said-out.txt";
    capture(out, [&] { unplug({&a}); });
    check(lines_with(out, "gamepads: \"virtual pad said\" pulled out of port 1\n"
                          "gamepads: port 1 sent south 3, east 1, misc6 1, left stick x -32768 to 32767 y 0 to 0, right stick "
                          "x 0 to 0 y 0 to 1000, triggers left 0 to 32767 right 0 to 0\n"
                          "gamepads: port 1 rumble on 2 times, 0 of 4 calls failed\n") == 1,
          "a gamepad pulled out says, after its pulled-out line, each button it pressed and how often, each axis's extents and "
          "its rumble");

    // A rumble that fails says each of its reasons once while the gamepad is on its port, not
    // at each call or each on: on for five frames and off, on for three and off, then on for
    // two, accepted, and off, refused for another reason.
    Virtual f{"virtual pad refusing"};
    f.refuses = true;
    attach(f);
    frame();
    const std::string failing = dir + "/said-failing.txt";
    capture(failing, [&] {
        for (const int on : {5, 3}) {
            set_motor(0, true);
            for (int k = 0; k < on; k++) frame();
            set_motor(0, false);
            frame();
        }
        f.refuses = false;
        set_motor(0, true);
        frame();
        frame();
        f.refuses = true;
        f.why = "virtual pad refuses to stop";
        set_motor(0, false);
        frame();
        frame();
    });
    capture(dir + "/said-failing-out.txt", [&] { unplug({&f}); });
    check(lines_with(failing, "gamepads: port 1 rumble failed: virtual pad refuses rumble\n") == 1 &&
              lines_with(failing, "gamepads: port 1 rumble failed: virtual pad refuses to stop\n") == 1 &&
              lines_with(failing, "rumble failed") == 2 &&
              lines_with(dir + "/said-failing-out.txt", "gamepads: port 1 rumble on 3 times, 9 of 13 calls failed\n") == 1,
          "a rumble that fails says each reason once while the gamepad is on its port, and the pull-out line counts the "
          "failed calls");
    // Plugged in again, it lands afresh, so a reason it gave before is said again.
    f.why = "virtual pad refuses rumble";
    attach(f);
    frame();
    const std::string again = dir + "/said-failing-again.txt";
    capture(again, [] { motor(0); });
    capture(dir + "/said-failing-again-out.txt", [&] { unplug({&f}); });
    check(lines_with(again, "gamepads: port 1 rumble failed: virtual pad refuses rumble\n") == 1,
          "a gamepad plugged in again says why its rumble fails again");

    // The game's motor on port 2 on and off three times, where the gamepad SDL says has no
    // rumble is: it's never asked for one.
    const std::string none = dir + "/said-no-rumble.txt";
    capture(none, [] {
        for (int k = 0; k < 3; k++) motor(1);
    });

    // At shutdown each gamepad still on a port says what it sent: one with a press, one with none.
    Virtual b{"virtual pad shut"};
    attach(b);
    frame();
    press(b, SDL_GAMEPAD_BUTTON_WEST, true);
    frame();
    press(b, SDL_GAMEPAD_BUTTON_WEST, false);
    const std::string shut = dir + "/said-shutdown.txt";
    capture(shut, [] { gamepads_shutdown(); });
    const char* const still = "left stick x 0 to 0 y 0 to 0, right stick x 0 to 0 y 0 to 0, triggers left 0 to 0 right 0 to 0\n";
    check(lines_with(shut, (std::string("gamepads: port 1 sent west 1, ") + still).c_str()) == 1 &&
              lines_with(shut, (std::string("gamepads: port 2 sent ") + still).c_str()) == 1 &&
              lines_with(shut, "gamepads: port 1 rumble on 0 times, 0 of 0 calls failed\n") == 1,
          "at shutdown each gamepad still on a port says what it sent, a press or none, and its rumble");
    check(lines_with(none, "rumble failed") == 0 &&
              lines_with(shut, "gamepads: port 2 rumble on 3 times, 0 of 0 calls failed\n") == 1,
          "a gamepad SDL says has no rumble makes no rumble call and says no failure, and its line counts the game's ons");
    unplug({&b, &quiet});
}

// ---- A GameCube pad is asked for rumble whatever SDL says of it. SDL's GameCube driver
// says a pad has none if it lands before the adapter's second cable is plugged in, and never
// says otherwise, though it rumbles the pad once the cable is in.
void gamecube_rumble(const std::string& dir) {
    power_on(1);
    gamepads_init();
    Virtual v{"virtual gamecube adapter"};
    v.vendor = 0x057e;
    v.product = 0x0337;
    attach(v);
    // The test's own hold on the device, so the capability it clears is still there when the
    // frontend opens it.
    SDL_Joystick* j = SDL_OpenJoystick(v.id);
    SDL_SetBooleanProperty(SDL_GetJoystickProperties(j), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
    const std::string landed = dir + "/gamecube-landed.txt", out = dir + "/gamecube-out.txt";
    capture(landed, [] { frame(); });
    SDL_CloseJoystick(j);
    capture(out, [] {
        for (int k = 0; k < 3; k++) motor(0);
        gamepads_shutdown();
    });
    check(lines_with(landed, "gamepads: port 1 is gamecube, unknown, 057e:0337, rumble no, south label A, east label X\n") == 1 &&
              v.rumbles == 3 && v.stops == 3 && lines_with(out, "gamepads: port 1 rumble on 3 times, 0 of 6 calls failed\n") == 1,
          "a GameCube pad SDL says has no rumble is still asked for it, and rumbles");
    detach(v);

    // One with no rumble at all, as a WaveBird or an input-only pad SDL types as GameCube is,
    // is refused at each on, and says so once. SDL takes each off as done without asking the
    // driver, since the refused on left its rumble at 0 (SDL_RumbleJoystick).
    power_on(1);
    gamepads_init();
    Virtual w{"virtual gamecube no rumble"};
    w.vendor = 0x057e;
    w.product = 0x0337;
    w.can_rumble = false;
    attach(w);
    frame();
    const std::string refused = dir + "/gamecube-refused.txt";
    capture(refused, [] {
        for (int k = 0; k < 3; k++) motor(0);
        gamepads_shutdown();
    });
    check(lines_with(refused, "rumble failed") == 1 &&
              lines_with(refused, "gamepads: port 1 rumble failed: That operation is not supported\n") == 1 &&
              lines_with(refused, "gamepads: port 1 rumble on 3 times, 3 of 6 calls failed\n") == 1,
          "a GameCube pad with no rumble says SDL's refusal once, and its line counts each refused call");
    detach(w);
}

// ---- GameCube A, B, X and Y are the face buttons labeled A, B, X and Y, as Nintendo's own
// GameCube games on its consoles have them, and a pad whose faces carry no letters has them
// by Xbox position. Each pad lands on port 1, and each of its face buttons is pressed alone.
void labels(const std::string& dir) {
    struct Case {
        const char* name;
        Uint16 vendor, product;
        const char* line;            // the placement line
        uint16_t south, east, west, north;  // what each face button is to the game
    };
    static const Case kCases[] = {
        {"virtual switch pro", 0x057e, 0x2009, "gamepads: port 1 is switchpro, unknown, 057e:2009, rumble yes, south label B, east label A\n",
         PAD_B, PAD_A, PAD_Y, PAD_X},
        {"virtual xbox one", 0x045e, 0x02ea, "gamepads: port 1 is xboxone, unknown, 045e:02ea, rumble yes, south label A, east label B\n",
         PAD_A, PAD_B, PAD_X, PAD_Y},
        {"virtual dualsense", 0x054c, 0x0ce6, "gamepads: port 1 is ps5, unknown, 054c:0ce6, rumble yes, south label cross, east label circle\n",
         PAD_A, PAD_B, PAD_X, PAD_Y},
    };
    for (const Case& c : kCases) {
        power_on(1);
        gamepads_init();
        Virtual v{c.name};
        v.vendor = c.vendor;
        v.product = c.product;
        const std::string landed = dir + "/labels-landed.txt";
        capture(landed, [&] {
            attach(v);
            frame();
        });
        // What port 1 reads while `button` alone is held.
        auto sends = [&](int button) {
            press(v, button, true);
            const uint16_t got = read(0).buttons;
            press(v, button, false);
            return got;
        };
        const bool faces = sends(SDL_GAMEPAD_BUTTON_SOUTH) == c.south && sends(SDL_GAMEPAD_BUTTON_EAST) == c.east &&
                           sends(SDL_GAMEPAD_BUTTON_WEST) == c.west && sends(SDL_GAMEPAD_BUTTON_NORTH) == c.north;
        check(lines_with(landed, c.line) == 1 && faces,
              (std::string(c.name) + ": its face buttons reach the game by their labels, or by Xbox position for "
                                     "symbols, as its placement line's labels say").c_str());
        unplug({&v});
        gamepads_shutdown();
    }
}

// ---- The headset's default layout (vr::default_gamepad_layout) takes GameController's
// buttons, A south, B east, X west and Y north, to the GameCube's A, B, X and Y, as the
// desktop's gamepads have them by their labels.
void headset_layout() {
    auto sends = [](uint32_t button) {
        vr::Gamepad g;
        g.buttons = button;
        PadState p;
        vr::default_gamepad_layout(g, p);
        return p.buttons;
    };
    check(sends(vr::Gamepad::South) == PAD_A && sends(vr::Gamepad::East) == PAD_B &&
              sends(vr::Gamepad::West) == PAD_X && sends(vr::Gamepad::North) == PAD_Y,
          "the headset's default layout: south is GameCube A, east B, west X and north Y");
}

}  // namespace
