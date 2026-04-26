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

// Returns the effective wind magnitude (0..0x7f) an object currently
// feels. 0 = no wind (outside the zone or too weak). Used by the
// particle system: the 6502's add_wind_particle_using_velocities at
// &3f73-&3f7b only emits one WIND particle when `rnd & 0x7f` is below
// this value — so stronger wind → visibly more drift particles.
uint8_t surface_wind_magnitude(const Object& obj);

// Returns the desired surface-wind vector at this object's position,
// matching the (vector_x, vector_y) the 6502 builds in &b4/&b6 across
// the apply_surface_wind_loop at &1c4d-&1c8d before calling
// add_wind_particle_using_velocities. The vector is signed and points
// toward the wind centre, i.e. the direction wind is pushing the
// object. Both components are zero outside the wind zone.
void surface_wind_vector(const Object& obj, int8_t& vx, int8_t& vy);

// Port of the &3f18 / &3f41 / &3fa3 tile update routines: each tile
// type may apply a per-tile wind or water-current force to an object
// touching the tile. Dispatches on the resolved tile type at the
// object's position:
//
//   TILE_VARIABLE_WIND (&0e &3f18): time-varying angle/magnitude in
//       the windy caverns; constant downdraft of &70 in the two square
//       caverns south of the west stone door (h-flipped variant).
//   TILE_CONSTANT_WIND (&0b &3f41): wind direction packed into the
//       tile's tertiary data byte (top nibble = vy, bottom nibble = vx).
//   TILE_WATER         (&0d &3fa3): water current looked up from the
//       tile's flip bits via water_velocities_table at &1e44 — flowing
//       river / waterfall in Triax's lab. Still water (flip=0) is a
//       no-op here.
//
// Velocity is applied via the same weight-scaled "accelerate toward
// desired" math as &31f6, with extra weight when fully submerged. Wind
// effects are gated to 16/32 frames for airborne objects (frame_counter
// bit 4); objects in water feel the current every frame. Emits a
// PARTICLE_WIND aligned with the wind vector when magnitude is high
// enough, mirroring add_wind_particle_using_velocities at &3f73.
void apply_tile_environment(Object& obj,
                            const Landscape& landscape,
                            const ObjectManager& mgr,
                            uint8_t frame_counter,
                            Random& rng,
                            ParticleSystem& particles);

} // namespace Wind
