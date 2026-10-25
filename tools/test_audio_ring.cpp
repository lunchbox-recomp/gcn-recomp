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
// The device stage's accounting (audio_stats.cpp) is checked on the same runs, from the
// lines it prints: starting up isn't an underrun, the stall's underruns cover the pulls it
// left silent and no more, a ring with 100 frames left runs short where those frames run
// out, the output's place in the DMA's stream is right after a cut and after a refill (also
// after a dry spell too short for a periodic position line to fall in it), a callback 1.6
// periods after the last is late where one 1.4 periods after isn't, and each callbacks line
// covers only the gaps since the one before.
//
// DVD music (DTK) holds with the guest. The disc is a fake whose block n has levels of its
// own, so a sample says where in the stream it came from. The real pause gate holds a guest
// thread while the music is pulled, and every run is set against the same stream with no hold.
//
// Held, the music must fade out over the ring's dry fade and then stand silent and still.
// Released, it must fade back in over the ring's rise fade from the sample after the last one
// it played. A pause that's asked for but not yet held, and a run that's never held, must mix
// as they did before the hold existed, the second against a hash taken then.
//
// usage: test_audio_ring
// Exits 0 only if every check passes.
#include "audio.cpp"
#include "audio_stats.cpp"
#include "pause.cpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <string>
#include <thread>

namespace {
// What the AI and the disc give the music, as a check sets them. The stream plays only when
// a check turns it on.
bool g_stream_on;
uint32_t g_stream_rate = 48000, g_vol_left = 255, g_vol_right = 255;
uint64_t g_advanced;  // the samples ai_stream_advance has been told of
bool g_flat_disc;     // every block the same, where a block otherwise has levels of its own
constexpr uint32_t kDiscBlocks = 1000;
}  // namespace

uint32_t ai_volume_left() { return g_vol_left; }
uint32_t ai_volume_right() { return g_vol_right; }
bool ai_stream_playing() { return g_stream_on; }
uint32_t ai_stream_rate() { return g_stream_rate; }
void ai_stream_advance(uint32_t samples) { g_advanced += samples; }
uint64_t host_ns() { return 0; }
void clock_hold(uint64_t) {}

// Block n of the disc has 28 stereo samples at predictor 0 and scale 0, so a sample is its
// nibble, signed, times 4096. The left level is 1 to 7 times that and the right is -7 to -3
// times it, each stepping from one block to the next. A flat disc has 4 and -4 in every block.
bool iso_read(uint64_t off, void* buf, uint32_t n) {
    if (off % 32 || n != 32 || off / 32 >= kDiscBlocks) return false;
    const uint32_t block = (uint32_t)(off / 32);
    const uint8_t left = g_flat_disc ? 4 : 1 + block % 7, right = g_flat_disc ? 12 : 9 + block % 5;
    auto* b = (uint8_t*)buf;
    memset(b, 0, 32);
    for (int i = 4; i < 32; i++) b[i] = (uint8_t)(right << 4 | left);
    return true;
}

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

// The [audio] lines, as audio_stats printed them.
std::vector<std::string> g_lines;
void collect(const char* line) { g_lines.emplace_back(line); }

struct Underrun { unsigned long long at; int frames; };

// The underrun lines from line `from` on.
std::vector<Underrun> underruns_from(size_t from) {
    std::vector<Underrun> got;
    for (size_t i = from; i < g_lines.size(); i++) {
        Underrun u;
        if (sscanf(g_lines[i].c_str(), "[audio] underrun at output frame %llu frames=%d", &u.at, &u.frames) == 2)
            got.push_back(u);
    }
    return got;
}

// The DMA frame the position line at output frame `out` gives, or -1 if there's none.
long long position_at(unsigned long long out) {
    for (const auto& line : g_lines) {
        unsigned long long f, d;
        if (sscanf(line.c_str(), "[audio] output frame %llu plays DMA frame %llu", &f, &d) == 2 && f == out)
            return (long long)d;
    }
    return -1;
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
    double skew = 0;  // how far behind its exact time the device's callbacks start, in seconds

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
        audio_stats_callback(std::llround((pull_time() + skew) * 1e9), g_out_frames);
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

namespace {

// ---- DTK music, held with the guest ----

// Music pulled and the ring's silence mixed into it, as audio_render hands it out.
struct Pcm {
    std::vector<int16_t> l, r;
};

// Starts a stream of `blocks` disc blocks at `rate`, from the first, with nothing decoded.
void dtk_start(uint32_t rate, uint64_t blocks, bool flat) {
    std::lock_guard<std::mutex> lk(g_dtk_mutex);
    g_dtk = DtkState{};
    g_dtk.playing = true;
    g_dtk.len = blocks * 32;
    g_dtk_buf.clear();
    g_dtk_buf_pos = 0;
    g_dtk_pos = 0;
    g_stream_on = true;
    g_stream_rate = rate;
    g_flat_disc = flat;
    g_advanced = 0;
}

// `frames` output frames of music in pulls of `pull`, the last one shorter if it has to be.
// Whatever the ring has left is silent by now, so what comes out is the music's.
void dtk_play(Pcm& pcm, int frames, int pull) {
    int16_t out[2 * 512];
    for (int done = 0; done < frames; done += pull) {
        const int n = std::min(pull, frames - done);
        audio_render(out, n);
        for (int i = 0; i < n; i++) {
            pcm.l.push_back(out[2 * i]);
            pcm.r.push_back(out[2 * i + 1]);
        }
    }
}

// Where the stream stands, apart from what ai_stream_advance was told.
struct DtkAt {
    uint64_t cur;
    size_t buf_pos, buf_size;
    double pos;
    bool operator==(const DtkAt&) const = default;
};
DtkAt dtk_at() { return {g_dtk.cur, g_dtk_buf_pos, g_dtk_buf.size(), g_dtk_pos}; }

// A guest thread held at a frame boundary by the pause gate, as a game's is.
struct HeldGuest {
    std::thread t;
    HeldGuest() {
        pause_request(true);
        t = std::thread([] { pause_gate(0); });
        for (int i = 0; i < 2000 && !paused(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    void release() {
        pause_request(false);
        t.join();
    }
};

// `a` scaled by the fraction n/d, the way the mix scales a sample.
int16_t scaled(int16_t a, int n, int d) { return (int16_t)((int64_t)a * n / d); }

// The frames [from, to) of both channels equal the music's `ref` frames from `ref_from` on,
// each scaled by gain(j) for its place j in the window.
template <class Gain>
bool matches(const Pcm& got, size_t from, size_t to, const Pcm& ref, size_t ref_from, Gain gain) {
    for (size_t j = 0; j < to - from; j++) {
        const auto [n, d] = gain(j);
        if (got.l[from + j] != scaled(ref.l[ref_from + j], n, d) || got.r[from + j] != scaled(ref.r[ref_from + j], n, d))
            return false;
    }
    return true;
}

// The largest step between neighboring left samples in [from, to).
int worst_step(const Pcm& p, size_t from, size_t to) {
    int w = 0;
    for (size_t i = from + 1; i < to; i++) w = std::max(w, std::abs(p.l[i] - p.l[i - 1]));
    return w;
}

// `pre` pulls of music, then the guest held through `held` pulls, then `post` pulls after the
// release, each of `pull` frames, set against the same stream run without a hold. The music
// fades out over the first kDryFade frames of the hold, so the run with no hold plays on that
// far past the hold's start, and the released music picks up there.
void dtk_hold_run(int pull, int pre, int held, int post, bool flat, uint32_t rate = 32000, uint32_t vol_left = 255,
                  uint32_t vol_right = 255) {
    char what[160], kind[64];
    snprintf(kind, sizeof kind, "%s", flat ? "flat" : "varied");
    if (rate != 32000) snprintf(kind + strlen(kind), sizeof kind - strlen(kind), ", %u Hz", rate);
    if (vol_left != 255 || vol_right != 255)
        snprintf(kind + strlen(kind), sizeof kind - strlen(kind), ", volume %u/%u", vol_left, vol_right);
    g_vol_left = vol_left;
    g_vol_right = vol_right;
    const int lead = pre * pull, hold = held * pull, tail = post * pull;
    const int fade_pulls = (kDryFade + pull - 1) / pull;
    const size_t at_hold = lead, quiet = lead + kDryFade, at_release = lead + hold;

    dtk_start(rate, 400, flat);
    Pcm ref;
    dtk_play(ref, lead + kDryFade + tail, pull);
    const DtkAt ref_end = dtk_at();
    const uint64_t ref_advanced = g_advanced;

    dtk_start(rate, 400, flat);
    Pcm got;
    dtk_play(got, lead, pull);
    const DtkAt before = dtk_at();

    HeldGuest guest;
    snprintf(what, sizeof what, "%s, pulls of %d, the pause gate holds the guest", kind, pull);
    check(paused(), what);
    dtk_play(got, fade_pulls * pull, pull);
    const DtkAt faded = dtk_at();
    const uint64_t advanced_faded = g_advanced;
    dtk_play(got, hold - fade_pulls * pull, pull);
    const DtkAt held_still = dtk_at();
    const uint64_t advanced_held = g_advanced;
    guest.release();
    snprintf(what, sizeof what, "%s, pulls of %d, the guest is let go", kind, pull);
    check(!paused(), what);
    dtk_play(got, tail, pull);

    snprintf(what, sizeof what, "%s, pulls of %d, the music before the hold is the stream's", kind, pull);
    check(matches(got, 0, at_hold, ref, 0, [](size_t) { return std::pair{1, 1}; }), what);
    // Linear, and 0 at its last frame, as the ring's fade to silence is.
    snprintf(what, sizeof what, "%s, pulls of %d, the hold fades the music out linearly over %d frames", kind, pull, kDryFade);
    check(matches(got, at_hold, quiet, ref, at_hold, [](size_t j) { return std::pair{kDryFade - 1 - (int)j, kDryFade}; }),
          what);
    bool silent = true;
    for (size_t i = quiet; i < at_release; i++) silent = silent && got.l[i] == 0 && got.r[i] == 0;
    snprintf(what, sizeof what, "%s, pulls of %d, the music is silent through the rest of the hold", kind, pull);
    check(silent, what);
    snprintf(what, sizeof what, "%s, pulls of %d, the fade-out plays the stream on", kind, pull);
    check(!(faded == before), what);
    snprintf(what, sizeof what, "%s, pulls of %d, the stream's place doesn't move once the music is silent", kind, pull);
    check(held_still == faded, what);
    snprintf(what, sizeof what, "%s, pulls of %d, ai_stream_advance isn't told of samples the held music doesn't play", kind, pull);
    check(advanced_held == advanced_faded, what);
    // The released music is the stream from the frame after the last one the fade-out played,
    // not from where an unheld stream would be now, and it rises as the ring comes back, 1/384
    // at its first frame and all of it at its last.
    snprintf(what, sizeof what, "%s, pulls of %d, the release fades the music in linearly over %d frames", kind, pull, kRiseFade);
    check(matches(got, at_release, at_release + kRiseFade, ref, quiet, [](size_t j) { return std::pair{(int)j + 1, kRiseFade}; }),
          what);
    snprintf(what, sizeof what, "%s, pulls of %d, the music goes on from the same block, as if it hadn't been held", kind, pull);
    check(matches(got, at_release + kRiseFade, got.l.size(), ref, quiet + kRiseFade, [](size_t) { return std::pair{1, 1}; }),
          what);
    snprintf(what, sizeof what, "%s, pulls of %d, the stream ends where a run with no hold, the same length of music, does", kind, pull);
    check(dtk_at() == ref_end, what);
    snprintf(what, sizeof what, "%s, pulls of %d, ai_stream_advance is told of samples again after the release", kind, pull);
    check(g_advanced > advanced_held, what);
    // The static fraction it carries from one run to the next can move the total by one.
    snprintf(what, sizeof what, "%s, pulls of %d, the samples ai_stream_advance is told of add up to a run with no hold's", kind, pull);
    check(std::llabs((long long)g_advanced - (long long)ref_advanced) <= 1, what);

    if (!flat) return;
    // On a flat stream the fades are a line, and a click is a step bigger than its slope. A
    // hard stop or a hard start would step by the whole level.
    const int level = ref.l[0];
    const int bar_out = level / kDryFade + 1, bar_in = level / kRiseFade + 1;
    printf("  flat level %d, steepest step %d going out and %d coming in\n", level,
           worst_step(got, at_hold, quiet + 1), worst_step(got, at_release, at_release + kRiseFade + 1));
    snprintf(what, sizeof what, "%s, pulls of %d, a hard stop would step far over the fade-out's steepest step", kind, pull);
    check(level > 10 * bar_out, what);
    snprintf(what, sizeof what, "%s, pulls of %d, a hard start would step far over the fade-in's steepest step", kind, pull);
    check(level > 10 * bar_in, what);
    snprintf(what, sizeof what, "%s, pulls of %d, nothing steps into the fade-out, through it or into the silence", kind, pull);
    check(worst_step(got, at_hold - 1, quiet + 1) <= bar_out && got.l[at_hold - 1] == level, what);
    snprintf(what, sizeof what, "%s, pulls of %d, nothing steps out of the silence, through the fade-in or out of it", kind, pull);
    check(worst_step(got, at_release - 1, at_release + kRiseFade + 1) <= bar_in && got.l[at_release + kRiseFade] == level, what);
}

// `pulls` pulls with the guest held, or let go.
struct Leg {
    bool held;
    int pulls;
};

// What a script's gain did, so a check can tell its pauses were the kind it meant.
struct Walk {
    bool part_way = false;     // a hold ended before the gain reached 0
    bool in_the_rise = false;  // a hold began with the gain between 0 and 1
    bool reached_zero = false;
};

// The stream through a script of holds and releases, in pulls of `pull` frames, against what
// the fades must make of it. A frame plays the stream's next frame at a gain that falls 1/kDryFade
// of the way to 0 in a held frame and rises 1/kRiseFade of the way to 1 in any other. A held
// frame at gain 0 plays nothing and uses no frame, so a pause shorter than the fade stops
// part way, and one that comes in the rise falls from where the rise had got.
Walk dtk_script(const char* name, int pull, uint32_t rate, uint32_t vol_left, uint32_t vol_right, const std::vector<Leg>& script) {
    char what[200];
    constexpr int kFull = kDryFade * kRiseFade;  // the gain's 1, so each step is a whole number
    g_vol_left = vol_left;
    g_vol_right = vol_right;
    int frames = 0;
    for (const Leg& leg : script) frames += leg.pulls * pull;

    dtk_start(rate, 400, false);
    Pcm ref;
    dtk_play(ref, frames, pull);

    Walk walk;
    Pcm want;
    int gain = kFull;
    size_t used = 0;
    for (const Leg& leg : script) {
        if (leg.held && gain > 0 && gain < kFull) walk.in_the_rise = true;
        for (int i = 0; i < leg.pulls * pull; i++) {
            if (leg.held && gain == 0) {
                want.l.push_back(0);
                want.r.push_back(0);
                continue;
            }
            gain = leg.held ? std::max(0, gain - kRiseFade) : std::min(kFull, gain + kDryFade);
            walk.reached_zero = walk.reached_zero || gain == 0;
            want.l.push_back(scaled(ref.l[used], gain, kFull));
            want.r.push_back(scaled(ref.r[used], gain, kFull));
            used++;
        }
        if (leg.held && gain > 0) walk.part_way = true;
    }

    dtk_start(rate, 400, false);
    Pcm got;
    for (const Leg& leg : script) {
        if (!leg.held) {
            dtk_play(got, leg.pulls * pull, pull);
            continue;
        }
        HeldGuest guest;
        dtk_play(got, leg.pulls * pull, pull);
        guest.release();
    }
    const DtkAt got_end = dtk_at();
    const uint64_t got_advanced = g_advanced;

    dtk_start(rate, 400, false);
    Pcm plain;
    dtk_play(plain, (int)used, pull);

    snprintf(what, sizeof what, "%s, the music is the stream at the gain the fades give, frame by frame", name);
    check(got.l == want.l && got.r == want.r, what);
    snprintf(what, sizeof what, "%s, the stream ends where a run with no hold does after the frames it played", name);
    check(got_end == dtk_at(), what);
    snprintf(what, sizeof what, "%s, the samples ai_stream_advance is told of add up to that run's", name);
    check(std::llabs((long long)got_advanced - (long long)g_advanced) <= 1, what);
    g_vol_left = g_vol_right = 255;
    return walk;
}

// A pause that's asked for and not held yet leaves the guest running, so the music plays on.
void dtk_asked_not_held() {
    dtk_start(32000, 400, false);
    Pcm ref;
    dtk_play(ref, 1500, 100);
    dtk_start(32000, 400, false);
    Pcm got;
    pause_request(true);
    check(pause_requested() && !paused(), "a pause is asked for and the guest isn't held");
    dtk_play(got, 1500, 100);
    pause_request(false);
    check(got.l == ref.l && got.r == ref.r, "a pause asked for and not held leaves the music as it was");
}

// The mix of streams that are never held, hashed (FNV-1a over both channels), against the
// hash of the same runs taken before the hold was added.
uint64_t dtk_unheld_hash() {
    uint64_t h = 1469598103934665603ull;
    auto add = [&h](const Pcm& p) {
        for (size_t i = 0; i < p.l.size(); i++)
            for (int16_t v : {p.l[i], p.r[i]}) h = (h ^ (uint16_t)v) * 1099511628211ull;
    };
    // At 32 kHz and under full volume, one region of the disc into the next and then the
    // stream's end, in pulls of unequal sizes.
    g_vol_left = 200;
    g_vol_right = 100;
    dtk_start(32000, 30, false);
    g_dtk.next_start = 40 * 32;
    g_dtk.next_len = 20 * 32;
    Pcm a;
    const int pulls[] = {512, 100, 37, 480, 1};
    int done = 0;
    for (int k = 0; done < 4000; k++) {
        if (done >= 1500) g_dtk.stop_at_end = true;
        dtk_play(a, pulls[k % 5], pulls[k % 5]);
        done += pulls[k % 5];
    }
    add(a);
    // At 48 kHz and full volume, a flat disc.
    g_vol_left = g_vol_right = 255;
    dtk_start(48000, 12, true);
    Pcm b;
    dtk_play(b, 500, 128);
    add(b);
    return h;
}

void dtk_hold_checks() {
    dtk_hold_run(100, 15, 30, 50, false);
    dtk_hold_run(512, 3, 6, 8, false);
    dtk_hold_run(37, 40, 70, 120, false);
    dtk_hold_run(100, 15, 30, 50, true);
    dtk_hold_run(100, 15, 30, 50, false, 48000);
    dtk_hold_run(37, 40, 70, 120, false, 32000, 200, 100);
    g_vol_left = g_vol_right = 255;

    // A pause shorter than the fade, and one that comes during the rise. Each has to fall
    // from the gain it finds to exactly 0 or stop where it is, and rise again from there.
    Walk walk = dtk_script("a pause of 100 frames", 100, 32000, 255, 255, {{false, 10}, {true, 1}, {false, 12}});
    check(walk.part_way && !walk.reached_zero, "a pause of 100 frames ends before the fade does");
    walk = dtk_script("a pause in the rise", 100, 32000, 255, 255,
                      {{false, 10}, {true, 24}, {false, 1}, {true, 24}, {false, 10}});
    check(walk.in_the_rise && walk.reached_zero, "a pause in the rise falls from the gain the rise had reached to 0");
    walk = dtk_script("a pause of 111 frames at 48 kHz, volume 200/100", 37, 48000, 200, 100,
                      {{false, 30}, {true, 3}, {false, 20}});
    check(walk.part_way && !walk.reached_zero, "a pause of 111 frames ends before the fade does");
    walk = dtk_script("a pause in the rise at 48 kHz, volume 200/100", 37, 48000, 200, 100,
                      {{false, 30}, {true, 40}, {false, 3}, {true, 40}, {false, 20}});
    check(walk.in_the_rise && walk.reached_zero, "a pause in the rise at 48 kHz falls from the gain the rise had reached to 0");

    dtk_asked_not_held();
    // Taken from the mix before it knew of a hold. Printed when it differs.
    constexpr uint64_t kUnheldHash = 0x4f1ce1ed9911c824ull;
    const uint64_t got = dtk_unheld_hash();
    if (got != kUnheldHash) printf("  the unheld mix hashes to 0x%016llx\n", (unsigned long long)got);
    check(got == kUnheldHash, "streams that are never held mix as they did before the hold existed");
    g_stream_on = false;
}

}  // namespace

int main() {
    // The cushion sets when the ring cuts and runs dry, so the caller's setting must not.
    // The [audio] lines are on, and collected here instead of printed. The pause gate reads
    // GCN_PAUSE_AT on its first boundary, so that's cleared too.
#ifdef _WIN32
    _putenv_s("GCN_AUDIO_CUSHION_MS", "100");
    _putenv_s("GCN_AUDIOSTATS", "1");
    _putenv_s("GCN_PAUSE_AT", "");
#else
    setenv("GCN_AUDIO_CUSHION_MS", "100", 1);
    setenv("GCN_AUDIOSTATS", "1", 1);
    unsetenv("GCN_PAUSE_AT");
#endif
    g_print = collect;
    audio_stats_opened("test", OUT_RATE, 2, kPull);

    Run run;
    run.play(1, true);
    const int clean = run.worst(0.3, 1);
    const int bar = 3 * clean, far = 10 * bar;
    const double level = run.rms(0.3, 1);
    printf("a clean pull: second difference up to %d, RMS %.0f\n", clean, level);
    check(clean > 0 && level > kAmp / 2, "the tone comes through");

    check(!g_lines.empty() && g_lines[0] == "[audio] device driver=test rate=48000 channels=2 buffer=512\n",
          "the device line gives what the device was opened with");
    check(underruns_from(0).empty(), "the ring filling at the start isn't an underrun");
    check(g.late == 0, "a device that keeps exact time is never late");
    // The position lines: the first where the ring first plays, from the stream's first frame,
    // then one every 0.25 s, the stream moving 2/3 of a frame an output frame give or take the
    // trim's half a percent.
    std::vector<std::pair<unsigned long long, unsigned long long>> pos;
    for (const auto& line : g_lines) {
        unsigned long long f, d;
        if (sscanf(line.c_str(), "[audio] output frame %llu plays DMA frame %llu", &f, &d) == 2) pos.emplace_back(f, d);
    }
    check(!pos.empty() && pos[0].second == 0, "the ring first plays from the stream's first frame");
    bool steady = pos.size() >= 3;
    for (size_t i = 1; i < pos.size(); i++) {
        const double moved = (double)(pos[i].second - pos[i - 1].second);
        const double want = (double)(pos[i].first - pos[i - 1].first) * DMA_RATE / OUT_RATE;
        steady = steady && std::fabs(moved - want) <= want * 0.005 + 1;
    }
    check(steady, "between position lines the stream moves at the output's pace");

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
    const uint64_t cut_out = g_out_frames;
    run.pull();
    check(g_stat_cuts == 1, "the burst overfills the ring once");
    check(position_at(cut_out) == read_at + drop, "after the cut the output plays from where the cut moved the read position");
    const long long pull_frames = (long long)kPull * DMA_RATE / OUT_RATE;
    printf("  the cut dropped %lld frames, and a plain splice would make a second difference of %.0f\n", drop, splice);
    check(splice > far, "a plain splice at the cut would be far over the bar");
    check(std::llabs((long long)g_ring_count - (long long)ring_cushion()) <= pull_frames + 2,
          "the cut leaves the ring at its cushion");
    run.play(2.1, true);
    printf("  around the cut: %d\n", run.worst(1, 1.2));
    check(run.worst(1, 1.2) <= bar, "the cut blends instead of splicing");

    // A stall: nothing for 300 ms, so the ring runs dry and refills. Stopping dead would step
    // from the last frame pushed to zero, and starting dead would step from zero to the frame
    // the refill plays first, the one left in the ring. Nothing is pushed in the stall, so
    // that is the last frame pushed too.
    const long long last = run.tone_frames - 1;
    const double f = tone(last);
    const double stop = std::max(std::fabs(tone(last - u) - 2 * f), std::fabs(f));
    const double start = std::max(std::fabs(tone(last + u) - 2 * f), std::fabs(f));
    const size_t lines_before_stall = g_lines.size();
    run.play(2.4, false);
    run.play(3.2, true);
    check(g_stat_underruns == 1, "the stall runs the ring dry once");
    // The pulls it left silent, as audio_stats saw them: one unbroken stretch from inside the
    // stall to where the ring plays again, from the last frame pushed before the stall, since
    // running dry leaves that one frame in the ring.
    const auto stalled = underruns_from(lines_before_stall);
    bool joined = !stalled.empty() && stalled[0].at > 2.1 * OUT_RATE && stalled[0].at < 2.4 * OUT_RATE;
    for (size_t i = 1; i < stalled.size(); i++) joined = joined && stalled[i].at == stalled[i - 1].at + stalled[i - 1].frames;
    printf("  the stall left %zu pulls short\n", stalled.size());
    check(joined, "the stall's underruns are one unbroken stretch that starts inside it");
    const unsigned long long back = stalled.empty() ? 0 : stalled.back().at + stalled.back().frames;
    check(position_at(back) == last, "the ring plays again where the underruns end, from where the stall left the stream");
    check(g.underruns == stalled.size(), "every underrun line is counted");
    printf("  a hard stop would make a second difference of %.0f, a hard start %.0f\n", stop, start);
    check(stop > far, "a hard stop at the stall would be far over the bar");
    check(start > far, "a hard start after the stall would be far over the bar");
    printf("  running dry: %d, coming back: %d\n", run.worst(2.1, 2.4), run.worst(2.4, 2.7));
    check(run.worst(2.1, 2.4) <= bar, "running dry fades out instead of stopping dead");
    check(run.worst(2.4, 2.7) <= bar, "the sound fades back in after the refill");
    check(run.rms(2.8, 3.2) > level / 2, "the tone comes back");

    // Late callbacks. The device's period is 512 frames at 48 kHz, 10.67 ms, and a callback
    // more than 1.5 of them after the last is late: 1.4 isn't, 1.6 is, at 17.1 ms.
    const double period = (double)kPull / OUT_RATE;
    run.skew += 0.4 * period;
    run.push_due(true);
    run.pull();
    check(g.late == 0, "a callback 1.4 periods after the last is on time");
    run.skew += 0.6 * period;
    run.push_due(true);
    char late_line[96];
    snprintf(late_line, sizeof late_line, "[audio] late callback at output frame %llu gap_ms=17.1\n",
             (unsigned long long)g_out_frames);
    run.pull();
    check(g.late == 1 && std::find(g_lines.begin(), g_lines.end(), late_line) != g_lines.end(),
          "a callback 1.6 periods after the last is late, at the output frame it came at");
    // On to the 400th callback, which prints the counts. Its window of 399 gaps spans 399
    // periods and the one the late callbacks added, and the longest gap is the late one.
    run.play(4.5, true);
    unsigned long long n = 0, late = 0, short_pulls = 0;
    double mean_ms = 0, worst_ms = 0;
    for (const auto& line : g_lines)
        if (sscanf(line.c_str(), "[audio] callbacks n=%llu period_ms=%lf late=%llu worst_ms=%lf underruns=%llu", &n,
                   &mean_ms, &late, &worst_ms, &short_pulls) == 5)
            break;
    printf("  the 400th callback: period %.2f ms, worst %.1f ms\n", mean_ms, worst_ms);
    check(n == 400 && late == 1 && short_pulls == stalled.size(), "the callbacks line counts the callbacks, the late one and the underruns");
    check(std::fabs(mean_ms - 400.0 / 399 * period * 1000) < 0.01 && std::fabs(worst_ms - 1.6 * period * 1000) < 0.06,
          "the callbacks line gives the mean gap and the longest");
    // The 800th callback prints the next line, whose window starts over: 400 gaps of one period
    // each and none late. A window that kept the first one's gaps would show its late gap in the
    // worst, and a mean over 800 gaps.
    run.play(8.7, true);
    {
        int lines = 0;
        unsigned long long n2 = 0, late2 = 0, short2 = 0;
        double mean2 = 0, worst2 = 0;
        for (const auto& line : g_lines)
            if (sscanf(line.c_str(), "[audio] callbacks n=%llu period_ms=%lf late=%llu worst_ms=%lf underruns=%llu", &n2,
                       &mean2, &late2, &worst2, &short2) == 5 && ++lines == 2)
                break;
        printf("  the 800th callback: period %.3f ms, worst %.2f ms\n", mean2, worst2);
        check(lines == 2 && n2 == 800 && late2 == 1, "the second callbacks line carries the counts so far");
        check(std::fabs(mean2 - period * 1000) < 0.01 && std::fabs(worst2 - period * 1000) < 0.06,
              "the second callbacks line gives the mean gap and the longest of its own window");
    }

    // A dry spell that starts just after a position line and ends before the next is due, so
    // only the refill's own jump can put a line where the ring plays again. The stall is
    // 110 ms, over the 100 ms cushion and under the 250 ms between position lines.
    {
        size_t seen = g_lines.size();
        for (bool marked = false; !marked;) {
            run.push_due(true);
            run.pull();
            for (size_t i = seen; i < g_lines.size(); i++)
                if (g_lines[i].rfind("[audio] output frame", 0) == 0) marked = true;
            seen = g_lines.size();
        }
        const long long last_pushed = run.tone_frames - 1;
        const size_t lines_before_spell = g_lines.size();
        run.play(run.pull_time() + 0.11, false);
        run.play(run.pull_time() + 0.2, true);
        const auto spell = underruns_from(lines_before_spell);
        const unsigned long long returned = spell.empty() ? 0 : spell.back().at + spell.back().frames;
        printf("  the short stall left %zu pulls short, and the ring played again at output frame %llu\n", spell.size(),
               returned);
        check(!spell.empty() && position_at(returned) == last_pushed,
              "a dry spell shorter than the time between position lines still marks where the ring plays again");
    }

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
    const bool waited = g_stat_cuts == cuts + 2;
    short_pull();
    check(waited && g_stat_cuts == cuts + 3, "the cut comes on the pull after the crossfade ends");

    // A ring with 100 frames in hand and nothing coming runs dry once 99 of them are read, at
    // 2/3 of a frame an output frame 148.5 output frames in, give or take the trim. So the
    // underrun starts about 149 frames into the pull and runs to its end, and the next pull is
    // short all through.
    while (g_cut_left) short_pull();
    g_ring_r = (g_ring_w + g_ring.size() / 2 - 100) % (g_ring.size() / 2);
    g_ring_count = 100;
    g_dma_pos = 0;
    g_ring_avg = (double)ring_cushion();
    g_ring_filling = false;
    const size_t lines_before_dry = g_lines.size();
    const unsigned long long dry_from = g_out_frames;
    int16_t pulled[kPull * 2];
    audio_render(pulled, kPull);
    audio_render(pulled, kPull);
    const auto dry = underruns_from(lines_before_dry);
    if (!dry.empty()) printf("  100 frames ran out %llu frames into the pull\n", dry[0].at - dry_from);
    check(dry.size() == 2 && dry[0].at + 2 >= dry_from + 149 && dry[0].at <= dry_from + 151 &&
          dry[0].at + dry[0].frames == dry_from + kPull,
          "a ring with 100 frames left runs short where they run out, to the end of the pull");
    check(dry.size() == 2 && dry[1].at == dry_from + kPull && dry[1].frames == kPull,
          "the next pull with nothing in the ring is short all through");

    dtk_hold_checks();

    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
