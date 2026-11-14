// The menu's sounds, made (ui_sound_synth.h).
#include "ui_sound_synth.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

constexpr double kPi = std::numbers::pi;

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
            for (int k = 1; f[1] > 0.0f && k * f[1] < 20000.0f; k += 2)
                tri += ((k / 2) % 2 ? -1.0 : 1.0) * sine(k * f[1], t) / (k * k);
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
            for (int k = 1; f[0] > 0.0f && k * f[0] < 4000.0f; k += 2)
                x[n] += sine(k * f[0], (double)n / kUiSoundRate) / k;
        break;
    }
    return x;
}

}  // namespace

static_assert(2.0f * kUiSoundLongestEdgeMs <= kUiSoundLongestMs, "a voice's two edges fit in the longest voice");

UiSoundTheme ui_sound_fit(const UiSoundTheme& theme) {
    UiSoundTheme t = theme;
    t.peak_dbfs = std::isfinite(t.peak_dbfs) ? std::min(t.peak_dbfs, kUiSoundMaxDbfs) : kUiSoundMaxDbfs;
    t.pan = std::isfinite(t.pan) ? std::clamp(t.pan, 0.0f, 1.0f) : 0.0f;
    for (UiVoice& v : t.voices) {
        for (float& hz : v.notes) hz = std::isfinite(hz) && hz >= kUiSoundLowestHz ? std::min(hz, 20000.0f) : 0.0f;
        const auto edge = [](float ms) {
            return std::isfinite(ms) ? std::clamp(ms, kUiSoundEdgeMs, kUiSoundLongestEdgeMs) : kUiSoundEdgeMs;
        };
        v.attack_ms = edge(v.attack_ms);
        v.release_ms = edge(v.release_ms);
        v.decay_ms = std::isfinite(v.decay_ms) ? std::max(v.decay_ms, 0.0f) : 0.0f;
        const float edges = v.attack_ms + v.release_ms;  // never over kUiSoundLongestMs
        v.ms = std::isfinite(v.ms) ? std::clamp(v.ms, edges, kUiSoundLongestMs) : edges;
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
