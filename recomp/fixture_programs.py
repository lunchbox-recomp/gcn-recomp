#!/usr/bin/env python3
"""The made-up programs tools/test_interp_run.cpp runs natively and through the interpreter.

Usage: fixture_programs.py OUTDIR

Writes OUTDIR/main.dol with its .modules file, OUTDIR/symbols.txt and OUTDIR/tables/ for recomp.py.
OUTDIR/fixture_programs.h gives the test every function's and label's address and the words it puts
in RAM.

Every address, word and name here is made up. The words come from the small assembler below, so the
test and the DOL agree on where everything is.
"""
import os
import struct
import sys
import warnings

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rel  # noqa: E402
from recomp import FUNCS_PER_FILE  # noqa: E402

BLOB = 0x80080000    # code that's in RAM only, below the text, as a mod's would be
TEXT = 0x80100000
DATA = 0x80180000
MODULE = 0x80200000  # a module's code, which has no guard
STACK = 0x8037FF00
WINDOW = (0x80000000, 0x400000)  # all the programs touch
FILLERS = 40


# ---- the words ----

def d_form(op, d, a, imm):
    return op << 26 | d << 21 | a << 16 | (imm & 0xFFFF)


def x_form(d, a, b, xo):
    return 31 << 26 | d << 21 | a << 16 | b << 11 | xo << 1


def li(d, v):
    return d_form(14, d, 0, v)


def addi(d, a, v):
    return d_form(14, d, a, v)


def mulli(d, a, v):
    return d_form(7, d, a, v)


def cmpwi(a, v):
    return d_form(11, 0, a, v)


def ori(a, s, v):
    return d_form(24, s, a, v)


def stw(s, off, a):
    return d_form(36, s, a, off)


def stwu(s, off, a):
    return d_form(37, s, a, off)


def lwz(d, off, a):
    return d_form(32, d, a, off)


def add(d, a, b):
    return x_form(d, a, b, 266)


def mr(a, s):
    return x_form(s, a, s, 444)


def lwzx(d, a, b):
    return x_form(d, a, b, 23)


def slwi(a, s, n):
    return 21 << 26 | s << 21 | a << 16 | n << 11 | (31 - n) << 1


def mfspr(d, spr):
    return x_form(d, spr & 31, spr >> 5, 339)


def mtspr(spr, s):
    return x_form(s, spr & 31, spr >> 5, 467)


def mflr(d):
    return mfspr(d, 8)


def mtlr(s):
    return mtspr(8, s)


def mtctr(s):
    return mtspr(9, s)


def mfmsr(d):
    return x_form(d, 0, 0, 83)


def mtmsr(s):
    return x_form(s, 0, 0, 146)


BLR, BLRL, BCTR, BCTRL, RFI = 0x4E800020, 0x4E800021, 0x4E800420, 0x4E800421, 0x4C000064
BEQCTRL = 19 << 26 | 12 << 21 | 2 << 16 | 528 << 1 | 1  # bcctrl 12,2: to CTR and links when cr0's EQ is set


# A word that names a label is a function of the pc it's at and `at`, which gives a label's address.
# A label starting with a dot is the code's own.

def b(t, lk=0):
    return lambda pc, at: 0x48000000 | ((at(t) - pc) & 0x03FFFFFC) | lk


def bl(t):
    return b(t, 1)


def bc(bo, bi, t, lk=0):
    return lambda pc, at: 16 << 26 | bo << 21 | bi << 16 | ((at(t) - pc) & 0xFFFC) | lk


def bne(t):
    return bc(4, 2, t)


def beql(t):
    return bc(12, 2, t, 1)


def bnel(t):
    return bc(4, 2, t, 1)


def bcl(t):
    """bcl 20,31, which always branches and links."""
    return bc(20, 31, t, 1)


def bdnz(t):
    return bc(16, 0, t)


def bdnzl(t):
    return bc(16, 0, t, 1)


def address(r, t):
    """lis and ori that put t's address in r."""
    return [lambda pc, at: d_form(15, r, 0, at(t) >> 16), lambda pc, at: d_form(24, r, r, at(t) & 0xFFFF)]


class Code:
    """Words at an address. A body item is a word, a function giving one or a label 'x:', which is
    NAME.x outside. opts hold the code's lines in recomp.py's tables."""

    def __init__(self, name, body, **opts):
        self.name, self.body, self.opts = name, body, opts

    def size(self):
        return 4 * sum(not isinstance(x, str) for x in self.body)

    def place(self, addr, labels):
        self.addr = addr
        labels[self.name] = addr
        pc = addr
        for x in self.body:
            if isinstance(x, str):
                labels[f'{self.name}.{x.rstrip(":")}'] = pc
            else:
                pc += 4

    def words(self, labels):
        def at(t):
            return labels[self.name + t if t.startswith('.') else t]
        out, pc = [], self.addr
        for x in self.body:
            if not isinstance(x, str):
                out.append(x(pc, at) if callable(x) else x)
                pc += 4
        return out

    def label(self, labels, x):
        return labels[f'{self.name}.{x}']


# ---- the programs ----

P16 = [mflr(0), stw(0, 4, 1), stwu(1, -16, 1)]
E16 = [lwz(0, 20, 1), addi(1, 1, 16), mtlr(0), BLR]
P32 = [mflr(0), stw(0, 4, 1), stwu(1, -32, 1), stw(31, 28, 1)]
E32 = [lwz(31, 28, 1), lwz(0, 36, 1), addi(1, 1, 32), mtlr(0), BLR]

SITE2_WORD = mulli(3, 3, 7)
BLOB_LOOP = P16 + [li(3, 4), li(4, 0), 'loop:', addi(4, 4, 3), addi(3, 3, -1), cmpwi(3, 0), bne('.loop'),
                   bl('f_callee')] + E16
# The hook's code. It runs the word it replaced last, then goes back into the hooked function.
HOOK = [li(4, 3), 'loop:', addi(3, 3, 10), addi(4, 4, -1), cmpwi(4, 0), bne('.loop'), addi(3, 3, 1)]
HOOKED_TAIL = [mulli(3, 3, 3), addi(3, 3, 2), BLR]
# A mod's tail call to an HLE'd function, with a b and then with a bctr.
BLOB_TAIL = address(3, 'context') + [b('made_up_save')]
BLOB_BCTR_TAIL = address(3, 'context') + address(12, 'made_up_save') + [mtctr(12), BCTR]
# A bl and a bcl to the next word, which read their own address, and what the hardware does with them.
BLOB_NEXT = (P16 + address(12, '.n1') + [bl('.n1'), 'n1:', mflr(3)]
             + address(12, '.n2') + [bcl('.n2'), 'n2:', mflr(4)] + E16)
NEXT_TWIN = (P16 + address(12, 'blob_next.n1') + [mtlr(12), mflr(3)]
             + address(12, 'blob_next.n2') + [mtlr(12), mflr(4)] + E16)
# A taken bdnzl and a bcl not taken, each to the next word, and what the hardware does with them.
BLOB_NEXT2 = (P16 + [li(0, 2), mtctr(0)] + address(12, '.n1') + [bdnzl('.n1'), 'n1:', mflr(3), li(5, 0), cmpwi(5, 0)]
              + address(12, '.n2') + [bnel('.n2'), 'n2:', mflr(4)] + E16)
NEXT2_TWIN = (P16 + [li(0, 2), mtctr(0)] + address(12, 'blob_next2.n1')
              + [mtlr(12), li(0, 1), mtctr(0), mflr(3), li(5, 0), cmpwi(5, 0)]
              + address(12, 'blob_next2.n2') + [mtlr(12), mflr(4)] + E16)
# A mod's branch onto f_multi's mid-entry, which f_midcaller's bl makes, once the word there changed.
MID_WORD = mulli(3, 3, 7)
BLOB_MID = [li(3, 5), b('f_multi.mid')]
# A blrl and a bctrl whose LR and CTR hold the next word, and what the hardware does with each.
BLOB_BLRL_NEXT = P16 + address(12, '.n') + [mtlr(12), BLRL, 'n:', mflr(3)] + E16
BLRL_TWIN = P16 + address(12, 'blob_blrl_next.n') + [mtlr(12), mtlr(12), mflr(3)] + E16
BLOB_BCTRL_NEXT = P16 + address(12, '.n') + [mtctr(12), BCTRL, 'n:', mflr(3)] + E16
BCTRL_TWIN = P16 + address(12, 'blob_bctrl_next.n') + [mtctr(12), mtlr(12), mflr(3)] + E16
# A mod's branch onto a patch site in a function's middle.
BLOB_SITE = [li(3, 1), b('f_patch.site')]
# A loop through a setjmp site and a context site that never ends. Only an interpreted run takes it,
# and the 4 MiB stop ends it.
TWO_SITE_LOOP = (P16 + ['again:'] + address(3, 'jmp_buf') + ['a:', bl('made_up_setjmp')] + address(3, 'context')
                 + ['b:', bl('made_up_save'), b('.again')])
GAP_WORD = li(3, 99)  # what fills a gap no symbol holds

FUNCS = [
    Code('f_loop', [li(3, 3), li(4, 0), 'loop:', add(4, 4, 3), addi(3, 3, -1), cmpwi(3, 0), bne('.loop'), BLR]),
    Code('f_idle', [li(3, 3), 'loop:', addi(3, 3, -1), cmpwi(3, 0), 'spin:', bne('.loop'), BLR], idle='spin'),
    Code('f_mtmsr', [mfmsr(4), ori(4, 4, 0x8000), mtmsr(4), addi(3, 3, 1), BLR]),
    Code('f_callee', [addi(3, 3, 5), BLR]),
    Code('f_tail', [addi(3, 3, 1), b('f_callee')]),
    Code('f_call', P16 + [bl('f_callee'), addi(3, 3, 100)] + E16),
    Code('f_regcalls', P16 + address(12, 'f_callee') + [mtlr(12), BLRL, mflr(5), mtctr(12), BCTRL, mflr(6)] + E16),
    Code('f_switch', address(4, 'jumptable') + [slwi(0, 3, 2), lwzx(0, 4, 0), mtctr(0), BCTR,
                                                'case0:', li(5, 10), b('.end'), 'case1:', li(5, 11), b('.end'),
                                                'case2:', li(5, 12), 'end:', BLR]),
    # Its first pass takes the table's first case, which is back before the bctr, and its second the end.
    Code('f_switch_back', address(4, 'jumptable_back') + [li(6, 0), 'again:', slwi(0, 6, 2), lwzx(0, 4, 0),
                                                          addi(6, 6, 1), mtctr(0), BCTR, 'end:', BLR]),
    Code('f_condbl', P16 + [cmpwi(3, 0), beql('f_callee'), mflr(6), bnel('f_callee'), mflr(7)] + E16),
    Code('f_multi', [li(3, 1), 'mid:', addi(3, 3, 2), BLR]),
    Code('f_midcaller', P16 + [bl('f_multi.mid'), addi(3, 3, 1)] + E16),
    Code('f_falls', [addi(3, 3, 1), addi(4, 4, 1)]),
    Code('f_next', [addi(3, 3, 10), BLR]),
    Code('f_patch', [li(3, 1), 'site:', addi(3, 3, 2), addi(3, 3, 4), BLR],
         patch='heap_trace_alloc(c, 0x{site:08X}u); c->r[3] = c->r[3] * 3 + 1;'),
    Code('f_patch_ret', [li(3, 1), 'site:', addi(3, 3, 2), li(3, 77), BLR],
         patch='heap_trace_alloc_result(c, (int)c->r[3]); c->r[3] = 9; RET();'),
    Code('f_site2', [li(3, 4), 'site:', addi(3, 3, 1), addi(3, 3, 2), BLR],
         patch='heap_trace_free(c); c->r[3] += 100;'),
    Code('f_site2_twin', [li(3, 4), SITE2_WORD, addi(3, 3, 2), BLR]),
    Code('f_step', [li(3, 2), 'site:', addi(3, 3, 3), BLR], step='A'),
    # recomp.py warns about this one's goto, which is what it's for.
    Code('f_goto', ['site:', li(3, 3), 'loop:', addi(3, 3, -1), cmpwi(3, 0), bne('.loop'), BLR],
         patch='c->r[3] = 3; goto L_{loop:08X};'),
    Code('made_up_setjmp', [mflr(0), stw(0, 0, 3), stw(1, 4, 3), li(3, 0), BLR], special='setjmp'),
    Code('made_up_longjmp', [lwz(0, 0, 3), mtlr(0), lwz(1, 4, 3), mr(3, 4), BLR], special='longjmp'),
    Code('f_sj', P32 + [li(31, 0)] + address(3, 'jmp_buf') + [bl('made_up_setjmp'), addi(31, 31, 1), cmpwi(3, 0),
                                                              bne('.done'), li(4, 5)] + address(3, 'jmp_buf')
         + [bl('f_sj_deeper'), 'done:', mr(5, 31)] + E32),
    Code('f_sj_deeper', P16 + [addi(6, 6, 1), bl('made_up_longjmp')] + E16),
    Code('made_up_save', [stw(1, 0, 3), li(3, 0), BLR], hle=True, special='made_up_save'),
    Code('made_up_load', [BLR], hle=True),
    Code('f_ctx', P32 + [li(31, 0)] + address(3, 'context') + [bl('made_up_save'), addi(31, 31, 1), cmpwi(3, 0),
                                                               bne('.done')] + address(3, 'context')
         + [bl('f_ctx_deeper'), 'done:', mr(5, 31)] + E32),
    Code('f_ctx_deeper', P16 + [addi(6, 6, 1), bl('made_up_load')] + E16),
    Code('f_irq_loop', [li(3, 5), li(4, 0), 'loop:', add(4, 4, 3), addi(3, 3, -1), cmpwi(3, 0), bne('.loop'), BLR]),
    Code('f_handler', [li(3, 99), li(4, 77)] + address(5, 'mark') + [stw(3, 0, 5), 'loop:', addi(4, 4, -1),
                                                                    cmpwi(4, 70), bne('.loop'), bl('made_up_resume'),
                                                                    BLR]),
    Code('made_up_resume', [BLR], hle=True),
    Code('f_lr', [mflr(5), addi(0, 5, 8), mtlr(0), 'ret:', BLR]),
    Code('f_blob_twin', BLOB_LOOP),
    Code('f_hooked', [addi(3, 3, 1), 'after:'] + HOOKED_TAIL),
    Code('f_hooked_twin', HOOK + HOOKED_TAIL),
    Code('f_blob_tail_twin', BLOB_TAIL),
    Code('f_blob_bctr_tail_twin', BLOB_BCTR_TAIL),
    Code('f_blob_next_twin', NEXT_TWIN),
    Code('f_blrl_next_twin', BLRL_TWIN),
    Code('f_bctrl_next_twin', BCTRL_TWIN),
    # A bl and a bcl to the next word in a function. Native code sets LR and runs on, as the hardware does,
    # so r3 counts one run and r4 holds the next word's address.
    Code('f_bl_next', P16 + [li(3, 0), bl('.n1'), 'n1:', addi(3, 3, 1), mflr(4)] + E16),
    Code('f_bcl_next', P16 + [li(3, 0), bcl('.n1'), 'n1:', addi(3, 3, 1), mflr(4)] + E16),
    # A taken bdnzl and a bnel not taken, each to the next word, so CTR and LR both have to come out right.
    Code('f_bdnzl_next', P16 + [li(0, 2), mtctr(0), li(3, 0), bdnzl('.n1'), 'n1:', addi(3, 3, 1), mflr(4), li(5, 0),
                                cmpwi(5, 0), bnel('.n2'), 'n2:', addi(3, 3, 1), mflr(6)] + E16),
    # A bl in a function's last word, to the function that starts right after it. Native code sets LR and
    # runs on into it, once.
    Code('f_last_bl', [li(3, 0), bl('f_last_bl_next')]),
    Code('f_last_bl_next', [addi(3, 3, 1), mflr(4), BLR]),
    # A blrl whose LR holds the next word, which native code calls as it does any blrl, and stops at.
    Code('f_blrl_next', P16 + address(12, '.n') + [mtlr(12), BLRL, 'n:', mflr(3)] + E16),
    # The same for a bctrl whose CTR holds the next word.
    Code('f_bctrl_next', P16 + address(12, '.n') + [mtctr(12), BCTRL, 'n:', mflr(3)] + E16),
    # A bcctrl not taken, which sets LR all the same. r6 holds the word after it, and CTR keeps its value.
    Code('f_bcctrl_skip', P16 + address(12, 'f_callee') + [mtctr(12), li(5, 1), cmpwi(5, 0), BEQCTRL, mflr(6)] + E16),
    Code('f_rfi', [li(3, 5), RFI, li(3, 6), BLR]),
    Code('f_bdnz', [li(0, 4), mtctr(0), 'spin:', bdnz('.spin'), BLR]),
    Code('f_blob_site_twin', BLOB_SITE),
    # It runs off its end into two words that no symbol holds.
    Code('f_gap_falls', [addi(3, 3, 1), 'gap:'], gap=8),
    # A setjmp site in a loop, taken 10,000 times. The call to filler_00 moves LR each time round, so the
    # site has to set it again.
    Code('f_sj_loop', P32 + [li(31, 0), 'again:'] + address(3, 'jmp_buf')
         + [bl('made_up_setjmp'), bl('filler_00'), addi(31, 31, 1), cmpwi(31, 10000), bne('.again'), mr(3, 31)]
         + E32),
    # A setjmp site, then a call to a function with a context site, each in a C frame of its own.
    Code('f_two_sites', P32 + address(3, 'jmp_buf') + [bl('made_up_setjmp'), bl('f_ctx_site')] + E32),
    Code('f_ctx_site', P16 + address(3, 'context') + [bl('made_up_save')] + E16),
    # A setjmp site and then a context site in one function with no loop, which share r1 and not LR.
    Code('f_sj_then_ctx', P32 + address(3, 'jmp_buf') + [bl('made_up_setjmp')] + address(3, 'context')
         + [bl('made_up_save')] + E32),
    Code('f_two_site_loop', TWO_SITE_LOOP),
    # A setjmp site taken five times round a loop, then a longjmp back to it.
    Code('f_sj_again', P32 + [li(31, 0), 'again:'] + address(3, 'jmp_buf')
         + [bl('made_up_setjmp'), addi(31, 31, 1), cmpwi(3, 0), bne('.done'), cmpwi(31, 5), bne('.again'), li(4, 7)]
         + address(3, 'jmp_buf') + [bl('f_sj_deeper'), 'done:', mr(5, 31)] + E32),
    # A setjmp site in a loop that returns from its middle on the third time round.
    Code('f_sj_midret', P32 + [li(31, 0), 'again:'] + address(3, 'jmp_buf')
         + [bl('made_up_setjmp'), addi(31, 31, 1), cmpwi(31, 3), bne('.more'), mr(3, 31)] + E32
         + ['more:', b('.again')]),
    # A bl in a function's last word, to a setjmp function that starts right after it. Native code takes it as
    # a site, and then the function runs off its end into the same setjmp function again.
    Code('f_sj_next', address(3, 'jmp_buf') + [bl('made_up_sj_next')]),
    Code('made_up_sj_next', [mflr(0), stw(0, 0, 3), stw(1, 4, 3), BLR], special='setjmp'),
    # The same with a context function, which is a site of its own kind.
    Code('f_ctx_next', address(3, 'context') + [bl('made_up_ctx_next')]),
    Code('made_up_ctx_next', [stw(1, 0, 3), BLR], special='made_up_ctx_next'),
    Code('f_next2_twin', NEXT2_TWIN),
    Code('f_mid_twin', [li(3, 1), 'mid:', MID_WORD, BLR]),
    Code('f_blob_mid_twin', [li(3, 5), b('f_mid_twin.mid')]),
] + [Code(f'filler_{k:02d}', [addi(3, 3, 1), BLR]) for k in range(FILLERS)]
MODULE_FUNCS = [Code('made_up_module_fn', [li(3, 8), BLR])]
BLOBS = [(BLOB, Code('blob_loop', BLOB_LOOP)), (BLOB + 0x100, Code('blob_hook', HOOK + [b('f_hooked.after')])),
         (BLOB + 0x200, Code('blob_tail', BLOB_TAIL)), (BLOB + 0x300, Code('blob_next', BLOB_NEXT)),
         (BLOB + 0x400, Code('blob_site', BLOB_SITE)), (BLOB + 0x500, Code('blob_blrl_next', BLOB_BLRL_NEXT)),
         (BLOB + 0x600, Code('blob_bctrl_next', BLOB_BCTRL_NEXT)), (BLOB + 0x700, Code('blob_next2', BLOB_NEXT2)),
         (BLOB + 0x800, Code('blob_mid', BLOB_MID)), (BLOB + 0x900, Code('blob_two_site_loop', TWO_SITE_LOOP)),
         (BLOB + 0xA00, Code('blob_bctr_tail', BLOB_BCTR_TAIL))]
# The data holds the two jump tables, then the guest jmp_buf, the context and the handler's mark.
DATA_LABELS = {'jumptable': DATA, 'jumptable_back': DATA + 0x20, 'jmp_buf': DATA + 0x40, 'context': DATA + 0x80,
               'mark': DATA + 0xC0}
JUMP_TABLES = (('jumptable', 'f_switch', ('case0', 'case1', 'case2')),
               ('jumptable_back', 'f_switch_back', ('again', 'end')))
DATA_SIZE = 0x100


def layout():
    """Places everything, and returns the labels with each section's words. A function's gap is that
    many bytes of GAP_WORD after it."""
    labels = dict(DATA_LABELS)
    for codes, base in ((FUNCS, TEXT), (MODULE_FUNCS, MODULE)):
        a = base
        for f in codes:
            f.place(a, labels)
            a += f.size() + f.opts.get('gap', 0)
    for a, blob in BLOBS:
        blob.place(a, labels)
    text = [w for f in FUNCS for w in f.words(labels) + [GAP_WORD] * (f.opts.get('gap', 0) // 4)]
    module = [w for f in MODULE_FUNCS for w in f.words(labels)]
    data = [0] * (DATA_SIZE // 4)
    for table, func, cases in JUMP_TABLES:
        f = FUNCS[[g.name for g in FUNCS].index(func)]
        k = (labels[table] - DATA) // 4
        data[k:k + len(cases)] = [f.label(labels, case) for case in cases]
    return labels, text, module, data


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)


def tables(labels):
    """recomp.py's table files, by name."""
    hle, specials, idle, patches, steps = [], [], [], [], []
    for f in FUNCS:
        o = f.opts
        if o.get('hle'):
            hle.append(f.name)
        if 'special' in o:
            specials.append(f'{f.addr:08X} {o["special"]}')
        if 'idle' in o:
            idle.append(f'{f.label(labels, o["idle"]):08X} {f.name}_spin')
        if 'patch' in o:
            local = {k.split('.', 1)[1]: v for k, v in labels.items() if k.startswith(f.name + '.')}
            patches.append(f'{local["site"]:08X} {o["patch"].format(**local)}')
        if 'step' in o:
            steps.append(f'{f.label(labels, "site"):08X} {o["step"]}')
    return {'hle.txt': hle, 'special_calls.txt': specials, 'idle.txt': idle, 'patches.txt': patches,
            'steps.txt': steps}


def c_words(name, words):
    rows = [', '.join(f'0x{w:08X}u' for w in words[k:k + 6]) for k in range(0, len(words), 6)]
    return [f'static const uint32_t {name}[] = {{'] + [f'\t{r},' for r in rows] + ['};']


def header(labels, text, module, data):
    out = ['/* What tools/test_interp_run.cpp needs to know about its programs, from recomp/fixture_programs.py. */',
           '#pragma once', '#include <stdint.h>', '']
    for name, a in sorted(labels.items(), key=lambda kv: (kv[1], kv[0])):
        out.append(f'#define FX_{name.replace(".", "_").upper()} 0x{a:08X}u')
    out += ['', f'#define FX_STACK 0x{STACK:08X}u', f'#define FX_WINDOW_BASE 0x{WINDOW[0]:08X}u',
            f'#define FX_WINDOW_SIZE 0x{WINDOW[1]:X}u', f'#define FX_BLOB_BASE 0x{BLOB:08X}u',
            f'#define FX_BLOB_END 0x{TEXT:08X}u', f'#define FX_FILLER_COUNT {FILLERS}u',
            "/* The words the test writes over f_hooked's first, f_site2's site and f_multi's mid-entry. */",
            f'#define FX_HOOK_WORD 0x{b("blob_hook")(labels["f_hooked"], labels.get):08X}u',
            f'#define FX_SITE2_WORD 0x{SITE2_WORD:08X}u', f'#define FX_MID_WORD 0x{MID_WORD:08X}u', '',
            'typedef struct { uint32_t addr, count; const uint32_t* words; } FxWords;']
    image = [('fx_text', TEXT, text), ('fx_module', MODULE, module), ('fx_data', DATA, data)]
    image += [(f'fx_{blob.name}', a, blob.words(labels)) for a, blob in BLOBS]
    for name, _, words in image:
        out += c_words(name, words)
    out.append('/* RAM as the test starts each run: the DOL\'s sections and the blobs. */')
    out.append('static const FxWords fx_image[] = {')
    out += [f'\t{{0x{a:08X}u, {len(words)}u, {name}}},' for name, a, words in image]
    out += ['};', '']
    return '\n'.join(out)


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: fixture_programs.py OUTDIR')
    out = sys.argv[1]
    if len(FUNCS) + len(MODULE_FUNCS) >= FUNCS_PER_FILE:
        sys.exit('error: more functions than recomp.py puts in one file, which CMakeLists.txt expects')
    labels, text, module, data = layout()
    os.makedirs(out, exist_ok=True)
    pack = lambda words: struct.pack(f'>{len(words)}I', *words)  # noqa: E731
    # rel.py's writer leaves its file for the collector to close, which Python can warn about.
    with warnings.catch_warnings():
        warnings.simplefilter('ignore', ResourceWarning)
        rel.write_dol(os.path.join(out, 'main.dol'),
                      [(TEXT, pack(text), True), (MODULE, pack(module), True), (DATA, pack(data), False)],
                      (DATA + 0x1000, 0x100), TEXT)
    write(os.path.join(out, 'main.dol.modules'), f'{MODULE:08X} {len(module) * 4:X} 0 made_up_module\n')
    syms = [f'{f.name} = .text:0x{f.addr:08X}; // type:function size:0x{f.size():X} scope:global'
            for f in FUNCS + MODULE_FUNCS]
    for table, _, cases in JUMP_TABLES:
        a = labels[table]
        syms.append(f'jumptable_{a:08X} = .data:0x{a:08X}; // type:object size:0x{len(cases) * 4:X} scope:local')
    write(os.path.join(out, 'symbols.txt'), '\n'.join(syms) + '\n')
    for name, lines in tables(labels).items():
        write(os.path.join(out, 'tables', name), ''.join(line + '\n' for line in lines))
    write(os.path.join(out, 'fixture_programs.h'), header(labels, text, module, data))


if __name__ == '__main__':
    main()
