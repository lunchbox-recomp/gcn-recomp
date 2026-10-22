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
    mem_w16(kSetup + 6 * 3 + 4, 0xFFF0);      // AuxA L's delta
    run_list();
    check(g_sw.voice_ms == 5 && g_sw.src[0] == 5 && g_sw.coef[1] == 5 && g_sw.coef[0] == 0,
          "a polyphase voice counts 5 ms under src0 and coefficient set 1");
    check(g_sw.lpf == 5, "the later build counts the low-pass filter on in all 5 ms");
    check(g_sw.ramp == 5 && g_sw.env_ramp == 5, "a ramped bus and an envelope delta count in all 5 ms");
    check(g_sw.depop == 1, "a setup that slopes one bus counts as a depop list");

    // The same voice with its ramp bit clear and no deltas.
    reset(kHashLater);
    pb.w[PB_MIXER_CONTROL] = 0x0003;
    pb.w[PB_VOL_DELTA] = 0;
    store_pb(pb);
    run_list();
    check(g_sw.ramp == 0 && g_sw.env_ramp == 0 && g_sw.depop == 0,
          "a delta with its ramp bit clear, and a flat setup, count nothing");

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

    // The library fades a stopped voice out through the setup command: main L starting at
    // 500 and falling 3 a sample, with no voice running, is the whole of main L.
    reset(kHashLater);
    mem_w16(kSetup + 2, 500);
    mem_w16(kSetup + 4, (uint16_t)-3);
    store_pb(PB{});
    run_list();
    check(out_l(0) == 500 && out_l(1) == 497 && out_l(159) == 23, "setup's start and slope reach the output");
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

    // Samples alternating between +10000 and -10000 sit at the input's Nyquist frequency. The
    // full-band set passes them and the half-band set (cut at half of it) holds them under a
    // third.
    int peak[3] = {};
    for (uint16_t coef : {0, 2}) {
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
    char what[96];
    snprintf(what, sizeof what, "Nyquist at 10000 comes out at %d through the full-band set and %d through the half-band",
             peak[2], peak[0]);
    check(peak[2] >= 9999 && peak[0] < 3333, what);

    // Two lists of a stream of distinct samples at ratios under and over 1, against the
    // position worked out from scratch for each sample out: (i + 1) * ratio, of which the
    // whole part counts the samples in and the fraction's top 7 bits pick the phase. The
    // ratios are odd, so 32 samples out never end on a whole sample in and the fraction has
    // to carry from one millisecond to the next.
    for (uint32_t ratio : {0x0C3A5u, 0x1A0F7u}) {
        reset(kHashLater);
        constexpr int kIn = 1024;
        PB pb = pcm_voice(0, kIn, 0x8000, 0x8000);
        auto input = [](int64_t n) { return n < 0 ? 0 : (int)(n * 37 % 2001) - 1000; };
        for (int n = 0; n < kIn; n++) {
            g_aram[2 * (kSampleAt + n)] = (uint8_t)((uint16_t)input(n) >> 8);
            g_aram[2 * (kSampleAt + n) + 1] = (uint8_t)input(n);
        }
        pb.w[PB_SRC_TYPE] = 0;
        pb.w[PB_COEF_SELECT] = 0;
        pb.set32(PB_RATIO_HI, ratio);
        store_pb(pb);
        int bad = 0;
        for (int list = 0; list < 2; list++) {
            run_list();
            for (int j = 0; j < SPF; j++) {
                const int64_t pos = (int64_t)(list * SPF + j + 1) * ratio, in = pos >> 16;
                const int16_t* c = g_poly[0][(pos & 0xFFFF) >> 9];
                int64_t sum = 0;
                for (int k = 0; k < 4; k++) sum += (int64_t)input(in - 4 + k) * c[k];
                bad += out_l(j) != (int16_t)std::clamp<int64_t>(sum >> 15, -32768, 32767);
            }
        }
        const int64_t end = (int64_t)2 * SPF * ratio, in = end >> 16;
        bool state = pb_word(PB_FRAC) == (uint16_t)end;
        for (int k = 0; k < 4; k++) state &= (int16_t)pb_word(PB_LAST + k) == input(in - 4 + k);
        char what[96];
        snprintf(what, sizeof what, "ratio %05X: 320 samples out match the position worked out for each", ratio);
        check(bad == 0, what);
        snprintf(what, sizeof what, "ratio %05X: the fraction and the last four samples carry to the next list", ratio);
        check(state, what);
    }
}

}  // namespace

int main() {
    g_mem = (uint8_t*)calloc(RAM_SIZE, 1);
    g_aram = (uint8_t*)calloc(ARAM_SIZE, 1);
    if (!g_mem || !g_aram) return 2;
    stats_checks();
    depop_checks();
    polyphase_checks();
    printf("%s: %d failed\n", g_failed ? "FAIL" : "ok", g_failed);
    return g_failed ? 1 : 0;
}
