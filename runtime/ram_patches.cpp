#include "ram_patches.h"
#include "runtime.h"
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
std::string patch_file;

[[noreturn]] void refuse(size_t line, const char* reason) {
    fatal("%s:%zu: %s", patch_file.c_str(), line, reason);
}

int digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<uint8_t> bytes(const std::string& s, size_t line) {
    if (s.empty() || s.size() % 2) refuse(line, "a byte string needs an even number of hex digits");
    std::vector<uint8_t> result;
    result.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = digit(s[i]), lo = digit(s[i + 1]);
        if (hi < 0 || lo < 0) refuse(line, "a byte string contains a non-hex digit");
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
    if (addr < 0x80000000u || size > RAM_SIZE || uint64_t(addr) + size > 0x80000000ull + RAM_SIZE)
        refuse(line, "the patch reaches outside main RAM");
}
} // namespace

std::string ram_patches_choose(const char* variable, int argc, char** argv) {
    std::string result = variable ? variable : "";
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--ram-patches=", 14)) result = argv[i] + 14;
    return result;
}

void ram_patches_set_file(const std::string& path) { patch_file = path; }

void ram_patches_apply(const char* disc_id) {
    if (patch_file.empty()) return;
    std::ifstream input(patch_file);
    if (!input) fatal("%s:1: can't open the RAM patch file", patch_file.c_str());
    std::string line;
    if (!std::getline(input, line) || line != "gcn-ram-patches 1") refuse(1, "expected gcn-ram-patches 1");
    if (!std::getline(input, line)) refuse(2, "expected base followed by the disc ID");
    {
        std::istringstream words(line);
        std::string key, id, extra;
        if (!(words >> key >> id) || key != "base" || (words >> extra) || id.size() != 6)
            refuse(2, "expected base followed by the six character disc ID");
        if (id.compare(0, 6, disc_id, 6)) refuse(2, "the base isn't the disc's ID");
    }
    size_t count = 0, written = 0, number = 2;
    while (std::getline(input, line)) {
        number++;
        std::istringstream words(line);
        std::string key, address, value, original, extra;
        if (!(words >> key >> address >> value) || key != "patch" || (words >> original && (words >> extra)))
            refuse(number, "expected patch, address, bytes and optional original bytes");
        if (address.size() != 8) refuse(number, "the address needs eight hex digits");
        uint32_t addr = 0;
        for (char c : address) {
            int n = digit(c);
            if (n < 0) refuse(number, "the address contains a non-hex digit");
            addr = (addr << 4) | uint32_t(n);
        }
        const std::vector<uint8_t> value_bytes = bytes(value, number);
        const std::vector<uint8_t> original_bytes = original.empty() ? std::vector<uint8_t>() : bytes(original, number);
        inside_ram(addr, value_bytes.size(), number);
        if (!original_bytes.empty()) inside_ram(addr, original_bytes.size(), number);
        uint8_t* destination = mem_ptr(addr);
        if (!original_bytes.empty() && memcmp(destination, original_bytes.data(), original_bytes.size())) {
            const std::string held = hex(destination, original_bytes.size());
            const std::string expected = hex(original_bytes.data(), original_bytes.size());
            fatal("%s:%zu: at %08x RAM holds %s, the patch expects %s", patch_file.c_str(), number,
                  addr, held.c_str(), expected.c_str());
        }
        memcpy(destination, value_bytes.data(), value_bytes.size());
        gcn_code_written(addr, uint32_t(value_bytes.size()));
        count++;
        written += value_bytes.size();
    }
    if (input.bad()) fatal("%s:%zu: can't read the RAM patch file", patch_file.c_str(), number + 1);
    LOG(LOG_OS, "RAM patches: %zu patches, %zu bytes written", count, written);
}
