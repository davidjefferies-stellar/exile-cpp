#pragma once
#include "objects/object.h"

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

} // namespace Wind
