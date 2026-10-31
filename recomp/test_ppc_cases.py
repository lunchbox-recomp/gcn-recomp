#!/usr/bin/env python3
"""Checks for ppc_cases.py, which writes the interpreter's test cases. Run them with
`python3 -m unittest discover -s recomp -p 'test_*.py'`.

Every word here is generated, and nothing is read from a disc.
"""
import collections
import os
import random
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ppc  # noqa: E402
import ppc_cases  # noqa: E402

SCRIPT = os.path.join(HERE, 'ppc_cases.py')
CPU_CPP = os.path.join(os.path.dirname(HERE), 'runtime', 'cpu.cpp')
K = 5
D, A, B, C, M = 21, 16, 11, 6, 1


def field(i, shift):
    return (i >> shift) & 31


def is_free(form, shift):
    return form.mask & (31 << shift) == 0


def accepted(pc, i):
    try:
        if ppc.decode_branch(pc, i) is not None:
            return True
    except ppc.Unimpl:
        pass
    try:
        ppc.translate(pc, i)
        return True
    except ppc.Unimpl:
        return False


def run(out, seed, per_form):
    subprocess.run([sys.executable, '-B', SCRIPT, out, '--seed', str(seed), '--per-form', str(per_form)],
                   check=True)


def read_all(out):
    data = {}
    for name in sorted(os.listdir(out)):
        with open(os.path.join(out, name), 'rb') as f:
            data[name] = f.read()
    return data


class Generated(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cases = ppc_cases.generate(7, K)
        cls.words = collections.defaultdict(list)
        for c in cls.cases.steps + cls.cases.branches:
            cls.words[c.form].append(c.insn)

    def test_steps_hold_translate_text(self):
        for s in self.cases.steps:
            self.assertEqual(s.text, ppc.translate(s.pc, s.insn), f'{s.insn:08X}')

    def test_branches_hold_what_decode_branch_says(self):
        for b in self.cases.branches:
            br = ppc.decode_branch(b.pc, b.insn)
            self.assertEqual((b.branch.kind, b.branch.link, b.branch.target, b.branch.cond, b.branch.ctr_dec),
                             (br.kind, br.link, br.target, br.cond, br.ctr_dec), f'{b.insn:08X}')

    def test_refused_words_are_refused_by_both(self):
        self.assertTrue(self.cases.refused)
        for r in self.cases.refused:
            self.assertFalse(accepted(r.pc, r.insn), f'{r.insn:08X}')

    def test_each_case_is_in_its_form(self):
        for c in self.cases.steps + self.cases.branches:
            form = self.cases.forms[c.form]
            self.assertEqual(c.insn & form.mask, form.value, f'{c.insn:08X}')
            self.assertEqual(ppc_cases.form_of(c.insn), (form.mask, form.value))

    def test_every_accepted_form_has_k_cases(self):
        count = collections.Counter()
        for c in self.cases.steps + self.cases.branches:
            form = self.cases.forms[c.form]
            count[form.mask, form.value] += 1
        rng = random.Random(3)
        for op in range(64):
            for low in range(2048):
                i = op << 26 | low | (rng.getrandbits(32) & 0x03FFF800)
                if accepted(0x80001000, i):
                    self.assertGreaterEqual(count[ppc_cases.form_of(i)], K, f'{i:08X}')

    def test_forms_split_where_translate_does(self):
        def form(i):
            return ppc_cases.form_of(i)
        add, addo, add_rc = 0x7C632214, 0x7C632614, 0x7C632215
        self.assertEqual(len({form(add), form(addo), form(add_rc)}), 3)
        self.assertEqual(form(add), form(add ^ 0x03FFF800))  # registers
        fadd_c1, fadd_c2 = 0xFC22007A, 0xFC2200BA  # an A-form's C field is an operand
        self.assertEqual(form(fadd_c1), form(fadd_c2))
        fcmpu, fcmpo = 0xFC011000, 0xFC011040  # an X-form's bits 6-10 are its opcode
        self.assertNotEqual(form(fcmpu), form(fcmpo))
        psq_lx, psq_lux = 0x1022000C, 0x1022004C
        self.assertNotEqual(form(psq_lx), form(psq_lux))
        self.assertEqual(form(psq_lx), form(psq_lx | 0x780))  # W and I
        bc, bcl, bca = 0x41820010, 0x41820011, 0x41820012
        self.assertEqual(len({form(bc), form(bcl), form(bca)}), 3)

    def test_families(self):
        expect = {0x38630004: 'int', 0x7C632214: 'int', 0x4C221A02: 'int', 0x80630000: 'mem',
                  0x7C63202E: 'mem', 0x7C001FEC: 'mem', 0x7C6802A6: 'sys', 0x44000002: 'sys',
                  0x4C000064: 'sys', 0x48000001: 'branch', 0x4E800421: 'branch', 0xFC22182A: 'fp',
                  0xEC22182A: 'fp', 0x1022182A: 'ps', 0xC0230000: 'fpmem', 0x7C23242E: 'fpmem',
                  0x1022000C: 'fpmem', 0xE0230000: 'fpmem'}
        for i, fam in expect.items():
            self.assertEqual(ppc_cases.family(i), fam, f'{i:08X}')
        for c in self.cases.steps + self.cases.branches:
            form = self.cases.forms[c.form]
            self.assertEqual(ppc_cases.FAMILIES[form.family], ppc_cases.family(c.insn))

    def test_edge_fields_are_present(self):
        for n, form in enumerate(self.cases.forms):
            if ppc_cases.FAMILIES[form.family] == 'branch':
                continue  # their fields are BO, BI and a displacement, below
            words = self.words[n]
            if is_free(form, A):
                self.assertIn(0, [field(w, A) for w in words], f'rA 0 in {form.name}')
            if is_free(form, D) and is_free(form, A):
                self.assertTrue(any(field(w, D) == field(w, A) for w in words), f'rD = rA in {form.name}')
            if is_free(form, B):
                shifts = {field(w, B) for w in words}
                self.assertTrue({0, 31} <= shifts, f'shifts 0 and 31 in {form.name}')
            if is_free(form, C) and is_free(form, M):
                self.assertTrue(any(field(w, C) > field(w, M) for w in words), f'MB past ME in {form.name}')
        rlw = [f for f in self.cases.forms if f.value >> 26 in (20, 21, 23)]
        self.assertEqual(len(rlw), 6)

    def test_fp_edge_fields_are_present(self):
        a_forms = psq = mtfsf = 0
        for n, form in enumerate(self.cases.forms):
            words = self.words[n]
            if is_free(form, C) and not is_free(form, M):  # an A-form, whose C field is a register
                a_forms += 1
                for x, name in ((D, 'rD'), (A, 'rA'), (B, 'rB')):
                    self.assertTrue(any(field(w, x) == field(w, C) for w in words), f'{name} = rC in {form.name}')
            op = form.value >> 26
            if op in ppc_cases.PSQ_D or form.mask == 0xFC00007E:
                psq += 1
                w_at, i_at = (15, 12) if op in ppc_cases.PSQ_D else (10, 7)
                seen = {(w >> w_at & 1, w >> i_at & 7) for w in words}
                self.assertEqual(seen, {(w, i) for w in (0, 1) for i in range(8)}, f'W and I in {form.name}')
            if op == 63 and form.mask == 0xFC0007FF and (form.value >> 1) & 0x3FF == 711:
                mtfsf += 1
                self.assertTrue({0, 0xFF, 0x80, 0x01} <= {w >> 17 & 0xFF for w in words}, form.name)
        self.assertGreater(a_forms, 40)
        self.assertEqual(psq, 8)
        self.assertEqual(mtfsf, 2)

    def test_every_spr_the_runtime_handles(self):
        with open(CPU_CPP) as f:
            named = {int(v) for v in re.findall(r'SPR_\w+ = (\d+)', f.read())}
        self.assertTrue(named)
        own = {ppc.SPR_XER, ppc.SPR_LR, ppc.SPR_CTR} | set(range(912, 920))
        self.assertTrue(named | own <= set(ppc_cases.SPRS))
        spr_forms = [n for n, f in enumerate(self.cases.forms)
                     if f.value >> 26 == 31 and (f.value >> 1) & 0x3FF in (339, 371, 467)]
        self.assertEqual(len(spr_forms), 6)
        for n in spr_forms:
            sprs = {field(w, A) | field(w, B) << 5 for w in self.words[n]}
            self.assertTrue(set(ppc_cases.SPRS) <= sprs, self.cases.forms[n].name)

    def test_branch_edges(self):
        bos = collections.defaultdict(set)
        for b in self.cases.branches:
            bos[b.form].add(field(b.insn, D))
        for n, form in enumerate(self.cases.forms):
            if form.value >> 26 in (16, 19) and ppc_cases.FAMILIES[form.family] == 'branch':
                kept = {bo for bo in ppc_cases.BO_EDGES if form.value >> 26 == 16 or bo & 4 or
                        (form.value >> 1) & 0x3FF == 16}
                self.assertTrue(kept <= bos[n], form.name)
        lis = {b.insn & 0x03FFFFFC for b in self.cases.branches if b.insn >> 26 == 18}
        self.assertTrue({0, 0x01FFFFFC, 0x02000000, 0x03FFFFFC} <= lis)
        pcs = {b.pc for b in self.cases.branches}
        self.assertTrue({0, 0xFFFFFFFC} <= pcs)
        # Each call that tests a condition has words not taken for some CR and CTR.
        conditional = {(b.branch.kind, b.branch.link) for b in self.cases.branches if b.branch.cond}
        for kind in ('bc', 'blr', 'bctr'):
            self.assertIn((kind, True), conditional)

    def test_memory_cases_stay_in_the_window(self):
        mem = [s for s in self.cases.steps if s.mem is not None]
        self.assertTrue(mem)
        fams = {ppc_cases.FAMILIES[self.cases.forms[s.form].family] for s in mem}
        self.assertEqual(fams, {'mem', 'fpmem', 'ps'})
        for s in mem:
            base, index, disp, span = s.mem
            self.assertEqual(base, field(s.insn, A))
            self.assertIn(index, (ppc_cases.NO_REG, field(s.insn, B)))
            self.assertTrue(0 < span <= 128)
            if base == 0 and index == ppc_cases.NO_REG:
                self.assertTrue(0 <= disp and disp + span <= ppc_cases.WINDOW, f'{s.insn:08X}')
        lmw = [s for s in mem if s.insn >> 26 == 46]
        self.assertTrue(all(s.mem[3] == 4 * (32 - field(s.insn, D)) for s in lmw))
        rest = [s for s in self.cases.steps if s.mem is None]
        touch = r'\b(LD|ST)[SF]?[0-9]|hle_(dcb|lswi|stswi)|psq_(load|store)'
        self.assertFalse([s.text for s in rest if re.search(touch, s.text)])
        self.assertTrue(all(re.search(touch, s.text) or s.text == ';' for s in mem))


class Written(unittest.TestCase):
    def test_same_seed_same_files(self):
        with tempfile.TemporaryDirectory() as t:
            one, two, other = (os.path.join(t, n) for n in ('one', 'two', 'other'))
            run(one, 11, 2)
            run(two, 11, 2)
            run(other, 12, 2)
            first = read_all(one)
            self.assertEqual(sorted(first), sorted(ppc_cases.FILES))
            self.assertEqual(first, read_all(two))
            self.assertNotEqual(first, read_all(other))

    def test_files_name_each_case_once(self):
        cases = ppc_cases.generate(5, 2)
        with tempfile.TemporaryDirectory() as t:
            ppc_cases.write(t, cases)
            text = read_all(t)
        steps = sum(v.count(b'\n\t{0x') for k, v in text.items() if k.startswith('cases_') and k != 'cases_index.c')
        self.assertEqual(steps, len(cases.steps) + len(cases.branches))
        index = text['cases_index.c'].decode()
        self.assertIn(f'g_case_form_count = {len(cases.forms)}u;', index)
        self.assertIn(f'g_case_refused_count = {len(cases.refused)}u;', index)
        self.assertIn('g_case_seed = 5u', index)
        for s in cases.steps[:50]:
            self.assertIn(s.text.encode(), text[f'cases_{ppc_cases.FAMILIES[cases.forms[s.form].family]}.c'])

    def test_bad_arguments(self):
        r = subprocess.run([sys.executable, '-B', SCRIPT, '--seed', 'x'], capture_output=True)
        self.assertEqual(r.returncode, 2)


if __name__ == '__main__':
    unittest.main()
