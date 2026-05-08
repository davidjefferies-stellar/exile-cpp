#pragma once
#include <cstdint>

// Per-frame debug record of a single damage application. Pushed by every
// damage-dealing routine (bullets, explosion radius, lightning, red drop,
// coronium radiation, …) when the "Damage" overlay checkbox is on, and
// drawn by render.cpp as a floating number + (for area effects) a radius
// circle around the source. Cleared at the start of every frame.
struct DamageVisual {
    uint8_t src_x = 0;        // damage source whole-tile X
    uint8_t src_y = 0;        // damage source whole-tile Y
    uint8_t src_x_frac = 0;
    uint8_t src_y_frac = 0;
    uint8_t tgt_x = 0;        // victim whole-tile X (== src_x for self-damage)
    uint8_t tgt_y = 0;        // victim whole-tile Y
    uint8_t tgt_x_frac = 0;
    uint8_t tgt_y_frac = 0;
    // Target slot index (0..15), or -1 if the event isn't tied to a
    // live primary (e.g. radius-only marker). render.cpp refreshes
    // tgt_x/y each frame from the slot's current position so damage
    // numbers track moving NPCs instead of being stranded at the
    // hit-frame coordinates.
    int8_t tgt_slot = -1;
    uint16_t amount = 0;      // damage dealt (clamped at target's energy)
    uint8_t radius_tiles = 0; // 0 = point damage; > 0 = draw an outline at src
    uint8_t ttl = 30;         // frames remaining before the renderer drops it
};

// Generic floating-text notification. Pushed on rare game events
// (e.g. an imp absorbing food) so the player gets transient visual
// feedback without needing any debug-overlay checkbox active.
// Tracks the source object's position so the text moves with it
// while the TTL counts down.
struct FloatingLabel {
    uint8_t world_x = 0;
    uint8_t world_y = 0;
    uint8_t x_frac  = 0;
    uint8_t y_frac  = 0;
    char    text[8] = {0};
    uint32_t rgb    = 0xffee33;
    uint8_t ttl     = 60;     // ~1.2s at 50fps
};
