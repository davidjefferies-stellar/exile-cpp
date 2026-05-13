#pragma once
#include "objects/object.h"

// Physics engine - applies gravity, velocity, and damping to objects.
// Faithful port of the acceleration/velocity system from &1f01.
namespace Physics {

// Apply gravity and inertia decay. Called every frame; some effects only
// trigger every 16 frames (matching the original's timer system).
void apply_acceleration(Object& obj, int8_t accel_x, int8_t accel_y,
                        bool every_sixteen_frames);

// Add velocities to position. Port of &2a31 add_velocities_to_position
// (calls &2a36 twice with X=2,0; inner add_A_to_position at &2a38-&2a45):
//   &2a38 LDA &43,X ; velocity_x   ; A = velocity for this axis
//   &2a38 BPL &2a3c                ; if negative, pre-DEC the whole byte
//   &2a3a DEC &53,X ; position
//   &2a3c CLC / ADC &4f,X ; frac   ; fraction += velocity (signed)
//   &2a3f STA &4f,X
//   &2a41 BCC &2a45                ; if no carry, done
//   &2a43 INC &53,X                ; carry → bump whole byte
// Then X -= 2 to loop axes.
void add_velocities_to_position(Object& obj);

} // namespace Physics
