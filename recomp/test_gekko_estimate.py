#!/usr/bin/env python3
"""Checks that ppc.translate gives the 750's estimates for fres, frsqrte, ps_res and ps_rsqrte.
Run them with `python3 -m unittest discover -s recomp -p 'test_*.py'`.

Each reads frB through GEKKO_FRES or GEKKO_FRSQRTE (runtime/recomp.h), in each lane it writes,
and none divides or takes a square root. Then translate's C is built and run: fres writes its
estimate to both lanes, frsqrte to ps0 alone, and the paired forms take each lane's, ps_rsqrte's
rounded to the nearest single. tools/test_gekko_estimate.cpp checks the estimates themselves.
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
from test_round_frc import HARNESS, PC, RUNTIME, B, D, a_form  # noqa: E402

# By primary and 5-bit extended opcode: the helper, and the frB lanes it reads.
ESTIMATES = {
    (59, 24): ('GEKKO_FRES', (f'c->f[{B}].d',)),
    (63, 26): ('GEKKO_FRSQRTE', (f'c->f[{B}].d',)),
    (4, 24): ('GEKKO_FRES', (f'c->f[{B}].d', f'c->ps1[{B}]')),
    (4, 26): ('GEKKO_FRSQRTE', (f'c->f[{B}].d', f'c->ps1[{B}]')),
}

THREE, NEG_SEVEN_QUARTERS = 0x4008000000000000, 0xBFFC000000000000
TWO, FOUR, POINT_THREE = 0x4000000000000000, 0x4010000000000000, 0x3FD3333333333333
UNTOUCHED = 0x3FF0000020000000


class Translate(unittest.TestCase):
    def test_each_estimate_reads_frb_through_its_helper(self):
        for (op, xo), (helper, lanes) in ESTIMATES.items():
            with self.subTest(op=op, xo=xo):
                text = ppc.translate(PC, a_form(op, xo, a=0, c=0))
                for lane in lanes:
                    self.assertIn(f'{helper}({lane})', text)
                self.assertNotIn('1.0 /', text)
                self.assertNotIn('sqrt(', text)


class Run(unittest.TestCase):
    """translate's C for each case, built against runtime/recomp.h and run from set registers."""

    # label, word, frB's (ps0, ps1), the (ps0, ps1) fD should hold
    CASES = [
        ('fres', a_form(59, 24, a=0, c=0), (THREE, TWO), (0x3FD5550000000000, 0x3FD5550000000000)),
        ('frsqrte', a_form(63, 26, a=0, c=0), (TWO, FOUR), (0x3FE69FA000000000, UNTOUCHED)),
        ('ps_res', a_form(4, 24, a=0, c=0), (THREE, NEG_SEVEN_QUARTERS), (0x3FD5550000000000, 0xBFE2488000000000)),
        # frsqrte(0.3) is 0x3FFD368178000000, over halfway to the next single.
        ('ps_rsqrte', a_form(4, 26, a=0, c=0), (POINT_THREE, FOUR), (0x3FFD368180000000, 0x3FDFFE8000000000)),
    ]

    def test_estimates_land_in_the_lanes_the_hardware_writes(self):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('no C compiler')
        lines = []
        for label, word, (p0, p1), _ in self.CASES:
            lines.append('    memset(c, 0, sizeof(*c));')
            lines.append(f'    c->f[{D}].u = 0x{UNTOUCHED:X}ull; c->ps1[{D}] = d(0x{UNTOUCHED:X}ull);')
            lines.append(f'    c->f[{B}].u = 0x{p0:X}ull; c->ps1[{B}] = d(0x{p1:X}ull);')
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
