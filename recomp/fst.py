#!/usr/bin/env python3
"""Read and write a GameCube disc's FST (its table of files), and list or extract from it.

The disc header holds the FST's offset (0x424), size (0x428) and max size (0x42C). The FST is
a table of 12-byte entries and then a name table.

Entry 0 is the root directory, and every other entry follows the directory it's in. An entry
starts with a type byte (1 for a directory) and a 24-bit offset into the name table. A file
then has its byte offset on the disc and its size.

A directory has its parent's index and the index of the first entry after it. The name table
holds each name and a NUL byte, in entry order. The root has no name.

parse reads the bytes into a tree of Entry and build writes a tree back. build puts each
directory's names in sort_key order, so a name added to a directory goes in its place. A name
has to be ASCII and work as one part of a path, so a slash, '.', '..' and an empty name are
refused.

Usage:
  fst.py list <image>
      print `offset size path` for each file in FST order, the offset in hex and the size in
      decimal
  fst.py extract <image> <outdir> [<path>...]
      copy the named files out of the disc, or every file when none is named. outdir has to be
      empty or not exist yet

The image is a plain image or a CISO.
"""
import argparse
import os
import struct
import sys
from dataclasses import dataclass
from typing import Iterator, List, Optional, Sequence, Tuple

sys.path.insert(0, os.path.dirname(__file__))
from dol import DiscImage  # noqa: E402

CHUNK = 1 << 20
MAX_U32 = 0xFFFFFFFF


class FstError(ValueError):
    """The FST can't be read or written. The message names the entry."""


@dataclass
class Entry:
    """A file has an offset and a size. A directory has children, and the root's name is ''."""
    name: str
    offset: int = 0
    size: int = 0
    children: Optional[List['Entry']] = None


def sort_key(name: str) -> Tuple[str, str]:
    """How build orders the names in a directory, as Dolphin's FST writer and retail discs do.

    Names compare in uppercase, so '_' sorts after the letters. Names that match that way
    compare as they're spelled.
    """
    return name.upper(), name


def _check_name(where: str, name: str) -> None:
    if not name.isascii():
        raise FstError(f"{where} ({name!r}) has a name that isn't ASCII")
    if not name or name in ('.', '..') or '/' in name or '\0' in name:
        raise FstError(f"{where} ({name!r}) has a name that can't be a path part")


def _read_name(data: bytes, table: int, index: int, offset: int) -> str:
    start = table + offset
    if start >= len(data):
        raise FstError(f'entry {index} has its name at {offset:#x}, past the name table')
    end = data.find(b'\0', start)
    if end < 0:
        raise FstError(f"entry {index} has a name that's cut off")
    name = data[start:end].decode('latin-1')
    _check_name(f'entry {index}', name)
    return name


def parse(data: bytes) -> Entry:
    """The tree that FST bytes hold. A directory's offset and size are 0."""
    if len(data) < 12:
        raise FstError('entry 0 is cut off')
    count = struct.unpack_from('>I', data, 8)[0]
    if data[0] != 1 or count < 1:
        raise FstError('entry 0 is not a root directory')
    table = count * 12
    if len(data) < table:
        raise FstError(f'entry {len(data) // 12} is cut off')
    root = Entry('', children=[])
    # each directory still being read, with where its entries end and the names it holds so far
    open_dirs = [(root, count, set())]
    for i in range(1, count):
        while i >= open_dirs[-1][1]:
            open_dirs.pop()
        parent, end, names = open_dirs[-1]
        word, a, b = struct.unpack_from('>III', data, i * 12)
        name = _read_name(data, table, i, word & 0xFFFFFF)
        kind = word >> 24
        if kind > 1:
            raise FstError(f'entry {i} ({name!r}) has type {kind}')
        if name in names:
            raise FstError(f'entry {i} ({name!r}) repeats a name in its directory')
        names.add(name)
        if kind:
            if not i < b <= end:
                raise FstError(f'entry {i} ({name!r}) ends at entry {b}, outside its directory')
            entry = Entry(name, children=[])
            parent.children.append(entry)
            open_dirs.append((entry, b, set()))
        else:
            parent.children.append(Entry(name, a, b))
    return root


def build(root: Entry) -> bytes:
    """FST bytes for a tree, with each directory's children in sort_key order."""
    nodes = []  # (entry, parent's index) in FST order
    todo = [(root, 0)]
    while todo:
        entry, parent = todo.pop()
        nodes.append((entry, parent))
        if entry.children is not None:
            ordered = sorted(entry.children, key=lambda c: sort_key(c.name))
            todo.extend((c, len(nodes) - 1) for c in reversed(ordered))
    span = [1] * len(nodes)  # each entry and everything below it
    for i in range(len(nodes) - 1, 0, -1):
        span[nodes[i][1]] += span[i]
    table = bytearray()
    names = bytearray()
    for i, (entry, parent) in enumerate(nodes):
        word = 0
        if i:
            _check_name(f'entry {i}', entry.name)
            word = len(names)
            names += entry.name.encode('ascii') + b'\0'
        if entry.children is not None:
            table += struct.pack('>III', 1 << 24 | word, parent, i + span[i])
        elif 0 <= entry.offset <= MAX_U32 and 0 <= entry.size <= MAX_U32:
            table += struct.pack('>III', word, entry.offset, entry.size)
        else:
            raise FstError(f'entry {i} ({entry.name!r}) has an offset or size that '
                           "isn't 32 bits")
    return bytes(table + names)


def read(img: DiscImage) -> Tuple[int, int, int, Entry]:
    """The FST words in the header of an open image (offset, size, max), then its tree."""
    head = img.read(0x424, 12)
    if len(head) < 12:
        raise FstError("the image ends before its header's FST words")
    offset, size, max_size = struct.unpack('>3I', head)
    return offset, size, max_size, parse(img.read(offset, size))


def walk(root: Entry) -> Iterator[Tuple[str, Entry]]:
    """Every file under root as (path, entry), in the order the tree holds them."""
    todo = [('', root)]
    while todo:
        path, entry = todo.pop()
        if entry.children is None:
            yield path, entry
        else:
            todo.extend((f'{path}/{c.name}' if path else c.name, c)
                        for c in reversed(entry.children))


def _check_tree(root: Entry) -> None:
    """Refuse a tree that can't be written under a directory, whether or not parse made it.

    That means a name that can't be a path part, or two names in one directory that differ only
    by case, which the disk would treat as one.
    """
    todo = [('', root)]
    while todo:
        path, entry = todo.pop()
        here = repr(path) if path else 'the root'
        spelled = {}  # each lowercase name to its first spelling in this directory
        for child in entry.children or ():
            _check_name(f'an entry in {here}', child.name)
            key = child.name.lower()
            if key in spelled:
                raise FstError(f'{here} has {spelled[key]!r} and {child.name!r}, '
                               'which the disk would treat as one name')
            spelled[key] = child.name
            if child.children is not None:
                todo.append((f'{path}/{child.name}' if path else child.name, child))


def extract(img: DiscImage, root: Entry, out: str, paths: Sequence[str] = ()) -> None:
    """Copy the named files, or every file, into out.

    Nothing is written if a name in the tree can't be a path part or a path is unknown.
    """
    _check_tree(root)
    files = dict(walk(root))
    wanted = list(dict.fromkeys(paths)) or list(files)
    for path in wanted:
        if path not in files:
            raise FstError(f'{path} is not a file on the disc')
    if os.path.exists(out) and (not os.path.isdir(out) or os.listdir(out)):
        raise FstError(f'{out} has to be an empty directory')
    for path in wanted:
        entry = files[path]
        dest = os.path.join(out, *path.split('/'))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, 'xb') as f:
            for pos in range(0, entry.size, CHUNK):
                n = min(CHUNK, entry.size - pos)
                data = img.read(entry.offset + pos, n)
                if len(data) != n:
                    raise FstError(f'the image ends inside {path}')
                f.write(data)


def main(argv: Optional[Sequence[str]] = None) -> None:
    parser = argparse.ArgumentParser(description='Read the files on a GameCube disc image.')
    commands = parser.add_subparsers(dest='command', required=True)
    lister = commands.add_parser('list', help='print offset, size and path for each file')
    lister.add_argument('image')
    extractor = commands.add_parser('extract', help='copy files out of the disc')
    extractor.add_argument('image')
    extractor.add_argument('outdir')
    extractor.add_argument('paths', nargs='*', metavar='path')
    args = parser.parse_args(argv)
    try:
        img = DiscImage(args.image)
        try:
            root = read(img)[3]
            if args.command == 'list':
                for path, entry in walk(root):
                    print(f'{entry.offset:#010x} {entry.size:>10} {path}')
            else:
                extract(img, root, args.outdir, args.paths)
        finally:
            img.f.close()
    except (FstError, OSError) as e:
        sys.exit(f'error: {e}')


if __name__ == '__main__':
    main()
