// What the Controllers page (overlay_controllers.cpp) has for the overlay's other files.
#pragma once

union SDL_Event;

// overlay.cpp's, as the menu closes: the press-A order ends.
void controllers_stop();
// overlay_input.cpp's: an event the press-A order takes, true if it took it.
bool controllers_event(const SDL_Event& e);
// overlay_menu.cpp's: the page.
void controllers_page();
