// The random and fixed CPU states test_interp runs each case from.
#include "test_interp_states.h"
#include <cstring>

namespace {

const uint32_t kEdges[] = {0, 1, 2, 31, 32, 33, 63, 64, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF,
                           0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu};

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
    case 4: {  // either sign, at least 2^-20 and under 2^21, and a single exactly half the time
        const uint64_t u = (uint64_t)r.below(2) << 63 | (uint64_t)(1003 + r.below(41)) << 52 |
                           (r.next() & 0xFFFFFFFFFFFFFull);
        return r.below(2) ? u : u & ~0x1FFFFFFFull;
    }
    default: return r.next();
    }
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
//
// A memory form whose reach is 4 bytes finds single where the reach starts, and a longer one finds a
// there. Random RAM seldom holds a signaling NaN, which a single has to be for lfs to quiet it.
struct FixedFp {
    uint64_t a, b, c;
    uint32_t fpscr, gqr;
    uint32_t single;
};
constexpr uint32_t kSingleSnan = 0x7FA00001u, kSingleSnanNeg = 0xFF800001u;
constexpr uint32_t kSingleQnan = 0x7FC00001u, kSingleQnanNeg = 0xFFC00002u;
constexpr uint64_t kHalf = 0x3FE0000000000000ull, kTwoAndHalf = 0x4004000000000000ull;
constexpr uint64_t kOneAndHalfNeg = 0xBFF8000000000000ull;
const FixedFp kFixedFp[] = {
    {kOneUp, kOneNeg, kOneDown, 0, gqr(0, 0), kSingleSnan},          // a*c + b fused or not
    {kOneUp, kOne, kOneDown, 1, gqr(4, 1), kSingleSnanNeg},          // a*c - b fused or not
    {kQnan, kSnan, kQnanNeg, 2, gqr(5, 31), kSingleQnan},            // which NaN comes out
    {kSnan, kQnan, kSnanNeg, 3, gqr(6, 32), kSingleQnanNeg},
    {kInf, kInf, kZero, 0xF8, gqr(7, 63), 0x7F800000u},              // inf - inf and inf * 0
    {kInfNeg, kInf, kInfNeg, 0xFFFFFFFFu, gqr(1, 0), 0xFF800000u},   // inf + -inf
    {kZero, kZeroNeg, kZeroNeg, 0x9FF80700u, gqr(2, 1), 0x80000000u},  // the signs of zero results
    {kZeroNeg, kZeroNeg, kZero, 0xFB, gqr(3, 31), 0},
    {0x1ull, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x60000004u, gqr(4, 32), 0x00000001u},  // denormals
    {0xB80FFFFFFFFFFFFFull, 0x3810000000000000ull, 0x36A0000000000000ull, 0, gqr(5, 63), 0x807FFFFFu},  // single's least
    {0x47EFFFFFE0000000ull, 0x47F0000000000000ull, 0x7FEFFFFFFFFFFFFFull, 1, gqr(6, 0), 0x7F7FFFFFu},   // overflow
    {0x4004000000000000ull, 0x41DFFFFFFFE00000ull, 0xBFF8000000000000ull, 2, gqr(7, 1), kSingleSnan},   // fctiw's top
    {0x3FE0000000000000ull, 0xC1E0000000100000ull, 0x41E0000000000000ull, 3, gqr(0, 31), kSingleSnanNeg},  // its bottom
    {kOne, kZero, kZeroNeg, 0xF8, gqr(1, 32), kSingleQnan},          // 1 / 0, and 1 / sqrt(0)
    {kOneNeg, kOneNeg, kZero, 0, gqr(2, 63), kSingleQnanNeg},        // 1 / sqrt(-1)
    // The ties 0.5, 2.5 and -1.5 in fB, which fctiw, fctiwz, frsp, fres and frsqrte read, under round
    // to nearest.
    {kOneAndHalfNeg, kHalf, kTwoAndHalf, 0, gqr(3, 63), 0x3F000000u},
    {kHalf, kTwoAndHalf, kOneAndHalfNeg, 0xF8, gqr(4, 0), 0x40200000u},
    {kTwoAndHalf, kOneAndHalfNeg, kHalf, 0, gqr(5, 1), 0xBFC00000u},
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

}  // namespace

Rng rng_for(uint32_t insn, uint32_t pc, int state) {
    Rng r{(uint64_t)insn << 32 ^ pc ^ (uint64_t)(state + 1) * 0xD1B54A32D192ED03ull};
    r.next();
    return r;
}

uint32_t value(Rng& r) {
    switch (r.below(8)) {
    case 0:
    case 1:
    case 2: return kEdges[r.below(sizeof kEdges / sizeof *kEdges)];
    case 3: return r.below(64);
    default: return r.u32();
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

const int kFixedAll = kFixedCount + kFixedFpCount;

void fix_state(CPU* c, uint32_t insn, int k) {
    if (k < kFixedCount) fix(c, insn, kFixed[k]);
    else fix_fp(c, insn, kFixedFp[k - kFixedCount]);
}

uint32_t fixed_memory(int k, uint32_t span, uint8_t out[8]) {
    if (k < kFixedCount || span < 4) return 0;
    const FixedFp& f = kFixedFp[k - kFixedCount];
    const uint32_t n = span == 4 ? 4 : 8;
    const uint64_t v = n == 4 ? (uint64_t)f.single << 32 : f.a;
    for (uint32_t b = 0; b < n; b++) out[b] = (uint8_t)(v >> (56 - 8 * b));  // big-endian, as the guest has it
    return n;
}
