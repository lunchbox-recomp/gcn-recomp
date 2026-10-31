// Integer loads and stores, the string and multiple-word forms and the cache operations, each as
// ppc.translate writes it, through recomp.h's LD and ST. An update form loads or stores first and
// then sets rA to the address, even when rA is 0 or rD.
//
// The FP and quantized loads and stores are here too, through LDF32, STF32, LD64, ST64 and the
// psq_load and psq_store that translate's text calls. A quantized type and its scale are theirs to
// read from the GQR, so nothing here works one out.
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

// lfs and the rest, by the primary opcode's offset from 48 or the X-form's extended opcode's from
// 535 in steps of 32. Both run lfs lfsu lfd lfdu stfs stfsu stfd stfdu in that order, so bit 0 says
// update.
InterpStep fp(CPU* c, uint32_t i, uint32_t ea, uint32_t kind) {
    const uint32_t d = insn_d(i);
    switch (kind >> 1) {
    case 0: {
        const double v = LDF32(ea);
        c->f[d].d = v;
        c->ps1[d] = v;
        break;
    }
    case 1: c->f[d].u = LD64(ea); break;
    case 2: STF32(ea, c->f[d].d); break;
    case 3: ST64(ea, c->f[d].u); break;
    }
    if (kind & 1) c->r[insn_a(i)] = ea;
    return INTERP_NEXT;
}

// psq_l and the rest, W and I where the form keeps them.
InterpStep psq(CPU* c, uint32_t i, uint32_t ea, bool store, bool update, uint32_t w, uint32_t gq) {
    if (store) psq_store(c, ea, (int)insn_d(i), (int)w, (int)gq);
    else psq_load(c, ea, (int)insn_d(i), (int)w, (int)gq);
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
    case 535: case 567: case 599: case 631: case 663: case 695: case 727: case 759:
        return fp(c, i, ea, (insn_xo(i) - 535) / 32);  // lfsx and the rest, 32 apart
    case 983: ST32(ea, (uint32_t)c->f[d].u); return INTERP_NEXT;  // stfiwx
    default: return INTERP_UNIMPL;
    }
}

// Op 4's indexed quantized loads and stores, which bits 1-6 give as psq_lx 6, psq_stx 7, psq_lux 38
// and psq_stux 39.
InterpStep op4(CPU* c, uint32_t i) {
    const uint32_t xo6 = (i >> 1) & 0x3F;
    if (xo6 != 6 && xo6 != 7 && xo6 != 38 && xo6 != 39) return INTERP_UNIMPL;
    const uint32_t a = insn_a(i);
    const uint32_t ea = (a ? c->r[a] : 0) + c->r[insn_b(i)];
    return psq(c, i, ea, xo6 & 1, xo6 & 32, (i >> 10) & 1, (i >> 7) & 7);
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
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
        return fp(c, i, ea, op - 48);  // lfs and the rest
    case 56: case 57: case 60: case 61: {  // psq_l psq_lu psq_st psq_stu, with a 12-bit displacement
        const uint32_t qea = (a ? c->r[a] : 0) + (uint32_t)((int32_t)(i << 20) >> 20);
        return psq(c, i, qea, op >= 60, update, (i >> 15) & 1, (i >> 12) & 7);
    }
    case 31: return op31(c, i);
    case 4: return op4(c, i);
    default: return INTERP_UNIMPL;
    }
}
