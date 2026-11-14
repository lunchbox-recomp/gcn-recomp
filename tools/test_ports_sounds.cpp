// ---- The menu's sounds (ui_sound.h) as the menu raises them, over a window from SDL's dummy
// video driver with menu_sounds on: a swell as it opens and as it closes, a tick as the focus
// moves, but none as an open or a page turn puts it on the first item, a confirm as a button is
// pressed, a swish to the side a page turns to, a buzz past the last page, and a back as Escape
// leaves Restart?, the press-A order, press-to-bind and the Buttons view, as a gamepad's B ends
// the order and as its own Back ends press-to-bind. With them off, nothing is queued. No audio
// thread runs here, so each check takes what the menu queued off the queue itself
// (ui_sound_take). Read in by tools/test_ports.cpp after the frontend, with GCN_OVERLAY.
#include "test_ports_support.h"
#include "ui_sound.h"

namespace {

void sounds_page(void*) {
    ImGui::Button("Press");
    ImGui::Button("Again");
}

// What a press queued: exactly `want`. Otherwise what it did queue goes to stdout.
bool queued(std::initializer_list<UiSound> want) {
    static const char* const kNames[kUiSounds] = {"move", "confirm", "back", "page_back", "page_on", "open", "close",
                                                  "refused"};
    const std::vector<UiSound> got = ui_sound_take();
    if (got == std::vector<UiSound>(want)) return true;
    printf("     queued:");
    for (UiSound id : got) printf(" %s", kNames[(int)id]);
    printf("\n");
    return false;
}

void menu_sounds(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    overlay_test_reset_pages();
    overlay_add_page("Sounds", sounds_page, nullptr);
    OverlayPicture picture;
    picture.path = dir + "/sounds-settings.txt";
    picture.apply = record_apply;
    picture.now.menu_motion = false;
    picture.now.menu_sounds = true;
    overlay_init(window, "Test title", picture);
    gamepads_init();
    Virtual pad{"virtual pad sounds"};
    attach(pad);
    Guest guest;
    host_frame(guest);
    ui_sound_take();

    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    check(overlay_open() && queued({UiSound::open}),
          "with menu_sounds on, F1 opens the menu with its swell, and the focus put on the first item doesn't tick");
    tap(guest, SDL_SCANCODE_DOWN, SDLK_DOWN);
    check(queued({UiSound::move}), "Down moves the focus with a tick");
    tap(guest, SDL_SCANCODE_E, SDLK_E);
    const bool on = overlay_page() == 1 && queued({UiSound::page_on});
    tap(guest, SDL_SCANCODE_E, SDLK_E);
    check(on && overlay_page() == 2 && queued({UiSound::page_on}),
          "E turns each page with a swish to the right, and the focus put on its first item doesn't tick");
    tap(guest, SDL_SCANCODE_E, SDLK_E);
    check(overlay_page() == 2 && queued({UiSound::refused}), "E on the last page buzzes and turns nothing");
    tap(guest, SDL_SCANCODE_SPACE, SDLK_SPACE);
    check(queued({UiSound::confirm}), "Space presses the focused button with a confirm, and once a press");
    tap(guest, SDL_SCANCODE_Q, SDLK_Q);
    check(overlay_page() == 1 && queued({UiSound::page_back}), "Q turns a page back with a swish to the left");

    bottom_row(guest);
    ui_sound_take();
    tap(guest, SDL_SCANCODE_SPACE, SDLK_SPACE);
    const bool asked = overlay_asking() == OverlayAsk::restart && queued({UiSound::confirm});
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    check(asked && overlay_open() && overlay_asking() == OverlayAsk::none && queued({UiSound::back}),
          "Restart game asks with a confirm, and Escape goes back from the question with a back");

    overlay_controllers_order();
    host_frame(guest);
    ui_sound_take();
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    check(overlay_open() && overlay_controllers_asking() < 0 && queued({UiSound::back}),
          "Escape ends the press-A order with a back");
    overlay_controllers_order();
    host_frame(guest);
    ui_sound_take();
    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    check(overlay_open() && overlay_controllers_asking() < 0 && queued({UiSound::back}),
          "and so does a gamepad's B");

    const bool shown = overlay_bindings_open(pad.id);
    host_frame(guest);
    const bool listens = overlay_bindings_listen(PadControl::B);
    host_frame(guest);
    ui_sound_take();
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    const bool cancelled = !overlay_bindings_listening() && overlay_bindings_shown() && queued({UiSound::back});
    overlay_bindings_listen(PadControl::B);
    host_frame(guest);
    ui_sound_take();
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    const bool own_back = !overlay_bindings_listening() && overlay_bindings_shown() && queued({UiSound::back});
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    check(shown && listens && cancelled && own_back && overlay_open() && !overlay_bindings_shown() &&
              queued({UiSound::back}),
          "Escape, and the gamepad's own Back, cancel press-to-bind with a back, and Escape leaves the Buttons view "
          "with another");

    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    check(!overlay_open() && queued({UiSound::close}), "F1 closes the menu with its swell");

    Settings off = picture.now;
    off.menu_sounds = false;
    overlay_display_change(off);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    tap(guest, SDL_SCANCODE_DOWN, SDLK_DOWN);
    tap(guest, SDL_SCANCODE_Q, SDLK_Q);
    tap(guest, SDL_SCANCODE_Q, SDLK_Q);
    tap(guest, SDL_SCANCODE_SPACE, SDLK_SPACE);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    check(!ui_sound_enabled() && ui_sound_queued() == 0 && ui_sound_take().empty(),
          "with menu_sounds off, opening, moving, turning past the first page, pressing and closing queue nothing");

    unplug({&pad});
    gamepads_shutdown();
    close_window(window);
}

}  // namespace
