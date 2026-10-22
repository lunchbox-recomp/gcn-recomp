// See settings.h.
#include "settings.h"
#include "platform.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace {

// `text` without the spaces, tabs and CR around it.
std::string trimmed(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return "";
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

// A line's key and value. False for a blank line or a comment.
bool split(const std::string& line, std::string& key, std::string& value) {
    const std::string t = trimmed(line);
    if (t.empty() || t[0] == '#') return false;
    const size_t end = t.find_first_of(" \t");
    key = t.substr(0, end);
    value = end == std::string::npos ? "" : trimmed(t.substr(end));
    return true;
}

// A whole number from 1 up, in digits alone and no more than nine of them, so that it fits.
bool parse_positive(const std::string& text, int& out) {
    if (text.empty() || text.size() > 9 || text.find_first_not_of("0123456789") != std::string::npos) return false;
    const int v = atoi(text.c_str());
    if (v < 1) return false;
    out = v;
    return true;
}

// The file's lines, without their newlines. `found` says whether it exists; false if it
// does and can't be read.
bool read_lines(const std::string& path, std::vector<std::string>& lines, bool& found) {
    FILE* f = fopen(path.c_str(), "rb");
    found = f != nullptr;
    if (!f) return errno == ENOENT;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    const bool ok = !ferror(f);
    fclose(f);
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return ok;
}

}  // namespace

bool settings_parse_aspect(const std::string& text, float& ratio) {
    if (text == "window") {
        ratio = 0.0f;
        return true;
    }
    const size_t colon = text.find(':');
    int w, h;
    if (colon == std::string::npos || !parse_positive(text.substr(0, colon), w) ||
        !parse_positive(text.substr(colon + 1), h))
        return false;
    ratio = (float)w / (float)h;
    return true;
}

Settings settings_load(const std::string& path) {
    Settings s;
    std::vector<std::string> lines;
    bool found;
    if (!read_lines(path, lines, found)) {
        fprintf(stderr, "%s: can't read it, so the defaults are used\n", path.c_str());
        return s;
    }
    int n = 0;
    for (const std::string& line : lines) {
        n++;
        std::string key, value;
        if (!split(line, key, value)) continue;
        const char* takes = nullptr;
        float ratio;
        if (key == "aspect") {
            if (settings_parse_aspect(value, ratio)) s.aspect = value;
            else takes = "W:H with two positive whole numbers, or window";
        } else if (key == "fullscreen") {
            if (value == "0" || value == "1") s.fullscreen = value == "1";
            else takes = "0 or 1";
        } else if (key == "scale") {
            if (!parse_positive(value, s.scale)) takes = "a whole number from 1 up";
        }
        if (takes)
            fprintf(stderr, "%s:%d: %s takes %s, not \"%s\", so the line is skipped\n", path.c_str(), n,
                    key.c_str(), takes, value.c_str());
    }
    if (found)
        fprintf(stderr, "%s: aspect %s, fullscreen %d, scale %d\n", path.c_str(), s.aspect.c_str(),
                (int)s.fullscreen, s.scale);
    return s;
}

bool settings_save(const std::string& path, const Settings& s) {
    // Only what a load takes, which also keeps a newline out of the file.
    float ratio;
    int scale;
    if (!settings_parse_aspect(s.aspect, ratio)) {
        fprintf(stderr, "%s: aspect \"%s\" isn't one a load takes, so nothing is saved\n", path.c_str(),
                s.aspect.c_str());
        return false;
    }
    if (!parse_positive(std::to_string(s.scale), scale)) {
        fprintf(stderr, "%s: scale %d isn't one a load takes, so nothing is saved\n", path.c_str(), s.scale);
        return false;
    }
    std::vector<std::string> lines;
    bool found;
    if (!read_lines(path, lines, found)) {
        fprintf(stderr, "%s: can't read it, so it isn't saved over\n", path.c_str());
        return false;
    }
    const std::pair<const char*, std::string> owned[] = {
        {"aspect", s.aspect}, {"fullscreen", s.fullscreen ? "1" : "0"}, {"scale", std::to_string(s.scale)}};
    bool written[3] = {};
    std::string text;
    for (const std::string& line : lines) {
        std::string key, value, out = line;
        if (split(line, key, value)) {
            for (int i = 0; i < 3; i++) {
                if (key != owned[i].first) continue;
                out = key + " " + owned[i].second + (line.back() == '\r' ? "\r" : "");
                written[i] = true;
            }
        }
        text += out + "\n";
    }
    for (int i = 0; i < 3; i++)
        if (!written[i]) text += std::string(owned[i].first) + " " + owned[i].second + "\n";

    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "%s: can't write it (%s), so %s isn't saved\n", tmp.c_str(), strerror(errno), path.c_str());
        return false;
    }
    const bool wrote = fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = fclose(f) == 0;
    if (!wrote || !closed || !plat_replace_file(tmp.c_str(), path.c_str())) {
        fprintf(stderr, "%s: the save failed (%s), and the old file is as it was\n", path.c_str(), strerror(errno));
        remove(tmp.c_str());
        return false;
    }
    return true;
}
