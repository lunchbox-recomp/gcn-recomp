// Which recompiled functions run interpreted. A function's bit in g_code_changed_bits sends its guard
// to interp_enter (runtime/recomp.h), and it's set while any of the function's marks is.
// GCN_INTERPRET forces functions, and GCN_INTERPRET_STATS reports what each ran interpreted.
#include "interp.h"
#include "platform.h"
#include "runtime.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

namespace {

std::vector<uint8_t>& marks() {
    static std::vector<uint8_t> m(g_recomp_range_count);
    return m;
}

// Interpreted instructions by function, with loose frames' last.
std::vector<std::atomic<uint64_t>>& counts() {
    static std::vector<std::atomic<uint64_t>> v(g_recomp_range_count + 1);
    return v;
}

// Hex with or without 0x, and nothing else.
bool parse_hex(const char* s, uint32_t* out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    const size_t n = strspn(s, "0123456789abcdefABCDEF");
    if (!n || n > 8 || s[n]) return false;
    *out = (uint32_t)strtoul(s, nullptr, 16);
    return true;
}

// Forces the starts f lists, with what the line printed says in *said. Each line that isn't a
// recompiled function's start goes to stderr, and it returns how many of those there were.
//
// An HLE'd function's start is guarded, and only its original body runs interpreted. A module's has
// no guard, so it's taken and left native.
unsigned force_file(FILE* f, const char* spec, std::string* said) {
    std::set<uint32_t> listed, forced;
    unsigned bad = 0;
    char line[1024];
    for (unsigned no = 1; fgets(line, sizeof line, f); no++) {
        if (char* hash = strchr(line, '#')) *hash = 0;
        char* s = line;
        while (isspace((unsigned char)*s)) s++;
        char* e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
        if (!*s) continue;
        uint32_t addr, n;
        if (!parse_hex(s, &addr) || !code_table_find(addr, &n) || g_recomp_ranges[n].start != addr) {
            fprintf(stderr, "GCN_INTERPRET: line %u of %s is \"%s\", which isn't a recompiled function's start\n", no,
                    spec, s);
            bad++;
            continue;
        }
        listed.insert(addr);
        if (g_recomp_ranges[n].flags & RECOMP_RANGE_NO_GUARD) continue;
        code_table_mark(n, CODE_FORCED, true);
        forced.insert(n);
    }
    *said = "[interp] GCN_INTERPRET=" + std::string(spec) + ": forced " + std::to_string(forced.size()) + " of the " +
            std::to_string(listed.size()) + " starts listed";
    if (forced.size() != listed.size()) *said += " (" + std::to_string(listed.size() - forced.size()) + " with no guard)";
    return bad;
}

}  // namespace

void code_table_mark(uint32_t n, CodeMark mark, bool on) {
    uint8_t& m = marks()[n];
    m = on ? (uint8_t)(m | mark) : (uint8_t)(m & ~mark);
    const uint8_t bit = (uint8_t)(1u << (n & 7));
    if (m) g_code_changed_bits[n >> 3] |= bit;
    else g_code_changed_bits[n >> 3] &= (uint8_t)~bit;
}

bool code_table_find(uint32_t addr, uint32_t* n) {
    const RecompRange* end = g_recomp_ranges + g_recomp_range_count;
    const RecompRange* r =
        std::upper_bound(g_recomp_ranges, end, addr, [](uint32_t a, const RecompRange& x) { return a < x.start; });
    if (r == g_recomp_ranges || addr >= r[-1].end) return false;
    *n = (uint32_t)(r - 1 - g_recomp_ranges);
    return true;
}

std::atomic<uint64_t>* code_table_counter(uint32_t n) { return &counts()[n]; }

std::string code_table_force(const char* spec) {
    char line[1024];
    if (!strcmp(spec, "all")) {
        uint32_t forced = 0;
        for (uint32_t n = 0; n < g_recomp_range_count; n++) {
            if (g_recomp_ranges[n].flags & RECOMP_RANGE_NO_GUARD) continue;
            code_table_mark(n, CODE_FORCED, true);
            forced++;
        }
        snprintf(line, sizeof line, "[interp] GCN_INTERPRET=all: forced %u functions", forced);
        return line;
    }
    FILE* f = fopen(spec, "r");
    if (!f) fatal("GCN_INTERPRET: can't read %s", spec);
    unsigned bad;
    bool unread;
    {
        std::string said;
        bad = force_file(f, spec, &said);
        unread = ferror(f) != 0;  // a directory opens, and then its first read fails
        fclose(f);
        if (!bad && !unread) return said;
    }
    if (unread) fatal("GCN_INTERPRET: can't read %s", spec);
    fatal("GCN_INTERPRET: %s names no recompiled function's start on %u line%s, listed above", spec, bad,
          bad == 1 ? "" : "s");
}

std::string code_table_stats() {
    // It runs at exit, on whichever thread exits, while a guest thread may still be in a frame.
    std::vector<uint64_t> v(counts().size());
    for (size_t n = 0; n < v.size(); n++) v[n] = counts()[n].load(std::memory_order_relaxed);
    std::vector<uint32_t> order;
    uint64_t total = 0;
    for (uint32_t n = 0; n <= g_recomp_range_count; n++) {
        total += v[n];
        if (n < g_recomp_range_count && v[n]) order.push_back(n);
    }
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return v[a] != v[b] ? v[a] > v[b] : a < b; });
    if (order.size() > 30) order.resize(30);
    char line[256];
    snprintf(line, sizeof line, "[interp] GCN_INTERPRET_STATS: %llu instructions interpreted, %llu of them in loose frames\n",
             (unsigned long long)total, (unsigned long long)v[g_recomp_range_count]);
    std::string out = line;
    for (uint32_t n : order) {
        const uint32_t start = g_recomp_ranges[n].start;
        snprintf(line, sizeof line, "[interp] %12llu  %08X %s\n", (unsigned long long)v[n], start, func_name(start));
        out += line;
    }
    return out;
}

void code_table_boot() {
    const char* spec = getenv("GCN_INTERPRET");
    if (spec && *spec) fprintf(stderr, "%s\n", code_table_force(spec).c_str());
    static bool reporting;
    const char* stats = getenv("GCN_INTERPRET_STATS");
    if (!reporting && stats && !strcmp(stats, "1")) {
        reporting = true;
        plat_at_exit([] { fputs(code_table_stats().c_str(), stderr); });
    }
}
