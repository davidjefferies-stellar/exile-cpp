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

    // &2a36-&2a47 add signed velocity. Pessimistic pre-decrement for
    // negative vel, then unsigned add with carry to whole.
    void add_velocity(int8_t vel) {
        uint8_t uvel = static_cast<uint8_t>(vel);
        if (vel < 0) whole--;
        uint16_t sum = static_cast<uint16_t>(fraction) + uvel;
        fraction = static_cast<uint8_t>(sum);
        if (sum > 0xFF) whole++;
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
