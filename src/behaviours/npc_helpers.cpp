#include "behaviours/npc_helpers.h"
#include "objects/object_data.h"
#include "rendering/sprite_atlas.h"
#include "audio/audio.h"
#include "core/types.h"
#include <cstdlib>

namespace NPC {

// Port-only helper. 6502 inlines `DEC this_object_acceleration_y` per
// flier (birds &4686, wasps &4f31) to cancel &1f01's +1 gravity.
void cancel_gravity(Object& obj) {
    // Counteract the +1 gravity applied by physics each frame.
    if (obj.velocity_y > 0) obj.velocity_y--;
}

// Reduced port. 6502 uses &3311 calculate_firing_vector_from_angle_A
// with magnitude+angle; this is the cheap "head to (target_x,_y) at
// speed" form for imps / clawed robots.
void move_toward(Object& obj, uint8_t target_x, uint8_t target_y, int8_t speed) {
    int8_t dx = static_cast<int8_t>(target_x - obj.x.whole);
    int8_t dy = static_cast<int8_t>(target_y - obj.y.whole);

    if (dx > 0) obj.velocity_x = speed;
    else if (dx < 0) obj.velocity_x = -speed;

    if (dy > 0) obj.velocity_y = speed;
    else if (dy < 0) obj.velocity_y = -speed;
}

// Port-only helper. 6502 path is &3292 change_object_sprite_to_base_plus_A
// fed from &2555; use that when matching original behaviour.
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
// Our helper folds in the "touching the player?" guard so callers
// don't have to repeat it at every site.
void damage_player_if_touching(Object& obj, Object& player, uint8_t damage,
                               std::vector<DamageVisual>* damage_events) {
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        if (obj.touching == 0) { // Touching player (slot 0)
            uint16_t hurt = std::min<uint16_t>(damage, player.energy);
            if (player.energy > damage) {
                player.energy -= damage;
            } else {
                player.energy = 0;
            }
            if (damage_events && hurt > 0) {
                DamageVisual ev;
                ev.src_x = obj.x.whole; ev.src_y = obj.y.whole;
                ev.src_x_frac = obj.x.fraction; ev.src_y_frac = obj.y.fraction;
                ev.tgt_x = player.x.whole; ev.tgt_y = player.y.whole;
                ev.tgt_x_frac = player.x.fraction; ev.tgt_y_frac = player.y.fraction;
                ev.tgt_slot = 0;
                ev.amount = hurt;
                damage_events->push_back(ev);
            }
        }
    }
}

// Port of &352e give_object_minimum_energy:
//   &352e LDA &15 ; this_object_energy
//   &3530 BEQ &3537 ; leave            ; energy==0 → mid-explode, skip
//   &3532 JSR &3bc1 get_maximum_of_A_and_Y
//   &3535 STA &15
//   &3537 RTS
void enforce_minimum_energy(Object& obj, uint8_t min_energy) {
    if (obj.energy == 0) return;
    if (obj.energy < min_energy) {
        obj.energy = min_energy;
    }
}

// Port-only wrapper. 6502 path is &3bf8/&3d26 plus target_and_flags.
void seek_player(Object& obj, const Object& player, int8_t speed) {
    move_toward(obj, player.x.whole, player.y.whole, speed);
}

// Port-only inverse of seek_player. 6502 equivalent is &3c09
// avoid_fireballs with negated angle into move_towards_target.
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

// &2578 consider_flipping_object_to_match_velocity_x. 1-in-4 rng gate
// suppresses sprite flicker from single-frame velocity_x sign changes.
void consider_face_movement_direction(Object& obj, Random& rng) {
    if ((rng.next() & 0x03) != 0) return;
    face_movement_direction(obj);
}

// Reduced port of &33b8 create_child_object / &33ab create_projectile.
// Just spawns at the parent's position — callers compute velocities.
int fire_projectile(Object& obj, ObjectType bullet_type, UpdateContext& ctx) {
    return ctx.mgr.create_object_at(bullet_type, 4, obj);
}

// &33b8-&342f create_child_object X/Y offset. Shifts onto firing side
// with relative-velocity pre-compensation. Skipping it makes bullets
// spawn inside the parent's tile and explode on frame 1.
void offset_child_from_parent(Object& child, const Object& parent) {
    if (child.sprite > 0x80 || parent.sprite > 0x80) return;

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
    // &22d4 calculate_angle_from_vector (also reached from &22cc for
    // velocities). Duplicated from projectile.cpp to avoid cross-TU dep.
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
    // &2357. Per-quadrant rel∈[0,0x40]: near=min(0x20,rel),
    // far=min(0x20,0x40-rel); at magnitude=0x20 matches 6502 tables
    // at &6173 byte-for-byte.
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
        if (o.sprite <= 0x80) {
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

    // &3362-&338c gravity compensation. The 6502's div loop at &336a-&3374
    // has an off-by-one (first ROL consumes pre-loop ASL carry) so the
    // effective formula is gravity_comp = (mag << (tiles_log-1)) / fvel.
    // Dropping the halving overshoots and fires nearly-vertical.
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
    // &278a-&2791. Random firing velocity in [&b4,&f3] (=&2d..&3c after
    // >>2). The &276c "fire this frame?" gate is caller responsibility.
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

// Port of update_sprite_offset_using_velocities (&2555 → &2557-&256c).
// "Max of |vx|, |vy|" shifted right `divide_shift` times, plus 1, plus
// existing timer, mod `modulus`. Faster movers cycle frames faster.
uint8_t update_sprite_offset_using_velocities(Object& obj, uint8_t modulus,
                                              uint8_t divide_shift) {
    uint8_t ax = static_cast<uint8_t>(std::abs(obj.velocity_x));
    uint8_t ay = static_cast<uint8_t>(std::abs(obj.velocity_y));
    uint8_t m  = (ax > ay) ? ax : ay;
    if (divide_shift > 7) divide_shift = 7;
    m = static_cast<uint8_t>(m >> divide_shift);
    uint16_t sum = static_cast<uint16_t>(obj.timer) + 1 + m;
    if (modulus == 0) modulus = 1;
    obj.timer = static_cast<uint8_t>(sum % modulus);
    return obj.timer;
}

// &3292 change_object_sprite_to_base_plus_A. Runs the &329e-&32b3
// centring shim so the visual centre stays fixed across a sprite swap.
// Width/height stored as (n-1)<<shift in fraction units.
static int sprite_width_units(uint8_t s) {
    if (s > 0x80) return 0;
    int w = sprite_atlas[s].w;
    return (w > 0 ? (w - 1) * 16 : 0);
}
static int sprite_height_units(uint8_t s) {
    if (s > 0x80) return 0;
    int h = sprite_atlas[s].h;
    return (h > 0 ? (h - 1) * 8 : 0);
}
static void shift_position_signed(uint8_t& whole, uint8_t& frac, int delta) {
    int combined = int(whole) * 256 + int(frac) + delta;
    whole = static_cast<uint8_t>((combined >> 8) & 0xff);
    frac  = static_cast<uint8_t>(combined & 0xff);
}
void change_object_sprite_to_base_plus_A(Object& obj, uint8_t offset) {
    uint8_t tidx = static_cast<uint8_t>(obj.type);
    if (tidx >= static_cast<uint8_t>(ObjectType::COUNT)) return;
    uint8_t new_sprite = static_cast<uint8_t>(object_types_sprite[tidx] + offset);
    if (new_sprite == obj.sprite) return;
    int dx = (sprite_width_units(obj.sprite)  - sprite_width_units(new_sprite))  / 2;
    int dy = (sprite_height_units(obj.sprite) - sprite_height_units(new_sprite)) / 2;
    obj.sprite = new_sprite;
    shift_position_signed(obj.x.whole, obj.x.fraction, dx);
    shift_position_signed(obj.y.whole, obj.y.fraction, dy);
}

// Slime sprite-update via &32aa subtract_width_from_position. X-only:
// the ceiling y-anchor must stay pinned; a -20 dy would underflow
// y.whole and put RED_DROP spawns inside the ceiling tile.
void change_object_sprite_x_only(Object& obj, uint8_t offset) {
    uint8_t tidx = static_cast<uint8_t>(obj.type);
    if (tidx >= static_cast<uint8_t>(ObjectType::COUNT)) return;
    uint8_t new_sprite = static_cast<uint8_t>(object_types_sprite[tidx] + offset);
    if (new_sprite == obj.sprite) return;
    int dx = (sprite_width_units(obj.sprite) - sprite_width_units(new_sprite)) / 2;
    obj.sprite = new_sprite;
    shift_position_signed(obj.x.whole, obj.x.fraction, dx);
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

// &31f6 apply_weighted_acceleration_to_this_object_velocity, per-axis.
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

// &3be1 consider_absorbing_object_touched. Port-only: skips the &3bd5
// glancing-angle gate. PENDING_REMOVAL is our set_object_for_removal —
// main-loop GC reaps the slot.
void consider_absorbing_object_touched(Object& obj, ObjectType prey_type,
                                       ObjectManager& mgr) {
    if (obj.touching >= GameConstants::PRIMARY_OBJECT_SLOTS) return;
    Object& touched = mgr.object(obj.touching);
    if (!touched.is_active() || touched.type != prey_type) return;
    touched.flags |= ObjectFlags::PENDING_REMOVAL;
    // &14ad-&14b0 play_low_beep — the 4-byte sound block from &14b0.
    static constexpr uint8_t kSoundLowBeep[4] = { 0x5d, 0x04, 0xff, 0x05 };
    Audio::play_at(Audio::CH_ANY, kSoundLowBeep, obj.x.whole, obj.y.whole);
}

// &3bf8 consider_finding_target. Reduced for "nearest object of type X"
// (Y=0 / no range gate). Skips the &3c2a LOS cutoff used by bird→wasp
// at &4671 and fish→piranha at &4774. Chebyshev via int8_t wraps.
void consider_finding_target(Object& obj, ObjectType prey_type,
                             UpdateContext& ctx) {
    if (!ctx.every_sixteen_frames) return;
    int best_dist = 0x7fff;
    int best_slot = -1;
    for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        const Object& cand = ctx.mgr.object(i);
        if (!cand.is_active() || cand.type != prey_type) continue;
        int8_t dx = static_cast<int8_t>(cand.x.whole - obj.x.whole);
        int8_t dy = static_cast<int8_t>(cand.y.whole - obj.y.whole);
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        int d   = adx > ady ? adx : ady;
        if (d < best_dist) { best_dist = d; best_slot = i; }
    }
    if (best_slot >= 0) {
        obj.target_and_flags =
            static_cast<uint8_t>(best_slot & TargetFlags::OBJECT_MASK) |
            TargetFlags::DIRECTNESS_ONE;
    }
}

// &31da move_towards_target_with_probability_X. Approximates the &3347
// diamond-metric vector with per-axis sign*magnitude; apply_weighted_
// acceleration clamps to max_accel anyway. A smaller nudge gets killed
// by bounce_reflect's |v|<=2 zero, so newly-spawned birds get stuck.
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

    // Approximates &3347's diamond metric with sign*magnitude per axis;
    // the max_accel clamp saturates long before the ratio matters for
    // small-max_accel creatures (birds: 8, wasps: 4).
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

// &3235 calculate_seven_eighths: v - sign(v) * ((|v| + 7) >> 3). Used
// by dampen_this_object_velocity_y to bleed off ~12.5% of |vy| per call
// while preserving the sign.
static int8_t calculate_seven_eighths(int8_t v) {
    int abs_v = v < 0 ? -int(v) : int(v);
    int eighth = (abs_v + 7) >> 3;
    if (v < 0) eighth = -eighth;
    int r = int(v) - eighth;
    if (r >  127) r =  127;
    if (r < -128) r = -128;
    return static_cast<int8_t>(r);
}

void consider_hovering_over_ground(Object& obj, UpdateContext& ctx) {
    // &3a1e: BIT every_four_frames; BPL leave. Runs once every four ticks.
    if (!ctx.every_four_frames) return;

    // 6502 measures unobstructed space below via check_for_space_below_
    // object then branches on three cases:
    //   space == 0xff (≥ 1 full tile clear)  : DEC accel_y once + dampen
    //   space in (half_height, 0xff)         : leave (mid-air, no thrust)
    //   space < half_height, rnd carry       : DEC accel_y twice + dampen
    //   space < half_height, no rnd carry    : DEC accel_y three times + dampen
    // Our port lacks the per-pixel space measurement, so we collapse to
    // a binary proxy: SUPPORTED flag = "near ground" path; else = the
    // gentle "≥ 1 tile clear" path. cancel_gravity already sits on top,
    // so this just adds the ADDITIONAL upward thrust from &3a3d-&3a41
    // (1, 2, or 3 DECs of accel_y, which translate to subtracting from
    // vy directly in our model).
    int thrust;
    if (obj.is_supported()) {
        // Near-ground path. 50/50 split between -2 and -3 (rnd carry).
        thrust = ((ctx.rng.next() & 0x80) != 0) ? -2 : -3;
    } else {
        // ≥ 1 tile clear path: -1 vy.
        thrust = -1;
    }
    int vy = int(obj.velocity_y) + thrust;
    if (vy >  127) vy =  127;
    if (vy < -128) vy = -128;
    obj.velocity_y = static_cast<int8_t>(vy);

    // &3a43 JMP dampen_this_object_velocity_y.
    obj.velocity_y = calculate_seven_eighths(obj.velocity_y);
}

} // namespace NPC
