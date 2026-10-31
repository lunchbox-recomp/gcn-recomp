// The interpreter's way in. Every recompiled body's entry guard calls interp_enter while its
// function's bit in g_code_changed_bits is set (runtime/recomp.h). This build has no
// interpreter yet, so a guard that fires stops the game.
#include "interp.h"
#include "runtime.h"

extern "C" void interp_enter(CPU* c, uint32_t entry) {
    (void)c;
    fatal("the code at %08X changed, and this build has no interpreter yet", entry);
}

void interp_run_loose(CPU* c, uint32_t pc) {
    (void)c;
    fatal("the code at %08X is in no function, and this build has no interpreter yet", pc);
}

extern "C" void interp_site_unusable(CPU* c, uint32_t addr) {
    (void)c;
    fatal("the patch at %08X has a goto, which works only inside its function", addr);
}
