#pragma once
#include <string>

// Select the RAM patch list at process start. A flag, including an empty one, wins. A bare flag
// returns false with a one-line reason in `error` and leaves `file` as it was.
bool ram_patches_choose(const char* variable, int argc, char** argv, std::string& file, std::string& error);
void ram_patches_set_file(const std::string& path);
const std::string& ram_patches_file();  // what ram_patches_set_file last took, empty for none
void ram_patches_apply(const char* disc_id);
