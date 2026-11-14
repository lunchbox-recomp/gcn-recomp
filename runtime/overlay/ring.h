// The focus ring (overlay.h), in place of Dear ImGui's own nav highlight: a rounded rect in the
// theme's accent around the item with the focus, focus_width wide and focus_gap outside the item
// at 1080 lines, its corners the item's own rounded out that far. It shows when Dear ImGui would
// show its own, after a key or a gamepad moves the focus and not after the mouse, and only on the
// menu's own window.
#pragma once

// Last in the menu's window, after everything on it: the ring around the item with the focus.
void ring_draw();
