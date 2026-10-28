// GCN_SESSION=<dir> (session.h). The switches it turns on are read where they always were;
// this sets them in the environment before anything reads them, and records what a reader
// of the directory needs to know about the run.
#include "session.h"
#include "audio_stats.h"
#include "platform.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

namespace {

std::string g_dir;               // the session's directory, "" without one
std::vector<const char*> g_set;  // the switches this process set itself
std::vector<std::string> g_argv;
time_t g_start;

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// Sets `name` to `value` unless it's set already: a switch set by hand wins.
void set_default(const char* name, const std::string& value) {
    if (getenv(name)) return;
    set_env(name, value.c_str());
    g_set.push_back(name);
}

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            out += esc;
        } else {
            out += (char)c;
        }
    }
    return out + "\"";
}

std::string json_or_null(const std::string& s) { return s.empty() ? "null" : json_string(s); }

// Whether a file can be made in `dir`. plat_make_dirs says nothing when it fails, and the
// recordings that follow open their files without a word.
bool writable(const std::string& dir) {
    const std::string probe = dir + "/.session-write-test";
    FILE* f = fopen(probe.c_str(), "w");
    if (!f) return false;
    fclose(f);
    remove(probe.c_str());
    return true;
}

// The start as local time with its offset from UTC, as ISO 8601 writes it.
std::string start_text() {
    const long offset = plat_utc_offset_seconds();
    const time_t local = g_start + offset;
    char when[32], zone[8];
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", gmtime(&local));
    const long minutes = (offset < 0 ? -offset : offset) / 60;
    snprintf(zone, sizeof zone, "%c%02ld:%02ld", offset < 0 ? '-' : '+', minutes / 60, minutes % 60);
    return std::string(when) + zone;
}

}  // namespace

std::string session_begin(int argc, char** argv) {
    const char* dir = getenv("GCN_SESSION");
    if (!dir || !*dir) return "";
    g_dir = dir;
    // A process that finds the session already begun (the game reset the console, which
    // relaunches it, or the directory was used before) goes on in one of its own inside, so
    // nothing of the first run is written over.
    if (plat_readable((g_dir + "/session.json").c_str())) {
        int n = 2;
        while (plat_readable((g_dir + "/run-" + std::to_string(n) + "/session.json").c_str())) n++;
        g_dir += "/run-" + std::to_string(n);
    }
    plat_make_dirs(g_dir);
    if (!writable(g_dir)) {
        fprintf(stderr, "[session] can't write %s, so no session is recorded\n", g_dir.c_str());
        g_dir.clear();
        return "";
    }
    set_default("GCN_WAV", g_dir + "/out.wav");
    set_default("GCN_AIDWAV", g_dir + "/aid.raw");
    set_default("GCN_AXSTATS", "1");
    set_default("GCN_AUDIOSTATS", "1");
    set_default("GCN_FRAMETIME", "1");
    g_start = time(nullptr);
    g_argv.assign(argv, argv + argc);
    fprintf(stderr, "[session] %s\n", g_dir.c_str());
    return g_dir + "/inputs";
}

void session_write(double refresh_hz, const char* engine_sha, bool fast_disc) {
    if (g_dir.empty()) return;
    const std::string path = g_dir + "/session.json";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "[session] can't write %s\n", path.c_str());
        return;
    }
    fprintf(f, "{\n  \"start\": %s,\n  \"start_epoch\": %lld,\n", json_string(start_text()).c_str(), (long long)g_start);
    fprintf(f, "  \"host\": {\"os\": %s, \"cpu\": %s, \"refresh_hz\": ", json_or_null(plat_os_version()).c_str(),
            json_or_null(plat_cpu_model()).c_str());
    if (refresh_hz > 0) fprintf(f, "%g},\n", refresh_hz);
    else fprintf(f, "null},\n");
    fprintf(f, "  \"engine_sha\": %s,\n", json_or_null(engine_sha ? engine_sha : "").c_str());
    fprintf(f, "  \"disc\": \"%s\",\n", fast_disc ? "fast" : "console");
    if (const AudioDeviceInfo* d = audio_stats_device())
        fprintf(f, "  \"audio_device\": {\"driver\": %s, \"rate\": %d, \"channels\": %d, \"buffer\": %d},\n",
                json_string(d->driver).c_str(), d->rate, d->channels, d->buffer);
    else
        fprintf(f, "  \"audio_device\": null,\n");
    fprintf(f, "  \"command_line\": [");
    for (size_t i = 0; i < g_argv.size(); i++) fprintf(f, "%s%s", i ? ", " : "", json_string(g_argv[i]).c_str());
    fprintf(f, "]\n}\n");
    fclose(f);
}

void session_relaunching() {
    for (const char* name : g_set) {
#ifdef _WIN32
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }
    g_set.clear();
}
