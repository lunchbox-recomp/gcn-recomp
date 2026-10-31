// The PowerPC interpreter's single steps. It runs a guest function from RAM when that function's
// code changed at run time, a word at a time, and each word does exactly what ppc.translate's C for
// it does (recomp/ppc.py). tools/test_interp.cpp holds it to that text, case by case.
#pragma once
#include "recomp.h"

enum InterpStep { INTERP_NEXT, INTERP_RFI, INTERP_UNIMPL };

// insn at pc, run as ppc.translate(pc, insn)'s text runs it. INTERP_RFI says hle_rfi ran, after which
// the recompiled function returns. INTERP_UNIMPL says translate refuses the word, and nothing changed.
InterpStep interp_step(CPU* c, uint32_t pc, uint32_t insn);

enum InterpBranchKind : uint8_t { INTERP_B, INTERP_BC, INTERP_BLR, INTERP_BCTR };
struct InterpBranch {
    InterpBranchKind kind;
    bool link, taken;
    uint32_t target;
};

// insn at pc as ppc.decode_branch reads it, or false with nothing changed for a word it doesn't
// return a branch for. The CTR decrement is applied, and a BLR or BCTR target is LR or CTR as it was
// before the branch. A branch that links sets LR to pc + 4 whether it's taken or not.
bool interp_branch(CPU* c, uint32_t pc, uint32_t insn, InterpBranch* out);

// The families interp_step tries in turn. Each returns INTERP_UNIMPL, with nothing changed, for a word
// that isn't one of its own.
InterpStep interp_int(CPU* c, uint32_t insn);
InterpStep interp_mem(CPU* c, uint32_t insn);
InterpStep interp_sys(CPU* c, uint32_t pc, uint32_t insn);

// A word's fields.
inline uint32_t insn_d(uint32_t i) { return (i >> 21) & 31; }
inline uint32_t insn_a(uint32_t i) { return (i >> 16) & 31; }
inline uint32_t insn_b(uint32_t i) { return (i >> 11) & 31; }
inline uint32_t insn_xo(uint32_t i) { return (i >> 1) & 0x3FF; }
inline uint32_t insn_simm(uint32_t i) { return (uint32_t)(int32_t)(int16_t)i; }
