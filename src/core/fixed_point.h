#pragma once
#include <cstdint>

// 8.8 fixed-point position matching the BBC Micro's representation.
// whole = tile coordinate (0-255, wraps), fraction = sub-tile (0-255).
// In the original, 0x20 fraction = 1 pixel, 0x10 fraction = 1/2 pixel.
struct Fixed8_8 {
    uint8_t fraction = 0;
    uint8_t whole = 0;

    Fixed8_8() = default;
    constexpr Fixed8_8(uint8_t w, uint8_t f) : fraction(f), whole(w) {}

    // Add signed velocity to position. Exact port of &2a36-&2a47.
    //
    // 6502 algorithm:
    //   LDA velocity           ; signed byte
    //   BPL skip_underflow
    //   DEC whole              ; pre-decrement for negative velocity
    //   skip_underflow:
    //   CLC
    //   ADC fraction           ; add unsigned velocity byte to fraction
    //   STA fraction
    //   BCC skip_overflow
    //   INC whole              ; carry from fraction addition
    //   skip_overflow:
    void add_velocity(int8_t vel) {
        uint8_t uvel = static_cast<uint8_t>(vel);

        // Step 1: If velocity is negative, pre-decrement whole (pessimistic borrow)
        if (vel < 0) {
            whole--;
        }

        // Step 2: CLC; ADC fraction (add unsigned velocity to fraction)
        uint16_t sum = static_cast<uint16_t>(fraction) + uvel;
        fraction = static_cast<uint8_t>(sum);

        // Step 3: If carry (fraction overflowed), INC whole
        if (sum > 0xFF) {
            whole++;
        }
    }
};

// Velocity is signed 8-bit, clamped to +/- 0x40 by the engine.
static constexpr int8_t VELOCITY_MAX = 0x40;
static constexpr int8_t VELOCITY_MIN = -0x40;

inline int8_t clamp_velocity(int vel) {
    if (vel > VELOCITY_MAX) return VELOCITY_MAX;
    if (vel < VELOCITY_MIN) return VELOCITY_MIN;
    return static_cast<int8_t>(vel);
}
