// The overlay's easings and tweens (motion.h).
#include "overlay/motion.h"
#include <algorithm>

float ease(Ease e, float t, float overshoot) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;  // what's left
    switch (e) {
    case Ease::out_quad: return 1.0f - u * u;
    case Ease::out_cubic: return 1.0f - u * u * u;
    case Ease::in_quad: return t * t;
    case Ease::out_back: return 1.0f - (overshoot + 1.0f) * u * u * u + overshoot * u * u;
    }
    return t;
}

float Tween::at(uint64_t now) const {
    const uint64_t begin = start + (uint64_t)((double)delay_ms * 1e6);
    if (now < begin) return from;
    if (ms <= 0.0f) return to;
    const float t = (float)((double)(now - begin) / (ms * 1e6));
    return t >= 1.0f ? to : from + (to - from) * ease(e, t, overshoot);
}

bool Tween::done(uint64_t now) const { return now >= start + (uint64_t)(((double)delay_ms + ms) * 1e6); }

Tween tween_to(const Tween& was, uint64_t now, float to, float ms, Ease e, float delay_ms, float overshoot) {
    Tween t;
    t.from = was.at(now);
    t.to = to;
    t.start = now;
    t.ms = ms;
    t.delay_ms = delay_ms;
    t.e = e;
    t.overshoot = overshoot;
    return t;
}
