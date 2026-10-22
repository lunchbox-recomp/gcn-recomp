// The comparison's inputs: one full-screen triangle, eight textures, eight palettes and both
// uniform blocks, the same for GL and for Metal, and the image each renderer hands back.
#pragma once
#include "msl_blocks.h"
#include <cstdint>
#include <vector>

static const int W = 256, H = 256, TS = 64;
static const int kFloats = 3 + 4 + 4 + 8 * 3;  // per vertex

struct Inputs {
    float verts[3 * kFloats];
    uint8_t tex[8][TS * TS * 4];
    uint8_t pal[8][256 * 4];
    uint8_t dst[W * H * 4];  // the target's contents before the draw (logic op test)
    blk::FsBlock fs;
    blk::VsBlock vs;
};

struct Image {
    std::vector<uint8_t> rgba;
    std::vector<float> depth;
};

// Options of the draw, set by the `compare` and `logic` arguments.
extern bool g_linear;
extern bool g_cull_back, g_mtl_ccw;

void make_inputs(Inputs& in);
