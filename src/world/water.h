#pragma once
#include "objects/object.h"
#include <cstdint>

// Water system - port of &2cbc (get_waterline_for_x) and water physics.
namespace Water {

// Get the waterline Y coordinate for a given X position.
// Port of &2cbc: 4 x-ranges with different water levels.
uint8_t get_waterline_y(uint8_t x);

// Check if a world position is underwater.
bool is_underwater(uint8_t x, uint8_t y);

// Apply water effects to an object below the waterline.
// Buoyancy: reduce downward velocity. Damping: reduce both velocity components.
void apply_water_effects(Object& obj, uint8_t weight);

} // namespace Water
