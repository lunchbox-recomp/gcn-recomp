// Virtual clock checks: the `test_clock` target. Runs with no device, disc or game.
//
// runtime/cpu.cpp is compiled into this file, and its clock runs as a replay's would: the
// guest's running moves the back-edge count on, and clock_pace makes the recorded catch-up
// jumps. The AI DMA (hw/dsp.cpp) leaves those jumps out of the time it gives the guest to
// queue its next buffer, taking them from clock_jumped_ticks(). So that total has to be just
// the virtual time the jumps added, at whatever rate the clock runs (clock_set_cpu_scale):
// the clock less the total is then the guest's own running.
//
// Paced, clock_pace holds the guest's time to the host's by waiting whenever the guest gets
// ahead. It sleeps in shrinking steps toward the time and spins the last of it.
//
// Under GCN_CLOCK=host the clock is the host's, less the time pauses held the guest
// (clock_hold), so that the guest doesn't see a pause as time gone by. The last check runs
// that clock.
//
// The clock's switches are read through gcn_env (runtime.h), which takes an empty value as
// unset. The first checks cover it, GCN_TIMESCALE as timing_init reads it, and GCN_CATCHUP
// and GCN_CLOCKLOG, which are read once a process and so are each tried in a child of its own.
//
// usage: test_clock
// Exits 0 only if every check passes.
#include "cpu.cpp"
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

// What cpu.cpp's clock calls elsewhere in the runtime. The input log is a replay with one
// jump pending at a time. timing_init builds the call table, which here has no functions.
std::atomic<uint64_t> g_dec_deadline{UINT64_MAX};
uint64_t g_aid_frames_delivered;
uint32_t gx_frames_submitted() { return 0; }
extern "C" {
const RecompFunc g_recomp_funcs[1] = {};
const uint32_t g_recomp_func_count = 0;
}
void modules_init() {}
static uint64_t g_replay_at = UINT64_MAX, g_replay_edges;
bool input_replay_active() { return true; }
void input_log_jump(uint64_t, uint64_t) {}
uint64_t input_replay_next_jump_at() { return g_replay_at; }
uint64_t input_replay_take_jump(uint64_t count) {
    if (count < g_replay_at) return 0;
    g_replay_at = UINT64_MAX;
    return g_replay_edges;
}

namespace {

int g_failed;
uint64_t g_ran;  // the guest's own running, in ticks

void check(bool ok, const char* what, double scale) {
    printf("%s: %s, at CPU scale %g\n", ok ? "ok" : "FAIL", what, scale);
    if (!ok) g_failed++;
}

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

// What `body` did in a child process with `name` set to `value`, or unset for null: its exit
// status (-1 if it didn't exit) and what it wrote to stderr.
struct Child {
    int status = -1;
    std::string said;
};
Child in_child(const char* name, const char* value, const std::function<int()>& body) {
    Child out;
    int fds[2];
    if (pipe(fds) != 0) return out;
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        if (value) setenv(name, value, 1);
        else unsetenv(name);
        const int code = body();
        fflush(stderr);
        _exit(code);
    }
    close(fds[1]);
    char buf[256];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;) out.said.append(buf, (size_t)n);
    close(fds[0]);
    int status = 0;
    if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status)) out.status = WEXITSTATUS(status);
    return out;
}

// The guest runs `edges` loop iterations.
void run(uint64_t edges) {
    const uint64_t t = now_ticks();
    g_vcount += edges;
    g_ran += now_ticks() - t;
}

}  // namespace

int main(int argc, char** argv) {
    // A child started again as `test_clock catchup` says whether catch-up is on, which
    // cpu.cpp decides as the process starts.
    if (argc == 2 && strcmp(argv[1], "catchup") == 0) return g_catchup ? 0 : 1;

    unsetenv("GCN_TEST_EMPTY");
    const bool unset_none = gcn_env("GCN_TEST_EMPTY") == nullptr;
    setenv("GCN_TEST_EMPTY", "", 1);
    const bool empty_none = gcn_env("GCN_TEST_EMPTY") == nullptr;
    setenv("GCN_TEST_EMPTY", "0", 1);
    const char* zero = gcn_env("GCN_TEST_EMPTY");
    const bool zero_kept = zero && !strcmp(zero, "0");
    unsetenv("GCN_TEST_EMPTY");
    check(unset_none && empty_none && zero_kept,
          "gcn_env has no value for an unset or an empty switch, and gives a set one as it is");

    setenv("GCN_TIMESCALE", "", 1);
    clock_set_scale(1);
    timing_init();
    const bool empty_paced = clock_scale() == 1.0;
    setenv("GCN_TIMESCALE", "0", 1);
    timing_init();
    const bool zero_unpaced = clock_scale() == 0.0;
    unsetenv("GCN_TIMESCALE");
    check(empty_paced && zero_unpaced, "an empty GCN_TIMESCALE keeps the pace, and GCN_TIMESCALE=0 still turns it off");

    const auto catchup = [&](const char* value) {
        return in_child("GCN_CATCHUP", value, [&] {
                   execl(argv[0], argv[0], "catchup", (char*)nullptr);
                   return 2;
               }).status;
    };
    check(catchup(nullptr) == 0 && catchup("") == 0 && catchup("1") == 0 && catchup("0") == 1,
          "an empty GCN_CATCHUP keeps catch-up on, as an unset one does, and GCN_CATCHUP=0 turns it off");

    const auto clocklog = [](const char* value) {
        return in_child("GCN_CLOCKLOG", value, [] {
            clock_log();
            return 0;
        });
    };
    const Child log_unset = clocklog(nullptr), log_empty = clocklog(""), log_on = clocklog("1");
    check(log_unset.status == 0 && log_unset.said.empty() && log_empty.status == 0 && log_empty.said.empty() &&
              log_on.status == 0 && log_on.said.find("[clock] virtual") != std::string::npos,
          "an empty GCN_CLOCKLOG prints nothing, as an unset one does, and GCN_CLOCKLOG=1 prints the clock");

    clock_set_scale(0);  // unpaced, as a --fast replay runs: only the recorded jumps
    for (double scale : {1.0, 2.0, 0.5}) {
        clock_set_cpu_scale(scale);
        run(1000);
        g_replay_at = g_vcount;
        g_replay_edges = 500;
        const uint64_t t = now_ticks(), jumped = clock_jumped_ticks();
        clock_pace();
        const uint64_t added = now_ticks() - t;
        check(added > 0 && clock_jumped_ticks() - jumped == added, "a jump adds to the jumped total what it adds to the clock", scale);
        run(300);
        check(now_ticks() - clock_jumped_ticks() == g_ran, "the clock less the jumped total is the guest's own running", scale);
    }

    // The pacer's sleeps on the way to a time 20 ms off. Each is two thirds of the wait left
    // beyond its last 100 us, capped at 1 ms. One under 50 us is skipped, leaving the spin.
    const uint64_t ms = 1000000, us = 1000;
    const bool steps = pace_sleep_ns(0, 20 * ms) == ms && pace_sleep_ns(19 * ms, 20 * ms) == 600 * us &&
                       pace_sleep_ns(20 * ms - 175 * us, 20 * ms) == 50 * us &&
                       pace_sleep_ns(20 * ms - 174 * us, 20 * ms) == 0 &&
                       pace_sleep_ns(20 * ms - 100 * us, 20 * ms) == 0 && pace_sleep_ns(21 * ms, 20 * ms) == 0;
    printf("%s: the pacer sleeps two thirds of the wait beyond its last 100 us, from 50 us to 1 ms\n",
           steps ? "ok" : "FAIL");
    if (!steps) g_failed++;

    // The paced clock 20 ms ahead of the host, five times. clock_pace returns once the host
    // gets there, and never before.
    clock_set_scale(1);
    g_t0 = std::chrono::steady_clock::now();
    const auto virt_ns = [] { return (int64_t)((double)now_ticks() * (1e9 / TB_FREQ)); };
    bool on_time = true;
    for (int i = 0; i < 5; i++) {
        g_pace_offset_ns = virt_ns() - (int64_t)host_ns();  // the two clocks agree
        g_vcount += (uint64_t)(0.020 * TB_FREQ) / g_ticks_per_edge;
        const int64_t until = virt_ns() - g_pace_offset_ns;
        clock_pace();
        on_time = on_time && (int64_t)host_ns() >= until;
    }
    printf("%s: the pacer waits until the host reaches the guest's time\n", on_time ? "ok" : "FAIL");
    if (!on_time) g_failed++;

    // The host clock 100 s in, and a 1 s hold. The host moves on a little between the reads,
    // so the clock may go back by up to 10 ms less than the hold, and a tick of rounding more.
    g_virtual_clock = false;
    g_t0 = std::chrono::steady_clock::now() - std::chrono::seconds(100);
    const uint64_t held = TB_FREQ, before = now_ticks();
    clock_hold(1000000000);
    const uint64_t after = now_ticks();
    const bool back = after < before && before - after <= held + 1 && before - after >= held - TB_FREQ / 100;
    printf("%s: a pause's hold takes its time out of the host clock\n", back ? "ok" : "FAIL");
    if (!back) g_failed++;

    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
