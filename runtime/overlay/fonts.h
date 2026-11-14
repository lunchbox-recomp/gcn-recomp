// The overlay's fonts: finding a face in a font file by its full name.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

// The index of the face in the font file `data` whose full name, name ID 4 of its name table in
// a Windows Unicode or a Mac Roman record, is exactly `name`: its place in a collection (TTC),
// or 0 for a single font. -1 if no face has that name, if the file isn't a font or is cut off,
// or if the face lacks a table Dear ImGui's loader needs, which would refuse it.
int fonts_find_face(const uint8_t* data, size_t size, std::string_view name);
