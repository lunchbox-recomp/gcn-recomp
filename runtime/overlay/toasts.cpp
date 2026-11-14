// The notices' motion as toasts (toasts.h).
#include "overlay/toasts.h"
#include <algorithm>
#include <cmath>

namespace {

constexpr float kToastMs = (float)(kToastNs / 1000000);

// A change's length, none with motion off or for a time that isn't a finite number, and never
// more than a toast's whole time.
float span(float ms, bool motion) { return motion && std::isfinite(ms) ? std::clamp(ms, 0.0f, kToastMs) : 0.0f; }

}  // namespace

ToastLook toast_look(const OverlayTheme& t, uint64_t ns, bool motion, bool over) {
    if (ns >= kToastNs) return {0.0f, 0.0f};
    const float ms = (float)((double)ns / 1e6);
    const float in = span(t.toast_in_ms, motion), out = span(t.toast_out_ms, motion);
    const float come = in > 0.0f ? ease(Ease::out_cubic, ms / in) : 1.0f;
    const float go = out > 0.0f ? 1.0f - ease(Ease::in_quad, (ms - (kToastMs - out)) / out) : 1.0f;
    float rise = std::isfinite(t.toast_rise) ? t.toast_rise : 0.0f;
    if (over && t.item_spacing < rise) rise = t.item_spacing;
    return {std::min(come, go), (1.0f - come) * rise};
}

bool toast_moving(const OverlayTheme& t, uint64_t ns, bool motion) {
    if (ns >= kToastNs) return false;
    const float ms = (float)((double)ns / 1e6);
    const float in = span(t.toast_in_ms, motion), out = span(t.toast_out_ms, motion);
    return ms < in || (out > 0.0f && ms >= kToastMs - out);
}

Tween toast_reflow(const OverlayTheme& t, const Tween& was, uint64_t now, float slot, bool motion) {
    return tween_to(was, now, slot, span(t.toast_reflow_ms, motion), Ease::out_cubic);
}
