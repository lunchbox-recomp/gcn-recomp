#!/usr/bin/env python3
"""Checks that ppc.translate rounds frC where Gekko's single-precision multiplies do. Run them
with `python3 -m unittest discover -s recomp -p 'test_*.py'`.

fmuls, fmadds, fmsubs, fnmadds and fnmsubs, and the paired singles' ps_mul, ps_muls0/1,
ps_madd, ps_madds0/1, ps_msub, ps_nmadd and ps_nmsub read frC through ROUND_FRC
(runtime/recomp.h), and only frC, in the lanes each multiplies by. Nothing else rounds an
operand: not the double-precision forms, not the sums, ps_sel or the estimates. The words are
made up from their encodings.
"""
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

RUNTIME = os.path.join(os.path.dirname(HERE), 'runtime')
D, A, B, C = 1, 2, 3, 4
PC = 0x80004000

# The single-precision multiplies by their primary and 5-bit extended opcodes, with the lanes of
# frC each one multiplies by: 0 for ps0, 1 for ps1.
MULTIPLIES = {
    (59, 25): (0,), (59, 28): (0,), (59, 29): (0,), (59, 30): (0,), (59, 31): (0,),
    (4, 25): (0, 1), (4, 28): (0, 1), (4, 29): (0, 1), (4, 30): (0, 1), (4, 31): (0, 1),
    (4, 12): (0,), (4, 13): (1,), (4, 14): (0,), (4, 15): (1,),
}
# Forms that read no rounded operand: the other single and paired arithmetic, ps_sel, the sums,
# the estimates and every double-precision A-form.
UNROUNDED = ([(59, x) for x in (18, 20, 21, 24)] + [(4, x) for x in (10, 11, 18, 20, 21, 23, 24, 26)]
             + [(63, x) for x in (18, 20, 21, 22, 23, 25, 26, 28, 29, 30, 31)])
LANE = {0: f'c->f[{C}].d', 1: f'c->ps1[{C}]'}


def a_form(op, xo, d=D, a=A, b=B, c=C):
    return op << 26 | d << 21 | a << 16 | b << 11 | c << 6 | xo << 1


def rounded(text):
    """The operands ROUND_FRC is applied to in TEXT."""
    return set(re.findall(r'ROUND_FRC\(([^()]*)\)', text))


def reads_whole(text, operand):
    """Whether TEXT reads OPERAND anywhere but inside ROUND_FRC."""
    return operand in text.replace(f'ROUND_FRC({operand})', '')


class Translate(unittest.TestCase):
    def test_each_multiply_rounds_the_lanes_of_frc_it_multiplies_by(self):
        for (op, xo), lanes in MULTIPLIES.items():
            with self.subTest(op=op, xo=xo):
                text = ppc.translate(PC, a_form(op, xo))
                self.assertEqual(rounded(text), {LANE[n] for n in lanes})
                for n in lanes:
                    self.assertFalse(reads_whole(text, LANE[n]), text)

    def test_no_other_form_rounds_an_operand(self):
        for op, xo in UNROUNDED:
            with self.subTest(op=op, xo=xo):
                self.assertNotIn('ROUND_FRC', ppc.translate(PC, a_form(op, xo)))

    def test_fra_and_frb_are_never_rounded(self):
        for (op, xo) in MULTIPLIES:
            with self.subTest(op=op, xo=xo):
                text = ppc.translate(PC, a_form(op, xo))
                for r in (A, B):
                    self.assertNotIn(f'ROUND_FRC(c->f[{r}].d)', text)
                    self.assertNotIn(f'ROUND_FRC(c->ps1[{r}])', text)

    def test_the_step_forms_round_frc_as_the_plain_ones_do(self):
        # A step form scales a carried operand (translate_step); the multiply still reads frC
        # rounded, and the double-precision form reads it whole.
        for op, xo, x in ((59, 25, 'A'), (59, 29, 'B'), (59, 29, 'C'), (4, 25, 'A'), (4, 14, 'B'),
                          (4, 15, 'A')):
            with self.subTest(op=op, xo=xo, x=x):
                text = ppc.translate_step(PC, a_form(op, xo), x)
                lanes = MULTIPLIES[(op, xo)]
                self.assertEqual(rounded(text), {LANE[n] for n in lanes})
                for n in lanes:
                    self.assertFalse(reads_whole(text, LANE[n]), text)
        self.assertNotIn('ROUND_FRC', ppc.translate_step(PC, a_form(63, 25), 'A'))


HARNESS = r'''
#include "recomp.h"
#include <stdio.h>

static CPU cpu;

static double d(uint64_t u) { double v; memcpy(&v, &u, 8); return v; }
static uint64_t u(double v) { uint64_t r; memcpy(&r, &v, 8); return r; }

int main(void) {
    CPU* c = &cpu;
@CASES@
    return 0;
}
'''
ONE, ONE_NEG, SEVEN = 0x3FF0000000000000, 0xBFF0000000000000, 0x401C000000000000
# 1 + 2^-24 + 2^-26. Rounded to 25 bits it's 1 + 2^-24, and 1 times that rounds to even, 1.0;
# whole, 1 times it is over the tie and rounds to 1 + 2^-23.
FRC = 0x3FF0000014000000
ONE_UP = 0x3FF0000020000000


class Run(unittest.TestCase):
    """translate's C for each case, built against runtime/recomp.h and run from set registers."""

    # label, word, {register: (ps0, ps1)}, the (ps0, ps1) fD should hold
    CASES = [
        ('fmuls', a_form(59, 25), {A: (ONE, ONE), C: (FRC, FRC)}, (ONE, ONE)),
        ('fmadds', a_form(59, 29), {A: (ONE, ONE), B: (0, 0), C: (FRC, FRC)}, (ONE, ONE)),
        ('fnmsubs', a_form(59, 30), {A: (ONE_NEG, ONE), B: (0, 0), C: (FRC, FRC)}, (ONE, ONE)),
        ('ps_mul', a_form(4, 25), {A: (ONE, ONE_NEG), C: (FRC, FRC)}, (ONE, ONE_NEG)),
        ('ps_muls0', a_form(4, 12), {A: (ONE, ONE_NEG), C: (FRC, SEVEN)}, (ONE, ONE_NEG)),
        ('ps_madds1', a_form(4, 15), {A: (ONE, ONE_NEG), B: (0, 0), C: (SEVEN, FRC)}, (ONE, ONE_NEG)),
        # The double-precision fmul reads frC whole, keeps every bit of the product and leaves fD's
        # ps1 as it was.
        ('fmul', a_form(63, 25), {A: (ONE, ONE), C: (FRC, FRC)}, (FRC, ONE_UP)),
    ]

    def test_products_with_a_double_in_frc_are_the_hardware_s(self):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('no C compiler')
        lines = []
        for label, word, regs, _ in self.CASES:
            lines.append('    memset(c, 0, sizeof(*c));')
            lines.append(f'    c->f[{D}].u = 0x{ONE_UP:X}ull; c->ps1[{D}] = d(0x{ONE_UP:X}ull);')
            for r, (p0, p1) in regs.items():
                lines.append(f'    c->f[{r}].u = 0x{p0:X}ull; c->ps1[{r}] = d(0x{p1:X}ull);')
            lines.append(f'    {ppc.translate(PC, word)}')
            lines.append(f'    printf("{label} %016llX %016llX\\n", (unsigned long long)c->f[{D}].u, '
                         f'(unsigned long long)u(c->ps1[{D}]));')
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = os.path.join(tmp, 'run.c'), os.path.join(tmp, 'run')
            with open(src, 'w') as f:
                f.write(HARNESS.replace('@CASES@', '\n'.join(lines)))
            r = subprocess.run([cc, '-std=gnu11', '-O1', '-ffp-contract=off', '-I', RUNTIME, src, '-o', exe,
                                '-lm'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout
        got = {label: (int(p0, 16), int(p1, 16)) for label, p0, p1 in (line.split() for line in out.splitlines())}
        for label, _, _, want in self.CASES:
            with self.subTest(label=label):
                self.assertEqual(got[label], want)


if __name__ == '__main__':
    unittest.main()
