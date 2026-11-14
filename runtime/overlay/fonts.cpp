// The overlay's fonts (fonts.h). A face is found by its full name rather than its index, as a
// collection's order can change between versions of the system that ships it, and Dear ImGui
// is told the index as ImFontConfig::FontNo.
#include "overlay/fonts.h"
#include <string>

namespace {

// ---- Reading a font file's tables, every read checked against the file's size.

struct Bytes {
    const uint8_t* data;
    size_t size;

    bool has(size_t at, size_t n) const { return at <= size && n <= size - at; }
    uint32_t u16(size_t at) const { return (uint32_t)data[at] << 8 | data[at + 1]; }
    uint32_t u32(size_t at) const { return u16(at) << 16 | u16(at + 2); }
};

uint32_t tag(const char* t) { return (uint32_t)(uint8_t)t[0] << 24 | (uint8_t)t[1] << 16 | (uint8_t)t[2] << 8 | (uint8_t)t[3]; }

struct Span {
    size_t at = 0, size = 0;
    bool found = false;
};

// The table `t` of the face whose table directory is at `face`, if the file holds all of it.
Span table(const Bytes& b, size_t face, const char* t) {
    const uint32_t want = tag(t);
    const size_t count = b.u16(face + 4);
    for (size_t i = 0; i < count; i++) {
        const size_t rec = face + 12 + 16 * i;
        if (b.u32(rec) != want) continue;
        const size_t at = b.u32(rec + 8), size = b.u32(rec + 12);
        if (b.has(at, size)) return {at, size, true};
        return {};
    }
    return {};
}

// UTF-16BE text as UTF-8.
std::string utf8(const Bytes& b, size_t at, size_t size) {
    std::string out;
    for (size_t i = 0; i + 1 < size; i += 2) {
        uint32_t c = b.u16(at + i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < size) {
            const uint32_t low = b.u16(at + i + 2);
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | c >> 6);
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | c >> 12);
            out += (char)(0x80 | (c >> 6 & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | c >> 18);
            out += (char)(0x80 | (c >> 12 & 0x3F));
            out += (char)(0x80 | (c >> 6 & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Whether the name table at `name` gives `want` as a full name (ID 4), in a Unicode or Windows
// Unicode record, or a Mac Roman one that's all ASCII.
bool named(const Bytes& b, const Span& name, std::string_view want) {
    if (name.size < 6) return false;
    const size_t count = b.u16(name.at + 2), strings = name.at + b.u16(name.at + 4);
    if (6 + 12 * count > name.size) return false;
    for (size_t i = 0; i < count; i++) {
        const size_t rec = name.at + 6 + 12 * i;
        const uint32_t platform = b.u16(rec), encoding = b.u16(rec + 2), id = b.u16(rec + 6);
        const size_t size = b.u16(rec + 8), at = strings + b.u16(rec + 10);
        if (id != 4 || at < name.at || at + size > name.at + name.size) continue;
        if (platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10))) {
            if (utf8(b, at, size) == want) return true;
        } else if (platform == 1 && encoding == 0) {
            const std::string_view text((const char*)b.data + at, size);
            bool ascii = true;
            for (char c : text) ascii = ascii && (unsigned char)c < 0x80;
            if (ascii && text == want) return true;
        }
    }
    return false;
}

// Whether the face has what stb_truetype, Dear ImGui's loader, needs to take it: cmap, head,
// hhea and hmtx, glyf and loca or else CFF, and a cmap subtable it reads, Unicode or Windows
// Unicode. Without them Dear ImGui would refuse the face, with an assertion in a debug build.
bool loadable(const Bytes& b, size_t face) {
    const Span cmap = table(b, face, "cmap"), head = table(b, face, "head"), hhea = table(b, face, "hhea");
    if (!cmap.found || head.size < 54 || hhea.size < 36 || !table(b, face, "hmtx").found) return false;
    if (!(table(b, face, "glyf").found && table(b, face, "loca").found) && !table(b, face, "CFF ").found) return false;
    if (cmap.size < 4 || 4 + 8 * (size_t)b.u16(cmap.at + 2) > cmap.size) return false;
    for (size_t i = 0; i < b.u16(cmap.at + 2); i++) {
        const size_t rec = cmap.at + 4 + 8 * i;
        const uint32_t platform = b.u16(rec), encoding = b.u16(rec + 2);
        if ((platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10))) && b.u32(rec + 4) < cmap.size)
            return true;
    }
    return false;
}

bool face_is(const Bytes& b, size_t face, std::string_view want) {
    if (!b.has(face, 12) || !b.has(face + 12, 16 * (size_t)b.u16(face + 4))) return false;
    const Span name = table(b, face, "name");
    return name.found && named(b, name, want) && loadable(b, face);
}

}  // namespace

int fonts_find_face(const uint8_t* data, size_t size, std::string_view name) {
    if (!data || name.empty()) return -1;
    const Bytes b{data, size};
    if (!b.has(0, 12)) return -1;
    const uint32_t version = b.u32(0);
    if (version == tag("ttcf")) {
        const size_t count = b.u32(8);
        if (!b.has(12, 4 * count)) return -1;
        for (size_t i = 0; i < count; i++)
            if (face_is(b, b.u32(12 + 4 * i), name)) return (int)i;
        return -1;
    }
    if (version != 0x00010000 && version != tag("true") && version != tag("OTTO")) return -1;
    return face_is(b, 0, name) ? 0 : -1;
}
