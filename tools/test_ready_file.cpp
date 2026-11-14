// The ready file's checks: the `test_ready_file` target. Runs with no window, GL, disc or game.
//
// runtime/ready_file.cpp writes GCN_READY_FILE's lines through a temporary file that then
// replaces it, so a launcher watching it sees each version whole. These check the lines and
// their order, that each word goes in once, that one that can't be written says so once and
// isn't tried again for a second, that a reader polling the file meanwhile never
// sees a line cut short, that no line waits for a sync, that a run starts the file afresh,
// what a console reset leaves for the run after, and that the times count from the process's
// start. It prints what a line costs.
//
// usage: test_ready_file WORKDIR
// Exits 0 only if every check passes.
#include "ready_file.h"
#include "platform.h"
#include "test_sync_spy.h"  // seen_sync and g_syncs
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

std::string slurp(const std::string& path) {
    std::string s;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        fclose(f);
    }
    return s;
}

bool exists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f) fclose(f);
    return f != nullptr;
}

// The lines said on stderr while `fn` runs, which go to `path`.
template <class Fn>
int lines_said(const std::string& path, Fn fn) {
    fflush(stderr);
    const int saved = dup(2);
    FILE* f = freopen(path.c_str(), "w", stderr);
    fn();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    if (f) clearerr(stderr);
    int lines = 0;
    for (char c : slurp(path)) lines += c == '\n';
    return lines;
}

}  // namespace

int main(int argc, char** argv) {
    const auto t_main = std::chrono::steady_clock::now();
    if (argc != 2) {
        fprintf(stderr, "usage: test_ready_file WORKDIR\n");
        return 2;
    }
    const std::string dir = argv[1];
    plat_make_dirs(dir);

    const std::string a = dir + "/a.txt";
    check(ready_file_append(a.c_str(), "window", 12), "a first line is written");
    check(slurp(a) == "window 12\n", "as the word, a space, the milliseconds and a newline");
    check(ready_file_append(a.c_str(), "frame", 345), "a second line is written");
    check(slurp(a) == "window 12\nframe 345\n", "after the first, which stays");
    check(!exists(a + ".tmp"), "and no temporary file is left");

    // An append keeps what the file held; a start, a run's first line, replaces it.
    const std::string b = dir + "/b.txt";
    if (FILE* f = fopen(b.c_str(), "wb")) {
        fputs("launch 0\n", f);
        fclose(f);
    }
    ready_file_append(b.c_str(), "window", 7);
    check(slurp(b) == "launch 0\nwindow 7\n", "what the file held is kept");
    check(ready_file_start(b.c_str(), "window", 3) && slurp(b) == "window 3\n", "and a start replaces it");

    check(!ready_file_append((dir + "/no/such/dir/c.txt").c_str(), "window", 1),
          "a path that can't be written says so, and nothing more happens");

    // A reader polling the file while lines go in sees only whole ones.
    const std::string d = dir + "/d.txt";
    std::atomic<bool> done{false};
    std::atomic<int> reads{0}, torn{0};
    std::thread reader([&] {
        while (!done) {
            const std::string s = slurp(d);
            reads++;
            if (!s.empty() && s.back() != '\n') torn++;
        }
    });
    for (int i = 0; i < 300; i++) ready_file_append(d.c_str(), i % 2 ? "frame" : "window", i);
    done = true;
    reader.join();
    check(torn == 0 && reads > 0, "a reader never sees part of a line");

    // A line goes in on the main thread, so it waits for no sync.
    g_synced = dir + "/s.txt";
    plat_sync_for_tests = seen_sync;
    ready_file_append(g_synced.c_str(), "window", 1);
    plat_sync_for_tests = nullptr;
    check(g_syncs == 0 && slurp(g_synced) == "window 1\n", "a line is written with no sync");
    // What one costs on this machine's disk, the system's sync left in.
    const std::string t = dir + "/t.txt";
    double ms[50];
    for (double& m : ms) {
        const auto t0 = std::chrono::steady_clock::now();
        ready_file_append(t.c_str(), "frame", 2);
        m = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    std::sort(std::begin(ms), std::end(ms));
    printf("[ready] a line takes a median of %.3f ms, at most %.3f, over %zu\n", ms[25], ms[49], std::size(ms));

    // GCN_READY_FILE: each word once, with the time since the process started. Its directory
    // comes only after the first marks, so those fail, and a second after the first the word is
    // tried again; then it holds an earlier run's lines, which the run's first line replaces.
    const std::string later = dir + "/later", e = later + "/e.txt";
    setenv("GCN_READY_FILE", e.c_str(), 1);
    const double before = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_main).count();
    // Marks at times well before now, so the ones at now are past every wait.
    const long long ago = (long long)plat_ms_since_start() - 10000;
    const int said = lines_said(dir + "/said.txt", [&] {
        ready_mark_at("window", ago);
        ready_mark_at("window", ago + 999);
        ready_mark_at("window", ago + 1000);
    });
    check(!exists(e) && said == 1, "a mark whose line can't be written writes nothing, and says so once however often it's tried");
    plat_make_dirs(later);
    if (FILE* f = fopen(e.c_str(), "wb")) {
        fputs("window 1\nframe 2\n", f);
        fclose(f);
    }
    ready_mark_at("window", ago + 1999);
    check(slurp(e) == "window 1\nframe 2\n", "a word whose write failed isn't tried again for a second");
    ready_mark("window");
    ready_mark("window");
    ready_relaunching();
    check(getenv("GCN_READY_FILE"), "a console reset before the frame line leaves the variable for the run after");
    ready_mark("frame");
    ready_mark("frame");
    const std::string got = slurp(e);
    long long w = -1, fr = -1;
    const bool parsed = sscanf(got.c_str(), "window %lld\nframe %lld\n", &w, &fr) == 2;
    int lines = 0;
    for (char c : got) lines += c == '\n';
    check(parsed && lines == 2,
          "GCN_READY_FILE gets window once its write works, over the earlier run's lines, then frame, each once");
    check(parsed && w <= fr, "the frame's time is no earlier than the window's");
    // The process started before main did, and this run is well under a minute.
    check(parsed && (double)w + 1.0 >= before && fr < 60000, "the times count from the process's start");
    check(plat_ms_since_start() >= before, "plat_ms_since_start counts from before main");

    // A console reset after both lines: the run after writes no second pair.
    ready_relaunching();
    check(!getenv("GCN_READY_FILE"), "a console reset after the frame line takes the variable away");

    return g_failed ? 1 : 0;
}
