// Gamepad binding checks: the `test_pad_bindings` target. Runs with no disc image, device, SDL
// or game. runtime/pad_bindings.cpp is linked as it is, with port_memory.cpp for the serial's
// encoding and platform.cpp for its file replace.
//
// The map with nothing saved is checked against a copy of the fixed map gamepads_merge had
// before bindings, over a grid of stick, trigger and button readings. A binding is checked as
// the game would read it (pad_binding_merge), press-to-bind on readings made up here with the
// time moved by hand, and pads.txt by its bytes, a round trip, lines that can't be read and
// saves that fail. A save that fails must leave the old file whole.
//
// usage: test_pad_bindings <work dir>
// The work dir must not exist. It's made, and the files are left in it. Exits 0 only if
// every check passes.
#include "pad_bindings.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

std::string path_of(const char* name) { return g_dir + "/" + name; }

// Whether a line of `said` starts with `prefix`.
bool has_line(const std::string& said, const char* prefix) {
    return said.rfind(prefix, 0) == 0 || said.find(std::string("\n") + prefix) != std::string::npos;
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

// What stderr gets while `f` runs.
template <typename F> std::string stderr_of(const char* name, F f) {
    const std::string path = path_of(name);
    fflush(stderr);
    const int saved = dup(2);
    FILE* out = fopen(path.c_str(), "w");
    dup2(fileno(out), 2);
    f();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    fclose(out);
    return read_file(path);
}

const std::array<int, 4> kXbox = {kPadSouth, kPadEast, kPadWest, kPadNorth};
const std::array<int, 4> kNintendo = {kPadEast, kPadSouth, kPadNorth, kPadWest};

PadSource button(int b) { return {false, b, 0, false}; }
PadSource axis(int a, int half = 0, bool invert = false) { return {true, a, half, invert}; }

// The fixed map gamepads_merge had before bindings, with the gamepad's faces.
uint8_t axis_to_u8(int v, bool invert) {
    float f = v / 32767.0f;
    if (invert) f = -f;
    int r = 128 + (int)(f * 100.0f);
    return (uint8_t)std::clamp(r, 0, 255);
}
void old_map(const std::array<int, 4>& face, const PadReading& r, PadState& p) {
    auto b = [&](int btn) { return r.button[btn]; };
    if (b(face[0])) p.buttons |= PAD_A;
    if (b(face[1])) p.buttons |= PAD_B;
    if (b(face[2])) p.buttons |= PAD_X;
    if (b(face[3])) p.buttons |= PAD_Y;
    if (b(kPadStart)) p.buttons |= PAD_START;
    if (b(kPadRightShoulder)) p.buttons |= PAD_Z;
    if (b(kPadUp)) p.buttons |= PAD_UP;
    if (b(kPadDown)) p.buttons |= PAD_DOWN;
    if (b(kPadLeft)) p.buttons |= PAD_LEFT;
    if (b(kPadRight)) p.buttons |= PAD_RIGHT;
    int lx = r.axis[kPadLeftX], ly = r.axis[kPadLeftY];
    if (abs(lx) > 4000 || abs(ly) > 4000) { p.stick_x = axis_to_u8(lx, false); p.stick_y = axis_to_u8(ly, true); }
    int rx = r.axis[kPadRightX], ry = r.axis[kPadRightY];
    if (abs(rx) > 1000 || abs(ry) > 1000) { p.cstick_x = axis_to_u8(rx, false); p.cstick_y = axis_to_u8(ry, true); }
    int lt = r.axis[kPadLeftTrigger], rt = r.axis[kPadRightTrigger];
    if (lt > 1000) { p.trig_l = std::max(p.trig_l, (uint8_t)(lt * 255 / 32767)); if (lt > 30000) p.buttons |= PAD_L; }
    if (rt > 1000) { p.trig_r = std::max(p.trig_r, (uint8_t)(rt * 255 / 32767)); if (rt > 30000) p.buttons |= PAD_R; }
}

bool same(const PadState& a, const PadState& b) {
    return a.connected == b.connected && a.buttons == b.buttons && a.stick_x == b.stick_x && a.stick_y == b.stick_y &&
           a.cstick_x == b.cstick_x && a.cstick_y == b.cstick_y && a.trig_l == b.trig_l && a.trig_r == b.trig_r;
}

PadState merged(const PadBinding& b, const PadReading& r, PadState p = {}) {
    pad_binding_merge(b, r, p);
    return p;
}

PadReading pressing(int b) {
    PadReading r;
    r.button[b] = true;
    return r;
}

PadReading pushing(int a, int v) {
    PadReading r;
    r.axis[a] = v;
    return r;
}

// ---- With nothing saved, today's map

void default_is_old_map() {
    static const int kValues[] = {-32768, -32767, -20000, -4001, -4000, -1001, -1000, 0, 999, 1000, 1001,
                                  3999, 4000, 4001, 16384, 29999, 30000, 30001, 32767};
    for (const auto& faces : {kXbox, kNintendo}) {
        const PadBinding b = pad_binding_default(faces);
        bool all = true;
        // Each button alone, and all of them at once.
        for (int k = 0; k < kPadButtons; k++) all = all && same(merged(b, pressing(k)), [&] {
            PadState p;
            old_map(faces, pressing(k), p);
            return p;
        }());
        PadReading every;
        every.button.fill(true);
        PadState old;
        old_map(faces, every, old);
        all = all && same(merged(b, every), old);
        // Each stick's two axes over the grid, and each trigger, with the other stick pushed too.
        for (int x : kValues)
            for (int y : kValues) {
                PadReading r;
                r.axis = {x, y, y, x, x < 0 ? 0 : x, y < 0 ? 0 : y};
                PadState p, q;
                // What an earlier source left is taken over only while pushed, and a trigger
                // keeps the most of the two.
                q.stick_x = 3;
                q.cstick_y = 250;
                q.trig_l = 200;
                q.trig_r = 40;
                q.buttons = PAD_START;
                p = q;
                old_map(faces, r, p);
                pad_binding_merge(b, r, q);
                all = all && same(p, q);
            }
        check(all, faces == kXbox ? "with nothing saved, an Xbox pad's binding reads as the fixed map did, button by "
                                    "button and over a grid of sticks and triggers"
                                  : "and the same for a pad with Nintendo's faces, east A and south B");
    }
    const PadBinding b = pad_binding_default(kXbox);
    check(b.source[(int)PadControl::A] == button(kPadSouth) && b.source[(int)PadControl::Z] == button(kPadRightShoulder) &&
              b.source[(int)PadControl::L] == axis(kPadLeftTrigger) &&
              b.source[(int)PadControl::StickY] == axis(kPadLeftY, 0, true) && b.stick_deadzone == 4000 &&
              b.cstick_deadzone == 1000 && b.trigger_deadzone == 1000 && b.trigger_click == kGamepadTriggerClick,
          "the map gives A south, Z the right shoulder, L the left trigger, stick_y the left stick's y turned round");
}

// ---- A binding as the game reads it

void west_on_a() {
    PadBinding b = pad_binding_default(kXbox);
    pad_binding_set(b, PadControl::A, button(kPadWest));
    check(merged(b, pressing(kPadWest)).buttons == PAD_A, "west bound to A: a west press is A, and only A");
    check(merged(b, pressing(kPadSouth)).buttons == PAD_X && b.source[(int)PadControl::X] == button(kPadSouth),
          "X, which had west, takes south, what A had, so south is X");
    PadBinding c = pad_binding_default(kXbox);
    pad_binding_set(c, PadControl::A, axis(kPadRightTrigger));
    check(merged(c, pushing(kPadRightTrigger, 16385)).buttons == PAD_A &&
              merged(c, pushing(kPadRightTrigger, 16384)).buttons == 0 && c.source[(int)PadControl::R] == button(kPadSouth) &&
              merged(c, pressing(kPadSouth)).trig_r == 255,
          "the right trigger bound to A is A past half its travel, and R takes south, what A had");
    PadBinding z = pad_binding_default(kXbox);
    pad_binding_set(z, PadControl::L, button(kPadRightShoulder));
    check(z.source[(int)PadControl::Z] == axis(kPadLeftTrigger) && merged(z, pressing(kPadRightShoulder)).buttons == PAD_L,
          "the right shoulder bound to L: Z takes the left trigger, and the shoulder is L alone");
    PadBinding d = pad_binding_default(kXbox);
    pad_binding_set(d, PadControl::DUp, axis(kPadLeftY, -1));
    const PadState up = merged(d, pushing(kPadLeftY, -20000));
    check((up.buttons & PAD_UP) && !(merged(d, pushing(kPadLeftY, 20000)).buttons & PAD_UP),
          "the left stick's up bound to dpad_up presses it when pushed up past half, not down");
}

void sticks_and_triggers() {
    PadBinding b = pad_binding_default(kXbox);
    pad_binding_set(b, PadControl::StickX, axis(kPadRightX));
    pad_binding_set(b, PadControl::StickY, axis(kPadRightY, 0, true));
    const PadState p = merged(b, pushing(kPadRightX, 32767));
    check(p.stick_x == 228 && p.cstick_x == 0x80 && b.source[(int)PadControl::CStickX] == axis(kPadLeftX) &&
              b.source[(int)PadControl::CStickY] == axis(kPadLeftY, 0, true),
          "the right stick on the main stick moves it to full, and the C-stick takes the left stick, which the main "
          "stick had");
    const PadState lx = merged(b, pushing(kPadLeftX, 32767));
    check(lx.stick_x == 0x80 && lx.cstick_x == 228, "the left stick's x moves the C-stick, and not the main stick");
    PadBinding xy = pad_binding_default(kXbox);
    pad_binding_set(xy, PadControl::StickX, axis(kPadLeftY));
    check(xy.source[(int)PadControl::StickY] == axis(kPadLeftX) && merged(xy, pushing(kPadLeftY, 32767)).stick_y == 0x80,
          "the left stick's y on stick_x: stick_y takes its x, so neither axis is on both");

    // However a binding is changed, no gamepad input is on two controls: a stick's axes among
    // the stick controls, and the buttons and triggers among the others.
    PadBinding any = pad_binding_default(kNintendo);
    const PadSource kTries[] = {axis(kPadRightY), button(kPadLeftShoulder), axis(kPadLeftTrigger), axis(kPadLeftX, -1),
                                button(kPadSouth), axis(kPadLeftX), axis(kPadRightX, 0, true), button(kPadRightShoulder)};
    auto inputs = [](const PadBinding& k, bool sticks) {
        std::vector<std::array<int, 3>> in;
        for (int d = 0; d < kPadControls; d++)
            if (pad_control_is_stick((PadControl)d) == sticks) in.push_back({k.source[d].axis, k.source[d].index, k.source[d].half});
        std::sort(in.begin(), in.end());
        return in;
    };
    auto once = [](const std::vector<std::array<int, 3>>& in) { return std::adjacent_find(in.begin(), in.end()) == in.end(); };
    bool kept = true;
    for (int c = 0; c < kPadControls; c++)
        for (const PadSource& s : kTries) {
            if ((s.axis && s.half == 0 && s.index != kPadLeftTrigger) != pad_control_is_stick((PadControl)c)) continue;
            pad_binding_set(any, (PadControl)c, s);
            kept = kept && any.source[c] == s && once(inputs(any, true)) && once(inputs(any, false));
        }
    check(kept, "binding any control to any input it takes leaves no input on two controls");

    PadBinding t = pad_binding_default(kXbox);
    t.trigger_click = 20000;
    t.trigger_deadzone = 5000;
    const PadState at = merged(t, pushing(kPadLeftTrigger, 20001));
    check((at.buttons & PAD_L) && at.trig_l == 20001 * 255 / 32767 && merged(t, pushing(kPadLeftTrigger, 5000)).trig_l == 0,
          "a trigger click moved to 20000 clicks past it, and a dead zone of 5000 keeps its travel out up to there");
    t.stick_deadzone = 10000;
    check(merged(t, pushing(kPadLeftX, 9999)).stick_x == 0x80 && merged(t, pushing(kPadLeftX, 10001)).stick_x != 0x80,
          "a stick dead zone of 10000 leaves the stick at rest up to it");

    PadBinding l = pad_binding_default(kXbox);
    pad_binding_set(l, PadControl::L, button(kPadLeftShoulder));
    const PadState sh = merged(l, pressing(kPadLeftShoulder));
    check((sh.buttons & PAD_L) && sh.trig_l == 255, "the left shoulder bound to L is the whole trigger and its click");
    pad_binding_set(l, PadControl::R, axis(kPadLeftX, -1));
    const PadState left = merged(l, pushing(kPadLeftX, -32768));
    check(left.trig_r == 255 && (left.buttons & PAD_R) && merged(l, pushing(kPadLeftX, 32767)).trig_r == 0,
          "the left stick pushed left bound to R is R's travel, right of centre nothing");
}

// ---- Press-to-bind

void listen() {
    PadReading rest;
    PadReading holding_a = pressing(kPadSouth);
    PadSource got;
    BindListen l = bind_listen(PadControl::A, holding_a, 1000);
    check(bind_hear(l, holding_a, 2000, got) == BindHeard::waiting, "a button held as it starts is passed over");
    check(bind_hear(l, rest, 3000, got) == BindHeard::waiting, "letting it go binds nothing");
    check(bind_hear(l, holding_a, 4000, got) == BindHeard::bound && got == button(kPadSouth),
          "pressed again once let go, it's bound");

    l = bind_listen(PadControl::A, rest, 0);
    check(bind_hear(l, rest, kBindListenNs - 1, got) == BindHeard::waiting &&
              bind_hear(l, rest, kBindListenNs, got) == BindHeard::timed_out,
          "with nothing pressed it waits 5 s and then gives up");
    l = bind_listen(PadControl::B, rest, 0);
    check(bind_hear(l, pressing(kPadBack), 10, got) == BindHeard::cancelled, "Back cancels");
    l = bind_listen(PadControl::B, rest, 0);
    check(bind_hear(l, pressing(kPadGuide), 10, got) == BindHeard::cancelled, "Guide cancels");
    l = bind_listen(PadControl::B, pressing(kPadBack), 0);
    check(bind_hear(l, pressing(kPadBack), 10, got) == BindHeard::waiting,
          "a Back held as it starts cancels nothing until it's pressed again");

    l = bind_listen(PadControl::X, rest, 0);
    check(bind_hear(l, pushing(kPadLeftX, kBindHalf), 10, got) == BindHeard::waiting &&
              bind_hear(l, pushing(kPadLeftX, -kBindHalf - 1), 20, got) == BindHeard::bound && got == axis(kPadLeftX, -1),
          "an axis at half is passed over, and past half, left, gives one way of it for a button");
    l = bind_listen(PadControl::Y, rest, 0);
    check(bind_hear(l, pushing(kPadRightTrigger, 20000), 10, got) == BindHeard::bound && got == axis(kPadRightTrigger),
          "a trigger past half gives all of it for a button");
    l = bind_listen(PadControl::Y, pushing(kPadRightTrigger, 32767), 0);
    const bool held_trigger = bind_hear(l, pushing(kPadRightTrigger, 32767), 10, got) == BindHeard::waiting;
    bind_hear(l, rest, 20, got);
    check(held_trigger && bind_hear(l, pushing(kPadRightTrigger, 32767), 30, got) == BindHeard::bound,
          "a trigger in as it starts is passed over until it's back under half");
    l = bind_listen(PadControl::Y, pushing(kPadRightTrigger, 32767), 0);
    bind_hear(l, pushing(kPadRightTrigger, kBindHalf), 10, got);
    check(bind_hear(l, pushing(kPadRightTrigger, 32767), 20, got) == BindHeard::bound,
          "back to exactly half is let go, so pulled again it's bound");

    l = bind_listen(PadControl::StickX, rest, 0);
    const bool no_button = bind_hear(l, pressing(kPadSouth), 10, got) == BindHeard::waiting;
    const bool no_trigger = bind_hear(l, pushing(kPadLeftTrigger, 32767), 20, got) == BindHeard::waiting;
    check(no_button && no_trigger && bind_hear(l, pushing(kPadRightX, 30000), 30, got) == BindHeard::bound &&
              got == axis(kPadRightX),
          "a stick axis passes over buttons and triggers, and takes a stick pushed right as all of its x");
    l = bind_listen(PadControl::StickY, rest, 0);
    check(bind_hear(l, pushing(kPadLeftY, -30000), 10, got) == BindHeard::bound && got == axis(kPadLeftY, 0, true),
          "pushed up for stick_y, it's the stick's y turned round, as the map has it");
    l = bind_listen(PadControl::L, rest, 0);
    check(bind_hear(l, pressing(kPadLeftShoulder), 10, got) == BindHeard::bound && got == button(kPadLeftShoulder),
          "L takes a button");
    // A GameCube adapter's full pull: the trigger click (SDL's misc4) and the trigger's axis in one reading.
    PadReading pull = pressing(23);
    pull.axis[kPadRightTrigger] = 32767;
    l = bind_listen(PadControl::R, rest, 0);
    const bool r_axis = bind_hear(l, pull, 10, got) == BindHeard::bound && got == axis(kPadRightTrigger);
    pull.axis[kPadRightTrigger] = kBindHalf;
    l = bind_listen(PadControl::R, rest, 0);
    const bool r_button = bind_hear(l, pull, 10, got) == BindHeard::bound && got == button(23);
    pull.axis[kPadRightTrigger] = 32767;
    l = bind_listen(PadControl::Z, rest, 0);
    check(r_axis && r_button && bind_hear(l, pull, 10, got) == BindHeard::bound && got == button(23),
          "for L or R a trigger past half beats a button in the same reading, its travel kept; at half, and for "
          "the other controls, the button wins");
    l = bind_listen(PadControl::L, pushing(kPadLeftTrigger, 32767), 0);
    const bool l_held = bind_hear(l, pushing(kPadLeftTrigger, 32767), 10, got) == BindHeard::waiting;
    bind_hear(l, rest, 20, got);
    check(l_held && bind_hear(l, pushing(kPadLeftTrigger, 32767), 30, got) == BindHeard::bound && got == axis(kPadLeftTrigger),
          "L picked with a trigger pulled in waits for it to be let go, then takes it pulled again");
}

// ---- pads.txt

const char* const kExpected =
    "# Each gamepad's controls, as a player set them. The runtime writes this file. A block\n"
    "# starts with the gamepad's GUID and serial, and its lines are controls.txt's.\n"
    "# gamepad 030000005e040000130b000000000000 -\n"
    "A = \"Xbox Wireless Controller\" gamepad west\n"
    "B = \"Xbox Wireless Controller\" gamepad east\n"
    "X = \"Xbox Wireless Controller\" gamepad south\n"
    "Y = \"Xbox Wireless Controller\" gamepad north\n"
    "Z = \"Xbox Wireless Controller\" gamepad rightshoulder\n"
    "start = \"Xbox Wireless Controller\" gamepad start\n"
    "dpad_up = \"Xbox Wireless Controller\" gamepad up\n"
    "dpad_down = \"Xbox Wireless Controller\" gamepad down\n"
    "dpad_left = \"Xbox Wireless Controller\" gamepad left\n"
    "dpad_right = \"Xbox Wireless Controller\" gamepad right\n"
    "L = \"Xbox Wireless Controller\" gamepad lefttrigger deadzone 0.0305185 threshold 0.915555\n"
    "R = \"Xbox Wireless Controller\" gamepad righttrigger deadzone 0.0305185 threshold 0.915555\n"
    "stick_x = \"Xbox Wireless Controller\" gamepad leftx deadzone 0.122074\n"
    "stick_y = \"Xbox Wireless Controller\" gamepad lefty invert deadzone 0.122074\n"
    "cstick_x = \"Xbox Wireless Controller\" gamepad rightx deadzone 0.0305185\n"
    "cstick_y = \"Xbox Wireless Controller\" gamepad righty invert deadzone 0.0305185\n";

PortDevice xbox() { return {"030000005e040000130b000000000000", "", 0, 0}; }

void file_text() {
    PadBinding b = pad_binding_default(kXbox);
    pad_binding_set(b, PadControl::A, button(kPadWest));
    SavedBindings saved;
    bindings_put(saved, xbox(), "Xbox Wireless Controller", b);
    check(bindings_format(saved) == kExpected, "a block is the gamepad's line, then a controls.txt line for each control");
    SavedBindings back;
    const std::string said = stderr_of("parse-clean.txt", [&] { back = bindings_parse(kExpected, "pads.txt"); });
    check(said.empty() && back.size() == 1 && bindings_find(back, xbox(), PadBinding{}) == b,
          "read back, the block gives the same binding, and nothing is said");
    check(bindings_format(back) == kExpected && back[0].name == "Xbox Wireless Controller",
          "and written again it's the same text, the gamepad's name and all");

    // A serial with a space, every setting moved, every source kind.
    PadBinding m = pad_binding_default(kNintendo);
    m.stick_deadzone = 1;
    m.cstick_deadzone = 32000;
    m.trigger_deadzone = 0;
    m.trigger_click = 12345;
    pad_binding_set(m, PadControl::StickX, axis(kPadRightX, 0, true));
    pad_binding_set(m, PadControl::DLeft, axis(kPadLeftX, -1));
    pad_binding_set(m, PadControl::R, button(kPadLeftShoulder));
    const PortDevice pro{"050000007e0500000920000001800000", "AB 12%", 0, 0};
    bindings_put(back, pro, "Pro Controller", m);
    const SavedBindings again = bindings_parse(bindings_format(back), "pads.txt");
    check(again.size() == 2 && bindings_find(again, pro, PadBinding{}) == m && bindings_find(again, xbox(), PadBinding{}) == b,
          "two blocks, a serial with a space and a percent sign, every setting and every kind of source come back as they were");
    check(bindings_format(back).find("# gamepad 050000007e0500000920000001800000 AB%2012%25\n") != std::string::npos,
          "the serial is percent-encoded as ports.txt has it");
    PadBinding other = m;
    other.trigger_click = 1000;
    bindings_put(back, pro, "Pro Controller", other);
    check(back.size() == 2 && bindings_find(back, pro, PadBinding{}) == other, "putting a gamepad again replaces its block");
    check(bindings_forget(back, pro) && back.size() == 1 && !bindings_forget(back, pro) &&
              bindings_find(back, pro, b) == b,
          "a block taken out leaves the gamepad with what it's given instead");
    const PortDevice serialed{xbox().guid, "1234", 0, 0};
    check(bindings_find(back, serialed, m) == m, "the same GUID with another serial is another gamepad");
    const PortDevice other_ids{xbox().guid, "", 0x1234, 0x5678};
    check(bindings_same_block(xbox(), other_ids) && !bindings_same_block(xbox(), serialed) &&
              !bindings_same_block(xbox(), pro),
          "one block is the same GUID and serial, whatever the USB IDs");

    // Each of the words controls.txt's tokenizer stops at, in a serial and in a name.
    bool marks = true;
    for (const char* mark : {"#", "=", "\""}) {
        const PortDevice odd{"050000007e0500000920000001800000", std::string("AB") + mark + "12", 0, 0};
        const std::string name = std::string("Pad ") + mark + "Pro" + mark + " 100%";
        SavedBindings one;
        bindings_put(one, odd, name, m);
        const std::string text = bindings_format(one);
        SavedBindings read;
        const std::string said = stderr_of("parse-marks.txt", [&] { read = bindings_parse(text, "pads.txt"); });
        marks = marks && said.empty() && read.size() == 1 && read[0].id.serial == odd.serial && read[0].name == name &&
                bindings_find(read, odd, PadBinding{}) == m && bindings_format(read) == text;
    }
    check(marks, "a serial or a name with a '#', a '=' or a '\"' in it comes back as it was, with nothing said");
}

void file_forgives() {
    // A block with only A, after one with a bad line, then a later block for the same gamepad.
    const std::string text =
        "# a comment\n"
        "A = \"pad\" gamepad west\n"
        "# gamepad 030000005e040000130b000000000000 -\n"
        "A = \"Xbox Wireless Controller\" gamepad west\n"
        "B = \"Xbox Wireless Controller\" gamepad sideways\n"
        "stick_x = \"Xbox Wireless Controller\" gamepad south\n"
        "stick_y = \"Xbox Wireless Controller\" gamepad leftx+\n"
        "L = \"Xbox Wireless Controller\" gamepad lefttrigger+\n"
        "Q = \"Xbox Wireless Controller\" gamepad south\n"
        "Z = \"Xbox Wireless Controller\" button 3\n"
        "R = \"Xbox Wireless Controller\" gamepad righttrigger threshold 2\n"
        "# gamepad 000000000000000000000000000000zz -\n"
        "\n"
        "# gamepad 03000000c82d00000000000000000000 -\n"
        "L = \"8BitDo\" gamepad lefttrigger deadzone 0.5\n"
        "# gamepad 03000000c82d00000000000000000000 -\n"
        "Y = \"8BitDo\" gamepad south\n";
    SavedBindings saved;
    const std::string said = stderr_of("parse-bad.txt", [&] { saved = bindings_parse(text, "pads.txt"); });
    const PadBinding fallback = pad_binding_default(kXbox);
    PadBinding want = fallback;
    want.source[(int)PadControl::A] = button(kPadWest);
    check(bindings_find(saved, xbox(), fallback) == want,
          "a block with only A good gives A west, and every other control the map's, its X too");
    const int lines = (int)std::count(said.begin(), said.end(), '\n');
    check(lines == 9 && said.find("pads.txt:2: ") != std::string::npos && said.find("pads.txt:5: ") != std::string::npos &&
              said.find("pads.txt:11: ") != std::string::npos && said.find("pads.txt:12: ") != std::string::npos,
          "each line that can't be read is said once with its number: a control before any block, an unknown source or "
          "control, a button on a stick axis, a half on a stick axis or a trigger, a controls.txt source, a threshold "
          "past 1, a GUID that isn't one");
    const PortDevice bitdo{"03000000c82d00000000000000000000", "", 0, 0};
    PadBinding later = fallback;
    later.source[(int)PadControl::Y] = button(kPadSouth);
    check(bindings_find(saved, bitdo, fallback) == later,
          "a later block for the same gamepad replaces the earlier one, settings and all");
}

// Lines that read as words but give a control what it can't take, each in a good block.
void file_refuses() {
    static const char* const kRefused[] = {
        "A = \"pad\" gamepad west invert",            // a one-way control turned round
        "stick_x = \"pad\" gamepad lefttrigger",      // a trigger on a stick axis
        "A = \"pad\" gamepad west deadzone 0.2",      // a dead zone on a button
        "A = \"pad\" gamepad west threshold 0.5",     // a click on a button
        "stick_x = \"pad\" gamepad leftx threshold 0.5",
        "L = \"pad\" gamepad lefttrigger deadzone -0.1",
        "L = \"pad\" gamepad lefttrigger deadzone nan",
        "R = \"pad\" gamepad righttrigger threshold 1",
    };
    const PadBinding fallback = pad_binding_default(kXbox);
    bool each = true;
    for (const char* line : kRefused) {
        SavedBindings saved;
        const std::string text = std::string("# gamepad 030000005e040000130b000000000000 -\n") + line + "\n";
        const std::string said = stderr_of("parse-refused.txt", [&] { saved = bindings_parse(text, "pads.txt"); });
        const bool refused = saved.size() == 1 && bindings_find(saved, xbox(), fallback) == fallback &&
                             said.find("pads.txt:2: ") == 0 && std::count(said.begin(), said.end(), '\n') == 1;
        if (!refused) printf("     not refused: %s\n", line);
        each = each && refused;
    }
    check(each, "a control turned round, a trigger on a stick axis, a dead zone on a button, a threshold on anything "
                "but L or R, and a negative or NaN fraction are each refused, said once");

    // A good block, a header that isn't one, then a line that would have gone into the block.
    const std::string text =
        "# gamepad 030000005e040000130b000000000000 -\n"
        "A = \"pad\" gamepad west\n"
        "# gamepad 030000005e040000130b000000000000 - extra\n"
        "B = \"pad\" gamepad north\n";
    SavedBindings saved;
    const std::string said = stderr_of("parse-header.txt", [&] { saved = bindings_parse(text, "pads.txt"); });
    PadBinding want = fallback;
    want.source[(int)PadControl::A] = button(kPadWest);
    check(saved.size() == 1 && bindings_find(saved, xbox(), fallback) == want && has_line(said, "pads.txt:3: ") &&
              has_line(said, "pads.txt:4: "),
          "a header with a word too many is refused, and the lines after it go into no block");

    const std::string word(300, 'q');
    const std::string long_said = stderr_of("parse-long.txt", [&] {
        bindings_parse("# gamepad 030000005e040000130b000000000000 -\n" + word + " = \"pad\" gamepad west\n"
                       "A = \"pad\" gamepad west " + word + "\n", "pads.txt");
    });
    check(std::count(long_said.begin(), long_said.end(), '\n') == 2 && long_said.size() < 300,
          "an unknown control or option of 300 letters is said cut short");
}


void files() {
    bool readable = false;
    const SavedBindings none = bindings_load(path_of("no-such.txt"), readable);
    check(none.empty() && readable, "a missing file gives no blocks, and may be written");
    const std::string dir_path = path_of("a-dir.txt");
    mkdir(dir_path.c_str(), 0755);
    std::string said = stderr_of("load-dir.txt", [&] { bindings_load(dir_path, readable); });
    check(!readable && !said.empty(), "a file that can't be read gives no blocks, says so, and isn't to be saved over");

    PadBinding b = pad_binding_default(kXbox);
    pad_binding_set(b, PadControl::A, button(kPadWest));
    SavedBindings saved;
    bindings_put(saved, xbox(), "Xbox Wireless Controller", b);
    const std::string file = path_of("saves/pads.txt");
    check(bindings_save(file, saved) && read_file(file) == kExpected, "a save makes saves/ and writes the file's text");
    const SavedBindings back = bindings_load(file, readable);
    check(readable && bindings_find(back, xbox(), PadBinding{}) == b, "loaded, it gives the binding back");

    mkdir((file + ".tmp").c_str(), 0755);
    SavedBindings changed = saved;
    bindings_put(changed, {"050000007e0500000920000001800000", "", 0, 0}, "Pro Controller", b);
    bool ok = true;
    said = stderr_of("save-fail.txt", [&] { ok = bindings_save(file, changed); });
    check(!ok && !said.empty() && read_file(file) == kExpected, "a save that fails says why and leaves the old file whole");
}

void words() {
    std::vector<std::string> w;
    std::string err;
    check(controls_words("A = \"Pad # 2\" gamepad west # B\n", w, err) &&
              w == std::vector<std::string>{"A", "=", "Pad # 2", "gamepad", "west"},
          "a quoted name is one word, '=' is a word of its own, and '#' outside quotes ends the line");
    w.clear();
    check(!controls_words("A=\"Pad", w, err) && !err.empty(), "a quote that isn't closed is an error");
    bool names = true;
    for (int k = 0; k < kPadButtons; k++) {
        PadSource s;
        names = names && pad_source_parse(pad_button_name(k), s) && s == button(k) && pad_source_name(s) == pad_button_name(k);
    }
    for (int a = 0; a < kPadAxes; a++) {
        PadSource s;
        names = names && pad_source_parse(pad_source_name(axis(a)), s) && s == axis(a);
    }
    PadSource s;
    check(names && pad_source_parse("righty-", s) && s == axis(kPadRightY, -1) && pad_source_name(s) == "righty-" &&
              !pad_source_parse("lefttrigger-", s) && !pad_source_parse("west+", s) && !pad_source_parse("3", s),
          "every button and axis name reads back as itself, a stick axis takes + or -, and a trigger or a button doesn't");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_pad_bindings <work dir>\n");
        return 2;
    }
    g_dir = argv[1];
    struct stat st;
    if (stat(g_dir.c_str(), &st) == 0 || mkdir(g_dir.c_str(), 0755) != 0) {
        fprintf(stderr, "%s exists or can't be made: give a new directory\n", g_dir.c_str());
        return 2;
    }
    default_is_old_map();
    west_on_a();
    sticks_and_triggers();
    listen();
    file_text();
    file_forgives();
    file_refuses();
    files();
    words();
    printf("test_pad_bindings: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
