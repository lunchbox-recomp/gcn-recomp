// Which recompiled functions run interpreted. A function's bit in g_code_changed_bits is set while any
// of its marks is. This build reads neither GCN_INTERPRET nor GCN_INTERPRET_STATS yet.
#include "interp.h"
#include <vector>

void code_table_mark(uint32_t n, CodeMark mark, bool on) {
    static std::vector<uint8_t> marks(g_recomp_range_count);
    uint8_t& m = marks[n];
    m = on ? (uint8_t)(m | mark) : (uint8_t)(m & ~mark);
    const uint8_t bit = (uint8_t)(1u << (n & 7));
    if (m) g_code_changed_bits[n >> 3] |= bit;
    else g_code_changed_bits[n >> 3] &= (uint8_t)~bit;
}

bool code_table_find(uint32_t, uint32_t*) { return false; }

uint64_t* code_table_counter(uint32_t) {
    static uint64_t none;
    return &none;
}

std::string code_table_force(const char*) { return ""; }

std::string code_table_stats() { return ""; }

void code_table_boot() {}
