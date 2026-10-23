// SRAM file checks: the `test_sram` target. Runs with no disc image, device, SDL or game.
// runtime/hw/sram.cpp is linked as it is, and platform.cpp for its directories and file
// replace.
//
// The SRAM used to start from the defaults at every launch and was never saved, so a sound
// mode a game set lasted only until it quit. Each case writes a file into the work dir,
// loads or saves it, and checks the bytes. A file that's missing, the wrong size or fails
// a checksum must give the defaults, which is how every run started before. A save that
// fails must leave the old file whole, so two failures are made to happen: a directory
// where the save writes its temp file, and a file size limit (RLIMIT_FSIZE) that stops
// that write partway as a full disk would.
//
// usage: test_sram <work dir>
// The work dir must not exist. It's made, and the files are left in it. Exits 0 only if
// every check passes.
#include "hw/sram.h"
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int g_checks, g_failed;
std::string g_dir;

void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}

std::string path_of(const char* name) { return g_dir + "/" + name; }

void write_file(const std::string& path, const void* data, size_t size) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return; }
    fwrite(data, 1, size, f);
    fclose(f);
}

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

std::string bytes(const uint8_t* s) { return std::string((const char*)s, SRAM_SIZE); }

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool is_defaults(const uint8_t* s) {
    uint8_t d[SRAM_SIZE];
    sram_defaults(d);
    return !memcmp(s, d, SRAM_SIZE);
}

// What the guest's OS does after changing the SRAM: the checksum pair over 0x0C to 0x13.
void fix_checksums(uint8_t* s) {
    uint16_t sum = 0, inv = 0;
    for (int i = 0x0C; i < 0x14; i += 2) {
        const uint16_t w = (uint16_t)((s[i] << 8) | s[i + 1]);
        sum += w;
        inv += (uint16_t)~w;
    }
    s[0] = sum >> 8;
    s[1] = (uint8_t)sum;
    s[2] = inv >> 8;
    s[3] = (uint8_t)inv;
}

// The defaults with the sound mode set to mono, as a game's options screen leaves them.
void mono(uint8_t* s) {
    sram_defaults(s);
    s[0x13] &= ~4;
    fix_checksums(s);
}

void defaults() {
    uint8_t s[SRAM_SIZE];
    sram_defaults(s);
    check(s[0x13] == 0x2C && s[0x12] == 0, "the defaults are stereo and English");
    check(s[0] == 0x00 && s[1] == 0x2C && s[2] == 0xFF && s[3] == 0xD0,
          "with the checksum pair 002C, FFD0 over the words 0x0C to 0x13");
    check(s[0x3A] == 0xFF && s[0x3B] == 0xFF, "and FF for each zero flash ID's checksum");
    check(sram_checksums_ok(s), "which the check takes");
}

void missing_file() {
    uint8_t s[SRAM_SIZE];
    memset(s, 0xA5, sizeof(s));
    const std::string p = path_of("none.raw");
    check(!sram_load(p, s) && is_defaults(s), "a missing file gives the defaults");
    check(!exists(p), "and a load doesn't make one");
}

void save_and_load() {
    uint8_t want[SRAM_SIZE], got[SRAM_SIZE];
    mono(want);
    const std::string p = path_of("saves/sram.raw");
    check(sram_save(p, want), "a save into a directory that isn't there yet succeeds");
    check(read_file(p) == bytes(want), "the file is the 64 bytes as they were");
    check(!exists(p + ".tmp"), "and a save leaves no temp file behind");
    memset(got, 0, sizeof(got));
    check(sram_load(p, got) && !memcmp(got, want, SRAM_SIZE) && (got[0x13] & 4) == 0,
          "a load gives back what was saved, mono included");

    uint8_t stereo[SRAM_SIZE];
    sram_defaults(stereo);
    check(sram_save(p, stereo) && read_file(p) == bytes(stereo), "a save over an existing file replaces it");
}

void bad_files() {
    uint8_t good[SRAM_SIZE], s[SRAM_SIZE];
    mono(good);
    struct Case {
        const char* name;
        int at;  // the byte to change, or -1 for the size
        size_t size;
        const char* what;
    };
    const Case cases[] = {
        {"sum.raw", 1, SRAM_SIZE, "a checksum one out gives the defaults"},
        {"inv.raw", 3, SRAM_SIZE, "an inverse checksum one out gives the defaults"},
        {"flags.raw", 0x13, SRAM_SIZE, "a flags byte the checksums don't match gives the defaults"},
        {"flash_a.raw", 0x14, SRAM_SIZE, "slot A's flash ID changed under its checksum gives the defaults"},
        {"flash_b.raw", 0x3B, SRAM_SIZE, "slot B's flash ID checksum one out gives the defaults"},
        {"short.raw", -1, SRAM_SIZE - 1, "63 bytes give the defaults"},
        {"long.raw", -1, SRAM_SIZE + 1, "65 bytes give the defaults"},
        {"empty.raw", -1, 0, "an empty file gives the defaults"},
    };
    for (const Case& c : cases) {
        uint8_t file[SRAM_SIZE + 1];
        memcpy(file, good, SRAM_SIZE);
        file[SRAM_SIZE] = 0;
        if (c.at >= 0) file[c.at] ^= 1;
        write_file(path_of(c.name), file, c.size);
        memset(s, 0xA5, sizeof(s));
        check(!sram_load(path_of(c.name), s) && is_defaults(s), c.what);
    }
    check(sram_checksums_ok(good), "while the file they were made from passes");
}

void failed_save_no_temp() {
    uint8_t before[SRAM_SIZE], s[SRAM_SIZE];
    mono(before);
    const std::string p = path_of("blocked.raw");
    write_file(p, before, SRAM_SIZE);
    mkdir((p + ".tmp").c_str(), 0755);
    sram_defaults(s);
    check(!sram_save(p, s), "a save fails when a directory holds its temp file's name");
    check(read_file(p) == bytes(before), "and leaves the old file whole");
}

void failed_save_cut_short() {
    uint8_t before[SRAM_SIZE], s[SRAM_SIZE];
    mono(before);
    const std::string p = path_of("full.raw");
    write_file(p, before, SRAM_SIZE);
    // Past the limit a write fails with EFBIG, once SIGXFSZ no longer ends the process.
    signal(SIGXFSZ, SIG_IGN);
    rlimit old;
    getrlimit(RLIMIT_FSIZE, &old);
    rlimit limit = old;
    limit.rlim_cur = 32;
    setrlimit(RLIMIT_FSIZE, &limit);
    sram_defaults(s);
    const bool saved = sram_save(p, s);
    setrlimit(RLIMIT_FSIZE, &old);
    // A stderr sent to a file failed under the limit too, and stays failed until cleared.
    clearerr(stderr);
    check(!saved, "a save fails when only 32 bytes of its file can be written");
    check(read_file(p) == bytes(before), "and leaves the old file whole");
    check(!exists(p + ".tmp"), "and removes the part it wrote");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_sram <work dir>\n");
        return 2;
    }
    g_dir = argv[1];
    if (exists(g_dir) || mkdir(g_dir.c_str(), 0755) != 0) {
        fprintf(stderr, "%s exists or can't be made: give a new directory\n", g_dir.c_str());
        return 2;
    }
    defaults();
    missing_file();
    save_and_load();
    bad_files();
    failed_save_no_temp();
    failed_save_cut_short();
    printf("test_sram: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
