// GCN_READY_FILE=PATH: the window frontend tells whoever started it how far along it is, so a
// launcher can keep its own window up until the game's has something to show. It appends a
// line `<word> <ms>` for each step, the milliseconds since the process started:
//
//   window <ms>   the first frame the window shows, the boot splash's or else the game's
//   frame <ms>    the first frame the game presents
//
// Each line goes in once. The file is rewritten through PATH.tmp, which then replaces it, so a
// reader sees it whole, with every line complete; a watcher of the file itself sees it renamed
// over, so watch its directory or open it again at each change. Nothing waits for the disk
// (plat_replace_whole_file), so a line costs the main thread no sync. What the file held
// before is kept, so a launcher that wants only this run's lines removes it first.
#pragma once

// Appends the word's line to GCN_READY_FILE the first time it's called with that word, and
// does nothing after or without the variable. Main thread only.
void ready_mark(const char* word);

// `word ms` and a newline after what `path` holds, through `path`.tmp. False, with the reason on
// stderr, when it can't be written.
bool ready_file_append(const char* path, const char* word, long long ms);
