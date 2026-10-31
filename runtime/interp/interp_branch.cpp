// Branches as ppc.decode_branch reads them. Where a branch goes is the frame's to decide, since
// recomp.py turns the same branch into a goto, a call or a return depending on where it lands.
#include "interp.h"

namespace {

// ppc.bo_cond. CTR is decremented and tested first when BO says to, then CR bit BI when BO tests one.
bool taken(CPU* c, uint32_t bo, uint32_t bi) {
    bool ok = true;
    if (!(bo & 0x04)) {
        c->ctr--;
        ok = (bo & 0x02) ? c->ctr == 0 : c->ctr != 0;
    }
    if (!(bo & 0x10)) ok = ok && CRB(bi) == ((bo >> 3) & 1);
    return ok;
}

}  // namespace

bool interp_branch(CPU* c, uint32_t pc, uint32_t i, InterpBranch* out) {
    const uint32_t bo = insn_d(i), bi = insn_a(i);
    const bool link = i & 1;
    switch (i >> 26) {
    case 18: {  // b, its 26-bit displacement sign-extended
        const uint32_t li = (uint32_t)((int32_t)(i << 6) >> 6) & ~3u;
        *out = {INTERP_B, link, true, (i & 2) ? li : pc + li};
        break;
    }
    case 16: {  // bc
        const uint32_t bd = (uint32_t)(int32_t)(int16_t)(i & 0xFFFC);
        const uint32_t target = (i & 2) ? bd : pc + bd;
        *out = {INTERP_BC, link, taken(c, bo, bi), target};
        break;
    }
    case 19:
        if (insn_xo(i) == 16) {  // bclr
            const uint32_t target = c->lr;
            *out = {INTERP_BLR, link, taken(c, bo, bi), target};
        } else if (insn_xo(i) == 528 && (bo & 0x04)) {  // bcctr, which decode_branch refuses with a CTR decrement
            *out = {INTERP_BCTR, link, taken(c, bo, bi), c->ctr};
        } else {
            return false;
        }
        break;
    default: return false;
    }
    if (link) c->lr = pc + 4;
    return true;
}
