// The focus ring (overlay.h), in place of Dear ImGui's own nav highlight: a rounded rect in the
// theme's accent around the item with the focus, focus_width wide and focus_gap outside the item
// at 1080 lines, its corners the item's own rounded out that far. It shows when Dear ImGui would
// show its own, as the menu opens (draw_menu) and after a key or a gamepad moves the focus, not
// after the mouse, and only on the menu's own window.
//
// When the focus moves the ring follows on the host clock (motion.h): each frame it goes
// 1 - exp(-dt / focus_ms) of the way left, dt the host time since the frame before, so it eases
// in at the same speed however often frames come, and it's there once it's within half a pixel.
// It's there at once, with no motion, for a move over half the panel's width or height, as the
// menu opens, on a page change or a question or a view in place of the page, when it first
// shows, and always with motion off. While a page change slides the page's contents in, the ring
// slides and fades in with them (tabs.h).
#pragma once
#include <cstdint>

void ring_motion(bool on);
// The next frame puts the ring on its item at once, as the menu opening does.
void ring_snap();
// Last in the menu's window, after everything on it: the ring around the item with the focus,
// moved toward it at `now` (host_ns).
void ring_draw(uint64_t now);
// Whether the ring was still on its way to its item at its last frame.
bool ring_moving();
#ifdef GCN_OVERLAY_TEST
// At the last frame, where the ring was, as the rect it goes around, and the rect of the item
// with the focus, each {left, top, right, bottom} in the window's pixels. False if it didn't show.
bool ring_test(float at[4], float item[4]);
#endif
