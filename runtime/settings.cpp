// See settings.h.
#include "settings.h"
#include "platform.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

constexpr int kMaxScale = 999999999;  // nine digits, which atoi reads without overflow
constexpr int kMaxAspectSide = 9999;  // four, which --aspect takes (gx::parse_aspect)

// A whole number from 1 to `max`, in digits alone and no more of them than `max` has. That keeps
// it from overflowing, and keeps a zero-padded side as short as --aspect would take it.
bool parse_positive(const std::string& text, int max, int& out) {
    if (text.empty() || text.size() > std::to_string(max).size() ||
        text.find_first_not_of("0123456789") != std::string::npos)
        return false;
    const int v = atoi(text.c_str());
    if (v < 1 || v > max) return false;
    out = v;
    return true;
}

constexpr const char* kByteOrderMark = "\xEF\xBB\xBF";

// The file's lines, without their newlines. `found` says whether it exists, and `bom` whether it
// starts with a UTF-8 byte order mark, which isn't part of the first line. `crlf` says whether
// its first newline follows a CR, and `open` whether its last line has no newline. False if
// the file does exist and can't be read.
bool read_lines(const std::string& path, std::vector<std::string>& lines, bool& found, bool& bom, bool& crlf,
                bool& open) {
    bom = crlf = open = false;
    FILE* f = fopen(path.c_str(), "rb");
    found = f != nullptr;
    if (!f) return errno == ENOENT;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    const bool ok = !ferror(f);
    fclose(f);
    // An editor may put a UTF-8 byte order mark first, which isn't part of the first key.
    bom = text.compare(0, 3, kByteOrderMark) == 0;
    if (bom) text.erase(0, 3);
    const size_t first = text.find('\n');
    crlf = first != std::string::npos && first > 0 && text[first - 1] == '\r';
    open = !text.empty() && text.back() != '\n';
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
    if (colon == std::string::npos || !parse_positive(text.substr(0, colon), kMaxAspectSide, w) ||
        !parse_positive(text.substr(colon + 1), kMaxAspectSide, h))
        return false;
    ratio = (float)w / (float)h;
    return true;
}

bool settings_parse_disc(const std::string& text, bool& fast) {
    if (text != "fast" && text != "console") return false;
    fast = text == "fast";
    return true;
}

Settings settings_load(const std::string& path) {
    Settings s;
    std::vector<std::string> lines;
    bool found, bom, crlf, open;
    if (!read_lines(path, lines, found, bom, crlf, open)) {
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
            else takes = "W:H with two whole numbers from 1 to 9999, or window";
        } else if (key == "fullscreen") {
            if (value == "0" || value == "1") s.fullscreen = value == "1";
            else takes = "0 or 1";
        } else if (key == "scale") {
            if (!parse_positive(value, kMaxScale, s.scale)) takes = "a whole number from 1 up";
        } else if (key == "disc") {
            if (!settings_parse_disc(value, s.fast_disc)) takes = "fast or console";
        }
        if (takes)
            fprintf(stderr, "%s:%d: %s takes %s, not \"%s\", so the line is skipped\n", path.c_str(), n,
                    key.c_str(), takes, value.c_str());
    }
    if (found)
        fprintf(stderr, "%s: aspect %s, fullscreen %d, scale %d, disc %s\n", path.c_str(), s.aspect.c_str(),
                (int)s.fullscreen, s.scale, s.fast_disc ? "fast" : "console");
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
    if (!parse_positive(std::to_string(s.scale), kMaxScale, scale)) {
        fprintf(stderr, "%s: scale %d isn't one a load takes, so nothing is saved\n", path.c_str(), s.scale);
        return false;
    }
    std::vector<std::string> lines;
    bool found, bom, crlf, open;
    if (!read_lines(path, lines, found, bom, crlf, open)) {
        fprintf(stderr, "%s: can't read it, so it isn't saved over\n", path.c_str());
        return false;
    }
    const std::pair<const char*, std::string> owned[] = {{"aspect", s.aspect},
                                                         {"fullscreen", s.fullscreen ? "1" : "0"},
                                                         {"scale", std::to_string(s.scale)},
                                                         {"disc", s.fast_disc ? "fast" : "console"}};
    constexpr int kOwned = (int)(sizeof(owned) / sizeof(owned[0]));
    bool written[kOwned] = {};
    std::string text = bom ? kByteOrderMark : "";
    // A key added to the file, and a last line it left without a newline, take the line
    // ending of its first newline. Windows Notepad writes CR LF with none after the last line.
    const char* newline = crlf ? "\r\n" : "\n";
    for (size_t n = 0; n < lines.size(); n++) {
        const std::string& line = lines[n];
        std::string key, value, out = line;
        if (split(line, key, value)) {
            for (int i = 0; i < kOwned; i++) {
                if (key != owned[i].first) continue;
                out = key + " " + owned[i].second + (line.back() == '\r' ? "\r" : "");
                written[i] = true;
            }
        }
        text += out + (open && n + 1 == lines.size() && line.back() != '\r' ? newline : "\n");
    }
    for (int i = 0; i < kOwned; i++)
        if (!written[i]) text += std::string(owned[i].first) + " " + owned[i].second + newline;

    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "%s: can't write it (%s), so %s isn't saved\n", tmp.c_str(), strerror(errno), path.c_str());
        return false;
    }
    const bool wrote = fwrite(text.data(), 1, text.size(), f) == text.size();
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
