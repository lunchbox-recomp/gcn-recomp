"""Apply the patches of Riivolution packages to a disc's files and to RAM.

A Patcher holds the disc's file tree and the RAM patch list, and takes the packages one at a
time. File and folder patches follow Dolphin's ApplyFilePatchToFST and ApplyFolderPatchToFST
(DiscIO/RiivolutionPatcher.cpp:418-486), and memory patches follow its ApplyMemoryPatch
(:506-557, :618-636). The disc side changes only the tree, so a patched file is a host file and
a size until make_overlay lays it out. See docs/riivolution.md.
"""
import os
import sys
from dataclasses import dataclass
from typing import Callable, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fst  # noqa: E402
from disc_overlay import Item  # noqa: E402
from riivolution_paths import PathError, control, cut, fold, resolve  # noqa: E402
from riivolution_xml import Element, Package, Patch, Refused, VariableError, expand  # noqa: E402

RAM_START = 0x80000000
RAM_END = 0x81800000  # main RAM is 24 MiB
UNSUPPORTED = {
    'savegame': "isn't supported, since the engine doesn't redirect saves",
    'dolphin_sys_file': "isn't supported, since it patches Dolphin's own files",
    'dolphin_sys_folder': "isn't supported, since it patches Dolphin's own files",
}


class Node:
    """A file or folder of the disc as the patches leave it."""
    __slots__ = ('name', 'parent', 'children', 'size', 'host', 'owner')

    def __init__(self, name: str, parent: Optional['Node'], folder: bool, size: int = 0) -> None:
        self.name = name
        self.parent = parent
        self.children: Optional[List['Node']] = [] if folder else None
        self.size = size
        self.host: Optional[str] = None  # the host file that now holds its bytes
        self.owner: Optional[str] = None  # the XML whose patch reached it

    def parts(self) -> Tuple[str, ...]:
        names: List[str] = []
        node: Optional[Node] = self
        while node is not None and node.parent is not None:
            names.append(node.name)
            node = node.parent
        return tuple(reversed(names))


@dataclass
class RamPatch:
    addr: int
    value: bytes
    original: bytes


def from_fst(entry: fst.Entry) -> Node:
    """The tree an FST holds, with each directory's children in the FST's order."""
    root = Node('', None, True)
    todo = [(entry, root)]
    while todo:
        source, node = todo.pop()
        for child in source.children or ():
            made = Node(child.name, node, child.children is not None, child.size)
            assert node.children is not None
            node.children.append(made)
            if child.children is not None:
                todo.append((child, made))
    return root


def find_name(root: Node, name: str) -> Optional[Node]:
    """The first file called name, ignoring case, going through the tree in FST order.

    That's FindFilenameNodeInFST (RiivolutionPatcher.cpp:398-416). A name with a slash in it
    matches nothing, since a file's own name has none.
    """
    key = fold(name)
    assert root.children is not None
    todo = [iter(root.children)]
    while todo:
        for node in todo[-1]:
            if node.children is not None:
                todo.append(iter(node.children))
                break
            if fold(node.name) == key:
                return node
        else:
            todo.pop()
    return None


def find_path(root: Node, parts: List[str],
              create: Optional[Callable[[str], None]]) -> Optional[Node]:
    """The file at a path from the root, ignoring case, or None when nothing is there.

    That's FindFileNodeInFST (RiivolutionPatcher.cpp:360-396). A part that is a file where the
    path wants a folder, or a folder where it wants a file, finds nothing. With create, a missing
    part is made, after create has had a look at its name.
    """
    node = root
    for depth, name in enumerate(parts):
        last = depth + 1 == len(parts)
        assert node.children is not None
        key = fold(name)
        hit = next((c for c in node.children if fold(c.name) == key), None)
        if hit is None:
            if create is None:
                return None
            create(name)
            hit = Node(name, node, not last)
            node.children.append(hit)
        elif (hit.children is None) != last:
            return None
        node = hit
    return node


def join_disc(a: str, b: str) -> str:
    """Two disc paths with one slash between them, as the folder patch builds each file's."""
    if not a or not b:
        return a or b
    return (a[:-1] if a.endswith('/') else a) + '/' + (b[1:] if b.startswith('/') else b)


class Patcher:
    """The disc tree and the RAM patches after the packages given to apply."""

    def __init__(self, sd_root: str, game_id: str, disc: fst.Entry) -> None:
        self.sd_root = sd_root
        self.game_id = game_id
        self.root = from_fst(disc)
        self.ram: List[RamPatch] = []
        self.writes: List[Tuple[int, int, str]] = []  # each RAM patch's span and its XML
        self.xml = ''
        self.folder = ''
        self.patch: Optional[Patch] = None
        self.root_dir: Optional[str] = None

    def apply(self, package: Package, patches: List[Patch], folder: str) -> None:
        """Apply the patches a package's chosen choices turned on, in their order.

        folder is the host folder the XML sits in. In a patch, files go first, then folders,
        then memory, as Dolphin does (RiivolutionPatcher.cpp:488-504).
        """
        self.xml = package.xml
        self.folder = folder
        for patch in patches:
            self.patch = patch
            self.root_dir = None
            for part in patch.parts:
                if part.tag in UNSUPPORTED:
                    raise part.refuse(UNSUPPORTED[part.tag])
            for tag, one in (('file', self._file), ('folder', self._folder),
                             ('memory', self._memory)):
                for part in patch.parts:
                    if part.tag == tag:
                        one(part)

    def items(self, out_dir: str) -> List[Item]:
        """An overlay item for each disc file that a patch reached, with its path from out_dir."""
        items: List[Item] = []
        todo = [self.root]
        while todo:
            node = todo.pop()
            if node.children is not None:
                todo.extend(reversed(node.children))
            elif node.host is not None:
                rel = os.path.relpath(node.host, out_dir).replace(os.sep, '/')
                if control(rel):
                    raise Refused(f'has the file {cut(rel)!r}, whose path holds a control '
                                  'character', node.owner or '')
                items.append(Item(rel, node.size, node.parts()))
        return items

    def ram_patches(self) -> List[RamPatch]:
        """The RAM patches in order, after a check that no two packages write the same byte."""
        spans = []
        for xml in sorted({x for _, _, x in self.writes}):
            mine = sorted((s, e) for s, e, x in self.writes if x == xml)
            joined = [mine[0]]
            for start, end in mine[1:]:
                if start < joined[-1][1]:
                    joined[-1] = (joined[-1][0], max(joined[-1][1], end))
                else:
                    joined.append((start, end))
            spans += [(s, e, xml) for s, e in joined]
        spans.sort()
        for before, after in zip(spans, spans[1:]):
            if after[0] < before[1]:
                first, second = sorted((before[2], after[2]))
                raise Refused(f'{first} and {second} both write RAM at {after[0]:08x}',
                              packages=[first, second])
        return self.ram

    # Text and paths

    def _expanded(self, el: Element, text: str) -> str:
        try:
            return expand(text, self.game_id)
        except VariableError as e:
            raise el.refuse(str(e)) from None

    def _text(self, el: Element, name: str) -> str:
        return self._expanded(el, el.text(name))

    def _root_dir(self) -> str:
        """The folder a path with no leading slash starts from, which the patch's root names."""
        if self.root_dir is None:
            assert self.patch is not None
            element = self.patch.element
            text = self._expanded(element, self.patch.root)
            try:
                path = resolve(self.sd_root, self.folder, text) if text else self.folder
            except PathError as e:
                raise element.refuse(f'has the root {cut(text)!r}, which {e}') from None
            if not os.path.isdir(path):
                raise element.refuse(f"has the root {cut(text)!r}, which isn't a folder")
            self.root_dir = path
        return self.root_dir

    def _host(self, el: Element, text: str) -> str:
        here = self.sd_root if text.startswith('/') else self._root_dir()
        try:
            return resolve(self.sd_root, here, text)
        except PathError as e:
            raise el.refuse(f'has the path {cut(text)!r}, which {e}') from None

    def _shown(self, host: str) -> str:
        return cut(os.path.relpath(host, self.sd_root))

    # Files and folders

    def _file(self, el: Element) -> None:
        disc = self._text(el, 'disc')
        external = self._text(el, 'external')
        if el.number('offset', 0) & ~3:  # Dolphin drops the low two bits too
            raise el.refuse("has an offset, which isn't supported")
        if el.number('fileoffset', 0):
            raise el.refuse("has a fileoffset, which isn't supported")
        if not disc:
            raise el.refuse('has no disc path')
        host = self._host(el, external)
        if not os.path.isfile(host):
            raise el.refuse(f"has the path {cut(external)!r}, which isn't a file")
        self._put(el, disc, host, el.flag('resize', True), el.flag('create', False),
                  el.number('length', 0))

    def _folder(self, el: Element) -> None:
        external = self._text(el, 'external')
        host = self._host(el, external)
        if not os.path.isdir(host):
            raise el.refuse(f"has the path {cut(external)!r}, which isn't a folder")
        self._walk(el, self._text(el, 'disc'), host, el.flag('resize', True),
                   el.flag('create', False), el.flag('recursive', True), el.number('length', 0))

    def _walk(self, el: Element, disc: str, host_dir: str, resize: bool, create: bool,
              recursive: bool, length: int) -> None:
        """Put each file of a host folder where the folder patch says, in name order.

        Dotfiles are skipped. A disc path with no leading slash matches a file by its name
        alone, so nothing below the top folder can match and the walk doesn't go there.
        """
        with os.scandir(host_dir) as it:
            entries = sorted(it, key=lambda e: e.name)
        for entry in entries:
            if entry.name.startswith('.'):
                continue
            if control(entry.name):
                raise el.refuse(f"holds the name {cut(entry.name)!r}, which has a control "
                                'character')
            here = join_disc(disc, entry.name)
            if entry.is_symlink():
                raise el.refuse(f"holds the symbolic link {self._shown(entry.path)!r}, "
                                "which isn't read")
            if entry.is_dir(follow_symlinks=False):
                if recursive and here.startswith('/'):
                    self._walk(el, here, entry.path, resize, create, recursive, length)
            elif entry.is_file(follow_symlinks=False):
                self._put(el, here, entry.path, resize, create, length)
            else:
                raise el.refuse(f"holds {self._shown(entry.path)!r}, "
                                "which isn't a file or a folder")

    def _put(self, el: Element, disc: str, host: str, resize: bool, create: bool,
             length: int) -> None:
        """Make a host file the contents of the disc file a disc path names, if there is one.

        The new size is length bytes of the host file, or all of it when length is 0. A patch
        that leaves the old file's tail in place, which resize="false" does when it ends
        before the old file does, would need that tail from the disc, and the overlay has none.
        """
        node = self._locate(el, disc, create)
        if node is None:
            return
        have = os.stat(host).st_size
        if length > have:
            raise el.refuse(f'has length {length}, which is past the end of '
                            f'{self._shown(host)!r} (size {have})')
        size = length or have
        if not resize and node.size > size:
            raise el.refuse(f'has resize="false", and {self._shown(host)!r} (size {size}) is '
                            f"smaller than {cut(disc)!r} (size {node.size}), which isn't "
                            'supported')
        if node.owner not in (None, self.xml):
            assert node.owner is not None
            first, second = sorted((node.owner, self.xml))
            where = '/'.join(node.parts())
            raise Refused(f'{first} and {second} both patch the disc file /{where}',
                          packages=[first, second])
        node.owner = self.xml
        node.host = host
        node.size = size

    def _locate(self, el: Element, disc: str, create: bool) -> Optional[Node]:
        if disc.startswith('/'):
            parts = disc[1:].split('/')
            if any(p in ('', '.', '..') for p in parts):
                raise el.refuse(f'has the disc path {cut(disc)!r}, which has an empty or dot '
                                'part')
            return find_path(self.root, parts, (lambda name: self._new_name(el, name))
                             if create else None)
        if fold(disc) == 'main.dol':
            raise el.refuse("patches main.dol, which isn't supported")
        return find_name(self.root, disc)

    def _new_name(self, el: Element, name: str) -> None:
        if not name.isascii() or control(name):
            raise el.refuse(f"would add the name {cut(name)!r} to the disc, and a disc name has "
                            'to be plain ASCII')

    # Memory

    def _memory(self, el: Element) -> None:
        for flag in ('search', 'ocarina'):
            if el.flag(flag, False):
                raise el.refuse(f"uses {flag}, which isn't supported")
        offset = el.number('offset', 0)
        if not offset:
            raise el.refuse('has no offset')
        addr = offset | RAM_START  # Dolphin does the same (RiivolutionPatcher.cpp:555)
        value = el.hexbytes('value')
        blob = self._text(el, 'valuefile')
        if blob and value is not None:
            raise el.refuse('has both value and valuefile')
        if blob:
            value = self._blob(el, blob)
        if not value:
            raise el.refuse('has no value')
        original = el.hexbytes('original') or b''
        end = addr + max(len(value), len(original))
        if end > RAM_END:
            raise el.refuse(f'covers {addr:08x} to {end - 1:08x}, which isn\'t inside main RAM '
                            f'({RAM_START:08x} to {RAM_END - 1:08x})')
        self.ram.append(RamPatch(addr, value, original))
        self.writes.append((addr, addr + len(value), self.xml))

    def _blob(self, el: Element, name: str) -> bytes:
        host = self._host(el, name)
        if not os.path.isfile(host):
            raise el.refuse(f"has the path {cut(name)!r}, which isn't a file")
        if os.stat(host).st_size > RAM_END - RAM_START:
            raise el.refuse(f'has a valuefile, {cut(name)!r}, that is bigger than main RAM')
        with open(host, 'rb') as f:
            return f.read()
