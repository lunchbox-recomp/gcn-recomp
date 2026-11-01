#!/usr/bin/env python3
"""Checks for the entry guards and the interpreter's tables that recomp.py writes. Run them with
`python3 -m unittest discover -s recomp -p 'test_*.py'`.

The DOLs are assembled here from hand-encoded words, so no game is needed. Every address, size,
word and name in the fixture is made up, apart from the SDK's OSLink. The last class compiles the
output with the C compiler and runs it, to see each guard hand over its own entry.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import warnings
from types import SimpleNamespace

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import interp_tables  # noqa: E402
import ppc  # noqa: E402
import rel  # noqa: E402
from dol import Dol  # noqa: E402
from recomp import FUNCS_PER_FILE  # noqa: E402

RECOMP = os.path.join(HERE, 'recomp.py')
RUNTIME = os.path.join(os.path.dirname(HERE), 'runtime')

NOP = 0x60000000
BLR = 0x4E800020


def li(r, v):
    return 0x38000000 | r << 21 | (v & 0xFFFF)


def addi(r, v):
    return 0x38000000 | r << 21 | r << 16 | (v & 0xFFFF)


def bl(src, dst):
    return 0x48000001 | ((dst - src) & 0x03FFFFFC)


CMPWI_R3_0 = 0x2C030000
BNE_BACK_8 = 0x4082FFF8  # bne to the word two back
BL_BACK_8 = 0x4BFFFFF9   # bl to the word two back

# The main text section. f_caller's two calls land inside f_multi, which gives f_multi entries
# at +0xC and +0x10. The nop at +0x5C is in no function.
T = 0x80104000
T_WORDS = [
    li(3, 1), BLR,                                     # 00 f_plain
    li(3, 2), addi(3, 1), BLR,                         # 08 f_multi
    BL_BACK_8, BL_BACK_8, BLR,                         # 14 f_caller
    li(3, 3), addi(3, -1), CMPWI_R3_0, BNE_BACK_8, BLR,  # 20 f_loop
    addi(3, 1), BLR,                                   # 34 f_step
    li(3, 4), BLR,                                     # 3C made_up_hle
    li(4, 0), BLR,                                     # 44 made_up_longjmp
    li(3, 0), BLR,                                     # 4C made_up_setjmp
    li(3, 0), BLR,                                     # 54 made_up_save
    NOP,                                               # 5C
]
# A second text section, which the DOL header lists first. The addi at +0x14 is in no function.
# f_sites calls made_up_setjmp and made_up_save, so both calls become resume points.
T2 = 0x80108000
T2_WORDS = [
    li(3, 6), addi(3, -1), CMPWI_R3_0, BNE_BACK_8, BLR,  # 00 f_far
    addi(3, 1), NOP, NOP,                               # 14
    bl(T2 + 0x20, T + 0x4C), bl(T2 + 0x24, T + 0x54), BLR,  # 20 f_sites
    NOP,
]
# Enough two-word functions that the recompiled code fills more than one file. OSLink's name
# gives it the hook the SDK's module linker gets when there are modules.
T3 = 0x8010C000
FILLERS = FUNCS_PER_FILE
T3_WORDS = [w for k in range(FILLERS) for w in (li(3, k), BLR)]
# A module's and a relocatable module's code, at canonical addresses where RAM doesn't hold it.
MOD = 0x80200000
MOD_WORDS = [li(3, 8), BLR]
REL = 0x80300000
REL_WORDS = [li(3, 9), BLR]
DATA = 0x80400000

# (name, start, size, flags), sorted by start, so the index in this list is the function's N.
FUNCS = [
    ('f_plain', T + 0x00, 0x08, '0'),
    ('f_multi', T + 0x08, 0x0C, '0'),
    ('f_caller', T + 0x14, 0x0C, '0'),
    ('f_loop', T + 0x20, 0x14, '0'),
    ('f_step', T + 0x34, 0x08, '0'),
    ('made_up_hle', T + 0x3C, 0x08, 'RECOMP_RANGE_HLE'),
    ('made_up_longjmp', T + 0x44, 0x08, 'RECOMP_RANGE_LONGJMP'),
    ('made_up_setjmp', T + 0x4C, 0x08, '0'),
    ('made_up_save', T + 0x54, 0x08, '0'),
    ('f_far', T2, 0x14, '0'),
    ('f_sites', T2 + 0x20, 0x0C, '0'),
] + [('OSLink' if k == 0 else f'filler_{k:03d}', T3 + 8 * k, 0x08, '0') for k in range(FILLERS)] + [
    ('made_up_module_fn', MOD, 0x08, 'RECOMP_RANGE_NO_GUARD'),
    ('made_up_rel_fn', REL, 0x08, 'RECOMP_RANGE_NO_GUARD'),
]
N = {name: k for k, (name, _, _, _) in enumerate(FUNCS)}
START = {name: a for name, a, _, _ in FUNCS}
MULTI_ENTRIES = [T + 0x08, T + 0x0C, T + 0x10]
UNGUARDED = {'made_up_module_fn', 'made_up_rel_fn'}

PATCHES = [
    f'{T + 0x00:08X} c->r[3] = 7;',
    f'{T + 0x10:08X} c->r[3] = 9; RET();',
    f'{T + 0x20:08X} c->r[3] = 3; goto L_{T + 0x24:08X};',
    f'{T + 0x5C:08X} c->r[3] = 5;',
]
GOTO_SITE = T + 0x20
STEP = T + 0x34
OUTSIDE = (T + 0x5C, T2 + 0x14)  # a patch and a step in no function


def section_bytes(words):
    return struct.pack(f'>{len(words)}I', *words)


def write_dol(path, text):
    """A DOL holding the text sections `text` [(addr, words)] in that slot order, and one data
    section of zeros, which hold no address a scan could take for a branch target. rel.py's
    writer leaves its file to be closed by the collector, which Python warns about."""
    with warnings.catch_warnings():
        warnings.simplefilter('ignore', ResourceWarning)
        rel.write_dol(path, [(a, section_bytes(w), True) for a, w in text] + [(DATA, bytes(0x20), False)],
                      (DATA + 0x100, 0x100), text[0][0])


def write(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(content)


def read(path):
    with open(path) as f:
        return f.read()


def write_fixture(d):
    """Writes the fixture's DOL with its side files, its symbols and its tables under d, and
    returns their paths as recomp.py takes them. The symbols are out of address order."""
    dol = os.path.join(d, 'main.dol')
    write_dol(dol, [(T2, T2_WORDS), (T, T_WORDS), (MOD, MOD_WORDS), (T3, T3_WORDS), (REL, REL_WORDS)])
    write(dol + '.modules', f'{MOD:08X} {len(MOD_WORDS) * 4:X} 0 made_up_module\n')
    write(dol + '.rels', f'module 0 1 made_up_rel\nsection 1 {REL:08X} {len(REL_WORDS) * 4:X} 1\n')
    write(dol + '.relocs', '')
    symbols = os.path.join(d, 'symbols.txt')
    write(symbols, ''.join(f'{name} = .text:0x{a:08X}; // type:function size:0x{size:X} scope:global\n'
                           for name, a, size, _ in FUNCS[1::2] + FUNCS[::2]))
    tables = os.path.join(d, 'tables')
    write(os.path.join(tables, 'hle.txt'), 'made_up_hle\n')
    # Out of order, as the sorted lists mustn't depend on the files' order.
    write(os.path.join(tables, 'special_calls.txt'),
          f'{T + 0x54:08X} made_up_save\n{T + 0x44:08X} longjmp\n{T + 0x4C:08X} setjmp\n')
    write(os.path.join(tables, 'idle.txt'), f'{T2 + 0x0C:08X} far_spin\n{T + 0x2C:08X} spin\n')
    write(os.path.join(tables, 'patches.txt'), '\n'.join(PATCHES) + '\n')
    # The step at T shares its address with a patch, which wins.
    write(os.path.join(tables, 'steps.txt'), f'{STEP:08X} A\n{T:08X} A\n{OUTSIDE[1]:08X} A\n')
    return dol, symbols, tables


def cc_command(cc, out, harness, sources, exe, flags=()):
    """The command line that builds `harness` and recomp.py's `sources`, which it wrote to `out`,
    into `exe`. The flags are the game's, with the guest call stack on so ENTER and EXIT show;
    `flags` come after them."""
    return ([cc, '-std=gnu11', '-O1', '-DGCN_TRACE_CALLS', '-fno-strict-aliasing', '-ffp-contract=off',
             '-Wno-unused-label', '-Wno-unused-variable', '-Wno-parentheses-equality'] + list(flags)
            + ['-I', RUNTIME, '-I', out, harness] + list(sources) + ['-o', exe, '-lm'])


def guard(n, entry):
    return f'\tif (UNLIKELY(GCN_CODE_CHANGED({n}))) {{ interp_enter(c, {entry}); RET(); }}\n'


def rows(text, table):
    """The rows of `table`'s initializer in recomp_interp.c, as lists of their fields."""
    m = re.search(r'\b' + table + r'\[\] = \{\n(.*?)\};\n', text, re.S)
    assert m, table
    return [[x.strip() for x in line.strip().strip('{},').split(',')]
            for line in m.group(1).splitlines() if line.strip()]


def count(text, name):
    m = re.search(r'const uint32_t ' + name + r' = (\d+);', text)
    assert m, name
    return int(m.group(1))


class Recompiled(unittest.TestCase):
    """recomp.py run once on the whole fixture. self.c is every recomp_NNN.c that sources.cmake
    lists, in its order."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = cls.tmp.name
        cls.out = os.path.join(cls.dir, 'out')
        dol, symbols, tables = write_fixture(cls.dir)
        r = subprocess.run([sys.executable, '-B', RECOMP, dol, symbols, cls.out, tables],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        cls.stderr = r.stderr
        cls.sources = re.findall(r'\$\{RECOMP_GEN_DIR\}/(\S+)', read(os.path.join(cls.out, 'sources.cmake')))
        cls.code_files = [f for f in cls.sources if re.fullmatch(r'recomp_\d{3}\.c', f)]
        cls.c = ''.join(read(os.path.join(cls.out, f)) for f in cls.code_files)
        cls.interp = read(os.path.join(cls.out, 'recomp_interp.c'))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()


class Guards(Recompiled):
    def test_the_functions_fill_more_than_one_file(self):
        self.assertEqual(self.code_files, ['recomp_000.c', 'recomp_001.c'])

    def test_a_single_entry_body_has_its_guard_right_after_enter(self):
        others = {'f_multi', 'made_up_hle', 'made_up_longjmp', 'OSLink'} | UNGUARDED
        for name, a, _, _ in FUNCS:
            if name not in others:
                self.assertIn(f'void fn_{a:08X}(CPU* c) {{\n\tENTER(0x{a:08X}u);\n' + guard(N[name], f'0x{a:08X}u')
                              + f'\t/* {a:08X} ', self.c)

    def test_a_multi_entry_body_has_one_guard_passing_the_entry_and_every_entry_reaches_it(self):
        a = START['f_multi']
        self.assertIn(f'static void body_{a:08X}(CPU* c, uint32_t entry) {{\n\tENTER(entry);\n'
                      + guard(N['f_multi'], 'entry') + '\tswitch (entry) {\n', self.c)
        for e in MULTI_ENTRIES:
            self.assertIn(f'void fn_{e:08X}(CPU* c) {{ body_{a:08X}(c, 0x{e:08X}u); }}', self.c)
        self.assertEqual(self.c.count(f'GCN_CODE_CHANGED({N["f_multi"]})'), 1)

    def test_an_hle_wrapper_has_no_guard_and_its_original_body_has_one(self):
        a = START['made_up_hle']
        self.assertIn(f'void fn_{a:08X}(CPU* c) {{ hle_made_up_hle(c); }}', self.c)
        self.assertIn(f'void orig_made_up_hle(CPU* c) {{\n\tENTER(0x{a:08X}u);\n'
                      + guard(N['made_up_hle'], f'0x{a:08X}u'), self.c)

    def test_a_module_linker_hook_has_no_guard_and_its_original_body_has_one(self):
        a = START['OSLink']
        self.assertIn(f'void fn_{a:08X}(CPU* c) {{ uint32_t m = c->r[3]; rels_linking(m); orig_OSLink(c); '
                      'if (c->r[3]) rels_linked(m); }', self.c)
        self.assertIn(f'void orig_OSLink(CPU* c) {{\n\tENTER(0x{a:08X}u);\n' + guard(N['OSLink'], f'0x{a:08X}u'),
                      self.c)

    def test_the_guard_comes_before_longjmps_jmp_buf(self):
        a = START['made_up_longjmp']
        self.assertIn(f'\tENTER(0x{a:08X}u);\n' + guard(N['made_up_longjmp'], f'0x{a:08X}u')
                      + '\tconst uint32_t jb = c->r[3];', self.c)

    def test_setjmp_and_context_call_sites_follow_the_guard(self):
        self.assertIn(f'if (GCN_SETJMP(*hle_setjmp_jmpbuf(c)) == 0) fn_{START["made_up_setjmp"]:08X}(c);', self.c)
        self.assertIn(f'if (GCN_SETJMP(*hle_context_jmpbuf(c)) == 0) fn_{START["made_up_save"]:08X}(c);', self.c)

    def test_module_and_rel_code_has_no_guard(self):
        for name, words in (('made_up_module_fn', MOD_WORDS), ('made_up_rel_fn', REL_WORDS)):
            a = START[name]
            self.assertIn(f'void fn_{a:08X}(CPU* c) {{\n\tENTER(0x{a:08X}u);\n\t/* {a:08X} {words[0]:08X} */', self.c)

    def test_every_other_function_has_exactly_one_guard(self):
        found = sorted(int(n) for n in re.findall(r'GCN_CODE_CHANGED\((\d+)\)', self.c))
        self.assertEqual(found, sorted(N[name] for name, _, _, _ in FUNCS if name not in UNGUARDED))

    def test_native_code_keeps_its_patches_inline(self):
        self.assertIn(f'/* {T:08X} {li(3, 1):08X} PATCHED */ c->r[3] = 7;', self.c)
        self.assertIn(f'/* {GOTO_SITE:08X} {li(3, 3):08X} PATCHED */ c->r[3] = 3; goto L_{T + 0x24:08X};', self.c)


class Tables(Recompiled):
    def test_sources_cmake_lists_the_tables(self):
        self.assertEqual(self.sources[-2:], ['recomp_table.c', 'recomp_interp.c'])

    def test_the_ranges_follow_the_symbols_sorted_with_their_flags(self):
        want = [[f'0x{a:08X}u', f'0x{a + size:08X}u', flags] for _, a, size, flags in FUNCS]
        self.assertEqual(rows(self.interp, 'g_recomp_ranges'), want + [['0', '0', '0']])
        self.assertEqual(count(self.interp, 'g_recomp_range_count'), len(FUNCS))

    def test_the_bits_start_zeroed_one_per_function(self):
        self.assertIn(f'uint8_t g_code_changed_bits[{(len(FUNCS) + 7) // 8}] = {{0}};', self.interp)

    def test_each_site_holds_its_text_and_the_word_it_replaced(self):
        want = [[f'0x{a:08X}u', f'0x{w:08X}u', f'site_{a:08X}']
                for a, w in ((T, li(3, 1)), (T + 0x10, BLR), (GOTO_SITE, li(3, 3)), (STEP, addi(3, 1)))]
        self.assertEqual(rows(self.interp, 'g_recomp_sites'), want + [['0', '0', '0']])
        self.assertEqual(count(self.interp, 'g_recomp_site_count'), 4)
        self.assertIn(f'static int site_{T:08X}(CPU* c) {{\n\t/* {T:08X} {li(3, 1):08X} PATCHED */ c->r[3] = 7;\n'
                      '\treturn 0;\n}', self.interp)
        self.assertIn(f'static int site_{T + 0x10:08X}(CPU* c) {{\n\t/* {T + 0x10:08X} {BLR:08X} PATCHED */ '
                      'c->r[3] = 9; RET();\n\treturn 0;\n}', self.interp)
        step = ppc.translate_step(STEP, addi(3, 1), 'A', 1.0)
        self.assertIn(f'static int site_{STEP:08X}(CPU* c) {{\n\t/* {STEP:08X} {addi(3, 1):08X} STEP */ {step}\n'
                      '\treturn 0;\n}', self.interp)

    def test_a_patch_or_step_in_no_function_gets_no_site(self):
        sites = [row[0] for row in rows(self.interp, 'g_recomp_sites')]
        for a in OUTSIDE:
            self.assertNotIn(f'0x{a:08X}u', sites)
            self.assertNotIn(f'site_{a:08X}', self.interp)
            self.assertNotIn(f'/* {a:08X} ', self.c)

    def test_inside_the_sites_ret_returns_1(self):
        define = self.interp.index('#undef RET\n#define RET() return 1\n')
        self.assertLess(define, self.interp.index('static int site_'))

    def test_a_patch_with_a_goto_gets_a_site_that_stops_and_a_warning(self):
        self.assertIn(f'static int site_{GOTO_SITE:08X}(CPU* c) {{\n\t/* {GOTO_SITE:08X} {li(3, 3):08X} PATCHED */ '
                      f'interp_site_unusable(c, 0x{GOTO_SITE:08X}u);\n\treturn 0;\n}}', self.interp)
        self.assertNotIn('goto', self.interp)
        lines = [line for line in self.stderr.splitlines() if 'goto' in line]
        self.assertEqual(len(lines), 1, self.stderr)
        self.assertIn(f'{GOTO_SITE:08X}', lines[0])
        self.assertTrue(lines[0].startswith('warning: '), lines[0])

    def test_idle_and_special_calls_are_sorted_lists(self):
        self.assertEqual(rows(self.interp, 'g_recomp_idle'), [[f'0x{T + 0x2C:08X}u'], [f'0x{T2 + 0x0C:08X}u'], ['0']])
        self.assertEqual(count(self.interp, 'g_recomp_idle_count'), 2)
        self.assertEqual(rows(self.interp, 'g_recomp_specials'),
                         [[f'0x{T + 0x44:08X}u', 'RECOMP_SPECIAL_LONGJMP'], [f'0x{T + 0x4C:08X}u', 'RECOMP_SPECIAL_SETJMP'],
                          [f'0x{T + 0x54:08X}u', 'RECOMP_SPECIAL_CONTEXT'], ['0', '0']])
        self.assertEqual(count(self.interp, 'g_recomp_special_count'), 3)

    def test_the_text_is_the_sections_in_ram_sorted_with_their_fnv(self):
        self.assertEqual(interp_tables.fnv1a64(b''), 0xCBF29CE484222325)
        self.assertEqual(interp_tables.fnv1a64(b'a'), 0xAF63DC4C8601EC8C)
        in_ram = ((T, T_WORDS), (T2, T2_WORDS), (T3, T3_WORDS))
        self.assertEqual(rows(self.interp, 'g_recomp_text'),
                         [[f'0x{a:08X}u', f'0x{len(w) * 4:X}u'] for a, w in in_ram] + [['0', '0']])
        self.assertEqual(count(self.interp, 'g_recomp_text_count'), 3)
        fnv = interp_tables.fnv1a64(b''.join(section_bytes(w) for _, w in in_ram))
        self.assertIn(f'const uint64_t g_recomp_text_fnv = 0x{fnv:016X}ull;', self.interp)


def func(addr, size, hle=False, guarded=True):
    """A stand-in for recomp.py's Func, with what interp_tables reads."""
    return SimpleNamespace(addr=addr, end=addr + size, hle=hle, guarded=guarded)


class Direct(unittest.TestCase):
    """interp_tables.source called on its own, with no recompile."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = tmp.name

    def dol(self, text):
        path = os.path.join(self.dir, 'main.dol')
        write_dol(path, text)
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', ResourceWarning)
            return Dol(path)

    def test_with_one_function_and_no_tables_the_lists_are_empty(self):
        text = interp_tables.source(self.dol([(T, [li(3, 1), BLR])]), [func(T, 8)], {}, {}, set(), {}, [])
        for table, end in (('g_recomp_sites', ['0', '0', '0']), ('g_recomp_idle', ['0']),
                           ('g_recomp_specials', ['0', '0'])):
            self.assertEqual(rows(text, table), [end])
        for name in ('g_recomp_site_count', 'g_recomp_idle_count', 'g_recomp_special_count'):
            self.assertEqual(count(text, name), 0)
        self.assertEqual(rows(text, 'g_recomp_ranges'), [[f'0x{T:08X}u', f'0x{T + 8:08X}u', '0'], ['0', '0', '0']])
        self.assertIn('uint8_t g_code_changed_bits[1] = {0};', text)
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if cc:
            out = os.path.join(self.dir, 'out')
            write(os.path.join(out, 'recomp_funcs.h'), '#pragma once\n#include "recomp.h"\n')
            write(os.path.join(out, 'recomp_interp.c'), text)
            r = subprocess.run([cc, '-std=gnu11', '-fsyntax-only', '-I', RUNTIME, '-I', out,
                                os.path.join(out, 'recomp_interp.c')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_a_text_section_that_overlaps_loaded_code_is_left_out(self):
        words = [li(3, 1), BLR]
        dol = self.dol([(T, words), (T2, words), (T3, words)])
        # One range ends where T starts, one covers T2's last word and one T3's first.
        loaded = [(T - 0x10, T), (T2 + 4, T2 + 0x100), (T3 - 4, T3 + 4)]
        text = interp_tables.source(dol, [func(T, 8)], {}, {}, set(), {}, loaded)
        self.assertEqual(rows(text, 'g_recomp_text'), [[f'0x{T:08X}u', '0x8u'], ['0', '0']])
        fnv = interp_tables.fnv1a64(section_bytes(words))
        self.assertIn(f'const uint64_t g_recomp_text_fnv = 0x{fnv:016X}ull;', text)


# Calls every entry of the fixture once with each function's bit set alone, and once with none
# set. A guard that fires at depth 1, which is ENTER's push, gets a fire line with the entry it
# handed to interp_enter. One reached through a native call fires deeper and gets none.
HARNESS = r'''
#include "recomp_funcs.h"
#include <stdio.h>
#include <stdlib.h>

volatile uint64_t g_vcount, g_vlimit = ~0ull;
void irq_poll(CPU* c) { (void)c; }
void idle_hint(CPU* c) { (void)c; }
void call_indirect(CPU* c, uint32_t a) { (void)c; printf("call_indirect %08X\n", a); }
void unimpl(CPU* c, uint32_t pc, uint32_t i) { (void)c; (void)i; printf("unimpl %08X\n", pc); exit(3); }
static jmp_buf setjmp_buf, context_buf;
jmp_buf* hle_setjmp_jmpbuf(CPU* c) { (void)c; return &setjmp_buf; }
jmp_buf* hle_context_jmpbuf(CPU* c) { (void)c; return &context_buf; }
/* The runtime's never returns. It puts back the CPU its setjmp saw, a frame further out. */
void hle_longjmp_resume(CPU* c, uint32_t jb) { (void)jb; wr_pop(c); }
void rels_linking(uint32_t m) { (void)m; }
void rels_linked(uint32_t m) { (void)m; }
uint32_t gcn_step_int(uint32_t pc, double p) { (void)pc; (void)p; return 1; }

static int hle_calls;
void hle_made_up_hle(CPU* c) { (void)c; hle_calls++; }
static uint32_t first, first_depth, unusable;
void interp_enter(CPU* c, uint32_t entry) {
    if (!first) { first = entry; first_depth = c->depth; }
}
void interp_site_unusable(CPU* c, uint32_t addr) { (void)c; unusable = addr; }

/* k is the bit set, and the range count means none. */
static void run(uint32_t k, const char* what, RecompFn fn) {
    static CPU cpu;
    CPU* c = &cpu;
    memset(g_code_changed_bits, 0, (g_recomp_range_count + 7) / 8);
    if (k < g_recomp_range_count) g_code_changed_bits[k >> 3] |= (uint8_t)(1u << (k & 7));
    memset(c, 0, sizeof(*c));
    first = first_depth = 0;
    hle_calls = 0;
    fn(c);
    if (first && first_depth == 1) printf("fire %u %s %08X\n", k, what, first);
    if (c->depth) printf("unbalanced %u %s %u\n", k, what, c->depth);
    if (k == g_recomp_range_count) printf("native %s %d %u %08X\n", what, hle_calls, c->r[3], first);
}

int main(void) {
    for (uint32_t k = 0; k <= g_recomp_range_count; k++) {
        for (uint32_t i = 0; i < g_recomp_func_count; i++) {
            char what[16];
            snprintf(what, sizeof(what), "%08X", g_recomp_funcs[i].addr);
            run(k, what, g_recomp_funcs[i].fn);
        }
        run(k, "orig_made_up_hle", orig_made_up_hle);
    }
    for (uint32_t i = 0; i < g_recomp_site_count; i++) {
        static CPU cpu;
        cpu.r[3] = 10;
        unusable = 0;
        int ret = g_recomp_sites[i].fn(&cpu);
        printf("site %08X %d %u %08X\n", g_recomp_sites[i].addr, ret, cpu.r[3], unusable);
    }
    return 0;
}
'''


class Compiled(Recompiled):
    """Every file sources.cmake lists, built with -DGCN_TRACE_CALLS so the guest call stack shows
    whether ENTER and EXIT balance, then run."""

    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            raise unittest.SkipTest('no C compiler')
        super().setUpClass()
        harness = os.path.join(cls.dir, 'harness.c')
        write(harness, HARNESS)
        exe = os.path.join(cls.dir, 'harness')
        srcs = [os.path.join(cls.out, f) for f in cls.sources]
        r = subprocess.run(cc_command(cc, cls.out, harness, srcs, exe), capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
        assert r.returncode == 0, r.stdout + r.stderr
        cls.fires, cls.unbalanced, cls.native, cls.sites, cls.other = set(), [], {}, {}, []
        for line in r.stdout.splitlines():
            f = line.split()
            if f[0] == 'fire':
                cls.fires.add((int(f[1]), f[2], int(f[3], 16)))
            elif f[0] == 'unbalanced':
                cls.unbalanced.append(line)
            elif f[0] == 'native':
                cls.native[f[1]] = (int(f[2]), int(f[3]), int(f[4], 16))
            elif f[0] == 'site':
                cls.sites[int(f[1], 16)] = (int(f[2]), int(f[3]), int(f[4], 16))
            else:
                cls.other.append(line)

    def owner(self, addr):
        for k, (_, a, size, _) in enumerate(FUNCS):
            if a <= addr < a + size:
                return k
        raise KeyError(hex(addr))

    def entries(self):
        return sorted({a for _, a, _, _ in FUNCS} | set(MULTI_ENTRIES))

    def test_each_guard_fires_on_its_own_bit_alone_and_hands_over_its_entry(self):
        want = {(self.owner(e), f'{e:08X}', e) for e in self.entries()
                if FUNCS[self.owner(e)][0] not in UNGUARDED and e != START['made_up_hle']}
        want.add((N['made_up_hle'], 'orig_made_up_hle', START['made_up_hle']))
        self.assertEqual(self.fires, want)

    def test_enter_and_exit_balance_in_every_run(self):
        self.assertEqual(self.unbalanced, [])
        self.assertEqual(self.other, [])

    def test_with_no_bit_set_the_code_runs_natively(self):
        self.assertEqual(sorted(self.native), sorted([f'{e:08X}' for e in self.entries()] + ['orig_made_up_hle']))
        self.assertEqual({first for _, _, first in self.native.values()}, {0})
        self.assertEqual(self.native[f'{T:08X}'][1], 7)  # f_plain, with its patch inline
        self.assertEqual(self.native[f'{START["made_up_hle"]:08X}'][0], 1)  # the wrapper runs the replacement
        self.assertEqual(self.native[f'{START["f_sites"]:08X}'][1], 0)  # both resume points taken and returned
        self.assertEqual(self.native[f'{MOD:08X}'][1], 8)
        self.assertEqual(self.native[f'{REL:08X}'][1], 9)

    def test_a_site_returns_1_only_when_its_patch_returns(self):
        self.assertEqual(self.sites, {T: (0, 7, 0), T + 0x10: (1, 9, 0), GOTO_SITE: (0, 10, GOTO_SITE),
                                      STEP: (0, 11, 0)})


if __name__ == '__main__':
    unittest.main()
