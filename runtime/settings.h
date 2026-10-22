// The player's settings: settings.txt in the working directory.
//
// One `key value` a line, the value being the rest of the line; blank lines and lines
// whose first character past any spaces is # are ignored:
//
//   # Picture
//   aspect 16:9
//   fullscreen 1
//   scale 3
//
// The runtime owns three keys:
//   aspect       the output's shape, "W:H" with two positive integers, or "window" for
//                the window's own shape (settings_parse_aspect). Default 4:3
//   fullscreen   1 for fullscreen, 0 for a window. Default 0
//   scale        the internal resolution, N times 640x480, N a whole number from 1 up.
//                Default 2. The renderer caps it (gx::kMaxInternalScale)
// A bad value for one of them is reported on stderr and leaves the key as it was. Every
// other key is kept in `extra` as written, for a game's own hooks, which may read the file
// with settings_load too.
//
// The desktop frontend reads the file at start, and a flag beats it for that run without
// being written back. It applies `scale`; this build has no output aspect or fullscreen,
// so `aspect` and `fullscreen` are only read and kept.
#pragma once
#include <map>
#include <string>

struct Settings {
    std::string aspect = "4:3";
    bool fullscreen = false;
    int scale = 2;
    // Every key the runtime doesn't own, with its value as written; the last line wins.
    std::map<std::string, std::string> extra;
};

// The settings in the file at `path`, over the defaults. A missing file gives the defaults,
// and so does one that can't be read, with a line on stderr.
Settings settings_load(const std::string& path);

// Writes the runtime's keys from `s` to the file at `path`. Each line of a key it owns is
// rewritten with the new value, and an owned key the file lacks is added at the end. Every
// other line is kept as it was, comments, blank lines and unknown keys included; `extra`
// isn't written. The new file goes to `path`.tmp and is renamed over `path`, so a save that
// fails leaves the old file whole. False, with the reason on stderr, if it failed, and if
// the old file can't be read, so that its lines aren't lost.
bool settings_save(const std::string& path, const Settings& s);

// An aspect as the file writes it: "W:H", two positive integers such as 16:9, or "window".
// Sets `ratio` to W/H, or to 0 for "window". False, leaving `ratio` alone, for anything
// else.
bool settings_parse_aspect(const std::string& text, float& ratio);
