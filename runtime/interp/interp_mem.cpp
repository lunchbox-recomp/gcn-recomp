// Integer loads and stores, the string and multiple-word forms and the cache operations, each as
// ppc.translate writes it, through recomp.h's LD and ST. An update form loads or stores first and
// then sets rA to the address, even when rA is 0 or rD.
#include "interp.h"

namespace {

enum Width { W8, W16, WS16, W32, W32BR, W16BR };

uint32_t load(Width w, uint32_t ea) {
    switch (w) {
    case W8: return LD8(ea);
    case W16: return LD16(ea);
    case WS16: return LDS16(ea);
    case W32: return LD32(ea);
    case W32BR: return LD32BR(ea);
    case W16BR: return LD16BR(ea);
    }
    return 0;
}

void store(Width w, uint32_t ea, uint32_t v) {
    switch (w) {
    case W8: ST8(ea, v); return;
    case W16: ST16(ea, v); return;
    case W32: ST32(ea, v); return;
    case W32BR: ST32BR(ea, v); return;
    case W16BR: ST16BR(ea, v); return;
    case WS16: return;
    }
}

InterpStep ld(CPU* c, uint32_t i, uint32_t ea, Width w, bool update) {
    c->r[insn_d(i)] = load(w, ea);
    if (update) c->r[insn_a(i)] = ea;
    return INTERP_NEXT;
}

InterpStep st(CPU* c, uint32_t i, uint32_t ea, Width w, bool update) {
    store(w, ea, c->r[insn_d(i)]);
    if (update) c->r[insn_a(i)] = ea;
    return INTERP_NEXT;
}

InterpStep op31(CPU* c, uint32_t i) {
    const uint32_t d = insn_d(i), a = insn_a(i), b = insn_b(i);
    const uint32_t ea = (a ? c->r[a] : 0) + c->r[b];
    switch (insn_xo(i)) {
    case 23: case 20: return ld(c, i, ea, W32, false);  // lwzx, lwarx
    case 55: return ld(c, i, ea, W32, true);            // lwzux
    case 87: return ld(c, i, ea, W8, false);            // lbzx
    case 119: return ld(c, i, ea, W8, true);            // lbzux
    case 279: return ld(c, i, ea, W16, false);          // lhzx
    case 311: return ld(c, i, ea, W16, true);           // lhzux
    case 343: return ld(c, i, ea, WS16, false);         // lhax
    case 375: return ld(c, i, ea, WS16, true);          // lhaux
    case 534: return ld(c, i, ea, W32BR, false);        // lwbrx
    case 790: return ld(c, i, ea, W16BR, false);        // lhbrx
    case 151: return st(c, i, ea, W32, false);          // stwx
    case 183: return st(c, i, ea, W32, true);           // stwux
    case 215: return st(c, i, ea, W8, false);           // stbx
    case 247: return st(c, i, ea, W8, true);            // stbux
    case 407: return st(c, i, ea, W16, false);          // sthx
    case 439: return st(c, i, ea, W16, true);           // sthux
    case 662: return st(c, i, ea, W32BR, false);        // stwbrx
    case 918: return st(c, i, ea, W16BR, false);        // sthbrx
    case 150:  // stwcx., which always succeeds
        ST32(ea, c->r[d]);
        c->cr[0] = 2 | c->xer_so;
        return INTERP_NEXT;
    case 597: hle_lswi(c, a ? c->r[a] : 0, (int)d, b ? (int)b : 32); return INTERP_NEXT;
    case 725: hle_stswi(c, a ? c->r[a] : 0, (int)d, b ? (int)b : 32); return INTERP_NEXT;
    case 1014: hle_dcbz(c, ea); return INTERP_NEXT;
    case 470: hle_dcbi(c, ea); return INTERP_NEXT;
    case 54: case 86: case 246: case 278: case 982: return INTERP_NEXT;  // dcbst dcbf dcbtst dcbt icbi
    default: return INTERP_UNIMPL;
    }
}

}  // namespace

InterpStep interp_mem(CPU* c, uint32_t i) {
    const uint32_t op = i >> 26, a = insn_a(i);
    const uint32_t ea = (a ? c->r[a] : 0) + insn_simm(i);
    const bool update = op & 1;
    switch (op) {
    case 32: case 33: return ld(c, i, ea, W32, update);   // lwz lwzu
    case 34: case 35: return ld(c, i, ea, W8, update);    // lbz lbzu
    case 40: case 41: return ld(c, i, ea, W16, update);   // lhz lhzu
    case 42: case 43: return ld(c, i, ea, WS16, update);  // lha lhau
    case 36: case 37: return st(c, i, ea, W32, update);   // stw stwu
    case 38: case 39: return st(c, i, ea, W8, update);    // stb stbu
    case 44: case 45: return st(c, i, ea, W16, update);   // sth sthu
    case 46:  // lmw, its address taken before any register is loaded
        for (uint32_t n = insn_d(i); n < 32; n++) c->r[n] = LD32(ea + (n - insn_d(i)) * 4);
        return INTERP_NEXT;
    case 47:  // stmw
        for (uint32_t n = insn_d(i); n < 32; n++) ST32(ea + (n - insn_d(i)) * 4, c->r[n]);
        return INTERP_NEXT;
    case 31: return op31(c, i);
    default: return INTERP_UNIMPL;
    }
}
