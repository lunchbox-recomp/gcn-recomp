#pragma once
#include <string>

// Select the RAM patch list at process start. A flag, including an empty one, wins.
std::string ram_patches_choose(const char* variable, int argc, char** argv);
void ram_patches_set_file(const std::string& path);
void ram_patches_apply(const char* disc_id);
