// The overlay's fonts: each kind of text's chain of faces (theme.h) loaded into one Dear ImGui
// atlas, which the menu's and the notices' contexts share. A font file is read from where the
// player has it, at run time, and kept in memory for the atlas; it's never copied anywhere.
#pragma once
#include "overlay/theme.h"
#include <cstddef>
#include <cstdint>
#include <string_view>

struct ImFont;
struct ImFontAtlas;

enum class FontRole { title, tabs, items, body, footer };

// The index of the face in the font file `data` whose full name, name ID 4 of its name table in
// a Windows Unicode or a Mac Roman record, is exactly `name`: its place in a collection (TTC),
// or 0 for a single font. -1 if no face has that name, if the file isn't a font or is cut off,
// or if the face lacks a table Dear ImGui's loader needs, which would refuse it.
int fonts_find_face(const uint8_t* data, size_t size, std::string_view name);

// A new atlas, from here on the one fonts_atlas gives, with a font for each kind of text: the
// first face of its chain that loads, or else Dear ImGui's own embedded font. Each face that
// doesn't load says so and where the text goes next in one `[overlay] font` line on stderr; a
// theme that lists no face at all says so in one line. Kinds that take the same face share
// one font. An atlas is never freed, as the contexts on it never are.
// fonts_find_face checks each of its reads against the file's size, but once a face loads,
// stb_truetype, Dear ImGui's loader, reads its tables (cmap, loca, glyf, hmtx and the rest) with
// no bounds. So a font file a theme lists from its own path is read with no such guarantee: one
// that's broken can be read past its end.
void fonts_build(const OverlayFonts& fonts);

ImFontAtlas* fonts_atlas();

// The font for a kind of text, from the last fonts_build.
ImFont* fonts_role(FontRole role);

// The atlas's own update for a frame, which no context makes as none owns the atlas: once each
// time the overlay lays out its frame, before the contexts' NewFrame.
void fonts_new_frame();
