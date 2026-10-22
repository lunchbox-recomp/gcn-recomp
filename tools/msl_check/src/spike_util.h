// Small helpers shared by the modes: a clock, a text file writer, key list loading and the
// summary line that timings print.
#pragma once
#include "keys_io.h"
#include <string>
#include <vector>

double now_ms();
void write_text(const std::string& path, const std::string& s);
std::vector<gx::ShaderKey> load_keys(const char* path);
void stats(const char* what, std::vector<double> v);
