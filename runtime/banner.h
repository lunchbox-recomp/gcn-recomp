// The disc's banner: opening.bnr in the root of its file table, a 96x32 picture with
// transparency that every GameCube disc carries, which the boot splash shows (splash.h).
//
// The FST's offset and size are big-endian words at 0x424 and 0x428 of the disc. Its entries
// are 12 bytes: the first word's high byte says a directory, its low 24 bits are the name's
// offset in the string table that follows the entries; the second is a file's offset, and the
// third its size, or for a directory the index past its last entry. Entry 0 is the root, whose
// third word is the count. The banner is "BNR1" (one block of names, 0x1960 bytes) or "BNR2"
// (six, 0x1FA0), with its picture at 0x20: 0x1800 bytes of big-endian RGB5A3 in 4x4 tiles, 24
// across and 8 down, each tile's 16 pixels row by row. A pixel with bit 15 set is RGB555 and
// opaque, otherwise A3RGB444.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

constexpr int kBannerW = 96, kBannerH = 32;

// Reads `n` bytes at disc offset `off` into `dst`; false on a short read.
using DiscRead = std::function<bool(uint64_t off, void* dst, uint32_t n)>;

// Decodes a whole opening.bnr into `rgba`, kBannerW x kBannerH pixels of four bytes, top row
// first, with straight (not premultiplied) alpha. False for another magic, or a size that isn't
// the one its magic has.
bool banner_decode(const uint8_t* bnr, size_t size, uint8_t* rgba);

// Where the file named `name` is among the root's own entries (not in a directory's): false
// when it isn't there, or the table is under 12 bytes or over 1 MiB, its entries don't fit it,
// its last byte isn't a NUL or an entry points outside it.
bool fst_find_root_file(const uint8_t* fst, size_t size, const char* name, uint32_t* off, uint32_t* len);

// The disc's opening.bnr, decoded as banner_decode does. Reads its 8 bytes of FST place, the
// FST and the banner, and nothing else. False when any of those fails.
bool banner_read(const DiscRead& read, uint8_t* rgba);
