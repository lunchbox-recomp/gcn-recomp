// System calls, traps, rfi, the moves to and from the special registers and the condition register,
// the segment registers, the TLB and the barriers, each as ppc.translate writes it. LR, CTR, XER and
// the GQRs are read and written in place, and every other SPR goes through hle_mfspr and hle_mtspr.
// The IRQ_CHECK native code has after mtmsr is the frame's, since it's recomp.py's and not translate's.
#include "interp.h"

namespace {

uint32_t mfspr(CPU* c, uint32_t spr) {
    switch (spr) {
    case 8: return c->lr;
    case 9: return c->ctr;
    case 1: return GET_XER();
    default: return spr - 912 < 8 ? c->gqr[spr - 912] : hle_mfspr(c, spr);
    }
}

void mtspr(CPU* c, uint32_t spr, uint32_t v) {
    switch (spr) {
    case 8: c->lr = v; return;
    case 9: c->ctr = v; return;
    case 1: SET_XER(v); return;
    default:
        if (spr - 912 < 8) c->gqr[spr - 912] = v;
        else hle_mtspr(c, spr, v);
    }
}

InterpStep op31(CPU* c, uint32_t pc, uint32_t i) {
    const uint32_t d = insn_d(i), a = insn_a(i), b = insn_b(i);
    const uint32_t spr = a | b << 5;
    switch (insn_xo(i)) {
    case 4: hle_trap(c, pc, i); return INTERP_NEXT;  // tw
    case 19: c->r[d] = GET_CR(); return INTERP_NEXT;  // mfcr
    case 144: {  // mtcrf
        const uint32_t crm = (i >> 12) & 0xFF, s = c->r[d];
        for (int f = 0; f < 8; f++)
            if (crm & (0x80 >> f)) c->cr[f] = (s >> (28 - 4 * f)) & 0xF;
        return INTERP_NEXT;
    }
    case 512:  // mcrxr
        c->cr[d >> 2] = (uint8_t)((c->xer_so << 3) | (c->xer_ov << 2) | (c->xer_ca << 1));
        c->xer_so = c->xer_ov = c->xer_ca = 0;
        return INTERP_NEXT;
    case 83: c->r[d] = c->msr; return INTERP_NEXT;      // mfmsr
    case 146: hle_mtmsr(c, c->r[d]); return INTERP_NEXT;  // mtmsr
    case 339: case 371: c->r[d] = mfspr(c, spr); return INTERP_NEXT;  // mfspr, mftb
    case 467: mtspr(c, spr, c->r[d]); return INTERP_NEXT;
    case 595: c->r[d] = hle_mfsr(c, (i >> 16) & 15); return INTERP_NEXT;    // mfsr
    case 659: c->r[d] = hle_mfsr(c, c->r[b] >> 28); return INTERP_NEXT;     // mfsrin
    case 210: hle_mtsr(c, (i >> 16) & 15, c->r[d]); return INTERP_NEXT;     // mtsr
    case 242: hle_mtsr(c, c->r[b] >> 28, c->r[d]); return INTERP_NEXT;      // mtsrin
    case 306: hle_tlbie(c, c->r[b]); return INTERP_NEXT;
    case 598: case 854: case 566: return INTERP_NEXT;  // sync, eieio, tlbsync
    default: return INTERP_UNIMPL;
    }
}

}  // namespace

InterpStep interp_sys(CPU* c, uint32_t pc, uint32_t i) {
    switch (i >> 26) {
    case 17: hle_sc(c, pc); return INTERP_NEXT;
    case 3: hle_trap(c, pc, i); return INTERP_NEXT;  // twi
    case 19:
        switch (insn_xo(i)) {
        case 150: return INTERP_NEXT;  // isync
        case 50: hle_rfi(c, pc); return INTERP_RFI;
        default: return INTERP_UNIMPL;
        }
    case 31: return op31(c, pc, i);
    default: return INTERP_UNIMPL;
    }
}
