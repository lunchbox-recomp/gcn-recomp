// See sram.h.
#include "sram.h"
#include "../platform.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

// The checksum pair over the words 0x0C to 0x13, as the SDK's OS keeps it.
void checksums(const uint8_t* s, uint16_t& sum, uint16_t& inv) {
    sum = inv = 0;
    for (int i = 0x0C; i < 0x14; i += 2) {
        const uint16_t w = (uint16_t)((s[i] << 8) | s[i + 1]);
        sum += w;
        inv += (uint16_t)~w;
    }
}

}  // namespace

void sram_defaults(uint8_t* s) {
    memset(s, 0, SRAM_SIZE);
    // Counter bias, display offset, ntd and language (English) zero; flags 0x2C, whose bit 2
    // is stereo.
    s[0x13] = 0x2C;
    // Both flash IDs are zero, so each checksum is the complement of zero.
    s[0x3A] = 0xFF;
    s[0x3B] = 0xFF;
    uint16_t sum, inv;
    checksums(s, sum, inv);
    s[0] = sum >> 8;
    s[1] = (uint8_t)sum;
    s[2] = inv >> 8;
    s[3] = (uint8_t)inv;
}

bool sram_checksums_ok(const uint8_t* s) {
    uint16_t sum, inv;
    checksums(s, sum, inv);
    if (((s[0] << 8) | s[1]) != sum || ((s[2] << 8) | s[3]) != inv) return false;
    for (int card = 0; card < 2; card++) {
        uint8_t id = 0;
        for (int i = 0; i < 12; i++) id += s[0x14 + 12 * card + i];
        if (s[0x3A + card] != (uint8_t)~id) return false;
    }
    return true;
}

bool sram_load(const std::string& path, uint8_t* s) {
    uint8_t buf[SRAM_SIZE + 1];
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (errno != ENOENT) fprintf(stderr, "%s: can't read it (%s), so the SRAM starts from the defaults\n", path.c_str(), strerror(errno));
        sram_defaults(s);
        return false;
    }
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    const char* bad = n != SRAM_SIZE ? "isn't 64 bytes" : !sram_checksums_ok(buf) ? "fails its checksums" : nullptr;
    if (bad) {
        fprintf(stderr, "%s: %s, so the SRAM starts from the defaults\n", path.c_str(), bad);
        sram_defaults(s);
        return false;
    }
    memcpy(s, buf, SRAM_SIZE);
    return true;
}

bool sram_save(const std::string& path, const uint8_t* s) {
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) plat_make_dirs(path.substr(0, slash));
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "%s: can't write it (%s), so the SRAM isn't saved\n", tmp.c_str(), strerror(errno));
        return false;
    }
    const bool wrote = fwrite(s, 1, SRAM_SIZE, f) == SRAM_SIZE;
    const bool closed = fclose(f) == 0;
    if (!wrote || !closed || !plat_replace_file(tmp.c_str(), path.c_str())) {
#ifdef _WIN32
        // MoveFileExA reports through GetLastError and leaves errno as it was. Any call can
        // change the last error, so it is read before anything else is.
        const DWORD win_error = GetLastError();
#endif
        std::string why = strerror(errno);
#ifdef _WIN32
        if (wrote && closed) why = "Windows error " + std::to_string(win_error);
#endif
        fprintf(stderr, "%s: the save failed (%s), and the old file is as it was\n", path.c_str(), why.c_str());
        remove(tmp.c_str());
        return false;
    }
    return true;
}
