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

    // The A that starts press-to-bind is still down as it starts, so it's passed over.
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_UpdateJoysticks();  // SDL takes the press in, as a host frame before this would have
    overlay_bindings_listen(PadControl::A);
    host_frame(guest);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    host_frame(guest);
    const bool passed_over = overlay_bindings_listening();
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

    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    check(!overlay_bindings_shown() && overlay_open(), "B in the view goes back to the Controllers page");
    overlay_bindings_open(pad.id);
    overlay_bindings_listen(PadControl::Z);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    check(!overlay_open() && !overlay_bindings_listening() && !overlay_bindings_shown(),
          "F1 while it listens closes the menu, and the view and the listening end with it");
    set_button(pad, SDL_GAMEPAD_BUTTON_WEST, true);
    host_frame(guest);
    const bool a_in_game = read(0).buttons == PAD_A;
    set_button(pad, SDL_GAMEPAD_BUTTON_WEST, false);
    host_frame(guest);
    check(a_in_game, "with the menu closed, the game reads the gamepad's west as A");

    load_controls(dir + "/view-controls.txt", "A = \"virtual pad buttons other\" button 0\n");
    check(!overlay_bindings_open(other.id), "a gamepad controls.txt binds can't be put up in the view");
    no_controls(dir);
    unplug({&pad, &other});
    gamepads_shutdown();
    gamepads_bindings_file(nullptr);
    gamepads_remember(nullptr);
}

}  // namespace
