// See ready_file.h.
#include "ready_file.h"
#include "env.h"
#include "platform.h"
#include <cstdio>
#include <set>
#include <string>

bool ready_file_append(const char* path, const char* word, long long ms) {
    std::string text;
    if (FILE* f = fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
    }
    char line[64];
    snprintf(line, sizeof(line), "%s %lld\n", word, ms);
    text += line;
    return plat_save_file(path, text.data(), text.size(), "the ready line");
}

void ready_mark(const char* word) {
    static std::set<std::string> s_done;
    const char* path = getenv_nonempty("GCN_READY_FILE");
    if (!path || !s_done.insert(word).second) return;
    ready_file_append(path, word, (long long)plat_ms_since_start());
}
