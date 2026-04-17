#include "objects/physics.h"
#include "core/fixed_point.h"
#include <cstdlib>

namespace Physics {

void add_velocities_to_position(Object& obj) {
    obj.x.add_velocity(obj.velocity_x);
    obj.y.add_velocity(obj.velocity_y);
}

// Helper matching &3256: make positive (absolute value for signed byte)
static uint8_t make_positive(int8_t v) {
    return (v < 0) ? static_cast<uint8_t>(-v) : static_cast<uint8_t>(v);
}

// Helper matching &327f: prevent signed overflow
static int8_t prevent_overflow(int val) {
    if (val > 127) return 127;
    if (val < -128) return -128;
    return static_cast<int8_t>(val);
}

// Port of &1f01-&1f3c: apply_acceleration_to_velocities.
// Exact port preserving the quirks of the original:
//  - step 3 skip-limit: if new velocity has opposite sign to accel (i.e.
//    external momentum is still carrying the object the "wrong" way), skip
//    the cap entirely — important for explosions/wind/knockback.
//  - step 5 clamp sign: the ±0x40 cap takes the sign of the OLD velocity,
//    not the new one. For normal gameplay (|accel| << 0x40) this matches
//    sign(new), but extreme accelerations can produce surprising results.
void apply_acceleration(Object& obj, int8_t accel_x, int8_t accel_y,
                        bool every_sixteen_frames) {
    for (int axis = 0; axis < 2; axis++) {
        int8_t accel       = (axis == 0) ? accel_y : accel_x;
        int8_t& velocity   = (axis == 0) ? obj.velocity_y : obj.velocity_x;
        int8_t old_vel     = velocity;
        int    gravity_bit = (axis == 0) ? 1 : 0;

        // &1f05-&1f0a: ADC acceleration + velocity (+ gravity carry for Y)
        int sum = static_cast<int>(accel) + static_cast<int>(old_vel) + gravity_bit;
        int8_t new_vel = prevent_overflow(sum);

        // &1f0f-&1f16: skip-limit test. The original computes
        //   A = invert_if_negative(new_vel, sign_of_accel)
        //   A -= 0x40
        // and branches if A >= 0x40 unsigned. That branch is taken unless A
        // (post-invert) lies in [0x40, 0x7F] — i.e. unless new_vel lies in
        // the direction of accel with magnitude >= 0x40. Accel==0 is treated
        // as "sign = positive" (LDA 0 leaves N clear).
        bool want_limit;
        if (accel < 0) {
            want_limit = (new_vel <= -0x40);
        } else {
            want_limit = (new_vel >=  0x40);
        }

        // &1f18-&1f20: skip-limit if |old_vel| >= 0x40 (preserves velocities
        // already past the cap — wind, explosions, etc).
        if (want_limit && make_positive(old_vel) < 0x40) {
            // &1f22-&1f29: clamp to ±0x40 with sign of OLD velocity (CPY #&00
            // followed by invert_if_negative on A=0x40). old_vel==0 → positive.
            new_vel = (old_vel < 0) ? -0x40 : 0x40;
        }

        // &1f2a-&1f36: inertia decay every 16 frames, reducing |v| by 1.
        if (every_sixteen_frames) {
            if      (new_vel > 0) new_vel--;
            else if (new_vel < 0) new_vel++;
        }

        velocity = new_vel;
    }
}

} // namespace Physics
