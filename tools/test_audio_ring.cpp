// Audio ring checks: the `test_audio_ring` target. Runs with no device, disc or game.
//
// runtime/audio.cpp is compiled into this file. A 440 Hz tone goes in as the AI DMA hands
// it over, 160 frames every 5 ms, and a device that keeps exact time pulls 512 frames every
// 512/48000 s, both on one virtual clock, so a run is the same every time. Stubbed: the AI's
// volume and stream (no DVD music plays) and the disc read.
//
// The tone alone sets what a clean pull looks like: the largest second difference between
// output samples. The ring then overfills, after a burst from the guest, and runs dry, after
// a stall, and the output around each must stay within 3 times that, so neither may splice
// or drop to silence in one step.
//
// usage: test_audio_ring
// Exits 0 only if every check passes.
#include "audio.cpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

uint32_t ai_volume_left() { return 255; }
uint32_t ai_volume_right() { return 255; }
bool ai_stream_playing() { return false; }
uint32_t ai_stream_rate() { return 48000; }
void ai_stream_advance(uint32_t) {}
bool iso_read(uint64_t, void*, uint32_t) { return false; }

namespace {

constexpr int kPull = 512;
constexpr int kBlock = 160;
constexpr double kAmp = 10000;

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

struct Run {
    std::vector<int16_t> left;  // the device's left channel, every pull
    long long tone_frames = 0;

    // One 5 ms block of the tone, big endian R, L as AX writes it.
    void push_block() {
        int16_t be[kBlock * 2];
        for (int i = 0; i < kBlock; i++, tone_frames++) {
            const auto v = (int16_t)std::lround(kAmp * std::sin(2 * M_PI * 440 * tone_frames / DMA_RATE));
            be[2 * i] = be[2 * i + 1] = (int16_t)GCN_BSWAP16((uint16_t)v);
        }
        audio_push_dma(be, kBlock);
    }

    // Plays from `from` to `to` seconds of virtual time, pushing blocks only when `pushing`.
    void play(double from, double to, bool pushing) {
        long long push_n = std::llround(from / 0.005), pull_n = std::llround(from * OUT_RATE / kPull);
        while (true) {
            const double push_t = push_n * 0.005, pull_t = (double)pull_n * kPull / OUT_RATE;
            if (std::min(push_t, pull_t) >= to) return;
            if (push_t <= pull_t) {
                if (pushing) push_block();
                push_n++;
            } else {
                int16_t out[kPull * 2];
                audio_render(out, kPull);
                for (int i = 0; i < kPull; i++) left.push_back(out[2 * i]);
                pull_n++;
            }
        }
    }

    // The largest second difference in output samples [from, to) seconds.
    int worst(double from, double to) const {
        int w = 0;
        const size_t a = (size_t)(from * OUT_RATE) + 1, b = std::min(left.size() - 1, (size_t)(to * OUT_RATE));
        for (size_t i = a; i < b; i++) w = std::max(w, std::abs(left[i + 1] - 2 * left[i] + left[i - 1]));
        return w;
    }

    double rms(double from, double to) const {
        double sum = 0;
        const size_t a = (size_t)(from * OUT_RATE), b = std::min(left.size(), (size_t)(to * OUT_RATE));
        for (size_t i = a; i < b; i++) sum += (double)left[i] * left[i];
        return std::sqrt(sum / (double)(b - a));
    }
};

}  // namespace

int main() {
    Run run;
    run.play(0, 1, true);
    const int clean = run.worst(0.3, 1);
    const double level = run.rms(0.3, 1);
    printf("a clean pull: second difference up to %d, RMS %.0f\n", clean, level);
    check(clean > 0 && level > kAmp / 2, "the tone comes through");

    // A burst: 250 ms more than the pace, as after a guest stall that the device rode out.
    for (int i = 0; i < 50; i++) run.push_block();
    run.play(1, 2, true);
    check(g_stat_cuts == 1, "the burst overfills the ring once");
    printf("  around the cut: %d\n", run.worst(1, 1.2));
    check(run.worst(1, 1.2) <= 3 * clean, "the cut blends instead of splicing");
    check(g_ring_count <= ring_cushion() + kPull, "the cut leaves the ring at its cushion");

    // A stall: nothing for 300 ms, so the ring runs dry and refills.
    run.play(2, 2.3, false);
    run.play(2.3, 3.2, true);
    check(g_stat_underruns == 1, "the stall runs the ring dry once");
    printf("  running dry: %d, coming back: %d\n", run.worst(2, 2.3), run.worst(2.3, 2.6));
    check(run.worst(2, 2.3) <= 3 * clean, "running dry fades out instead of stopping dead");
    check(run.worst(2.3, 2.6) <= 3 * clean, "the sound fades back in after the refill");
    check(run.rms(2.8, 3.2) > level / 2, "the tone comes back");

    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
