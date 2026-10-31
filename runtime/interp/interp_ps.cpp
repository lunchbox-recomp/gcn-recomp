// Op 4's paired singles and dcbz_l, each as ppc.translate writes it. Both lanes are worked out before
// either is written, and a rounded pair records lane 0 for FPRF, as translate's pair() does. Like
// interp_fp.cpp, it has to compile with the game's FP flags.
//
// Each case reads its registers in the order translate's text names them. Clang orders a sum's or a
// product's operands by when they were read, and when both are NaNs that order can decide which comes
// out. Read up front, ps_muls0's lane 1 would get A1's NaN where translate's C gets C0's.
#include "interp.h"

namespace {

InterpStep pair(CPU* c, uint32_t i, double v0, double v1, bool round) {
    if (round) {
        v0 = (double)(float)v0;
        v1 = (double)(float)v1;
    }
    c->f[insn_d(i)].d = v0;
    c->ps1[insn_d(i)] = v1;
    if (round) interp_fprf(c, v0, true);
    interp_cr1(c, i);
    return INTERP_NEXT;
}

}  // namespace

InterpStep interp_ps(CPU* c, uint32_t i) {
    if (i >> 26 != 4) return INTERP_UNIMPL;
    const uint32_t xo6 = (i >> 1) & 0x3F;
    if (xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39) return INTERP_UNIMPL;  // interp_mem's psq_lx and the rest
    const uint32_t a = insn_a(i), b = insn_b(i), cc = insn_c(i);
    const auto A0 = [=] { return c->f[a].d; };
    const auto A1 = [=] { return c->ps1[a]; };
    const auto B0 = [=] { return c->f[b].d; };
    const auto B1 = [=] { return c->ps1[b]; };
    const auto C0 = [=] { return c->f[cc].d; };
    const auto C1 = [=] { return c->ps1[cc]; };
    switch (insn_xo5(i)) {
    case 18: return pair(c, i, A0() / B0(), A1() / B1(), true);                          // ps_div
    case 20: return pair(c, i, A0() - B0(), A1() - B1(), true);                          // ps_sub
    case 21: return pair(c, i, A0() + B0(), A1() + B1(), true);                          // ps_add
    case 25: return pair(c, i, A0() * C0(), A1() * C1(), true);                          // ps_mul
    case 24: return pair(c, i, 1.0 / B0(), 1.0 / B1(), true);                            // ps_res
    case 26: return pair(c, i, 1.0 / sqrt(B0()), 1.0 / sqrt(B1()), true);                // ps_rsqrte
    case 23: return pair(c, i, (A0() >= 0.0) ? C0() : B0(), (A1() >= 0.0) ? C1() : B1(), false);  // ps_sel
    case 28: return pair(c, i, fma(A0(), C0(), -B0()), fma(A1(), C1(), -B1()), true);    // ps_msub
    case 29: return pair(c, i, fma(A0(), C0(), B0()), fma(A1(), C1(), B1()), true);      // ps_madd
    case 30: return pair(c, i, -fma(A0(), C0(), -B0()), -fma(A1(), C1(), -B1()), true);  // ps_nmsub
    case 31: return pair(c, i, -fma(A0(), C0(), B0()), -fma(A1(), C1(), B1()), true);    // ps_nmadd
    case 10: return pair(c, i, A0() + B1(), C1(), true);                                 // ps_sum0
    case 11: return pair(c, i, C0(), A0() + B1(), true);                                 // ps_sum1
    case 12: return pair(c, i, A0() * C0(), A1() * C0(), true);                          // ps_muls0
    case 13: return pair(c, i, A0() * C1(), A1() * C1(), true);                          // ps_muls1
    case 14: return pair(c, i, fma(A0(), C0(), B0()), fma(A1(), C0(), B1()), true);      // ps_madds0
    case 15: return pair(c, i, fma(A0(), C1(), B0()), fma(A1(), C1(), B1()), true);      // ps_madds1
    }
    const uint32_t crf = insn_d(i) >> 2;
    switch (insn_xo(i)) {
    case 0:
    case 32: c->cr[crf] = FCMP(A0(), B0()); return INTERP_NEXT;  // ps_cmpu0, ps_cmpo0
    case 64:
    case 96: c->cr[crf] = FCMP(A1(), B1()); return INTERP_NEXT;  // ps_cmpu1, ps_cmpo1
    case 40: return pair(c, i, -B0(), -B1(), false);                    // ps_neg
    case 72: return pair(c, i, B0(), B1(), false);                      // ps_mr
    case 136: return pair(c, i, -fabs(B0()), -fabs(B1()), false);       // ps_nabs
    case 264: return pair(c, i, fabs(B0()), fabs(B1()), false);         // ps_abs
    case 528: return pair(c, i, A0(), B0(), false);                     // ps_merge00
    case 560: return pair(c, i, A0(), B1(), false);                     // ps_merge01
    case 592: return pair(c, i, A1(), B0(), false);                     // ps_merge10
    case 624: return pair(c, i, A1(), B1(), false);                     // ps_merge11
    case 1014: hle_dcbz_l(c, (a ? c->r[a] : 0) + c->r[b]); return INTERP_NEXT;  // dcbz_l
    default: return INTERP_UNIMPL;
    }
}
