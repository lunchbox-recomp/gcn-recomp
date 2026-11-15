// The menu's sounds, queued and mixed (ui_sound.h).
#include "ui_sound.h"
#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {

constexpr int kQueue = 16;   // ids waiting for the next pull
constexpr int kVoices = 8;   // voices playing at once
constexpr int kChunk = 512;  // samples the mix sums at a time

// What the main thread made and the audio thread reads once an id is queued. A game may set the
// theme from a static initializer before this file's, so it and g_made are constinit: a dynamic
// initializer here won't compile.
constinit UiSoundTheme g_theme;
constinit bool g_made;
std::vector<int16_t> g_voices[kUiSounds];
std::atomic<bool> g_on{false};

// The queue: the main thread writes an id at g_head and moves it on, and the audio thread reads
// up to it from g_tail. Each counts every id it has passed, so head - tail is how many wait.
uint8_t g_queue[kQueue];
std::atomic<uint32_t> g_head{0}, g_tail{0};

// The audio thread's own.
struct Playing {
    const std::vector<int16_t>* samples;
    size_t at;
};
Playing g_playing[kVoices];
int g_count;
std::atomic<int> g_count_seen{0};

}  // namespace

void ui_sound_set_theme(const UiSoundTheme& theme) {
    if (g_made) {
        fprintf(stderr, "[sound] the menu's sounds are made once they're first on, so this theme is refused\n");
        return;
    }
    g_theme = theme;
}

void ui_sound_enable(bool on) {
    if (on && !g_made) {
        for (int i = 0; i < kUiSounds; i++) g_voices[i] = ui_sound_render(g_theme, (UiSound)i);
        g_made = true;
    }
    g_on.store(on, std::memory_order_release);
}

bool ui_sound_enabled() { return g_on.load(std::memory_order_acquire); }

void ui_sound_play(UiSound id) {
    if (!g_on.load(std::memory_order_relaxed) || (int)id >= kUiSounds) return;
    const uint32_t head = g_head.load(std::memory_order_relaxed);
    if (head - g_tail.load(std::memory_order_acquire) >= (uint32_t)kQueue) return;
    g_queue[head % kQueue] = (uint8_t)id;
    g_head.store(head + 1, std::memory_order_release);
}

void ui_sound_mix(int16_t* out, int frames) {
    const uint32_t head = g_head.load(std::memory_order_acquire);
    uint32_t tail = g_tail.load(std::memory_order_relaxed);
    if (tail != head) {
        for (; tail != head; tail++)
            if (g_count < kVoices) g_playing[g_count++] = {&g_voices[g_queue[tail % kQueue]], 0};
        g_tail.store(tail, std::memory_order_release);
    }
    // A chunk at a time, what's there and every voice summed in 32 bits, then held to 16 once.
    const size_t total = (size_t)frames * 2;
    int32_t sum[kChunk];
    for (size_t from = 0; from < total && g_count > 0; from += kChunk) {
        const size_t len = std::min((size_t)kChunk, total - from);
        for (size_t i = 0; i < len; i++) sum[i] = out[from + i];
        for (int v = 0; v < g_count;) {
            Playing& p = g_playing[v];
            const size_t n = std::min(len, p.samples->size() - p.at);
            const int16_t* s = p.samples->data() + p.at;
            for (size_t i = 0; i < n; i++) sum[i] += s[i];
            p.at += n;
            if (p.at == p.samples->size()) g_playing[v] = g_playing[--g_count];
            else v++;
        }
        for (size_t i = 0; i < len; i++) out[from + i] = (int16_t)std::clamp(sum[i], -32768, 32767);
    }
    g_count_seen.store(g_count, std::memory_order_relaxed);
}

size_t ui_sound_queued() {
    return g_head.load(std::memory_order_acquire) - g_tail.load(std::memory_order_acquire);
}

int ui_sound_playing() { return g_count_seen.load(std::memory_order_relaxed); }

#ifdef GCN_OVERLAY_TEST
std::vector<UiSound> ui_sound_take() {
    std::vector<UiSound> ids;
    const uint32_t head = g_head.load(std::memory_order_acquire);
    uint32_t tail = g_tail.load(std::memory_order_relaxed);
    for (; tail != head; tail++) ids.push_back((UiSound)g_queue[tail % kQueue]);
    g_tail.store(tail, std::memory_order_release);
    return ids;
}

const UiSoundTheme& ui_sound_test_theme() { return g_theme; }
#endif
