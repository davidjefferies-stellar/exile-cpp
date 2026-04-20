#include "world/wind.h"
#include "core/types.h"
#include "objects/object_data.h"
#include <cstdlib>

namespace Wind {

// Port of &1c47-&1c92: apply_surface_wind.
//
// Wind is centred at (x=0x9B, y=0x4E) and blows INWARD. The original
// routine computes a desired velocity from distance, then at &1c84
// hands off to `add_weighted_vector_component_to_this_object_velocity`
// (&3f94) which:
//   delta = desired - current_velocity
//   delta >>= Y                                   (weight factor)
//   |delta| clamped to max_acceleration (0x0c)    (&3213)
//   current_velocity += delta                     (&31fc)
//
// The previous port was missing this "accelerate toward desired"
// step — it added `desired >> Y` to velocity every frame, so wind
// never saturated and felt much stronger than the 6502. It also had
// the base shift off by one and didn't implement the overflow-ceiling
// case at dist >= 0x48 (&1c72 BPL/DEY DEY).
void apply_surface_wind(Object& obj) {
    // Only above surface (y < 0x4f, &1c49 CMP #&4f / BCS skip)
    if (obj.y.whole >= 0x4f) return;

    uint8_t type_idx = static_cast<uint8_t>(obj.type);
    uint8_t weight = (type_idx < static_cast<uint8_t>(ObjectType::COUNT))
        ? (object_types_flags[type_idx] & ObjectTypeFlags::WEIGHT_MASK)
        : 3;
    if (weight >= 7) return;

    struct { uint8_t center; int8_t* velocity; uint8_t pos; } axes[2] = {
        {0x4e, &obj.velocity_y, obj.y.whole},
        {GameConstants::WIND_CENTER_X, &obj.velocity_x, obj.x.whole},
    };

    for (int i = 0; i < 2; i++) {
        int16_t dist_signed = static_cast<int16_t>(axes[i].pos) -
                              static_cast<int16_t>(axes[i].center);
        bool negative = dist_signed < 0;
        uint8_t dist = negative ? static_cast<uint8_t>(-dist_signed)
                                : static_cast<uint8_t>(dist_signed);

        // &1c61: no wind if |dist| < 0x1e
        if (dist < 0x1e) continue;

        // Weight factor:
        //   base = weight + 2  (LDY weight / INY at &1c5b then INY at &1c78)
        //   -1 at dist >= 0x32 (&1c69 DEY)
        //   -1 at dist >= 0x3c (&1c6e DEY)
        //   -2 at dist >= 0x48 (&1c74 DEY DEY — strength ceiling hit)
        int shift_count = weight + 2;
        if (dist >= 0x32) shift_count--;
        if (dist >= 0x3c) shift_count--;

        // Strength = 2 * (dist - 8), clamped to 0x7f (&1c6f-&1c76)
        int strength = 2 * (static_cast<int>(dist) - 0x08);
        if (strength > 0x7f) {
            strength = 0x7f;
            shift_count -= 2;
        }
        if (shift_count < 0) shift_count = 0; // &1c79-&1c7b: floor Y at 0

        // Sign: wind pushes TOWARD centre, so desired velocity is
        // opposite sign of (pos - centre). Port of &1c5c ROR + &1c7f
        // invert_if_negative.
        int desired = negative ? strength : -strength;

        // &3f94 → &31f6: delta = desired - current; |delta| >>= Y;
        // clamp to ±max_acceleration (0x0c); current += delta.
        int current = *axes[i].velocity;
        int delta = desired - current;
        bool delta_neg = delta < 0;
        int mag = delta_neg ? -delta : delta;
        mag >>= shift_count;
        if (mag > 0x0c) mag = 0x0c; // maximum_acceleration at &9c
        int accel = delta_neg ? -mag : mag;
        int new_vel = current + accel;
        if (new_vel > 127) new_vel = 127;
        if (new_vel < -128) new_vel = -128;
        *axes[i].velocity = static_cast<int8_t>(new_vel);
    }
}

} // namespace Wind
