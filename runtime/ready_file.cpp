// See ready_file.h.
#include "ready_file.h"
#include "env.h"
#include "platform.h"
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

namespace {

std::set<std::string> g_done;  // the words whose lines are in

// `text` and then the word's line, through `path`.tmp.
bool write_line(const char* path, std::string text, const char* word, long long ms) {
    char line[64];
    snprintf(line, sizeof(line), "%s %lld\n", word, ms);
    text += line;
    return plat_replace_whole_file(path, text.data(), text.size(), "the ready line");
}

}  // namespace

bool ready_file_append(const char* path, const char* word, long long ms) {
    std::string text;
    if (FILE* f = fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
    }
    return write_line(path, std::move(text), word, ms);
}

bool ready_file_start(const char* path, const char* word, long long ms) {
    return write_line(path, "", word, ms);
}

void ready_mark(const char* word) {
    const char* path = getenv_nonempty("GCN_READY_FILE");
    if (!path || g_done.count(word)) return;
    const long long ms = (long long)plat_ms_since_start();
    // The run's first line replaces what an earlier run left.
    const bool wrote = g_done.empty() ? ready_file_start(path, word, ms) : ready_file_append(path, word, ms);
    if (wrote) g_done.insert(word);
}

void ready_relaunching() {
    if (!g_done.count("frame")) return;
#ifdef _WIN32
    _putenv_s("GCN_READY_FILE", "");
#else
    unsetenv("GCN_READY_FILE");
#endif
}
