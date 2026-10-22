// ShaderKey to MSL: the prelude that gives MSL GLSL's vocabulary, and the two functions that
// wrap shadergen's shader bodies in the declarations and entry points Metal needs.
#pragma once
#include "keys_io.h"
#include <string>

extern const char* const kPrelude;

// Negative control of `compare --flipfc`: the fragment entry point flips gl_FragCoord's y.
extern bool g_flip_fc;

// logic_op >= 0 adds the GX logic op on the target's color (framebuffer fetch).
// nonce != 0 adds a test on it, so that a compile cannot come from the OS's shader cache.
std::string msl_fragment(const gx::ShaderKey& k, int logic_op = -1, int nonce = 0);
std::string msl_vertex();
