#!/usr/bin/env python3
"""The interpreter's test cases, written from ppc.translate and ppc.decode_branch themselves, so
runtime/interp/ is held to what the recompiled C does. tools/test_interp.cpp runs them.

    ppc_cases.py OUT --seed N [--per-form K]

It sweeps every primary opcode and every extended opcode of ops 4, 19, 31, 59 and 63. A form is a
word's opcode bits, placed as the Gekko's instruction formats place them, and its operand fields
get edge values and then random ones.

Each form translate accepts gets at least K words, each a C function holding translate's text. Each
branch form gets K words, each a function doing what decode_branch says, and the words neither
accepts are listed as refused. A memory form's case names its base and index registers, so the test
can point them into its RAM window.
"""
import argparse
import collections
import os
import random
import sys

import ppc

# Which interpreter file runs a word, and the files written for them.
FAMILIES = ('int', 'mem', 'sys', 'branch', 'fp', 'ps', 'fpmem')
FILES = ('interp_cases.h', 'cases_index.c') + tuple(f'cases_{f}.c' for f in FAMILIES)

NO_REG = 0xFF
# The RAM test_interp compares, from guest 0x80000000. That's physical 0 too, where a word with rA 0
# addresses its displacement.
WINDOW = 0x10000

# Op 63's and op 4's A-form extended opcodes (bits 1-5), whose bits 6-10 are the C register, and op
# 4's indexed quantized loads and stores (bits 1-6), whose bits 7-10 are W and I. All of op 59 is
# A-form, and every other extended opcode of ops 4, 19, 31 and 63 is bits 1-10. These only group the
# words, and what each one means still comes from translate.
A_FORMS = {63: frozenset({18, 20, 21, 22, 23, 25, 26, 28, 29, 30, 31}),
           4: frozenset({10, 11, 12, 13, 14, 15, 18, 20, 21, 23, 24, 25, 26, 28, 29, 30, 31})}
PSQ_X = frozenset({6, 7, 38, 39})

# Every SPR translate reads or writes itself (XER, LR, CTR and the GQRs), every one runtime/cpu.cpp's
# hle_mfspr and hle_mtspr handle on their own, and both ends of each range it keeps once for the
# whole console.
SPRS = (1, 8, 9, 22, 25, 26, 27, 268, 269, 284, 285, 528, 543, 912, 913, 914, 915, 916, 917, 918,
        919, 920, 921, 922, 923, 936, 958, 1008, 1009, 1013, 1019)
# BO with each test on and off, CTR's either way.
BO_EDGES = (0, 2, 4, 8, 10, 12, 16, 18, 20)
PC_EDGES = (0, 0x7FFFFFFC, 0x80000000, 0xFFFFFFFC)

D, A, B, C, M = 21, 16, 11, 6, 1  # where each five-bit field starts

WIDTH = {'LD8': 1, 'ST8': 1, 'LD16': 2, 'LDS16': 2, 'ST16': 2, 'LD16BR': 2, 'ST16BR': 2,
         'LD32': 4, 'ST32': 4, 'LD32BR': 4, 'ST32BR': 4}
D_SPAN = {op: WIDTH[mac] for op, (mac, _) in list(ppc.INT_LOADS.items()) + list(ppc.INT_STORES.items())}
D_SPAN.update({48: 4, 49: 4, 50: 8, 51: 8, 52: 4, 53: 4, 54: 8, 55: 8})
PSQ_D = (56, 57, 60, 61)
X_SPAN = {xo: WIDTH[mac] for xo, (mac, _) in list(ppc.X_LOADS.items()) + list(ppc.X_STORES.items())}
X_SPAN.update({150: 4, 470: 32, 1014: 32, 54: 1, 86: 1, 246: 1, 278: 1, 982: 1,
               535: 4, 567: 4, 599: 8, 631: 8, 663: 4, 695: 4, 727: 8, 759: 8, 983: 4})
FP_X = frozenset({535, 567, 599, 631, 663, 695, 727, 759, 983})
SYS_X = frozenset({4, 19, 83, 144, 146, 210, 242, 306, 339, 371, 467, 512, 566, 595, 598, 659, 854})

Form = collections.namedtuple('Form', 'mask value name family')
Step = collections.namedtuple('Step', 'insn pc form text mem')
BranchCase = collections.namedtuple('BranchCase', 'insn pc form branch')
Refused = collections.namedtuple('Refused', 'insn pc')
Cases = collections.namedtuple('Cases', 'seed per_form forms steps branches refused')


class Rng:
    """Draws from getrandbits alone, which gives the same numbers on every Python 3."""

    def __init__(self, seed):
        self.r = random.Random(seed)

    def bits(self, n):
        return self.r.getrandbits(n)

    def below(self, n):
        k = max(1, (n - 1).bit_length())
        while True:
            v = self.r.getrandbits(k)
            if v < n:
                return v


def form_of(i):
    """(mask, value) of the opcode bits that pick the instruction i is."""
    op = i >> 26
    if op == 4 and (i >> 1) & 0x3F in PSQ_X:
        mask = 0xFC00007E
    elif op == 59 or (op in A_FORMS and (i >> 1) & 0x1F in A_FORMS[op]):
        mask = 0xFC00003F
    elif op in (4, 19, 31, 63):
        mask = 0xFC0007FF
    elif op in (16, 18):  # AA and LK
        mask = 0xFC000003
    elif op in (20, 21, 23):  # Rc
        mask = 0xFC000001
    else:
        mask = 0xFC000000
    return mask, i & mask


def patterns():
    """Every form, once each, in opcode order."""
    seen = set()
    out = []
    for op in range(64):
        for low in range(2048):
            key = form_of(op << 26 | low)
            if key not in seen:
                seen.add(key)
                out.append(key)
    return out


def form_name(mask, value):
    op = value >> 26
    if mask == 0xFC00007E:
        return f'op {op} xo {(value >> 1) & 0x3F}'
    if mask == 0xFC00003F:
        return f'op {op} xo {(value >> 1) & 0x1F} rc {value & 1}'
    if mask == 0xFC0007FF:
        return f'op {op} xo {(value >> 1) & 0x3FF} {"lk" if op == 19 else "rc"} {value & 1}'
    if mask == 0xFC000003:
        return f'op {op} aa {(value >> 1) & 1} lk {value & 1}'
    if mask == 0xFC000001:
        return f'op {op} rc {value & 1}'
    return f'op {op}'


def family(i):
    op, xo = i >> 26, (i >> 1) & 0x3FF
    if op in (16, 18) or (op == 19 and xo in (16, 528)):
        return 'branch'
    if 48 <= op <= 57 or op in (60, 61) or (op == 31 and xo in FP_X):
        return 'fpmem'
    if op == 4:
        return 'fpmem' if (i >> 1) & 0x3F in PSQ_X else 'ps'
    if op in (59, 63):
        return 'fp'
    if 32 <= op <= 47 or (op == 31 and xo in X_SPAN) or (op == 31 and xo in (597, 725)):
        return 'mem'
    if op in (3, 17) or (op == 19 and xo in (50, 150)) or (op == 31 and xo in SYS_X):
        return 'sys'
    return 'int'


def memory(i):
    """(base, index, disp, span) for a word that reads or writes memory, or None. It reaches span
    bytes from rA|0 + rB, or from rA|0 + disp when index is NO_REG."""
    op, d, a, b = i >> 26, (i >> 21) & 31, (i >> 16) & 31, (i >> 11) & 31
    xo = (i >> 1) & 0x3FF
    if op in (46, 47):  # lmw, stmw
        return a, NO_REG, ppc.sx16(i), 4 * (32 - d)
    if op in PSQ_D:
        return a, NO_REG, ppc.sx12(i), 8
    if op in D_SPAN:
        return a, NO_REG, ppc.sx16(i), D_SPAN[op]
    if op == 31 and xo in (597, 725):  # lswi, stswi
        return a, NO_REG, 0, b or 32
    if op == 31 and xo in X_SPAN:
        return a, b, 0, X_SPAN[xo]
    if op == 4 and (i >> 1) & 0x3F in PSQ_X:
        return a, b, 0, 8
    if op == 4 and xo == 1014:  # dcbz_l
        return a, b, 0, 32
    return None


def fit_memory(i):
    """A load or store with rA 0 addresses its displacement alone, so the displacement's sign bit
    is cleared to keep it in the window."""
    op, a = i >> 26, (i >> 16) & 31
    if a == 0 and op in PSQ_D:
        return i & ~0x800
    if a == 0 and (op in D_SPAN or op in (46, 47)):
        return i & ~0x8000
    return i


def put(i, shift, v):
    return (i & ~(31 << shift)) | ((v & 31) << shift)


def get(i, shift):
    return (i >> shift) & 31


def edge_words(mask, value, rng):
    """The form's words with edge operands: every field 0 and every bit 1, rA 0, registers equal,
    shifts of 0 and 31, MB past ME, the 16-bit immediate's ends, each SPR, CRM and BO, and the
    branch displacements' ends."""
    free = ~mask & 0xFFFFFFFF

    def rand():
        return value | (rng.bits(32) & free)

    def has(*shifts):
        return all(mask & (31 << s) == 0 for s in shifts)

    out = [value, value | free]
    if has(A):
        out.append(put(rand(), A, 0))
    for x, y in ((D, A), (D, B), (A, B)):
        if has(x, y):
            w = rand()
            out.append(put(w, x, get(w, y)))
    if has(B):
        out += [put(rand(), B, 0), put(rand(), B, 31)]
    if has(C, M):
        me = rng.below(31)
        out.append(put(put(rand(), C, me + 1 + rng.below(31 - me)), M, me))
        me = rng.below(31)
        out.append(put(put(rand(), C, me + 1), M, me))
    if mask & 0xFFFF == 0:
        out += [(rand() & ~0xFFFF) | v for v in (0, 1, 0x7FFF, 0x8000, 0xFFFF)]
    op, xo = value >> 26, (value >> 1) & 0x3FF
    x_form = mask == 0xFC0007FF
    if x_form and op == 31 and xo in (339, 371, 467):
        out += [put(put(rand(), A, s & 31), B, s >> 5) for s in SPRS]
    if x_form and op == 31 and xo == 144:
        out += [(rand() & ~0xFF000) | crm << 12 for crm in (0, 0xFF, 0x80, 0x01, 0x5A)]
    if op == 16 or (x_form and op == 19 and xo in (16, 528)):
        out += [put(rand(), D, bo) for bo in BO_EDGES]
    if op == 16:
        out += [(rand() & ~0xFFFC) | bd for bd in (0, 0x7FFC, 0x8000, 0xFFFC)]
    if op == 18:
        out += [(rand() & ~0x03FFFFFC) | li for li in (0, 0x01FFFFFC, 0x02000000, 0x03FFFFFC)]
    return out


def pick_pc(i, rng):
    if i >> 26 in (16, 18, 19) and rng.below(4) == 0:
        return PC_EDGES[rng.below(len(PC_EDGES))]
    return 0x80000000 | rng.bits(23) << 2


def classify(pc, i):
    """(the Branch decode_branch returns, translate's text), either None where it refuses."""
    try:
        br = ppc.decode_branch(pc, i)
    except ppc.Unimpl:
        br = None
    try:
        text = ppc.translate(pc, i)
    except ppc.Unimpl:
        text = None
    if br is not None and text is not None:
        sys.exit(f'error: {i:08X} is both a branch and an instruction to translate')
    return br, text


def sweep(mask, value, k, rng):
    """The accepted and refused words of one form, as (word, pc, branch, text). Its edge words come
    first, then random ones until K of those are accepted. A form with none accepted in 32 tries is
    refused."""
    free = ~mask & 0xFFFFFFFF
    seen = set()
    acc, ref = [], []

    def take(i):
        i = fit_memory(i)
        if i in seen:
            return False
        seen.add(i)
        pc = pick_pc(i, rng)
        br, text = classify(pc, i)
        ok = br is not None or text is not None  # an mtcrf with no fields is ''
        (acc if ok else ref).append((i, pc, br, text))
        return ok

    for i in edge_words(mask, value, rng):
        take(i)
    n = tries = 0
    while n < k and tries < 64 * k + 32 and (acc or tries < 32):
        tries += 1
        n += take(value | (rng.bits(32) & free))
    return acc, ref


def generate(seed, per_form):
    rng = Rng(seed)
    forms, steps, branches, refused = [], [], [], []
    for mask, value in patterns():
        acc, ref = sweep(mask, value, per_form, rng)
        fam = FAMILIES.index(family(value))
        if not acc:
            ref = ref[:1] + ref[-1:]
        refused += [Refused(i, pc) for i, pc, _, _ in ref]
        if not acc:
            continue
        n = len(forms)
        forms.append(Form(mask, value, form_name(mask, value), fam))
        for i, pc, br, text in acc:
            if br is not None:
                branches.append(BranchCase(i, pc, n, br))
            else:
                steps.append(Step(i, pc, n, text, memory(i)))
    return Cases(seed, per_form, forms, steps, branches, refused)


HEADER = '''\
/* The interpreter's test cases, written by recomp/ppc_cases.py and run by tools/test_interp.cpp. */
#pragma once
#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A word as ppc.translate's text runs it. */
typedef void (*CaseStepFn)(CPU* c);
/* A branch as ppc.decode_branch says it goes. One that links sets LR to the next word either way. */
typedef void (*CaseBranchFn)(CPU* c, int* taken, uint32_t* target);

#define CASE_NO_REG 0xFF
enum { CASE_B, CASE_BC, CASE_BLR, CASE_BCTR };

/* A memory form reaches span bytes from rA|0 + rB, or from rA|0 + disp when index is CASE_NO_REG.
   base is rA, and both are CASE_NO_REG in a word that doesn't touch memory. */
typedef struct {
    uint32_t insn, pc;
    CaseStepFn fn;
    uint16_t form;
    uint8_t base, index;
    int32_t disp;
    uint32_t span;
} CaseStep;
typedef struct {
    uint32_t insn, pc;
    CaseBranchFn fn;
    uint16_t form;
    uint8_t kind, link;
} CaseBranch;
typedef struct { uint32_t insn, pc; } CaseRefused;
typedef struct { const char* name; } CaseForm;
typedef struct {
    const char* name;
    const CaseStep* steps;
    uint32_t step_count;
    const CaseBranch* branches;
    uint32_t branch_count;
} CaseFamily;

extern const CaseFamily* const g_case_families[];
extern const uint32_t g_case_family_count;
extern const CaseForm g_case_forms[];
extern const uint32_t g_case_form_count;
extern const CaseRefused g_case_refused[];
extern const uint32_t g_case_refused_count;
extern const uint32_t g_case_seed, g_case_per_form;

#ifdef __cplusplus
}
#endif
'''

KIND = {'b': 'CASE_B', 'bc': 'CASE_BC', 'blr': 'CASE_BLR', 'bctr': 'CASE_BCTR'}


def branch_text(pc, br):
    """decode_branch's CTR decrement, then its target, condition and link. LR and CTR are read as the
    target before LR is set. decode_branch gives an absolute b whose LI sign bit is set a negative
    target, and it's written as C converts that to 32 bits."""
    parts = [br.ctr_dec] if br.ctr_dec else []
    target = {'blr': 'c->lr', 'bctr': 'c->ctr'}.get(br.kind) or f'0x{br.target & 0xFFFFFFFF:08X}u'
    parts.append(f'*target = {target};')
    parts.append(f'*taken = {br.cond or 1};')
    if br.link:
        parts.append(f'c->lr = 0x{(pc + 4) & 0xFFFFFFFF:08X}u;')
    return ' '.join(parts)


def write_file(out, name, lines):
    with open(os.path.join(out, name), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def write(out, cases):
    os.makedirs(out, exist_ok=True)
    note = f'/* Written by recomp/ppc_cases.py --seed {cases.seed} --per-form {cases.per_form}. */'
    write_file(out, 'interp_cases.h', [HEADER.rstrip('\n')])
    for n, fam in enumerate(FAMILIES):
        steps = [s for s in cases.steps if cases.forms[s.form].family == n]
        branches = [b for b in cases.branches if cases.forms[b.form].family == n]
        lines = [note, '#include "interp_cases.h"', '']
        lines += [f'static void s{k}(CPU* c) {{ {s.text} }}' for k, s in enumerate(steps)]
        lines += [f'static void b{k}(CPU* c, int* taken, uint32_t* target) {{ {branch_text(b.pc, b.branch)} }}'
                  for k, b in enumerate(branches)]
        if steps:
            lines += ['', 'static const CaseStep steps[] = {']
            for k, s in enumerate(steps):
                base, index, disp, span = s.mem or (NO_REG, NO_REG, 0, 0)
                lines.append(f'\t{{0x{s.insn:08X}u, 0x{s.pc:08X}u, s{k}, {s.form}, {base}, {index}, {disp}, {span}}},')
            lines.append('};')
        if branches:
            lines += ['', 'static const CaseBranch branches[] = {']
            for k, b in enumerate(branches):
                lines.append(f'\t{{0x{b.insn:08X}u, 0x{b.pc:08X}u, b{k}, {b.form}, {KIND[b.branch.kind]}, '
                             f'{int(b.branch.link)}}},')
            lines.append('};')
        lines += ['', f'const CaseFamily g_case_family_{fam} = {{"{fam}", {"steps" if steps else "0"}, '
                      f'{len(steps)}u, {"branches" if branches else "0"}, {len(branches)}u}};']
        write_file(out, f'cases_{fam}.c', lines)

    lines = [note, '#include "interp_cases.h"', '']
    lines += [f'extern const CaseFamily g_case_family_{fam};' for fam in FAMILIES]
    lines += ['', 'const CaseFamily* const g_case_families[] = {']
    lines += [f'\t&g_case_family_{fam},' for fam in FAMILIES]
    lines += ['};', f'const uint32_t g_case_family_count = {len(FAMILIES)}u;', '']
    lines.append('const CaseForm g_case_forms[] = {')
    lines += [f'\t{{"{f.name}"}},' for f in cases.forms]
    lines += ['};', f'const uint32_t g_case_form_count = {len(cases.forms)}u;', '']
    lines.append('const CaseRefused g_case_refused[] = {')
    lines += [f'\t{{0x{r.insn:08X}u, 0x{r.pc:08X}u}},' for r in cases.refused]
    lines += ['};', f'const uint32_t g_case_refused_count = {len(cases.refused)}u;', '',
              f'const uint32_t g_case_seed = {cases.seed}u, g_case_per_form = {cases.per_form}u;']
    write_file(out, 'cases_index.c', lines)


def main():
    p = argparse.ArgumentParser(description='Write the interpreter\'s test cases from ppc.translate.')
    p.add_argument('out', help='the folder to write them in')
    p.add_argument('--seed', type=int, required=True)
    p.add_argument('--per-form', type=int, default=16, metavar='K',
                   help='random words of each form beside its edge words (16)')
    args = p.parse_args()
    if args.per_form < 1:
        p.error('--per-form must be 1 or more')
    write(args.out, generate(args.seed, args.per_form))


if __name__ == '__main__':
    main()
