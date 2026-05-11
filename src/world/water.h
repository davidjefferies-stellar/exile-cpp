#pragma once
#include "objects/object.h"
#include "world/landscape.h"
#include <cstdint>

// Water system - port of &2cbc (get_waterline_for_x) and water physics.
namespace Water {

// Get the waterline Y coordinate for a given X position.
// Port of &2cbc: 4 x-ranges with different water levels.
uint8_t get_waterline_y(uint8_t x);

// &2ef7-&2f53 this_object_in_water (&1f). Below waterline OR tile is
// WATER — the tile check catches upper-world ponds that sit above the
// global waterline (without it flasks don't fill).
bool is_underwater(const Landscape& landscape, uint8_t x, uint8_t y);

// &2f41-&2f8a water effects. Four-iteration apply_buoyancy_loop at &2f57
// (DEC count depends on weight + depth), then 7/8 velocity damping every
// four frames (&2f85 → &3222). `every_four_frames` is the &2f85 BIT &c5.
void apply_water_effects(const Landscape& landscape, Object& obj,
                         uint8_t weight, bool every_four_frames);

} // namespace Water
