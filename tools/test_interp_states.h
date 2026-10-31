// The CPU states tools/test_interp.cpp runs each case from. Every field of a state comes from a
// generator seeded by the word, its address and the state's number, so a failing run repeats by
// itself.
#pragma once
#include "recomp.h"

struct Rng {
    uint64_t s;
    uint64_t next() {  // splitmix64
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t u32() { return (uint32_t)(next() >> 32); }
    uint32_t below(uint32_t n) { return (uint32_t)(((next() >> 32) * n) >> 32); }
};

// The generator for the word insn at pc in state number state.
Rng rng_for(uint32_t insn, uint32_t pc, int state);

// A register's value, an edge or a small number often enough to matter.
uint32_t value(Rng& r);

// A state with every field picked by r, the FP ones from FP edges often enough to matter.
void random_cpu(CPU* c, Rng& r);

// How many fixed states a step runs from as well, the integer ones first and then the FP ones.
extern const int kFixedAll;

// Fixed state k, from 0 to kFixedAll - 1, given to insn's registers over a random state.
void fix_state(CPU* c, uint32_t insn, int k);
