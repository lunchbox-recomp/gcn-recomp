// The notices' motion as toasts (overlay.h), on the host clock (motion.h): each one's look as it
// ages through its time, and its place in the stack, which moves down as an older one goes.
#pragma once
#include "overlay/motion.h"
#include "overlay/theme.h"
#include <cstdint>

// How long each notice shows, in host time: exactly 2 s, with motion or without.
constexpr uint64_t kToastNs = 2000000000;

// A toast's alpha, from 0 to 1, and how far it sits below its place in the stack, in pixels at
// 1080 lines.
struct ToastLook {
    float alpha = 1.0f, below = 0.0f;
};

// The toast `ns` after it was queued. It rises from the theme's toast_rise below its place and
// fades in over its toast_in_ms, out-cubic, is whole until the last toast_out_ms of kToastNs,
// and fades out over those, in-quad, to nothing at kToastNs. One queued `over` another rises
// from no further than the theme's item_spacing, the gap between them, so it never enters the
// pill below. With motion off it's whole and in its place from its first frame to its last. A
// game's theme may give a time or a rise that's negative, infinite or not a number: such a time
// takes none and such a rise none, and no time is longer than kToastNs.
ToastLook toast_look(const OverlayTheme& t, uint64_t ns, bool motion, bool over = false);

// Whether the toast `ns` after it was queued is rising, fading in or fading out, which with
// motion off it never is.
bool toast_moving(const OverlayTheme& t, uint64_t ns, bool motion);

// A toast's place in the stack, 0 the lowest, from where `was` has it at `now` to `slot`, over
// the theme's toast_reflow_ms, out-cubic, as an older one under it goes; at once with motion off.
Tween toast_reflow(const OverlayTheme& t, const Tween& was, uint64_t now, float slot, bool motion);
