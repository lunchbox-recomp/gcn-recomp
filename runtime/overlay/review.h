// The menu for a run nobody is at (overlay.h), review.cpp's: GCN_OVERLAY_AT, GCN_OVERLAY_PAGE
// and GCN_OVERLAY_TOASTS read, and under --dump-output the menu's frame or the notices' watched
// until it settles, then dumped to a PNG.
#pragma once
#include <cstdint>

struct ImDrawData;
struct SDL_Window;

// A watch on a context's frames as they settle, started again by a fresh one.
struct ReviewWatch {
    uint64_t last = 0;  // the frame before's draw, hashed
    int same = 0;       // frames in a row that matched the one before
};

// GCN_OVERLAY_AT=<frame>:<host frames>, both whole numbers, the second over 0, into `frame` and
// `left`, which a value it refuses leaves alone.
bool review_parse_at(const char* e, long long& frame, long& left);
// GCN_OVERLAY_PAGE=<n>, a page from 0 (Display) to `last`, or -1 for a value it refuses.
int review_parse_page(const char* e, int last);
// GCN_OVERLAY_TOASTS=<n>, how many notices showing at once the notices' dump waits for, 1 or
// more, or 0 for a value it refuses.
int review_parse_toasts(const char* e);
// Whether `d` looks as the frames before it did, by `watch`.
bool review_settled(ReviewWatch& watch, const ImDrawData* d);
// `d` drawn again into a target of the window's size, over the held frame or, with `plain`,
// plain grey, and written to the dump directory as `name`.
void review_write(SDL_Window* window, ImDrawData* d, const char* name, bool plain);
// The menu's: review_write to overlay_page<page>.png, or with a page of -1 to overlay_NNNNN.png.
void review_dump(SDL_Window* window, ImDrawData* d, int page, bool plain);
