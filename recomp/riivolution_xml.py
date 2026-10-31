"""Read one Riivolution XML: the discs it is for, its options and its patches.

A package is a `wiidisc` XML. Its `id` says which discs it fits, its `options` offer choices and a
choice turns patches on. The rules are Dolphin's GameCube path (DiscIO/RiivolutionParser.cpp).
What the reader can't honor is refused by name. A file is refused before it is parsed when it is
over 1 MiB, isn't UTF-8, holds a NUL or another control character or holds a DOCTYPE or an
entity, and the parser itself refuses a DOCTYPE or an entity that gets past that. See
docs/riivolution.md.
"""
import os
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Sequence
from xml.parsers import expat

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from riivolution_paths import cut  # noqa: E402

MAX_XML = 1 << 20
MAX_U32 = 0xFFFFFFFF
MAX_DIGITS = 10  # after any 0x, which fits every 32 bit number
DTD = re.compile(r'<!\s*(DOCTYPE|ENTITY)', re.IGNORECASE)
# NUL and the C0 controls but tab, CR and LF. A NUL is how UTF-16 and UTF-32 text shows in UTF-8.
CONTROL = re.compile('[\x00-\x08\x0b\x0c\x0e-\x1f]')
NUMBER = re.compile(r'\s*([+-]?)(0[xX][0-9A-Fa-f]+|[0-9]+)\s*')
HEX = re.compile(r'(0[xX])?((?:[0-9A-Fa-f]{2})*)')
# A name stops at the next { or $ or }, so a run of unclosed {$ is read once and not rescanned.
VARIABLE = re.compile(r'\{\$([^{}$]*)\}')
LABELLED = ('id', 'name', 'disc', 'external', 'offset', 'valuefile')
# The elements of a patch that the reader looks at. Anything else in a patch is ignored.
PATCH_PARTS = ('file', 'folder', 'memory', 'savegame', 'dolphin_sys_file', 'dolphin_sys_folder')


class Refused(ValueError):
    """A package, or part of one, that the reader won't use.

    The message names the XML and the element, as `<file disc="/a.bin"> in pkg/a.xml has ...`.
    packages holds the XML paths at fault, which are the XML or the ones the reason names.
    """

    def __init__(self, reason: str, xml: str = '', element: str = '',
                 packages: Sequence[str] = ()) -> None:
        super().__init__(reason)
        self.reason = reason
        self.xml = xml
        self.element = element
        self.packages = list(packages) or ([xml] if xml else [])

    def __str__(self) -> str:
        if self.element:
            return f'{self.element} in {self.xml} {self.reason}'
        return f'{self.xml} {self.reason}' if self.xml else self.reason


class VariableError(ValueError):
    """A `{$name}` the reader has no value for."""


class Element:
    """An XML element with typed attributes, and the label a message uses for it."""

    def __init__(self, node: ET.Element, xml: str) -> None:
        self.node = node
        self.xml = xml
        self.tag = node.tag

    def label(self) -> str:
        shown = ''
        for key in LABELLED:
            value = self.node.get(key)
            if value is not None:
                shown += f' {key}="{repr(cut(value))[1:-1]}"'  # escapes a newline in a value
        return f'<{self.tag}{shown}>'

    def refuse(self, reason: str) -> Refused:
        return Refused(reason, self.xml, self.label())

    def text(self, name: str, default: str = '') -> str:
        return self.node.get(name, default)

    def number(self, name: str, default: int, signed: bool = False) -> int:
        """A decimal or 0x number, as a missing attribute's default when it isn't there."""
        value = self.node.get(name)
        if value is None:
            return default
        found = NUMBER.fullmatch(value)
        if not found or (found.group(1) == '-' and not signed):
            raise self.refuse(f"has {name}={cut(value)!r}, which isn't a number")
        digits = found.group(2)
        hexed = digits[:2] in ('0x', '0X')
        if len(digits) - 2 * hexed > MAX_DIGITS:
            raise self.refuse(f"has {name}={cut(value)!r}, which has more than {MAX_DIGITS} "
                              "digits")
        try:  # a leading zero is still decimal, as pugixml reads it
            number = int(digits, 16) if hexed else int(digits, 10)
        except ValueError:
            raise self.refuse(f"has {name}={cut(value)!r}, which isn't a number") from None
        number = -number if found.group(1) == '-' else number
        if number > MAX_U32 or number < -(1 << 31):
            raise self.refuse(f"has {name}={cut(value)!r}, which doesn't fit in 32 bits")
        return number

    def flag(self, name: str, default: bool) -> bool:
        """A yes or no the way Riivolution reads one: true when it starts with 1, t or y."""
        value = self.node.get(name)
        return default if value is None else value[:1] in ('1', 't', 'T', 'y', 'Y')

    def hexbytes(self, name: str) -> Optional[bytes]:
        """Whole hex bytes with an optional 0x, or None when the attribute isn't there."""
        value = self.node.get(name)
        if value is None:
            return None
        found = HEX.fullmatch(value)
        if not found:
            raise self.refuse(f"has {name}={cut(value)!r}, which isn't whole hex bytes")
        return bytes.fromhex(found.group(2))

    def children(self, tag: str) -> List['Element']:
        return [Element(c, self.xml) for c in self.node if c.tag == tag]


@dataclass
class GameFilter:
    """Which discs a package is for. A filter that is None doesn't look at that part."""
    game: Optional[str] = None
    developer: Optional[str] = None
    disc: Optional[int] = None
    version: Optional[int] = None
    regions: Optional[List[str]] = None

    def allows(self, game_id: str, revision: int, disc_number: int) -> bool:
        """Dolphin's IsValidForGame (RiivolutionParser.cpp:235-259), for a six character ID."""
        if len(game_id) != 6:
            return False
        if self.game is not None and not game_id.startswith(self.game):
            return False
        if self.developer is not None and game_id[4:6] != self.developer:
            return False
        if self.disc is not None and disc_number != self.disc:
            return False
        if self.version is not None and revision != self.version:
            return False
        return self.regions is None or game_id[3] in self.regions


@dataclass
class Choice:
    name: str
    refs: List[Element]


@dataclass
class Option:
    """One option. key is its id, or its section's name and its own when it has no id."""
    key: str
    name: str
    section: str
    default: int
    choices: List[Choice]


@dataclass
class Patch:
    id: str
    root: str
    element: Element
    parts: List[Element] = field(default_factory=list)


@dataclass
class Package:
    xml: str
    game_filter: GameFilter
    options: List[Option]
    patches: Dict[str, Patch]

    def active(self, picks: Dict[str, int]) -> List[Patch]:
        """The patches the chosen choices turn on, in option order, each one once.

        A pick is a choice number from 1, or 0 for none, and an option with no pick uses its
        default. Dolphin walks the options the same way (RiivolutionParser.cpp:295-343).
        """
        out: List[Patch] = []
        seen = set()
        for option in self.options:
            pick = picks.get(option.key, option.default)
            if not 0 < pick <= len(option.choices):
                continue
            for ref in option.choices[pick - 1].refs:
                patch = self.patches.get(ref.text('id'))
                if patch is None:
                    raise ref.refuse("names a patch the XML doesn't define")
                if patch.id not in seen:
                    seen.add(patch.id)
                    out.append(patch)
        return out


def expand(text: str, game_id: str) -> str:
    """Put the game's ID parts in for {$__gameid}, {$__region} and {$__maker}.

    Dolphin fills the same three (RiivolutionParser.cpp:311-316). Any other variable would come
    from a param, which the reader refuses, so it has no value and is refused here.
    """
    values = {'__gameid': game_id[:3], '__region': game_id[3:4], '__maker': game_id[4:6]}

    def swap(found: 're.Match[str]') -> str:
        if found.group(1) not in values:
            raise VariableError(f"uses {cut(found.group(0))}, which isn't supported")
        return values[found.group(1)]

    return VARIABLE.sub(swap, text)


def _no_dtd(xml: str) -> Callable[..., None]:
    def refuse(*_args: object) -> None:
        raise Refused('has a DOCTYPE or an entity, and neither is read', xml)
    return refuse


def parse_text(text: str, xml: str) -> ET.Element:
    """The root element of XML text, with the parser itself refusing a DOCTYPE or an entity.

    The byte scan in read_root should have stopped one already. This is the second wall, for
    a declaration that the scan couldn't see. Names are read as written, with no namespaces.
    """
    builder = ET.TreeBuilder()
    parser = expat.ParserCreate()
    refuse = _no_dtd(xml)
    parser.StartDoctypeDeclHandler = refuse
    parser.EntityDeclHandler = refuse
    parser.UnparsedEntityDeclHandler = refuse
    parser.ExternalEntityRefHandler = refuse
    parser.StartElementHandler = builder.start
    parser.EndElementHandler = builder.end
    try:
        parser.Parse(text, True)
    except expat.ExpatError as e:
        raise Refused(f"isn't well-formed XML ({e})", xml) from None
    return builder.close()


def read_root(path: str, xml: str) -> ET.Element:
    """The root element of an XML file, after the checks that come before parsing."""
    try:
        with open(path, 'rb') as f:
            data = f.read(MAX_XML + 1)
    except OSError as e:
        raise Refused(f"can't be read ({e.strerror})", xml) from None
    if len(data) > MAX_XML:
        raise Refused("is over 1 MiB, so it isn't read", xml)
    try:
        text = data.decode('utf-8-sig')
    except UnicodeDecodeError:
        raise Refused("isn't UTF-8", xml) from None
    if CONTROL.search(text):
        raise Refused("has a NUL or another control character, so it isn't read", xml)
    if DTD.search(text):
        raise Refused('has a DOCTYPE or an entity, and neither is read', xml)
    return parse_text(text, xml)


def read_package(path: str, xml: str) -> Optional[Package]:
    """The package in an XML file, or None when it isn't a wiidisc file at all."""
    return parse(read_root(path, xml), xml)


def parse(root: ET.Element, xml: str) -> Optional[Package]:
    if root.tag != 'wiidisc':
        return None
    head = Element(root, xml)
    if head.number('version', -1, signed=True) != 1:
        raise head.refuse("isn't version 1, and only version 1 is read")
    return Package(xml, _game_filter(root, xml), _options(root, xml), _patches(root, head, xml))


def _game_filter(root: ET.Element, xml: str) -> GameFilter:
    found = GameFilter()
    node = root.find('id')
    if node is None:
        return found
    ident = Element(node, xml)
    found.game = node.get('game')
    found.developer = node.get('developer')
    if node.get('disc') is not None:
        found.disc = ident.number('disc', -1, signed=True)
    if node.get('version') is not None:
        found.version = ident.number('version', -1, signed=True)
    regions = ident.children('region')
    if regions:
        found.regions = [r.text('type') for r in regions]
    return found


def _no_params(node: ET.Element, xml: str) -> None:
    for child in node:
        if child.tag == 'param':
            raise Element(child, xml).refuse("isn't supported, since the reader fills no params")


def _options(root: ET.Element, xml: str) -> List[Option]:
    block = root.find('options')
    if block is None:
        return []
    for macro in block.findall('macros'):
        raise Element(macro, xml).refuse("isn't supported, since the reader fills no params")
    options: List[Option] = []
    keys = set()
    for section in block.findall('section'):
        section_name = section.get('name', '')
        for node in section.findall('option'):
            option = Element(node, xml)
            _no_params(node, xml)
            key = option.text('id') or section_name + option.text('name')
            if key in keys:
                raise option.refuse(f'repeats the option id {cut(key)!r}')
            keys.add(key)
            choices = []
            for choice in node.findall('choice'):
                _no_params(choice, xml)
                for ref in choice.findall('patch'):
                    _no_params(ref, xml)
                refs = Element(choice, xml).children('patch')
                choices.append(Choice(choice.get('name', ''), refs))
            options.append(Option(key, option.text('name'), section_name,
                                  option.number('default', 0), choices))
    return options


def _patches(root: ET.Element, head: Element, xml: str) -> Dict[str, Patch]:
    patches: Dict[str, Patch] = {}
    for node in root.findall('patch'):
        element = Element(node, xml)
        patch_id = element.text('id')
        if patch_id not in patches:  # the first patch with an id is the one a choice reaches
            parts = [Element(c, xml) for c in node if c.tag in PATCH_PARTS]
            patches[patch_id] = Patch(patch_id, element.text('root') or head.text('root'),
                                      element, parts)
    return patches
