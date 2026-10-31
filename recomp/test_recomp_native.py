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
from test_recomp_interp import BLR, RECOMP, RUNTIME, addi, read, write, write_dol  # noqa: E402

T = 0x80104000
MFLR_R3 = 0x7C6802A6


def bcctr(bo, bi, lk):
    return 0x4C000420 | bo << 21 | bi << 16 | lk


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


if __name__ == '__main__':
    unittest.main()
