// The interpreter's frames: the `test_interp_run` and `test_interp_run_trace` targets. They run with
// no device, disc or game.
//
// recomp/fixture_programs.py writes made-up programs, and recomp.py compiles them into
// <build>/interp_fixtures/ as it does a game. Each case runs one natively, then through the
// interpreter from the same state, once with only its own function forced and once with every guarded
// function forced. The CPU, the RAM the programs use, the guest clock and the calls into the runtime
// must come out the same, and an interpreted run must have interpreted something.
//
// A case whose code was changed in RAM runs a twin natively instead, a function holding the code as
// RAM now has it. test_interp_run_trace is built with GCN_TRACE_CALLS, and each of its runs must leave
// the guest call stack as deep as it found it.
//
// After the cases come the bit setter, GCN_INTERPRET_STATS's report, --log=interp's lines, the two
// stops (a longjmp frame that returns, and a patch with a goto) and GCN_INTERPRET's two forms. The
// runtime's services are fakes that record each call, and the ones that resume a guest do it as
// threads.cpp does.
//
// usage: test_interp_run WORKDIR
// Exits 0 only if everything matches.
#include "interp/interp.h"
#include "fixture_programs.h"
#include "platform.h"
#include "runtime.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace {

struct Call {
    std::string fn;
    uint32_t a, b, c;
    bool operator==(const Call&) const = default;
};
std::vector<Call> g_calls;

void note(const char* fn, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) { g_calls.push_back({fn, a, b, c}); }

std::vector<std::string> g_logs;  // --log=interp's lines
int g_failed;

// fatal's way back to the check that hit it.
jmp_buf g_fatal_jb;
bool g_fatal_armed;
std::string g_fatal;

std::unordered_map<uint32_t, RecompFn> g_fns;

RecompFn lookup(uint32_t a) {
    auto it = g_fns.find(a);
    return it == g_fns.end() ? nullptr : it->second;
}

// The setjmp sites' resume points, kept as threads.cpp keeps them.
struct GuestJmp {
    jmp_buf jb;
    uint32_t lr, sp, depth;
};
std::map<uint32_t, GuestJmp> g_jmps;
bool g_longjmp_returns;  // hle_longjmp_resume comes back, which the runtime's never does

// The context bindings, as hle_OSSaveContext makes them and resume_binding resumes them.
struct Binding {
    jmp_buf jb;
    CPU snapshot;
};
std::map<uint32_t, Binding> g_bindings;

// The interrupt the next irq_poll delivers, as irq_deliver delivers one.
bool g_irq_armed;
CPU g_irq_snapshot;
jmp_buf g_irq_jb;

int g_at_exit;  // plat_at_exit's calls

uint32_t depth_of(const CPU* c) {
#ifdef GCN_CALL_TRACE
    return c->depth;
#else
    (void)c;
    return 0;
#endif
}

}  // namespace

uint8_t* g_mem;
uint8_t g_gp_buf[64];
uint32_t g_gp_len;
volatile uint64_t g_vcount;
volatile uint64_t g_vlimit = UINT64_MAX;
bool g_log_enabled[LOG_COUNT];

void log_msg(LogCat cat, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (cat == LOG_INTERP) g_logs.push_back(b);
}

void fatal(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (!g_fatal_armed) {
        fprintf(stderr, "FATAL outside a check: %s\n", b);
        exit(2);
    }
    g_fatal = b;
    g_fatal_armed = false;
    GCN_LONGJMP(g_fatal_jb, 1);
}

const char* func_name(uint32_t addr) {
    for (uint32_t i = 0; i < g_recomp_func_count; i++)
        if (g_recomp_funcs[i].addr == addr) return g_recomp_funcs[i].name;
    return "?";
}

void plat_at_exit(void (*fn)()) {
    (void)fn;
    g_at_exit++;
}

extern "C" {
void orig_made_up_save(CPU* c);

void call_indirect(CPU* c, uint32_t addr) {
    if (RecompFn fn = lookup(addr)) {
        fn(c);
        return;
    }
    // A blob is code in RAM that no table knows, and it runs as a loose frame.
    if (addr >= FX_BLOB_BASE && addr < FX_BLOB_END) {
        interp_run_loose(c, addr);
        return;
    }
    fatal("call_indirect: no function at %08X (lr=%08X)", addr, c->lr);
}
void unimpl(CPU*, uint32_t pc, uint32_t inst) { note("unimpl", pc, inst); }

void irq_poll(CPU* c) {
    note("irq_poll", (uint32_t)g_vcount, c->r[3], c->msr);
    g_vlimit = UINT64_MAX;
    if (!g_irq_armed) return;
    g_irq_armed = false;
    g_irq_snapshot = *c;
    if (GCN_SETJMP(g_irq_jb) != 0) return;  // resumed from the snapshot
    call_indirect(c, FX_F_HANDLER);
    fatal("the interrupt handler returned");
}
void idle_hint(CPU* c) { note("idle_hint", c->r[3]); }

jmp_buf* hle_setjmp_jmpbuf(CPU* c) {
    note("hle_setjmp_jmpbuf", c->r[3], c->lr, c->r[1]);
    GuestJmp& g = g_jmps[c->r[3]];
    g.lr = c->lr;
    g.sp = c->r[1];
    g.depth = depth_of(c);
    return &g.jb;
}
void hle_longjmp_resume(CPU* c, uint32_t buf) {
    note("hle_longjmp_resume", buf, c->lr, c->r[1]);
    if (g_longjmp_returns) return;
    auto it = g_jmps.find(buf);
    if (it == g_jmps.end() || it->second.lr != c->lr || it->second.sp != c->r[1])
        fatal("longjmp through %08X to LR %08X, r1 %08X, which no setjmp call site recorded", buf, c->lr, c->r[1]);
#ifdef GCN_CALL_TRACE
    c->depth = it->second.depth;
#endif
    GCN_LONGJMP(it->second.jb, 1);
}
jmp_buf* hle_context_jmpbuf(CPU* c) {
    note("hle_context_jmpbuf", c->r[3]);
    return &g_bindings[c->r[3]].jb;
}
// made_up_save and made_up_load are HLE'd, and stand in for OSSaveContext and OSLoadContext.
void hle_made_up_save(CPU* c) {
    const uint32_t ctx = c->r[3];
    orig_made_up_save(c);
    note("hle_made_up_save", ctx, c->r[3]);
    g_bindings[ctx].snapshot = *c;
}
void hle_made_up_load(CPU* c) {
    note("hle_made_up_load", c->r[3]);
    auto it = g_bindings.find(c->r[3]);
    if (it == g_bindings.end()) fatal("no context was saved at %08X", c->r[3]);
    *c = it->second.snapshot;
    c->r[3] = 1;
    GCN_LONGJMP(it->second.jb, 1);
}
// The interrupt handler's way out, which resumes what the interrupt stopped.
void hle_made_up_resume(CPU* c) {
    note("hle_made_up_resume", c->r[3]);
    *c = g_irq_snapshot;
    GCN_LONGJMP(g_irq_jb, 1);
}

// What the patches and the step call.
void heap_trace_alloc(CPU* c, uint32_t v) { note("heap_trace_alloc", v, c->r[3]); }
void heap_trace_alloc_result(CPU* c, int ok) { note("heap_trace_alloc_result", (uint32_t)ok, c->r[3]); }
void heap_trace_free(CPU* c) { note("heap_trace_free", c->r[3]); }
double gcn_step_scale(uint32_t pc, double p) {
    note("gcn_step_scale", pc, (uint32_t)(int32_t)(p * 1000));
    return 1.5;
}
uint32_t gcn_step_int(uint32_t pc, double p) {
    note("gcn_step_int", pc, (uint32_t)(int32_t)(p * 1000));
    return 2;
}
double gcn_step_pow(uint32_t pc, double p, double k) {
    note("gcn_step_pow", pc, (uint32_t)(int32_t)(p * 1000));
    return k;
}
double gcn_step_damp(uint32_t pc, double p, double x, double k, double b) {
    note("gcn_step_damp", pc, (uint32_t)(int32_t)(p * 1000));
    return x * k + b;
}

void hle_sc(CPU*, uint32_t pc) { note("hle_sc", pc); }
void hle_trap(CPU*, uint32_t pc, uint32_t inst) { note("hle_trap", pc, inst); }
void hle_rfi(CPU*, uint32_t pc) { note("hle_rfi", pc); }
void hle_mtmsr(CPU* c, uint32_t v) {
    note("hle_mtmsr", v);
    c->msr = v;
}
uint32_t hle_mfspr(CPU*, uint32_t spr) {
    note("hle_mfspr", spr);
    return spr;
}
void hle_mtspr(CPU*, uint32_t spr, uint32_t v) { note("hle_mtspr", spr, v); }
void hle_set_fpscr(CPU* c, uint32_t v) {
    note("hle_set_fpscr", v);
    c->fpscr = v;
    c->fprf_pending = 0;
}
void hle_dcbz(CPU*, uint32_t ea) { note("hle_dcbz", ea); }
void hle_dcbz_l(CPU*, uint32_t ea) { note("hle_dcbz_l", ea); }
void hle_dcbi(CPU*, uint32_t ea) { note("hle_dcbi", ea); }
void hle_mtsr(CPU*, uint32_t sr, uint32_t v) { note("hle_mtsr", sr, v); }
uint32_t hle_mfsr(CPU*, uint32_t sr) {
    note("hle_mfsr", sr);
    return sr;
}
void hle_tlbie(CPU*, uint32_t ea) { note("hle_tlbie", ea); }
void hle_lswi(CPU*, uint32_t ea, int rd, int n) { note("hle_lswi", ea, (uint32_t)rd, (uint32_t)n); }
void hle_stswi(CPU*, uint32_t ea, int rs, int n) { note("hle_stswi", ea, (uint32_t)rs, (uint32_t)n); }
void psq_load_quantised(CPU*, uint32_t ea, int frd, int w, int gqr) {
    note("psq_load_quantised", ea, (uint32_t)frd, (uint32_t)(w << 8 | gqr));
}
void psq_store_quantised(CPU*, uint32_t ea, int frs, int w, int gqr) {
    note("psq_store_quantised", ea, (uint32_t)frs, (uint32_t)(w << 8 | gqr));
}
uint32_t mmio_read32(uint32_t a) {
    note("mmio_read32", a);
    return 0;
}
uint16_t mmio_read16(uint32_t a) {
    note("mmio_read16", a);
    return 0;
}
uint8_t mmio_read8(uint32_t a) {
    note("mmio_read8", a);
    return 0;
}
void mmio_write32(uint32_t a, uint32_t v) { note("mmio_write32", a, v); }
void mmio_write16(uint32_t a, uint16_t v) { note("mmio_write16", a, v); }
void mmio_write8(uint32_t a, uint8_t v) { note("mmio_write8", a, v); }
uint32_t vm_translate(uint32_t ea, int write) {
    note("vm_translate", ea, (uint32_t)write);
    return ea & 0x3FFFFFFFu;
}
void gp_flush_line(void) {
    note("gp_flush_line");
    g_gp_len = 0;
}
}

namespace {

constexpr uint32_t kLR = 0x80001230u;  // where each run's caller is
std::vector<uint8_t> g_image;          // the RAM window as each run starts
CPU g_cpu;

uint8_t* host(uint32_t addr) { return g_mem + (addr & 0x3FFFFFFFu); }

void poke(uint32_t addr, uint32_t w) {
    uint8_t* p = host(addr);
    p[0] = (uint8_t)(w >> 24);
    p[1] = (uint8_t)(w >> 16);
    p[2] = (uint8_t)(w >> 8);
    p[3] = (uint8_t)w;
}

uint32_t peek(uint32_t addr) {
    const uint8_t* p = host(addr);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void reset_ram() {
    memcpy(host(FX_WINDOW_BASE), g_image.data(), g_image.size());
}

void base_cpu(CPU* c) {
    memset(c, 0, sizeof *c);
    for (int k = 0; k < 32; k++) {
        c->r[k] = 0x9E3779B9u * (uint32_t)(k + 1);
        c->f[k].d = k * 0.75 - 3;
        c->ps1[k] = 1.0 / (k + 1);
    }
    for (int k = 0; k < 8; k++) {
        c->cr[k] = (uint8_t)((k * 5) & 0xF);
        c->gqr[k] = 0x00070007u * (uint32_t)k;
    }
    c->r[1] = FX_STACK;
    c->r[3] = 7;
    c->lr = kLR;
    c->ctr = 0x1234;
    c->msr = 0x2000;  // FP on, interrupts off
    c->xer_ca = 1;
}

// The test's own reading of the range table, apart from code_table_find's.
uint32_t range_of(uint32_t addr) {
    for (uint32_t n = 0; n < g_recomp_range_count; n++)
        if (g_recomp_ranges[n].start <= addr && addr < g_recomp_ranges[n].end) return n;
    fprintf(stderr, "no range holds %08X\n", addr);
    exit(2);
}

bool guarded_range(uint32_t n) { return !(g_recomp_ranges[n].flags & RECOMP_RANGE_NO_GUARD); }

void force_none() {
    for (uint32_t n = 0; n < g_recomp_range_count; n++) code_table_mark(n, CODE_FORCED, false);
}

void force_all() {
    for (uint32_t n = 0; n < g_recomp_range_count; n++)
        if (guarded_range(n)) code_table_mark(n, CODE_FORCED, true);
}

bool bit(uint32_t n) { return GCN_CODE_CHANGED(n) != 0; }

// Calls fn(arg), and returns false with g_fatal holding the message if fatal stopped it.
bool guarded(void (*fn)(void*), void* arg) {
    if (GCN_SETJMP(g_fatal_jb)) return false;
    g_fatal_armed = true;
    fn(arg);
    g_fatal_armed = false;
    return true;
}

void call_at(void* p) {
    const uint32_t a = *(const uint32_t*)p;
    RecompFn fn = lookup(a);
    if (!fn) fatal("no recompiled entry at %08X", a);
    fn(&g_cpu);
}

// Runs the entry at a on g_cpu, and returns fatal's message if it stopped, or "".
std::string call(uint32_t a) {
    return guarded(call_at, &a) ? std::string() : g_fatal;
}

void fail(const std::string& what) {
    printf("FAIL %s\n", what.c_str());
    g_failed++;
}

// ---- the cases ----

enum Mode { NATIVE, OWN, ALL };
const char* const kModes[] = {"natively", "with its own bit", "with every bit"};

struct Case {
    const char* name;
    uint32_t entry;     // a recompiled entry, or a blob for a loose frame
    uint32_t twin;      // what the native run calls in place of entry, when entry's code changed in RAM
    void (*setup)(CPU*);
    const char* needs;  // a call the native run has to make, or IRQ_CHECK for the clock to move
    bool loose;
};

const Case kCases[] = {
    {"an in-range loop", FX_F_LOOP, 0, nullptr, "IRQ_CHECK", false},
    {"an idle back edge", FX_F_IDLE, 0, nullptr, "idle_hint", false},
    {"mtmsr and the IRQ_CHECK after it", FX_F_MTMSR, 0, [](CPU*) { g_vlimit = 1; }, "irq_poll", false},
    {"a tail call", FX_F_TAIL, 0, nullptr, nullptr, false},
    {"a bl and its return", FX_F_CALL, 0, nullptr, nullptr, false},
    {"blrl and bctrl", FX_F_REGCALLS, 0, nullptr, nullptr, false},
    {"a jump-table bctr to its first case", FX_F_SWITCH, 0, [](CPU* c) { c->r[3] = 0; }, nullptr, false},
    {"a jump-table bctr to its last case", FX_F_SWITCH, 0, [](CPU* c) { c->r[3] = 2; }, nullptr, false},
    {"a conditional bl not taken, then one taken", FX_F_CONDBL, 0, nullptr, nullptr, false},
    {"a mid-entry", FX_F_MULTI_MID, 0, nullptr, nullptr, false},
    {"a bl to a mid-entry", FX_F_MIDCALLER, 0, nullptr, nullptr, false},
    {"running off the end into the next function", FX_F_FALLS, 0, nullptr, nullptr, false},
    {"a patch site", FX_F_PATCH, 0, nullptr, "heap_trace_alloc", false},
    {"a patch site that returns", FX_F_PATCH_RET, 0, nullptr, "heap_trace_alloc_result", false},
    {"a site whose word changed in RAM", FX_F_SITE2, FX_F_SITE2_TWIN,
     [](CPU*) { poke(FX_F_SITE2_SITE, FX_SITE2_WORD); }, nullptr, false},
    {"a step site", FX_F_STEP, 0, nullptr, "gcn_step_int", false},
    {"a setjmp site resumed by a longjmp", FX_F_SJ, 0, nullptr, "hle_longjmp_resume", false},
    {"a context site resumed after its frame went on", FX_F_CTX, 0, nullptr, "hle_made_up_load", false},
    {"an interrupt taken mid-loop and resumed from its snapshot", FX_F_IRQ_LOOP, 0,
     [](CPU*) {
         g_vlimit = 3;
         g_irq_armed = true;
     },
     "hle_made_up_resume", false},
    {"a loose frame in a RAM blob with its own loop", FX_BLOB_LOOP, FX_F_BLOB_TWIN, nullptr, "IRQ_CHECK", true},
    {"a trampoline into a function's middle and its return", FX_F_HOOKED, FX_F_HOOKED_TWIN,
     [](CPU*) { poke(FX_F_HOOKED, FX_HOOK_WORD); }, "IRQ_CHECK", false},
    {"a blr to an LR its frame wasn't entered with", FX_F_LR, 0, nullptr, nullptr, false},
};

struct Outcome {
    CPU cpu;
    std::vector<uint8_t> ram;
    uint64_t vcount;
    std::vector<Call> calls;
    std::string stop;
    uint64_t ran;  // the words interpreted in the entry's frame and any of its function's
};

struct Entry {
    const Case* k;
    Mode m;
};

void enter(void* p) {
    const Entry& e = *(const Entry*)p;
    if (e.k->loose && e.m != NATIVE) {
        interp_run_loose(&g_cpu, e.k->entry);
        return;
    }
    uint32_t a = e.m == NATIVE && e.k->twin ? e.k->twin : e.k->entry;
    call_at(&a);
}

Outcome run(const Case& k, Mode m) {
    reset_ram();
    base_cpu(&g_cpu);
    g_vcount = 0;
    g_vlimit = UINT64_MAX;
    g_calls.clear();
    g_jmps.clear();
    g_bindings.clear();
    g_irq_armed = g_longjmp_returns = false;
    force_none();
    if (k.setup) k.setup(&g_cpu);
    const uint32_t n = k.loose ? g_recomp_range_count : range_of(k.entry);
    if (m == OWN && !k.loose) code_table_mark(n, CODE_FORCED, true);
    if (m == ALL) force_all();
    const uint64_t before = *code_table_counter(n);
    Entry e{&k, m};
    Outcome o;
    if (!guarded(enter, &e)) o.stop = g_fatal;
    o.cpu = g_cpu;
    o.ram.assign(host(FX_WINDOW_BASE), host(FX_WINDOW_BASE) + FX_WINDOW_SIZE);
    o.vcount = g_vcount;
    o.calls = g_calls;
    o.ran = *code_table_counter(n) - before;
    force_none();
    return o;
}

std::string hex(const char* what, uint64_t a, uint64_t b) {
    char s[160];
    snprintf(s, sizeof s, "%s %llX natively, %llX interpreted", what, (unsigned long long)a, (unsigned long long)b);
    return s;
}

std::string call_text(const std::vector<Call>& calls, size_t k) {
    if (k >= calls.size()) return "nothing";
    char s[160];
    snprintf(s, sizeof s, "%s(%X, %X, %X)", calls[k].fn.c_str(), calls[k].a, calls[k].b, calls[k].c);
    return s;
}

template <typename T>
uint64_t bits_of(const T& v) {
    uint64_t u = 0;
    memcpy(&u, &v, sizeof v);
    return u;
}

// The first thing that differs between a native outcome and an interpreted one, or "".
std::string differ(const Outcome& n, const Outcome& o) {
    if (n.stop != o.stop) return "stopped with \"" + n.stop + "\" natively and \"" + o.stop + "\" interpreted";
    const CPU &x = n.cpu, &y = o.cpu;
    char what[32];
    for (int k = 0; k < 32; k++) {
        snprintf(what, sizeof what, "r%d", k);
        if (x.r[k] != y.r[k]) return hex(what, x.r[k], y.r[k]);
        snprintf(what, sizeof what, "f%d", k);
        if (x.f[k].u != y.f[k].u) return hex(what, x.f[k].u, y.f[k].u);
        snprintf(what, sizeof what, "ps1 of f%d", k);
        if (bits_of(x.ps1[k]) != bits_of(y.ps1[k])) return hex(what, bits_of(x.ps1[k]), bits_of(y.ps1[k]));
    }
    for (int k = 0; k < 8; k++) {
        snprintf(what, sizeof what, "cr%d", k);
        if (x.cr[k] != y.cr[k]) return hex(what, x.cr[k], y.cr[k]);
        snprintf(what, sizeof what, "gqr%d", k);
        if (x.gqr[k] != y.gqr[k]) return hex(what, x.gqr[k], y.gqr[k]);
    }
    const struct {
        const char* what;
        uint64_t a, b;
    } fields[] = {
        {"XER[CA]", x.xer_ca, y.xer_ca}, {"XER[SO]", x.xer_so, y.xer_so}, {"XER[OV]", x.xer_ov, y.xer_ov},
        {"XER's count", x.xer_bc, y.xer_bc}, {"LR", x.lr, y.lr}, {"CTR", x.ctr, y.ctr}, {"MSR", x.msr, y.msr},
        {"FPSCR", x.fpscr, y.fpscr}, {"the FPRF value", bits_of(x.fprf_v), bits_of(y.fprf_v)},
        {"the FPRF kind", x.fprf_pending, y.fprf_pending}, {"host", bits_of(x.host), bits_of(y.host)},
        {"the call stack's depth", depth_of(&x), depth_of(&y)}, {"the guest clock", n.vcount, o.vcount},
    };
    for (const auto& f : fields)
        if (f.a != f.b) return hex(f.what, f.a, f.b);
    for (int k = 0; k < 1024; k++) {
        snprintf(what, sizeof what, "SPR %d", k);
        if (x.spr[k] != y.spr[k]) return hex(what, x.spr[k], y.spr[k]);
    }
    for (size_t k = 0; k < n.ram.size(); k++)
        if (n.ram[k] != o.ram[k]) return hex("the RAM byte at", FX_WINDOW_BASE + k, FX_WINDOW_BASE + k);
    for (size_t k = 0; k < std::max(n.calls.size(), o.calls.size()); k++)
        if (k >= n.calls.size() || k >= o.calls.size() || !(n.calls[k] == o.calls[k]))
            return "call " + std::to_string(k) + " is " + call_text(n.calls, k) + " natively and " +
                   call_text(o.calls, k) + " interpreted";
    return "";
}

void check_case(const Case& k) {
    const Outcome nat = run(k, NATIVE);
    int before = g_failed;
    if (!nat.stop.empty()) fail(std::string(k.name) + ": the native run stopped with \"" + nat.stop + "\"");
    if (depth_of(&nat.cpu)) fail(std::string(k.name) + ": the native run left the call stack unbalanced");
    if (k.needs) {
        bool seen = !strcmp(k.needs, "IRQ_CHECK") && nat.vcount > 0;
        for (const Call& c : nat.calls) seen = seen || c.fn == k.needs;
        if (!seen) fail(std::string(k.name) + ": the native run never made the " + k.needs + " it's for");
    }
    for (Mode m : {OWN, ALL}) {
        const Outcome o = run(k, m);
        std::string d = differ(nat, o);
        if (d.empty() && !o.ran) d = "nothing was interpreted";
        if (!d.empty()) fail(std::string(k.name) + ", " + kModes[m] + ": " + d);
    }
    if (g_failed == before) printf("match  %s\n", k.name);
}

// ---- the code table ----

void check_setter() {
    const uint32_t n = range_of(FX_F_LOOP);
    code_table_mark(n, CODE_FORCED, true);
    const bool forced = bit(n);
    code_table_mark(n, CODE_CHANGED, true);
    code_table_mark(n, CODE_FORCED, false);
    const bool changed = bit(n);
    code_table_mark(n, CODE_CHANGED, false);
    const bool neither = bit(n);
    if (!forced || !changed || neither || bit(n + 1))
        fail("the setter: forced " + std::to_string(forced) + ", changed " + std::to_string(changed) + ", neither " +
             std::to_string(neither) + ", the next function " + std::to_string(bit(n + 1)));
    else printf("match  the setter, with a bit set while either mark is\n");
}

// Runs filler k forced k + 1 times, each two words, so the 30 that ran the most are 39 down to 10.
// It runs first, while every count is still 0.
void check_stats() {
    std::vector<uint32_t> starts(FX_FILLER_COUNT);
    for (uint32_t k = 0; k < FX_FILLER_COUNT; k++) {
        char name[16];
        snprintf(name, sizeof name, "filler_%02u", k);
        for (uint32_t i = 0; i < g_recomp_func_count; i++)
            if (!strcmp(g_recomp_funcs[i].name, name)) starts[k] = g_recomp_funcs[i].addr;
    }
    uint64_t total = 0;
    std::string stop;
    reset_ram();
    for (uint32_t k = 0; k < FX_FILLER_COUNT; k++) {
        const uint32_t n = range_of(starts[k]);
        code_table_mark(n, CODE_FORCED, true);
        for (uint32_t j = 0; j <= k; j++) {
            base_cpu(&g_cpu);
            const std::string s = call(starts[k]);
            if (stop.empty() && !s.empty()) stop = "filler_" + std::to_string(k) + " stopped with \"" + s + "\"";
            total += 2;
        }
        code_table_mark(n, CODE_FORCED, false);
    }
    if (!stop.empty()) fail("GCN_INTERPRET_STATS: " + stop);
    char line[160];
    snprintf(line, sizeof line, "[interp] GCN_INTERPRET_STATS: %llu instructions interpreted, 0 of them in loose frames\n",
             (unsigned long long)total);
    std::string want = line;
    for (uint32_t k = FX_FILLER_COUNT; k-- > FX_FILLER_COUNT - 30;) {
        snprintf(line, sizeof line, "[interp] %12llu  %08X filler_%02u\n", (unsigned long long)(2 * (k + 1)), starts[k], k);
        want += line;
    }
    const std::string got = code_table_stats();
    if (got != want) fail("GCN_INTERPRET_STATS printed\n" + got + "where it should print\n" + want);
    else printf("match  GCN_INTERPRET_STATS's 30 functions and its total\n");
}

// The cases' runs left one line for the blr that returned to an LR its frame wasn't entered with, and
// one for the site whose word changed, though each ran twice interpreted.
void check_logs() {
    char blr[160], site[160];
    snprintf(blr, sizeof blr, "blr at %08X returns to %08X, and its frame was entered with LR %08X", FX_F_LR_RET,
             kLR + 8, kLR);
    snprintf(site, sizeof site, "site at %08X holds %08X, not the %08X its patch replaced, so that word runs",
             FX_F_SITE2_SITE, FX_SITE2_WORD, peek(FX_F_SITE2_SITE));
    const std::vector<std::string> want = {site, blr};
    if (g_logs != want) {
        std::string got;
        for (const std::string& s : g_logs) got += "  " + s + "\n";
        fail("--log=interp printed\n" + got + "where it should print\n  " + site + "\n  " + blr);
    } else {
        printf("match  --log=interp's two lines, once each\n");
    }
}

void stop_case(const char* what, uint32_t entry, void (*setup)(CPU*), const std::string& want) {
    reset_ram();
    base_cpu(&g_cpu);
    g_calls.clear();
    g_jmps.clear();
    g_longjmp_returns = false;
    if (setup) setup(&g_cpu);
    const uint32_t n = range_of(entry);
    code_table_mark(n, CODE_FORCED, true);
    const std::string stop = call(entry);
    code_table_mark(n, CODE_FORCED, false);
    g_longjmp_returns = false;
    if (stop.find(want) == std::string::npos) fail(std::string(what) + ": stopped with \"" + stop + "\", not \"" + want + "\"");
    else printf("match  %s\n", what);
}

void check_stops() {
    char want[160];
    snprintf(want, sizeof want, "the interpreted longjmp made_up_longjmp at %08X returned", FX_MADE_UP_LONGJMP);
    stop_case("a longjmp frame that would return stops", FX_MADE_UP_LONGJMP,
              [](CPU* c) {
                  g_longjmp_returns = true;
                  c->r[3] = FX_JMP_BUF;
              },
              want);
    snprintf(want, sizeof want, "the patch at %08X has a goto", FX_F_GOTO_SITE);
    stop_case("a patch with a goto stops", FX_F_GOTO, nullptr, want);
}

// ---- GCN_INTERPRET ----

std::string g_forced;

void force_spec(void* spec) { g_forced = code_table_force((const char*)spec); }

// The functions whose bits are set, by start.
std::vector<uint32_t> set_bits() {
    std::vector<uint32_t> out;
    for (uint32_t n = 0; n < g_recomp_range_count; n++)
        if (bit(n)) out.push_back(g_recomp_ranges[n].start);
    return out;
}

void write_file(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "can't write %s\n", path.c_str());
        exit(2);
    }
    fputs(text.c_str(), f);
    fclose(f);
}

void check_interpret(const std::string& dir) {
    // The list has each kind of line, which are bare upper-case hex, 0x with lower case, 0X with a
    // comment after, a blank line, spaces and tabs around, an HLE'd function's start, a module's start
    // and a start listed twice. The HLE'd wrapper stays native while its original body is forced, and
    // the module's code has no guard.
    char text[512];
    snprintf(text, sizeof text, "# starts to force\n%08X\n0x%08x\n0X%08X  # with a comment\n\n \t%08X\t \n%08X\n%08X\n%08X\n",
             FX_F_LOOP, FX_F_IDLE, FX_F_CALL, FX_F_TAIL, FX_MADE_UP_SAVE, FX_MADE_UP_MODULE_FN, FX_F_LOOP);
    const std::string list = dir + "/starts.txt";
    write_file(list, text);
    force_none();
    g_forced.clear();
    char want[256];
    snprintf(want, sizeof want, "[interp] GCN_INTERPRET=%s: forced 5 of the 6 starts listed (1 with no guard)", list.c_str());
    const std::vector<uint32_t> want_bits = {FX_F_LOOP, FX_F_IDLE, FX_F_CALL, FX_F_TAIL, FX_MADE_UP_SAVE};
    if (!guarded(force_spec, (void*)list.c_str())) fail("GCN_INTERPRET=FILE stopped with \"" + g_fatal + "\"");
    else if (g_forced != want) fail("GCN_INTERPRET=FILE printed \"" + g_forced + "\", not \"" + want + "\"");
    else if (set_bits() != want_bits) fail("GCN_INTERPRET=FILE set other bits than the five guarded starts'");
    else printf("match  GCN_INTERPRET=FILE with each kind of line\n");

    // The HLE'd function's wrapper runs its replacement natively, which calls the forced original.
    reset_ram();
    base_cpu(&g_cpu);
    g_calls.clear();
    g_bindings.clear();
    g_cpu.r[3] = FX_CONTEXT;
    const uint64_t before = *code_table_counter(range_of(FX_MADE_UP_SAVE));
    const std::string stop = call(FX_MADE_UP_SAVE);
    bool replaced = false;
    for (const Call& c : g_calls) replaced = replaced || c.fn == "hle_made_up_save";
    if (!stop.empty() || !replaced || *code_table_counter(range_of(FX_MADE_UP_SAVE)) == before)
        fail("a forced HLE'd function: stopped \"" + stop + "\", replacement ran " + std::to_string(replaced) +
             ", original interpreted " + std::to_string(*code_table_counter(range_of(FX_MADE_UP_SAVE)) != before));
    else printf("match  a forced HLE'd function runs its replacement, which interprets the original\n");
    force_none();

    // Each line that names no recompiled function's start stops it, naming the line.
    char mid[16], extra[32];
    snprintf(mid, sizeof mid, "%08X", FX_F_LOOP + 4);
    snprintf(extra, sizeof extra, "%08X 1", FX_F_LOOP);
    for (const char* bad : {(const char*)mid, "f_loop", (const char*)extra, "0x", "-8010000"}) {
        snprintf(text, sizeof text, "%08X\n%s\n", FX_F_IDLE, bad);
        const std::string path = dir + "/bad.txt";
        write_file(path, text);
        snprintf(want, sizeof want, "GCN_INTERPRET: line 2 of %s is \"%s\", which isn't a recompiled function's start",
                 path.c_str(), bad);
        if (guarded(force_spec, (void*)path.c_str())) fail(std::string("GCN_INTERPRET took the line \"") + bad + "\"");
        else if (g_fatal != want) fail("GCN_INTERPRET stopped with \"" + g_fatal + "\", not \"" + want + "\"");
        else printf("match  GCN_INTERPRET stops at the line \"%s\"\n", bad);
        force_none();
    }
    const std::string missing = dir + "/missing.txt";
    snprintf(want, sizeof want, "GCN_INTERPRET: can't read %s", missing.c_str());
    if (guarded(force_spec, (void*)missing.c_str()) || g_fatal.find(want) != 0)
        fail("GCN_INTERPRET with no such file: \"" + g_fatal + "\"");
    else printf("match  GCN_INTERPRET stops when its file isn't there\n");

    uint32_t guarded_count = 0;
    std::vector<uint32_t> all_bits;
    for (uint32_t n = 0; n < g_recomp_range_count; n++)
        if (guarded_range(n)) {
            guarded_count++;
            all_bits.push_back(g_recomp_ranges[n].start);
        }
    snprintf(want, sizeof want, "[interp] GCN_INTERPRET=all: forced %u functions", guarded_count);
    if (!guarded(force_spec, (void*)"all") || g_forced != want || set_bits() != all_bits)
        fail("GCN_INTERPRET=all printed \"" + g_forced + "\", not \"" + want + "\", or set other bits");
    else printf("match  GCN_INTERPRET=all forces every guarded function\n");
    force_none();

    // code_table_boot reads both variables, and registers the report once however often it runs.
    setenv("GCN_INTERPRET", "all", 1);
    setenv("GCN_INTERPRET_STATS", "1", 1);
    g_at_exit = 0;
    code_table_boot();
    code_table_boot();
    if (set_bits() != all_bits || g_at_exit != 1)
        fail("code_table_boot: " + std::to_string(set_bits().size()) + " bits set, the report registered " +
             std::to_string(g_at_exit) + " times");
    else printf("match  code_table_boot reads GCN_INTERPRET and GCN_INTERPRET_STATS\n");
    unsetenv("GCN_INTERPRET");
    unsetenv("GCN_INTERPRET_STATS");
    force_none();
}

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_interp_run <work dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    if (exists(dir) || mkdir(dir.c_str(), 0755) != 0) {
        fprintf(stderr, "%s exists or can't be made: give a new directory\n", dir.c_str());
        return 2;
    }
    // All of the guest's address space plus a megabyte past it, as mem_init reserves it.
    g_mem = (uint8_t*)calloc(1, 0x40000000u + 0x100000u);
    if (!g_mem) {
        fprintf(stderr, "test_interp_run: no memory for the guest's\n");
        return 2;
    }
    for (uint32_t i = 0; i < g_recomp_func_count; i++) g_fns[g_recomp_funcs[i].addr] = g_recomp_funcs[i].fn;
    g_image.assign(FX_WINDOW_SIZE, 0);
    for (const FxWords& s : fx_image)
        for (uint32_t k = 0; k < s.count; k++) {
            const uint32_t at = s.addr + 4 * k - FX_WINDOW_BASE, w = s.words[k];
            g_image[at] = (uint8_t)(w >> 24);
            g_image[at + 1] = (uint8_t)(w >> 16);
            g_image[at + 2] = (uint8_t)(w >> 8);
            g_image[at + 3] = (uint8_t)w;
        }
    g_log_enabled[LOG_INTERP] = true;
#ifdef GCN_CALL_TRACE
    printf("with GCN_TRACE_CALLS: each run's call stack has to balance\n");
#endif

    check_stats();
    check_setter();
    for (const Case& k : kCases) check_case(k);
    check_logs();
    check_stops();
    check_interpret(dir);

    if (g_failed) printf("%d differences\n", g_failed);
    else printf("everything matches\n");
    return g_failed ? 1 : 0;
}
