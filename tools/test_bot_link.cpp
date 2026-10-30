// Bot link checks: the `test_bot_link` target. Runs with no disc image, device, SDL or game.
//
// runtime/hw/si.cpp is compiled into this file, so each port is read as the guest reads it:
// pad_get, with the bot's state in place of the live one, a script's presses on top and the
// input log recording the result. bot_link.cpp, input_script.cpp, input_log.cpp and
// platform.cpp are linked as they are. Stubbed: the frame counter (gx::g_frames_submitted,
// gx_frames_submitted), which a check sets by hand, guest memory (g_mem), which a check fills,
// the RTC (rtc_set_base), the event queue and the interrupt line (event_schedule_in,
// pi_set_interrupt), the SI's log (g_log_enabled, log_msg) and the disc speed the log's header
// records (g_fast_disc).
//
// This process plays the bot on a Unix socket in the work dir. For a step that goes through,
// the bot's lines wait in the socket before the poll, and what the link sent is read after it.
// One check answers from a thread of its own, late, to show the guest waits for `done`. A step
// that stops the game runs in a child process, which the stop ends: the bot reads the frame
// line, answers, then hangs up or stops sending, and the child's exit status, its stderr and
// the input log's end say what the stop did.
//
// usage: test_bot_link <work dir>
// The work dir must not exist. It's made, and the files are left in it. Exits 0 only if every
// check passes.
#include "hw/si.cpp"
#include "bot_link.h"
#include "gx/render.h"
#include "input_log.h"
#include "input_script.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace gx {
std::atomic<uint32_t> g_frames_submitted{0};
}
uint32_t gx_frames_submitted() { return gx::g_frames_submitted.load(); }
uint8_t* g_mem;
bool g_fast_disc = true;
bool g_log_enabled[LOG_COUNT];
void log_msg(LogCat, const char*, ...) {}
void rtc_set_base(time_t) {}
void event_schedule_in(uint64_t, EventFn) {}
void pi_set_interrupt(uint32_t, bool) {}

namespace {

int g_checks, g_failed;
int g_listener = -1;
const char* const kSocket = "bot.sock";  // in the work dir, which is the working directory
const char* const kLog = "log";

void check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) g_failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    fflush(stdout);
}

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

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// What `fn` writes to stderr.
std::string stderr_of(const std::function<void()>& fn) {
    fflush(stderr);
    const int saved = dup(2);
    const int fd = open("stderr.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, 2);
    close(fd);
    fn();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    return read_file("stderr.txt");
}

// Whether a connection is waiting on the listener.
bool knocked() {
    pollfd p{g_listener, POLLIN, 0};
    return poll(&p, 1, 0) == 1;
}

// A new link driving `ports` ports. Returns the bot's end of it.
int link(int ports) {
    bool ok = false;
    stderr_of([&] { ok = bot_link_start(kSocket, ports, false); });
    const int bot = ok ? accept(g_listener, nullptr, nullptr) : -1;
    if (bot < 0) {
        printf("FAIL the link to %s can't be made\n", kSocket);
        exit(1);
    }
    return bot;
}

void say(int bot, const std::string& lines) {
    for (size_t at = 0; at < lines.size();) {
        const ssize_t n = send(bot, lines.data() + at, lines.size() - at, 0);
        if (n <= 0) return;
        at += (size_t)n;
    }
}

// What the link has sent that's waiting, read without waiting for more.
std::string heard(int bot) {
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = recv(bot, buf, sizeof(buf), MSG_DONTWAIT)) > 0) out.append(buf, (size_t)n);
    return out;
}

// The link's first line, waiting up to five seconds for it.
std::string first_line(int bot) {
    std::string out;
    char c;
    for (;;) {
        pollfd p{bot, POLLIN, 0};
        if (poll(&p, 1, 5000) != 1 || recv(bot, &c, 1, 0) != 1) return out + "<nothing more>";
        out.push_back(c);
        if (c == '\n') return out;
    }
}

PadState poll_at(int chan, uint32_t frame) {
    gx::g_frames_submitted = frame;
    return pad_get(chan);
}

PadState pad(uint16_t buttons, uint8_t sx = 128, uint8_t sy = 128, uint8_t cx = 128, uint8_t cy = 128, uint8_t l = 0,
             uint8_t r = 0) {
    PadState s;
    s.connected = true;
    s.buttons = buttons;
    s.stick_x = sx;
    s.stick_y = sy;
    s.cstick_x = cx;
    s.cstick_y = cy;
    s.trig_l = l;
    s.trig_r = r;
    return s;
}

std::string pad_line(int port, const PadState& s) {
    char line[64];
    snprintf(line, sizeof(line), "pad %d %04X %u %u %u %u %u %u\n", port, s.buttons, s.stick_x, s.stick_y, s.cstick_x,
             s.cstick_y, s.trig_l, s.trig_r);
    return line;
}

bool same(const PadState& a, const PadState& b) {
    return a.connected == b.connected && a.buttons == b.buttons && a.stick_x == b.stick_x && a.stick_y == b.stick_y &&
           a.cstick_x == b.cstick_x && a.cstick_y == b.cstick_y && a.trig_l == b.trig_l && a.trig_r == b.trig_r;
}

// The frame of the input log's last line when that's its end marker, or -1.
long log_end_frame() {
    const std::string text = read_file(std::string(kLog) + "/inputs.txt");
    const size_t at = text.rfind('\n', text.size() >= 2 ? text.size() - 2 : 0);
    unsigned poll, frame;
    if (sscanf(text.c_str() + (at == std::string::npos ? 0 : at + 1), "# end %u %u", &poll, &frame) != 2) return -1;
    return frame;
}

struct Stop {
    int status = -1;  // the child's exit status, -1 when a signal ended it
    std::string err;  // its stderr
    std::string heard;  // what the link sent the bot
};

// The game, in a child process, polls port 1 at `frame`, with its stderr in child_stderr.txt.
// It closes its copy of the bot's end of the link, `bot`, so that the bot's own close is a
// hang-up.
pid_t game_child(uint32_t frame, int bot) {
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        close(bot);
        close(g_listener);
        const int fd = open("child_stderr.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, 2);
        close(fd);
        poll_at(0, frame);
        _exit(0);  // the link let the step through
    }
    return pid;
}

void end_child(pid_t pid, Stop& s) {
    int status = 0;
    waitpid(pid, &status, 0);
    s.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    s.err = read_file("child_stderr.txt");
}

// The game, in a child process with a new link driving `ports` ports, polls port 1 at `frame`.
// The bot reads the frame line and sends `answer`, then hangs up when `hang_up` says so and
// otherwise stops sending, so a link that wants more lines finds the bot gone rather than
// waiting for good.
Stop stop_case(int ports, uint32_t frame, const std::string& answer, bool hang_up) {
    const int bot = link(ports);
    const pid_t pid = game_child(frame, bot);
    Stop s;
    s.heard = first_line(bot);
    say(bot, answer);
    if (hang_up) close(bot);
    else shutdown(bot, SHUT_WR);
    end_child(pid, s);
    if (!hang_up) {
        s.heard += heard(bot);
        close(bot);
    }
    return s;
}

// The bot hangs up before the game steps it, so the frame line finds it gone.
Stop gone_case(uint32_t frame) {
    const int bot = link(1);
    close(bot);
    Stop s;
    end_child(game_child(frame, bot), s);
    return s;
}

// A line the link doesn't take, after the lines in `before`, stops the game at it, quoting it.
void refuses(int ports, const std::string& line, const char* why, const char* what, const std::string& before = "") {
    const Stop s = stop_case(ports, 50, before + line + "\n", false);
    check(s.status == 1 && has(s.err, "[bot] frame 50: ") && has(s.err, why) && has(s.err, "\"" + line + "\"") &&
              s.heard == "frame 50\n",
          std::string(what) + " stops the game, quoting the line (" + s.err.substr(0, s.err.find('\n')) + ")");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_bot_link <work dir>\n");
        return 2;
    }
    // A link that waits for a line no check sends would wait for good; this ends it.
    alarm(60);
    struct stat st;
    if (stat(argv[1], &st) == 0 || mkdir(argv[1], 0755) != 0 || chdir(argv[1]) != 0) {
        fprintf(stderr, "%s exists or can't be made: give a new directory\n", argv[1]);
        return 2;
    }
    g_mem = (uint8_t*)calloc(1, RAM_SIZE);
    g_listener = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, kSocket);
    if (!g_mem || g_listener < 0 || bind(g_listener, (const sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(g_listener, 4) != 0) {
        fprintf(stderr, "can't listen on %s/%s: %s\n", argv[1], kSocket, strerror(errno));
        return 2;
    }
    if (!input_log_start(kLog, "no_card.raw", "no_sram.raw")) return 2;
    input_script_init("20:B:1");  // port 1 presses B at frame 20

    // No bot: the live state, as before.
    const PadState live = pad(PAD_START, 10, 20);
    pad_set_state(0, live);
    check(same(poll_at(0, 1), live), "without a bot a poll reads the live state");

    // A replay already decides the pads.
    bool ok = true;
    std::string err = stderr_of([&] { ok = bot_link_start(kSocket, 1, true); });
    check(!ok && has(err, "--replay") && !knocked(), "--bot with --replay is refused before it connects");

    // Nobody listening.
    err = stderr_of([&] { ok = bot_link_start("nobody.sock", 1, false); });
    check(!ok && has(err, "nobody.sock") && has(err, strerror(ENOENT)),
          "a connect that fails says the path and the error (" + err.substr(0, err.find('\n')) + ")");
    const std::string long_path(200, 'x');
    err = stderr_of([&] { ok = bot_link_start(long_path.c_str(), 1, false); });
    check(!ok && has(err, "bytes at most"), "a path too long for a Unix socket is refused");

    // Two ports driven. The bot's states replace the live one, and every poll of the frame,
    // on any port, reads them from the one exchange. Ports 3 and 4 read unplugged.
    int bot = link(2);
    const PadState p1 = pad(PAD_A, 200, 128, 128, 128, 0, 0), p2 = pad(0);
    say(bot, pad_line(1, p1) + pad_line(2, p2) + "done\n");
    bool all = true;
    for (int i = 0; i < 3; i++) all = all && same(poll_at(0, 5), p1) && same(poll_at(1, 5), p2);
    all = all && !poll_at(2, 5).connected && !poll_at(3, 5).connected;
    check(all, "every poll of a frame reads the bot's states, in place of the live one");
    check(heard(bot) == "frame 5\n", "a frame polled several times on several ports is one exchange");

    // The frame count jumps from 5 to 9: the next step says 9.
    const PadState p1b = pad(PAD_B | PAD_L, 128, 30, 128, 128, 255, 0);
    say(bot, pad_line(1, p1b) + pad_line(2, p2) + "done\n");
    const bool jumped = same(poll_at(1, 9), p2) && same(poll_at(0, 9), p1b);
    check(jumped && heard(bot) == "frame 9\n", "a jump in the frame count steps the bot once, at the new count");

    // Reads, the last bytes of RAM among them, answered in the order asked.
    g_mem[0] = 0x01; g_mem[1] = 0x02; g_mem[2] = 0x03; g_mem[3] = 0xAB;
    std::string last = "data ";
    for (int i = 0; i < 256; i++) {
        g_mem[RAM_SIZE - 256 + i] = (uint8_t)i;
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", i);
        last += hex;
    }
    say(bot, "read 80000000 4\nread 817FFF00 256\npad 1 0000 128 128 128 128 0 0\nread 817fffff 1\n" + pad_line(2, p2) +
                 "done\n");
    poll_at(0, 10);
    check(heard(bot) == "frame 10\ndata 010203ab\n" + last + "\ndata ff\n",
          "reads are answered in hex, up to and including the last byte of RAM");

    // The input log records what the bot pressed, so a replay needs no bot.
    const std::string log = read_file(std::string(kLog) + "/inputs.txt");
    check(has(log, " 5 0 1 0100 C8 80 80 80 00 00\n") && has(log, " 9 0 1 0240 80 1E 80 80 FF 00\n"),
          "the input log records the bot's states");

    // A script's presses go on top of the bot's.
    say(bot, pad_line(1, p1) + pad_line(2, p2) + "done\n");
    const PadState scripted = poll_at(0, 20);
    say(bot, pad_line(1, p1) + pad_line(2, p2) + "done\n");
    const PadState after = poll_at(0, 21);
    check(scripted.buttons == (PAD_A | PAD_B) && same(after, p1), "a script's press goes on top of the bot's state");
    heard(bot);

    // Lockstep: a bot that takes its time is waited for.
    std::thread late([bot, p1, p2] {
        const std::string line = first_line(bot);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        say(bot, line == "frame 30\n" ? pad_line(1, p1) + pad_line(2, p2) + "done\n" : "wrong frame\n");
    });
    const PadState waited = poll_at(0, 30);
    late.join();
    check(same(waited, p1), "the guest waits for the bot's done");
    close(bot);

    // The stops. Each ends the child with status 1 and the reason on stderr.
    Stop s = stop_case(2, 40, pad_line(1, p1) + "done\n", false);
    check(s.status == 1 && has(s.err, "[bot] frame 40: done without a pad line for port 2") && has(s.err, "\"done\""),
          "done without a port the bot drives stops the game, naming the port (" + s.err.substr(0, s.err.find('\n')) +
              ")");
    check(log_end_frame() == 40, "the input log ends at the frame the stop came at");

    s = stop_case(1, 41, "read 817FFF01 256\n", false);
    check(s.status == 1 && has(s.err, "outside RAM") && has(s.err, "\"read 817FFF01 256\"") && s.heard == "frame 41\n",
          "a read that runs past the end of RAM stops the game, quoting the line (" + s.err.substr(0, s.err.find('\n')) +
              ")");

    s = stop_case(1, 42, pad_line(1, p1), true);
    check(s.status == 1 && s.err == "[bot] frame 42: the bot hung up\n",
          "the bot dying mid-frame stops the game with \"the bot hung up\"");
    check(log_end_frame() == 42, "the input log of a bot that died mid-frame ends at that frame");
    s = gone_case(43);
    check(s.status == 1 && s.err == "[bot] frame 43: the bot hung up\n",
          "a bot gone before its frame line stops the game with \"the bot hung up\", not a SIGPIPE");

    refuses(1, "read 80000000 0", "not read ADDR LEN", "a read of 0 bytes");
    refuses(1, "read 80000000 257", "not read ADDR LEN", "a read of 257 bytes");
    refuses(1, "read 7FFFFFFF 1", "outside RAM", "a read below RAM");
    refuses(1, "read 81800000 1", "outside RAM", "a read past RAM");
    refuses(1, "read 0x80000000 4", "not read ADDR LEN", "an address with 0x");
    refuses(1, "pad 2 0000 128 128 128 128 0 0", "port 2 isn't one the bot drives", "a pad line for a port past --pads");
    refuses(4, "pad 5 0000 128 128 128 128 0 0", "port 5 isn't one the bot drives", "port 5");
    refuses(1, "pad 1 0080 128 128 128 128 0 0", "bits that aren't buttons", "a button bit the pad hasn't got");
    refuses(1, "pad 1 0000 256 128 128 128 0 0", "the rest 0 to 255", "a stick at 256");
    refuses(1, "pad 1 0000 128 128 128 128 0", "not pad PORT", "a pad line a value short");
    refuses(2, "pad 1 0100 128 128 128 128 0 0", "a second pad line", "a second pad line for a port",
            "pad 1 0000 128 128 128 128 0 0\n");
    refuses(1, "done now", "not read, pad or done", "done with more after it");
    refuses(1, "press A", "not read, pad or done", "a word the link doesn't know");

    printf("%d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
