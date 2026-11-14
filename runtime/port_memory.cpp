// See port_memory.h.
#include "port_memory.h"
#include "platform.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

// What a word can't hold: a space, a control character, and the '%' that escapes them.
bool not_plain(unsigned char c) { return c <= 0x20 || c == 0x7F || c == '%'; }

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string percent_encode(const std::string& text, bool (*escaped)(unsigned char)) {
    std::string out;
    for (unsigned char c : text) {
        if (!escaped(c)) {
            out += (char)c;
        } else {
            char esc[4];
            snprintf(esc, sizeof(esc), "%%%02X", c);
            out += esc;
        }
    }
    return out;
}

bool percent_decode(const std::string& text, std::string& out) {
    out.clear();
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != '%') {
            out += text[i];
            continue;
        }
        const int hi = i + 2 < text.size() ? hex_digit(text[i + 1]) : -1;
        const int lo = i + 2 < text.size() ? hex_digit(text[i + 2]) : -1;
        if (hi < 0 || lo < 0) return false;
        out += (char)(hi * 16 + lo);
        i += 2;
    }
    return true;
}

// A serial as one word. `-` stands for none, and a percent escape stands for any byte a word
// can't hold. A serial that's only `-` is escaped too, so that it can't be taken for none.
std::string ports_encode_serial(const std::string& serial) {
    if (serial.empty()) return "-";
    if (serial == "-") return "%2D";
    return percent_encode(serial, not_plain);
}

bool ports_decode_serial(const std::string& word, std::string& out) {
    out.clear();
    return word == "-" || percent_decode(word, out);
}

bool ports_is_guid(const std::string& text) {
    if (text.size() != 32) return false;
    for (char c : text)
        if (hex_digit(c) < 0) return false;
    return true;
}

std::vector<std::string> ports_words(const std::string& line) {
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

namespace {

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

// One line's port (0 to 3) and its device, none for `none`. False if it isn't a port line.
bool parse_line(const std::vector<std::string>& t, int& port, std::optional<PortDevice>& device) {
    if (t.empty() || t[0].size() != 1 || t[0][0] < '1' || t[0][0] > '4') return false;
    port = t[0][0] - '1';
    if (t.size() == 2 && t[1] == "none") {
        device.reset();
        return true;
    }
    if (t.size() != 4 || !ports_is_guid(t[1])) return false;
    const size_t colon = t[3].find(':');
    PortDevice d;
    if (colon == std::string::npos || !parse_id(t[3].substr(0, colon), d.vendor) ||
        !parse_id(t[3].substr(colon + 1), d.product) || !ports_decode_serial(t[2], d.serial))
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
        text += saved[p]->guid + " " + ports_encode_serial(saved[p]->serial) + " " + ids + "\n";
    }
    return text;
}

SavedPorts ports_parse(const std::string& text, const char* name) {
    SavedPorts saved;
    int n = 0;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::vector<std::string> t = ports_words(text.substr(start, end - start));
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
    std::string text;
    int err = 0;
    readable = plat_read_file(path, text, err) || err == ENOENT;
    if (!readable)
        fprintf(stderr, "%s: can't read it (%s), so no ports are remembered and it isn't saved over\n", path.c_str(),
                strerror(err));
    return readable ? ports_parse(text, path.c_str()) : SavedPorts{};
}

bool ports_save(const std::string& path, const SavedPorts& saved) {
    const std::string text = ports_format(saved);
    return plat_save_file_making_dirs(path, text.data(), text.size(), path.c_str());
}
