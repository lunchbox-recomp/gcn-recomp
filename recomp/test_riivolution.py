#!/usr/bin/env python3
"""Checks for riivolution.py and the modules it reads and patches through. Run them with
`python3 -m unittest discover -s recomp -p 'test_*.py'`.

The images, SD roots and packages are written here with made-up IDs, names and numbers, so no
disc and no release is needed. The image is the fixture test_fst.py writes.
"""
import contextlib
import io
import json
import os
import shutil
import struct
import re
import subprocess
import sys
import tempfile
import time
import types
import unittest
import xml.etree.ElementTree as ET
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import disc_overlay as ov  # noqa: E402
import fst  # noqa: E402
import riivolution as rv  # noqa: E402
import riivolution_paths as rpaths  # noqa: E402
import riivolution_patch as rpatch  # noqa: E402
import riivolution_xml as rxml  # noqa: E402
from test_disc_overlay import SLOT, START, read_manifest  # noqa: E402
from test_fst import SIZES, Disc, ciso  # noqa: E402

RIIVOLUTION_PY = os.path.join(HERE, 'riivolution.py')
FST_LEN = len(Disc().fst)
OUTPUTS = ['disc-overlay.txt', 'fst.bin', 'ram-patches.txt']

PACKAGE = '''<wiidisc version="1"{root}>
  {ident}
  <options>
    <section name="Mod">
      <option name="Opt" id="opt" default="{default}">
        <choice name="On"><patch id="p"/></choice>
      </option>
    </section>
  </options>
  <patch id="p">{body}</patch>
</wiidisc>
'''


class SdCase(unittest.TestCase):
    """An SD root with a riivolution folder, an OUT folder inside it and the fixture image."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='riivolution test ')
        self.addCleanup(tmp.cleanup)
        self.tmp = os.path.realpath(tmp.name)
        self.root = os.path.join(self.tmp, 'sd root')
        self.out = os.path.join(self.root, '.out')
        os.makedirs(os.path.join(self.root, 'riivolution'))
        self.disc = Disc()
        self.image = self.write(self.tmp, 'game.iso', self.disc.iso)

    def write(self, folder, rel, data):
        path = os.path.join(folder, *rel.split('/'))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)
        return path

    def put(self, rel, data=b'x'):
        return self.write(self.root, rel, data)

    def link(self, target, path):
        try:
            os.symlink(target, path)
        except (OSError, NotImplementedError):
            self.skipTest('this system has no symbolic links to make')

    def xml(self, rel, text):
        return self.write(self.root, 'riivolution/' + rel, text.encode('utf-8'))

    def package(self, rel, body, ident='<id game="TEST"/>', default=1, root=''):
        """A package with one option, "opt", whose one choice turns the patch p on."""
        text = PACKAGE.format(body=body, ident=ident, default=default,
                              root=f' root="{root}"' if root else '')
        return self.xml(rel, text)

    def image_with(self, game=b'TEST00', disc=0, revision=0, tree=None):
        data = bytearray(self.disc.iso)
        data[0:6] = game
        data[6] = disc
        data[7] = revision
        if tree is not None:
            table = fst.build(tree)
            struct.pack_into('>2I', data, 0x428, len(table), len(table) + 0x40)
            data[0x2000:0x2000 + len(table)] = table
        return self.write(self.tmp, 'other.iso', bytes(data))

    def run_main(self, argv):
        """The JSON line main prints. What it printed stays in self.printed, exit or not."""
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                rv.main(argv)
        finally:
            self.printed = buf.getvalue()
        return json.loads(self.printed)

    def apply(self, *extra, image=None, out=None, root=None):
        return self.run_main(['apply', image or self.image, root or self.root, out or self.out]
                             + list(extra))

    def listing(self):
        return self.run_main(['list', self.root])['packages']

    def refused(self, pattern, *extra, **kw):
        """apply exits 1 with `error: ` and a message that matches, and writes nothing.

        It returns what apply printed on stdout, which refusal() reads as JSON.
        """
        with self.assertRaises(SystemExit) as cm:
            self.apply(*extra, **kw)
        self.assertRegex(str(cm.exception.code), r'^error: ')
        self.assertRegex(str(cm.exception.code), pattern)
        if 'out' not in kw:
            self.assertFalse(os.path.exists(self.out))
        return self.printed

    def refusal(self, pattern, *extra, **kw):
        """Like refused, and gives back the one JSON line apply printed before it exited."""
        shown = self.refused(pattern, *extra, **kw)
        self.assertEqual(len(shown.splitlines()), 1, shown)
        return json.loads(shown)

    def bad_arguments(self, pattern, *extra, **kw):
        err = io.StringIO()
        with self.assertRaises(SystemExit) as cm, contextlib.redirect_stderr(err):
            self.apply(*extra, **kw)
        self.assertEqual(cm.exception.code, 2)
        self.assertRegex(err.getvalue(), pattern)

    def outputs(self):
        return sorted(os.listdir(self.out)) if os.path.isdir(self.out) else []

    def ram(self):
        with open(os.path.join(self.out, 'ram-patches.txt'), 'rb') as f:
            return f.read().decode('ascii')

    def table(self):
        with open(os.path.join(self.out, 'fst.bin'), 'rb') as f:
            return fst.parse(f.read())

    def placed(self):
        """Each file the overlay moved, as disc path to (host file, size)."""
        ranges = {at: (rel, size) for at, size, rel in read_manifest(self.out)[3]}
        return {path: (ranges[e.offset][0], e.size) for path, e in fst.walk(self.table())
                if e.offset in ranges and e.size}

    def host(self, rel):
        """The file a manifest path names, which is relative to OUT."""
        return os.path.normpath(os.path.join(self.out, *rel.split('/')))

    def stale(self):
        """Old outputs, so a run shows what it removes and what it leaves."""
        for name in OUTPUTS:
            self.write(self.out, name, b'old')


class TestList(SdCase):
    def test_a_package_lists_its_options_with_choices_and_defaults(self):
        self.xml('two.xml', '''<wiidisc version="1"><id game="TEST"/><options>
          <section name="First"><option name="Music" id="music" default="2">
            <choice name="Off"/><choice name="Loud"/></option></section>
          <section name="Second"><option name="Speed" id="speed">
            <choice name="Fast"/></option></section></options></wiidisc>''')
        self.assertEqual(self.listing(), [{'xml': 'two.xml', 'options': [
            {'id': 'music', 'name': 'Music', 'section': 'First', 'default': 2,
             'choices': ['Off', 'Loud']},
            {'id': 'speed', 'name': 'Speed', 'section': 'Second', 'default': 0,
             'choices': ['Fast']}]}])

    def test_an_option_with_no_id_is_named_by_its_section_and_its_name(self):
        self.xml('a.xml', '''<wiidisc version="1"><options><section name="Sec ">
          <option name="Opt"><choice name="x"/></option></section></options></wiidisc>''')
        self.assertEqual(self.listing()[0]['options'][0]['id'], 'Sec Opt')

    def test_a_package_with_no_options_lists_none(self):
        self.xml('a.xml', '<wiidisc version="1"><patch id="p"/></wiidisc>')
        self.assertEqual(self.listing(), [{'xml': 'a.xml', 'options': []}])

    def test_packages_come_in_name_order(self):
        for rel in ('z.xml', 'b b.xml', 'a.XML', 'B.xml'):
            self.package(rel, '')
        self.assertEqual([p['xml'] for p in self.listing()],
                         ['B.xml', 'a.XML', 'b b.xml', 'z.xml'])

    def test_files_that_are_not_packages_are_left_out(self):
        self.package('real.xml', '')
        self.xml('TEST.xml', '<riivolution version="2"><option id="x" default="1"/>'
                 '</riivolution>')
        self.xml('.hidden.xml', '<wiidisc version="1"/>')
        self.xml('._real.xml', 'not xml at all')
        self.xml('notes.txt', '<wiidisc version="1"/>')
        self.assertEqual([p['xml'] for p in self.listing()], ['real.xml'])

    def test_only_the_top_of_the_riivolution_folder_is_read(self):
        self.package('top.xml', '')
        self.package('config/TEST.xml', '')
        self.package('mods/inner/b.xml', '')
        self.xml('.git/inner.xml', '<wiidisc version="1"/>')
        self.assertEqual([p['xml'] for p in self.listing()], ['top.xml'])
        self.assertEqual(self.apply()['packages'], ['top.xml'])

    def test_a_folder_that_cannot_be_read_does_not_stop_the_listing(self):
        if hasattr(os, 'geteuid') and os.geteuid() == 0:
            self.skipTest('root can read any folder')
        self.package('good.xml', '')
        locked = os.path.join(self.root, 'riivolution', 'locked')
        os.mkdir(locked)
        os.chmod(locked, 0)
        self.addCleanup(os.chmod, locked, 0o755)
        self.assertEqual([p['xml'] for p in self.listing()], ['good.xml'])

    def test_a_symbolic_link_for_the_riivolution_folder_is_refused(self):
        elsewhere = os.path.join(self.tmp, 'elsewhere')
        self.write(elsewhere, 'a.xml', b'<wiidisc version="1"/>')
        shutil.rmtree(os.path.join(self.root, 'riivolution'))
        self.link(elsewhere, os.path.join(self.root, 'riivolution'))
        with self.assertRaises(SystemExit) as cm:
            self.run_main(['list', self.root])
        self.assertEqual(str(cm.exception.code),
                         "error: The folder 'riivolution' is a symbolic link, so it isn't read")
        self.assertEqual(json.loads(self.printed), {
            'error': "The folder 'riivolution' is a symbolic link, so it isn't read",
            'packages': []})
        self.refused(r"The folder 'riivolution' is a symbolic link")

    def test_no_riivolution_folder_lists_nothing(self):
        os.rmdir(os.path.join(self.root, 'riivolution'))
        self.assertEqual(self.listing(), [])

    def test_the_folder_is_found_whatever_its_case(self):
        shutil.rmtree(os.path.join(self.root, 'riivolution'))
        self.write(self.root, 'Riivolution/a.xml', b'<wiidisc version="1"/>')
        self.assertEqual([p['xml'] for p in self.listing()], ['a.xml'])

    def test_a_root_that_is_not_a_folder_is_bad_arguments(self):
        err = io.StringIO()
        with self.assertRaises(SystemExit) as cm, contextlib.redirect_stderr(err):
            self.run_main(['list', os.path.join(self.tmp, 'none')])
        self.assertEqual(cm.exception.code, 2)
        self.assertIn("isn't a folder", err.getvalue())

    def test_a_package_with_an_error_lists_the_error_and_the_rest_still_list(self):
        self.package('good.xml', '')
        self.xml('bad.xml', '<!DOCTYPE wiidisc []><wiidisc version="1"/>')
        found = self.listing()
        self.assertEqual([p['xml'] for p in found], ['bad.xml', 'good.xml'])
        self.assertEqual(found[0], {'xml': 'bad.xml',
                                    'error': 'bad.xml has a DOCTYPE or an entity, '
                                             'and neither is read'})
        self.assertIn('options', found[1])

    def test_unsupported_structure_is_an_error_naming_the_element(self):
        cases = {
            'macros': '<options><macros id="a" name="b"/></options>',
            'option param': '<options><section name="s"><option name="o">'
                            '<param name="p" value="v"/></option></section></options>',
            'choice param': '<options><section name="s"><option name="o"><choice name="c">'
                            '<param name="p" value="v"/></choice></option></section></options>',
            'ref param': '<options><section name="s"><option name="o"><choice name="c">'
                         '<patch id="x"><param name="p" value="v"/></patch></choice></option>'
                         '</section></options>',
        }
        for name, body in cases.items():
            with self.subTest(name):
                self.xml('a.xml', f'<wiidisc version="1">{body}</wiidisc>')
                error = self.listing()[0]['error']
                self.assertRegex(error, r"^<(macros|param)[^>]*> in a\.xml isn't supported")

    def test_a_repeated_option_id_is_an_error(self):
        self.xml('a.xml', '''<wiidisc version="1"><options><section name="s">
          <option name="o1" id="same"/><option name="o2" id="same"/></section></options>
          </wiidisc>''')
        self.assertRegex(self.listing()[0]['error'],
                         r"^<option .*> in a\.xml repeats the option id 'same'")

    def test_a_version_other_than_1_is_an_error(self):
        self.xml('a.xml', '<wiidisc version="2"/>')
        self.xml('b.xml', '<wiidisc/>')
        self.assertRegex(self.listing()[0]['error'], r"^<wiidisc> in a\.xml isn't version 1")
        self.assertRegex(self.listing()[1]['error'], r"^<wiidisc> in b\.xml isn't version 1")

    def test_an_xml_name_with_a_control_character_is_an_error_entry_never_a_package(self):
        # A name like this would add a line to anything that keeps its choices one to a line.
        names = ['a\nother.xml o = 1\nz.xml', 'del\x7f.xml', 'tab\t.xml', 'one\x01.xml']
        for name in names:
            self.package(name, '<memory offset="0x80001000" value="aa"/>')
        self.package('good.xml', '<memory offset="0x80002000" value="bb"/>')
        found = {p['xml']: p for p in self.listing()}
        self.assertEqual(sorted(found), sorted(names + ['good.xml']))
        for name in names:
            self.assertEqual(found[name], {'xml': name, 'error': (
                repr(name)[1:-1] + " has a control character in its name, so it isn't read")})
            self.assertNotIn('\n', found[name]['error'])
        self.assertIn('options', found['good.xml'])
        self.assertEqual(self.apply()['packages'], ['good.xml'])
        self.assertEqual(self.ram().splitlines()[2:], ['patch 80002000 bb'])

    def test_a_symbolic_link_to_an_xml_is_an_error_and_one_to_a_folder_is_not_entered(self):
        real = self.package('real.xml', '')
        elsewhere = self.write(self.tmp, 'elsewhere/b.xml', b'<wiidisc version="1"/>')
        self.link(real, os.path.join(self.root, 'riivolution', 'link.xml'))
        self.link(os.path.dirname(elsewhere), os.path.join(self.root, 'riivolution', 'linked'))
        found = {p['xml']: p for p in self.listing()}
        self.assertEqual(sorted(found), ['link.xml', 'real.xml'])
        self.assertIn('symbolic link', found['link.xml']['error'])
        self.assertIn('options', found['real.xml'])

    def test_a_malformed_or_odd_file_is_an_error(self):
        self.xml('open.xml', '<wiidisc version="1">')
        self.write(self.root, 'riivolution/latin.xml', b'<wiidisc version="1" a="\xe9"/>')
        found = {p['xml']: p['error'] for p in self.listing()}
        self.assertRegex(found['open.xml'], r"^open\.xml isn't well-formed XML")
        self.assertEqual(found['latin.xml'], "latin.xml isn't UTF-8")


class TestXmlSafety(SdCase):
    def read(self, data):
        path = self.write(self.tmp, 'one.xml', data)
        return rxml.read_package(path, 'one.xml')

    def test_a_file_of_exactly_1_mib_is_read(self):
        head = b'<wiidisc version="1"/>'
        package = self.read(head + b' ' * ((1 << 20) - len(head)))
        self.assertIsNotNone(package)

    def test_a_file_one_byte_over_1_mib_is_refused(self):
        head = b'<wiidisc version="1"/>'
        with self.assertRaisesRegex(rxml.Refused, r"^one\.xml is over 1 MiB"):
            self.read(head + b' ' * ((1 << 20) - len(head) + 1))

    def test_a_doctype_is_refused_before_the_file_is_parsed(self):
        # The file is also malformed, so a parse would have said so instead.
        with self.assertRaisesRegex(rxml.Refused, r'^one\.xml has a DOCTYPE or an entity'):
            self.read(b'<!DOCTYPE wiidisc [<!ENTITY a "b">]><wiidisc version="1">&a;<')

    def test_an_entity_is_refused_however_it_is_spelled(self):
        for text in (b'<!doctype x><wiidisc version="1"/>',
                     b'<!ENTITY a "b"><wiidisc version="1"/>',
                     b'<!  DOCTYPE x><wiidisc version="1"/>'):
            with self.subTest(text):
                with self.assertRaisesRegex(rxml.Refused, 'DOCTYPE or an entity'):
                    self.read(text)

    def test_an_entity_bomb_is_refused_without_being_expanded(self):
        lol = '<!ENTITY l0 "lol">' + ''.join(
            f'<!ENTITY l{i} "' + f'&l{i - 1};' * 10 + '">' for i in range(1, 9))
        with self.assertRaisesRegex(rxml.Refused, 'DOCTYPE or an entity'):
            self.read(f'<!DOCTYPE wiidisc [{lol}]><wiidisc version="1">&l8;</wiidisc>'.encode())

    def test_utf_16_is_refused_since_the_scan_before_parsing_could_not_see_into_it(self):
        text = '<!DOCTYPE x><wiidisc version="1"/>'
        with self.assertRaisesRegex(rxml.Refused, "isn't UTF-8"):
            self.read(text.encode('utf-16'))

    def test_utf_16_and_utf_32_with_no_byte_order_mark_are_refused_whatever_they_hold(self):
        # Their bytes are valid UTF-8 with NULs in, so the scan for a DOCTYPE can't see inside.
        text = ('<!DOCTYPE wiidisc [<!ENTITY a "EXPANDED">]><wiidisc version="1"><options>'
                '<section name="s"><option name="&a;&a;"/></section></options></wiidisc>')
        for codec in ('utf-16-le', 'utf-16-be', 'utf-32-le', 'utf-32-be'):
            with self.subTest(codec):
                with self.assertRaisesRegex(
                        rxml.Refused, r'^one\.xml has a NUL or another control character'):
                    self.read(text.encode(codec))

    def test_a_listing_never_shows_what_an_entity_in_such_a_file_would_expand_to(self):
        text = ('<!DOCTYPE wiidisc [<!ENTITY a "EXPANDED">]><wiidisc version="1"><options>'
                '<section name="s"><option name="&a;&a;"/></section></options></wiidisc>')
        self.write(self.root, 'riivolution/le.xml', text.encode('utf-16-le'))
        self.write(self.root, 'riivolution/be.xml', text.encode('utf-16-be'))
        found = {p['xml']: p for p in self.listing()}
        self.assertEqual(sorted(found), ['be.xml', 'le.xml'])
        for name in found:
            self.assertRegex(found[name]['error'], 'control character')
            self.assertNotIn('EXPANDED', json.dumps(found[name]))

    def test_an_entity_bomb_in_utf_16_with_no_byte_order_mark_is_refused_fast(self):
        # Six levels expand to a few MB, which is enough to show a parse that wasn't stopped.
        lol = '<!ENTITY l0 "lol">' + ''.join(
            f'<!ENTITY l{i} "' + f'&l{i - 1};' * 10 + '">' for i in range(1, 7))
        text = f'<!DOCTYPE wiidisc [{lol}]><wiidisc version="1">&l6;</wiidisc>'
        for codec in ('utf-16-le', 'utf-16-be'):
            with self.subTest(codec):
                started = time.monotonic()
                with self.assertRaisesRegex(rxml.Refused, 'control character'):
                    self.read(text.encode(codec))
                self.assertLess(time.monotonic() - started, 1.0)

    def test_a_nul_or_a_control_character_is_refused_but_tab_cr_and_lf_are_not(self):
        for char in ('\x00', '\x01', '\x08', '\x0b', '\x0c', '\x0e', '\x1f'):
            with self.subTest(repr(char)):
                with self.assertRaisesRegex(rxml.Refused, 'control character'):
                    self.read(f'<wiidisc version="1"/>{char}'.encode())
                with self.assertRaisesRegex(rxml.Refused, 'control character'):
                    self.read(f'<wiidisc version="1" a="{char}"/>'.encode())
        self.assertIsNotNone(self.read(b'<wiidisc\tversion="1"\r\n>\r\n\t</wiidisc>\n'))

    def test_a_byte_order_mark_is_allowed(self):
        self.assertIsNotNone(self.read(b'\xef\xbb\xbf<wiidisc version="1"/>'))

    def test_a_declared_encoding_is_not_trusted_over_utf_8(self):
        package = self.read(b'<?xml version="1.0" encoding="ISO-8859-1"?><wiidisc version="1"/>')
        self.assertIsNotNone(package)

    def test_a_deeply_nested_file_parses_or_is_refused_but_never_crashes(self):
        deep = b'<wiidisc version="1">' + b'<a>' * 150000 + b'</a>' * 150000 + b'</wiidisc>'
        try:
            self.read(deep)
        except rxml.Refused:
            pass

    def test_apply_with_a_choice_for_such_a_package_exits_1_naming_it(self):
        self.xml('bad.xml', '<!DOCTYPE wiidisc []><wiidisc version="1"/>')
        self.refused(r'bad\.xml has a DOCTYPE', '--choice', 'bad.xml:opt=1')

    def test_choice_zero_for_a_package_with_an_error_is_allowed_and_does_nothing(self):
        self.xml('bad.xml', '<!DOCTYPE wiidisc []><wiidisc version="1"/>')
        self.package('good.xml', '<memory offset="0x80001000" value="aa"/>')
        self.assertEqual(self.apply('--choice', 'bad.xml:opt=0')['packages'], ['good.xml'])
        self.assertEqual(self.apply('--choice', 'bad.xml:no such option=0')['packages'],
                         ['good.xml'])
        self.refused(r'bad\.xml has a DOCTYPE', '--choice', 'bad.xml:opt=1', out=self.out)

    def test_apply_leaves_a_package_with_an_error_off_when_no_choice_names_it(self):
        self.xml('bad.xml', '<!DOCTYPE wiidisc []><wiidisc version="1"/>')
        self.package('good.xml', '<memory offset="0x80001000" value="aa"/>')
        self.assertEqual(self.apply()['packages'], ['good.xml'])


class TestDtdWall(unittest.TestCase):
    """The parser's own refusal of a DOCTYPE, which holds if the scan before it is fooled."""

    def test_a_doctype_that_got_past_the_scan_still_stops_the_parse(self):
        lol = '<!ENTITY l0 "lol">' + ''.join(
            f'<!ENTITY l{i} "' + f'&l{i - 1};' * 10 + '">' for i in range(1, 6))
        for text in ('<!DOCTYPE wiidisc><wiidisc version="1"/>',
                     '<!DOCTYPE wiidisc [<!ENTITY a "b">]><wiidisc version="1">&a;</wiidisc>',
                     '<!DOCTYPE wiidisc SYSTEM "file:///nowhere"><wiidisc version="1"/>',
                     f'<!DOCTYPE wiidisc [{lol}]><wiidisc version="1" a="&l5;"/>'):
            with self.subTest(text[:40]):
                with self.assertRaisesRegex(
                        rxml.Refused, r'^f\.xml has a DOCTYPE or an entity, and neither is read'):
                    rxml.parse_text(text, 'f.xml')

    def test_every_declaration_handler_refuses(self):
        made = []
        real = rxml.expat.ParserCreate

        def create(*args, **kw):
            made.append(real(*args, **kw))
            return made[-1]

        with mock.patch.object(rxml.expat, 'ParserCreate', create):
            rxml.parse_text('<wiidisc version="1"/>', 'f.xml')
        for name in ('StartDoctypeDeclHandler', 'EntityDeclHandler',
                     'UnparsedEntityDeclHandler', 'ExternalEntityRefHandler'):
            with self.subTest(name):
                handler = getattr(made[0], name)
                self.assertIsNotNone(handler)
                with self.assertRaisesRegex(rxml.Refused, '^f.xml has a DOCTYPE or an entity'):
                    handler('x', 'y', 'z')

    def test_a_plain_document_comes_out_as_elements_with_no_namespaces_read(self):
        root = rxml.parse_text('<wiidisc version="1" xmlns:q="u"><q:a id="1">text<b/></q:a>'
                               '<!-- note --><?pi x?></wiidisc>', 'f.xml')
        self.assertEqual((root.tag, root.get('version'), root.get('xmlns:q')),
                         ('wiidisc', '1', 'u'))
        self.assertEqual([c.tag for c in root], ['q:a'])
        self.assertEqual([c.tag for c in root[0]], ['b'])

    def test_a_document_that_is_not_well_formed_is_refused_naming_the_file(self):
        for text in ('<wiidisc version="1">', '<a></b>', '', 'plain text', '<a>&nope;</a>'):
            with self.subTest(text):
                with self.assertRaisesRegex(rxml.Refused, r"^f\.xml isn't well-formed XML"):
                    rxml.parse_text(text, 'f.xml')


class TestAttributes(unittest.TestCase):
    def el(self, text):
        return rxml.Element(ET.fromstring(text), 'f.xml')

    def test_numbers_are_decimal_or_hex_and_the_default_stands_in_for_a_missing_one(self):
        el = self.el('<a x="12" y="0x1F" z=" 7 " w="-3" v="+5"/>')
        self.assertEqual([el.number(n, 99) for n in 'xyz'], [12, 31, 7])
        self.assertEqual(el.number('w', 0, signed=True), -3)
        self.assertEqual(el.number('v', 0), 5)
        self.assertEqual(el.number('missing', 99), 99)

    def test_a_leading_zero_is_decimal_as_pugixml_reads_it(self):
        el = self.el('<a v="01" w="0100" x="010" y="00" z="0x0100" u="0009"/>')
        self.assertEqual([el.number(n, 99) for n in 'vwxyzu'], [1, 100, 10, 0, 256, 9])

    def test_a_number_is_at_most_10_digits_after_any_0x(self):
        el = self.el('<a x="4294967295" y="0x00000000ff" z="0000000009" w="0xFFFFFFFF"/>')
        self.assertEqual([el.number(n, 99) for n in 'xyzw'], [4294967295, 255, 9, 4294967295])
        for bad in ('1' * 11, '0' * 11, '0x' + '0' * 11, '1' * 5000, '0x' + 'f' * 5000,
                    '-' + '1' * 11, '+' + '1' * 5000):
            with self.subTest(len(bad)):
                with self.assertRaisesRegex(rxml.Refused, r'^<a id="q"> in f\.xml has x=') as cm:
                    self.el(f'<a id="q" x="{bad}"/>').number('x', 0, signed=True)
                self.assertLess(len(str(cm.exception)), 200)

    def test_a_conversion_error_is_a_refusal_and_never_a_crash(self):
        # Python 3.11 and later stop int() at 4300 digits whatever the cap here says.
        with mock.patch.object(rxml, 'MAX_DIGITS', 10 ** 6):
            for text in ('9' * 5000, '0x' + 'f' * 5000):
                with self.subTest(len(text)):
                    with self.assertRaisesRegex(rxml.Refused, r'^<a id="q"> in f\.xml has x='):
                        self.el(f'<a id="q" x="{text}"/>').number('x', 0)

    def test_a_number_that_is_not_one_is_refused_naming_the_element(self):
        for bad in ('abc', '', '1 2', '0x', '1.5', '-1', '0x100000000', '4294967296'):
            with self.subTest(bad):
                with self.assertRaisesRegex(rxml.Refused, r'^<a id="q"> in f\.xml has x='):
                    self.el(f'<a id="q" x="{bad}"/>').number('x', 0)

    def test_a_negative_number_is_allowed_where_the_attribute_is_signed(self):
        self.assertEqual(self.el('<a x="-1"/>').number('x', 0, signed=True), -1)
        with self.assertRaises(rxml.Refused):
            self.el('<a x="-2147483649"/>').number('x', 0, signed=True)

    def test_flags_are_true_on_1_t_or_y_as_riivolution_reads_them(self):
        for value in ('1', 't', 'true', 'True', 'T', 'y', 'yes', 'Y'):
            self.assertTrue(self.el(f'<a f="{value}"/>').flag('f', False), value)
        for value in ('0', 'f', 'false', 'no', 'n', '', 'x'):
            self.assertFalse(self.el(f'<a f="{value}"/>').flag('f', True), value)
        self.assertTrue(self.el('<a/>').flag('f', True))
        self.assertFalse(self.el('<a/>').flag('f', False))

    def test_hex_bytes_take_an_optional_0x_and_whole_bytes(self):
        self.assertEqual(self.el('<a v="0xDEad"/>').hexbytes('v'), b'\xde\xad')
        self.assertEqual(self.el('<a v="0012"/>').hexbytes('v'), b'\x00\x12')
        self.assertEqual(self.el('<a v=""/>').hexbytes('v'), b'')
        self.assertEqual(self.el('<a v="0x"/>').hexbytes('v'), b'')
        self.assertIsNone(self.el('<a/>').hexbytes('v'))
        for bad in ('123', '0x123', 'zz', '12 34', '0xg0'):
            with self.subTest(bad):
                with self.assertRaisesRegex(rxml.Refused, "which isn't whole hex bytes"):
                    self.el(f'<a v="{bad}"/>').hexbytes('v')

    def test_a_label_shows_the_identifying_attributes_and_cuts_a_long_one(self):
        el = self.el(f'<file disc="/a.bin" other="x" external="{"e" * 80}"/>')
        label = el.label()
        self.assertTrue(label.startswith('<file disc="/a.bin" external="eee'))
        self.assertTrue(label.endswith('...">'))
        self.assertNotIn('other', label)

    def test_variables_fill_from_the_id_and_anything_else_is_refused(self):
        self.assertEqual(rxml.expand('/{$__gameid}/{$__region}{$__maker}.bin', 'ABCDEF'),
                         '/ABC/DEF.bin')
        self.assertEqual(rxml.expand('{$__gameid}{$__gameid}', 'ABCDEF'), 'ABCABC')
        self.assertEqual(rxml.expand('no variables {here', 'ABCDEF'), 'no variables {here')
        with self.assertRaisesRegex(rxml.VariableError, r'uses \{\$speed\}'):
            rxml.expand('/{$speed}/x', 'ABCDEF')

    def test_a_variable_that_never_closes_is_left_alone_and_the_next_one_still_fills(self):
        self.assertEqual(rxml.expand('{$a{$__gameid}', 'ABCDEF'), '{$aABC')
        self.assertEqual(rxml.expand('{$$__gameid}', 'ABCDEF'), '{$$__gameid}')
        self.assertEqual(rxml.expand('{$__gameid', 'ABCDEF'), '{$__gameid')
        self.assertEqual(rxml.expand('{$' * 1000 + '{$__region}', 'ABCDEF'), '{$' * 1000 + 'D')

    def test_a_long_variable_name_is_refused_with_a_short_message(self):
        with self.assertRaisesRegex(rxml.VariableError, r'^uses \{\$x+\.\.\.') as cm:
            rxml.expand('{$' + 'x' * 100000 + '}', 'ABCDEF')
        self.assertLess(len(str(cm.exception)), 100)

    def test_a_variable_is_filled_in_one_pass(self):
        # An ID whose first characters finish a variable's name can't make a second one.
        self.assertEqual(rxml.expand('{$__gameid}_region}', '{$_xyz'), '{$__region}')


class TestIdCheck(SdCase):
    BODY = '<memory offset="0x80001000" value="aa"/>'

    def on(self, ident, **image):
        self.package('a.xml', self.BODY, ident=ident)
        return self.apply(image=self.image_with(**image) if image else None)['packages']

    def test_no_id_element_fits_any_disc(self):
        self.assertEqual(self.on(''), ['a.xml'])

    def test_game_is_a_prefix_of_the_disc_id(self):
        for game, fits in (('TEST', True), ('TES', True), ('TEST00', True), ('T', True),
                           ('', True), ('TEXT', False), ('EST', False), ('TEST001', False)):
            with self.subTest(game):
                self.assertEqual(self.on(f'<id game="{game}"/>') == ['a.xml'], fits)

    def test_developer_is_the_last_two_characters(self):
        self.assertEqual(self.on('<id developer="00"/>'), ['a.xml'])
        self.assertEqual(self.on('<id developer="01"/>'), [])
        self.assertEqual(self.on('<id game="ZZQ" developer="7A"/>', game=b'ZZQE7A'), ['a.xml'])
        self.assertEqual(self.on('<id game="ZZQ" developer="7B"/>', game=b'ZZQE7A'), [])

    def test_regions_are_checked_against_the_fourth_character(self):
        both = '<id><region type="J"/><region type="E"/></id>'
        self.assertEqual(self.on(both, game=b'ZZQE7A'), ['a.xml'])
        self.assertEqual(self.on(both, game=b'ZZQP7A'), [])
        self.assertEqual(self.on(both, game=b'ZZQJ7A'), ['a.xml'])
        self.assertEqual(self.on('<id game="ZZQ"/>', game=b'ZZQP7A'), ['a.xml'])

    def test_disc_is_the_header_byte_at_6_and_version_the_byte_at_7(self):
        self.assertEqual(self.on('<id disc="1"/>', disc=1), ['a.xml'])
        self.assertEqual(self.on('<id disc="1"/>', disc=0), [])
        self.assertEqual(self.on('<id disc="0"/>'), ['a.xml'])
        self.assertEqual(self.on('<id version="2"/>', revision=2), ['a.xml'])
        self.assertEqual(self.on('<id version="2"/>', revision=1), [])
        self.assertEqual(self.on('<id disc="1" version="0x2"/>', disc=1, revision=2), ['a.xml'])
        self.assertEqual(self.on('<id disc="1" version="3"/>', disc=1, revision=2), [])

    def test_a_package_for_another_disc_is_not_on_and_writes_nothing(self):
        self.package('a.xml', self.BODY, ident='<id game="ELSE"/>')
        self.assertEqual(self.apply(), {
            'packages': [], 'overlay': None, 'files_added': 0, 'files_replaced': 0,
            'ram_patches': 0, 'ram_bytes': 0})
        self.assertEqual(self.outputs(), [])

    def test_a_choice_for_a_package_for_another_disc_is_allowed_and_does_nothing(self):
        self.package('a.xml', self.BODY, ident='<id game="ELSE"/>')
        self.assertEqual(self.apply('--choice', 'a.xml:opt=1')['packages'], [])

    def test_a_disc_id_that_is_not_printable_is_refused(self):
        self.refused(r"the image's disc ID isn't printable ASCII", image=self.image_with(
            game=b'TE T00'))

    def test_a_filter_the_xml_gets_wrong_is_an_error(self):
        self.package('a.xml', self.BODY, ident='<id disc="one"/>')
        self.assertRegex(self.listing()[0]['error'], r"^<id disc=\"one\"> in a\.xml has disc='one'")


class TestChoices(SdCase):
    TWO = '''<wiidisc version="1"><id game="TEST"/><options>
      <section name="Mod"><option name="Pick" id="pick" default="{default}">
        <choice name="A"><patch id="a"/></choice>
        <choice name="B"><patch id="b"/><patch id="c"/></choice></option></section></options>
      <patch id="a"><memory offset="0x80001000" value="aa"/></patch>
      <patch id="b"><memory offset="0x80001000" value="bb"/></patch>
      <patch id="c"><memory offset="0x80001001" value="cc"/></patch></wiidisc>'''

    def two(self, default=0):
        self.xml('two mod.xml', self.TWO.format(default=default))

    def lines(self):
        return self.ram().splitlines()[2:]

    def test_an_option_with_no_default_is_off(self):
        self.two()
        self.assertEqual(self.apply()['packages'], [])
        self.assertEqual(self.outputs(), [])

    def test_the_default_applies_without_a_choice(self):
        self.two(default=2)
        self.assertEqual(self.apply()['packages'], ['two mod.xml'])
        self.assertEqual(self.lines(), ['patch 80001000 bb', 'patch 80001001 cc'])

    def test_a_choice_beats_the_default_and_zero_turns_it_off(self):
        self.two(default=2)
        self.apply('--choice', 'two mod.xml:pick=1')
        self.assertEqual(self.lines(), ['patch 80001000 aa'])
        self.assertEqual(self.apply('--choice', 'two mod.xml:pick=0')['packages'], [])
        self.assertEqual(self.outputs(), [])

    def test_a_default_past_the_choices_is_off(self):
        self.two(default=3)
        self.assertEqual(self.apply()['packages'], [])

    def test_a_choice_with_no_patch_references_turns_nothing_on(self):
        self.xml('a.xml', '''<wiidisc version="1"><options><section name="s">
          <option name="o" id="o" default="1"><choice name="c"/></option></section></options>
          </wiidisc>''')
        self.assertEqual(self.apply()['packages'], [])

    def test_a_colon_in_a_path_or_an_option_id_is_read_right(self):
        self.xml('odd:x.xml', self.TWO.format(default=0).replace('id="pick"', 'id="a:b=c"'))
        self.apply('--choice', 'odd:x.xml:a:b=c=1')
        self.assertEqual(self.lines(), ['patch 80001000 aa'])

    def test_an_option_with_no_id_is_chosen_by_its_section_and_name(self):
        self.xml('a.xml', '''<wiidisc version="1"><options><section name="Mod ">
          <option name="Pick"><choice name="A"><patch id="a"/></choice></option></section>
          </options><patch id="a"><memory offset="0x80001000" value="aa"/></patch></wiidisc>''')
        self.apply('--choice', 'a.xml:Mod Pick=1')
        self.assertEqual(self.lines(), ['patch 80001000 aa'])

    def test_a_patch_two_options_reach_is_applied_once_where_it_first_comes(self):
        self.xml('a.xml', '''<wiidisc version="1"><options><section name="s">
          <option name="o1" id="o1" default="1"><choice name="c"><patch id="a"/></choice></option>
          <option name="o2" id="o2" default="1"><choice name="c"><patch id="b"/>
            <patch id="a"/></choice></option></section></options>
          <patch id="a"><memory offset="0x80001000" value="aa" original="00"/></patch>
          <patch id="b"><memory offset="0x80001004" value="bb"/></patch></wiidisc>''')
        self.apply()
        self.assertEqual(self.lines(), ['patch 80001000 aa 00', 'patch 80001004 bb'])

    def test_patches_apply_in_the_order_the_options_name_them_not_the_order_defined(self):
        self.xml('a.xml', '''<wiidisc version="1"><options><section name="s">
          <option name="o1" id="o1" default="1"><choice name="c"><patch id="late"/></choice>
          </option><option name="o2" id="o2" default="1"><choice name="c"><patch id="early"/>
          </choice></option></section></options>
          <patch id="early"><memory offset="0x80001000" value="11"/></patch>
          <patch id="late"><memory offset="0x80001004" value="22"/></patch></wiidisc>''')
        self.apply()
        self.assertEqual(self.lines(), ['patch 80001004 22', 'patch 80001000 11'])

    def test_the_first_patch_with_an_id_is_the_one_a_choice_reaches(self):
        self.xml('a.xml', '''<wiidisc version="1"><options><section name="s">
          <option name="o" id="o" default="1"><choice name="c"><patch id="p"/></choice></option>
          </section></options><patch id="p"><memory offset="0x80001000" value="11"/></patch>
          <patch id="p"><memory offset="0x80001000" value="22"/></patch></wiidisc>''')
        self.apply()
        self.assertEqual(self.lines(), ['patch 80001000 11'])

    def test_a_reference_to_a_patch_the_xml_lacks_is_refused(self):
        self.xml('a.xml', '''<wiidisc version="1"><options><section name="s">
          <option name="o" id="o" default="1"><choice name="c"><patch id="ghost"/></choice>
          </option></section></options></wiidisc>''')
        self.refused(r"<patch id=\"ghost\"> in a\.xml names a patch the XML doesn't define")

    def test_unknown_packages_options_and_numbers_are_bad_arguments(self):
        self.two()
        for spec, pattern in (
                ('none.xml:pick=1', 'names no package'),
                ('two mod.xml:nope=1', "names no option 'nope'"),
                ('two mod.xml:pick=3', r'past the 2 choices'),
                ('two mod.xml:pick', 'isn.t XML:OPTION=CHOICE'),
                ('two mod.xml:pick=-1', 'isn.t XML:OPTION=CHOICE'),
                ('two mod.xml:pick=x', 'isn.t XML:OPTION=CHOICE'),
                ('pick=1', 'names no package')):
            with self.subTest(spec):
                self.bad_arguments(pattern, '--choice', spec)

    def test_naming_one_option_twice_is_a_bad_argument(self):
        self.two()
        self.bad_arguments('twice', '--choice', 'two mod.xml:pick=1',
                           '--choice', 'two mod.xml:pick=2')

    def test_a_missing_image_and_a_bad_out_are_bad_arguments(self):
        self.bad_arguments("isn't a file", image=os.path.join(self.tmp, 'none.iso'))
        self.bad_arguments("isn't a folder", out=self.put('plain file'))
        self.bad_arguments("isn't a folder", root=os.path.join(self.tmp, 'none'))


class TestFiles(SdCase):
    def replace(self, disc, size=9, external='/mod/n.bin', extra=''):
        self.put('mod/n.bin', b'n' * size)
        self.package('a.xml', f'<file disc="{disc}" external="{external}"{extra}/>')
        return self.apply()

    def test_a_file_replaces_the_disc_file_at_its_path(self):
        summary = self.replace('/Data/readme text.txt')
        self.assertEqual(summary['overlay'], {
            'added_files': 0, 'added_bytes': 0, 'replaced_files': 1, 'replaced_bytes': 9,
            'fst_growth': 0, 'arena_hi_drop': 0})
        self.assertEqual((summary['files_added'], summary['files_replaced']), (0, 1))
        self.assertEqual(summary['packages'], ['a.xml'])
        self.assertEqual(self.placed(), {'Data/readme text.txt': ('../mod/n.bin', 9)})
        self.assertEqual(read_manifest(self.out)[3], [
            (START, FST_LEN, 'fst.bin'), (START + SLOT, 9, '../mod/n.bin')])
        self.assertEqual(self.outputs(), ['disc-overlay.txt', 'fst.bin'])

    def test_the_path_ignores_case_and_the_disc_keeps_its_spelling(self):
        self.replace('/DATA/MAPS/Ground.TEX')
        self.assertEqual(list(self.placed()), ['Data/maps/ground.tex'])

    def test_a_name_with_no_slash_finds_the_first_file_of_that_name_in_fst_order(self):
        tree = fst.parse(self.disc.fst)
        data = next(c for c in tree.children if c.name == 'Data')
        data.children.append(fst.Entry('sfx.dat', 0x6000, 4))
        image = self.image_with(tree=tree)
        self.put('mod/n.bin', b'n' * 3)
        self.package('a.xml', '<file disc="SFX.DAT" external="/mod/n.bin"/>')
        self.apply(image=image)
        self.assertEqual(list(self.placed()), ['audio/sfx.dat'])

    def test_a_name_with_no_slash_does_not_match_a_folder(self):
        self.replace('bgm')
        self.replace('Data')
        self.assertEqual(self.outputs(), [])

    def test_a_name_with_a_slash_and_no_leading_slash_matches_nothing(self):
        summary = self.replace('maps/ground.tex')
        self.assertEqual(summary['packages'], ['a.xml'])
        self.assertEqual(self.outputs(), [])

    def test_a_path_to_nothing_is_skipped_unless_create_is_on(self):
        self.replace('/Data/new.cfg')
        self.assertEqual(self.outputs(), [])
        summary = self.replace('/Data/new.cfg', extra=' create="true"')
        self.assertEqual((summary['files_added'], summary['files_replaced']), (1, 0))
        self.assertEqual(self.placed(), {'Data/new.cfg': ('../mod/n.bin', 9)})

    def test_create_makes_folders_and_joins_an_existing_one_in_another_case(self):
        self.replace('/AUDIO/Fresh/Deep/x.cfg', extra=' create="true"')
        self.assertEqual(list(self.placed()), ['audio/Fresh/Deep/x.cfg'])
        self.assertEqual(self.table().children[0].name, 'audio')

    def test_a_file_where_the_path_wants_a_folder_or_the_reverse_matches_nothing(self):
        self.replace('/audio', extra=' create="true"')
        self.assertEqual(self.outputs(), [])
        self.replace('/Zeta.bin/inner.cfg', extra=' create="true"')
        self.assertEqual(self.outputs(), [])

    def test_a_later_patch_on_the_same_file_wins(self):
        self.put('mod/one.bin', b'1' * 4)
        self.put('mod/two.bin', b'2' * 6)
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/one.bin"/>'
                              '<file disc="zeta.bin" external="/mod/two.bin"/>')
        summary = self.apply()
        self.assertEqual(summary['overlay']['replaced_files'], 1)
        self.assertEqual(self.placed(), {'Zeta.bin': ('../mod/two.bin', 6)})

    def test_a_created_file_is_found_by_a_later_patch(self):
        self.put('mod/one.bin', b'1' * 4)
        self.put('mod/two.bin', b'2' * 6)
        self.package('a.xml', '<file disc="/Brand/new.cfg" external="/mod/one.bin" create="1"/>'
                              '<file disc="NEW.CFG" external="/mod/two.bin"/>')
        self.apply()
        self.assertEqual(self.placed(), {'Brand/new.cfg': ('../mod/two.bin', 6)})

    def test_the_whole_file_is_used_and_length_cuts_it(self):
        self.replace('/Zeta.bin', size=20, extra=' length="12"')
        self.assertEqual(self.placed(), {'Zeta.bin': ('../mod/n.bin', 12)})
        self.assertEqual(read_manifest(self.out)[3][-1], (START + SLOT, 12, '../mod/n.bin'))
        self.replace('/Zeta.bin', size=20, extra=' length="20"')
        self.assertEqual(self.placed(), {'Zeta.bin': ('../mod/n.bin', 20)})

    def test_an_empty_file_gives_an_empty_disc_file(self):
        self.replace('/Zeta.bin', size=0)
        self.assertEqual(self.table().children[-1].size, 0)
        self.assertEqual(read_manifest(self.out)[3], [(START, FST_LEN, 'fst.bin')])

    def test_resize_false_is_fine_when_the_new_bytes_reach_the_end_of_the_old_file(self):
        for size in (SIZES['Zeta.bin'], SIZES['Zeta.bin'] + 5):
            self.replace('/Zeta.bin', size=size, extra=' resize="false"')
            self.assertEqual(self.placed(), {'Zeta.bin': ('../mod/n.bin', size)})

    def test_resize_false_is_fine_for_a_file_the_patch_creates(self):
        self.replace('/Data/new.cfg', size=3, extra=' resize="false" create="true"')
        self.assertEqual(self.placed(), {'Data/new.cfg': ('../mod/n.bin', 3)})

    def test_an_offset_of_0_to_3_is_dropped_as_dolphin_drops_it(self):
        self.replace('/Zeta.bin', extra=' offset="3" fileoffset="0"')
        self.assertEqual(list(self.placed()), ['Zeta.bin'])

    def test_the_variables_of_the_game_id_fill_in_a_path(self):
        self.put('mod/TES/T00.bin', b'v' * 5)
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/{$__gameid}/'
                              '{$__region}{$__maker}.bin"/>')
        self.apply()
        self.assertEqual(self.placed(), {'Zeta.bin': ('../mod/TES/T00.bin', 5)})

    def test_a_new_name_that_is_not_plain_ascii_is_refused(self):
        self.put('mod/n.bin')
        self.package('a.xml', '<file disc="/Data/café.cfg" external="/mod/n.bin" '
                              'create="true"/>')
        self.refused(r"<file disc=\".*\" external=\"/mod/n\.bin\"> in a\.xml would add the name "
                     r"'caf.*' to the disc")


class TestFolders(SdCase):
    def tree(self):
        self.put('mod/dir/Zeta.bin', b'z' * 3)
        self.put('mod/dir/SFX.DAT', b's' * 4)
        self.put('mod/dir/nomatch.cfg', b'q')
        self.put('mod/dir/inner/ground.tex', b'g' * 5)
        self.put('mod/dir/inner/deeper/readme text.txt', b'r' * 6)

    def test_a_folder_with_no_disc_matches_only_its_top_level_files_by_name(self):
        self.tree()
        self.package('a.xml', '<folder external="/mod/dir"/>')
        summary = self.apply()
        self.assertEqual(summary['files_replaced'], 2)
        self.assertEqual(self.placed(), {'Zeta.bin': ('../mod/dir/Zeta.bin', 3),
                                         'audio/sfx.dat': ('../mod/dir/SFX.DAT', 4)})

    def test_a_folder_with_a_disc_that_has_no_slash_matches_nothing(self):
        self.tree()
        self.package('a.xml', '<folder disc="Data" external="/mod/dir"/>')
        self.apply()
        self.assertEqual(self.outputs(), [])

    def test_a_folder_at_the_root_with_create_adds_what_it_holds_and_replaces_what_matches(self):
        self.tree()
        self.package('a.xml', '<folder disc="/" external="/mod/dir" create="true"/>')
        summary = self.apply()
        self.assertEqual((summary['files_added'], summary['files_replaced']), (4, 1))
        self.assertEqual(self.placed(), {
            'Zeta.bin': ('../mod/dir/Zeta.bin', 3),
            'inner/ground.tex': ('../mod/dir/inner/ground.tex', 5),
            'inner/deeper/readme text.txt': ('../mod/dir/inner/deeper/readme text.txt', 6),
            'nomatch.cfg': ('../mod/dir/nomatch.cfg', 1),
            'SFX.DAT': ('../mod/dir/SFX.DAT', 4)})

    def test_without_create_a_folder_at_a_path_replaces_only_what_exists(self):
        self.put('mod/dir/ground.tex', b'g' * 5)
        self.put('mod/dir/other.tex', b'o')
        self.package('a.xml', '<folder disc="/Data/maps" external="/mod/dir"/>')
        self.apply()
        self.assertEqual(self.placed(), {'Data/maps/ground.tex': ('../mod/dir/ground.tex', 5)})

    def test_recursive_false_stops_at_the_top_folder(self):
        self.tree()
        self.package('a.xml', '<folder disc="/" external="/mod/dir" create="true" '
                              'recursive="false"/>')
        self.apply()
        self.assertEqual(sorted(self.placed()), ['SFX.DAT', 'Zeta.bin', 'nomatch.cfg'])

    def test_a_trailing_slash_on_the_disc_path_makes_no_difference(self):
        self.tree()
        self.package('a.xml', '<folder disc="/Fresh/" external="/mod/dir/inner" create="true"/>')
        self.apply()
        self.assertEqual(sorted(self.placed()), ['Fresh/deeper/readme text.txt',
                                                 'Fresh/ground.tex'])

    def test_dotfiles_and_dot_folders_are_not_added(self):
        self.put('mod/dir/.DS_Store')
        self.put('mod/dir/._Zeta.bin')
        self.put('mod/dir/.hidden/x.cfg')
        self.put('mod/dir/kept.cfg')
        self.package('a.xml', '<folder disc="/" external="/mod/dir" create="true"/>')
        self.apply()
        self.assertEqual(list(self.placed()), ['kept.cfg'])

    def test_length_applies_to_each_file(self):
        self.put('mod/dir/Zeta.bin', b'z' * 9)
        self.put('mod/dir/SFX.DAT', b's' * 2)
        self.package('a.xml', '<folder external="/mod/dir" length="2"/>')
        self.apply()
        self.assertEqual({k: v[1] for k, v in self.placed().items()},
                         {'Zeta.bin': 2, 'audio/sfx.dat': 2})

    def test_resize_false_on_a_folder_refuses_a_file_that_is_too_short(self):
        self.put('mod/dir/Zeta.bin', b'z')
        self.package('a.xml', '<folder external="/mod/dir" resize="false"/>')
        self.refused(r'<folder external="/mod/dir"> in a\.xml has resize="false"')

    def test_files_go_in_name_order_so_the_last_of_two_for_one_disc_file_wins(self):
        # A listing with two names that differ only by case, which not every file system can
        # hold, so the folder's listing is made up here and the files behind it are real.
        one = self.put('mod/dir/one.bin', b'1')
        two = self.put('mod/dir/two.bin', b'22')
        self.package('a.xml', '<folder disc="/" external="/mod/dir" create="true"/>')
        real = os.scandir

        def entry(name, path):
            return types.SimpleNamespace(
                name=name, path=path, is_symlink=lambda: False,
                is_dir=lambda **kw: False, is_file=lambda **kw: True)

        def scandir(path):
            if path == os.path.join(self.root, 'mod', 'dir'):
                return contextlib.nullcontext([entry('a.cfg', one), entry('A.cfg', two)])
            return real(path)

        with mock.patch.object(rpatch.os, 'scandir', scandir):
            self.apply()
        self.assertEqual(self.placed(), {'A.cfg': ('../mod/dir/one.bin', 1)})

    def test_a_symbolic_link_in_a_folder_is_refused(self):
        self.put('mod/dir/Zeta.bin', b'z')
        self.link(self.put('mod/real.bin'), os.path.join(self.root, 'mod', 'dir', 'link.bin'))
        self.package('a.xml', '<folder external="/mod/dir"/>')
        self.refused(r"<folder external=\"/mod/dir\"> in a\.xml holds the symbolic link "
                     r"'mod/dir/link\.bin', which isn't read")

    def test_a_folder_that_is_missing_or_a_file_is_refused(self):
        self.put('mod/file.bin')
        self.package('a.xml', '<folder external="/mod/nope"/>')
        self.refused(r"<folder external=\"/mod/nope\"> in a\.xml has the path '/mod/nope', "
                     r"which has no 'nope'")
        self.package('a.xml', '<folder external="/mod/file.bin"/>')
        self.refused(r"in a\.xml has the path '/mod/file\.bin', which isn't a folder")

    def test_a_main_dol_in_a_folder_with_no_disc_is_refused(self):
        self.put('mod/dir/Main.DOL', bytes(0x200))
        self.package('a.xml', '<folder external="/mod/dir"/>')
        self.refused(r"<folder external=\"/mod/dir\"> in a\.xml patches main\.dol, which isn't "
                     r"supported")

    def test_a_main_dol_in_a_folder_with_a_path_is_just_a_file(self):
        self.put('mod/dir/main.dol', bytes(0x200))
        self.package('a.xml', '<folder disc="/" external="/mod/dir" create="true"/>')
        self.apply()
        self.assertEqual(list(self.placed()), ['main.dol'])


class TestPaths(SdCase):
    """External paths, as Dolphin resolves them, through a file patch and through resolve."""

    def via(self, external, root=''):
        self.package('a.xml', f'<file disc="/Zeta.bin" external="{external}"/>', root=root)
        return self.apply()

    def placed_from(self):
        return self.placed()['Zeta.bin'][0]

    def test_a_path_with_no_slash_starts_at_the_xml_folder(self):
        self.put('riivolution/n.bin', b'n')
        self.package('a.xml', '<file disc="/Zeta.bin" external="n.bin"/>')
        self.apply()
        self.assertEqual(self.placed_from(), '../riivolution/n.bin')

    def test_a_path_with_a_leading_slash_starts_at_the_sd_root(self):
        self.put('mod/n.bin', b'n')
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/n.bin"/>')
        self.apply()
        self.assertEqual(self.placed_from(), '../mod/n.bin')

    def test_a_root_with_no_slash_starts_at_the_xml_folder(self):
        self.put('riivolution/pack/n.bin', b'n')
        self.package('a.xml', '<file disc="/Zeta.bin" external="n.bin"/>', root='pack')
        self.apply()
        self.assertEqual(self.placed_from(), '../riivolution/pack/n.bin')

    def test_a_root_with_a_slash_starts_at_the_sd_root(self):
        self.put('pack/n.bin', b'n')
        self.package('a.xml', '<file disc="/Zeta.bin" external="n.bin"/>', root='/pack')
        self.apply()
        self.assertEqual(self.placed_from(), '../pack/n.bin')

    def test_a_patch_root_beats_the_root_of_the_wiidisc_element(self):
        self.put('one/n.bin', b'1')
        self.put('two/n.bin', b'22')
        self.xml('a.xml', '''<wiidisc version="1" root="/one"><options><section name="s">
          <option name="o" id="o" default="1"><choice name="c"><patch id="p"/></choice></option>
          </section></options><patch id="p" root="/two">
          <file disc="/Zeta.bin" external="n.bin"/></patch></wiidisc>''')
        self.apply()
        self.assertEqual(self.placed_from(), '../two/n.bin')

    def test_the_root_variables_fill_in_too(self):
        self.put('TES/n.bin', b'n')
        self.package('a.xml', '<file disc="/Zeta.bin" external="n.bin"/>',
                     root='/{$__gameid}')
        self.apply()
        self.assertEqual(self.placed_from(), '../TES/n.bin')

    def test_a_root_that_goes_nowhere_is_refused(self):
        self.put('riivolution/n.bin')
        self.package('a.xml', '<file disc="/Zeta.bin" external="n.bin"/>', root='../out')
        self.refused(r"<patch id=\"p\"> in a\.xml has the root '\.\./out', which goes above")
        self.package('a.xml', '<file disc="/Zeta.bin" external="n.bin"/>', root='/missing')
        self.refused(r"<patch id=\"p\"> in a\.xml has the root '/missing', which has no 'missing'")
        self.package('a.xml', '<file disc="/Zeta.bin" external="n.bin"/>',
                     root='/riivolution/n.bin')
        self.refused(r"<patch id=\"p\"> in a\.xml has the root '/riivolution/n\.bin', "
                     r"which isn't a folder")

    def test_dots_and_extra_slashes_are_skipped_and_dot_dot_goes_back_up(self):
        self.put('mod/a/n.bin', b'n')
        self.put('mod/b/x', b'')
        for external in ('/mod/./a//n.bin', '/mod/b/../a/n.bin', '//mod/a/n.bin',
                         '/mod/a/n.bin/', '/mod/a/../a/./n.bin'):
            with self.subTest(external):
                self.via(external)
                self.assertEqual(self.placed_from(), '../mod/a/n.bin')

    def test_dot_dot_past_where_the_path_started_is_refused(self):
        self.put('mod/n.bin')
        for external in ('../n.bin', '/../mod/n.bin', '/mod/../../n.bin', '..',
                         '/mod/n.bin/../../..'):
            with self.subTest(external):
                self.package('a.xml', f'<file disc="/Zeta.bin" external="{external}"/>')
                self.refused(r"<file .*> in a\.xml has the path '.*', "
                             r"which goes above the folder it starts from")

    def test_three_or_more_dots_are_refused(self):
        self.put('mod/n.bin')
        for external in ('/mod/.../n.bin', '/mod/..../n.bin', '.../n.bin', '/mod/n.bin/...'):
            with self.subTest(external):
                self.package('a.xml', f'<file disc="/Zeta.bin" external="{external}"/>')
                self.refused(r"<file .*> in a\.xml has the path '.*', which has a part made "
                             r"only of dots")

    def test_a_name_with_dots_in_it_is_a_name(self):
        self.put('mod/..n/a..bin', b'n')
        self.via('/mod/..n/a..bin')
        self.assertEqual(self.placed_from(), '../mod/..n/a..bin')

    def test_each_part_matches_the_host_spelling_whatever_its_case(self):
        self.put('Mod/Sub Dir/N.BIN', b'n')
        self.via('/mod/sub dir/n.bin')
        self.assertEqual(self.placed_from(), '../Mod/Sub Dir/N.BIN')

    def test_an_exact_spelling_beats_a_name_that_differs_by_case_and_twins_are_refused(self):
        # A folder with 'A.bin' and 'a.bin', which not every file system can hold, is made up.
        entries = [types.SimpleNamespace(name=n) for n in ('A.bin', 'a.bin', 'b.bin')]
        with mock.patch.object(rpaths.os, 'scandir',
                               return_value=contextlib.nullcontext(entries)):
            self.assertEqual(rpaths.match('/x', 'a.bin'), 'a.bin')
            self.assertEqual(rpaths.match('/x', 'B.BIN'), 'b.bin')
            self.assertIsNone(rpaths.match('/x', 'c.bin'))
            with self.assertRaisesRegex(rpaths.PathError,
                                        r"^matches 'A\.bin', 'a\.bin' by case alone"):
                rpaths.match('/x', 'A.BIN')

    def test_only_ascii_letters_fold(self):
        self.assertEqual(rpaths.fold('ABCxyz-_09'), 'abcxyz-_09')
        self.assertEqual(rpaths.fold('Kİ'), 'Kİ')

    def test_a_missing_part_is_refused_naming_it(self):
        self.put('mod/n.bin')
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/missing/n.bin"/>')
        self.refused(r"<file .*> in a\.xml has the path '/mod/missing/n\.bin', "
                     r"which has no 'missing'")

    def test_a_folder_where_a_file_goes_is_refused(self):
        self.put('mod/dir/n.bin')
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/dir"/>')
        self.refused(r"in a\.xml has the path '/mod/dir', which isn't a file")
        self.package('a.xml', '<file disc="/Zeta.bin" external=""/>')
        self.refused(r"in a\.xml has the path '', which isn't a file")

    def test_an_absolute_host_path_is_read_from_the_sd_root_not_from_the_host(self):
        outside = self.write(self.tmp, 'outside/n.bin', b'n')
        self.package('a.xml', f'<file disc="/Zeta.bin" external="{outside}"/>')
        self.refused(r"<file .*> in a\.xml has the path '.*', which has no ")

    def test_a_symbolic_link_to_a_file_is_refused(self):
        self.link(self.put('real/n.bin'), os.path.join(self.root, 'mod.bin'))
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod.bin"/>')
        self.refused(r"in a\.xml has the path '/mod\.bin', which passes the symbolic link "
                     r"'mod\.bin'")

    def test_a_symbolic_link_to_a_folder_in_the_middle_of_a_path_is_refused(self):
        self.put('real/n.bin')
        self.link(os.path.join(self.root, 'real'), os.path.join(self.root, 'linked'))
        self.package('a.xml', '<file disc="/Zeta.bin" external="/linked/n.bin"/>')
        self.refused(r"which passes the symbolic link 'linked'")
        self.package('a.xml', '<file disc="/Zeta.bin" external="/LINKED/n.bin"/>')
        self.refused(r"which passes the symbolic link 'linked'")

    def test_a_symbolic_link_to_a_file_that_would_leave_the_root_is_refused(self):
        outside = self.write(self.tmp, 'outside/secret.bin', b'secret')
        self.link(outside, os.path.join(self.root, 'mod.bin'))
        self.package('a.xml', '<memory offset="0x80001000" valuefile="/mod.bin"/>')
        self.refused(r"<memory .*> in a\.xml has the path '/mod\.bin', which passes the symbolic")

    def test_a_symbolic_link_for_the_root_itself_is_followed(self):
        self.put('mod/n.bin', b'n')
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/n.bin"/>')
        link = os.path.join(self.tmp, 'link to root')
        self.link(self.root, link)
        self.apply(root=link)
        self.assertEqual(self.placed_from(), '../mod/n.bin')

    def test_a_root_with_spaces_works_and_the_manifest_keeps_the_spaces(self):
        self.put('my mod/some file.bin', b'n')
        self.via('/my mod/some file.bin')
        self.assertEqual(self.placed_from(), '../my mod/some file.bin')
        with open(os.path.join(self.out, 'disc-overlay.txt'), 'rb') as f:
            self.assertIn(b'../my mod/some file.bin\n', f.read())

    def test_a_relative_root_argument_gives_the_same_overlay(self):
        self.put('mod/n.bin', b'n')
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/n.bin"/>')
        self.apply()
        want = read_manifest(self.out)
        here = os.getcwd()
        os.chdir(self.tmp)
        try:
            self.apply(root='sd root', out='sd root/.out')
        finally:
            os.chdir(here)
        self.assertEqual(read_manifest(self.out), want)

    def test_a_variable_that_is_not_the_id_is_refused(self):
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/{$speed}.bin"/>')
        self.refused(r"<file .*> in a\.xml uses \{\$speed\}, which isn't supported")


class TestMemory(SdCase):
    def patches(self, body, *extra):
        self.package('a.xml', body)
        summary = self.apply(*extra)
        return summary, self.ram().splitlines()

    def test_the_list_has_the_header_the_base_and_one_line_per_patch(self):
        summary, lines = self.patches(
            '<memory offset="0x80001000" value="4E800020"/>'
            '<memory offset="0x80001004" value="0x00ff" original="aabb"/>')
        self.assertEqual(lines, ['gcn-ram-patches 1', 'base TEST00', 'patch 80001000 4e800020',
                                 'patch 80001004 00ff aabb'])
        self.assertEqual((summary['ram_patches'], summary['ram_bytes']), (2, 6))
        with open(os.path.join(self.out, 'ram-patches.txt'), 'rb') as f:
            self.assertTrue(f.read().endswith(b'aabb\n'))
        self.assertEqual(self.outputs(), ['ram-patches.txt'])
        self.assertIsNone(summary['overlay'])

    def test_an_offset_is_ored_with_0x80000000(self):
        _, lines = self.patches('<memory offset="0x1234" value="aa"/>'
                                '<memory offset="3000" value="bb"/>'
                                '<memory offset="0x81000000" value="cc"/>'
                                '<memory offset="0x80000010" value="dd"/>')
        self.assertEqual(lines[2:], ['patch 80001234 aa', 'patch 80000bb8 bb',
                                     'patch 81000000 cc', 'patch 80000010 dd'])

    def test_main_ram_runs_from_80000000_to_817fffff(self):
        _, lines = self.patches(
            '<memory offset="0x817FFFFF" value="aa"/>'
            '<memory offset="0x817FFFFC" value="aabbccdd" original="00000000"/>'
            '<memory offset="0x00000004" value="aa"/>')
        self.assertEqual(len(lines), 5)

    def test_a_patch_outside_main_ram_is_refused(self):
        for offset, value, original in (('0x81800000', 'aa', ''), ('0x817FFFFF', 'aabb', ''),
                                        ('0x817FFFFF', 'aa', ' original="aabb"'),
                                        ('0xC0001000', 'aa', ''), ('0x82000000', 'aa', ''),
                                        ('0xFFFFFFFF', 'aa', '')):
            with self.subTest(offset, value=value):
                self.package('a.xml', f'<memory offset="{offset}" value="{value}"{original}/>')
                self.refused(r"<memory offset=\".*\"> in a\.xml covers [0-9a-f]{8} to "
                             r"[0-9a-f]{8}, which isn't inside main RAM \(80000000 to 817fffff\)")

    def test_a_memory_patch_with_no_offset_or_a_zero_offset_is_refused(self):
        for attrs in ('value="aa"', 'offset="0" value="aa"', 'offset="0x0" value="aa"'):
            with self.subTest(attrs):
                self.package('a.xml', f'<memory {attrs}/>')
                self.refused(r"^error: <memory[^>]*> in a\.xml has no offset")

    def test_a_memory_patch_with_nothing_to_write_is_refused(self):
        for attrs in ('offset="0x1000"', 'offset="0x1000" value=""', 'offset="0x1000" value="0x"'):
            with self.subTest(attrs):
                self.package('a.xml', f'<memory {attrs}/>')
                self.refused(r"<memory .*> in a\.xml has no value")

    def test_bad_hex_is_refused_naming_the_attribute(self):
        for attrs, name in (('value="abc"', 'value'), ('value="zz"', 'value'),
                            ('value="aa" original="a"', 'original'),
                            ('value="aa" original="0xzz"', 'original')):
            with self.subTest(attrs):
                self.package('a.xml', f'<memory offset="0x1000" {attrs}/>')
                self.refused(rf"<memory .*> in a\.xml has {name}=.*, which isn't whole hex bytes")

    def test_value_and_valuefile_together_are_refused(self):
        self.put('mod/blob.bin', b'abcd')
        self.package('a.xml', '<memory offset="0x1000" value="aa" valuefile="/mod/blob.bin"/>')
        self.refused(r"<memory .*> in a\.xml has both value and valuefile")

    def test_a_valuefile_is_one_patch_of_the_file_s_bytes(self):
        self.put('mod/blob.bin', bytes(range(0x10, 0x30)))
        summary, lines = self.patches('<memory offset="0x80003100" valuefile="/mod/blob.bin"/>')
        self.assertEqual(lines[2:], ['patch 80003100 ' + bytes(range(0x10, 0x30)).hex()])
        self.assertEqual(summary['ram_bytes'], 32)

    def test_a_valuefile_that_is_missing_empty_or_too_big_is_refused(self):
        self.put('mod/empty.bin', b'')
        for name, pattern in (('/mod/none.bin', "which has no 'none.bin'"),
                              ('/mod/empty.bin', 'has no value')):
            with self.subTest(name):
                self.package('a.xml', f'<memory offset="0x1000" valuefile="{name}"/>')
                self.refused(rf"<memory .*> in a\.xml .*{pattern}")
        big = self.put('mod/big.bin')
        os.truncate(big, 0x1800001)
        self.package('a.xml', '<memory offset="0x1000" valuefile="/mod/big.bin"/>')
        self.refused(r"<memory .*> in a\.xml has a valuefile, '/mod/big\.bin', that is bigger "
                     r"than main RAM")

    def test_a_valuefile_the_size_of_ram_still_has_to_fit_where_it_starts(self):
        big = self.put('mod/big.bin')
        os.truncate(big, 0x1800000)
        self.package('a.xml', '<memory offset="0x2000" valuefile="/mod/big.bin"/>')
        self.refused(r"<memory .*> in a\.xml covers 80002000 to 81801fff, which isn't inside "
                     r"main RAM")

    def test_the_valuefile_variables_fill_in(self):
        self.put('mod/TES.bin', b'ab')
        self.patches('<memory offset="0x1000" valuefile="/mod/{$__gameid}.bin"/>')
        self.assertEqual(self.ram().splitlines()[2], 'patch 80001000 6162')

    def test_patches_keep_the_package_s_order_and_the_ones_inside_a_blob_check_in_order(self):
        blob = bytes(range(0x10, 0x30))
        self.put('mod/blob.bin', blob)
        body = ('<memory offset="0x80003100" valuefile="/mod/blob.bin"/>'
                '<memory offset="0x80003104" value="60000000" original="14151617"/>'
                '<memory offset="0x80003108" value="4e800020" original="18191a1b"/>'
                '<memory offset="0x80003104" value="38600001" original="60000000"/>'
                '<memory offset="0x800031f0" value="01020304" original="00000000"/>')
        summary, lines = self.patches(body)
        self.assertEqual(lines, [
            'gcn-ram-patches 1', 'base TEST00', 'patch 80003100 ' + blob.hex(),
            'patch 80003104 60000000 14151617', 'patch 80003108 4e800020 18191a1b',
            'patch 80003104 38600001 60000000', 'patch 800031f0 01020304 00000000'])
        self.assertEqual(summary['ram_patches'], 5)
        ram = bytearray(0x400)
        for line in lines[2:]:
            _, addr, value, *original = line.split(' ')
            at = int(addr, 16) - 0x80003000
            if original:
                self.assertEqual(bytes(ram[at:at + len(original[0]) // 2]),
                                 bytes.fromhex(original[0]), line)
            ram[at:at + len(value) // 2] = bytes.fromhex(value)
        self.assertEqual(bytes(ram[0x100:0x120][:4]), blob[:4])

    def test_search_and_ocarina_are_refused_unless_they_are_false(self):
        for attr in ('search', 'ocarina'):
            for value in ('true', '1', 'yes'):
                with self.subTest(attr, value=value):
                    self.package('a.xml', f'<memory offset="0x1000" value="aa" {attr}="{value}"/>')
                    self.refused(rf"<memory offset=\"0x1000\"> in a\.xml uses {attr}, "
                                 r"which isn't supported")
            self.package('a.xml', f'<memory offset="0x1000" value="aa" {attr}="false"/>')
            self.assertEqual(self.apply()['ram_patches'], 1)
            shutil.rmtree(self.out)

    def test_a_memory_patch_in_an_off_option_is_not_read(self):
        self.package('a.xml', '<memory offset="0" value="zz" search="true"/>', default=0)
        self.assertEqual(self.apply()['packages'], [])


class TestRefusals(SdCase):
    def test_each_unsupported_element_is_refused_naming_it(self):
        self.put('mod/n.bin', b'n')
        self.put('mod/dir/x.bin', b'n')
        cases = [
            ('<savegame external="/mod/save" clone="true"/>',
             r"<savegame external=\"/mod/save\"> in a\.xml isn't supported, since the engine "
             r"doesn't redirect saves"),
            ('<dolphin_sys_file disc="x" external="/mod/n.bin"/>',
             r"<dolphin_sys_file disc=\"x\" external=\"/mod/n\.bin\"> in a\.xml isn't supported"),
            ('<dolphin_sys_folder disc="x" external="/mod/dir"/>',
             r"<dolphin_sys_folder disc=\"x\" external=\"/mod/dir\"> in a\.xml isn't supported"),
            ('<memory offset="0x1000" value="aa" search="true"/>',
             r"<memory offset=\"0x1000\"> in a\.xml uses search"),
            ('<memory offset="0x1000" value="aa" ocarina="true"/>',
             r"<memory offset=\"0x1000\"> in a\.xml uses ocarina"),
            ('<file disc="/Zeta.bin" external="/mod/n.bin" offset="4"/>',
             r"<file disc=\"/Zeta\.bin\" external=\"/mod/n\.bin\" offset=\"4\"> in a\.xml has "
             r"an offset, which isn't supported"),
            ('<file disc="/Zeta.bin" external="/mod/n.bin" fileoffset="0x10"/>',
             r"<file .*> in a\.xml has a fileoffset, which isn't supported"),
            ('<file disc="main.dol" external="/mod/n.bin"/>',
             r"<file disc=\"main\.dol\" external=\"/mod/n\.bin\"> in a\.xml patches main\.dol, "
             r"which isn't supported"),
            ('<file disc="MAIN.DOL" external="/mod/n.bin"/>',
             r"<file .*> in a\.xml patches main\.dol"),
            ('<file disc="/Zeta.bin" external="/mod/n.bin" length="2"/>',
             r"<file .*> in a\.xml has length 2, which is past the end of 'mod/n\.bin' "
             r"\(size 1\)"),
            ('<file disc="/Zeta.bin" external="/mod/n.bin" length="0x10"/>',
             r"<file .*> in a\.xml has length 16, which is past the end of"),
            ('<file disc="/Zeta.bin" external="/mod/n.bin" resize="false"/>',
             r"<file .*> in a\.xml has resize=\"false\", and 'mod/n\.bin' \(size 1\) is "
             r"smaller than '/Zeta\.bin' \(size 4100\), which isn't supported"),
            ('<file disc="" external="/mod/n.bin"/>', r"<file .*> in a\.xml has no disc path"),
            ('<file disc="/a//b" external="/mod/n.bin"/>',
             r"<file .*> in a\.xml has the disc path '/a//b', which has an empty or dot part"),
            ('<file disc="/a/../b" external="/mod/n.bin"/>', r"which has an empty or dot part"),
            ('<file disc="/" external="/mod/n.bin"/>', r"which has an empty or dot part"),
            ('<file disc="/Zeta.bin" external="/mod/n.bin" offset="x"/>',
             r"<file .*> in a\.xml has offset='x', which isn't a number"),
        ]
        for body, pattern in cases:
            with self.subTest(body):
                self.package('a.xml', body)
                self.refused(pattern)

    def test_a_package_with_params_or_macros_is_refused_once_a_choice_names_it(self):
        self.xml('a.xml', '''<wiidisc version="1"><options><section name="s">
          <option name="o" id="o" default="1"><choice name="c"><patch id="p">
          <param name="x" value="y"/></patch></choice></option></section></options></wiidisc>''')
        self.assertEqual(self.apply()['packages'], [])
        self.refused(r"<param name=\"x\"> in a\.xml isn't supported", '--choice', 'a.xml:o=1')
        self.xml('a.xml', '<wiidisc version="1"><options><macros id="m"/></options></wiidisc>')
        self.refused(r"<macros id=\"m\"> in a\.xml isn't supported", '--choice', 'a.xml:o=1')

    def test_a_refusal_in_one_package_stops_the_run_and_leaves_old_outputs(self):
        self.put('mod/n.bin', b'n')
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/n.bin"/>')
        self.package('b.xml', '<savegame external="/mod/save"/>')
        self.stale()
        self.refused(r"<savegame .*> in b\.xml isn't supported", out=self.out)
        self.assertEqual(self.outputs(), OUTPUTS)
        for name in OUTPUTS:
            with open(os.path.join(self.out, name), 'rb') as f:
                self.assertEqual(f.read(), b'old')

    def test_turning_the_refused_package_off_lets_the_rest_apply(self):
        self.put('mod/n.bin', b'n')
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/n.bin"/>')
        self.package('b.xml', '<savegame external="/mod/save"/>')
        self.assertEqual(self.apply('--choice', 'b.xml:opt=0')['packages'], ['a.xml'])

    def test_an_unsupported_element_in_an_unchosen_patch_is_not_looked_at(self):
        self.xml('a.xml', '''<wiidisc version="1"><options><section name="s">
          <option name="o" id="o" default="1"><choice name="c"><patch id="ok"/></choice></option>
          </section></options><patch id="ok"><memory offset="0x1000" value="aa"/></patch>
          <patch id="never"><savegame external="x"/></patch></wiidisc>''')
        self.assertEqual(self.apply()['ram_patches'], 1)

    def test_other_elements_in_a_patch_are_ignored(self):
        self.package('a.xml', '<unknown thing="1"/><memory offset="0x1000" value="aa"/>')
        self.assertEqual(self.apply()['ram_patches'], 1)


class TestConflicts(SdCase):
    def setUp(self):
        super().setUp()
        self.put('mod/one.bin', b'1')
        self.put('mod/two.bin', b'22')

    def test_two_packages_on_one_disc_file_are_refused_naming_both(self):
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/one.bin"/>')
        self.package('b.xml', '<file disc="/Zeta.bin" external="/mod/two.bin"/>')
        self.refused(r"a\.xml and b\.xml both patch the disc file /Zeta\.bin")

    def test_the_message_names_the_same_two_whichever_runs_first(self):
        self.package('z.xml', '<file disc="/Zeta.bin" external="/mod/one.bin"/>')
        self.package('b.xml', '<file disc="/Zeta.bin" external="/mod/two.bin"/>')
        self.refused(r"b\.xml and z\.xml both patch the disc file /Zeta\.bin")

    def test_the_same_file_in_another_case_or_found_by_name_is_the_same_file(self):
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/one.bin"/>')
        self.package('b.xml', '<file disc="ZETA.BIN" external="/mod/two.bin"/>')
        self.refused(r"a\.xml and b\.xml both patch the disc file /Zeta\.bin")

    def test_two_packages_that_create_one_file_are_refused(self):
        self.package('a.xml', '<file disc="/New/x.cfg" external="/mod/one.bin" create="true"/>')
        self.package('b.xml', '<file disc="/new/X.cfg" external="/mod/two.bin" create="true"/>')
        self.refused(r"a\.xml and b\.xml both patch the disc file /New/x\.cfg")

    def test_two_packages_in_one_new_folder_are_fine(self):
        self.package('a.xml', '<file disc="/New/x.cfg" external="/mod/one.bin" create="true"/>')
        self.package('b.xml', '<file disc="/new/y.cfg" external="/mod/two.bin" create="true"/>')
        self.assertEqual(self.apply()['files_added'], 2)
        self.assertEqual(sorted(self.placed()), ['New/x.cfg', 'New/y.cfg'])

    def test_a_folder_patch_conflicts_file_by_file(self):
        self.put('mod/dir/Zeta.bin', b'z')
        self.package('a.xml', '<folder external="/mod/dir"/>')
        self.package('b.xml', '<file disc="/Zeta.bin" external="/mod/one.bin"/>')
        self.refused(r"a\.xml and b\.xml both patch the disc file /Zeta\.bin")

    def test_two_packages_on_different_files_are_fine(self):
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/one.bin"/>')
        self.package('b.xml', '<file disc="/opening.bnr" external="/mod/two.bin"/>')
        self.assertEqual(self.apply()['packages'], ['a.xml', 'b.xml'])

    def test_a_package_that_is_off_or_for_another_disc_does_not_conflict(self):
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/one.bin"/>')
        self.package('b.xml', '<file disc="/Zeta.bin" external="/mod/two.bin"/>')
        self.assertEqual(self.apply('--choice', 'b.xml:opt=0')['packages'], ['a.xml'])
        self.package('b.xml', '<file disc="/Zeta.bin" external="/mod/two.bin"/>',
                     ident='<id game="ELSE"/>')
        self.assertEqual(self.apply()['packages'], ['a.xml'])

    def test_two_packages_on_one_ram_byte_are_refused_naming_both(self):
        self.package('a.xml', '<memory offset="0x80001000" value="aabbccdd"/>')
        self.package('b.xml', '<memory offset="0x80001003" value="ee"/>')
        self.refused(r"a\.xml and b\.xml both write RAM at 80001003")

    def test_the_same_value_at_the_same_place_is_still_a_conflict(self):
        self.package('a.xml', '<memory offset="0x80001000" value="aa"/>')
        self.package('b.xml', '<memory offset="0x80001000" value="aa"/>')
        self.refused(r"a\.xml and b\.xml both write RAM at 80001000")

    def test_patches_side_by_side_do_not_conflict(self):
        self.package('a.xml', '<memory offset="0x80001000" value="aabbccdd"/>')
        self.package('b.xml', '<memory offset="0x80001004" value="ee"/>'
                              '<memory offset="0x80000fff" value="ff"/>')
        self.assertEqual(self.apply()['ram_patches'], 3)

    def test_a_check_of_another_package_s_bytes_is_not_a_conflict(self):
        self.package('a.xml', '<memory offset="0x80001000" value="aabbccdd"/>')
        self.package('b.xml', '<memory offset="0x80000ffe" value="ff" original="0000aabb"/>')
        self.assertEqual(self.apply()['ram_patches'], 2)

    def test_overlaps_inside_one_package_are_fine(self):
        self.package('a.xml', '<memory offset="0x80001000" value="aabbccdd"/>'
                              '<memory offset="0x80001002" value="11" original="cc"/>')
        self.assertEqual(self.apply()['ram_patches'], 2)

    def test_a_long_patch_does_not_hide_a_conflict_behind_its_own_package_s_short_ones(self):
        long = '<memory offset="0x80002000" value="' + '00' * 100 + '"/>'
        inside = '<memory offset="0x80002010" value="11"/>'
        self.package('a.xml', long + inside)
        self.package('b.xml', '<memory offset="0x80002050" value="22"/>')
        self.refused(r"a\.xml and b\.xml both write RAM at 80002050")

    def test_three_packages_find_the_pair_that_overlaps(self):
        self.package('a.xml', '<memory offset="0x80001000" value="aa"/>')
        self.package('b.xml', '<memory offset="0x80001001" value="bb"/>')
        self.package('c.xml', '<memory offset="0x80001001" value="cc"/>')
        self.refused(r"b\.xml and c\.xml both write RAM at 80001001")


class TestOutputs(SdCase):
    def setUp(self):
        super().setUp()
        self.put('mod/n.bin', b'n' * 5)

    FILE = '<file disc="/Zeta.bin" external="/mod/n.bin"/>'
    MEM = '<memory offset="0x80001000" value="aa"/>'

    def test_a_file_patch_writes_the_overlay_and_a_memory_patch_writes_the_list(self):
        self.package('a.xml', self.FILE)
        self.apply()
        self.assertEqual(self.outputs(), ['disc-overlay.txt', 'fst.bin'])
        self.package('a.xml', self.MEM)
        self.apply()
        self.assertEqual(self.outputs(), ['ram-patches.txt'])
        self.package('a.xml', self.FILE + self.MEM)
        self.apply()
        self.assertEqual(self.outputs(), OUTPUTS)

    def test_outputs_that_do_not_apply_are_removed(self):
        self.package('a.xml', self.MEM)
        self.stale()
        summary = self.apply()
        self.assertEqual(self.outputs(), ['ram-patches.txt'])
        self.assertEqual(summary['overlay'], None)
        self.package('a.xml', self.FILE)
        self.stale()
        self.apply()
        self.assertEqual(self.outputs(), ['disc-overlay.txt', 'fst.bin'])
        self.package('a.xml', self.FILE, default=0)
        self.stale()
        self.assertEqual(self.apply()['packages'], [])
        self.assertEqual(self.outputs(), [])

    def test_nothing_is_made_when_there_is_nothing_to_write(self):
        self.assertEqual(self.apply()['packages'], [])
        self.assertFalse(os.path.exists(self.out))

    def test_out_is_made_with_its_parents(self):
        self.package('a.xml', self.MEM)
        out = os.path.join(self.tmp, 'a', 'b', 'out')
        self.apply(out=out)
        self.assertTrue(os.path.isfile(os.path.join(out, 'ram-patches.txt')))

    def test_the_summary_line_has_what_the_player_needs(self):
        self.package('a.xml', self.FILE + self.MEM
                     + '<file disc="/Brand/new.cfg" external="/mod/n.bin" create="1"/>')
        summary = self.apply()
        growth = summary['overlay'].pop('fst_growth')
        self.assertGreater(growth, 0)
        self.assertEqual(summary, {
            'packages': ['a.xml'], 'files_added': 1, 'files_replaced': 1, 'ram_patches': 1,
            'ram_bytes': 1, 'overlay': {
                'added_files': 1, 'added_bytes': 5, 'replaced_files': 1, 'replaced_bytes': 5,
                'arena_hi_drop': 0}})

    def test_the_manifest_names_the_image_and_reaches_the_files_from_out(self):
        self.package('a.xml', self.FILE)
        self.apply()
        _, base, _, ranges = read_manifest(self.out)
        self.assertEqual(base[0], 'TEST00')
        for _, size, rel in ranges[1:]:
            self.assertTrue(rel.startswith('../mod/'), rel)
            self.assertEqual(os.path.getsize(self.host(rel)), size)

    def test_the_manifest_works_from_a_moved_out_and_a_moved_root(self):
        self.package('a.xml', self.FILE)
        self.apply()
        before = read_manifest(self.out)
        moved = os.path.join(self.tmp, 'moved root')
        os.rename(self.root, moved)
        self.root = moved
        self.out = os.path.join(moved, '.out')
        self.assertEqual(read_manifest(self.out), before)
        self.assertEqual(os.path.getsize(self.host(before[3][1][2])), 5)

    def test_two_runs_give_the_same_bytes(self):
        self.put('mod/dir/a.cfg', b'a')
        self.put('mod/dir/b/c.cfg', b'c' * 3)
        self.put('mod/dir/Zeta.bin', b'z')
        self.package('a.xml', '<folder disc="/" external="/mod/dir" create="true"/>' + self.MEM
                     + '<file disc="/opening.bnr" external="/mod/n.bin"/>')
        self.package('b.xml', '<memory offset="0x80002000" value="bb"/>')
        first = self.apply()
        files = {}
        for name in OUTPUTS:
            with open(os.path.join(self.out, name), 'rb') as f:
                files[name] = f.read()
        self.assertEqual(self.apply(), first)
        other = os.path.join(self.root, '.other')
        self.assertEqual(self.apply(out=other), first)
        for name in OUTPUTS:
            for folder in (self.out, other):
                with open(os.path.join(folder, name), 'rb') as f:
                    self.assertEqual(f.read(), files[name], name)

    def test_a_ciso_gives_the_same_outputs_as_a_plain_image(self):
        self.package('a.xml', self.FILE + self.MEM)
        plain = self.apply()
        want = {}
        for name in OUTPUTS:
            with open(os.path.join(self.out, name), 'rb') as f:
                want[name] = f.read()
        image = self.write(self.tmp, 'game.ciso', ciso(self.disc.iso))
        self.assertEqual(self.apply(image=image), plain)
        for name in OUTPUTS:
            with open(os.path.join(self.out, name), 'rb') as f:
                self.assertEqual(f.read(), want[name], name)

    def test_the_overlay_is_what_disc_overlay_would_make_for_the_same_files(self):
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/n.bin"/>'
                              '<file disc="/Data/new.cfg" external="/mod/n.bin" create="true"/>')
        self.apply()
        plain = os.path.join(self.tmp, 'plain')
        self.write(plain, 'files/Zeta.bin', b'n' * 5)
        self.write(plain, 'files/Data/new.cfg', b'n' * 5)
        with contextlib.redirect_stdout(io.StringIO()):
            ov.main([self.image, plain, '--files', os.path.join(plain, 'files')])
        with open(os.path.join(plain, 'fst.bin'), 'rb') as f, \
                open(os.path.join(self.out, 'fst.bin'), 'rb') as g:
            self.assertEqual(g.read(), f.read())
        want = read_manifest(plain)
        got = read_manifest(self.out)
        self.assertEqual((got[1], got[2]), (want[1], want[2]))
        self.assertEqual([r[:2] for r in got[3]], [r[:2] for r in want[3]])

    def test_a_layout_past_4_gib_is_refused_by_the_overlay(self):
        big = self.put('mod/big.bin')
        os.truncate(big, 3 << 30)
        if os.stat(big).st_blocks * 512 > 1 << 24:
            self.skipTest('this file system has no sparse files')
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/big.bin"/>')
        self.refused(r'the layout would end past 4 GiB, at Zeta\.bin')

    def test_a_path_with_a_control_character_is_refused_on_one_line_naming_the_package(self):
        self.put('mod/two\nlines.bin', b'x')
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/two&#10;lines.bin"/>')
        self.refused(r'^error: <file disc="/Zeta.bin" external="/mod/two\\nlines\.bin"> in a\.xml '
                     r"has the path '/mod/two\\nlines\.bin', which has a part with a control "
                     r"character$")

    def test_a_control_character_in_a_folder_above_the_sd_root_is_refused(self):
        # The manifest reaches the files from OUT, so the folders between them show in its paths.
        self.put('mod/n.bin', b'x')
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/n.bin"/>')
        above = os.path.join(self.tmp, 'new\nline')
        os.mkdir(above)
        moved = os.path.join(above, 'sd')
        os.rename(self.root, moved)
        self.refused(r"^error: a\.xml has the file '\.\./new\\nline/sd/mod/n\.bin', whose path "
                     r"holds a control character$", root=moved, out=os.path.join(self.tmp, 'out'))

    def test_an_image_that_is_not_a_disc_is_refused(self):
        self.package('a.xml', self.MEM)
        self.refused(r"the image's disc ID isn't printable ASCII",
                     image=self.write(self.tmp, 'junk.iso', bytes(0x3000)))
        junk = bytearray(self.disc.iso)
        junk[0x2000:0x2000 + FST_LEN] = bytes(FST_LEN)
        self.refused(r'entry 0 is not a root directory',
                     image=self.write(self.tmp, 'junk.iso', bytes(junk)))


class TestOutputGuards(SdCase):
    """OUT sits in a folder a release can fill, so nothing in it may steer where a write goes."""

    FILE = '<file disc="/Zeta.bin" external="/mod/n.bin"/>'
    MEM = '<memory offset="0x80001000" value="aa"/>'

    def setUp(self):
        super().setUp()
        self.put('mod/n.bin', b'n')
        self.outside = self.write(self.tmp, 'outside/target.txt', b'keep')

    def kept(self):
        with open(self.outside, 'rb') as f:
            self.assertEqual(f.read(), b'keep')

    def fixed(self):
        """Temp names that a package could plan for, so a collision can be made."""
        return mock.patch.object(rv, 'temp_name', lambda name: f'.{name}.fixed.tmp')

    def test_a_link_at_the_old_temp_name_is_not_written_through(self):
        self.package('a.xml', self.MEM)
        os.makedirs(self.out)
        self.link(self.outside, os.path.join(self.out, 'ram-patches.txt.tmp'))
        self.apply()
        self.kept()
        self.assertTrue(os.path.isfile(os.path.join(self.out, 'ram-patches.txt')))

    def test_a_link_at_the_manifest_temp_name_and_a_shell_name_stay_inside_out(self):
        self.put('mod/x;echo PWNED;.bin', b'n')
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/x;echo PWNED;.bin"/>')
        os.makedirs(self.out)
        self.link(self.outside, os.path.join(self.out, 'disc-overlay.txt.tmp'))
        self.apply()
        self.kept()
        with open(os.path.join(self.out, 'disc-overlay.txt'), 'rb') as f:
            self.assertIn(b' ../mod/x;echo PWNED;.bin\n', f.read())

    def test_a_link_at_the_temp_name_stops_the_write_and_is_not_followed(self):
        self.package('a.xml', self.MEM)
        os.makedirs(self.out)
        self.link(self.outside, os.path.join(self.out, '.ram-patches.txt.fixed.tmp'))
        with self.fixed():
            self.refused('File exists', out=self.out)
        self.kept()
        self.assertFalse(os.path.exists(os.path.join(self.out, 'ram-patches.txt')))

    def test_a_file_at_the_temp_name_stops_the_write_and_is_not_overwritten(self):
        self.package('a.xml', self.MEM)
        precious = self.write(self.out, '.ram-patches.txt.fixed.tmp', b'precious')
        with self.fixed():
            self.refused('File exists', out=self.out)
        with open(precious, 'rb') as f:
            self.assertEqual(f.read(), b'precious')
        self.assertFalse(os.path.exists(os.path.join(self.out, 'ram-patches.txt')))

    def test_nothing_is_replaced_when_a_later_temp_file_cannot_be_made(self):
        self.package('a.xml', self.FILE + self.MEM)
        self.stale()
        self.write(self.out, '.disc-overlay.txt.fixed.tmp', b'precious')
        with self.fixed():
            self.refused('File exists', out=self.out)
        for name in OUTPUTS:
            with open(os.path.join(self.out, name), 'rb') as f:
                self.assertEqual(f.read(), b'old', name)
        self.assertEqual(self.outputs(), sorted(OUTPUTS + ['.disc-overlay.txt.fixed.tmp']))

    def test_a_failed_replace_leaves_no_temp_file(self):
        self.package('a.xml', self.MEM)
        with mock.patch.object(rv.os, 'replace', side_effect=OSError('no space')):
            self.refused('no space', out=self.out)
        self.assertEqual(self.outputs(), [])

    def test_temp_names_are_new_each_time(self):
        names = {rv.temp_name('x') for _ in range(20)}
        self.assertEqual(len(names), 20)
        for name in names:
            self.assertRegex(name, r'^\.x\.[0-9a-f]{16}\.tmp$')

    def swap_for_link(self, target):
        """Move OUT aside and leave a link to target where it was, as a release's helper could."""
        os.rename(self.out, self.out + '-real')
        self.link(target, self.out)

    def test_out_swapped_for_a_link_before_it_is_opened_is_refused(self):
        elsewhere = os.path.join(self.tmp, 'elsewhere')
        os.makedirs(elsewhere)
        self.package('a.xml', self.FILE + self.MEM)
        stages = []

        def checkpoint(stage):
            stages.append(stage)
            if stage == 'open':
                self.swap_for_link(elsewhere)

        with mock.patch.object(rv, 'checkpoint', checkpoint):
            self.refused(r"^error: The output folder '.*' is a symbolic link, so it isn't "
                         r"written to$", out=self.out)
        self.assertEqual(stages, ['open'])
        self.assertEqual(os.listdir(elsewhere), [])
        self.assertEqual(os.listdir(self.out + '-real'), [])

    def test_out_swapped_for_a_link_after_it_is_opened_still_gets_every_write(self):
        elsewhere = os.path.join(self.tmp, 'elsewhere')
        os.makedirs(elsewhere)
        self.package('a.xml', self.FILE + self.MEM)

        def checkpoint(stage):
            if stage == 'temps':
                self.swap_for_link(elsewhere)

        with mock.patch.object(rv, 'checkpoint', checkpoint):
            self.assertEqual(self.apply()['packages'], ['a.xml'])
        self.assertEqual(os.listdir(elsewhere), [])
        self.assertEqual(sorted(os.listdir(self.out + '-real')), OUTPUTS)

    def test_out_swapped_for_a_link_between_the_replaces_still_gets_every_write(self):
        elsewhere = os.path.join(self.tmp, 'elsewhere')
        os.makedirs(elsewhere)
        self.package('a.xml', self.FILE + self.MEM)

        def checkpoint(stage):
            if stage == 'replace ram-patches.txt':
                self.swap_for_link(elsewhere)

        with mock.patch.object(rv, 'checkpoint', checkpoint):
            self.apply()
        self.assertEqual(os.listdir(elsewhere), [])
        self.assertEqual(sorted(os.listdir(self.out + '-real')), OUTPUTS)

    def test_a_name_that_becomes_a_folder_before_its_replace_stops_the_run_part_way(self):
        self.package('a.xml', self.FILE + self.MEM)
        self.stale()
        folder = os.path.join(self.out, 'ram-patches.txt')

        def checkpoint(stage):
            if stage == 'replace ram-patches.txt':
                os.unlink(folder)
                os.mkdir(folder)

        with mock.patch.object(rv, 'checkpoint', checkpoint):
            self.refused(r"^error: The output ram-patches\.txt is a folder, so nothing is "
                         r"written there$", out=self.out)
        # fst.bin was replaced first and the manifest goes last, so it is still the old one.
        self.assertEqual(self.outputs(), OUTPUTS)
        with open(os.path.join(self.out, 'fst.bin'), 'rb') as f:
            self.assertNotEqual(f.read(), b'old')
        with open(os.path.join(self.out, 'disc-overlay.txt'), 'rb') as f:
            self.assertEqual(f.read(), b'old')
        os.rmdir(folder)
        self.apply()
        self.assertEqual(self.outputs(), OUTPUTS)
        for name in OUTPUTS:
            with open(os.path.join(self.out, name), 'rb') as f:
                self.assertNotEqual(f.read(), b'old', name)
        self.assertTrue(self.ram().startswith('gcn-ram-patches 1\nbase TEST00\n'))

    def test_out_spelled_with_dots_through_a_link_is_refused(self):
        folder = os.path.join(self.tmp, 'elsewhere')
        os.makedirs(folder)
        self.link(folder, self.out)
        self.package('a.xml', self.FILE + self.MEM)
        for spelling in ('/.', '/../.out', '//', '/./', '/sub/..'):
            with self.subTest(spelling):
                self.refused(r"^error: The output folder '.*' is a symbolic link, so it isn't "
                             r"written to$", out=self.out + spelling)
                self.assertEqual(os.listdir(folder), [])

    def test_out_spelled_with_dots_works_when_it_is_a_folder(self):
        self.package('a.xml', self.MEM)
        self.apply(out=self.out + '/../.out')
        self.assertEqual(self.outputs(), ['ram-patches.txt'])

    def test_out_as_a_symbolic_link_is_refused_and_nothing_is_written(self):
        folder = os.path.join(self.tmp, 'elsewhere')
        os.makedirs(folder)
        dangling = os.path.join(self.tmp, 'dangling')
        self.package('a.xml', self.FILE + self.MEM)
        for target, spelling in ((folder, self.out), (folder, self.out + '/'),
                                 (dangling, self.out)):
            with self.subTest(os.path.basename(target) + spelling[-1]):
                if os.path.lexists(self.out):
                    os.unlink(self.out)
                self.link(target, self.out)
                self.refused(r"^error: The output folder '.*' is a symbolic link, so it isn't "
                             r"written to$", out=spelling)
                self.assertEqual(os.listdir(folder), [])
                self.assertFalse(os.path.exists(dangling))

    def test_an_output_name_that_is_a_link_or_a_folder_is_refused_and_nothing_is_touched(self):
        self.package('a.xml', self.FILE + self.MEM)
        for name in OUTPUTS:
            for kind in ('link to a file', 'link to a folder', 'folder'):
                with self.subTest(name=name, kind=kind):
                    shutil.rmtree(self.out, ignore_errors=True)
                    self.stale()
                    path = os.path.join(self.out, name)
                    os.unlink(path)
                    if kind == 'folder':
                        os.mkdir(path)
                    else:
                        self.link(self.outside if kind == 'link to a file'
                                  else os.path.dirname(self.outside), path)
                    self.refused(rf"The output {re.escape(name)} is a (symbolic link|folder), "
                                 r"so nothing is written", out=self.out)
                    self.kept()
                    self.assertEqual(os.listdir(os.path.dirname(self.outside)), ['target.txt'])
                    for other in OUTPUTS:
                        if other != name:
                            with open(os.path.join(self.out, other), 'rb') as f:
                                self.assertEqual(f.read(), b'old', other)

    def test_a_host_name_with_a_control_character_is_refused_whatever_finds_it(self):
        for char in ('\n', '\r', '\t', '\x01', '\x1f', '\x7f'):
            with self.subTest(repr(char)):
                shutil.rmtree(os.path.join(self.root, 'mod', 'dir'), ignore_errors=True)
                self.put(f'mod/dir/a{char}b.bin', b'x')
                self.package('a.xml', '<folder disc="/" external="/mod/dir" create="true"/>')
                self.refused(r"holds the name 'a.*b\.bin', which has a control character")

    def test_a_new_disc_name_with_a_control_character_is_refused(self):
        for ref in ('&#10;', '&#13;', '&#9;', '&#127;'):
            with self.subTest(ref):
                self.package('a.xml', f'<file disc="/new{ref}line.bin" external="/mod/n.bin" '
                                      'create="true"/>')
                self.refused(r"would add the name '.*' to the disc, and a disc name has to be "
                             r"plain ASCII")


class TestRefusalReport(SdCase):
    """What apply prints on stdout when it exits 1, so a caller can tell which package failed."""

    FILE = '<file disc="/Zeta.bin" external="/mod/n.bin"/>'
    MEM = '<memory offset="0x80001000" value="aa"/>'

    def test_a_refused_package_is_named(self):
        self.package('a b.xml', '<savegame external="x"/>')
        line = self.refusal(r"^error: <savegame external=\"x\"> in a b\.xml isn't supported")
        self.assertEqual(sorted(line), ['error', 'packages'])
        self.assertEqual(line['packages'], ['a b.xml'])
        self.assertEqual(line['error'], "<savegame external=\"x\"> in a b.xml isn't supported, "
                                        "since the engine doesn't redirect saves")

    def test_a_conflict_names_both_packages_whole(self):
        self.put('mod/n.bin')
        self.package('b in c.xml', self.FILE)
        self.package('a and b.xml', self.FILE)
        line = self.refusal(r"a and b\.xml and b in c\.xml both patch the disc file /Zeta\.bin")
        self.assertEqual(line['packages'], ['a and b.xml', 'b in c.xml'])
        self.package('b in c.xml', self.MEM)
        self.package('a and b.xml', self.MEM)
        line = self.refusal(r"both write RAM at 80001000")
        self.assertEqual(line['packages'], ['a and b.xml', 'b in c.xml'])

    def test_an_errored_package_named_by_a_choice_is_named(self):
        self.xml('bad.xml', '<!DOCTYPE wiidisc []><wiidisc version="1"/>')
        line = self.refusal(r'bad\.xml has a DOCTYPE', '--choice', 'bad.xml:opt=1')
        self.assertEqual(line['packages'], ['bad.xml'])

    def test_a_control_character_in_a_host_path_names_its_package(self):
        self.put('mod/two\nlines.bin', b'x')
        self.package('a.xml', '<file disc="/Zeta.bin" external="/mod/two&#10;lines.bin"/>')
        self.assertEqual(self.refusal('control character')['packages'], ['a.xml'])

    def test_a_failure_with_no_package_gives_an_empty_list(self):
        self.package('a.xml', self.MEM)
        line = self.refusal("the image's disc ID isn't printable ASCII",
                            image=self.write(self.tmp, 'junk.iso', bytes(0x3000)))
        self.assertEqual(line['packages'], [])
        self.assertIn("the image's disc ID", line['error'])

    def test_bad_arguments_print_no_json(self):
        self.bad_arguments('names no package', '--choice', 'nope.xml:x=1')
        self.assertEqual(self.printed, '')


class TestLongValues(SdCase):
    """No message repeats a long value, since a line of one goes to a player and a log."""

    def test_a_long_value_is_cut_in_every_message(self):
        long = 'v' * 100000
        self.put('mod/n.bin')
        cases = {
            'number': f'<memory offset="{"9" * 100000}" value="aa"/>',
            'hex': f'<memory offset="0x80001000" value="{"z" * 100000}"/>',
            'path': f'<file disc="/Zeta.bin" external="/{long}"/>',
            'disc path': f'<file disc="/{long}//x" external="/mod/n.bin"/>',
            'valuefile': f'<memory offset="0x80001000" valuefile="/{long}"/>',
            'variable': f'<file disc="/Zeta.bin" external="/{{${long}}}"/>',
            'new name': f'<file disc="/{"é" * 100000}" external="/mod/n.bin" create="true"/>',
        }
        for name, body in cases.items():
            with self.subTest(name):
                self.package('a.xml', body)
                with self.assertRaises(SystemExit) as cm:
                    self.apply()
                self.assertLess(len(str(cm.exception.code)), 600)
                self.assertLess(len(self.printed), 1200)

    def test_a_long_root_is_cut(self):
        self.put('mod/n.bin')
        self.package('a.xml', '<file disc="/Zeta.bin" external="n.bin"/>', root='/' + 'r' * 100000)
        with self.assertRaises(SystemExit) as cm:
            self.apply()
        self.assertLess(len(str(cm.exception.code)), 600)

    def test_a_long_value_in_a_listing_is_cut(self):
        self.xml('a.xml', f'<wiidisc version="1"><options><section name="s">'
                          f'<option name="a" id="{"i" * 100000}" default="{"9" * 100000}"/>'
                          '</section></options></wiidisc>')
        self.xml('b.xml', f'<wiidisc version="1"><options><section name="s">'
                          f'<option name="a" id="{"i" * 100000}"/><option name="b" '
                          f'id="{"i" * 100000}"/></section></options></wiidisc>')
        found = self.listing()
        self.assertEqual([p['xml'] for p in found], ['a.xml', 'b.xml'])
        for package in found:
            self.assertLess(len(package['error']), 400)

    def test_a_long_choice_argument_is_cut(self):
        err = io.StringIO()
        with self.assertRaises(SystemExit) as cm, contextlib.redirect_stderr(err):
            self.apply('--choice', 'x' * 100000 + '=1')
        self.assertEqual(cm.exception.code, 2)
        self.assertLess(len(err.getvalue()), 600)


class TestScript(SdCase):
    def run_script(self, *argv, timeout=None):
        return subprocess.run([sys.executable, '-B', RIIVOLUTION_PY] + list(argv),
                              capture_output=True, text=True, timeout=timeout)

    def test_apply_prints_one_json_line_and_exits_0(self):
        self.package('a.xml', '<memory offset="0x80001000" value="aa"/>')
        done = self.run_script('apply', self.image, self.root, self.out)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(len(done.stdout.splitlines()), 1)
        self.assertEqual(json.loads(done.stdout)['packages'], ['a.xml'])
        self.assertEqual(done.stderr, '')

    def test_list_prints_one_json_line_and_exits_0(self):
        self.package('a.xml', '')
        done = self.run_script('list', self.root)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(len(done.stdout.splitlines()), 1)
        self.assertEqual(json.loads(done.stdout)['packages'][0]['xml'], 'a.xml')

    def test_a_refused_package_exits_1_with_an_error_line_and_a_json_line(self):
        self.package('a.xml', '<savegame external="x"/>')
        done = self.run_script('apply', self.image, self.root, self.out)
        self.assertEqual(done.returncode, 1)
        self.assertRegex(done.stderr, r"^error: <savegame external=\"x\"> in a\.xml isn't "
                                      r"supported.*\n$")
        self.assertEqual(len(done.stdout.splitlines()), 1)
        self.assertEqual(json.loads(done.stdout), {
            'error': done.stderr[len('error: '):].rstrip('\n'), 'packages': ['a.xml']})
        self.assertFalse(os.path.exists(self.out))

    def test_a_megabyte_of_unclosed_variables_is_refused_in_seconds(self):
        # A pattern that rescanned from each {$ would take about 17 minutes on this.
        run = '{$' * 500000
        self.package('a.xml', f'<file disc="/Zeta.bin" external="{run}"/>')
        started = time.monotonic()
        done = self.run_script('apply', self.image, self.root, self.out, timeout=20)
        self.assertLess(time.monotonic() - started, 5.0)
        self.assertEqual(done.returncode, 1, done.stderr[:300])
        self.assertRegex(done.stderr, r"^error: <file .*> in a\.xml has the path '\{\$\{\$")
        self.assertLess(len(done.stderr), 600)

    def test_bad_arguments_exit_2(self):
        for argv in ([], ['apply'], ['apply', self.image, self.root], ['bogus'],
                     ['list', os.path.join(self.tmp, 'none')],
                     ['apply', self.image, self.root, self.out, '--choice', 'nope'],
                     ['apply', self.image, self.root, self.out, '--bogus']):
            with self.subTest(argv):
                done = self.run_script(*argv)
                self.assertEqual(done.returncode, 2, done.stderr)
                self.assertEqual(done.stdout, '')


class TestShaped(SdCase):
    """A package laid out like a release: a root, folders of files and a RAM payload."""

    BLOB = bytes(range(0x10, 0x30))

    def build(self, default=' default="1"'):
        self.put('shaped/replace/zeta.bin', b'z' * 7)
        self.put('shaped/replace/sfx.dat', b's' * 5)
        self.put('shaped/replace/nested/ground.tex', b'g' * 3)
        self.put('shaped/replace/nomatch.cfg', b'q')
        self.put('shaped/new/music/extra.hps', b'm' * 4)
        self.put('shaped/new/top.cfg', b'tt')
        self.put('shaped/blob.bin', self.BLOB)
        self.xml('shaped.xml', '''<wiidisc version="1" root="/shaped">
          <id game="TEST"/>
          <options><section name="Shaped"><option name="Shaped"''' + default + '''>
            <choice name="Enabled"><patch id="all"/></choice></option></section></options>
          <patch id="all">
            <folder external="replace"/>
            <folder disc="/" external="new" create="true"/>
            <memory offset="0x80003100" valuefile="blob.bin"/>
            <memory offset="0x80003104" value="60000000" original="14151617"/>
            <memory offset="0x80003108" value="4e800020" original="18191a1b"/>
            <memory offset="0x80003104" value="38600001" original="60000000"/>
            <memory offset="0x800031f0" value="01020304" original="00000000"/>
          </patch></wiidisc>''')

    def test_the_overlay_and_the_list_come_out_as_the_package_says(self):
        self.build()
        summary = self.apply()
        self.assertEqual(summary['packages'], ['shaped.xml'])
        self.assertEqual((summary['files_added'], summary['files_replaced']), (2, 2))
        self.assertEqual(self.placed(), {
            'Zeta.bin': ('../shaped/replace/zeta.bin', 7),
            'audio/sfx.dat': ('../shaped/replace/sfx.dat', 5),
            'music/extra.hps': ('../shaped/new/music/extra.hps', 4),
            'top.cfg': ('../shaped/new/top.cfg', 2)})
        with open(os.path.join(self.out, 'ram-patches.txt'), 'rb') as f:
            self.assertEqual(f.read().decode('ascii'), (
                'gcn-ram-patches 1\nbase TEST00\n'
                f'patch 80003100 {self.BLOB.hex()}\n'
                'patch 80003104 60000000 14151617\n'
                'patch 80003108 4e800020 18191a1b\n'
                'patch 80003104 38600001 60000000\n'
                'patch 800031f0 01020304 00000000\n'))

    def test_each_original_matches_the_ram_the_patches_before_it_leave(self):
        self.build()
        self.apply()
        ram = bytearray(0x400)  # starts at 80003000
        for line in self.ram().splitlines()[2:]:
            _, addr, value, *original = line.split(' ')
            at = int(addr, 16) - 0x80003000
            if original:
                self.assertEqual(bytes(ram[at:at + len(original[0]) // 2]),
                                 bytes.fromhex(original[0]), line)
            ram[at:at + len(value) // 2] = bytes.fromhex(value)

    def test_the_package_is_off_until_it_is_chosen_when_it_has_no_default(self):
        self.build(default='')
        self.assertEqual(self.apply()['packages'], [])
        self.assertEqual(self.apply('--choice', 'shaped.xml:ShapedShaped=1')['packages'],
                         ['shaped.xml'])


class TestPatcher(unittest.TestCase):
    """The tree walks, on a tree made here."""

    def tree(self):
        root = fst.Entry('', children=[
            fst.Entry('a', children=[fst.Entry('x.bin', 1, 1), fst.Entry('sub', children=[
                fst.Entry('X.bin', 2, 2)])]),
            fst.Entry('b', children=[fst.Entry('x.bin', 3, 3)]),
            fst.Entry('x.bin', 4, 4)])
        return rpatch.from_fst(root)

    def test_find_name_goes_depth_first_in_tree_order(self):
        found = rpatch.find_name(self.tree(), 'X.BIN')
        self.assertEqual(found.parts(), ('a', 'x.bin'))

    def test_find_name_skips_folders_and_misses_a_name_with_a_slash(self):
        tree = self.tree()
        self.assertIsNone(rpatch.find_name(tree, 'sub'))
        self.assertIsNone(rpatch.find_name(tree, 'a/x.bin'))
        self.assertIsNone(rpatch.find_name(tree, ''))

    def test_find_path_matches_each_part_and_checks_kinds(self):
        tree = self.tree()
        self.assertEqual(rpatch.find_path(tree, ['A', 'SUB', 'x.BIN'], None).size, 2)
        self.assertIsNone(rpatch.find_path(tree, ['a'], None))
        self.assertIsNone(rpatch.find_path(tree, ['x.bin', 'y'], None))
        self.assertIsNone(rpatch.find_path(tree, ['a', 'sub', 'none'], None))

    def test_find_path_makes_the_missing_parts_after_they_are_checked(self):
        tree = self.tree()
        seen = []
        node = rpatch.find_path(tree, ['b', 'New', 'deep.cfg'], seen.append)
        self.assertEqual(seen, ['New', 'deep.cfg'])
        self.assertEqual(node.parts(), ('b', 'New', 'deep.cfg'))
        self.assertIsNone(node.children)
        self.assertIs(rpatch.find_path(tree, ['b', 'new', 'DEEP.cfg'], None), node)

    def test_join_disc_follows_the_folder_patch_rule(self):
        for a, b, want in (('', 'x', 'x'), ('/a', '', '/a'), ('/', 'x', '/x'),
                           ('/a/', '/x', '/a/x'), ('/a', 'x', '/a/x'), ('a', 'x', 'a/x'),
                           ('', '', '')):
            self.assertEqual(rpatch.join_disc(a, b), want)


class TestRefusedMessage(unittest.TestCase):
    def test_a_message_names_the_element_and_the_xml(self):
        self.assertEqual(str(rxml.Refused("isn't supported", 'p/a.xml', '<savegame>')),
                         "<savegame> in p/a.xml isn't supported")
        self.assertEqual(str(rxml.Refused('is over 1 MiB', 'p/a.xml')), 'p/a.xml is over 1 MiB')
        self.assertEqual(str(rxml.Refused('a and b clash')), 'a and b clash')


if __name__ == '__main__':
    unittest.main()
