// The bit views that test_round_frc and test_gekko_estimate check results through: a double's
// bits, the double with given bits, and a double rounded to single as a single's bits.
#pragma once
#include <cstdint>
#include <cstring>

inline uint64_t bits(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

inline double value(uint64_t u) {
    double d;
    memcpy(&d, &u, 8);
    return d;
}

inline uint32_t single_bits(double d) {
    const float f = (float)d;
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
