// What the Buttons view (overlay_bindings.cpp) has for the overlay's other files.
#pragma once

struct SDL_Gamepad;
union SDL_Event;

// overlay.cpp's, as the menu closes: the view and any press-to-bind end, with nothing changed.
void bindings_stop();
// overlay_controllers.cpp's: whether the view went back to the page since the last call.
bool bindings_back();
// footer.cpp's: the gamepad press-to-bind listens to, null while it isn't listening.
SDL_Gamepad* bindings_pad();
// overlay_input.cpp's: press-to-bind reads its gamepad once a host frame, and an event the view
// takes, true if it took it.
void bindings_poll();
bool bindings_event(const SDL_Event& e);
// overlay_menu.cpp's: the view.
void bindings_page();
