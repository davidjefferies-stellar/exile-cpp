#include "game/game.h"

namespace {

// &22d4 calculate_angle_from_vector. Five-bit binary division of
// min(|vx|,|vy|) / max yields the slope; XOR an octant offset from the
// &14bf half-quadrant table. 0x00=right, 0x40=down, 0x80=left, 0xc0=up.
uint8_t angle_from_vector(int8_t vx, int8_t vy) {
    // &233d get_absolute_vector_components: |vx|, |vy|; record signs.
    bool y_pos = vy >= 0;
    bool x_pos = vx >= 0;
    uint8_t ay = static_cast<uint8_t>(y_pos ?  int(vy) : -int(vy));
    uint8_t ax = static_cast<uint8_t>(x_pos ?  int(vx) : -int(vx));

    // &22d7 swap so magnitude = max(|vx|,|vy|); x_ge_y is the octant bit
    // (&22e0 ROL &99) saying "x dominates".
    bool x_ge_y = ax >= ay;
    uint8_t magnitude = x_ge_y ? ax : ay;
    uint8_t small     = x_ge_y ? ay : ax;

    // &22e2-&22ef. Seed 0b00001000; ASL small / conditional SBC / ROL
    // angle five times leaves the division bits in low 5; the sentinel
    // overflows out on the 5th ROL and ends the loop. The 6502's CMP
    // clobbers the carry out of ASL, so the shifted-out bit is lost.
    uint8_t angle = 0x08;
    while (true) {
        small = static_cast<uint8_t>(small << 1);
        bool ge = small >= magnitude;
        if (ge) small = static_cast<uint8_t>(small - magnitude);
        bool sentinel_out = (angle & 0x80) != 0;
        angle = static_cast<uint8_t>((angle << 1) | (ge ? 1 : 0));
        if (sentinel_out) break;
    }

    // &14bf angle_calculation_half_quadrants_table indexed by
    // (y_pos << 2) | (x_pos << 1) | x_ge_y.
    static constexpr uint8_t HALF_QUADRANT[8] = {
        0xbf, 0x80, 0xc0, 0xff, 0x40, 0x7f, 0x3f, 0x00,
    };
    int idx = (x_ge_y ? 1 : 0) | (x_pos ? 2 : 0) | (y_pos ? 4 : 0);
    return static_cast<uint8_t>(angle ^ HALF_QUADRANT[idx]);
}

// &3906 set_spacesuit_sprite_from_angle. Quantises angle into 8 half-
// quadrants, flips to face the player, then picks one of four angled
// sprites or hands off to walking/standing/jumping for the vertical case.
void set_spacesuit_sprite_from_angle(Object& player,
                                     uint8_t angle,
                                     uint8_t x_flip_in) {
    // &3906-&390b five LSRs + ADC #&00. Last LSR's out-bit is bit 4 of
    // the original angle; the ADC #&00 rounds half-quadrant to nearest.
    bool carry = false;
    uint8_t a = angle;
    for (int i = 0; i < 5; ++i) {
        carry = (a & 1) != 0;
        a = static_cast<uint8_t>(a >> 1);
    }
    uint8_t hq = static_cast<uint8_t>(a + (carry ? 1 : 0));

    // &390d-&3913 if facing right (x_flip bit 7 clear), reverse the
    // sequence with EOR #&07 + ADC #&01. Carry into the ADC is always 0
    // since the prior add maxes at 7+1=8.
    if (!(x_flip_in & 0x80)) {
        hq = static_cast<uint8_t>((hq ^ 0x07) + 1);
    }

    // &3915-&391f this_object_x_flip from bit 2 of hq (head above feet);
    // this_object_y_flip from x_flip XOR x_flip_in.
    uint8_t x_flip = (hq & 0x04) ? 0x80 : 0x00;
    uint8_t y_flip = static_cast<uint8_t>(x_flip ^ x_flip_in);

    // &3922 AND #&03. 0 horizontal, 1 45-head-up, 2 vertical, 3 45-head-down.
    uint8_t quadrant = static_cast<uint8_t>(hq & 0x03);

    uint8_t sprite;
    if (quadrant != 2) {
        sprite = quadrant;
    } else {
        // &3928-&392e standing if (|vx| >> 1) == 0. invert_if_negative
        // leaves 0x80 untouched so vx == -128 reads as 0x80 -> 0x40 (not
        // standing).
        int abs_vx = player.velocity_x >= 0 ? int(player.velocity_x)
                                            : -int(player.velocity_x);
        // &3930 check_if_player_or_npc_jumping_or_flying: low nibble of
        // state >= 0x0a means airborne. Tracked in integrate_player_motion.
        bool jumping_or_flying = (player.state & 0x0f) >= 0x0a;
        if ((abs_vx >> 1) == 0) {
            sprite = 0x04; // SPRITE_SPACESUIT_VERTICAL (standing)
        } else if (jumping_or_flying) {
            sprite = 0x02; // SPRITE_SPACESUIT_JUMPING
        } else if (player.flags & ObjectFlags::TELEPORTING) {
            // Don't clobber player.timer while teleporting — it holds
            // the 0x20-down teleport-animation countdown, and overwriting
            // it with the walking-cycle counter (0..7) means timer never
            // hits 0x10 and the position rewrite at advance_player_
            // teleport never fires. Keep the standing sprite.
            sprite = 0x04;
        } else {
            // &3937-&3946 walking. &2555 update_sprite_offset_using_
            // velocities with modulus 8: timer += 1 + (max(|vx|,|vy|)/16)
            // wrapped to [0,8); LSR halves to four animation stages.
            int abs_vy = player.velocity_y >= 0 ? int(player.velocity_y)
                                                : -int(player.velocity_y);
            int max_vel = abs_vx > abs_vy ? abs_vx : abs_vy;
            uint8_t increment = static_cast<uint8_t>(1 + (max_vel >> 4));
            player.timer = static_cast<uint8_t>((player.timer + increment) & 0x07);
            uint8_t stage = static_cast<uint8_t>(player.timer >> 1);

            // &393e-&3946 walking-backwards check: sign(vx) XOR x_flip.
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

// &3795 update_player_angle_facing_and_sprite (sprite/angle/palette subset).
// Energy regen, immobility, rotation and the facing rewrite are owned by
// object_update / player_actions; this routine does the slew, the sprite
// pick (&3906), and the palette / damage flash (&38d9-&3903).
void Game::update_player_sprite(int8_t accel_x, int8_t accel_y) {
    Object& player = object_mgr_.player();

    // &37a4-&37c3 jetpack_functional. The 6502 ANDs in a reliability
    // roll against &da rnd_state+1; skipped here to keep the rng stream
    // free of new consumers. Movement/thrust immobility decrement is
    // owned by player_actions.
    bool jetpack_functional = (weapon_energy_[0] > 0) &&
                              (player_immobility_movement_ < 0x06) &&
                              (player_immobility_thrust_ == 0);

    // &38ec-&38fa protected suit 0x33 (rcY), unprotected 0x3e (mwY).
    uint8_t base_palette = (weapon_energy_[5] > 0) ? 0x33 : 0x3e;

    // &38d9-&3901 low-energy strobe. The 6502 compares (frame_counter
    // & 0x1f) * 2 against energy: a 32-frame outer cycle that produces
    // alternating strobe-on / strobe-off windows whose widths scale with
    // energy — lower energy widens the strobe window, full energy
    // disables it. Port-only deviation: inside the strobe window we
    // also gate on frame parity so the flicker reads as 25Hz (fast
    // damage cue) instead of a flat hold. EOR #&0b: 0x33<->0x38,
    // 0x3e<->0x35 (gyY).
    bool strobe_window = (((frame_counter_ & 0x1f) << 1) >= player.energy);
    if (strobe_window && (frame_counter_ & 0x01)) {
        base_palette ^= 0x0b;
    }
    player.palette = base_palette;

    // &3874-&3884 target angle. Acceleration-derived if accelerating AND
    // jetpack functional; else upright. Lying-down (&3876-&3880)
    // overrides both: target 0xfd facing right, 0x83 facing left
    // (slightly above horizontal, head above feet).
    uint8_t target_angle;
    if ((accel_x != 0 || accel_y != 0) && jetpack_functional) {
        target_angle = angle_from_vector(accel_x, accel_y);
    } else {
        target_angle = 0xc0; // upright (head up)
    }
    if (player_lying_down_) {
        target_angle = (player_facing_ & 0x80) ? 0x83 : 0xfd;
    }

    int8_t deviation = static_cast<int8_t>(target_angle - player_angle_);

    // &3885-&3891 vertical-only-accel clamp. If only accel_y is non-zero
    // and deviation is within ~17 degrees of 180 (0x74 <= dev < 0x8c),
    // pin to 0 so vertical thrust doesn't flip the body 180 degrees.
    if (accel_x == 0 && accel_y != 0) {
        uint8_t udev = static_cast<uint8_t>(deviation);
        uint8_t shifted = static_cast<uint8_t>(udev - 0x74);
        if (shifted < 0x18) deviation = 0;
    }

    // &389d-&38ae slew. Runs only when jumping_or_flying (&3b8c: low
    // nibble of state >= 0x0a) OR lying_down; standing-and-not-lying
    // freezes the angle. &3278 divide_by_four = arithmetic shift right 2.
    bool jumping_or_flying = (player.state & 0x0f) >= 0x0a;
    int8_t delta = 0;
    if (jumping_or_flying || player_lying_down_) {
        delta = static_cast<int8_t>(deviation >> 2);
    }
    player_angle_ = static_cast<uint8_t>(player_angle_ + delta);

    // &38b0-&38b7 facing rewrite is done by apply_player_input from the
    // raw move_left/right edges; re-deriving here flips the sprite on
    // deceleration since walking produces a braking accel_x.

    set_spacesuit_sprite_from_angle(player, player_angle_, player_facing_);
}
