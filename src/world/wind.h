#pragma once
#include "objects/object.h"

// Surface wind system - port of &1c47-&1c92.
// Wind blows objects on the surface away from the center point.
namespace Wind {

// Apply wind force to an object's velocity.
// Only affects objects above y=0x4F (surface level).
void apply_surface_wind(Object& obj);

} // namespace Wind
