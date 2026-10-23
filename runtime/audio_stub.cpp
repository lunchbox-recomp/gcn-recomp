// Stands in for audio.cpp in builds that leave SDL out (the benchmark and the headless
// EGL frontend). The rest of the runtime still references these, so they have to exist:
// the DSP HLE hands over mixed sample blocks, the DI reads and writes the DVD streaming
// state while servicing the game's audio-stream commands, and a pause's log line says
// where the sound had got to.
#include "hw/dtk.h"
#include <cstdint>
#include <mutex>

void audio_push_dma(const int16_t*, uint32_t) {}
uint64_t audio_frames_out() { return 0; }

DtkState g_dtk;
std::mutex g_dtk_mutex;
