// The IPL's SRAM: 64 bytes a console keeps on its battery, which hold the sound mode, the
// language, the display offset and the memory cards' flash IDs. The runtime keeps them in a
// file beside the memory card (exi.cpp), so a setting the game writes there outlasts the run.
// Free of the rest of the runtime, so tools/test_sram.cpp checks it on its own.
//
// The layout is the SDK's OSSram then OSSramEx, big-endian:
//   0x00 checksum, 0x02 its inverse: the 16-bit sums of the words 0x0C to 0x13 and of
//        their complements
//   0x0C counter bias, 0x10 display offset, 0x11 ntd, 0x12 language, 0x13 flags (bit 2 set
//        for stereo)
//   0x14 two 12-byte flash IDs, 0x3A each one's checksum, the complement of its bytes' sum
#pragma once
#include <cstdint>
#include <string>

constexpr int SRAM_SIZE = 64;

// The SRAM every run started from before it was saved: English, stereo, no display offset,
// zero flash IDs and the checksums to match.
void sram_defaults(uint8_t* sram);

// Whether the checksum pair at 0x00 and both flash IDs' checksums hold.
bool sram_checksums_ok(const uint8_t* sram);

// Fills `sram` from `path`. True if it did; false, with the defaults in `sram`, when the
// file is missing, isn't 64 bytes or fails a checksum. Says why on stderr, except for a
// missing file, which is how every fresh install starts.
bool sram_load(const std::string& path, uint8_t* sram);

// Writes `sram` to `path` through a temp file it then moves over the old one, making the
// directory first, so a save that fails leaves the old file whole. False, with a line on
// stderr, if it fails.
bool sram_save(const std::string& path, const uint8_t* sram);
