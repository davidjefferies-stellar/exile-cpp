#include "behaviours/projectile.h"
#include "behaviours/mood.h"
#include "behaviours/path.h"
#include "audio/audio.h"
#include "core/types.h"
#include "objects/collision.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "rendering/sprite_atlas.h"
#include "world/landscape.h"
#include "world/tertiary.h"
#include "world/water.h"
#include "particles/particle_system.h"
#include <algorithm>
#include <cstdlib>

namespace Behaviors {

// get_absolute_vector_component (&2346): CMP #&7f sets carry iff value
// is 0x00..0x7f (i.e. non-negative as the 6502 treats it); for &80..&ff
// it negates via EOR #&ff / ADC #&01.
static bool is_positive_6502(int8_t v) {
    return static_cast<uint8_t>(v) <= 0x7f;
}
static uint8_t abs_6502(int8_t v) {
    uint8_t u = static_cast<uint8_t>(v);
    return (u <= 0x7f) ? u : static_cast<uint8_t>((~u) + 1);
}

// Port of &4dd4 rotate_colour_from_A:
//   &4dd4 LSR A / LSR A           ; A >>= 2
//   &4dd6 AND #&03                ; A &= 3
//   &4dd8 TAX
//   &4dd9 LDA &4d82,X ; transporter_beams_palette_table
//   &4ddc STA &73 ; this_object_palette
//   &4dde RTS
// 4-entry palette cycle; we return the (a>>2)&3 index so callers can
// gate periodic events (sound, etc.) on the same low bits.
static uint8_t rotate_colour_from_A(Object& obj, uint8_t a) {
    static constexpr uint8_t PALETTE_TABLE[4] = { 0x52, 0x63, 0x35, 0x21 };
    uint8_t idx = (a >> 2) & 0x03;
    obj.palette = PALETTE_TABLE[idx];
    return idx;
}

// Port of &40db-&40ed explode_object_with_duration_A:
//   &40db JSR &13f8 play_sound_on_channel_zero (data 17 03 11 04)
//   &40e2 STA &3d ; tertiary_data_offset (explosion duration)
//   &40e4 LDA #&44 ; OBJECT_EXPLOSION
//   &40e6 STA &41 ; this_object_type
//   &40e8 LDA #&ce ; -50
//   &40ea STA &081d ; explosion_timer  (negative = explosion active)
//   &40ed RTS
// Mutates ONLY type and tertiary_data_offset; sprite/palette/energy/pos
// stay. update_explosion then takes over.
void explode_object_with_duration(Object& obj, uint8_t duration) {
    obj.tertiary_data_offset = duration;
    obj.type = ObjectType::EXPLOSION;
    // &40e8 global explosion_timer (screen flash) not wired to renderer.
}

// Port of &4005 add_to_player_mushroom_timer:
//   &4005 LDA #&3f
//   &4007 ADC &081a,X ; player_mushroom_timers
//   &400a BCS &400f                ; skip_ceiling — overflow leaves it alone
//   &400c STA &081a,X
//   &400f BIT &0815 ; mushroom_immunity_pill_collected
//   &4012 BMI &4041 ; leave        ; immunity pill → no immobility kick
//   &4014 CMP &ba,X ; immobility_timers
//   &4016 BCC &4041 ; leave        ; new timer below existing? skip
//   &4018 STA &ba,X
//   &401a RTS
// We do the timer add but skip the immobility-pill and immobility-
// timer paths (not wired yet). Call with which=0 red, =1 blue.
static void add_to_player_mushroom_timer(UpdateContext& ctx, int which, bool extra) {
    if (!ctx.player_mushroom_timers) return;
    uint8_t* t = &ctx.player_mushroom_timers[which];
    int sum = int(*t) + 0x3f + (extra ? 1 : 0);
    if (sum <= 0xff) *t = static_cast<uint8_t>(sum);
    // else: overflow — original "skip_ceiling" leaves value unchanged.
}

// Port of &1faf check_if_object_Y_damaged_by_projectiles. Returns the
// touched slot if damageable, or -1 if not touching / target is immune.
// Explosions, bushes, and (red clawed robot..Triax) are immune per &1fb6-&1fc3.
static int bullet_touching_damageable(const Object& obj, ObjectManager& mgr) {
    // Spawn-frame skip: bullets sit inside their parent's AABB until
    // velocity carries them out, so frame 1 obj.touching == parent.
    if (obj.flags & ObjectFlags::NEWLY_CREATED) return -1;
    uint8_t t = obj.touching;
    if (t >= GameConstants::PRIMARY_OBJECT_SLOTS) return -1;
    const Object& target = mgr.object(t);
    ObjectType tt = target.type;
    if (tt == ObjectType::EXPLOSION) return -1;
    if (tt == ObjectType::BUSH)      return -1;
    uint8_t ti = static_cast<uint8_t>(tt);
    uint8_t rc = static_cast<uint8_t>(ObjectType::RED_CLAWED_ROBOT);
    if (ti >= rc && ti <= rc + 1) return -1; // red clawed robot, triax
    return t;
}

// Port of &22cc calculate_angle_from_this_object_velocities. Returns the
// same 8-bit angle byte the 6502 produces, using the same divide-based
// recipe: 0x00 = +x (right), 0x40 = +y (down), 0x80 = -x (left),
// 0xc0 = -y (up). The 5-bit quotient + 3-bit octant index gives 32 steps.
static uint8_t calculate_angle_from_velocities(int8_t vx, int8_t vy) {
    // Carry from get_absolute_vector_component is rotated into
    // vector_signs after each component is processed.
    uint8_t abs_x = abs_6502(vx);
    uint8_t abs_y = abs_6502(vy);

    // Two ROLs: first rotates vy_positive into bit 0, then vx_positive
    // replaces it and the old value moves up to bit 1.
    uint8_t vector_signs = 0;
    vector_signs = static_cast<uint8_t>((vector_signs << 1) | (is_positive_6502(vy) ? 1 : 0));
    vector_signs = static_cast<uint8_t>((vector_signs << 1) | (is_positive_6502(vx) ? 1 : 0));

    // &22d7-&22e0: swap so A = min(abs_x, abs_y), B = max; ROL the swap
    // carry into vector_signs bit 0. After this, vector_signs low 3 bits
    // are { bit 0 = abs_x_ge_abs_y, bit 1 = vx_pos, bit 2 = vy_pos }.
    uint8_t A = abs_x;
    uint8_t B = abs_y;
    bool abs_x_ge_abs_y = (abs_x >= abs_y);
    if (abs_x_ge_abs_y) { uint8_t t = A; A = B; B = t; }
    vector_signs = static_cast<uint8_t>((vector_signs << 1) | (abs_x_ge_abs_y ? 1 : 0));

    // &22e2-&22ef division loop. The 0x08 sentinel shifts through the
    // angle byte, exiting when it falls out of bit 7. Produces a 5-bit
    // quotient of min/max in the low 5 bits of angle.
    uint8_t angle = 0x08;
    for (;;) {
        // ASL A on the 6502 puts old bit 7 into carry; a subsequent CMP B
        // doesn't see that extended bit directly, but SBC B (taken when
        // CMP set carry) does. We model this by working in 16 bits for the
        // compare-and-subtract step.
        uint16_t A16 = static_cast<uint16_t>(A) << 1;
        bool cmp_carry = (A16 >= B); // 6502 CMP sets carry iff A >= operand
        if (cmp_carry) {
            A = static_cast<uint8_t>(A16 - B);
        } else {
            A = static_cast<uint8_t>(A16 & 0xff);
        }
        bool angle_out_bit7 = (angle & 0x80) != 0;
        angle = static_cast<uint8_t>((angle << 1) | (cmp_carry ? 1 : 0));
        if (angle_out_bit7) break;
    }

    // &22f1-&22fb: EOR with half-quadrants table to steer the raw quotient
    // into the correct octant of the 0x00..0xff angle space.
    static constexpr uint8_t ANGLE_HALF_QUADRANTS[8] = {
        0xbf, 0x80, 0xc0, 0xff, 0x40, 0x7f, 0x3f, 0x00,
    };
    return static_cast<uint8_t>(angle ^ ANGLE_HALF_QUADRANTS[vector_signs & 0x07]);
}

// Port of move_bullet's tail (&4447-&4460). Given the bullet's computed
// angle, set flip_h / flip_v and pick one of six SPRITE_BULLET_* sprites
// relative to the type's base (0x08 = SPRITE_BULLET_HORIZONTAL).
static void orient_bullet_to_angle(Object& obj, uint8_t angle) {
    // &444a: STA &39 (y_flip). Sprite v-flip is bit 7 of the raw angle
    // (set when bullet is moving up, i.e. angle in 0x80..0xff).
    if (angle & 0x80) obj.flags |=  ObjectFlags::FLIP_VERTICAL;
    else              obj.flags &= ~ObjectFlags::FLIP_VERTICAL;

    // &444c-&4452: BIT &39 tests bit 6; if set, EOR #&ff before continuing
    // (the "moving left" branch in the disassembly comment). The result is
    // stored as x_flip, with its bit 7 driving horizontal flip.
    uint8_t x_flip = (angle & 0x40) ? static_cast<uint8_t>(~angle) : angle;
    if (x_flip & 0x80) obj.flags |=  ObjectFlags::FLIP_HORIZONTAL;
    else               obj.flags &= ~ObjectFlags::FLIP_HORIZONTAL;

    // &4454-&445e: AND #&7f, LSR×3 (16 buckets of 22.5°); then for buckets
    // 4..15 fold down via (A >> 1) ^ 6 so we end up with 6 sprite indices.
    uint8_t a = static_cast<uint8_t>((x_flip & 0x7f) >> 3);
    uint8_t offset = (a < 4) ? a : static_cast<uint8_t>((a >> 1) ^ 6);

    // &4460 change_object_sprite_to_base_plus_A. Only meaningful for types
    // whose base sprite is the bullet strip (0x08..0x0d). Balls and other
    // projectiles also run move_bullet but use different base sprites that
    // wouldn't make sense indexed as angles.
    uint8_t idx = static_cast<uint8_t>(obj.type);
    if (idx < static_cast<uint8_t>(ObjectType::COUNT)) {
        uint8_t base = object_types_sprite[idx];
        if (base == 0x08) {
            obj.sprite = static_cast<uint8_t>(base + offset);
        }
    }
}

// Port of &4425 explode_bullet: play the "ptack" pop sound, then mutate
// to EXPLOSION via &40e2 explode_object_with_duration_A_but_no_sound
// (duration 2). The "_but_no_sound" path is key — the BBC bullet path
// deliberately skips the squeal + explosion sounds that step 12's
// dispatch would otherwise add for types with explosion-flag bits 0xc0.
static void explode_bullet(Object& obj) {
    static constexpr uint8_t kSoundBulletPop[4] = { 0x17, 0x03, 0x1b, 0x02 };
    Audio::play_at(Audio::CH_PRIORITY, kSoundBulletPop,
                   obj.x.whole, obj.y.whole);
    explode_object_with_duration(obj, 2);
}

// &441b / &46bf bullet main body. Explodes on damage-target touch,
// solid-tile collision, or timer==0; otherwise faces velocity.
static void common_bullet_update(Object& obj, UpdateContext& ctx, uint8_t damage) {
    // &1faf: touching a damageable target?
    int tgt = bullet_touching_damageable(obj, ctx.mgr);
    if (tgt >= 0) {
        Object& target = ctx.mgr.object(tgt);

        // &2bb6 apply_collision_to_objects_velocities. 6502 ran this
        // BEFORE per-type update; we run it here. Doubling on larger-
        // speed axis ≈ direction of approach.
        bool x_dominant = std::abs(obj.velocity_x) >= std::abs(obj.velocity_y);
        auto vx = Collision::apply_mass_ratio_velocity(
            obj.velocity_x, target.velocity_x,
            obj.weight(), target.weight(), x_dominant);
        target.velocity_x = vx.other_v;
        auto vy = Collision::apply_mass_ratio_velocity(
            obj.velocity_y, target.velocity_y,
            obj.weight(), target.weight(), !x_dominant);
        target.velocity_y = vy.other_v;

        uint16_t hurt = std::min<uint16_t>(damage, target.energy);
        if (target.energy > damage) target.energy -= damage;
        else                        target.energy = 0;
        // 6502 &24a6 damage_object also sets WAS_DAMAGED on the target.
        target.flags |= ObjectFlags::WAS_DAMAGED;
        if (ctx.damage_events) {
            DamageVisual ev;
            ev.src_x = obj.x.whole; ev.src_y = obj.y.whole;
            ev.src_x_frac = obj.x.fraction; ev.src_y_frac = obj.y.fraction;
            ev.tgt_x = target.x.whole; ev.tgt_y = target.y.whole;
            ev.tgt_x_frac = target.x.fraction; ev.tgt_y_frac = target.y.fraction;
            ev.tgt_slot = static_cast<int8_t>(tgt);
            ev.amount = hurt;
            ctx.damage_events->push_back(ev);
        }
        explode_bullet(obj);
        return;
    }

    // &4434: reduce_energy_by_one (lifespan).
    if (obj.timer > 0) obj.timer--;
    if (obj.timer == 0) {
        explode_bullet(obj);
        return;
    }

    // &4439-&4445 tile-collision bounce vs explode. Energy >= 0x3e at the
    // moment of impact = fresh shot fired into a wall = explode (BCS path).
    // Otherwise the bullet keeps moving with energy reduced by 0x15 (SBC
    // #&14 with C=0 borrow → cost of 21). Underflow on the subtract also
    // explodes (BCC path). Tile collision physics already reflects the
    // velocity, so deducting timer here gives the bullet a couple of
    // ricochets before energy depletion finally detonates it.
    bool should_explode = false;
    bool damaged_door   = false;
    if (obj.tile_collision) {
        // Port-only: tile-collision-vs-door-substitute tile -> damage door
        // primary. Substituted obstruction sits above the primary AABB so
        // object-vs-object touching never fires; we resolve manually.
        int sprite_h = (obj.sprite <= 0x80)
                       ? sprite_atlas[obj.sprite].h : 1;
        int sprite_h_frac = (sprite_h > 0 ? sprite_h - 1 : 0) * 8;
        int feet_abs_y = static_cast<int>(obj.y.whole) * 256 +
                         static_cast<int>(obj.y.fraction) + sprite_h_frac;
        for (int dy = 0; dy <= 1; dy++) {
            uint8_t probe_ty = static_cast<uint8_t>(
                ((feet_abs_y >> 8) + dy) & 0xff);
            ResolvedTile r = resolve_tile_with_tertiary(
                ctx.landscape, obj.x.whole, probe_ty);
            uint8_t type = r.tile_and_flip & TileFlip::TYPE_MASK;
            bool is_door =
                type == static_cast<uint8_t>(TileType::METAL_DOOR) ||
                type == static_cast<uint8_t>(TileType::STONE_DOOR);
            if (!is_door) continue;
            int door_slot = -1;
            for (int j = 1; j < GameConstants::PRIMARY_OBJECT_SLOTS; j++) {
                Object& cand = ctx.mgr.object(j);
                if (!cand.is_active()) continue;
                if (cand.tertiary_slot !=
                    static_cast<uint16_t>(r.data_offset)) continue;
                door_slot = j;
                break;
            }
            if (door_slot < 0) continue;
            Object& door = ctx.mgr.object(door_slot);
            uint16_t hurt = std::min<uint16_t>(damage, door.energy);
            if (door.energy > damage) door.energy -= damage;
            else                       door.energy = 0;
            door.flags |= ObjectFlags::WAS_DAMAGED;
            if (ctx.damage_events) {
                DamageVisual ev;
                ev.src_x = obj.x.whole; ev.src_y = obj.y.whole;
                ev.src_x_frac = obj.x.fraction; ev.src_y_frac = obj.y.fraction;
                ev.tgt_x = door.x.whole; ev.tgt_y = door.y.whole;
                ev.tgt_x_frac = door.x.fraction; ev.tgt_y_frac = door.y.fraction;
                ev.tgt_slot = static_cast<int8_t>(door_slot);
                ev.amount = hurt;
                ctx.damage_events->push_back(ev);
            }
            damaged_door = true;
            break;
        }

        if (damaged_door) {
            // Door-substitute hit consumes the bullet immediately, same
            // as a damageable-target touch above.
            should_explode = true;
        } else if (obj.timer >= 0x3e) {
            // &443d CMP #&3e / BCS explode_bullet.
            should_explode = true;
        } else {
            // &4441 SBC #&14 with carry clear (BCS not taken => C=0), so
            // the effective subtraction is 0x14 + 1 = 0x15.
            int new_timer = static_cast<int>(obj.timer) - 0x15;
            if (new_timer < 0) {
                should_explode = true;  // &4443 BCC explode_bullet
            } else {
                obj.timer = static_cast<uint8_t>(new_timer);
            }
        }
    }

    if (should_explode) {
        explode_bullet(obj);
        return;
    }

    // &4447-&4460: orient bullet to its current velocity.
    uint8_t angle = calculate_angle_from_velocities(obj.velocity_x, obj.velocity_y);
    orient_bullet_to_angle(obj, angle);
}

// &42F7 update_active_grenade. Destroyed (energy==0) -> duration 10;
// fuse expiry (timer==0x60) -> duration 16; else tick palette through
// &4dd4 table and play tick sound. Firing while holding (player_object_
// fired == this slot, &0bbf) demotes back to INACTIVE_GRENADE so the
// player can re-pocket / disarm without exploding.
void update_active_grenade(Object& obj, UpdateContext& ctx) {
    // &42f7-&4302: check_if_object_fired -> change_object_type back to
    // OBJECT_INACTIVE_GRENADE with a fresh palette/sprite, fuse reset.
    if (static_cast<uint8_t>(ctx.this_slot) == ctx.player_object_fired) {
        obj.timer  = 0;
        obj.type   = ObjectType::INACTIVE_GRENADE;
        uint8_t idx = static_cast<uint8_t>(ObjectType::INACTIVE_GRENADE);
        obj.sprite  = object_types_sprite[idx];
        obj.palette = object_types_palette_and_pickup[idx] & 0x7f;
        return;
    }

    // &4305-&4309: destroyed -> quick explosion (duration 10).
    if (obj.energy == 0) {
        ctx.mgr.log_diag(
            "grenade p%d DESTROYED (fuse cut) timer=%u -> duration 10",
            ctx.this_slot, static_cast<unsigned>(obj.timer));
        explode_object_with_duration(obj, 0x0a);
        // Immediately seed the explosion with its first burst of
        // particles so the transition frame isn't silent.
        if (ctx.particles) {
            ctx.particles->emit(ParticleType::EXPLOSION, 8, obj, ctx.cosmetic_rng);
        }
        return;
    }

    // &430d-&4311: fuse expired -> full explosion (duration 16).
    if (obj.timer >= 0x60) {
        ctx.mgr.log_diag(
            "grenade p%d FUSE_EXPIRED timer=%u -> duration 16",
            ctx.this_slot, static_cast<unsigned>(obj.timer));
        explode_object_with_duration(obj, 0x10);
        if (ctx.particles) {
            ctx.particles->emit(ParticleType::EXPLOSION, 10, obj, ctx.cosmetic_rng);
        }
        return;
    }

    // &4316: advance fuse.
    obj.timer++;

    // &4318: palette cycle through the 4-colour table.
    uint8_t idx = rotate_colour_from_A(obj, obj.timer);

    // &431b-&4321: every 16 frames (when rotate_colour_from_A returns
    // X=0), emit the grenade's "tick … tick" countdown beep. The
    // 1-in-16 gate keeps the chirp at ~3 Hz, slow enough to count.
    if (idx == 0) {
        static constexpr uint8_t kSoundGrenadeTick[4] = { 0x57, 0x07, 0xcb, 0x82 };
        Audio::play_at(Audio::CH_ANY, kSoundGrenadeTick, obj.x.whole, obj.y.whole);
    }
}

// &46d9-&46e9 create_projectile_particle_trail. The 6502 looks the
// per-bullet cf byte up at &46ec (indexed by object type - &13) and
// writes it into particle_types_colour_and_flags_table + &2c (the
// PROJECTILE_TRAIL cf_base) before calling add_particle. We pass the
// byte through cf_base_override.
//   &46ec 02 ICER       : colour 2 / 3   (green / yellow)
//   &46ed 04 TRACER     : colour 4 / 5   (blue / magenta)
//   &46ee 08 CANNONBALL : 0x08 cycle bit + colour 0 / 1
//   &46ef 08 BLUE_DEATH : 0x08 cycle bit + colour 0 / 1
// RED_BULLET (&17) reads one byte past the table — the next byte is
// 0x20 (the JSR opcode at &46f0). With PROJECTILE_TRAIL's cf_rand=0x01
// the per-particle byte alternates between 0x20 (FOREGROUND + colour 0
// = invisible) and 0x21 (FOREGROUND + colour 1 = red). Net effect: a
// sparse red trail plotted in front of foliage. Same accidental
// behaviour as the 6502.
static uint8_t projectile_trail_cf(ObjectType type) {
    switch (type) {
        case ObjectType::ICER_BULLET:     return 0x02;
        case ObjectType::TRACER_BULLET:   return 0x04;
        case ObjectType::CANNONBALL:      return 0x08;
        case ObjectType::BLUE_DEATH_BALL: return 0x08;
        case ObjectType::RED_BULLET:      return 0x20;
        default:                          return 0x82; // type stock cf_base
    }
}
static void emit_projectile_trail(Object& obj, UpdateContext& ctx) {
    if (!ctx.particles) return;
    // &46d9-&46dd: trail leaves the rear of the bullet — start from the
    // bullet's motion angle (&b5 set by the &4447 orient step) then EOR
    // #&80. A pistol bullet moving right (angle 0x00) emits particles at
    // angle 0x80 (drifting left, behind the bullet).
    uint8_t bullet_angle = calculate_angle_from_velocities(
        obj.velocity_x, obj.velocity_y);
    uint8_t trail_angle = static_cast<uint8_t>(bullet_angle ^ 0x80);
    ctx.particles->emit(ParticleType::PROJECTILE_TRAIL, 1, obj, ctx.cosmetic_rng,
                        trail_angle, projectile_trail_cf(obj.type));
}

// &46BF: Icer bullet - freezes on contact, 2 frame explosion
void update_icer_bullet(Object& obj, UpdateContext& ctx) {
    common_bullet_update(obj, ctx, 20);
    // Icer has longer range - reset timer if still moving
    if (obj.velocity_x != 0 || obj.velocity_y != 0) {
        if (obj.timer < 2) obj.timer = 2;
    }
    emit_projectile_trail(obj, ctx);
}

// &4614 Tracer bullet. Never expires (pre-increment timer to cancel
// move_bullet's decrement), homes on player, explodes for 8/damage 15.
void update_tracer_bullet(Object& obj, UpdateContext& ctx) {
    // &4614 increase_energy_by_one — but timer is the equivalent here.
    if (obj.timer < 0xff) obj.timer++;

    common_bullet_update(obj, ctx, 15);

    // &467a-&4683 consider_moving_towards_player. 6502 chain:
    //   &467a JSR &3d26 consider_updating_npc_path -> NPC::update_npc_path
    //   &467d LDY #&08          maximum acceleration
    //   &467f LDA #&40          magnitude
    //   &4681 LDX #&40          1-in-4 probability gate (rnd < 0x40)
    //   &4683 JSR &31da move_towards_target_with_probability_X
    //   &4686 DEC accel_y       cancel gravity (handled below)
    // Tracers home on slot 0 (player); seed the target slot before the
    // path/move calls. The old per-frame ±1 nudge is replaced with the
    // 6502's every-4-frame +8 with RNG gate, which gives the slower,
    // more arc-like homing path the original is known for.
    obj.target_and_flags =
        (obj.target_and_flags & ~TargetFlags::OBJECT_MASK) | 0x00;
    NPC::update_npc_path(obj, ctx);
    NPC::move_towards_target_with_probability(obj, ctx,
                                              /*magnitude=*/0x40,
                                              /*max_accel=*/0x08,
                                              /*prob_threshold=*/0x40);

    // &4686 DEC accel_y. Tracers are flying enemies (no gravity).
    NPC::cancel_gravity(obj);

    // The 6502 doesn't re-orient after the homing nudge — move_bullet's
    // &4447 orient at the top of next frame's update reads the new
    // velocity, so the sprite is at most one frame stale.

    emit_projectile_trail(obj, ctx);
}

// &4332 update_blue_death_ball: damageable touch -> explode dur 16;
// tile collision -> explode dur 16; lifespan timer 0 -> explode dur 0;
// else face velocity + trail particles. The damage itself comes from
// the radial explosion, not the per-touch hit.
void update_blue_death_ball(Object& obj, UpdateContext& ctx) {
    int tgt = bullet_touching_damageable(obj, ctx.mgr);
    if (tgt >= 0)        { explode_object_with_duration(obj, 0x10); return; }
    if (obj.tile_collision) { explode_object_with_duration(obj, 0x10); return; }
    if (obj.timer > 0) obj.timer--;
    if (obj.timer == 0)  { explode_object_with_duration(obj, 0);    return; }
    emit_projectile_trail(obj, ctx);
}

// &4326 update_cannonball: cancel gravity, deal 170 damage on touch,
// then fall through to update_blue_death_ball for the explosion +
// tile-collision + lifespan-timer plumbing.
void update_cannonball(Object& obj, UpdateContext& ctx) {
    NPC::cancel_gravity(obj);
    int tgt = bullet_touching_damageable(obj, ctx.mgr);
    if (tgt >= 0) {
        Object& target = ctx.mgr.object(tgt);
        uint16_t hurt = std::min<uint16_t>(170, target.energy);
        if (target.energy > 170) target.energy -= 170;
        else                     target.energy = 0;
        target.flags |= ObjectFlags::WAS_DAMAGED;
        if (ctx.damage_events) {
            DamageVisual ev;
            ev.src_x = obj.x.whole; ev.src_y = obj.y.whole;
            ev.src_x_frac = obj.x.fraction; ev.src_y_frac = obj.y.fraction;
            ev.tgt_x = target.x.whole; ev.tgt_y = target.y.whole;
            ev.tgt_x_frac = target.x.fraction; ev.tgt_y_frac = target.y.fraction;
            ev.tgt_slot = static_cast<int8_t>(tgt);
            ev.amount = hurt;
            ctx.damage_events->push_back(ev);
        }
    }
    update_blue_death_ball(obj, ctx);
}

// Port of &434a update_red_bullet:
//   &434a LDA #&06 ; explosion duration
//   &434c LDX #&1e ; damage (30)
//   &434e JMP &461b update_bullet_with_particle_trail_and_consider_moving_towards_player
void update_red_bullet(Object& obj, UpdateContext& ctx) {
    common_bullet_update(obj, ctx, 30);
    emit_projectile_trail(obj, ctx);
}

// &441B: Pistol bullet - standard. The 6502 update tail at &4447-&4460
// is just calculate_angle / orient — no JMP &46d9, so the pistol has
// no trail particles in the original. common_bullet_update already
// covers the orient step for us.
void update_pistol_bullet(Object& obj, UpdateContext& ctx) {
    common_bullet_update(obj, ctx, 10);
}

// &4A88 update_plasma_ball. Contact -> duration-13 fireball, except for
// EXPLOSION / BUSH / FIREBALL (&1fd0 gate). Underwater: 1-in-4 random
// removal. Trail = 3 particles while energy >= 3, ramping to a 30-
// particle burst on the last two frames and at removal (&4aa7).
void update_plasma_ball(Object& obj, UpdateContext& ctx) {
    // &4a88-&4a90 turn-touched-object-into-fireball, gated by &1fd0
    // check_if_object_collides_with_plasma_ball: explosion / bush /
    // fireball are pass-throughs. Slot 0 (player) is excluded here as
    // a port-only safety so fireballs spawn at the target, not the player.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS && obj.touching != 0) {
        const Object& touched = ctx.mgr.object(obj.touching);
        bool pass_through =
            touched.type == ObjectType::EXPLOSION ||
            touched.type == ObjectType::BUSH      ||
            touched.type == ObjectType::FIREBALL;
        if (!pass_through) {
            obj.type   = ObjectType::FIREBALL;
            obj.timer  = 0x0d;
            obj.energy = 0x0d;
            // &4ac1 STA &0e: clear this_object_target_object to mark the
            // fireball as "from exploding object". That's the low 5 bits
            // of target_and_flags (&0906), not state (&0976).
            obj.target_and_flags &= ~TargetFlags::OBJECT_MASK;
            return;
        }
    }

    // &4a92-&4a98: 1-in-4 random removal while fully underwater.
    //   LDA in_water / ORA rnd_state / ORA rnd_state+3 / BPL remove
    // Uses the actual per-column waterline, not npc_helpers::is_underwater,
    // which compares against SURFACE_Y (upper-world ceiling 0x4f).
    if (Water::is_underwater(ctx.landscape, obj.x.whole, obj.y.whole)) {
        uint8_t r = ctx.rng.next() | ctx.rng.next();
        if (!(r & 0x80)) {
            // &4ac8 set_object_for_removal after the 30-particle burst -
            // setting energy=0 would re-enter step 12 and mutate into an
            // EXPLOSION instead of quietly fizzling.
            if (ctx.particles) ctx.particles->emit(ParticleType::PLASMA, 30, obj, ctx.cosmetic_rng);
            obj.flags |= ObjectFlags::PENDING_REMOVAL;
            return;
        }
    }

    // &4a9a: reduce_energy_by_one. If it reaches 0 -> remove (explode).
    if (obj.energy > 0) obj.energy--;
    if (obj.energy == 0) {
        // &4ac8 remove_plasma_ball_or_fireball jumps to the &4aa7
        // 30-particle "death" burst (flag 0xa1 = inherit object velocity).
        if (ctx.particles) ctx.particles->emit(ParticleType::PLASMA, 30, obj, ctx.cosmetic_rng);
        return;
    }

    // &4a9f-&4aab plasma trail particle count: 3 while energy >= 3,
    // ramps to 30 on the last two frames so the bolt blooms before it
    // fizzles. Flag bit 0 (&a0 vs &a1) toggles "inherit object velocity"
    // — our particle system always inherits, so we ignore it here.
    if (ctx.particles) {
        uint8_t count = (obj.energy >= 3) ? 3 : 30;
        ctx.particles->emit(ParticleType::PLASMA, count, obj, ctx.cosmetic_rng);
    }
}

// &4101-&4154 lightning. state = signed size in [-4,+4] (pos grow, neg
// shrink -> remove at 0); timer counts down, at -25 (&412b CMP #&e7)
// flips growth to shrink.
void update_lightning(Object& obj, UpdateContext& ctx) {
    // &1a35-&1a41 captures this_object_previous_velocity at frame start
    // (before any acceleration is applied this frame). update_lightning
    // runs before Physics::apply_acceleration in our port, so these are
    // the same value — save locally to restore at the end.
    int8_t prev_vx = obj.velocity_x;
    int8_t prev_vy = obj.velocity_y;

    bool touching = (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS);
    bool damaged_something = false;

    // &4101-&4110: damage the touching object (except ACTIVE_CHATTER).
    if (touching) {
        Object& target = ctx.mgr.object(obj.touching);
        if (target.type != ObjectType::ACTIVE_CHATTER) {
            uint16_t hurt = std::min<uint16_t>(80, target.energy);
            if (target.energy > 80) target.energy -= 80;
            else                    target.energy = 0;
            if (ctx.damage_events) {
                DamageVisual ev;
                ev.src_x = obj.x.whole; ev.src_y = obj.y.whole;
                ev.src_x_frac = obj.x.fraction; ev.src_y_frac = obj.y.fraction;
                ev.tgt_x = target.x.whole; ev.tgt_y = target.y.whole;
                ev.tgt_x_frac = target.x.fraction; ev.tgt_y_frac = target.y.fraction;
                ev.tgt_slot = static_cast<int8_t>(obj.touching);
                ev.amount = hurt;
                ctx.damage_events->push_back(ev);
            }
            damaged_something = true;
        }
    }

    int8_t size = static_cast<int8_t>(obj.state);

    // &4114-&411c: decide whether this counts as "collided with something".
    // Original ORs in the tile-top/bottom collision bit (&1b); we don't track
    // that yet, so a collision is just "touching an object or already
    // shrinking (size<0)".
    bool collided = damaged_something || size < 0;

    // &411e-&4125: if collided, flip sign so we start/continue shrinking.
    // invert_if_positive(size) = -size when size>=0; if the result is 0
    // force to -2 (&4125 LDX #&fe).
    if (collided) {
        if (size > 0) size = static_cast<int8_t>(-size);
        if (size == 0) size = -2;
    }

    // &4127-&4130: timer goes from 0 to more-negative each frame. At -25
    // (0xe7) swap to shrinking by setting size negative; otherwise INX.
    obj.timer = static_cast<uint8_t>(static_cast<int8_t>(obj.timer) - 1);
    if (static_cast<int8_t>(obj.timer) == -25) {
        size = static_cast<int8_t>(-std::abs(static_cast<int>(size)));
        if (size == 0) size = -1;
    } else {
        size = static_cast<int8_t>(size + 1);
        if (size == 0) {
            // &4130 BEQ -> remove lightning at zero size.
            obj.energy = 0;
            return;
        }
    }

    // &4133-&4138: keep_within_range Y=4 -> clamp to [-4, +4].
    if (size >  4) size =  4;
    if (size < -4) size = -4;
    obj.state = static_cast<uint8_t>(size);

    // &413a-&413f: sprite = |size| + (SPRITE_LIGHTNING_QUARTER-1) = +0x6c.
    // For |size| in [1..4] this gives 0x6d..0x70 (QUARTER..NEST).
    obj.sprite = static_cast<uint8_t>(std::abs(static_cast<int>(size)) + 0x6c);

    // &4142-&4148: v-flip every frame (bit 0 of frame_counter), h-flip
    // every two frames (bit 1).
    if (ctx.frame_counter & 0x01)
        obj.flags ^= ObjectFlags::FLIP_VERTICAL;
    if ((ctx.frame_counter & 0x03) == 0)
        obj.flags ^= ObjectFlags::FLIP_HORIZONTAL;

    // &414a-&4152: DEC acceleration_y (cancel gravity), then velocity =
    // previous_velocity. After update_lightning returns, our physics
    // adds +1 gravity to vy and, once every 16 frames, decays |v| by 1
    // (apply_acceleration's inertia tick). Pre-compensate both so the
    // post-physics velocity equals the frame-start prev_velocity.
    int target_vx = prev_vx;
    int target_vy = prev_vy - 1;  // physics will re-add +1 gravity
    if (ctx.every_sixteen_frames) {
        if      (prev_vy > 0) target_vy += 1;
        else if (prev_vy < 0) target_vy -= 1;
        if      (prev_vx > 0) target_vx += 1;
        else if (prev_vx < 0) target_vx -= 1;
    }
    if (target_vx >  127) target_vx =  127;
    if (target_vx < -128) target_vx = -128;
    if (target_vy >  127) target_vy =  127;
    if (target_vy < -128) target_vy = -128;
    obj.velocity_x = static_cast<int8_t>(target_vx);
    obj.velocity_y = static_cast<int8_t>(target_vy);
}

// &4698 mushroom balls. Fireball touch -> coronium crystal. Touch or
// timer-end -> 1-in-2 explode and add 0x3f to red/blue mushroom timer
// (palette parity selects which).
void update_red_mushroom_ball(Object& obj, UpdateContext& ctx) {
    // &4698-&46a3: touching a fireball -> convert to coronium crystal.
    bool touching = (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS);
    if (touching) {
        Object& touched = ctx.mgr.object(obj.touching);
        if (touched.type == ObjectType::FIREBALL) {
            obj.type  = ObjectType::CORONIUM_CRYSTAL;
            obj.timer = 0;
            return;
        }
    }

    // &46a6-&46a9: not touching -> reduce energy; return if still alive.
    if (!touching) {
        if (obj.energy > 0) obj.energy--;
        if (obj.energy != 0) return;
    }

    // &46ab-&46ad: 1-in-2 chance of exploding now.
    if (ctx.rng.next() & 0x80) return;

    // &46af-&46b2: palette LSR -> low bit selects red (0) or blue (1) timer.
    // The LSR's carry-out is the low bit of palette; passed as the +1 extra
    // to add_to_player_mushroom_timer (via ADC #&00 in play_sound_for_mushrooms).
    bool blue = (obj.palette & 0x01) != 0;
    if (obj.touching == 0) {
        add_to_player_mushroom_timer(ctx, blue ? 1 : 0, blue);
    }

    // &46b2 play_sound_for_mushrooms tail-calls &3f7f set_new_particles_
    // position_from_this_object, which shifts the ball's x/y fraction by
    // -0x40 (quarter-tile up-left) and emits ONE STAR_OR_MUSHROOM via
    // add_particle. Then &46b5 adds 32 more via add_particles — STAR_OR_
    // MUSHROOM's particle_flags=0x00 means it does NOT re-copy this_object
    // position, so the 32 inherit the same -0x40 base. Spawn a -0x40-
    // offset shim for the burst so it matches the 6502 cluster.
    if (ctx.particles) {
        Object spawn = obj;
        int yf = int(spawn.y.fraction) - 0x40;
        if (yf < 0) { yf += 256; spawn.y.whole--; }
        spawn.y.fraction = static_cast<uint8_t>(yf);
        int xf = int(spawn.x.fraction) - 0x40;
        if (xf < 0) { xf += 256; spawn.x.whole--; }
        spawn.x.fraction = static_cast<uint8_t>(xf);
        ctx.particles->emit(ParticleType::STAR_OR_MUSHROOM, 33, spawn, ctx.cosmetic_rng);
    }
    // &46bc JMP set_object_for_removal (&2529) — sets PENDING_REMOVAL.
    obj.flags |= ObjectFlags::PENDING_REMOVAL;
}

// Port of &4791 update_invisible_debris:
//   &4791 JSR &251f reduce_energy_by_one     ; limited lifespan
//   &4794 BNE &47c2 ; leave
//   &4796 JMP &2529 set_object_for_removal
// Port uses an additive timer instead of the 6502's decrementing
// energy, but the lifespan (~64 frames) matches.
void update_invisible_debris(Object& obj, UpdateContext& ctx) {
    obj.timer++;
    // &4796 JMP set_object_for_removal: quiet removal at lifespan end.
    // energy=0 would re-enter step 12 and mutate the slot into an
    // EXPLOSION with default duration + particles.
    if (obj.timer >= 64) obj.flags |= ObjectFlags::PENDING_REMOVAL;
}

// &4799 update_red_drop. Explodes on tile or non-slime object contact.
// 6502 lets gravity ramp the drop from spawn vy=4 toward the &40 cap;
// our port's INTANGIBLE-gravity-exempt skips that, so re-add the +1/
// frame ramp manually. Without it the drop crawls one tile and dies.
void update_red_drop(Object& obj, UpdateContext& ctx) {
    bool should_explode = false;
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& target = ctx.mgr.object(obj.touching);
        ObjectType tt = target.type;
        // &479e-&47a0: drop ignores its parent slime — fall-through.
        if (tt != ObjectType::RED_SLIME) {
            // &47a2-&47c8: yellow slime -> coronium boulder, and RTS
            // (drop survives the conversion, no explosion).
            if (tt == ObjectType::YELLOW_SLIME) {
                target.type = ObjectType::CORONIUM_BOULDER;
                return;
            } else if (tt != ObjectType::PIRANHA) {
                // &47aa-&47ad: damage sound (channel zero, bullet pop).
                static constexpr uint8_t kSoundBulletPop[4] = {
                    0x17, 0x03, 0x1b, 0x02 };
                Audio::play_at(Audio::CH_PRIORITY, kSoundBulletPop,
                               obj.x.whole, obj.y.whole);
                // &47ab: 100 damage to anyone else (incl. player).
                uint16_t hurt = std::min<uint16_t>(100, target.energy);
                if (target.energy > 100) target.energy -= 100;
                else                     target.energy = 0;
                target.flags |= ObjectFlags::WAS_DAMAGED;
                if (ctx.damage_events) {
                    DamageVisual ev;
                    ev.src_x = obj.x.whole; ev.src_y = obj.y.whole;
                    ev.src_x_frac = obj.x.fraction; ev.src_y_frac = obj.y.fraction;
                    ev.tgt_x = target.x.whole; ev.tgt_y = target.y.whole;
                    ev.tgt_x_frac = target.x.fraction; ev.tgt_y_frac = target.y.fraction;
                    ev.tgt_slot = static_cast<int8_t>(obj.touching);
                    ev.amount = hurt;
                    ctx.damage_events->push_back(ev);
                }
            }
            should_explode = true;
        }
    } else if (obj.tile_collision) {
        // Port-only door-tile redirect (same as common_bullet_update).
        // Substituted obstruction catches drop above the primary AABB
        // -> resolve the 100-damage hit manually.
        int sprite_h = (obj.sprite <= 0x80)
                       ? sprite_atlas[obj.sprite].h : 1;
        int sprite_h_frac = (sprite_h > 0 ? sprite_h - 1 : 0) * 8;
        int feet_abs_y = static_cast<int>(obj.y.whole) * 256 +
                         static_cast<int>(obj.y.fraction) + sprite_h_frac;
        for (int dy = 0; dy <= 1; dy++) {
            uint8_t probe_ty = static_cast<uint8_t>(
                ((feet_abs_y >> 8) + dy) & 0xff);
            ResolvedTile r = resolve_tile_with_tertiary(
                ctx.landscape, obj.x.whole, probe_ty);
            uint8_t type = r.tile_and_flip & TileFlip::TYPE_MASK;
            bool is_door =
                type == static_cast<uint8_t>(TileType::METAL_DOOR) ||
                type == static_cast<uint8_t>(TileType::STONE_DOOR);
            if (!is_door) continue;
            int door_slot = -1;
            for (int j = 1; j < GameConstants::PRIMARY_OBJECT_SLOTS; j++) {
                Object& cand = ctx.mgr.object(j);
                if (!cand.is_active()) continue;
                if (cand.tertiary_slot !=
                    static_cast<uint16_t>(r.data_offset)) continue;
                door_slot = j;
                break;
            }
            if (door_slot < 0) continue;
            Object& door = ctx.mgr.object(door_slot);
            uint16_t hurt = std::min<uint16_t>(100, door.energy);
            if (door.energy > 100) door.energy -= 100;
            else                    door.energy = 0;
            door.flags |= ObjectFlags::WAS_DAMAGED;
            if (ctx.damage_events) {
                DamageVisual ev;
                ev.src_x = obj.x.whole; ev.src_y = obj.y.whole;
                ev.src_x_frac = obj.x.fraction; ev.src_y_frac = obj.y.fraction;
                ev.tgt_x = door.x.whole; ev.tgt_y = door.y.whole;
                ev.tgt_x_frac = door.x.fraction; ev.tgt_y_frac = door.y.fraction;
                ev.tgt_slot = static_cast<int8_t>(door_slot);
                ev.amount = hurt;
                ctx.damage_events->push_back(ev);
            }
            break;
        }
        should_explode = true;
    }
    // &47b6-&47bb explode_red_drop uses duration_A_but_no_sound A=0.
    // Energy=0 path would fall through to duration 4 in step 12.
    if (should_explode) {
        // &47b6 JSR play_high_beep — high-pitched chirp on red drop
        // popping. The "_but_no_sound" explosion path means this is
        // the only sound the player hears for a red-drop impact.
        static constexpr uint8_t kSoundHighBeep[4] = { 0x17, 0x82, 0x13, 0xf2 };
        Audio::play_at(Audio::CH_ANY, kSoundHighBeep,
                       obj.x.whole, obj.y.whole);
        explode_object_with_duration(obj, 0);
        return;
    }
    if (obj.velocity_y < 0x40) obj.velocity_y++;
}

// &4ae8 consider_fireball_damage_and_animate. Shared tail of FIREBALL
// (&4ad6) and MOVING_FIREBALL (&4b26): damage the touched slot, cycle the
// warm palette, random flip, emit one PARTICLE_FIREBALL. base_damage is
// 10/20 for fireball temp/perm, 4 for moving; bumps to 90 at timer>=8.
static void fireball_damage_and_animate(Object& obj, UpdateContext& ctx,
                                         uint8_t base_damage) {
    uint8_t damage = base_damage;
    if (ctx.every_sixteen_frames && obj.timer >= 0x08) {
        damage = 90;
    }

    // &4af4-&4b00 damage_object on the touched slot — any object, not
    // just the player. &4af9 fire-immunity gate: if touching player AND
    // device collected, zero out the damage.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& target = ctx.mgr.object(obj.touching);
        if (obj.touching == 0 &&
            ctx.fire_immunity_collected && *ctx.fire_immunity_collected) {
            damage = 0;
        }
        uint16_t hurt = std::min<uint16_t>(damage, target.energy);
        target.energy = (target.energy > damage)
                      ? static_cast<uint8_t>(target.energy - damage)
                      : 0;
        if (obj.touching != 0) target.flags |= ObjectFlags::WAS_DAMAGED;
        if (ctx.damage_events && hurt > 0) {
            DamageVisual ev;
            ev.src_x = obj.x.whole; ev.src_y = obj.y.whole;
            ev.src_x_frac = obj.x.fraction; ev.src_y_frac = obj.y.fraction;
            ev.tgt_x = target.x.whole; ev.tgt_y = target.y.whole;
            ev.tgt_x_frac = target.x.fraction; ev.tgt_y_frac = target.y.fraction;
            ev.tgt_slot = static_cast<int8_t>(obj.touching);
            ev.amount = static_cast<uint8_t>(hurt);
            ctx.damage_events->push_back(ev);
        }
    }

    // &4b13-&4b1b: palette indexed by (timer & 7) into fireball_palettes_
    // table: { kyR, rwY, rwY, rwY, kyR, rwY, kyR, rwY }. The flame stays
    // in the warm half of the palette as the timer ticks.
    static constexpr uint8_t kFireballPalettes[8] = {
        0x10, 0x34, 0x34, 0x34, 0x10, 0x34, 0x10, 0x34,
    };
    obj.palette = kFireballPalettes[obj.timer & 0x07];

    // &4b0b-&4b11: random h/v flip every frame.
    uint8_t r = ctx.rng.next();
    obj.flags = (obj.flags & ~(ObjectFlags::FLIP_HORIZONTAL |
                               ObjectFlags::FLIP_VERTICAL)) |
                (r & (ObjectFlags::FLIP_HORIZONTAL |
                      ObjectFlags::FLIP_VERTICAL));

    // &4b1d-&4b23: emit one PARTICLE_FIREBALL each frame. The 6502 sets
    // angle=&c0 (straight up) so the ember rises out of the flame.
    if (ctx.particles) {
        ctx.particles->emit(ParticleType::FIREBALL, 1, obj, ctx.cosmetic_rng);
    }
}

// &4AD6 update_fireball. Stationary fire: gate on permanent vs temporary,
// then run the shared damage/animate tail.
void update_fireball(Object& obj, UpdateContext& ctx) {
    // &4ad6-&4adc water-removal: when completely underwater, AND the
    // waterline-negative flag with two peeked rnd bytes (state[1] and
    // state[2]); if all three have bit 7 set, fizzle. ~1-in-4 per frame
    // submerged. Without this, fireballs persist forever in water.
    if (Water::is_underwater(ctx.landscape, obj.x.whole, obj.y.whole) &&
        (ctx.rng.peek(1) & ctx.rng.peek(2) & 0x80)) {
        if (ctx.particles) ctx.particles->emit(ParticleType::PLASMA, 30, obj, ctx.cosmetic_rng);
        obj.flags |= ObjectFlags::PENDING_REMOVAL;
        return;
    }

    // &4ade reads this_object_target_object: zero = temporary fireball
    // (from plasma_ball mutation), non-zero = permanent (from tertiary).
    // That's the low 5 bits of target_and_flags (&0906), NOT state
    // (&0976) — &1edf seeds it with the slot index on nest spawn.
    bool permanent = (obj.target_and_flags & TargetFlags::OBJECT_MASK) != 0;

    // &4ae2-&4ae4 update_temporary_fireball: DEC timer; BMI removal.
    if (!permanent) {
        if (obj.timer == 0) {
            obj.flags |= ObjectFlags::PENDING_REMOVAL;
            return;
        }
        obj.timer--;
    }

    // &4ae6 / &4b60: temporary=10, permanent=20. MOVING_FIREBALL uses 4
    // via the &4b26 entry and never lands here.
    fireball_damage_and_animate(obj, ctx, permanent ? 20 : 10);
}

// &4B26 update_moving_fireball. Enters consider_fireball_damage_and_
// animate with X=4 (skipping the &4ade permanent/temporary gate), then
// runs &4672 move_fireball — find/avoid targets, update path, nudge
// velocity toward the player 1-in-4 frames, cancel gravity.
void update_moving_fireball(Object& obj, UpdateContext& ctx) {
    fireball_damage_and_animate(obj, ctx, 4);

    // &4672 move_fireball. Same shape as update_red_tracer (also at
    // &46d9 -> &4672): seed target = player (slot 0) so the OBJECT_MASK
    // override here doesn't disturb the permanent-fireball marker the
    // 6502 stores in the same byte for OBJECT_FIREBALL.
    obj.target_and_flags =
        (obj.target_and_flags & ~TargetFlags::OBJECT_MASK) | 0x00;
    NPC::update_npc_path(obj, ctx);
    NPC::move_towards_target_with_probability(obj, ctx,
                                              /*magnitude=*/0x40,
                                              /*max_accel=*/0x08,
                                              /*prob_threshold=*/0x40);
    NPC::cancel_gravity(obj);
}

// &4F9C: Explosion - expanding damage area
void update_explosion(Object& obj, UpdateContext& ctx) {
    // 6502 &4fbf: AND #&13 — cycles through the 8 explosion palettes
    // {kyK,rgK,rmK,rcK,kyR,rgR,rmR,rcR}. &0f would mix in unrelated indices.
    obj.palette = ctx.rng.next() & 0x13;

    // 6502 &4fc3-&4fc7: emit 10 particles BEFORE the duration check, so
    // a duration-0 explosion (e.g. red drop at &47b9) still gets one
    // frame of particles before removal.
    if (ctx.particles)
        ctx.particles->emit(ParticleType::EXPLOSION, 10, obj, ctx.cosmetic_rng);

    // 6502 &4fca-&4fce: BEQ to_set_object_for_removal if duration==0.
    // The main update loop's PENDING_REMOVAL step reaps the slot next frame.
    if (obj.tertiary_data_offset == 0) {
        obj.flags |= ObjectFlags::PENDING_REMOVAL;
        return;
    }
    obj.tertiary_data_offset--;

    ctx.mgr.log_diag(
        "exp p%d tdo=%u pal=0x%02x sprite=0x%02x @%u,%u",
        ctx.this_slot,
        static_cast<unsigned>(obj.tertiary_data_offset),
        static_cast<unsigned>(obj.palette),
        static_cast<unsigned>(obj.sprite),
        obj.x.whole, obj.y.whole);

    apply_explosion_radius(ctx.mgr, obj, /*source_slot=*/-1,
                           obj.tertiary_data_offset, ctx.damage_events);
}

// &4fd8-&4fe2 + accelerate_all_objects (&343a-&34b0). power = duration*4,
// damages only when duration>=8. Per target: remaining =
// power - (weight*2+8) - distance; skip if <0; weight==7 means no push.
void apply_explosion_radius(ObjectManager& mgr, const Object& source,
                            int source_slot, uint8_t duration,
                            std::vector<DamageVisual>* damage_events) {
    uint8_t power   = static_cast<uint8_t>(duration << 2);
    bool    damages = duration >= 8;
    // Effective tile reach: max distance where remaining > 0 against a
    // weight-0 target = power - 8 sub-tile units (weight_factor floor).
    // Clamp at 0 if the explosion is too small to reach anything.
    uint8_t radius_tiles = (power > 8)
        ? static_cast<uint8_t>((power - 8 + 7) / 8) : 0;

    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        if (i == source_slot) continue;
        Object& other = mgr.object(i);
        if (!other.is_active()) continue;
        if (&other == &source) continue;

        // Fractional position deltas — full 16-bit precision so a target
        // sharing a tile with the source still gets a non-zero direction.
        int other_fx = int(other.x.whole) * 256 + int(other.x.fraction);
        int other_fy = int(other.y.whole) * 256 + int(other.y.fraction);
        int src_fx   = int(source.x.whole) * 256 + int(source.x.fraction);
        int src_fy   = int(source.y.whole) * 256 + int(source.y.fraction);
        int dfx = other_fx - src_fx;
        int dfy = other_fy - src_fy;

        int adx_tiles = std::abs(dfx) / 256;
        int ady_tiles = std::abs(dfy) / 256;
        int dist_units = std::max(adx_tiles, ady_tiles) * 8;

        uint8_t weight        = other.weight();
        bool    static_target = weight >= 7;
        int     weight_factor = weight * 2 + 8;

        int remaining = int(power) - weight_factor - dist_units;
        if (remaining <= 0) continue;

        if (damages && remaining >= 4) {
            uint16_t hurt = static_cast<uint16_t>(std::min(255, remaining * 2));
            uint16_t actual = std::min<uint16_t>(hurt, other.energy);
            other.energy = (other.energy > hurt) ? (other.energy - hurt) : 0;
            if (damage_events && actual > 0) {
                DamageVisual ev;
                ev.src_x = source.x.whole; ev.src_y = source.y.whole;
                ev.src_x_frac = source.x.fraction; ev.src_y_frac = source.y.fraction;
                ev.tgt_x = other.x.whole; ev.tgt_y = other.y.whole;
                ev.tgt_x_frac = other.x.fraction; ev.tgt_y_frac = other.y.fraction;
                ev.tgt_slot = static_cast<int8_t>(i);
                ev.amount = actual;
                ev.radius_tiles = radius_tiles;
                damage_events->push_back(ev);
            }
        }

        if (static_target) continue;

        int accel = remaining / 2;
        int abs_fx = std::abs(dfx);
        int abs_fy = std::abs(dfy);
        int max_abs = std::max(abs_fx, abs_fy);
        int push_x, push_y;
        if (max_abs == 0) {
            push_x = 0;
            push_y = -accel;
        } else {
            push_x = accel * dfx / max_abs;
            push_y = accel * dfy / max_abs;
        }

        int vx = int(other.velocity_x) + push_x;
        int vy = int(other.velocity_y) + push_y;
        if (vx >  127) vx =  127;
        if (vx < -128) vx = -128;
        if (vy >  127) vy =  127;
        if (vy < -128) vy = -128;
        other.velocity_x = static_cast<int8_t>(vx);
        other.velocity_y = static_cast<int8_t>(vy);
    }

    // Always push a radius marker so the overlay can outline the
    // effective area even on frames when no target was inside it.
    if (damage_events && damages && radius_tiles > 0) {
        DamageVisual ev;
        ev.src_x = source.x.whole; ev.src_y = source.y.whole;
        ev.src_x_frac = source.x.fraction; ev.src_y_frac = source.y.fraction;
        ev.tgt_slot = -1;
        ev.radius_tiles = radius_tiles;
        damage_events->push_back(ev);
    }
}

} // namespace Behaviors
