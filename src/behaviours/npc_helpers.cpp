#include "behaviours/npc_helpers.h"
#include "objects/object_data.h"
#include "rendering/sprite_atlas.h"
#include "core/types.h"
#include <cstdlib>

namespace NPC {

// Not a single 6502 routine — the original doesn't centralise this.
// Most flying creatures call `DEC this_object_acceleration_y` inline
// after move_towards_target (e.g. birds at &4686, wasps at &4f31) to
// cancel the +1 gravity that apply_acceleration_to_velocities (&1f01)
// applies every frame. We factor it out so the behaviour .cpp's don't
// have to mutate velocity_y by hand.
void cancel_gravity(Object& obj) {
    // Counteract the +1 gravity applied by physics each frame.
    if (obj.velocity_y > 0) obj.velocity_y--;
}

// Not a direct 6502 port. The original uses the much more elaborate
// calculate_firing_vector_from_angle_A (&3311) fed with a magnitude-
// plus-angle pair from consider_finding_target / consider_updating_npc
// _path. This helper is the reduced "just move toward target_x / y at
// `speed`" used by simpler creatures (imps, clawed robots) that want a
// cheap homing update.
void move_toward(Object& obj, uint8_t target_x, uint8_t target_y, int8_t speed) {
    int8_t dx = static_cast<int8_t>(target_x - obj.x.whole);
    int8_t dy = static_cast<int8_t>(target_y - obj.y.whole);

    if (dx > 0) obj.velocity_x = speed;
    else if (dx < 0) obj.velocity_x = -speed;

    if (dy > 0) obj.velocity_y = speed;
    else if (dy < 0) obj.velocity_y = -speed;
}

// Not a 6502 port — helper. The 6502 picks animation frames per-type
// via change_object_sprite_to_base_plus_A (&3292) fed from the
// per-type sprite-offset calc at &2555. Use that pair instead when
// matching original behaviour; this helper stays for simple creatures
// that don't need the offset-plus-timer machinery.
void set_sprite_from_velocity(Object& obj, uint8_t base_sprite, int num_frames) {
    int frame = 0;
    if (obj.velocity_x != 0 || obj.velocity_y != 0) {
        frame = (std::abs(obj.velocity_x) + std::abs(obj.velocity_y)) & (num_frames - 1);
    }
    obj.sprite = base_sprite + frame;
}

// Not a direct 6502 port — convenience. The 6502's walking animation is
// driven by update_sprite_offset_using_velocities (&2555) plus per-type
// base sprites; see imp / frogman / chatter updates. This helper is a
// cheaper fixed-rate cycle used by updates we haven't fully ported yet.
void animate_walking(Object& obj, uint8_t base_sprite, uint8_t frame_counter) {
    uint8_t frame = (frame_counter >> 2) & 0x03;
    obj.sprite = base_sprite + frame;
}

// Thin wrapper over damage_object (&24a6). The 6502 routine takes
// A = damage, Y = target slot and applies damage to this_object_energy.
// Our helper folds in the "touching the player?" guard — callers
// previously had to do it themselves and many forgot.
void damage_player_if_touching(Object& obj, Object& player, uint8_t damage) {
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        if (obj.touching == 0) { // Touching player (slot 0)
            if (player.energy > damage) {
                player.energy -= damage;
            } else {
                player.energy = 0;
            }
        }
    }
}

// Port of give_object_minimum_energy (&352e). The 6502 leaves early if
// energy is 0 (object is exploding); we skip that guard because our
// caller never passes a zero-energy object here. Called as "hold this
// type's minimum energy" by most creature updates (bird, wasp, robots).
void enforce_minimum_energy(Object& obj, uint8_t min_energy) {
    if (obj.energy < min_energy) {
        obj.energy = min_energy;
    }
}

// Convenience wrapper — not a 6502 routine. The original reaches the
// player via consider_finding_target / consider_updating_npc_path
// (&3bf8 / &3d26) and `objects_target_object_and_flags`. seek_player
// shortcuts the lookup for updates that always want the player.
void seek_player(Object& obj, const Object& player, int8_t speed) {
    move_toward(obj, player.x.whole, player.y.whole, speed);
}

// Not a 6502 routine — the inverse of seek_player. The original avoids
// targets via `avoid_fireballs` (&3c09) which uses a negated angle
// feeding move_towards_target; flee_player is our simpler sign-only
// equivalent, used by a handful of behaviours that run from the player
// without needing the full vector math.
void flee_player(Object& obj, const Object& player, int8_t speed) {
    int8_t dx = static_cast<int8_t>(obj.x.whole - player.x.whole);
    int8_t dy = static_cast<int8_t>(obj.y.whole - player.y.whole);

    if (dx >= 0) obj.velocity_x = speed;
    else obj.velocity_x = -speed;

    if (dy >= 0) obj.velocity_y = speed;
    else obj.velocity_y = -speed;
}

// Port of flip_object_to_match_velocity_x (&257e) — always flips, no
// probability gate. Used directly by NPCs whose 6502 path enters at &257e
// (piranha/wasp at &4f68).
void face_movement_direction(Object& obj) {
    if (obj.velocity_x < 0) {
        obj.flags |= ObjectFlags::FLIP_HORIZONTAL;
    } else if (obj.velocity_x > 0) {
        obj.flags &= ~ObjectFlags::FLIP_HORIZONTAL;
    }
}

// Port of consider_flipping_object_to_match_velocity_x (&2578). The 6502
// does `LDA #&03 / AND rnd_state / BNE leave` — a 1-in-4 chance of even
// attempting the flip this frame. Prevents single-frame sign changes in
// velocity_x (common when seek_player trims the delta to zero near the
// target) from turning into visible sprite flicker.
void consider_face_movement_direction(Object& obj, Random& rng) {
    if ((rng.next() & 0x03) != 0) return;
    face_movement_direction(obj);
}

// Reduced port of create_child_object (&33b8) / create_projectile
// (&33ab). The 6502 routines handle vector-from-angle velocity, x_flip
// inheritance and sub-tile centring; this helper only does the "put a
// new primary at this object's position" part. Callers wanting the
// full launch math should compute their own velocities afterwards.
int fire_projectile(Object& obj, ObjectType bullet_type, UpdateContext& ctx) {
    return ctx.mgr.create_object_at(bullet_type, 4, obj);
}

// Port of create_child_object (&33b8-&342f) X/Y offset. Shifts `child`
// from the parent's origin onto the firing side of the parent's AABB with
// a relative-velocity pre-compensation. Called from NPC firing code and
// from Weapon::fire so that player bullets and turret/robot bullets share
// the same spawn geometry. Skipping this causes the bullet to spawn
// inside the parent's tile and explode on frame 1.
void offset_child_from_parent(Object& child, const Object& parent) {
    if (child.sprite > 0x7c || parent.sprite > 0x7c) return;

    const SpriteAtlasEntry& pe = sprite_atlas[parent.sprite];
    const SpriteAtlasEntry& be = sprite_atlas[child.sprite];

    // Byte-for-byte reconstruction of the 6502 width/height bytes: upper
    // nibble = (pixels_or_rows - 1), low bit = the flip flag.
    int parent_w_byte = (pe.w > 0 ? (pe.w - 1) : 0) * 16
                      | (pe.intrinsic_flip & 0x01);
    int child_w_byte  = (be.w > 0 ? (be.w - 1) : 0) * 16
                      | (be.intrinsic_flip & 0x01);
    int parent_h_byte = (pe.h > 0 ? (pe.h - 1) : 0) * 8
                      | ((pe.intrinsic_flip >> 1) & 0x01);
    int child_h_byte  = (be.h > 0 ? (be.h - 1) : 0) * 8
                      | ((be.intrinsic_flip >> 1) & 0x01);

    // &33d2-&33e2: Y-centre the child on the parent.
    {
        int dy = (parent_h_byte - child_h_byte) / 2;
        int new_y = static_cast<int>(child.y.whole) * 256
                  + static_cast<int>(child.y.fraction) + dy;
        child.y.whole    = static_cast<uint8_t>((new_y >> 8) & 0xff);
        child.y.fraction = static_cast<uint8_t>(new_y & 0xff);
    }

    // &33e5-&342d: X-offset using relative velocity (parent_vx - bullet_vx)
    // to pick the side, plus extra pre-compensation so the next frame
    // lands at the naive "past the edge" position.
    int parent_vx = parent.velocity_x;
    int bullet_vx = child.velocity_x;
    int rel_vx    = parent_vx - bullet_vx;
    int8_t rel_s  = static_cast<int8_t>(rel_vx);
    bool same_direction =
        ((static_cast<uint8_t>(rel_s) ^
          static_cast<uint8_t>(parent_vx)) & 0x80) != 0;

    int dx_primary;
    if (rel_s < 0) {
        dx_primary =  parent_w_byte + 0x18;
    } else {
        dx_primary = -(child_w_byte  + 0x18);
    }
    int extra = same_direction ? rel_s : -bullet_vx;
    int dx    = dx_primary + extra;

    int new_x = static_cast<int>(child.x.whole) * 256
              + static_cast<int>(child.x.fraction) + dx;
    child.x.whole    = static_cast<uint8_t>((new_x >> 8) & 0xff);
    child.x.fraction = static_cast<uint8_t>(new_x & 0xff);
}

uint8_t angle_from_deltas(int8_t dx, int8_t dy) {
    // Port of &22d4 calculate_angle_from_vector. Same core algorithm the
    // projectile.cpp port uses for bullet orientation — duplicated here to
    // avoid a cross-TU dependency, and because the 6502 itself reaches this
    // code from two separate entry points (&22cc for velocities, &22d4 for
    // arbitrary vectors).
    auto is_positive = [](int8_t v) { return static_cast<uint8_t>(v) <= 0x7f; };
    auto abs_u8      = [](int8_t v) {
        uint8_t u = static_cast<uint8_t>(v);
        return (u <= 0x7f) ? u : static_cast<uint8_t>((~u) + 1);
    };

    uint8_t abs_x = abs_u8(dx);
    uint8_t abs_y = abs_u8(dy);

    uint8_t vector_signs = 0;
    vector_signs = static_cast<uint8_t>((vector_signs << 1) | (is_positive(dy) ? 1 : 0));
    vector_signs = static_cast<uint8_t>((vector_signs << 1) | (is_positive(dx) ? 1 : 0));

    uint8_t A = abs_x, B = abs_y;
    bool x_ge_y = (abs_x >= abs_y);
    if (x_ge_y) { uint8_t t = A; A = B; B = t; }
    vector_signs = static_cast<uint8_t>((vector_signs << 1) | (x_ge_y ? 1 : 0));

    // Division loop producing a 5-bit quotient of min/max, 0x08 sentinel.
    uint8_t angle = 0x08;
    for (;;) {
        uint16_t A16 = static_cast<uint16_t>(A) << 1;
        bool cmp_c = (A16 >= B);
        A = static_cast<uint8_t>(cmp_c ? (A16 - B) : (A16 & 0xff));
        bool out_b7 = (angle & 0x80) != 0;
        angle = static_cast<uint8_t>((angle << 1) | (cmp_c ? 1 : 0));
        if (out_b7) break;
    }

    static constexpr uint8_t HALF_QUADRANTS[8] = {
        0xbf, 0x80, 0xc0, 0xff, 0x40, 0x7f, 0x3f, 0x00,
    };
    return static_cast<uint8_t>(angle ^ HALF_QUADRANTS[vector_signs & 0x07]);
}

void vector_from_magnitude_and_angle(uint8_t magnitude, uint8_t angle,
                                     int8_t& vx, int8_t& vy) {
    // Port of &2357. Within a quadrant, rel ∈ [0, 0x40]:
    //   a = min(0x20, rel)          "near" component
    //   b = min(0x20, 0x40 - rel)   "far" component
    // so |a| + |b| ranges from 0x20 (cardinals) to 0x40 (45°).
    // We scale linearly to the caller's magnitude; at magnitude=0x20 this
    // matches the 6502 tables at #&6173 byte-for-byte.
    uint8_t quad = angle >> 6;
    uint8_t rel  = angle & 0x3f;
    int a_raw = (rel <= 0x20) ? rel : 0x20;
    int b_raw = (0x40 - rel <= 0x20) ? (0x40 - rel) : 0x20;
    int a = a_raw * magnitude / 0x20;
    int b = b_raw * magnitude / 0x20;
    switch (quad) {
        case 0: vx = static_cast<int8_t>(b);  vy = static_cast<int8_t>(a);  break;
        case 1: vx = static_cast<int8_t>(-a); vy = static_cast<int8_t>(b);  break;
        case 2: vx = static_cast<int8_t>(-b); vy = static_cast<int8_t>(-a); break;
        case 3: vx = static_cast<int8_t>(a);  vy = static_cast<int8_t>(-b); break;
    }
}

bool compute_firing_vector(const Object& from, const Object& target,
                           uint8_t firing_velocity_times_four,
                           int8_t& vx, int8_t& vy) {
    // --- &22a0 centre-to-centre 16-bit delta -----------------------------
    auto centre = [](const Object& o, bool is_x) -> int {
        int pixels = 0;
        if (o.sprite <= 0x7c) {
            const SpriteAtlasEntry& e = sprite_atlas[o.sprite];
            pixels = is_x ? (e.w * 16) : (e.h * 8);  // match 6502 byte units
        } else {
            pixels = 64;                             // rough fallback
        }
        int whole = is_x ? o.x.whole    : o.y.whole;
        int frac  = is_x ? o.x.fraction : o.y.fraction;
        return whole * 256 + frac + (pixels / 2);
    };
    int sx = centre(from, true),   sy = centre(from, false);
    int tx = centre(target, true), ty = centre(target, false);
    int dx = tx - sx, dy = ty - sy;

    int adx = std::abs(dx), ady = std::abs(dy);
    int max_axis = (adx > ady) ? adx : ady;
    if (max_axis == 0) return false;

    // --- tiles_log = number of right-shifts to bring dx/dy into a byte.
    // Matches the 6502's &2322 ASL + repeated LSRs counted in Y: the ASL
    // adds one, then each LSR loop iter adds another. tiles_log >= 6 means
    // the target is 16+ tiles away in max axis (out of range per &335c).
    int tile_dist = max_axis / 256;
    int tiles_log = 0;
    for (int m = tile_dist; m > 0; m >>= 1) tiles_log++;
    tiles_log++;                              // &2322's leading ASL
    if (tiles_log >= 6) return false;

    // Normalise the 16-bit relative positions by the same `tiles_log`
    // right-shifts the 6502 applies at &2323-&232d. This brings the
    // max-axis component into a byte so the angle/magnitude calc below
    // treats it like a signed velocity.
    int nvx = dx >> tiles_log;
    int nvy = dy >> tiles_log;

    uint8_t angle     = angle_from_deltas(
        static_cast<int8_t>(nvx), static_cast<int8_t>(nvy));
    uint8_t magnitude = static_cast<uint8_t>(
        (std::abs(nvx) > std::abs(nvy)) ? std::abs(nvx) : std::abs(nvy));

    // --- &2357 vector_from_magnitude_and_angle --------------------------
    uint8_t firing_velocity = firing_velocity_times_four >> 2;
    if (firing_velocity == 0) return false;
    int8_t out_vx, out_vy;
    vector_from_magnitude_and_angle(firing_velocity, angle, out_vx, out_vy);

    // --- &3362-&338c gravity compensation -------------------------------
    // The 6502's 8-bit division loop at &336a-&3374 has an off-by-one that
    // makes its effective output `(magnitude * 8) / firing_velocity` — the
    // first ROL A consumes the carry left by the pre-loop ASL rather than
    // a dividend bit, so the quotient is half what a conventional
    // algorithm would produce. The scaling loop at &337e then shifts by
    // (tiles_log + 4) and keeps the high byte. Algebraic form:
    //
    //   gravity_comp = (magnitude << (tiles_log - 1)) / firing_velocity
    //
    // Works out the same as doing the literal 6502 register dance, and
    // produces the right numbers — e.g. a flat 2-tile shot at firing_
    // velocity 45 yields gravity_comp = 3, which lifts the bullet just
    // enough to reach the target without arcing past it. The earlier
    // attempt lost the halving and fired nearly-vertical on flat shots.
    if (firing_velocity == 0) return false;
    int shift = tiles_log - 1;
    int gravity_comp = (shift >= 0)
        ? ((static_cast<int>(magnitude) << shift) / firing_velocity)
        : (static_cast<int>(magnitude) / (firing_velocity << -shift));
    gravity_comp &= 0xff;

    // EOR #&ff + SEC ADC vy  ≡  vy - gravity_comp (two's complement).
    int new_vy = static_cast<int>(out_vy) - gravity_comp;
    if (new_vy < -128 || new_vy > 127) return false;   // &338a BVS leave
    out_vy = static_cast<int8_t>(new_vy);

    // --- &3392-&339a target leading --------------------------------------
    int new_vx = static_cast<int>(out_vx) + static_cast<int>(target.velocity_x);
    if (new_vx >  127) new_vx =  127;      // prevent_overflow
    if (new_vx < -128) new_vx = -128;
    out_vx = static_cast<int8_t>(new_vx);

    // --- &339f-&33a2 final "not too fast" cap ----------------------------
    int abs_vx = std::abs(static_cast<int>(out_vx));
    int abs_vy = std::abs(static_cast<int>(out_vy));
    int max_out = (abs_vx > abs_vy) ? abs_vx : abs_vy;
    if (max_out > firing_velocity_times_four) return false;

    vx = out_vx;
    vy = out_vy;
    return true;
}

bool fire_at_target(const Object& from, const Object& target,
                    Random& rng, int8_t& vx, int8_t& vy) {
    // Port of &278a-&2791. Random firing velocity in [&b4, &f3] (i.e.
    // firing_velocity in [&2d, &3c] after the >> 2). The RNG gate in
    // &276c that decides "fire this frame?" based on energy / entropy
    // is the caller's responsibility — typical sites already wrap the
    // call in an every-N-frames condition plus an energy check.
    uint8_t fvt4 = static_cast<uint8_t>(
        0xb4 + (rng.next() & 0x3f));
    return compute_firing_vector(from, target, fvt4, vx, vy);
}

void aim_toward(int8_t& vel_x, int8_t& vel_y,
                const Object& from, const Object& target, uint8_t speed) {
    // Signed tile delta (wraps are fine — within-viewport targets are
    // always well within int8_t range).
    int dx = static_cast<int8_t>(target.x.whole - from.x.whole);
    int dy = static_cast<int8_t>(target.y.whole - from.y.whole);

    int denom = std::abs(dx) + std::abs(dy);
    if (denom == 0) {                       // coincident → fire right
        vel_x = static_cast<int8_t>(speed);
        vel_y = 0;
        return;
    }

    int s = static_cast<int>(speed);
    int vx = dx * s / denom;                // diamond: |vx| + |vy| ≈ speed
    int vy = dy * s / denom;
    if (vx >  127) vx =  127; if (vx < -128) vx = -128;
    if (vy >  127) vy =  127; if (vy < -128) vy = -128;
    vel_x = static_cast<int8_t>(vx);
    vel_y = static_cast<int8_t>(vy);
}

// Port of update_sprite_offset_using_velocities (&2555-&256c). "Max of
// |vx|, |vy|" divided by 16, plus 1, plus existing timer, mod
// `modulus`. Faster-moving objects tick through their frames faster.
uint8_t update_sprite_offset_using_velocities(Object& obj, uint8_t modulus) {
    uint8_t ax = static_cast<uint8_t>(std::abs(obj.velocity_x));
    uint8_t ay = static_cast<uint8_t>(std::abs(obj.velocity_y));
    uint8_t m  = (ax > ay) ? ax : ay;
    m = static_cast<uint8_t>(m >> 4);
    uint16_t sum = static_cast<uint16_t>(obj.timer) + 1 + m;
    if (modulus == 0) modulus = 1;
    obj.timer = static_cast<uint8_t>(sum % modulus);
    return obj.timer;
}

// Port of change_object_sprite_to_base_plus_A (&3292). Indexes the
// per-type base sprite in object_types_sprite[] and adds `offset` for
// the animation frame. Pairs with update_sprite_offset_using_velocities
// (&2555) — that routine picks the offset, this one commits it.
void change_object_sprite_to_base_plus_A(Object& obj, uint8_t offset) {
    uint8_t tidx = static_cast<uint8_t>(obj.type);
    if (tidx >= static_cast<uint8_t>(ObjectType::COUNT)) return;
    obj.sprite = static_cast<uint8_t>(object_types_sprite[tidx] + offset);
}

// Port of dampen_this_object_velocities_twice (&321f). Two consecutive
// arithmetic shifts right per axis — signed halving preserves
// direction, two halves divides by 4. Used by birds when underwater
// (&4688) and a few other "slow things down" cases.
void dampen_velocities_twice(Object& obj) {
    for (int pass = 0; pass < 2; pass++) {
        obj.velocity_x = static_cast<int8_t>(obj.velocity_x >> 1);
        obj.velocity_y = static_cast<int8_t>(obj.velocity_y >> 1);
    }
}

// Per-axis port of the 6502's apply_weighted_acceleration_to_this_
// object_velocity (&31f6):
//   delta = desired_vel - current_vel      ; signed
//   delta = clamp(delta, -max_accel, max_accel)
//   current_vel += delta
//
// Free function (not a lambda) per CLAUDE.md.
static void apply_weighted_acceleration(int8_t& v, int8_t desired,
                                         uint8_t max_accel) {
    int delta = int(desired) - int(v);
    int cap = int(max_accel);
    if (delta >  cap) delta =  cap;
    if (delta < -cap) delta = -cap;
    int nv = int(v) + delta;
    if (nv >  127) nv =  127;
    if (nv < -128) nv = -128;
    v = static_cast<int8_t>(nv);
}

// Port of move_towards_target_with_probability_X (&31da). The 6502
// sequence is:
//   CPX rnd ; BCC leave                 ; prob X/256
//   STY maximum_acceleration            ; Y is the clamp magnitude
//   set_target_object_x_y_from_this_object_tx_ty
//   use_vector_between_object_centres (A = magnitude)
//   for axis in (y, x):
//     apply_weighted_acceleration_to_this_object_velocity(vector_N)
//
// `use_vector_between_object_centres` (&3347) returns (vector_x,
// vector_y) as a diamond-metric signed pair roughly proportional to
// (target - self) with magnitude `magnitude`. We approximate that
// with a per-axis sign*magnitude — good enough since apply_weighted_
// acceleration clamps the delta to max_accel anyway: as long as the
// desired-vel has the right sign and |desired - current| >= max_accel,
// the creature accelerates by the full max_accel per call (matching
// the 6502's big jumps).
//
// Previous reduced port nudged by max_accel/4 per call — too small to
// survive bounce_reflect (which zeroes any |v| <= 2), so newly-
// spawned birds couldn't build enough speed to escape the nest tile
// once they touched any adjacent solid.
void move_towards_target_with_probability(Object& obj, UpdateContext& ctx,
                                          uint8_t magnitude,
                                          uint8_t max_accel,
                                          uint8_t prob_threshold) {
    uint8_t roll = ctx.rng.next();
    if (roll > prob_threshold) return;

    // Resolve target slot — low 5 bits of target_and_flags, zero = player.
    uint8_t slot = obj.target_and_flags & 0x1f;
    const Object& target = (slot < GameConstants::PRIMARY_OBJECT_SLOTS &&
                            ctx.mgr.object(slot).is_active())
                           ? ctx.mgr.object(slot)
                           : ctx.mgr.player();

    int8_t tdx = static_cast<int8_t>(target.x.whole - obj.x.whole);
    int8_t tdy = static_cast<int8_t>(target.y.whole - obj.y.whole);

    // Desired velocity ≈ sign(delta) * magnitude on each axis. The real
    // &3347 uses a diamond metric that splits the magnitude between
    // axes by the direction ratio, but the apply_weighted_acceleration
    // clamp saturates to max_accel long before the ratio matters for
    // creatures whose max_accel is small (birds: 8, wasps: 4, etc.).
    int desired_x = (tdx > 0) ?  int(magnitude)
                  : (tdx < 0) ? -int(magnitude) : 0;
    int desired_y = (tdy > 0) ?  int(magnitude)
                  : (tdy < 0) ? -int(magnitude) : 0;
    if (desired_x >  127) desired_x =  127;
    if (desired_x < -128) desired_x = -128;
    if (desired_y >  127) desired_y =  127;
    if (desired_y < -128) desired_y = -128;

    apply_weighted_acceleration(obj.velocity_x,
                                 static_cast<int8_t>(desired_x), max_accel);
    apply_weighted_acceleration(obj.velocity_y,
                                 static_cast<int8_t>(desired_y), max_accel);
}

} // namespace NPC
