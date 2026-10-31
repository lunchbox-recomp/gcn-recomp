// The code table: the `test_code_table` target. It runs with no device, disc or game, on the made-up
// programs tools/fixture_runner.h runs.
//
// It checks the bit setter, GCN_INTERPRET_STATS's report, GCN_INTERPRET's two forms and
// code_table_boot. The FILE form's lists go in WORKDIR.
//
// usage: test_code_table WORKDIR
// Exits 0 only if everything matches.
#include "fixture_runner.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

void check_setter() {
    const uint32_t n = range_of(FX_F_LOOP);
    code_table_mark(n, CODE_FORCED, true);
    const bool forced = bit(n);
    code_table_mark(n, CODE_CHANGED, true);
    code_table_mark(n, CODE_FORCED, false);
    const bool changed = bit(n);
    code_table_mark(n, CODE_CHANGED, false);
    const bool neither = bit(n);
    if (!forced || !changed || neither || bit(n + 1))
        fail("the setter: forced " + std::to_string(forced) + ", changed " + std::to_string(changed) + ", neither " +
             std::to_string(neither) + ", the next function " + std::to_string(bit(n + 1)));
    else printf("match  the setter, with a bit set while either mark is\n");
}

// Runs filler k forced k + 1 times, each two words, so the 30 that ran the most are 39 down to 10.
// It runs first, while every count is still 0, and returns the total.
uint64_t check_stats() {
    std::vector<uint32_t> starts(FX_FILLER_COUNT);
    for (uint32_t k = 0; k < FX_FILLER_COUNT; k++) {
        char name[16];
        snprintf(name, sizeof name, "filler_%02u", k);
        for (uint32_t i = 0; i < g_recomp_func_count; i++)
            if (!strcmp(g_recomp_funcs[i].name, name)) starts[k] = g_recomp_funcs[i].addr;
    }
    uint64_t total = 0;
    std::string stop;
    reset_ram();
    for (uint32_t k = 0; k < FX_FILLER_COUNT; k++) {
        const uint32_t n = range_of(starts[k]);
        code_table_mark(n, CODE_FORCED, true);
        for (uint32_t j = 0; j <= k; j++) {
            base_cpu(&g_cpu);
            const std::string s = call(starts[k]);
            if (stop.empty() && !s.empty()) stop = "filler_" + std::to_string(k) + " stopped with \"" + s + "\"";
            total += 2;
        }
        code_table_mark(n, CODE_FORCED, false);
    }
    if (!stop.empty()) fail("GCN_INTERPRET_STATS: " + stop);
    char line[160];
    snprintf(line, sizeof line, "[interp] GCN_INTERPRET_STATS: %llu instructions interpreted, 0 of them in loose frames\n",
             (unsigned long long)total);
    std::string want = line;
    for (uint32_t k = FX_FILLER_COUNT; k-- > FX_FILLER_COUNT - 30;) {
        snprintf(line, sizeof line, "[interp] %12llu  %08X filler_%02u\n", (unsigned long long)(2 * (k + 1)), starts[k], k);
        want += line;
    }
    const std::string got = code_table_stats();
    if (got != want) fail("GCN_INTERPRET_STATS printed\n" + got + "where it should print\n" + want);
    else printf("match  GCN_INTERPRET_STATS's 30 functions and its total\n");
    return total;
}

void run_loose(void* pc) { interp_run_loose(&g_cpu, *(const uint32_t*)pc); }

// A loose frame's words count in the total as well as on their own.
void check_loose_stats(uint64_t total) {
    reset_ram();
    base_cpu(&g_cpu);
    uint32_t pc = FX_BLOB_LOOP;
    if (!guarded(run_loose, &pc)) fail("the loose run for GCN_INTERPRET_STATS stopped with \"" + g_fatal + "\"");
    const uint64_t loose = *code_table_counter(g_recomp_range_count);
    char want[160];
    snprintf(want, sizeof want, "[interp] GCN_INTERPRET_STATS: %llu instructions interpreted, %llu of them in loose frames\n",
             (unsigned long long)(total + loose), (unsigned long long)loose);
    const std::string got = code_table_stats();
    const std::string first = got.substr(0, got.find('\n') + 1);
    if (!loose || first != want) fail("after a loose run, GCN_INTERPRET_STATS printed\n" + first + "where it should print\n" + want);
    else printf("match  GCN_INTERPRET_STATS's total after a loose run\n");
}

std::string g_forced;

void force_spec(void* spec) { g_forced = code_table_force((const char*)spec); }

// The functions whose bits are set, by start.
std::vector<uint32_t> set_bits() {
    std::vector<uint32_t> out;
    for (uint32_t n = 0; n < g_recomp_range_count; n++)
        if (bit(n)) out.push_back(g_recomp_ranges[n].start);
    return out;
}

void write_file(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "can't write %s\n", path.c_str());
        exit(2);
    }
    fputs(text.c_str(), f);
    fclose(f);
}

void check_interpret(const std::string& dir) {
    // The list has each kind of line, which are bare upper-case hex, 0x with lower case, 0X with a
    // comment after, a blank line, spaces and tabs around, an HLE'd function's start, a module's start
    // and a start listed twice. The HLE'd wrapper stays native while its original body is forced, and
    // the module's code has no guard.
    char text[512];
    snprintf(text, sizeof text, "# starts to force\n%08X\n0x%08x\n0X%08X  # with a comment\n\n \t%08X\t \n%08X\n%08X\n%08X\n",
             FX_F_LOOP, FX_F_IDLE, FX_F_CALL, FX_F_TAIL, FX_MADE_UP_SAVE, FX_MADE_UP_MODULE_FN, FX_F_LOOP);
    const std::string list = dir + "/starts.txt";
    write_file(list, text);
    force_none();
    g_forced.clear();
    char want[256];
    snprintf(want, sizeof want, "[interp] GCN_INTERPRET=%s: forced 5 of the 6 starts listed (1 with no guard)", list.c_str());
    std::vector<uint32_t> want_bits = {FX_F_LOOP, FX_F_IDLE, FX_F_CALL, FX_F_TAIL, FX_MADE_UP_SAVE};
    std::sort(want_bits.begin(), want_bits.end());  // set_bits goes by start
    if (!guarded(force_spec, (void*)list.c_str())) fail("GCN_INTERPRET=FILE stopped with \"" + g_fatal + "\"");
    else if (g_forced != want) fail("GCN_INTERPRET=FILE printed \"" + g_forced + "\", not \"" + want + "\"");
    else if (set_bits() != want_bits) fail("GCN_INTERPRET=FILE set other bits than the five guarded starts'");
    else printf("match  GCN_INTERPRET=FILE with each kind of line\n");

    // The HLE'd function's wrapper runs its replacement natively, which calls the forced original.
    reset_ram();
    base_cpu(&g_cpu);
    g_calls.clear();
    g_bindings.clear();
    g_cpu.r[3] = FX_CONTEXT;
    const uint64_t before = *code_table_counter(range_of(FX_MADE_UP_SAVE));
    const std::string stop = call(FX_MADE_UP_SAVE);
    bool replaced = false;
    for (const Call& c : g_calls) replaced = replaced || c.fn == "hle_made_up_save";
    if (!stop.empty() || !replaced || *code_table_counter(range_of(FX_MADE_UP_SAVE)) == before)
        fail("a forced HLE'd function: stopped \"" + stop + "\", replacement ran " + std::to_string(replaced) +
             ", original interpreted " + std::to_string(*code_table_counter(range_of(FX_MADE_UP_SAVE)) != before));
    else printf("match  a forced HLE'd function runs its replacement, which interprets the original\n");
    force_none();

    // Each line that names no recompiled function's start stops it, naming the line.
    char mid[16], extra[32];
    snprintf(mid, sizeof mid, "%08X", FX_F_LOOP + 4);
    snprintf(extra, sizeof extra, "%08X 1", FX_F_LOOP);
    for (const char* bad : {(const char*)mid, "f_loop", (const char*)extra, "0x", "-8010000"}) {
        snprintf(text, sizeof text, "%08X\n%s\n", FX_F_IDLE, bad);
        const std::string path = dir + "/bad.txt";
        write_file(path, text);
        snprintf(want, sizeof want, "GCN_INTERPRET: line 2 of %s is \"%s\", which isn't a recompiled function's start",
                 path.c_str(), bad);
        if (guarded(force_spec, (void*)path.c_str())) fail(std::string("GCN_INTERPRET took the line \"") + bad + "\"");
        else if (g_fatal != want) fail("GCN_INTERPRET stopped with \"" + g_fatal + "\", not \"" + want + "\"");
        else printf("match  GCN_INTERPRET stops at the line \"%s\"\n", bad);
        force_none();
    }
    const std::string missing = dir + "/missing.txt";
    snprintf(want, sizeof want, "GCN_INTERPRET: can't read %s", missing.c_str());
    if (guarded(force_spec, (void*)missing.c_str()) || g_fatal.find(want) != 0)
        fail("GCN_INTERPRET with no such file: \"" + g_fatal + "\"");
    else printf("match  GCN_INTERPRET stops when its file isn't there\n");

    uint32_t guarded_count = 0;
    std::vector<uint32_t> all_bits;
    for (uint32_t n = 0; n < g_recomp_range_count; n++)
        if (guarded_range(n)) {
            guarded_count++;
            all_bits.push_back(g_recomp_ranges[n].start);
        }
    snprintf(want, sizeof want, "[interp] GCN_INTERPRET=all: forced %u functions", guarded_count);
    if (!guarded(force_spec, (void*)"all") || g_forced != want || set_bits() != all_bits)
        fail("GCN_INTERPRET=all printed \"" + g_forced + "\", not \"" + want + "\", or set other bits");
    else printf("match  GCN_INTERPRET=all forces every guarded function\n");
    force_none();

    // code_table_boot reads both variables, and registers the report once however often it runs.
    setenv("GCN_INTERPRET", "all", 1);
    setenv("GCN_INTERPRET_STATS", "1", 1);
    g_at_exit = 0;
    code_table_boot();
    code_table_boot();
    if (set_bits() != all_bits || g_at_exit != 1)
        fail("code_table_boot: " + std::to_string(set_bits().size()) + " bits set, the report registered " +
             std::to_string(g_at_exit) + " times");
    else printf("match  code_table_boot reads GCN_INTERPRET and GCN_INTERPRET_STATS\n");
    unsetenv("GCN_INTERPRET");
    unsetenv("GCN_INTERPRET_STATS");
    force_none();
}

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_code_table <work dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    if (exists(dir) || mkdir(dir.c_str(), 0755) != 0) {
        fprintf(stderr, "%s exists or can't be made: give a new directory\n", dir.c_str());
        return 2;
    }
    if (!runner_init("test_code_table")) return 2;

    check_loose_stats(check_stats());
    check_setter();
    check_interpret(dir);
    return runner_finish();
}
