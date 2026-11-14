// Supply the renderer's presented count without a disc or GL window.
#include "test_ports_support.h"

void progress_frames(const std::string& dir) {
    const std::string out = dir + "/progress-stderr.txt";
    const uint32_t was = g_presented;
    auto frames = [](uint32_t first, uint32_t last) {
        for (uint32_t frame = first; frame <= last; frame++) {
            g_presented = frame;
            if (g_progress_every) progress_presented(gx::present_count());
        }
    };

    unsetenv("GCN_PROGRESS");
    capture(out, [&] { progress_load(); frames(1, 8); });
    check(read_file(out).empty(), "unset GCN_PROGRESS says nothing on presented frames");

    setenv("GCN_PROGRESS", "3", 1);
    capture(out, [&] { progress_load(); frames(1, 8); });
    check(read_file(out) == "[progress] frame 3\n[progress] frame 6\n",
          "GCN_PROGRESS=3 prints exactly the third and sixth presented counts");

    setenv("GCN_PROGRESS", "1", 1);
    capture(out, [&] { progress_load(); frames(1, 2); });
    check(read_file(out) == "[progress] frame 1\n[progress] frame 2\n",
          "GCN_PROGRESS=1 prints each presented count starting at one");

    setenv("GCN_PROGRESS", "4294967295", 1);
    capture(out, [&] {
        progress_load();
        g_presented = UINT32_MAX - 1;
        if (g_progress_every) progress_presented(gx::present_count());
        g_presented = UINT32_MAX;
        if (g_progress_every) progress_presented(gx::present_count());
    });
    check(read_file(out) == "[progress] frame 4294967295\n",
          "the largest frame interval is accepted without changing the count");

    for (const char* bad : {"0", "-3", "+3", "3x", " 3", "4294967296", "3\n4"}) {
        setenv("GCN_PROGRESS", bad, 1);
        capture(out, [&] { progress_load(); frames(1, 8); });
        check(read_file(out) == "[progress] GCN_PROGRESS isn't a positive frame interval\n",
              "a bad GCN_PROGRESS is refused once and prints no frames");
    }

    setenv("GCN_PROGRESS", "", 1);
    capture(out, [&] { progress_load(); frames(1, 8); });
    check(read_file(out).empty(), "empty GCN_PROGRESS acts as unset");
    unsetenv("GCN_PROGRESS");
    g_presented = was;
}
