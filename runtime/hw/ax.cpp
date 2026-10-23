// High-level emulation of the AX audio ucode (the 2001 GameCube build, and the mixer control
// layout and compressor of later builds): processes the command lists the AX library sends,
// mixes voices from ARAM and writes the final 5ms stereo block that the AI DMA plays.
#include "../runtime.h"
#include "ax.h"
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr int SPMS = 32;          // samples per millisecond (32 kHz)
constexpr int SPF = 5 * SPMS;     // samples per 5ms frame

int32_t g_left[SPF], g_right[SPF], g_surround[SPF];
int32_t g_auxa_l[SPF], g_auxa_r[SPF], g_auxa_s[SPF];
int32_t g_auxb_l[SPF], g_auxb_r[SPF], g_auxb_s[SPF];

// The nine buses in the order the setup command and the mixer control words use.
int32_t* const kBuses[9] = {g_left, g_right, g_surround, g_auxa_l, g_auxa_r, g_auxa_s, g_auxb_l, g_auxb_r, g_auxb_s};

// ---- parameter block (word indices into the big-endian u16 array) ----
enum {
    PB_NEXT_HI = 0, PB_NEXT_LO = 1, PB_SRC_TYPE = 4, PB_COEF_SELECT = 5, PB_MIXER_CONTROL = 6,
    PB_RUNNING = 7, PB_IS_STREAM = 8,
    PB_MIXER = 9,           // 18 words: (vol, delta) pairs for L, R, AuxA L/R, AuxB L/R, AuxB S, S, AuxA S
    PB_ITD_ON = 27,         // initial time delay, 7 words from here; not applied
    PB_UPDATES = 34,        // num_updates[5], data_hi, data_lo
    PB_DPOP = 41,           // 9 words: main L, AuxA L, AuxB L, main R, AuxA R, AuxB R, main S, AuxA S, AuxB S
    PB_VOL = 50, PB_VOL_DELTA = 51,
    PB_LOOPING = 55, PB_FORMAT = 56, PB_LOOP_HI = 57, PB_LOOP_LO = 58, PB_END_HI = 59, PB_END_LO = 60,
    PB_CUR_HI = 61, PB_CUR_LO = 62,
    PB_COEFS = 63, PB_GAIN = 79, PB_PRED_SCALE = 80, PB_YN1 = 81, PB_YN2 = 82,
    PB_RATIO_HI = 83, PB_RATIO_LO = 84, PB_FRAC = 85, PB_LAST = 86,
    PB_LOOP_PRED_SCALE = 90, PB_LOOP_YN1 = 91, PB_LOOP_YN2 = 92,
    // Every build but the 2001 one goes on with a one-pole low-pass filter: on, yn1, a0, b0.
    PB_LPF_ON = 93, PB_LPF_YN1 = 94, PB_LPF_A0 = 95, PB_LPF_B0 = 96,
    PB_WORDS_2001 = 93, PB_WORDS_LATER = 97,
};

struct PB {
    uint16_t w[PB_WORDS_LATER] = {};  // the 2001 build's words, then zeros
    uint32_t get32(int i) const { return ((uint32_t)w[i] << 16) | w[i + 1]; }
    void set32(int i, uint32_t v) { w[i] = (uint16_t)(v >> 16); w[i + 1] = (uint16_t)v; }
};

// Whether this ucode build's PB has the low-pass filter words; set from the hash by ax_init.
bool g_has_lpf;
int pb_words() { return g_has_lpf ? PB_WORDS_LATER : PB_WORDS_2001; }

void read_pb(uint32_t addr, PB& pb) {
    for (int i = 0; i < pb_words(); i++) pb.w[i] = mem_r16(addr + 2 * i);
}
void write_pb(uint32_t addr, const PB& pb) {
    for (int i = 0; i < pb_words(); i++) mem_w16(addr + 2 * i, pb.w[i]);
}

// ---- GCN_AXSTATS: what the command lists ask for and what the mix makes ----
// Read once, by ax_init, and printed every 400 lists (docs/diagnostics.md).
bool g_axstats;

constexpr int NUM_CMDS = 0x14;  // 00 to 13; anything past that is unknown

// Counts since the last print.
struct StatsWindow {
    uint32_t cmds[NUM_CMDS], skipped, unknown;
    uint32_t peak[9], clipped;           // the buses in kBuses' order
    uint32_t depop;                      // setup commands that start a bus off zero
    std::map<uint16_t, uint32_t> mixer;  // mixer control -> voice-milliseconds
    uint32_t voice_ms, src[4], coef[4], delay, stream_loops;
    uint32_t lpf, ramp, env_ramp;
};
StatsWindow g_sw;
uint32_t g_stat_lists;
uint64_t g_stat_hash = 0xCBF29CE484222325ull;  // FNV-1a over every byte 0E has written

void stats_peak(const int32_t* buf) {
    for (int b = 0; b < 9; b++) {
        if (kBuses[b] != buf) continue;
        for (int i = 0; i < SPF; i++) {
            uint32_t m = buf[i] < 0 ? 0u - (uint32_t)buf[i] : (uint32_t)buf[i];
            g_sw.peak[b] = std::max(g_sw.peak[b], m);
        }
    }
}

// After 0E: the main buses' peaks, the L and R samples its clamp changed, and the bytes it
// wrote (surround, then interleaved R and L) into the hash.
void stats_output(uint32_t surround_addr, uint32_t lr_addr) {
    for (int32_t* b : {g_left, g_right, g_surround}) stats_peak(b);
    for (int i = 0; i < SPF; i++)
        g_sw.clipped += (g_left[i] < -32767 || g_left[i] > 32767) + (g_right[i] < -32767 || g_right[i] > 32767);
    for (uint32_t a : {surround_addr, lr_addr})
        for (uint32_t i = 0; i < 4 * SPF; i++) { g_stat_hash ^= mem_r8(a + i); g_stat_hash *= 0x100000001B3ull; }
}

void stats_list_end() {
    const uint32_t n = ++g_stat_lists;
    if (n % 400) return;
    std::string ids;
    for (int c = 0; c < NUM_CMDS; c++) {
        if (!g_sw.cmds[c]) continue;
        char b[24]; snprintf(b, sizeof b, " %02X=%u", c, g_sw.cmds[c]); ids += b;
    }
    fprintf(stderr, "[ax] cmds list=%u skipped=%u unknown=%u%s\n", n, g_sw.skipped, g_sw.unknown, ids.c_str());
    const uint32_t* pk = g_sw.peak;
    fprintf(stderr, "[ax] bus list=%u main_l=%u main_r=%u main_s=%u auxa_l=%u auxa_r=%u auxa_s=%u auxb_l=%u auxb_r=%u auxb_s=%u clipped=%u depop=%u\n",
            n, pk[0], pk[1], pk[2], pk[3], pk[4], pk[5], pk[6], pk[7], pk[8], g_sw.clipped, g_sw.depop);
    // The eight mixer control values with the most voice-milliseconds, ties in value order.
    std::vector<std::pair<uint16_t, uint32_t>> mix(g_sw.mixer.begin(), g_sw.mixer.end());
    std::stable_sort(mix.begin(), mix.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string top;
    uint32_t rest = 0;
    for (size_t i = 0; i < mix.size(); i++) {
        if (i >= 8) { rest += mix[i].second; continue; }
        char b[24]; snprintf(b, sizeof b, "%s%04X:%u", i ? "," : "", mix[i].first, mix[i].second); top += b;
    }
    const uint32_t* cf = g_sw.coef;
    fprintf(stderr, "[ax] pb list=%u voice_ms=%u mixer=%s mixer_rest=%u src0=%u src1=%u src2=%u src_other=%u src0_coef=%u/%u/%u/%u "
            "delay=%u stream_loops=%u lpf=%u ramp=%u env_ramp=%u\n",
            n, g_sw.voice_ms, top.empty() ? "-" : top.c_str(), rest, g_sw.src[0], g_sw.src[1], g_sw.src[2], g_sw.src[3],
            cf[0], cf[1], cf[2], cf[3], g_sw.delay, g_sw.stream_loops, g_sw.lpf, g_sw.ramp, g_sw.env_ramp);
    fprintf(stderr, "[ax] hash list=%u fnv=%016llX\n", n, (unsigned long long)g_stat_hash);
    g_sw = StatsWindow{};
}

inline uint8_t aram_byte(uint32_t a) { return g_aram[a & (ARAM_SIZE - 1)]; }

// The DSP "accelerator": fetches the next sample for a voice, handling ADPCM
// decoding, the end of the sample data and looping.
struct Accel {
    PB& pb;
    uint32_t cur, end, loop;
    uint16_t fmt;
    bool stopped = false;

    explicit Accel(PB& p) : pb(p), cur(p.get32(PB_CUR_HI)), end(p.get32(PB_END_HI)), loop(p.get32(PB_LOOP_HI)), fmt(p.w[PB_FORMAT]) {}

    int16_t sample() {
        if (stopped) return 0;
        int32_t val = 0;
        switch (fmt) {
        case 0x00: {  // 4-bit DSP ADPCM, addresses in nibbles
            if ((cur & 15) == 0) {
                pb.w[PB_PRED_SCALE] = aram_byte(cur >> 1);
                cur += 2;
            }
            uint16_t ps = pb.w[PB_PRED_SCALE];
            int scale = 1 << (ps & 0xF);
            int ci = (ps >> 4) & 7;
            int32_t c1 = (int16_t)pb.w[PB_COEFS + ci * 2], c2 = (int16_t)pb.w[PB_COEFS + ci * 2 + 1];
            uint8_t b = aram_byte(cur >> 1);
            int t = (cur & 1) ? (b & 0xF) : (b >> 4);
            if (t >= 8) t -= 16;
            int32_t yn1 = (int16_t)pb.w[PB_YN1], yn2 = (int16_t)pb.w[PB_YN2];
            val = scale * t + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
            val = std::clamp(val, -0x7FFF, 0x7FFF);
            pb.w[PB_YN2] = pb.w[PB_YN1];
            pb.w[PB_YN1] = (uint16_t)val;
            cur++;
            break;
        }
        case 0x0A:  // PCM16, addresses in samples
            val = (int16_t)((aram_byte(cur * 2) << 8) | aram_byte(cur * 2 + 1));
            pb.w[PB_YN2] = pb.w[PB_YN1];
            pb.w[PB_YN1] = (uint16_t)val;
            cur++;
            break;
        case 0x19:  // PCM8
            val = (int16_t)(aram_byte(cur) << 8);
            pb.w[PB_YN2] = pb.w[PB_YN1];
            pb.w[PB_YN1] = (uint16_t)val;
            cur++;
            break;
        default:
            stopped = true;
            return 0;
        }
        if (cur > end) {
            if (pb.w[PB_LOOPING]) {
                cur = loop;
                pb.w[PB_PRED_SCALE] = pb.w[PB_LOOP_PRED_SCALE];
                if (!pb.w[PB_IS_STREAM]) {
                    pb.w[PB_YN1] = pb.w[PB_LOOP_YN1];
                    pb.w[PB_YN2] = pb.w[PB_LOOP_YN2];
                }
                if (pb.w[PB_IS_STREAM] && g_axstats) g_sw.stream_loops++;
            } else {
                pb.w[PB_RUNNING] = 0;
                stopped = true;
            }
        }
        return (int16_t)val;
    }

    void finish() { pb.set32(PB_CUR_HI, cur); }
};

// Adds a voice's samples to a bus at its volume, and leaves the last sample it added in the
// bus's depop word, for the AX library to fade out from if it stops the voice.
void mix_add(int32_t* out, const int16_t* in, int n, PB& pb, int vol_idx, bool ramp, int dpop_idx) {
    uint16_t vol = pb.w[vol_idx];
    uint16_t delta = ramp ? pb.w[vol_idx + 1] : 0;
    int32_t s = 0;
    for (int i = 0; i < n; i++) {
        s = std::clamp((int32_t)(((int64_t)in[i] * vol) >> 15), -32767, 32767);
        out[i] += s;
        vol += delta;
    }
    pb.w[vol_idx] = vol;
    pb.w[dpop_idx] = (uint16_t)s;
}

// The mixer control (PB word 6) decoded per bus, in kBuses' order: main L, R, S, AuxA L, R, S,
// AuxB L, R, S. A bus mixes when its enable is set, and its volume steps by its delta per
// sample when its ramp is set.
struct MixFlags {
    bool enable[9] = {}, ramp[9] = {};
};

// Offset of each bus's (volume, delta) pair from PB_MIXER, and of its depop word from PB_DPOP.
constexpr int kMixerWord[9] = {0, 2, 14, 4, 6, 16, 8, 10, 12};
constexpr int kDpopWord[9] = {0, 3, 6, 1, 4, 7, 2, 5, 8};

// Old (2001) ucode: main L/R always; bit0 AuxA, bit1 AuxB, bit2 surround, bit3 volume ramps
// for every bus. Bit4 selects Dolby Pro Logic II mixing, where the low bits mean something else.
MixFlags decode_mixer_2001(uint16_t mc) {
    MixFlags f;
    bool auxa, auxb, surround;
    if (mc & 0x10) {
        auxb = (mc & 6) == 0;
        auxa = (mc & 7) == 1;
        surround = false;
    } else {
        auxa = mc & 1;
        auxb = mc & 2;
        surround = mc & 4;
    }
    const bool on[9] = {true, true, surround, auxa, auxa, auxa && ((mc & 0x10) || surround),
                        auxb, auxb, auxb && surround};
    for (int b = 0; b < 9; b++) {
        f.enable[b] = on[b];
        f.ramp[b] = mc & 8;
    }
    return f;
}

// Later ucodes: every bus has its own enable bit and the ramps come in groups.
//   enables: 0x0001 main L, 0x0002 main R, 0x0004 main S, 0x0010 AuxA L, 0x0020 AuxA R,
//            0x0080 AuxA S, 0x0200 AuxB L, 0x0400 AuxB R, 0x1000 AuxB S
//   ramps:   0x0008 main L, R and S, 0x0040 AuxA L and R, 0x0100 AuxA S,
//            0x0800 AuxB L and R, 0x2000 AuxB S
constexpr uint16_t kEnable[9] = {0x0001, 0x0002, 0x0004, 0x0010, 0x0020, 0x0080, 0x0200, 0x0400, 0x1000};
constexpr uint16_t kRamp[9] = {0x0008, 0x0008, 0x0008, 0x0040, 0x0040, 0x0100, 0x0800, 0x0800, 0x2000};
MixFlags decode_mixer_later(uint16_t mc) {
    MixFlags f;
    for (int b = 0; b < 9; b++) {
        f.enable[b] = mc & kEnable[b];
        f.ramp[b] = mc & kRamp[b];
    }
    return f;
}

// Which decoder the mixer control word goes through; chosen once, by ax_init.
bool g_layout_later;
// Whether this ucode build has command 12; set from the hash alone, so GCN_AX_LAYOUT leaves it be.
bool g_has_compressor;

// One running voice for one millisecond, as it stands before it is mixed.
void stats_voice(const PB& pb, const MixFlags& mf) {
    g_sw.voice_ms++;
    g_sw.mixer[pb.w[PB_MIXER_CONTROL]]++;
    g_sw.src[std::min<uint16_t>(pb.w[PB_SRC_TYPE], 3)]++;
    if (pb.w[PB_SRC_TYPE] == 0) g_sw.coef[std::min<uint16_t>(pb.w[PB_COEF_SELECT], 3)]++;
    if (pb.w[PB_ITD_ON]) g_sw.delay++;
    if (g_has_lpf && pb.w[PB_LPF_ON]) g_sw.lpf++;
    if (pb.w[PB_VOL_DELTA]) g_sw.env_ramp++;
    bool ramp = false;
    for (int b = 0; b < 9; b++) ramp |= mf.enable[b] && mf.ramp[b] && pb.w[PB_MIXER + kMixerWord[b] + 1];
    g_sw.ramp += ramp;
}

extern int g_stat_voices;
void process_voice(PB& pb, int offset) {
    if (!pb.w[PB_RUNNING]) return;
    if (offset == 0) g_stat_voices++;
    const uint16_t mc = pb.w[PB_MIXER_CONTROL];
    const MixFlags mf = g_layout_later ? decode_mixer_later(mc) : decode_mixer_2001(mc);
    if (g_axstats) stats_voice(pb, mf);
    int16_t samples[SPMS];
    Accel acc(pb);
    int16_t* last = (int16_t*)&pb.w[PB_LAST];
    if (pb.w[PB_SRC_TYPE] >= 2) {
        // Types 0 and 1 resample; every other type has no rate conversion: a sample in for every
        // sample out, whatever the ratio says. The history keeps the last four out, as the
        // interpolating types would have it.
        for (int i = 0; i < SPMS; i++) samples[i] = acc.sample();
        for (int k = 0; k < 4; k++) last[k] = samples[SPMS - 4 + k];
    } else {
        // Types 0 and 1: linear interpolation, for the polyphase and linear modes alike.
        uint32_t ratio = pb.get32(PB_RATIO_HI);
        uint32_t pos = pb.w[PB_FRAC];
        for (int i = 0; i < SPMS; i++) {
            int32_t frac = pos & 0xFFFF;
            samples[i] = (int16_t)(last[2] + (((last[3] - last[2]) * frac) >> 16));
            pos += ratio;
            while (pos >= 0x10000) {
                last[0] = last[1]; last[1] = last[2]; last[2] = last[3];
                last[3] = acc.sample();
                pos -= 0x10000;
            }
        }
        pb.w[PB_FRAC] = (uint16_t)pos;
    }
    acc.finish();
    // Volume envelope
    for (int i = 0; i < SPMS; i++) {
        uint16_t v = pb.w[PB_VOL];
        samples[i] = (int16_t)std::clamp(((int32_t)samples[i] * v) >> 15, -32767, 32767);
        pb.w[PB_VOL] = (uint16_t)(v + (int16_t)pb.w[PB_VOL_DELTA]);
    }
    for (int b = 0; b < 9; b++)
        if (mf.enable[b])
            mix_add(kBuses[b] + offset, samples, SPMS, pb, PB_MIXER + kMixerWord[b], mf.ramp[b], PB_DPOP + kDpopWord[b]);
}

void process_pb_list(uint32_t pb_addr) {
    PB pb;
    int guard = 0;
    while (pb_addr && guard++ < 256) {
        read_pb(pb_addr, pb);
        // Per-millisecond parameter updates: (word index, value) pairs.
        uint32_t upd = pb.get32(PB_UPDATES + 5);
        for (int ms = 0; ms < 5; ms++) {
            uint16_t n = pb.w[PB_UPDATES + ms];
            for (int k = 0; k < n; k++) {
                uint16_t idx = mem_r16(upd), val = mem_r16(upd + 2);
                upd += 4;
                if (idx < pb_words()) pb.w[idx] = val;
            }
            process_voice(pb, ms * SPMS);
        }
        write_pb(pb_addr, pb);
        pb_addr = pb.get32(PB_NEXT_HI);
    }
}

void write_bufs(uint32_t addr, std::initializer_list<int32_t*> bufs) {
    for (int32_t* b : bufs) {
        if (g_axstats) stats_peak(b);
        for (int i = 0; i < SPF; i++, addr += 4) mem_w32(addr, (uint32_t)b[i]);
    }
}
void add_bufs(uint32_t addr, std::initializer_list<int32_t*> bufs) {
    for (int32_t* b : bufs)
        for (int i = 0; i < SPF; i++, addr += 4) b[i] += (int32_t)mem_r32(addr);
}
void set_bufs(uint32_t addr, std::initializer_list<int32_t*> bufs) {
    for (int32_t* b : bufs)
        for (int i = 0; i < SPF; i++, addr += 4) b[i] = (int32_t)mem_r32(addr);
}

// Replace the main buffers with one downloaded mono buffer: L = R = in (0x07), or
// L = -in, R = in (0x11); surround cleared either way.
void set_main_from(uint32_t addr, bool opposite) {
    for (int i = 0; i < SPF; i++, addr += 4) {
        int32_t v = (int32_t)mem_r32(addr);
        g_left[i] = opposite ? -v : v;
        g_right[i] = v;
        g_surround[i] = 0;
    }
}

// 0x01: add nine downloaded buffers (main L/R/S, AuxA L/R/S, AuxB L/R/S, all read from
// the same address in turn) scaled by a volume per group.
void download_and_mix(uint32_t addr, uint16_t vol_main, uint16_t vol_auxa, uint16_t vol_auxb) {
    int32_t* groups[3][3] = {{g_left, g_right, g_surround}, {g_auxa_l, g_auxa_r, g_auxa_s}, {g_auxb_l, g_auxb_r, g_auxb_s}};
    uint16_t vols[3] = {vol_main, vol_auxa, vol_auxb};
    for (int g = 0; g < 3; g++) {
        uint32_t p = addr;
        for (int b = 0; b < 3; b++)
            for (int i = 0; i < SPF; i++, p += 4) groups[g][b][i] += (int32_t)(((int64_t)(int32_t)mem_r32(p) * vols[g]) >> 15);
    }
}

// 0x00: start each bus at a value and step it by a delta per sample, in kBuses' order. The
// AX library fills these from its depop sums, the last samples of voices it stopped.
void setup(uint32_t addr) {
    bool off_zero = false;
    for (int b = 0; b < 9; b++) {
        int32_t init = (int32_t)(((uint32_t)mem_r16(addr) << 16) | mem_r16(addr + 2));
        int16_t delta = (int16_t)mem_r16(addr + 4);
        addr += 6;
        off_zero |= init || delta;
        for (int i = 0; i < SPF; i++) { kBuses[b][i] = init; init += delta; }
    }
    if (g_axstats) g_sw.depop += off_zero;
}

// 0x12 on every build but the 2001 one: duck the main L and R when they are over the
// threshold, then ease back over `release` lists. The table holds 160-entry ramps, 11 attack
// ramps and then the release ramps, as unsigned 16-bit gains with 0x8000 as 1.0.
uint16_t g_comp_counter;  // lists of release left; survives from list to list

void compress(uint16_t threshold, uint16_t release, uint32_t table) {
    bool over = false;
    for (int i = 0; i < SPF && !over; i++)
        over = std::abs((int64_t)g_left[i]) > threshold || std::abs((int64_t)g_right[i]) > threshold;
    uint32_t ramp;
    if (over) {
        ramp = g_comp_counter;
        g_comp_counter = release;
    } else if (g_comp_counter) {
        ramp = 11 + --g_comp_counter;
    } else {
        return;
    }
    const uint32_t base = table + 2 * SPF * ramp;
    for (int i = 0; i < SPF; i++) {
        const int64_t c = mem_r16(base + 2 * i);
        g_left[i] = (int32_t)((g_left[i] * c) >> 15);
        g_right[i] = (int32_t)((g_right[i] * c) >> 15);
    }
}

int g_stat_voices, g_stat_frames, g_stat_peak;

void output(uint32_t surround_addr, uint32_t lr_addr) {
    for (int i = 0; i < SPF; i++) g_stat_peak = std::max(g_stat_peak, std::abs(g_left[i]));
    if (++g_stat_frames % 400 == 0 && g_axstats) {
        fprintf(stderr, "[ax] frames=%d voices(running, last frame)=%d peak=%d out=%08X\n", g_stat_frames, g_stat_voices, g_stat_peak, lr_addr);
        g_stat_peak = 0;
    }
    for (int i = 0; i < SPF; i++) mem_w32(surround_addr + 4 * i, (uint32_t)g_surround[i]);
    for (int i = 0; i < SPF; i++) {
        int32_t l = std::clamp(g_left[i], -32767, 32767), r = std::clamp(g_right[i], -32767, 32767);
        mem_w16(lr_addr + 4 * i, (uint16_t)r);       // interleaved R, L
        mem_w16(lr_addr + 4 * i + 2, (uint16_t)l);
    }
    if (g_axstats) stats_output(surround_addr, lr_addr);
}

}  // namespace

// The 2001 ucode is the one build with the old mixer control layout; every other AX build
// uses the later one, so a hash nobody has seen gets the later layout.
constexpr uint32_t kUcode2001Hash = 0x4E8A8B21;

void ax_init(uint32_t ucode_hash) {
    g_axstats = getenv("GCN_AXSTATS") != nullptr;
    g_layout_later = ucode_hash != kUcode2001Hash;
    g_has_compressor = ucode_hash != kUcode2001Hash;
    g_has_lpf = ucode_hash != kUcode2001Hash;
    g_comp_counter = 0;
    const char* over = getenv("GCN_AX_LAYOUT");
    const bool forced = over && (!strcmp(over, "2001") || !strcmp(over, "later"));
    if (forced) g_layout_later = !strcmp(over, "later");
    else if (over) fprintf(stderr, "GCN_AX_LAYOUT=%s is not 2001 or later, ignored\n", over);
    LOG(LOG_DSP, "AX: %s mixer layout, ucode hash=%08X%s", g_layout_later ? "later" : "2001", ucode_hash,
        forced ? " (set by GCN_AX_LAYOUT)" : "");
}

static void run_cmdlist(uint32_t addr, uint16_t size) {
    uint32_t pb_addr = 0;
    uint32_t p = addr, end = addr + size + 64;
    auto rd = [&]() { uint16_t v = mem_r16(p); p += 2; return v; };
    auto rd32 = [&]() { uint32_t hi = rd(); return (hi << 16) | rd(); };
    for (int guard = 0; guard < 256 && p < end; guard++) {
        uint16_t cmd = rd();
        if (g_axstats) (cmd < NUM_CMDS ? g_sw.cmds[cmd] : g_sw.unknown)++;
        switch (cmd) {
        case 0x00: setup(rd32()); break;
        case 0x01: { uint32_t a = rd32(); uint16_t vm = rd(), va = rd(), vb = rd(); download_and_mix(a, vm, va, vb); break; }
        case 0x02: pb_addr = rd32(); break;
        case 0x03: g_stat_voices = 0; process_pb_list(pb_addr); break;
        case 0x04: { uint32_t w = rd32(), r = rd32(); write_bufs(w, {g_auxa_l, g_auxa_r, g_auxa_s});
                     add_bufs(r, {g_left, g_right, g_surround}); break; }
        case 0x05: { uint32_t w = rd32(), r = rd32(); write_bufs(w, {g_auxb_l, g_auxb_r, g_auxb_s});
                     add_bufs(r, {g_left, g_right, g_surround}); break; }
        case 0x06: write_bufs(rd32(), {g_left, g_right, g_surround}); break;
        case 0x07: set_main_from(rd32(), false); break;
        case 0x08: p += 20; if (g_axstats) g_sw.skipped++; break;
        case 0x09: add_bufs(rd32(), {g_left, g_right, g_surround}); break;  // mix AuxB back in, nothing uploaded
        case 0x0A: case 0x0B: case 0x0C: if (g_axstats) g_sw.skipped++; break;
        case 0x0D: {  // continue in another command buffer: address, size
            uint32_t a = rd32();
            uint16_t sz = rd();
            p = a;
            end = a + sz + 64;
            break;
        }
        case 0x0E: { uint32_t s = rd32(), lr = rd32(); output(s, lr); break; }
        case 0x0F: return;
        case 0x10: {  // upload AuxB L/R, then download the CPU's version of them: add to main, keep as AuxB
            uint32_t up = rd32(), dl = rd32();
            write_bufs(up, {g_auxb_l, g_auxb_r});
            set_bufs(dl, {g_auxb_l, g_auxb_r});
            for (int i = 0; i < SPF; i++) { g_left[i] += g_auxb_l[i]; g_right[i] += g_auxb_r[i]; }
            break;
        }
        case 0x11: set_main_from(rd32(), true); break;
        case 0x12: {  // compressor: threshold, release lists, table address. The 2001 build has no such command
            uint16_t threshold = rd(), release = rd();
            uint32_t table = rd32();
            if (g_has_compressor) compress(threshold, release, table);
            else if (g_axstats) g_sw.skipped++;
            break;
        }
        case 0x13: {  // upload AuxA L/R/S and AuxB S; download and add main L, main R, AuxB L, AuxB R
            uint32_t auxa_up = rd32(), auxb_s_up = rd32(), main_l = rd32(), main_r = rd32(), auxb_l = rd32(), auxb_r = rd32();
            write_bufs(auxa_up, {g_auxa_l, g_auxa_r, g_auxa_s});
            write_bufs(auxb_s_up, {g_auxb_s});
            add_bufs(main_l, {g_left});
            add_bufs(main_r, {g_right});
            add_bufs(auxb_l, {g_auxb_l});
            add_bufs(auxb_r, {g_auxb_r});
            break;
        }
        default: {
            // Say so once, with the list itself, so a command this parser does not know
            // (or whose length it has wrong) can be identified.
            static int reported;
            if (reported++ < 2) {
                std::string words;
                for (uint32_t q = addr; q < addr + 2 * size && q < addr + 128; q += 2) {
                    char b[8]; snprintf(b, sizeof b, "%04X ", mem_r16(q)); words += b;
                }
                LOG(LOG_DSP, "AX: unknown command %04X at +%X in list %08X (%u words): %s", cmd, (unsigned)(p - 2 - addr), addr, size, words.c_str());
            }
            return;
        }
        }
    }
}

void ax_process_cmdlist(uint32_t addr, uint16_t size) {
    run_cmdlist(addr, size);
    if (g_axstats) stats_list_end();
}
