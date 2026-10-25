// Port memory checks: the `test_port_memory` target. Runs with no disc image, device, SDL or
// game. runtime/port_memory.cpp is linked as it is, and platform.cpp for its file replace.
//
// match_ports is checked on gamepads made up here, so the serial cases that SDL's virtual
// joysticks can't carry (they have no serial) are checked in this file. Each case builds the
// saved ports and the gamepads that are present, and compares the ports match_ports gives.
// The file half writes into the work dir, loads and saves, and checks the bytes. A save that
// fails must leave the old file whole, which one case makes happen with a directory where
// the save writes its new file.
//
// usage: test_port_memory <work dir>
// The work dir must not exist. It's made, and the files are left in it. Exits 0 only if
// every check passes.
#include "port_memory.h"
#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
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

std::string path_of(const char* name) { return g_dir + "/" + name; }

void write_file(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return; }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
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

// A GUID of 32 hex digits, all `c`.
std::string guid(char c) { return std::string(32, c); }

PortDevice device(char g, const char* serial = "", uint16_t vendor = 0x045e, uint16_t product = 0x0b13) {
    PortDevice d;
    d.guid = guid(g);
    d.serial = serial;
    d.vendor = vendor;
    d.product = product;
    return d;
}

PresentDevice present(const PortDevice& id, int index = -1, int held = -1) {
    PresentDevice p;
    p.id = id;
    p.index = index;
    p.held = held;
    return p;
}

// `saved` with each device on its port, 0 for port 1.
SavedPorts saved_of(std::initializer_list<std::pair<int, PortDevice>> ports) {
    SavedPorts s;
    for (const auto& [port, d] : ports) s[port] = d;
    return s;
}

using Ports = std::vector<int>;
const std::array<bool, 4> kNoPins = {false, false, false, false};

Ports match(const SavedPorts& saved, const std::vector<PresentDevice>& devices,
            const std::array<bool, 4>& pinned = kNoPins) {
    return match_ports(saved, devices, pinned);
}

// ---- match_ports

void nothing_saved() {
    const SavedPorts none;
    check(match(none, {present(device('a'), 0), present(device('b'), 1)}) == Ports{0, 1},
          "nothing saved: each gamepad takes the port of its player index");
    check(match(none, {present(device('a'), 2), present(device('b'), 2)}) == Ports{2, 0},
          "nothing saved: a player index another gamepad took gives the lowest free port");
    check(match(none, {present(device('a'), -1), present(device('b'), 0)}) == Ports{0, 1},
          "nothing saved: gamepads are placed in connect order, so one with no index takes port 1 before the next asks for it");
    check(match(none, {present(device('a'), 7)}) == Ports{0}, "nothing saved: an index past port 4 gives the lowest free port");
    check(match(none, {}) == Ports{}, "no gamepads: no ports");
}

void exact_match() {
    const SavedPorts saved = saved_of({{0, device('a')}, {1, device('b')}});
    check(match(saved, {present(device('b'), 0), present(device('a'), 1)}) == Ports{1, 0},
          "a saved port goes to the gamepad with its GUID, though they connected in the other order and SDL's indexes say the reverse");
    check(match(saved, {present(device('a'), 3)}) == Ports{0},
          "a lone saved gamepad takes its saved port over its player index");
    check(match(saved, {present(device('b'), 0)}) == Ports{1}, "and a lone one saved on port 2 gets port 2, though port 1 is free");
    const SavedPorts only_b = saved_of({{0, device('b')}});
    check(match(only_b, {present(device('a'), 0), present(device('b'), 1)}) == Ports{1, 0},
          "a saved gamepad that connects second still gets its port, and the first one connected gives way to it");
}

void serial_fallback() {
    const PortDevice usb = device('a', "SN-1");
    const SavedPorts saved = saved_of({{2, usb}});
    check(match(saved, {present(device('b', "SN-1"), 0)}) == Ports{2},
          "the same serial and USB IDs under another GUID (another connection) takes the saved port");
    check(match(saved, {present(device('b', "SN-1", 0x054c, 0x09cc), 0)}) == Ports{0},
          "the same serial under other USB IDs isn't the same gamepad");
    check(match(saved, {present(device('b', "SN-2"), 0)}) == Ports{0}, "another serial under another GUID isn't either");
    check(match(saved, {present(device('b', ""), 0)}) == Ports{0}, "nor is a gamepad with no serial");

    // The exact match comes first. The first gamepad listed has the serial of both saved
    // gamepads, but only the GUID of the second, so the second saved port is its own.
    const SavedPorts both = saved_of({{0, device('a', "SN-1")}, {1, device('b', "SN-1")}});
    check(match(both, {present(device('b', "SN-1"), 0), present(device('c', "SN-1"), 1)}) == Ports{1, 0},
          "a GUID and serial match is made before a serial and USB ID match");
}

void identical_with_serials() {
    const SavedPorts saved = saved_of({{0, device('a', "SN-1")}, {1, device('a', "SN-2")}});
    check(match(saved, {present(device('a', "SN-2"), 0), present(device('a', "SN-1"), 1)}) == Ports{1, 0},
          "two gamepads of one model with different serials each get their own saved port, whichever connects first");
    check(match(saved, {present(device('a', "SN-2"), 0)}) == Ports{1}, "and one of them alone gets its own port");
    check(match(saved, {present(device('a', "SN-3"), 0), present(device('a', "SN-1"), 1)}) == Ports{1, 0},
          "a third of that model, new to the file, takes the free port when its player index asks for a taken one");
}

void identical_without_serials() {
    const SavedPorts saved = saved_of({{0, device('a')}, {1, device('a')}});
    check(match(saved, {present(device('a'), 1), present(device('a'), 0)}) == Ports{0, 1},
          "two gamepads of one model with no serial fill ports 1 and 2 in the order they connect");
    check(match(saved, {present(device('a'), 1)}) == Ports{0}, "and one of them alone takes the lower port");
    const SavedPorts later = saved_of({{0, device('a')}, {1, device('a')}});
    check(match(later, {present(device('a'), 0, 0), present(device('a'), 3)}) == Ports{0, 1},
          "with one already on port 1, the next fills port 2");
}

void pinned() {
    const std::array<bool, 4> pin3 = {false, false, true, false};
    const SavedPorts saved = saved_of({{2, device('a')}});
    check(match(saved, {present(device('a'), 2)}, pin3) == Ports{0},
          "a saved port controls.txt pins isn't given: the gamepad takes the lowest free port");
    check(match(SavedPorts{}, {present(device('b'), 2)}, pin3) == Ports{0},
          "a player index that names a pinned port isn't given either");
    check(match(SavedPorts{}, {present(device('b'), 0), present(device('c'), 1), present(device('d'), 2),
                                present(device('e'), 3)}, pin3) == Ports{0, 1, 3, -1},
          "a pinned port is skipped when the ports fill, and the last gamepad is left out");
    const SavedPorts first = saved_of({{0, device('a')}});
    const std::array<bool, 4> pin1 = {true, false, false, false};
    check(match(first, {present(device('a'), 0), present(device('b'), 1)}, pin1) == Ports{1, 2},
          "with port 1 pinned, the gamepad saved there and the next one go to the next free ports");
}

void missing_device() {
    const SavedPorts saved = saved_of({{0, device('a')}});
    check(match(saved, {present(device('b'), 0)}) == Ports{0},
          "a saved port isn't a reservation: its gamepad is away, and another takes port 1 by its player index");
    check(match(saved, {present(device('b'), -1)}) == Ports{0}, "or by the lowest free port");
    check(match(saved, {present(device('b'), 2)}) == Ports{2}, "a gamepad whose index is another free port takes that one");
    check(match(saved, {present(device('b'), 0, 0), present(device('a'), 0)}) == Ports{0, 1},
          "the saved gamepad comes back to find its port taken, and takes the lowest free port");
}

void saved_gamepad_finds_port_taken() {
    const SavedPorts saved = saved_of({{0, device('a')}});
    check(match(saved, {present(device('b'), 0, 0), present(device('a'), 3)}) == Ports{0, 1},
          "a saved gamepad whose port is taken goes to the lowest free port, not to its player index");
    check(match(saved, {present(device('b'), 0, 0), present(device('c'), 1, 1), present(device('a'), 2)}) == Ports{0, 1, 2},
          "and with ports 1 and 2 taken, to port 3");
    check(match(saved, {present(device('b'), 0, 0), present(device('c'), 1, 1), present(device('d'), 2, 2),
                        present(device('e'), 3, 3), present(device('a'), 0)}) == Ports{0, 1, 2, 3, -1},
          "with every port taken, it's left out, and the four stay where they were");
    check(match(saved, {present(device('b'), 1, 3), present(device('a'), 1)}) == Ports{3, 0},
          "a gamepad that holds a port keeps it whatever its player index says");
}

void assign() {
    SavedPorts saved;
    check(ports_assign(saved, 1, device('a')) && saved[1] && *saved[1] == device('a'), "a gamepad put on an empty port is a change");
    check(!ports_assign(saved, 1, device('a')), "the same gamepad on the same port is not");
    check(ports_assign(saved, 1, device('b')), "another gamepad on that port is");
    check(ports_assign(saved, 1, device('b', "SN-1")), "so is the same one with a serial it didn't have");
    check(ports_assign(saved, 1, device('b', "SN-1", 0x054c, 0x09cc)), "or other USB IDs");
}

// ---- The file's text

void format_and_parse() {
    SavedPorts saved = saved_of({{0, device('a', "", 0x045e, 0x0b13)}, {2, device('b', "E8:47:3A", 0x054c, 0x09cc)}});
    const std::string text = ports_format(saved);
    check(text.find("1 " + guid('a') + " - 045e:0b13\n") != std::string::npos &&
              text.find("2 none\n") != std::string::npos &&
              text.find("3 " + guid('b') + " E8:47:3A 054c:09cc\n") != std::string::npos &&
              text.find("4 none\n") != std::string::npos,
          "each port is a line: the GUID, the serial or - and the USB IDs in hex, or none");
    check(ports_parse(text, "x") == saved, "and the text parses back to the same ports");

    SavedPorts odd = saved_of({{0, device('c', "two words")}, {1, device('d', "100%")}, {2, device('e', "-")},
                               {3, device('f', "tab\there\nnewline")}});
    const std::string odd_text = ports_format(odd);
    check(odd_text.find("two%20words") != std::string::npos && odd_text.find("100%25") != std::string::npos &&
              odd_text.find(" %2D ") != std::string::npos,
          "a serial with a space or a percent sign, or one that is only -, is percent-encoded");
    check(ports_parse(odd_text, "x") == odd, "and such serials come back as they were, newline and tab included");
    check(std::count(odd_text.begin(), odd_text.end(), '\n') == 5, "none of them adds a line to the file");
}

void parse_forgives() {
    const std::string text = "# a comment\n"
                             "\n"
                             "1 " + guid('a') + " SN-1 045e:0b13\r\n"
                             "  \t 3 \t none \n"
                             "2 " + guid('b') + " - 054C:09CC\n";
    const SavedPorts got = ports_parse(text, "x");
    check(got[0] && got[0]->serial == "SN-1" && got[0]->vendor == 0x045e && got[0]->product == 0x0b13 &&
              got[1] && got[1]->serial.empty() && got[1]->vendor == 0x054c && got[1]->product == 0x09cc && !got[2] && !got[3],
          "comments, blank lines, CR LF, extra spaces and hex in either case are read");
}

void parse_skips_bad_lines() {
    const std::string bad =
        "0 " + guid('a') + " - 045e:0b13\n"           // port 0
        "5 " + guid('a') + " - 045e:0b13\n"           // port 5
        "1x " + guid('a') + " - 045e:0b13\n"          // not a number
        "1 xyz - 045e:0b13\n"                         // a short GUID
        "1 " + std::string(32, 'g') + " - 045e:0b13\n"  // not hex
        "1 " + guid('a') + " - 045e\n"                // no product
        "1 " + guid('a') + " - 045e:0b13f\n"          // five digits
        "1 " + guid('a') + " - zz:0b13\n"             // not hex
        "1 " + guid('a') + " bad%2 045e:0b13\n"       // a cut-off escape
        "1 " + guid('a') + " bad%zz 045e:0b13\n"      // a bad escape
        "1 " + guid('a') + "\n"                       // too short
        "1\n"                                         // a port alone
        "2 none extra\n"                              // trailing word after none
        "2 " + guid('b') + " - 045e:0b13\n";          // this one is fine
    fflush(stdout);
    const SavedPorts got = ports_parse(bad, "bad.txt");
    check(!got[0] && got[1] && *got[1] == device('b') && !got[2] && !got[3],
          "lines that aren't a port with none or a GUID, serial and two IDs are skipped, and the good one beside them is kept");
}

void parse_last_line_wins() {
    const SavedPorts got = ports_parse("1 " + guid('a') + " - 045e:0b13\n1 " + guid('b') + " - 045e:0b13\n", "x");
    check(got[0] && *got[0] == device('b'), "a port named twice has its last line");
    const SavedPorts none = ports_parse("1 " + guid('a') + " - 045e:0b13\n1 none\n", "x");
    check(!none[0], "and a later none takes it back");
    check(ports_parse("", "x") == SavedPorts{}, "an empty file has no ports");
}

// ---- The file

void load_missing() {
    check(ports_load(path_of("missing.txt")) == SavedPorts{}, "a missing file gives no ports");
    check(!exists(path_of("missing.txt")), "and doesn't make one");
}

void load_unreadable() {
    const std::string p = path_of("locked.txt");
    write_file(p, "1 " + guid('a') + " - 045e:0b13\n");
    if (chmod(p.c_str(), 0) != 0 || access(p.c_str(), R_OK) == 0) {
        printf("skip load_unreadable: this account reads files whatever their mode\n");
        return;
    }
    check(ports_load(p) == SavedPorts{}, "a file that can't be read gives no ports");
    chmod(p.c_str(), 0644);
}

void save_and_load() {
    const std::string p = path_of("saves/ports.txt");
    SavedPorts saved = saved_of({{1, device('a', "SN-1")}, {3, device('b')}});
    check(!exists(path_of("saves")) && ports_save(p, saved), "a save makes the directory that holds the file");
    check(read_file(p) == ports_format(saved) && !exists(p + ".tmp"), "the file holds the formatted ports, and no temp file is left");
    check(ports_load(p) == saved, "and loads back to the same ports");
    saved[0] = device('c');
    check(ports_save(p, saved) && ports_load(p) == saved && !exists(p + ".tmp"), "a second save replaces the file");
}

void failed_save_leaves_old_file() {
    const std::string p = path_of("kept.txt");
    const SavedPorts old = saved_of({{0, device('a')}});
    ports_save(p, old);
    const std::string before = read_file(p);
    // A directory where the new file would go, so that it can't be opened.
    mkdir((p + ".tmp").c_str(), 0755);
    fflush(stdout);
    const bool saved = ports_save(p, saved_of({{0, device('b')}}));
    check(!saved && read_file(p) == before, "a save that can't write its new file fails and leaves the old one whole");
    rmdir((p + ".tmp").c_str());

    write_file(path_of("plain"), "a file where a directory is needed\n");
    fflush(stdout);
    check(!ports_save(path_of("plain/ports.txt"), old), "a save whose directory can't be made fails");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_port_memory <work dir>\n");
        return 2;
    }
    g_dir = argv[1];
    if (exists(g_dir) || mkdir(g_dir.c_str(), 0755) != 0) {
        fprintf(stderr, "%s exists or can't be made: give a new directory\n", g_dir.c_str());
        return 2;
    }
    nothing_saved();
    exact_match();
    serial_fallback();
    identical_with_serials();
    identical_without_serials();
    pinned();
    missing_device();
    saved_gamepad_finds_port_taken();
    assign();
    format_and_parse();
    parse_forgives();
    parse_skips_bad_lines();
    parse_last_line_wins();
    load_missing();
    load_unreadable();
    save_and_load();
    failed_save_leaves_old_file();
    printf("test_port_memory: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
