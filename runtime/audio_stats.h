// The sound device's side of the mix: what the device was opened with, how often it comes
// for sound and when it comes late, where the ring couldn't give it all it asked for, and
// which frame of the DMA's stream each stretch of output was made from. GCN_AUDIOSTATS (or
// GCN_AXSTATS) prints it as [audio] lines, described in docs/diagnostics.md. A backend
// reports its device and its callbacks, and audio.cpp the ring's side.
#pragma once
#include <cstdint>

struct AudioDeviceInfo {
    char driver[32];
    int rate;      // Hz
    int channels;
    int buffer;    // frames the device takes a period
};

// Whether the [audio] lines are on. Read once, on first use.
bool audio_stats_on();
// Called by a backend once it has opened its device, before the first callback.
void audio_stats_opened(const char* driver, int rate, int channels, int buffer_frames);
// What the device was opened with, or null before a backend said.
const AudioDeviceInfo* audio_stats_device();
// Called by a backend as each device callback starts: `now_ns` on a steady clock, and the
// output frames handed over before this callback.
void audio_stats_callback(int64_t now_ns, uint64_t out_frame);
// Called by audio.cpp when the ring left `frames` of a pull unfilled, from output frame `at`.
void audio_stats_underrun(uint64_t at, int frames);
// Called by audio.cpp at each pull the ring plays: output frame `out_frame` is made from
// frame `dma_frame` of the DMA's stream, counted from its first. `jumped` says the ring
// cut or came back from running dry at this pull.
void audio_stats_position(uint64_t out_frame, uint64_t dma_frame, bool jumped);
