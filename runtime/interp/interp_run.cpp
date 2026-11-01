// The interpreter's frames. A recompiled body's entry guard calls interp_enter while its function's bit
// in g_code_changed_bits is set (runtime/recomp.h), and the function frame it runs does with each word
// what recomp.py's emit_body made that word's C do, call for call. A loose frame runs code that no
// function holds, and a branch that doesn't link stays inside it unless it lands on a function's start.
//
// Each word goes to its site first, then to interp_branch, then to interp_step. The frame reads the CPU
// again for every word, since an interrupt or a context switch comes back through a longjmp with the
// registers put back.
#include "interp.h"
#include "runtime.h"
#include <algorithm>
#include <unordered_set>

namespace {

const RecompSite* site_at(const InterpFrame* f, uint32_t pc) {
    const RecompSite* lo = g_recomp_sites + f->site_lo;
    const RecompSite* hi = g_recomp_sites + f->site_hi;
    const RecompSite* s = std::lower_bound(lo, hi, pc, [](const RecompSite& x, uint32_t a) { return x.addr < a; });
    return s != hi && s->addr == pc ? s : nullptr;
}

uint32_t sites_from(uint32_t addr) {
    return (uint32_t)(std::lower_bound(g_recomp_sites, g_recomp_sites + g_recomp_site_count, addr,
                                       [](const RecompSite& x, uint32_t a) { return x.addr < a; }) -
                      g_recomp_sites);
}

bool idle_at(uint32_t pc) { return std::binary_search(g_recomp_idle, g_recomp_idle + g_recomp_idle_count, pc); }

uint32_t special_kind(uint32_t target) {
    const RecompSpecial* end = g_recomp_specials + g_recomp_special_count;
    const RecompSpecial* s = std::lower_bound(g_recomp_specials, end, target,
                                              [](const RecompSpecial& x, uint32_t a) { return x.addr < a; });
    return s != end && s->addr == target ? s->kind : 0;
}

bool starts_function(uint32_t addr) {
    uint32_t n;
    return code_table_find(addr, &n) && g_recomp_ranges[n].start == addr;
}

// IRQ_CHECK from recomp.h, written out because C++ deprecates ++ on a volatile.
void irq_check(CPU* c) {
    const uint64_t v = g_vcount + 1;
    g_vcount = v;
    if (UNLIKELY(v >= g_vlimit)) irq_poll(c);
}

// --log=interp reports each of these once for its address.
std::unordered_set<uint32_t> g_blr_seen, g_site_seen;

bool first_report(std::unordered_set<uint32_t>& seen, uint32_t pc) {
    return g_log_enabled[LOG_INTERP] && seen.insert(pc).second;
}

}  // namespace

bool interp_frame_run(CPU* c, const InterpFrame* f, uint32_t pc, uint32_t again) {
    for (;;) {
        // Running off the end calls what comes next, as recomp.py's fallthrough does.
        if (!f->loose && pc == f->end) {
            if (starts_function(pc)) call_indirect(c, pc);
            else unimpl(c, pc, 0);
            return false;
        }
        // The baton serialises every guest thread that adds to a count, so a relaxed load and store
        // lose nothing and stay a plain one.
        f->ran->store(f->ran->load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        const uint32_t insn = LD32(pc);
        // A site's function runs while RAM holds the word it replaced, and RAM's word runs otherwise.
        if (const RecompSite* s = site_at(f, pc)) {
            if (insn == s->original) {
                if (s->fn(c)) return false;
                pc += 4;
                continue;
            }
            if (first_report(g_site_seen, pc))
                log_msg(LOG_INTERP, "site at %08X holds %08X, not the %08X its patch replaced, so that word runs", pc,
                        insn, s->original);
        }
        InterpBranch br;
        if (interp_branch(c, pc, insn, &br)) {
            if (br.link) {
                // LR is pc + 4 already, taken or not. A bl or bcl to the next word reads its own address
                // and runs on, as the hardware does and recomp.py's C does. In a function frame, a blrl or
                // bctrl to it is a call, as in the C, and so is a bl to a setjmp or context function.
                if (br.taken && br.target == pc + 4 &&
                    (f->loose || (br.kind <= INTERP_BC && !special_kind(br.target)))) {
                    pc += 4;
                    continue;
                }
                if (br.taken) {
                    const uint32_t kind = br.kind <= INTERP_BC ? special_kind(br.target) : 0;
                    if (kind == RECOMP_SPECIAL_SETJMP || kind == RECOMP_SPECIAL_CONTEXT) {
                        // The site that ran this frame takes its setjmp again itself, so the host stack
                        // doesn't grow each time a loop comes round.
                        if (pc == again) return true;
                        interp_setjmp_site(c, f, kind, br.target, pc);
                        return false;
                    }
                    call_indirect(c, br.target);
                }
                pc += 4;
                continue;
            }
            if (!br.taken) {
                pc += 4;
                continue;
            }
            if (br.kind == INTERP_BLR) {
                if (f->longjmp) {
                    hle_longjmp_resume(c, f->jb);
                    return false;
                }
                if (br.target != f->entry_lr && first_report(g_blr_seen, pc))
                    log_msg(LOG_INTERP, "blr at %08X returns to %08X, and its frame was entered with LR %08X", pc,
                            br.target, f->entry_lr);
                return false;
            }
            // Out of the function's range, a branch that doesn't link is a call and then a return. A loose
            // frame has no range, so its tail call is a branch onto a function's start.
            if (f->loose ? starts_function(br.target) : (br.target < f->start || br.target >= f->end)) {
                call_indirect(c, br.target);
                return false;
            }
            if (br.kind != INTERP_BCTR && br.target <= pc) {
                if (idle_at(pc)) IDLE_CHECK();
                else irq_check(c);
            }
            pc = br.target;
            continue;
        }
        switch (interp_step(c, pc, insn)) {
        case INTERP_RFI: return false;
        case INTERP_UNIMPL: unimpl(c, pc, insn); break;
        case INTERP_NEXT:
            if ((insn >> 26) == 31 && insn_xo(insn) == 146) irq_check(c);  // mtmsr
            break;
        }
        pc += 4;
    }
}

extern "C" void interp_enter(CPU* c, uint32_t entry) {
    uint32_t n;
    if (!code_table_find(entry, &n)) fatal("interp_enter: no recompiled function holds %08X", entry);
    const RecompRange& r = g_recomp_ranges[n];
    InterpFrame f{};
    f.start = r.start;
    f.end = r.end;
    f.longjmp = (r.flags & RECOMP_RANGE_LONGJMP) != 0;
    f.entry_lr = c->lr;
    f.jb = c->r[3];
    f.site_lo = sites_from(r.start);
    f.site_hi = sites_from(r.end);
    f.ran = code_table_counter(n);
    f.host = (uintptr_t)GCN_FRAME_ADDRESS();
    interp_frame_run(c, &f, entry);
    // The guard's RET() would return to longjmp's caller, where the native longjmp never returns.
    if (f.longjmp)
        fatal("the interpreted longjmp %s at %08X returned, and its guard would return to longjmp's caller",
              func_name(r.start), r.start);
}

void interp_run_loose(CPU* c, uint32_t pc) {
    ENTER(pc);
    InterpFrame f{};
    f.loose = true;
    f.entry_lr = c->lr;
    f.site_hi = g_recomp_site_count;
    f.ran = code_table_counter(g_recomp_range_count);
    f.host = (uintptr_t)GCN_FRAME_ADDRESS();
    interp_frame_run(c, &f, pc);
    EXIT();
}

extern "C" void interp_site_unusable(CPU* c, uint32_t addr) {
    (void)c;
    fatal("the patch at %08X has a goto, which works only inside its function", addr);
}
