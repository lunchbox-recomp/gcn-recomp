// How a gamepad drives the GameCube's pad, and saves/pads.txt, which keeps what a player
// changed on the overlay's Controllers page (overlay.h).
//
// A binding gives each of the pad's controls one of the gamepad's buttons or axes, as SDL's
// gamepad API names them (SDL_GamepadButton, SDL_GamepadAxis), and the stick and trigger dead
// zones and the trigger's click. With nothing saved for a gamepad, its binding is the map
// gamepads.h describes: the faces labeled A, B, X and Y, Start, Z on the right shoulder, the
// D-pad, L and R on the triggers and the two sticks.
//
// saves/pads.txt is written by the runtime, a block for each gamepad a player has changed.
// The block's first line names the gamepad by its SDL GUID and its serial number (`-` for
// none, percent-encoded as ports.txt has it), and starts with `#`, so that controls.txt reads
// it as a comment. Each line after it is a controls.txt line (input_map.h), so a block can be
// copied into controls.txt as it is, where it binds the same buttons and axes but reads them
// as controls.txt does, not quite as here:
//   - Here a stick is taken over while either of its axes is past the dead zone, both axes
//     then, at 100 from the centre for full travel and with nothing taken off for the dead
//     zone, as the map always has been. There each axis has its own dead zone, the travel
//     past it is spread over the whole range, and that over the game's stick range. Raw 4001
//     on the main stick is 140 here and 128 there.
//   - A trigger clicks here once its raw travel passes the threshold. There the travel past
//     the dead zone is spread first, so the default click comes at about 30085, not 30000.
//   - There several lines may share a control; here each control has one source.
// A block reads:
//
//   # gamepad 030000005e040000130b000000000000 -
//   A        = "Xbox Wireless Controller" gamepad west
//   stick_y  = "Xbox Wireless Controller" gamepad lefty invert deadzone 0.122074
//   L        = "Xbox Wireless Controller" gamepad lefttrigger deadzone 0.0305185 threshold 0.915555
//
// A block lists every control. A control a block leaves out has its part of the map above,
// and a line that can't be read is reported and skipped. The name in the lines is the
// gamepad's, for controls.txt; this file goes by the block's first line, split on whitespace
// alone. The name is percent-encoded where it holds a `"`, a `%` or a control character,
// which controls.txt doesn't decode, and only those escapes are decoded here, so an older
// file's name with another `%XX` in it reads as it is. Two gamepads with the same GUID and no
// serial share a block.
//
// Press-to-bind (bind_listen, bind_hear) waits 5 s for the next button, or an axis pushed past
// half its travel, on one gamepad. Back and Guide cancel and are never bound. A button held or
// an axis past half as it starts is passed over until it's let go.
//
// This file needs no SDL. gamepads.cpp reads the gamepad's state from SDL and calls it, and
// pad_numbers.cpp checks at compile time that the numbers below are SDL's.
#pragma once
#include "hw/pad.h"
#include "port_memory.h"  // PortDevice
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// A gamepad's buttons and axes, numbered as SDL_GamepadButton and SDL_GamepadAxis are.
enum PadButton : int {
    kPadSouth, kPadEast, kPadWest, kPadNorth, kPadBack, kPadGuide, kPadStart, kPadLeftStick, kPadRightStick,
    kPadLeftShoulder, kPadRightShoulder, kPadUp, kPadDown, kPadLeft, kPadRight,
    kPadButtons = 26,
};
enum PadAxis : int { kPadLeftX, kPadLeftY, kPadRightX, kPadRightY, kPadLeftTrigger, kPadRightTrigger, kPadAxes };

// How far a trigger's axis (0 to 32767) goes before gamepads_merge takes it for the GameCube's
// digital L or R, the click at the bottom of its travel, unless a player has moved it. The
// overlay's chord and its menu's page turns take them the same way, the moved click too, on a
// gamepad SDL maps no click buttons on (overlay.h).
constexpr int kGamepadTriggerClick = 30000;

// The GameCube pad's controls, as controls.txt names them.
enum class PadControl : int {
    A, B, X, Y, Z, Start, DUp, DDown, DLeft, DRight, L, R, StickX, StickY, CStickX, CStickY, Count
};
constexpr int kPadControls = (int)PadControl::Count;
const char* pad_control_name(PadControl c);
// A stick axis, which takes a whole axis of the gamepad, as against a button or a trigger.
bool pad_control_is_stick(PadControl c);

// A button, or an axis: one way from its centre (`half` +1 or -1) or all of it (`half` 0,
// turned round with `invert`). A trigger's axis runs one way only, so all of it is 0 to 32767.
struct PadSource {
    bool axis = false;
    int index = 0;  // a PadButton, or a PadAxis
    int half = 0;
    bool invert = false;

    bool operator==(const PadSource&) const = default;
};
// The source as pads.txt writes it after `gamepad`: `west`, `leftx+`, `lefty`.
std::string pad_source_name(const PadSource& s);
// The button names pad_source_name uses, also the ones the run's output says (gamepads.h).
const char* pad_button_name(int button);

struct PadBinding {
    std::array<PadSource, kPadControls> source{};
    // Raw axis units, 0 to 32767: how far each stick goes before it's taken, how far a trigger
    // goes before its travel is, and where it clicks.
    int stick_deadzone = 4000;
    int cstick_deadzone = 1000;
    int trigger_deadzone = 1000;
    int trigger_click = kGamepadTriggerClick;

    bool operator==(const PadBinding&) const = default;
};

// The map gamepads.h describes, with `faces` the buttons that are the GameCube's A, B, X and
// Y on the gamepad (gamepads_faces).
PadBinding pad_binding_default(const std::array<int, 4>& faces);

// What a gamepad holds at one moment: each button, and each axis as SDL reads it.
struct PadReading {
    std::array<bool, kPadButtons> button{};
    std::array<int, kPadAxes> axis{};
};

// The pad as `b` reads `r`, merged into `p`: buttons are added, and a stick or trigger is
// taken over only while it's pushed past its dead zone. `p.connected` is left as it is.
void pad_binding_merge(const PadBinding& b, const PadReading& r, PadState& p);

// `c` given `s`. Another control that had the same input takes what `c` had, a stick axis
// among the four stick controls and a button, a trigger or one way of a stick axis among the
// others, so no control is left with nothing. That keeps an input off two controls, but for
// one: one way of a stick axis given to a button leaves a stick control on all of that axis,
// and all of it given to a stick control leaves a button on one way of it, since neither can
// take what the other had. Pushed that way, the stick then moves and presses the button too.
void pad_binding_set(PadBinding& b, PadControl c, const PadSource& s);

// ---- Press-to-bind

constexpr uint64_t kBindListenNs = 5000000000;  // how long it waits
constexpr int kBindHalf = 16384;                // an axis past this is pushed

struct BindListen {
    PadControl control = PadControl::A;
    uint64_t until = 0;   // host_ns when it gives up
    PadReading held;      // what was held as it began, each passed over until let go
};
enum class BindHeard { waiting, bound, cancelled, timed_out };

// Listening for `c` from `now`, with `r` what the gamepad holds as it begins.
BindListen bind_listen(PadControl c, const PadReading& r, uint64_t now);
// What the gamepad did by `now`, reading `r`. With bound, `out` is the source: for a stick
// axis, the stick axis that was pushed, turned round if it went left or up, so that the
// direction asked for (right, up) is the GameCube's plus; a stick axis takes no button or
// trigger. For L and R, a trigger, a button or one way of a stick axis, a trigger past half
// before a button pressed in the same reading, so that a pull that also clicks keeps its
// travel; for the others, a button, a trigger or one way of a stick axis.
BindHeard bind_hear(BindListen& l, const PadReading& r, uint64_t now, PadSource& out);

// ---- saves/pads.txt

// A gamepad's block. Its binding has `given` set for each control the block has a line for.
struct SavedBinding {
    PortDevice id;  // the GUID and serial; the USB IDs are left 0
    std::string name;
    PadBinding binding;
    std::array<bool, kPadControls> given{};
};
using SavedBindings = std::vector<SavedBinding>;

// The binding saved for `id`, with `fallback`'s source for each control the block leaves out
// and PadBinding's own value for each setting it leaves out, or `fallback` itself if none is
// saved. A line gives a dead zone only to a stick or a trigger and a threshold only to L or R.
PadBinding bindings_find(const SavedBindings& saved, const PortDevice& id, const PadBinding& fallback);
// Whether `a` and `b` share a block: the same GUID and serial, whatever their USB IDs.
bool bindings_same_block(const PortDevice& a, const PortDevice& b);
// `id`'s block replaced by `b`, every control given, or put last if it had none.
void bindings_put(SavedBindings& saved, const PortDevice& id, const std::string& name, const PadBinding& b);
// `id`'s block taken out. True if it had one.
bool bindings_forget(SavedBindings& saved, const PortDevice& id);

// The file's text for `saved`.
std::string bindings_format(const SavedBindings& saved);
// The blocks in `text`. A line that can't be read is skipped, with its line number and
// `name` on stderr. A later block for the same gamepad replaces an earlier one.
SavedBindings bindings_parse(const std::string& text, const char* name);
// The blocks in the file at `path`. A missing file gives none and is readable. A file that
// exists and can't be read gives none too, with a line on stderr, and `readable` false, so
// that the caller doesn't save over it.
SavedBindings bindings_load(const std::string& path, bool& readable);
// Writes `saved` to `path` as ports_save does: its directory made, a temp file renamed over
// it. False, with the reason on stderr, if it failed.
bool bindings_save(const std::string& path, const SavedBindings& saved);

// ---- The words of a controls.txt line, which input_map.cpp reads too

// `s` split into words: "quoted strings" are one word, '=' is a word of its own, and '#'
// outside quotes ends the line. False, with `err` set, for a quote that isn't closed.
bool controls_words(const char* s, std::vector<std::string>& out, std::string& err);
// A `gamepad` source's word, `west`, `leftx+` or `lefttrigger`, as `s`. False if it isn't one.
bool pad_source_parse(const std::string& word, PadSource& s);
// A number on a controls.txt line: any float strtof reads whole.
bool controls_float(const std::string& word, float& v);
