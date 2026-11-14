// The stand-ins for the system's sync that test_sram, test_exi and test_ready_file set as
// plat_sync_for_tests (platform.h), with the file reader the first two use. seen_sync keeps
// what the file being saved, g_synced, and its temporary file held when the save called the
// sync; refused_sync fails it as a full disk would. Each counts its calls in g_syncs, which a
// card's saver thread may bump.
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <string>

namespace {

// The file's bytes, or "<missing>".
std::string read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "<missing>";
    std::string text;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    return text;
}

std::atomic<int> g_syncs;
std::string g_synced, g_temp_at_sync, g_file_at_sync;

bool seen_sync(FILE*) {
    g_syncs++;
    g_temp_at_sync = read_file(g_synced + ".tmp");
    g_file_at_sync = read_file(g_synced);
    return true;
}

bool refused_sync(FILE*) {
    g_syncs++;
    errno = EIO;
    return false;
}

}  // namespace
