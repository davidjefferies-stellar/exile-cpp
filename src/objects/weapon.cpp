#include "objects/weapon.h"
#include "objects/object_tables.h"
#include "core/types.h"

namespace Weapon {

// Sine lookup for 256-step angles (quarter wave, 64 entries).
// The original uses an angle system where 0x00=right, 0x40=down, 0x80=left, 0xC0=up.
// Values represent magnitude 0-0x20 (max bullet speed).
static constexpr int8_t sine_quarter[] = {
    0x00, 0x03, 0x06, 0x09, 0x0c, 0x0f, 0x12, 0x15,
    0x18, 0x1a, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x1f, 0x1e, 0x1d, 0x1c, 0x1a,
    0x18, 0x15, 0x12, 0x0f, 0x0c, 0x09, 0x06, 0x03,
};

static int8_t sine(uint8_t angle) {
    uint8_t quadrant = angle >> 5; // 0-7 in 32-step increments
    uint8_t idx = angle & 0x1f;
    int8_t val;
    if (quadrant < 2) {
        val = sine_quarter[idx];
    } else if (quadrant < 4) {
        val = sine_quarter[31 - idx];
    } else if (quadrant < 6) {
        val = -sine_quarter[idx];
    } else {
        val = -sine_quarter[31 - idx];
    }
    return val;
}

static int8_t cosine(uint8_t angle) {
    return sine(static_cast<uint8_t>(angle + 0x40));
}

void get_firing_velocity(uint8_t aim_angle, bool facing_left,
                         int8_t& vel_x, int8_t& vel_y) {
    // Use aim angle to compute velocity components.
    // Angle 0x00 = right, 0x40 = down, 0x80 = left, 0xC0 = up.
    // If facing left, mirror the angle.
    uint8_t angle = aim_angle;
    if (facing_left && angle == 0) {
        angle = 0x80; // Default to firing left
    }

    vel_x = cosine(angle);
    vel_y = sine(angle);
}

int fire(ObjectManager& mgr, const Object& player,
         uint8_t weapon_type, uint8_t aim_angle,
         uint16_t& weapon_energy) {
    if (weapon_type > 5) return -1;

    int8_t bullet_type = weapon_bullet_type[weapon_type];
    if (bullet_type == 0) return -1;  // No bullet (jetpack, protection suit)
    if (bullet_type < 0) return -1;   // Blaster discharge mode

    uint8_t cost = weapon_energy_cost[weapon_type];
    if (weapon_energy < static_cast<uint16_t>(cost)) return -1;

    ObjectType obj_type = static_cast<ObjectType>(bullet_type);
    int slot = mgr.create_object_at(obj_type, 4, player);
    if (slot < 0) return -1;

    weapon_energy -= cost;

    Object& bullet = mgr.object(slot);
    int8_t vel_x, vel_y;
    get_firing_velocity(aim_angle, player.is_flipped_h(), vel_x, vel_y);
    bullet.velocity_x = vel_x;
    bullet.velocity_y = vel_y;

    return slot;
}

} // namespace Weapon
