#include "objects/weapon.h"
#include "objects/object_tables.h"
#include "behaviours/npc_helpers.h"
#include "audio/audio.h"
#include "core/types.h"
#include "rendering/sprite_atlas.h"
#include <algorithm>

namespace Weapon {

// Port of &2357 calculate_vector_from_magnitude_and_angle — diamond trig
// satisfying |vx|+|vy| = magnitude (rotated-square approximation; no
// trig tables). magnitude <= 0x7f. Bullet fire 0x40+rng3, AIM particles
// 0x20.
static void diamond_vector(uint8_t angle, int magnitude,
                           int8_t& vx, int8_t& vy) {
    uint8_t quad = angle >> 6;        // 0..3
    uint8_t rel  = angle & 0x3f;      // 0..0x3f within the quadrant
    int a = std::min<int>(magnitude, rel);
    int b = std::min<int>(magnitude, 0x40 - rel);
    switch (quad) {
        case 0: vx =  static_cast<int8_t>(b); vy =  static_cast<int8_t>(a); break; // right → down
        case 1: vx = -static_cast<int8_t>(a); vy =  static_cast<int8_t>(b); break; // down  → left
        case 2: vx = -static_cast<int8_t>(b); vy = -static_cast<int8_t>(a); break; // left  → up
        case 3: vx =  static_cast<int8_t>(a); vy = -static_cast<int8_t>(b); break; // up    → right
    }
}

void get_firing_velocity(uint8_t aim_angle, bool facing_left,
                         int8_t& vel_x, int8_t& vel_y, int magnitude) {
    // aim_angle is a signed -0x3f..+0x3f offset from the facing direction
    // (negative = up, positive = down). Fold facing into the 8-bit angle
    // used by diamond_vector: 0x00 = right, 0x80 = left. For left-facing we
    // mirror across the vertical axis so "aim up" still maps into the upper
    // hemisphere.
    int8_t s = static_cast<int8_t>(aim_angle);
    uint8_t angle = facing_left
        ? static_cast<uint8_t>(0x80 - s)
        : static_cast<uint8_t>(s);
    diamond_vector(angle, magnitude, vel_x, vel_y);
}

// Saturating int8_t add — port of &327f prevent_overflow which clamps a
// signed-overflowing ADC result to ±0x7f / 0x80.
static int8_t sat_add_i8(int a, int b) {
    int s = a + b;
    if (s >  127) s =  127;
    if (s < -128) s = -128;
    return static_cast<int8_t>(s);
}

int fire(ObjectManager& mgr, const Object& player,
         uint8_t weapon_type, uint8_t aim_angle,
         uint16_t& weapon_energy, int8_t& blaster_timer,
         Random& rng) {
    if (weapon_type > 5) return -1;

    int8_t bullet_type = weapon_bullet_type[weapon_type];
    if (bullet_type == 0) return -1;  // No bullet (jetpack, protection suit)

    uint8_t cost = weapon_energy_cost[weapon_type];
    if (weapon_energy < static_cast<uint16_t>(cost)) return -1;

    // &2d4a-&2d51 blaster path. weapons_bullet_type_table[3] is &fb (-5);
    // STA player_blaster_timer arms the per-frame discharge in tick_blaster
    // for 5 frames, then BMI skips the create-bullet path. The energy cost
    // (255) is paid once here; the discharge sound is retriggered per
    // tick by tick_blaster, matching &4a7d play_sound_on_channel_zero.
    if (bullet_type < 0) {
        weapon_energy -= cost;
        blaster_timer = bullet_type;          // -5
        return -1;
    }

    ObjectType obj_type = static_cast<ObjectType>(bullet_type);
    int slot = mgr.create_object_at(obj_type, 4, player);
    if (slot < 0) return -1;

    weapon_energy -= cost;

    Object& bullet = mgr.object(slot);
    // &3313-&3318: magnitude = 0x40 + (rng & 3). The +0..3 jitter keeps
    // simultaneous shots from stacking pixel-perfectly.
    int magnitude = 0x40 + static_cast<int>(rng.next() & 3);
    int8_t vel_x, vel_y;
    get_firing_velocity(aim_angle, player.is_flipped_h(),
                        vel_x, vel_y, magnitude);

    // &331d-&3340: fold the player's vx into the bullet's vx so a bullet
    // fired while running inherits forward momentum; then cap |vx| at
    // max(|player_vx| + 0x20, 0x50) when the result would otherwise be
    // ≥ 0x50. vy has no equivalent fold-in in the 6502.
    int8_t vx = sat_add_i8(vel_x, player.velocity_x);
    int abs_vx = vx < 0 ? -vx : vx;
    if (abs_vx >= 0x50) {
        int abs_pvx = player.velocity_x < 0
                        ? -static_cast<int>(player.velocity_x)
                        :  static_cast<int>(player.velocity_x);
        int cap = abs_pvx + 0x20;
        if (cap < 0x50) cap = 0x50;
        if (cap > 0x7f) cap = 0x7f;
        abs_vx = cap;
        vx = (vx < 0) ? static_cast<int8_t>(-abs_vx)
                      : static_cast<int8_t>( abs_vx);
    }
    bullet.velocity_x = vx;
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
