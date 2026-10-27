#!/usr/bin/env python3
"""Checks for dol_patch.py. Run them with `python3 -m unittest discover -s recomp -p 'test_*.py'`.

The DOLs are assembled here from hand-encoded words, so no game is needed. Every address, size
and word in the fixture is made up. The last class runs recomp.py on a patched fixture to see
that a hook from the middle of a function into new code and back compiles.
"""
import builtins
import contextlib
import hashlib
import io
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import warnings
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dol_patch  # noqa: E402
from dol import Dol, load_symbols  # noqa: E402

TOOL = os.path.join(HERE, 'dol_patch.py')
RECOMP = os.path.join(HERE, 'recomp.py')
BOOT_CPP = os.path.join(os.path.dirname(HERE), 'runtime', 'boot.cpp')
DOC = os.path.join(os.path.dirname(HERE), 'docs', 'dol-patches.md')

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
        # A run that opens a pipe it shouldn't would block, so a hang is an error here.
        return subprocess.run([sys.executable, '-B', TOOL, self.inp, self.out, *lists],
                              capture_output=True, text=True, cwd=cwd, timeout=30)

    def ok(self, *lists) -> bytes:
        r = self.run_tool(*lists)
        self.assertEqual(r.returncode, 0, r.stderr)
        return self.read(self.out)

    def refused(self, lists, at, why) -> None:
        """The run exits 1 on its first message, which starts with the first place in `at` (a
        path and a line, or None for the path alone), names every other place in it and holds
        `why`. Nothing is written."""
        sidecars = [self.out + '.syms', self.out + '.modules']
        for path in [self.out] + sidecars:
            if os.path.exists(path):
                os.remove(path)
        r = self.run_tool(*lists)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        places = [f'{path}:{line}' if line else path for path, line in at]
        self.assertTrue(r.stderr.startswith(places[0] + ': '), r.stderr)
        for place in places:
            self.assertIn(place, r.stderr)
        self.assertIn(why, r.stderr)
        for path in [self.out] + sidecars:
            self.assertFalse(os.path.exists(path), path)

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
    def test_a_later_list_cannot_write_into_an_earlier_lists_section(self) -> None:
        first = self.plist('a.txt', f'section text {H:08X} 20')
        second = self.plist('b.txt', f'word {H + 8:08X} {LI_R3_1:08X} 0')
        self.refused([first, second], [(second, 2), (first, 2)],
                     f'{second}:2: {H + 8:08X} is inside the section added at {first}:2, '
                     'which belongs to another list')
        self.refused([second, first], [(second, 2)], 'not inside a section')

    def test_a_list_may_write_into_its_own_sections_and_the_dols(self) -> None:
        first = self.plist('a.txt', f'section text {H:08X} 20', f'word {H:08X} {LI_R3_1:08X} 0')
        second = self.plist('b.txt', f'section text {H + 0x100:08X} 20', f'word {H + 0x100:08X} {NOP:08X} 0',
                            f'word {A + 4:08X} {B_A4_TO_H:08X} {LI_R3_1:08X}')
        out = self.ok(first, second)
        self.assertEqual([word_at(out, H), word_at(out, H + 0x100), word_at(out, A + 4)],
                         [LI_R3_1, NOP, B_A4_TO_H])
        again = self.plist('c.txt', f'section data {DATA + 0x1000:08X} 20', f'word {DATA + 0x1000:08X} 1 0')
        self.assertEqual(word_at(self.ok(first, again), DATA + 0x1000), 1)

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
        out = self.ok(self.plist('b.txt', 'section text 80000100 20', 'section data 817FDFE0 20'))
        self.assertEqual([slots(out)[k][1] for k in (1, 8)], [0x80000100, 0x817FDFE0])
        # The two ends of RAM are inside it, so what refuses a section there is boot.
        for addr in (0x80000000, 0x817FFFE0):
            path = self.plist('c.txt', f'section text {addr:08X} 20')
            self.refused([path], [(path, 2)], 'which boot writes after it loads the sections')

    def test_a_section_may_touch_but_not_overlap_the_dols_own(self) -> None:
        refused = [(A, 0x20), (A - 0x20, 0x40), (A - 0x40, 0x80), (A, 0x40), (DATA + 0x20, 0x20),
                   (DATA - 0x20, 0x40), (DATA, 0x100)]
        for addr, size in refused:
            path = self.plist('a.txt', f'section data {addr:08X} {size:X}')
            self.refused([path], [(path, 2)], 'overlaps the DOL')
        for addr in (A - 0x20, A + TEXT_SIZE, DATA - 0x20, DATA + DATA_SIZE):
            self.ok(self.plist('b.txt', f'section data {addr:08X} 20'))

    def test_an_overlap_message_names_what_it_overlaps_and_nothing_else(self) -> None:
        for kind, addr, label in (('data', A, "the DOL's text section 0"), ('text', DATA, "the DOL's data section 7"),
                                  ('text', BSS, "the DOL's bss")):
            path = self.plist('a.txt', f'section {kind} {addr:08X} 20')
            r = self.run_tool(path)
            self.assertEqual((r.returncode, r.stdout, r.stderr), (1, '', f'{path}:2: overlaps {label}\n'))
        first = self.plist('b.txt', f'section text {H:08X} 40')
        second = self.plist('c.txt', f'section data {H + 0x20:08X} 40')
        r = self.run_tool(first, second)
        self.assertEqual((r.returncode, r.stdout, r.stderr),
                         (1, '', f'{second}:2: overlaps the section added at {first}:2\n'))

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


class Sidecars(Base):
    """The files recomp.py reads beside a DOL carry from IN to OUT."""

    def sym(self, name, addr, size=None, kind='function') -> str:
        extra = f' size:0x{size:X}' if size is not None else ''
        return f'{name} = .text:0x{addr:08X}; // type:{kind}{extra} scope:global'

    def write_syms(self, *lines) -> str:
        return self.write('in.dol.syms', '\n'.join(lines) + '\n')

    def hooks(self, *lines) -> str:
        """A list that adds a text section at H, so its first added line is line 3."""
        return self.plist('a.txt', f'section text {H:08X} 20', *lines)

    def chain(self, name) -> None:
        """Make the last run's output the next run's input, and the next output `name`."""
        self.inp = self.out
        self.sha = hashlib.sha1(self.read(self.inp)).hexdigest()
        self.out = os.path.join(self.dir, name)

    def test_ins_symbols_come_first_and_the_new_functions_sort_in(self) -> None:
        self.write_syms('# a note', self.sym('high', 0x80050000, 8), '',
                        self.sym('obj', DATA, 0x40, 'object'), self.sym('target', A, 0x14))
        self.ok(self.hooks(f'function {H:08X} 8 hook_h'))
        self.assertEqual(self.syms(), '\n'.join([
            self.sym('target', A, 0x14), self.sym('hook_h', H, 8), self.sym('obj', DATA, 0x40, 'object'),
            self.sym('high', 0x80050000, 8)]) + '\n')

    def test_a_run_with_no_new_function_still_carries_ins_symbols(self) -> None:
        self.write_syms(self.sym('target', A, 0x14))
        self.ok(self.plist('a.txt'))
        self.assertEqual(self.syms(), self.sym('target', A, 0x14) + '\n')

    def test_an_indented_symbol_line_carries_through_without_its_indent(self) -> None:
        self.write_syms('  ' + self.sym('target', A, 0x14), '\t' + self.sym('other', 0x80050000, 8) + '  ')
        self.ok(self.plist('a.txt'))
        self.assertEqual(self.syms(), self.sym('target', A, 0x14) + '\n' + self.sym('other', 0x80050000, 8) + '\n')

    def test_a_symbol_with_non_ascii_text_carries_through_as_utf_8(self) -> None:
        line = self.sym('café', 0x80050000, 8)
        self.write_syms(line)
        self.ok(self.hooks(f'function {H:08X} 8 hook_h'))
        self.assertEqual(self.read(self.out + '.syms'), (self.sym('hook_h', H, 8) + '\n' + line + '\n').encode('utf-8'))

    def test_two_runs_in_a_row_keep_every_function_and_the_modules_file(self) -> None:
        self.write_syms(self.sym('target', A, 0x14))
        self.write('in.dol.modules', b'80100000 20 0 first\n')
        self.out = os.path.join(self.dir, 'b.dol')
        self.ok(self.hooks(f'function {H:08X} 8 first_fn'))
        self.chain('c.dol')
        self.ok(self.plist('b.txt', f'section text {H + 0x100:08X} 20', f'function {H + 0x100:08X} 8 second_fn'))
        self.assertEqual(self.syms(), '\n'.join([
            self.sym('target', A, 0x14), self.sym('first_fn', H, 8), self.sym('second_fn', H + 0x100, 8)]) + '\n')
        self.assertEqual(self.read(self.out + '.modules'), b'80100000 20 0 first\n')

    def test_a_stale_out_modules_goes_when_in_has_none(self) -> None:
        modules = self.write('in.dol.modules', b'80100000 20 0 first\n')
        self.ok(self.plist('a.txt'))
        self.assertTrue(os.path.exists(self.out + '.modules'))
        os.remove(modules)
        self.assertEqual(self.run_tool(self.plist('bad.txt', 'frob')).returncode, 1)
        self.assertTrue(os.path.exists(self.out + '.modules'), 'a refused run leaves it alone')
        self.ok(self.plist('a.txt'))
        self.assertFalse(os.path.exists(self.out + '.modules'))

    def test_a_function_that_overlaps_one_of_ins_is_refused_naming_the_line(self) -> None:
        syms = self.write_syms('# first', self.sym('taken', H + 8, 8))
        for addr, size in ((H, 0xC), (H + 0xC, 8), (H + 8, 8), (H + 0xC, 4)):
            path = self.hooks(f'function {addr:08X} {size:X} other')
            self.refused([path], [(path, 3)], f'{path}:3: overlaps taken at {syms}:2')
        self.ok(self.hooks(f'function {H:08X} 8 before', f'function {H + 0x10:08X} 8 after'))

    def test_only_a_function_with_a_size_blocks_a_new_one(self) -> None:
        self.write_syms(self.sym('obj', H, 0x20, 'object'), self.sym('bare', H + 8), self.sym('lbl', H + 8, kind='label'))
        self.ok(self.hooks(f'function {H:08X} 10 hook_h'))

    def test_a_name_ins_symbols_already_use_is_refused_naming_the_line(self) -> None:
        syms = self.write_syms('# first', self.sym('hook_h', 0x80050000, 8), self.sym('shared', 0x80060000, 4, 'object'))
        for name, line in (('hook_h', 2), ('shared', 3)):
            path = self.hooks(f'function {H:08X} 8 {name}')
            self.refused([path], [(path, 3)], f'{path}:3: {name} is already named at {syms}:{line}')

    def test_a_name_ins_symbols_use_twice_is_refused_naming_the_first_line(self) -> None:
        syms = self.write_syms('# first', self.sym('twice', 0x80050000, 8), self.sym('twice', 0x80060000, 8))
        path = self.hooks(f'function {H:08X} 8 twice')
        self.refused([path], [(path, 3)], f'{path}:3: twice is already named at {syms}:2')

    def test_a_symbols_file_that_cant_be_read_is_refused_by_name(self) -> None:
        syms = self.write('in.dol.syms', b'\xff\xfe')
        path = self.hooks()
        self.refused([path], [(syms, None)], "can't read the symbols file")
        os.remove(syms)
        os.mkdir(syms)
        self.refused([path], [(syms, None)], "can't read the symbols file")

    def test_a_size_that_is_not_hex_is_refused_naming_the_line(self) -> None:
        syms = self.write_syms('# one', 'x = .text:0x80050000; // type:function size:zz scope:global')
        path = self.hooks()
        self.refused([path], [(syms, 2)], 'size zz is not a hex number')

    def test_ins_modules_file_is_copied_byte_for_byte(self) -> None:
        modules = b'80100000 20 0 first\n\xff\xfe not text\r\n'
        self.write('in.dol.modules', modules)
        self.ok(self.plist('a.txt'))
        self.assertEqual(self.read(self.out + '.modules'), modules)

    def test_no_modules_file_is_written_when_in_has_none(self) -> None:
        self.ok(self.plist('a.txt'))
        self.assertFalse(os.path.exists(self.out + '.modules'))

    def test_a_modules_file_that_cant_be_read_is_refused_by_name(self) -> None:
        modules = os.path.join(self.dir, 'in.dol.modules')
        os.mkdir(modules)
        self.refused([self.plist('a.txt')], [(modules, None)], "can't read the modules file")

    def test_a_run_may_overwrite_its_input(self) -> None:
        self.write_syms(self.sym('target', A, 0x14))
        self.write('in.dol.modules', b'80100000 20 0 first\n')
        self.out = self.inp
        out = self.ok(self.hooks(f'function {H:08X} 8 hook_h', f'word {A + 4:08X} {B_A4_TO_H:08X} {LI_R3_1:08X}'))
        self.assertEqual(word_at(out, A + 4), B_A4_TO_H)
        self.assertEqual(self.syms(), '\n'.join([self.sym('target', A, 0x14), self.sym('hook_h', H, 8)]) + '\n')
        self.assertEqual(self.read(self.out + '.modules'), b'80100000 20 0 first\n')


class BootWrites(Base):
    """Boot writes the low-memory words and the top 8 KiB after it copies the DOL's sections."""

    # Hand-copied from runtime/boot.cpp, as (start, size, refused) around each range it writes.
    # A section starts 32-byte aligned, so only a small size reaches a range's unaligned edge,
    # and an upper edge that no aligned start can reach has no case.
    EDGES = (
        (0x80000000, 4, True), (0x80000020, 0x20, True), (0x80000040, 0x20, False),
        (0x800000A0, 0x40, True), (0x800000C0, 0x0C, False), (0x800000C0, 0x10, True),
        (0x800000E0, 0x10, False), (0x800000E0, 0x14, True), (0x800000E0, 0x20, True),
        (0x80000100, 0x20, False),
        (0x800002E0, 0x20, False), (0x800002E0, 0x24, True), (0x80000300, 4, True),
        (0x800007E0, 0x20, False), (0x800007E0, 0x24, True), (0x80000800, 4, True),
        (0x80000BE0, 0x20, False), (0x80000BE0, 0x24, True), (0x80000C00, 4, True),
        (0x817FDFE0, 0x20, False), (0x817FDFE0, 0x24, True), (0x817FE000, 4, True),
        (0x817FFFE0, 0x20, True),
    )

    def test_a_section_may_touch_but_not_overlap_what_boot_writes(self) -> None:
        for start, size, refused in self.EDGES:
            path = self.plist('a.txt', f'section text {start:08X} {size:X}')
            if refused:
                self.refused([path], [(path, 2)], 'which boot writes after it loads the sections')
            else:
                out = self.ok(path)
                self.assertEqual(slots(out)[1][1:], (start, size), hex(start))

    def test_the_message_names_the_section_and_the_range(self) -> None:
        path = self.plist('a.txt', 'section data 800000C0 20')
        self.refused([path], [(path, 2)], f'{path}:2: 800000C0 to 800000E0 overlaps 800000CC to 800000D4, '
                     'which boot writes after it loads the sections')

    def test_boot_is_checked_for_data_sections_and_for_a_files_bytes_too(self) -> None:
        self.write('code.bin', bytes(4))
        for line in ('section data 80000800 20', 'section text 80000800 20 code.bin'):
            path = self.plist('a.txt', line)
            self.refused([path], [(path, 2)], 'which boot writes after it loads the sections')

    def test_the_ranges_cover_every_address_boot_writes(self) -> None:
        with open(BOOT_CPP, encoding='utf-8') as f:
            text = f.read()

        def covered(start, size) -> bool:
            return any(lo <= start and start + size <= hi for lo, hi in dol_patch.BOOT_WRITES)

        stores = [(int(a, 16), int(bits) // 8) for bits, a in re.findall(r'mem_w(8|16|32)\((0x[0-9A-Fa-f]+),', text)]
        self.assertGreater(len(stores), 10, 'boot.cpp no longer writes words the way this reads them')
        for addr, n in stores:
            self.assertTrue(covered(addr, n), f'boot.cpp writes {addr:08X}')
        header = re.search(r'memcpy\(mem_ptr\((0x[0-9A-Fa-f]+)\), hdr, (0x[0-9A-Fa-f]+)\)', text)
        bi2 = re.search(r'bi2_addr = (0x[0-9A-Fa-f]+);', text)
        bi2_size = re.search(r'iso_read\(0x440, mem_ptr\(bi2_addr\), (0x[0-9A-Fa-f]+)\)', text)
        self.assertTrue(header and bi2 and bi2_size, 'boot.cpp no longer copies the header and BI2 this way')
        self.assertTrue(covered(int(header[1], 16), int(header[2], 16)))
        self.assertTrue(covered(int(bi2[1], 16), int(bi2_size[1], 16)))
        # Any other write to a fixed address would go through mem_ptr too, and this doesn't read it.
        self.assertEqual(re.findall(r'mem_ptr\((0x[0-9A-Fa-f]+)\)', text), [header[1]],
                         'boot.cpp writes a fixed address in a way this does not read')
        # And each range starts and ends where a write of boot's does, so none is wider than it cites.
        writes = [(a, a + n) for a, n in stores] + [
            (int(header[1], 16), int(header[1], 16) + int(header[2], 16)),
            (int(bi2[1], 16), int(bi2[1], 16) + int(bi2_size[1], 16))]
        for lo, hi in dol_patch.BOOT_WRITES:
            self.assertIn(lo, [a for a, _ in writes], f'{lo:08X} to {hi:08X}')
            self.assertIn(hi, [b for _, b in writes], f'{lo:08X} to {hi:08X}')


class Reads:
    """Stands in for a section file's handle and records how many bytes each read asked for."""

    def __init__(self, real, asked) -> None:
        self.real, self.asked = real, asked

    def __enter__(self):
        return self

    def __exit__(self, *exc) -> None:
        self.real.close()

    def read(self, n=-1) -> bytes:
        self.asked.append(n)
        return self.real.read(n)


class SectionFiles(Base):
    """The file a section line names is a regular file inside the folder that holds the list."""

    def line(self, name) -> str:
        return f'section text {H:08X} 20 {name}'

    def test_a_name_that_climbs_out_of_the_folder_is_refused(self) -> None:
        self.write('secret.bin', bytes(8))
        path = self.plist('mod/dol.txt', self.line('../secret.bin'))
        self.refused([path], [(path, 2)], f'{path}:2: ../secret.bin is outside the folder that holds the list')

    def test_a_sibling_folder_with_the_same_prefix_is_outside(self) -> None:
        self.write('mod2/x.bin', bytes(8))
        path = self.plist('mod/dol.txt', self.line('../mod2/x.bin'))
        self.refused([path], [(path, 2)], 'is outside the folder that holds the list')

    def test_a_name_that_leaves_and_comes_back_is_fine(self) -> None:
        self.write('mod/x.bin', struct.pack('>I', LI_R3_1))
        out = self.ok(self.plist('mod/dol.txt', self.line('../mod/./x.bin')))
        self.assertEqual(word_at(out, H), LI_R3_1)

    def test_an_absolute_path_is_refused_even_when_it_points_inside(self) -> None:
        outside = self.write('secret.bin', bytes(8))
        inside = self.write('mod/x.bin', bytes(8))
        for name in (outside, inside):
            path = self.plist('mod/dol.txt', self.line(name))
            self.refused([path], [(path, 2)], f'{path}:2: {name} is an absolute path')

    def test_a_symlink_out_of_the_folder_is_refused(self) -> None:
        outside = self.write('secret.bin', bytes(8))
        self.write('mod/.keep', b'')
        os.symlink(outside, os.path.join(self.dir, 'mod', 'link.bin'))
        os.symlink(self.dir, os.path.join(self.dir, 'mod', 'linkdir'))
        for name in ('link.bin', 'linkdir/secret.bin'):
            path = self.plist('mod/dol.txt', self.line(name))
            self.refused([path], [(path, 2)], 'is outside the folder that holds the list')

    def test_a_symlink_that_stays_inside_is_fine(self) -> None:
        self.write('mod/real.bin', struct.pack('>I', LI_R3_1))
        os.symlink('real.bin', os.path.join(self.dir, 'mod', 'alias.bin'))
        out = self.ok(self.plist('mod/dol.txt', self.line('alias.bin')))
        self.assertEqual(word_at(out, H), LI_R3_1)

    def test_a_linked_list_confines_its_files_to_the_folder_the_link_sits_in(self) -> None:
        real = self.plist('real/dol.txt', self.line('x.bin'))
        self.write('real/x.bin', struct.pack('>I', LI_R3_1))
        self.write('mod/.keep', b'')
        link = os.path.join(self.dir, 'mod', 'dol.txt')
        os.symlink(real, link)
        self.refused([link], [(link, 2)], f"{link}:2: can't read x.bin")
        self.write('mod/x.bin', struct.pack('>I', LI_R3_1))
        self.assertEqual(word_at(self.ok(link), H), LI_R3_1)

    def test_a_fifo_is_refused_without_being_opened(self) -> None:
        self.write('mod/.keep', b'')
        os.mkfifo(os.path.join(self.dir, 'mod', 'pipe.bin'))
        path = self.plist('mod/dol.txt', self.line('pipe.bin'))
        self.refused([path], [(path, 2)], f'{path}:2: pipe.bin is not a regular file')

    def test_a_folder_is_refused(self) -> None:
        self.write('mod/sub/x.bin', bytes(4))
        path = self.plist('mod/dol.txt', self.line('sub'))
        self.refused([path], [(path, 2)], 'sub is not a regular file')

    def test_a_file_is_read_up_to_one_byte_past_the_section(self) -> None:
        self.write('mod/code.bin', bytes(0x24))
        path = self.plist('mod/dol.txt', self.line('code.bin'))
        real_open = builtins.open
        asked = []

        def spy(file, mode='r', *args, **kwargs):
            handle = real_open(file, mode, *args, **kwargs)
            return Reads(handle, asked) if str(file).endswith('code.bin') else handle

        with mock.patch.object(dol_patch, 'open', spy, create=True), \
                contextlib.redirect_stderr(io.StringIO()) as err:
            code = dol_patch.main([self.inp, self.out, path])
        self.assertEqual(code, 1)
        self.assertIn('is more than the section', err.getvalue())
        self.assertEqual(asked, [0x21])
        self.assertFalse(os.path.exists(self.out))


class PatchListFiles(Base):
    """The list itself is a regular file, or a link to one, of at most 1 MiB."""

    def test_a_fifo_is_refused_without_being_opened(self) -> None:
        path = os.path.join(self.dir, 'pipe.txt')
        os.mkfifo(path)
        self.refused([path], [(path, None)], f'{path}: the patch list is not a regular file')

    def test_a_link_to_a_device_is_refused(self) -> None:
        path = os.path.join(self.dir, 'null.txt')
        os.symlink(os.devnull, path)
        self.refused([path], [(path, None)], f'{path}: the patch list is not a regular file')

    def test_a_link_to_a_regular_file_is_fine(self) -> None:
        real = self.plist('real/a.txt', f'word {A + 4:08X} {B_A4_TO_H:08X} {LI_R3_1:08X}')
        link = os.path.join(self.dir, 'link.txt')
        os.symlink(real, link)
        self.assertEqual(word_at(self.ok(link), A + 4), B_A4_TO_H)

    def test_a_list_may_be_a_mebibyte_and_no_more(self) -> None:
        head = f'dol {self.sha}\n'
        self.ok(self.write('big.txt', head + '#' * (0x100000 - len(head))))
        path = self.write('big.txt', head + '#' * (0x100001 - len(head)))
        self.refused([path], [(path, None)], f'{path}: the patch list is more than 1 MiB')

    def test_a_list_is_read_up_to_one_byte_past_a_mebibyte(self) -> None:
        path = self.plist('a.txt')
        real_open = builtins.open
        asked = []

        def spy(file, mode='r', *args, **kwargs):
            handle = real_open(file, mode, *args, **kwargs)
            return Reads(handle, asked) if str(file).endswith('a.txt') else handle

        with mock.patch.object(dol_patch, 'open', spy, create=True), \
                contextlib.redirect_stderr(io.StringIO()):
            code = dol_patch.main([self.inp, self.out, path])
        self.assertEqual(code, 0)
        self.assertEqual(asked, [0x100001])


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

    def test_two_new_functions_sharing_a_name_are_refused_naming_both_lines(self) -> None:
        one = self.lists(f'function {H:08X} 8 twin', f'function {H + 0x10:08X} 8 twin')
        self.refused([one], [(one, 4), (one, 3)], f'twin is already named at {one}:3')
        first = self.lists(f'function {H:08X} 8 twin')
        second = self.plist('b.txt', 'section text 8002A000 20', 'function 8002A000 8 twin')
        self.refused([first, second], [(second, 3), (first, 3)], f'twin is already named at {first}:3')
        self.ok(self.lists(f'function {H:08X} 8 one', f'function {H + 0x10:08X} 8 two'))

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

    def test_a_utf8_byte_order_mark_before_the_dol_line_is_fine(self) -> None:
        path = self.write('a.txt', b'\xef\xbb\xbf' + f'dol {self.sha}\n'.encode('ascii'))
        self.assertEqual(self.ok(path), self.dol)

    def test_only_a_newline_ends_a_line(self) -> None:
        hook = f'word {A + 4:08X} {B_A4_TO_H:08X} {LI_R3_1:08X}'
        for sep in (' ', ' ', '\x85', '\x0b', '\x0c', '\x1c', '\x1d', '\x1e', '\r'):
            out = self.ok(self.plist('a.txt', f'# note{sep}more words', hook))
            self.assertEqual(word_at(out, A + 4), B_A4_TO_H, repr(sep))
            path = self.plist('b.txt', f'# note{sep}more words', 'patch 1')
            self.refused([path], [(path, 3)], 'unknown line patch')

    def test_lines_may_end_in_a_carriage_return_and_a_newline(self) -> None:
        text = f'dol {self.sha}\r\n# note\r\nword {A + 4:08X} {B_A4_TO_H:08X} {LI_R3_1:08X}\r\n'
        out = self.ok(self.write('a.txt', text))
        self.assertEqual(word_at(out, A + 4), B_A4_TO_H)

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

    def test_an_output_that_cant_be_written_exits_1_with_a_reason_and_no_traceback(self) -> None:
        path = self.plist('a.txt')
        self.out = os.path.join(self.dir, 'missing', 'out.dol')
        r = self.run_tool(path)
        self.assertEqual(r.returncode, 1)
        self.assertIn(self.out, r.stderr)
        self.assertIn('No such file or directory', r.stderr)
        self.assertNotIn('Traceback', r.stderr)
        self.out = os.path.join(self.dir, 'out.dol')
        for sidecar in ('.syms', '.modules'):
            os.mkdir(self.out + sidecar)
            self.write('in.dol.modules', b'm\n')
            r = self.run_tool(path)
            self.assertEqual(r.returncode, 1, sidecar)
            self.assertIn(self.out + sidecar, r.stderr)
            self.assertIn('Is a directory', r.stderr)
            self.assertNotIn('Traceback', r.stderr)
            os.rmdir(self.out + sidecar)

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


class DocExample(Base):
    """The script in docs/dol-patches.md, run the way gcn_add_game runs a DOL_FIXUP script."""

    def blocks(self, language) -> list:
        with open(DOC, encoding='utf-8') as f:
            return re.findall(rf'```{language}\n(.*?)```', f.read(), re.S)

    def test_the_script_runs_as_written_from_another_folder(self) -> None:
        script = self.blocks('python')
        self.assertEqual(len(script), 1)
        self.write('game/dol_fixup.py', script[0])
        os.symlink(os.path.dirname(HERE), os.path.join(self.dir, 'game', 'gcn-recomp'))
        self.plist('game/code.txt', f'word {A + 4:08X} {B_A4_TO_H:08X} {LI_R3_1:08X}')
        elsewhere = tempfile.TemporaryDirectory()
        self.addCleanup(elsewhere.cleanup)
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
        r = subprocess.run([sys.executable, os.path.join(self.dir, 'game', 'dol_fixup.py'), self.inp, self.out,
                            'game.iso'], capture_output=True, text=True, cwd=elsewhere.name, env=env, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(word_at(self.read(self.out), A + 4), B_A4_TO_H)
        self.assertEqual(self.syms(), '')

    def test_the_doc_lists_the_patch_files_so_editing_them_rebuilds(self) -> None:
        listed = self.blocks('cmake')
        self.assertEqual(len(listed), 1)
        self.assertIn('CMAKE_CONFIGURE_DEPENDS', listed[0])
        self.assertIn('code.txt', listed[0])


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
