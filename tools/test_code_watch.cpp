// Code changes at run time: the `test_code_watch` target. It runs with no device, disc or game, on the
// made-up programs tools/fixture_runner.h runs.
//
// It writes over the programs' words in RAM, with an icbi and without one, and checks which functions
// run interpreted after. It also checks the warning for a copy of the text that doesn't match, the line
// for a change to an HLE'd function, GCN_CODE_CHECK's reports and call_indirect's miss path.
//
// What code_watch.cpp prints goes to files in WORKDIR.
//
// usage: test_code_watch WORKDIR
// Exits 0 only if everything matches.
#include "fixture_runner.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace fx;

namespace {

constexpr uint32_t kNop = 0x60000000u;  // ori 0,0,0, which is none of the programs' words

std::string g_dir;
unsigned g_files;

std::string read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        fprintf(stderr, "can't read %s\n", path.c_str());
        exit(2);
    }
    std::string text;
    char b[4096];
    for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) text.append(b, n);
    fclose(f);
    return text;
}

// Runs fn(arg) with stderr sent to a file of its own, and returns what it printed. A stop fails the
// check.
std::string printed(void (*fn)(void*), void* arg = nullptr) {
    const std::string path = g_dir + "/stderr_" + std::to_string(g_files++) + ".txt";
    fflush(stderr);
    const int saved = dup(2);
    FILE* f = fopen(path.c_str(), "w");
    if (saved < 0 || !f) {
        fprintf(stderr, "can't send stderr to %s\n", path.c_str());
        exit(2);
    }
    dup2(fileno(f), 2);
    const bool went_on = guarded(fn, arg);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    fclose(f);
    if (!went_on) fail("stopped with \"" + g_fatal + "\"");
    return read_file(path);
}

std::string hex(uint32_t a) {
    char s[16];
    snprintf(s, sizeof s, "%08X", a);
    return s;
}

// The functions whose bits are set, by start.
std::vector<uint32_t> set_bits() {
    std::vector<uint32_t> out;
    for (uint32_t n = 0; n < g_recomp_range_count; n++)
        if (bit(n)) out.push_back(g_recomp_ranges[n].start);
    return out;
}

std::string starts(const std::vector<uint32_t>& v) {
    std::string s;
    for (uint32_t a : v) s += (s.empty() ? "" : " ") + hex(a);
    return s.empty() ? "none" : s;
}

// RAM as the programs start with no function forced, and boot's copy of the text taken from it. It
// returns what code_watch_boot printed.
std::string boot() {
    reset_ram();
    force_none();
    const std::string said = printed([](void*) { code_watch_boot(); });
    if (!set_bits().empty()) fail("code_watch_boot left the bits of " + starts(set_bits()) + " set");
    return said;
}

// The guest's own icbi of the block holding addr, from f_icbi's word, run natively or interpreted. It
// returns what that printed.
std::string icbi(uint32_t addr, bool interpreted = false) {
    const uint32_t n = range_of(FX_F_ICBI);
    base_cpu(&g_cpu);
    g_cpu.r[3] = addr;
    if (interpreted) code_table_mark(n, CODE_FORCED, true);
    const uint64_t before = *code_table_counter(n);
    uint32_t entry = FX_F_ICBI;
    const std::string said = printed(call_at, &entry);
    const bool ran = *code_table_counter(n) != before;
    if (interpreted) code_table_mark(n, CODE_FORCED, false);
    if (ran != interpreted) fail("the icbi of " + hex(addr) + " ran " + (ran ? "interpreted" : "natively"));
    return said;
}

struct Ran {
    uint32_t r3;
    bool interpreted;
};

Ran run_fn(uint32_t entry) {
    base_cpu(&g_cpu);
    const uint32_t n = range_of(entry);
    const uint64_t before = *code_table_counter(n);
    const std::string stop = call(entry);
    if (!stop.empty()) fail(hex(entry) + " stopped with \"" + stop + "\"");
    return {g_cpu.r[3], *code_table_counter(n) != before};
}

void check_branch_and_back() {
    boot();
    const uint32_t n = range_of(FX_F_HOOKED), old = peek(FX_F_HOOKED);
    const Ran plain = run_fn(FX_F_HOOKED), twin = run_fn(FX_F_HOOKED_TWIN);
    if (plain.r3 == twin.r3) fail("f_hooked and its twin give the same r3, so the checks below can't tell them apart");
    poke(FX_F_HOOKED, FX_HOOK_WORD);
    const bool early = bit(n);
    icbi(FX_F_HOOKED);
    const Ran hooked = run_fn(FX_F_HOOKED);
    if (early || !bit(n) || !hooked.interpreted || hooked.r3 != twin.r3)
        fail("a branch over f_hooked's first word: marked before its icbi " + std::to_string(early) + ", after " +
             std::to_string(bit(n)) + ", interpreted " + std::to_string(hooked.interpreted) + ", r3 " + hex(hooked.r3) +
             " where the twin's is " + hex(twin.r3));
    else printf("match  a branch written over a word, then icbi, sends the function's next entry to the interpreter\n");

    const bool was = bit(n);
    poke(FX_F_HOOKED, old);
    icbi(FX_F_HOOKED, true);
    const Ran back = run_fn(FX_F_HOOKED);
    if (!was) fail("the old word back in f_hooked: it wasn't marked to begin with");
    else if (bit(n) || back.interpreted || back.r3 != plain.r3)
        fail("the old word back in f_hooked: marked " + std::to_string(bit(n)) + ", interpreted " +
             std::to_string(back.interpreted) + ", r3 " + hex(back.r3) + " where native's is " + hex(plain.r3));
    else printf("match  the old word written back, then an interpreted icbi, sends it native again\n");
}

// f_sj's first word and the one 64 bytes on are in different blocks.
void check_recount() {
    boot();
    const uint32_t f = FX_F_SJ, n = range_of(f), w0 = peek(f), w1 = peek(f + 64);
    poke(f, kNop);
    icbi(f);
    poke(f + 64, kNop);
    icbi(f + 64);
    poke(f, w0);
    icbi(f);
    const bool one_left = bit(n);
    poke(f + 64, w1);
    icbi(f + 64);
    if (!one_left || bit(n))
        fail("two changed words in f_sj: marked with one of them back " + std::to_string(one_left) +
             ", with both back " + std::to_string(bit(n)));
    else printf("match  a function stays marked until each of its changed words is back\n");

    code_table_mark(n, CODE_FORCED, true);
    poke(f, kNop);
    icbi(f);
    poke(f, w0);
    icbi(f);
    const bool forced = bit(n);
    code_table_mark(n, CODE_FORCED, false);
    if (!forced || bit(n))
        fail("a forced f_sj with its word back: marked " + std::to_string(forced) + " while forced, and " +
             std::to_string(bit(n)) + " after");
    else printf("match  a forced function stays interpreted once its changed word is back\n");
}

void check_ranges() {
    boot();
    // Two functions of two words each, side by side in one block. The icbi is of the block's last word.
    const auto two_words = [](uint32_t k) { return g_recomp_ranges[k].end == g_recomp_ranges[k].start + 8; };
    uint32_t n = 0;
    while (n + 1 < g_recomp_range_count &&
           !(two_words(n) && two_words(n + 1) && g_recomp_ranges[n + 1].start == g_recomp_ranges[n].end &&
             (g_recomp_ranges[n].start & ~31u) == ((g_recomp_ranges[n].start + 15) & ~31u)))
        n++;
    const uint32_t s = g_recomp_ranges[n].start;
    poke(s, kNop);
    poke(s + 8, kNop);
    icbi((s & ~31u) + 28);
    const std::vector<uint32_t> want = {s, s + 8};
    if (n + 1 >= g_recomp_range_count) fail("no block holds two functions of two words");
    else if (set_bits() != want)
        fail("an icbi of " + hex(s) + "'s block marked " + starts(set_bits()) + ", not " + starts(want));
    else printf("match  an icbi anywhere in a block marks each function there whose words changed\n");

    boot();
    poke(FX_F_IDLE - 4, kNop);
    poke(FX_F_IDLE, kNop);
    gcn_code_written(FX_F_IDLE - 4, 8);
    gcn_code_written(FX_JUMPTABLE, 0x100);
    icbi(FX_BLOB_LOOP);
    const std::vector<uint32_t> two = {FX_F_LOOP, FX_F_IDLE};
    if (set_bits() != two)
        fail("gcn_code_written across f_loop's last word marked " + starts(set_bits()) + ", not " + starts(two));
    else printf("match  gcn_code_written marks each function its range overlaps, and none outside the text\n");
}

// The second text section, and the text's ends. f_loop's first word is the text's lowest and
// f_second_next's last word its highest. The fillers end the first section, two words each.
void check_sections() {
    boot();
    poke(FX_F_SECOND, kNop);
    icbi(FX_F_SECOND);
    const std::vector<uint32_t> second = {FX_F_SECOND};
    if (set_bits() != second)
        fail("a change in the second text section marked " + starts(set_bits()) + ", not " + starts(second));
    else printf("match  a change in a second text section marks its function\n");

    boot();
    const uint32_t first_end = FX_FILLER_00 + 8 * FX_FILLER_COUNT;
    for (uint32_t a : {FX_F_LOOP, first_end - 4, FX_F_SECOND_NEXT + 4}) {
        poke(a, kNop);
        icbi(a);
    }
    const std::vector<uint32_t> ends = {FX_F_LOOP, first_end - 8, FX_F_SECOND_NEXT};
    if (set_bits() != ends)
        fail("changes at the text's ends and the first section's marked " + starts(set_bits()) + ", not " +
             starts(ends));
    else printf("match  a change at either end of the text, or at the first section's, marks its function\n");
}

void check_hle() {
    boot();
    const uint32_t f = FX_MADE_UP_SAVE, w = peek(f + 4);
    poke(f + 4, kNop);
    const std::string said = icbi(f + 4);
    poke(f + 4, w);
    icbi(f + 4);
    poke(f + 4, kNop);
    const std::string again = icbi(f + 4);
    const std::string want =
        "[interp] the HLE'd made_up_save at " + hex(f) + " changed, and its replacement still runs in its place\n";
    if (said != want) fail("a change to an HLE'd function printed\n" + said + "where it should print\n" + want);
    else if (!again.empty() || !bit(range_of(f))) fail("a second change to made_up_save printed \"" + again + "\"");
    else printf("match  a change to an HLE'd function prints one line naming it\n");
}

void check_mismatch() {
    setenv("GCN_CODE_CHECK", "1", 1);
    reset_ram();
    force_none();
    poke(FX_F_LOOP, kNop);
    const std::string said = printed([](void*) { code_watch_boot(); });
    poke(FX_F_IDLE, kNop);
    icbi(FX_F_IDLE);
    const std::string check = printed([](void*) { code_watch_frame(); });
    const std::string want = "[interp] the text in RAM at boot doesn't match what this build was recompiled from, "
                             "so code changes go unseen\n";
    if (said != want) fail("a copy of the text that doesn't match printed\n" + said + "where it should print\n" + want);
    else if (!set_bits().empty() || !check.empty())
        fail("with detection off, an icbi marked " + starts(set_bits()) + " and GCN_CODE_CHECK printed \"" + check +
             "\"");
    else printf("match  a copy of the text that doesn't match warns, and turns detection and GCN_CODE_CHECK off\n");
    unsetenv("GCN_CODE_CHECK");

    boot();
    poke(FX_F_IDLE, kNop);
    icbi(FX_F_IDLE);
    if (!bit(range_of(FX_F_IDLE))) fail("a boot that matches after one that didn't left detection off");
    else printf("match  a boot that matches turns detection on again\n");
}

void check_unmarked() {
    const uint32_t at = FX_F_LOOP + 4, old = peek(at);
    boot();
    poke(at, kNop);
    const std::string off = printed([](void*) { code_watch_frame(); });

    setenv("GCN_CODE_CHECK", "1", 1);
    boot();
    poke(at, kNop);
    poke(FX_F_CALLEE, kNop);
    icbi(FX_F_CALLEE);
    const bool marked = bit(range_of(at));
    const std::string first = printed([](void*) { code_watch_frame(); });
    const std::string again = printed([](void*) { code_watch_frame(); });
    unsetenv("GCN_CODE_CHECK");
    const std::string want = "[interp] GCN_CODE_CHECK: the word at " + hex(at) + " in f_loop is " + hex(kNop) +
                             ", not the " + hex(old) + " it was at boot, and nothing marked the function changed\n";
    if (!off.empty()) fail("with GCN_CODE_CHECK unset, a frame printed \"" + off + "\"");
    else if (marked) fail("a write to f_loop with no icbi marked it");
    else if (first != want) fail("GCN_CODE_CHECK printed\n" + first + "where it should print\n" + want);
    else if (!again.empty()) fail("GCN_CODE_CHECK's next frame printed \"" + again + "\"");
    else printf("match  a write with no icbi isn't marked, and GCN_CODE_CHECK reports it once\n");
}

CPU g_thread;  // a guest thread whose stack the check reads

void check_live() {
    setenv("GCN_CODE_CHECK", "1", 1);
    boot();
    unsetenv("GCN_CODE_CHECK");
    poke(FX_F_CALL + 16, kNop);
    icbi(FX_F_CALL + 16);
    // The frames' back chain and saved LRs, read as the ABI lays them out. The outer frame returns into
    // f_call after its bl, and the inner one into f_midcaller, which didn't change.
    const uint32_t sp0 = FX_STACK - 0x60, sp1 = FX_STACK - 0x40, sp2 = FX_STACK - 0x20;
    poke(sp0, sp1);
    poke(sp1, sp2);
    poke(sp1 + 4, FX_F_MIDCALLER + 16);
    poke(sp2, 0);
    poke(sp2 + 4, FX_F_CALL + 16);
    base_cpu(&g_thread);
    g_thread.r[1] = sp0;
    g_thread.lr = FX_F_CALL + 16;  // and LR too, which mustn't report f_call a second time
    const std::string first = printed([](void*) { code_watch_check({&g_thread}); });
    const std::string again = printed([](void*) { code_watch_check({&g_thread}); });
    const std::string want = "[interp] GCN_CODE_CHECK: f_call at " + hex(FX_F_CALL) +
                             " changed with a frame of it live on a guest stack, which returns to " +
                             hex(FX_F_CALL + 16) + "\n";
    if (first != want) fail("GCN_CODE_CHECK printed\n" + first + "where it should print\n" + want);
    else if (!again.empty()) fail("GCN_CODE_CHECK's next check printed \"" + again + "\"");
    else printf("match  GCN_CODE_CHECK reports a changed function live on a guest stack, at the first check\n");

    // A thread stopped in a call that f_call made, whose LR no frame on the stack has saved yet.
    setenv("GCN_CODE_CHECK", "1", 1);
    boot();
    unsetenv("GCN_CODE_CHECK");
    poke(FX_F_CALL + 16, kNop);
    icbi(FX_F_CALL + 16);
    poke(sp2, 0);
    base_cpu(&g_thread);
    g_thread.r[1] = sp2;
    g_thread.lr = FX_F_CALL + 16;
    const std::string lr = printed([](void*) { code_watch_check({&g_thread}); });
    if (lr != want) fail("GCN_CODE_CHECK with f_call live in LR printed\n" + lr + "where it should print\n" + want);
    else printf("match  GCN_CODE_CHECK reports a changed function that a thread's LR returns into\n");
}

void check_stats() {
    setenv("GCN_CODE_CHECK", "1", 1);
    boot();
    unsetenv("GCN_CODE_CHECK");
    for (int k = 0; k < 3; k++) icbi(FX_F_LOOP);
    std::string said = printed([](void*) { code_watch_frame(); });
    icbi(FX_F_LOOP);
    said += printed([](void*) { code_watch_frame(); });
    said += printed([](void*) { code_watch_frame(); });
    const std::string want = "[interp] GCN_CODE_CHECK: 4 icbi ran in 2 of 3 frames, and the most in one frame was 3\n";
    const std::string got = code_watch_stats();
    if (!said.empty()) fail("GCN_CODE_CHECK's frames with nothing changed printed \"" + said + "\"");
    else if (got != want) fail("GCN_CODE_CHECK's report is\n" + got + "where it should be\n" + want);
    else printf("match  GCN_CODE_CHECK counts the icbi in each frame\n");
}

uint32_t g_at;
bool g_ran;

void run_unknown(void*) { g_ran = code_watch_run_unknown(&g_cpu, g_at, g_cpu.lr); }

void check_unknown() {
    reset_ram();
    std::string want = "[interp] running unknown code at " + hex(FX_BLOB_LOOP) + " (lr=" + hex(kLR) + ")\n";
    std::string said;
    bool ran = true, moved = true;
    for (int k = 0; k < 2; k++) {
        base_cpu(&g_cpu);
        g_at = FX_BLOB_LOOP;
        const uint64_t before = *code_table_counter(g_recomp_range_count);
        said += printed(run_unknown);
        ran = ran && g_ran;
        moved = moved && *code_table_counter(g_recomp_range_count) != before;
    }
    if (!ran || !moved)
        fail("a call into a RAM blob: returned true " + std::to_string(ran) + ", ran loose " + std::to_string(moved));
    else if (said != want) fail("two calls into a RAM blob printed\n" + said + "where they should print\n" + want);
    else printf("match  a call into a RAM blob runs loose, with a line the first time\n");

    boot();
    base_cpu(&g_cpu);
    g_at = FX_BLOB_LOOP;
    said = printed(run_unknown);
    if (said != want)
        fail("a call into the RAM blob after another boot printed\n" + said + "where it should print\n" + want);
    else printf("match  another boot prints the line for the same blob again\n");

    bool none = true;
    for (uint32_t a : {FX_BLOB_LOOP + 2, FX_BLOB_LOOP + 1, 0x81800000u, 0x7FFFFFFCu}) {
        base_cpu(&g_cpu);
        g_at = a;
        const uint64_t before = *code_table_counter(g_recomp_range_count);
        said = printed(run_unknown);
        if (g_ran || *code_table_counter(g_recomp_range_count) != before || !said.empty()) {
            fail("a call to " + hex(a) + " ran loose, or printed \"" + said + "\"");
            none = false;
        }
    }
    if (none) printf("match  a misaligned call, or one outside main RAM, runs nothing and call_indirect stops\n");
}

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_code_watch <work dir>\n");
        return 2;
    }
    g_dir = argv[1];
    if (exists(g_dir) || mkdir(g_dir.c_str(), 0755) != 0) {
        fprintf(stderr, "%s exists or can't be made: give a new directory\n", g_dir.c_str());
        return 2;
    }
    if (!runner_init("test_code_watch")) return 2;
    unsetenv("GCN_CODE_CHECK");
    g_at_exit = 0;

    check_branch_and_back();
    check_recount();
    check_ranges();
    check_sections();
    check_hle();
    check_mismatch();
    check_unmarked();
    check_live();
    check_stats();
    check_unknown();
    if (g_at_exit != 1) fail("GCN_CODE_CHECK's report registered " + std::to_string(g_at_exit) + " times, not once");
    else printf("match  GCN_CODE_CHECK's report registers once however many boots read it\n");
    return runner_finish();
}
