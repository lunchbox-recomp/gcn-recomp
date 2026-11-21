#!/usr/bin/env python3
"""Checks Gekko's non-IEEE mode, FPSCR[NI], in translate's C. Run them with
`python3 -m unittest discover -s recomp -p 'test_*.py'`.

The 750CL manual's operand and result tables (Tables 2-24 and 2-25) have NI zero a denormalized
operand and return zero for a denormalized result. runtime/recomp.h's gcn_fp_follow sets the
host's flush mode from FPSCR[NI], and hle_set_fpscr and the thread switches call it. Each case
here runs translate's C for its words from set registers in IEEE mode and again in non-IEEE
mode, and checks the bits fD holds in each. Loads and stores aren't arithmetic, so lfs's and
stfs's conversions, gekko_single_value and gekko_single_bits, give the same bits in both modes,
and stfs denormalizes as the PowerPC PEM says, dropping the bits it shifts out.

The first case lerps from a table entry of 2^-147, a single denormal, to the next entry's 0, as
b - a, then (b - a) * t + a. In IEEE mode that gives a denormal. In non-IEEE mode a is zeroed
and the result is 0, as Dolphin gives.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ppc  # noqa: E402
from test_round_frc import HARNESS, PC, RUNTIME, a_form  # noqa: E402

TINY = 0x36C0000000000000  # 2^-147, a single denormal's value, word 00000004, as lfs gives it
NEG_T = 0xBFEB0C31A0000000  # the lerp's fraction, -0.845...
INF, NEG_INF = 0x7FF0000000000000, 0xFFF0000000000000
QNAN = 0x7FF8000000000000
GREATEST_SINGLE = 0x47EFFFFFE0000000
DOUBLE_DENORMAL = 0x0000000000000001
# Each input goes through a volatile, so the compiler can't work a case out in IEEE mode while
# it builds it, as clang does at -O1 with constants it can see.
OPAQUE = 'static uint64_t opq(uint64_t v) { volatile uint64_t x = v; return x; }\n'


def regs(**values):
    return ' '.join(f'c->f[{int(r[1:])}].u = opq(0x{v:X}ull); c->ps1[{int(r[1:])}] = d(opq(0x{v:X}ull));'
                    for r, v in values.items())


# label, the C that sets the registers, the C to run, what to print, IEEE's bits, non-IEEE's bits
CASES = [
    ('a lerp, fsubs then fmadds', regs(f4=0, f5=TINY, f0=NEG_T),
     ppc.translate(PC, a_form(59, 20, d=3, a=4, b=5, c=0)) + ' '
     + ppc.translate(PC + 4, a_form(59, 29, d=1, a=3, b=5, c=0)),
     'c->f[1].u', 0x36CC000000000000, 0),
    ('fmuls to a negative single denormal', regs(f2=0xBB90000000000000, f4=0x3B90000000000000),
     ppc.translate(PC, a_form(59, 25, d=1, a=2, c=4)), 'c->f[1].u',
     0xB730000000000000, 0x8000000000000000),
    ('fadd of a double denormal', regs(f2=DOUBLE_DENORMAL, f3=DOUBLE_DENORMAL),
     ppc.translate(PC, a_form(63, 21, d=1, a=2, b=3, c=0)), 'c->f[1].u', 0x0000000000000002, 0),
    ('fmul of a double denormal', regs(f2=DOUBLE_DENORMAL, f4=0x4330000000000000),
     ppc.translate(PC, a_form(63, 25, d=1, a=2, c=4)), 'c->f[1].u', 0x0010000000000000, 0),
    ('a normal result is the same', regs(f2=0x3FF8000000000000, f3=0x3FF0000000000000),
     ppc.translate(PC, a_form(59, 21, d=1, a=2, b=3, c=0)), 'c->f[1].u',
     0x4004000000000000, 0x4004000000000000),
    ('fres of a double denormal', regs(f3=DOUBLE_DENORMAL),
     ppc.translate(PC, a_form(59, 24, d=1, a=0, b=3, c=0)), 'c->f[1].u', GREATEST_SINGLE, INF),
    ('frsqrte of a negative double denormal', regs(f3=0x8000000000000001),
     ppc.translate(PC, a_form(63, 26, d=1, a=0, b=3, c=0)), 'c->f[1].u', QNAN, NEG_INF),
    ('fcmpu of a double denormal against zero', regs(f2=DOUBLE_DENORMAL, f3=0),
     ppc.translate(PC, 63 << 26 | 0 << 21 | 2 << 16 | 3 << 11), 'c->cr[0]', 4, 2),
    ('lfs of a single denormal', '', '', 'u(gekko_single_value((uint32_t)opq(0x00000004u)))', TINY, TINY),
    ('lfs of a negative single denormal', '', '', 'u(gekko_single_value((uint32_t)opq(0x80000007u)))',
     0xB6CC000000000000, 0xB6CC000000000000),
    ('stfs of a single denormal', '', '', 'gekko_single_bits(d(opq(0x36CC000000000000ull)))', 7, 7),
    # 7.6 times 2^-149: the PEM drops the .6, where rounding to nearest would give 8.
    ('stfs drops what denormalizing shifts out', '', '',
     'gekko_single_bits(d(opq(0xB6CE666666666666ull)))', 0x80000007, 0x80000007),
    ('stfs just under the least normal single', '', '',
     'gekko_single_bits(d(opq(0x380FFFFFFFFFFFFFull)))', 0x007FFFFF, 0x007FFFFF),
    ('stfs under 2^-149', '', '', 'gekko_single_bits(d(opq(0x368FFFFFFFFFFFFFull)))', 0, 0),
    ('stfs of a negative double denormal', '', '', 'gekko_single_bits(d(opq(0x8000000000000001ull)))',
     0x80000000, 0x80000000),
    ('stfs of the least normal single', '', '', 'gekko_single_bits(d(opq(0x3810000000000000ull)))',
     0x00800000, 0x00800000),
]


class Run(unittest.TestCase):
    def test_each_case_in_both_modes(self):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('no C compiler')
        lines = []
        for mode in (0, 1):
            lines.append(f'    gcn_fp_follow({mode} ? FPSCR_NI : 0);')
            lines.append(f'    printf("flushing %d\\n", gcn_fp_flushing());')
            for n, (_, setup, run, show, _, _) in enumerate(CASES):
                lines.append(f'    memset(c, 0, sizeof(*c)); {setup} {run}')
                lines.append(f'    printf("{mode} {n} %llX\\n", (unsigned long long)({show}));')
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = os.path.join(tmp, 'run.c'), os.path.join(tmp, 'run')
            with open(src, 'w') as f:
                f.write(HARNESS.replace('int main(void) {', OPAQUE + 'int main(void) {')
                        .replace('@CASES@', '\n'.join(lines)))
            r = subprocess.run([cc, '-std=gnu11', '-O1', '-ffp-contract=off', '-I', RUNTIME, src, '-o', exe,
                                '-lm'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.splitlines()
        if out[len(CASES) + 1] == 'flushing 0':
            self.skipTest('this host has no flush mode recomp.h sets')
        self.assertEqual((out[0], out[len(CASES) + 1]), ('flushing 0', 'flushing 1'))
        got = {}
        for line in out:
            if not line.startswith('flushing'):
                mode, n, bits = line.split()
                got[int(mode), int(n)] = int(bits, 16)
        for n, (label, _, _, _, ieee, non_ieee) in enumerate(CASES):
            with self.subTest(label=label):
                self.assertEqual((got[0, n], got[1, n]), (ieee, non_ieee))


if __name__ == '__main__':
    unittest.main()
