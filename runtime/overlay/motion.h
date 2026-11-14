// The overlay's motion (overlay.h): four easings, and tweens timed by the host clock (host_ns),
// never by frames, so a slow frame moves nothing slower and a fast one nothing faster.
#pragma once
#include <cstdint>

// out_cubic is for a page change's slide (F4) and the toasts (F7), which nothing draws yet.
enum class Ease { out_quad, out_cubic, in_quad, out_back };

// `e` at `t` from 0 to 1, clamped to that, from 0 to 1. out_back passes 1 before it settles there,
// by more the larger `overshoot` is; the others never leave 0 to 1.
float ease(Ease e, float t, float overshoot = 1.2f);

// A value moving from `from` to `to` over `ms` milliseconds, starting `delay_ms` after `start`
// (host_ns), shaped by `e`. One of no length is at `to` from `start` on.
struct Tween {
    float from = 0.0f, to = 0.0f;
    uint64_t start = 0;
    float ms = 0.0f, delay_ms = 0.0f;
    Ease e = Ease::out_quad;
    float overshoot = 1.2f;
    float at(uint64_t now) const;
    bool done(uint64_t now) const;
};

// A tween from where `was` is at `now` to `to`, so a change of course mid-way starts from the
// value showing rather than jumping.
Tween tween_to(const Tween& was, uint64_t now, float to, float ms, Ease e, float delay_ms = 0.0f,
               float overshoot = 1.2f);
