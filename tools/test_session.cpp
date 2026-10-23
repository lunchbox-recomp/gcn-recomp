// GCN_SESSION checks: the `test_session` target. Runs with no disc image, device, SDL or game.
// runtime/session.cpp is linked as it is, with audio_stats.cpp (the device it records) and
// platform.cpp (the host it records and the directory it makes).
//
// One process plays the part of each the session sees. The first starts a session in a new
// directory with one switch set by hand, and must leave that switch alone, turn on the others,
// send the input log into the directory and write session.json: the start with its offset
// from UTC, the host, the engine's commit, the sound device and the command line, whose
// quotes, backslashes and control characters have to be escaped. The game resetting the
// console relaunches it, so the switches the session set are cleared and a second and third
// process go on in run-2 and run-3 without touching the first's files. Without GCN_SESSION
// nothing is set or written. A directory that can't be written must be said out loud and
// leave no session, where the recordings after it would fail without a word.
//
// usage: test_session <work dir>
// The work dir must not exist. It's made, and the files are left in it. Exits 0 only if
// every check passes.
#include "audio_stats.h"
#include "platform.h"
#include "session.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

int g_checks, g_failed;
std::string g_dir;

void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}

// The switch's value, or "<unset>".
std::string env(const char* name) {
    const char* v = getenv(name);
    return v ? v : "<unset>";
}

// The file's bytes, or "<missing>".
std::string read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "<missing>";
    std::string text;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    return text;
}

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// The first line of `text` that begins with `start`, without its newline, or "<none>".
std::string line_starting(const std::string& text, const std::string& start) {
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (text.compare(at, start.size(), start) == 0) return text.substr(at, end - at);
        at = end + 1;
    }
    return "<none>";
}

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// What stderr got while `run` ran.
template <class F>
std::string stderr_during(F run) {
    const std::string path = g_dir + "/stderr.txt";
    fflush(stderr);
    const int saved = dup(2);
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, 2);
    close(fd);
    run();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    return read_file(path);
}

void set_env(const char* name, const std::string& value) { setenv(name, value.c_str(), 1); }

// The session's switches, none of them set.
void clear_switches() {
    for (const char* name : {"GCN_WAV", "GCN_AIDWAV", "GCN_AXSTATS", "GCN_AUDIOSTATS", "GCN_FRAMETIME"}) unsetenv(name);
}

// The start in session.json as seconds since 1970 by its own local time and offset, and the
// start_epoch beside it, or false if either line doesn't read.
bool start_times(const std::string& json, long long& from_text, long long& epoch) {
    const std::string start = line_starting(json, "  \"start\": ");
    const std::string stamp = line_starting(json, "  \"start_epoch\": ");
    struct tm t = {};
    char sign;
    int zone_h, zone_m;
    if (sscanf(start.c_str(), "  \"start\": \"%d-%d-%dT%d:%d:%d%c%d:%d\"", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour,
               &t.tm_min, &t.tm_sec, &sign, &zone_h, &zone_m) != 9 ||
        sscanf(stamp.c_str(), "  \"start_epoch\": %lld", &epoch) != 1)
        return false;
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    const long offset = (sign == '-' ? -1 : 1) * (zone_h * 3600L + zone_m * 60L);
    from_text = (long long)timegm(&t) - offset;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_session <work dir>\n");
        return 2;
    }
    g_dir = argv[1];
    if (exists(g_dir) || mkdir(g_dir.c_str(), 0755) != 0) {
        fprintf(stderr, "%s exists or can't be made: give a new directory\n", g_dir.c_str());
        return 2;
    }
    char a0[] = "game", a1[] = "--hidden", a2[] = "a\"b\\c", a3[] = "tab\there";
    char* args[] = {a0, a1, a2, a3};
    const int nargs = 4;

    // No session.
    clear_switches();
    unsetenv("GCN_SESSION");
    check(session_begin(nargs, args).empty() && env("GCN_WAV") == "<unset>" && env("GCN_AXSTATS") == "<unset>",
          "without GCN_SESSION nothing is set and no input directory is given");
    set_env("GCN_SESSION", "");
    check(session_begin(nargs, args).empty() && env("GCN_AIDWAV") == "<unset>", "an empty GCN_SESSION is no session");
    session_write(60, "sha");  // writes nothing, anywhere

    // The first process: one switch set by hand, one set to a value the session would not.
    const std::string dir = g_dir + "/sess";
    set_env("GCN_SESSION", dir);
    set_env("GCN_WAV", "hand.wav");
    set_env("GCN_FRAMETIME", "2");
    const time_t before = time(nullptr);
    const std::string inputs = session_begin(nargs, args);
    const time_t after = time(nullptr);
    check(inputs == dir + "/inputs", "the input log goes to <dir>/inputs");
    check(exists(dir), "the directory is made");
    check(env("GCN_WAV") == "hand.wav" && env("GCN_FRAMETIME") == "2", "a switch set by hand keeps its value");
    check(env("GCN_AIDWAV") == dir + "/aid.raw", "GCN_AIDWAV points into the directory");
    check(env("GCN_AXSTATS") == "1" && env("GCN_AUDIOSTATS") == "1", "the stats switches are on");

    // session.json before the sound device has opened and with nothing known of the display
    // or the engine. TZ is set to a zone without daylight saving, 7 hours behind UTC.
    setenv("TZ", "MST7", 1);
    tzset();
    session_write(0, "");
    std::string json = read_file(dir + "/session.json");
    check(json != "<missing>", "session.json is written");
    check(line_starting(json, "  \"audio_device\": ") == "  \"audio_device\": null,",
          "the sound device is null before it has opened");
    check(ends_with(line_starting(json, "  \"host\": "), "\"refresh_hz\": null},"), "an unknown refresh rate is null");
    check(line_starting(json, "  \"engine_sha\": ") == "  \"engine_sha\": null,", "an unknown engine commit is null");

    // Then with all of it known. The commit is made up with a quote, a backslash and a newline in it.
    audio_stats_opened("dummy", 48000, 2, 512);
    session_write(119.88, "abc\"def\\\n");
    json = read_file(dir + "/session.json");
    check(line_starting(json, "  \"audio_device\": ") ==
              "  \"audio_device\": {\"driver\": \"dummy\", \"rate\": 48000, \"channels\": 2, \"buffer\": 512},",
          "the sound device gives its driver, rate, channels and buffer");
    check(ends_with(line_starting(json, "  \"host\": "), "\"refresh_hz\": 119.88},"), "the refresh rate is written");
    check(line_starting(json, "  \"engine_sha\": ") == R"j(  "engine_sha": "abc\"def\\\u000a",)j",
          "a quote, a backslash and a newline in the commit are escaped");
    check(line_starting(json, "  \"command_line\": ") ==
              R"j(  "command_line": ["game", "--hidden", "a\"b\\c", "tab\u0009here"])j",
          "the command line is written with its quotes, backslash and tab escaped");
    long long from_text = 0, epoch = 0;
    const bool read = start_times(json, from_text, epoch);
    check(read && line_starting(json, "  \"start\": ").find("-07:00\"") != std::string::npos,
          "the start is local time with its offset from UTC, 7 hours behind here");
    check(read && from_text == epoch, "the start and start_epoch are the same moment");
    check(read && epoch >= before && epoch <= after, "start_epoch is when the process began");

    // A zone ahead of UTC by a half hour.
    setenv("TZ", "IST-5:30", 1);
    tzset();
    session_write(0, "");
    json = read_file(dir + "/session.json");
    check(start_times(json, from_text, epoch) && line_starting(json, "  \"start\": ").find("+05:30\"") != std::string::npos &&
              from_text == epoch,
          "a zone 5 hours 30 minutes ahead reads +05:30 and is the same moment");
    const std::string first_json = json;

    // The game resets the console: the session's switches are cleared and the process starts
    // again, and goes on in a directory of its own.
    session_relaunching();
    check(env("GCN_AIDWAV") == "<unset>" && env("GCN_AXSTATS") == "<unset>" && env("GCN_AUDIOSTATS") == "<unset>",
          "a relaunch clears the switches the session set");
    check(env("GCN_WAV") == "hand.wav" && env("GCN_FRAMETIME") == "2", "a relaunch leaves the switches set by hand");
    check(env("GCN_SESSION") == dir, "a relaunch keeps GCN_SESSION");
    check(session_begin(nargs, args) == dir + "/run-2/inputs", "the second process goes on in <dir>/run-2");
    check(env("GCN_AIDWAV") == dir + "/run-2/aid.raw", "and points GCN_AIDWAV into it");
    session_write(0, "");
    check(exists(dir + "/run-2/session.json"), "and writes its own session.json");
    check(read_file(dir + "/session.json") == first_json, "the first process's session.json is as it was");
    session_relaunching();
    check(session_begin(nargs, args) == dir + "/run-3/inputs", "the third process goes on in <dir>/run-3");

    // A directory that can't be written. A process that is root writes anywhere.
    if (geteuid() == 0) {
        printf("skip root can write any directory, so the unwritable cases aren't run\n");
    } else {
        const std::string ro = g_dir + "/ro";
        mkdir(ro.c_str(), 0555);
        session_relaunching();
        clear_switches();
        set_env("GCN_SESSION", ro);
        std::string got;
        const std::string said = stderr_during([&] { got = session_begin(nargs, args); });
        check(said.find("can't write " + ro) != std::string::npos, "a directory that can't be written is named on stderr");
        check(got.empty(), "and no input directory is given");
        check(env("GCN_WAV") == "<unset>" && env("GCN_AIDWAV") == "<unset>" && env("GCN_AXSTATS") == "<unset>",
              "and no recording switch is turned on");
        session_write(60, "sha");
        check(!exists(ro + "/session.json"), "and no session.json is written");

        set_env("GCN_SESSION", ro + "/new");
        check(session_begin(nargs, args).empty() && !exists(ro + "/new"), "a directory that can't be made is no session either");

        // The next process finds a directory it can write and goes on as usual.
        const std::string again = g_dir + "/again";
        session_relaunching();
        clear_switches();
        set_env("GCN_SESSION", again);
        check(session_begin(nargs, args) == again + "/inputs" && env("GCN_AIDWAV") == again + "/aid.raw",
              "a writable directory after that records as usual");
        chmod(ro.c_str(), 0755);
    }

    printf("test_session: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
