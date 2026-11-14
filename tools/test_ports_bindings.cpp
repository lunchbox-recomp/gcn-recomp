// ---- Each gamepad's binding (pad_bindings.h, gamepads.h): saves/pads.txt under the work dir,
// read as a gamepad lands and changed through gamepads_bind, checked on the ports as the game
// reads them. Read in by tools/test_ports.cpp after the frontend.
#include "test_ports_support.h"

namespace {

// Port 1's buttons while `button` alone is held on `v`.
uint16_t sends(const Virtual& v, int button) {
    press(v, button, true);
    const uint16_t got = read(0).buttons;
    press(v, button, false);
    return got;
}

void bindings_merged(const std::string& dir) {
    const std::string file = dir + "/saves/pads.txt";
    gamepads_bindings_file(file.c_str());
    power_on(1);
    gamepads_init();
    Virtual v{"virtual pad bind"};
    attach(v);
    frame();
    const bool map = sends(v, SDL_GAMEPAD_BUTTON_SOUTH) == PAD_A && sends(v, SDL_GAMEPAD_BUTTON_WEST) == PAD_X &&
                     sends(v, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) == PAD_Z;
    check(map && !exists_path(file), "with no pads.txt a gamepad has the map, and playing writes no pads.txt");
    const std::string guid = guid_of(v);
    unplug({&v});
    gamepads_shutdown();

    // A block written by hand: west is A, and south X.
    const std::string name = "\"virtual pad bind\" gamepad ";
    write_file(file, "# gamepad " + guid + " -\nA = " + name + "west\nX = " + name + "south\n");
    power_on(1);
    gamepads_init();
    attach(v);
    frame();
    check(sends(v, SDL_GAMEPAD_BUTTON_WEST) == PAD_A && sends(v, SDL_GAMEPAD_BUTTON_SOUTH) == PAD_X &&
              sends(v, SDL_GAMEPAD_BUTTON_EAST) == PAD_B,
          "pads.txt binding west to A: a west press is A on the port, south is X, and what it leaves out is the map's");

    PadBinding b;
    const bool got = gamepads_binding(v.id, b);
    pad_binding_set(b, PadControl::B, {false, SDL_GAMEPAD_BUTTON_NORTH, 0, false});
    const bool bound = gamepads_bind(v.id, b);
    const std::string text = read_file(file);
    check(got && bound && sends(v, SDL_GAMEPAD_BUTTON_NORTH) == PAD_B && sends(v, SDL_GAMEPAD_BUTTON_EAST) == PAD_Y &&
              has(text, "B = \"virtual pad bind\" gamepad north\n") && has(text, "Y = \"virtual pad bind\" gamepad east\n"),
          "north bound to B takes effect at once, Y takes east, which B had, and pads.txt lists every control");

    Virtual w{"virtual pad bind other"};
    attach(w);
    frame();
    gamepads_swap(0, 1);
    frame();
    press(v, SDL_GAMEPAD_BUTTON_NORTH, true);
    const bool follows = read(1).buttons == PAD_B && !read(0).buttons;
    press(v, SDL_GAMEPAD_BUTTON_NORTH, false);
    check(follows && sends(w, SDL_GAMEPAD_BUTTON_NORTH) == PAD_Y, "the binding goes with its gamepad to another port");
    gamepads_swap(0, 1);
    unplug({&w});

    gamepads_shutdown();
    power_on(1);
    gamepads_init();
    frame();
    check(sends(v, SDL_GAMEPAD_BUTTON_NORTH) == PAD_B, "at the next launch it has the binding pads.txt saved");
    check(gamepads_reset_binding(v.id) && sends(v, SDL_GAMEPAD_BUTTON_SOUTH) == PAD_A &&
              !has(read_file(file), "# gamepad " + guid),
          "set back, it has the map again and its block is gone from pads.txt");
    check(!gamepads_binding(kKeyboard, b) && !gamepads_bind(12345, b), "the keyboard and a device that isn't here have none");
    unplug({&v});
    gamepads_shutdown();

    load_controls(dir + "/bind-controls.txt", "A = \"virtual pad bind\" button 0\n");
    power_on(1);
    gamepads_init();
    attach(v);
    frame();
    const uint64_t before = file_id(file);
    check(!gamepads_binding(v.id, b) && !gamepads_bind(v.id, b) && file_id(file) == before,
          "a gamepad controls.txt binds has no binding here, and none is saved for it");
    unplug({&v});
    gamepads_shutdown();
    no_controls(dir);
    gamepads_bindings_file(nullptr);
}

}  // namespace
