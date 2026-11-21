// Gekko's non-IEEE mode on the host thread: the `test_fp_mode` target. Runs with no device,
// disc or game.
//
// runtime/cpu.cpp is compiled into this file, as in test_clock.cpp, so hle_set_fpscr is the
// runtime's own. An FPSCR write with NI set turns the host thread's flush mode on, and one
// without it turns it off (recomp.h's gcn_fp_follow), so the smallest double denormal times
// 2^52 is then 0 or 2^-1022. platform.cpp's plat_thread_role takes every thread out of the
// mode, since on Linux a thread starts in its creator's and a render thread a guest thread
// spawned would flush. A host recomp.h sets no flush mode on passes with a note.
//
// usage: test_fp_mode
// Exits 0 only if every check passes.
#include "cpu.cpp"
#include "platform.h"

// What cpu.cpp's kept code calls elsewhere in the runtime, as in test_clock.cpp.
std::atomic<uint64_t> g_dec_deadline{UINT64_MAX};
uint64_t g_aid_frames_delivered;
uint32_t gx_frames_submitted() { return 0; }
extern "C" {
const RecompFunc g_recomp_funcs[1] = {};
const uint32_t g_recomp_func_count = 0;
}
void modules_init() {}
bool input_replay_active() { return false; }
void input_log_jump(uint64_t, uint64_t) {}
uint64_t input_replay_next_jump_at() { return UINT64_MAX; }
uint64_t input_replay_take_jump(uint64_t) { return 0; }

namespace {

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

// Through volatiles, so the compiler can't work the product out as it builds.
volatile uint64_t g_least_bits = 1;  // 2^-1074
volatile double g_scale = 0x1p52;

uint64_t product() {
    double least;
    const uint64_t bits = g_least_bits;
    memcpy(&least, &bits, 8);
    const double p = least * g_scale;
    uint64_t out;
    memcpy(&out, &p, 8);
    return out;
}

}  // namespace

int main() {
#if !(defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))) && !defined(__x86_64__) && !defined(_M_X64)
    printf("ok: recomp.h sets no flush mode on this host, so there's nothing to check\n");
    return 0;
#else
    CPU c{};
    hle_set_fpscr(&c, FPSCR_NI);
    check(gcn_fp_flushing() == 1 && c.fpscr == FPSCR_NI, "hle_set_fpscr with NI turns the flush mode on");
    check(product() == 0, "in non-IEEE mode the least double denormal times 2^52 is zero");
    hle_set_fpscr(&c, 0x80000000u);
    check(gcn_fp_flushing() == 0, "hle_set_fpscr without NI turns it off, whatever else it sets");
    check(product() == 0x0010000000000000ull, "in IEEE mode the same product is 2^-1022");

    const ThreadRole roles[] = {ThreadRole::GuestMain, ThreadRole::GuestWorker, ThreadRole::RenderMain,
                                ThreadRole::RenderWorker};
    bool all_cleared = true;
    for (ThreadRole role : roles) {
        hle_set_fpscr(&c, FPSCR_NI);
        plat_thread_role(role, "test_fp_mode");
        all_cleared = all_cleared && gcn_fp_flushing() == 0;
    }
    check(all_cleared, "plat_thread_role takes a thread of every role out of the flush mode");
    return g_failed ? 1 : 0;
#endif
}
