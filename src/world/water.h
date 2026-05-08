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

// Apply water effects to an object in water. Faithful port of the
// &2f41-&2f8a chain: buoyancy via the four-iteration apply_buoyancy_loop
// at &2f57 (DEC velocity_y count depends on weight + how deeply
// submerged — lighter / shorter / more-submerged objects rise faster),
// then 7/8 velocity damping every four frames (&2f85 → &3222
// dampen_this_object_velocities).
//
// `every_four_frames` is the &2f85 BIT &c5 gate from the per-tick timer
// flags; pass `true` when frame_counter matches the every-four-frames
// boundary, `false` otherwise.
void apply_water_effects(const Landscape& landscape, Object& obj,
                         uint8_t weight, bool every_four_frames);

} // namespace Water
