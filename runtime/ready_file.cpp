// See ready_file.h.
#include "ready_file.h"
#include "env.h"
#include "platform.h"
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace {

// A word ready_mark has been called with.
struct Word {
    bool in = false;             // its line is in
    bool said = false;           // a write of it has failed and said so
    long long next = LLONG_MIN;  // after a failed write, the time it's tried again from
};
std::map<std::string, Word> g_words;

// `text` and then the word's line, through `path`.tmp.
bool write_line(const char* path, std::string text, const char* word, long long ms, bool say) {
    char line[64];
    snprintf(line, sizeof(line), "%s %lld\n", word, ms);
    text += line;
    return plat_replace_whole_file(path, text.data(), text.size(), "the ready line", say);
}

}  // namespace

bool ready_file_append(const char* path, const char* word, long long ms, bool say) {
    std::string text;
    if (FILE* f = fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
    }
    return write_line(path, std::move(text), word, ms, say);
}

bool ready_file_start(const char* path, const char* word, long long ms, bool say) {
    return write_line(path, "", word, ms, say);
}

void ready_mark(const char* word) { ready_mark_at(word, (long long)plat_ms_since_start()); }

void ready_mark_at(const char* word, long long ms) {
    const char* path = getenv_nonempty("GCN_READY_FILE");
    if (!path) return;
    Word& w = g_words[word];
    // A path that never works costs a try a second, not two a frame.
    if (w.in || ms < w.next) return;
    // frame waits for window, so a window that failed once doesn't come after it.
    if (std::string(word) == "frame" && !g_words["window"].in) return;
    bool first = true;
    for (const auto& [name, other] : g_words) first = first && !other.in;
    // The run's first line replaces what an earlier run left.
    const bool wrote = first ? ready_file_start(path, word, ms, !w.said) : ready_file_append(path, word, ms, !w.said);
    if (wrote) {
        w.in = true;
    } else {
        w.said = true;
        w.next = ms + 1000;
    }
}

void ready_relaunching() {
    const auto frame = g_words.find("frame");
    if (frame == g_words.end() || !frame->second.in) return;
#ifdef _WIN32
    _putenv_s("GCN_READY_FILE", "");
#else
    unsetenv("GCN_READY_FILE");
#endif
}
