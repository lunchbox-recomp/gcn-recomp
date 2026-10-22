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
// with the volume envelope at `env` and main L at `vol_l`. Nothing else is set.
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

}  // namespace

int main() {
    g_mem = (uint8_t*)calloc(RAM_SIZE, 1);
    g_aram = (uint8_t*)calloc(ARAM_SIZE, 1);
    if (!g_mem || !g_aram) return 2;
    stats_checks();
    printf("%s: %d failed\n", g_failed ? "FAIL" : "ok", g_failed);
    return g_failed ? 1 : 0;
}
