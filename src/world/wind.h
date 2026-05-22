#pragma once
#include "objects/object.h"
#include "core/random.h"

class Landscape;
class ObjectManager;
class ParticleSystem;

// Surface wind system - port of &1c47-&1c92.
// Wind blows objects on the surface away from the center point.
namespace Wind {

// Apply wind force to an object's velocity.
// Only affects objects above y=0x4F (surface level).
void apply_surface_wind(Object& obj);

// Effective wind magnitude (0..0x7f). Drives &3f73-&3f7b particle gate:
// WIND particle emitted only when (rnd & 0x7f) < magnitude.
uint8_t surface_wind_magnitude(const Object& obj);

// Desired surface-wind vector at obj position. Signed, points TOWARD
// wind centre (push direction). Mirrors &b4/&b6 across &1c4d-&1c8d.
void surface_wind_vector(const Object& obj, int8_t& vx, int8_t& vy);

// Per-tile wind/current dispatch: &3f18 TILE_VARIABLE_WIND (caverns +
// downdraft), &3f41 TILE_CONSTANT_WIND (nibbles = vy/vx in tert data),
// &3fa3 TILE_WATER (water_velocities_table &1e44 by flip bits). Wind
// gated bit 4 of frame_counter for airborne; water acts every frame.
// `rng` drives the 6502-equivalent rolls (wind magnitude + particle-gate
// at &3f73); `cosmetic_rng` drives particle internals only.
void apply_tile_environment(Object& obj,
                            const Landscape& landscape,
                            const ObjectManager& mgr,
                            uint8_t frame_counter,
                            Random& rng,
                            Random& cosmetic_rng,
                            ParticleSystem& particles);

} // namespace Wind
