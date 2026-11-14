// See banner.h.
#include "banner.h"
#include <cstring>
#include <vector>

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

constexpr size_t kBnr1Size = 0x1960, kBnr2Size = 0x1FA0, kPictureAt = 0x20;
constexpr size_t kMaxFst = 1u << 20;

// 5, 4 and 3 bits to 8, the top bits repeated below.
uint8_t from5(uint32_t v) { return (uint8_t)(v << 3 | v >> 2); }
uint8_t from4(uint32_t v) { return (uint8_t)(v * 17); }
uint8_t from3(uint32_t v) { return (uint8_t)(v << 5 | v << 2 | v >> 1); }

}  // namespace

bool banner_decode(const uint8_t* bnr, size_t size, uint8_t* rgba) {
    const bool bnr1 = size == kBnr1Size && memcmp(bnr, "BNR1", 4) == 0;
    const bool bnr2 = size == kBnr2Size && memcmp(bnr, "BNR2", 4) == 0;
    if (!bnr1 && !bnr2) return false;
    const uint8_t* src = bnr + kPictureAt;
    for (int ty = 0; ty < kBannerH / 4; ty++)
        for (int tx = 0; tx < kBannerW / 4; tx++)
            for (int i = 0; i < 16; i++, src += 2) {
                const uint32_t v = (uint32_t)src[0] << 8 | src[1];
                uint8_t* p = rgba + ((size_t)(ty * 4 + i / 4) * kBannerW + tx * 4 + i % 4) * 4;
                if (v & 0x8000) {
                    p[0] = from5(v >> 10 & 31);
                    p[1] = from5(v >> 5 & 31);
                    p[2] = from5(v & 31);
                    p[3] = 255;
                } else {
                    p[0] = from4(v >> 8 & 15);
                    p[1] = from4(v >> 4 & 15);
                    p[2] = from4(v & 15);
                    p[3] = from3(v >> 12 & 7);
                }
            }
    return true;
}

bool fst_find_root_file(const uint8_t* fst, size_t size, const char* name, uint32_t* off, uint32_t* len) {
    if (size < 12 || size > kMaxFst || fst[size - 1] != 0) return false;
    const uint32_t count = be32(fst + 8);
    if (count == 0 || (uint64_t)count * 12 > size) return false;
    const size_t names = (size_t)count * 12;
    for (uint32_t i = 1; i < count;) {
        const uint8_t* e = fst + (size_t)i * 12;
        if (e[0]) {  // a directory: on past its entries
            const uint32_t end = be32(e + 8);
            if (end <= i || end > count) return false;
            i = end;
            continue;
        }
        const size_t at = names + (be32(e) & 0xFFFFFF);
        if (at >= size) return false;
        if (strcmp((const char*)fst + at, name) == 0) {
            *off = be32(e + 4);
            *len = be32(e + 8);
            return true;
        }
        i++;
    }
    return false;
}

bool banner_read(const DiscRead& read, uint8_t* rgba) {
    uint8_t place[8];
    if (!read(0x424, place, sizeof(place))) return false;
    const uint32_t fst_off = be32(place), fst_size = be32(place + 4);
    if (fst_size < 12 || fst_size > kMaxFst) return false;
    std::vector<uint8_t> fst(fst_size);
    if (!read(fst_off, fst.data(), fst_size)) return false;
    uint32_t off = 0, len = 0;
    if (!fst_find_root_file(fst.data(), fst.size(), "opening.bnr", &off, &len)) return false;
    if (len != kBnr1Size && len != kBnr2Size) return false;
    std::vector<uint8_t> bnr(len);
    return read(off, bnr.data(), len) && banner_decode(bnr.data(), bnr.size(), rgba);
}
