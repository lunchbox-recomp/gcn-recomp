// The menu for a run nobody is at (overlay.h), review.cpp's: GCN_OVERLAY_AT and
// GCN_OVERLAY_PAGE read, and under --dump-output the menu's frame watched until it settles, then
// dumped to a PNG.
#pragma once

struct ImDrawData;
struct SDL_Window;

// GCN_OVERLAY_AT=<frame>:<host frames>, both whole numbers, the second over 0, into `frame` and
// `left`, which a value it refuses leaves alone.
bool review_parse_at(const char* e, long long& frame, long& left);
// GCN_OVERLAY_PAGE=<n>, a page from 0 (Display) to `last`, or -1 for a value it refuses.
int review_parse_page(const char* e, int last);
// The watch on the menu's frame as it settles, started again as the menu opens, and whether `d`
// looks as the frames before it did.
void review_watch_reset();
bool review_settled(const ImDrawData* d);
// `d` drawn again into a target of the window's size, over the held frame or, with `plain`,
// plain grey, and written to the dump directory as overlay_page<page>.png, or with a page of -1
// as overlay_NNNNN.png.
void review_dump(SDL_Window* window, ImDrawData* d, int page, bool plain);
