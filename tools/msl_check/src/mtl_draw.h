// The Metal side of the comparison: draws a key's MSL translation with the same inputs as
// gl_draw.
#pragma once
#import <Metal/Metal.h>
#include "keys_io.h"
#include "test_inputs.h"

// Draws into a 256x256 RGBA8 plus Depth32F target and reads it back. logic_op >= 0 draws
// over in.dst with that GX logic op, through framebuffer fetch. Exits on a compile failure.
Image mtl_draw(id<MTLDevice> dev, id<MTLCommandQueue> q, const gx::ShaderKey& k, const Inputs& in, int logic_op);
