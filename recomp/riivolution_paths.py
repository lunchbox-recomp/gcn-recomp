"""Find the host file a Riivolution path names.

An external path is read the way Dolphin reads it (DiscIO/RiivolutionPatcher.cpp:29-161). One
that starts with / is relative to the SD root, and any other is relative to the folder the caller
gives, which is the patch's root or its XML's folder. A `.` part is skipped. A `..` part goes back
up the path so far and is refused past where the path started. A part made only of dots with three
or more is refused, since some file systems read it as several `..`. So is a part with a control
character, since a manifest line can't hold one.

Riivolution assumes a file system that ignores case, so a part matches the host's spelling of
it. This reader also refuses a symbolic link in the path, so a package can't point outside its
own folder.
"""
import os
from typing import Optional


class PathError(ValueError):
    """A path that can't be used. The message finishes the sentence `the path ..., which`."""


def cut(value: str, limit: int = 60) -> str:
    """A value as a message shows it, ended with ... when it is long."""
    return value if len(value) <= limit else value[:limit - 3] + '...'


def control(name: str) -> bool:
    """Whether a name holds a C0 control character or DEL, which no manifest line can carry."""
    return any(ord(c) < 0x20 or ord(c) == 0x7F for c in name)


def fold(name: str) -> str:
    """Lowercase for A to Z only, so only ASCII names compare equal by case."""
    return ''.join(chr(ord(c) + 32) if 'A' <= c <= 'Z' else c for c in name)


def match(directory: str, name: str) -> Optional[str]:
    """The spelling of name in a directory, or None when it has nothing by that name.

    An exact spelling wins. Otherwise the one name that differs only by case is the match, and
    several of them are refused, since there is no telling which the package means.
    """
    try:
        with os.scandir(directory) as it:
            names = sorted(e.name for e in it)
    except OSError:
        return None
    if name in names:
        return name
    key = fold(name)
    twins = [n for n in names if fold(n) == key]
    if len(twins) > 1:
        raise PathError(f"matches {', '.join(repr(cut(n)) for n in twins)} by case alone")
    if twins:
        return twins[0]
    # A file system that folds more than ASCII case can hold a name its listing spells another way.
    return name if os.path.lexists(os.path.join(directory, name)) else None


def resolve(sd_root: str, here: str, text: str) -> str:
    """The host path text names. Every part has to exist.

    sd_root is the SD card's root and here is the folder a path with no leading / starts from.
    """
    path = sd_root if text.startswith('/') else here
    depth = 0
    for part in text.split('/'):
        if not part or part == '.':
            continue
        if control(part):
            raise PathError('has a part with a control character')
        if part == '..':
            if depth == 0:
                raise PathError('goes above the folder it starts from')
            depth -= 1
            path = os.path.dirname(path)
        elif set(part) == {'.'}:
            raise PathError('has a part made only of dots, and only . and .. are read')
        else:
            found = match(path, part)
            if found is None:
                raise PathError(f'has no {cut(part)!r}')
            depth += 1
            path = os.path.join(path, found)
            if os.path.islink(path):
                raise PathError(f'passes the symbolic link {cut(found)!r}')
    return path
