// The menu's sounds checks: the `test_ui_sound` target. Runs with no device, disc or game.
//
// runtime/audio.cpp is compiled into this file, as tools/test_audio_ring.cpp compiles it, with
// the AI's volume and stream (no DVD music plays) and the disc read stubbed; runtime/ui_sound.cpp
// is linked as it is.
//
// Each voice of the default theme is made and must peak at -14 dBFS, no louder than -12, start
// and end on a zero sample, rise over its first 2 ms and fall over its last 2 ms no faster than
// a raised cosine over 2 ms does, and have no click: no step from one sample to the next over 3
// times the largest step within 2 ms either side of it, and over 1% of its peak. A page's swish is
// louder on the side the page turns to, and every other voice is the same in both channels.
// A theme's level, notes and length change the voice, a level over -12 dBFS is held to it, and an
// attack or release under 2 ms is held to that.
//
// The queue: off, the default, a play queues nothing and a pull adds nothing. On, a voice starts
// in the pull after it's queued, audible in that pull, and plays exactly as it was made across
// pulls, summed with what's there and held to 16 bits. No more than 16 wait and 8 play, and a
// voice playing as the sounds go off plays to its end. The theme is fixed once the sounds are
// first on. Through audio_render, a GCN_WAV recording holds none of a voice that the device's
// samples and the movie tap both hold.
//
// Last it prints what a pull of 512 frames costs the mix with nothing playing, one voice and
// eight. Not checks: they measure.
//
// usage: test_ui_sound <work dir> [<wav dir>]
// The work dir must not exist; it's made, and GCN_WAV's file is left in it. With a wav dir, each
// voice of the default theme is written there too, as <voice>.wav, 48 kHz 16-bit stereo, to
// listen to. Exits 0 only if every check passes.
#include "audio.cpp"
#include "audio_stats.cpp"
#include "pause.cpp"
#include "ui_sound.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <string>
#include <sys/stat.h>
#include <vector>

uint32_t ai_volume_left() { return 255; }
uint32_t ai_volume_right() { return 255; }
bool ai_stream_playing() { return false; }
uint32_t ai_stream_rate() { return 48000; }
void ai_stream_advance(uint32_t) {}
uint64_t host_ns() { return 0; }
void clock_hold(uint64_t) {}
void exi_card_held(uint64_t) {}
bool iso_read(uint64_t, void*, uint32_t) { return false; }

namespace {

constexpr int kPull = 512;
constexpr int kEdge = 96;  // 2 ms of frames
const char* const kNames[kUiSounds] = {"move", "confirm", "back", "page_back", "page_on", "open", "close", "refused"};

int g_checks, g_failed;

void check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) g_failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    fflush(stdout);
}

int peak_of(const std::vector<int16_t>& s, size_t from = 0, size_t to = SIZE_MAX) {
    int most = 0;
    for (size_t i = from; i < std::min(to, s.size()); i++) most = std::max(most, std::abs((int)s[i]));
    return most;
}

// The largest sample a peak of `dbfs` allows, rounded down.
int level(float dbfs) { return (int)std::floor(32768.0 * std::pow(10.0, dbfs / 20.0)); }

// A raised cosine rising over 2 ms, at frame `n` of it.
double edge_ramp(int n) { return n >= kEdge ? 1.0 : (1.0 - std::cos(std::numbers::pi * n / kEdge)) / 2.0; }

// Whether channel `c` of stereo `s` has a click: a step from one sample to the next over 3 times
// the largest step within 2 ms either side of it, the steps beside it left out, and over 1% of
// `peak`, under which no step is heard as one.
bool clicks(const std::vector<int16_t>& s, int c, int peak) {
    const int frames = (int)s.size() / 2;
    std::vector<int> step(frames, 0);
    for (int n = 1; n < frames; n++) step[n] = std::abs(s[2 * n + c] - s[2 * (n - 1) + c]);
    for (int n = 1; n < frames; n++) {
        int around = 0;
        for (int m = std::max(1, n - kEdge); m <= std::min(frames - 1, n + kEdge); m++)
            if (std::abs(m - n) > 1) around = std::max(around, step[m]);
        if (step[n] > 3 * around && step[n] > peak / 100) {
            printf("     a step of %d at frame %d, where the steps around it are %d at most\n", step[n], n, around);
            return true;
        }
    }
    return false;
}

// One voice's shape: its length, its peak at `dbfs`, its edges and no click.
void shape(const UiSoundTheme& theme, UiSound id, float dbfs, const std::string& what) {
    const std::vector<int16_t> s = ui_sound_render(theme, id);
    const UiVoice v = ui_sound_fit(theme).voices[(int)id];
    const int frames = (int)s.size() / 2, peak = peak_of(s);
    check(frames == (int)std::lround(v.ms * kUiSoundRate / 1000.0f), what + ": lasts its length");
    check(peak == level(dbfs) && peak <= level(kUiSoundMaxDbfs),
          what + ": peaks at " + std::to_string((int)dbfs) + " dBFS (" + std::to_string(peak) + "), no louder than -12");
    check(frames > 1 && s[0] == 0 && s[1] == 0 && s[s.size() - 2] == 0 && s[s.size() - 1] == 0,
          what + ": starts and ends on a zero sample in both channels");
    bool under = true;
    for (int n = 0; n < frames; n++) {
        const double e = ui_sound_envelope(v, n, frames);
        under = under && e <= edge_ramp(n) + 1e-6 && e <= edge_ramp(frames - 1 - n) + 1e-6 && e >= 0.0 && e <= 1.0;
    }
    check(under, what + ": its envelope rises over its first 2 ms and falls over its last no faster than a 2 ms raised cosine");
    check(peak_of(s, 0, kEdge) <= peak / 2 && peak_of(s, s.size() - kEdge) <= peak / 2,
          what + ": stays under half its peak over its first and last millisecond");
    check(!clicks(s, 0, peak) && !clicks(s, 1, peak), what + ": has no click");
}

void voices() {
    const UiSoundTheme t = ui_sound_theme_default();
    for (int i = 0; i < kUiSounds; i++) shape(t, (UiSound)i, -14.0f, std::string("the ") + kNames[i] + " voice");
    const auto sides = [&](UiSound id) {
        const std::vector<int16_t> s = ui_sound_render(t, id);
        int left = 0, right = 0;
        for (size_t i = 0; i < s.size(); i += 2) {
            left = std::max(left, std::abs((int)s[i]));
            right = std::max(right, std::abs((int)s[i + 1]));
        }
        return std::pair{left, right};
    };
    const auto back = sides(UiSound::page_back), on = sides(UiSound::page_on);
    check(back.first > 2 * back.second && on.second > 2 * on.first,
          "a page back swishes on the left and a page on on the right");
    bool middle = true;
    for (UiSound id : {UiSound::move, UiSound::confirm, UiSound::back, UiSound::open, UiSound::close, UiSound::refused}) {
        const std::vector<int16_t> s = ui_sound_render(t, id);
        for (size_t i = 0; i < s.size(); i += 2) middle = middle && s[i] == s[i + 1];
    }
    check(middle, "every other voice is the same in both channels");
}

void themes() {
    UiSoundTheme quiet = ui_sound_theme_default();
    quiet.peak_dbfs = -20.0f;
    shape(quiet, UiSound::confirm, -20.0f, "a theme's confirm at -20 dBFS");
    UiSoundTheme loud = ui_sound_theme_default();
    loud.peak_dbfs = -3.0f;
    shape(loud, UiSound::open, kUiSoundMaxDbfs, "a theme's open at -3 dBFS");
    UiSoundTheme sharp = ui_sound_theme_default();
    for (UiVoice& v : sharp.voices) v.attack_ms = v.release_ms = 0.0f;
    for (int i = 0; i < kUiSounds; i++)
        shape(sharp, (UiSound)i, -14.0f, std::string("a theme's ") + kNames[i] + " with no attack or release");
    UiSoundTheme notes = ui_sound_theme_default();
    notes.voices[(int)UiSound::confirm].notes[0] = 440.0f;
    notes.voices[(int)UiSound::confirm].ms = 40.0f;
    const std::vector<int16_t> given = ui_sound_render(notes, UiSound::confirm);
    check(given.size() == 2 * 1920 && given != ui_sound_render(ui_sound_theme_default(), UiSound::confirm),
          "a theme's notes and length make a confirm of its own, 40 ms long");
}

// `frames` of stereo from ui_sound_mix over `in`.
std::vector<int16_t> mix(int frames, int16_t in = 0) {
    std::vector<int16_t> out(2 * (size_t)frames, in);
    ui_sound_mix(out.data(), frames);
    return out;
}

// Pulls until nothing plays, as their samples one after another.
std::vector<int16_t> drain() {
    std::vector<int16_t> all;
    for (int i = 0; i < 1000 && (ui_sound_playing() > 0 || ui_sound_queued() > 0 || all.empty()); i++) {
        const std::vector<int16_t> pull = mix(kPull);
        all.insert(all.end(), pull.begin(), pull.end());
    }
    return all;
}

// Whether `all` is `voice` and then silence.
bool plays_as(const std::vector<int16_t>& all, const std::vector<int16_t>& voice) {
    if (all.size() < voice.size() || !std::equal(voice.begin(), voice.end(), all.begin())) return false;
    return std::all_of(all.begin() + (long)voice.size(), all.end(), [](int16_t s) { return s == 0; });
}

void queue(const UiSoundTheme& made) {
    check(!ui_sound_enabled(), "the sounds are off until something turns them on");
    ui_sound_play(UiSound::move);
    const size_t queued_off = ui_sound_queued();
    const std::vector<int16_t> off = mix(kPull, 1234);
    check(queued_off == 0 && std::all_of(off.begin(), off.end(), [](int16_t s) { return s == 1234; }),
          "off, a play queues nothing and a pull adds nothing to what's there");

    ui_sound_set_theme(made);
    ui_sound_enable(true);
    check(ui_sound_enabled(), "on once ui_sound_enable says so");
    ui_sound_play(UiSound::confirm);
    check(ui_sound_queued() == 1, "on, a play queues its id");
    const std::vector<int16_t> first = mix(kPull);
    check(ui_sound_queued() == 0 && ui_sound_playing() == 1 && peak_of(first) >= level(-60.0f),
          "the next pull takes it and it's audible in that pull, over -60 dBFS");
    std::vector<int16_t> all = first;
    const std::vector<int16_t> rest = drain();
    all.insert(all.end(), rest.begin(), rest.end());
    const std::vector<int16_t> confirm = ui_sound_render(made, UiSound::confirm);
    check(plays_as(all, confirm) && ui_sound_playing() == 0, "across pulls it plays exactly as it was made, then stops");

    ui_sound_play(UiSound::move);
    const std::vector<int16_t> over = mix(kPull, 32000);
    const std::vector<int16_t> tick = ui_sound_render(made, UiSound::move);
    bool summed = true;
    for (int i = 0; i < 2 * kPull; i++) summed = summed && over[i] == std::clamp(32000 + tick[i], -32768, 32767);
    check(summed && peak_of(over) == 32767, "it's summed with what's there and held to 16 bits");
    drain();

    for (int i = 0; i < 20; i++) ui_sound_play(UiSound::move);
    const size_t waiting = ui_sound_queued();
    mix(kPull);
    check(waiting == 16 && ui_sound_queued() == 0 && ui_sound_playing() == 8,
          "no more than 16 wait and 8 play, and the rest are let go");
    drain();

    ui_sound_play(UiSound::close);
    std::vector<int16_t> to_end = mix(kPull);
    ui_sound_enable(false);
    ui_sound_play(UiSound::move);
    const size_t after_off = ui_sound_queued();
    const std::vector<int16_t> tail = drain();
    to_end.insert(to_end.end(), tail.begin(), tail.end());
    check(after_off == 0 && plays_as(to_end, ui_sound_render(made, UiSound::close)),
          "a voice playing as the sounds go off plays to its end, and nothing more is queued");

    ui_sound_set_theme(ui_sound_theme_default());
    ui_sound_enable(true);
    ui_sound_play(UiSound::confirm);
    check(plays_as(drain(), confirm), "a theme given once the sounds have been on is refused, and they stay as made");
    ui_sound_enable(false);
}

// What the movie tap was handed.
std::vector<int16_t> g_tapped;
void tap(const int16_t* s, int frames) { g_tapped.insert(g_tapped.end(), s, s + 2 * frames); }

std::string read_file(const std::string& path) {
    std::string text;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        fclose(f);
    }
    return text;
}

void through_audio_render(const std::string& dir, const UiSoundTheme& made) {
    const std::string wav = dir + "/out.wav";
    g_wav = fopen(wav.c_str(), "wb");
    wav_header(g_wav, 0);
    g_audio_tap = tap;
    ui_sound_enable(false);
    ui_sound_play(UiSound::open);
    std::vector<int16_t> silent(2 * kPull, 1);
    audio_render(silent.data(), kPull);
    const bool none_off = std::all_of(silent.begin(), silent.end(), [](int16_t s) { return s == 0; });
    ui_sound_enable(true);
    ui_sound_play(UiSound::open);
    std::vector<int16_t> device;
    for (int i = 0; i < 20; i++) {
        std::vector<int16_t> pull(2 * kPull);
        audio_render(pull.data(), kPull);
        device.insert(device.end(), pull.begin(), pull.end());
    }
    ui_sound_enable(false);
    fclose(g_wav);
    g_wav = nullptr;
    g_audio_tap = nullptr;
    const std::string bytes = read_file(wav);
    const std::vector<int16_t> open = ui_sound_render(made, UiSound::open);
    const std::vector<int16_t> tapped(g_tapped.begin() + 2 * kPull, g_tapped.end());
    check(none_off, "through audio_render with the sounds off, a play adds nothing to the device's samples");
    check(plays_as(device, open) && tapped == device,
          "on, the device's samples hold the voice exactly, and so does the movie tap");
    check(bytes.size() == 44 + 21 * kPull * 4 && bytes.compare(36, 4, "data") == 0 &&
              std::all_of(bytes.begin() + 44, bytes.end(), [](char c) { return c == 0; }),
          "and GCN_WAV's recording of the same pulls holds none of it, only the silent ring");
}

void costs() {
    ui_sound_enable(true);
    using clock = std::chrono::steady_clock;
    const auto per_pull = [](int voices) {
        constexpr int kPulls = 20000;
        std::vector<int16_t> out(2 * kPull);
        clock::duration spent{};
        for (int i = 0; i < kPulls; i++) {
            while (ui_sound_playing() < voices && ui_sound_queued() == 0) {
                for (int v = ui_sound_playing(); v < voices; v++) ui_sound_play(UiSound::open);
                mix(1);
            }
            const auto at = clock::now();
            ui_sound_mix(out.data(), kPull);
            spent += clock::now() - at;
        }
        return std::chrono::duration<double, std::nano>(spent).count() / kPulls;
    };
    drain();
    const double idle = per_pull(0), one = per_pull(1), eight = per_pull(8);
    printf("[sound] the mix's cost per 512-frame pull: %.0f ns idle, %.0f ns with 1 voice, %.0f ns with 8\n", idle,
           one, eight);
    drain();
    ui_sound_enable(false);
}

void write_wav(const std::string& path, const std::vector<int16_t>& s) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        check(false, "can write " + path);
        return;
    }
    const auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    const auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    const uint32_t bytes = (uint32_t)(s.size() * 2);
    fwrite("RIFF", 1, 4, f);
    w32(36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(2); w32(kUiSoundRate); w32(kUiSoundRate * 4); w16(4); w16(16);
    fwrite("data", 1, 4, f);
    w32(bytes);
    fwrite(s.data(), 2, s.size(), f);
    fclose(f);
    printf("[sound] %s, %zu frames\n", path.c_str(), s.size() / 2);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: test_ui_sound <work dir> [<wav dir>]\n");
        return 2;
    }
    const std::string dir = argv[1];
    if (mkdir(dir.c_str(), 0755) != 0) {
        fprintf(stderr, "%s: can't make it (%s)\n", dir.c_str(), strerror(errno));
        return 2;
    }
    // The voices the mix plays here are a theme of their own, so a mix of the default's would show.
    UiSoundTheme made = ui_sound_theme_default();
    made.peak_dbfs = -16.0f;
    voices();
    themes();
    queue(made);
    through_audio_render(dir, made);
    costs();
    if (argc == 3)
        for (int i = 0; i < kUiSounds; i++)
            write_wav(std::string(argv[2]) + "/" + kNames[i] + ".wav", ui_sound_render(ui_sound_theme_default(), (UiSound)i));
    printf("test_ui_sound: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
