// The menu's sounds, made and mixed (ui_sound.h).
#include "ui_sound.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace {

constexpr double kPi = std::numbers::pi;
constexpr int kQueue = 16;   // ids waiting for the next pull
constexpr int kVoices = 8;   // voices playing at once

int frames_of(float ms) { return (int)std::lround(ms * kUiSoundRate / 1000.0f); }

// A sine of `hz` at `t` seconds.
double sine(double hz, double t) { return std::sin(2.0 * kPi * hz * t); }

// Fixed noise, the same every time a theme is made, from -1 to 1.
struct Noise {
    uint32_t s = 0x9E3779B9u;
    double next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s / 2147483647.5 - 1.0;
    }
};

// Voice `id`'s signal before its envelope and level, `frames` long.
std::vector<double> oscillate(const UiVoice& v, UiSound id, int frames) {
    std::vector<double> x(frames);
    const float* f = v.notes;
    switch (id) {
    case UiSound::move:
        for (int n = 0; n < frames; n++) {
            const double t = (double)n / kUiSoundRate;
            // A triangle from its odd harmonics under 20 kHz, alternating in sign, each 1/k^2.
            double tri = 0.0;
            for (int k = 1; k * f[1] < 20000.0f; k += 2) tri += ((k / 2) % 2 ? -1.0 : 1.0) * sine(k * f[1], t) / (k * k);
            x[n] = sine(f[0], t) + 0.25 * 8.0 / (kPi * kPi) * tri;
        }
        break;
    case UiSound::confirm: case UiSound::back: {
        double phase = 0.0;
        for (int n = 0; n < frames; n++) {
            x[n] = std::sin(phase);
            phase += 2.0 * kPi * (n < frames / 2 ? f[0] : f[1]) / kUiSoundRate;
        }
        break;
    }
    case UiSound::page_back: case UiSound::page_on: {
        // A band-pass of constant peak gain (Robert Bristow-Johnson's cookbook), Q 1.2, its
        // centre moved each frame.
        Noise noise;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (int n = 0; n < frames; n++) {
            const double at = frames > 1 ? (double)n / (frames - 1) : 0.0;
            const double w = 2.0 * kPi * (f[0] + (f[1] - f[0]) * at) / kUiSoundRate;
            const double alpha = std::sin(w) / (2.0 * 1.2), a0 = 1.0 + alpha;
            const double in = noise.next();
            const double y = (alpha * in - alpha * x2 + 2.0 * std::cos(w) * y1 - (1.0 - alpha) * y2) / a0;
            x2 = x1;
            x1 = in;
            y2 = y1;
            y1 = y;
            x[n] = y;
        }
        break;
    }
    case UiSound::open: case UiSound::close:
        for (int n = 0; n < frames; n++)
            for (float hz : v.notes)
                if (hz > 0.0f) x[n] += sine(hz, (double)n / kUiSoundRate);
        break;
    case UiSound::refused:
        for (int n = 0; n < frames; n++)
            for (int k = 1; k * f[0] < 4000.0f; k += 2) x[n] += sine(k * f[0], (double)n / kUiSoundRate) / k;
        break;
    }
    return x;
}

// What the main thread made and the audio thread reads once an id is queued.
UiSoundTheme g_theme;
bool g_made;
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

UiSoundTheme ui_sound_fit(const UiSoundTheme& theme) {
    UiSoundTheme t = theme;
    t.peak_dbfs = std::isfinite(t.peak_dbfs) ? std::min(t.peak_dbfs, kUiSoundMaxDbfs) : kUiSoundMaxDbfs;
    t.pan = std::isfinite(t.pan) ? std::clamp(t.pan, 0.0f, 1.0f) : 0.0f;
    for (UiVoice& v : t.voices) {
        for (float& hz : v.notes) hz = std::isfinite(hz) ? std::clamp(hz, 0.0f, 20000.0f) : 0.0f;
        const auto edge = [](float ms) { return std::isfinite(ms) ? std::max(ms, kUiSoundEdgeMs) : kUiSoundEdgeMs; };
        v.attack_ms = edge(v.attack_ms);
        v.release_ms = edge(v.release_ms);
        v.decay_ms = std::isfinite(v.decay_ms) ? std::max(v.decay_ms, 0.0f) : 0.0f;
        v.ms = std::isfinite(v.ms) ? std::clamp(v.ms, v.attack_ms + v.release_ms, 2000.0f) : v.attack_ms + v.release_ms;
    }
    return t;
}

float ui_sound_envelope(const UiVoice& voice, int n, int frames) {
    UiSoundTheme one;
    one.voices[0] = voice;
    const UiVoice v = ui_sound_fit(one).voices[0];
    const int attack = frames_of(v.attack_ms), release = frames_of(v.release_ms), last = frames - 1;
    if (n <= 0 || n >= last) return 0.0f;
    double e = 1.0;
    if (n < attack) e *= (1.0 - std::cos(kPi * n / attack)) / 2.0;
    else if (v.decay_ms > 0.0f) e *= std::exp(-(n - attack) * 1000.0 / (v.decay_ms * kUiSoundRate));
    if (last - n < release) e *= (1.0 - std::cos(kPi * (last - n) / release)) / 2.0;
    return (float)e;
}

std::vector<int16_t> ui_sound_render(const UiSoundTheme& theme, UiSound id) {
    const UiSoundTheme t = ui_sound_fit(theme);
    const UiVoice& v = t.voices[(int)id];
    const int frames = frames_of(v.ms);
    std::vector<double> x = oscillate(v, id, frames);
    double most = 0.0;
    for (int n = 0; n < frames; n++) {
        x[n] *= ui_sound_envelope(v, n, frames);
        most = std::max(most, std::fabs(x[n]));
    }
    // A page's swish to its side by constant power, the rest in the middle.
    const double side = id == UiSound::page_back ? -1.0 : id == UiSound::page_on ? 1.0 : 0.0;
    const double angle = (1.0 + side * t.pan) * kPi / 4.0;
    const double left = std::cos(angle), right = std::sin(angle);
    // The louder channel's peak at the level, rounded down to a whole sample so none is over it.
    const double peak = std::floor(32768.0 * std::pow(10.0, t.peak_dbfs / 20.0));
    const double scale = most > 0.0 ? peak / (most * std::max(left, right)) : 0.0;
    std::vector<int16_t> out(2 * (size_t)frames);
    for (int n = 0; n < frames; n++) {
        out[2 * n] = (int16_t)std::lround(x[n] * left * scale);
        out[2 * n + 1] = (int16_t)std::lround(x[n] * right * scale);
    }
    return out;
}

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
    for (int v = 0; v < g_count;) {
        Playing& p = g_playing[v];
        const size_t n = std::min((size_t)frames * 2, p.samples->size() - p.at);
        const int16_t* s = p.samples->data() + p.at;
        for (size_t i = 0; i < n; i++) out[i] = (int16_t)std::clamp(out[i] + s[i], -32768, 32767);
        p.at += n;
        if (p.at == p.samples->size()) g_playing[v] = g_playing[--g_count];
        else v++;
    }
    g_count_seen.store(g_count, std::memory_order_relaxed);
}

size_t ui_sound_queued() {
    return g_head.load(std::memory_order_acquire) - g_tail.load(std::memory_order_acquire);
}

int ui_sound_playing() { return g_count_seen.load(std::memory_order_relaxed); }

std::vector<UiSound> ui_sound_take() {
    std::vector<UiSound> ids;
    const uint32_t head = g_head.load(std::memory_order_acquire);
    uint32_t tail = g_tail.load(std::memory_order_relaxed);
    for (; tail != head; tail++) ids.push_back((UiSound)g_queue[tail % kQueue]);
    g_tail.store(tail, std::memory_order_release);
    return ids;
}
