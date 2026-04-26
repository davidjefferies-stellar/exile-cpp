#include "game/game.h"

namespace {

// Port of &22d4 calculate_angle_from_vector. Returns the 8-bit angle of a
// 2D vector, where 0xc0 = "head up" (negative y), 0x40 = "head down",
// 0x00 = pointing right, 0x80 = pointing left. The algorithm divides the
// smaller absolute component by the larger to get a five-bit slope, then
// XORs an octant offset from the sign/magnitude half-quadrant table at &14bf.
uint8_t angle_from_vector(int8_t vx, int8_t vy) {
    bool y_pos = vy >= 0;
    bool x_pos = vx >= 0;
    uint8_t ay = static_cast<uint8_t>(y_pos ?  int(vy) : -int(vy));
    uint8_t ax = static_cast<uint8_t>(x_pos ?  int(vx) : -int(vx));

    bool x_ge_y = ax >= ay;
    uint8_t magnitude = x_ge_y ? ax : ay;
    uint8_t small     = x_ge_y ? ay : ax;

    // 8-bit angle starts as 0x08 and is rotated left with division bits
    // until the sentinel overflows (after 5 meaningful iterations).
    uint8_t angle = 0x08;
    if (magnitude != 0) {
        while (true) {
            bool carry_in = (small & 0x80) != 0;
            small = static_cast<uint8_t>(small << 1);
            bool ge = carry_in || small >= magnitude;
            if (ge) small = static_cast<uint8_t>(small - magnitude);
            bool sentinel_out = (angle & 0x80) != 0;
            angle = static_cast<uint8_t>((angle << 1) | (ge ? 1 : 0));
            if (sentinel_out) break;
        }
    } else {
        angle = 0;
    }

    static constexpr uint8_t HALF_QUADRANT[8] = {
        0xbf, 0x80, 0xc0, 0xff, 0x40, 0x7f, 0x3f, 0x00,
    };
    int idx = (x_ge_y ? 1 : 0) | (x_pos ? 2 : 0) | (y_pos ? 4 : 0);
    return static_cast<uint8_t>(angle ^ HALF_QUADRANT[idx]);
}

// Port of &3906 set_spacesuit_sprite_from_angle. Quantises the current
// player angle into 8 half-quadrants, derives x/y flip bits from the
// half-quadrant, then picks one of four angled sprites (HORIZONTAL,
// FORTY_FIVE_HEAD_UP, JUMPING, FORTY_FIVE_HEAD_DOWN). For the vertical
// quadrant the sprite is chosen by walking/standing state in the usual
// way.
void set_spacesuit_sprite_from_angle(Object& player,
                                     uint8_t angle,
                                     uint8_t x_flip_in) {
    // Five LSRs + ADC #&00 — divide by 32 with carry rounding up.
    uint8_t a = angle;
    bool carry = false;
    for (int i = 0; i < 5; ++i) {
        carry = (a & 1) != 0;
        a = static_cast<uint8_t>(a >> 1);
    }
    uint8_t hq = static_cast<uint8_t>(a + (carry ? 1 : 0));
    bool adc00_carry = (a == 0xff && carry); // effectively never; carry stays 0
    (void)adc00_carry;

    // If facing right (x_flip_in bit 7 clear), reverse the sequence and
    // add 1 (the original ADC #&01 carries the previous ADC's carry,
    // which is 0 here since hq ≤ 0x07).
    if (!(x_flip_in & 0x80)) {
        hq = static_cast<uint8_t>((hq ^ 0x07) + 1);
    }
    // hq now in 0..8; only low 3 bits matter for the quadrant lookup.

    // Derive x_flip = 0x80 when bit 2 of hq is set, y_flip = x_flip XOR x_flip_in.
    uint8_t x_flip = (hq & 0x04) ? 0x80 : 0x00;
    uint8_t y_flip = static_cast<uint8_t>(x_flip ^ x_flip_in);

    uint8_t quadrant = static_cast<uint8_t>(hq & 0x03);

    uint8_t sprite;
    if (quadrant != 2) {
        // Angled sprite — 0 HORIZONTAL, 1 45_HEAD_UP, 3 45_HEAD_DOWN.
        sprite = quadrant;
    } else {
        // Vertical: standing / jumping / walking.
        int abs_vx = player.velocity_x >= 0 ? player.velocity_x : -player.velocity_x;
        bool supported = (player.flags & ObjectFlags::SUPPORTED) != 0;
        if ((abs_vx >> 1) == 0) {
            sprite = 0x04; // SPACESUIT_VERTICAL (standing)
        } else if (!supported) {
            sprite = 0x02; // SPACESUIT_JUMPING
        } else {
            // Walking: advance timer using &2555 update_sprite_offset_using_velocities.
            int abs_vy = player.velocity_y >= 0 ? player.velocity_y : -player.velocity_y;
            int max_vel = abs_vx > abs_vy ? abs_vx : abs_vy;
            uint8_t increment = static_cast<uint8_t>(1 + (max_vel >> 4));
            player.timer = static_cast<uint8_t>((player.timer + increment) & 0x07);
            uint8_t stage = static_cast<uint8_t>(player.timer >> 1); // 0..3

            // If walking opposite to facing, reverse the animation.
            bool moving_left = player.velocity_x < 0;
            bool facing_left = (x_flip & 0x80) != 0;
            if (moving_left != facing_left) stage ^= 0x03;

            sprite = static_cast<uint8_t>(0x04 + stage);
        }
    }

    player.sprite = sprite;

    if (x_flip & 0x80) player.flags |= ObjectFlags::FLIP_HORIZONTAL;
    else               player.flags &= ~ObjectFlags::FLIP_HORIZONTAL;
    if (y_flip & 0x80) player.flags |= ObjectFlags::FLIP_VERTICAL;
    else               player.flags &= ~ObjectFlags::FLIP_VERTICAL;
}

} // namespace

// Port of &3795 update_player_angle_facing_and_sprite (stripped down).
// Computes a target body angle from the current acceleration vector and
// slews the player's angle toward it at a quarter of the deviation per
// frame. Updates the facing direction from horizontal acceleration, then
// hands off to set_spacesuit_sprite_from_angle.
void Game::update_player_sprite(int8_t accel_x, int8_t accel_y) {
    Object& player = object_mgr_.player();
    bool supported = (player.flags & ObjectFlags::SUPPORTED) != 0;

    // Lying-down short-circuit. The 6502 forces the spacesuit
    // horizontal at &2c7f-&2c91 by snapping the angle accumulator to
    // the horizontal half-quadrant; we go straight to the HORIZONTAL
    // sprite (0x00) and leave the flip alone so the player retains
    // their last facing direction.
    if (player_lying_down_) {
        player.sprite = 0x00;
        if (player_facing_ & 0x80) player.flags |= ObjectFlags::FLIP_HORIZONTAL;
        else                       player.flags &= ~ObjectFlags::FLIP_HORIZONTAL;
        // Keep angle parked at horizontal so we don't pop back to
        // upright the moment lying-down is toggled off — feels less
        // jarring than the angle suddenly flipping 90°.
        player_angle_ = (player_facing_ & 0x80) ? 0x80 : 0x00;
        return;
    }

    uint8_t target_angle;
    if ((accel_x != 0 || accel_y != 0) && !supported) {
        // Airborne thrust: the 6502 at &385d-&3870 passes the raw
        // acceleration straight into calculate_angle_from_vector, whose
        // result already points in the thrust direction. No negation.
        target_angle = angle_from_vector(accel_x, accel_y);
    } else {
        target_angle = 0xc0; // upright (head up)
    }

    // Slew: angle += deviation / 4 (signed arithmetic shift).
    int8_t deviation = static_cast<int8_t>(target_angle - player_angle_);
    int8_t delta = static_cast<int8_t>(deviation / 4);
    player_angle_ = static_cast<uint8_t>(player_angle_ + delta);

    // player_facing_ is set in apply_player_input from the move_left /
    // move_right edge — matches the 6502's order at &38b0-&38b7
    // (facing decided BEFORE the walking branch's slope-vector rewrites
    // acceleration_x). Re-deriving from accel_x here would flip the
    // sprite 180° on every deceleration, since the walking model now
    // produces a negative accel_x to brake a positive velocity.
    set_spacesuit_sprite_from_angle(player, player_angle_, player_facing_);
}
