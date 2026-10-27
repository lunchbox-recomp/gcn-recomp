#!/usr/bin/env python3
"""Checks for disc_overlay.py. Run them with `python3 -m unittest discover -s recomp -p 'test_*.py'`.

The images and the files folders are written here, so no disc is needed. The images are the
fixture test_fst.py writes, as a plain image and as a CISO.
"""
import contextlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import disc_overlay as ov  # noqa: E402
import fst  # noqa: E402
from dol import DiscImage  # noqa: E402
from test_fst import FILES, FST_AT, SIZES, Disc, ciso, subdir  # noqa: E402

OVERLAY_PY = os.path.join(HERE, 'disc_overlay.py')

START = 0x57058000  # a GameCube disc's size, where the layout starts for a small image
SLOT = 0x8000
FST_LEN = len(Disc().fst)
OLD_MAX = FST_LEN + 0x40


def arena(fst_max):
    return (0x817FE000 - fst_max) & ~0x1F


def listing(tree):
    return {path: (e.offset, e.size) for path, e in fst.walk(tree)}


def read_manifest(out):
    """The manifest as (line kinds, base fields, header words, ranges)."""
    with open(os.path.join(out, 'disc-overlay.txt'), 'rb') as f:
        text = f.read().decode('utf-8')
    lines = text.split('\n')
    assert lines.pop() == '', 'the manifest has to end in a newline'
    kinds = [line.split(' ')[0] for line in lines]
    headers, ranges, base = {}, [], None
    for line in lines[1:]:
        kind, rest = line.split(' ', 1)
        if kind == 'base':
            base = rest.split(' ')
        elif kind == 'header':
            word, value = rest.split(' ')
            headers[int(word, 16)] = int(value, 16)
        elif kind == 'range':
            at, size, rel = rest.split(' ', 2)
            ranges.append((int(at, 16), int(size, 16), rel))
    return kinds, base, headers, ranges


def ciso_far(block, stored_blocks):
    """A CISO with a block of zeros at each index in stored_blocks, and the fixture in block 0."""
    flags = bytearray(0x8000 - 8)
    for i in stored_blocks:
        flags[i] = 1
    head = b'CISO' + struct.pack('<I', block) + bytes(flags)
    first = Disc().iso
    return head + first + bytes(block - len(first)) + bytes(block) * (len(stored_blocks) - 1)


class OverlayCase(unittest.TestCase):
    """OUT holds the files folder, as the tool needs. The fixture image sits beside OUT."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='overlay test ')
        self.addCleanup(tmp.cleanup)
        self.tmp = os.path.realpath(tmp.name)
        self.out = os.path.join(self.tmp, 'out dir')
        self.mod = os.path.join(self.out, 'mod files')
        os.makedirs(self.mod)
        self.disc = Disc()
        self.image = self.write('game.iso', self.disc.iso)

    def write(self, name, data):
        path = os.path.join(self.tmp, name)
        with open(path, 'wb') as f:
            f.write(data)
        return path

    def put(self, rel, data=b'x', folder=None):
        path = os.path.join(folder or self.mod, *rel.split('/'))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)
        return path

    def sparse(self, rel, size):
        path = self.put(rel)
        os.truncate(path, size)
        if os.stat(path).st_blocks * 512 > 1 << 24:
            self.skipTest('this file system has no sparse files')
        return path

    def main(self, *extra, files=None, out=None, image=None):
        argv = [image or self.image, out or self.out, '--files', files or self.mod] + list(extra)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            ov.main(argv)
        return json.loads(buf.getvalue())

    def refuses(self, pattern, *extra, **kw):
        with self.assertRaises(SystemExit) as cm:
            self.main(*extra, **kw)
        self.assertRegex(str(cm.exception.code), r'^error: ' + pattern)
        for name in ('fst.bin', 'disc-overlay.txt'):
            self.assertFalse(os.path.exists(os.path.join(self.out, name)), name)

    def base_line(self):
        """The base fields for the fixture. They name the disc's FST, never the grown one."""
        return ['TEST00', f'{FST_AT:x}', f'{FST_LEN:x}', f'{ov.fnv1a64(self.disc.fst):016x}']

    def table(self, out=None):
        with open(os.path.join(out or self.out, 'fst.bin'), 'rb') as f:
            return f.read()

    def tree(self, out=None):
        return fst.parse(self.table(out))

    def twin_image(self):
        """The fixture with a 4-byte 'zeta.bin' at a made-up offset beside 'Zeta.bin'."""
        tree = fst.parse(self.disc.fst)
        tree.children.append(fst.Entry('zeta.bin', 0x6000, 4))
        table = fst.build(tree)
        data = bytearray(self.disc.iso)
        struct.pack_into('>2I', data, 0x428, len(table), len(table) + 0x40)
        data[FST_AT:FST_AT + len(table)] = table
        return self.write('twins.iso', bytes(data))


class TestReplace(OverlayCase):
    def test_a_same_size_replacement_keeps_the_size_words_and_the_heap(self):
        self.put('Data/readme text.txt', b'y' * SIZES['Data/readme text.txt'])
        summary = self.main()
        self.assertEqual(summary, {
            'added_files': 0, 'added_bytes': 0, 'replaced_files': 1, 'replaced_bytes': 33,
            'fst_growth': 0, 'arena_hi_drop': 0})
        kinds, base, headers, ranges = read_manifest(self.out)
        self.assertEqual(headers, {0x424: START})
        self.assertEqual(len(self.table()), FST_LEN)
        self.assertEqual(ranges, [(START, FST_LEN, 'fst.bin'),
                                  (START + SLOT, 33, 'mod files/Data/readme text.txt')])

    def test_the_manifest_text(self):
        self.put('Data/readme text.txt', b'y' * 33)
        self.main()
        with open(os.path.join(self.out, 'disc-overlay.txt'), 'rb') as f:
            text = f.read().decode('ascii')
        base = ov.fnv1a64(self.disc.fst)
        self.assertEqual(text, (
            'gcn-disc-overlay 1\n'
            f'base TEST00 2000 {FST_LEN:x} {base:016x}\n'
            'header 424 57058000\n'
            f'range 57058000 {FST_LEN:x} fst.bin\n'
            'range 57060000 21 mod files/Data/readme text.txt\n'))

    def test_the_image_is_not_changed(self):
        self.put('Zeta.bin', b'new')
        self.put('extra.cfg')
        self.main()
        with open(self.image, 'rb') as f:
            self.assertEqual(f.read(), self.disc.iso)

    def test_a_replacement_in_another_case_replaces_the_file(self):
        self.put('DATA/MAPS/GROUND.TEX', b'a' * 5)
        self.put('audio/BGM/THEME.hps', b'b' * 6)
        summary = self.main()
        self.assertEqual((summary['added_files'], summary['replaced_files']), (0, 2))
        found = listing(self.tree())
        self.assertEqual(sorted(found), sorted(p for p, _ in FILES))
        self.assertEqual(found['audio/bgm/theme.hps'], (START + SLOT, 6))
        self.assertEqual(found['Data/maps/ground.tex'], (START + 2 * SLOT, 5))

    def test_a_new_file_in_a_folder_spelled_another_way_joins_that_folder(self):
        self.put('AUDIO/Bgm/extra.hps', b'e')
        summary = self.main()
        self.assertEqual((summary['added_files'], summary['replaced_files']), (1, 0))
        found = listing(self.tree())
        self.assertIn('audio/bgm/extra.hps', found)
        self.assertEqual(len(found), len(FILES) + 1)
        root = self.tree()
        self.assertEqual([c.name for c in root.children],
                         ['audio', 'Data', 'opening.bnr', 'Zeta.bin'])

    def test_every_base_file_is_listed_once_at_its_size(self):
        self.put('opening.bnr', b'n' * 7)
        self.put('Data/maps/new map.tex', b'm' * 9)
        self.put('fresh/inner/deep.cfg', b'd' * 4)
        self.main()
        found = listing(self.tree())
        self.assertEqual(len(found), len(FILES) + 2)
        for path, size in FILES:
            if path == 'opening.bnr':
                self.assertEqual(found[path][1], 7)
            else:
                self.assertEqual(found[path], (self.disc.offsets[path], size), path)
        self.assertEqual(found['Data/maps/new map.tex'][1], 9)
        self.assertEqual(found['fresh/inner/deep.cfg'][1], 4)

    def test_a_ciso_gives_the_same_overlay_as_a_plain_image(self):
        self.put('Data/new.cfg', b'n')
        self.put('Zeta.bin', b'z' * 3)
        plain = self.main()
        want = (self.table(), read_manifest(self.out))
        image = self.write('game.ciso', ciso(self.disc.iso))
        self.assertEqual(self.main(image=image), plain)
        self.assertEqual((self.table(), read_manifest(self.out)), want)


class TestAdd(OverlayCase):
    def test_an_addition_grows_the_fst_inside_the_max(self):
        self.put('Data/new.cfg', b'abc')
        summary = self.main()
        growth = 12 + len('new.cfg') + 1
        self.assertEqual(growth, 20)
        self.assertLess(FST_LEN + growth, OLD_MAX)
        self.assertEqual(summary, {
            'added_files': 1, 'added_bytes': 3, 'replaced_files': 0, 'replaced_bytes': 0,
            'fst_growth': growth, 'arena_hi_drop': 0})
        _, base, headers, _ = read_manifest(self.out)
        self.assertEqual(headers, {0x424: START, 0x428: FST_LEN + growth, 0x42C: OLD_MAX})
        self.assertEqual(len(self.table()), FST_LEN + growth)
        self.assertEqual(base, self.base_line())

    def test_growth_past_the_max_raises_fst_max_and_lowers_arena_hi(self):
        for i in range(6):
            self.put(f'Data/extra/file number {i}.cfg')
        summary = self.main()
        size = len(self.table())
        self.assertGreater(size, OLD_MAX)
        _, base, headers, _ = read_manifest(self.out)
        self.assertEqual(headers, {0x424: START, 0x428: size, 0x42C: size})
        self.assertEqual(base, self.base_line())
        self.assertEqual(summary['fst_growth'], size - FST_LEN)
        self.assertGreater(summary['arena_hi_drop'], 0)
        self.assertEqual(summary['arena_hi_drop'], arena(OLD_MAX) - arena(size))
        self.assertEqual(summary['arena_hi_drop'] % 32, 0)

    def test_new_directories_nest_right(self):
        self.put('newdir/sub/a.cfg', b'a')
        self.put('audio/fresh/b.cfg', b'bb')
        self.put('audio/Aardvark.dat', b'c')
        self.put('audio/zz.dat', b'd')
        self.main()
        want = fst.parse(self.disc.fst)
        audio = want.children[0]
        audio.children.append(fst.Entry('Aardvark.dat', START + SLOT, 1))
        subdir(audio, 'fresh').children.append(fst.Entry('b.cfg', START + 2 * SLOT, 2))
        audio.children.append(fst.Entry('zz.dat', START + 3 * SLOT, 1))
        want.children.append(fst.Entry('newdir', children=[
            fst.Entry('sub', children=[fst.Entry('a.cfg', START + 4 * SLOT, 1)])]))
        self.assertEqual(self.table(), fst.build(want))
        got = self.tree()
        self.assertEqual([c.name for c in got.children[0].children],
                         ['Aardvark.dat', 'bgm', 'empty_dir', 'fresh', 'sfx.dat', 'zz.dat',
                          '_notes.txt'])
        self.assertEqual([c.name for c in got.children],
                         ['audio', 'Data', 'newdir', 'opening.bnr', 'Zeta.bin'])

    def test_a_new_name_goes_in_sort_order_not_after_the_underscore(self):
        self.put('audio/Zulu.dat')
        self.main()
        audio = self.tree().children[0]
        self.assertEqual([c.name for c in audio.children],
                         ['bgm', 'empty_dir', 'sfx.dat', 'Zulu.dat', '_notes.txt'])


class TestDol(OverlayCase):
    def dol(self, size=0x200, name='boot.dol'):
        return self.put(name, bytes(size), folder=self.out)

    def test_dol_maps_word_0x420_to_a_range_after_the_fst(self):
        self.put('Data/new.cfg', b'abc')
        summary = self.main('--dol', self.dol())
        headers, ranges = read_manifest(self.out)[2:]
        self.assertEqual(headers[0x420], START + SLOT)
        self.assertEqual(headers[0x424], START)
        self.assertIn((START + SLOT, 0x200, 'boot.dol'), ranges)
        self.assertEqual(ranges[-1], (START + 2 * SLOT, 3, 'mod files/Data/new.cfg'))
        self.assertEqual((summary['added_files'], summary['added_bytes']), (1, 3))

    def test_a_dol_alone_changes_only_words_0x420_and_0x424(self):
        self.main('--dol', self.dol())
        self.assertEqual(read_manifest(self.out)[2], {0x420: START + SLOT, 0x424: START})

    def test_a_dol_that_is_too_small_is_refused(self):
        self.refuses(r'the DOL .* under 0x100 bytes', '--dol', self.dol(0xFF))

    def test_a_dol_outside_out_is_refused(self):
        path = self.put('boot.dol', bytes(0x200), folder=self.tmp)
        self.refuses(r'the DOL .* has to be inside', '--dol', path)

    def test_a_dol_that_is_a_symlink_is_refused(self):
        os.symlink(self.dol(), os.path.join(self.out, 'link.dol'))
        self.refuses(r'the DOL .* is a symbolic link', '--dol', os.path.join(self.out, 'link.dol'))

    def test_a_missing_dol_is_refused(self):
        self.refuses(r"the DOL .* isn't a file", '--dol', os.path.join(self.out, 'none.dol'))


class TestSkips(OverlayCase):
    def test_dotfiles_and_dot_folders_are_skipped(self):
        self.put('.DS_Store')
        self.put('Data/.DS_Store')
        self.put('.hidden/inside.cfg')
        self.put('Data/.git/config')
        self.put('Data/kept.cfg', b'k')
        summary = self.main()
        self.assertEqual((summary['added_files'], summary['replaced_files']), (1, 0))
        ranges = read_manifest(self.out)[3]
        self.assertEqual([r[2] for r in ranges], ['fst.bin', 'mod files/Data/kept.cfg'])
        found = listing(self.tree())
        self.assertEqual(len(found), len(FILES) + 1)

    def test_a_dotfile_that_is_a_symlink_is_skipped_not_refused(self):
        os.symlink(self.tmp, os.path.join(self.mod, '.link'))
        self.put('a.cfg')
        self.assertEqual(self.main()['added_files'], 1)

    def test_an_empty_folder_adds_nothing(self):
        os.makedirs(os.path.join(self.mod, 'Data', 'empty folder'))
        self.assertEqual(self.main()['added_files'], 0)
        self.assertEqual(self.table(), self.disc.fst)


class TestRefusals(OverlayCase):
    def test_a_symlink_to_a_file_is_refused(self):
        target = self.put('real.cfg', folder=self.out)
        os.symlink(target, os.path.join(self.mod, 'link.cfg'))
        self.refuses(r'link\.cfg is a symbolic link')

    def test_a_symlink_to_a_folder_is_refused_in_a_subfolder(self):
        other = os.path.join(self.out, 'other')
        os.makedirs(other)
        self.put('Data/real.cfg')
        os.symlink(other, os.path.join(self.mod, 'Data', 'linked'))
        self.refuses(r'Data/linked is a symbolic link')

    def test_a_files_folder_that_is_a_symlink_is_refused(self):
        link = os.path.join(self.out, 'link to mod')
        os.symlink(self.mod, link)
        self.put('a.cfg')
        self.refuses(r'the files folder .* is a symbolic link', files=link)
        self.refuses(r'the files folder .* is a symbolic link', files=link + os.sep)

    def test_a_name_that_is_not_ascii_is_refused(self):
        self.put('Data/café.cfg')
        self.refuses(r"Data/caf.*\.cfg has a name that isn't ASCII")

    def test_a_control_character_in_a_path_is_refused(self):
        self.put('Data/two\nlines.cfg')
        self.refuses(r'.* has a control character in its path')

    def test_a_files_folder_outside_out_is_refused(self):
        elsewhere = os.path.join(self.tmp, 'elsewhere')
        self.put('a.cfg', folder=elsewhere)
        self.refuses(r'the files folder .* has to be inside', files=elsewhere)
        sneaky = os.path.join(self.mod, '..', '..', 'elsewhere')
        self.refuses(r'the files folder .* has to be inside', files=sneaky)

    def test_out_itself_is_not_a_files_folder(self):
        self.refuses(r'the files folder .* has to be inside', files=self.out)

    def test_out_has_to_be_a_folder(self):
        self.refuses(r".*nothing isn't a folder", out=os.path.join(self.tmp, 'nothing'))

    def test_a_missing_files_folder_is_refused(self):
        self.refuses(r"the files folder .* isn't a folder",
                     files=os.path.join(self.out, 'missing'))

    def test_a_layout_past_4_gib_is_refused(self):
        self.sparse('big.cfg', 3 << 30)
        self.refuses(r'the layout would end past 4 GiB, at big\.cfg')

    def test_a_layout_that_ends_at_4_gib_is_allowed_and_one_byte_more_is_not(self):
        room = (1 << 32) - (START + SLOT)  # the FST takes the first slot
        path = self.sparse('big.cfg', room + 1)
        self.refuses(r'the layout would end past 4 GiB, at big\.cfg')
        os.truncate(path, room)
        self.main()
        self.assertEqual(read_manifest(self.out)[3][-1], (START + SLOT, room, 'mod files/big.cfg'))

    def test_a_file_where_the_disc_has_a_directory_is_refused(self):
        self.put('audio', b'x')
        self.refuses(r'audio is a directory on the disc, not a file')

    def test_a_directory_where_the_disc_has_a_file_is_refused(self):
        self.put('zeta.BIN/inner.cfg')
        self.refuses(r'zeta\.BIN is a file on the disc, not a directory')

    def test_an_image_with_a_cut_fst_is_refused(self):
        image = self.write('cut.iso', self.disc.iso[:FST_AT + 30])
        self.refuses(r'entry 2 is cut off', image=image)

    def test_an_image_with_no_disc_id_is_refused(self):
        data = bytearray(self.disc.iso)
        data[0:6] = b'TE T00'
        self.refuses(r"the image's disc ID isn't printable ASCII",
                     image=self.write('odd.iso', bytes(data)))

    def test_nothing_is_written_when_a_file_is_refused_after_others_pass(self):
        self.put('a.cfg')
        self.put('b café.cfg')
        self.refuses(r"b caf.* has a name that isn't ASCII")


class TestCaseTwins(OverlayCase):
    """A disc directory that holds two names differing only by case."""

    def test_an_exact_spelling_replaces_that_twin_and_leaves_the_other(self):
        image = self.twin_image()
        upper = self.disc.offsets['Zeta.bin']
        for host, moved, kept, kept_at, kept_size in (
                ('Zeta.bin', 'Zeta.bin', 'zeta.bin', 0x6000, 4),
                ('zeta.bin', 'zeta.bin', 'Zeta.bin', upper, SIZES['Zeta.bin'])):
            shutil.rmtree(self.mod)
            self.put(host, b'n' * 9)
            summary = self.main(image=image)
            self.assertEqual((summary['added_files'], summary['replaced_files']), (0, 1))
            found = listing(self.tree())
            self.assertEqual(len(found), len(FILES) + 1)
            self.assertEqual(found[moved], (START + SLOT, 9))
            self.assertEqual(found[kept], (kept_at, kept_size))

    def test_a_name_that_matches_the_twins_only_by_case_is_refused_naming_both(self):
        self.put('ZETA.BIN')
        self.refuses(r"ZETA\.BIN matches 'Zeta\.bin', 'zeta\.bin' on the disc by case alone",
                     image=self.twin_image())


class TestMerge(unittest.TestCase):
    def twins(self):
        root = fst.parse(Disc().fst)
        root.children.append(fst.Entry('zeta.bin', 0x6000, 4))
        root.children.append(fst.Entry('data', children=[fst.Entry('x.cfg', 0x6100, 1)]))
        return root

    def test_each_exact_spelling_reaches_its_own_twin(self):
        root = self.twins()
        items = [ov.Item('a', 9, ('Zeta.bin',)), ov.Item('b', 8, ('zeta.bin',)),
                 ov.Item('c', 7, ('data', 'x.cfg')), ov.Item('d', 6, ('Data', 'new.cfg'))]
        ov.merge(root, items)
        self.assertEqual([i.disc for i in items], [
            ('Zeta.bin',), ('zeta.bin',), ('data', 'x.cfg'), ('Data', 'new.cfg')])
        self.assertEqual([i.replaced for i in items], [True, True, True, False])
        sizes = {c.name: c.size for c in root.children if c.children is None}
        self.assertEqual((sizes['Zeta.bin'], sizes['zeta.bin']), (9, 8))

    def test_a_directory_that_matches_twins_only_by_case_is_refused(self):
        with self.assertRaisesRegex(ov.OverlayError,
                                    r"DATA matches 'Data', 'data' on the disc by case alone"):
            ov.merge(self.twins(), [ov.Item('a', 1, ('DATA', 'x.cfg'))])

    def test_two_spellings_of_a_directory_are_refused(self):
        items = [ov.Item('a', 1, ('Data', 'x.cfg')), ov.Item('b', 1, ('DATA', 'y.cfg'))]
        with self.assertRaisesRegex(ov.OverlayError,
                                    r"'Data' and 'DATA' in the files folder are one name"):
            ov.merge(fst.parse(Disc().fst), items)

    def test_two_spellings_of_a_file_are_refused(self):
        items = [ov.Item('a', 1, ('Data', 'A.cfg')), ov.Item('b', 1, ('Data', 'a.cfg'))]
        with self.assertRaisesRegex(ov.OverlayError, r"'A.cfg' and 'a.cfg' in Data are one name"):
            ov.merge(fst.parse(Disc().fst), items)

    def test_two_spellings_of_a_disc_file_are_refused(self):
        items = [ov.Item('a', 1, ('zeta.bin',)), ov.Item('b', 1, ('ZETA.BIN',))]
        with self.assertRaisesRegex(ov.OverlayError, r"'zeta.bin' and 'ZETA.BIN'"):
            ov.merge(fst.parse(Disc().fst), items)

    def test_a_match_keeps_the_disc_spelling_and_a_new_name_keeps_the_folders(self):
        root = fst.parse(Disc().fst)
        items = [ov.Item('a', 70, ('DATA', 'MAPS', 'GROUND.TEX')),
                 ov.Item('b', 4, ('DATA', 'MAPS', 'New.Tex')),
                 ov.Item('c', 4, ('Brand', 'New'))]
        ov.merge(root, items)
        self.assertEqual([i.disc for i in items], [
            ('Data', 'maps', 'ground.tex'), ('Data', 'maps', 'New.Tex'), ('Brand', 'New')])
        self.assertEqual([i.replaced for i in items], [True, False, False])


class TestLayout(OverlayCase):
    def test_each_file_starts_on_its_own_32_kib_boundary_in_path_order(self):
        for name, size in (('d_empty.cfg', 0), ('c_one.cfg', 1), ('b_over.cfg', 0x8001),
                           ('a_full.cfg', 0x8000), ('e_last.cfg', 5)):
            self.put(name, b'q' * size)
        self.main()
        found = listing(self.tree())
        self.assertEqual({k: v for k, v in found.items() if k.endswith('.cfg')}, {
            'a_full.cfg': (START + SLOT, 0x8000),
            'b_over.cfg': (START + 2 * SLOT, 0x8001),
            'c_one.cfg': (START + 4 * SLOT, 1),
            'd_empty.cfg': (START + 5 * SLOT, 0),
            'e_last.cfg': (START + 5 * SLOT, 5)})
        ranges = read_manifest(self.out)[3]
        self.assertEqual([(a, n) for a, n, _ in ranges], [
            (START, len(self.table())), (START + SLOT, 0x8000), (START + 2 * SLOT, 0x8001),
            (START + 4 * SLOT, 1), (START + 5 * SLOT, 5)])
        for at, _, _ in ranges:
            self.assertEqual(at % SLOT, 0)

    def test_a_big_fst_takes_as_many_slots_as_it_needs(self):
        for i in range(2000):
            self.put(f'many/file number {i:04d}.cfg', b'')
        self.main()
        size = len(self.table())
        self.assertGreater(size, SLOT)
        self.assertEqual(ov.align_up(size) // SLOT, 3)
        ranges = read_manifest(self.out)[3]
        self.assertEqual(ranges, [(START, size, 'fst.bin')])
        self.assertTrue(all(e.offset == START + 3 * SLOT
                            for p, e in fst.walk(self.tree()) if p.startswith('many/')))

    def test_ranges_are_sorted_when_the_fst_order_differs_from_the_name_order(self):
        # 'Zeta.bin' sorts first as a host name, and last in the FST.
        self.put('Zeta.bin', b'z')
        self.put('audio/new.cfg', b'n')
        self.main()
        ranges = read_manifest(self.out)[3]
        self.assertEqual(ranges, [(START, len(self.table()), 'fst.bin'),
                                  (START + SLOT, 1, 'mod files/audio/new.cfg'),
                                  (START + 2 * SLOT, 1, 'mod files/Zeta.bin')])

    def test_manifest_lines_come_in_order_with_plain_lowercase_hex(self):
        self.put('Data/new.cfg', b'abc')
        self.main('--dol', self.put('boot.dol', bytes(0x200), folder=self.out))
        kinds, base, headers, ranges = read_manifest(self.out)
        self.assertEqual(kinds, ['gcn-disc-overlay', 'base'] + ['header'] * 4
                         + ['range'] * 3)
        with open(os.path.join(self.out, 'disc-overlay.txt'), 'rb') as f:
            lines = f.read().decode('ascii').splitlines()
        self.assertEqual([int(line.split(' ')[1], 16) for line in lines[2:6]],
                         [0x420, 0x424, 0x428, 0x42C])
        for line in lines[2:]:
            for field in line.split(' ')[1:3]:
                self.assertRegex(field, r'^[0-9a-zA-Z]+$')
                self.assertNotRegex(field, r'^0x|[A-F]')
        self.assertEqual(len(base[3]), 16)

    def test_the_layout_starts_at_the_image_size_when_that_is_past_the_disc_size(self):
        for size, start in ((START - SLOT, START), (START, START), (START + 1, START + SLOT),
                            (START + 0x1234, START + SLOT), (START + 2 * SLOT, START + 2 * SLOT)):
            image = os.path.join(self.tmp, f'big {size:x}.iso')
            with open(image, 'wb') as f:
                f.write(self.disc.iso)
                f.truncate(size)
            if os.stat(image).st_blocks * 512 > 1 << 24:
                self.skipTest('this file system has no sparse files')
            self.put('a.cfg')
            self.main(image=image)
            self.assertEqual(read_manifest(self.out)[2][0x424], start, hex(size))
            os.unlink(image)

    def test_the_layout_starts_past_a_ciso_s_last_stored_block(self):
        block = 0x10000
        image = self.write('far.ciso', ciso_far(block, [0, 0x5706]))
        self.put('a.cfg')
        self.main(image=image)
        self.assertEqual(read_manifest(self.out)[2][0x424], 0x5707 * block)

    def test_a_table_smaller_than_the_headers_size_is_padded_and_headers_stay(self):
        data = bytearray(self.disc.iso)
        struct.pack_into('>2I', data, 0x428, FST_LEN + 0x30, OLD_MAX)
        data[FST_AT + FST_LEN:FST_AT + FST_LEN + 0x30] = b'\xaa' * 0x30
        image = self.write('padded.iso', bytes(data))
        self.put('Data/new.cfg', b'abc')
        summary = self.main(image=image)
        _, base, headers, ranges = read_manifest(self.out)
        self.assertEqual(headers, {0x424: START})
        self.assertEqual(summary['fst_growth'], 0)
        built = fst.build(self.tree())
        self.assertEqual(len(built), FST_LEN + 20)
        self.assertEqual(self.table(), built.ljust(FST_LEN + 0x30, b'\0'))
        self.assertEqual(ranges[0], (START, FST_LEN + 0x30, 'fst.bin'))
        self.assertEqual(base[2], f'{FST_LEN + 0x30:x}')
        padded = bytes(data[FST_AT:FST_AT + FST_LEN + 0x30])
        self.assertEqual(base[3], f'{ov.fnv1a64(padded):016x}')


class TestOutputs(OverlayCase):
    def test_two_runs_give_the_same_bytes(self):
        self.put('Zeta.bin', b'z')
        self.put('Data/new one.cfg', b'n')
        self.put('fresh/x.cfg', b'f')
        first = self.main()
        files = [self.table(), read_manifest(self.out)]
        with open(os.path.join(self.out, 'disc-overlay.txt'), 'rb') as f:
            text = f.read()
        self.assertEqual(self.main(), first)
        with open(os.path.join(self.out, 'disc-overlay.txt'), 'rb') as f:
            self.assertEqual(f.read(), text)
        self.assertEqual([self.table(), read_manifest(self.out)], files)

    def test_out_can_move(self):
        self.put('Data/new one.cfg', b'nn')
        self.put('Zeta.bin', b'z')
        self.main()
        moved = os.path.join(self.tmp, 'moved here', 'out')
        shutil.copytree(self.out, moved)
        for name in ('fst.bin', 'disc-overlay.txt'):
            with open(os.path.join(self.out, name), 'rb') as f, \
                    open(os.path.join(moved, name), 'rb') as g:
                self.assertEqual(f.read(), g.read())
        shutil.rmtree(self.out)
        for at, size, rel in read_manifest(moved)[3]:
            self.assertFalse(os.path.isabs(rel))
            self.assertEqual(os.path.getsize(os.path.join(moved, *rel.split('/'))), size)

    def test_the_script_prints_one_json_line(self):
        self.put('Data/new.cfg', b'abc')
        done = subprocess.run([sys.executable, '-B', OVERLAY_PY, self.image, self.out,
                               '--files', self.mod], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(len(done.stdout.splitlines()), 1)
        self.assertEqual(json.loads(done.stdout)['added_files'], 1)

    def test_the_script_exits_with_an_error_not_a_traceback(self):
        self.put('Data/café.cfg')
        done = subprocess.run([sys.executable, '-B', OVERLAY_PY, self.image, self.out,
                               '--files', self.mod], capture_output=True, text=True)
        self.assertEqual(done.returncode, 1)
        self.assertRegex(done.stderr, r"^error: .*isn't ASCII\n$")
        self.assertEqual(done.stdout, '')


class TestHelpers(unittest.TestCase):
    def test_fnv1a64_matches_the_published_vectors(self):
        self.assertEqual(ov.fnv1a64(b''), 0xCBF29CE484222325)
        self.assertEqual(ov.fnv1a64(b'a'), 0xAF63DC4C8601EC8C)
        self.assertEqual(ov.fnv1a64(b'foobar'), 0x85944171F73967E8)

    def test_align_up(self):
        self.assertEqual([ov.align_up(n) for n in (0, 1, 0x7FFF, 0x8000, 0x8001)],
                         [0, 0x8000, 0x8000, 0x8000, 0x10000])

    def test_arena_hi_drops_in_steps_of_32(self):
        self.assertEqual(ov.arena_hi(0x1238), 0x817FCDC0)
        self.assertEqual(ov.arena_hi(0x1240), 0x817FCDC0)
        self.assertEqual(ov.arena_hi(0x1241), 0x817FCDA0)

    def test_place_allows_a_layout_that_ends_at_4_gib(self):
        top = 1 << 32
        self.assertEqual(ov.place(top - SLOT, [('a', SLOT)]), [top - SLOT])

    def test_place_refuses_a_layout_one_byte_past_4_gib(self):
        with self.assertRaisesRegex(ov.OverlayError, r'past 4 GiB, at a'):
            ov.place((1 << 32) - SLOT, [('a', SLOT + 1)])

    def test_place_refuses_an_empty_file_with_no_offset_to_give(self):
        with self.assertRaisesRegex(ov.OverlayError, r'past 4 GiB, at z'):
            ov.place((1 << 32) - SLOT, [('a', SLOT), ('z', 0)])


class TestImageSize(OverlayCase):
    def size_of(self, path):
        img = DiscImage(path)
        self.addCleanup(img.f.close)
        return ov.image_size(img)

    def test_a_plain_image_is_its_length(self):
        self.assertEqual(self.size_of(self.image), len(self.disc.iso))

    def test_a_ciso_ends_with_its_last_stored_block(self):
        far = self.write('far.ciso', ciso_far(0x10000, [0, 0x40]))
        self.assertEqual(self.size_of(far), 0x41 * 0x10000)

    def test_a_ciso_with_no_stored_block_is_empty(self):
        self.assertEqual(self.size_of(self.write('none.ciso', ciso_far(0x10000, [])[:0x8000])), 0)


@unittest.skipUnless(os.environ.get('GCN_TEST_IMAGE'), 'GCN_TEST_IMAGE names no image')
class TestDisc(OverlayCase):
    """The overlay on the image GCN_TEST_IMAGE names. They print counts and pass or fail, and
    never a name, an offset or a size from that image. The replaced file's bytes are filler,
    since the tool only reads sizes."""

    NEW = {'overlay check new.cfg': 5, 'overlay check dir/inner/deep.cfg': 7,
           'overlay check dir/second.cfg': 3}

    def setUp(self):
        super().setUp()
        self.image = os.environ['GCN_TEST_IMAGE']
        img = DiscImage(self.image)
        self.addCleanup(img.f.close)
        refused = False
        try:
            self.old_at, self.old_size, self.old_max, tree = fst.read(img)
        except fst.FstError:
            refused = True  # the message names a disc entry, so it's never printed
        self.assertFalse(refused, 'fst.read refused the image')
        self.base = listing(tree)
        self.assertTrue(not any(p.lower().startswith('overlay check') for p in self.base),
                        'the image already has a name this check adds')
        sized = sorted((size, path) for path, (_, size) in self.base.items() if size)
        self.assertTrue(sized, 'the image has no file with bytes')
        self.picked = sized[0][1]
        self.put(self.picked, b'q' * self.base[self.picked][1])

    def build(self):
        try:
            return self.main()
        except SystemExit:
            self.fail('the tool refused the files')

    def test_one_replaced_file_and_three_added(self):
        for rel, size in self.NEW.items():
            self.put(rel, b'n' * size)
        summary = self.build()
        table = self.table()
        found = listing(fst.parse(table))
        headers = read_manifest(self.out)[2]
        new_max = max(self.old_max, len(table))
        self.assertTrue(len(found) == len(self.base) + 3, 'the new FST has the wrong file count')
        self.assertTrue(all(p in found for p in self.base), 'the new FST lost a base file')
        self.assertTrue(all(found[p][1] == s for p, (_, s) in self.base.items()),
                        'a base file changed size')
        self.assertTrue([p for p in self.base if found[p][0] != self.base[p][0]] == [self.picked],
                        'something other than the replaced file moved')
        self.assertTrue(found[self.picked][0] >= START and found[self.picked][0] % SLOT == 0,
                        'the replaced file is not in the layout')
        self.assertTrue(sorted(found[p][1] for p in found if p not in self.base) == [3, 5, 7],
                        'an added file has the wrong size')
        self.assertTrue((summary['added_files'], summary['replaced_files']) == (3, 1),
                        'the counts are wrong')
        self.assertTrue(headers[0x424] >= START and headers[0x424] % SLOT == 0,
                        'the FST is not in the layout')
        self.assertTrue(len(table) > self.old_size, 'the FST did not grow')
        self.assertTrue((headers[0x428], headers[0x42C]) == (len(table), new_max),
                        'the size words are wrong')
        self.assertTrue(summary['fst_growth'] == len(table) - self.old_size, 'the growth is wrong')
        self.assertTrue(summary['arena_hi_drop'] == arena(self.old_max) - arena(new_max),
                        'the arena hi drop is wrong')

    def test_a_replacement_alone_keeps_the_size_words(self):
        summary = self.build()
        table = self.table()
        headers = read_manifest(self.out)[2]
        self.assertTrue(list(headers) == [0x424], 'a word other than the FST offset changed')
        self.assertTrue(len(table) == self.old_size, 'the FST changed size')
        self.assertTrue(summary['fst_growth'] == 0 and summary['arena_hi_drop'] == 0,
                        'the summary shows growth')
        found = listing(fst.parse(table))
        self.assertTrue(sorted(found) == sorted(self.base), 'the new FST lists other files')


if __name__ == '__main__':
    unittest.main()
