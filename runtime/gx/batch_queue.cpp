// The submission queue: each frame's batch from the guest thread to the renderer (render.h).
#include "gx.h"
#include "render.h"
#include "../pace.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace gx {

static std::mutex g_q_mutex;
static std::condition_variable g_q_cv;
static std::deque<std::unique_ptr<Batch>> g_queue;

// A batch holds every vertex of a frame -- around 10 MB during a race -- so the queue
// has to be a hard bound, not a hint. If the renderer is slower than the guest (a
// mobile GPU, say) an advisory wait that queues anyway grows this by tens of megabytes
// a second; on a Quest 3 that reached 3.8 GB and the app was killed by lowmemorykiller.
// It is also latency: when the renderer is the slow stage the queue stays full, and every
// frame shown is as many frames old as it holds, so it holds one (see kFeMaxFrames in
// fifo.cpp).
static constexpr size_t kMaxQueuedBatches = 1;

void submit_batch(std::unique_ptr<Batch> b) {
    std::unique_lock<std::mutex> lk(g_q_mutex);
    StallTimer stall(Stall::RenderFull);
    // Wait for room, which throttles the guest to the renderer's rate. The timeout is
    // only a safety valve: if nothing is consuming at all (no window, a stalled render
    // thread) the guest must not block forever, so drop the oldest frame instead.
    if (!g_q_cv.wait_for(lk, std::chrono::seconds(2),
                         [] { return g_queue.size() < kMaxQueuedBatches; })) {
        if (!g_queue.empty()) g_queue.pop_front();
    }
    g_queue.push_back(std::move(b));
    g_q_cv.notify_all();
}

size_t queue_depth() {
    std::lock_guard<std::mutex> lk(g_q_mutex);
    return g_queue.size();
}

// Frames the renderer has taken off the queue, for GCN_STALLS's latency figure.
static std::atomic<uint32_t> g_frames_taken{0};
uint32_t frames_taken() { return g_frames_taken.load(std::memory_order_relaxed); }

// The oldest batch, taken off the queue, which isn't empty, with the queue's lock held.
static std::unique_ptr<Batch> take_front() {
    auto b = std::move(g_queue.front());
    g_queue.pop_front();
    g_frames_taken.fetch_add(1, std::memory_order_relaxed);
    g_q_cv.notify_all();
    return b;
}

std::unique_ptr<Batch> take_batch(int timeout_ms) {
    std::unique_lock<std::mutex> lk(g_q_mutex);
    if (!g_q_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return !g_queue.empty(); })) return nullptr;
    return take_front();
}

// One wait until `until` ends late where timers coalesce: 2 to 3 ms for a held frame's on macOS,
// as much at 16.7 ms as at 8.3. So this one sleeps toward it in the guest pacer's steps (pace.h),
// and where the pacer would spin it waits until `until` once more, which ends late by tens of us
// rather than ms. Each wait is on the queue, which a batch ends at once.
std::unique_ptr<Batch> take_batch_until(std::chrono::steady_clock::time_point until) {
    const auto ns = [](std::chrono::steady_clock::time_point t) {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    };
    std::unique_lock<std::mutex> lk(g_q_mutex);
    for (;;) {
        if (!g_queue.empty()) return take_front();
        const uint64_t now = ns(std::chrono::steady_clock::now());
        if (now >= ns(until)) return nullptr;
        if (const uint64_t sleep = pace_sleep_ns(now, ns(until)))
            g_q_cv.wait_for(lk, std::chrono::nanoseconds(sleep));
        else
            g_q_cv.wait_until(lk, until);
    }
}

}  // namespace gx
