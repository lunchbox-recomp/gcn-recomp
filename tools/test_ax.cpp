// AX mixer checks: the `test_ax` target. Runs with no device, disc or game.
//
// runtime/hw/ax.cpp is compiled into this file over a fake main memory and ARAM. Each check
// writes parameter blocks, sample data and a command list into that memory by hand, runs
// the list through ax_process_cmdlist as the DSP mail would, and compares what the mixer
// wrote or counted with numbers worked out by hand here. Each check is written so that the
// mistake it covers would change those numbers.
//
// usage: test_ax
// Exits 0 only if every check passes.
#include "hw/ax.cpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

uint8_t* g_mem;
uint8_t* g_aram;
bool g_log_enabled[LOG_COUNT];
void log_msg(LogCat, const char*, ...) {}
uint32_t vm_translate(uint32_t ea, int) { return ea; }  // the checks use only BAT addresses

namespace {

// Where the checks put things in guest memory, and where the samples sit in ARAM.
constexpr uint32_t kList = 0x80001000, kSetup = 0x80002000, kPB = 0x80003000;
constexpr uint32_t kSurround = 0x80004000, kLR = 0x80005000, kUpdates = 0x80006000;
constexpr uint32_t kSampleAt = 0x800;  // in samples, so byte 0x1000 for PCM16
constexpr uint32_t kHashLater = 0x07F88145, kHash2001 = 0x4E8A8B21;

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

void reset(uint32_t hash) {
    memset(g_mem, 0, RAM_SIZE);
    memset(g_aram, 0, ARAM_SIZE);
    ax_init(hash);
    g_axstats = true;
    g_sw = StatsWindow{};
}

// A running PCM16 voice with no rate conversion that loops `n` samples of `value` forever,
// with the volume envelope at `env` and main L at `vol_l`. Nothing else is set. The
// samples go into ARAM, so call it after reset().
PB pcm_voice(int16_t value, int n, uint16_t env, uint16_t vol_l) {
    for (int i = 0; i < n; i++) {
        g_aram[2 * (kSampleAt + i)] = (uint8_t)((uint16_t)value >> 8);
        g_aram[2 * (kSampleAt + i) + 1] = (uint8_t)value;
    }
    PB pb;
    pb.w[PB_RUNNING] = 1;
    pb.w[PB_SRC_TYPE] = 2;
    pb.w[PB_MIXER_CONTROL] = 0x0001;
    pb.w[PB_MIXER + kMixerWord[0]] = vol_l;
    pb.w[PB_VOL] = env;
    pb.w[PB_LOOPING] = 1;
    pb.w[PB_FORMAT] = 0x0A;
    pb.set32(PB_LOOP_HI, kSampleAt);
    pb.set32(PB_END_HI, kSampleAt + n - 1);
    pb.set32(PB_CUR_HI, kSampleAt);
    pb.set32(PB_RATIO_HI, 0x10000);
    return pb;
}

// All 97 words, whatever the build, so a check can see which of them the mixer reads.
void store_pb(const PB& pb) {
    for (int i = 0; i < PB_WORDS_LATER; i++) mem_w16(kPB + 2 * i, pb.w[i]);
}
uint16_t pb_word(int i) { return mem_r16(kPB + 2 * i); }

// One list: setup from kSetup, the one PB at kPB, then output.
void run_list() {
    uint32_t p = kList;
    auto put = [&](uint16_t v) { mem_w16(p, v); p += 2; };
    auto put32 = [&](uint32_t v) { put((uint16_t)(v >> 16)); put((uint16_t)v); };
    put(0x00); put32(kSetup);
    put(0x02); put32(kPB);
    put(0x03);
    put(0x0E); put32(kSurround); put32(kLR);
    put(0x0F);
    ax_process_cmdlist(kList, (uint16_t)(p - kList));
}

int16_t out_l(int i) { return (int16_t)mem_r16(kLR + 4 * i + 2); }
int16_t out_r(int i) { return (int16_t)mem_r16(kLR + 4 * i); }

// GCN_AXSTATS counts what each running voice asks for, once a millisecond.
void stats_checks() {
    reset(kHashLater);
    PB pb = pcm_voice(1000, 32, 0x4000, 0x8000);
    pb.w[PB_SRC_TYPE] = 0;
    pb.w[PB_COEF_SELECT] = 1;
    pb.w[PB_MIXER_CONTROL] = 0x000B;          // main L and R, ramped
    pb.w[PB_MIXER + kMixerWord[1] + 1] = 4;   // main R's delta
    pb.w[PB_VOL_DELTA] = 0xFFFF;
    pb.w[PB_LPF_ON] = 1;
    pb.w[PB_LPF_A0] = 0x7FFF;
    store_pb(pb);
    mem_w16(kSetup + 6 * 3 + 2, 160);         // AuxA L's start
    mem_w16(kSetup + 6 * 3 + 4, 0xFFFF);      // and delta
    run_list();
    check(g_sw.voice_ms == 5 && g_sw.src[0] == 5 && g_sw.coef[1] == 5 && g_sw.coef[0] == 0,
          "a polyphase voice counts 5 ms under src0 and coefficient set 1");
    check(g_sw.lpf == 5, "the later build counts the low-pass filter on in all 5 ms");
    check(g_sw.ramp == 5 && g_sw.env_ramp == 5, "a ramped bus and an envelope delta count in all 5 ms");
    check(g_sw.depop == 1, "a setup that starts one bus off zero counts as a depop list");

    // The same voice with its ramp bit clear and no deltas.
    reset(kHashLater);
    pb.w[PB_MIXER_CONTROL] = 0x0003;
    pb.w[PB_VOL_DELTA] = 0;
    store_pb(pb);
    run_list();
    check(g_sw.ramp == 0 && g_sw.env_ramp == 0 && g_sw.depop == 0,
          "a delta with its ramp bit clear, and a flat setup, count nothing");

    // A type 2 voice with set 1 in word 5, mixed into main L, with AuxA's ramp bit and a delta
    // but AuxA off.
    reset(kHashLater);
    PB plain = pcm_voice(1000, 32, 0x4000, 0x8000);
    plain.w[PB_COEF_SELECT] = 1;
    plain.w[PB_MIXER_CONTROL] = 0x0041;
    plain.w[PB_MIXER + kMixerWord[3] + 1] = 4;   // AuxA L's delta
    store_pb(plain);
    run_list();
    check(g_sw.src[2] == 5 && g_sw.coef[1] == 0 && g_sw.ramp == 0,
          "a type 2 voice counts no coefficient set, and a ramped bus that's off counts no ramp");

    // The 2001 build's PB stops at word 92, so word 93 is the next thing in memory, and an
    // update to it in the first millisecond is dropped there and kept on the later build.
    for (uint32_t hash : {kHash2001, kHashLater}) {
        reset(hash);
        pb.w[PB_MIXER_CONTROL] = 0x0000;
        pb.w[PB_UPDATES] = 1;
        pb.set32(PB_UPDATES + 5, kUpdates);
        mem_w16(kUpdates, PB_LPF_YN1);
        mem_w16(kUpdates + 2, 0x5555);
        store_pb(pb);
        run_list();
        if (hash == kHash2001) {
            check(g_sw.voice_ms == 5 && g_sw.lpf == 0, "the 2001 build never reads a low-pass filter");
            check(pb_word(PB_LPF_YN1) == 0, "the 2001 build drops an update past word 92");
        } else {
            check(pb_word(PB_LPF_YN1) == 0x5555, "the later build keeps an update to word 94");
        }
    }
}

// Each enabled bus leaves the last sample it took from a voice in the voice's depop word,
// which is where the AX library reads it from when it stops the voice.
void depop_checks() {
    // 1000 at an envelope of 0.5 is 500 into the mixer: main L at 1.0, main R at 0.5 and
    // AuxA L at 0.25. Every depop word starts at a marker the mixer must leave on the six
    // buses it doesn't mix.
    reset(kHashLater);
    PB pb = pcm_voice(1000, 32, 0x4000, 0x8000);
    pb.w[PB_MIXER_CONTROL] = 0x0013;
    pb.w[PB_MIXER + kMixerWord[1]] = 0x4000;
    pb.w[PB_MIXER + kMixerWord[3]] = 0x2000;
    for (int k = 0; k < 9; k++) pb.w[PB_DPOP + k] = 0x1234;
    store_pb(pb);
    run_list();
    check(out_l(0) == 500 && out_l(159) == 500, "a voice of 1000 at an envelope of 0.5 mixes 500 into main L");
    check(pb_word(PB_DPOP + 0) == 500 && pb_word(PB_DPOP + 3) == 250 && pb_word(PB_DPOP + 1) == 125,
          "main L, main R and AuxA L leave 500, 250 and 125 in depop words 0, 3 and 1");
    bool kept = true;
    for (int k : {2, 4, 5, 6, 7, 8}) kept &= pb_word(PB_DPOP + k) == 0x1234;
    check(kept, "the six buses the voice isn't mixed into leave their depop words alone");

    // Ramping main L from 0.5 by 16 a sample, the last sample of the list goes out at
    // 0x4000 + 159 * 16 = 18928, and 500 * 18928 >> 15 is 288.
    reset(kHashLater);
    pb = pcm_voice(1000, 32, 0x4000, 0x4000);
    pb.w[PB_MIXER_CONTROL] = 0x0009;
    pb.w[PB_MIXER + kMixerWord[0] + 1] = 16;
    store_pb(pb);
    run_list();
    check(out_l(0) == 250 && out_l(159) == 288 && pb_word(PB_DPOP + 0) == 288,
          "a ramped bus leaves the list's last sample, 288, not its first");

    // A negative voice that runs out of samples 8 into its second millisecond: the last
    // millisecond it plays ends in zeros, and the mixer skips it after that.
    reset(kHashLater);
    pb = pcm_voice(-1000, 40, 0x4000, 0x8000);
    pb.w[PB_LOOPING] = 0;
    pb.w[PB_DPOP + 0] = 0x1234;
    store_pb(pb);
    run_list();
    check(out_l(39) == -500 && out_l(40) == 0 && pb_word(PB_RUNNING) == 0,
          "a one-shot voice of 40 samples plays 40 and stops");
    check(pb_word(PB_DPOP + 0) == 0, "a voice that ran out leaves 0, the last sample it played");
    reset(kHashLater);
    pb = pcm_voice(-1000, 32, 0x4000, 0x8000);
    store_pb(pb);
    run_list();
    check(pb_word(PB_DPOP + 0) == (uint16_t)-500, "a looping voice of -1000 leaves -500");

    // -20000 at a main L volume of 0xFFFF is -40000, which the mix clamps to -32768. 0E's output
    // clamp stops there too, so it changes no sample.
    reset(kHashLater);
    pb = pcm_voice(-20000, 32, 0x8000, 0xFFFF);
    store_pb(pb);
    run_list();
    check(pb_word(PB_DPOP + 0) == 0x8000 && out_l(0) == -32768, "a sample past -32768 leaves -32768 in its depop word and the output");
    check(g_sw.clipped == 0, "and 0E's clamp, which stops at -32768, counts no clipped sample");

    // The same voice over a main L that setup started at -5 is -32773, and a main R started at
    // -40000 is that with no voice. 0E clamps both to -32768 in all 160 samples.
    reset(kHashLater);
    pb = pcm_voice(-20000, 32, 0x8000, 0xFFFF);
    mem_w16(kSetup, 0xFFFF);
    mem_w16(kSetup + 2, (uint16_t)-5);
    mem_w16(kSetup + 6, 0xFFFF);
    mem_w16(kSetup + 8, (uint16_t)-40000);
    store_pb(pb);
    run_list();
    check(out_l(0) == -32768 && out_l(159) == -32768 && out_r(0) == -32768 && out_r(159) == -32768 && g_sw.clipped == 2 * SPF,
          "main L and R sums past -32768 come out at -32768 and count as clipped");

    // The library fades a stopped voice out through the setup command: main L starting at
    // 500 and falling 3 a sample, with no voice running, is the whole of main L.
    reset(kHashLater);
    mem_w16(kSetup + 2, 500);
    mem_w16(kSetup + 4, (uint16_t)-3);
    store_pb(PB{});
    run_list();
    check(out_l(0) == 500 && out_l(1) == 497 && out_l(159) == 23, "setup's start and slope reach the output");

    // A slope with a start of 0 is dropped, so main L stays silent and the list isn't a depop.
    reset(kHashLater);
    mem_w16(kSetup + 4, (uint16_t)-3);
    store_pb(PB{});
    run_list();
    check(out_l(0) == 0 && out_l(159) == 0 && g_sw.depop == 0, "a slope with a start of 0 leaves main L silent and counts no depop");
}

// One list of an ADPCM voice over 0x100 bytes of ARAM, each 8-byte frame of them the header
// byte 0xFF and then seven bytes of `data`. With that header the scale is 1 << 15 and the
// prediction coefficients, in PB words that stay 0, add nothing, so each sample decodes to
// its nibble times 32768.
void run_adpcm(uint8_t data) {
    reset(kHashLater);
    constexpr uint32_t kNibble = 0x1000;  // at ARAM byte 0x800, and a multiple of 16 for a header
    for (uint32_t i = 0; i < 0x100; i++) g_aram[kNibble / 2 + i] = i % 8 ? data : 0xFF;
    PB pb = pcm_voice(0, 1, 0x8000, 0x8000);
    pb.w[PB_FORMAT] = 0x00;
    pb.set32(PB_LOOP_HI, kNibble);
    pb.set32(PB_END_HI, kNibble + 0x1FF);
    pb.set32(PB_CUR_HI, kNibble);
    store_pb(pb);
    run_list();
}

// The ADPCM decoder saturates to 16 bits, and what it decoded feeds the next prediction.
void adpcm_checks() {
    // Every nibble of 0xF is -1, so each sample decodes to -32768 exactly, where a clamp at
    // -32767 would give -32767.
    run_adpcm(0xFF);
    check(pb_word(PB_YN1) == 0x8000 && pb_word(PB_YN2) == 0x8000, "an ADPCM sample that decodes to -32768 is kept as -32768 in the history");
    // A nibble of 8 is -8, so -262144, which wrapped to 16 bits would be 0.
    run_adpcm(0x88);
    check(pb_word(PB_YN1) == 0x8000 && pb_word(PB_YN2) == 0x8000, "an ADPCM sample below -32768 saturates to -32768");
    // A nibble of 7 gives 229376, which wrapped would be -32768.
    run_adpcm(0x77);
    check(pb_word(PB_YN1) == 0x7FFF && pb_word(PB_YN2) == 0x7FFF, "an ADPCM sample above 32767 saturates to 32767");
}

// Source type 0 filters the last four samples in with one of 128 phases of a computed set.
void polyphase_checks() {
    reset(kHashLater);
    bool sums = true, mirrored = true;
    for (int s = 0; s < kPolySets; s++) {
        for (int p = 0; p < kPolyPhases; p++) {
            int total = 0;
            for (int k = 0; k < 4; k++) total += g_poly[s][p][k];
            sums &= total == 0x7FFF;
            for (int k = 0; k < 4 && p > 0 && p < kPolyPhases / 2; k++)
                mirrored &= g_poly[s][p][k] == g_poly[s][kPolyPhases - p][3 - k];
        }
    }
    check(sums, "every phase of every set sums to 0x7FFF");
    check(mirrored, "phase p and phase 128 - p are each other's taps reversed");
    // Phase 64 is its own mirror image, as near as an odd sum lets it be.
    bool middle = true;
    for (int s = 0; s < kPolySets; s++) {
        const int16_t* c = g_poly[s][kPolyPhases / 2];
        middle &= c[0] == c[3] && std::abs(c[1] - c[2]) == 1;
    }
    check(middle, "phase 64's outer taps match and its middle taps differ by 1");
    const int16_t* full = g_poly[2][0];
    check(full[0] == 0 && full[1] == 0x7FFF && full[2] == 0 && full[3] == 0,
          "the full-band set's phase 0 takes the second oldest sample alone");
    // Entries pinned at what the windows give, so that a change to a window, a cutoff or the
    // rounding shows here as well as in every audio hash. Set 2's phase 1 is one whose middle
    // tap would pass 0x7FFF.
    auto entry_is = [](int s, int p, std::initializer_list<int> want) {
        bool same = true;
        int k = 0;
        for (int v : want) same &= g_poly[s][p][k++] == v;
        return same;
    };
    check(entry_is(0, 0, {6675, 19417, 6675, 0}) && entry_is(0, 64, {1252, 15132, 15131, 1252}) &&
              entry_is(1, 32, {420, 23507, 9044, -204}) && entry_is(2, 1, {-136, 32767, 146, -10}) &&
              entry_is(2, 32, {-2140, 28372, 7018, -483}),
          "five pinned entries keep the values the windows give");

    // A constant 1000 at a ratio of 1 always lands on phase 0: 1000 * 0x7FFF >> 15 is 999 for
    // the polyphase sets. Linear interpolation, which a set past the three falls back to,
    // passes 1000.
    for (uint16_t coef : {0, 2, 3}) {
        reset(kHashLater);
        PB pb = pcm_voice(1000, 64, 0x8000, 0x8000);
        pb.w[PB_SRC_TYPE] = 0;
        pb.w[PB_COEF_SELECT] = coef;
        store_pb(pb);
        run_list();
        char what[96];
        snprintf(what, sizeof what, "a constant 1000 comes out at %d with coefficient set %u", coef < 3 ? 999 : 1000, coef);
        check(out_l(100) == (coef < 3 ? 999 : 1000), what);
    }
    // Source type 1 interpolates linearly whatever set word 5 picks.
    reset(kHashLater);
    PB lin = pcm_voice(1000, 64, 0x8000, 0x8000);
    lin.w[PB_SRC_TYPE] = 1;
    store_pb(lin);
    run_list();
    check(out_l(100) == 1000, "a constant 1000 comes out at 1000 through source type 1 with set 0");

    // Samples alternating between +10000 and -10000 sit at the input's Nyquist frequency. The
    // full-band set passes them, and the three-quarter and half-band sets hold them to about
    // 0.6 and 0.19 of that.
    int peak[3] = {};
    for (uint16_t coef : {0, 1, 2}) {
        reset(kHashLater);
        PB pb = pcm_voice(10000, 64, 0x8000, 0x8000);
        for (int i = 1; i < 64; i += 2) {
            g_aram[2 * (kSampleAt + i)] = (uint8_t)((uint16_t)-10000 >> 8);
            g_aram[2 * (kSampleAt + i) + 1] = (uint8_t)(uint16_t)-10000;
        }
        pb.w[PB_SRC_TYPE] = 0;
        pb.w[PB_COEF_SELECT] = coef;
        store_pb(pb);
        run_list();
        for (int i = 8; i < SPF; i++) peak[coef] = std::max(peak[coef], std::abs((int)out_l(i)));
    }
    char what[128];
    snprintf(what, sizeof what, "Nyquist at 10000 comes out at %d, %d and %d through the full-band, three-quarter and half-band sets",
             peak[2], peak[1], peak[0]);
    check(peak[2] >= 9999 && peak[1] >= 5500 && peak[1] <= 6500 && peak[0] >= 1600 && peak[0] <= 2100, what);

    // Set 2's phase 64 has a gain of 1.18, so [-32768, 32767, 32767, -32768] there comes to
    // 38677, which saturates at 32767 and would wrap to -26859. A loop of those four samples at
    // a ratio of 1 from a fraction of a half puts every sample out on phase 64, with the last
    // four in lined up that way once in four and the other way round once in four.
    reset(kHashLater);
    PB loud = pcm_voice(-32768, 4, 0x8000, 0x8000);
    for (int i : {1, 2}) {
        g_aram[2 * (kSampleAt + i)] = 0x7F;
        g_aram[2 * (kSampleAt + i) + 1] = 0xFF;
    }
    loud.w[PB_SRC_TYPE] = 0;
    loud.w[PB_COEF_SELECT] = 2;
    loud.w[PB_FRAC] = 0x8000;
    store_pb(loud);
    run_list();
    int lo = 0, hi = 0;
    for (int i = 8; i < SPF; i++) {
        lo = std::min(lo, (int)out_l(i));
        hi = std::max(hi, (int)out_l(i));
    }
    snprintf(what, sizeof what, "full scale through set 2's phase 64 saturates at %d and %d instead of wrapping", lo, hi);
    check(lo == -32767 && hi == 32767, what);

    // Two lists of a stream of distinct samples at ratios under and over 1, against the
    // position worked out from scratch for each sample out: (i + 1) * ratio, of which the
    // whole part counts the samples in and the fraction's top 7 bits pick the phase. The
    // ratios are odd, so 32 samples out never end on a whole sample in and the fraction has
    // to carry from one millisecond to the next. Source type 1 takes the same stream at the
    // lower ratio. It takes each sample out before the step, at i * ratio, from the line
    // between the newest two samples in.
    struct Run {
        uint16_t type;
        uint32_t ratio;
    };
    for (Run run : {Run{0, 0x0C3A5u}, Run{0, 0x1A0F7u}, Run{1, 0x0C3A5u}}) {
        reset(kHashLater);
        constexpr int kIn = 1024;
        const uint32_t ratio = run.ratio;
        PB pb = pcm_voice(0, kIn, 0x8000, 0x8000);
        auto input = [](int64_t n) { return n < 0 ? 0 : (int)(n * 37 % 2001) - 1000; };
        for (int n = 0; n < kIn; n++) {
            g_aram[2 * (kSampleAt + n)] = (uint8_t)((uint16_t)input(n) >> 8);
            g_aram[2 * (kSampleAt + n) + 1] = (uint8_t)input(n);
        }
        pb.w[PB_SRC_TYPE] = run.type;
        pb.w[PB_COEF_SELECT] = 0;
        pb.set32(PB_RATIO_HI, ratio);
        store_pb(pb);
        int bad = 0;
        for (int list = 0; list < 2; list++) {
            run_list();
            for (int j = 0; j < SPF; j++) {
                const int64_t i = list * SPF + j;
                int16_t want;
                if (run.type == 0) {
                    const int64_t pos = (i + 1) * ratio, in = pos >> 16;
                    const int16_t* c = g_poly[0][(pos & 0xFFFF) >> 9];
                    int64_t sum = 0;
                    for (int k = 0; k < 4; k++) sum += (int64_t)input(in - 4 + k) * c[k];
                    want = (int16_t)std::clamp<int64_t>(sum >> 15, -32768, 32767);
                } else {
                    const int64_t pos = i * ratio, in = pos >> 16, a = input(in - 2), b = input(in - 1);
                    want = (int16_t)(a + (((b - a) * (pos & 0xFFFF)) >> 16));
                }
                bad += out_l(j) != want;
            }
        }
        const int64_t end = (int64_t)2 * SPF * ratio, in = end >> 16;
        bool state = pb_word(PB_FRAC) == (uint16_t)end;
        for (int k = 0; k < 4; k++) state &= (int16_t)pb_word(PB_LAST + k) == input(in - 4 + k);
        char what[96];
        snprintf(what, sizeof what, "type %u at ratio %05X: 320 samples out match the position worked out for each",
                 run.type, ratio);
        check(bad == 0, what);
        snprintf(what, sizeof what, "type %u at ratio %05X: the fraction and the last four samples carry to the next list",
                 run.type, ratio);
        check(state, what);
    }
}

// A voice loops or stops only on reading the sample at its end address, as the DSP's
// accelerator does. A stream moves its loop address to the next slot before it moves its end
// there, so a voice that loops into a slot above its old end has to play on from the loop.
void end_checks() {
    // 1000 for 32 samples, then 2000 for 0x200 samples from 0x100 on. The voice starts two
    // before its end, so it plays two of 1000, loops to 0x100 and plays 158 of 2000.
    reset(kHashLater);
    PB pb = pcm_voice(1000, 32, 0x8000, 0x8000);
    for (int i = 0; i < 0x200; i++) {
        g_aram[2 * (kSampleAt + 0x100 + i)] = 2000 >> 8;
        g_aram[2 * (kSampleAt + 0x100 + i) + 1] = 2000 & 0xFF;
    }
    pb.set32(PB_LOOP_HI, kSampleAt + 0x100);
    pb.set32(PB_CUR_HI, kSampleAt + 30);
    store_pb(pb);
    run_list();
    check(out_l(1) == 1000 && out_l(2) == 2000 && out_l(159) == 2000 && pb_word(PB_RUNNING) == 1,
          "a voice that loops above its end plays its last sample, then the loop on");
    check(mem_r16(kPB + 2 * PB_CUR_LO) == (uint16_t)(kSampleAt + 0x100 + 158),
          "and is 158 samples past the loop after the list, not stuck on the loop's first sample");

    // One ADPCM frame at nibble 0x1000 that loops to its own header: 14 samples a frame, so
    // 160 samples are 11 frames and 6 more, which leaves the voice at nibble 0x1000 + 2 + 6.
    reset(kHashLater);
    for (uint32_t i = 0; i < 8; i++) g_aram[0x800 + i] = i ? 0x11 : 0x0F;
    pb = pcm_voice(0, 1, 0x8000, 0x8000);
    pb.w[PB_FORMAT] = 0x00;
    pb.set32(PB_LOOP_HI, 0x1000);
    pb.set32(PB_END_HI, 0x100F);
    pb.set32(PB_CUR_HI, 0x1000);
    store_pb(pb);
    run_list();
    check(pb_word(PB_RUNNING) == 1 && mem_r16(kPB + 2 * PB_CUR_LO) == 0x1008,
          "an ADPCM voice whose end is a frame's last nibble loops there, past the next header");
}

// What `f` writes to stderr.
std::string stderr_of(void (*f)()) {
    fflush(stderr);
    FILE* tmp = tmpfile();
    if (!tmp) return "";
    const int saved = dup(2);
    dup2(fileno(tmp), 2);
    f();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    std::string text;
    rewind(tmp);
    char buf[256];
    for (size_t n; (n = fread(buf, 1, sizeof buf, tmp)) > 0;) text.append(buf, n);
    fclose(tmp);
    return text;
}

bool has(const std::string& text, const char* line) { return text.find(line) != std::string::npos; }

// GCN_AXVOICES prints a line when the game starts, changes or stops a voice and when the
// mixer loops or ends one, and every 200 lists each voice's loudest sample.
void voices_run() {
    reset(kHashLater);
    g_axvoices = true;
    g_stat_lists = 198;
    PB pb = pcm_voice(1000, 0x100, 0x4000, 0x8000);
    pb.set32(PB_CUR_HI, kSampleAt + 0xFE);
    store_pb(pb);
    run_list();                                       // list 199: runs, and loops in its first ms
    mem_w16(kPB + 2 * PB_END_LO, kSampleAt + 0x1FF);
    run_list();                                       // 200: a new end, then the level line
    mem_w16(kPB + 2 * PB_CUR_LO, kSampleAt + 0x20);
    run_list();                                       // 201: a new place
    mem_w16(kPB + 2 * PB_LOOPING, 0);
    mem_w16(kPB + 2 * PB_CUR_LO, kSampleAt + 0x1FE);
    run_list();                                       // 202: a one-shot two from its end
    mem_w16(kPB + 2 * PB_RUNNING, 1);
    run_list();                                       // 203: run again
    mem_w16(kPB + 2 * PB_RUNNING, 0);
    run_list();                                       // 204: stopped by the game
    g_axvoices = false;
}

void voice_log_checks() {
    const std::string log = stderr_of(voices_run);
    check(has(log, "[axv] list=199 pb=80003000 run run=1 looping=1 stream=0 fmt=0A src=2 loop=00000800 end=000008FF "
                   "cur=000008FE ratio=00010000\n"),
          "a voice the mixer hasn't seen running prints run with its PB");
    check(has(log, "[axv] list=199 pb=80003000 loop run=1 looping=1 stream=0 fmt=0A src=2 loop=00000800 "
                   "end=000008FF cur=0000081E ratio=00010000\n"),
          "a voice that loops in a millisecond prints loop with where it is after it");
    check(has(log, "[axv] list=200 pb=80003000 set ") && has(log, " end=000009FF cur=0000089E "),
          "an end the game moved prints set");
    check(has(log, "[axv] list=200 pb=80003000 level peak=500 cur=0000093E\n"),
          "the 200th list prints the voice's loudest sample, 500");
    check(has(log, "[axv] list=201 pb=80003000 seek ") && has(log, " cur=00000820 "), "a place the game moved prints seek");
    check(has(log, "[axv] list=202 pb=80003000 set run=1 looping=0") &&
              has(log, "[axv] list=202 pb=80003000 end run=0 looping=0"),
          "a one-shot that reaches its end prints set, then end with run=0");
    check(has(log, "[axv] list=203 pb=80003000 run run=1") && has(log, "[axv] list=204 pb=80003000 stop run=0"),
          "the game running it again prints run, and stopping it prints stop");
    int lines = 0;
    for (char c : log) lines += c == '\n';
    check(lines == 9, "and nothing else prints");
}

}  // namespace

int main() {
    g_mem = (uint8_t*)calloc(RAM_SIZE, 1);
    g_aram = (uint8_t*)calloc(ARAM_SIZE, 1);
    if (!g_mem || !g_aram) return 2;
    stats_checks();
    depop_checks();
    adpcm_checks();
    polyphase_checks();
    end_checks();
    voice_log_checks();
    printf("%s: %d failed\n", g_failed ? "FAIL" : "ok", g_failed);
    return g_failed ? 1 : 0;
}
