// See port_memory.h.
#include "port_memory.h"
#include "platform.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

// Whether the saved device is `d`, down to the GUID.
bool same_gamepad(const PortDevice& saved, const PortDevice& d) { return saved.guid == d.guid && saved.serial == d.serial; }

// Whether the saved device is `d` on another connection, going by its serial and USB IDs.
bool same_gamepad_elsewhere(const PortDevice& saved, const PortDevice& d) {
    return !d.serial.empty() && saved.serial == d.serial && saved.vendor == d.vendor && saved.product == d.product;
}

// Whether any saved port names `d`.
bool is_saved(const SavedPorts& saved, const PortDevice& d) {
    for (const auto& s : saved)
        if (s && (same_gamepad(*s, d) || same_gamepad_elsewhere(*s, d))) return true;
    return false;
}

// ---- The file's text

bool plain(unsigned char c) { return c > 0x20 && c != 0x7F && c != '%'; }

// A serial as one token. `-` stands for none, and a percent escape stands for any byte a
// token can't hold. A serial that's only `-` is escaped too, so that it can't be taken for
// none.
std::string encode_serial(const std::string& serial) {
    if (serial.empty()) return "-";
    if (serial == "-") return "%2D";
    std::string out;
    for (unsigned char c : serial) {
        if (plain(c)) {
            out += (char)c;
        } else {
            char esc[4];
            snprintf(esc, sizeof(esc), "%%%02X", c);
            out += esc;
        }
    }
    return out;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool decode_serial(const std::string& token, std::string& out) {
    out.clear();
    if (token == "-") return true;
    for (size_t i = 0; i < token.size(); i++) {
        if (token[i] != '%') {
            out += token[i];
            continue;
        }
        const int hi = i + 2 < token.size() ? hex_digit(token[i + 1]) : -1;
        const int lo = i + 2 < token.size() ? hex_digit(token[i + 2]) : -1;
        if (hi < 0 || lo < 0) return false;
        out += (char)(hi * 16 + lo);
        i += 2;
    }
    return true;
}

// A USB ID, one to four hex digits.
bool parse_id(const std::string& text, uint16_t& out) {
    if (text.empty() || text.size() > 4) return false;
    unsigned v = 0;
    for (char c : text) {
        const int d = hex_digit(c);
        if (d < 0) return false;
        v = v * 16 + (unsigned)d;
    }
    out = (uint16_t)v;
    return true;
}

bool parse_guid(const std::string& text) {
    if (text.size() != 32) return false;
    for (char c : text)
        if (hex_digit(c) < 0) return false;
    return true;
}

std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        const size_t start = line.find_first_not_of(" \t\r", i);
        if (start == std::string::npos) break;
        const size_t end = line.find_first_of(" \t\r", start);
        out.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        i = end;
    }
    return out;
}

// One line's port (0 to 3) and its device, none for `none`. False if it isn't a port line.
bool parse_line(const std::vector<std::string>& t, int& port, std::optional<PortDevice>& device) {
    if (t.empty() || t[0].size() != 1 || t[0][0] < '1' || t[0][0] > '4') return false;
    port = t[0][0] - '1';
    if (t.size() == 2 && t[1] == "none") {
        device.reset();
        return true;
    }
    if (t.size() != 4 || !parse_guid(t[1])) return false;
    const size_t colon = t[3].find(':');
    PortDevice d;
    if (colon == std::string::npos || !parse_id(t[3].substr(0, colon), d.vendor) ||
        !parse_id(t[3].substr(colon + 1), d.product) || !decode_serial(t[2], d.serial))
        return false;
    d.guid = t[1];
    device = d;
    return true;
}

}  // namespace

std::vector<int> match_ports(const SavedPorts& saved, const std::vector<PresentDevice>& present,
                             const std::array<bool, 4>& pinned) {
    std::vector<int> port(present.size());
    std::array<bool, 4> taken = pinned;
    for (size_t i = 0; i < present.size(); i++) {
        port[i] = present[i].held;
        if (port[i] >= 0) taken[port[i]] = true;
    }

    // Steps 1 and 2. Each saved port, lowest first, takes the first device to connect that fits.
    auto claim = [&](bool (*fits)(const PortDevice&, const PortDevice&)) {
        for (int p = 0; p < 4; p++) {
            if (!saved[p] || taken[p]) continue;
            for (size_t i = 0; i < present.size(); i++) {
                if (port[i] >= 0 || !fits(*saved[p], present[i].id)) continue;
                port[i] = p;
                taken[p] = true;
                break;
            }
        }
    };
    claim(same_gamepad);
    claim(same_gamepad_elsewhere);

    // Steps 3 and 4, in the order the devices connected. A device a saved port names has found
    // that port taken or pinned. It goes to the lowest free port, not to its player index's.
    for (size_t i = 0; i < present.size(); i++) {
        if (port[i] >= 0) continue;
        const int index = present[i].index;
        if (!is_saved(saved, present[i].id) && index >= 0 && index < 4 && !taken[index]) {
            port[i] = index;
        } else {
            for (int p = 0; p < 4 && port[i] < 0; p++)
                if (!taken[p]) port[i] = p;
        }
        if (port[i] >= 0) taken[port[i]] = true;
    }
    return port;
}

bool ports_assign(SavedPorts& saved, int port, const PortDevice& device) {
    if (saved[port] && *saved[port] == device) return false;
    saved[port] = device;
    return true;
}

std::string ports_format(const SavedPorts& saved) {
    std::string text = "# The gamepad last on each port. The runtime writes this file.\n";
    for (int p = 0; p < 4; p++) {
        text += std::to_string(p + 1) + " ";
        if (!saved[p]) {
            text += "none\n";
            continue;
        }
        char ids[16];
        snprintf(ids, sizeof(ids), "%04x:%04x", saved[p]->vendor, saved[p]->product);
        text += saved[p]->guid + " " + encode_serial(saved[p]->serial) + " " + ids + "\n";
    }
    return text;
}

SavedPorts ports_parse(const std::string& text, const char* name) {
    SavedPorts saved;
    int n = 0;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::vector<std::string> t = tokens(text.substr(start, end - start));
        start = end + 1;
        n++;
        if (t.empty() || t[0][0] == '#') continue;
        int port;
        std::optional<PortDevice> device;
        if (parse_line(t, port, device)) saved[port] = device;
        else fprintf(stderr, "%s:%d: not a port with none or a gamepad, so the line is skipped\n", name, n);
    }
    return saved;
}

SavedPorts ports_load(const std::string& path, bool& readable) {
    readable = true;
    auto refuse = [&](int err) {
        readable = false;
        fprintf(stderr, "%s: can't read it (%s), so no ports are remembered and it isn't saved over\n", path.c_str(),
                strerror(err));
        return SavedPorts{};
    };
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return errno == ENOENT ? SavedPorts{} : refuse(errno);
    std::string text;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    const bool ok = !ferror(f);
    const int err = errno;
    fclose(f);
    if (!ok) return refuse(err);
    return ports_parse(text, path.c_str());
}

bool ports_save(const std::string& path, const SavedPorts& saved) {
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos && slash > 0) plat_make_dirs(path.substr(0, slash));
    const std::string text = ports_format(saved);
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
