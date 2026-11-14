// How the menu's sounds (ui_sound.h) are made: each from sines and noise, never taken from a
// disc, at 48 kHz stereo, the mix's rate (audio.cpp). Each a pure function of its arguments, run
// on the main thread as the sounds first go on and by the checks (tools/test_ui_sound.cpp).
//
// Each voice has an envelope that rises from silence over its attack, a raised cosine, decays
// after it where it has a decay, and falls to silence over its release, a raised cosine, each at
// least kUiSoundEdgeMs, as is a decay. So every voice starts and ends on a zero sample, and its
// envelope changes from one sample to the next no faster than a raised cosine over kUiSoundEdgeMs
// does, so no edge clicks. Each is scaled so its louder channel peaks at the theme's level, which
// is never over kUiSoundMaxDbfs.
#pragma once
#include <cstdint>
#include <vector>

enum class UiSound : uint8_t { move, confirm, back, page_back, page_on, open, close, refused };
constexpr int kUiSounds = 8;

constexpr int kUiSoundRate = 48000;
constexpr float kUiSoundEdgeMs = 2.0f;     // the shortest attack and release
constexpr float kUiSoundMaxDbfs = -12.0f;  // the loudest peak a theme may ask for
constexpr float kUiSoundLongestEdgeMs = 1000.0f;  // the longest attack and release
constexpr float kUiSoundLongestMs = 2000.0f;      // the longest voice
constexpr float kUiSoundLowestHz = 20.0f;         // a note under it, or one not a number, is 0

// A voice's notes, in Hz, are read by kind; a note of 0 is left out, and a voice of none is silent:
//   move           notes[0] a sine and notes[1] a triangle over it at a quarter of its level
//   confirm, back  notes[0] for the first half and notes[1] for the second, the phase carried over
//   page_*         noise through a band-pass whose centre sweeps from notes[0] to notes[1]
//   open, close    notes[0], notes[1] and notes[2] together, each a sine
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

// The theme as the voices are made from it: the level no louder than kUiSoundMaxDbfs, each note
// 0 or from kUiSoundLowestHz to 20 kHz, each edge from kUiSoundEdgeMs to kUiSoundLongestEdgeMs,
// a decay none or at least kUiSoundEdgeMs, a voice at least as long as its two edges and no
// longer than kUiSoundLongestMs, and the pan from 0 to 1. An infinity is taken as the largest
// float of its sign. No value, however large or small or not a number, makes a voice that never
// ends.
UiSoundTheme ui_sound_fit(const UiSoundTheme& theme);
// Voice `id` of a fitted theme, interleaved stereo.
std::vector<int16_t> ui_sound_render(const UiSoundTheme& theme, UiSound id);
// The envelope of `v`, fitted, at frame `n` of its `frames`, from 0 to 1.
float ui_sound_envelope(const UiVoice& v, int n, int frames);
