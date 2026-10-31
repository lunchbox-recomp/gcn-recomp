// The runtime's fakes and the case runner that tools/test_interp_run.cpp and tools/test_code_table.cpp
// share. They run with no device, disc or game.
//
// recomp/fixture_programs.py writes made-up programs, and recomp.py compiles them into
// <build>/interp_fixtures/ as it does a game. A case runs one natively, then through the interpreter
// from the same state, once with only its own function forced and once with every guarded function
// forced. The CPU, the RAM the programs use, the guest clock and the calls into the runtime must come
// out the same, and an interpreted run must have interpreted something.
//
// A case whose code was changed in RAM runs a twin natively instead, a function holding the code as
// RAM now has it. The runtime's services are fakes that record each call, and the ones that resume a
// guest do it as threads.cpp does.
#pragma once
#include "interp/interp.h"
#include "fixture_programs.h"
#include "runtime.h"
#include <map>
#include <string>
#include <vector>

struct Call {
    std::string fn;
    uint32_t a, b, c;
    bool operator==(const Call&) const = default;
};
extern std::vector<Call> g_calls;

void note(const char* fn, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);

extern std::vector<std::string> g_logs;  // --log=interp's lines
extern int g_failed;
extern std::string g_fatal;  // what stopped the last guarded call

// The setjmp sites' resume points, kept as threads.cpp keeps them.
struct GuestJmp {
    jmp_buf jb;
    uint32_t lr, sp, depth;
};
extern std::map<uint32_t, GuestJmp> g_jmps;
extern bool g_longjmp_returns;  // hle_longjmp_resume comes back, which the runtime's never does

// The context bindings, as hle_OSSaveContext makes them and resume_binding resumes them.
struct Binding {
    jmp_buf jb;
    CPU snapshot;
};
extern std::map<uint32_t, Binding> g_bindings;

extern bool g_irq_armed;  // the next irq_poll delivers an interrupt, as irq_deliver delivers one
extern int g_at_exit;     // plat_at_exit's calls

uint32_t depth_of(const CPU* c);
RecompFn lookup(uint32_t a);

constexpr uint32_t kLR = 0x80001230u;  // where each run's caller is
extern CPU g_cpu;

uint8_t* host(uint32_t addr);
void poke(uint32_t addr, uint32_t w);
uint32_t peek(uint32_t addr);
void reset_ram();
void base_cpu(CPU* c);

// The test's own reading of the range table, apart from code_table_find's.
uint32_t range_of(uint32_t addr);
bool guarded_range(uint32_t n);
void force_none();
void force_all();
bool bit(uint32_t n);

// Calls fn(arg), and returns false with g_fatal holding the message if fatal stopped it.
bool guarded(void (*fn)(void*), void* arg);
void call_at(void* p);
// Runs the entry at a on g_cpu, and returns fatal's message if it stopped, or "".
std::string call(uint32_t a);
void fail(const std::string& what);

// ---- the cases ----

enum Mode { NATIVE, OWN, ALL };
extern const char* const kModes[];

struct Case {
    const char* name;
    uint32_t entry;     // a recompiled entry, or a blob for a loose frame
    uint32_t twin;      // what the native run calls in place of entry, when entry's code changed in RAM
    void (*setup)(CPU*);
    // A call the native run has to make, the start of the stop it has to end in, or IRQ_CHECK for the
    // clock to move.
    const char* needs;
    bool loose;
};

struct Outcome {
    CPU cpu;
    std::vector<uint8_t> ram;
    uint64_t vcount;
    std::vector<Call> calls;
    std::string stop;
    uint64_t ran;  // the words interpreted in the entry's frame and any of its function's
    // The bytes between the highest and lowest host stack frames that asked the runtime for a setjmp or
    // context site's jmp_buf, which is 0 when each site is taken in the same C frame each time.
    uint64_t site_spread;
};

Outcome run(const Case& k, Mode m);
// The first thing that differs between a native outcome and an interpreted one, or "".
std::string differ(const Outcome& n, const Outcome& o);
void check_case(const Case& k);

// Gives the guest its memory and puts the fixtures in it, or says why it can't for program.
bool runner_init(const char* program);
// Prints the result, and returns the exit code.
int runner_finish();
