// SDL device output for the portable mix in audio.cpp. Desktop only; Android uses
// audio_aaudio.cpp, which is the same handful of lines against a different API.
#include "runtime.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <string>

void audio_render(int16_t* out, int frames);
void audio_open_wav();

static constexpr int OUT_RATE = 48000;
static SDL_AudioStream* g_stream;
static std::string g_device;  // empty: the system's default

// A device by the name SDL lists it under, for a headset whose runtime names its own.
void audio_set_device(const char* name) { g_device = name ? name : ""; }

// SDL asks for `additional` bytes more whenever the device is about to run short. The
// stream takes the mix as it is (48 kHz stereo) and converts it to whatever the device runs.
static void SDLCALL audio_callback(void*, SDL_AudioStream* stream, int additional, int) {
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

bool audio_open() {
    audio_open_wav();
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
    // A stream's device opens paused.
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}
