// The command modes of msl_spike. main() picks one by name.
#pragma once
#include <cstdint>
#include <string>

// Metal compile and pipeline timings (cmd_compile.mm).
int cmd_emit(const char* keys_path, const std::string& dir);
int cmd_compile(const char* keys_path, bool cold, const char* err_path);
int cmd_blendvar(const char* keys_path);
int cmd_compile_parallel(const char* keys_path);
int cmd_floor();
int cmd_floor_files(int argc, char** argv);

// The same keys through GL (cmd_glcompile.cpp).
int cmd_glcompile(const char* keys_path);

// GL against Metal, with or without a logic op (cmd_compare.mm).
int cmd_compare(const char* keys_path, int logic_op, int argc, char** argv);

// The made-up key list (cmd_synth.cpp).
int cmd_synth(const char* out, uint32_t seed, int count);
