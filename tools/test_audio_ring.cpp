// Audio ring checks: the `test_audio_ring` target. Runs with no device, disc or game.
//
// runtime/audio.cpp is compiled into this file. Two tones go in as the AI DMA hands them
// over, 160 frames every 5 ms, and a device that keeps exact time pulls 512 frames every
// 512/48000 s, both on one virtual clock, so a run is the same every time. Neither tone's
// period divides the 5 ms block, so a dropped stretch of the stream changes the phase of
// both. Stubbed: the AI's volume and stream (no DVD music plays) and the disc read.
//
// The tones alone set what a clean pull looks like: the largest second difference between
// output samples. The ring then overfills, after a burst from the guest, and runs dry, after
// a stall, and the output around each must stay within 3 times that, so neither may splice
// or drop to silence in one step. A check that passes proves nothing if the scenario would
// have passed anyway, so the signal it generated sets the step a plain splice at the cut
// would have made, and the step a hard stop and a hard start at the stall's edges would
// have made, each as a second difference of the generated signal, and each must be over 10
// times that threshold. Last, short pulls check the cut rules in the fade's own counter.
//
// usage: test_audio_ring
// Exits 0 only if every check passes.
#include "audio.cpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>

uint32_t ai_volume_left() { return 255; }
uint32_t ai_volume_right() { return 255; }
bool ai_stream_playing() { return false; }
uint32_t ai_stream_rate() { return 48000; }
void ai_stream_advance(uint32_t) {}
bool iso_read(uint64_t, void*, uint32_t) { return false; }

namespace {

constexpr int kPull = 512;
constexpr int kBlock = 160;
constexpr double kAmp = 5000;  // of each tone, so the sum peaks at twice this
constexpr double kHzLow = 437, kHzHigh = 619;

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

// The guest's signal at ring frame n.
double tone(double n) {
    const double t = n / DMA_RATE;
    return kAmp * (std::sin(2 * std::numbers::pi * kHzLow * t) + std::sin(2 * std::numbers::pi * kHzHigh * t));
}

struct Run {
    std::vector<int16_t> left;  // the device's left channel, every pull
    long long tone_frames = 0;  // ring frames pushed so far
    long long push_n = 0, pull_n = 0;  // 5 ms blocks due and 512-frame pulls made

    // One 5 ms block of the tones, big endian R, L as AX writes it.
    void push_block() {
        int16_t be[kBlock * 2];
        for (int i = 0; i < kBlock; i++, tone_frames++) {
            const auto v = (int16_t)std::lround(tone(tone_frames));
            be[2 * i] = be[2 * i + 1] = (int16_t)GCN_BSWAP16((uint16_t)v);
        }
        audio_push_dma(be, kBlock);
    }

    double pull_time() const { return (double)pull_n * kPull / OUT_RATE; }

    // The blocks due by the next pull, pushed only when `pushing`.
    void push_due(bool pushing) {
        while (push_n * 0.005 <= pull_time()) {
            if (pushing) push_block();
            push_n++;
        }
    }

    void pull() {
        int16_t out[kPull * 2];
        audio_render(out, kPull);
        for (int i = 0; i < kPull; i++) left.push_back(out[2 * i]);
        pull_n++;
    }

    // Plays on to `to` seconds of virtual time.
    void play(double to, bool pushing) {
        while (pull_time() < to) {
            push_due(pushing);
            pull();
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
    // The cushion sets when the ring cuts and runs dry, so the caller's setting must not.
#ifdef _WIN32
    _putenv_s("GCN_AUDIO_CUSHION_MS", "100");
#else
    setenv("GCN_AUDIO_CUSHION_MS", "100", 1);
#endif

    Run run;
    run.play(1, true);
    const int clean = run.worst(0.3, 1);
    const int bar = 3 * clean, far = 10 * bar;
    const double level = run.rms(0.3, 1);
    printf("a clean pull: second difference up to %d, RMS %.0f\n", clean, level);
    check(clean > 0 && level > kAmp / 2, "the tone comes through");

    // A burst: 250 ms more than the pace, as after a guest stall that the device rode out.
    for (int i = 0; i < 50; i++) run.push_block();
    run.push_due(true);
    // What a plain splice would have done. The read position is the oldest frame in the ring,
    // and the cut moves it on by everything over the cushion.
    // Each is a second difference of the generated signal, at the output's spacing of u ring
    // frames, across the point, so it is the same kind of number as `bar`.
    const double u = (double)DMA_RATE / OUT_RATE;
    const size_t before = g_ring_count;
    const long long read_at = run.tone_frames - (long long)before;
    const long long drop = (long long)before - (long long)ring_cushion();
    const double a = tone(read_at - 1 - u), b = tone(read_at - 1), c = tone(read_at + drop), d = tone(read_at + drop + u);
    const double splice = std::max(std::fabs(a - 2 * b + c), std::fabs(b - 2 * c + d));
    run.pull();
    check(g_stat_cuts == 1, "the burst overfills the ring once");
    const long long pull_frames = (long long)kPull * DMA_RATE / OUT_RATE;
    printf("  the cut dropped %lld frames, and a plain splice would make a second difference of %.0f\n", drop, splice);
    check(splice > far, "a plain splice at the cut would be far over the bar");
    check(std::llabs((long long)g_ring_count - (long long)ring_cushion()) <= pull_frames + 2,
          "the cut leaves the ring at its cushion");
    run.play(2.1, true);
    printf("  around the cut: %d\n", run.worst(1, 1.2));
    check(run.worst(1, 1.2) <= bar, "the cut blends instead of splicing");

    // A stall: nothing for 300 ms, so the ring runs dry and refills. Stopping dead would step
    // from the last frame pushed to zero, and starting dead from zero to the first one after.
    // Nothing is pushed in the stall, so the last frame pushed is also the one left in the
    // ring, and the one the refill plays first.
    const long long last = run.tone_frames - 1;
    const double f = tone(last);
    const double stop = std::max(std::fabs(tone(last - u) - 2 * f), std::fabs(f));
    const double start = std::max(std::fabs(tone(last + u) - 2 * f), std::fabs(f));
    run.play(2.4, false);
    run.play(3.2, true);
    check(g_stat_underruns == 1, "the stall runs the ring dry once");
    printf("  a hard stop would make a second difference of %.0f, a hard start %.0f\n", stop, start);
    check(stop > far, "a hard stop at the stall would be far over the bar");
    check(start > far, "a hard start after the stall would be far over the bar");
    printf("  running dry: %d, coming back: %d\n", run.worst(2.1, 2.4), run.worst(2.4, 2.7));
    check(run.worst(2.1, 2.4) <= bar, "running dry fades out instead of stopping dead");
    check(run.worst(2.4, 2.7) <= bar, "the sound fades back in after the refill");
    check(run.rms(2.8, 3.2) > level / 2, "the tone comes back");

    // Cuts, seen in the fade's own counter. Pulls of 100 frames are shorter than the 480 of a
    // crossfade, so one still running is there to see after the pull that armed it.
    auto short_pull = [] {
        int16_t out[100 * 2];
        audio_render(out, 100);
    };
    const int cuts = g_stat_cuts;
    // More than half full: the cut splices and arms no fade.
    for (int i = 0; i < 100; i++) run.push_block();
    check(g_ring_count > g_ring.size() / 4, "the ring is over half full");
    short_pull();
    check(g_stat_cuts == cuts + 1 && g_cut_left == 0, "a cut over half full splices with no crossfade");
    // Under half full: the cut arms a fade, and a second burst during it waits its turn.
    for (int i = 0; i < 30; i++) run.push_block();
    short_pull();
    check(g_stat_cuts == cuts + 2 && g_cut_left == kCutFade - 100, "a cut under half full runs a crossfade");
    for (int i = 0; i < 30; i++) run.push_block();
    check(g_ring_count > ring_limit(), "the ring is over the limit again during the fade");
    short_pull();
    check(g_stat_cuts == cuts + 2 && g_cut_left == kCutFade - 200, "a cut during a running crossfade waits");
    while (g_cut_left) short_pull();
    short_pull();
    check(g_stat_cuts == cuts + 3, "the cut comes on the pull after the crossfade ends");

    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
