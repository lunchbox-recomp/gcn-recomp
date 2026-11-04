#pragma once
#include <cstdio>
#include <string>

struct FeedDump { FILE* dump; FILE* index; };

inline FeedDump open_feed_dump(const char* path) {
    if (!path || !*path) return {nullptr, nullptr};
    FILE* dump = fopen(path, "wb");
    if (!dump) return {nullptr, nullptr};
    FILE* index = fopen((std::string(path) + ".idx").c_str(), "w");
    if (!index) { fclose(dump); return {nullptr, nullptr}; }
    return {dump, index};
}
