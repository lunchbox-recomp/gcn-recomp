// fres and frsqrte estimate checks: the `test_gekko_estimate` target. Runs with no window, GL,
// disc or game.
//
// GEKKO_FRES and GEKKO_FRSQRTE (runtime/recomp.h, with the tables in runtime/gekko_estimate.h)
// give the 750's estimates. First, ordinary operands: chosen ones as bits, then a sweep over
// every table entry at the ends and middle of its step, which hashes to what Cemu's
// frsqrte_espresso and fres_espresso give on the same operands, each result within the
// manual's one part in 4096. Then the special values of the 750CL manual's tables: zeros,
// infinities, NaNs quiet and signaling, negative operands to frsqrte, denormal operands, and
// fres results above and under the single range.
//
// usage: test_gekko_estimate
// Exits 0 only if every check passes.
#include "recomp.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int g_failed;

uint64_t bits(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

double value(uint64_t u) {
    double d;
    memcpy(&d, &u, 8);
    return d;
}

uint32_t single_bits(double d) {
    const float f = (float)d;
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        g_failed++;
    }
}

void gives(double (*f)(double), uint64_t in, uint64_t want, const char* what) {
    const uint64_t got = bits(f(value(in)));
    if (got != want) {
        std::printf("FAIL: %s: %016llX gave %016llX, not %016llX\n", what, (unsigned long long)in,
                    (unsigned long long)got, (unsigned long long)want);
        g_failed++;
    }
}

// fres's result as a single's bits, checking it's a single.
void gives_single(uint64_t in, uint32_t want, const char* what) {
    const double r = GEKKO_FRES(value(in));
    const uint32_t got = single_bits(r);
    if (got != want || bits((double)(float)r) != bits(r)) {
        std::printf("FAIL: %s: %016llX gave %016llX (single %08X), not single %08X\n", what,
                    (unsigned long long)in, (unsigned long long)bits(r), got, want);
        g_failed++;
    }
}

uint64_t fnv(uint64_t h, uint64_t r) {
    return (h ^ r) * 0x100000001B3ull;
}

constexpr uint64_t kJunk = 0x1234567;  // under the bits either table reads
constexpr uint64_t kGreatestSingle = 0x47EFFFFFE0000000ull;
constexpr uint64_t kDefaultQnan = 0x7FF8000000000000ull;

void ordinary() {
    gives(GEKKO_FRSQRTE, 0x3FF0000000000000ull, 0x3FEFFE8000000000ull, "frsqrte(1)");
    gives(GEKKO_FRSQRTE, 0x4000000000000000ull, 0x3FE69FA000000000ull, "frsqrte(2), the odd exponent's half");
    gives(GEKKO_FRSQRTE, 0x4010000000000000ull, 0x3FDFFE8000000000ull, "frsqrte(4)");
    gives(GEKKO_FRSQRTE, 0x3FD3333333333333ull, 0x3FFD368178000000ull, "frsqrte(0.3), a step into an entry");
    gives(GEKKO_FRSQRTE, 0x0020000000000000ull, 0x5FD69FA000000000ull, "frsqrte(2^-1021)");
    gives(GEKKO_FRES, 0x3FF0000000000000ull, 0x3FEFFF0000000000ull, "fres(1)");
    gives(GEKKO_FRES, 0x4008000000000000ull, 0x3FD5550000000000ull, "fres(3)");
    gives(GEKKO_FRES, 0x3FD3333333333333ull, 0x400AAB4DC0000000ull, "fres(0.3)");
    gives(GEKKO_FRES, 0x3FFC000000000000ull, 0x3FE2488000000000ull, "fres(1.75)");
    gives(GEKKO_FRES, 0xBFFC000000000000ull, 0xBFE2488000000000ull, "fres(-1.75)");
    gives(GEKKO_FRES, 0x37F0000000000000ull, 0x47EFFF0000000000ull, "fres(2^-128), the greatest in range");
    gives(GEKKO_FRES, 0x47C0000000000000ull, 0x381FFF0000000000ull, "fres(2^125), the least normal result");

    const int rs_exp[] = {1, 2, 1000, 1021, 1022, 1023, 1024, 2045, 2046};
    const uint64_t rs_step[] = {0, 1, 1024, 2047};
    uint64_t h = 0xCBF29CE484222325ull;
    bool near = true;
    for (int e : rs_exp)
        for (uint64_t i = 0; i < 16; i++)
            for (uint64_t s : rs_step) {
                const uint64_t x = (uint64_t)e << 52 | i << 48 | s << 37 | kJunk;
                const double r = GEKKO_FRSQRTE(value(x));
                h = fnv(h, bits(r));
                near = near && std::fabs(r * std::sqrt(value(x)) - 1) <= 1.0 / 4096;
            }
    check(h == 0xEFD7E91561083025ull, "frsqrte over every entry hashes to Cemu's");
    check(near, "frsqrte over every entry is within one part in 4096");

    const int re_exp[] = {895, 896, 1000, 1022, 1023, 1024, 1147, 1148};
    const uint64_t re_step[] = {0, 1, 512, 1023};
    h = 0xCBF29CE484222325ull;
    near = true;
    bool odd = true;
    for (int e : re_exp)
        for (uint64_t i = 0; i < 32; i++)
            for (uint64_t s : re_step) {
                const uint64_t x = (uint64_t)e << 52 | i << 47 | s << 37 | kJunk;
                const double r = GEKKO_FRES(value(x));
                h = fnv(h, bits(r));
                near = near && std::fabs(r * value(x) - 1) <= 1.0 / 4096;
                odd = odd && bits(GEKKO_FRES(-value(x))) == (bits(r) | 0x8000000000000000ull);
            }
    check(h == 0xB988C6CC9F287325ull, "fres over every entry hashes to Cemu's");
    check(near, "fres over every entry is within one part in 4096");
    check(odd, "fres of a negative operand is the positive one's, negated");
}

void special() {
    // frsqrte, page 427
    gives(GEKKO_FRSQRTE, 0x0000000000000000ull, 0x7FF0000000000000ull, "frsqrte(+0) is +inf");
    gives(GEKKO_FRSQRTE, 0x8000000000000000ull, 0xFFF0000000000000ull, "frsqrte(-0) is -inf");
    gives(GEKKO_FRSQRTE, 0x7FF0000000000000ull, 0x0000000000000000ull, "frsqrte(+inf) is +0");
    gives(GEKKO_FRSQRTE, 0xFFF0000000000000ull, kDefaultQnan, "frsqrte(-inf) is the invalid QNaN");
    gives(GEKKO_FRSQRTE, 0xBFF0000000000000ull, kDefaultQnan, "frsqrte(-1) is the invalid QNaN");
    gives(GEKKO_FRSQRTE, 0x8000000000000001ull, kDefaultQnan, "frsqrte of a negative denormal too");
    gives(GEKKO_FRSQRTE, 0x7FF8000000000001ull, 0x7FF8000000000001ull, "frsqrte keeps a QNaN");
    gives(GEKKO_FRSQRTE, 0xFFF8000000000123ull, 0xFFF8000000000123ull, "frsqrte keeps a negative QNaN");
    gives(GEKKO_FRSQRTE, 0x7FF0000000000001ull, 0x7FF8000000000001ull, "frsqrte quiets an SNaN");
    gives(GEKKO_FRSQRTE, 0xFFF4000000000000ull, 0xFFFC000000000000ull, "frsqrte quiets an SNaN, keeping its sign");
    // A denormal reads as normalized: 2^-1023 gives 2^-1021's estimate times 2, the least
    // denormal 2^-1074 gives 1's times 2^537, and 1.5 * 2^-1023 gives 1.5 * 2^-1021's times 2.
    gives(GEKKO_FRSQRTE, 0x0008000000000000ull, 0x5FE69FA000000000ull, "frsqrte(2^-1023)");
    gives(GEKKO_FRSQRTE, 0x0000000000000001ull, 0x617FFE8000000000ull, "frsqrte(2^-1074)");
    gives(GEKKO_FRSQRTE, 0x000C000000000000ull, bits(GEKKO_FRSQRTE(value(0x0028000000000000ull))) + (1ull << 52),
          "frsqrte(1.5 * 2^-1023)");

    // fres, page 424
    gives(GEKKO_FRES, 0x0000000000000000ull, 0x7FF0000000000000ull, "fres(+0) is +inf");
    gives(GEKKO_FRES, 0x8000000000000000ull, 0xFFF0000000000000ull, "fres(-0) is -inf");
    gives(GEKKO_FRES, 0x7FF0000000000000ull, 0x0000000000000000ull, "fres(+inf) is +0");
    gives(GEKKO_FRES, 0xFFF0000000000000ull, 0x8000000000000000ull, "fres(-inf) is -0");
    gives(GEKKO_FRES, 0x7FF8000000000001ull, 0x7FF8000000000001ull, "fres keeps a QNaN");
    gives(GEKKO_FRES, 0xFFF0000000000001ull, 0xFFF8000000000001ull, "fres quiets an SNaN, keeping its sign");
    // Above the single range: the greatest single of the operand's sign.
    gives(GEKKO_FRES, 0x37E0000000000000ull, kGreatestSingle, "fres(2^-129)");
    gives(GEKKO_FRES, 0xB7E0000000000000ull, kGreatestSingle | 0x8000000000000000ull, "fres(-2^-129)");
    gives(GEKKO_FRES, 0x0000000000000001ull, kGreatestSingle, "fres of the least denormal");
    gives(GEKKO_FRES, 0x800FFFFFFFFFFFFFull, kGreatestSingle | 0x8000000000000000ull, "fres of a negative denormal");
    // Under it: a single denormal with its low bits cut, then a zero of the operand's sign.
    gives_single(0x47D0000000000000ull, 0x007FFC00u, "fres(2^126)");
    gives_single(0xC7D0000000000000ull, 0x807FFC00u, "fres(-2^126)");
    gives_single(0x47D0002000000000ull, 0x007FFB07u, "fres(2^126 + a step), cut and not rounded");
    gives_single(0x4930000000000000ull, 0x00000001u, "fres(2^148), the least denormal");
    gives_single(0x4940000000000000ull, 0x00000000u, "fres(2^149) is +0");
    gives_single(0xC940000000000000ull, 0x80000000u, "fres(-2^149) is -0");
    gives_single(0x7FEFFFFFFFFFFFFFull, 0x00000000u, "fres of the greatest double is +0");
}

}  // namespace

int main() {
    ordinary();
    special();
    if (g_failed) {
        std::printf("%d failed\n", g_failed);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
