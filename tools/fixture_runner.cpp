// The runtime's fakes and the case runner, for tools/fixture_runner.h.
#include "fixture_runner.h"
#include "platform.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

std::vector<Call> g_calls;

void note(const char* fn, uint32_t a, uint32_t b, uint32_t c) { g_calls.push_back({fn, a, b, c}); }

std::vector<std::string> g_logs;
int g_failed;
std::string g_fatal;
std::map<uint32_t, GuestJmp> g_jmps;
bool g_longjmp_returns;
std::map<uint32_t, Binding> g_bindings;
bool g_irq_armed;
int g_at_exit;

namespace {

// fatal's way back to the check that hit it.
jmp_buf g_fatal_jb;
bool g_fatal_armed;

std::unordered_map<uint32_t, RecompFn> g_fns;

CPU g_irq_snapshot;
jmp_buf g_irq_jb;

std::vector<uint8_t> g_image;  // the RAM window as each run starts

}  // namespace

RecompFn lookup(uint32_t a) {
    auto it = g_fns.find(a);
    return it == g_fns.end() ? nullptr : it->second;
}

uint32_t depth_of(const CPU* c) {
#ifdef GCN_CALL_TRACE
    return c->depth;
#else
    (void)c;
    return 0;
#endif
}

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

std::string call(uint32_t a) {
    return guarded(call_at, &a) ? std::string() : g_fatal;
}

void fail(const std::string& what) {
    printf("FAIL %s\n", what.c_str());
    g_failed++;
}

// ---- the cases ----

const char* const kModes[] = {"natively", "with its own bit", "with every bit"};

namespace {

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

}  // namespace

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

bool runner_init(const char* program) {
    // All of the guest's address space plus a megabyte past it, as mem_init reserves it.
    g_mem = (uint8_t*)calloc(1, 0x40000000u + 0x100000u);
    if (!g_mem) {
        fprintf(stderr, "%s: no memory for the guest's\n", program);
        return false;
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
    return true;
}

int runner_finish() {
    if (g_failed) printf("%d differences\n", g_failed);
    else printf("everything matches\n");
    return g_failed ? 1 : 0;
}
