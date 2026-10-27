// Disc overlay checks: the `test_disc_overlay` target. Runs with no disc image, device, SDL or
// game. runtime/hw/disc_overlay.cpp is linked as it is, and platform.cpp for its file reads.
//
// Every number here is made up. The test writes a small image, a CISO of it, a dozen range
// files and the manifests that join them into its work dir, and builds the disc those should
// make as a plain byte array. Reads through the overlay, on a plain base and on a CISO base,
// are compared with that array: every start with a few lengths, 10,000 at random, and the same
// from four threads at once, since the game's threads and the audio callback read together.
// Each way the loader refuses a manifest is made to happen and has to name its line. 300 range
// files load in a child process whose open-file limit is cut to 100, so only the loader's own
// raise of the limit lets them open.
//
// usage: test_disc_overlay <work dir>
// The work dir must not exist. It's made, and the files are left in it. Exits 0 only if every
// check passes.
#include "hw/disc_overlay.h"
#include "platform.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_checks, g_failed;
std::string g_dir;  // the work dir
std::string g_out;  // where the manifests and range files go, a name with a space in it

void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}

// ---- the made-up disc ----
constexpr uint64_t kImage = 0x6000;  // six blocks, the fourth of them all zeros
constexpr uint32_t kBlock = 0x1000;
constexpr uint32_t kFstAt = 0x5A00, kFstSize = 0x100, kFstMax = 0x140, kDolAt = 0x1200;
constexpr char kId[] = "FIXT01";
constexpr uint64_t kEnd = 0x9100;  // the virtual disc the reads stay inside

struct RangeFile {
    const char* rel;
    uint64_t at;
    uint32_t len;
};
// Sorted by offset. The first sits inside the image, the second starts where the image ends,
// the third and fourth touch, the fifth is one byte, and the last two touch in a folder with a
// space in its name.
const RangeFile kFiles[] = {
    {"inner.bin", 0x2000, 0x100},      {"fst.bin", 0x6000, 0x160},
    {"dol.bin", 0x8000, 0x200},        {"next.bin", 0x8200, 0x10},
    {"one.bin", 0x8400, 1},            {"mod files/with space.bin", 0x9000, 0x80},
    {"tail.bin", 0x9080, 0x60},
};
constexpr size_t kInner = 0, kTail = 6;
struct Word {
    uint32_t at, value;
};
const Word kWords[] = {{0x420, 0x8000}, {0x424, 0x6000}, {0x428, 0x160}, {0x42C, 0x160}};

std::vector<uint8_t> g_image;
std::vector<std::vector<uint8_t>> g_data;  // what each of kFiles holds
std::vector<uint64_t> g_edges;             // the offsets where something starts or ends

std::vector<uint8_t> stream(uint32_t seed, size_t n) {
    uint32_t s = seed * 0x9E3779B1u | 1u;
    std::vector<uint8_t> v(n);
    for (uint8_t& b : v) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        b = (uint8_t)(s >> 11);
    }
    return v;
}

void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// The test's own FNV-1a, so a slip in the loader's hash can't also be in the manifests.
uint64_t ref_fnv(const uint8_t* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

std::string hex(uint64_t v, int digits = 1) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%0*llx", digits, (unsigned long long)v);
    return buf;
}

bool write_file(const std::string& path, const void* data, size_t size) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return false; }
    const bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

bool write_text(const std::string& path, const std::string& text) { return write_file(path, text.data(), text.size()); }

int open_fds() {
    int n = 0;
    for (int fd = 0; fd < 1024; fd++)
        if (fcntl(fd, F_GETFD) != -1) n++;
    return n;
}

// ---- the bases ----
enum class Base { Plain, Ciso };

PlatFile* g_plain_file;
PlatFile* g_ciso_file;
uint32_t g_ciso_block;
std::vector<int32_t> g_ciso_map;

bool plain_base(uint64_t off, void* dst, uint32_t len) { return plat_read_at(g_plain_file, off, dst, len); }

// A block that isn't stored, or is past the map, reads as zeros.
bool ciso_base(uint64_t off, void* dst, uint32_t len) {
    uint8_t* p = (uint8_t*)dst;
    while (len) {
        const uint64_t blk = off / g_ciso_block;
        const uint32_t in = (uint32_t)(off % g_ciso_block);
        const uint32_t n = (uint32_t)std::min<uint64_t>(len, g_ciso_block - in);
        const int32_t stored = blk < g_ciso_map.size() ? g_ciso_map[blk] : -1;
        if (stored < 0) memset(p, 0, n);
        else if (!plat_read_at(g_ciso_file, 0x8000 + (uint64_t)stored * g_ciso_block + in, p, n)) return false;
        p += n;
        off += n;
        len -= n;
    }
    return true;
}

bool failing_base(uint64_t, void*, uint32_t) { return false; }

// An image whose header claims an FST of 2 GiB, which the loader must refuse before it reads.
bool huge_fst_base(uint64_t off, void* dst, uint32_t len) {
    memset(dst, 0, len);
    if (off == 0 && len >= 0x42C) {
        memcpy(dst, kId, 6);
        put_be32((uint8_t*)dst + 0x424, 0x100);
        put_be32((uint8_t*)dst + 0x428, 0x80000000u);
    }
    return true;
}

BaseRead base_fn(Base b) { return b == Base::Plain ? plain_base : ciso_base; }

// ---- the disc the overlay should give, built without the loader ----
struct Model {
    std::vector<uint8_t> bytes;  // zeros past the image, where no range or word says otherwise
    bool overlaid = false;
};

Model bare_model() {
    Model m;
    m.bytes.assign(kEnd, 0);
    memcpy(m.bytes.data(), g_image.data(), kImage);
    return m;
}

Model overlay_model() {
    Model m = bare_model();
    m.overlaid = true;
    for (size_t i = 0; i < std::size(kFiles); i++) memcpy(&m.bytes[kFiles[i].at], g_data[i].data(), kFiles[i].len);
    for (const Word& w : kWords) put_be32(&m.bytes[w.at], w.value);
    return m;
}

Model g_bare, g_over;

// A read through the loaded overlay gives the model's bytes, on either base. With no overlay a
// plain image's read past its end fails, as it always did.
bool matches(const Model& m, uint64_t off, uint32_t len, Base b) {
    uint8_t got[0x400];
    memset(got, 0xEE, len);
    const bool ok = disc_overlay_read(off, got, len, base_fn(b));
    const bool want = m.overlaid || b == Base::Ciso || off + len <= kImage;
    if (ok != want) return false;
    return !ok || memcmp(got, &m.bytes[off], len) == 0;
}

// Every start with a few lengths, so a read ends on, starts on and crosses every edge.
bool sweep(const Model& m, Base b) {
    static const uint32_t lens[] = {1, 2, 3, 4, 5, 7, 16, 33};
    for (uint64_t off = 0; off < kEnd; off++)
        for (uint32_t len : lens)
            if (off + len <= kEnd && !matches(m, off, len, b)) {
                printf("      first mismatch: off %llx len %x\n", (unsigned long long)off, len);
                return false;
            }
    return true;
}

// Half the reads start anywhere and run long. The other half start just before an edge.
bool random_reads(const Model& m, Base b, int n, uint32_t seed) {
    uint32_t s = seed | 1u;
    auto next = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    for (int i = 0; i < n; i++) {
        const uint32_t r1 = next(), r2 = next(), r3 = next();
        uint64_t off;
        uint32_t len;
        if (r1 & 1) {
            off = r2 % kEnd;
            len = 1 + r3 % 0x300;
        } else {
            const uint64_t e = g_edges[r2 % g_edges.size()];
            off = e > r3 % 40 ? e - r3 % 40 : 0;
            len = 1 + (r3 >> 8) % 80;
        }
        if (off >= kEnd) off = kEnd - 1;
        len = (uint32_t)std::min<uint64_t>(len, kEnd - off);
        if (!matches(m, off, len, b)) {
            printf("      mismatch: off %llx len %x\n", (unsigned long long)off, len);
            return false;
        }
    }
    return true;
}

// ---- manifests ----
std::string manifest_path() { return g_out + "/disc-overlay.txt"; }

std::string base_line(uint64_t hash = 0) {
    return "base " + std::string(kId) + " " + hex(kFstAt) + " " + hex(kFstSize) + " " +
           hex(hash ? hash : ref_fnv(&g_image[kFstAt], kFstSize), 16);
}

std::string range_line(size_t i) { return "range " + hex(kFiles[i].at) + " " + hex(kFiles[i].len) + " " + kFiles[i].rel; }

// The manifest the builder would write for the fixture: line 1 the magic, 2 the base, 3 to 6
// the header words and 7 to 13 the ranges.
std::vector<std::string> good_lines() {
    std::vector<std::string> l = {"gcn-disc-overlay 1", base_line()};
    for (const Word& w : kWords) l.push_back("header " + hex(w.at) + " " + hex(w.value));
    for (size_t i = 0; i < std::size(kFiles); i++) l.push_back(range_line(i));
    return l;
}

std::string join(const std::vector<std::string>& lines) {
    std::string t;
    for (const std::string& s : lines) t += s + "\n";
    return t;
}

bool load_text(const std::string& text, Base b, std::string* err) {
    write_text(manifest_path(), text);
    return disc_overlay_load(manifest_path().c_str(), base_fn(b), err);
}

bool load_lines(const std::vector<std::string>& lines, Base b, std::string* err = nullptr) {
    return load_text(join(lines), b, err);
}

// The load has to fail, name the manifest and `line`, contain each needle, load nothing and
// leave no file open.
void refused_text(const char* what, const std::string& text, int line, const char* n1 = nullptr, const char* n2 = nullptr) {
    const int fds = open_fds();
    std::string err;
    const bool loaded = load_text(text, Base::Plain, &err);
    char at[32];
    snprintf(at, sizeof(at), ", line %d: ", line);
    const bool named = err.find(manifest_path()) == 0 && err.find(at) != std::string::npos;
    const bool needles = (!n1 || err.find(n1) != std::string::npos) && (!n2 || err.find(n2) != std::string::npos);
    const bool ok = !loaded && named && needles && !disc_overlay_loaded() && open_fds() == fds;
    check(ok, what);
    if (!ok) printf("      error was: %s\n", err.c_str());
}

void refused(const char* what, const std::vector<std::string>& lines, int line, const char* n1 = nullptr, const char* n2 = nullptr) {
    refused_text(what, join(lines), line, n1, n2);
}

std::vector<std::string> with(std::vector<std::string> l, size_t at, const std::string& line) {
    l[at] = line;
    return l;
}

std::vector<std::string> inserted(std::vector<std::string> l, size_t at, const std::string& line) {
    l.insert(l.begin() + (long)at, line);
    return l;
}

// ---- fixtures ----
bool make_fixture() {
    g_image = stream(1, kImage);
    memset(&g_image[0x3000], 0, kBlock);
    memcpy(g_image.data(), kId, 6);
    put_be32(&g_image[0x420], kDolAt);
    put_be32(&g_image[0x424], kFstAt);
    put_be32(&g_image[0x428], kFstSize);
    put_be32(&g_image[0x42C], kFstMax);
    bool ok = write_file(g_dir + "/game.iso", g_image.data(), g_image.size());

    // A CISO of it: the block of zeros is left out.
    std::vector<uint8_t> ciso(0x8000, 0);
    memcpy(ciso.data(), "CISO", 4);
    ciso[4] = kBlock & 0xFF;
    ciso[5] = kBlock >> 8;
    for (uint32_t b = 0; b < kImage / kBlock; b++) {
        if (b == 3) continue;
        ciso[8 + b] = 1;
        ciso.insert(ciso.end(), g_image.begin() + b * kBlock, g_image.begin() + (b + 1) * kBlock);
    }
    ok = write_file(g_dir + "/game.ciso", ciso.data(), ciso.size()) && ok;

    ok = mkdir(g_out.c_str(), 0755) == 0 && mkdir((g_out + "/mod files").c_str(), 0755) == 0 && ok;
    for (size_t i = 0; i < std::size(kFiles); i++) {
        g_data.push_back(stream(10 + (uint32_t)i, kFiles[i].len));
        ok = write_file(g_out + "/" + kFiles[i].rel, g_data[i].data(), g_data[i].size()) && ok;
    }

    g_plain_file = plat_open_read((g_dir + "/game.iso").c_str());
    g_ciso_file = plat_open_read((g_dir + "/game.ciso").c_str());
    if (!g_plain_file || !g_ciso_file) return false;
    uint8_t hdr[0x8000];
    ok = plat_read_at(g_ciso_file, 0, hdr, sizeof(hdr)) && ok;
    g_ciso_block = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    g_ciso_map.assign(sizeof(hdr) - 8, -1);
    int32_t stored = 0;
    for (size_t i = 0; i < g_ciso_map.size(); i++)
        if (hdr[8 + i] == 1) g_ciso_map[i] = stored++;

    for (const RangeFile& f : kFiles) {
        g_edges.push_back(f.at);
        g_edges.push_back(f.at + f.len);
    }
    for (const Word& w : kWords) g_edges.push_back(w.at);
    g_edges.push_back(0x430);
    g_edges.push_back(0x440);
    g_edges.push_back(kImage);
    g_bare = bare_model();
    g_over = overlay_model();
    return ok;
}

// ---- the checks ----
void hash_vectors() {
    check(disc_overlay_fnv1a64("", 0) == 0xCBF29CE484222325ull, "the FNV-1a of nothing is its basis");
    check(disc_overlay_fnv1a64("a", 1) == 0xAF63DC4C8601EC8Cull, "the FNV-1a of \"a\" is the published value");
    check(disc_overlay_fnv1a64("foobar", 6) == 0x85944171F73967E8ull, "the FNV-1a of \"foobar\" is the published value");
    const uint8_t zeros[3] = {0, 0, 0};
    check(disc_overlay_fnv1a64(zeros, 3) == ref_fnv(zeros, 3) && disc_overlay_fnv1a64(g_image.data(), 777) == ref_fnv(g_image.data(), 777),
          "and it agrees with the test's own on zeros and on a run of the image");
}

void path_choice() {
    unsetenv("GCN_DISC_OVERLAY");
    g_disc_overlay_flag = nullptr;
    check(disc_overlay_path() == nullptr, "with no flag and no variable there is no overlay");
    setenv("GCN_DISC_OVERLAY", "from variable.txt", 1);
    check(std::string(disc_overlay_path()) == "from variable.txt", "GCN_DISC_OVERLAY names the manifest");
    g_disc_overlay_flag = "from flag.txt";
    check(std::string(disc_overlay_path()) == "from flag.txt", "--disc-overlay beats the variable");
    g_disc_overlay_flag = "";
    check(disc_overlay_path() != nullptr && *disc_overlay_path() == 0, "and an empty flag still wins, so the load can refuse it");
    g_disc_overlay_flag = nullptr;
    setenv("GCN_DISC_OVERLAY", "", 1);
    check(disc_overlay_path() == nullptr, "an empty variable counts as unset");
    unsetenv("GCN_DISC_OVERLAY");
}

void no_overlay() {
    check(!disc_overlay_loaded(), "nothing is loaded at first");
    check(random_reads(g_bare, Base::Plain, 10000, 0x1234), "10,000 random reads with no overlay match the image byte for byte on a plain image");
    check(random_reads(g_bare, Base::Ciso, 10000, 0x4321), "and on a CISO");
    uint8_t one;
    check(!disc_overlay_read(kImage, &one, 1, plain_base) && disc_overlay_read(kImage, &one, 1, ciso_base),
          "a base's own failures and zeros are passed on as they are");
    check(disc_overlay_read(5, &one, 0, failing_base) == false, "a zero-length read with no overlay is the base's");
}

std::vector<std::string> g_logged;
void capture(const char* line) { g_logged.push_back(line); }

void load_and_close() {
    for (Base b : {Base::Plain, Base::Ciso}) {
        const int fds = open_fds();
        std::string err = "stale";
        const bool ok = load_lines(good_lines(), b, &err);
        check(ok && disc_overlay_loaded(), b == Base::Plain ? "the fixture's manifest loads on a plain image" : "and on a CISO");
        if (!ok) printf("      error was: %s\n", err.c_str());
        check(open_fds() - fds == (int)std::size(kFiles), "each range file is open once");
        disc_overlay_unload();
        check(!disc_overlay_loaded() && open_fds() == fds, "an unload closes them all and drops the overlay");
    }
    check(load_lines(good_lines(), Base::Plain) && disc_overlay_loaded(), "a load can leave out the error string");
    disc_overlay_unload();
    check(!load_lines(with(good_lines(), 0, "nope"), Base::Plain), "and so can a refusal");

    uint64_t total = 0;
    for (const RangeFile& f : kFiles) total += f.len;
    g_logged.clear();
    g_disc_overlay_log = capture;
    load_lines(good_lines(), Base::Ciso);
    char want[64];
    snprintf(want, sizeof(want), "%zu files, %llu bytes", std::size(kFiles), (unsigned long long)total);
    check(g_logged.size() == 1 && g_logged[0].find(want) != std::string::npos, "--log=dvd gets the load, with its file count and bytes");
    g_logged.clear();
    uint8_t buf[0x40];
    disc_overlay_read(0x2010, buf, 0x20, ciso_base);
    check(g_logged.size() == 1 && g_logged[0].find("inner.bin") != std::string::npos, "and a read inside a range, naming the file");
    g_logged.clear();
    disc_overlay_read(0x100, buf, 0x20, ciso_base);
    check(g_logged.empty(), "a read of the image alone says nothing");
    disc_overlay_read(0x2100, buf, 0x10, ciso_base);
    check(g_logged.empty(), "and neither does one that starts where a range ends");
    disc_overlay_read(0x420, buf, 4, ciso_base);
    check(g_logged.size() == 1 && g_logged[0].find("header word 420") != std::string::npos, "a header word is logged");
    g_disc_overlay_log = nullptr;
    disc_overlay_unload();
}

void reads_through() {
    check(load_lines(good_lines(), Base::Ciso), "the fixture loads for the read checks");
    uint8_t got[0x500], want[0x500];

    // Inside one range.
    check(disc_overlay_read(0x2010, got, 0x20, ciso_base) && !memcmp(got, &g_data[kInner][0x10], 0x20), "a read inside one range comes from its file");
    check(disc_overlay_read(0x2000, got, 1, ciso_base) && got[0] == g_data[kInner][0], "the range's first byte comes from its file");
    check(disc_overlay_read(0x20FF, got, 1, ciso_base) && got[0] == g_data[kInner][0xFF], "and its last");
    check(disc_overlay_read(0x1FFF, got, 1, ciso_base) && got[0] == g_image[0x1FFF], "the byte before it comes from the image");
    check(disc_overlay_read(0x2100, got, 1, ciso_base) && got[0] == g_image[0x2100], "and the byte after it");

    // Base, range, base.
    memcpy(want, &g_image[0x1FF0], 0x10);
    memcpy(want + 0x10, g_data[kInner].data(), 0x100);
    memcpy(want + 0x110, &g_image[0x2100], 0x10);
    check(disc_overlay_read(0x1FF0, got, 0x120, ciso_base) && !memcmp(got, want, 0x120), "a read from the image through a range and back into the image joins the three");
    check(disc_overlay_read(0x1FF0, got, 0x120, plain_base) && !memcmp(got, want, 0x120), "the same on a plain base");

    // Image into a range that starts at the image's end, and range into range.
    memcpy(want, &g_image[0x5FF0], 0x10);
    memcpy(want + 0x10, g_data[1].data(), 0x20);
    check(disc_overlay_read(0x5FF0, got, 0x30, plain_base) && !memcmp(got, want, 0x30), "a read from the last of a plain image into the range after it works");
    memcpy(want, &g_data[2][0x1F0], 0x10);
    memcpy(want + 0x10, g_data[3].data(), 0x10);
    check(disc_overlay_read(0x81F0, got, 0x20, plain_base) && !memcmp(got, want, 0x20), "a read from one range straight into the next works");
    memset(want, 0, 0x20);
    memcpy(want, g_data[3].data(), 0x10);
    check(disc_overlay_read(0x8200, got, 0x20, plain_base) && !memcmp(got, want, 0x20), "and one running from a range into the gap after it reads zeros there on a plain base");
    memset(want, 0, 0x230);
    memcpy(want, &g_data[2][0x1F0], 0x10);
    memcpy(want + 0x10, g_data[3].data(), 0x10);
    memcpy(want + 0x210, g_data[4].data(), 1);
    check(disc_overlay_read(0x81F0, got, 0x221, ciso_base) && !memcmp(got, want, 0x221), "a read over three ranges and the gaps between them joins them all");

    // The header words.
    memcpy(want, &g_image[0x41C], 4);
    put_be32(want + 4, kWords[0].value);
    put_be32(want + 8, kWords[1].value);
    put_be32(want + 12, kWords[2].value);
    put_be32(want + 16, kWords[3].value);
    check(disc_overlay_read(0x41C, got, 0x14, ciso_base) && !memcmp(got, want, 0x14), "a read starting before 0x420 takes the four header words");
    check(disc_overlay_read(0x41C, got, 0x14, plain_base) && !memcmp(got, want, 0x14), "on a plain base too");
    check(disc_overlay_read(0x422, got, 2, ciso_base) && got[0] == 0x80 && got[1] == 0x00, "a read starting inside a word takes the rest of it");
    check(disc_overlay_read(0x420, got, 2, ciso_base) && got[0] == 0x00 && got[1] == 0x00, "and one ending inside a word takes the start of it");
    check(disc_overlay_read(0x42B, got, 2, ciso_base) && got[0] == 0x60 && got[1] == 0x00, "a read across two words takes the end of one and the start of the next");
    check(disc_overlay_read(0x430, got, 4, ciso_base) && !memcmp(got, &g_image[0x430], 4), "the word after the last one is the image's");
    check(disc_overlay_read(0x41F, got, 1, ciso_base) && got[0] == g_image[0x41F], "and so is the byte before the first");
    memcpy(want, g_image.data(), 0x440);
    for (const Word& w : kWords) put_be32(want + w.at, w.value);
    check(disc_overlay_read(0, got, 0x440, ciso_base) && !memcmp(got, want, 0x440), "the whole header reads as the image's with four words changed");

    // Past the end of the offsets, and far past the disc.
    check(!disc_overlay_read(~0ull - 1, got, 4, ciso_base), "a read that runs past the end of the offsets fails");
    memset(got, 0xEE, 4);
    check(disc_overlay_read(1ull << 40, got, 4, ciso_base) && !got[0] && !got[3], "while one far past every range and the image reads zeros on a CISO");
    memset(got, 0xEE, 4);
    check(disc_overlay_read(1ull << 40, got, 4, plain_base) && !got[0] && !got[3], "and on a plain image");
    check(disc_overlay_read(0x2000, got, 0, failing_base) && disc_overlay_read(0x10, got, 0, failing_base), "a read of no bytes succeeds without touching the base");
    disc_overlay_unload();
}

// The game reads in 32-byte units, so a file whose size isn't one is read on into the gap after
// it, which lies past the end of the image. Both bases give the file, then zeros, and succeed.
void padding_reads() {
    check(load_lines(good_lines(), Base::Plain), "the fixture loads for the padding reads");
    for (Base b : {Base::Plain, Base::Ciso}) {
        const std::string on = b == Base::Plain ? " on a plain base" : " on a CISO base";
        auto named = [&on](const char* what) { return std::string(what) + on; };
        uint8_t got[0x60], want[0x60];

        // one.bin is 1 byte at 0x8400 and nothing follows it until 0x9000.
        memset(want, 0, sizeof(want));
        want[0] = g_data[4][0];
        memset(got, 0xEE, sizeof(got));
        check(disc_overlay_read(0x8400, got, 0x20, base_fn(b)) && !memcmp(got, want, 0x20), named("a 32-byte read of a 1-byte file is the byte and 31 zeros").c_str());
        memset(got, 0xEE, sizeof(got));
        check(disc_overlay_read(0x8400, got, 0x60, base_fn(b)) && !memcmp(got, want, 0x60), named("and a 96-byte one is the byte and 95 zeros").c_str());

        // next.bin is 0x10 bytes at 0x8200.
        memset(want, 0, sizeof(want));
        memcpy(want, g_data[3].data(), 0x10);
        memset(got, 0xEE, sizeof(got));
        check(disc_overlay_read(0x8200, got, 0x20, base_fn(b)) && !memcmp(got, want, 0x20), named("a 32-byte read of a 16-byte file is its bytes and 16 zeros").c_str());
        memset(got, 0xEE, sizeof(got));
        check(disc_overlay_read(0x8210, got, 0x20, base_fn(b)) && std::all_of(got, got + 0x20, [](uint8_t v) { return v == 0; }), named("and one that starts in the gap after it is all zeros").c_str());
    }

    // The whole stretch from before the image's end to the end of the disc, both bases at once.
    constexpr uint64_t from = 0x5E00;
    std::vector<uint8_t> plain(kEnd - from, 0xEE), ciso(kEnd - from, 0xEE);
    const bool ok = disc_overlay_read(from, plain.data(), (uint32_t)plain.size(), plain_base) && disc_overlay_read(from, ciso.data(), (uint32_t)ciso.size(), ciso_base);
    check(ok && plain == ciso, "a read from inside the image to the end of the disc gives a plain base and a CISO base the same bytes");
    check(ok && !memcmp(plain.data(), &g_over.bytes[from], plain.size()), "and they are the model's");
    disc_overlay_unload();

    // A gap that starts inside the image and runs past its end, with a range far past it.
    check(load_lines({"gcn-disc-overlay 1", base_line(), "range 8000 4 dol.bin"}, Base::Plain), "a manifest with one range past the image loads");
    uint8_t got[0x40], want[0x40];
    memset(want, 0, sizeof(want));
    memcpy(want, &g_image[kImage - 0x10], 0x10);
    for (Base b : {Base::Plain, Base::Ciso}) {
        memset(got, 0xEE, sizeof(got));
        check(disc_overlay_read(kImage - 0x10, got, 0x40, base_fn(b)) && !memcmp(got, want, 0x40),
              b == Base::Plain ? "a gap that starts in the image and runs off its end is the image's bytes and zeros on a plain base" : "and on a CISO base");
    }
    disc_overlay_unload();
}

void header_extras() {
    // A word the builder never writes, at the last place one fits.
    uint8_t got[0x40], want[0x20];
    check(load_lines({"gcn-disc-overlay 1", base_line(), "header 43c 12345678"}, Base::Ciso), "a header word at 0x43C loads");
    check(disc_overlay_read(0x438, got, 8, ciso_base) && !memcmp(got, &g_image[0x438], 4) && got[4] == 0x12 && got[5] == 0x34 && got[6] == 0x56 && got[7] == 0x78,
          "and reads at the end of the header");
    check(disc_overlay_read(0x440, got, 4, ciso_base) && !memcmp(got, &g_image[0x440], 4), "while the byte after the header is the image's");
    disc_overlay_unload();

    // A range that covers a word, and a header line before it.
    check(load_lines({"gcn-disc-overlay 1", base_line(), "header 420 11223344", "range 41c 20 inner.bin"}, Base::Ciso), "a range under a header word loads");
    memcpy(want, g_data[kInner].data(), 0x20);
    put_be32(want + 4, 0x11223344);
    check(disc_overlay_read(0x41C, got, 0x20, ciso_base) && !memcmp(got, want, 0x20), "and the header word wins over it");
    disc_overlay_unload();
}

void shared_and_longer_files() {
    std::vector<std::string> l = {"gcn-disc-overlay 1", base_line(), "range 2000 80 inner.bin", "range 3000 80 inner.bin"};
    const int fds = open_fds();
    uint8_t got[0x80];
    check(load_lines(l, Base::Ciso) && open_fds() - fds == 1, "two ranges that name one file open it once");
    check(disc_overlay_read(0x3000, got, 0x80, ciso_base) && !memcmp(got, g_data[kInner].data(), 0x80), "and each reads from the start of it");
    disc_overlay_unload();

    l = good_lines();
    l[12] = "range 9080 50 tail.bin";  // the file holds 0x60
    check(load_lines(l, Base::Ciso), "a file longer than its range loads");
    uint8_t buf[0x60];
    check(disc_overlay_read(0x9080, buf, 0x60, ciso_base) && !memcmp(buf, g_data[kTail].data(), 0x50) && buf[0x50] == 0 && buf[0x5F] == 0,
          "and only the range's bytes come from it");
    disc_overlay_unload();
}

void replacing() {
    check(load_lines(good_lines(), Base::Ciso), "a manifest loads");
    uint8_t got[4];
    std::vector<std::string> l = {"gcn-disc-overlay 1", base_line(), "range 8000 4 dol.bin"};
    check(load_lines(l, Base::Ciso) && disc_overlay_read(0x2000, got, 4, ciso_base) && !memcmp(got, &g_image[0x2000], 4),
          "a second load replaces the first");
    check(!load_lines(with(good_lines(), 1, "base"), Base::Ciso) && !disc_overlay_loaded(), "a refused load leaves nothing loaded, even after a good one");
    check(disc_overlay_read(0x2000, got, 4, ciso_base) && !memcmp(got, &g_image[0x2000], 4), "so reads are the base's again");
}

void whole_disc() {
    check(load_lines(good_lines(), Base::Plain), "the fixture loads on a plain image for the sweeps");
    check(sweep(g_over, Base::Plain), "every start with eight lengths matches the model on a plain base");
    check(random_reads(g_over, Base::Plain, 10000, 0xBEEF), "10,000 random reads with an overlay match the model on a plain base");
    disc_overlay_unload();

    check(load_lines(good_lines(), Base::Ciso), "and on a CISO");
    check(sweep(g_over, Base::Ciso), "every start with eight lengths matches the model on a CISO base");
    check(random_reads(g_over, Base::Ciso, 10000, 0xF00D), "10,000 random reads with an overlay match the model on a CISO base");

    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < 4; t++)
        threads.emplace_back([&bad, t]() {
            if (!random_reads(g_over, t & 1 ? Base::Plain : Base::Ciso, 2500, 0x777 + t)) bad++;
        });
    for (std::thread& th : threads) th.join();
    check(bad == 0, "four threads reading at once, on both bases, all match the model");
    disc_overlay_unload();
}

void refusals() {
    const std::vector<std::string> good = good_lines();
    const std::string image_hash = hex(ref_fnv(&g_image[kFstAt], kFstSize), 16);
    std::string upper = image_hash;
    for (char& c : upper) c = (char)toupper((unsigned char)c);
    check(upper != image_hash && hex(kFstAt).find_first_of("abcdef") != std::string::npos,
          "the fixture's hash and FST offset have hex letters, so the case checks below mean something");

    // The first two lines.
    refused_text("an empty file", "", 1);
    refused("a first line of another version", with(good, 0, "gcn-disc-overlay 2"), 1);
    refused("a first line that isn't the magic", with(good, 0, "disc-overlay 1"), 1);
    refused_text("a manifest with no base line", "gcn-disc-overlay 1\n", 2);
    refused("a second line that isn't the base", {good[0], good[2]}, 2);
    refused("a base line with a field missing", with(good, 1, "base FIXT01 5a00 100"), 2);
    refused("a base line with a field too many", with(good, 1, base_line() + " 1"), 2);
    refused("a base line with a double space", with(good, 1, "base FIXT01  5a00 100 " + image_hash), 2);
    refused("a disc ID of 5 characters", with(good, 1, "base FIXT0 5a00 100 " + image_hash), 2);
    refused("a disc ID of 7 characters", with(good, 1, "base FIXT011 5a00 100 " + image_hash), 2);
    refused("an FST offset with 0x", with(good, 1, "base FIXT01 0x5a00 100 " + image_hash), 2);
    refused("an FST offset in capitals", with(good, 1, "base FIXT01 5A00 100 " + image_hash), 2);
    refused("an FST offset of more than 8 digits", with(good, 1, "base FIXT01 000005a00 100 " + image_hash), 2);
    refused("an FST size that isn't hex", with(good, 1, "base FIXT01 5a00 10g " + image_hash), 2);
    refused("a hash in capitals", with(good, 1, "base FIXT01 5a00 100 " + upper), 2, "16 lowercase hex digits");
    refused("a hash of 15 digits", with(good, 1, "base FIXT01 5a00 100 " + image_hash.substr(1)), 2, "16 lowercase hex digits");
    refused("a hash of 17 digits", with(good, 1, "base FIXT01 5a00 100 0" + image_hash), 2, "16 lowercase hex digits");
    refused("a second base line", inserted(good, 2, good[1]), 3, "more than one base");

    // The image has to be the one the manifest was made for, and the message names both hashes.
    const std::string wrong = "0123456789abcdef";
    refused("another disc ID", with(good, 1, "base FIXT02 5a00 100 " + image_hash), 2, image_hash.c_str());
    refused("another FST offset", with(good, 1, "base FIXT01 5b00 100 " + image_hash), 2, image_hash.c_str());
    refused("another FST size", with(good, 1, "base FIXT01 5a00 80 " + image_hash), 2, image_hash.c_str());
    refused("another FST hash, naming both", with(good, 1, "base FIXT01 5a00 100 " + wrong), 2, wrong.c_str(), image_hash.c_str());
    refused("a hash one digit out", with(good, 1, base_line(ref_fnv(&g_image[kFstAt], kFstSize) ^ 1)), 2, image_hash.c_str());
    {
        const int fds = open_fds();
        std::string err;
        const bool ok = load_lines(good, Base::Plain, &err);
        disc_overlay_unload();
        check(ok && open_fds() == fds, "the unmodified manifest still loads after all those refusals");
        write_text(manifest_path(), join(good));
        err.clear();
        check(!disc_overlay_load(manifest_path().c_str(), failing_base, &err) && err.find(", line 2: ") != std::string::npos, "a base that can't be read is refused at the base line");
        err.clear();
        check(!disc_overlay_load(manifest_path().c_str(), huge_fst_base, &err) && err.find(", line 2: ") != std::string::npos && err.find("2147483648 bytes") != std::string::npos,
              "and an image that claims a 2 GiB FST is refused before it is read");
    }

    // Header lines.
    refused("a header line with no value", with(good, 2, "header 420"), 3);
    refused("a header line with a field too many", with(good, 2, "header 420 8000 1"), 3);
    refused("a header offset off a 4-byte boundary", with(good, 2, "header 421 8000"), 3);
    refused("a header offset past the header", with(good, 2, "header 440 8000"), 3);
    refused("a header offset of 0x43D", with(good, 2, "header 43d 8000"), 3);
    refused("a header value of more than 32 bits", with(good, 2, "header 420 100000000"), 3);
    refused("a header value that isn't hex", with(good, 2, "header 420 80g0"), 3);
    refused("a header offset in capitals", with(good, 5, "header 42C 160"), 6);
    refused("a header word set twice", inserted(good, 6, "header 420 9000"), 7);

    // Range lines.
    refused("a range line with no path", with(good, 6, "range 2000 100"), 7);
    refused("a range line with an empty path", with(good, 6, "range 2000 100 "), 7, "<path>");
    refused("a range with no bytes", with(good, 8, "range 8000 0 dol.bin"), 9, "no bytes");
    refused("a range length that isn't hex", with(good, 6, "range 2000 1g0 inner.bin"), 7);
    refused("a range offset of 17 digits", with(good, 6, "range 10000000000000000 100 inner.bin"), 7);
    refused("a range that runs past the end of the offsets", with(good, 12, "range ffffffffffffff00 100 tail.bin"), 13, "past the end of the offsets");
    refused("a range that overlaps the next by one byte", with(good, 8, "range 8000 201 dol.bin"), 10);
    refused("a range that starts inside the one before", with(good, 7, "range 20ff 160 fst.bin"), 8);
    refused("a range that starts before the one before", {good[0], good[1], good[2], good[3], good[4], good[5], good[7], good[6]}, 8);
    refused("a tab in a path", with(good, 6, "range 2000 100 inner\t.bin"), 7, "control character");
    refused("a CR at the end of a line", with(good, 6, good[6] + "\r"), 7, "control character");
    refused("a DEL in a path", with(good, 6, "range 2000 100 inner\x7f.bin"), 7, "control character");
    refused("a line that is neither header nor range", inserted(good, 6, "frob 1 2"), 7);
    refused("a blank line", inserted(good, 6, ""), 7);
    refused_text("a last line with no newline, which may be cut short", join(good).substr(0, join(good).size() - 1), 13);

    // The host files.
    refused("a file that isn't there", with(good, 7, "range 6000 160 absent.bin"), 8, "absent.bin");
    refused("a file one byte short", with(good, 10, "range 8400 2 one.bin"), 11, "one.bin");
    refused("a path that is a folder", with(good, 6, "range 2000 100 mod files"), 7, "mod files", "folder");

    // The manifest itself.
    std::string err;
    check(!disc_overlay_load("", plain_base, &err) && err.find("--disc-overlay=") == 0 && err.find("no manifest path") != std::string::npos,
          "an empty manifest name is refused as the flag with nothing after it");
    const std::string none = g_out + "/none.txt";
    check(!disc_overlay_load(none.c_str(), plain_base, &err) && err.find(none) == 0, "a manifest that isn't there is refused naming it");
    check(!disc_overlay_load(g_out.c_str(), plain_base, &err) && err.find(g_out) == 0, "and so is a folder");
    const std::string big = g_out + "/big.txt";
    write_text(big, join({good[0], good[1]}));
    check(truncate(big.c_str(), (16 << 20) + 1) == 0, "a manifest of 16 MiB and a byte is made");
    check(!disc_overlay_load(big.c_str(), plain_base, &err) && err.find(big) == 0 && err.find("16 MiB") != std::string::npos, "and refused as too big to be one");
    check(truncate(big.c_str(), 16 << 20) == 0, "one of exactly 16 MiB is made");
    check(!disc_overlay_load(big.c_str(), plain_base, &err) && err.find(big) == 0 && err.find("16 MiB") == std::string::npos,
          "and is read, then refused for what it holds");
}

// 300 range files in a child whose limit on open files is cut to 100 and whose hard limit is
// 350. They only open if the loader raises the soft limit, and only if it stops at the hard one.
constexpr int kMany = 300;
constexpr uint64_t kManyAt = 0x100000, kManyStride = 0x80, kManyLen = 0x40;

std::string many_name(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "many/f%03d.bin", i);
    return buf;
}

bool many_child() {
    const int before = g_failed;
    rlimit lim;
    getrlimit(RLIMIT_NOFILE, &lim);
    if (lim.rlim_max != RLIM_INFINITY && lim.rlim_max < 350) {
        check(false, "the hard limit on open files is 350 or more, which this check needs");
        return false;
    }
    lim.rlim_cur = 100;
    lim.rlim_max = 350;
    check(setrlimit(RLIMIT_NOFILE, &lim) == 0, "the child cuts its open-file limits to 100 and 350");
    const int fds = open_fds();

    std::string err;
    const bool loaded = disc_overlay_load(manifest_path().c_str(), ciso_base, &err);
    check(loaded, "300 range files load with only 100 files allowed at the start");
    if (!loaded) {
        printf("      error was: %s\n", err.c_str());
        return false;
    }
    check(open_fds() - fds == kMany, "and each is open once");
    getrlimit(RLIMIT_NOFILE, &lim);
    check(lim.rlim_cur > (rlim_t)kMany && lim.rlim_cur <= 350, "the soft limit rose, and stopped at the hard one");

    std::vector<uint8_t> region(kMany * kManyStride, 0);
    for (int i = 0; i < kMany; i++) {
        const std::vector<uint8_t> d = stream(100 + (uint32_t)i, kManyLen);
        memcpy(&region[i * kManyStride], d.data(), kManyLen);
    }
    bool each = true;
    for (int i = 0; i < kMany; i++) {
        uint8_t got[kManyLen];
        each = each && disc_overlay_read(kManyAt + i * kManyStride, got, kManyLen, ciso_base) && !memcmp(got, &region[i * kManyStride], kManyLen);
    }
    check(each, "every one of the 300 files reads back whole");
    std::vector<uint8_t> all(region.size());
    check(disc_overlay_read(kManyAt, all.data(), (uint32_t)all.size(), ciso_base) && all == region, "and one read across all 300 and the gaps between them joins them");
    uint32_t s = 99;
    bool random = true;
    for (int i = 0; i < 3000 && random; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const uint64_t off = s % region.size();
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const uint32_t len = (uint32_t)std::min<uint64_t>(1 + s % 0x300, region.size() - off);
        uint8_t got[0x300];
        random = disc_overlay_read(kManyAt + off, got, len, ciso_base) && !memcmp(got, &region[off], len);
    }
    check(random, "3,000 random reads over them match");
    disc_overlay_unload();
    check(open_fds() == fds, "an unload closes all 300");
    return g_failed == before;
}

// The same files in a child that can't raise its limit past 100. The load fails when the files
// run out, and its message gives the limit.
bool limited_child() {
    const int before = g_failed;
    rlimit lim;
    lim.rlim_cur = lim.rlim_max = 100;
    check(setrlimit(RLIMIT_NOFILE, &lim) == 0, "the child cuts both open-file limits to 100");
    const int fds = open_fds();
    std::string err;
    const bool loaded = disc_overlay_load(manifest_path().c_str(), ciso_base, &err);
    check(!loaded && err.find(manifest_path()) == 0 && err.find(", line ") != std::string::npos && err.find("can't be opened") != std::string::npos &&
              err.find("limit on open files stops at 100") != std::string::npos,
          "300 files don't load when the limit can't rise, and the message gives the limit");
    if (loaded || err.find("stops at 100") == std::string::npos) printf("      error was: %s\n", err.c_str());
    check(!disc_overlay_loaded() && open_fds() == fds, "and the files it did open are closed");
    return g_failed == before;
}

bool in_child(bool (*fn)()) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        const bool passed = fn();
        fflush(stdout);
        _exit(passed ? 0 : 1);
    }
    int status = 0;
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void many_files() {
    bool ok = mkdir((g_out + "/many").c_str(), 0755) == 0;
    std::vector<std::string> l = {"gcn-disc-overlay 1", base_line()};
    for (int i = 0; i < kMany; i++) {
        const std::vector<uint8_t> d = stream(100 + (uint32_t)i, kManyLen);
        ok = write_file(g_out + "/" + many_name(i), d.data(), d.size()) && ok;
        l.push_back("range " + hex(kManyAt + i * kManyStride) + " " + hex(kManyLen) + " " + many_name(i));
    }
    write_text(manifest_path(), join(l));
    check(ok, "300 range files and their manifest are written");
    check(in_child(many_child), "the 300-file child passed");
    check(in_child(limited_child), "and so did the one whose limit can't rise");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_disc_overlay <work dir>\n");
        return 2;
    }
    g_dir = argv[1];
    struct stat st;
    if (stat(g_dir.c_str(), &st) == 0 || mkdir(g_dir.c_str(), 0755) != 0) {
        fprintf(stderr, "%s exists or can't be made: give a new directory\n", g_dir.c_str());
        return 2;
    }
    g_out = g_dir + "/out dir";
    if (!make_fixture()) {
        fprintf(stderr, "the fixture couldn't be written under %s\n", g_dir.c_str());
        return 2;
    }
    hash_vectors();
    path_choice();
    no_overlay();
    load_and_close();
    reads_through();
    padding_reads();
    header_extras();
    shared_and_longer_files();
    replacing();
    whole_disc();
    refusals();
    many_files();
    printf("test_disc_overlay: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
