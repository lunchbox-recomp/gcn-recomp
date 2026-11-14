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
// one.
int tabs_page();
void tabs_show(int page);
// The page shown from here on, as a click on its pill or, `moved`, a shoulder or Q or E picks it:
// a change focuses the page's first item at its next draw (tabs_focus), shown as a move shows it
// where `moved`.
void tabs_pick(int page, bool moved);
// In the menu's window, at the top: the header over `pages`, the titles in order, each a pill.
// L1, L2 or Q goes back a page and R1, R2 or E on a page, no further than the first or the last,
// and a click on a pill goes to its page. With `live` off, as while the menu asks or shows a view
// in place of a page, the pills show the page it was on and nothing moves it. While an item is
// active, a slider held, say, the shoulders and keys move nothing either.
void tabs_header(const std::string& title, const std::vector<std::string>& pages, bool live);
// Just before the page's contents: after a change, the focus to the page's first item, with
// nothing pressed, where the menu is `open`, and none while it fades out.
void tabs_focus(bool open);
