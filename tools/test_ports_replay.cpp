// ---- Recording and replay (input_log.cpp), each a mode of its own: --record, then --replay and
// --replay-no-sram on its log, and --replay-v4 and --replay-console on logs written by hand.
// Read in by tools/test_ports.cpp after the frontend.
#include "test_ports_support.h"

namespace {

// ---- Recording and replay: the same reads in both runs. Each step sets what the live
// gamepads do (when there are any) and the game's rumble motors, runs a frame and reads the
// four ports once, at presented frame 100 + step.
const int kSteps = 6;

// The motor lines a log of those steps has: port 1's motor on at step 1 and still on at step
// 2, off at step 3, and port 2's on from step 4 to the end. Each is written at the poll that
// reads the port (the step's polls are 4 * step to 4 * step + 3), at the step's frame.
const char* const kMotorLines = "motor 4 101 0 1\nmotor 12 103 0 0\nmotor 17 104 1 1\n";

std::string steps(Virtual* a, Virtual* b, std::vector<PadState>& seen) {
    std::string out;
    for (int s = 0; s < kSteps; s++) {
        SDL_Joystick* ja = a ? SDL_GetJoystickFromID(a->id) : nullptr;
        SDL_Joystick* jb = b ? SDL_GetJoystickFromID(b->id) : nullptr;
        if (jb) SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, s == 1 || s == 2);
        if (ja) SDL_SetJoystickVirtualButton(ja, SDL_GAMEPAD_BUTTON_EAST, s == 3);
        if (jb) SDL_SetJoystickVirtualAxis(jb, SDL_GAMEPAD_AXIS_LEFTX, s == 4 ? 32767 : 0);
        set_motor(0, s == 1 || s == 2);
        set_motor(1, s >= 4);
        frame();
        for (int i = 0; i < 4; i++) {
            seen.push_back(read(i, 100 + s));
            out += describe(seen.back());
        }
    }
    return out;
}

// The log in the folder `log` loaded to replay, saying on stderr into `said`. `sram` is the
// SRAM the replay starts from.
bool load_replay(const std::string& log, const std::string& said, std::string& sram) {
    std::string card;
    bool loaded = false;
    capture(said, [&] { loaded = input_replay_load(log, card, sram); });
    return loaded;
}

int run_record(const std::string& dir) {
    if (!sdl_up()) return 1;
    no_controls(dir);
    input_script_init("");
    power_on(1);
    gamepads_init();
    Virtual a{"virtual pad a"}, b{"virtual pad b"};
    attach(a);
    attach(b);
    frame();
    const std::string sram = "the SRAM the run started with\n";
    write_file(dir + "/sram.raw", sram);
    if (!input_log_start(dir + "/log", dir + "/no-card.raw", dir + "/sram.raw")) return 1;
    check(read_file(dir + "/log/sram.raw") == sram, "the log keeps a snapshot of the SRAM");
    std::vector<PadState> seen;
    const std::string text = steps(&a, &b, seen);
    input_log_end();
    write_file(dir + "/expected.txt", text);
    bool two = true, rest = true;
    for (int s = 0; s < kSteps; s++) {
        two = two && seen[s * 4 + 1].connected;
        rest = rest && !seen[s * 4 + 2].connected && !seen[s * 4 + 3].connected;
    }
    check(two && rest && seen[1 * 4 + 1].buttons == PAD_A && seen[2 * 4 + 1].buttons == PAD_A && seen[3 * 4].buttons == PAD_B &&
              seen[4 * 4 + 1].stick_x == 228 && at_rest(seen[5 * 4]) && at_rest(seen[5 * 4 + 1]),
          "recording with --pads=1: port 2's gamepad (A, then the stick) and port 1's (east button, labeled B) reach the guest; ports 3 and 4 unplugged");
    const std::string log = dir + "/log/inputs.txt";
    check(read_file(log).rfind("# gcn-recomp input log v5\n", 0) == 0 &&
              lines_starting(log, "# motor ") ==
                  "# motor <poll> <frame> <chan> <0|1>: the game turned that channel's rumble motor on or off there (decimal)\n",
          "the log is version 5, and its header gives the motor line");
    check(lines_starting(log, "# disc ") == "# disc fast\n",
          "the log's header says the run read the disc at disc fast, the speed the drive is left at");
    check(lines_starting(log, "motor ") == kMotorLines,
          "the log has a motor line for each change of a port's rumble motor, at the poll that read it, and none where it stayed");
    unplug({&a, &b});
    gamepads_shutdown();
    return finish("--record");
}

int run_replay(const std::string& dir) {
    if (!sdl_up()) return 1;
    no_controls(dir);
    input_script_init("");
    std::string sram;
    const std::string said = dir + "/replay-stderr.txt";
    g_fast_disc = false;  // as settings.txt's disc console or --disc=console leave it
    if (!load_replay(dir + "/log", said, sram)) return 1;
    check(sram == dir + "/log/sram_replay.raw" && read_file(sram) == "the SRAM the run started with\n",
          "a replay gets a scratch copy of the logged SRAM");
    check(lines_with(said, "(with its SRAM snapshot)") == 1, "and its summary line says the log had an SRAM snapshot");
    check(g_fast_disc && lines_with(said, "[input] the log was recorded at disc fast, so the replay reads at that and not "
                                          "at disc console\n") == 1,
          "a log recorded at disc fast replays at disc fast with disc console asked for, and says so");
    // Logged as the frontend logs a replay, into a log of its own.
    if (!input_log_start(dir + "/replay-log", dir + "/no-card.raw", sram)) return 1;
    check(lines_starting(dir + "/replay-log/inputs.txt", "# disc ") == "# disc fast\n",
          "and the replay's own log says disc fast");
    power_on(1);
    gamepads_init();
    frame();
    std::vector<PadState> seen;
    const std::string text = steps(nullptr, nullptr, seen);
    check(text == read_file(dir + "/expected.txt"),
          "replayed with no gamepad and --pads=1: every read of all four ports is the recording's, its motor lines skipped");
    check(lines_starting(dir + "/replay-log/inputs.txt", "motor ") == kMotorLines,
          "the replay's own log has the recording's motor lines, with no gamepad to rumble");
    read(0);  // the log's end marker: the replay holds through this read
    const PadState after2 = read(1);
    check(read(0).connected && after2.connected && at_rest(after2) && !read(2).connected && !read(3).connected,
          "past the log's end: port 2, plugged in when the log ended, stays plugged in at rest with no gamepad on it");
    Virtual n{"virtual pad n"};
    attach(n);
    frame();
    check(port(n) == 1, "after the replay a gamepad that connects takes port 1 and drives it live");
    unplug({&n});
    gamepads_shutdown();
    return finish("--replay");
}

// The recording's log with its SRAM snapshot taken away, after --replay has left a scratch
// copy beside it. A log with no snapshot replays from the default SRAM, so the copy an
// earlier replay left must be cleared, not loaded in its place.
int run_replay_without_sram(const std::string& dir) {
    remove((dir + "/log/sram.raw").c_str());
    write_file(dir + "/log/sram_replay.raw", "left by an earlier replay\n");
    std::string sram;
    const std::string said = dir + "/replay-without-sram-stderr.txt";
    if (!load_replay(dir + "/log", said, sram)) return 1;
    check(sram == dir + "/log/sram_replay.raw" && !plat_readable(sram.c_str()),
          "a log with no SRAM snapshot replays from the default SRAM: the scratch copy an earlier replay left is cleared");
    check(lines_with(said, "SRAM snapshot") == 0, "and its summary line doesn't claim a snapshot");
    check(g_fast_disc && lines_with(said, "so the replay reads at") == 0,
          "a replay asked for the disc speed its log was recorded at reads at it and says nothing of it");
    return finish("--replay-no-sram");
}

// ---- A version 4 log, from before the motor lines, replays as it always did: three steps
// of reads of the four ports, each port at its logged state from its line's poll on. It has
// no disc line either, so it replays at disc console.
int run_replay_v4(const std::string& dir) {
    write_log(dir + "/log", "# gcn-recomp input log v4\n# game TEST00  started 2026-10-28T12:00:00\n"
                            "# epoch 1792000000\n"
                            "0 0 0 1 0000 80 80 80 80 00 00\n0 0 1 0 0000 80 80 80 80 00 00\n"
                            "0 0 2 0 0000 80 80 80 80 00 00\n0 0 3 0 0000 80 80 80 80 00 00\n"
                            "5 0 1 1 0100 E4 80 80 80 00 00\n8 0 0 1 1000 80 80 80 80 00 00\n# end 12 0\n");
    std::string sram;
    const std::string said = dir + "/replay-stderr.txt";
    if (!load_replay(dir + "/log", said, sram)) return 1;
    input_script_init("");
    power_on(1);
    std::string text;
    for (int s = 0; s < 3; s++)
        for (int i = 0; i < 4; i++) text += describe(read(i));
    const std::string rest = "1 0000 80 80 80 80 00 00\n", out = "0 0000 80 80 80 80 00 00\n";
    const std::string with_a = "1 0100 E4 80 80 80 00 00\n", start = "1 1000 80 80 80 80 00 00\n";
    check(text == rest + out + out + out + rest + with_a + out + out + start + with_a + out + out,
          "a version 4 log replays: every read of all four ports is its logged state");
    check(!g_fast_disc && lines_with(said, "[input] the log has no disc line, so the replay reads at disc console and "
                                           "not at disc fast\n") == 1,
          "a log with no disc line replays at disc console with disc fast asked for, and says so");
    return finish("--replay-v4");
}

// ---- A log recorded at disc console, written by hand, replays at disc console with disc
// fast asked for, and the replay's own log says disc console. One whose disc line names a
// speed the runtime doesn't know replays at disc console too, and says that rather than
// that the line is missing.
int run_replay_console(const std::string& dir) {
    write_log(dir + "/log", "# gcn-recomp input log v5\n# game TEST00  started 2026-10-28T12:00:00\n"
                            "# epoch 1792000000\n# disc console\n0 0 0 1 0000 80 80 80 80 00 00\n"
                            "# end 4 0\n");
    std::string sram;
    const std::string said = dir + "/replay-stderr.txt";
    if (!load_replay(dir + "/log", said, sram)) return 1;
    check(!g_fast_disc && lines_with(said, "[input] the log was recorded at disc console, so the replay reads at that "
                                           "and not at disc fast\n") == 1,
          "a log recorded at disc console replays at disc console with disc fast asked for, and says so");
    if (!input_log_start(dir + "/replay-log", dir + "/no-card.raw", sram)) return 1;
    check(lines_starting(dir + "/replay-log/inputs.txt", "# disc ") == "# disc console\n",
          "and the replay's own log says disc console");

    // A log whose disc line names a speed this runtime doesn't know. Its changes join the
    // first log's, which nothing reads from here on.
    write_log(dir + "/turbo", "# gcn-recomp input log v5\n# game TEST00  started 2026-10-28T12:00:00\n"
                              "# epoch 1792000000\n# disc turbo\n0 0 0 1 0000 80 80 80 80 00 00\n"
                              "# end 4 0\n");
    g_fast_disc = true;  // as settings.txt's disc fast or --disc=fast leave it
    const std::string said_turbo = dir + "/turbo-stderr.txt";
    if (!load_replay(dir + "/turbo", said_turbo, sram)) return 1;
    check(!g_fast_disc &&
              lines_with(said_turbo, "[input] the log's disc line names neither fast nor console, so the replay reads at "
                                     "disc console and not at disc fast\n") == 1 &&
              lines_with(said_turbo, "no disc line") == 0,
          "a log whose disc line names neither speed replays at disc console with disc fast asked for, and says that");
    return finish("--replay-console");
}

}  // namespace
