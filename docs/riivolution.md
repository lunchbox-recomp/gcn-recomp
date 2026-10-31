# Riivolution mods

`recomp/riivolution.py` turns a folder of Riivolution mod XMLs and their files into a disc overlay and a list of RAM patches. File and folder patches become the overlay (`docs/disc-overlay.md`) and memory patches become the list. The image is never touched.

The rules are the ones Dolphin follows for a GameCube disc (`DiscIO/RiivolutionParser.cpp` and `RiivolutionPatcher.cpp`), so a release that loads there reads the same way here. Where Dolphin skips something quietly, this reader usually stops and names it.

## The layout

ROOT is the SD root a release unzips into. The XMLs sit directly in `ROOT/riivolution/`, which is where Dolphin reads them. Their files sit wherever the XMLs point.

```
ROOT/
  riivolution/
    pack.xml
  pack/
    files/
    payload.bin
```

An XML is a package when its root element is `wiidisc`. Other XMLs, like the settings Riivolution keeps in a `config` folder, aren't packages and are left alone. No folder in `riivolution/` is entered, so XMLs inside a release's data folders are never read as packages.

A symbolic link to an XML isn't followed, and it shows up in the list with an error. A symbolic link for `riivolution/` itself is refused by both `list` and `apply`, since it could point anywhere.

A name that starts with a dot is skipped, so `.DS_Store` and the `._` files of a macOS zip never get in. A folder patch with `external="/"` skips an OUT folder named `.out` the same way.

## Running it

```
python3 recomp/riivolution.py list ROOT
python3 recomp/riivolution.py apply IMAGE ROOT OUT [--choice XML:OPTION=CHOICE ...]
```

`list` prints one JSON line, `{"packages": [...]}`. Each package has `xml`, its file name in `ROOT/riivolution/`. It also has `options`, or an `error` when its XML can't be used.

An option has an `id`, a `name`, the `section` it sits in and its `default`. It also has `choices`, the names of its choices in order. The `id` is the one the XML gives, or its section's name and its own name joined when the XML gives none, and two options in one package can't share one.

`list` doesn't read an image, so it shows every package whatever disc it fits.

`apply` takes a plain image or a CISO. It writes `OUT/disc-overlay.txt` and `OUT/fst.bin` when a file patch applies, and `OUT/ram-patches.txt` when a memory patch does. It removes any of the three that doesn't apply, and it makes OUT only when something goes in it.

Then it prints one JSON line.

| Key | Holds |
|---|---|
| `packages` | the XML names of the packages that are on |
| `files_added`, `files_replaced` | the disc files the overlay adds and replaces |
| `ram_patches`, `ram_bytes` | the lines in the list and the bytes they write |
| `overlay` | `disc_overlay.py`'s summary, or null with no file patch |

| Exit | Meaning |
|---|---|
| 0 | done |
| 1 | a package was refused or the overlay couldn't be built, with one `error:` line on stderr naming the XML and the element |
| 2 | bad arguments, including a `--choice` that names no package, no option or a number past the choices |

On exit 1 the command also prints one JSON line on stdout, `{"error": "...", "packages": [...]}`. The `error` is the stderr message without `error: `, and `packages` holds the XML names at fault, both for a conflict. It's empty when no package is at fault, and `list` prints the same line when `riivolution/` is a link.

Nothing is written when anything is refused, so earlier outputs stay as they were. The same inputs give the same bytes.

## Where apply writes

OUT usually sits inside ROOT, which a release fills, so `apply` checks it before it writes. It refuses when OUT is a symbolic link, or when one of its three output names is a symbolic link or a folder. A link in a folder above OUT is the player's own and is followed.

Each output goes to a new temp file that's never opened through a link. Every temp file is made before any output is replaced, so a write that fails leaves the old outputs as they were.

A name with a control character (C0 or DEL) is refused, since a manifest line can't hold one. That covers a part of an `external` path, a name in the host folder of a folder patch and a new disc name. It also covers the folders above ROOT that show in a manifest path.

## Choices

A package offers options, and an option offers choices. `--choice` takes the XML name as `list` prints it, the option's `id` and a choice number, as in `--choice pack.xml:music=2`. Choice 1 is the first choice. Choice 0 turns the option off.

An option with no `--choice` uses its `default`, which is 0 when the XML gives none. A package is on when its chosen choices name a patch. Settings that Riivolution saves in `riivolution/config/` aren't read.

A choice names patches by id. They apply in option order, and a patch that several choices name applies once, where it first comes. When two `patch` elements share an id, the first is the one a choice reaches.

A package that fits another disc is never on, whatever its choices say. A package with an error is left out.

A `--choice` above 0 that names a package with an error stops `apply` with that error. Choice 0 is allowed and does nothing, so an off that was saved for a package that later broke doesn't stop the rest.

## Which disc a package fits

The `id` element of the XML narrows a package to some discs, with Dolphin's `IsValidForGame` (`RiivolutionParser.cpp:235-259`). An attribute or child that isn't there doesn't look at that part.

| In the XML | Matches |
|---|---|
| `game` | the start of the six character disc ID |
| `developer` | the last two characters of the ID |
| `region` children, by `type` | the ID's fourth character |
| `disc` | the header byte at 6, the disc number |
| `version` | the header byte at 7, the revision |

## Disc files

A `file` patch names a disc file in `disc` and a host file in `external`. A `folder` patch names a disc folder and a host folder, and each file in the host folder is patched in turn, in name order.

- A disc path that starts with `/` is a path from the disc's root. Each part matches ignoring case and keeps the disc's spelling. With `create="true"` a path with no file gets one, and the folders it needs are made as the XML spells them.
- Any other disc path is a name, and it matches the first file of that name in FST order, in any folder. A folder patch whose disc path doesn't start with `/` therefore reads only the top of its host folder, since nothing below it could match by name.
- A path with nothing to match is skipped, which is how one XML can fit several versions of a disc. A path whose parts are files where it wants folders, or the other way round, matches nothing.
- The new file is the whole host file, or its first `length` bytes. A later patch on the same disc file wins, and in each patch the files go first, then the folders.
- A folder patch honors `recursive`, `create`, `resize` and `length`. A new name has to be plain ASCII, and a symbolic link in a host folder is refused.

A patch can't leave part of the old file in place. `resize="false"` keeps the old file's tail when the new bytes end before the old file does, and the overlay has no way to point into the disc image for that, so it's refused. A new file that's as long as the old one, or longer, works as it does by default.

## RAM patches

A `memory` patch writes bytes into RAM. Its `offset` is ORed with `0x80000000`, as Dolphin does (`RiivolutionPatcher.cpp:555`). Every byte it writes or checks has to lie in main RAM, `80000000` to `817fffff`.

The bytes come from `value`, which is hex with an optional `0x`, or from the file that `valuefile` names. `original` is optional and holds the bytes the patch expects to find. A file is read whole, and one bigger than main RAM is refused before it's read.

```
gcn-ram-patches 1
base TEST00
patch 80003100 101112131415161718191a1b1c1d1e1f
patch 80003104 60000000 14151617
```

Numbers and byte strings are lowercase hex with no `0x`, and the address has eight digits. `base` is the image's disc ID. A `valuefile` becomes one `patch` line whose value is the file's bytes.

The lines keep the order the patches turn on, with the packages in the order of their XML paths. A line's `original` is what RAM holds once the lines before it are in. That's why a patch can sit inside the bytes a `valuefile` patch wrote, and why overlaps inside one package are fine.

## Two packages

Two packages that are on can't reach the same disc file or the same RAM byte, and the error names both.

- A disc file is the same file whichever case or route finds it.
- A folder patch counts file by file, and a created file counts like any other.
- A byte that a patch only checks with `original` doesn't count.

## Paths

A path in `external`, `valuefile` or `root` follows Dolphin (`RiivolutionPatcher.cpp:29-161`).

- A path that starts with `/` starts at ROOT, so an absolute host path means a path under ROOT. Any other path starts at the patch's `root`, or at the XML's folder when it has none.
- A `root` that starts with `/` starts at ROOT too. Any other `root` starts at the XML's folder.
- `.` is skipped, and extra slashes are ignored. `..` goes back up the path so far, and it's refused where the path began.
- A part made of three or more dots is refused, since some file systems read it as several `..`.
- Each part matches the host's spelling ignoring ASCII case. An exact spelling wins, and two names that differ only by case are refused when nothing matches exactly.
- A symbolic link anywhere in the path is refused. Every part has to exist.
- A part with a control character is refused.
- A `file` has to be a file, and a `folder` or a `root` has to be a folder.

The variables `{$__gameid}`, `{$__region}` and `{$__maker}` are filled from the disc ID in a `root`, a `disc`, an `external` and a `valuefile`, as Dolphin does (`RiivolutionParser.cpp:311-316`). Any other variable is refused, and a `{$` that never closes is left as it is.

## Why the manifest's paths can start with `..`

The files of a release stay where the player unzipped them. `disc_overlay.py` wants every file inside its output folder, which a mods folder isn't, so this tool calls `make_overlay` directly with paths from OUT to each host file.

OUT sits next to the files, so a path looks like `../pack/files/x.dat`. The runtime joins the manifest's folder and the path with no `..` check (`runtime/hw/disc_overlay.cpp:327-346`), and it opens each host file once at boot. A manifest only works with OUT in the same place beside the files, so `apply` runs again after either one moves.

## What is refused

Nothing in a refused package is used. The message names the XML and the element, as in `<savegame external="x"> in pack.xml isn't supported`.

| Element or attribute | Why |
|---|---|
| `search` or `ocarina` on a memory patch | They look for bytes in RAM as the game runs |
| `savegame` | The engine doesn't redirect saves |
| `dolphin_sys_file`, `dolphin_sys_folder` | They patch Dolphin's own files |
| `macros`, `param` | They rewrite options and paths. Only the three ID variables are filled |
| `offset` on a file, apart from its low two bits | A patch in the middle of a file needs the disc file around it |
| `fileoffset` | It starts in the middle of the host file |
| a `file` whose disc path is `main.dol` | The executable isn't swapped |
| `length` past the end of the host file | The file would need zeros the overlay doesn't hold |
| `resize="false"` on a file that ends early | See above |

An overlay item is one host file, or its first bytes. A patch that needs more is refused.

A message cuts any long value it quotes to 60 characters, so a package can't make a line as long as its XML.

The reader also stops where Dolphin quietly skips.

- A path that can't be resolved or doesn't exist.
- A number or hex string that doesn't parse.
- A memory patch with no `offset` or no value, or with both `value` and `valuefile`.
- A memory patch whose bytes or `original` fall outside main RAM.
- A disc path with an empty or dot part.
- A choice that names a patch the XML doesn't have.

## The XML itself

A file over 1 MiB is refused. A file that isn't UTF-8, holds a NUL or a control character other than tab, CR and LF, or has a DOCTYPE or an entity is refused before it's parsed.

The NUL rule is what stops UTF-16 and UTF-32 text with no byte order mark, since its bytes pass as UTF-8 and the scan for a DOCTYPE can't read them. The parser also refuses a DOCTYPE, an entity declaration and an external entity itself, in case one gets past the scan. No entity is ever expanded.

A number is decimal or `0x` hex, with at most 10 digits after any `0x`. A leading zero doesn't make it octal, so `010` is ten. A longer number, or one that doesn't fit in 32 bits, is refused.

An XML whose root isn't `wiidisc` is left out of the list. A `wiidisc` XML that isn't version 1 is listed with an error. Elements in a patch that the reader doesn't know are ignored.

## Where the code is

| File | Job |
|---|---|
| `recomp/riivolution.py` | The command line, the package scan and the outputs |
| `recomp/riivolution_xml.py` | One XML into options, patches and the disc filter |
| `recomp/riivolution_paths.py` | One external path into a host path |
| `recomp/riivolution_patch.py` | Patches onto the disc tree and the RAM list |
| `recomp/test_riivolution.py` | The checks, which write their own images and packages |
