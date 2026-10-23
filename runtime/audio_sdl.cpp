// SDL device output for the portable mix in audio.cpp. Desktop only; Android uses
// audio_aaudio.cpp, which is the same handful of lines against a different API.
#include "runtime.h"
#include "audio_stats.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

void audio_render(int16_t* out, int frames);
void audio_open_wav();
uint64_t audio_frames_out();

static constexpr int OUT_RATE = 48000;
static SDL_AudioStream* g_stream;
static std::string g_device;  // empty: the system's default

// A device by the name SDL lists it under, for a headset whose runtime names its own.
void audio_set_device(const char* name) { g_device = name ? name : ""; }

// Each callback's start, for audio_stats: how often the device comes, and when it's late.
static void callback_started() {
    if (!audio_stats_on()) return;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    audio_stats_callback(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(), audio_frames_out());
}

// SDL asks for `additional` bytes more whenever the device is about to run short. The
// stream takes the mix as it is (48 kHz stereo) and converts it to whatever the device runs.
static void SDLCALL audio_callback(void*, SDL_AudioStream* stream, int additional, int) {
    callback_started();
    static constexpr int kBlock = 1024;
    int16_t buf[kBlock * 2];
    for (int frames = additional / 4; frames > 0;) {
        const int n = std::min(frames, kBlock);
        audio_render(buf, n);
        SDL_PutAudioStreamData(stream, buf, n * 4);
        frames -= n;
    }
}

// SDL3 opens a device by its ID; this finds the one listed under `name`, or 0.
static SDL_AudioDeviceID find_device(const std::string& name) {
    int n = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
    SDL_AudioDeviceID found = 0;
    for (int i = 0; ids && i < n && !found; i++) {
        const char* d = SDL_GetAudioDeviceName(ids[i]);
        if (d && name == d) found = ids[i];
    }
    SDL_free(ids);
    if (!found) SDL_SetError("no output device has that name");
    return found;
}

// SDL's dummy driver waits a relative 10 ms after each pull, so the oversleep and the mix's
// own time add to every period. It takes sound slower than a sound card does (90.8% of the
// rate in one run, 81.7 s of sound in 90 s) and the mix's ring overfills and cuts about once
// a second. In its place, a thread pulls the same 512 frames on deadlines counted from the
// start, so a late wake-up doesn't carry over to the next and the pace is a sound card's,
// without a sound. A stop of the process (a debugger, a signal) doesn't make the thread pull
// all the missed time back to back: more than 100 ms behind, it counts from now instead.
static constexpr int kSilentFrames = 512;
static void silent_clock() {
    using clock = std::chrono::steady_clock;
    static constexpr int kFrames = kSilentFrames;
    static constexpr auto kMaxLate = std::chrono::milliseconds(100);
    int16_t buf[kFrames * 2];
    auto ns_of = [](long long frames) {
        return std::chrono::nanoseconds(frames / OUT_RATE * 1'000'000'000LL + frames % OUT_RATE * 1'000'000'000LL / OUT_RATE);
    };
    clock::time_point start = clock::now();
    for (long long frames = kFrames;; frames += kFrames) {
        callback_started();
        audio_render(buf, kFrames);
        if (clock::now() - (start + ns_of(frames)) > kMaxLate) start = clock::now() + ns_of(kFrames) - ns_of(frames);
        std::this_thread::sleep_until(start + ns_of(frames));
    }
}

bool audio_open() {
    audio_open_wav();
    if (const char* driver = SDL_GetCurrentAudioDriver(); driver && !strcmp(driver, "dummy")) {
        LOG(LOG_AI, "audio: SDL's dummy driver, so a silent clock takes the sound instead");
        audio_stats_opened(driver, OUT_RATE, 2, kSilentFrames);
        std::thread(silent_clock).detach();
        return true;
    }
    const SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, OUT_RATE};
    // 512 frames a pull, as SDL2's device was asked for; SDL3's own choice at 48 kHz is
    // 1024, twice the latency the mix's cushion (audio.cpp) was tuned against.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    g_stream = nullptr;
    if (const char* e = getenv("GCN_AUDIO_DEVICE"); e && *e) g_device = e;
    if (!g_device.empty()) {
        if (const SDL_AudioDeviceID dev = find_device(g_device))
            g_stream = SDL_OpenAudioDeviceStream(dev, &spec, audio_callback, nullptr);
        if (g_stream) fprintf(stderr, "audio: %s\n", g_device.c_str());
        else fprintf(stderr, "audio: cannot open %s (%s); using the default\n", g_device.c_str(), SDL_GetError());
    }
    if (!g_stream) {
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, nullptr);
        const char* name = g_stream ? SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(g_stream)) : nullptr;
        if (g_stream) LOG(LOG_AI, "audio: the default device, %s", name ? name : "?");
    }
    if (!g_stream) {
        LOG(LOG_AI, "audio: cannot open device: %s", SDL_GetError());
        return false;
    }
    // What the device itself runs at, which the stream converts the mix to.
    SDL_AudioSpec dev{};
    int dev_frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_stream), &dev, &dev_frames))
        audio_stats_opened(SDL_GetCurrentAudioDriver(), dev.freq, dev.channels, dev_frames);
    // A stream's device opens paused.
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}
