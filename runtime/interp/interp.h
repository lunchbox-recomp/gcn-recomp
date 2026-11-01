// The PowerPC interpreter. It runs a guest function from RAM when that function's code changed at run
// time, a word at a time, and each word does exactly what ppc.translate's C for it does
// (recomp/ppc.py). tools/test_interp.cpp holds the single steps to that text, case by case, and
// tools/test_interp_run.cpp holds the frames to what recomp.py's C does with them.
#pragma once
#include "recomp.h"
#include <atomic>
#include <string>

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
InterpStep interp_fp(CPU* c, uint32_t insn);
InterpStep interp_ps(CPU* c, uint32_t insn);

// A word's fields.
inline uint32_t insn_d(uint32_t i) { return (i >> 21) & 31; }
inline uint32_t insn_a(uint32_t i) { return (i >> 16) & 31; }
inline uint32_t insn_b(uint32_t i) { return (i >> 11) & 31; }
inline uint32_t insn_xo(uint32_t i) { return (i >> 1) & 0x3FF; }
inline uint32_t insn_simm(uint32_t i) { return (uint32_t)(int32_t)(int16_t)i; }
inline uint32_t insn_c(uint32_t i) { return (i >> 6) & 31; }
inline uint32_t insn_xo5(uint32_t i) { return (i >> 1) & 31; }

// An FP result's FPSCR[FPRF], kept for FPSCR_READ to work out, as translate's FPRF_S and FPRF_D
// record it. single says the result was rounded to single.
inline void interp_fprf(CPU* c, double v, bool single) {
    c->fprf_v = v;
    c->fprf_pending = single ? 2 : 1;
}

// An FP form's record bit, which copies FPSCR's top four bits to CR1 as translate's cr1 does.
inline void interp_cr1(CPU* c, uint32_t i) {
    if (i & 1) c->cr[1] = (uint8_t)(c->fpscr >> 28);
}

// ---- the frames (interp_run.cpp, interp_setjmp.cpp) ----
// An interpreted guest call is one frame. A function frame runs a recompiled function from the entry
// its guard handed over (interp_enter, in recomp.h), and a loose frame runs code that no function
// holds, so it has no range.
struct InterpFrame {
    uint32_t start, end;        // the function's range, which a loose frame hasn't got
    bool loose, longjmp;        // longjmp says the function is RECOMP_RANGE_LONGJMP
    uint32_t entry_lr;          // LR as the frame was entered
    uint32_t jb;                // r3 as a longjmp frame was entered, the guest jmp_buf
    uint32_t site_lo, site_hi;  // the rows of g_recomp_sites it can reach
    // Where its interpreted instructions are counted.
    std::atomic<uint64_t>* ran;
    // The host stack frame of the interp_enter or interp_run_loose that made it, which every frame
    // running its code is under.
    uintptr_t host;
};

// Runs f's code from pc, and returns false when the guest returns from it. It returns true instead when
// it reaches the setjmp or context site at again, which 0 never is, with LR set for it.
bool interp_frame_run(CPU* c, const InterpFrame* f, uint32_t pc, uint32_t again = 0);
// A loose frame at pc, with its own ENTER and EXIT, for code in RAM that no table knows.
void interp_run_loose(CPU* c, uint32_t pc);
// The bl at site_pc to a setjmp or context row's function, with LR set already. It takes the setjmp and
// calls target when that returns 0, then runs the rest of f's code from the next word. So a longjmp
// finds the setjmp's frame for as long as the guest function hasn't returned. The code coming back
// round to site_pc takes the setjmp again in the same C frame.
void interp_setjmp_site(CPU* c, const InterpFrame* f, uint32_t kind, uint32_t target, uint32_t site_pc);

// ---- which functions run interpreted (code_table.cpp) ----
// A function's bit in g_code_changed_bits is set while any of its marks is. GCN_INTERPRET sets
// CODE_FORCED, and CODE_CHANGED is for code that changed at run time.
enum CodeMark : uint8_t { CODE_FORCED = 1, CODE_CHANGED = 2 };
// The one place the bits change, with n the function's index in g_recomp_ranges.
void code_table_mark(uint32_t n, CodeMark mark, bool on);
// The index of the range holding addr, or false when no recompiled function holds it.
bool code_table_find(uint32_t addr, uint32_t* n);
// Where function n's interpreted instructions are counted. Loose frames count at g_recomp_range_count.
// A frame adds to a count with a relaxed load and store. Only the guest thread holding the baton runs
// frames, so every adder is serialised and none is lost, and the report at exit reads them from
// another thread.
std::atomic<uint64_t>* code_table_counter(uint32_t n);
// Forces what GCN_INTERPRET's value names, "all" or a file of starts, and returns the line saying how
// many functions that was. Each line of the file that isn't a recompiled function's start is printed,
// and then the game stops.
std::string code_table_force(const char* spec);
// GCN_INTERPRET_STATS's report, with the 30 functions that ran the most interpreted instructions and
// the total.
std::string code_table_stats();
// Reads GCN_INTERPRET and GCN_INTERPRET_STATS, once the DOL is in RAM.
void code_table_boot();
