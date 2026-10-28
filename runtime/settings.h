// The player's settings: settings.txt in the working directory.
//
// One `key value` a line, the value being the rest of the line; blank lines and lines
// whose first character past any spaces is # are ignored. A UTF-8 byte order mark at the
// start of the file is skipped, and a save writes it back:
//
//   # Picture
//   aspect 16:9
//   fullscreen 1
//   scale 3
//   disc console
//
// The runtime owns four keys:
//   aspect       the output's shape, "W:H" with two whole numbers from 1 to 9999, or
//                "window" for the window's own shape (settings_parse_aspect). Default 4:3
//   fullscreen   1 for fullscreen, 0 for a window. Default 0
//   scale        the internal resolution, N times 640x480, N a whole number from 1 up.
//                Default 2. The renderer caps it (gx::kMaxInternalScale)
//   disc         how long a disc read takes (hw/drive.h). "fast" reads every block at the
//                32 MiB/s of one the drive has read ahead, and "console" times reads as a
//                console's drive does, seeks and all. Default fast
// A bad value for one of them is reported on stderr and leaves the key as it was. Every
// other key is left alone: a load skips it and a save keeps its line.
//
// The desktop frontend reads the file at start, and a flag beats it for that run without
// being written back. It applies `scale` and `disc` (--disc=fast or --disc=console).
// `--aspect` and `--fullscreen` don't read the file yet, so `aspect` and `fullscreen` are
// only read and kept.
#pragma once
#include <string>

struct Settings {
    std::string aspect = "4:3";
    bool fullscreen = false;
    int scale = 2;
    bool fast_disc = true;  // disc fast, or disc console when false
};

// The settings in the file at `path`, over the defaults. A missing file gives the defaults,
// and so does one that can't be read, with a line on stderr.
Settings settings_load(const std::string& path);

// Writes the runtime's keys from `s` to the file at `path`. Each line of a key it owns is
// rewritten with the new value, and an owned key the file lacks is added at the end. That
// line, and a last line the file left without a newline, end as the file's first newline
// does, in CR LF or LF. Every other line is kept as it was, comments, blank lines and unknown
// keys included, and so is a byte order mark at the start of the file. The new file goes to
// `path`.tmp and is renamed over `path`, so a save that fails leaves the old file whole.
// False, with the reason on stderr, if it failed, if the old file can't be read, so that its
// lines aren't lost, and if `s` holds a value a load would skip, such as an aspect
// settings_parse_aspect refuses (one with a newline included) or a scale under 1. Nothing is
// written then.
bool settings_save(const std::string& path, const Settings& s);

// An aspect as the file writes it: "W:H", two whole numbers from 1 to 9999 such as 16:9, or
// "window". Sets `ratio` to W/H, or to 0 for "window". False, leaving `ratio` alone, for
// anything else.
bool settings_parse_aspect(const std::string& text, float& ratio);

// Reads a disc speed as the file and --disc write it. "fast" sets `fast`, and "console"
// clears it. False, leaving `fast` alone, for anything else.
bool settings_parse_disc(const std::string& text, bool& fast);
