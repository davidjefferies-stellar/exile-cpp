#pragma once
#include "objects/object.h"

// Physics engine - applies gravity, velocity, and damping to objects.
// Faithful port of the acceleration/velocity system from &1f01.
namespace Physics {

// Apply gravity and inertia decay. Called every frame; some effects only
// trigger every 16 frames (matching the original's timer system).
void apply_acceleration(Object& obj, int8_t accel_x, int8_t accel_y,
                        bool every_sixteen_frames);

// Add velocities to position. Port of &2a31.
void add_velocities_to_position(Object& obj);

} // namespace Physics
