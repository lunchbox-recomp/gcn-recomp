// GCN_READY_FILE=PATH: the window frontend tells whoever started it how far along it is, so a
// launcher can keep its own window up until the game's has something to show. It writes a
// line `<word> <ms>` for each step, the milliseconds since the process started:
//
//   window <ms>   the first frame the window shows, the boot splash's or else the game's
//   frame <ms>    the first frame the game presents
//
// A --hidden run shows nothing, and still writes window, with frame, at the game's first frame,
// so a launcher that waits for both isn't left waiting.
//
// Each line goes in once. The file is rewritten through PATH.tmp, which then replaces it, so a
// reader sees it whole, with every line complete; a watcher of the file itself sees it renamed
// over, so watch its directory or open it again at each change. Nothing waits for the disk
// (plat_replace_whole_file), so a line costs the main thread no sync. A launcher removes the
// path before it starts the game: an earlier run's file stays there until this run's first
// line, often hundreds of milliseconds in. That first line replaces whatever the file held,
// which only guards a launcher that forgets.
#pragma once

// Appends the word's line to GCN_READY_FILE the first time it's called with that word, and
// does nothing after or without the variable. A line that can't be written (on Windows, a
// launcher holding the file open) is tried again at the word's next call. Main thread only.
void ready_mark(const char* word);

// Before a console reset runs the game again (plat_relaunch): takes GCN_READY_FILE out of the
// environment once this run has written its frame line, so the run after writes no second
// pair. A launcher waits for one window and one frame a launch, and has both. A reset before
// the game's first frame leaves it, so the run after writes its own pair over the file; its times
// count from this run's start on macOS and Linux, where the relaunch keeps the process, and
// from the new process's on Windows.
void ready_relaunching();

// `word ms` and a newline after what `path` holds, through `path`.tmp. False, with the reason on
// stderr, when it can't be written.
bool ready_file_append(const char* path, const char* word, long long ms);
// The same with nothing before it: `path` holds the one line after.
bool ready_file_start(const char* path, const char* word, long long ms);
