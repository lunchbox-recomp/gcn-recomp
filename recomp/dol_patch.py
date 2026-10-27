#!/usr/bin/env python3
"""Apply patch lists to a DOL before it is recompiled.

A patch list changes words, adds sections and names the functions in the new code, so the
recompiler compiles them like the game's own. See docs/dol-patches.md for the format.

Usage:
  dol_patch.py IN OUT PATCH...
      write OUT, the DOL with each patch list applied in the order given, and OUT.syms, the
      symbols in dtk's format that recomp.py reads beside any DOL. OUT.syms holds the symbol
      lines of IN.syms when there is one, then the new functions, sorted by address. IN.modules
      is copied to OUT.modules when there is one, and an old OUT.modules goes when there isn't.
      Exits 0, or 1 with `file:line: reason` on the
      first refused patch, or 2 on bad arguments. Nothing is written when anything is refused.
      The same inputs give the same bytes.
"""
import argparse
import hashlib
import os
import re
import stat
import struct
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol import SYM_RE  # noqa: E402

HEADER = 0x100
SLOTS = 18
TEXT_SLOTS = 7
ALIGN = 0x20
RAM_START = 0x80000000
RAM_END = 0x81800000
MAX_U32 = 0xFFFFFFFF
# What runtime/boot.cpp writes after it copies the DOL's sections to RAM, as half-open ranges. A
# section there would hold something else once the game starts. The first range is the disc
# header and the boot info words (:30-35) with the arena words (:51-53). The next is the words at
# :36-37, and then come the words at :38-40 with :48. The three single words are :41-43. The last
# range is the BI2 (:46-47), with the FST just below it. A test reads boot.cpp to keep these in
# step.
BOOT_WRITES = (
    (0x80000000, 0x80000040),
    (0x800000CC, 0x800000D4),
    (0x800000F0, 0x80000100),
    (0x80000300, 0x80000304),
    (0x80000800, 0x80000804),
    (0x80000C00, 0x80000C04),
    (0x817FE000, 0x81800000),
)

COMMENT = re.compile(r'(?:^|\s)#.*$')
HEX = re.compile(r'(?:0[xX])?[0-9A-Fa-f]+')
SHA1 = re.compile(r'[0-9A-Fa-f]{40}')
NAME = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')


class PatchError(ValueError):
    """A refused patch. The message starts with the file and line at fault."""


@dataclass
class Section:
    addr: int
    size: int
    offset: int
    text: bool
    slot: int
    where: Optional[str] = None  # the line that added it, or None for the DOL's own
    owner: int = -1  # which list added it, counting from 0

    @property
    def end(self) -> int:
        return self.addr + self.size

    @property
    def label(self) -> str:
        if self.where is None:
            return f"the DOL's {'text' if self.text else 'data'} section {self.slot}"
        return f'the section added at {self.where}'


@dataclass
class Function:
    addr: int
    size: int
    name: str
    where: str


@dataclass
class Symbol:
    """A line of IN's symbols file, as recomp.py reads it."""
    name: str
    addr: int
    line: str
    where: str
    size: Optional[int]
    function: bool


def number(where: str, what: str, token: str) -> int:
    if not HEX.fullmatch(token):
        raise PatchError(f'{where}: {what} {token} is not a hex number')
    value = int(token, 16)
    if value > MAX_U32:
        raise PatchError(f'{where}: {what} {token} is more than 32 bits')
    return value


def arity(where: str, op: str, args: Sequence[str], *counts: int) -> None:
    if len(args) not in counts:
        wanted = ' or '.join(str(c) for c in counts)
        raise PatchError(f'{where}: {op} takes {wanted} arguments, not {len(args)}')


def read_symbols(path: str) -> List[Symbol]:
    """The symbol lines of a dtk symbols file. Any other line is one recomp.py skips too."""
    try:
        with open(path, encoding='utf-8') as f:
            lines = f.read().split('\n')
    except (OSError, ValueError):
        raise PatchError(f"{path}: can't read the symbols file")
    out = []
    for n, raw in enumerate(lines, 1):
        line = raw.strip()
        m = SYM_RE.match(line)
        if not m:
            continue
        name, _, addr, rest = m.groups()
        where = f'{path}:{n}'
        attrs = dict(tok.split(':', 1) for tok in rest.split() if ':' in tok)
        size = number(where, 'size', attrs['size']) if 'size' in attrs else None
        out.append(Symbol(name, int(addr, 16), line, where, size, attrs.get('type') == 'function'))
    return out


def read_modules(path: str) -> Optional[bytes]:
    """The bytes of IN's modules file, or None when it has none."""
    if not os.path.exists(path):
        return None
    try:
        with open(path, 'rb') as f:
            return f.read()
    except OSError:
        raise PatchError(f"{path}: can't read the modules file")


def section_file(where: str, base: str, name: str, size: int) -> bytes:
    """The bytes of a section's file, which has to be a regular file inside the list's folder.

    The name is resolved first, so a `..` or a symlink can't leave the folder. A pipe or a device
    is refused before it's opened, since opening one can block, and the read stops one byte past
    the section so a file that never ends can't grow without bound.
    """
    if os.path.isabs(name):
        raise PatchError(f'{where}: {name} is an absolute path')
    root = os.path.realpath(base)
    real = os.path.realpath(os.path.join(root, name))
    if os.path.commonpath([root, real]) != root:
        raise PatchError(f'{where}: {name} is outside the folder that holds the list')
    try:
        regular = stat.S_ISREG(os.stat(real).st_mode)
    except OSError:
        raise PatchError(f"{where}: can't read {name}")
    if not regular:
        raise PatchError(f'{where}: {name} is not a regular file')
    try:
        with open(real, 'rb') as f:
            payload = f.read(size + 1)
    except OSError:
        raise PatchError(f"{where}: can't read {name}")
    if len(payload) > size:
        raise PatchError(f"{where}: {name} is more than the section's {size:X} bytes")
    return payload


class Patcher:
    def __init__(self, name: str, data: bytes, symbols: Sequence[Symbol] = ()) -> None:
        if len(data) < HEADER:
            raise PatchError(f'{name}: too short to be a DOL')
        self.sha1 = hashlib.sha1(data).hexdigest()
        self.out = bytearray(data)
        offsets = struct.unpack_from('>18I', data, 0x00)
        addrs = struct.unpack_from('>18I', data, 0x48)
        sizes = struct.unpack_from('>18I', data, 0x90)
        self.bss_addr, self.bss_size = struct.unpack_from('>2I', data, 0xD8)
        self.slots: List[Optional[Section]] = [None] * SLOTS
        for slot in range(SLOTS):
            if sizes[slot]:
                if offsets[slot] + sizes[slot] > len(data):
                    raise PatchError(f'{name}: section {slot} runs past the end of the file')
                self.slots[slot] = Section(addrs[slot], sizes[slot], offsets[slot], slot < TEXT_SLOTS, slot)
        self.written: Dict[int, str] = {}
        self.functions: List[Function] = []
        self.owner = -1  # the list being applied
        # IN's symbols, which go to OUT.syms and which a new function can't overlap or rename.
        # `names` also takes each new function's, so no two share one.
        self.symbols = list(symbols)
        self.names: Dict[str, str] = {}
        for s in self.symbols:
            self.names.setdefault(s.name, s.where)
        self.inherited = [Function(s.addr, s.size, s.name, s.where)
                          for s in self.symbols if s.function and s.size]

    def sections(self) -> List[Section]:
        return [s for s in self.slots if s is not None]

    def apply(self, path: str) -> None:
        self.owner += 1
        try:
            # A byte order mark is dropped, and only a newline ends a line, so a separator
            # inside a comment doesn't start a new one. `newline=''` keeps a lone CR as it is.
            with open(path, encoding='utf-8-sig', newline='') as f:
                lines = f.read().split('\n')
        except (OSError, ValueError):
            raise PatchError(f"{path}: can't read the patch list")
        base = os.path.dirname(os.path.abspath(path))
        seen_dol = False
        for n, raw in enumerate(lines, 1):
            line = COMMENT.sub('', raw).strip()
            if not line:
                continue
            where = f'{path}:{n}'
            op = line.split(None, 1)[0]
            args = line.split(None, 4)[1:] if op == 'section' else line.split()[1:]
            if op == 'dol':
                if seen_dol:
                    raise PatchError(f'{where}: dol is only allowed once, on the first line')
                self.dol(where, args)
                seen_dol = True
            elif not seen_dol:
                raise PatchError(f'{where}: the first line has to be dol <sha1 of the input>')
            elif op == 'section':
                self.section(where, base, args)
            elif op == 'word':
                self.word(where, args)
            elif op == 'function':
                self.function(where, args)
            else:
                raise PatchError(f'{where}: unknown line {op}')
        if not seen_dol:
            raise PatchError(f'{path}:1: the first line has to be dol <sha1 of the input>')

    def dol(self, where: str, args: Sequence[str]) -> None:
        arity(where, 'dol', args, 1)
        if not SHA1.fullmatch(args[0]):
            raise PatchError(f'{where}: {args[0]} is not a SHA-1')
        if args[0].lower() != self.sha1:
            raise PatchError(f'{where}: the list is for the DOL with SHA-1 {args[0].lower()}, '
                             f'and the input is {self.sha1}')

    def section(self, where: str, base: str, args: Sequence[str]) -> None:
        arity(where, 'section', args, 3, 4)
        kind = args[0]
        if kind not in ('text', 'data'):
            raise PatchError(f'{where}: a section is text or data, not {kind}')
        addr = number(where, 'address', args[1])
        size = number(where, 'size', args[2])
        if addr % ALIGN:
            raise PatchError(f'{where}: address {addr:08X} is not 32-byte aligned')
        if size == 0 or size % 4:
            raise PatchError(f'{where}: size {size:X} is not a positive multiple of 4')
        if addr < RAM_START or addr + size > RAM_END:
            raise PatchError(f'{where}: {addr:08X} to {addr + size:08X} is not inside main RAM '
                             f'below {RAM_END:08X}')
        for lo, hi in BOOT_WRITES:
            if addr < hi and lo < addr + size:
                raise PatchError(f'{where}: {addr:08X} to {addr + size:08X} overlaps {lo:08X} to '
                                 f'{hi:08X}, which boot writes after it loads the sections')
        for s in self.sections():
            if addr < s.end and s.addr < addr + size:
                raise PatchError(f'{where}: overlaps {s.label}')
        if self.bss_size and addr < self.bss_addr + self.bss_size and self.bss_addr < addr + size:
            raise PatchError(f'{where}: overlaps the DOL\'s bss')
        first, stop = (0, TEXT_SLOTS) if kind == 'text' else (TEXT_SLOTS, SLOTS)
        free = [slot for slot in range(first, stop) if self.slots[slot] is None]
        if not free:
            raise PatchError(f'{where}: all {stop - first} {kind} slots are in use')
        payload = section_file(where, base, args[3], size) if len(args) == 4 else b''
        slot = free[0]
        self.out += bytes(-len(self.out) % ALIGN)
        offset = len(self.out)
        self.out += payload + bytes(size - len(payload))
        self.slots[slot] = Section(addr, size, offset, kind == 'text', slot, where, self.owner)
        struct.pack_into('>I', self.out, 4 * slot, offset)
        struct.pack_into('>I', self.out, 0x48 + 4 * slot, addr)
        struct.pack_into('>I', self.out, 0x90 + 4 * slot, size)

    def word(self, where: str, args: Sequence[str]) -> None:
        arity(where, 'word', args, 3)
        addr = number(where, 'address', args[0])
        value = number(where, 'value', args[1])
        expect = number(where, 'expected value', args[2])
        if addr % 4:
            raise PatchError(f'{where}: address {addr:08X} is not 4-aligned')
        section = next((s for s in self.sections() if s.addr <= addr and addr + 4 <= s.end), None)
        if section is None:
            raise PatchError(f'{where}: {addr:08X} is not inside a section')
        if section.where is not None and section.owner != self.owner:
            raise PatchError(f'{where}: {addr:08X} is inside the section added at {section.where}, '
                             'which belongs to another list')
        if addr in self.written:
            raise PatchError(f'{where}: word {addr:08X} was already written at {self.written[addr]}')
        at = section.offset + addr - section.addr
        have = struct.unpack_from('>I', self.out, at)[0]
        if have != expect:
            raise PatchError(f'{where}: word {addr:08X} holds {have:08X}, not the expected {expect:08X}')
        struct.pack_into('>I', self.out, at, value)
        self.written[addr] = where

    def function(self, where: str, args: Sequence[str]) -> None:
        arity(where, 'function', args, 3)
        addr = number(where, 'address', args[0])
        size = number(where, 'size', args[1])
        name = args[2]
        if addr % 4:
            raise PatchError(f'{where}: address {addr:08X} is not 4-aligned')
        if size == 0 or size % 4:
            raise PatchError(f'{where}: size {size:X} is not a positive multiple of 4')
        if not NAME.fullmatch(name):
            raise PatchError(f'{where}: {name} is not a C identifier')
        inside = any(s.where is not None and s.text and s.addr <= addr and addr + size <= s.end
                     for s in self.sections())
        if not inside:
            raise PatchError(f'{where}: {addr:08X} to {addr + size:08X} is not inside one added text section')
        if name in self.names:
            raise PatchError(f'{where}: {name} is already named at {self.names[name]}')
        for f in self.inherited + self.functions:
            if addr < f.addr + f.size and f.addr < addr + size:
                raise PatchError(f'{where}: overlaps {f.name} at {f.where}')
        self.functions.append(Function(addr, size, name, where))
        self.names[name] = where

    def dol_bytes(self) -> bytes:
        return bytes(self.out)

    def syms(self) -> bytes:
        new = [(f.addr, f'{f.name} = .text:0x{f.addr:08X}; // type:function size:0x{f.size:X} scope:global')
               for f in self.functions]
        lines = sorted([(s.addr, s.line) for s in self.symbols] + new, key=lambda pair: pair[0])
        return ''.join(f'{line}\n' for _, line in lines).encode('utf-8')


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog='dol_patch.py', description=__doc__.split('\n\n')[0])
    ap.add_argument('dol_in', metavar='IN', help='the DOL to patch, which is left alone')
    ap.add_argument('dol_out', metavar='OUT', help='the patched DOL, with OUT.syms and OUT.modules beside it')
    ap.add_argument('patches', metavar='PATCH', nargs='+', help='patch lists, applied in this order')
    args = ap.parse_args(argv)
    try:
        try:
            with open(args.dol_in, 'rb') as f:
                data = f.read()
        except OSError:
            raise PatchError(f"{args.dol_in}: can't read the DOL")
        # Everything IN brings is read before anything is written, so OUT can be IN.
        syms_path = args.dol_in + '.syms'
        symbols = read_symbols(syms_path) if os.path.exists(syms_path) else []
        modules = read_modules(args.dol_in + '.modules')
        patcher = Patcher(args.dol_in, data, symbols)
        for path in args.patches:
            patcher.apply(path)
        with open(args.dol_out, 'wb') as f:
            f.write(patcher.dol_bytes())
        with open(args.dol_out + '.syms', 'wb') as f:
            f.write(patcher.syms())
        if modules is not None:
            with open(args.dol_out + '.modules', 'wb') as f:
                f.write(modules)
        elif os.path.exists(args.dol_out + '.modules'):
            # One left by an earlier run would be loaded beside the new OUT.
            os.remove(args.dol_out + '.modules')
    except (PatchError, OSError) as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
