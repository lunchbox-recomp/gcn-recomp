# Disc overlay

A disc overlay lets a folder of files replace or add files on a GameCube disc without changing the image. `recomp/disc_overlay.py` builds one from an image and a folder. The result is a manifest that maps byte ranges of a virtual disc onto host files, and a rewritten FST that points the disc's files at those ranges.

## Building one

```
python3 recomp/disc_overlay.py IMAGE OUT --files DIR [--dol FILE]
```

IMAGE is a plain image or a CISO. The tool writes `OUT/disc-overlay.txt` and `OUT/fst.bin`, then prints one JSON line. DIR and FILE have to sit inside OUT, so the manifest's paths are relative and OUT can move.

- Each file under DIR replaces the disc file at the same path. Names match ignoring case, and a replaced file keeps the disc's spelling. Given case twins on the disc, an exact spelling wins and a name that matches several only by case is refused.
- A file with no match is added with DIR's spelling, inside a new directory when it needs one. New names go in their directory in the order the FST keeps, which compares names in uppercase.
- Dotfiles and dot folders are skipped, so a `.DS_Store` never reaches the disc. An empty folder adds nothing.
- Symbolic links, names that aren't ASCII and a layout that would end past 4 GiB are refused. Nothing is written when anything is refused.
- `--dol` puts a DOL on the virtual disc and points the header at it. The DOL can't also sit inside DIR, since it would go on the disc twice.

The JSON line has `added_files`, `added_bytes`, `replaced_files` and `replaced_bytes`. It also has `fst_growth` for the FST's growth in bytes and `arena_hi_drop` for how far arena hi falls. The same inputs give the same bytes.

## The manifest

```
gcn-disc-overlay 1
base <disc id> <fst offset> <fst size> <fnv1a64 of the base FST>
header <offset> <value>
range <disc offset> <length> <path relative to this file>
```

Numbers are hex in lowercase with no `0x`. The FNV-1a hash is 64 bits and written with 16 digits.

`base` names the image the overlay was made for, by its disc ID and its FST's offset, size and hash. A reader can tell an overlay made for another image from one made for this one.

Each `header` line gives a 32-bit big-endian word to read in place of the disc header's word at that offset. Each `range` line says that the bytes from the disc offset for that length come from the start of the host file. The path is the rest of the line, so it can hold spaces.

Ranges are sorted by disc offset and never overlap. Every other byte of the virtual disc reads from the image.

## Where ranges go

The FST and the DOL go past the end of the image, and so does every added or replaced file. Untouched files keep their offsets and read from the image. A file with no bytes gets an FST entry and no range.

The layout starts at the first 32 KiB boundary at or past the larger of `0x57058000` and the image's size. The first number is 1,459,978,240 bytes, the size of a GameCube disc. A CISO's size is the end of its last stored block.

Each item starts on its own 32 KiB boundary, as Dolphin lays out a virtual disc. The FST comes first, then the DOL if one was given, then the files in the order the FST lists them. The FST's offsets are 32 bits, so nothing may end past 4 GiB.

## The header words

- `0x424` always changes, to the new FST's offset, since the FST always moves.
- `0x428` and `0x42C` change only when the FST grows. `0x428` becomes the new size and `0x42C` becomes the larger of the old max and the new size.
- `0x420` changes with `--dol`.

A table that doesn't grow is padded with zeros to the old size, so `fst.bin` is always as long as the header says. A replacement alone leaves `0x428` and `0x42C` as the disc has them, unless the rebuilt table comes out larger.

## Why fst_max moves and arena hi drops

Boot copies the FST to `(0x817FE000 - fst_max) & ~0x1F` just below the BI2, and that address is also arena hi. The room is fst_max, not the FST's size. An FST that outgrows it would run into the BI2 above.

An addition moves nothing while the new FST still fits inside the old max, and `arena_hi_drop` is 0. Once it passes the old max, fst_max becomes the new size, so the copy moves down and arena hi drops with it, which leaves the game less heap. `arena_hi_drop` is the exact figure, a multiple of 32 that can differ from the rise in fst_max by up to 31 bytes.

Dolphin does the same when it builds a virtual disc for a folder of files. It writes the FST's size to both words, where this tool keeps the larger of the old max and the new size.

## Running with one

`--disc-overlay=FILE` or `GCN_DISC_OVERLAY=FILE` names the manifest, and the flag wins when both are set. The runtime loads it before boot reads the disc header, so boot copies the new FST and the new DOL with no other change. Every later read of an added or replaced file comes from its host file.

Load stops the run, with a message that names the manifest and the line, when anything is wrong. The image's disc ID and its FST's offset, size and hash have to match the `base` line, and the message names both hashes when they don't. A line that isn't in the format above stops it too, and so does a range file that's missing, is a folder or is shorter than its range.

A header word reads as the manifest says wherever a read covers it, even when a range covers the same bytes. A read that starts in the image and ends in a file, or crosses several ranges, is joined from each place it passes. Bytes no range covers read from the image.

The disc interface reads in 32-byte units, so a read of a file whose size isn't one runs on into the gap after it. Where the image has no bytes, which is past a plain image's end, that gap and anything farther past every range and the image reads as zeros, and the read succeeds. A CISO gives zeros there already, so both give the game the same bytes, while a run with no overlay still fails past a plain image's end.

The runtime raises its limit on open files when the count needs it, since macOS starts at 256. A load that still runs out of files says where the limit stops.

Nothing changes after load, so the game's threads and the audio callback read without a lock. `--log=dvd` prints the load with its file count and bytes, then each read the overlay serves.
