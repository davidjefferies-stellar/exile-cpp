#pragma once
#include "objects/object.h"
#include "world/landscape.h"
#include <cstdint>

// Water system — port of &2cbc (get_waterline_for_x) and water physics.
namespace Water {

// Per-range waterline state — live mutable mirror of the 6502's
// waterline_x_ranges_y[4] (&0832), _y_fraction[4] (&082e), _desired_y[4]
// (&0836). update_waterlines steps the current values toward desired_y
// each frame; Triax-lab logic rewrites desired_y[1] to control flooding.
void    reset();
uint8_t get_y(int range);
uint8_t get_y_fraction(int range);
uint8_t get_desired_y(int range);
void    set_y(int range, uint8_t y, uint8_t fraction);
void    set_desired_y(int range, uint8_t y);

// Port of &2626-&265b update_waterlines_loop. Run once per game frame.
// Steps each range's (y, y_fraction) toward desired_y by ±2 fraction
// units, with an additional cyclic ±2 ripple driven by bit 5 of the
// frame counter (raise 32 of 64 frames, lower the other 32).
void update_waterlines(uint8_t frame_counter);

// Port of &2cbc get_waterline_for_x. Reads the live per-range y; the
// Triax-lab range (1) acts as a ceiling on the others so the lab can
// drain everything above it.
uint8_t get_waterline_y(uint8_t x);

// Sub-tile fraction (0..255) paired with get_waterline_y for the same x.
// Lets the renderer animate the waterline at sub-tile resolution instead
// of snapping at whole-tile boundaries during fill/drain.
uint8_t get_waterline_y_fraction(uint8_t x);

// &2ef7-&2f53 this_object_in_water (&1f). Below waterline OR tile is
// WATER — the tile check catches upper-world ponds that sit above the
// global waterline (without it flasks don't fill).
bool is_underwater(const Landscape& landscape, uint8_t x, uint8_t y);

// &2f41-&2f8a water effects. Four-iteration apply_buoyancy_loop at &2f57
// (DEC count depends on weight + depth), then 7/8 velocity damping every
// four frames (&2f85 -> &3222). `every_four_frames` is the &2f85 BIT &c5.
void apply_water_effects(const Landscape& landscape, Object& obj,
                         uint8_t weight, bool every_four_frames);

} // namespace Water
