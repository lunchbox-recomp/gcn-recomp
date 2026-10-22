// The GL reference renderer: draws a key's real GLSL, from shadergen, on Apple's GL.
#pragma once
#include "keys_io.h"
#include "test_inputs.h"

// Opens a 3.2 core context on the default renderer and prints its name. False if none.
bool gl_init();

// Draws the inputs with the key into a 256x256 RGBA8 plus Depth32F target and reads it back.
// logic_op >= 0 draws over in.dst with that GX logic op.
Image gl_draw(const gx::ShaderKey& k, const Inputs& in, int logic_op);
