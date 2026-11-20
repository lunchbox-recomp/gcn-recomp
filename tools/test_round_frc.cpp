// frC rounding checks: the `test_round_frc` target. Runs with no window, GL, disc or game.
//
// Gekko's single-precision multiplies round frC's significand to 25 bits before they multiply
// (ROUND_FRC in runtime/recomp.h). These are its results on chosen values, as bits: normal
// doubles rounding down, up, on a tie and with a carry into the exponent, singles and values
// of 25 bits that come through unchanged, denormals by their leading one, zeros, infinities
// and NaNs. Then products: 1 times a frC that isn't a single comes out as the hardware gives
// it, where the product of the whole value would round the other way.
//
// usage: test_round_frc
// Exits 0 only if every check passes.
#include "recomp.h"
#include "test_fp_bits.h"
#include <cstdio>

namespace {

int g_failed;

void rounds(uint64_t in, uint64_t want, const char* what) {
    const uint64_t got = bits(ROUND_FRC(value(in)));
    const bool ok = got == want;
    printf("%s: %s: %016llX -> %016llX", ok ? "ok" : "FAIL", what, (unsigned long long)in,
           (unsigned long long)got);
    if (!ok) printf(", want %016llX", (unsigned long long)want);
    printf("\n");
    if (!ok) g_failed++;
}

// fmuls of a by c: the product with frC rounded, then rounded to single, as translate's C has it.
void product(uint64_t a, uint64_t c, uint32_t want, uint32_t whole, const char* what) {
    const uint32_t got = single_bits(value(a) * ROUND_FRC(value(c)));
    const uint32_t unrounded = single_bits(value(a) * value(c));
    const bool ok = got == want && unrounded == whole && want != whole;
    printf("%s: %s: %08X (frC whole gives %08X)", ok ? "ok" : "FAIL", what, got, unrounded);
    if (!ok) printf(", want %08X and %08X whole", want, whole);
    printf("\n");
    if (!ok) g_failed++;
}

}  // namespace

int main() {
    // Normal doubles: the top 24 fraction bits stay, and bit 27, the first that goes, rounds.
    rounds(0x3FF0000000000000ull, 0x3FF0000000000000ull, "1.0 is a single");
    rounds(0x3FF0000020000000ull, 0x3FF0000020000000ull, "1 + 2^-23 is a single");
    rounds(0x3FF0000010000000ull, 0x3FF0000010000000ull, "1 + 2^-24 has 25 bits");
    rounds(0x3FF0000007FFFFFFull, 0x3FF0000000000000ull, "just under half a step rounds down");
    rounds(0x3FF0000008000000ull, 0x3FF0000010000000ull, "a tie rounds up, away from zero");
    rounds(0xBFF0000008000000ull, 0xBFF0000010000000ull, "a negative tie rounds away from zero");
    rounds(0x3FF0000018000000ull, 0x3FF0000020000000ull, "a tie above an odd step rounds up");
    rounds(0x3FF0000017FFFFFFull, 0x3FF0000010000000ull, "under a tie above an odd step");
    rounds(0x3FF000000FFFFFFFull, 0x3FF0000010000000ull, "over half a step rounds up");
    rounds(0x3FFFFFFFF8000000ull, 0x4000000000000000ull, "a carry goes into the exponent");
    rounds(0x0010000008000000ull, 0x0010000010000000ull, "the least normal's tie");
    rounds(0x7FEFFFFFF8000000ull, 0x7FF0000000000000ull, "a tie under 2^1024 rounds to infinity");
    rounds(0x7FEFFFFFF7FFFFFFull, 0x7FEFFFFFF0000000ull, "the largest double that stays finite");
    // Double denormals keep 25 bits from their leading one.
    rounds(0x0000000000000001ull, 0x0000000000000001ull, "the least denormal");
    rounds(0x0000000001FFFFFFull, 0x0000000001FFFFFFull, "a denormal of 25 bits");
    rounds(0x0000000003FFFFFFull, 0x0000000004000000ull, "a denormal of 26 ones rounds up");
    rounds(0x0000000002000001ull, 0x0000000002000002ull, "a denormal of 26 bits on a tie");
    rounds(0x000800000A000000ull, 0x0008000008000000ull, "a denormal under a tie rounds down");
    rounds(0x000FFFFFFFFFFFFFull, 0x0010000000000000ull, "the largest denormal rounds to normal");
    rounds(0x800800000C000000ull, 0x8008000010000000ull, "a negative denormal's tie");
    // Zeros, infinities and NaNs.
    rounds(0x0000000000000000ull, 0x0000000000000000ull, "+0");
    rounds(0x8000000000000000ull, 0x8000000000000000ull, "-0");
    rounds(0x7FF0000000000000ull, 0x7FF0000000000000ull, "+infinity");
    rounds(0xFFF0000000000000ull, 0xFFF0000000000000ull, "-infinity");
    rounds(0x7FFFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, "a NaN whose rounding would carry stays");
    rounds(0x7FF0000008000000ull, 0x7FF0000008000000ull, "a signaling NaN stays");
    rounds(0xFFF8000000000001ull, 0xFFF8000000000001ull, "a negative quiet NaN stays");

    // 1 + 2^-24 + 2^-26 in frC: rounded to 1 + 2^-24, its product with 1 is a tie that goes to
    // even, 1.0, where the whole value is over the tie and gives 1 + 2^-23.
    product(0x3FF0000000000000ull, 0x3FF0000014000000ull, 0x3F800000u, 0x3F800001u,
            "1 * (1 + 2^-24 + 2^-26)");
    product(0xBFF0000000000000ull, 0x3FF0000014000000ull, 0xBF800000u, 0xBF800001u,
            "-1 * (1 + 2^-24 + 2^-26)");
    // A denormal frC by 2^1023: rounded from its leading one, it gives 1.0, where the whole
    // value gives 1 + 2^-23. Rounded at bit 27 as a normal double is, it would give the second.
    product(0x7FE0000000000000ull, 0x000800000A000000ull, 0x3F800000u, 0x3F800001u,
            "2^1023 * a denormal");

    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
