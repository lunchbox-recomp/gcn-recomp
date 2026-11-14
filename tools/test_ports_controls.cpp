// ---- What the files beside the game bind (input_map.cpp, gamepads.cpp): a controls.txt that
// names a device or pins it to a port with `port <n>`, and a gamecontrollerdb.txt that makes a
// gamepad of a joystick SDL has no mapping for. Read in by tools/test_ports.cpp after the
// frontend.
#include "test_ports_support.h"

namespace {

// ---- controls.txt naming a gamepad.
void named(const std::string& dir) {
    const std::string controls = dir + "/controls.txt";
    Virtual bound{"virtual pad bound"};

    // One it names connects holding port 1's gamepad's index: SDL moves the gamepad's, and
    // it gets index 0 back although the newcomer takes no port.
    load_controls(controls, "A = \"virtual pad named\" button 0\n");
    power_on(1);
    gamepads_init();
    Virtual pa{"virtual pad pa"}, named{"virtual pad named"};
    attach(pa);
    frame();
    attach(named, 0);
    const bool pa_moved = player(pa) != 0;
    frame();
    check(pa_moved && port(pa) == 1,
          "a device controls.txt names connects holding port 1's gamepad's index: port 1's gets index 0 back");
    unplug({&pa, &named});
    gamepads_shutdown();

    load_controls(controls, "A = \"virtual pad bound\" button 0\n");
    power_on(1);
    gamepads_init();
    attach(bound);
    frame();
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, true);
    const PadState own = read(0);
    const bool own_alone = !read(1).buttons && !read(2).buttons && !read(3).buttons;
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, false);
    press(bound, SDL_GAMEPAD_BUTTON_WEST, true);
    bool mapped = false;
    for (int i = 0; i < 4; i++) mapped = mapped || read(i).buttons;
    press(bound, SDL_GAMEPAD_BUTTON_WEST, false);
    check(own.buttons == PAD_A && own_alone && !mapped,
          "a gamepad controls.txt names gets no port: its button 0 is A on port 1 by the file, and its B reaches no port");
    unplug({&bound});
    gamepads_shutdown();
    no_controls(dir);
}

// ---- controls.txt binding a gamepad's buttons and axes by SDL's gamepad names, as a block
// copied from pads.txt has them.
void gamepad_sources(const std::string& dir) {
    load_controls(dir + "/gamepad-controls.txt",
                  "# gamepad 00000000000000000000000000000000 -\n"
                  "A = \"virtual pad by name\" gamepad west\n"
                  "L = \"virtual pad by name\" gamepad lefttrigger threshold 0.9\n"
                  "dpad_up = \"virtual pad by name\" gamepad lefty-\n"
                  "stick_x = \"virtual pad by name\" gamepad rightx invert\n");
    power_on(1);
    gamepads_init();
    Virtual v{"virtual pad by name"};
    attach(v);
    frame();
    press(v, SDL_GAMEPAD_BUTTON_WEST, true);
    const PadState west = read(0);
    press(v, SDL_GAMEPAD_BUTTON_WEST, false);
    SDL_Joystick* j = SDL_GetJoystickFromID(v.id);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
    frame();
    const PadState pushed = read(0);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 0);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    frame();
    check(west.buttons == PAD_A, "a controls.txt line for a gamepad's west makes its west press A");
    check(pushed.trig_l == 255 && (pushed.buttons & PAD_L) && (pushed.buttons & PAD_UP) && pushed.stick_x < 0x80,
          "its left trigger is L and L's click, its left stick up is dpad_up, and its right stick turned round is stick_x");
    unplug({&v});
    gamepads_shutdown();
    no_controls(dir);
}

// ---- A `port <n>` line pins its device to the port. A device line with port 3, and a
// key line with port 5, which is refused.
void pins(const std::string& dir) {
    Virtual bound{"virtual pad bound"};
    power_on(1);  // port 3 starts out unplugged, whatever the section before left
    frame();
    load_controls(dir + "/controls.txt", "A = \"virtual pad bound\" button 0 port 3\nB = key X port 5\n"
                                         "stick_x = \"virtual pad bound\" axis 0 port 3\n");
    const bool port3_was_out = !read(2).connected;
    power_on(1);
    const bool before_frame = read(1).connected && read(2).connected && !read(3).connected;
    gamepads_init();
    frame();
    check(port3_was_out && before_frame && input_map_port_count() == 3 && read(1).connected && read(2).connected &&
              !read(3).connected,
          "controls.txt pins a device to port 3 (a port 5 line refused): ports 1 to 3 plugged in, before the first frame too");
    Virtual x{"virtual pad x"}, y{"virtual pad y"};
    attach(bound);
    const int bound_sdl = player(bound);
    frame();
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, true);
    const PadState p3 = read(2);
    const bool alone = !read(0).buttons && !read(1).buttons && !read(3).buttons;
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, false);
    check(bound_sdl == 0 && p3.buttons == PAD_A && alone && player(bound) == 2 && bound.player == 2,
          "a gamepad pinned to port 3, given index 0 by SDL, drives port 3 alone, and its player index follows the pin to 2");
    attach(x);
    frame();
    check(port(x) == 1, "the next gamepad (SDL index 0) takes port 1");
    attach(y, 2);
    frame();
    check(port(y) == 2 && player(bound) == 2 && bound.player == 2,
          "a gamepad with player index 2 does not take pinned port 3: it lands on port 2, and the pinned device keeps index 2");
    set_motor(2, true);
    frame();
    const int ffb_on3 = g_ffb_motor;
    set_motor(2, false);
    set_motor(0, true);
    frame();
    const int ffb_on1 = g_ffb_motor;
    set_motor(0, false);
    frame();
    check(ffb_on3 == 1 && ffb_on1 == 0 && x.rumbles == 1 && y.rumbles == 0,
          "the wheel's force feedback follows port 3's motor, the steering line's port, and port 1's motor runs port 1's gamepad");

    // Ports 1, 2 and 4 taken and port 3 pinned: a gamepad holding index 2 is left out, and
    // only the pin's re-set on another device's arrival gives the pinned device index 2 back.
    Virtual q4{"virtual pad q4"}, q5{"virtual pad q5"};
    attach(q4);
    frame();
    const bool q4_on4 = port(q4) == 4;
    attach(q5, 2);
    const bool bound_moved = player(bound) != 2;
    frame();
    check(q4_on4 && bound_moved && player(q5) == -1 && player(bound) == 2 && bound.player == 2 && port(x) == 1 &&
              port(y) == 2 && port(q4) == 4,
          "ports 1, 2 and 4 taken and port 3 pinned: a gamepad holding index 2 is left out, and the pinned device gets 2 back");
    unplug({&bound, &x, &y, &q4, &q5});
    gamepads_shutdown();
}

// ---- The other pin scenarios, each with its own controls.txt and pads.
void pins_more(const std::string& dir) {
    const std::string controls = dir + "/controls.txt";
    Virtual bound{"virtual pad bound"}, x{"virtual pad x"}, y{"virtual pad y"}, z{"virtual pad z"};

    // At launch, the pinned device first in SDL's list, and a gamepad whose index is the
    // pinned port's.
    load_controls(controls, "A = \"virtual pad bound\" button 0 port 2\n");
    power_on(1);
    attach(bound);
    attach(z);
    const bool sdl_order = player(bound) == 0 && player(z) == 1;
    gamepads_init();
    frame();
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, true);
    const PadState p2 = read(1);
    const bool alone2 = !read(0).buttons && !read(2).buttons && !read(3).buttons;
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, false);
    check(sdl_order && port(z) == 1 && p2.buttons == PAD_A && alone2 && player(bound) == 1 && bound.player == 1,
          "at launch, a gamepad with the pinned port's index 1 takes port 1 instead; the device pinned to port 2 drives it with index 1");
    unplug({&bound, &z});
    gamepads_shutdown();

    // `port 1` pins port 1 too: a line that says it keeps gamepads off port 1.
    load_controls(controls, "A = \"virtual pad bound\" button 0 port 1\n");
    power_on(1);
    gamepads_init();
    attach(bound);
    frame();
    attach(x);
    const bool x_sdl = player(x) == 1;
    frame();
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, true);
    const PadState pin1 = read(0);
    press(bound, SDL_GAMEPAD_BUTTON_SOUTH, false);
    attach(y, 0);
    frame();
    check(x_sdl && pin1.buttons == PAD_A && port(x) == 2 && port(y) == 3 && player(bound) == 0 && bound.player == 0,
          "`port 1` pins port 1: the pinned device drives it with index 0, and gamepads, even one holding index 0, take ports 2 and 3");
    unplug({&bound, &x, &y});
    gamepads_shutdown();

    // Two plain joysticks pinned to port 2, as a wheel and its pedals would be: the first
    // shows index 1 and the second none, so no slot is taken that a gamepad needs.
    load_controls(controls, "A = \"virtual stick w\" button 0 port 2\nB = \"virtual stick p\" button 1 port 2\n");
    power_on(1);
    gamepads_init();
    Virtual sw{"virtual stick w", false}, sp{"virtual stick p", false}, gp{"virtual pad gp"};
    attach(sw);
    frame();
    attach(sp);
    frame();
    const bool first_shows = player(sw) == 1 && sw.player == 1 && player(sp) == -1;
    attach(gp);
    frame();
    check(first_shows && port(gp) == 1 && player(sw) == 1 && player(sp) == -1,
          "two plain joysticks pinned to port 2: the first has index 1, the second none, and a gamepad then takes port 1");
    unplug({&sw, &sp, &gp});
    gamepads_shutdown();

    // An unpinned plain joystick first, then a gamepad: the joystick is no gamepad, so it
    // does not push the lone gamepad off port 1. SDL gives a plain joystick no player index
    // (-1) and the gamepad index 0, which is why: the joystick holds no index to take.
    no_controls(dir);
    power_on(1);
    gamepads_init();
    Virtual loose{"virtual stick loose", false}, lone{"virtual pad lone"};
    attach(loose);
    frame();
    attach(lone);
    const int loose_index = player(loose);
    const int lone_index = player(lone);
    frame();
    check(loose_index == -1 && lone_index == 0 && port(lone) == 1,
          "an unpinned plain joystick attached first does not push a lone gamepad off port 1");
    unplug({&loose, &lone});
    gamepads_shutdown();

    // A port number past int range must not wrap into 1 to 4: the whole line is refused.
    load_controls(controls, "A = \"virtual pad bound\" button 0 port 4294967299\n");
    power_on(1);
    gamepads_init();
    attach(bound);
    frame();
    check(input_map_port_count() == 1 && !input_map_pins(0) && !input_map_pins(1) && !input_map_pins(2) &&
              !input_map_pins(3) && port(bound) == 1 && player(bound) == 0,
          "a controls.txt line with port 4294967299 is refused: no port is pinned and none is plugged in");
    unplug({&bound});
    gamepads_shutdown();

    // A device number past int range must not wrap into a real one: the name matches no device.
    load_controls(controls, "A = \"virtual pad bound #4294967297\" button 0\n");
    power_on(1);
    gamepads_init();
    attach(bound);
    frame();
    check(!input_map_binds(bound.id) && port(bound) == 1,
          "a controls.txt device named \"#4294967297\" names no device, so the first one is not bound");
    unplug({&bound});
    gamepads_shutdown();

    // A key line's port plugs the port in but pins nothing: a gamepad holding its index
    // still takes it.
    load_controls(controls, "B = key X port 3\n");
    power_on(1);
    const bool key_plugged = read(1).connected && read(2).connected && !read(3).connected;
    gamepads_init();
    Virtual k1{"virtual pad k1"}, k2{"virtual pad k2"}, k3{"virtual pad k3"};
    attach(k1, 0);
    attach(k2, 1);
    attach(k3, 2);
    frame();
    check(key_plugged && port(k1) == 1 && port(k2) == 2 && port(k3) == 3,
          "a key line with port 3 plugs ports 1 to 3 in before the first frame, but pins nothing: a gamepad with index 2 takes port 3");
    unplug({&k1, &k2, &k3});
    gamepads_shutdown();
    no_controls(dir);
}

// How many mappings SDL has.
int mapping_count() {
    int n = 0;
    SDL_free(SDL_GetGamepadMappings(&n));
    return n;
}

// ---- gamecontrollerdb.txt beside the executable, the one mappings file the frontend
// loads (SDL_GAMECONTROLLERCONFIG_FILE is SDL's own, read at SDL_Init).
void mappings(const std::string& dir) {
    power_on(1);
    const char* base = SDL_GetBasePath();
    const std::string beside = std::string(base ? base : "./") + "gamecontrollerdb.txt";
    if (plat_readable(beside.c_str())) {
        printf("%s exists: not touching it\n", beside.c_str());
        check(false, (beside + " is there already: the mappings checks did not run; move it away and run again").c_str());
        return;
    }
    Virtual stick{"virtual stick", false};
    attach(stick);
    frame();
    const std::string guid = guid_of(stick);
    const std::string platform = SDL_GetPlatform();
    const int before = mapping_count();

    const std::string quiet = dir + "/mappings-none.txt";
    capture(quiet, [] { gamepads_load_mappings(); });
    check(mapping_count() == before && !SDL_IsGamepad(stick.id) && read_file(quiet).empty(),
          "no gamecontrollerdb.txt beside the executable: nothing loaded, nothing said");

    write_file(beside, "");
    gamepads_load_mappings();
    check(mapping_count() == before && !SDL_IsGamepad(stick.id), "an empty gamecontrollerdb.txt changes nothing");

    write_file(beside, "this is not a mapping\n"
                       "platform:" + platform + ",\n" +
                       guid + "platform:" + platform + ",\n" +
                       guid + ",virtual stick,a:b0,b:b1,leftx:a0,lefty:a1,\n" +
                       guid + ",virtual stick,a:b0,b:b1,leftx:a0,lefty:a1,platform:Windows,\n" +
                       guid + ",virtual stick,a:b0,b:b1,leftx:a0,lefty:a1,platform:" + platform);
    gamepads_load_mappings();
    check(mapping_count() == before && !SDL_IsGamepad(stick.id),
          "a malformed gamecontrollerdb.txt (no fields, no platform, another platform, a cut-off line) changes nothing");

    // A line for this platform, then one for Windows with the same GUID, which SDL must skip
    // (it would replace the first, and its mapping has no b:b1).
    write_file(beside, "# made up for this test\n" + guid + ",virtual stick,a:b0,b:b1,x:b2,y:b3,leftx:a0,lefty:a1,platform:" +
                           platform + ",\n" + guid + ",virtual stick,a:b0,platform:Windows,\n");
    gamepads_load_mappings();
    char* mapping = SDL_GetGamepadMappingForGUID(SDL_GetJoystickGUIDForID(stick.id));
    const bool this_platform = mapping && std::string(mapping).find("b:b1") != std::string::npos;
    SDL_free(mapping);
    gamepads_init();
    frame();
    check(mapping_count() == before + 1 && this_platform && SDL_IsGamepad(stick.id) && port(stick) == 1,
          "a joystick SDL has no mapping for, mapped by gamecontrollerdb.txt (its line for this platform alone) before the "
          "gamepads are taken, gets port 1");
    // Copied, then removed: a rename fails when the work dir is on another filesystem.
    const bool kept = write_file(dir + "/gamecontrollerdb.beside.txt", read_file(beside));
    check(kept && std::remove(beside.c_str()) == 0,
          "the test's gamecontrollerdb.txt is copied to the work dir and removed from beside the executable");
    unplug({&stick});
    gamepads_shutdown();
}

}  // namespace
