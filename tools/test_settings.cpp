// settings.txt checks: the `test_settings` target. Runs with no disc image, device, SDL or
// game. runtime/settings.cpp is linked as it is, and platform.cpp for its file replace.
//
// Each case writes a file into the work dir, loads or saves it, and checks the struct or
// the file's bytes. A save that fails must leave the old file whole, so four failures are
// made to happen. One is a directory where the save writes its new file. Two are a file size
// limit (RLIMIT_FSIZE) that stops that write partway as a full disk would: on a file that
// fits in stdio's buffer, where fclose's flush hits the limit, and on one that doesn't,
// where fwrite does. The last is an old file the save can't read. A save that wrote the
// file in place fails the directory case, which it gets past, and the limit cases, which
// leave it a cut-off file.
//
// usage: test_settings <work dir>
// The work dir must not exist. It's made, and the files are left in it. Exits 0 only if
// every check passes.
#include "settings.h"
#include <cmath>
#include <csignal>
#include <cstdio>
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

void write_file(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return; }
    fwrite(text.data(), 1, text.size(), f);
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

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

void missing_file() {
    const Settings s = settings_load(path_of("none.txt"));
    check(s.aspect == "4:3" && !s.fullscreen && s.scale == 2,
          "a missing file gives aspect 4:3, fullscreen 0 and scale 2");
}

void parsing() {
    const std::string p = path_of("parse.txt");
    write_file(p,
               "# The picture\n"
               "aspect 16:9\n"
               "\n"
               "  fullscreen\t1\n"
               "scale 3   \n"
               "  # an indented comment\n"
               "vsync on\n"
               "greeting hello there\r\n"
               "vsync off");
    const Settings s = settings_load(p);
    check(s.aspect == "16:9", "aspect 16:9 is read");
    check(s.fullscreen, "fullscreen 1 is read past leading spaces and a tab");
    check(s.scale == 3, "scale 3 is read with trailing spaces");
}

void byte_order_mark() {
    const std::string p = path_of("bom.txt");
    // Two literals, since \xBF would otherwise take the a of aspect as another hex digit.
    write_file(p, "\xEF\xBB\xBF" "aspect 16:9\nscale 3\n");
    const Settings s = settings_load(p);
    check(s.aspect == "16:9" && s.scale == 3, "a UTF-8 byte order mark before the first key doesn't hide it");
    Settings w;
    w.aspect = "21:9";
    w.scale = 3;
    check(settings_save(p, w) && read_file(p) == "aspect 21:9\nscale 3\nfullscreen 0\n",
          "and a save rewrites that line, without the mark, rather than adding a second");
}

void aspects() {
    float r = -1;
    check(settings_parse_aspect("16:9", r) && std::fabs(r - 16.0f / 9.0f) < 1e-6f, "16:9 is a ratio of 16/9");
    check(settings_parse_aspect("21:9", r) && std::fabs(r - 21.0f / 9.0f) < 1e-6f, "21:9 is a ratio of 21/9");
    check(settings_parse_aspect("16:10", r) && std::fabs(r - 1.6f) < 1e-6f, "16:10 is a ratio of 1.6");
    check(settings_parse_aspect("window", r) && r == 0.0f, "window is a ratio of 0");
    static const char* kBad[] = {"", "16", "16:", ":9", "16:0", "0:9", "-16:9", "16:-9", "+16:9",
                                 "16x9", "16:9:1", "16 :9", "1.5:1", "wide", "Window", "9999999999:1"};
    int refused = 0;
    for (const char* text : kBad) {
        r = 7.0f;
        if (!settings_parse_aspect(text, r) && r == 7.0f) refused++;
        else printf("     \"%s\" was taken\n", text);
    }
    check(refused == (int)(sizeof(kBad) / sizeof(kBad[0])), "16 malformed aspects are refused and leave the ratio alone");
}

void bad_values() {
    const std::string p = path_of("bad.txt");
    write_file(p,
               "aspect 21:9\naspect 16:0\naspect wide\n"
               "fullscreen 1\nfullscreen 2\nfullscreen yes\n"
               "scale 3\nscale 0\nscale -1\nscale two\nscale 2x\nscale\n");
    const Settings s = settings_load(p);
    check(s.aspect == "21:9", "a bad aspect leaves the one before it");
    check(s.fullscreen, "a bad fullscreen leaves the one before it");
    check(s.scale == 3, "a bad scale leaves the one before it");
}

void save_keeps_lines() {
    const std::string p = path_of("keep.txt");
    write_file(p,
               "# Display\n"
               "aspect 4:3\n"
               "vsync on\n"
               "\n"
               "   # the game's own\n"
               "greeting hello there\n"
               "scale 1\r\n"
               "scale 2\n");
    Settings s;
    s.aspect = "16:9";
    s.fullscreen = true;
    s.scale = 3;
    check(settings_save(p, s), "a save over a file succeeds");
    check(read_file(p) ==
              "# Display\n"
              "aspect 16:9\n"
              "vsync on\n"
              "\n"
              "   # the game's own\n"
              "greeting hello there\n"
              "scale 3\r\n"
              "scale 3\n"
              "fullscreen 1\n",
          "it rewrites each owned line in place, keeps every other line and adds the missing key");
    const Settings back = settings_load(p);
    check(back.aspect == "16:9" && back.fullscreen && back.scale == 3, "a load gives back what was saved");
    check(!exists(p + ".tmp"), "a save leaves no temp file behind");
}

void save_refuses_bad_values() {
    const std::string p = path_of("refused.txt");
    const std::string before = "aspect 4:3\nvsync on\n";
    write_file(p, before);
    static const char* kAspects[] = {"", "16x9", "16:9\nvsync off"};
    static const int kScales[] = {0, -1, 1000000000};
    int saved = 0;
    for (const char* aspect : kAspects) {
        Settings s;
        s.aspect = aspect;
        if (settings_save(p, s)) {
            saved++;
            printf("     aspect \"%s\" was saved\n", aspect);
        }
    }
    for (int scale : kScales) {
        Settings s;
        s.scale = scale;
        if (settings_save(p, s)) {
            saved++;
            printf("     scale %d was saved\n", scale);
        }
    }
    check(saved == 0, "a save refuses an empty aspect, 16x9, an aspect with a newline, and a scale of 0, -1 or 10 digits");
    check(read_file(p) == before, "and leaves the old file whole");
    Settings bad;
    bad.scale = 0;
    const std::string q = path_of("refused_new.txt");
    check(!settings_save(q, bad) && !exists(q), "and makes no file where there was none");
    Settings edge;
    edge.aspect = "window";
    edge.scale = 999999999;
    const bool took = settings_save(p, edge);
    const Settings back = settings_load(p);
    check(took && back.aspect == "window" && back.scale == 999999999, "a save takes window and a scale of 9 digits");
}

void save_after_unterminated_line() {
    const std::string p = path_of("unterminated.txt");
    write_file(p, "vsync on");
    check(settings_save(p, Settings{}) && read_file(p) == "vsync on\naspect 4:3\nfullscreen 0\nscale 2\n",
          "a key added after a last line with no newline goes on a line of its own");
}

void save_creates() {
    const std::string p = path_of("new.txt");
    Settings s;
    s.scale = 4;
    check(settings_save(p, s) && read_file(p) == "aspect 4:3\nfullscreen 0\nscale 4\n",
          "a save with no file writes the three owned keys");
}

void failed_save_no_temp() {
    const std::string p = path_of("blocked.txt");
    const std::string before = "aspect 4:3\nvsync on\n";
    write_file(p, before);
    mkdir((p + ".tmp").c_str(), 0755);
    Settings s;
    s.aspect = "16:9";
    check(!settings_save(p, s), "a save fails when a directory holds its temp file's name");
    check(read_file(p) == before, "and leaves the old file whole");
}

struct LimitedSave {
    bool saved, whole, no_temp;
};

// A save over a file of `comment_lines` comment lines and an aspect line, with the size limit
// at 256 bytes. A comment line is 50 bytes, so 40 of them fit in stdio's buffer and 400
// don't.
LimitedSave save_under_size_limit(const char* name, int comment_lines) {
    const std::string p = path_of(name);
    std::string before;
    for (int i = 0; i < comment_lines; i++) before += "# a comment line, filler for the size of the file\n";
    before += "aspect 4:3\n";
    write_file(p, before);
    // Past the limit a write fails with EFBIG, once SIGXFSZ no longer ends the process.
    signal(SIGXFSZ, SIG_IGN);
    rlimit old;
    getrlimit(RLIMIT_FSIZE, &old);
    rlimit limit = old;
    limit.rlim_cur = 256;
    setrlimit(RLIMIT_FSIZE, &limit);
    Settings s;
    s.aspect = "16:9";
    const bool saved = settings_save(p, s);
    setrlimit(RLIMIT_FSIZE, &old);
    // A stderr sent to a file failed under the limit too, and stays failed until cleared.
    clearerr(stderr);
    return {saved, read_file(p) == before, !exists(p + ".tmp")};
}

// The file fits in stdio's buffer, so fwrite takes all of it and the limit bites when fclose
// flushes.
void failed_save_cut_short_at_close() {
    const LimitedSave r = save_under_size_limit("full.txt", 40);
    check(!r.saved, "a save fails when only 256 bytes of its 2 KB file can be written");
    check(r.whole, "and leaves the old file whole");
    check(r.no_temp, "and removes the part it wrote");
}

// The file is bigger than stdio's buffer, so fwrite itself comes back short.
void failed_save_cut_short_at_write() {
    const LimitedSave r = save_under_size_limit("full_big.txt", 400);
    check(!r.saved, "a save fails when only 256 bytes of its 20 KB file can be written");
    check(r.whole, "and leaves the old file whole");
    check(r.no_temp, "and removes the part it wrote");
}

void failed_save_unreadable() {
    if (geteuid() == 0) {
        printf("skip an unreadable file, since root reads it anyway\n");
        return;
    }
    const std::string p = path_of("locked.txt");
    const std::string before = "aspect 4:3\nvsync on\n";
    write_file(p, before);
    chmod(p.c_str(), 0);
    const bool saved = settings_save(p, Settings{});
    chmod(p.c_str(), 0644);
    check(!saved, "a save fails when it can't read the old file, rather than drop its lines");
    check(read_file(p) == before, "and leaves the old file whole");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_settings <work dir>\n");
        return 2;
    }
    g_dir = argv[1];
    if (exists(g_dir) || mkdir(g_dir.c_str(), 0755) != 0) {
        fprintf(stderr, "%s exists or can't be made: give a new directory\n", g_dir.c_str());
        return 2;
    }
    missing_file();
    parsing();
    byte_order_mark();
    aspects();
    bad_values();
    save_keeps_lines();
    save_refuses_bad_values();
    save_after_unterminated_line();
    save_creates();
    failed_save_no_temp();
    failed_save_cut_short_at_close();
    failed_save_cut_short_at_write();
    failed_save_unreadable();
    printf("test_settings: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
