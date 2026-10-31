// interp_step's dispatch. A word goes to each family in turn, the commonest first, and the first that
// takes it runs it. No two families take the same word, so the order only costs time.
#include "interp.h"

InterpStep interp_step(CPU* c, uint32_t pc, uint32_t insn) {
    InterpStep r = interp_int(c, insn);
    if (r != INTERP_UNIMPL) return r;
    r = interp_mem(c, insn);
    if (r != INTERP_UNIMPL) return r;
    r = interp_fp(c, insn);
    if (r != INTERP_UNIMPL) return r;
    r = interp_ps(c, insn);
    if (r != INTERP_UNIMPL) return r;
    return interp_sys(c, pc, insn);
}
