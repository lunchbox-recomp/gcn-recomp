// How a wait for a time sleeps toward it: the guest's pacer (cpu.cpp), which spins the rest, and
// the frontend's held redraw (gx::take_batch_until), which waits out the rest. A sleep ends when
// the scheduler gets back to the thread, which is later than asked, so the pacer sleeps toward
// the time and spins the rest, as
// Dolphin's PrecisionTimer::SleepUntil does (Common/Timer.cpp, which spins the last 1 ms and
// 20 us). The spin doesn't yield, since a yield on a busy core hands it to whatever else is
// queued there.
//
// Apple platforms coalesce timers, letting a sleep end late by a share of its length that
// the process's latency tier sets (the kern.timer_coalesce_tier* sysctls), up to half at
// tier 2. In a hidden run the guest thread's 1 ms sleeps ended 0.26 ms late. So each sleep
// is two thirds of the wait left beyond kSpinNs, at most kStepNs, and even if it runs half
// again as long, it ends before the last kSpinNs. The pacer waits at every event poll, about
// six times a frame, so the spin is kept to kSpinNs and a sleep under kMinSleepNs is skipped.
// Android sleeps the whole way in one go, as Dolphin does there by default, since a headset's
// cores and battery are the tighter limit.
#pragma once
#include <algorithm>
#include <cstdint>

// How long the wait for `until` sleeps next at time `now`, both in ns, or 0 when the spin takes
// over.
#ifdef __ANDROID__
inline uint64_t pace_sleep_ns(uint64_t now, uint64_t until) { return until > now ? until - now : 0; }
#else
inline constexpr uint64_t kSpinNs = 100000, kStepNs = 1000000, kMinSleepNs = 50000;

inline uint64_t pace_sleep_ns(uint64_t now, uint64_t until) {
    if (until <= now + kSpinNs) return 0;
    const uint64_t ns = std::min((until - now - kSpinNs) * 2 / 3, kStepNs);
    return ns >= kMinSleepNs ? ns : 0;
}
#endif
