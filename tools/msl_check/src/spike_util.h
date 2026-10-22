// Small helpers shared by the modes: a clock, a text file writer, key list loading, the
// summary line that timings print and the parsing of numeric arguments.
#pragma once
#include "keys_io.h"
#include <string>
#include <vector>

double now_ms();
void write_text(const std::string& path, const std::string& s);
std::vector<gx::ShaderKey> load_keys(const char* path);
void stats(const char* what, std::vector<double> v);

// The whole of s as a decimal integer from lo to hi. False for anything else, an empty
// string, leading spaces and trailing characters included.
bool parse_long(const char* s, long lo, long hi, long* out);
// The whole of s as a float, inf and nan included. False for anything else or a value too
// large for a float.
bool parse_float(const char* s, float* out);
