#pragma once
#include "objects/object.h"
#include "objects/object_manager.h"
#include <cstdint>

// Weapon firing system - port of &2d33
namespace Weapon {

// Fire the player's current weapon. Creates a bullet object if applicable.
// Returns the slot of the created bullet, or -1 if firing failed.
// weapon_type: 0=jetpack, 1=pistol, 2=icer, 3=blaster, 4=plasma, 5=suit
int fire(ObjectManager& mgr, const Object& player,
         uint8_t weapon_type, uint8_t aim_angle,
         uint16_t& weapon_energy);

// Get velocity components from aim angle using sine/cosine lookup
void get_firing_velocity(uint8_t aim_angle, bool facing_left,
                         int8_t& vel_x, int8_t& vel_y);

} // namespace Weapon
