#!/usr/bin/env python3
"""Checks for fst.py. Run them with `python3 -m unittest discover -s recomp -p 'test_*.py'`.

The images are written here, each as a plain image and as a CISO, so no disc is needed.
The whole-disc checks at the bottom run on those, and also on the image GCN_TEST_IMAGE names
when it's set. They print counts and never a name, an offset or a size from that image.
"""
import contextlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fst  # noqa: E402
import rel  # noqa: E402
from dol import DiscImage  # noqa: E402

FST_PY = os.path.join(HERE, 'fst.py')

# Two directories and a file, where the entry order (b, z, c) isn't the name order (b, c, z).
SMALL = bytes.fromhex(
    '01000000 00000000 00000004'
    '01000000 00000000 00000003'
    '00000002 00001000 00000005'
    '00000004 00002000 00000006'
    '6200 7a00 6300')


def small_tree():
    return fst.Entry('', children=[
        fst.Entry('c', 0x2000, 6),
        fst.Entry('b', children=[fst.Entry('z', 0x1000, 5)])])


def raw(entries, names):
    """An FST from (type, name offset, a, b) tuples and the name table's bytes."""
    return b''.join(struct.pack('>III', t << 24 | n, a, b) for t, n, a, b in entries) + names


# The fixture's files with their sizes, in FST order. Names compare in uppercase, so '_' sorts
# after the letters. That puts '_notes.txt' last in its directory, and 'ground_lit.tex' after
# 'groundMap.tex', where a lowercase compare would put each one before.
FILES = [
    ('audio/bgm/theme.hps', 5000),
    ('audio/sfx.dat', 0x800),
    ('audio/_notes.txt', 12),
    ('Data/empty.bin', 0),
    ('Data/maps/ground.tex', 70),
    ('Data/maps/groundMap.tex', 9),
    ('Data/maps/ground_lit.tex', 21),
    ('Data/readme text.txt', 33),
    ('opening.bnr', 1),
    ('Zeta.bin', 4100),
]
EMPTY_DIRS = ['audio/empty_dir']
SIZES = dict(FILES)
FST_AT = 0x2000
DATA_AT = 0x4000


def payload(path):
    return bytes((len(path) * 13 + k * 7) % 255 + 1 for k in range(SIZES[path]))


def subdir(node, name):
    for child in node.children:
        if child.name == name:
            return child
    child = fst.Entry(name, children=[])
    node.children.append(child)
    return child


def ciso(iso, block=0x800):
    iso = iso + bytes(-len(iso) % block)
    blocks = [iso[i:i + block] for i in range(0, len(iso), block)]
    stored = [1 if any(b) else 0 for b in blocks]
    head = b'CISO' + struct.pack('<I', block) + bytes(stored)
    return head + bytes(0x8000 - len(head)) + b''.join(b for b, s in zip(blocks, stored) if s)


class Disc:
    """The fixture: files in a tree inserted back to front, so build has to sort it."""

    def __init__(self):
        self.offsets = {}
        pos = DATA_AT
        for path, size in FILES:
            self.offsets[path] = pos
            pos += (size + 0x1F) & ~0x1F
        self.tree = fst.Entry('', children=[])
        for path in [p for p, _ in reversed(FILES)] + EMPTY_DIRS:
            *dirs, name = path.split('/')
            node = self.tree
            for d in dirs:
                node = subdir(node, d)
            if path in SIZES:
                node.children.append(fst.Entry(name, self.offsets[path], SIZES[path]))
            else:
                subdir(node, name)
        self.fst = fst.build(self.tree)
        iso = bytearray(pos + (-pos % 0x800))
        iso[0:6] = b'TEST00'
        self.max = len(self.fst) + 0x40
        struct.pack_into('>4I', iso, 0x420, 0x1000, FST_AT, len(self.fst), self.max)
        iso[FST_AT:FST_AT + len(self.fst)] = self.fst
        for path, size in FILES:
            iso[self.offsets[path]:self.offsets[path] + size] = payload(path)
        self.iso = bytes(iso)

    def listing(self):
        return [f'{self.offsets[p]:#010x} {s:>10} {p}' for p, s in FILES]


class TmpCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='fst test ')
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name

    def images(self):
        """(path, DiscImage) for the fixture as a plain image and as a CISO."""
        disc = Disc()
        for name, data in (('game.iso', disc.iso), ('game.ciso', ciso(disc.iso))):
            path = os.path.join(self.tmp, name)
            with open(path, 'wb') as f:
                f.write(data)
            img = DiscImage(path)
            self.addCleanup(img.f.close)
            yield path, img


class TestLayout(unittest.TestCase):
    def test_build_writes_names_in_entry_order(self):
        self.assertEqual(fst.build(small_tree()), SMALL)

    def test_parse_reads_the_layout(self):
        self.assertEqual(fst.parse(SMALL), fst.Entry('', children=[
            fst.Entry('b', children=[fst.Entry('z', 0x1000, 5)]),
            fst.Entry('c', 0x2000, 6)]))

    def test_empty_directory_is_followed_by_its_sibling(self):
        data = fst.build(fst.Entry('', children=[
            fst.Entry('a', children=[]), fst.Entry('b', 7, 8)]))
        self.assertEqual(data, bytes.fromhex(
            '01000000 00000000 00000003'
            '01000000 00000000 00000002'
            '00000002 00000007 00000008'
            '6100 6200'))
        self.assertEqual(fst.build(fst.parse(data)), data)

    def test_nested_directories_carry_their_parent_index(self):
        tree = fst.Entry('', children=[fst.Entry('a', children=[
            fst.Entry('c', children=[fst.Entry('g', 0x10, 2)]),
            fst.Entry('b', children=[fst.Entry('f', 0, 1)])])])
        data = bytes.fromhex(
            '01000000 00000000 00000006'
            '01000000 00000000 00000006'   # a: parent 0
            '01000002 00000001 00000004'   # b: parent 1
            '00000004 00000000 00000001'   # f
            '01000006 00000001 00000006'   # c: parent 1, not b's index
            '00000008 00000010 00000002'   # g
            '6100 6200 6600 6300 6700')
        self.assertEqual(fst.build(tree), data)
        self.assertEqual(fst.build(fst.parse(data)), data)

    def test_build_orders_each_directory_by_uppercase_name_then_raw_name(self):
        names = ['Zed', 'alpha', '_under', 'Beta.bin', 'beta2', '10', 'Alpha']
        tree = fst.Entry('', children=[fst.Entry(n, 0, 0) for n in names])
        order = [e.name for e in fst.parse(fst.build(tree)).children]
        self.assertEqual(order, ['10', 'Alpha', 'alpha', 'Beta.bin', 'beta2', 'Zed', '_under'])

    def test_a_layout_in_uppercase_order_round_trips(self):
        # 'bc.bin' before 'b_c.bin', as a disc written in uppercase order has them.
        data = raw([(1, 0, 0, 3), (0, 0, 0x1000, 1), (0, 7, 0x2000, 1)], b'bc.bin\0b_c.bin\0')
        self.assertEqual(fst.build(fst.parse(data)), data)

    def test_build_leaves_its_input_in_order(self):
        tree = small_tree()
        fst.build(tree)
        self.assertEqual([c.name for c in tree.children], ['c', 'b'])

    def test_an_offset_or_size_that_isnt_32_bits_is_refused(self):
        for offset, size in ((1 << 32, 0), (0, 1 << 32), (-1, 0), (0, -1)):
            tree = fst.Entry('', children=[fst.Entry('big', offset, size)])
            with self.assertRaisesRegex(fst.FstError, r"entry 1 \('big'\)"):
                fst.build(tree)

    def test_a_name_offset_that_isnt_24_bits_is_refused(self):
        def tree(length):
            return fst.Entry('', children=[fst.Entry('x' * length, 0, 1), fst.Entry('y', 0, 1)])
        data = fst.build(tree(0xFFFFFE))  # 'y' lands at 0xFFFFFF, the last offset that fits
        self.assertEqual(struct.unpack_from('>I', data, 24)[0], 0xFFFFFF)
        with self.assertRaisesRegex(fst.FstError, r"entry 2 \('y'\).*24 bits"):
            fst.build(tree(0xFFFFFF))

    def test_the_largest_offset_and_size_round_trip(self):
        tree = fst.Entry('', children=[fst.Entry('big', 0xFFFFFFFF, 0xFFFFFFFF)])
        self.assertEqual(fst.parse(fst.build(tree)), tree)


class TestRefusals(unittest.TestCase):
    def test_parse_refuses_a_non_ascii_name(self):
        data = raw([(1, 0, 0, 3), (0, 0, 0, 1), (0, 6, 0, 1)], b'caf\xc3\xa9\0ok\0')
        with self.assertRaisesRegex(fst.FstError, r"entry 1 \('caf.*ASCII"):
            fst.parse(data)

    def test_build_refuses_a_non_ascii_name(self):
        tree = fst.Entry('', children=[fst.Entry('ok', 0, 1), fst.Entry('xé', 0, 1)])
        with self.assertRaisesRegex(fst.FstError, r"entry 2 \('x.*ASCII"):
            fst.build(tree)

    def test_names_that_cant_be_path_parts_are_refused(self):
        for name in ('', '.', '..', 'a/b'):
            tree = fst.Entry('', children=[fst.Entry(name, 0, 1)])
            with self.assertRaisesRegex(fst.FstError, r'entry 1 .*path'):
                fst.build(tree)
        data = raw([(1, 0, 0, 2), (0, 0, 0, 1)], b'..\0')
        with self.assertRaisesRegex(fst.FstError, r"entry 1 \('\.\.'\).*path"):
            fst.parse(data)

    def test_parse_refuses_a_table_cut_inside_an_entry(self):
        with self.assertRaisesRegex(fst.FstError, r'entry 2 is cut off'):
            fst.parse(SMALL[:30])

    def test_parse_refuses_a_table_with_no_whole_root(self):
        for length in (0, 5, 11):
            with self.assertRaisesRegex(fst.FstError, r'entry 0 is cut off'):
                fst.parse(SMALL[:length])

    def test_parse_refuses_a_table_cut_before_its_names(self):
        with self.assertRaisesRegex(fst.FstError, r'entry 1 .*past the name table'):
            fst.parse(SMALL[:48])

    def test_parse_refuses_a_name_cut_before_its_end(self):
        with self.assertRaisesRegex(fst.FstError, r'entry 3 .*cut off'):
            fst.parse(SMALL[:-1])

    def test_parse_refuses_a_name_offset_past_the_table(self):
        data = raw([(1, 0, 0, 3), (0, 0, 0, 1), (0, 100, 0, 1)], b'a\0b\0')
        with self.assertRaisesRegex(fst.FstError, r'entry 2 .*past the name table'):
            fst.parse(data)

    def test_parse_refuses_a_name_offset_exactly_at_the_table_end(self):
        data = raw([(1, 0, 0, 2), (0, 4, 0, 1)], b'a\0b\0')
        with self.assertRaisesRegex(fst.FstError, r'entry 1 .*past the name table'):
            fst.parse(data)

    def test_parse_refuses_a_directory_that_ends_outside_its_parent(self):
        for end in (9, 1, 0):
            data = raw([(1, 0, 0, 3), (1, 0, 0, end), (0, 2, 0, 1)], b'a\0b\0')
            with self.assertRaisesRegex(fst.FstError, r"entry 1 \('a'\).*ends"):
                fst.parse(data)

    def test_parse_refuses_an_unknown_type(self):
        data = raw([(1, 0, 0, 2), (2, 0, 0, 1)], b'a\0')
        with self.assertRaisesRegex(fst.FstError, r"entry 1 \('a'\).*type 2"):
            fst.parse(data)

    def test_parse_refuses_two_entries_with_one_name_in_a_directory(self):
        for second in ((0, 0, 0, 1), (1, 0, 0, 3)):
            data = raw([(1, 0, 0, 3), (0, 0, 0, 1), second], b'a\0')
            with self.assertRaisesRegex(fst.FstError, r"entry 2 \('a'\) repeats"):
                fst.parse(data)

    def test_parse_allows_one_name_in_two_directories(self):
        data = raw([(1, 0, 0, 4), (1, 0, 0, 3), (0, 2, 0, 1), (0, 2, 0, 1)], b'd\0a\0')
        self.assertEqual(fst.parse(data), fst.Entry('', children=[
            fst.Entry('d', children=[fst.Entry('a', 0, 1)]), fst.Entry('a', 0, 1)]))

    def test_parse_refuses_a_root_that_is_a_file(self):
        with self.assertRaisesRegex(fst.FstError, r'entry 0 '):
            fst.parse(raw([(0, 0, 0, 1)], b''))


class TestImages(TmpCase):
    def test_read_gives_the_header_words_and_the_tree(self):
        disc = Disc()
        for _, img in self.images():
            off, size, mx, tree = fst.read(img)
            self.assertEqual((off, size, mx), (FST_AT, len(disc.fst), disc.max))
            self.assertEqual([(p, e.offset, e.size) for p, e in fst.walk(tree)],
                             [(p, disc.offsets[p], s) for p, s in FILES])

    def test_round_trip_is_byte_identical(self):
        disc = Disc()
        self.assertEqual(fst.build(fst.parse(disc.fst)), disc.fst)
        for _, img in self.images():
            tree = fst.read(img)[3]
            self.assertEqual(fst.build(tree), disc.fst)
            self.assertEqual(tree, fst.parse(disc.fst))

    def test_directories_come_out_in_build_order(self):
        root = fst.parse(Disc().fst)
        self.assertEqual([c.name for c in root.children],
                         ['audio', 'Data', 'opening.bnr', 'Zeta.bin'])
        audio, data = root.children[:2]
        self.assertEqual([c.name for c in audio.children],
                         ['bgm', 'empty_dir', 'sfx.dat', '_notes.txt'])
        maps = data.children[1]
        self.assertEqual([c.name for c in maps.children],
                         ['ground.tex', 'groundMap.tex', 'ground_lit.tex'])

    def test_read_refuses_an_image_with_no_header(self):
        path = os.path.join(self.tmp, 'short.iso')
        with open(path, 'wb') as f:
            f.write(b'TEST00')
        img = DiscImage(path)
        self.addCleanup(img.f.close)
        with self.assertRaisesRegex(fst.FstError, r'header'):
            fst.read(img)

    def test_read_refuses_an_image_cut_inside_its_fst(self):
        path = os.path.join(self.tmp, 'cut.iso')
        with open(path, 'wb') as f:
            f.write(Disc().iso[:FST_AT + 30])
        img = DiscImage(path)
        self.addCleanup(img.f.close)
        with self.assertRaisesRegex(fst.FstError, r'entry 2 is cut off'):
            fst.read(img)


class TestCli(TmpCase):
    def run_main(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            fst.main(list(args))
        return out.getvalue()

    def tree_on_disk(self, out):
        found = {}
        for base, _, names in os.walk(out):
            for name in names:
                path = os.path.join(base, name)
                with open(path, 'rb') as f:
                    found[os.path.relpath(path, out).replace(os.sep, '/')] = f.read()
        return found

    def test_list_prints_offset_size_and_path_per_file(self):
        want = Disc().listing()
        for path, _ in self.images():
            self.assertEqual(self.run_main('list', path).splitlines(), want)

    def test_list_runs_as_a_script(self):
        want = Disc().listing()
        for path, _ in self.images():
            done = subprocess.run([sys.executable, '-B', FST_PY, 'list', path],
                                  capture_output=True, text=True)
            self.assertEqual((done.returncode, done.stdout.splitlines()), (0, want))

    def test_extract_copies_every_file_into_a_path_with_a_space(self):
        want = {p: payload(p) for p, _ in FILES}
        for i, (path, _) in enumerate(self.images()):
            out = os.path.join(self.tmp, f'out dir {i}')
            self.run_main('extract', path, out)
            self.assertEqual(self.tree_on_disk(out), want)

    def test_extract_into_an_existing_empty_directory(self):
        out = os.path.join(self.tmp, 'empty out')
        os.mkdir(out)
        path = next(self.images())[0]
        self.run_main('extract', path, out)
        self.assertEqual(self.tree_on_disk(out), {p: payload(p) for p, _ in FILES})

    def test_extract_copies_only_the_named_files(self):
        picks = ['Data/readme text.txt', 'audio/bgm/theme.hps', 'Data/empty.bin']
        for i, (path, _) in enumerate(self.images()):
            out = os.path.join(self.tmp, f'some {i}')
            self.run_main('extract', path, out, *picks)
            self.assertEqual(self.tree_on_disk(out), {p: payload(p) for p in picks})

    def test_extract_refuses_a_directory_that_has_files(self):
        path = next(self.images())[0]
        out = os.path.join(self.tmp, 'full')
        os.mkdir(out)
        keep = os.path.join(out, 'keep.txt')
        with open(keep, 'wb') as f:
            f.write(b'mine')
        with self.assertRaises(SystemExit) as cm:
            self.run_main('extract', path, out)
        self.assertRegex(str(cm.exception.code), r'^error: .*full')
        self.assertEqual(os.listdir(out), ['keep.txt'])

    def test_extract_refuses_an_out_that_is_a_file(self):
        path = next(self.images())[0]
        with self.assertRaises(SystemExit):
            self.run_main('extract', path, path)

    def test_extract_writes_nothing_when_a_path_is_missing(self):
        path = next(self.images())[0]
        out = os.path.join(self.tmp, 'none')
        for bad in ('nope.bin', 'audio'):
            with self.assertRaises(SystemExit) as cm:
                self.run_main('extract', path, out, 'opening.bnr', bad)
            self.assertRegex(str(cm.exception.code), r'^error: ' + bad)
            self.assertFalse(os.path.exists(out))

    def test_extract_refuses_an_image_that_ends_inside_a_file(self):
        path = os.path.join(self.tmp, 'short.iso')
        disc = Disc()
        with open(path, 'wb') as f:
            f.write(disc.iso[:disc.offsets['Zeta.bin'] + 10])
        out = os.path.join(self.tmp, 'cut out')
        with self.assertRaises(SystemExit) as cm:
            self.run_main('extract', path, out, 'Zeta.bin')
        self.assertRegex(str(cm.exception.code), r'^error: .*Zeta.bin')

    def test_a_missing_image_is_an_error_not_a_traceback(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_main('list', os.path.join(self.tmp, 'absent.iso'))
        self.assertRegex(str(cm.exception.code), r'^error: ')


class TestExtractTree(TmpCase):
    """extract on trees that parse didn't make, such as one built from a folder of files."""

    def refuses(self, tree, pattern, *paths):
        img = next(self.images())[1]
        out = os.path.join(self.tmp, 'out')
        with self.assertRaisesRegex(fst.FstError, pattern):
            fst.extract(img, tree, out, paths)
        self.assertFalse(os.path.exists(out))
        self.assertFalse(os.path.exists(os.path.join(self.tmp, 'escaped')))

    def test_a_name_that_cant_be_a_path_part_is_refused_before_anything_is_written(self):
        for name in ('..', '.', '', 'a/b', 'a\0b'):
            tree = fst.Entry('', children=[
                fst.Entry(name, children=[fst.Entry('escaped', 0, 1)])])
            self.refuses(tree, r"an entry in the root \(.*can't be a path part")
        tree = fst.Entry('', children=[fst.Entry('d', children=[fst.Entry('..', 0, 1)])])
        self.refuses(tree, r"an entry in 'd' \('\.\.'\)")

    def test_names_that_differ_only_by_case_are_refused_naming_both(self):
        folder = fst.Entry('d', children=[fst.Entry('A.bin', 0, 1), fst.Entry('a.bin', 0, 1)])
        self.refuses(fst.Entry('', children=[folder]), r"'d' has 'A.bin' and 'a.bin'")
        twins = [fst.Entry('Dir', children=[fst.Entry('f', 0, 1)]),
                 fst.Entry('dir', children=[fst.Entry('g', 0, 1)])]
        self.refuses(fst.Entry('', children=twins), r"the root has 'Dir' and 'dir'")

    def test_a_name_twice_in_a_directory_is_refused(self):
        tree = fst.Entry('', children=[fst.Entry('x', 0, 1), fst.Entry('x', 0, 1)])
        self.refuses(tree, r"the root has 'x' and 'x'")

    def test_one_name_in_two_directories_extracts(self):
        tree = fst.Entry('', children=[fst.Entry('d', children=[fst.Entry('a', 0, 1)]),
                                       fst.Entry('e', children=[fst.Entry('a', 0, 1)])])
        img = next(self.images())[1]
        out = os.path.join(self.tmp, 'out')
        fst.extract(img, tree, out)
        for path in ('d/a', 'e/a'):
            with open(os.path.join(out, *path.split('/')), 'rb') as f:
                self.assertEqual(f.read(), b'T')

    def test_a_bad_name_in_a_file_that_isnt_asked_for_is_still_refused(self):
        tree = fst.Entry('', children=[fst.Entry('ok', 0, 1), fst.Entry('a/b', 0, 1)])
        self.refuses(tree, r'a/b', 'ok')


class DiscChecks:
    """Checks on a whole disc. They run on the fixture, and on a real disc when one is named."""

    def open_image(self):
        raise NotImplementedError

    def setUp(self):
        super().setUp()
        self.img = self.open_image()
        self.addCleanup(self.img.f.close)
        refused = False
        try:
            self.off, self.size, self.max, self.tree = fst.read(self.img)
        except fst.FstError:
            refused = True  # the message names a disc entry, so it's never printed
        self.assertFalse(refused, 'fst.read refused the image')

    def test_round_trip_is_byte_identical(self):
        data = self.img.read(self.off, self.size)
        self.assertTrue(len(data) == self.size, 'the image ends inside its FST')
        self.assertTrue(fst.build(self.tree) == data, 'build(parse(fst)) differs from the FST')

    def test_agrees_with_rel_disc_files(self):
        ours = {path: (e.offset, e.size) for path, e in fst.walk(self.tree)}
        theirs = rel.disc_files(self.img)
        self.assertEqual(len(ours), len(theirs))
        self.assertTrue(ours == theirs, 'fst and rel.disc_files disagree')

    def test_every_directory_is_in_build_order(self):
        todo, dirs, wrong = [self.tree], 0, 0
        while todo:
            node = todo.pop()
            dirs += 1
            names = [c.name for c in node.children]
            if names != sorted(names, key=fst.sort_key):
                wrong += 1
            todo.extend(c for c in node.children if c.children is not None)
        self.assertGreater(dirs, 1)
        self.assertEqual(wrong, 0, f'{wrong} of {dirs} directories are out of order')


class TestFixtureDisc(DiscChecks, TmpCase):
    ciso = False

    def open_image(self):
        data = Disc().iso
        path = os.path.join(self.tmp, 'game.ciso' if self.ciso else 'game.iso')
        with open(path, 'wb') as f:
            f.write(ciso(data) if self.ciso else data)
        return DiscImage(path)


class TestFixtureCisoDisc(TestFixtureDisc):
    ciso = True


@unittest.skipUnless(os.environ.get('GCN_TEST_IMAGE'), 'GCN_TEST_IMAGE names no image')
class TestDisc(DiscChecks, unittest.TestCase):
    def open_image(self):
        return DiscImage(os.environ['GCN_TEST_IMAGE'])


if __name__ == '__main__':
    unittest.main()
