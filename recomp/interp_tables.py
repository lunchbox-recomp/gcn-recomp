"""recomp_interp.c, the tables the interpreter reads (runtime/recomp.h). recomp.py calls source()
once it knows its functions, and the tests call it on their own."""
import bisect
import re
import sys

import ppc

SPECIAL_KINDS = {'setjmp': 'RECOMP_SPECIAL_SETJMP', 'longjmp': 'RECOMP_SPECIAL_LONGJMP'}


def fnv1a64(data):
    """64-bit FNV-1a."""
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def _flags(f, longjmps):
    flags = [name for name, on in (('RECOMP_RANGE_HLE', f.hle), ('RECOMP_RANGE_LONGJMP', f.addr in longjmps),
                                   ('RECOMP_RANGE_NO_GUARD', not f.guarded)) if on]
    return ' | '.join(flags) or '0'


def _sites(dol, funcs, patches, steps):
    """(addr, original word, kind, C text) of each patch and step, sorted. The interpreter calls
    each as a function of its own, and only those inside a function count, since native code has
    no others."""
    starts = [f.addr for f in funcs]
    sites = []
    for a in sorted(set(patches) | set(steps)):
        k = bisect.bisect_right(starts, a) - 1
        if k < 0 or a >= funcs[k].end:
            continue
        word = dol.read32(a)
        if a in patches:
            code = patches[a]
            if re.search(r'\bgoto\b', code):
                print(f'warning: patches.txt {a:08X} has a goto, which works only inside its function, '
                      'so an interpreted run stops there', file=sys.stderr)
                code = f'interp_site_unusable(c, 0x{a:08X}u);'
            sites.append((a, word, 'PATCHED', code))
        else:
            sites.append((a, word, 'STEP', ppc.translate_step(a, word, *steps[a])))
    return sites


def _text(dol, loaded):
    """(start, bytes) of each text section, sorted, leaving out any that overlaps loaded code."""
    return [(a, dol.read_bytes(a, sz)) for a, sz, _ in sorted(dol.text_sections())
            if not any(lo < a + sz and a < hi for lo, hi in loaded)]


def source(dol, funcs, patches, steps, idle_loops, special_calls, loaded):
    """The text of recomp_interp.c. funcs are recomp.py's Func objects sorted by start, each with
    hle and guarded set, and patches and steps are its tables by address. special_calls maps a
    called function's start to its name, and loaded holds the (start, end) of code that isn't in
    RAM at its own address."""
    longjmps = {a for a, n in special_calls.items() if n == 'longjmp'}
    sites = _sites(dol, funcs, patches, steps)
    text = _text(dol, loaded)
    out = ['/* The tables the interpreter reads (runtime/recomp.h), from recomp.py. */',
           '#include "recomp_funcs.h"', '#include <setjmp.h>', '', 'const RecompRange g_recomp_ranges[] = {']
    out += [f'\t{{0x{f.addr:08X}u, 0x{f.end:08X}u, {_flags(f, longjmps)}}},' for f in funcs]
    out += ['\t{0, 0, 0}};', f'const uint32_t g_recomp_range_count = {len(funcs)};',
            f'uint8_t g_code_changed_bits[{max(1, (len(funcs) + 7) // 8)}] = {{0}};', '',
            '/* A site returns 1 when its patch returns from the guest function. */',
            '#undef RET', '#define RET() return 1']
    for a, word, kind, code in sites:
        out += [f'static int site_{a:08X}(CPU* c) {{', f'\t/* {a:08X} {word:08X} {kind} */ {code}', '\treturn 0;', '}']
    out += ['const RecompSite g_recomp_sites[] = {']
    out += [f'\t{{0x{a:08X}u, 0x{word:08X}u, site_{a:08X}}},' for a, word, _, _ in sites]
    out += ['\t{0, 0, 0}};', f'const uint32_t g_recomp_site_count = {len(sites)};', '',
            'const uint32_t g_recomp_idle[] = {']
    out += [f'\t0x{a:08X}u,' for a in sorted(idle_loops)]
    out += ['\t0};', f'const uint32_t g_recomp_idle_count = {len(idle_loops)};',
            'const RecompSpecial g_recomp_specials[] = {']
    out += [f'\t{{0x{a:08X}u, {SPECIAL_KINDS.get(n, "RECOMP_SPECIAL_CONTEXT")}}},' for a, n in sorted(special_calls.items())]
    out += ['\t{0, 0}};', f'const uint32_t g_recomp_special_count = {len(special_calls)};', '',
            'const RecompText g_recomp_text[] = {']
    out += [f'\t{{0x{a:08X}u, 0x{len(b):X}u}},' for a, b in text]
    out += ['\t{0, 0}};', f'const uint32_t g_recomp_text_count = {len(text)};',
            f'const uint64_t g_recomp_text_fnv = 0x{fnv1a64(b"".join(b for _, b in text)):016X}ull;']
    return '\n'.join(out) + '\n'
