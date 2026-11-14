// The menu's footer (overlay.h): hints on the left, each a key's name and what it does, and on a
// page Restart game and Quit game on the right, small, in the footer font. Each of those asks
// first, in a card centred in place of the page, with Cancel focused and the confirm on the
// theme's danger; Cancel, B or Escape goes back to the button that asked. A replay has no
// Restart game, since its log can't hold one (input_log.h).
#pragma once
#include <string>

// Below the page, or a view in its place: the hints, and on a page (`on_page`) Restart game and
// Quit game. While the Buttons view listens for a press to bind, the hints are only its way out.
void footer(bool on_page);
// While the menu asks (overlay_asking), in place of the page: the question's card.
void footer_card();
// Back from the question to the button that asked, as B or Escape goes back.
void footer_back();
// Any question ended with nothing done, as the menu closes.
void footer_end_ask();
#ifdef GCN_OVERLAY_TEST
// The hints the footer last drew, each its key and what it does, joined by ", ".
const std::string& footer_test_hints();
#endif
