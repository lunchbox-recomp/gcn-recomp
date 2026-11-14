// The disc banner's checks: the `test_banner` target. Runs with no window, GL, disc or game.
//
// runtime/banner.cpp finds opening.bnr in a disc's file table and decodes its RGB5A3 picture.
// Every disc and banner here is made up by this file: the pixel modes and all eight alphas,
// the tile order, BNR1 and BNR2, and the tables and banners it must refuse. `ninja unit` builds
// it with the address and undefined-behaviour sanitizers where the compiler has them.
//
// usage: test_banner
// Exits 0 only if every check passes.
#include "banner.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    if (v.size() < at + 4) v.resize(at + 4);
    v[at] = (uint8_t)(x >> 24);
    v[at + 1] = (uint8_t)(x >> 16);
    v[at + 2] = (uint8_t)(x >> 8);
    v[at + 3] = (uint8_t)x;
}

// A banner of `magic` and `size` whose pixel (x, y) is `px(x, y)`.
template <class F>
std::vector<uint8_t> make_banner(const char* magic, size_t size, F px) {
    std::vector<uint8_t> b(size, 0);
    memcpy(b.data(), magic, 4);
    for (int y = 0; y < kBannerH; y++)
        for (int x = 0; x < kBannerW; x++) {
            const size_t i = 0x20 + ((size_t)(y / 4 * 24 + x / 4) * 16 + (y % 4) * 4 + x % 4) * 2;
            const uint16_t v = px(x, y);
            b[i] = (uint8_t)(v >> 8);
            b[i + 1] = (uint8_t)v;
        }
    return b;
}

struct Entry {
    bool dir;
    std::string name;
    uint32_t a, b;  // a file's offset and size, or a directory's parent and end index
};

// An FST of the root and `entries`, in that order.
std::vector<uint8_t> make_fst(const std::vector<Entry>& entries) {
    const uint32_t count = (uint32_t)entries.size() + 1;
    std::vector<uint8_t> f(count * 12, 0);
    std::string names(1, '\0');  // the root's empty name
    f[0] = 1;
    put32(f, 8, count);
    for (size_t i = 0; i < entries.size(); i++) {
        const size_t at = (i + 1) * 12;
        put32(f, at, (entries[i].dir ? 0x01000000u : 0) | (uint32_t)names.size());
        put32(f, at + 4, entries[i].a);
        put32(f, at + 8, entries[i].b);
        names += entries[i].name;
        names += '\0';
    }
    f.insert(f.end(), names.begin(), names.end());
    return f;
}

// A disc with `fst` at 0x1000 and `banner` at 0x8000, and a reader of it that counts its bytes.
struct Disc {
    std::vector<uint8_t> bytes;
    uint64_t read = 0;
    DiscRead reader() {
        return [this](uint64_t off, void* dst, uint32_t n) {
            read += n;
            if (off + n > bytes.size()) return false;
            memcpy(dst, bytes.data() + off, n);
            return true;
        };
    }
};
Disc make_disc(const std::vector<uint8_t>& fst, const std::vector<uint8_t>& banner) {
    Disc d;
    d.bytes.assign(0x8000 + banner.size(), 0);
    put32(d.bytes, 0x424, 0x1000);
    put32(d.bytes, 0x428, (uint32_t)fst.size());
    memcpy(d.bytes.data() + 0x1000, fst.data(), fst.size());
    memcpy(d.bytes.data() + 0x8000, banner.data(), banner.size());
    return d;
}

const uint8_t* at(const std::vector<uint8_t>& rgba, int x, int y) { return &rgba[((size_t)y * kBannerW + x) * 4]; }
bool is(const uint8_t* p, int r, int g, int b, int a) { return p[0] == r && p[1] == g && p[2] == b && p[3] == a; }

}  // namespace

int main() {
    std::vector<uint8_t> rgba(kBannerW * kBannerH * 4);

    // The pixel modes, one per column of the top row; the rest are 0 (clear black).
    const uint16_t modes[] = {0xFFFF, 0x8000, 0xFC00, 0x83E0, 0x801F, 0x8421, 0x7FFF, 0x0F00, 0x00F0, 0x000F, 0x0123};
    const auto px_modes = [&](int x, int y) -> uint16_t {
        return y == 0 && x < (int)(sizeof(modes) / 2) ? modes[x] : 0;
    };
    check(banner_decode(make_banner("BNR1", 0x1960, px_modes).data(), 0x1960, rgba.data()), "a BNR1 of 0x1960 bytes decodes");
    check(is(at(rgba, 0, 0), 255, 255, 255, 255) && is(at(rgba, 1, 0), 0, 0, 0, 255), "RGB555 white and black are opaque");
    check(is(at(rgba, 2, 0), 255, 0, 0, 255) && is(at(rgba, 3, 0), 0, 255, 0, 255) && is(at(rgba, 4, 0), 0, 0, 255, 255),
          "RGB555's red, green and blue are bits 10-14, 5-9 and 0-4");
    check(is(at(rgba, 5, 0), 8, 8, 8, 255), "a 5-bit 1 is 8, the top bits repeated below");
    check(is(at(rgba, 6, 0), 255, 255, 255, 255), "A3RGB444 with alpha 7 is opaque");
    check(is(at(rgba, 7, 0), 255, 0, 0, 0) && is(at(rgba, 8, 0), 0, 255, 0, 0) && is(at(rgba, 9, 0), 0, 0, 255, 0),
          "A3RGB444's colour keeps its value at alpha 0, straight, not premultiplied");
    check(is(at(rgba, 10, 0), 17, 34, 51, 0), "a 4-bit value is 17 times itself");
    check(is(at(rgba, 0, 1), 0, 0, 0, 0), "a 0 pixel is clear");

    // All eight alphas: 3 bits become 8 with the top bits repeated below.
    bool alphas = true;
    for (int a = 0; a < 8; a++) {
        const auto px = [&](int x, int y) -> uint16_t { return x == 0 && y == 0 ? (uint16_t)(a << 12 | 0x0FFF) : 0; };
        banner_decode(make_banner("BNR1", 0x1960, px).data(), 0x1960, rgba.data());
        const int want = (a << 5) | (a << 2) | (a >> 1);
        if (!is(at(rgba, 0, 0), 255, 255, 255, want)) alphas = false;
    }
    check(alphas, "the eight alphas are 0, 36, 73, 109, 146, 182, 219 and 255");

    // Tiles: each pixel says where it is, so every one decoded in its place shows its own x, y.
    const auto px_where = [](int x, int y) -> uint16_t { return (uint16_t)(0x8000 | (x & 31) << 10 | (y & 31) << 5 | (x >> 5)); };
    banner_decode(make_banner("BNR2", 0x1FA0, px_where).data(), 0x1FA0, rgba.data());
    bool placed = true;
    for (int y = 0; y < kBannerH; y++)
        for (int x = 0; x < kBannerW; x++) {
            const uint8_t* p = at(rgba, x, y);
            if (p[0] >> 3 != (x & 31) || p[1] >> 3 != y || p[2] >> 3 != x >> 5) placed = false;
        }
    check(placed, "a BNR2 of 0x1FA0 bytes decodes, 4x4 tiles 24 across, each row by row");

    const std::vector<uint8_t> bnr1 = make_banner("BNR1", 0x1960, px_modes);
    check(!banner_decode(make_banner("BNR1", 0x1FA0, px_modes).data(), 0x1FA0, rgba.data()), "a BNR1 of BNR2's size is refused");
    check(!banner_decode(make_banner("BNR2", 0x1960, px_modes).data(), 0x1960, rgba.data()), "a BNR2 of BNR1's size is refused");
    check(!banner_decode(make_banner("BNR3", 0x1960, px_modes).data(), 0x1960, rgba.data()), "another magic is refused");
    check(!banner_decode(bnr1.data(), 0x1000, rgba.data()), "a short banner is refused");

    // The file table: opening.bnr among the root's own files, past a directory holding another.
    const std::vector<Entry> tree = {
        {false, "a.bin", 0x2000, 0x10},
        {true, "sub", 0, 4},  // its entries are 3; the next root entry is 4
        {false, "opening.bnr", 0x3000, 0x1960},
        {false, "opening.bnr", 0x8000, 0x1960},
    };
    Disc disc = make_disc(make_fst(tree), bnr1);
    uint32_t off = 0, len = 0;
    const std::vector<uint8_t> fst = make_fst(tree);
    check(fst_find_root_file(fst.data(), fst.size(), "opening.bnr", &off, &len) && off == 0x8000 && len == 0x1960,
          "the root's opening.bnr is found, not the one in a directory");
    check(banner_read(disc.reader(), rgba.data()) && is(at(rgba, 2, 0), 255, 0, 0, 255), "and read from the disc");
    check(disc.read <= 8 + fst.size() + 0x1960, "reading only its place, the table and the banner");

    const std::vector<uint8_t> only_sub = make_fst({{true, "sub", 0, 3}, {false, "opening.bnr", 0x8000, 0x1960}});
    check(!fst_find_root_file(only_sub.data(), only_sub.size(), "opening.bnr", &off, &len), "one in a directory alone isn't taken");
    const std::vector<uint8_t> cased = make_fst({{false, "OPENING.BNR", 0x8000, 0x1960}});
    check(!fst_find_root_file(cased.data(), cased.size(), "opening.bnr", &off, &len), "the name has to match exactly");
    const std::vector<uint8_t> none = make_fst({{false, "a.bin", 0x2000, 0x10}});
    check(!fst_find_root_file(none.data(), none.size(), "opening.bnr", &off, &len), "a disc with none has none");

    std::vector<uint8_t> bad = fst;
    check(!fst_find_root_file(bad.data(), 11, "opening.bnr", &off, &len), "a table under 12 bytes is refused");
    std::vector<uint8_t> big = fst;
    big.resize((1u << 20) + 1, 0);
    check(!fst_find_root_file(big.data(), big.size(), "opening.bnr", &off, &len), "one over 1 MiB is refused");
    // Each of the next three is refused by one bound alone. Past the table's end, in the bytes
    // the caller has there, is what a walk without it would read: a name that matches, or the
    // end of a block exactly the table's size, which the sanitizers stop (CMakeLists.txt).
    {
        // 13 bytes: the root's entry, saying there are two, and a NUL. The second isn't there.
        // Without the bound the walk still refuses it, as every name starts at count * 12 or
        // later, past the table, so no input gets a wrong answer from it. What it stops is reads
        // past the table: an entry there that looks like a directory sends the walk to its end,
        // up to count * 12 bytes on, which can crash a build without the sanitizers too. Here
        // the missing entry is read just past the 13 bytes, which only the sanitizers see, so
        // a build without them doesn't check this bound.
        std::unique_ptr<uint8_t[]> t(new uint8_t[13]());
        t[0] = 1;
        t[11] = 2;
        check(!fst_find_root_file(t.get(), 13, "opening.bnr", &off, &len),
              "a count whose entries don't fit is refused, with nothing past them read");
    }
    // The table ends with the root's opening.bnr's name cut to "opening"; ".bnr" and its NUL
    // come after.
    check(!fst_find_root_file(fst.data(), fst.size() - 5, "opening.bnr", &off, &len),
          "a table not ending in a NUL is refused, though its last name goes on past it");
    // The table ends before the root's opening.bnr's name, which is there just past it.
    check(!fst_find_root_file(fst.data(), fst.size() - 12, "opening.bnr", &off, &len),
          "a name starting at the table's end is refused, though it matches past it");
    bad = fst;
    put32(bad, 3 * 12 + 0, 0x00FFFFFF);  // the first opening.bnr's name, far past the end
    put32(bad, 4 * 12 + 0, 0x00FFFFFF);
    check(!fst_find_root_file(bad.data(), bad.size(), "opening.bnr", &off, &len), "a name far past the end is refused");
    bad = fst;
    put32(bad, 2 * 12 + 8, 2);  // a directory that ends where it starts
    check(!fst_find_root_file(bad.data(), bad.size(), "opening.bnr", &off, &len), "a directory going nowhere is refused");
    bad = fst;
    put32(bad, 2 * 12 + 8, 0x10000);  // one past the table
    check(!fst_find_root_file(bad.data(), bad.size(), "opening.bnr", &off, &len), "one ending past the table ends the walk");

    Disc past = make_disc(fst, bnr1);
    put32(past.bytes, 0x424, 0x7FFFFFF0);
    check(!banner_read(past.reader(), rgba.data()), "a table past the disc's end is refused");
    Disc short_banner = make_disc(make_fst({{false, "opening.bnr", 0x8000, 0x1960}}), std::vector<uint8_t>(0x100, 0));
    check(!banner_read(short_banner.reader(), rgba.data()), "and a banner past it");
    const std::vector<uint8_t> wrong_fst = make_fst({{false, "opening.bnr", 0x8000, 0x1000}});
    Disc wrong_size = make_disc(wrong_fst, bnr1);
    check(!banner_read(wrong_size.reader(), rgba.data()) && wrong_size.read == 8 + wrong_fst.size(),
          "and one whose file isn't a banner's size, read no further");

    return g_failed ? 1 : 0;
}
