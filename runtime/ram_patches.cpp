#include "ram_patches.h"
#include "runtime.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
std::string patch_file;
constexpr size_t kMaxPatchList = 4u * RAM_SIZE + 4096u;

[[noreturn]] void refuse(size_t line, const char* reason) {
    fatal("%s:%zu: %s", patch_file.c_str(), line, reason);
}

int digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::vector<uint8_t> bytes(const std::string& s, size_t line) {
    if (s.size() % 2) refuse(line, "a byte string needs an even number of hex digits.");
    std::vector<uint8_t> result;
    result.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = digit(s[i]), lo = digit(s[i + 1]);
        if (hi < 0 || lo < 0) refuse(line, "a byte string contains a non-hex digit.");
        result.push_back(uint8_t((hi << 4) | lo));
    }
    return result;
}

std::string hex(const uint8_t* data, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; i++) {
        result += digits[data[i] >> 4];
        result += digits[data[i] & 15];
    }
    return result;
}

void inside_ram(uint32_t addr, size_t size, size_t line) {
    if (addr < 0x80000000u || uint64_t(addr) + size > 0x80000000ull + RAM_SIZE)
        refuse(line, "the patch reaches outside main RAM.");
}

std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> result;
    size_t at = 0;
    while (true) {
        size_t space = line.find(' ', at);
        result.push_back(line.substr(at, space == std::string::npos ? space : space - at));
        if (space == std::string::npos) return result;
        at = space + 1;
    }
}
} // namespace

std::string ram_patches_choose(const char* variable, int argc, char** argv) {
    std::string result = variable ? variable : "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ram-patches"))
            fatal("--ram-patches: the RAM patch list's path goes after an =, as in --ram-patches=FILE.");
        if (!strncmp(argv[i], "--ram-patches=", 14)) result = argv[i] + 14;
    }
    return result;
}

void ram_patches_set_file(const std::string& path) { patch_file = path; }

void ram_patches_apply(const char* disc_id) {
    if (patch_file.empty()) return;
    std::ifstream input(patch_file, std::ios::binary);
    if (!input) fatal("%s:1: the RAM patch list can't be opened.", patch_file.c_str());
    std::string text;
    char buffer[8192];
    while (input && text.size() <= kMaxPatchList) {
        input.read(buffer, sizeof buffer);
        text.append(buffer, size_t(input.gcount()));
    }
    if (input.bad()) refuse(1, "the RAM patch list can't be read.");
    if (text.size() > kMaxPatchList) refuse(1, "the file is bigger than the RAM patch list limit.");
    if (text.empty() || text.back() != '\n')
        refuse(1 + size_t(std::count(text.begin(), text.end(), '\n')), "the last line needs a newline, since it may be cut short.");
    std::istringstream input_lines(text);
    std::string line;
    size_t number = 0;
    auto next = [&]() {
        if (!std::getline(input_lines, line)) return false;
        number++;
        for (unsigned char c : line)
            if (c < 0x20 || c == 0x7f)
                refuse(number, "the line has a control character, such as a tab or the CR of a Windows line ending.");
        return true;
    };
    if (!next() || line != "gcn-ram-patches 1") refuse(1, "expected gcn-ram-patches 1.");
    if (!next()) refuse(2, "expected base followed by the disc ID.");
    {
        const auto f = fields(line);
        if (f.size() != 2 || f[0] != "base" || f[1].size() != 6)
            refuse(2, "expected base followed by the six character disc ID.");
        if (memcmp(f[1].data(), disc_id, 6)) refuse(2, "the base isn't the disc's ID.");
    }
    size_t count = 0, written = 0;
    while (next()) {
        const auto f = fields(line);
        if ((f.size() != 3 && f.size() != 4) || f[0] != "patch" ||
            f[1].empty() || f[2].empty() || (f.size() == 4 && f[3].empty()))
            refuse(number, "expected patch, address, bytes and optional original bytes.");
        const std::string& address = f[1];
        if (address.size() != 8) refuse(number, "the address needs eight hex digits.");
        uint32_t addr = 0;
        for (char c : address) {
            int n = digit(c);
            if (n < 0) refuse(number, "the address contains a non-hex digit.");
            addr = (addr << 4) | uint32_t(n);
        }
        const std::vector<uint8_t> value_bytes = bytes(f[2], number);
        const std::vector<uint8_t> original_bytes = f.size() == 4 ? bytes(f[3], number) : std::vector<uint8_t>();
        inside_ram(addr, value_bytes.size(), number);
        if (!original_bytes.empty()) inside_ram(addr, original_bytes.size(), number);
        uint8_t* destination = mem_ptr(addr);
        if (!original_bytes.empty() && memcmp(destination, original_bytes.data(), original_bytes.size())) {
            size_t first = 0;
            while (destination[first] == original_bytes[first]) first++;
            const size_t shown = std::min(size_t(16), original_bytes.size() - first);
            const std::string held = hex(destination + first, shown);
            const std::string expected = hex(original_bytes.data() + first, shown);
            fatal("%s:%zu: at %08x RAM holds %s, the patch expects %s", patch_file.c_str(), number,
                  addr + uint32_t(first), held.c_str(), expected.c_str());
        }
        memcpy(destination, value_bytes.data(), value_bytes.size());
        gcn_code_written(addr, uint32_t(value_bytes.size()));
        count++;
        written += value_bytes.size();
    }
    LOG(LOG_OS, "RAM patches: %zu patch%s, %zu bytes written", count, count == 1 ? "" : "es", written);
}
