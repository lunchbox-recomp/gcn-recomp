// The overlay's font checks: the `test_overlay_fonts` target. Runs with no window, GL, disc or
// game.
//
// A face is found in a font file by its full name in the font's name table, whatever its place
// in a collection, and never by another name, a prefix of its own, a cut-off file or a face that
// lacks a table Dear ImGui's loader needs (runtime/overlay/fonts.cpp). The font files here are
// ones this check writes, a few hundred bytes each with one empty glyph; no real font is kept
// with the repository.
//
// usage: test_overlay_fonts <work dir>
// The work dir must not exist. It's made, and the files are left in it. Exits 0 only if every
// check passes.
#include "overlay/fonts.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

// ---- Font files written here: big-endian sfnt tables, as a single font or a collection.

void put16(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v >> 8));
    b.push_back((uint8_t)v);
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
    put16(b, v >> 16);
    put16(b, v & 0xFFFF);
}

void set16(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = (uint8_t)(v >> 8);
    b[at + 1] = (uint8_t)v;
}

void set32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    set16(b, at, v >> 16);
    set16(b, at + 2, v & 0xFFFF);
}

struct Face {
    std::string name;            // the full name, name ID 4
    bool windows = true;         // in a Windows record, UTF-16BE
    bool mac = false;            // in a Mac Roman record
    std::string family;          // name ID 1, when not empty
    bool has_hhea = true;        // a table Dear ImGui's loader needs
};

struct Table {
    const char* tag;
    std::vector<uint8_t> data;
};

std::vector<uint8_t> name_table(const Face& f) {
    struct Record {
        uint16_t platform, encoding, language, id;
        std::vector<uint8_t> text;
    };
    std::vector<Record> records;
    const auto utf16 = [](const std::string& s) {
        std::vector<uint8_t> out;
        for (unsigned char c : s) put16(out, c);
        return out;
    };
    if (!f.family.empty()) records.push_back({3, 1, 0x409, 1, utf16(f.family)});
    if (f.mac) records.push_back({1, 0, 0, 4, std::vector<uint8_t>(f.name.begin(), f.name.end())});
    if (f.windows) records.push_back({3, 1, 0x409, 4, utf16(f.name)});
    std::vector<uint8_t> b;
    put16(b, 0);
    put16(b, (uint32_t)records.size());
    put16(b, 6 + 12 * (uint32_t)records.size());
    uint32_t offset = 0;
    for (const Record& r : records) {
        put16(b, r.platform);
        put16(b, r.encoding);
        put16(b, r.language);
        put16(b, r.id);
        put16(b, (uint32_t)r.text.size());
        put16(b, offset);
        offset += (uint32_t)r.text.size();
    }
    for (const Record& r : records) b.insert(b.end(), r.text.begin(), r.text.end());
    return b;
}

// The tables of a font with one empty glyph that stb_truetype, Dear ImGui's loader, takes.
std::vector<Table> tables(const Face& f) {
    std::vector<Table> t;
    std::vector<uint8_t> cmap;  // one Windows Unicode subtable, format 4, mapping nothing
    put16(cmap, 0), put16(cmap, 1);
    put16(cmap, 3), put16(cmap, 1), put32(cmap, 12);
    for (uint32_t v : {4u, 24u, 0u, 2u, 2u, 0u, 0u, 0xFFFFu, 0u, 0xFFFFu, 1u, 0u}) put16(cmap, v);
    t.push_back({"cmap", cmap});
    std::vector<uint8_t> head(54, 0);
    set32(head, 0, 0x00010000);
    set32(head, 12, 0x5F0F3CF5);
    set16(head, 18, 1000);  // units per em
    t.push_back({"head", head});
    if (f.has_hhea) {
        std::vector<uint8_t> hhea(36, 0);
        set32(hhea, 0, 0x00010000);
        set16(hhea, 4, 800);
        set16(hhea, 6, (uint16_t)-200);
        set16(hhea, 34, 1);  // one horizontal metric
        t.push_back({"hhea", hhea});
    }
    t.push_back({"hmtx", {0x01, 0xF4, 0, 0}});
    t.push_back({"maxp", {0, 0, 0x50, 0, 0, 1}});
    t.push_back({"loca", {0, 0, 0, 0}});
    t.push_back({"glyf", {0, 0, 0, 0}});
    t.push_back({"name", name_table(f)});
    return t;
}

// The faces as one file: a collection (TTC) if `collection`, else the one face as a font.
// Each face's table directory, then its tables; every offset is from the file's start.
std::vector<uint8_t> font_file(const std::vector<Face>& faces, bool collection) {
    std::vector<uint8_t> b;
    if (collection) {
        b.insert(b.end(), {'t', 't', 'c', 'f'});
        put32(b, 0x00010000);
        put32(b, (uint32_t)faces.size());
        for (size_t i = 0; i < faces.size(); i++) put32(b, 0);
    }
    for (size_t i = 0; i < faces.size(); i++) {
        if (collection) set32(b, 12 + 4 * i, (uint32_t)b.size());
        const std::vector<Table> t = tables(faces[i]);
        const size_t dir = b.size();
        put32(b, 0x00010000);
        put16(b, (uint32_t)t.size());
        put16(b, 0), put16(b, 0), put16(b, 0);
        for (size_t k = 0; k < t.size(); k++) {
            b.insert(b.end(), t[k].tag, t[k].tag + 4);
            put32(b, 0), put32(b, 0), put32(b, 0);
        }
        for (size_t k = 0; k < t.size(); k++) {
            while (b.size() % 4) b.push_back(0);
            set32(b, dir + 12 + 16 * k + 8, (uint32_t)b.size());
            set32(b, dir + 12 + 16 * k + 12, (uint32_t)t[k].data.size());
            b.insert(b.end(), t[k].data.begin(), t[k].data.end());
        }
    }
    return b;
}

int find(const std::vector<uint8_t>& b, const char* name, size_t size = SIZE_MAX) {
    return fonts_find_face(b.data(), std::min(size, b.size()), name);
}

// ---- The checks.

void lookup() {
    const std::vector<Face> four = {{"Test Sans Bold"}, {"Test Sans Demi Bold"}, {"Test Sans Medium"}, {"Test Sans Heavy"}};
    const std::vector<uint8_t> ttc = font_file(four, true);
    check(find(ttc, "Test Sans Heavy") == 3 && find(ttc, "Test Sans Demi Bold") == 1 && find(ttc, "Test Sans Bold") == 0,
          "each face of a collection is found by its full name, at its place in the file");
    std::vector<Face> turned = four;
    std::swap(turned[0], turned[3]);
    check(find(font_file(turned, true), "Test Sans Heavy") == 0, "the same face moved to another place is found there");
    check(find(ttc, "Test Sans") == -1 && find(ttc, "Test Sans Heavy Italic") == -1 && find(ttc, "test sans heavy") == -1 &&
              find(ttc, "") == -1,
          "a prefix, a longer name, another case or no name finds no face");
    const std::vector<uint8_t> ttf = font_file({{"Test Sans Medium"}}, false);
    check(find(ttf, "Test Sans Medium") == 0 && find(ttf, "Test Sans Bold") == -1,
          "a single font, not a collection, is face 0 when its name is the one asked for");
    Face mac{"Test Sans Mac"};
    mac.windows = false;
    mac.mac = true;
    check(find(font_file({{"Test Sans Bold"}, mac}, true), "Test Sans Mac") == 1,
          "a full name only in a Mac Roman record is found too");
    Face family{"Test Sans Light"};
    family.family = "Test Sans Heavy";
    check(find(font_file({family}, true), "Test Sans Heavy") == -1 && find(font_file({family}, true), "Test Sans Light") == 0,
          "a family name (name ID 1) is never taken for a full name");
    Face lacking{"Test Sans Thin"};
    lacking.has_hhea = false;
    check(find(font_file({lacking, {"Test Sans Medium"}}, true), "Test Sans Thin") == -1,
          "a face without a table Dear ImGui's loader needs (hhea) isn't found");
    bool cut = true;
    for (size_t n = 0; n < ttc.size(); n++) cut = cut && find(ttc, "Test Sans Heavy", n) == -1;
    check(cut, "no cut-off copy of the collection finds its last face, at any length");
    const std::vector<uint8_t> junk = {'h', 'e', 'l', 'l', 'o', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::vector<uint8_t> many = ttc;
    set32(many, 8, 0x7FFFFFFF);  // more faces than the file has room to list
    check(find(junk, "hello") == -1 && find(many, "Test Sans Heavy") == -1 && fonts_find_face(nullptr, 0, "x") == -1,
          "a file that isn't a font, a collection listing more faces than it holds and no data find nothing");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_overlay_fonts <work dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    if (std::filesystem::exists(dir) || !std::filesystem::create_directories(dir)) {
        fprintf(stderr, "%s exists or can't be made: give a new directory\n", dir.c_str());
        return 2;
    }
    lookup();
    printf("test_overlay_fonts: %s\n", g_failed ? "FAILED" : "passed");
    return g_failed ? 1 : 0;
}
