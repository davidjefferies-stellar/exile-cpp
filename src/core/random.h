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

    // Carry out of the last ADC inside next(). Mirrors the 6502 caller
    // doing AND #imm (which preserves C) and feeding C into a later ADC.
    uint8_t last_carry() const { return carry_; }

    // Get current state for save/load
    uint8_t state(int i) const { return state_[i]; }

    // Peek a rnd_state byte without advancing the LFSR. Mirrors the 6502's
    // bare LDA/BIT &d9/&da/&db/&dc reads which thread random bits into a
    // computation without burning a fresh rng byte. idx 0..3 = &d9..&dc.
    uint8_t peek(int idx) const { return state_[idx]; }

private:
    uint8_t state_[4]; // &d9, &da, &db, &dc
    uint8_t carry_ = 0;
};
