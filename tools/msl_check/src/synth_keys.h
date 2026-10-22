// The made-up key list: not game data, built from the key's fields alone.
#pragma once
#include "keys_io.h"
#include <cstdint>
#include <vector>

// The directed keys first (their count goes to *directed), then `count` random ones.
std::vector<gx::ShaderKey> synth_keys(uint32_t seed, int count, size_t* directed);
