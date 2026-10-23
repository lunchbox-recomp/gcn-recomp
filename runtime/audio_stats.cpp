// The device stage's accounting (audio_stats.h). Portable: a backend hands it the device it
// opened and the time each callback started, so a test can drive it on a clock of its own.
#include "audio_stats.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr uint64_t kEvery = 400;            // callbacks between [audio] callbacks lines
constexpr double kLate = 1.5;            // a gap over this many periods is a late callback
constexpr uint64_t kPositionEvery = 12000;  // output frames between position lines, 0.25 s at 48 kHz

struct State {
    AudioDeviceInfo device{};
    bool opened = false;
    int64_t last_ns = -1;  // when the last callback started
    uint64_t callbacks = 0, late = 0, underruns = 0;
    double window_ms = 0, window_worst_ms = 0;  // the gaps since the last callbacks line
    uint64_t window_gaps = 0;
    uint64_t next_position = 0;  // the output frame the next position line is due at
};
State g;

void print_stderr(const char* line) { fputs(line, stderr); }
void (*g_print)(const char*) = print_stderr;  // a test collects the lines instead

}  // namespace

bool audio_stats_on() {
    static const bool on = getenv("GCN_AUDIOSTATS") != nullptr || getenv("GCN_AXSTATS") != nullptr;
    return on;
}

void audio_stats_opened(const char* driver, int rate, int channels, int buffer_frames) {
    snprintf(g.device.driver, sizeof g.device.driver, "%s", driver && *driver ? driver : "unknown");
    g.device.rate = rate;
    g.device.channels = channels;
    g.device.buffer = buffer_frames;
    g.opened = true;
    if (!audio_stats_on()) return;
    char line[128];
    snprintf(line, sizeof line, "[audio] device driver=%s rate=%d channels=%d buffer=%d\n", g.device.driver,
             rate, channels, buffer_frames);
    g_print(line);
}

const AudioDeviceInfo* audio_stats_device() { return g.opened ? &g.device : nullptr; }

void audio_stats_callback(int64_t now_ns, uint64_t out_frame) {
    if (!audio_stats_on()) return;
    char line[160];
    if (g.last_ns >= 0) {
        const double gap = (double)(now_ns - g.last_ns) / 1e6;
        g.window_ms += gap;
        g.window_gaps++;
        g.window_worst_ms = std::max(g.window_worst_ms, gap);
        // The period is the device's own, the buffer it takes at its rate, so a device that
        // is late every time still shows as late.
        const double period = g.device.rate > 0 ? 1000.0 * g.device.buffer / g.device.rate : 0;
        if (period > 0 && gap > kLate * period) {
            g.late++;
            snprintf(line, sizeof line, "[audio] late callback at output frame %llu gap_ms=%.1f\n",
                     (unsigned long long)out_frame, gap);
            g_print(line);
        }
    }
    g.last_ns = now_ns;
    if (++g.callbacks % kEvery == 0) {
        snprintf(line, sizeof line, "[audio] callbacks n=%llu period_ms=%.2f late=%llu worst_ms=%.1f underruns=%llu\n",
                 (unsigned long long)g.callbacks, g.window_gaps ? g.window_ms / (double)g.window_gaps : 0.0,
                 (unsigned long long)g.late, g.window_worst_ms, (unsigned long long)g.underruns);
        g_print(line);
        g.window_ms = g.window_worst_ms = 0;
        g.window_gaps = 0;
    }
}

void audio_stats_underrun(uint64_t at, int frames) {
    if (!audio_stats_on()) return;
    g.underruns++;
    char line[128];
    snprintf(line, sizeof line, "[audio] underrun at output frame %llu frames=%d\n", (unsigned long long)at, frames);
    g_print(line);
}

void audio_stats_position(uint64_t out_frame, uint64_t dma_frame, bool jumped) {
    if (!audio_stats_on() || (!jumped && out_frame < g.next_position)) return;
    g.next_position = out_frame + kPositionEvery;
    char line[128];
    snprintf(line, sizeof line, "[audio] output frame %llu plays DMA frame %llu\n", (unsigned long long)out_frame,
             (unsigned long long)dma_frame);
    g_print(line);
}
