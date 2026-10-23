// Pausing the guest at a frame boundary; see pause.h.
#include "pause.h"
#include "runtime.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>

uint64_t audio_frames_out();

static std::mutex g_mutex;
static std::condition_variable g_lifted;
static std::atomic<bool> g_asked{false}, g_held{false};
static std::atomic<std::chrono::steady_clock::rep> g_resumed_at{0};
static std::atomic<long> g_test_left{0};  // host frames left of GCN_PAUSE_AT's pause

void pause_request(bool on) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_asked = on;
    }
    if (!on) g_lifted.notify_all();
}

bool pause_requested() { return g_asked.load(); }
bool paused() { return g_held.load(); }

std::chrono::steady_clock::time_point pause_resumed_at() {
    return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(g_resumed_at.load()));
}

// GCN_PAUSE_AT=<frame>:<host frames>, both whole numbers and the second over 0. Anything
// else asks for no pause and says so.
struct TestPause {
    uint32_t frame = 0;
    long host_frames = 0;
};
static TestPause test_pause() {
    TestPause t;
    const char* e = getenv("GCN_PAUSE_AT");
    if (!e) return t;
    auto digit = [](const char* p) { return *p >= '0' && *p <= '9'; };
    char* end = nullptr;
    const unsigned long frame = digit(e) ? strtoul(e, &end, 10) : 0;
    if (end && *end == ':' && digit(end + 1) && frame <= UINT32_MAX) {
        const long host = strtol(end + 1, &end, 10);
        if (!*end && host > 0) {
            t.frame = (uint32_t)frame;
            t.host_frames = host;
            return t;
        }
    }
    fprintf(stderr, "[pause] GCN_PAUSE_AT=%s isn't <frame>:<host frames>, so there's no pause\n", e);
    return t;
}

void pause_gate(uint32_t frame) {
    static const TestPause test = test_pause();
    if (test.host_frames && frame == test.frame) {
        g_test_left = test.host_frames;
        pause_request(true);
    }
    if (!g_asked.load()) return;
    std::unique_lock<std::mutex> lk(g_mutex);
    if (!g_asked) return;
    const uint64_t t0 = host_ns();
    g_held = true;
    fprintf(stderr, "[pause] held at frame %u, output frame %llu\n", frame,
            (unsigned long long)audio_frames_out());
    g_lifted.wait(lk, [] { return !g_asked.load(); });
    const uint64_t held = host_ns() - t0;
    clock_hold(held);
    g_resumed_at = std::chrono::steady_clock::now().time_since_epoch().count();
    g_held = false;
    fprintf(stderr, "[pause] resumed at frame %u after %.1f ms, output frame %llu\n", frame, (double)held / 1e6,
            (unsigned long long)audio_frames_out());
}

void pause_host_frame() {
    if (g_test_left.load() > 0 && --g_test_left == 0) pause_request(false);
}
