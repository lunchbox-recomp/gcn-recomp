// The submission queue: each frame's batch from the guest thread to the renderer (render.h).
#include "gx.h"
#include "render.h"
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

std::unique_ptr<Batch> take_batch(int timeout_ms) {
    std::unique_lock<std::mutex> lk(g_q_mutex);
    if (!g_q_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return !g_queue.empty(); })) return nullptr;
    auto b = std::move(g_queue.front());
    g_queue.pop_front();
    g_frames_taken.fetch_add(1, std::memory_order_relaxed);
    g_q_cv.notify_all();
    return b;
}

}  // namespace gx
