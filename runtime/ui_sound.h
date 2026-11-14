// The menu's sounds: a tick as the focus moves, a confirm, a back, a page's swish to the side it
// turns to, a swell as the menu opens and as it closes, and a buzz for a press that does nothing.
// Each is made here from sines and noise, never taken from a disc, at 48 kHz stereo, the mix's
// rate (audio.cpp).
//
// The main thread asks for one with ui_sound_play, which puts its id on a lock-free queue for
// one producer and one consumer. The audio thread's ui_sound_mix takes what's queued at the start
// of each pull and adds every voice playing into the pull's samples. audio_render calls it after
// GCN_WAV's capture and before the movie tap (record.h), so a WAV recording, and the audio hashes
// taken from one, never hold a menu sound, while a --record movie and the device do.
//
// They're off until ui_sound_enable turns them on, as settings.txt's menu_sounds does (settings.h),
// and off by default. While off, ui_sound_play queues nothing, so nothing is mixed; a voice that
// was playing as they went off plays to its end.
//
// Each voice has an envelope that rises from silence over its attack, a raised cosine, decays
// after it where it has a decay, and falls to silence over its release, a raised cosine, each at
// least kUiSoundEdgeMs. So every voice starts and ends on a zero sample and no edge clicks. Each
// is scaled so its louder channel peaks at the theme's level, which is never over kUiSoundMaxDbfs.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

enum class UiSound : uint8_t { move, confirm, back, page_back, page_on, open, close, refused };
constexpr int kUiSounds = 8;

constexpr int kUiSoundRate = 48000;
constexpr float kUiSoundEdgeMs = 2.0f;     // the shortest attack and release
constexpr float kUiSoundMaxDbfs = -12.0f;  // the loudest peak a theme may ask for

// A voice's notes, in Hz, are read by kind:
//   move           notes[0] a sine and notes[1] a triangle over it at a quarter of its level
//   confirm, back  notes[0] for the first half and notes[1] for the second, the phase carried over
//   page_*         noise through a band-pass whose centre sweeps from notes[0] to notes[1]
//   open, close    notes[0], notes[1] and notes[2] together, each a sine; a note of 0 is left out
//   refused        notes[0], the fundamental of a square built from its odd harmonics under 4 kHz
struct UiVoice {
    float notes[3];
    float ms;          // how long it lasts
    float attack_ms;   // rising from silence
    float decay_ms;    // after the attack, the time it takes to fall to 1/e; 0 for none
    float release_ms;  // falling to silence at its end
};

struct UiSoundTheme {
    float peak_dbfs = -14.0f;  // each voice's peak, its louder channel's
    float pan = 0.6f;          // how far to its side a page's swish is, 0 the middle and 1 all the way
    UiVoice voices[kUiSounds] = {
        {{1400.0f, 2800.0f, 0.0f}, 22.0f, 2.0f, 6.0f, 4.0f},          // move
        {{660.0f, 990.0f, 0.0f}, 80.0f, 3.0f, 45.0f, 12.0f},          // confirm
        {{990.0f, 660.0f, 0.0f}, 80.0f, 3.0f, 45.0f, 12.0f},          // back
        {{1200.0f, 3200.0f, 0.0f}, 90.0f, 30.0f, 0.0f, 55.0f},        // page_back, to the left
        {{1200.0f, 3200.0f, 0.0f}, 90.0f, 30.0f, 0.0f, 55.0f},        // page_on, to the right
        {{523.25f, 659.25f, 783.99f}, 160.0f, 70.0f, 0.0f, 85.0f},    // open, C major swelling in
        {{392.0f, 493.88f, 587.33f}, 160.0f, 12.0f, 60.0f, 110.0f},   // close, G major falling away
        {{150.0f, 0.0f, 0.0f}, 60.0f, 3.0f, 0.0f, 20.0f},             // refused
    };
};

inline UiSoundTheme ui_sound_theme_default() { return UiSoundTheme{}; }

// The theme the voices are made from. Before the first ui_sound_enable(true), which makes them;
// after it, one line on stderr and nothing changes.
void ui_sound_set_theme(const UiSoundTheme& theme);
// On or off, from the main thread. The first time on makes the voices.
void ui_sound_enable(bool on);
bool ui_sound_enabled();
// From the main thread: `id` starts at the next pull. Nothing while off, and nothing once 16
// are queued or, at the pull, once 8 voices are playing, so no voice is ever cut short.
void ui_sound_play(UiSound id);
// From the audio thread: what's queued started, and every voice playing added into `out`,
// `frames` of stereo, each sum held to 16 bits.
void ui_sound_mix(int16_t* out, int frames);

// For the checks (tools/test_ui_sound.cpp), each a pure function of its arguments.
// The theme as the voices are made from it: the level no louder than kUiSoundMaxDbfs, each edge
// at least kUiSoundEdgeMs, a voice at least as long as its two edges, and the pan from 0 to 1.
UiSoundTheme ui_sound_fit(const UiSoundTheme& theme);
// Voice `id` of a fitted theme, interleaved stereo.
std::vector<int16_t> ui_sound_render(const UiSoundTheme& theme, UiSound id);
// The envelope of `v`, fitted, at frame `n` of its `frames`, from 0 to 1.
float ui_sound_envelope(const UiVoice& v, int n, int frames);
// How many ids are queued, and how many voices play, as the audio thread last left them.
size_t ui_sound_queued();
int ui_sound_playing();
