// What the menu (overlay_menu.cpp) has for overlay.cpp.
#pragma once

#include <cstdint>

// The menu set up with its title on its first page, how many pages a game added, the menu opened
// on a page (-1 for where it was) at a time, and its frame.
void menu_init(const char* title);
int menu_added_pages();
void menu_opened(int page, uint64_t now);
void menu_frame();
#ifdef GCN_OVERLAY_TEST
void menu_clear_pages();
#endif
