// Code changes at run time. Boot's copy of the text holds each recompiled function's words as the build
// compiled them, and a function whose words in RAM differ from it runs interpreted from its next entry.
// The guest's icbi and any other writer of code (gcn_code_written) recount the functions they touch.
//
// GCN_CODE_CHECK=1 compares all the text at each frame boundary, looking for a change that nothing
// marked, and counts the icbi each frame ran. call_indirect's miss path, which runs code in RAM that no
// table knows, is here too.
#include "interp.h"
#include "platform.h"
#include "runtime.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace {

struct Section {
    uint32_t start, end;
    std::vector<uint8_t> bytes;  // as boot found them
};

// Only the guest thread holding the baton reads or changes these.
struct Watch {
    bool on = false;     // boot's copy matched the text the build was recompiled from
    bool check = false;  // GCN_CODE_CHECK=1
    std::vector<Section> text;
    uint32_t lo = 0, hi = 0;  // the text's lowest address and the end of its highest
    // By a function's index in g_recomp_ranges, its words that differ from boot's copy as last counted.
    std::vector<uint32_t> changed;
    std::vector<uint8_t> hle_said;      // a change to the HLE'd function has printed its line
    std::vector<uint8_t> check_saw;     // the last check found the function's words changed
    std::unordered_set<uint32_t> said;  // the unmarked words the check reported
    std::unordered_set<uint32_t> seen;  // the unknown code that call_indirect's miss path ran
    uint64_t frame_icbi = 0;            // the icbi since the last frame boundary
};
Watch g_watch;

// GCN_CODE_CHECK's counts by frame, which only the guest thread adds to and the report at exit reads
// from another thread.
std::atomic<uint64_t> g_icbi, g_frames, g_icbi_frames, g_icbi_most;

void add(std::atomic<uint64_t>& v, uint64_t k) {
    v.store(v.load(std::memory_order_relaxed) + k, std::memory_order_relaxed);
}

uint8_t* ram(uint32_t a) { return g_mem + (a & 0x3FFFFFFFu); }

uint32_t word(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

bool in_ram(uint32_t a) { return a - 0x80000000u < RAM_SIZE; }

// Boot's copy of [a, a + len), or null when no one text section holds all of it.
const uint8_t* copy_of(uint32_t a, uint32_t len) {
    for (const Section& s : g_watch.text)
        if (a >= s.start && a + len <= s.end) return s.bytes.data() + (a - s.start);
    return nullptr;
}

// Counts function n's words that differ from boot's copy, and marks it while any do. A module's or a
// REL's function isn't in the copy, and has no guard to mark.
void recount(uint32_t n) {
    const RecompRange& r = g_recomp_ranges[n];
    const uint8_t* was = copy_of(r.start, r.end - r.start);
    if (!was) return;
    uint32_t differ = 0;
    for (uint32_t off = 0; off < r.end - r.start; off += 4) differ += memcmp(ram(r.start + off), was + off, 4) != 0;
    if (differ && !g_watch.changed[n] && (r.flags & RECOMP_RANGE_HLE) && !g_watch.hle_said[n]) {
        g_watch.hle_said[n] = 1;
        fprintf(stderr, "[interp] the HLE'd %s at %08X changed, and its replacement still runs in its place\n",
                func_name(r.start), r.start);
    }
    g_watch.changed[n] = differ;
    code_table_mark(n, CODE_CHANGED, differ != 0);
}

// Recounts each function [lo, hi) overlaps whose words there differ from boot's copy, or that has a
// count already. When nothing changed that's a lookup and a compare, and a range outside the text is
// only the compare with its ends.
void watch(uint32_t lo, uint32_t hi) {
    if (!g_watch.on || hi <= g_watch.lo || lo >= g_watch.hi) return;
    const RecompRange* end = g_recomp_ranges + g_recomp_range_count;
    const RecompRange* r =
        std::upper_bound(g_recomp_ranges, end, lo, [](uint32_t a, const RecompRange& x) { return a < x.end; });
    for (; r != end && r->start < hi; r++) {
        const uint32_t n = (uint32_t)(r - g_recomp_ranges);
        const uint32_t a = std::max(lo, r->start), b = std::min(hi, r->end);
        const uint8_t* was = copy_of(a, b - a);
        if (g_watch.changed[n] || (was && memcmp(ram(a), was, b - a))) recount(n);
    }
}

// The words of the text that differ from boot's copy, by function. Each one whose function nothing
// marked is reported once.
std::vector<uint8_t> compare_text() {
    std::vector<uint8_t> differs(g_recomp_range_count);
    constexpr uint32_t kSpan = 256;  // compared whole first, as nearly all of it matches
    for (const Section& s : g_watch.text)
        for (uint32_t off = 0; off < s.bytes.size(); off += kSpan) {
            const uint32_t len = std::min<uint32_t>(kSpan, (uint32_t)s.bytes.size() - off);
            if (!memcmp(ram(s.start + off), s.bytes.data() + off, len)) continue;
            for (uint32_t k = off; k < off + len; k += 4) {
                const uint32_t a = s.start + k;
                uint32_t n;
                if (!memcmp(ram(a), s.bytes.data() + k, 4) || !code_table_find(a, &n)) continue;
                differs[n] = 1;
                if (g_watch.changed[n] || !g_watch.said.insert(a).second) continue;
                fprintf(stderr,
                        "[interp] GCN_CODE_CHECK: the word at %08X in %s is %08X, not the %08X it was at boot, and "
                        "nothing marked the function changed\n",
                        a, func_name(g_recomp_ranges[n].start), word(ram(a)), word(s.bytes.data() + k));
            }
        }
    return differs;
}

}  // namespace

extern "C" void hle_icbi(CPU* c, uint32_t ea) {
    (void)c;
    g_watch.frame_icbi++;
    const uint32_t lo = ea & ~31u;
    watch(lo, lo + 32);
}

extern "C" void gcn_code_written(uint32_t addr, uint32_t len) { watch(addr, addr + len); }

void code_watch_boot() {
    for (uint32_t n = 0; n < g_watch.changed.size(); n++)
        if (g_watch.changed[n]) code_table_mark(n, CODE_CHANGED, false);
    g_watch = Watch{};
    g_watch.changed.assign(g_recomp_range_count, 0);
    g_watch.hle_said.assign(g_recomp_range_count, 0);
    g_watch.check_saw.assign(g_recomp_range_count, 0);
    uint64_t fnv = 0xCBF29CE484222325ull;  // 64-bit FNV-1a, as recomp/interp_tables.py takes it
    for (uint32_t k = 0; k < g_recomp_text_count; k++) {
        const RecompText& t = g_recomp_text[k];
        Section s{t.start, t.start + t.size, std::vector<uint8_t>(ram(t.start), ram(t.start) + t.size)};
        for (uint8_t b : s.bytes) fnv = (fnv ^ b) * 0x100000001B3ull;
        g_watch.text.push_back(std::move(s));
    }
    g_watch.on = fnv == g_recomp_text_fnv;
    if (!g_watch.on) {
        g_watch.text.clear();
        fprintf(stderr, "[interp] the text in RAM at boot doesn't match what this build was recompiled from, so code "
                        "changes go unseen\n");
    } else if (!g_watch.text.empty()) {
        g_watch.lo = g_watch.text.front().start;
        g_watch.hi = g_watch.text.back().end;
    }
    const char* check = getenv("GCN_CODE_CHECK");
    g_watch.check = g_watch.on && check && !strcmp(check, "1");
    for (std::atomic<uint64_t>* v : {&g_icbi, &g_frames, &g_icbi_frames, &g_icbi_most})
        v->store(0, std::memory_order_relaxed);
    static bool reporting;
    if (g_watch.check && !reporting) {
        reporting = true;
        plat_at_exit([] { fputs(code_watch_stats().c_str(), stderr); });
    }
}

void code_watch_check(const std::vector<const CPU*>& threads) {
    if (!g_watch.on) return;
    const std::vector<uint8_t> differs = compare_text();
    // A function is reported live at the first check that finds it changed.
    std::vector<uint8_t> fresh(g_recomp_range_count);
    for (uint32_t n = 0; n < g_recomp_range_count; n++) {
        fresh[n] = differs[n] && !g_watch.check_saw[n];
        g_watch.check_saw[n] = differs[n];
    }
    const auto live = [&fresh](uint32_t ret) {
        uint32_t n;
        if (!code_table_find(ret - 4, &n) || !fresh[n]) return;
        fresh[n] = 0;
        fprintf(stderr,
                "[interp] GCN_CODE_CHECK: %s at %08X changed with a frame of it live on a guest stack, which returns "
                "to %08X\n",
                func_name(g_recomp_ranges[n].start), g_recomp_ranges[n].start, ret);
    };
    // A guest thread's LR returns into the function that made the call the thread is in, until something
    // saves it. The check reads it, then the thread's back chain as debug_sampler_start in threads.cpp
    // walks it. The word at [sp] is the caller's frame, and the word 4 past it holds the return address.
    for (const CPU* t : threads) {
        live(t->lr);
        for (uint32_t sp = t->r[1]; in_ram(sp);) {
            const uint32_t prev = mem_r32(sp);
            if (!in_ram(prev) || prev <= sp) break;
            live(mem_r32(prev + 4));
            sp = prev;
        }
    }
}

void code_watch_frame() {
    if (!g_watch.check) return;
    code_watch_check(guest_threads());
    const uint64_t k = g_watch.frame_icbi;
    g_watch.frame_icbi = 0;
    add(g_icbi, k);
    add(g_frames, 1);
    if (k) add(g_icbi_frames, 1);
    if (k > g_icbi_most.load(std::memory_order_relaxed)) g_icbi_most.store(k, std::memory_order_relaxed);
}

std::string code_watch_stats() {
    char line[200];
    snprintf(line, sizeof line,
             "[interp] GCN_CODE_CHECK: %llu icbi ran in %llu of %llu frames, and the most in one frame was %llu\n",
             (unsigned long long)g_icbi.load(std::memory_order_relaxed),
             (unsigned long long)g_icbi_frames.load(std::memory_order_relaxed),
             (unsigned long long)g_frames.load(std::memory_order_relaxed),
             (unsigned long long)g_icbi_most.load(std::memory_order_relaxed));
    return line;
}

bool code_watch_run_unknown(CPU* c, uint32_t addr, uint32_t entry_lr) {
    if ((addr & 3) || !in_ram(addr)) return false;
    if (g_watch.seen.insert(addr).second)
        fprintf(stderr, "[interp] running unknown code at %08X (lr=%08X), which returns to %08X\n", addr, c->lr,
                entry_lr);
    interp_run_loose(c, addr, entry_lr);
    return true;
}
