// Host OS services that differ between POSIX and Windows. Implemented in platform.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ---- virtual memory ----
// Reserve `size` bytes of address space without committing it. Returns nullptr on failure.
// With `at`, there and nowhere else (POSIX only; elsewhere `at` is ignored).
void* plat_reserve(size_t size, void* at = nullptr);
// Make [base, base+size) readable and writable within an existing reservation.
bool plat_commit(void* base, size_t size);

// ---- diagnostics ----
// Print a symbolized backtrace of the calling thread to stderr.
void plat_backtrace_print();
// Install handlers for an interrupt (SIGINT, SIGTERM or SIGHUP, or the console's Ctrl+C) and
// for memory faults. Call it before starting threads: on POSIX it blocks the interrupt
// signals, which every thread started after it inherits, and takes them on a thread of its
// own, so `on_interrupt` runs on an ordinary thread and can take locks and wait for others.
// `on_fault` runs in the signal handler and is passed the faulting address (0 if unknown).
// Both callbacks must not return.
void plat_install_crash_handlers(void (*on_interrupt)(), void (*on_fault)(const void* addr, int sig));
// Hard-exit after `seconds` no matter what, so a crash handler can never hang.
void plat_watchdog(int seconds, int exit_code);
// Record a fatal message where the platform keeps crash reports, which on Android is the
// one place the app's own logging cannot overwrite it. See the definition.
void plat_record_fatal(const char* msg);
// Run the plat_at_exit functions, then exit without running destructors or flushing other
// threads' buffers.
[[noreturn]] void plat_exit_now(int code);
// Run the plat_at_exit functions, then replace this process with a fresh run of the same
// program and arguments (argv as main had it). Returns only if that failed.
void plat_relaunch(char** argv);
// Add `fn` to what the two above run on the way out, for state that has to reach a file
// first, since they skip destructors. The functions run once, in the order they were added,
// on the thread that's leaving, and never inside a signal handler: an interrupt reaches
// on_interrupt on a thread of its own (plat_install_crash_handlers), and an exit after a
// fault skips them, since a crashed process may hold broken state. An exit from another
// thread while they run waits for them.
void plat_at_exit(void (*fn)());
// Run the plat_at_exit functions now, as plat_exit_now would, for a way out that doesn't
// go through it, such as abort().
void plat_run_at_exit();

// ---- threads ----
// Start a thread with a `stack_size`-byte stack (reserved, not committed).
bool plat_thread_start(void* (*fn)(void*), void* arg, size_t stack_size);
// Terminate the calling thread. Used when the process is shutting down, so skipping
// destructor/unwind cleanup is intentional.
[[noreturn]] void plat_thread_exit();
// What a host thread does, for a frontend that can tell the OS: a headset runtime's thread
// hints decide which cores and what priority the game's threads get beside its own.
enum class ThreadRole { GuestMain, GuestWorker, RenderMain, RenderWorker };
// Called by a thread about itself: names it (top -H, the profiles) and records its OS
// thread id under `role` for the listener below.
void plat_thread_role(ThreadRole role, const char* name);
// One listener, called with every thread recorded so far and then with each as it is
// recorded, on that thread. Null removes it.
void plat_set_thread_role_listener(void (*fn)(ThreadRole role, long tid));
// The threads recorded so far, by name and OS thread id.
struct PlatThread { const char* name; long tid; };
std::vector<PlatThread> plat_threads();
// What the OS's scheduler says of a thread so far: nanoseconds it has run, nanoseconds it
// has waited runnable for a core, and the core it last ran on. False where the platform
// does not say (Linux and Android do, in /proc).
bool plat_thread_sched(long tid, uint64_t* run_ns, uint64_t* wait_ns, int* cpu);

// ---- time ----
// Seconds to add to UTC to get local wall-clock time, including any DST in effect.
long plat_utc_offset_seconds();
// Milliseconds since this process started, by the OS's record of its start (macOS, Linux and
// Windows keep one), so the time before main and the static constructors is counted too.
// Where the OS can't say, since the first call.
double plat_ms_since_start();

// ---- the host ----
// The OS and its version ("macOS 26.6 (25G104)", "Linux 6.8.0"), and the CPU's model name,
// each "" where the platform doesn't say.
std::string plat_os_version();
std::string plat_cpu_model();

// ---- files ----
bool plat_readable(const char* path);
// Create `path` and any missing parents. Ignores already-exists.
void plat_make_dirs(const std::string& path);
// Replace `to` with `from`, overwriting an existing file.
bool plat_replace_file(const char* from, const char* to);
// Flushes `f` and puts what was written to it on the disk: F_FULLFSYNC on macOS, which the
// drive's own cache can't hold back as it can fsync, or fsync where the file system doesn't
// take it, _commit on Windows and fsync elsewhere. False if either step fails, with errno set.
bool plat_sync_file(FILE* f);
// When set, plat_sync_file calls this after its flush in place of the system's sync, and
// returns what it does. For tests.
extern bool (*plat_sync_for_tests)(FILE* f);
// Writes `size` bytes to `path` through `path`.tmp, which is put on the disk (plat_sync_file)
// and then replaces it. A save that fails leaves the old file whole and no temporary file, and
// a power cut during or after one leaves the old file or the new one whole, never an empty one.
// It says on stderr what failed, and a temporary file it can't make ends the line "so
// <unsaved> isn't saved".
bool plat_save_file(const std::string& path, const void* data, size_t size, const char* unsaved);
// plat_save_file, after making the directory `path` is in.
bool plat_save_file_making_dirs(const std::string& path, const void* data, size_t size, const char* unsaved);
// The whole file at `path` into `text`. False, with `err` the errno, if it can't be read:
// ENOENT where there's no file.
bool plat_read_file(const std::string& path, std::string& text, int& err);
// First entry in `dir` whose name ends with `suffix` (case-insensitive), or "".
std::string plat_find_file(const char* dir, const char* suffix);

// A read-only file supporting concurrent positional reads.
struct PlatFile;
PlatFile* plat_open_read(const char* path);
bool plat_read_at(PlatFile* f, uint64_t offset, void* dst, uint32_t len);
// The file's size in bytes. False if the platform can't say.
bool plat_file_size(PlatFile* f, uint64_t* size);
void plat_close_read(PlatFile* f);
// Raise the soft limit on open files to `n` when it is lower, as far as the hard limit allows.
// macOS stops at 256 files by default, which a disc overlay of many files passes. Returns ""
// when the limit now covers `n`, and otherwise why it doesn't, for a caller whose files then
// fail to open. Does nothing, and returns "", where the platform has no such limit.
std::string plat_reserve_open_files(uint32_t n);
