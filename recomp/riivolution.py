#!/usr/bin/env python3
"""Turn a folder of Riivolution mods into a disc overlay and a RAM patch list.

ROOT is the SD root a Riivolution release unzips into, so the XMLs sit in ROOT/riivolution/.
Each XML there that has a wiidisc root is a package. A package offers options, and each option
has choices that turn patches on. File and folder patches become a disc overlay (see
docs/disc-overlay.md) and memory patches become a RAM patch list. See docs/riivolution.md for
what is read and what is refused.

Usage:
  riivolution.py list ROOT
      print one JSON line, {"packages": [...]}. A package has its XML's name and either its
      options or an error. An option has an id, a name, its section, the names of its choices
      and its default, which is a choice number from 1, or 0 for none. The id is the option's
      own, or its section's name and its name together when it has none.
  riivolution.py apply IMAGE ROOT OUT [--choice XML:OPTION=CHOICE ...]
      write OUT/disc-overlay.txt and OUT/fst.bin when a file patch applies and OUT/ram-patches.txt
      when a memory patch does, and remove the ones that don't apply. Then print one JSON line
      with the packages that are on, the files replaced and added, the RAM patches and bytes
      and the overlay's summary. An option with no --choice uses its default, and choice 0 turns
      it off, also for a package with an error. The packages are taken in the order of their names.
      Exits 0, or 1 with `error: ...` naming the XML and the element of the first refused
      package, or 2 on bad arguments. On 1 it also prints one JSON line,
      {"error": "...", "packages": [...]}, with the XML names at fault, both for a conflict.
      Nothing is written when anything is refused. The same inputs give the same bytes.

The image is a plain image or a CISO. OUT is made when it has something to hold.
"""
import argparse
import contextlib
import json
import os
import re
import secrets
import stat
import sys
from typing import Dict, List, Optional, Sequence, Tuple, Union

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fst  # noqa: E402
from disc_overlay import FST_FILE, MANIFEST, OverlayError, make_overlay  # noqa: E402
from dol import DiscImage  # noqa: E402
from riivolution_patch import Patcher, RamPatch  # noqa: E402
from riivolution_paths import PathError, control, cut, fold, match  # noqa: E402
from riivolution_xml import Package, Refused, read_package  # noqa: E402

RAM_LIST = 'ram-patches.txt'
OUTPUT_NAMES = (FST_FILE, RAM_LIST, MANIFEST)
# A temp file is made new and is never followed through a link, since OUT can sit in a folder
# that a release fills.
NEW_FILE = os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, 'O_NOFOLLOW', 0)
OPEN_OUT = os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0) | getattr(os, 'O_NOFOLLOW', 0)
Found = Tuple[str, str, Union[Package, Refused]]  # an XML's name, its host path


class UsageError(ValueError):
    """Arguments that don't fit the packages on disk."""


def scan(root: str) -> List[Found]:
    """Every package directly in ROOT/riivolution, in the order of their names.

    Dolphin reads only the XMLs at the top of that folder, so no folder in it is entered. A name
    that starts with a dot is skipped, and so is an XML with another root element. A symbolic
    link to an XML is listed with an error instead of followed, and the folder itself must not
    be a link. An XML whose name has a control character is listed with an error too, with the
    name escaped in the message, since a name like that could add a line to a caller's own files.
    """
    try:
        top = match(root, 'riivolution')
    except PathError as e:
        raise Refused(f"The folder 'riivolution' {e}") from None
    if top is None:
        return []
    folder = os.path.join(root, top)
    if os.path.islink(folder):
        raise Refused("The folder 'riivolution' is a symbolic link, so it isn't read")
    found: List[Found] = []
    with os.scandir(folder) as it:
        entries = sorted(it, key=lambda e: e.name)
    for entry in entries:
        if entry.name.startswith('.') or not fold(entry.name).endswith('.xml'):
            continue
        if control(entry.name):
            odd = Refused("has a control character in its name, so it isn't read",
                          repr(entry.name)[1:-1])
            found.append((entry.name, entry.path, odd))
        elif entry.is_symlink():
            link = Refused("is a symbolic link, so it isn't read", entry.name)
            found.append((entry.name, entry.path, link))
        elif entry.is_file(follow_symlinks=False):
            try:
                package = read_package(entry.path, entry.name)
            except Refused as e:
                found.append((entry.name, entry.path, e))
            else:
                if package is not None:
                    found.append((entry.name, entry.path, package))
    return found


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

    The XML is the longest package name the argument starts with, so a colon in a name or an
    option id doesn't confuse it. A choice of 0 for a package with an error is allowed and does
    nothing, so an off that was saved for a package that later broke doesn't stop the rest.
    """
    picks: Dict[str, Dict[str, int]] = {}
    for spec in specs:
        parts = re.fullmatch(r'(.*)=([0-9]+)', spec)
        if not parts:
            raise UsageError(f"--choice {cut(spec)!r} isn't XML:OPTION=CHOICE with a choice "
                             'number')
        head, number = parts.group(1), int(parts.group(2))
        xml = max((x for x in found if head.startswith(x + ':')), key=len, default=None)
        if xml is None:
            raise UsageError(f'--choice {cut(spec)!r} names no package under riivolution/')
        package = found[xml]
        if isinstance(package, Refused):
            if number:
                raise package
            continue
        key = head[len(xml) + 1:]
        option = next((o for o in package.options if o.key == key), None)
        if option is None:
            raise UsageError(f'--choice {cut(spec)!r} names no option {cut(key)!r} in {xml}')
        if number > len(option.choices):
            raise UsageError(f'--choice {cut(spec)!r} is past the {len(option.choices)} '
                             f'choices of {cut(key)!r}')
        if key in picks.setdefault(xml, {}):
            raise UsageError(f'--choice names {cut(key)!r} in {xml} twice')
        picks[xml][key] = number
    return picks


def ram_text(disc_id: str, patches: Sequence[RamPatch]) -> bytes:
    lines = ['gcn-ram-patches 1', f'base {disc_id}']
    for p in patches:
        line = f'patch {p.addr:08x} {p.value.hex()}'
        lines.append(line + (f' {p.original.hex()}' if p.original else ''))
    return ('\n'.join(lines) + '\n').encode('ascii')


def check_output(name: str, mode: int) -> None:
    """Refuse an output name that is a link or a folder, given the mode lstat gave it."""
    if stat.S_ISLNK(mode):
        raise Refused(f'The output {name} is a symbolic link, so nothing is written there')
    if stat.S_ISDIR(mode):
        raise Refused(f'The output {name} is a folder, so nothing is written there')


def check_out(out: str) -> str:
    """Refuse an OUT, or an output name in it, that a write could be steered through.

    OUT is usually a folder inside ROOT, which a release fills, so a release could leave a link
    where OUT or an output goes. A link in a folder above OUT is the player's own and is followed.
    The OUT it gives back is the one normpath spells, which every later step uses, so `.out/.` or
    `.out/../.out` can't hide a link at `.out`.
    """
    norm = os.path.normpath(out)
    if os.path.islink(norm):
        raise Refused(f"The output folder {cut(out)!r} is a symbolic link, so it isn't written to")
    for output in OUTPUT_NAMES:
        try:
            mode = os.lstat(os.path.join(norm, output)).st_mode
        except FileNotFoundError:
            continue
        check_output(output, mode)
    return norm


def checkpoint(stage: str) -> None:
    """Called where publish could be caught between two steps. A test replaces it to change OUT."""


def temp_name(name: str) -> str:
    """A name for the temp file of an output, which nothing can have planned for."""
    return f'.{name}.{secrets.token_hex(8)}.tmp'


def open_out(out: str) -> int:
    """OUT opened as a folder and not through a link, so every write lands in the folder opened."""
    try:
        return os.open(out, OPEN_OUT)
    except OSError:
        if os.path.islink(out):
            raise Refused(f"The output folder {cut(out)!r} is a symbolic link, so it isn't "
                          'written to') from None
        raise


def recheck(fd: int, name: str) -> None:
    """Refuse an output name in the open folder that has become a link or a folder."""
    try:
        mode = os.lstat(name, dir_fd=fd).st_mode
    except FileNotFoundError:
        return
    check_output(name, mode)


def write_outputs(fd: int, files: Dict[str, Optional[bytes]]) -> None:
    for name in OUTPUT_NAMES:
        recheck(fd, name)
    checkpoint('temps')
    made: List[Tuple[str, str]] = []
    try:
        for name, data in files.items():
            if data is not None:
                temp = temp_name(name)
                made_fd = os.open(temp, NEW_FILE, 0o644, dir_fd=fd)
                made.append((temp, name))
                with os.fdopen(made_fd, 'wb') as f:
                    f.write(data)
        for temp, name in made:  # in order, so the manifest goes last
            checkpoint(f'replace {name}')
            recheck(fd, name)
            os.replace(temp, name, src_dir_fd=fd, dst_dir_fd=fd)
    except BaseException:
        for temp, _ in made:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(temp, dir_fd=fd)
        raise
    for name, data in files.items():
        if data is None:
            recheck(fd, name)
            with contextlib.suppress(FileNotFoundError):
                os.unlink(name, dir_fd=fd)


def publish(out: str, files: Dict[str, Optional[bytes]]) -> None:
    """Replace each output that has bytes and remove each that has None.

    OUT is opened once, not through a link, and every write goes through that open folder, so a
    link put in OUT's place later changes nothing. Every temp file is made, new and never
    through a link, before any output changes, so a write that can't be made leaves the old
    outputs as they were. Each name is looked at again just before its replace. If the folder
    changes during the run, a replace can still fail part way, and running it again puts all
    three in place.
    """
    if any(data is not None for data in files.values()):
        os.makedirs(out, exist_ok=True)
    elif not os.path.lexists(out):
        return
    checkpoint('open')
    fd = open_out(out)
    try:
        write_outputs(fd, files)
    finally:
        os.close(fd)


def apply_packages(image: str, root: str, out: str, specs: Sequence[str]) -> Dict[str, object]:
    out = check_out(out)
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
        packages = e.packages if isinstance(e, Refused) else []
        print(json.dumps({'error': str(e), 'packages': packages}, sort_keys=True))
        sys.exit(f'error: {e}')


if __name__ == '__main__':
    main()
