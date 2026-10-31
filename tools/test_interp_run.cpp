// The interpreter's frames: the `test_interp_run` and `test_interp_run_trace` targets. They run with
// no device, disc or game.
//
// Each case runs a made-up program natively and interpreted, as tools/fixture_runner.h says.
// test_interp_run_trace is built with GCN_TRACE_CALLS, and each of its runs must leave the guest call
// stack as deep as it found it.
//
// After the cases come --log=interp's lines and the two stops, a longjmp frame that returns and a
// patch with a goto.
//
// usage: test_interp_run
// Exits 0 only if everything matches.
#include "fixture_runner.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace fx;

namespace {

const Case kCases[] = {
    {"an in-range loop", FX_F_LOOP, 0, nullptr, "IRQ_CHECK", false},
    {"an idle back edge", FX_F_IDLE, 0, nullptr, "idle_hint", false},
    {"mtmsr and the IRQ_CHECK after it", FX_F_MTMSR, 0, [](CPU*) { g_vlimit = 1; }, "irq_poll", false},
    {"a tail call", FX_F_TAIL, 0, nullptr, nullptr, false},
    {"a bl and its return", FX_F_CALL, 0, nullptr, nullptr, false},
    {"blrl and bctrl", FX_F_REGCALLS, 0, nullptr, nullptr, false},
    {"a jump-table bctr to its first case", FX_F_SWITCH, 0, [](CPU* c) { c->r[3] = 0; }, nullptr, false},
    {"a jump-table bctr to its last case", FX_F_SWITCH, 0, [](CPU* c) { c->r[3] = 2; }, nullptr, false},
    {"a jump-table bctr back to a case before it", FX_F_SWITCH_BACK, 0, nullptr, nullptr, false},
    {"a conditional bl not taken, then one taken", FX_F_CONDBL, 0, nullptr, nullptr, false},
    {"a mid-entry", FX_F_MULTI_MID, 0, nullptr, nullptr, false},
    {"a bl to a mid-entry", FX_F_MIDCALLER, 0, nullptr, nullptr, false},
    {"running off the end into the next function", FX_F_FALLS, 0, nullptr, nullptr, false},
    {"a patch site", FX_F_PATCH, 0, nullptr, "heap_trace_alloc", false},
    {"a patch site that returns", FX_F_PATCH_RET, 0, nullptr, "heap_trace_alloc_result", false},
    {"a site whose word changed in RAM", FX_F_SITE2, FX_F_SITE2_TWIN,
     [](CPU*) { poke(FX_F_SITE2_SITE, FX_SITE2_WORD); }, nullptr, false},
    {"a step site", FX_F_STEP, 0, nullptr, "gcn_step_int", false},
    {"a setjmp site resumed by a longjmp", FX_F_SJ, 0, nullptr, "hle_longjmp_resume", false},
    {"a context site resumed after its frame went on", FX_F_CTX, 0, nullptr, "hle_made_up_load", false},
    {"an interrupt taken mid-loop and resumed from its snapshot", FX_F_IRQ_LOOP, 0,
     [](CPU*) {
         g_vlimit = 3;
         g_irq_armed = true;
     },
     "hle_made_up_resume", false},
    {"a loose frame in a RAM blob with its own loop", FX_BLOB_LOOP, FX_F_BLOB_TWIN, nullptr, "IRQ_CHECK", true},
    {"a trampoline into a function's middle and its return", FX_F_HOOKED, FX_F_HOOKED_TWIN,
     [](CPU*) { poke(FX_F_HOOKED, FX_HOOK_WORD); }, "IRQ_CHECK", false},
    {"a blr to an LR its frame wasn't entered with", FX_F_LR, 0, nullptr, nullptr, false},
    {"a loose frame's tail call to an HLE'd function", FX_BLOB_TAIL, FX_F_BLOB_TAIL_TWIN, nullptr, "hle_made_up_save",
     true},
    {"a loose frame's bl and bcl to the next word, against the hardware's", FX_BLOB_NEXT, FX_F_BLOB_NEXT_TWIN,
     nullptr, nullptr, true},
    // This pins native's behaviour today, which calls the next word as a mid-entry and then runs it again
    // once that returns. F2a changes native to set LR and run on, which flips this case.
    {"a function's bl to the next word, which native runs twice", FX_F_BL_NEXT, 0, nullptr, nullptr, false},
    {"an rfi, which returns", FX_F_RFI, 0, nullptr, "hle_rfi", false},
    {"a bdnz to itself, with an IRQ_CHECK each time", FX_F_BDNZ, 0, nullptr, "IRQ_CHECK", false},
    {"a loose frame's branch onto a patch site", FX_BLOB_SITE, FX_F_BLOB_SITE_TWIN, nullptr, "heap_trace_alloc", true},
    {"running off the end into a gap no function holds", FX_F_GAP_FALLS, 0, nullptr, "unimplemented instruction",
     false},
    {"a setjmp site taken 10,000 times in a loop, in one C frame", FX_F_SJ_LOOP, 0, nullptr, "hle_setjmp_jmpbuf",
     false},
};

// The cases' runs left one line for the site whose word changed and one for the blr that returned to
// an LR its frame wasn't entered with, though each ran twice interpreted. The mid-entry frame that a
// function's bl to the next word starts returns to its caller's LR, which leaves a third.
void check_logs() {
    char blr[160], site[160], next[160];
    snprintf(site, sizeof site, "site at %08X holds %08X, not the %08X its patch replaced, so that word runs",
             FX_F_SITE2_SITE, FX_SITE2_WORD, peek(FX_F_SITE2_SITE));
    snprintf(blr, sizeof blr, "blr at %08X returns to %08X, and its frame was entered with LR %08X", FX_F_LR_RET,
             kLR + 8, kLR);
    // f_bl_next's blr is four words after the mid-entry.
    snprintf(next, sizeof next, "blr at %08X returns to %08X, and its frame was entered with LR %08X",
             FX_F_BL_NEXT_N1 + 16, kLR, FX_F_BL_NEXT_N1);
    const std::vector<std::string> want = {site, blr, next};
    if (g_logs != want) {
        std::string got, wanted;
        for (const std::string& s : g_logs) got += "  " + s + "\n";
        for (const std::string& s : want) wanted += (wanted.empty() ? "  " : "\n  ") + s;
        fail("--log=interp printed\n" + got + "where it should print\n" + wanted);
    } else {
        printf("match  --log=interp's three lines, once each\n");
    }
}

void stop_case(const char* what, uint32_t entry, void (*setup)(CPU*), const std::string& want) {
    reset_ram();
    base_cpu(&g_cpu);
    g_calls.clear();
    g_jmps.clear();
    g_longjmp_returns = false;
    if (setup) setup(&g_cpu);
    const uint32_t n = range_of(entry);
    code_table_mark(n, CODE_FORCED, true);
    const std::string stop = call(entry);
    code_table_mark(n, CODE_FORCED, false);
    g_longjmp_returns = false;
    if (stop.find(want) == std::string::npos) fail(std::string(what) + ": stopped with \"" + stop + "\", not \"" + want + "\"");
    else printf("match  %s\n", what);
}

void check_stops() {
    char want[160];
    snprintf(want, sizeof want, "the interpreted longjmp made_up_longjmp at %08X returned", FX_MADE_UP_LONGJMP);
    stop_case("a longjmp frame that would return stops", FX_MADE_UP_LONGJMP,
              [](CPU* c) {
                  g_longjmp_returns = true;
                  c->r[3] = FX_JMP_BUF;
              },
              want);
    snprintf(want, sizeof want, "the patch at %08X has a goto", FX_F_GOTO_SITE);
    stop_case("a patch with a goto stops", FX_F_GOTO, nullptr, want);
}

}  // namespace

int main(int argc, char**) {
    if (argc != 1) {
        fprintf(stderr, "usage: test_interp_run\n");
        return 2;
    }
    if (!runner_init("test_interp_run")) return 2;
#ifdef GCN_CALL_TRACE
    printf("with GCN_TRACE_CALLS: each run's call stack has to balance\n");
#endif

    for (const Case& k : kCases) check_case(k);
    check_logs();
    check_stops();
    return runner_finish();
}
