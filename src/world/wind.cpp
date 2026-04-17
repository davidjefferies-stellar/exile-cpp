#include "world/wind.h"
#include "core/types.h"
#include "objects/object_data.h"
#include <cstdlib>

namespace Wind {

// Port of &1c47-&1c92: apply_surface_wind.
// Wind is centred at (x=0x9B, y=0x4E) and blows outward.
// Strength increases with distance, doubles at 0x32 and again at 0x3C.
// Heavier objects are affected more (matching original: weight+1 shifts).
void apply_surface_wind(Object& obj) {
    // Only above surface
    if (obj.y.whole >= 0x4f) return;

    // Get object weight
    uint8_t type_idx = static_cast<uint8_t>(obj.type);
    uint8_t weight;
    if (type_idx < static_cast<uint8_t>(ObjectType::COUNT)) {
        weight = object_types_flags[type_idx] & ObjectTypeFlags::WEIGHT_MASK;
    } else {
        weight = 3;
    }
    if (weight >= 7) return; // Static objects unaffected

    // Process Y component (distance from y=0x4E), then X component (from x=0x9B)
    // The original loops X=2 (y), then X=0 (x), applying the same formula
    struct { uint8_t center; int8_t* velocity; } axes[2] = {
        {0x4e, &obj.velocity_y},
        {GameConstants::WIND_CENTER_X, &obj.velocity_x},
    };

    for (int i = 0; i < 2; i++) {
        int8_t dist_signed = static_cast<int8_t>(
            (i == 0) ? (obj.y.whole - axes[i].center)
                     : (obj.x.whole - axes[i].center));
        bool negative = dist_signed < 0;
        uint8_t dist = negative ? static_cast<uint8_t>(-dist_signed)
                                : static_cast<uint8_t>(dist_signed);

        // No wind if distance < 0x1E from center
        if (dist < 0x1e) continue;

        // Calculate wind strength
        int shift_count = weight + 1; // Heavier = more shifts right = less effect

        // Double at 0x32, double again at 0x3C
        if (dist >= 0x32) shift_count--;
        if (dist >= 0x3c) shift_count--;

        // Strength = (distance - 0x08) * 2, capped at 0x7F
        int strength = (static_cast<int>(dist) - 0x08) * 2;
        if (strength > 0x7f) strength = 0x7f;
        if (strength < 0) strength = 0;

        // Shift right by weight to reduce effect on heavier objects
        // Clamp shift count
        if (shift_count < 0) shift_count = 0;
        strength >>= shift_count;

        // Apply wind: push away from center
        int8_t wind_vel = static_cast<int8_t>(negative ? -strength : strength);

        // Add to velocity (clamped)
        int new_vel = static_cast<int>(*axes[i].velocity) + wind_vel;
        if (new_vel > 127) new_vel = 127;
        if (new_vel < -128) new_vel = -128;
        *axes[i].velocity = static_cast<int8_t>(new_vel);
    }
}

} // namespace Wind
