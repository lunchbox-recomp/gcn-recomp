// The bot link. See bot_link.h.
#include "bot_link.h"
#include "runtime.h"
#include "input_log.h"
#include "platform.h"
#include "gx/render.h"
#include <cstdio>
#include <cstring>

#ifdef _WIN32

bool bot_link_start(const char* path, int, bool) {
    fprintf(stderr, "--bot %s: the bot link needs a Unix socket, which this build doesn't have\n", path);
    return false;
}

void bot_link_apply(PadState&, int) {}

#else

#include <algorithm>
#include <cerrno>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

int g_fd = -1;          // the socket, -1 without a bot
int g_ports;            // the bot drives ports 1 to g_ports
bool g_stepped;         // the bot has been stepped at least once
uint32_t g_frame;       // the frame it was last stepped at
PadState g_state[4];    // what it sent then, for every poll of that frame
std::string g_in;       // what it has sent past the last whole line read

// The longest line taken. A pad line is about 40 characters.
constexpr size_t kMaxLine = 256;
constexpr uint32_t kRamBase = 0x80000000u;  // where the game sees main RAM
constexpr unsigned kMaxRead = 256;
constexpr uint16_t kButtons = 0x1F7F;       // the PADStatus button bits (hw/pad.h)

// Says why on stderr, at the frame being stepped, ends the input log there and ends the run.
[[noreturn]] void stop(const char* fmt, ...) GCN_PRINTF_FMT(1, 2);
void stop(const char* fmt, ...) {
    fprintf(stderr, "[bot] frame %u: ", g_frame);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    input_log_end();
    plat_exit_now(1);
}

[[noreturn]] void bad(const std::string& line, const char* why) { stop("%s: \"%s\"", why, line.c_str()); }

void send_all(const std::string& s) {
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;  // a bot that's gone is EPIPE here, not SIGPIPE
#else
    const int flags = 0;  // SO_NOSIGPIPE, set at the connect, does the same
#endif
    for (size_t at = 0; at < s.size();) {
        const ssize_t n = send(g_fd, s.data() + at, s.size() - at, flags);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EPIPE || errno == ECONNRESET)) stop("the bot hung up");
        if (n < 0) stop("can't send to the bot: %s", strerror(errno));
        at += (size_t)n;
    }
}

// The next line the bot sent, without its newline (or a carriage return before it).
std::string read_line() {
    for (;;) {
        const size_t nl = g_in.find('\n');
        if (nl != std::string::npos) {
            std::string line = g_in.substr(0, nl);
            g_in.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() > kMaxLine) bad(line.substr(0, kMaxLine) + "...", "a line longer than the link takes");
            return line;
        }
        if (g_in.size() > kMaxLine) bad(g_in.substr(0, kMaxLine) + "...", "a line longer than the link takes");
        char buf[4096];
        const ssize_t n = recv(g_fd, buf, sizeof(buf), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0 || (n < 0 && errno == ECONNRESET)) stop("the bot hung up");
        if (n < 0) stop("can't read from the bot: %s", strerror(errno));
        g_in.append(buf, (size_t)n);
    }
}

std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        at = line.find_first_not_of(" \t", at);
        if (at == std::string::npos) return out;
        const size_t end = std::min(line.find_first_of(" \t", at), line.size());
        out.push_back(line.substr(at, end - at));
        at = end;
    }
}

// A number in `base` (10 or 16) of at most `digits` digits, and nothing else.
bool number(const std::string& s, int base, size_t digits, uint32_t& out) {
    if (s.empty() || s.size() > digits) return false;
    out = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        out = out * (uint32_t)base + (uint32_t)d;
    }
    return true;
}

// `read ADDR LEN`: the bytes, in hex.
void answer_read(const std::string& line, const std::vector<std::string>& w) {
    uint32_t addr, len;
    if (w.size() != 3 || !number(w[1], 16, 8, addr) || !number(w[2], 10, 3, len) || len < 1 || len > kMaxRead)
        bad(line, "not read ADDR LEN, ADDR in hex and LEN 1 to 256");
    if (addr < kRamBase || (uint64_t)(addr - kRamBase) + len > RAM_SIZE)
        bad(line, "a read outside RAM, 80000000 to 817FFFFF");
    static const char kHex[] = "0123456789abcdef";
    std::string out = "data ";
    for (const uint8_t* p = g_mem + (addr - kRamBase), *end = p + len; p < end; p++) {
        out.push_back(kHex[*p >> 4]);
        out.push_back(kHex[*p & 15]);
    }
    out.push_back('\n');
    send_all(out);
}

// `pad PORT BUTTONS SX SY CX CY L R`: the state of a port the bot drives.
void take_pad(const std::string& line, const std::vector<std::string>& w, PadState* next, bool* have) {
    uint32_t v[8];
    bool ok = w.size() == 9 && number(w[1], 10, 1, v[0]) && number(w[2], 16, 4, v[1]);
    for (int i = 2; ok && i < 8; i++) ok = number(w[i + 1], 10, 3, v[i]) && v[i] <= 255;
    if (!ok) bad(line, "not pad PORT BUTTONS SX SY CX CY L R, BUTTONS in hex and the rest 0 to 255");
    const uint32_t port = v[0];
    if (port < 1 || (int)port > g_ports) {
        char why[64];
        snprintf(why, sizeof(why), "port %u isn't one the bot drives (--pads=%d)", port, g_ports);
        bad(line, why);
    }
    if (v[1] & ~(uint32_t)kButtons) bad(line, "BUTTONS has bits that aren't buttons (1F7F)");
    if (have[port - 1]) bad(line, "a second pad line for the port this frame");
    have[port - 1] = true;
    PadState& s = next[port - 1];
    s.connected = true;
    s.buttons = (uint16_t)v[1];
    s.stick_x = (uint8_t)v[2];
    s.stick_y = (uint8_t)v[3];
    s.cstick_x = (uint8_t)v[4];
    s.cstick_y = (uint8_t)v[5];
    s.trig_l = (uint8_t)v[6];
    s.trig_r = (uint8_t)v[7];
}

// One step: `frame N`, the bot's reads answered, then its pads up to `done`.
void step(uint32_t frame) {
    g_frame = frame;
    send_all("frame " + std::to_string(frame) + "\n");
    PadState next[4];  // a port the bot doesn't drive stays unplugged
    bool have[4] = {};
    for (;;) {
        const std::string line = read_line();
        const std::vector<std::string> w = words(line);
        if (w.empty()) continue;
        if (w[0] == "read") answer_read(line, w);
        else if (w[0] == "pad") take_pad(line, w, next, have);
        else if (w[0] == "done" && w.size() == 1) {
            for (int port = 1; port <= g_ports; port++)
                if (!have[port - 1]) stop("done without a pad line for port %d, which the bot drives: \"%s\"", port, line.c_str());
            for (int i = 0; i < 4; i++) g_state[i] = next[i];
            return;
        } else {
            bad(line, "not read, pad or done");
        }
    }
}

}  // namespace

bool bot_link_start(const char* path, int ports, bool replaying) {
    if (replaying) {
        fprintf(stderr, "--bot can't go with --replay: a replay already decides the pads\n");
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "--bot %s: a Unix socket's path is %zu bytes at most\n", path, sizeof(addr.sun_path) - 1);
        return false;
    }
    strcpy(addr.sun_path, path);
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "--bot %s: can't make a socket: %s\n", path, strerror(errno));
        return false;
    }
    // The game resetting the console runs it again in this process (plat_relaunch), which
    // connects again; the bot sees this connection end.
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (connect(fd, (const sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "--bot %s: can't connect: %s\n", path, strerror(errno));
        close(fd);
        return false;
    }
    if (g_fd >= 0) close(g_fd);  // a second link, which only the checks make
    g_fd = fd;
    g_ports = ports;
    g_stepped = false;
    g_in.clear();
    for (auto& s : g_state) s = PadState();
    if (ports > 1) fprintf(stderr, "[bot] linked to %s, driving ports 1 to %d\n", path, ports);
    else fprintf(stderr, "[bot] linked to %s, driving port 1\n", path);
    return true;
}

void bot_link_apply(PadState& p, int chan) {
    if (g_fd < 0) return;
    const uint32_t frame = gx::g_frames_submitted.load();
    if (!g_stepped || frame != g_frame) {
        // The guest is held while the bot answers, as by a pause, and the host time that took
        // goes to the clock (clock_hold): the pacer doesn't take it for time to catch up, so
        // how long the bot took changes nothing in the run, paced or not.
        const uint64_t t0 = host_ns();
        step(frame);
        clock_hold(host_ns() - t0);
        g_stepped = true;
    }
    p = g_state[chan];
}

#endif
