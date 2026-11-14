// ---- The Buttons view (overlay/overlay_bindings.cpp) and its press-to-bind, over the window and
// guest of tools/test_ports_menu_window.cpp's overlay_buttons, which calls it, checked on the
// ports as the game reads them and in pads.txt. Read in by tools/test_ports.cpp after the
// frontend, with GCN_OVERLAY.
#include "test_ports_support.h"

namespace {

void bindings_page_checks(Guest& guest, const std::string& dir) {
    const std::string file = dir + "/saves/pads.txt";
    gamepads_bindings_file(file.c_str());
    Virtual pad{"virtual pad buttons view"}, other{"virtual pad buttons other"};
    launch_in_menu(guest, dir, {&pad, &other});

    // Change buttons is right of Set player order, the page's first button.
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    pick_tab(guest, "Controllers");
    for (int i = 0; i < 20; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(overlay_bindings_shown() && overlay_open() && !overlay_bindings_listening(),
          "Change buttons, right of Set player order, puts up the Buttons view");

    // A D-pad press down from where the view opens is A's row. The A that starts press-to-bind
    // starts it as the A goes down, so it's still down as it starts, and passed over.
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    const bool on_press = overlay_bindings_listening();
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    host_frame(guest);
    const bool passed_over = on_press && overlay_bindings_listening();
    tap(guest, other, SDL_GAMEPAD_BUTTON_NORTH);
    const bool other_pad = overlay_bindings_listening();
    tap(guest, pad, SDL_GAMEPAD_BUTTON_WEST);
    const std::string text = read_file(file);
    check(passed_over && other_pad && !overlay_bindings_listening() && overlay_bindings_shown() &&
              has(text, "A = \"virtual pad buttons view\" gamepad west\n") &&
              has(text, "X = \"virtual pad buttons view\" gamepad south\n"),
          "press-to-bind passes over the A held as it starts and another gamepad's press, then takes west for A, "
          "X takes south, and pads.txt says so");
    check(has(overlay_bindings_said(), "A is now ") && has(overlay_bindings_said(), " X is now "),
          "the view says what A and X are now");

    overlay_bindings_listen(PadControl::B);
    g_host_ns += kBindListenNs;
    host_frame(guest);
    check(!overlay_bindings_listening() && has(overlay_bindings_said(), "Nothing was pressed in 5 seconds, so B is still "),
          "with nothing pressed for 5 s it gives up and says so");

    const uint64_t kept = file_id(file);
    overlay_bindings_listen(PadControl::B);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    const bool back = !overlay_bindings_listening() && overlay_open() && overlay_bindings_shown();
    overlay_bindings_listen(PadControl::B);
    tap(guest, other, SDL_GAMEPAD_BUTTON_BACK);
    const bool other_back = !overlay_bindings_listening() && overlay_open();
    overlay_bindings_listen(PadControl::B);
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    check(back && other_back && !overlay_bindings_listening() && overlay_open() && overlay_bindings_shown() &&
              file_id(file) == kept && has(overlay_bindings_said(), "B is still "),
          "Back on its gamepad or another, or Escape, cancels it with the menu still open and nothing saved");

    // Guide cancels as Back does, on its gamepad or another, and on one with Guide and no Back,
    // whose Guide would otherwise open and close the menu, it cancels and does nothing more.
    overlay_bindings_listen(PadControl::B);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_GUIDE);
    const bool guide = !overlay_bindings_listening() && overlay_open();
    overlay_bindings_listen(PadControl::B);
    tap(guest, other, SDL_GAMEPAD_BUTTON_GUIDE);
    const bool other_guide = !overlay_bindings_listening() && overlay_open();
    Virtual home{"home key pad buttons"};
    home.button_mask = 1u << SDL_GAMEPAD_BUTTON_SOUTH | 1u << SDL_GAMEPAD_BUTTON_EAST | 1u << SDL_GAMEPAD_BUTTON_WEST |
                       1u << SDL_GAMEPAD_BUTTON_NORTH | 1u << SDL_GAMEPAD_BUTTON_START | 1u << SDL_GAMEPAD_BUTTON_GUIDE;
    attach(home);
    host_frame(guest);
    overlay_bindings_open(home.id);
    overlay_bindings_listen(PadControl::B);
    tap(guest, home, SDL_GAMEPAD_BUTTON_GUIDE);
    check(guide && other_guide && !overlay_bindings_listening() && overlay_open() && overlay_bindings_shown() &&
              file_id(file) == kept && has(overlay_bindings_said(), "B is still ") &&
              !has(overlay_bindings_said(), "seconds"),
          "Guide on its gamepad or another cancels it too, and on a gamepad with no Back it doesn't close the menu");
    unplug({&home});

    // Press-to-bind's prompt, the gamepad gone as it waits, and a press that changes nothing.
    Virtual gone{"virtual pad buttons gone"};
    plug({&gone});
    host_frame(guest);
    overlay_bindings_open(gone.id);
    overlay_bindings_listen(PadControl::Y);
    host_frame(guest);
    detach(gone);
    host_frame(guest);
    check(!overlay_bindings_listening() && overlay_bindings_said() == "virtual pad buttons gone was disconnected, so nothing changed.",
          "a gamepad pulled out as it waits ends it, saying the gamepad's name");
    overlay_bindings_open(pad.id);
    overlay_bindings_listen(PadControl::A);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_WEST);
    check(file_id(file) == kept && has(overlay_bindings_said(), "A is still "),
          "the button A has already, pressed for A, changes nothing and saves nothing");

    // One way of a stick axis on a button, and all of it on a stick control, don't swap: the
    // view says the stick drives both. Then each is set back as it was.
    PadBinding before;
    gamepads_binding(pad.id, before);
    auto push_for = [&](PadControl c, int axis, Sint16 value) {
        overlay_bindings_listen(c);
        set_axis(pad, axis, value);
        host_frame(guest);
        set_axis(pad, axis, 0);
        host_frame(guest);
    };
    push_for(PadControl::DUp, SDL_GAMEPAD_AXIS_LEFTY, -32767);
    const std::string up = overlay_bindings_said();
    push_for(PadControl::StickX, SDL_GAMEPAD_AXIS_LEFTY, 32767);
    const std::string whole = overlay_bindings_said();
    push_for(PadControl::StickX, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    push_for(PadControl::StickY, SDL_GAMEPAD_AXIS_LEFTY, -32767);
    overlay_bindings_listen(PadControl::DUp);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    PadBinding after;
    check(up == "D-pad up is now Left stick up. Control stick up and down still moves with it." &&
              has(whole, "Control stick left and right is now Left stick.") &&
              has(whole, " Left stick up still presses D-pad up too.") && gamepads_binding(pad.id, after) &&
              after == before,
          "a stick's up on D-pad up says the control stick still moves with it, the stick's y on the control stick "
          "says it still presses D-pad up, and each sets back");

    // Two gamepads pads.txt can't tell apart both take a change, and the view says so.
    Virtual twin{"virtual pad buttons view"};
    plug({&twin});
    host_frame(guest);
    overlay_bindings_listen(PadControl::Start);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_NORTH);
    check(has(overlay_bindings_said(), "Player 3's controller has it too"),
          "a change one gamepad shares with another it can't be told from says which player has it too");
    unplug({&twin});

    // Test rumble asks a GameCube pad whose flag says it has none, as the game's rumble does.
    Virtual cube{"gc rumble pad buttons"};
    cube.vendor = 0x057e;
    cube.product = 0x0337;
    attach(cube);
    SDL_Joystick* held = SDL_OpenJoystick(cube.id);
    SDL_SetBooleanProperty(SDL_GetJoystickProperties(held), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
    host_frame(guest);
    overlay_bindings_open(cube.id);
    host_frame(guest);
    for (int i = 0; i < 40; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(cube.rumbles == 1 && overlay_bindings_said() == "Rumble sent to gc rumble pad buttons for 1 second.",
          "Test rumble, above Done, rumbles a GameCube pad SDL says has none, and says it was sent");
    SDL_CloseJoystick(held);
    unplug({&cube});
    overlay_bindings_open(pad.id);

    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    check(!overlay_bindings_shown() && overlay_open(), "B in the view goes back to the Controllers page");
    overlay_bindings_open(pad.id);
    overlay_bindings_listen(PadControl::Z);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    g_host_ns += 1000000000;  // past the close's fade, which still draws the view
    host_frame(guest);
    check(!overlay_open() && !overlay_bindings_listening() && !overlay_bindings_shown(),
          "F1 while it listens closes the menu, and the view and the listening end once it's faded");
    set_button(pad, SDL_GAMEPAD_BUTTON_WEST, true);
    host_frame(guest);
    const bool a_in_game = read(0).buttons == PAD_A;
    set_button(pad, SDL_GAMEPAD_BUTTON_WEST, false);
    host_frame(guest);
    check(a_in_game, "with the menu closed, the game reads the gamepad's west as A");

    // Escape and F1 in one frame: the cancel picks the row the focus goes back to, and the menu
    // closes before a draw takes it there. The view opened again has no row picked for it.
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    overlay_bindings_open(pad.id);
    overlay_bindings_listen(PadControl::B);
    push_key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, true);
    push_key(SDL_SCANCODE_F1, SDLK_F1, true);
    host_frame(guest);
    push_key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, false);
    push_key(SDL_SCANCODE_F1, SDLK_F1, false);
    host_frame(guest);
    const bool closed = !overlay_open();
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    overlay_bindings_open(pad.id);
    host_frame(guest);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(closed && overlay_bindings_shown() && !overlay_bindings_listening(),
          "a row press-to-bind ended on as the menu closed isn't picked when the view opens again");
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);

    // A save that fails says the change lasts until the game quits: a folder that can't be
    // written, with no pads.txt in it, so the file set anew reads as the map and north is a
    // change for Start.
    const std::string locked = dir + "/locked";
    mkdir(locked.c_str(), 0555);
    gamepads_bindings_file((locked + "/pads.txt").c_str());
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    overlay_bindings_open(pad.id);
    overlay_bindings_listen(PadControl::Start);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_NORTH);
    check(has(overlay_bindings_said(), "Start is now ") && has(overlay_bindings_said(), "until the game quits"),
          "a change pads.txt couldn't take says it lasts until the game quits");
    // Trigger click, above Reset to default, taken with A, one step right, and let go with A.
    for (int i = 0; i < 40; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    PadBinding clicked;
    check(gamepads_binding(pad.id, clicked) && clicked.trigger_click == 30473 &&
              overlay_bindings_said() == "Trigger click is now 93%. pads.txt isn't saved, so this lasts until the "
                                         "game quits.",
          "a slider moved a step and let go says what it's now, and that pads.txt isn't saved");
    for (int i = 0; i < 40; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(overlay_bindings_said() == "virtual pad buttons view has its default buttons, dead zones and trigger click "
                                     "again. pads.txt isn't saved, so this lasts until the game quits.",
          "Reset to default, right of Test rumble, says what it put back, and that pads.txt isn't saved");
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    chmod(locked.c_str(), 0755);

    load_controls(dir + "/view-controls.txt", "A = \"virtual pad buttons other\" button 0\n");
    check(!overlay_bindings_open(other.id), "a gamepad controls.txt binds can't be put up in the view");
    no_controls(dir);
    unplug({&pad, &other});
    gamepads_shutdown();
    gamepads_bindings_file(nullptr);
    gamepads_remember(nullptr);
}

}  // namespace
