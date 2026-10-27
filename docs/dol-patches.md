# DOL patches

A DOL patch changes a game's executable before the recompiler reads it. `recomp/dol_patch.py` applies patch lists to a DOL, adds new sections and names the new functions in them. The recompiled game can't run PowerPC the recompiler didn't translate, so any new code has to be in the DOL before it's recompiled.

## Running it

```
python3 recomp/dol_patch.py IN OUT PATCH...
```

The tool writes `OUT` and `OUT.syms`, then exits 0. It applies the patch lists in the order given and leaves `IN` alone.

A refused patch exits 1 and prints one line, `file:line: reason`, for the first problem it finds. Nothing is written then, so an earlier `OUT` stays as it was. Bad arguments exit 2, and the same inputs give the same bytes.

## The patch list

```
# comment
dol <sha1 of IN>
section text|data <addr> <size> [<file>]
word <addr> <value> <expect>
function <addr> <size> <name>
```

Numbers are hex and fit in 32 bits, and a `0x` prefix is optional. A `#` starts a comment when it's at the start of a line or after a space. Blank lines are skipped.

A list is UTF-8, with or without a byte order mark. Only a newline ends a line, so a comment can hold any other separator.

- `dol` comes first in every list, and its SHA-1 has to match `IN`. Patches apply to a copy, so the check that the build starts from the supported DOL never relaxes.
- `section` adds a section. It's zero-filled, or it starts with the bytes of `<file>`. The file's path is the rest of the line, so it can hold spaces.
- `word` replaces one word, and `expect` is the word it replaces.
- `function` names code in an added text section, so the recompiler compiles it.

A section starts 32-byte aligned in main RAM and ends below `0x81800000`, where `rel.py` links modules. Its size is a positive multiple of 4, and it overlaps no section of the DOL, no section an earlier line added and no bss. It takes the first free slot of its kind, out of the 7 text and 11 data slots a DOL header has.

A section's file has to be a regular file inside the folder that holds the list, named by a relative path. An absolute path is refused, and so is a `..` or a symlink that leaves the folder. A pipe or a device is refused before it's opened.

Boot writes the low-memory words and the top 8 KiB after it loads the sections (`runtime/boot.cpp`), so a section can't overlap them. The SDK's usual layout puts the stacks right after the bss and arena lo after them, so a section there needs its list to raise arena lo past the stacks, not just past the bss, with `word` lines that are game-specific. Boot also copies the FST just under the top 8 KiB, at a place the disc's FST size sets, so the tool can't check that range and a list stays clear of it.

A word is 4-aligned and sits inside a section, whether the DOL's or an added one. `expect` is checked against the DOL as patched so far, so a word in a new zero-filled section expects 0. A refused `expect` prints the word the DOL holds.

A function lies inside one added text section that an earlier line added, and it overlaps no other function. Its name is a C identifier, since it ends up in the generated code. No two new functions share a name, whether they're in one list or two.

A name also has to stay clear of the names in a game's `hle.txt` and of `OSLink`, `OSLinkFixed` and `OSUnlink`. The tool can't see those tables, and `recomp.py` would send the new code to the hook instead of running it.

Lists apply in the order given. A section belongs to the list that adds it, so a `word` from a later list can't land inside. Two lines that write one word are refused, and so are two sections that overlap, each with a message that names both lines.

## What it writes

`OUT` is `IN` with the changed words, and the added sections at the end of the file on 32-byte boundaries. The header's first free slot of each kind points at them, and the rest of `IN` isn't moved.

`OUT.syms` holds the symbol lines of `IN.syms` if it exists, then the new functions in dtk's symbols format, sorted by address. Lines that `recomp.py` skips, such as comments, are dropped. `recomp.py` reads `<dol>.syms` beside any DOL it's given, so the functions compile like the game's own.

A new function can't overlap a function of `IN.syms` or take one of its names, and the message names the line. The file is always written, and it's empty when there's nothing to carry or add.

`IN.modules` is copied to `OUT.modules` unchanged when it exists, since `recomp.py` reads that file beside a DOL too. If `IN` has none, a run removes an `OUT.modules` left by an earlier run, so `recomp.py` never loads a stale one.

Both files carry through, so a second run on the first run's output keeps what the first run named. A run may write over its input, because everything is read before anything is written.

## Hooking from the middle of a function

A hook branches from inside a function into new code and back. Take a function at `80010000`, where the word at `80010004` is `li r3, 1`. The new code runs that instruction and branches back to `80010008`.

```
dol <sha1 of IN>
word 80010004 4800FFFC 38600001      # b 80020000
section text 80020000 20 hook.bin
function 80020000 8 hook
```

Here `hook.bin` holds `38600001` and `4BFF0004`, which is `b 80010008`. The recompiler already handles this.

A branch into the middle of another function registers an entry there, and a branch out of a function becomes a C call followed by `RET()`. The hooked word calls the new function and returns. The new function ends by calling the entry at `80010008`, so the rest of the host function runs as a nested call.

`recomp/test_dol_patch.py` pins that against `recomp.py`.

## Calling it from a build

`gcn_add_game` runs a `DOL_FIXUP` script as `script main.dol out.dol image` after it checks the DOL's hash, and the recompiler reads `out.dol` instead. A script that applies patch lists is a few lines.

```python
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
tool = os.path.join(here, 'gcn-recomp', 'recomp', 'dol_patch.py')
dol_in, dol_out = sys.argv[1:3]
sys.exit(subprocess.run([sys.executable, tool, dol_in, dol_out, os.path.join(here, 'code.txt')]).returncode)
```

The fixup runs at configure time, and CMake runs it again only when the script changes. Editing `code.txt` or a section's binary won't rebuild by itself. List them as configure dependencies in the `CMakeLists.txt` that calls `gcn_add_game`, with the script and the files beside it.

```cmake
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/code.txt ${CMAKE_CURRENT_SOURCE_DIR}/hook.bin)
```

A game with `RELS` needs `rel.py link` to carry `.syms` through first. It runs after the fixup and writes its own `.syms` from the modules' symbols, so the patch tool's file beside its input doesn't survive. Link also adds a text and a data section of its own, so a patch list for such a game leaves a slot of each kind free.

## Why the boot DOL has to be the patched one

Boot copies each DOL section from the disc into RAM (`runtime/boot.cpp:17-27`). The DOL it loads has to be the one the recompiler read, byte for byte. Otherwise the added sections and changed data words never reach RAM, and the game reads zeros or the old values where it expects the patch.

The disc overlay serves it. `disc_overlay.py --dol OUT` puts the patched DOL on the virtual disc and points header word `0x420` at it, so boot loads it with no change to `boot.cpp` (`docs/disc-overlay.md`). The disc itself stays vanilla.
