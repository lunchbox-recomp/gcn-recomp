// Which gamepad was last on each port, so that a gamepad finds its port again at the next
// launch whatever order the controllers connect in.
//
// saves/ports.txt is written by the runtime, one line a port. A line holds the port, 1 to
// 4, and either `none` or the gamepad last put there. That's its SDL GUID, its serial
// number (`-` when it has none) and its USB vendor and product IDs in hex:
//
//   1 030000005e040000130b000000000000 - 045e:0b13
//   2 none
//
// A serial is percent-encoded where it holds a space, a control character or a percent
// sign. A line that can't be read is reported and skipped.
//
// A saved port is a preference, not a reservation. A gamepad that's on a port stays there,
// so no player moves in the middle of a game, and a port whose gamepad isn't plugged in
// goes to whichever gamepad comes. A gamepad pulled out doesn't change the file either, so
// one that comes back later is still remembered.
//
// This file needs no SDL. gamepads.cpp reads a gamepad's identity from SDL and calls it.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// What tells one gamepad from another across launches.
struct PortDevice {
    std::string guid;    // SDL's GUID as 32 hex digits
    std::string serial;  // empty when the device has none
    uint16_t vendor = 0;
    uint16_t product = 0;

    bool operator==(const PortDevice&) const = default;
};

// The gamepad last on each port, 0 for port 1. An empty slot is a port with none.
using SavedPorts = std::array<std::optional<PortDevice>, 4>;

// A gamepad that's plugged in.
struct PresentDevice {
    PortDevice id;
    int index = -1;  // the player index SDL gave it, -1 for none
    int held = -1;   // the port it's on now, -1 if it has none yet
};

// The port, 0 to 3, for each device in `present`, or -1 for a device left out because no
// port is free. A device that holds a port keeps it, and a port in `pinned` (controls.txt
// binds a device to it) is never given. The others are placed in this order.
//   1. A saved port goes to a device with the same GUID and serial.
//   2. Then to one with the same serial and USB IDs under another GUID, which is the
//      same gamepad on another connection.
//   3. A device no saved port names takes the port of its player index if that's free.
//   4. Any device still without a port, including one a saved port names whose port was
//      taken, takes the lowest free port.
// Within a step a saved port takes the first device to connect that fits. Two gamepads
// with the same GUID and no serial can't be told apart, so they fill their ports in the
// order they connect.
std::vector<int> match_ports(const SavedPorts& saved, const std::vector<PresentDevice>& present,
                             const std::array<bool, 4>& pinned);

// Puts `device` on `port` in `saved`. True if that changed what was saved there.
bool ports_assign(SavedPorts& saved, int port, const PortDevice& device);

// The file's text for `saved`.
std::string ports_format(const SavedPorts& saved);
// The ports in `text`. A line that can't be read is skipped, with its line number and
// `name` on stderr.
SavedPorts ports_parse(const std::string& text, const char* name);

// The ports in the file at `path`. A missing file gives none. So does one that exists and
// can't be read, with a line on stderr, and `readable` is false then so that the caller
// doesn't save over what it couldn't read. A missing file is readable.
SavedPorts ports_load(const std::string& path, bool& readable);
// Writes `saved` to `path`, making its directory if need be. The new file goes to
// `path`.tmp and is renamed over `path`, so a save that fails leaves the old file whole.
// False, with the reason on stderr, if it failed.
bool ports_save(const std::string& path, const SavedPorts& saved);
