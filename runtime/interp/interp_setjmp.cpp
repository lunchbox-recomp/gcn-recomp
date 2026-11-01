// A setjmp or context-save call site in interpreted code. Native code takes the host setjmp in the
// function's own C frame, and a longjmp comes back to that frame while the function runs.
//
// This takes it in a frame of its own and runs the rest of the guest function from there, since
// nothing here changes after the setjmp. The frame loop's pc changes with every word, so a setjmp
// taken in the loop couldn't be trusted once a longjmp came back.
//
// When the guest function comes back round to the same site, its frame hands the site back here. The
// setjmp is then taken again in this C frame, as native code takes it again in the function's.
//
// A loop through two sites still nests a frame for each, which has to stay live for its jmp_buf, so
// a site past 4 MiB of host stack under its function's frame stops the game.
#include "interp.h"
#include "runtime.h"

namespace {
constexpr uintptr_t kNestLimit = 4u << 20;
}

void interp_setjmp_site(CPU* c, const InterpFrame* f, uint32_t kind, uint32_t target, uint32_t site_pc) {
    if (f->host - (uintptr_t)__builtin_frame_address(0) > kNestLimit)
        fatal("%s's setjmp and context sites nested past 4 MiB of the host stack, at the site at %08X",
              f->loose ? "a loose frame" : func_name(f->start), site_pc);
    do {
        jmp_buf* jb = kind == RECOMP_SPECIAL_SETJMP ? hle_setjmp_jmpbuf(c) : hle_context_jmpbuf(c);
        if (GCN_SETJMP(*jb) == 0) call_indirect(c, target);
    } while (interp_frame_run(c, f, site_pc + 4, site_pc));
}
