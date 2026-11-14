// The menu's header and the page it shows (overlay.h). The header has the menu's title on the
// left in the title font and a pill for each page centred between the shoulder buttons' names in
// the tabs font, the page showing on the theme's accent_dark. A click on a pill shows its page.
// The pages themselves still draw through overlay_add_page's callbacks (overlay_menu.cpp).
#pragma once
#include <string>
#include <vector>

// Back on the first page, as overlay_init leaves the menu.
void tabs_reset();
// The page showing, 0 for Display, and the page shown from here on, as GCN_OVERLAY_PAGE opens on
// one or a click on its pill picks it.
int tabs_page();
void tabs_show(int page);
// In the menu's window, at the top: the header over `pages`, the titles in order, each a pill.
// With `live` off, as while the menu asks or shows a view in place of a page, the pills show
// the page it was on and a click picks nothing.
void tabs_header(const std::string& title, const std::vector<std::string>& pages, bool live);
