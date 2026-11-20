/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Gekko's fres and frsqrte estimates: the hardware's two 32-entry tables and the arithmetic
   that reads them. Taken from Cemu (https://github.com/cemu-project/Cemu), by the Cemu
   contributors, from src/Cafe/HW/Espresso/Interpreter/PPCInterpreterFPU.cpp at commit
   ac8379ba66e8d149564f003569d9441464e7b228: frsqrteLookupTable, fresLookupTable and the
   estimate arithmetic of frsqrte_espresso and fres_espresso, split here into an exponent and
   a fraction for one positive normalized operand.

   This file alone is under the MPL-2.0; the rest of gcn-recomp is MIT. The special values
   (zeros, infinities, NaNs, negative operands, denormal operands, and fres results outside
   the single range) are handled by GEKKO_FRES and GEKKO_FRSQRTE in recomp.h, which call
   these. Must stay C. */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t offset;
    uint32_t step;
} gekko_estimate_entry;

/* Indexed by the exponent's low bit, then the top four fraction bits. */
static const gekko_estimate_entry gekko_frsqrte_table[32] = {
    {0x1a7e800, 0x568}, {0x17cb800, 0x4f3}, {0x1552800, 0x48d}, {0x130c000, 0x435},
    {0x10f2000, 0x3e7}, {0xeff000, 0x3a2},  {0xd2e000, 0x365},  {0xb7c000, 0x32e},
    {0x9e5000, 0x2fc},  {0x867000, 0x2d0},  {0x6ff000, 0x2a8},  {0x5ab800, 0x283},
    {0x46a000, 0x261},  {0x339800, 0x243},  {0x218800, 0x226},  {0x105800, 0x20b},
    {0x3ffa000, 0x7a4}, {0x3c29000, 0x700}, {0x38aa000, 0x670}, {0x3572000, 0x5f2},
    {0x3279000, 0x584}, {0x2fb7000, 0x524}, {0x2d26000, 0x4cc}, {0x2ac0000, 0x47e},
    {0x2881000, 0x43a}, {0x2665000, 0x3fa}, {0x2468000, 0x3c2}, {0x2287000, 0x38e},
    {0x20c1000, 0x35e}, {0x1f12000, 0x332}, {0x1d79000, 0x30a}, {0x1bf4000, 0x2e6},
};

/* Indexed by the top five fraction bits. */
static const gekko_estimate_entry gekko_fres_table[32] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340},
    {0x638800, 0x313}, {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0},
    {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
    {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1},
    {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
    {0x124400, 0x143}, {0xfbe00, 0x143},  {0xd3800, 0x12d},  {0xade00, 0x12d},
    {0x88400, 0x11a},  {0x65000, 0x11a},  {0x41c00, 0x108},  {0x20c00, 0x106},
};

/* frsqrte of a positive operand whose biased exponent is e (0 or below for a normalized
   denormal) and whose 52-bit fraction is frac: the result's biased exponent, and its
   fraction's top 26 bits (to go at bits 26-51). */
static inline int gekko_frsqrte_exponent(int e) {
    return 2046 - (e + 1025) / 2; /* 1023 - floor((e - 1021) / 2), for e down to -1025 */
}
static inline uint32_t gekko_frsqrte_fraction(int e, uint64_t frac) {
    const uint32_t idx = ((uint32_t)(e & 1) << 4) | (uint32_t)(frac >> 48);
    const uint32_t step_mul = (uint32_t)(frac >> 37) & 0x7FF;
    return gekko_frsqrte_table[idx].offset - gekko_frsqrte_table[idx].step * step_mul;
}

/* fres of an operand whose biased exponent is e and whose 52-bit fraction is frac: the
   result's biased double exponent, and its 23-bit single fraction (to go at bits 29-51). */
static inline int gekko_fres_exponent(int e) {
    return 2045 - e;
}
static inline uint32_t gekko_fres_fraction(uint64_t frac) {
    const uint32_t idx = (uint32_t)(frac >> 47);
    const uint32_t step_mul = (uint32_t)(frac >> 37) & 0x3FF;
    return gekko_fres_table[idx].offset - (gekko_fres_table[idx].step * step_mul + 1) / 2;
}
