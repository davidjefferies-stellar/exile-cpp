#include "objects/physics.h"
#include "core/fixed_point.h"
#include <cstdlib>

namespace Physics {

void add_velocities_to_position(Object& obj) {
    obj.x.add_velocity(obj.velocity_x);
    obj.y.add_velocity(obj.velocity_y);
}

// Port of &3254 make_positive (falls through into &3256 invert_if_negative):
//   &3254 CMP #&00
//   &3256 CLC
//   &3257 BPL &325d ; leave
//   &3259 EOR #&ff
//   &325b ADC #&01                    ; |A|
//   &325d RTS
static uint8_t make_positive(int8_t v) {
    return (v < 0) ? static_cast<uint8_t>(-v) : static_cast<uint8_t>(v);
}

// Port of &327f prevent_overflow:
//   &327f BVC &3285 ; leave
//   &3281 LDA #&7f
//   &3283 ADC #&00                    ; A becomes &7f or &80 (sign)
//   &3285 RTS
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

        // Port deviation (not in 6502): skip gravity when SUPPORTED and no
        // y-axis input. The 6502 always adds gravity, which produces a
        // gravity-vs-floor cycle that oscillates position by 1 push width
        // each cycle. On the BBC's 8-px-tall tile that's invisible; at our
        // 32-px tile resolution it crosses a pixel boundary. Cleared bit
        // re-enables gravity the moment SUPPORTED clears (walked off a
        // ledge, etc.) so the deviation is local to the resting case.
        bool supported = (obj.flags & ObjectFlags::SUPPORTED) != 0;
        if (axis == 0 && supported && accel == 0) {
            gravity_bit = 0;
        }

        // &1f05-&1f0a: ADC acceleration + velocity (+ gravity carry for Y)
        int sum = static_cast<int>(accel) + static_cast<int>(old_vel) + gravity_bit;
        int8_t new_vel = prevent_overflow(sum);

        // Port deviation: clamp downward vy to 0 while SUPPORTED. Our
        // 8-frac/pixel resolution magnifies the &3b3a-&3b44 floor-bias
        // into a per-frame bounce; clamp pins vy=0 on flat ground without
        // affecting jumps (vy<0) or slope descents (via vx).
        if (axis == 0 && supported && new_vel > 0) {
            new_vel = 0;
        }

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
            // followed by invert_if_negative on A=0x40). old_vel==0 -> positive.
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
