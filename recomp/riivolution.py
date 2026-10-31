#!/usr/bin/env python3
"""Turn a folder of Riivolution mods into a disc overlay and a RAM patch list.

ROOT is the SD root a Riivolution release unzips into, so the XMLs sit under ROOT/riivolution/.
Each XML that has a wiidisc root is a package. A package offers options, and each option has
choices that turn patches on. File and folder patches become a disc overlay (see
docs/disc-overlay.md) and memory patches become a RAM patch list. See docs/riivolution.md for
what is read and what is refused.

Usage:
  riivolution.py list ROOT
      print one JSON line, {"packages": [...]}. A package has its XML's path under
      ROOT/riivolution/ and either its options or an error. An option has an id, a name, its
      section, the names of its choices and its default, which is a choice number from 1, or 0
      for none. The id is the option's own, or its section's name and its name together when it
      has none.
  riivolution.py apply IMAGE ROOT OUT [--choice XML:OPTION=CHOICE ...]
      write OUT/disc-overlay.txt and OUT/fst.bin when a file patch applies and OUT/ram-patches.txt
      when a memory patch does, and remove the ones that don't apply. Then print one JSON line
      with the packages that are on, the files replaced and added, the RAM patches and bytes
      and the overlay's summary. An option with no --choice uses its default, and choice 0 turns
      it off. The packages are taken in the order of their XML paths.
      Exits 0, or 1 with `error: ...` naming the XML and the element of the first refused
      package, or 2 on bad arguments. Nothing is written when anything is refused. The same
      inputs give the same bytes.

The image is a plain image or a CISO. OUT is made when it has something to hold.
"""
import argparse
import json
import os
import re
import sys
from typing import Dict, List, Optional, Sequence, Tuple, Union

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fst  # noqa: E402
from disc_overlay import FST_FILE, MANIFEST, OverlayError, make_overlay  # noqa: E402
from dol import DiscImage  # noqa: E402
from riivolution_patch import Patcher, RamPatch  # noqa: E402
from riivolution_paths import PathError, fold, match  # noqa: E402
from riivolution_xml import Package, Refused, read_package  # noqa: E402

RAM_LIST = 'ram-patches.txt'
Found = Tuple[str, str, Union[Package, Refused]]  # an XML's path under riivolution/, its host path


class UsageError(ValueError):
    """Arguments that don't fit the packages on disk."""


def scan(root: str) -> List[Found]:
    """Every package under ROOT/riivolution, in the order of their paths.

    A file or folder whose name starts with a dot is skipped, and so is an XML with another root
    element. A symbolic link to a folder or an XML is listed with an error instead of followed.
    """
    try:
        top = match(root, 'riivolution')
    except PathError as e:
        raise Refused(f"The folder 'riivolution' {e}") from None
    if top is None:
        return []
    found: List[Found] = []
    todo = [((), os.path.join(root, top))]
    while todo:
        names, folder = todo.pop()
        with os.scandir(folder) as it:
            entries = sorted(it, key=lambda e: e.name)
        for entry in entries:
            xml = '/'.join(names + (entry.name,))
            if entry.name.startswith('.'):
                continue
            if entry.is_symlink():
                if fold(entry.name).endswith('.xml') or entry.is_dir():
                    link = Refused("is a symbolic link, so it isn't read", xml)
                    found.append((xml, entry.path, link))
            elif entry.is_dir(follow_symlinks=False):
                todo.append((names + (entry.name,), entry.path))
            elif entry.is_file(follow_symlinks=False) and fold(entry.name).endswith('.xml'):
                try:
                    package = read_package(entry.path, xml)
                except Refused as e:
                    found.append((xml, entry.path, e))
                else:
                    if package is not None:
                        found.append((xml, entry.path, package))
    return sorted(found, key=lambda f: f[0])


def list_packages(root: str) -> Dict[str, object]:
    packages: List[Dict[str, object]] = []
    for xml, _, found in scan(os.path.realpath(root)):
        if isinstance(found, Refused):
            packages.append({'xml': xml, 'error': str(found)})
        else:
            packages.append({'xml': xml, 'options': [
                {'id': o.key, 'name': o.name, 'section': o.section, 'default': o.default,
                 'choices': [c.name for c in o.choices]} for o in found.options]})
    return {'packages': packages}


def parse_picks(specs: Sequence[str],
                found: Dict[str, Union[Package, Refused]]) -> Dict[str, Dict[str, int]]:
    """The choices from --choice XML:OPTION=CHOICE, by package and option.

    The XML is the longest package path the argument starts with, so a colon in a path or an
    option id doesn't confuse it.
    """
    picks: Dict[str, Dict[str, int]] = {}
    for spec in specs:
        parts = re.fullmatch(r'(.*)=([0-9]+)', spec)
        if not parts:
            raise UsageError(f"--choice {spec!r} isn't XML:OPTION=CHOICE with a choice number")
        head, number = parts.group(1), int(parts.group(2))
        xml = max((x for x in found if head.startswith(x + ':')), key=len, default=None)
        if xml is None:
            raise UsageError(f'--choice {spec!r} names no package under riivolution/')
        package = found[xml]
        if isinstance(package, Refused):
            raise package
        key = head[len(xml) + 1:]
        option = next((o for o in package.options if o.key == key), None)
        if option is None:
            raise UsageError(f'--choice {spec!r} names no option {key!r} in {xml}')
        if number > len(option.choices):
            raise UsageError(f'--choice {spec!r} is past the {len(option.choices)} choices of '
                             f'{key!r}')
        if key in picks.setdefault(xml, {}):
            raise UsageError(f'--choice names {key!r} in {xml} twice')
        picks[xml][key] = number
    return picks


def ram_text(disc_id: str, patches: Sequence[RamPatch]) -> bytes:
    lines = ['gcn-ram-patches 1', f'base {disc_id}']
    for p in patches:
        line = f'patch {p.addr:08x} {p.value.hex()}'
        lines.append(line + (f' {p.original.hex()}' if p.original else ''))
    return ('\n'.join(lines) + '\n').encode('ascii')


def publish(out: str, files: Dict[str, Optional[bytes]]) -> None:
    """Write each file that has bytes and remove each that has None."""
    if any(data is not None for data in files.values()):
        os.makedirs(out, exist_ok=True)
    for name, data in files.items():
        path = os.path.join(out, name)
        if data is None:
            if os.path.lexists(path):
                os.unlink(path)
        else:
            with open(path + '.tmp', 'wb') as f:
                f.write(data)
            os.replace(path + '.tmp', path)


def apply_packages(image: str, root: str, out: str, specs: Sequence[str]) -> Dict[str, object]:
    sd_root = os.path.realpath(root)
    out_dir = os.path.realpath(out)
    entries = scan(sd_root)
    picks = parse_picks(specs, {xml: package for xml, _, package in entries})
    img = DiscImage(image)
    try:
        head = img.read(0, 8)
        if len(head) < 8 or not all(0x21 <= b < 0x7F for b in head[:6]):
            raise OverlayError("the image's disc ID isn't printable ASCII")
        game_id = head[:6].decode('ascii')
        tree = fst.read(img)[3]
        patcher = Patcher(sd_root, game_id, tree)
        on: List[str] = []
        for xml, path, package in entries:
            if isinstance(package, Refused) or not package.game_filter.allows(
                    game_id, head[7], head[6]):
                continue
            patches = package.active(picks.get(xml, {}))
            if patches:
                patcher.apply(package, patches, os.path.dirname(path))
                on.append(xml)
        items = patcher.items(out_dir)
        ram = patcher.ram_patches()
        overlay = make_overlay(img, items) if items else None
    finally:
        img.f.close()
    publish(out_dir, {
        FST_FILE: overlay.fst if overlay else None,
        RAM_LIST: ram_text(game_id, ram) if ram else None,
        MANIFEST: overlay.manifest if overlay else None})
    summary = overlay.summary if overlay else None
    return {'packages': on, 'overlay': summary,
            'files_added': summary['added_files'] if summary else 0,
            'files_replaced': summary['replaced_files'] if summary else 0,
            'ram_patches': len(ram), 'ram_bytes': sum(len(p.value) for p in ram)}


def main(argv: Optional[Sequence[str]] = None) -> None:
    parser = argparse.ArgumentParser(
        description='Turn a folder of Riivolution mods into a disc overlay and RAM patches.')
    commands = parser.add_subparsers(dest='command', required=True)
    lister = commands.add_parser('list', help='print the packages and their options')
    lister.add_argument('root')
    applier = commands.add_parser('apply', help='write the overlay and the RAM patch list')
    applier.add_argument('image')
    applier.add_argument('root')
    applier.add_argument('out')
    applier.add_argument('--choice', action='append', default=[], metavar='XML:OPTION=CHOICE',
                         help='a choice number for an option, 0 for none')
    args = parser.parse_args(argv)
    if not os.path.isdir(args.root):
        parser.error(f"{args.root} isn't a folder")
    try:
        if args.command == 'list':
            result = list_packages(args.root)
        else:
            if not os.path.isfile(args.image):
                parser.error(f"{args.image} isn't a file")
            if os.path.exists(args.out) and not os.path.isdir(args.out):
                parser.error(f"{args.out} isn't a folder")
            result = apply_packages(args.image, args.root, args.out, args.choice)
        print(json.dumps(result, sort_keys=True))
    except UsageError as e:
        parser.error(str(e))
    except (Refused, OverlayError, fst.FstError, OSError) as e:
        sys.exit(f'error: {e}')


if __name__ == '__main__':
    main()
