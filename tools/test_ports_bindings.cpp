// ---- Each gamepad's binding (pad_bindings.h, gamepads.h): saves/pads.txt under the work dir,
// read as a gamepad lands and changed through gamepads_bind, checked on the ports as the game
// reads them. Read in by tools/test_ports.cpp after the frontend.
#include "test_ports_support.h"

namespace {

// Port `on` + 1's buttons while `button` alone is held on `v`.
uint16_t sends(const Virtual& v, int button, int on = 0) {
    press(v, button, true);
    const uint16_t got = read(on).buttons;
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
    PadBinding same;
    check(gamepads_binding(v.id, same) && gamepads_bind(v.id, same) == Bound::unchanged &&
              gamepads_reset_binding(v.id) == Bound::unchanged && !exists_path(file),
          "binding a gamepad to what it has already, or resetting one with nothing saved, changes nothing and writes "
          "no pads.txt");
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
    const bool bound = gamepads_bind(v.id, b) == Bound::saved;
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
    unplug({&v});
    plug({&v});
    check(sends(v, SDL_GAMEPAD_BUTTON_NORTH) == PAD_B, "pulled out and plugged in again, it still has its binding");

    // Two gamepads pads.txt can't tell apart, the same GUID and no serial, share a block.
    Virtual twin{"virtual pad bind"}, apart{"virtual pad bind apart"};
    plug({&twin, &apart});
    const std::vector<int> sharing = gamepads_sharing(v.id);
    PadBinding t;
    gamepads_binding(twin.id, t);
    pad_binding_set(t, PadControl::A, {false, SDL_GAMEPAD_BUTTON_EAST, 0, false});
    const bool twin_bound = gamepads_bind(twin.id, t) == Bound::saved;
    check(guid_of(twin) == guid && sharing == std::vector<int>{0, 1} && gamepads_sharing(apart.id) == std::vector<int>{2} &&
              twin_bound && sends(v, SDL_GAMEPAD_BUTTON_EAST) == PAD_A && sends(twin, SDL_GAMEPAD_BUTTON_EAST, 1) == PAD_A &&
              sends(apart, SDL_GAMEPAD_BUTTON_EAST, 2) == PAD_B,
          "a binding given to one of two gamepads with the same GUID and no serial is both's at once, and not another's");
    gamepads_binding(twin.id, t);
    pad_binding_set(t, PadControl::A, {false, SDL_GAMEPAD_BUTTON_WEST, 0, false});
    gamepads_bind(twin.id, t);
    unplug({&twin, &apart});

    gamepads_shutdown();
    power_on(1);
    gamepads_init();
    frame();
    check(sends(v, SDL_GAMEPAD_BUTTON_NORTH) == PAD_B, "at the next launch it has the binding pads.txt saved");
    check(gamepads_reset_binding(v.id) == Bound::saved && sends(v, SDL_GAMEPAD_BUTTON_SOUTH) == PAD_A &&
              !has(read_file(file), "# gamepad " + guid),
          "set back, it has the map again and its block is gone from pads.txt");
    check(!gamepads_binding(kKeyboard, b) && gamepads_bind(12345, b) == Bound::refused, "the keyboard and a device that isn't here have none");
    unplug({&v});
    gamepads_shutdown();

    load_controls(dir + "/bind-controls.txt", "A = \"virtual pad bind\" button 0\n");
    power_on(1);
    gamepads_init();
    attach(v);
    frame();
    const uint64_t before = file_id(file);
    check(!gamepads_binding(v.id, b) && gamepads_bind(v.id, b) == Bound::refused && file_id(file) == before,
          "a gamepad controls.txt binds has no binding here, and none is saved for it");
    unplug({&v});
    gamepads_shutdown();
    no_controls(dir);

    // A pads.txt that's there and can't be read: nothing is said until a gamepad needs it, and
    // then once, and a change is this session's alone, the file left as it was.
    write_file(file, "# gamepad " + guid + " -\nA = " + name + "north\n");
    if (chmod(file.c_str(), 0) != 0 || access(file.c_str(), R_OK) == 0) {
        chmod(file.c_str(), 0644);
        check(false, "pads.txt made unreadable (this check can't run as root)");
    } else {
        const uint64_t locked = file_id(file);
        const std::string quiet = dir + "/pads-unread-none.txt", said = dir + "/pads-unread.txt";
        capture(quiet, [] {
            power_on(1);
            gamepads_init();
            frame();
        });
        Bound kept = Bound::refused;
        capture(said, [&] {
            attach(v);
            frame();
            gamepads_binding(v.id, b);
            pad_binding_set(b, PadControl::B, {false, SDL_GAMEPAD_BUTTON_NORTH, 0, false});
            kept = gamepads_bind(v.id, b);
        });
        check(read_file(quiet).empty() && lines_with(said, "can't read it") == 1 && kept == Bound::unsaved &&
                  sends(v, SDL_GAMEPAD_BUTTON_NORTH) == PAD_B && sends(v, SDL_GAMEPAD_BUTTON_SOUTH) == PAD_A &&
                  file_id(file) == locked,
              "an unreadable pads.txt: a launch with no gamepad says nothing, the first gamepad says it once with the "
              "map, and a change is kept for the session but never written over the file");
        chmod(file.c_str(), 0644);
        unplug({&v});
        gamepads_shutdown();
    }
    gamepads_bindings_file(nullptr);
}

}  // namespace
