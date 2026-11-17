// What the Display page (overlay_display.cpp) has for the overlay's other files.
#pragma once

struct OverlayPicture;
struct SDL_Window;

// overlay.cpp's, at overlay_init: the picture the page shows and changes.
void display_init(const OverlayPicture& picture);
// overlay_menu.cpp's: the page.
void display_page(SDL_Window* window);
