// Host OS services that differ between POSIX and Windows. Implemented in platform.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
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
// First entry in `dir` whose name ends with `suffix` (case-insensitive), or "".
std::string plat_find_file(const char* dir, const char* suffix);

// A read-only file supporting concurrent positional reads.
struct PlatFile;
PlatFile* plat_open_read(const char* path);
bool plat_read_at(PlatFile* f, uint64_t offset, void* dst, uint32_t len);
void plat_close_read(PlatFile* f);
// Raise the soft limit on open files to `n` when it is lower, as far as the hard limit allows.
// macOS stops at 256 files by default, which a disc overlay of many files passes. Does
// nothing where the platform has no such limit.
void plat_reserve_open_files(uint32_t n);
