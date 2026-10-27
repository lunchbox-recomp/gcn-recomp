#!/usr/bin/env python3
"""Build a disc overlay: loose files that stand in for files on a GameCube disc image.

An overlay is a manifest that maps byte ranges of a virtual disc onto host files, and a rewritten
FST that points the disc's files at those ranges. The image itself is never touched. See
docs/disc-overlay.md for the manifest and the layout.

Each file under the files folder replaces the disc file at the same path, matched ignoring case,
or is added under the folder's spelling. A replaced file keeps the disc's spelling. Untouched
files keep their offsets and read from the image.

Replaced and added files, the new FST and the DOL (when there is one) go past the end of the
image. They start at the first 32 KiB boundary at or past max(0x57058000, the image's size),
which is the size of a GameCube disc. Each one starts on its own 32 KiB boundary: the FST, then
the DOL, then the files in the order the FST lists them. Nothing may end past 4 GiB, since the
FST's offsets are 32 bits.

The header word at 0x424 always changes, to the new FST's offset. Words 0x428 and 0x42C change
only when the FST grows: 0x428 becomes the new size and 0x42C the larger of the old max and the
new size. Word 0x420 changes with --dol. A smaller table is padded with zeros to the old size, so
fst.bin is always as long as the header says.

Usage:
  disc_overlay.py <image> <out> --files <dir> [--dol <file>]
      write <out>/disc-overlay.txt and <out>/fst.bin, then print one JSON line with the counts
      and bytes of added and replaced files, the FST's growth and how far arena hi drops. The
      dir and the file have to be inside out, so the manifest's paths are relative and out can
      move. Dotfiles are skipped, and symbolic links, names that aren't ASCII and layouts past
      4 GiB are refused. The same inputs give the same bytes.

The image is a plain image or a CISO.
"""
import argparse
import json
import os
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

sys.path.insert(0, os.path.dirname(__file__))
import fst  # noqa: E402
from dol import DiscImage  # noqa: E402

ALIGN = 0x8000
MIN_START = 0x57058000  # 1,459,978,240 bytes, the size of a GameCube disc
LIMIT = 1 << 32
ARENA_TOP = 0x817FE000  # where the game's BI2 sits, and the FST below it
MIN_DOL = 0x100  # the header that boot reads
MANIFEST = 'disc-overlay.txt'
FST_FILE = 'fst.bin'
FNV_BASIS = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


class OverlayError(ValueError):
    """The overlay can't be built. The message names the file or word at fault."""


@dataclass
class Item:
    """A host file that goes on the virtual disc, or the DOL.

    rel is its path from the manifest. parts are its names under the files folder, and disc is
    the same path as the disc spells it once the file is merged into the tree.
    """
    rel: str
    size: int
    parts: Tuple[str, ...] = ()
    disc: Tuple[str, ...] = ()
    replaced: bool = False
    entry: Optional[fst.Entry] = field(default=None, repr=False)


@dataclass
class Overlay:
    manifest: bytes
    fst: bytes
    summary: Dict[str, int]


def fnv1a64(data: bytes) -> int:
    h = FNV_BASIS
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def align_up(n: int) -> int:
    return -(-n // ALIGN) * ALIGN


def arena_hi(fst_max: int) -> int:
    """Where the boot code puts arena hi, which is the FST's copy in RAM."""
    return (ARENA_TOP - fst_max) & ~0x1F


def image_size(img: DiscImage) -> int:
    """A plain image's length, or a CISO's up to the end of its last stored block."""
    if img.block:
        return (max(img.map) + 1) * img.block if img.map else 0
    return img.f.seek(0, os.SEEK_END)


def place(start: int, items: Sequence[Tuple[str, int]]) -> List[int]:
    """Offsets for items of (label, size), each on its own 32 KiB boundary from start."""
    offsets = []
    pos = start
    for label, size in items:
        if pos > fst.MAX_U32 or pos + size > LIMIT:
            raise OverlayError(f'the layout would end past 4 GiB, at {label}')
        offsets.append(pos)
        pos += align_up(size)
    return offsets


def merge(root: fst.Entry, items: Sequence[Item]) -> None:
    """Put each item in the tree, replacing the file it matches or adding it.

    A name matches the disc's spelling exactly when it can, and otherwise ignoring case. A name
    that matches several disc names only by case is refused, since the disc holds case twins. A
    replaced file or a directory that exists keeps the disc's spelling. Two items that reach one
    entry with two spellings are refused.
    """
    folded: Dict[int, Dict[str, List[fst.Entry]]] = {}  # a directory's children by lowercase name
    claimed: Dict[int, str] = {}  # each entry an item reached, as the first item spelled it
    for item in items:
        node = root
        disc = []
        for depth, name in enumerate(item.parts):
            last = depth + 1 == len(item.parts)
            shown = '/'.join(item.parts[:depth + 1])
            if id(node) not in folded:
                table: Dict[str, List[fst.Entry]] = {}
                for c in node.children:
                    table.setdefault(c.name.lower(), []).append(c)
                folded[id(node)] = table
            same = folded[id(node)].setdefault(name.lower(), [])
            exact = [c for c in same if c.name == name]
            if len(same) > 1 and not exact:
                twins = ', '.join(repr(c.name) for c in same)
                raise OverlayError(f'{shown} matches {twins} on the disc by case alone')
            child = exact[0] if exact else same[0] if same else None
            if child is None:
                child = fst.Entry(name, 0, item.size) if last else fst.Entry(name, children=[])
                node.children.append(child)
                same.append(child)
                claimed[id(child)] = name
            else:
                first = claimed.setdefault(id(child), name)
                if first != name:
                    where = '/'.join(item.parts[:depth]) or 'the files folder'
                    raise OverlayError(
                        f'{first!r} and {name!r} in {where} are one name on the disc')
                if last and child.children is not None:
                    raise OverlayError(f'{shown} is a directory on the disc, not a file')
                if not last and child.children is None:
                    raise OverlayError(f'{shown} is a file on the disc, not a directory')
                if last:
                    child.size = item.size
                    item.replaced = True
            disc.append(child.name)
            node = child
        item.disc = tuple(disc)
        item.entry = node


def make_overlay(img: DiscImage, items: Sequence[Item], dol: Optional[Item] = None) -> Overlay:
    """The manifest and FST for these items on this image. Nothing is written."""
    disc_id = img.read(0, 6)
    if len(disc_id) < 6 or not all(0x21 <= b < 0x7F for b in disc_id):
        raise OverlayError("the image's disc ID isn't printable ASCII")
    old_at, old_size, old_max, root = fst.read(img)
    base = img.read(old_at, old_size)
    merge(root, items)
    built = len(fst.build(root))
    table_size = max(built, old_size)

    ordered = sorted(items, key=lambda i: [fst.sort_key(p) for p in i.disc])
    laid = [('the FST', table_size)] + ([('the DOL', dol.size)] if dol else [])
    laid += [('/'.join(i.disc), i.size) for i in ordered]
    offsets = place(align_up(max(MIN_START, image_size(img))), laid)
    fst_at = offsets[0]
    dol_at = offsets[1] if dol else 0
    for item, at in zip(ordered, offsets[len(laid) - len(ordered):]):
        item.entry.offset = at
    table = fst.build(root).ljust(table_size, b'\0')

    headers = {0x424: fst_at}
    new_max = old_max
    if dol:
        headers[0x420] = dol_at
    if built > old_size:
        new_max = max(old_max, built)
        headers[0x428] = built
        headers[0x42C] = new_max
    ranges = [(fst_at, len(table), FST_FILE)]
    if dol:
        ranges.append((dol_at, dol.size, dol.rel))
    ranges += [(i.entry.offset, i.size, i.rel) for i in items if i.size]
    ranges.sort()

    lines = ['gcn-disc-overlay 1',
             f"base {disc_id.decode('ascii')} {old_at:x} {old_size:x} {fnv1a64(base):016x}"]
    lines += [f'header {word:x} {value:x}' for word, value in sorted(headers.items())]
    lines += [f'range {at:x} {size:x} {rel}' for at, size, rel in ranges]
    try:
        manifest = ('\n'.join(lines) + '\n').encode('utf-8')
    except UnicodeEncodeError:
        raise OverlayError("a path isn't valid UTF-8") from None

    added = [i for i in items if not i.replaced]
    replaced = [i for i in items if i.replaced]
    summary = {'added_files': len(added), 'added_bytes': sum(i.size for i in added),
               'replaced_files': len(replaced), 'replaced_bytes': sum(i.size for i in replaced),
               'fst_growth': max(0, built - old_size),
               'arena_hi_drop': arena_hi(old_max) - arena_hi(new_max)}
    return Overlay(manifest, table, summary)


def _locate(out: str, path: str, what: str, folder: bool) -> str:
    """The real path of a folder or file that sits inside out and isn't a symbolic link."""
    bare = path.rstrip(os.sep) or os.sep
    if os.path.islink(bare):
        raise OverlayError(f'{what} {path} is a symbolic link')
    real = os.path.realpath(bare)
    if real == out or os.path.commonpath([out, real]) != out:
        raise OverlayError(f'{what} {path} has to be inside {out}')
    if not (os.path.isdir(real) if folder else os.path.isfile(real)):
        raise OverlayError(f"{what} {path} isn't a {'folder' if folder else 'file'}")
    return real


def _item(out: str, path: str, parts: Tuple[str, ...]) -> Item:
    rel = os.path.relpath(path, out).replace(os.sep, '/')
    if any(ord(c) < 0x20 or ord(c) == 0x7F for c in rel):
        raise OverlayError(f'{rel!r} has a control character in its path')
    return Item(rel, os.stat(path).st_size, parts)


def scan(out: str, folder: str) -> List[Item]:
    """Every file under folder, which has to be inside out. Dotfiles are skipped."""
    root = _locate(out, folder, 'the files folder', True)
    items = []
    todo: List[Tuple[str, ...]] = [()]
    while todo:
        parts = todo.pop()
        with os.scandir(os.path.join(root, *parts)) as it:
            entries = sorted(it, key=lambda e: e.name)
        for e in entries:
            if e.name.startswith('.'):
                continue
            path = parts + (e.name,)
            shown = '/'.join(path)
            if e.is_symlink():
                raise OverlayError(f'{shown} is a symbolic link')
            if not e.name.isascii():
                raise OverlayError(f"{shown} has a name that isn't ASCII")
            if e.is_dir(follow_symlinks=False):
                todo.append(path)
            elif e.is_file(follow_symlinks=False):
                items.append(_item(out, e.path, path))
            else:
                raise OverlayError(f"{shown} isn't a file or a folder")
    return items


def scan_dol(out: str, path: str) -> Item:
    item = _item(out, _locate(out, path, 'the DOL', False), ())
    if item.size < MIN_DOL:
        raise OverlayError(f"the DOL {path} is under {MIN_DOL:#x} bytes, so it isn't a DOL")
    return item


def main(argv: Optional[Sequence[str]] = None) -> None:
    parser = argparse.ArgumentParser(description='Build a disc overlay for a GameCube image.')
    parser.add_argument('image')
    parser.add_argument('out')
    parser.add_argument('--files', required=True, metavar='DIR',
                        help='the folder of files, inside out')
    parser.add_argument('--dol', metavar='FILE', help='a DOL to boot, inside out')
    args = parser.parse_args(argv)
    try:
        out = os.path.realpath(args.out)
        if not os.path.isdir(out):
            raise OverlayError(f"{args.out} isn't a folder")
        items = scan(out, args.files)
        dol = scan_dol(out, args.dol) if args.dol else None
        img = DiscImage(args.image)
        try:
            overlay = make_overlay(img, items, dol)
        finally:
            img.f.close()
        for name, data in ((FST_FILE, overlay.fst), (MANIFEST, overlay.manifest)):
            with open(os.path.join(out, name), 'wb') as f:
                f.write(data)
        print(json.dumps(overlay.summary, sort_keys=True))
    except (OverlayError, fst.FstError, OSError) as e:
        sys.exit(f'error: {e}')


if __name__ == '__main__':
    main()
