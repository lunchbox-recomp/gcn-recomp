#!/usr/bin/env python3
"""Checks for dol_patch.py. Run them with `python3 -m unittest discover -s recomp -p 'test_*.py'`.

The DOLs are assembled here from hand-encoded words, so no game is needed. Every address, size
and word in the fixture is made up. The last class runs recomp.py on a patched fixture to see
that a hook from the middle of a function into new code and back compiles.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dol import Dol, load_symbols  # noqa: E402

TOOL = os.path.join(HERE, 'dol_patch.py')
RECOMP = os.path.join(HERE, 'recomp.py')

HEADER = 0x100
# Function A: mflr r0, li r3,1, addi r3,r3,2, mtlr r0, blr. Three nops pad its section to 0x20.
A = 0x80010000
A_WORDS = (0x7C0802A6, 0x38600001, 0x38630002, 0x7C0803A6, 0x4E800020)
NOP = 0x60000000
TEXT_SIZE = 0x20
DATA = 0x80030000
DATA_SIZE = 0x40
BSS = 0x80040000
BSS_SIZE = 0x100
# The hook: A+4 becomes `b H`, and H runs the instruction A+4 held, then does `b A+8`.
H = 0x80020000
LI_R3_1 = 0x38600001
B_A4_TO_H = 0x4800FFFC
B_H4_TO_A8 = 0x4BFF0004


def make_dol() -> bytes:
    text = struct.pack('>8I', *A_WORDS, NOP, NOP, NOP)
    data = bytes((k * 5 + 1) % 251 for k in range(DATA_SIZE))
    offs, addrs, sizes = [0] * 18, [0] * 18, [0] * 18
    body = b''
    pos = HEADER
    for slot, addr, blob in ((0, A, text), (7, DATA, data)):
        offs[slot], addrs[slot], sizes[slot] = pos, addr, len(blob)
        body += blob
        pos += len(blob)
    head = struct.pack('>18I18I18I3I', *offs, *addrs, *sizes, BSS, BSS_SIZE, A)
    return head + bytes(HEADER - len(head)) + body


def slots(dol: bytes):
    """(file offset, address, size) for each of the 18 header slots."""
    return list(zip(struct.unpack_from('>18I', dol, 0x00), struct.unpack_from('>18I', dol, 0x48),
                    struct.unpack_from('>18I', dol, 0x90)))


def quietly(fn, *args):
    """dol.py leaves the files it reads open, which Python warns about."""
    with warnings.catch_warnings():
        warnings.simplefilter('ignore', ResourceWarning)
        return fn(*args)


def word_at(dol: bytes, addr: int) -> int:
    for off, a, size in slots(dol):
        if size and a <= addr < a + size:
            return struct.unpack_from('>I', dol, off + addr - a)[0]
    raise KeyError(hex(addr))


class Base(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = tmp.name
        self.dol = make_dol()
        self.sha = hashlib.sha1(self.dol).hexdigest()
        self.inp = self.write('in.dol', self.dol)
        self.out = os.path.join(self.dir, 'out.dol')

    def write(self, name, content) -> str:
        path = os.path.join(self.dir, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if isinstance(content, str):
            content = content.encode('utf-8')
        with open(path, 'wb') as f:
            f.write(content)
        return path

    def read(self, path) -> bytes:
        with open(path, 'rb') as f:
            return f.read()

    def plist(self, name, *lines) -> str:
        """A patch list that starts with the right dol line, so its first line is line 2."""
        return self.write(name, '\n'.join([f'dol {self.sha}'] + list(lines)) + '\n')

    def run_tool(self, *lists, cwd=None):
        return subprocess.run([sys.executable, '-B', TOOL, self.inp, self.out, *lists],
                              capture_output=True, text=True, cwd=cwd)

    def ok(self, *lists) -> bytes:
        r = self.run_tool(*lists)
        self.assertEqual(r.returncode, 0, r.stderr)
        return self.read(self.out)

    def refused(self, lists, at, why) -> None:
        """The run exits 1 on its first message, which starts with the first place in `at` (a
        path and a line, or None for the path alone), names every other place in it and holds
        `why`. Nothing is written."""
        for path in (self.out, self.out + '.syms'):
            if os.path.exists(path):
                os.remove(path)
        r = self.run_tool(*lists)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        places = [f'{path}:{line}' if line else path for path, line in at]
        self.assertTrue(r.stderr.startswith(places[0] + ': '), r.stderr)
        for place in places:
            self.assertIn(place, r.stderr)
        self.assertIn(why, r.stderr)
        self.assertFalse(os.path.exists(self.out))
        self.assertFalse(os.path.exists(self.out + '.syms'))

    def syms(self) -> str:
        return self.read(self.out + '.syms').decode('ascii')


class Words(Base):
    def test_a_list_with_only_dol_copies_the_input(self) -> None:
        self.assertEqual(self.ok(self.plist('a.txt')), self.dol)
        self.assertEqual(self.syms(), '')

    def test_a_word_changes_only_that_word(self) -> None:
        out = self.ok(self.plist('a.txt', f'word {A + 4:08X} {B_A4_TO_H:08X} {LI_R3_1:08X}'))
        self.assertEqual(len(out), len(self.dol))
        self.assertEqual([k for k in range(len(out)) if out[k] != self.dol[k]], [0x104, 0x105, 0x106, 0x107])
        self.assertEqual(word_at(out, A + 4), B_A4_TO_H)
        self.assertEqual(word_at(out, A + 8), A_WORDS[2])

    def test_a_word_in_a_data_section(self) -> None:
        old = word_at(self.dol, DATA + 8)
        out = self.ok(self.plist('a.txt', f'word {DATA + 8:08x} CAFE0001 {old:08x}'))
        self.assertEqual(word_at(out, DATA + 8), 0xCAFE0001)
        self.assertEqual(word_at(out, DATA + 4), word_at(self.dol, DATA + 4))

    def test_the_first_and_last_word_of_a_section(self) -> None:
        out = self.ok(self.plist('a.txt',
                                 f'word {A:08X} 11111111 {A_WORDS[0]:08X}',
                                 f'word {A + TEXT_SIZE - 4:08X} 22222222 {NOP:08X}'))
        self.assertEqual((word_at(out, A), word_at(out, A + 0x1C)), (0x11111111, 0x22222222))

    def test_an_expect_that_differs_is_refused(self) -> None:
        path = self.plist('a.txt', f'word {A + 4:08X} {B_A4_TO_H:08X} 38600002')
        self.refused([path], [(path, 2)], f'holds {LI_R3_1:08X}, not the expected 38600002')

    def test_a_word_in_a_new_zero_filled_section_expects_zero(self) -> None:
        out = self.ok(self.plist('a.txt', f'section text {H:08X} 20',
                                 f'word {H:08X} {LI_R3_1:08X} 0'))
        self.assertEqual(word_at(out, H), LI_R3_1)
        path = self.plist('b.txt', f'section text {H:08X} 20', f'word {H:08X} {LI_R3_1:08X} {NOP:08X}')
        self.refused([path], [(path, 3)], 'not the expected')

    def test_a_word_in_a_new_section_expects_the_files_bytes(self) -> None:
        self.write('code.bin', struct.pack('>2I', LI_R3_1, B_H4_TO_A8))
        out = self.ok(self.plist('a.txt', f'section text {H:08X} 20 code.bin',
                                 f'word {H + 4:08X} 12345678 {B_H4_TO_A8:08X}'))
        self.assertEqual((word_at(out, H), word_at(out, H + 4)), (LI_R3_1, 0x12345678))
        path = self.plist('b.txt', f'section text {H:08X} 20 code.bin', f'word {H + 4:08X} 12345678 0')
        self.refused([path], [(path, 3)], 'not the expected')

    def test_a_word_that_is_not_4_aligned_is_refused(self) -> None:
        for addr in (A + 1, A + 2, A + 6):
            path = self.plist('a.txt', f'word {addr:08X} 0 0')
            self.refused([path], [(path, 2)], 'not 4-aligned')

    def test_a_word_outside_every_section_is_refused(self) -> None:
        for addr in (A - 4, A + TEXT_SIZE, DATA - 4, DATA + DATA_SIZE, BSS, BSS + 8, 0x80050000):
            path = self.plist('a.txt', f'word {addr:08X} 0 0')
            self.refused([path], [(path, 2)], 'not inside a section')

    def test_a_word_cannot_straddle_the_end_of_a_section(self) -> None:
        head = bytearray(self.dol)
        struct.pack_into('>I', head, 0x90 + 4 * 7, DATA_SIZE - 2)
        self.inp = self.write('odd.dol', bytes(head))
        self.sha = hashlib.sha1(bytes(head)).hexdigest()
        path = self.plist('a.txt', f'word {DATA + DATA_SIZE - 4:08X} 0 {word_at(self.dol, DATA + DATA_SIZE - 4):08X}')
        self.refused([path], [(path, 2)], 'not inside a section')
        before = word_at(self.dol, DATA + DATA_SIZE - 8)
        self.ok(self.plist('b.txt', f'word {DATA + DATA_SIZE - 8:08X} 0 {before:08X}'))

    def test_two_lists_writing_one_word_are_refused_naming_both_lines(self) -> None:
        first = self.plist('a.txt', f'word {A + 4:08X} {B_A4_TO_H:08X} {LI_R3_1:08X}')
        for expect in (LI_R3_1, B_A4_TO_H):
            second = self.plist('b.txt', f'word {A + 8:08X} 0 {A_WORDS[2]:08X}',
                                f'word {A + 4:08X} 12345678 {expect:08X}')
            self.refused([first, second], [(second, 3), (first, 2)], 'already written')

    def test_one_list_writing_a_word_twice_is_refused_naming_both_lines(self) -> None:
        path = self.plist('a.txt', f'word {A + 4:08X} {B_A4_TO_H:08X} {LI_R3_1:08X}',
                          f'word {A + 4:08X} 12345678 {B_A4_TO_H:08X}')
        self.refused([path], [(path, 3), (path, 2)], 'already written')


class Order(Base):
    def test_a_later_list_can_write_into_an_earlier_lists_section(self) -> None:
        first = self.plist('a.txt', f'section text {H:08X} 20')
        second = self.plist('b.txt', f'word {H:08X} {LI_R3_1:08X} 0')
        self.assertEqual(word_at(self.ok(first, second), H), LI_R3_1)
        self.refused([second, first], [(second, 2)], 'not inside a section')

    def test_sections_take_slots_in_the_order_the_lists_come(self) -> None:
        first = self.plist('a.txt', f'section text {H:08X} 20', f'section data {H + 0x1000:08X} 20')
        second = self.plist('b.txt', f'section text {H + 0x100:08X} 20', f'section data {H + 0x1100:08X} 20')
        out = self.ok(first, second)
        self.assertEqual([slots(out)[k][1] for k in (1, 2, 8, 9)], [H, H + 0x100, H + 0x1000, H + 0x1100])
        out = self.ok(second, first)
        self.assertEqual([slots(out)[k][1] for k in (1, 2, 8, 9)], [H + 0x100, H, H + 0x1100, H + 0x1000])

    def test_the_same_inputs_give_the_same_bytes(self) -> None:
        self.write('code.bin', struct.pack('>2I', LI_R3_1, B_H4_TO_A8))
        lists = [self.plist('a.txt', f'section text {H:08X} 20 code.bin', f'function {H:08X} 8 hook_h',
                            f'word {A + 4:08X} {B_A4_TO_H:08X} {LI_R3_1:08X}'),
                 self.plist('b.txt', f'section data {DATA + 0x1000:08X} 40')]
        first = self.ok(*lists)
        first_syms = self.syms()
        os.remove(self.out)
        os.remove(self.out + '.syms')
        self.assertEqual(self.ok(*lists), first)
        self.assertEqual(self.syms(), first_syms)


class DolLine(Base):
    def test_a_list_has_to_start_with_dol(self) -> None:
        path = self.write('a.txt', f'word {A + 4:08X} 0 {LI_R3_1:08X}\ndol {self.sha}\n')
        self.refused([path], [(path, 1)], 'first line has to be dol')

    def test_comments_and_blank_lines_may_come_before_dol(self) -> None:
        path = self.write('a.txt', f'# a comment\n\n   \ndol {self.sha}\n')
        self.assertEqual(self.ok(path), self.dol)

    def test_a_list_with_no_lines_is_refused(self) -> None:
        for text in ('', '# nothing\n\n'):
            path = self.write('a.txt', text)
            self.refused([path], [(path, 1)], 'first line has to be dol')

    def test_a_dol_that_does_not_match_the_input_is_refused(self) -> None:
        other = hashlib.sha1(b'another dol').hexdigest()
        path = self.write('a.txt', f'dol {other}\n')
        self.refused([path], [(path, 1)], other)
        self.assertIn(self.sha, self.run_tool(path).stderr)

    def test_the_hash_may_be_upper_case(self) -> None:
        path = self.write('a.txt', f'dol {self.sha.upper()}\n')
        self.assertEqual(self.ok(path), self.dol)

    def test_every_list_has_to_match(self) -> None:
        first = self.plist('a.txt')
        second = self.write('b.txt', f'dol {hashlib.sha1(b"x").hexdigest()}\n')
        self.refused([first, second], [(second, 1)], 'the list is for the DOL')

    def test_a_hash_that_is_not_a_sha1_is_refused(self) -> None:
        for bad in (self.sha[:-1], self.sha + '0', 'z' * 40):
            path = self.write('a.txt', f'dol {bad}\n')
            self.refused([path], [(path, 1)], 'is not a SHA-1')
        path = self.write('b.txt', 'dol\n')
        self.refused([path], [(path, 1)], 'dol takes 1 arguments')

    def test_dol_is_only_allowed_once(self) -> None:
        path = self.plist('a.txt', f'dol {self.sha}')
        self.refused([path], [(path, 2)], 'only allowed once')


class Sections(Base):
    def test_a_zero_filled_section(self) -> None:
        out = self.ok(self.plist('a.txt', f'section text {H:08X} 20'))
        self.assertEqual(slots(out)[0], slots(self.dol)[0])
        self.assertEqual(slots(out)[7], slots(self.dol)[7])
        off, addr, size = slots(out)[1]
        self.assertEqual((addr, size), (H, 0x20))
        self.assertEqual(off % 0x20, 0)
        self.assertGreaterEqual(off, len(self.dol))
        self.assertEqual(out[off:off + size], bytes(0x20))
        self.assertEqual(len(out), off + size)
        self.assertEqual(out[HEADER:len(self.dol)], self.dol[HEADER:])
        parsed = quietly(Dol, self.out)
        self.assertEqual(sorted(parsed.sections), sorted([(A, 0x20, 0x100, True), (H, 0x20, off, True),
                                                          (DATA, 0x40, 0x120, False)]))

    def test_added_sections_start_on_32_byte_boundaries_in_the_file(self) -> None:
        padded = self.dol + bytes(5)
        self.inp = self.write('padded.dol', padded)
        self.sha = hashlib.sha1(padded).hexdigest()
        out = self.ok(self.plist('a.txt', f'section text {H:08X} 20', f'section data {H + 0x1000:08X} 24'))
        self.assertEqual((slots(out)[1][0], slots(out)[8][0]), (0x180, 0x1A0))
        self.assertEqual(len(out), 0x1C4)
        self.assertEqual(out[HEADER:len(padded)], padded[HEADER:])

    def test_a_section_holding_a_files_bytes(self) -> None:
        code = struct.pack('>2I', LI_R3_1, B_H4_TO_A8)
        self.write('code.bin', code)
        out = self.ok(self.plist('a.txt', f'section data {DATA + 0x1000:08X} 30 code.bin'))
        off, addr, size = slots(out)[8]
        self.assertEqual((addr, size), (DATA + 0x1000, 0x30))
        self.assertEqual(out[off:off + size], code + bytes(0x30 - len(code)))

    def test_a_files_path_is_relative_to_the_list_not_the_working_directory(self) -> None:
        code = struct.pack('>2I', LI_R3_1, B_H4_TO_A8)
        self.write('my mod/some code.bin', code)
        path = self.plist('my mod/dol.txt', f'section text {H:08X} 20 some code.bin')
        elsewhere = tempfile.TemporaryDirectory()
        self.addCleanup(elsewhere.cleanup)
        r = self.run_tool(path, cwd=elsewhere.name)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(word_at(self.read(self.out), H), LI_R3_1)
        os.remove(self.out)
        os.remove(self.out + '.syms')
        r = self.run_tool(os.path.join('my mod', 'dol.txt'), cwd=self.dir)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(word_at(self.read(self.out), H), LI_R3_1)

    def test_a_file_that_is_missing_or_too_big_is_refused(self) -> None:
        path = self.plist('a.txt', f'section text {H:08X} 20 nothing.bin')
        self.refused([path], [(path, 2)], "can't read nothing.bin")
        self.write('big.bin', bytes(0x24))
        path = self.plist('b.txt', f'section text {H:08X} 20 big.bin')
        self.refused([path], [(path, 2)], 'more than the section')
        self.write('exact.bin', bytes(0x20))
        self.ok(self.plist('c.txt', f'section text {H:08X} 20 exact.bin'))

    def test_text_and_data_take_the_first_free_slot_of_their_kind(self) -> None:
        out = self.ok(self.plist('a.txt', f'section data {H:08X} 20', f'section text {H + 0x100:08X} 20',
                                 f'section data {H + 0x200:08X} 20'))
        self.assertEqual([slots(out)[k][1] for k in (1, 8, 9)], [H + 0x100, H, H + 0x200])
        self.assertEqual([k for k in range(18) if slots(out)[k][2]], [0, 1, 7, 8, 9])

    def test_an_address_that_is_not_32_byte_aligned_is_refused(self) -> None:
        for addr in (H + 4, H + 0x10, H + 0x1C):
            path = self.plist('a.txt', f'section text {addr:08X} 20')
            self.refused([path], [(path, 2)], '32-byte aligned')

    def test_a_size_that_is_zero_or_not_a_multiple_of_4_is_refused(self) -> None:
        for size in ('0', '1', '22', '3E'):
            path = self.plist('a.txt', f'section text {H:08X} {size}')
            self.refused([path], [(path, 2)], 'multiple of 4')
        self.ok(self.plist('b.txt', f'section data {H:08X} 24'))

    def test_the_ram_bounds(self) -> None:
        for addr, size in ((0x7FFFFFE0, 0x20), (0x00003100, 0x20), (0xC0000000, 0x20),
                           (0x817FFFE0, 0x40), (0x81800000, 0x20), (0x80100000, 0x01800000)):
            path = self.plist('a.txt', f'section text {addr:08X} {size:X}')
            self.refused([path], [(path, 2)], 'main RAM below 81800000')
        out = self.ok(self.plist('b.txt', 'section text 80000000 20', 'section data 817FFFE0 20'))
        self.assertEqual([slots(out)[k][1] for k in (1, 8)], [0x80000000, 0x817FFFE0])

    def test_a_section_may_touch_but_not_overlap_the_dols_own(self) -> None:
        refused = [(A, 0x20), (A - 0x20, 0x40), (A - 0x40, 0x80), (A, 0x40), (DATA + 0x20, 0x20),
                   (DATA - 0x20, 0x40), (DATA, 0x100)]
        for addr, size in refused:
            path = self.plist('a.txt', f'section data {addr:08X} {size:X}')
            self.refused([path], [(path, 2)], 'overlaps the DOL')
        for addr in (A - 0x20, A + TEXT_SIZE, DATA - 0x20, DATA + DATA_SIZE):
            self.ok(self.plist('b.txt', f'section data {addr:08X} 20'))

    def test_a_section_may_touch_but_not_overlap_bss(self) -> None:
        for addr, size in ((BSS, 0x20), (BSS + 0x80, 0x20), (BSS - 0x20, 0x40), (BSS + BSS_SIZE - 0x20, 0x40),
                           (BSS - 0x20, 0x200)):
            path = self.plist('a.txt', f'section data {addr:08X} {size:X}')
            self.refused([path], [(path, 2)], 'bss')
        for addr in (BSS - 0x20, BSS + BSS_SIZE):
            self.ok(self.plist('b.txt', f'section data {addr:08X} 20'))

    def test_a_dol_with_no_bss_leaves_that_range_free(self) -> None:
        head = bytearray(self.dol)
        struct.pack_into('>2I', head, 0xD8, BSS, 0)
        self.inp = self.write('nobss.dol', bytes(head))
        self.sha = hashlib.sha1(bytes(head)).hexdigest()
        self.ok(self.plist('a.txt', f'section data {BSS:08X} 20'))
        self.ok(self.plist('b.txt', f'section data {BSS - 0x20:08X} 40'))

    def test_sections_overlapping_each_other_are_refused_naming_both_lines(self) -> None:
        first = self.plist('a.txt', f'section text {H:08X} 40')
        second = self.plist('b.txt', f'section data {H + 0x20:08X} 40')
        self.refused([first, second], [(second, 2), (first, 2)], 'overlaps')
        one = self.plist('c.txt', f'section text {H:08X} 40', f'section text {H + 0x20:08X} 20')
        self.refused([one], [(one, 3), (one, 2)], 'overlaps')
        self.ok(self.plist('d.txt', f'section text {H:08X} 40', f'section text {H + 0x40:08X} 20'))

    def test_the_slots_run_out_after_6_more_text_and_10_more_data(self) -> None:
        text = [f'section text {0x80100000 + k * 0x100:08X} 20' for k in range(6)]
        data = [f'section data {0x80200000 + k * 0x100:08X} 20' for k in range(10)]
        out = self.ok(self.plist('a.txt', *text, *data))
        self.assertEqual(len([s for s in slots(out) if s[2]]), 18)
        self.assertEqual(slots(out)[6][1], 0x80100500)
        self.assertEqual(slots(out)[17][1], 0x80200900)
        for lines, kind in ((text, 'text'), (data, 'data')):
            extra = f'section {kind} 80300000 20'
            path = self.plist('b.txt', *lines, extra)
            self.refused([path], [(path, len(lines) + 2)], f'all {7 if kind == "text" else 11} {kind} slots')
        self.ok(self.plist('c.txt', *text, 'section data 80300000 20'))
        self.ok(self.plist('d.txt', *data, 'section text 80300000 20'))

    def test_a_section_line_with_the_wrong_shape_is_refused(self) -> None:
        for line, why in ((f'section rodata {H:08X} 20', 'text or data'), (f'section text {H:08X}', 'takes 3 or 4'),
                          ('section text', 'takes 3 or 4'), (f'section text {H:08X} 20 a.bin b', "can't read a.bin b"),
                          ('section text zz 20', 'not a hex number'), (f'section text {H:08X} 1G', 'not a hex number'),
                          (f'section text {H:08X} 100000000', 'more than 32 bits')):
            path = self.plist('a.txt', line)
            self.refused([path], [(path, 2)], why)


class Functions(Base):
    def lists(self, *lines) -> str:
        return self.plist('a.txt', f'section text {H:08X} 20', *lines)

    def test_syms_holds_each_function_in_address_order(self) -> None:
        first = self.plist('a.txt', f'section text {H:08X} 20', f'function {H + 0x10:08X} 8 later_one',
                           f'function {H:08X} 10 hook_h')
        second = self.plist('b.txt', 'section text 8002A000 20', 'function 8002A010 C last_one')
        self.ok(first, second)
        self.assertEqual(self.syms(),
                         'hook_h = .text:0x80020000; // type:function size:0x10 scope:global\n'
                         'later_one = .text:0x80020010; // type:function size:0x8 scope:global\n'
                         'last_one = .text:0x8002A010; // type:function size:0xC scope:global\n')
        loaded = quietly(load_symbols, self.out + '.syms')
        self.assertEqual([(s['name'], s['addr'], s['type'], s['size']) for s in loaded],
                         [('hook_h', H, 'function', 0x10), ('later_one', H + 0x10, 'function', 8),
                          ('last_one', 0x8002A010, 'function', 0xC)])

    def test_syms_is_written_even_with_no_functions(self) -> None:
        self.write('out.dol.syms', 'stale = .text:0x80020000; // type:function size:0x8\n')
        self.ok(self.plist('a.txt', f'word {A + 4:08X} 0 {LI_R3_1:08X}'))
        self.assertEqual(self.syms(), '')

    def test_a_function_has_to_lie_inside_an_added_text_section(self) -> None:
        for line in (f'function {A:08X} 14 in_the_dol',
                     f'function {H + 0x1C:08X} 8 past_the_end',
                     f'function {H - 8:08X} 10 before_it',
                     f'function {H + 0x20:08X} 4 just_after',
                     f'function {DATA + 0x1000:08X} 8 in_nothing'):
            path = self.lists(line)
            self.refused([path], [(path, 3)], 'not inside one added text section')

    def test_a_function_may_fill_its_section(self) -> None:
        self.ok(self.lists(f'function {H:08X} 20 whole'))
        self.ok(self.lists(f'function {H + 0x1C:08X} 4 last_word'))

    def test_a_function_in_an_added_data_section_is_refused(self) -> None:
        path = self.plist('a.txt', f'section data {H:08X} 20', f'function {H:08X} 8 in_data')
        self.refused([path], [(path, 3)], 'not inside one added text section')

    def test_a_function_cannot_span_two_sections(self) -> None:
        path = self.plist('a.txt', f'section text {H:08X} 20', f'section text {H + 0x20:08X} 20',
                          f'function {H + 0x18:08X} 10 spans')
        self.refused([path], [(path, 4)], 'not inside one added text section')

    def test_a_function_needs_its_section_first(self) -> None:
        path = self.plist('a.txt', f'function {H:08X} 8 early', f'section text {H:08X} 20')
        self.refused([path], [(path, 2)], 'not inside one added text section')

    def test_a_function_in_an_earlier_lists_section(self) -> None:
        first = self.plist('a.txt', f'section text {H:08X} 20')
        second = self.plist('b.txt', f'function {H:08X} 8 hook_h')
        self.ok(first, second)
        self.assertIn('hook_h', self.syms())

    def test_functions_may_touch_but_not_overlap(self) -> None:
        for addr, size in ((H + 4, 8), (H + 0xC, 8), (H + 8, 8), (H + 8, 4), (H, 0x14)):
            path = self.lists(f'function {H + 8:08X} 8 existing', f'function {addr:08X} {size:X} other')
            self.refused([path], [(path, 4), (path, 3)], 'overlaps existing')
        self.ok(self.lists(f'function {H + 8:08X} 8 existing', f'function {H:08X} 8 before'))
        self.ok(self.lists(f'function {H + 8:08X} 8 existing', f'function {H + 0x10:08X} 8 after'))

    def test_functions_in_two_lists_that_overlap_name_both_lines(self) -> None:
        first = self.lists(f'function {H:08X} 10 one')
        second = self.plist('b.txt', f'function {H + 8:08X} 8 two')
        self.refused([first, second], [(second, 2), (first, 3)], 'overlaps one')

    def test_a_function_that_is_unaligned_or_empty_or_oddly_sized_is_refused(self) -> None:
        for line, why in ((f'function {H + 2:08X} 8 odd', 'not 4-aligned'), (f'function {H:08X} 0 empty', 'multiple of 4'),
                          (f'function {H:08X} 6 odd_size', 'multiple of 4')):
            path = self.lists(line)
            self.refused([path], [(path, 3)], why)

    def test_a_name_has_to_be_a_c_identifier(self) -> None:
        for name in ('1abc', 'a-b', 'a"b', 'a/b', 'a.b', 'a*/b', 'café'):
            path = self.lists(f'function {H:08X} 8 {name}')
            self.refused([path], [(path, 3)], 'not a C identifier')
        self.ok(self.lists(f'function {H:08X} 8 _Hook_9'))

    def test_a_function_line_with_the_wrong_shape_is_refused(self) -> None:
        for line, why in ((f'function {H:08X} 8', 'takes 3 arguments'), (f'function {H:08X} 8 a b', 'takes 3 arguments'),
                          ('function zz 8 a', 'not a hex number')):
            path = self.lists(line)
            self.refused([path], [(path, 3)], why)


class Syntax(Base):
    def test_comments_blank_lines_prefixes_and_case(self) -> None:
        text = (f'# header\n\ndol {self.sha}   # the input\n'
                f'  section   text\t0x{H:x}  0X20   # a section\n'
                f'word 0x{H:x} {LI_R3_1:x} 0 # a word\n'
                f'#word {A:08X} 0 0\n')
        path = self.write('a.txt', text)
        self.assertEqual(word_at(self.ok(path), H), LI_R3_1)

    def test_a_hash_inside_a_file_name_is_not_a_comment(self) -> None:
        self.write('code#1.bin', bytes(4))
        self.ok(self.plist('a.txt', f'section text {H:08X} 20 code#1.bin'))

    def test_unknown_lines_and_wrong_argument_counts_are_refused(self) -> None:
        for line, why in (('patch 1 2 3', 'unknown line patch'), (f'word {A:08X} 0', 'takes 3 arguments'),
                          (f'word {A:08X} 0 0 0', 'takes 3 arguments'), ('word', 'takes 3 arguments')):
            path = self.plist('a.txt', line)
            self.refused([path], [(path, 2)], why)

    def test_numbers_are_hex_and_fit_in_32_bits(self) -> None:
        for line, why in ((f'word {A:08X} 0 -0', 'not a hex number'), (f'word {A:08X} 0x 0', 'not a hex number'),
                          (f'word {A:08X} 1_0 0', 'not a hex number'), (f'word {A:08X} 0 +1', 'not a hex number'),
                          (f'word {A:08X} 100000000 0', 'more than 32 bits'),
                          (f'word 1{A:08X} 0 0', 'more than 32 bits'),
                          (f'word {A:08X} 0 100000000', 'more than 32 bits')):
            path = self.plist('a.txt', line)
            self.refused([path], [(path, 2)], why)
        out = self.ok(self.plist('b.txt', f'word {A + 4:08X} FFFFFFFF {LI_R3_1:08X}'))
        self.assertEqual(word_at(out, A + 4), 0xFFFFFFFF)

    def test_an_unreadable_list_is_refused_by_name(self) -> None:
        missing = os.path.join(self.dir, 'missing.txt')
        self.refused([missing], [(missing, None)], "can't read the patch list")
        binary = self.write('bin.txt', b'\xff\xfe\x00dol\n')
        self.refused([binary], [(binary, None)], "can't read the patch list")


class Command(Base):
    def test_bad_arguments_exit_2(self) -> None:
        for args in ([], [self.inp], [self.inp, self.out]):
            r = subprocess.run([sys.executable, '-B', TOOL, *args], capture_output=True, text=True)
            self.assertEqual(r.returncode, 2, r.stderr)
            self.assertIn('usage', r.stderr)
        self.assertFalse(os.path.exists(self.out))

    def test_a_refusal_leaves_an_earlier_output_alone(self) -> None:
        self.write('out.dol', b'earlier')
        self.write('out.dol.syms', 'earlier')
        path = self.plist('a.txt', f'word {A + 4:08X} 0 0')
        r = self.run_tool(path)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(self.read(self.out), b'earlier')
        self.assertEqual(self.syms(), 'earlier')

    def test_the_input_is_left_alone(self) -> None:
        self.ok(self.plist('a.txt', f'section text {H:08X} 20', f'word {A + 4:08X} 0 {LI_R3_1:08X}'))
        self.assertEqual(self.read(self.inp), self.dol)

    def test_an_input_that_is_not_a_dol_is_refused(self) -> None:
        path = self.plist('a.txt')
        self.inp = os.path.join(self.dir, 'missing.dol')
        r = self.run_tool(path)
        self.assertEqual(r.returncode, 1)
        self.assertIn("missing.dol: can't read the DOL", r.stderr)
        self.inp = self.write('short.dol', bytes(0xFF))
        r = self.run_tool(path)
        self.assertEqual(r.returncode, 1)
        self.assertIn('short.dol: too short to be a DOL', r.stderr)
        cut = bytearray(self.dol)[:-4]
        self.inp = self.write('cut.dol', bytes(cut))
        r = self.run_tool(path)
        self.assertEqual(r.returncode, 1)
        self.assertIn('cut.dol: section 7 runs past the end of the file', r.stderr)
        self.assertFalse(os.path.exists(self.out))


class Hook(Base):
    """A hook from the middle of function A into new code and back, through recomp.py."""

    def build(self) -> str:
        self.write('hook.bin', struct.pack('>2I', LI_R3_1, B_H4_TO_A8))
        path = self.plist('hook.txt', f'word {A + 4:08X} {B_A4_TO_H:08X} {LI_R3_1:08X}',
                          f'section text {H:08X} 20 hook.bin', f'function {H:08X} 8 hook_h')
        self.ok(path)
        symbols = self.write('symbols.txt',
                             f'target = .text:0x{A:08X}; // type:function size:0x14 scope:global\n')
        tables = os.path.join(self.dir, 'tables')
        os.mkdir(tables)
        gen = os.path.join(self.dir, 'gen')
        r = subprocess.run([sys.executable, '-B', RECOMP, self.out, symbols, gen, tables],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.stderr = r.stderr
        with open(os.path.join(gen, 'recomp_000.c')) as f:
            return f.read()

    def test_the_hook_compiles_as_a_call_then_return_and_a_call_back_into_a(self) -> None:
        c = self.build()
        self.assertNotIn('branches to non-code', self.stderr)
        self.assertNotIn('warning', self.stderr)
        self.assertIn('mid_entries: 1', self.stderr)
        self.assertIn('functions: 2', self.stderr)
        a_part, h_part = c.split('/* hook_h ')
        self.assertTrue(a_part.lstrip().startswith('#include') and '/* target  size 0x14 */' in a_part)
        # A gets an entry at A+8, so H can call back into the middle of A.
        self.assertIn('case 0x80010008u: goto L_80010008;', a_part)
        self.assertIn('L_80010008:', a_part)
        self.assertIn('void fn_80010008(CPU* c) { body_80010000(c, 0x80010008u); }', a_part)
        # A+4 is a call to H, then a return.
        self.assertIn(f'/* 80010004 {B_A4_TO_H:08X} */ fn_80020000(c); RET();', a_part)
        # H runs the word A+4 held, then calls A+8's entry and returns.
        self.assertIn(f'/* 80020000 {LI_R3_1:08X} */', h_part)
        self.assertIn(f'/* 80020004 {B_H4_TO_A8:08X} */ fn_80010008(c); RET();', h_part)
        self.assertNotIn('goto', h_part)
        with open(os.path.join(self.dir, 'gen', 'recomp_table.c')) as f:
            table = f.read()
        self.assertIn('{0x80010008u, fn_80010008, "target+0x8"},', table)
        self.assertIn('{0x80020000u, fn_80020000, "hook_h"},', table)

    def test_without_the_syms_file_the_hook_is_a_branch_to_non_code(self) -> None:
        self.build()
        os.remove(self.out + '.syms')
        symbols = os.path.join(self.dir, 'symbols.txt')
        r = subprocess.run([sys.executable, '-B', RECOMP, self.out, symbols, os.path.join(self.dir, 'gen2'),
                            os.path.join(self.dir, 'tables')], capture_output=True, text=True)
        self.assertIn(f'{A + 4:08X} branches to non-code {H:08X}', r.stderr)


if __name__ == '__main__':
    unittest.main()
