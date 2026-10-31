// A setjmp or context-save call site in interpreted code. Native code takes the host setjmp in the
// function's own C frame, and a longjmp comes back to that frame while the function runs.
//
// This takes it in a frame of its own and runs the rest of the guest function from there, since
// nothing here changes after the setjmp. The frame loop's pc changes with every word, so a setjmp
// taken in the loop couldn't be trusted once a longjmp came back.
#include "interp.h"

void interp_setjmp_site(CPU* c, const InterpFrame* f, uint32_t kind, uint32_t target, uint32_t resume_pc) {
    jmp_buf* jb = kind == RECOMP_SPECIAL_SETJMP ? hle_setjmp_jmpbuf(c) : hle_context_jmpbuf(c);
    if (GCN_SETJMP(*jb) == 0) call_indirect(c, target);
    interp_frame_run(c, f, resume_pc);
}
