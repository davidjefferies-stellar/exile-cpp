#pragma once
#include "objects/object.h"
#include "objects/object_manager.h"
#include "core/random.h"
#include <cstdint>

// Weapon firing system - port of &2d33
namespace Weapon {

// Fire the player's current weapon. Returns bullet slot or -1.
// weapon_type: 0=jetpack, 1=pistol, 2=icer, 3=blaster, 4=plasma, 5=suit.
// Blaster discharges via blaster_timer (&2d4f STA -5 → ticks to 0,
// radius-10 explosion at player) rather than spawning a primary.
int fire(ObjectManager& mgr, const Object& player,
         uint8_t weapon_type, uint8_t aim_angle,
         uint16_t& weapon_energy, int8_t& blaster_timer,
         Random& rng);

// Direction vector from aim angle. Magnitude defaults to 0x20 (the
// AIM-particle speed); pass a larger magnitude (e.g. 0x40) to match the
// 6502's actual bullet firing velocity at &3318.
void get_firing_velocity(uint8_t aim_angle, bool facing_left,
                         int8_t& vel_x, int8_t& vel_y,
                         int magnitude = 0x20);

} // namespace Weapon
