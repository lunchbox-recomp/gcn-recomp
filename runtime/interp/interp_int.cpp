// Integer arithmetic, logic, rotates, shifts and compares, and the condition register's own
// operations, each as ppc.translate writes it. Every source is read before the destination is
// written, as translate's text reads them.
#include "interp.h"

namespace {

uint32_t mask32(uint32_t mb, uint32_t me) {
    const uint32_t lo = 0xFFFFFFFFu >> mb, hi = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? lo & hi : lo | hi;
}

// x + y + ci into rD, with the carry out, and OV from x and y as ppc.arith_xo has it.
void carry(CPU* c, uint32_t d, uint32_t x, uint32_t y, uint32_t ci, bool oe) {
    const uint64_t t = (uint64_t)x + y + ci;
    const uint32_t r = (uint32_t)t;
    c->r[d] = r;
    c->xer_ca = (uint8_t)(t >> 32);
    if (oe) SET_OV(((x ^ r) & (y ^ r)) >> 31);
}

// Op 31's XO-form arithmetic (ppc.arith_xo), by the low nine bits of the extended opcode. False for
// one that isn't.
bool arith(CPU* c, uint32_t xo9, uint32_t d, uint32_t a, uint32_t b, bool oe) {
    const uint32_t A = c->r[a], B = c->r[b];
    switch (xo9) {
    case 266: {  // add
        const uint32_t r = A + B;
        c->r[d] = r;
        if (oe) SET_OV(((A ^ r) & (B ^ r)) >> 31);
        return true;
    }
    case 40: {  // subf
        const uint32_t x = ~A, r = B - A;
        c->r[d] = r;
        if (oe) SET_OV(((x ^ r) & (B ^ r)) >> 31);
        return true;
    }
    case 10: carry(c, d, A, B, 0, oe); return true;                          // addc
    case 138: carry(c, d, A, B, c->xer_ca, oe); return true;                 // adde
    case 202: carry(c, d, A, 0, c->xer_ca, oe); return true;                 // addze
    case 234: carry(c, d, A, 0xFFFFFFFFu, c->xer_ca, oe); return true;       // addme
    case 8: carry(c, d, ~A, B, 1, oe); return true;                          // subfc
    case 136: carry(c, d, ~A, B, c->xer_ca, oe); return true;                // subfe
    case 200: carry(c, d, ~A, 0, c->xer_ca, oe); return true;                // subfze
    case 232: carry(c, d, ~A, 0xFFFFFFFFu, c->xer_ca, oe); return true;      // subfme
    case 104:  // neg
        c->r[d] = (uint32_t)(-(int64_t)(int32_t)A);
        if (oe) SET_OV(A == 0x80000000u);
        return true;
    case 235: {  // mullw
        const int64_t p = (int64_t)(int32_t)A * (int32_t)B;
        c->r[d] = (uint32_t)p;
        if (oe) SET_OV(p != (int32_t)p);
        return true;
    }
    case 75: c->r[d] = (uint32_t)(((int64_t)(int32_t)A * (int32_t)B) >> 32); return true;  // mulhw
    case 11: c->r[d] = (uint32_t)(((uint64_t)A * B) >> 32); return true;                    // mulhwu
    case 491: {  // divw
        const int32_t x = (int32_t)A, y = (int32_t)B;
        const int bad = (y == 0) || (x == INT32_MIN && y == -1);
        c->r[d] = bad ? (x < 0 ? 0xFFFFFFFFu : 0) : (uint32_t)(x / y);
        if (oe) SET_OV(bad);
        return true;
    }
    case 459:  // divwu
        c->r[d] = B ? A / B : 0;
        if (oe) SET_OV(B == 0);
        return true;
    default: return false;
    }
}

// Op 31's logic and shifts into rA, false for an extended opcode that isn't one.
bool logic(CPU* c, uint32_t xo, uint32_t s, uint32_t a, uint32_t b) {
    const uint32_t S = c->r[s], B = c->r[b];
    uint32_t r;
    switch (xo) {
    case 28: r = S & B; break;     // and
    case 60: r = S & ~B; break;    // andc
    case 444: r = S | B; break;    // or
    case 412: r = S | ~B; break;   // orc
    case 316: r = S ^ B; break;    // xor
    case 124: r = ~(S | B); break; // nor
    case 476: r = ~(S & B); break; // nand
    case 284: r = ~(S ^ B); break; // eqv
    case 24: r = (B & 0x20) ? 0 : (S << (B & 31)); break;   // slw
    case 536: r = (B & 0x20) ? 0 : (S >> (B & 31)); break;  // srw
    case 792: {  // sraw
        const int32_t x = (int32_t)S;
        const uint32_t n = B & 0x3F;
        if (n >= 32) {
            r = (uint32_t)(x >> 31);
            c->xer_ca = x < 0;
        } else {
            r = (uint32_t)(x >> n);
            c->xer_ca = (x < 0) && n && ((uint32_t)x << (32 - n)) != 0;
        }
        break;
    }
    case 824: {  // srawi, its shift in the rB field
        const int32_t x = (int32_t)S;
        if (b == 0) {
            r = S;
            c->xer_ca = 0;
        } else {
            r = (uint32_t)(x >> b);
            c->xer_ca = (x < 0) && ((uint32_t)x & ((1u << b) - 1)) != 0;
        }
        break;
    }
    case 26: r = S ? GCN_CLZ32(S) : 32; break;                // cntlzw
    case 954: r = (uint32_t)(int32_t)(int8_t)S; break;       // extsb
    case 922: r = (uint32_t)(int32_t)(int16_t)S; break;      // extsh
    default: return false;
    }
    c->r[a] = r;
    return true;
}

InterpStep op31(CPU* c, uint32_t i) {
    const uint32_t d = insn_d(i), a = insn_a(i), b = insn_b(i), xo = insn_xo(i);
    const bool rc = i & 1;
    if (arith(c, xo & 0x1FF, d, a, b, xo >> 9)) {
        if (rc) SET_CR0(c->r[d]);
        return INTERP_NEXT;
    }
    switch (xo) {
    case 0: c->cr[d >> 2] = CMPS((int32_t)c->r[a], (int32_t)c->r[b]); return INTERP_NEXT;  // cmp
    case 32: c->cr[d >> 2] = CMPU(c->r[a], c->r[b]); return INTERP_NEXT;                    // cmpl
    }
    if (!logic(c, xo, d, a, b)) return INTERP_UNIMPL;
    if (rc) SET_CR0(c->r[a]);
    return INTERP_NEXT;
}

// Op 19's condition register operations: mcrf and the eight logical ones on single bits.
InterpStep op19(CPU* c, uint32_t i) {
    const uint32_t d = insn_d(i), a = insn_a(i), b = insn_b(i);
    if (insn_xo(i) == 0) {  // mcrf
        c->cr[d >> 2] = c->cr[a >> 2];
        return INTERP_NEXT;
    }
    const uint32_t x = CRB(a), y = CRB(b);
    uint32_t v;
    switch (insn_xo(i)) {
    case 257: v = x & y; break;     // crand
    case 449: v = x | y; break;     // cror
    case 193: v = x ^ y; break;     // crxor
    case 225: v = !(x & y); break;  // crnand
    case 33: v = !(x | y); break;   // crnor
    case 289: v = !(x ^ y); break;  // creqv
    case 129: v = x & !y; break;    // crandc
    case 417: v = x | !y; break;    // crorc
    default: return INTERP_UNIMPL;
    }
    SET_CRB(d, v);
    return INTERP_NEXT;
}

}  // namespace

InterpStep interp_int(CPU* c, uint32_t i) {
    const uint32_t d = insn_d(i), a = insn_a(i), b = insn_b(i);
    const uint32_t simm = insn_simm(i), uimm = i & 0xFFFF;
    const bool rc = i & 1;
    switch (i >> 26) {
    case 14: c->r[d] = (a ? c->r[a] : 0) + simm; return INTERP_NEXT;          // addi
    case 15: c->r[d] = (a ? c->r[a] : 0) + (uimm << 16); return INTERP_NEXT;  // addis
    case 12:  // addic
    case 13: {  // addic.
        const uint64_t t = (uint64_t)c->r[a] + simm;
        c->r[d] = (uint32_t)t;
        c->xer_ca = (uint8_t)(t >> 32);
        if (i >> 26 == 13) SET_CR0(c->r[d]);
        return INTERP_NEXT;
    }
    case 8: {  // subfic
        const uint64_t t = (uint64_t)(uint32_t)~c->r[a] + simm + 1;
        c->r[d] = (uint32_t)t;
        c->xer_ca = (uint8_t)(t >> 32);
        return INTERP_NEXT;
    }
    case 7: c->r[d] = (uint32_t)((int64_t)(int32_t)c->r[a] * (int32_t)simm); return INTERP_NEXT;  // mulli
    case 11: c->cr[d >> 2] = CMPS((int32_t)c->r[a], (int32_t)simm); return INTERP_NEXT;          // cmpi
    case 10: c->cr[d >> 2] = CMPU(c->r[a], uimm); return INTERP_NEXT;                             // cmpli
    case 24: c->r[a] = c->r[d] | uimm; return INTERP_NEXT;          // ori, and nop as ori 0,0,0
    case 25: c->r[a] = c->r[d] | uimm << 16; return INTERP_NEXT;    // oris
    case 26: c->r[a] = c->r[d] ^ uimm; return INTERP_NEXT;          // xori
    case 27: c->r[a] = c->r[d] ^ uimm << 16; return INTERP_NEXT;    // xoris
    case 28: c->r[a] = c->r[d] & uimm; SET_CR0(c->r[a]); return INTERP_NEXT;        // andi.
    case 29: c->r[a] = c->r[d] & uimm << 16; SET_CR0(c->r[a]); return INTERP_NEXT;  // andis.
    case 21:  // rlwinm, its shift in the rB field
        c->r[a] = ROTL(c->r[d], b) & mask32((i >> 6) & 31, (i >> 1) & 31);
        if (rc) SET_CR0(c->r[a]);
        return INTERP_NEXT;
    case 23:  // rlwnm
        c->r[a] = ROTL(c->r[d], c->r[b] & 31) & mask32((i >> 6) & 31, (i >> 1) & 31);
        if (rc) SET_CR0(c->r[a]);
        return INTERP_NEXT;
    case 20: {  // rlwimi
        const uint32_t m = mask32((i >> 6) & 31, (i >> 1) & 31);
        c->r[a] = (ROTL(c->r[d], b) & m) | (c->r[a] & ~m);
        if (rc) SET_CR0(c->r[a]);
        return INTERP_NEXT;
    }
    case 31: return op31(c, i);
    case 19: return op19(c, i);
    default: return INTERP_UNIMPL;
    }
}
