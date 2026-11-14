// The ready file's checks: the `test_ready_file` target. Runs with no window, GL, disc or game.
//
// runtime/ready_file.cpp writes GCN_READY_FILE's lines through a temporary file that then
// replaces it, so a launcher watching it sees each version whole. These check the lines and
// their order, that each word goes in once, that one that can't be written says so once, is
// tried again at each mark for the 250 ms after that failure and then a second later and no
// sooner, and keeps its first mark's time, that a line that can't replace the file says so
// unless asked not to, as does one whose file is there but can't be read, that a reader
// polling the file meanwhile never sees a line cut short, that no line waits for a sync, that a
// run starts the file afresh, what a console reset leaves for the run after, and that the
// times count from the process's start. It prints what a line costs.
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
#include <sys/stat.h>
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
    // A directory with a file in it: its .tmp is written, but can't replace it. A start, which
    // reads nothing first, since an append can't read a directory.
    const std::string full = dir + "/full";
    plat_make_dirs(full + "/in");
    bool replaced = true;
    const int loud = lines_said(dir + "/full-said.txt", [&] { replaced = ready_file_start(full.c_str(), "window", 1); });
    const bool failed = !replaced && slurp(dir + "/full-said.txt").find("the save failed") != std::string::npos;
    const int quiet = lines_said(dir + "/quiet-said.txt", [&] { replaced = ready_file_start(full.c_str(), "window", 1, false); });
    check(failed && !replaced && loud == 1 && quiet == 0 && !exists(full + ".tmp"),
          "one whose line can't replace the file says so in one line, or in none when asked not to, and leaves no .tmp");
    // A file there that can't be read, a link to itself, isn't taken for an empty one for the
    // line to replace.
    const std::string loop = dir + "/loop.txt";
    symlink("loop.txt", loop.c_str());
    bool appended = true;
    const int unread = lines_said(dir + "/loop-said.txt", [&] { appended = ready_file_append(loop.c_str(), "window", 1); });
    const bool told = slurp(dir + "/loop-said.txt").find("can't read it") != std::string::npos;
    const int unread_quiet =
        lines_said(dir + "/loop-quiet.txt", [&] { appended = appended || ready_file_append(loop.c_str(), "window", 1, false); });
    struct stat link;
    check(!appended && told && unread == 1 && unread_quiet == 0 && lstat(loop.c_str(), &link) == 0 && S_ISLNK(link.st_mode),
          "a file there that can't be read says so in one line, or in none when asked not to, and is left as it was");

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

    // GCN_READY_FILE: each word once, with the time of its first mark since the process
    // started. The directory comes only after window's first marks, so those fail, each mark
    // tried for 250 ms and then once a second; by then it holds an earlier run's lines, which
    // window's line replaces. frame's first mark, at the clock, waits for window, and its first
    // try comes two seconds after it: a directory where the file's .tmp goes keeps that try out,
    // as it would for any user, root too, and for a mark 249 ms later. A failed try inside the
    // 250 ms after the failure leaves the next mark free to try, and that one writes it.
    const std::string later = dir + "/later", e = later + "/e.txt";
    setenv("GCN_READY_FILE", e.c_str(), 1);
    const double before = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_main).count();
    // Marks at times well before now, so the ones at now are past every wait.
    const long long ago = (long long)plat_ms_since_start() - 10000;
    const int said = lines_said(dir + "/said.txt", [&] {
        ready_mark_at("window", ago);
        ready_mark_at("window", ago + 100);
        ready_mark_at("window", ago + 250);
    });
    check(!exists(e) && said == 1, "a mark whose line can't be written writes nothing, and says so once however often it's tried");
    plat_make_dirs(later);
    if (FILE* f = fopen(e.c_str(), "wb")) {
        fputs("window 1\nframe 2\n", f);
        fclose(f);
    }
    const long long frame_from = (long long)plat_ms_since_start();
    ready_mark("frame");
    const long long frame_to = (long long)plat_ms_since_start();
    check(slurp(e) == "window 1\nframe 2\n", "frame waits until window is in, so the lines stay in order");
    ready_mark_at("window", ago + 1249);
    check(slurp(e) == "window 1\nframe 2\n", "a word still failing 250 ms after its first try isn't tried again for a second");
    ready_mark_at("window", ago + 1250);
    check(slurp(e) == "window " + std::to_string(ago) + "\n",
          "a second after its last try it's tried again, and its line, with its first mark's time, replaces the earlier run's");
    ready_relaunching();
    check(getenv("GCN_READY_FILE"), "a console reset before the frame line leaves the variable for the run after");
    mkdir((e + ".tmp").c_str(), 0755);
    const int frame_said = lines_said(dir + "/frame-said.txt", [&] {
        ready_mark_at("frame", frame_to + 2000);
        ready_mark_at("frame", frame_to + 2249);
    });
    rmdir((e + ".tmp").c_str());
    const bool held = slurp(e) == "window " + std::to_string(ago) + "\n";
    ready_mark_at("frame", frame_to + 2260);
    check(held && frame_said == 1 && slurp(e).find("\nframe ") != std::string::npos,
          "a word whose write failed is tried again at each mark for 250 ms from that failure, not from its first mark");
    ready_mark("window");
    ready_mark("frame");
    const std::string got = slurp(e);
    long long w = -1, fr = -1;
    const bool parsed = sscanf(got.c_str(), "window %lld\nframe %lld\n", &w, &fr) == 2;
    int lines = 0;
    for (char c : got) lines += c == '\n';
    check(parsed && lines == 2 && w == ago, "GCN_READY_FILE gets window once, then frame, each once");
    check(parsed && w <= fr, "the frame's time is no earlier than the window's");
    // frame's first mark was at plat_ms_since_start, and its line keeps that time, not its
    // first try's. The process started before main did, and this run is well under a minute.
    check(parsed && fr >= frame_from && fr <= frame_to && (double)fr + 1.0 >= before && fr < 60000,
          "frame's line keeps its first mark's time, which counts from the process's start");
    check(plat_ms_since_start() >= before, "plat_ms_since_start counts from before main");

    // A console reset after both lines: the run after writes no second pair.
    ready_relaunching();
    check(!getenv("GCN_READY_FILE"), "a console reset after the frame line takes the variable away");

    return g_failed ? 1 : 0;
}
