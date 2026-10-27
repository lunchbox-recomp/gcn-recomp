// A disc overlay at run time: byte ranges of the virtual disc that read from host files, and
// header words that read as the manifest says. recomp/disc_overlay.py writes the manifest and
// docs/disc-overlay.md describes it.
//
// Free of the rest of the runtime, so tools/test_disc_overlay.cpp checks it on its own. Load
// one before the game starts and nothing changes after, so reads from the guest's threads and
// the audio callback need no lock.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// The read the overlay sits on, which is the image, plain or CISO. False if it can't give all
// len bytes.
using BaseRead = bool (*)(uint64_t off, void* dst, uint32_t len);

// Read the manifest and open every host file it names, replacing any overlay already loaded.
// The image has to be the one the manifest was made for: its disc ID and its FST's offset,
// size and FNV-1a hash are read through `base` and compared. On a refusal nothing is loaded
// and *error (if given) says why, naming the manifest and the line.
bool disc_overlay_load(const char* manifest, BaseRead base, std::string* error);

// len bytes of the virtual disc from off. Ranges come from their host files, header words
// from the manifest, and every other byte from `base`, except that what `base` doesn't have,
// which is a plain image's bytes past its end, reads as zeros. A CISO does that already, so
// both bases give the game the same bytes, and a read the game rounds up to 32 bytes past the
// end of a file succeeds. With no overlay loaded it is `base`, failures included.
bool disc_overlay_read(uint64_t off, void* dst, uint32_t len, BaseRead base);

bool disc_overlay_loaded();
// Close the host files and drop the overlay.
void disc_overlay_unload();

// The manifest to load: --disc-overlay=FILE if g_disc_overlay_flag is set, else the
// GCN_DISC_OVERLAY variable, else null. An empty variable counts as unset.
extern const char* g_disc_overlay_flag;
const char* disc_overlay_path();

// Where --log=dvd lines go, or null for none. Called with the load and with each piece of a
// read that an overlay serves.
extern void (*g_disc_overlay_log)(const char* line);

// 64-bit FNV-1a, the hash the manifest's base line holds.
uint64_t disc_overlay_fnv1a64(const void* data, size_t size);
