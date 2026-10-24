// What runtime_check's include path gives main.cpp, which test_runtime_engine_sha.cmake
// reads preprocessed: the engine's commit from gcn_engine_sha.h, as the game's own build
// gets it, and not main.cpp's fallback of "".
#if __has_include("gcn_engine_sha.h")
#include "gcn_engine_sha.h"
#else
#define GCN_ENGINE_SHA "no gcn_engine_sha.h"
#endif
const char* gcn_engine_sha_seen = GCN_ENGINE_SHA;
