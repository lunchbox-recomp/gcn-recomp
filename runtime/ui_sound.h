// The menu's sounds: a tick as the focus moves, a confirm, a back, a page's swish to the side it
// turns to, a swell as the menu opens and as it closes, and a buzz for a press that does nothing.
// How each is made, from a theme, is ui_sound_synth.h's.
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
#pragma once
#include "ui_sound_synth.h"
#include <cstddef>
#include <cstdint>
#include <vector>

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
// `frames` of stereo, what was there and every voice summed in 32 bits, then held to 16 once.
void ui_sound_mix(int16_t* out, int frames);

// For the checks (tools/test_ui_sound.cpp): how many ids are queued, and how many voices play,
// as the audio thread last left them.
size_t ui_sound_queued();
int ui_sound_playing();
#ifdef GCN_OVERLAY_TEST
// For a check with no audio thread (tools/test_ports_sounds.cpp): the ids queued, in order,
// taken off the queue as a pull takes them, but not played.
std::vector<UiSound> ui_sound_take();
// The theme the voices are made from, as ui_sound_set_theme last left it
// (tools/test_ports_init_order.cpp).
const UiSoundTheme& ui_sound_test_theme();
#endif
