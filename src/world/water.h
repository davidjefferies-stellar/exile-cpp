#pragma once
#include "objects/object.h"
#include "world/landscape.h"
#include <cstdint>

// Water system - port of &2cbc (get_waterline_for_x) and water physics.
namespace Water {

// Get the waterline Y coordinate for a given X position.
// Port of &2cbc: 4 x-ranges with different water levels.
uint8_t get_waterline_y(uint8_t x);

// Check if a world position is in water. Port of the 6502's
// this_object_in_water (&1f) logic at &2ef7-&2f53: true if the point is
// below the global waterline OR the tile at (x, y) is TileType::WATER.
// The tile check catches upper-world ponds that sit above the global
// waterline — without it, flasks don't fill and objects don't float in
// surface-water pockets.
bool is_underwater(const Landscape& landscape, uint8_t x, uint8_t y);

// Apply water effects to an object in water. Buoyancy reduces downward
// velocity, damping reduces both velocity components. Uses the same
// dual waterline/tile check as is_underwater above.
void apply_water_effects(const Landscape& landscape, Object& obj, uint8_t weight);

} // namespace Water
