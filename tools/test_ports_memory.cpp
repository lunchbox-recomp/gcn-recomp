// ---- Remembered ports (port_memory.cpp): saves/ports.txt under the work dir. SDL's virtual
// joysticks have a GUID made from their name and USB IDs of 0. They have no serial, so what a
// serial adds is checked on its own in test_port_memory. Read in by tools/test_ports.cpp after
// the frontend.
#include "test_ports_support.h"
#include <sys/stat.h>
#include <unistd.h>

namespace {

// A first launch: no ports.txt, the console on and the gamepads taken.
void first_launch(const std::string& file) {
    fresh_file(file);
    power_on(1);
    gamepads_init();
}

// A new launch. The gamepads are let go, and the file is read again at the first gamepad.
void relaunch() {
    gamepads_shutdown();
    power_on(1);
}

// Gamepads pulled out and plugged in the other order get their saved ports back. Pulling
// them out writes nothing, and neither does plugging them back in.
void memory_order(const std::string& file) {
    first_launch(file);
    Virtual a{"virtual pad ma"}, b{"virtual pad mb"};
    plug({&a, &b});
    const bool first = port(a) == 1 && port(b) == 2;
    const std::string text = read_file(file);
    check(first && has(text, line_for(1, a)) && has(text, line_for(2, b)) && has(text, "\n3 none\n"),
          "two gamepads take ports 1 and 2, and ports.txt names them there");
    const uint64_t written = file_id(file);

    unplug({&a, &b});
    check(file_id(file) == written, "pulling both out writes nothing");
    plug({&b, &a});
    check(port(b) == 2 && port(a) == 1 && file_id(file) == written,
          "plugged back in the other order, each gets its saved port, and nothing is written");
    unplug({&a, &b});

    relaunch();
    attach(b);
    attach(a);
    gamepads_init();
    frame();
    check(port(b) == 2 && port(a) == 1 && file_id(file) == written,
          "at the next launch, listed in the other order, they get their saved ports, and nothing is written");
    unplug({&a, &b});
    gamepads_shutdown();
}

// Four gamepads connecting in the reverse order, which asks every player index to move.
void memory_reversed(const std::string& file) {
    first_launch(file);
    Virtual p1{"virtual pad r1"}, p2{"virtual pad r2"}, p3{"virtual pad r3"}, p4{"virtual pad r4"};
    plug({&p1, &p2, &p3, &p4});
    const bool in_order = port(p1) == 1 && port(p2) == 2 && port(p3) == 3 && port(p4) == 4;
    unplug({&p1, &p2, &p3, &p4});
    relaunch();
    for (Virtual* v : {&p4, &p3, &p2, &p1}) attach(*v);
    gamepads_init();
    frame();
    check(in_order && port(p1) == 1 && port(p2) == 2 && port(p3) == 3 && port(p4) == 4,
          "four gamepads that connect in the reverse order at launch each get their saved port and player index");
    unplug({&p1, &p2, &p3, &p4});
    relaunch();
    plug({&p3, &p1, &p4, &p2});
    check(port(p1) == 1 && port(p2) == 2 && port(p3) == 3 && port(p4) == 4,
          "and plugged in one at a time in a shuffled order, the same");
    unplug({&p1, &p2, &p3, &p4});
    gamepads_shutdown();
}

// Two gamepads of one model have the same GUID and no serial, so they can't be told apart.
// They fill their ports in the order they connect.
void memory_twins(const std::string& file) {
    first_launch(file);
    Virtual t1{"virtual pad twin"}, t2{"virtual pad twin"};
    plug({&t1, &t2});
    const uint64_t written = file_id(file);
    check(port(t1) == 1 && port(t2) == 2 && has(read_file(file), line_for(1, t1)) && has(read_file(file), line_for(2, t2)),
          "two gamepads with the same GUID take ports 1 and 2");
    unplug({&t1, &t2});
    relaunch();
    plug({&t2, &t1});
    check(port(t2) == 1 && port(t1) == 2 && file_id(file) == written,
          "and when they come back, the first to connect gets port 1: the file can't tell them apart, and says the same");
    unplug({&t1, &t2});
    gamepads_shutdown();
}

// A saved port isn't a reservation. Its gamepad is away, and another takes it. The
// gamepad that comes back holding port 1's player index doesn't move the one on port 1.
void memory_missing(const std::string& file) {
    first_launch(file);
    Virtual a{"virtual pad away"};
    attach(a);
    frame();
    const bool on_one = port(a) == 1;
    const std::string away = guid_of(a);
    unplug({&a});
    relaunch();
    Virtual c{"virtual pad cover"};
    attach(c);
    frame();
    const std::string text = read_file(file);
    check(on_one && port(c) == 1 && has(text, line_for(1, c)) && !has(text, away),
          "port 1's saved gamepad is away: another takes port 1, and the file now names that one there");
    attach(a, 0);
    frame();
    check(port(c) == 1 && port(a) == 2,
          "the first comes back holding port 1's player index and finds port 1 taken: it takes port 2, and the one on port 1 stays");
    unplug({&a, &c});
    gamepads_shutdown();
}

// A port controls.txt pins beats the file. The gamepad saved there takes the lowest free
// port, not the port of its player index.
void memory_pinned(const std::string& dir, const std::string& file) {
    first_launch(file);
    Virtual d{"virtual pad pinned away"};
    attach(d, 2);
    frame();
    const bool on_three = port(d) == 3;
    unplug({&d});
    relaunch();

    load_controls(dir + "/controls.txt", "A = \"virtual pad pinning\" button 0 port 3\n");
    power_on(1);
    gamepads_init();
    attach(d, 1);
    frame();
    const bool one = port(d) == 1;
    check(on_three && one && has(read_file(file), line_for(1, d)),
          "port 3 saved for a gamepad and pinned by controls.txt: it takes the lowest free port, 1, though its player index says 2");
    unplug({&d});
    gamepads_shutdown();
    no_controls(dir);
}

// A gamepad on a port keeps it, though the file also names it on a lower port that's free
// by now. The gamepad landed on port 2 because controls.txt pinned port 1 then, and the pin
// is gone when the next gamepad asks for port 2.
void memory_held(const std::string& dir, const std::string& file) {
    fresh_file(file);
    load_controls(dir + "/controls.txt", "A = \"virtual pad nobody\" button 0 port 1\n");
    power_on(1);
    Virtual held{"virtual pad held"}, next{"virtual pad next"};
    attach(held);
    write_file(file, "1 " + guid_of(held) + " - 0000:0000\n");
    gamepads_init();
    frame();
    const bool on_two = port(held) == 2 && has(read_file(file), line_for(2, held));
    no_controls(dir);
    attach(next, 1);
    frame();
    check(on_two && port(held) == 2 && port(next) == 1,
          "a gamepad saved on port 1 but put on port 2 by a pin keeps port 2, and a newcomer holding index 1 takes port 1");
    unplug({&held, &next});
    gamepads_shutdown();
}

// A gamepad left out for want of a port isn't saved. A file of lines that aren't ports is
// replaced when a gamepad lands.
void memory_file(const std::string& dir, const std::string& file) {
    first_launch(file);
    Virtual g[5] = {{"virtual pad f1"}, {"virtual pad f2"}, {"virtual pad f3"}, {"virtual pad f4"}, {"virtual pad f5"}};
    plug({&g[0], &g[1], &g[2], &g[3], &g[4]});
    const std::string text = read_file(file);
    check(port(g[4]) == 0 && has(text, line_for(4, g[3])) && !has(text, guid_of(g[4])),
          "a fifth gamepad is left out and ports.txt doesn't name it");
    unplug({&g[0], &g[1], &g[2], &g[3], &g[4]});
    gamepads_shutdown();

    write_file(file, "this is not a port\n9 none\n1 " + std::string(32, 'a') + " - 0000:0000 more\n");
    gamepads_remember(file.c_str());
    power_on(1);
    gamepads_init();
    const std::string said = dir + "/memory-bad-stderr.txt";
    Virtual v{"virtual pad after the bad file"};
    capture(said, [&] {
        attach(v);
        frame();
    });
    check(lines_with(said, "so the line is skipped") == 3 && port(v) == 1 && has(read_file(file), line_for(1, v)),
          "a file of lines that aren't ports is read as empty with each line said, the gamepad takes port 1, and the file is written whole");
    unplug({&v});
    gamepads_shutdown();
}

// A launch with no gamepad connected reads nothing and writes nothing. Each port reads as
// it does with no memory at all.
void memory_no_pads(const std::string& dir, const std::string& file) {
    // The ports as the guest reads them over a few frames.
    auto ports_seen = [] {
        std::string out;
        for (int s = 0; s < 4; s++) {
            frame();
            for (int i = 0; i < 4; i++) out += describe(read(i));
        }
        return out;
    };

    gamepads_remember(nullptr);
    power_on(1);
    gamepads_init();
    const std::string plain = ports_seen();
    gamepads_shutdown();

    // With no file, nothing is made. That includes its directory.
    fresh_file(file);
    rmdir((dir + "/saves").c_str());
    const std::string quiet = dir + "/memory-none-stderr.txt";
    std::string with_none;
    capture(quiet, [&] {
        power_on(1);
        gamepads_init();
        with_none = ports_seen();
    });
    check(with_none == plain && !exists_path(file) && !exists_path(dir + "/saves") && read_file(quiet).empty(),
          "no gamepad and no ports.txt: the ports read as with no memory, and no file or directory is made");
    gamepads_shutdown();

    // A file with ports in it that can't even be opened. A launch that tried to read it would say so.
    plat_make_dirs(dir + "/saves");
    write_file(file, "1 " + std::string(32, 'a') + " - 0000:0000\n");
    const uint64_t before = file_id(file);
    const std::string kept = read_file(file);
    gamepads_remember(file.c_str());
    const bool locked = chmod(file.c_str(), 0) == 0 && access(file.c_str(), R_OK) != 0;
    const std::string loud = dir + "/memory-locked-stderr.txt";
    std::string with_file;
    capture(loud, [&] {
        power_on(1);
        gamepads_init();
        with_file = ports_seen();
    });
    chmod(file.c_str(), 0644);
    if (!locked) printf("this account reads files whatever their mode: the unreadable-file check is weaker here\n");
    check(with_file == plain && read_file(loud).empty() && file_id(file) == before && read_file(file) == kept,
          "no gamepad and a ports.txt that can't be read: it isn't opened, nothing is said, and the file is as it was");
    gamepads_shutdown();

    // The same with the file readable.
    gamepads_remember(file.c_str());
    power_on(1);
    gamepads_init();
    const std::string readable = ports_seen();
    check(readable == plain && file_id(file) == before, "with a readable ports.txt and no gamepad, the file isn't rewritten");
    gamepads_shutdown();
}

// A ports.txt that exists and can't be read isn't saved over. The gamepads still land, with
// nothing remembered, and the file is as it was when they have.
void memory_unreadable(const std::string& dir, const std::string& file) {
    plat_make_dirs(dir + "/saves");
    write_file(file, "1 " + std::string(32, 'a') + " - 0000:0000\n");
    const uint64_t before = file_id(file);
    const std::string kept = read_file(file);
    if (chmod(file.c_str(), 0) != 0 || access(file.c_str(), R_OK) == 0) {
        chmod(file.c_str(), 0644);
        printf("this account reads files whatever their mode, so the unreadable ports.txt check didn't run\n");
        return;
    }
    gamepads_remember(file.c_str());
    power_on(1);
    gamepads_init();
    Virtual a{"virtual pad locked a"}, b{"virtual pad locked b"};
    const std::string said = dir + "/memory-unreadable-stderr.txt";
    capture(said, [&] { plug({&a, &b}); });
    chmod(file.c_str(), 0644);
    check(port(a) == 1 && port(b) == 2 && lines_with(said, "can't read it") == 1 && file_id(file) == before &&
              read_file(file) == kept && !exists_path(file + ".tmp"),
          "a ports.txt that can't be read is said once, the gamepads still take ports 1 and 2, and nothing is written");
    unplug({&a, &b});
    gamepads_shutdown();
    remove(file.c_str());
}

}  // namespace
