// The menu's header and the page it shows (overlay.h). The header has the menu's title on the
// left in the title font and a pill for each page centred between the shoulder buttons' names in
// the tabs font, the page showing on the theme's accent_dark. A click on a pill shows its page.
// The pages themselves still draw through overlay_add_page's callbacks (overlay_menu.cpp).
//
// A page change moves on the host clock: the pill under the page showing slides to its new
// place and width over the theme's page_ms, out-quad, and the new page's contents come in from
// the side its pill is on, page_slide pixels across and fading in over the same time, out-cubic.
// With motion off each change is whole at once.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ImDrawData;

void tabs_motion(bool on);
// Back on the first page, as overlay_init leaves the menu.
void tabs_reset();
// The page showing, 0 for Display, and the page shown from here on, as GCN_OVERLAY_PAGE opens on
// one, in place with no motion.
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
// `now` is host_ns, which times a change's motion.
void tabs_header(const std::string& title, const std::vector<std::string>& pages, bool live, uint64_t now);
// Just before the page's contents: after a change, the focus to the page's first item, with
// nothing pressed, where the menu is `open`, and none while it fades out.
void tabs_focus(bool open);
// Around the page's contents in the menu's window, which a change moves in; and none, for a
// frame that shows a question or a view in place of a page.
void tabs_contents_begin();
void tabs_contents_end();
void tabs_no_contents();
// Whether the item with the focus was among the page's contents at the last frame, so a change
// moves it in with them, not one in the footer or a question.
bool tabs_contents_focused();
// After ImGui::Render and before the panel's own motion (appear_apply): the page's contents in
// `d` moved across and faded as a change brings them in at `now`.
void tabs_apply(ImDrawData* d, uint64_t now);
// How far across, in the window's pixels, and how faded the page's contents are at `now` as a
// change brings them in: 0 and 1 once it's done. The focus ring comes in with them while its item
// is one of them (ring.h).
void tabs_contents_moved(uint64_t now, float& dx, float& alpha);
// Whether a change is still moving the pill or the page's contents.
bool tabs_moving(uint64_t now);
#ifdef GCN_OVERLAY_TEST
// The middle of page `page`'s pill as the header last laid it out, in the window's pixels, for a
// click there. False for no such pill.
bool tabs_test_pill(int page, float& x, float& y);
#endif
