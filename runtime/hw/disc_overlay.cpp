// A disc overlay at run time. See disc_overlay.h.
#include "disc_overlay.h"
#include "../platform.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

const char* g_disc_overlay_flag = nullptr;
void (*g_disc_overlay_log)(const char* line) = nullptr;

namespace {

constexpr uint32_t kHeaderEnd = 0x440;         // the disc header, where the BI2 begins
constexpr size_t kMaxManifest = 16u << 20;     // a bigger file isn't a manifest
constexpr uint32_t kMaxFst = 16u << 20;        // the most of the image's FST that load will hash
constexpr uint32_t kSpareFiles = 128;          // files the rest of the process holds open
constexpr const char* kMagic = "gcn-disc-overlay 1";

struct Range {
    uint64_t off, len;
    uint32_t file;  // into Overlay::files
};

struct Word {
    uint32_t off, value;
};

// What load makes and nothing changes afterward, so reads need no lock.
struct Overlay {
    std::vector<Range> ranges;  // sorted by off, none overlapping
    std::vector<Word> words;
    std::vector<std::string> names;
    std::vector<PlatFile*> files;
};

Overlay* g_overlay;

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void say(const char* fmt, ...) {
    if (!g_disc_overlay_log) return;
    char line[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    g_disc_overlay_log(line);
}

uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Lowercase hex with no 0x and 1 to `max` digits, as the builder writes it.
bool parse_hex(const std::string& s, size_t max, uint64_t* out) {
    if (s.empty() || s.size() > max) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c >= '0' && c <= '9') v = v << 4 | (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v << 4 | (uint64_t)(c - 'a' + 10);
        else return false;
    }
    *out = v;
    return true;
}

// Splits at single spaces, at most `max` fields, the last one holding the rest of the line.
std::vector<std::string> split(const std::string& s, size_t max) {
    std::vector<std::string> out;
    size_t at = 0;
    while (out.size() + 1 < max) {
        const size_t sp = s.find(' ', at);
        if (sp == std::string::npos) break;
        out.push_back(s.substr(at, sp - at));
        at = sp + 1;
    }
    out.push_back(s.substr(at));
    return out;
}

std::string printable(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) s += (p[i] >= 0x21 && p[i] < 0x7F) ? (char)p[i] : '?';
    return s;
}

std::string hex(uint64_t v, int digits = 1) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%0*llx", digits, (unsigned long long)v);
    return buf;
}

struct BaseLine {
    std::string id;
    uint32_t fst_at = 0, fst_size = 0;
    uint64_t hash = 0;
    int line = 0;
};

struct RangeLine {
    uint64_t off = 0, len = 0;
    std::string rel;
    int line = 0;
};

struct Manifest {
    BaseLine base;
    std::vector<Word> words;
    std::vector<RangeLine> ranges;
};

// The errors name the manifest and the line, so one place builds them.
struct Failure {
    const char* manifest;
    std::string* error;
    bool at(int line, const std::string& why) const {
        if (error) *error = std::string(manifest) + ", line " + std::to_string(line) + ": " + why;
        return false;
    }
};

bool parse_line(const Failure& fail, const std::string& text, int n, Manifest* m) {
    for (unsigned char c : text)
        if (c < 0x20 || c == 0x7F)
            return fail.at(n, "the line has a control character, such as a tab or the CR of a Windows line ending");
    if (n == 1) {
        if (text != kMagic) return fail.at(n, std::string("the first line has to be \"") + kMagic + "\"");
        return true;
    }
    if (n == 2) {
        const std::vector<std::string> f = split(text, 6);
        uint64_t at, size, hash;
        if (f[0] != "base") return fail.at(n, "the second line has to be the base line");
        if (f.size() != 5) return fail.at(n, "the base line is \"base <disc id> <fst offset> <fst size> <hash>\"");
        if (f[1].size() != 6 || std::any_of(f[1].begin(), f[1].end(), [](char c) { return c < 0x21 || c > 0x7E; }))
            return fail.at(n, "the disc ID has to be 6 printable characters");
        if (!parse_hex(f[2], 8, &at) || !parse_hex(f[3], 8, &size))
            return fail.at(n, "the FST's offset and size are hex with no 0x, up to 8 lowercase digits");
        if (f[4].size() != 16 || !parse_hex(f[4], 16, &hash))
            return fail.at(n, "the FST's hash is 16 lowercase hex digits");
        m->base = BaseLine{f[1], (uint32_t)at, (uint32_t)size, hash, n};
        return true;
    }
    if (text.compare(0, 5, "base ") == 0) return fail.at(n, "there is more than one base line");
    if (text.compare(0, 7, "header ") == 0) {
        const std::vector<std::string> f = split(text, 4);
        uint64_t off, value;
        if (f.size() != 3) return fail.at(n, "a header line is \"header <offset> <value>\"");
        if (!parse_hex(f[1], 8, &off) || !parse_hex(f[2], 8, &value))
            return fail.at(n, "the offset and the value are hex with no 0x, up to 8 lowercase digits");
        if (off % 4 || off + 4 > kHeaderEnd)
            return fail.at(n, "a header word sits on a 4-byte boundary inside the first 0x440 bytes");
        for (const Word& w : m->words)
            if (w.off == off) return fail.at(n, "this header word is already set");
        m->words.push_back(Word{(uint32_t)off, (uint32_t)value});
        return true;
    }
    if (text.compare(0, 6, "range ") == 0) {
        const std::vector<std::string> f = split(text, 4);
        uint64_t off, len;
        if (f.size() != 4 || f[3].empty()) return fail.at(n, "a range line is \"range <offset> <length> <path>\"");
        if (!parse_hex(f[1], 16, &off) || !parse_hex(f[2], 16, &len))
            return fail.at(n, "the offset and the length are hex with no 0x, up to 16 lowercase digits");
        if (len == 0) return fail.at(n, "the range has no bytes");
        if (len > UINT64_MAX - off) return fail.at(n, "the range runs past the end of the offsets");
        if (!m->ranges.empty()) {
            const RangeLine& prev = m->ranges.back();
            if (off < prev.off + prev.len)
                return fail.at(n, "the ranges have to be sorted and never overlap, and this one starts before the end of the one on line " + std::to_string(prev.line));
        }
        m->ranges.push_back(RangeLine{off, len, f[3], n});
        return true;
    }
    return fail.at(n, "the line isn't a header or a range line");
}

bool parse(const Failure& fail, const std::string& text, Manifest* m) {
    if (text.empty()) return fail.at(1, "the file is empty");
    int n = 0;
    size_t at = 0;
    while (at < text.size()) {
        n++;
        const size_t nl = text.find('\n', at);
        if (nl == std::string::npos) return fail.at(n, "the file ends without a newline, so this line may be cut short");
        if (!parse_line(fail, text.substr(at, nl - at), n, m)) return false;
        at = nl + 1;
    }
    if (n < 2) return fail.at(2, "the base line is missing");
    return true;
}

// The disc ID, FST offset and size, and the hash of the FST, read from the image as it is.
bool check_base(const Failure& fail, const BaseLine& want, BaseRead base) {
    uint8_t hdr[0x430];
    if (!base(0, hdr, sizeof(hdr))) return fail.at(want.line, "the disc image's header can't be read");
    const uint32_t at = be32(hdr + 0x424), size = be32(hdr + 0x428);
    if (size > kMaxFst) return fail.at(want.line, "the image's FST is " + std::to_string(size) + " bytes, which is too many to be this overlay's base");
    std::vector<uint8_t> fst(size);
    if (size && !base(at, fst.data(), size)) return fail.at(want.line, "the image's FST can't be read");
    const uint64_t hash = disc_overlay_fnv1a64(fst.data(), fst.size());
    if (memcmp(hdr, want.id.data(), 6) == 0 && at == want.fst_at && size == want.fst_size && hash == want.hash) return true;
    return fail.at(want.line,
                   "the overlay was made for another image. It names disc " + want.id + " with its FST at " + hex(want.fst_at) +
                       " of " + hex(want.fst_size) + " bytes, hash " + hex(want.hash, 16) + ". This image is disc " +
                       printable(hdr, 6) + " with its FST at " + hex(at) + " of " + hex(size) + " bytes, hash " + hex(hash, 16) + ".");
}

void close_all(Overlay* o) {
    for (PlatFile* f : o->files) plat_close_read(f);
    delete o;
}

// A piece of the disc that no range covers. The image gives what it has, and what it hasn't
// reads as zeros: a plain image has nothing past its end, and a CISO gives zeros there, so
// this keeps the two bases giving the game the same bytes.
bool read_gap(BaseRead base, uint64_t pos, uint8_t* dst, uint32_t len) {
    if (base(pos, dst, len)) return true;
    // The base has the first n bytes exactly when it can read the byte at n - 1. `have` is the
    // most it's known to have and `lack` the least it's known not to.
    uint32_t have = 0, lack = len;
    uint8_t probe;
    while (lack - have > 1) {
        const uint32_t mid = have + (lack - have) / 2;
        if (base(pos + mid - 1, &probe, 1)) have = mid;
        else lack = mid;
    }
    if (have && !base(pos, dst, have)) return false;
    memset(dst + have, 0, len - have);
    return true;
}

}  // namespace

uint64_t disc_overlay_fnv1a64(const void* data, size_t size) {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

const char* disc_overlay_path() {
    if (g_disc_overlay_flag) return g_disc_overlay_flag;
    const char* env = getenv("GCN_DISC_OVERLAY");
    return env && *env ? env : nullptr;
}

bool disc_overlay_loaded() { return g_overlay != nullptr; }

void disc_overlay_unload() {
    Overlay* o = g_overlay;
    g_overlay = nullptr;
    if (o) close_all(o);
}

bool disc_overlay_load(const char* manifest, BaseRead base, std::string* error) {
    disc_overlay_unload();
    const Failure fail{manifest, error};

    // The whole file, since a manifest is small and a line cut short must not be taken for a shorter one.
    std::string text;
    FILE* f = fopen(manifest, "rb");
    if (!f) {
        if (error) *error = std::string(manifest) + ": the manifest can't be opened";
        return false;
    }
    char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() <= kMaxManifest) text.append(buf, got);
    const bool read_failed = ferror(f) != 0;
    fclose(f);
    if (read_failed || text.size() > kMaxManifest) {
        if (error) *error = std::string(manifest) + (read_failed ? ": the manifest can't be read" : ": the file is bigger than 16 MiB, so it isn't a manifest");
        return false;
    }

    Manifest m;
    if (!parse(fail, text, &m) || !check_base(fail, m.base, base)) return false;

    // Every host file opens once now, so a missing one stops the game at boot and a read never opens or locks.
    std::string dir = manifest;
    dir.erase(dir.find_last_of("/\\") + 1);  // npos + 1 is 0, which leaves "" for a bare name
    std::set<std::string> distinct;
    for (const RangeLine& r : m.ranges) distinct.insert(r.rel);
    plat_reserve_open_files((uint32_t)distinct.size() + kSpareFiles);

    Overlay* o = new Overlay;
    o->words = m.words;
    std::map<std::string, uint32_t> opened;
    uint64_t bytes = 0;
    for (const RangeLine& r : m.ranges) {
        auto it = opened.find(r.rel);
        if (it == opened.end()) {
            PlatFile* file = plat_open_read((dir + r.rel).c_str());
            if (!file) {
                close_all(o);
                return fail.at(r.line, "the file " + r.rel + " can't be opened");
            }
            it = opened.emplace(r.rel, (uint32_t)o->files.size()).first;
            o->files.push_back(file);
            o->names.push_back(r.rel);
        }
        uint8_t last;
        if (!plat_read_at(o->files[it->second], r.len - 1, &last, 1)) {
            close_all(o);
            return fail.at(r.line, "the file " + r.rel + " is shorter than the range's " + std::to_string(r.len) + " bytes");
        }
        o->ranges.push_back(Range{r.off, r.len, it->second});
        bytes += r.len;
    }
    g_overlay = o;
    say("disc overlay %s: %u files, %llu bytes", manifest, (unsigned)o->files.size(), (unsigned long long)bytes);
    return true;
}

bool disc_overlay_read(uint64_t off, void* dst, uint32_t len, BaseRead base) {
    const Overlay* o = g_overlay;
    if (!o) return base(off, dst, len);
    if (len == 0) return true;
    const uint64_t end = off + len;
    if (end < off) return false;
    uint8_t* out = (uint8_t*)dst;
    uint64_t pos = off;
    auto r = std::partition_point(o->ranges.begin(), o->ranges.end(), [&](const Range& x) { return x.off + x.len <= pos; });
    while (pos < end) {
        uint64_t stop;
        if (r != o->ranges.end() && r->off <= pos) {
            stop = std::min(end, r->off + r->len);
            if (!plat_read_at(o->files[r->file], pos - r->off, out + (pos - off), (uint32_t)(stop - pos))) return false;
            say("overlay read %09llX +%X from %s", (unsigned long long)pos, (unsigned)(stop - pos), o->names[r->file].c_str());
            ++r;
        } else {
            stop = r != o->ranges.end() ? std::min(end, r->off) : end;
            if (!read_gap(base, pos, out + (pos - off), (uint32_t)(stop - pos))) return false;
        }
        pos = stop;
    }
    if (off < kHeaderEnd) {
        for (const Word& w : o->words) {
            const uint64_t from = std::max<uint64_t>(off, w.off), to = std::min<uint64_t>(end, w.off + 4);
            if (from >= to) continue;
            for (uint64_t i = from; i < to; i++) out[i - off] = (uint8_t)(w.value >> (8 * (3 - (i - w.off))));
            say("overlay header word %X is %08X", (unsigned)w.off, (unsigned)w.value);
        }
    }
    return true;
}
