#!/usr/bin/env python3
"""Checks that the C recomp.py writes does what the PowerPC does. Run them with
`python3 -m unittest discover -s recomp -p 'test_*.py'`.

Each class assembles a few functions from hand-encoded words into a made-up DOL, recompiles it,
builds the output with the C compiler against a small stand-in for the runtime and runs the
functions from set-up states. Every address and word is made up.
"""
import collections
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ppc  # noqa: E402
from test_recomp_interp import BLR, NOP, RECOMP, RUNTIME, addi, read, write, write_dol  # noqa: E402

T = 0x80104000
MFLR_R3 = 0x7C6802A6


def b(li, aa=0, lk=0):
    return 0x48000000 | (li & 0x03FFFFFC) | aa << 1 | lk


def bc(bo, bi, bd, aa=0, lk=0):
    return 0x40000000 | bo << 21 | bi << 16 | (bd & 0xFFFC) | aa << 1 | lk


def bcctr(bo, bi, lk):
    return 0x4C000420 | bo << 21 | bi << 16 | lk


def mulli(d, a, simm):
    return 0x1C000000 | d << 21 | a << 16 | (simm & 0xFFFF)


# A set-up state for one run of a function: the registers it starts with. A field left None is zero.
Case = collections.namedtuple('Case', 'label fn regs lr ctr cr0', defaults=(None, None, None, None))

HARNESS = r'''
#include "recomp_funcs.h"
#include <stdio.h>

volatile uint64_t g_vcount, g_vlimit = ~0ull;
void irq_poll(CPU* c) { (void)c; }
void idle_hint(CPU* c) { (void)c; }
void unimpl(CPU* c, uint32_t pc, uint32_t i) { (void)c; (void)pc; (void)i; }
void interp_enter(CPU* c, uint32_t entry) { (void)c; (void)entry; }
void interp_site_unusable(CPU* c, uint32_t addr) { (void)c; (void)addr; }

static int calls;
static uint32_t last_target;
void call_indirect(CPU* c, uint32_t a) { (void)c; calls++; last_target = a; }

static CPU cpu;

static void report(const char* label) {
    CPU* c = &cpu;
    printf("%s r3=%u r4=%u r5=%u r6=%u r7=%u r8=%u lr=%u ctr=%u depth=%u calls=%d target=%u\n", label,
           (unsigned)c->r[3], (unsigned)c->r[4], (unsigned)c->r[5], (unsigned)c->r[6], (unsigned)c->r[7],
           (unsigned)c->r[8], (unsigned)c->lr, (unsigned)c->ctr, (unsigned)c->depth, calls, (unsigned)last_target);
}

int main(void) {
    CPU* c = &cpu;
@CASES@
    return 0;
}
'''


class Fixture(unittest.TestCase):
    """FUNCS is [(name, words)], laid out back to back from T. CASES are run on the built C.

    self.c is the recompiled C, self.header recomp_funcs.h and self.stderr what recomp.py said.
    self.problem is '' when the build and the run both worked, and self.results maps each case's
    label to the fields the harness printed for it."""

    FUNCS = ()
    CASES = ()

    @classmethod
    def compiler(cls):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            raise unittest.SkipTest('no C compiler')
        return cc

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = cls.tmp.name
        cls.out = os.path.join(cls.dir, 'out')
        cls.addr, symbols, words, a = {}, [], [], T
        for name, w in cls.FUNCS:
            cls.addr[name] = a
            symbols.append(f'{name} = .text:0x{a:08X}; // type:function size:0x{len(w) * 4:X} scope:global\n')
            words += w
            a += len(w) * 4
        dol = os.path.join(cls.dir, 'main.dol')
        write_dol(dol, [(T, words)])
        sym_path = os.path.join(cls.dir, 'symbols.txt')
        write(sym_path, ''.join(symbols))
        tables = os.path.join(cls.dir, 'tables')
        os.makedirs(tables)
        r = subprocess.run([sys.executable, '-B', RECOMP, dol, sym_path, cls.out, tables],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        cls.stderr = r.stderr
        sources = re.findall(r'\$\{RECOMP_GEN_DIR\}/(\S+)', read(os.path.join(cls.out, 'sources.cmake')))
        cls.sources = [os.path.join(cls.out, f) for f in sources]
        cls.c = ''.join(read(p) for p in cls.sources if re.fullmatch(r'recomp_\d{3}\.c', os.path.basename(p)))
        cls.header = read(os.path.join(cls.out, 'recomp_funcs.h'))
        cls.problem, cls.results = cls.build_and_run('plain', [])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @classmethod
    def harness(cls):
        lines = []
        for case in cls.CASES:
            lines.append('    memset(c, 0, sizeof(*c)); calls = 0; last_target = 0;')
            for r, v in (case.regs or {}).items():
                lines.append(f'    c->r[{r}] = 0x{v:X}u;')
            for field, v in (('lr', case.lr), ('ctr', case.ctr)):
                if v is not None:
                    lines.append(f'    c->{field} = 0x{v:X}u;')
            if case.cr0 is not None:
                lines.append(f'    c->cr[0] = {case.cr0};')
            lines.append(f'    fn_{cls.addr[case.fn]:08X}(c); report("{case.label}");')
        return HARNESS.replace('@CASES@', '\n'.join(lines))

    @classmethod
    def build_and_run(cls, name, flags):
        """Builds the output with the harness and runs it. Returns ('', results) when both worked,
        and otherwise what went wrong and no results."""
        cc = cls.compiler()
        exe = os.path.join(cls.dir, name)
        write(exe + '.c', cls.harness())
        r = subprocess.run([cc, '-std=gnu11', '-O1', '-DGCN_TRACE_CALLS', '-fno-strict-aliasing', '-ffp-contract=off',
                            '-Wno-unused-label', '-Wno-unused-variable', '-Wno-parentheses-equality'] + flags
                           + ['-I', RUNTIME, '-I', cls.out, exe + '.c'] + cls.sources + ['-o', exe, '-lm'],
                           capture_output=True, text=True)
        if r.returncode:
            return f'the build failed:\n{r.stderr}', {}
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
        if r.returncode:
            return f'the run exited {r.returncode}:\n{r.stdout}{r.stderr}', {}
        results = {}
        for line in r.stdout.splitlines():
            label, *fields = line.split()
            results[label] = {k: int(v) for k, v in (f.split('=') for f in fields)}
        return '', results

    def got(self, label):
        self.assertEqual(self.problem, '')
        return self.results[label]


class ConditionalBcctrl(Fixture):
    """beqctrl and bnectrl, with the condition true and false. The hardware sets LR either way."""

    FUNCS = [
        ('f_beqctrl', [bcctr(12, 2, 1), addi(4, 1), BLR]),
        ('f_bnectrl', [bcctr(4, 2, 1), addi(4, 1), BLR]),
    ]
    CTR = T + 0x1000
    # cr0 of 2 is EQ set, and 0 is EQ clear.
    TAKEN = {('f_beqctrl', 2): True, ('f_beqctrl', 0): False, ('f_bnectrl', 2): False, ('f_bnectrl', 0): True}
    CASES = [Case(f'{fn}_{cr0}', fn, lr=0xDEAD0000, ctr=T + 0x1000, cr0=cr0) for fn, cr0 in TAKEN]

    def test_lr_is_set_whether_or_not_the_branch_is_taken(self):
        for (fn, cr0), taken in self.TAKEN.items():
            with self.subTest(fn=fn, cr0=cr0, taken=taken):
                self.assertEqual(f"{self.got(f'{fn}_{cr0}')['lr']:08X}", f'{self.addr[fn] + 4:08X}')

    def test_the_call_happens_only_when_the_branch_is_taken_and_the_function_runs_on(self):
        for (fn, cr0), taken in self.TAKEN.items():
            with self.subTest(fn=fn, cr0=cr0, taken=taken):
                r = self.got(f'{fn}_{cr0}')
                self.assertEqual((r['calls'], r['target'] if taken else 0), (int(taken), self.CTR if taken else 0))
                self.assertEqual((r['r4'], r['depth']), (1, 0))


class AbsoluteTargets(unittest.TestCase):
    """decode_branch gives the address an absolute branch names as a 32-bit one, never a negative."""

    def target(self, word, pc=T):
        return ppc.decode_branch(pc, word).target

    def test_a_b_whose_li_has_its_sign_bit_set_names_a_high_address(self):
        for word, want in ((b(-4, aa=1), 0xFFFFFFFC), (b(-4, aa=1, lk=1), 0xFFFFFFFC),
                           (b(-0x2000000, aa=1), 0xFE000000), (b(-0x2000000, aa=1, lk=1), 0xFE000000)):
            with self.subTest(word=f'{word:08X}'):
                self.assertEqual(self.target(word), want)

    def test_a_b_whose_li_has_its_sign_bit_clear_names_a_low_address(self):
        for word, want in ((b(0x100, aa=1), 0x100), (b(0x1FFFFFC, aa=1, lk=1), 0x1FFFFFC)):
            with self.subTest(word=f'{word:08X}'):
                self.assertEqual(self.target(word), want)

    def test_a_bc_masks_its_displacement_too(self):
        for word, want in ((bc(12, 2, -4, aa=1), 0xFFFFFFFC), (bc(12, 2, 0x7FFC, aa=1, lk=1), 0x7FFC)):
            with self.subTest(word=f'{word:08X}'):
                self.assertEqual(self.target(word), want)

    def test_a_relative_target_wraps_at_32_bits(self):
        self.assertEqual(self.target(b(0x20), 0xFFFFFFF0), 0x10)
        self.assertEqual(self.target(b(-0x20), 0x10), 0xFFFFFFF0)


class AbsoluteBranchEmitted(Fixture):
    """ba, bla and bca to the top of the address space, which no function holds."""

    FUNCS = [
        ('f_ba', [b(-4, aa=1)]),
        ('f_bla', [b(-4, aa=1, lk=1), BLR]),
        ('f_bca', [bc(12, 2, -4, aa=1), BLR]),
    ]
    CASES = [Case('ba', 'f_ba'), Case('bla', 'f_bla'), Case('bca_taken', 'f_bca', cr0=2),
             Case('bca_clear', 'f_bca', cr0=0)]

    def test_the_target_is_written_as_a_32_bit_literal(self):
        self.assertNotIn('0x-', self.c)
        self.assertEqual(self.c.count('call_indirect(c, 0xFFFFFFFCu)'), 3)

    def test_the_warning_names_the_32_bit_address(self):
        self.assertEqual(self.stderr.count('branches to non-code FFFFFFFC'), 3, self.stderr)

    def test_each_branch_calls_the_top_address_and_the_function_returns(self):
        for label, calls in (('ba', 1), ('bla', 1), ('bca_taken', 1), ('bca_clear', 0)):
            with self.subTest(label=label):
                r = self.got(label)
                self.assertEqual((r['calls'], r['target'] if calls else 0, r['depth']),
                                 (calls, 0xFFFFFFFC if calls else 0, 0))
        self.assertEqual(self.got('bla')['lr'], self.addr['f_bla'] + 4)


UBSAN = ['-fsanitize=undefined', '-fno-sanitize-recover=all']


class Mulli(Fixture):
    """mulli by values that overflow a signed int: the product is the low 32 bits of the true one."""

    MULLI = [(5, 3, 2), (6, 4, -1), (7, 3, -32768), (8, 3, 32767)]  # rD, rA, SIMM
    FUNCS = [('f_mulli', [mulli(d, a, simm) for d, a, simm in MULLI] + [BLR])]
    STATES = [('edge', {3: 0x7FFFFFFF, 4: 0x80000000}), ('negative', {3: 0xFFFF8000, 4: 0x12345678}),
              ('small', {3: 3, 4: 5})]
    CASES = [Case(label, 'f_mulli', regs=regs) for label, regs in STATES]

    def test_the_products_wrap_to_32_bits(self):
        for label, regs in self.STATES:
            for d, a, simm in self.MULLI:
                with self.subTest(label=label, rD=d):
                    signed = regs[a] - (1 << 32 if regs[a] & 0x80000000 else 0)
                    self.assertEqual(f"{self.got(label)[f'r{d}']:08X}", f'{(signed * simm) & 0xFFFFFFFF:08X}')

    def test_no_product_overflows_a_signed_int(self):
        probe = os.path.join(self.dir, 'probe')
        write(probe + '.c', 'int main(void) { return 0; }\n')
        r = subprocess.run([self.compiler()] + UBSAN + [probe + '.c', '-o', probe], capture_output=True, text=True)
        if r.returncode or subprocess.run([probe]).returncode:
            self.skipTest('the C compiler cannot build with -fsanitize=undefined')
        problem, _ = self.build_and_run('ubsan', UBSAN)
        self.assertEqual(problem, '')


# bcl to the next word as (BO, BI): the PIC idiom bcl 20,31, then bdnzl, bdzl, beql, bnel, bdnztl and
# bdzfl, which decrement CTR or test a CR bit or both. Each function reads LR into r3 and counts
# how many times the word after the branch runs in r4.
NEXT_BCL = [(20, 31), (16, 0), (18, 0), (12, 2), (4, 2), (8, 2), (2, 2)]
NEXT_FUNCS = [('f_bl', [b(4, lk=1), MFLR_R3, addi(4, 1), BLR])] + [
    (f'f_bcl_{bo}_{bi}', [bc(bo, bi, 4, lk=1), MFLR_R3, addi(4, 1), BLR]) for bo, bi in NEXT_BCL]
DECREMENTS = dict([('f_bl', False)] + [(f'f_bcl_{bo}_{bi}', not bo & 4) for bo, bi in NEXT_BCL])
NEXT_STATES = [(ctr, cr0) for ctr in (0, 1, 2) for cr0 in (0, 2)]  # CTR, and EQ clear or set in cr0
# bl and bcl into the body that are not the next word. They are called, and none is run here.
OTHER_FUNCS = [
    ('f_over', [b(8, lk=1), 0, BLR]),              # bl over a data word, to the blr
    ('f_back', [NOP, NOP, b(-4, lk=1), BLR]),      # bl back to the second nop
    ('f_bcl_over', [bc(12, 2, 8, lk=1), 0, BLR]),  # bcl over a data word, to the blr
]


class NextWordCall(Fixture):
    """A bl or bcl to the next word sets LR and runs on, taken or not, so the rest of the function
    runs once. A bcl still decrements CTR where its BO says to."""

    FUNCS = NEXT_FUNCS
    CASES = [Case(f'{fn}_{ctr}_{cr0}', fn, lr=0xDEAD0000, ctr=ctr, cr0=cr0)
             for fn, _ in NEXT_FUNCS for ctr, cr0 in NEXT_STATES]

    def test_the_rest_of_the_function_runs_once_with_lr_and_ctr_as_the_hardware_leaves_them(self):
        for fn, _ in self.FUNCS:
            for ctr, cr0 in NEXT_STATES:
                with self.subTest(fn=fn, ctr=ctr, cr0=cr0):
                    pc = self.addr[fn]
                    r = self.got(f'{fn}_{ctr}_{cr0}')
                    want_ctr = (ctr - 1) & 0xFFFFFFFF if DECREMENTS[fn] else ctr
                    self.assertEqual({k: r[k] for k in ('r3', 'r4', 'lr', 'ctr', 'depth', 'calls')},
                                     {'r3': pc + 4, 'r4': 1, 'lr': pc + 4, 'ctr': want_ctr, 'depth': 0, 'calls': 0})

    def test_each_warns_that_it_is_the_next_word(self):
        want = [f'warning: {self.addr[fn]:08X} bl to own body {self.addr[fn] + 4:08X} (next word)'
                for fn, _ in self.FUNCS]
        self.assertEqual(sorted(line for line in self.stderr.splitlines() if 'bl to own body' in line), sorted(want))

    def test_the_next_word_is_not_an_entry(self):
        for fn, _ in self.FUNCS:
            with self.subTest(fn=fn):
                next_word = f'fn_{self.addr[fn] + 4:08X}'
                self.assertEqual((self.c.count(next_word), self.header.count(next_word),
                                  self.c.count(f'body_{self.addr[fn]:08X}')), (0, 0, 0))


class OtherOwnBodyCalls(Fixture):
    """A bl or bcl into the body that is not the next word is still a call to that word, which is
    an entry."""

    FUNCS = OTHER_FUNCS
    # (function, index of the call, index of its target) in words
    CALLS = [('f_over', 0, 2), ('f_back', 2, 1), ('f_bcl_over', 0, 2)]

    def test_each_is_still_a_call_to_an_entry(self):
        for fn, at, to in self.CALLS:
            with self.subTest(fn=fn):
                pc, t = self.addr[fn] + 4 * at, self.addr[fn] + 4 * to
                self.assertIn(f'c->lr = 0x{pc + 4:08X}u; fn_{t:08X}(c);', self.c)
                self.assertIn(f'void fn_{t:08X}(CPU* c);', self.header)

    def test_each_still_warns_that_it_is_another_word(self):
        want = [f'warning: {self.addr[fn] + 4 * at:08X} bl to own body {self.addr[fn] + 4 * to:08X} (other)'
                for fn, at, to in self.CALLS]
        self.assertEqual(sorted(line for line in self.stderr.splitlines() if 'bl to own body' in line), sorted(want))


if __name__ == '__main__':
    unittest.main()
