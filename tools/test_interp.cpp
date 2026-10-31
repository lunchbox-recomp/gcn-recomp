// The interpreter's differential checks: the `test_interp` target. Runs with no device, disc or
// game.
//
// recomp/ppc_cases.py writes ppc.translate's own C for random and edge-case words of every form it
// accepts, and what ppc.decode_branch says each branch does, into <build>/interp_cases/. Each case
// runs here from seeded random CPU states, and a step also from a few fixed ones, once as that C and
// once through interp_step or interp_branch. Every CPU field, a 64 KiB window of RAM and the calls
// into the runtime must come out the same, bit for bit, and a band of RAM on each side of the window
// must come out as it went in.
//
// A word translate refuses must come back INTERP_UNIMPL with nothing changed, and one decode_branch
// refuses must come back false. The runtime's services are fakes that record each call, and the
// clock never moves. A memory form's base and index registers point into the window.
//
// usage: test_interp [--states M]
// Exits 0 only if nothing differs.
#include "interp/interp.h"
#include "interp_cases.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static_assert((int)INTERP_B == CASE_B && (int)INTERP_BC == CASE_BC && (int)INTERP_BLR == CASE_BLR &&
                  (int)INTERP_BCTR == CASE_BCTR,
              "the cases' branch kinds are the interpreter's");

uint8_t* g_mem;
uint8_t g_gp_buf[64];
uint32_t g_gp_len;
volatile uint64_t g_vcount;
volatile uint64_t g_vlimit = UINT64_MAX;

namespace {

struct Call {
    const char* fn;
    uint32_t a, b, c;
};
std::vector<Call> g_calls;

void note(const char* fn, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) { g_calls.push_back({fn, a, b, c}); }

}  // namespace

// The runtime's services as translate's text calls them. Each records its call, and one whose result
// the guest sees answers with something that depends on what it was passed.
extern "C" {
void hle_sc(CPU*, uint32_t pc) { note("hle_sc", pc); }
void hle_trap(CPU*, uint32_t pc, uint32_t inst) { note("hle_trap", pc, inst); }
void hle_rfi(CPU*, uint32_t pc) { note("hle_rfi", pc); }
void hle_mtmsr(CPU* c, uint32_t v) {
    note("hle_mtmsr", v);
    c->msr = v;
}
uint32_t hle_mfspr(CPU* c, uint32_t spr) {
    note("hle_mfspr", spr);
    return c->spr[spr & 1023] ^ spr * 0x9E3779B9u;
}
void hle_mtspr(CPU* c, uint32_t spr, uint32_t v) {
    note("hle_mtspr", spr, v);
    c->spr[spr & 1023] = v;
}
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
    return sr * 0x01010101u ^ 0xA5A5A5A5u;
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
    return a ^ 0x5A5A5A5Au;
}
uint16_t mmio_read16(uint32_t a) {
    note("mmio_read16", a);
    return (uint16_t)(a ^ 0x5A5A);
}
uint8_t mmio_read8(uint32_t a) {
    note("mmio_read8", a);
    return (uint8_t)(a ^ 0x5A);
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

constexpr uint32_t kWindowBase = 0x80000000u;  // and so physical 0, where an rA 0 load lands
constexpr uint32_t kWindow = 0x10000;

// The guard bands, by physical address. The one below the window is at the top of the guest's
// gigabyte, where an address just under kWindowBase lands.
constexpr uint32_t kGuard = 0x1000;
constexpr uint32_t kGuardAt[] = {0x40000000u - kGuard, kWindow};
uint8_t g_guard[2][kGuard];  // the pattern they're filled with

struct Rng {
    uint64_t s;
    uint64_t next() {  // splitmix64
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t u32() { return (uint32_t)(next() >> 32); }
    uint32_t below(uint32_t n) { return (uint32_t)(((next() >> 32) * n) >> 32); }
};

Rng rng_for(uint32_t insn, uint32_t pc, int state) {
    Rng r{(uint64_t)insn << 32 ^ pc ^ (uint64_t)(state + 1) * 0xD1B54A32D192ED03ull};
    r.next();
    return r;
}

const uint32_t kEdges[] = {0, 1, 2, 31, 32, 33, 63, 64, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF,
                           0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu};

uint32_t value(Rng& r) {
    switch (r.below(8)) {
    case 0:
    case 1:
    case 2: return kEdges[r.below(sizeof kEdges / sizeof *kEdges)];
    case 3: return r.below(64);
    default: return r.u32();
    }
}

// FP values as bits, starting with NaNs quiet and signaling of both signs, whose payloads rounding to
// single keeps or loses. Then both infinities and zeros, denormals of both formats, single's least
// normal and a negative just under it, single's largest, 2^128 and double's largest. Then ones, ties,
// fctiw's ends and 1 + 2^-27 and 1 - 2^-27, whose product rounds to 1 unless it's fused.
const uint64_t kFpEdges[] = {
    0x7FFC000020000000ull, 0xFFF8000000000001ull, 0x7FF4000020000000ull, 0xFFF0000000000001ull,
    0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x0000000000000000ull, 0x8000000000000000ull,
    0x0000000000000001ull, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x3810000000000000ull,
    0xB80FFFFFFFFFFFFFull, 0x47EFFFFFE0000000ull, 0x47F0000000000000ull, 0x7FEFFFFFFFFFFFFFull,
    0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x3FE0000000000000ull, 0x4004000000000000ull,
    0xBFF8000000000000ull, 0x41DFFFFFFFE00000ull, 0xC1E0000000100000ull, 0x41E0000000000000ull,
    0x3FF0000002000000ull, 0x3FEFFFFFFC000000ull,
};
constexpr uint64_t kQnan = 0x7FFC000020000000ull, kQnanNeg = 0xFFF8000000000001ull;
constexpr uint64_t kSnan = 0x7FF4000020000000ull, kSnanNeg = 0xFFF0000000000001ull;
constexpr uint64_t kInf = 0x7FF0000000000000ull, kInfNeg = 0xFFF0000000000000ull;
constexpr uint64_t kZero = 0, kZeroNeg = 0x8000000000000000ull;
constexpr uint64_t kOne = 0x3FF0000000000000ull, kOneNeg = 0xBFF0000000000000ull;
constexpr uint64_t kOneUp = 0x3FF0000002000000ull, kOneDown = 0x3FEFFFFFFC000000ull;

// FPSCRs with each rounding mode, every enable bit, the sticky exception bits or all of it.
const uint32_t kFpscrEdges[] = {0, 1, 2, 3, 0xF8, 0xFB, 0x9FF80700u, 0x60000004u, 0xFFFFFFFFu};

uint64_t fp_value(Rng& r) {
    switch (r.below(8)) {
    case 0:
    case 1:
    case 2: return kFpEdges[r.below(sizeof kFpEdges / sizeof *kFpEdges)];
    case 3:
    case 4: {  // within 2^20 of 1, and in case 4 a single exactly
        const uint64_t u = (uint64_t)r.below(2) << 63 | (uint64_t)(1003 + r.below(41)) << 52 |
                           (r.next() & 0xFFFFFFFFFFFFFull);
        return r.below(2) ? u : u & ~0x1FFFFFFFull;
    }
    default: return r.next();
    }
}

void random_cpu(CPU* c, Rng& r) {
    memset(c, 0, sizeof *c);
    for (uint32_t& v : c->r) v = value(r);
    for (FPR& f : c->f) f.u = fp_value(r);
    for (double& d : c->ps1) {
        const uint64_t u = fp_value(r);
        memcpy(&d, &u, 8);
    }
    for (uint8_t& f : c->cr) f = (uint8_t)r.below(16);
    c->xer_ca = (uint8_t)r.below(2);
    c->xer_so = (uint8_t)r.below(2);
    c->xer_ov = (uint8_t)r.below(2);
    c->xer_bc = (uint8_t)r.below(128);
    c->lr = value(r);
    c->ctr = r.below(4) ? value(r) : r.below(3);
    c->msr = r.u32();
    c->fpscr = r.below(4) ? r.u32() : kFpscrEdges[r.below(sizeof kFpscrEdges / sizeof *kFpscrEdges)];
    const uint64_t u = fp_value(r);
    memcpy(&c->fprf_v, &u, 8);
    c->fprf_pending = (uint8_t)r.below(3);
    for (uint32_t& g : c->gqr) g = r.u32();
    for (uint32_t& s : c->spr) s = r.u32();
}

// Pairs of register values a result turns on, which random picks seldom bring together. A fixed state
// gives x to rS and rA, then y to rB and its own CA, over a random state. A memory form's base and
// index are pointed into the window after that as usual.
struct Fixed {
    uint32_t x, y;
    uint8_t ca;
};
const Fixed kFixed[] = {
    {0x80000000u, 0xFFFFFFFFu, 0},  // INT_MIN / -1, and the negation of INT_MIN
    {0x9ABCDEF0u, 0, 1},            // a negative dividend over 0
    {0x12345678u, 0, 0},            // a positive one
    {0xFFFFFFFFu, 0, 1},            // the carry out of addze, adde and addme
    {0x7FFFFFFFu, 1, 0},            // an overflow past INT_MAX
    {0, 0, 0},                      // cntlzw of 0, and shifts and rotates of 0
    {0xFFFFFFFFu, 32, 1},           // shifts by 32, and cntlzw of all ones
    {0x80000001u, 31, 0},           // shifts by 31
};
constexpr int kFixedCount = sizeof kFixed / sizeof *kFixed;

void fix(CPU* c, uint32_t insn, const Fixed& f) {
    c->r[insn >> 21 & 31] = f.x;
    c->r[insn >> 16 & 31] = f.x;
    c->r[insn >> 11 & 31] = f.y;
    c->xer_ca = f.ca;
}

// A GQR whose loads and stores both use one type and scale.
constexpr uint32_t gqr(uint32_t type, uint32_t scale) { return scale << 24 | type << 16 | scale << 8 | type; }

// FP operands a result turns on together. A fixed FP state gives a to fD and fA, then b to fB and c
// to fC, both lanes of each, over a random state. Its FPSCR and every GQR are its own too, so between
// them the states cover each rounding mode, the enables, the sticky bits and every quantized type.
struct FixedFp {
    uint64_t a, b, c;
    uint32_t fpscr, gqr;
};
const FixedFp kFixedFp[] = {
    {kOneUp, kOneNeg, kOneDown, 0, gqr(0, 0)},                    // a*c + b fused or not
    {kOneUp, kOne, kOneDown, 1, gqr(4, 1)},                       // a*c - b fused or not
    {kQnan, kSnan, kQnanNeg, 2, gqr(5, 31)},                      // which NaN comes out
    {kSnan, kQnan, kSnanNeg, 3, gqr(6, 32)},
    {kInf, kInf, kZero, 0xF8, gqr(7, 63)},                        // inf - inf and inf * 0
    {kInfNeg, kInf, kInfNeg, 0xFFFFFFFFu, gqr(1, 0)},             // inf + -inf
    {kZero, kZeroNeg, kZeroNeg, 0x9FF80700u, gqr(2, 1)},          // the signs of zero results
    {kZeroNeg, kZeroNeg, kZero, 0xFB, gqr(3, 31)},
    {0x1ull, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x60000004u, gqr(4, 32)},  // denormals
    {0xB80FFFFFFFFFFFFFull, 0x3810000000000000ull, 0x36A0000000000000ull, 0, gqr(5, 63)},  // single's least
    {0x47EFFFFFE0000000ull, 0x47F0000000000000ull, 0x7FEFFFFFFFFFFFFFull, 1, gqr(6, 0)},   // overflow
    {0x4004000000000000ull, 0x41DFFFFFFFE00000ull, 0xBFF8000000000000ull, 2, gqr(7, 1)},   // fctiw's top
    {0x3FE0000000000000ull, 0xC1E0000000100000ull, 0x41E0000000000000ull, 3, gqr(0, 31)},  // its bottom
    {kOne, kZero, kZeroNeg, 0xF8, gqr(1, 32)},                    // 1 / 0, and 1 / sqrt(0)
    {kOneNeg, kOneNeg, kZero, 0, gqr(2, 63)},                     // 1 / sqrt(-1)
};
constexpr int kFixedFpCount = sizeof kFixedFp / sizeof *kFixedFp;

void fix_fp(CPU* c, uint32_t insn, const FixedFp& f) {
    const uint64_t v[] = {f.a, f.a, f.b, f.c};
    const int at[] = {21, 16, 11, 6};
    for (int k = 0; k < 4; k++) {
        const uint32_t n = insn >> at[k] & 31;
        c->f[n].u = v[k];
        memcpy(&c->ps1[n], &v[k], 8);
    }
    c->fpscr = f.fpscr;
    for (uint32_t& g : c->gqr) g = f.gqr;
}

// A state below 0 is fixed state kFixedAll + state, the integer ones first and then the FP ones.
constexpr int kFixedAll = kFixedCount + kFixedFpCount;

void fix_state(CPU* c, uint32_t insn, int k) {
    if (k < kFixedCount) fix(c, insn, kFixed[k]);
    else fix_fp(c, insn, kFixedFp[k - kFixedCount]);
}

// A memory form's registers, set so its whole reach is inside the window.
void point(CPU* c, const CaseStep& k, Rng& r) {
    if (k.base == CASE_NO_REG) return;
    uint32_t off = r.below(kWindow - k.span + 1);
    if (r.below(4)) off &= ~3u;
    uint32_t ea = kWindowBase + off;
    if (k.index == CASE_NO_REG) {
        if (k.base) c->r[k.base] = ea - (uint32_t)k.disp;  // rA 0's displacement is in the window already
    } else if (k.base == 0) {
        c->r[k.index] = ea;
    } else if (k.base == k.index) {
        ea &= ~1u;
        c->r[k.base] = ea / 2;
    } else {
        c->r[k.base] = value(r);
        c->r[k.index] = ea - c->r[k.base];
    }
}

std::string differ(const CPU& n, const CPU& i) {
    char b[200];
    auto u32 = [&](const char* name, int k, uint32_t x, uint32_t y) {
        if (x == y) return false;
        if (k < 0) snprintf(b, sizeof b, "%s native %08X interp %08X", name, x, y);
        else snprintf(b, sizeof b, "%s[%d] native %08X interp %08X", name, k, x, y);
        return true;
    };
    auto u64 = [&](const char* name, int k, uint64_t x, uint64_t y) {
        if (x == y) return false;
        const unsigned long long nx = x, ny = y;
        if (k < 0) snprintf(b, sizeof b, "%s native %016llX interp %016llX", name, nx, ny);
        else snprintf(b, sizeof b, "%s[%d] native %016llX interp %016llX", name, k, nx, ny);
        return true;
    };
    auto bits = [](double d) {
        uint64_t u;
        memcpy(&u, &d, 8);
        return u;
    };
    for (int k = 0; k < 32; k++)
        if (u32("r", k, n.r[k], i.r[k])) return b;
    for (int k = 0; k < 32; k++)
        if (u64("f", k, n.f[k].u, i.f[k].u) || u64("ps1", k, bits(n.ps1[k]), bits(i.ps1[k]))) return b;
    for (int k = 0; k < 8; k++)
        if (u32("cr", k, n.cr[k], i.cr[k])) return b;
    if (u32("xer_ca", -1, n.xer_ca, i.xer_ca) || u32("xer_so", -1, n.xer_so, i.xer_so) ||
        u32("xer_ov", -1, n.xer_ov, i.xer_ov) || u32("xer_bc", -1, n.xer_bc, i.xer_bc) ||
        u32("lr", -1, n.lr, i.lr) || u32("ctr", -1, n.ctr, i.ctr) || u32("msr", -1, n.msr, i.msr) ||
        u32("fpscr", -1, n.fpscr, i.fpscr) || u64("fprf_v", -1, bits(n.fprf_v), bits(i.fprf_v)) ||
        u32("fprf_pending", -1, n.fprf_pending, i.fprf_pending))
        return b;
    for (int k = 0; k < 8; k++)
        if (u32("gqr", k, n.gqr[k], i.gqr[k])) return b;
    for (int k = 0; k < 1024; k++)
        if (u32("spr", k, n.spr[k], i.spr[k])) return b;
    if (n.host != i.host) return "host";
    if (memcmp(&n, &i, sizeof n)) return "a byte of CPU outside its named fields";
    return "";
}

std::string calls_differ(const std::vector<Call>& n, const std::vector<Call>& i) {
    char b[200];
    for (size_t k = 0; k < n.size() || k < i.size(); k++) {
        if (k < n.size() && k < i.size() && !strcmp(n[k].fn, i[k].fn) && n[k].a == i[k].a && n[k].b == i[k].b &&
            n[k].c == i[k].c)
            continue;
        auto say = [](const std::vector<Call>& v, size_t k) {
            if (k >= v.size()) return std::string("nothing");
            char s[96];
            snprintf(s, sizeof s, "%s(%X, %X, %X)", v[k].fn, v[k].a, v[k].b, v[k].c);
            return std::string(s);
        };
        snprintf(b, sizeof b, "call %zu native %s interp %s", k, say(n, k).c_str(), say(i, k).c_str());
        return b;
    }
    return "";
}

std::string window_differs(const uint8_t* want) {
    if (!memcmp(want, g_mem, kWindow)) return "";
    uint32_t k = 0;
    while (want[k] == g_mem[k]) k++;
    char b[96];
    snprintf(b, sizeof b, "RAM at %08X native %02X interp %02X", kWindowBase + k, want[k], g_mem[k]);
    return b;
}

// The window as a state has it, and the guard bands as they always start.
void load_ram(const uint8_t* window) {
    memcpy(g_mem, window, kWindow);
    for (int k = 0; k < 2; k++) memcpy(g_mem + kGuardAt[k], g_guard[k], kGuard);
}

std::string guard_changed() {
    for (int k = 0; k < 2; k++) {
        const uint8_t* got = g_mem + kGuardAt[k];
        if (!memcmp(g_guard[k], got, kGuard)) continue;
        uint32_t n = 0;
        while (g_guard[k][n] == got[n]) n++;
        char b[96];
        snprintf(b, sizeof b, "RAM at physical %08X, outside the window", kGuardAt[k] + n);
        return b;
    }
    return "";
}

std::string clock_moved() { return g_vcount == 0 && g_vlimit == UINT64_MAX ? "" : "the clock moved"; }

std::vector<std::vector<uint8_t>> g_windows;  // each state's RAM, seeded
uint8_t g_native[kWindow];
int g_failed;
std::vector<int> g_form_fails;
int g_refused_fails;

std::string fields(uint32_t i) {
    char b[96];
    snprintf(b, sizeof b, "d %u a %u b %u c %u me %u imm %04X", i >> 21 & 31, i >> 16 & 31, i >> 11 & 31,
             i >> 6 & 31, i >> 1 & 31, i & 0xFFFF);
    return b;
}

// form is the word's form, or -1 for a refused word. A state below 0 is fixed state kFixedAll + state.
void fail(int form, uint32_t insn, uint32_t pc, int state, const std::string& what) {
    g_failed++;
    int& shown = form < 0 ? g_refused_fails : g_form_fails[form];
    if (shown++ >= 2) return;
    printf("FAIL %08X at %08X (%s; %s) %s %d: %s\n", insn, pc, form < 0 ? "refused" : g_case_forms[form].name,
           fields(insn).c_str(), state < 0 ? "fixed state" : "state", state < 0 ? kFixedAll + state : state,
           what.c_str());
}

// The word's run through interp_step must refuse it and change nothing.
bool step_refuses(int form, uint32_t insn, uint32_t pc, const char* who) {
    Rng r = rng_for(insn, pc, 0);
    CPU st;
    random_cpu(&st, r);
    load_ram(g_windows[0].data());
    g_calls.clear();
    CPU in = st;
    std::string what;
    if (interp_step(&in, pc, insn) != INTERP_UNIMPL)
        what = std::string("interp_step took a word ") + who;
    else if ((what = differ(st, in)).empty() && (what = calls_differ({}, g_calls)).empty() &&
             (what = window_differs(g_windows[0].data())).empty() && (what = guard_changed()).empty())
        return true;
    else
        what = "interp_step refused it but changed " + what;
    fail(form, insn, pc, 0, what);
    return false;
}

bool branch_refuses(int form, uint32_t insn, uint32_t pc, const char* who) {
    Rng r = rng_for(insn, pc, 0);
    CPU st;
    random_cpu(&st, r);
    CPU in = st;
    InterpBranch out{};
    std::string what;
    if (interp_branch(&in, pc, insn, &out))
        what = std::string("interp_branch took a word ") + who;
    else if ((what = differ(st, in)).empty())
        return true;
    else
        what = "interp_branch refused it but changed " + what;
    fail(form, insn, pc, 0, what);
    return false;
}

// A state s below 0 is fixed state kFixedAll + s, run over window 0.
bool run_step(const CaseStep& k, int s) {
    Rng r = rng_for(k.insn, k.pc, s);
    CPU st;
    random_cpu(&st, r);
    if (s < 0) fix_state(&st, k.insn, kFixedAll + s);
    point(&st, k, r);
    const uint8_t* w = g_windows[s < 0 ? 0 : s].data();

    load_ram(w);
    g_calls.clear();
    CPU nat = st;
    k.fn(&nat);
    const std::vector<Call> nat_calls = g_calls;
    memcpy(g_native, g_mem, kWindow);
    const std::string nat_guard = guard_changed();
    bool rfi = false;
    for (const Call& call : nat_calls) rfi |= !strcmp(call.fn, "hle_rfi");

    load_ram(w);
    g_calls.clear();
    CPU in = st;
    const InterpStep ret = interp_step(&in, k.pc, k.insn);
    std::string what;
    if (!nat_guard.empty())
        what = "translate's C itself changed " + nat_guard;
    else if (ret == INTERP_UNIMPL)
        what = "interp_step refused a word translate accepts";
    else if (ret != (rfi ? INTERP_RFI : INTERP_NEXT))
        what = rfi ? "hle_rfi ran and interp_step didn't return INTERP_RFI" : "INTERP_RFI with no hle_rfi";
    else if ((what = differ(nat, in)).empty() && (what = calls_differ(nat_calls, g_calls)).empty() &&
             (what = window_differs(g_native)).empty() && (what = guard_changed()).empty() &&
             (what = clock_moved()).empty())
        return true;
    fail(k.form, k.insn, k.pc, s, what);
    return false;
}

// Runs not taken and taken, by form, of each branch that links and can test something, counted from
// the native side.
std::vector<uint64_t> g_link_not_taken, g_link_taken;

bool run_branch(const CaseBranch& k, int s) {
    Rng r = rng_for(k.insn, k.pc, s);
    CPU st;
    random_cpu(&st, r);
    g_calls.clear();
    CPU nat = st;
    int taken = 0;
    uint32_t target = 0;
    k.fn(&nat, &taken, &target);
    if (k.link && k.kind != CASE_B) (taken ? g_link_taken : g_link_not_taken)[k.form]++;
    CPU in = st;
    InterpBranch out{};
    std::string what;
    char b[96];
    if (!interp_branch(&in, k.pc, k.insn, &out)) {
        what = "interp_branch refused a word decode_branch accepts";
    } else if (out.kind != k.kind || out.link != (k.link != 0)) {
        snprintf(b, sizeof b, "kind %d link %d, interp %d link %d", k.kind, k.link, out.kind, out.link);
        what = b;
    } else if (out.taken != (taken != 0)) {
        snprintf(b, sizeof b, "taken native %d interp %d", taken, out.taken);
        what = b;
    } else if (out.target != target) {
        snprintf(b, sizeof b, "target native %08X interp %08X", target, out.target);
        what = b;
    } else if ((what = differ(nat, in)).empty() && (what = calls_differ({}, g_calls)).empty() &&
               (what = clock_moved()).empty()) {
        return true;
    }
    fail(k.form, k.insn, k.pc, s, what);
    return false;
}

// The family's forms, and how many of them differed somewhere.
void count_forms(const CaseFamily& fam, uint32_t* forms, uint32_t* differ) {
    std::vector<bool> in(g_case_form_count);
    for (uint32_t k = 0; k < fam.step_count; k++) in[fam.steps[k].form] = true;
    for (uint32_t k = 0; k < fam.branch_count; k++) in[fam.branches[k].form] = true;
    *forms = *differ = 0;
    for (uint32_t f = 0; f < g_case_form_count; f++) {
        *forms += in[f];
        *differ += in[f] && g_form_fails[f];
    }
}

}  // namespace

int main(int argc, char** argv) {
    int states = 8;
    for (int k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "--states") && k + 1 < argc && atoi(argv[k + 1]) > 0) {
            states = atoi(argv[++k]);
        } else {
            fprintf(stderr, "usage: test_interp [--states M]\n");
            return 2;
        }
    }
    // All of the guest's address space plus a megabyte past it, as mem_init reserves it, so a wrong
    // address in the interpreter shows up as a difference rather than a fault.
    g_mem = (uint8_t*)calloc(1, 0x40000000u + 0x100000u);
    if (!g_mem) {
        fprintf(stderr, "test_interp: no memory for the guest's\n");
        return 2;
    }
    Rng wr{g_case_seed};
    g_windows.resize(states);
    for (auto& w : g_windows) {
        w.resize(kWindow);
        for (uint8_t& b : w) b = (uint8_t)wr.next();
    }
    for (auto& band : g_guard)
        for (uint8_t& b : band) b = (uint8_t)wr.next();
    g_form_fails.assign(g_case_form_count, 0);
    g_link_not_taken.assign(g_case_form_count, 0);
    g_link_taken.assign(g_case_form_count, 0);
    printf("cases from ppc_cases.py --seed %u --per-form %u, %d states each and %d fixed ones for a step\n",
           g_case_seed, g_case_per_form, states, kFixedAll);

    for (uint32_t f = 0; f < g_case_family_count; f++) {
        const CaseFamily& fam = *g_case_families[f];
        uint64_t runs = 0;
        for (uint32_t k = 0; k < fam.step_count; k++)
            branch_refuses(fam.steps[k].form, fam.steps[k].insn, fam.steps[k].pc, "decode_branch refuses");
        for (uint32_t k = 0; k < fam.branch_count; k++)
            step_refuses(fam.branches[k].form, fam.branches[k].insn, fam.branches[k].pc, "translate refuses");
        for (uint32_t k = 0; k < fam.step_count; k++)
            for (int s = -kFixedAll; s < states; s++, runs++) run_step(fam.steps[k], s);
        for (uint32_t k = 0; k < fam.branch_count; k++)
            for (int s = 0; s < states; s++, runs++) run_branch(fam.branches[k], s);
        uint32_t forms, differ;
        count_forms(fam, &forms, &differ);
        printf("%-7s %5u cases %3u forms %7llu runs  ", fam.name, fam.step_count + fam.branch_count, forms,
               (unsigned long long)runs);
        if (differ) printf("DIFFER in %u forms\n", differ);
        else printf("all match\n");
    }

    int refused_bad = 0;
    for (uint32_t k = 0; k < g_case_refused_count; k++) {
        const CaseRefused& r = g_case_refused[k];
        refused_bad += !step_refuses(-1, r.insn, r.pc, "translate refuses");
        refused_bad += !branch_refuses(-1, r.insn, r.pc, "decode_branch refuses");
    }
    printf("refused %5u words  %s\n", g_case_refused_count, refused_bad ? "DIFFER" : "all refused");

    // A branch that links sets LR whether it's taken or not, so each such form has to have run both
    // ways for that to have been checked.
    uint64_t not_taken = 0;
    for (uint32_t f = 0; f < g_case_form_count; f++) {
        if (!g_link_not_taken[f] && !g_link_taken[f]) continue;
        not_taken += g_link_not_taken[f];
        if (!g_link_not_taken[f] || !g_link_taken[f]) {
            g_failed++;
            printf("FAIL %s ran %s only, so its LR went unchecked the other way\n", g_case_forms[f].name,
                   g_link_taken[f] ? "taken" : "not taken");
        }
    }
    printf("calls that test something ran %llu times not taken\n", (unsigned long long)not_taken);

    if (g_failed) printf("%d differences\n", g_failed);
    else printf("everything matches\n");
    return g_failed ? 1 : 0;
}
