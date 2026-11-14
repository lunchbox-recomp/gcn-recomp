// The overlay's font checks: the `test_overlay_fonts` target. Runs with no window, GL, disc or
// game, on Dear ImGui contexts of its own.
//
// A face is found in a font file by its full name in the font's name table, whatever its place
// in a collection, and never by another name, a prefix of its own, a cut-off file or a face that
// lacks a table Dear ImGui's loader needs (runtime/overlay/fonts.cpp). The font files here are
// ones this check writes, a few hundred bytes each with one empty glyph; no real font is kept
// with the repository. Each kind of text takes the first face of its chain that loads, then
// Dear ImGui's own, with one [overlay] font line on stderr for each step down, and a theme that
// lists no font at all takes Dear ImGui's own with one line. An atlas no context is on is freed
// when the next is built. The menu's and the notices'
// contexts share one atlas, and the text of every kind at sizes up to 2160 lines at twice the
// pixel density, 1440 in Dear ImGui's own font, is baked into its 8 MB texture, no glyph falling
// back. Where the Mac's own Avenir Next is installed, the
// default chains load from it with no step down, read where it is and never copied.
//
// usage: test_overlay_fonts <work dir>
// The work dir must not exist. It's made, and the files are left in it. Exits 0 only if every
// check passes.
#include "overlay/fonts.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>
#include "imgui.h"
#include "imgui_internal.h"

namespace {

int g_failed;

// Dear ImGui's allocations not yet freed, as main has it allocate through these.
long g_live;
void* counted_alloc(size_t size, void*) {
    g_live++;
    return malloc(size);
}
void counted_free(void* p, void*) {
    g_live -= p != nullptr;
    free(p);
}

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
// Each face's table directory, then its tables, or in a collection with `tables_first` its
// tables and then its directory; every offset is from the file's start.
std::vector<uint8_t> font_file(const std::vector<Face>& faces, bool collection, bool tables_first = false) {
    std::vector<uint8_t> b;
    if (collection) {
        b.insert(b.end(), {'t', 't', 'c', 'f'});
        put32(b, 0x00010000);
        put32(b, (uint32_t)faces.size());
        for (size_t i = 0; i < faces.size(); i++) put32(b, 0);
    }
    for (size_t i = 0; i < faces.size(); i++) {
        const std::vector<Table> t = tables(faces[i]);
        std::vector<uint32_t> at(t.size());
        const auto put_tables = [&] {
            for (size_t k = 0; k < t.size(); k++) {
                while (b.size() % 4) b.push_back(0);
                at[k] = (uint32_t)b.size();
                b.insert(b.end(), t[k].data.begin(), t[k].data.end());
            }
        };
        if (tables_first) put_tables();
        while (b.size() % 4) b.push_back(0);
        if (collection) set32(b, 12 + 4 * i, (uint32_t)b.size());
        const size_t dir = b.size();
        put32(b, 0x00010000);
        put16(b, (uint32_t)t.size());
        put16(b, 0), put16(b, 0), put16(b, 0);
        for (size_t k = 0; k < t.size(); k++) {
            b.insert(b.end(), t[k].tag, t[k].tag + 4);
            put32(b, 0), put32(b, 0), put32(b, 0);
        }
        if (!tables_first) put_tables();
        for (size_t k = 0; k < t.size(); k++) {
            set32(b, dir + 12 + 16 * k + 8, at[k]);
            set32(b, dir + 12 + 16 * k + 12, (uint32_t)t[k].data.size());
        }
    }
    return b;
}

void write_file(const std::string& path, const std::vector<uint8_t>& b) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(b.data(), 1, b.size(), f);
        fclose(f);
    }
}

uint32_t get32(const std::vector<uint8_t>& b, size_t at) {
    return (uint32_t)b[at] << 24 | (uint32_t)b[at + 1] << 16 | (uint32_t)b[at + 2] << 8 | b[at + 3];
}

// Where the table `t` of a single font (not a collection) is listed in its directory: the
// entry's tag, then its checksum, offset and length.
size_t entry(const std::vector<uint8_t>& b, const char* t) {
    const size_t count = (size_t)b[4] << 8 | b[5];
    for (size_t k = 0; k < count; k++)
        if (std::equal(t, t + 4, b.begin() + (ptrdiff_t)(12 + 16 * k))) return 12 + 16 * k;
    return SIZE_MAX;
}

// The face named `name` in the first `size` bytes of `b`, copied to a buffer of just that size,
// so that a read past its end is one the address sanitizer sees.
int find(const std::vector<uint8_t>& b, const char* name, size_t size = SIZE_MAX) {
    const std::vector<uint8_t> cut(b.begin(), b.begin() + (ptrdiff_t)std::min(size, b.size()));
    return fonts_find_face(cut.data(), cut.size(), name);
}

// ---- What's said on stderr while `fn` runs, a line at a time.

template <class Fn>
std::vector<std::string> said(const std::string& path, Fn fn) {
    fflush(stderr);
    const int saved = dup(2);
    FILE* f = freopen(path.c_str(), "w", stderr);
    fn();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    if (f) clearerr(stderr);
    std::vector<std::string> lines;
    if (FILE* in = fopen(path.c_str(), "r")) {
        char line[512];
        while (fgets(line, sizeof(line), in)) lines.push_back(line);
        fclose(in);
    }
    for (const std::string& l : lines) printf("  said: %s", l.c_str());
    return lines;
}

bool has(const std::string& line, const std::string& part) { return line.find(part) != std::string::npos; }

bool all_start(const std::vector<std::string>& lines, const char* prefix) {
    for (const std::string& l : lines)
        if (!l.starts_with(prefix)) return false;
    return true;
}

constexpr FontRole kRoles[] = {FontRole::title, FontRole::tabs, FontRole::items, FontRole::body, FontRole::footer};

// The face a kind of text took: its name and its index in its file, or "" for Dear ImGui's own.
std::string took(FontRole r, int* no = nullptr) {
    const ImFont* f = fonts_role(r);
    if (!f || f->Sources.empty()) return "?";
    if (no) *no = (int)f->Sources[0]->FontNo;
    const std::string name = f->Sources[0]->Name;
    return name == "ProggyForever.ttf" ? "" : name;
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
    Face mac_bold = mac;
    mac_bold.name = "Test Sans Mac Bold";
    check(find(font_file({{"Test Sans Bold"}, mac}, true), "Test Sans Mac") == 1 &&
              find(font_file({{"Test Sans Bold"}, mac_bold}, true), "Test Sans Mac") == -1,
          "a full name only in a Mac Roman record is found too, and a longer one there isn't taken for it");
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
    // Here the face's directory comes last, so a cut through it leaves every table it lists.
    const std::vector<uint8_t> last = font_file({{"Test Sans Medium"}}, true, true);
    check(find(last, "Test Sans Medium") == 0 && find(last, "Test Sans Medium", last.size() - 1) == -1,
          "a face whose table directory is cut off isn't found, though the tables it lists are whole");
    // A full name whose string runs past the name table, the file's last: into bytes the file
    // has and the table doesn't, the rest of the name, or past the file's end.
    const std::string heavy_name = "Test Sans Heavy";
    const std::vector<uint8_t> heavy = font_file({{heavy_name}}, false);
    const size_t name = entry(heavy, "name"), name_at = get32(heavy, name + 8);
    std::vector<uint8_t> short_table = heavy, long_string = heavy;
    set32(short_table, name + 12, get32(heavy, name + 12) - 4);
    set16(long_string, name_at + 6 + 8, 2 * (uint32_t)heavy_name.size() + 2);  // its one record's length
    check(find(heavy, heavy_name.c_str()) == 0 && find(short_table, heavy_name.c_str()) == -1 &&
              find(long_string, heavy_name.c_str()) == -1,
          "a full name whose string runs past the name table isn't read, into the file's next bytes or past its end");
    // A cmap listing one subtable more than it has room for, and one whose only subtable starts
    // at the table's end.
    const size_t cmap = entry(heavy, "cmap"), cmap_at = get32(heavy, cmap + 8), cmap_size = get32(heavy, cmap + 12);
    std::vector<uint8_t> count_past = heavy, at_end = heavy;
    set16(count_past, cmap_at + 2, (uint32_t)(cmap_size - 4) / 8 + 1);
    set32(at_end, cmap_at + 4 + 4, (uint32_t)cmap_size);
    check(find(count_past, heavy_name.c_str()) == -1 && find(at_end, heavy_name.c_str()) == -1,
          "a cmap listing more subtables than it has room for, or a subtable starting at its end, isn't taken");
    const std::vector<uint8_t> junk = {'h', 'e', 'l', 'l', 'o', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::vector<uint8_t> two = ttc, three = ttc;
    set32(two, 4, 0x00020000);
    set32(three, 4, 0x00030000);
    check(find(two, "Test Sans Heavy") == 3 && find(three, "Test Sans Heavy") == -1,
          "a collection of version 2.0 is read as 1.0 is, and one of another version isn't read");
    std::vector<uint8_t> many = ttc;
    set32(many, 8, 0x7FFFFFFF);  // more faces than the file has room to list
    check(find(junk, "hello") == -1 && find(many, "Test Sans Heavy") == -1 && fonts_find_face(nullptr, 0, "x") == -1,
          "a file that isn't a font, a collection listing more faces than it holds and no data find nothing");
}

void chains(const std::string& dir) {
    const std::string ttc = dir + "/test.ttc", gone = dir + "/missing.ttc";
    write_file(ttc, font_file({{"Test Sans Bold"}, {"Test Sans Demi Bold"}, {"Test Sans Medium"}, {"Test Sans Heavy"}}, true));
    OverlayFonts f;
    f.title = {{gone, "Test Sans Heavy"}, {ttc, "Test Sans Heavy"}};
    f.tabs = {{ttc, "Test Sans Demi Bold"}};
    f.items = {{ttc, "Test Sans Nope"}, {ttc, "Test Sans Medium"}};
    f.body = {{ttc, "Test Sans Medium"}};
    f.footer = {{gone, "Test Sans Demi Bold"}, {ttc, "Test Sans Nope"}};
    const auto lines = said(dir + "/chains-stderr.txt", [&] { fonts_build(f); });
    int title = -1, tabs = -1, items = -1, body = -1;
    check(took(FontRole::title, &title) == "Test Sans Heavy" && title == 3 && took(FontRole::tabs, &tabs) == "Test Sans Demi Bold" &&
              tabs == 1 && took(FontRole::items, &items) == "Test Sans Medium" && items == 2 &&
              took(FontRole::body, &body) == "Test Sans Medium" && body == 2,
          "each kind of text takes the first face of its chain that loads, at its index in the file");
    check(fonts_role(FontRole::items) == fonts_role(FontRole::body), "two kinds of text with the same face share one font");
    check(took(FontRole::footer).empty(), "a kind whose faces all fail takes Dear ImGui's own");
    check(lines.size() == 4 && all_start(lines, "[overlay] font") && has(lines[0], "missing.ttc, so the title text tries the next") &&
              has(lines[1], "\"Test Sans Nope\" in " + ttc + ", so the items text tries the next") &&
              has(lines[3], "so the footer text takes Dear ImGui's own"),
          "one [overlay] font line for each step down, four here, each naming the face or file and where the text goes");
    // A theme that lists a face for the body alone.
    OverlayFonts body_only;
    body_only.body = {{ttc, "Test Sans Medium"}};
    const auto only = said(dir + "/body-only-stderr.txt", [&] { fonts_build(body_only); });
    check(took(FontRole::body) == "Test Sans Medium" && took(FontRole::title).empty() && only.size() == 4 &&
              has(only[0], "the title text lists no font, so it takes Dear ImGui's own"),
          "each kind that lists no face, in a theme that lists some, takes Dear ImGui's own with a line of its own");
    // A theme where every face is in a file that isn't there.
    OverlayFonts nowhere;
    for (auto* chain : {&nowhere.title, &nowhere.tabs, &nowhere.items, &nowhere.body, &nowhere.footer})
        *chain = {{gone, "Test Sans Medium"}, {dir + "/also-missing.ttf", "Test Sans Medium"}};
    const auto none = said(dir + "/nowhere-stderr.txt", [&] { fonts_build(nowhere); });
    bool own = true;
    for (FontRole r : kRoles) own = own && took(r).empty() && fonts_role(r) == fonts_role(FontRole::body);
    check(own && fonts_atlas()->Fonts.Size == 1, "with no file there, every kind takes Dear ImGui's own, one font for all");
    check(none.size() == 10 && all_start(none, "[overlay] font"), "and says so once for each of the ten faces");
    // No font listed at all, as the fork's theme is off macOS.
    const auto empty = said(dir + "/empty-stderr.txt", [&] { fonts_build(OverlayFonts{}); });
    own = true;
    for (FontRole r : kRoles) own = own && took(r).empty();
    check(own && fonts_atlas()->Fonts.Size == 1 && empty.size() == 1 && has(empty[0], "[overlay] font: the theme lists no font"),
          "a theme that lists no font takes Dear ImGui's own embedded font and says so in one line");
    // No context is on that atlas, so the next build frees it: Dear ImGui holds as many
    // allocations after as before, not a second atlas's.
    const long live = g_live;
    said(dir + "/again-stderr.txt", [&] { fonts_build(OverlayFonts{}); });
    check(live > 0 && g_live == live, "an atlas no context is on is freed when the next is built");
}

// A context of the overlay's kind on the shared atlas, with a renderer that takes texture
// updates but draws nothing, `height` lines tall at `density` pixels to each.
ImGuiContext* context(float height, float density) {
    ImGuiContext* c = ImGui::CreateContext(fonts_atlas());
    ImGui::SetCurrentContext(c);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = ImVec2(height * 16.0f / 9.0f, height);
    io.DisplayFramebufferScale = ImVec2(density, density);
    io.FontDefault = fonts_role(FontRole::body);
    return c;
}

// The glyphs drawn with the fallback glyph instead of their own, as Dear ImGui draws one it has
// no room in the atlas for.
int g_fell_back;

// A frame of `c` with each kind of text's printable ASCII and a few accented letters at its size
// at `height` lines, all in view, so every glyph is baked or else falls back.
void frame(ImGuiContext* c, const OverlayTheme& t, float height) {
    ImGui::SetCurrentContext(c);
    fonts_new_frame();
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("text", nullptr, ImGuiWindowFlags_NoDecoration);
    const float sizes[] = {t.title_size, t.tab_size, t.item_text_size, t.body_size, t.footer_size};
    std::string ascii;
    std::vector<ImWchar> letters;
    for (char ch = ' '; ch <= '~'; ch++) {
        ascii += ch;
        letters.push_back((ImWchar)ch);
    }
    ascii += "\xC3\xA9\xC3\xA8\xC3\xBC\xC3\xB1";  // é è ü ñ
    for (ImWchar accented : {0xE9, 0xE8, 0xFC, 0xF1}) letters.push_back(accented);
    for (size_t i = 0; i < 5; i++) {
        ImGui::PushFont(fonts_role(kRoles[i]), sizes[i] * height / 1080.0f);
        ImGui::TextUnformatted(ascii.c_str());
        ImFontBaked* baked = ImGui::GetFontBaked();
        for (ImWchar l : letters) g_fell_back += !baked->IsGlyphLoaded(l);
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::Render();
    // As a renderer would, take each texture's creation and updates.
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
            tex->SetTexID((ImTextureID)1);
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantDestroy) {
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

// Two contexts, the menu's and the notices', on one atlas, at sizes from 1080 to 2160 lines at
// one pixel to each and then from 1080 to `most_at_two` lines at two, as a window on a Retina
// display moved between screens.
void shared(const OverlayFonts& f, const char* which, float most_at_two) {
    fonts_build(f);
    const OverlayTheme t;
    ImGuiContext* menu = context(1080.0f, 1.0f);
    ImGuiContext* notices = context(1080.0f, 1.0f);
    char what[200];
    snprintf(what, sizeof(what), "%s: the menu's and the notices' contexts share one atlas", which);
    check(fonts_atlas()->RefCount == 2 && fonts_atlas()->OwnerContext == nullptr, what);
    frame(menu, t, 1080.0f);
    const int baked = fonts_atlas()->Builder->BakedPool.Size;
    const int textures = fonts_atlas()->TexList.Size;
    frame(notices, t, 1080.0f);
    snprintf(what, sizeof(what), "%s: text the menu has drawn is drawn by the notices from the same glyphs", which);
    check(baked > 0 && fonts_atlas()->Builder->BakedPool.Size == baked && fonts_atlas()->TexList.Size == textures, what);
    // The atlas's own texture, not the old one a repack keeps for a frame beside it.
    size_t live = 0;
    g_fell_back = 0;
    for (float density : {1.0f, 2.0f})
        for (float height : {1080.0f, 1440.0f, 2160.0f}) {
            if (density == 2.0f && height > most_at_two) continue;
            for (ImGuiContext* c : {menu, notices}) {
                ImGui::SetCurrentContext(c);
                ImGui::GetIO().DisplaySize = ImVec2(height * 16.0f / 9.0f, height);
                ImGui::GetIO().DisplayFramebufferScale = ImVec2(density, density);
                frame(c, t, height);
                live = std::max(live, (size_t)fonts_atlas()->TexData->GetSizeInBytes());
            }
        }
    printf("[fonts] %s: the atlas texture at most %.2f MB, %d glyphs fell back\n", which, live / 1048576.0, g_fell_back);
    snprintf(what, sizeof(what), "%s: every kind of text up to %.0f lines at twice the density is baked, none falling back, in 8 MB of texture",
             which, most_at_two);
    check(g_fell_back == 0 && live <= 8u << 20, what);
    for (ImGuiContext* c : {notices, menu}) ImGui::DestroyContext(c);
}

// The Mac's own fonts, where they're installed, read in place.
void system_fonts(const std::string& dir) {
    const OverlayFonts f = overlay_fonts_default();
    if (f.body.empty() || !std::filesystem::exists(f.body[0].path)) {
        printf("skipped: %s isn't here\n", f.body.empty() ? "a default chain" : f.body[0].path.c_str());
        return;
    }
    const auto lines = said(dir + "/system-stderr.txt", [&] { fonts_build(f); });
    bool first = true;
    const std::vector<OverlayFace>* chains[] = {&f.title, &f.tabs, &f.items, &f.body, &f.footer};
    for (size_t i = 0; i < 5; i++) {
        first = first && took(kRoles[i]) == (*chains[i])[0].name.substr(0, 39);
        printf("  %s\n", took(kRoles[i]).c_str());
    }
    check(first && lines.empty(), "the default chains load the Mac's Avenir Next faces with no step down");
    // The second choice of each, as on a Mac without Avenir Next.
    OverlayFonts neue = f;
    for (auto* chain : {&neue.title, &neue.tabs, &neue.items, &neue.body, &neue.footer}) chain->erase(chain->begin());
    if (std::filesystem::exists(neue.body[0].path)) {
        const auto said_neue = said(dir + "/neue-stderr.txt", [&] { fonts_build(neue); });
        bool second = true;
        for (FontRole r : kRoles) second = second && took(r).starts_with("Helvetica Neue");
        check(second && said_neue.empty(), "each Helvetica Neue face the chains fall to is there too");
    }
    // The third, as on a Mac without either.
    OverlayFonts helvetica = neue;
    for (auto* chain : {&helvetica.title, &helvetica.tabs, &helvetica.items, &helvetica.body, &helvetica.footer})
        chain->erase(chain->begin());
    if (std::filesystem::exists(helvetica.body[0].path)) {
        const auto said_helvetica = said(dir + "/helvetica-stderr.txt", [&] { fonts_build(helvetica); });
        bool third = true;
        for (FontRole r : kRoles) third = third && took(r).starts_with("Helvetica") && !took(r).starts_with("Helvetica Neue");
        check(third && said_helvetica.empty(), "and each Helvetica face after them");
    }
    shared(f, "the Mac's fonts", 2160.0f);
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
    ImGui::SetAllocatorFunctions(counted_alloc, counted_free);
    lookup();
    chains(dir);
    // Dear ImGui's own font is wider than the Mac's. A size's glyphs stay in the atlas for two
    // frames after it's last drawn, and at 2160 lines at twice the density, beside those of 1440,
    // its text doesn't fit and some glyphs fall back, so its sizes at twice stop at 1440.
    shared(OverlayFonts{}, "Dear ImGui's own font", 1440.0f);
    system_fonts(dir);
    printf("test_overlay_fonts: %s\n", g_failed ? "FAILED" : "passed");
    return g_failed ? 1 : 0;
}
