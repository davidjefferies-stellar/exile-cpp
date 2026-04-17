#pragma once
#include <cstdint>

// Deterministic PRNG matching the original at &2587.
// Uses 4 bytes of state (rnd_state at &d9-&dc).
// The 6502 routine chains ADC operations through the carry flag.
class Random {
public:
    Random();
    void seed(uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3);

    // Generate next random byte. Port of &2587.
    // In the original, carry state from the caller leaks into the first ADC.
    // We maintain internal carry across calls, which produces a valid PRNG
    // sequence but not necessarily identical to any specific call site.
    uint8_t next();

    // Convenience: random value AND mask
    uint8_t next_masked(uint8_t mask) { return next() & mask; }

    // Get current state for save/load
    uint8_t state(int i) const { return state_[i]; }

private:
    uint8_t state_[4]; // &d9, &da, &db, &dc
    uint8_t carry_ = 0;
};
