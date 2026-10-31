// Floating point, op 59's single-precision arithmetic and op 63's double-precision arithmetic,
// compares, conversions, moves and FPSCR operations, each as ppc.translate writes it. A result is
// rounded, recorded for FPRF and copied to CR1 just as translate's text does, through the same
// helpers in recomp.h. This file has to compile with the game's FP flags, or a contracted multiply
// and add would round once where translate's C rounds twice.
//
// A, B and C are read up front, which is the order translate's text names them in for every sum and
// product here, and that gives the operand order translate's C compiles to on its own. In a larger
// function clang orders those operands by where their values come from, so which of two different NaNs
// native code gives in fadd, fadds, fmul or fmuls depends on the code around it. The interpreter can
// give the other one.
#include "interp.h"

namespace {

// translate59, whose results are all rounded to single and go to both lanes. fnmsubs and fnmadds
// negate after the rounding, which gives the same value translate's C does. With the negation inside
// the switch, clang folds it into an FNMSUB or FNMADD, which gives +0 where -fma gives -0 and can give
// a NaN the other sign.
InterpStep op59(CPU* c, uint32_t i) {
    const double A = c->f[insn_a(i)].d, B = c->f[insn_b(i)].d, C = c->f[insn_c(i)].d;
    double r;
    bool neg = false;
    switch (insn_xo5(i)) {
    case 18: r = A / B; break;                        // fdivs
    case 20: r = A - B; break;                        // fsubs
    case 21: r = A + B; break;                        // fadds
    case 25: r = A * C; break;                        // fmuls
    case 24: r = 1.0 / B; break;                      // fres
    case 28: r = fma(A, C, -B); break;                // fmsubs
    case 29: r = fma(A, C, B); break;                 // fmadds
    case 30: r = fma(A, C, -B); neg = true; break;    // fnmsubs
    case 31: r = fma(A, C, B); neg = true; break;     // fnmadds
    default: return INTERP_UNIMPL;
    }
    const float s = (float)r;
    const double v = neg ? -(double)s : (double)s;
    c->f[insn_d(i)].d = v;
    c->ps1[insn_d(i)] = v;
    interp_fprf(c, v, true);
    interp_cr1(c, i);
    return INTERP_NEXT;
}

// translate63's A-forms, which it tells apart by bits 1-5 before it looks at bits 1-10. Here fnmsub's
// and fnmadd's -fma folds into an FNMSUB or FNMADD, just as it does in translate's own C.
bool op63_a(CPU* c, uint32_t i) {
    const double A = c->f[insn_a(i)].d, B = c->f[insn_b(i)].d, C = c->f[insn_c(i)].d;
    double v;
    switch (insn_xo5(i)) {
    case 18: v = A / B; break;              // fdiv
    case 20: v = A - B; break;              // fsub
    case 21: v = A + B; break;              // fadd
    case 25: v = A * C; break;              // fmul
    case 26: v = 1.0 / sqrt(B); break;      // frsqrte
    case 28: v = fma(A, C, -B); break;      // fmsub
    case 29: v = fma(A, C, B); break;       // fmadd
    case 30: v = -fma(A, C, -B); break;     // fnmsub
    case 31: v = -fma(A, C, B); break;      // fnmadd
    case 22: v = sqrt(B); break;            // fsqrt
    case 23:                                // fsel
        c->f[insn_d(i)].d = (A >= 0.0) ? C : B;
        interp_cr1(c, i);
        return true;
    default: return false;
    }
    c->f[insn_d(i)].d = v;
    interp_fprf(c, v, false);
    interp_cr1(c, i);
    return true;
}

InterpStep op63(CPU* c, uint32_t i) {
    if (op63_a(c, i)) return INTERP_NEXT;
    const uint32_t d = insn_d(i), b = insn_b(i);
    const uint64_t sign = 0x8000000000000000ull;
    switch (insn_xo(i)) {
    case 0:
    case 32:  // fcmpu, fcmpo
        c->cr[d >> 2] = FCMP(c->f[insn_a(i)].d, c->f[b].d);
        return INTERP_NEXT;
    case 64:  // mcrfs
        c->cr[d >> 2] = (FPSCR_READ(c) >> (28 - 4 * (insn_a(i) >> 2))) & 0xF;
        return INTERP_NEXT;
    case 12: {  // frsp
        const double v = (double)(float)c->f[b].d;
        c->f[d].d = v;
        c->ps1[d] = v;
        interp_fprf(c, v, true);
        break;
    }
    case 14: c->f[d].u = 0xFFF8000000000000ull | (uint32_t)FCTIW(c->f[b].d); break;
    case 15: c->f[d].u = 0xFFF8000000000000ull | (uint32_t)FCTIWZ(c->f[b].d); break;
    case 72: c->f[d].u = c->f[b].u; break;           // fmr
    case 40: c->f[d].u = c->f[b].u ^ sign; break;    // fneg
    case 264: c->f[d].u = c->f[b].u & ~sign; break;  // fabs
    case 136: c->f[d].u = c->f[b].u | sign; break;   // fnabs
    case 583: c->f[d].u = 0xFFF8000000000000ull | FPSCR_READ(c); break;  // mffs
    case 711: {  // mtfsf, FM's bits naming FPSCR's fields from the top
        const uint32_t fm = (i >> 17) & 0xFF;
        uint32_t m = 0;
        for (int k = 0; k < 8; k++)
            if (fm & (0x80 >> k)) m |= 0xF0000000u >> (4 * k);
        hle_set_fpscr(c, (FPSCR_READ(c) & ~m) | ((uint32_t)c->f[b].u & m));
        break;
    }
    case 134: {  // mtfsfi
        const uint32_t sh = 28 - 4 * (d >> 2), imm = (i >> 12) & 0xF;
        hle_set_fpscr(c, (FPSCR_READ(c) & ~(0xFu << sh)) | (imm << sh));
        break;
    }
    case 38: hle_set_fpscr(c, FPSCR_READ(c) | (0x80000000u >> d)); break;   // mtfsb1
    case 70: hle_set_fpscr(c, FPSCR_READ(c) & ~(0x80000000u >> d)); break;  // mtfsb0
    default: return INTERP_UNIMPL;
    }
    interp_cr1(c, i);
    return INTERP_NEXT;
}

}  // namespace

InterpStep interp_fp(CPU* c, uint32_t i) {
    switch (i >> 26) {
    case 59: return op59(c, i);
    case 63: return op63(c, i);
    default: return INTERP_UNIMPL;
    }
}
