#include "objects/weapon.h"
#include "objects/object_tables.h"
#include "behaviours/npc_helpers.h"
#include "audio/audio.h"
#include "core/types.h"
#include "rendering/sprite_atlas.h"
#include <algorithm>

namespace Weapon {

// Port of &2357 calculate_vector_from_magnitude_and_angle — the BBC's very
// approximate "diamond" trig. Each quadrant is a piecewise-linear clamp to
// magnitude 0x20, giving these anchor points (see the table in the
// disassembly comment at ~&2172):
//
//   angle 0x00 (right) → ( 0x20,  0x00)
//   angle 0x40 (down)  → ( 0x00,  0x20)
//   angle 0x80 (left)  → (-0x20,  0x00)
//   angle 0xc0 (up)    → ( 0x00, -0x20)
//
// The "diamond" name comes from the fact that |vx| + |vy| ≤ 0x40 — points
// trace a rotated square rather than a circle. Good enough for pixel-space
// gameplay and avoids trig tables entirely.
static void diamond_vector(uint8_t angle, int8_t& vx, int8_t& vy) {
    uint8_t quad = angle >> 6;       // 0..3
    uint8_t rel  = angle & 0x3f;     // 0..0x3f within the quadrant
    int a = std::min<int>(0x20, rel);
    int b = std::min<int>(0x20, 0x40 - rel);
    switch (quad) {
        case 0: vx =  static_cast<int8_t>(b); vy =  static_cast<int8_t>(a); break; // right → down
        case 1: vx = -static_cast<int8_t>(a); vy =  static_cast<int8_t>(b); break; // down  → left
        case 2: vx = -static_cast<int8_t>(b); vy = -static_cast<int8_t>(a); break; // left  → up
        case 3: vx =  static_cast<int8_t>(a); vy = -static_cast<int8_t>(b); break; // up    → right
    }
}

void get_firing_velocity(uint8_t aim_angle, bool facing_left,
                         int8_t& vel_x, int8_t& vel_y) {
    // aim_angle is a signed -0x3f..+0x3f offset from the facing direction
    // (negative = up, positive = down). Fold facing into the 8-bit angle
    // used by diamond_vector: 0x00 = right, 0x80 = left. For left-facing we
    // mirror across the vertical axis so "aim up" still maps into the upper
    // hemisphere.
    int8_t s = static_cast<int8_t>(aim_angle);
    uint8_t angle = facing_left
        ? static_cast<uint8_t>(0x80 - s)
        : static_cast<uint8_t>(s);
    diamond_vector(angle, vel_x, vel_y);
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

    // Port of create_child_object (&33b8-&342f). Shared implementation
    // lives in NPC::offset_child_from_parent so NPC firing (turrets,
    // robots, Triax, cannons) and player firing produce identical spawn
    // geometry.
    NPC::offset_child_from_parent(bullet, player);
    // Initial lifespan — common_bullet_update explodes the bullet the instant
    // its timer hits zero, and the icer/pistol updaters re-arm the timer
    // while the bullet is still moving. Starting at 0 (as init_object_from_type
    // leaves it) would blow the bullet up on its very first frame.
    bullet.timer = 0x30;

    // &2d58-&2d72: per-weapon firing sound. The 6502 dispatch is
    //   weapon_type-1 == 0 → pistol, == 1 → icer, else plasma.
    // Our weapon enum splits plasma and blaster, both of which use the
    // 6502's plasma path (play_low_beep at &14ad).
    static constexpr uint8_t kSoundPistol[4]  = { 0x3d, 0x04, 0x3d, 0x04 };  // &2d72
    static constexpr uint8_t kSoundIcer[4]    = { 0x3d, 0x04, 0x3d, 0xd3 };  // &2d69
    static constexpr uint8_t kSoundLowBeep[4] = { 0x5d, 0x04, 0xff, 0x05 };  // &14b0
    switch (weapon_type) {
        case 1: Audio::play(Audio::CH_ANY, kSoundPistol);  break;
        case 2: Audio::play(Audio::CH_ANY, kSoundIcer);    break;
        case 3:                                                                 // blaster
        case 4: Audio::play(Audio::CH_ANY, kSoundLowBeep); break;               // plasma
        default: break;
    }

    return slot;
}

} // namespace Weapon
