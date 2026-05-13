#include "behaviours/projectile.h"
#include "behaviours/mood.h"
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
    // get_absolute_vector_component (&2346): CMP #&7f sets carry iff value
    // is 0x00..0x7f (i.e. non-negative as the 6502 treats it). For &80..&ff
    // it negates via EOR #&ff / ADC #&01. Carry is rotated into
    // vector_signs after each component is processed.
    auto is_positive_6502 = [](int8_t v) -> bool {
        return static_cast<uint8_t>(v) <= 0x7f;
    };
    auto abs_6502 = [](int8_t v) -> uint8_t {
        uint8_t u = static_cast<uint8_t>(v);
        return (u <= 0x7f) ? u : static_cast<uint8_t>((~u) + 1);
    };

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
        // &4425-&4428 explode_bullet sound (SN76489 noise channel "ptack").
        static constexpr uint8_t kSoundBulletPop[4] = { 0x17, 0x03, 0x1b, 0x02 };
        Audio::play_at(Audio::CH_PRIORITY, kSoundBulletPop,
                       obj.x.whole, obj.y.whole);
        obj.energy = 0; // explode_bullet
        return;
    }

    // &4434: reduce_energy_by_one (lifespan).
    if (obj.timer > 0) obj.timer--;
    if (obj.timer == 0) {
        obj.energy = 0;
        return;
    }

    // &4439: tile collision -> explode. The 6502's SBC #&14 / distance check
    // isn't ported yet; we just explode on any solid-tile collision.
    if (obj.tile_collision) {
        // Port-only: tile-collision-vs-door-substitute tile -> damage door
        // primary. Substituted obstruction sits above the primary AABB so
        // object-vs-object touching never fires; we resolve manually.
        int sprite_h = (obj.sprite <= 0x80)
                       ? sprite_atlas[obj.sprite].h : 1;
        int sprite_h_frac = (sprite_h > 0 ? sprite_h - 1 : 0) * 8;
        int feet_abs_y = static_cast<int>(obj.y.whole) * 256 +
                         static_cast<int>(obj.y.fraction) + sprite_h_frac;
        // Probe the bullet's bottom-edge tile and one tile down — the
        // bullet might have just nicked into the door tile from above.
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
            // Find the primary owning this tertiary slot.
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
            break;
        }
        obj.energy = 0;
        return;
    }

    // &4447-&4460: orient bullet to its current velocity.
    uint8_t angle = calculate_angle_from_velocities(obj.velocity_x, obj.velocity_y);
    orient_bullet_to_angle(obj, angle);
}

// &42F7 update_active_grenade. Destroyed (energy==0) -> duration 10;
// fuse expiry (timer==0x60) -> duration 16; else tick palette through
// &4dd4 table and play tick sound. Player-cancel via &0bbf is TODO.
void update_active_grenade(Object& obj, UpdateContext& ctx) {
    // &4305-&4309: destroyed -> quick explosion (duration 10).
    if (obj.energy == 0) {
        ctx.mgr.log_diag(
            "grenade p%d DESTROYED (fuse cut) timer=%u -> duration 10",
            ctx.this_slot, static_cast<unsigned>(obj.timer));
        explode_object_with_duration(obj, 0x0a);
        // Immediately seed the explosion with its first burst of
        // particles so the transition frame isn't silent.
        if (ctx.particles) {
            ctx.particles->emit(ParticleType::EXPLOSION, 8, obj, ctx.rng);
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
            ctx.particles->emit(ParticleType::EXPLOSION, 10, obj, ctx.rng);
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

// &46d9-&46e9 trail emission inside update_bullet_with_particle_trail.
// Port simplification: skips the per-bullet colour override table at
// &46ec (icer/tracer/cannonball/blue death ball) — uses type default.
static void emit_projectile_trail(Object& obj, UpdateContext& ctx) {
    if (!ctx.particles) return;
    ctx.particles->emit(ParticleType::PROJECTILE_TRAIL, 1, obj, ctx.rng);
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
// Re-orient AFTER homing — our +1/frame nudge changes direction often
// enough that the 6502's one-frame staleness becomes visible.
void update_tracer_bullet(Object& obj, UpdateContext& ctx) {
    // &4614 increase_energy_by_one — but timer is the equivalent here.
    if (obj.timer < 0xff) obj.timer++;

    common_bullet_update(obj, ctx, 15);

    // &4686 DEC acceleration_y — tracers are flying enemies (no
    // gravity); without this the tracer can't rise toward a player above.
    NPC::cancel_gravity(obj);

    // &467a-&4683 consider_moving_towards_player: homing on player.
    const Object& player = ctx.mgr.player();
    int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
    int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
    if (dx > 0 && obj.velocity_x <  0x20) obj.velocity_x++;
    if (dx < 0 && obj.velocity_x > -0x20) obj.velocity_x--;
    if (dy > 0 && obj.velocity_y <  0x20) obj.velocity_y++;
    if (dy < 0 && obj.velocity_y > -0x20) obj.velocity_y--;

    // Port-only extra orient: cheaper sprite-sync than porting the
    // every-4-frame +8 homing and rng gate from
    // move_towards_target_with_probability.
    uint8_t angle = calculate_angle_from_velocities(obj.velocity_x,
                                                    obj.velocity_y);
    orient_bullet_to_angle(obj, angle);

    emit_projectile_trail(obj, ctx);
}

// &4326: Cannonball — 170 (&aa) damage, no gravity, no timer-based
// lifespan (unlike most projectiles). The 6502 routine falls through into
// blue-death-ball (&4332) which shares the expiry-on-tile-hit logic.
void update_cannonball(Object& obj, UpdateContext& ctx) {
    NPC::cancel_gravity(obj);
    // Damage on touch (any object, not just player)
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& target = ctx.mgr.object(obj.touching);
        uint16_t hurt = std::min<uint16_t>(170, target.energy);
        if (target.energy > 170) target.energy -= 170;
        else                     target.energy = 0;
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
        obj.energy = 0; // explode
    }
    emit_projectile_trail(obj, ctx);
}

// &4332: Blue death ball - dangerous projectile
void update_blue_death_ball(Object& obj, UpdateContext& ctx) {
    common_bullet_update(obj, ctx, 40);
    // Death ball has slight homing
    if (ctx.every_eight_frames) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        if (dx > 0 && obj.velocity_x < 0x10) obj.velocity_x++;
        if (dx < 0 && obj.velocity_x > -0x10) obj.velocity_x--;
    }
    emit_projectile_trail(obj, ctx);
}

// Port of &434a update_red_bullet:
//   &434a LDA #&06 ; explosion duration
//   &434c LDX #&1e ; damage (30)
//   &434e JMP &461b update_bullet_with_particle_trail_and_consider_moving_towards_player
void update_red_bullet(Object& obj, UpdateContext& ctx) {
    common_bullet_update(obj, ctx, 30);
    emit_projectile_trail(obj, ctx);
}

// &441B: Pistol bullet - standard
void update_pistol_bullet(Object& obj, UpdateContext& ctx) {
    common_bullet_update(obj, ctx, 10);
    emit_projectile_trail(obj, ctx);
}

// &4A88 update_plasma_ball. On contact -> fireball; underwater 1-in-4
// removal; energy=0 -> explode. Falls under main-loop gravity (6502
// doesn't touch acceleration_y here). TODO: &4aa7 particles, &4a8d
// fireball type-match.
void update_plasma_ball(Object& obj, UpdateContext& ctx) {
    // &4a88: if touching another object, turn that object into a duration-13
    // fireball (the plasma ball "becomes" the fireball in the same slot).
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS && obj.touching != 0) {
        obj.type   = ObjectType::FIREBALL;
        obj.timer  = 0x0d;
        obj.energy = 0x0d;
        obj.state  = 0; // zero target-object — "from exploding object"
        return;
    }

    // &4a92-&4a98: 1-in-4 random removal while fully underwater.
    //   LDA in_water / ORA rnd_state / ORA rnd_state+3 / BPL remove
    // Uses the actual per-column waterline, not npc_helpers::is_underwater,
    // which compares against SURFACE_Y (upper-world ceiling 0x4f).
    if (Water::is_underwater(ctx.landscape, obj.x.whole, obj.y.whole)) {
        uint8_t r = ctx.rng.next() | ctx.rng.next();
        if (!(r & 0x80)) {
            obj.energy = 0;
            return;
        }
    }

    // &4a9a: reduce_energy_by_one. If it reaches 0 -> remove (explode).
    if (obj.energy > 0) obj.energy--;
    if (obj.energy == 0) {
        // &4ac8 remove_plasma_ball_or_fireball adds a burst of particles.
        if (ctx.particles) ctx.particles->emit(ParticleType::PLASMA, 4, obj, ctx.rng);
        return;
    }

    // &4a9f+: trail particles while alive.
    if (ctx.particles) ctx.particles->emit(ParticleType::PLASMA, 1, obj, ctx.rng);
}

// &4101-&4154 lightning. state = signed size in [-4,+4] (pos grow, neg
// shrink -> remove at 0); timer counts down, at -25 (&412b CMP #&e7)
// flips growth to shrink.
void update_lightning(Object& obj, UpdateContext& ctx) {
    auto s8 = [](uint8_t u) { return static_cast<int8_t>(u); };

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

    int8_t size = s8(obj.state);

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
    obj.timer = static_cast<uint8_t>(s8(obj.timer) - 1);
    if (s8(obj.timer) == -25) {
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

    // &414a-&4152: no gravity; carry previous velocity forward so the
    // lightning holds its original motion vector instead of decaying.
    NPC::cancel_gravity(obj);
    // TODO: set velocity = previous_velocity once the "previous velocity"
    // fields are part of Object.
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

    // &46b5: 32 mushroom particles.
    if (ctx.particles)
        ctx.particles->emit(ParticleType::STAR_OR_MUSHROOM, 32, obj, ctx.rng);
    // &46bc: remove mushroom ball.
    obj.energy = 0;
}

// Port of &4791 update_invisible_debris:
//   &4791 JSR &251f reduce_energy_by_one     ; limited lifespan
//   &4794 BNE &47c2 ; leave
//   &4796 JMP &2529 set_object_for_removal
// Port uses an additive timer instead of the 6502's decrementing
// energy, but the lifespan (~64 frames) matches.
void update_invisible_debris(Object& obj, UpdateContext& ctx) {
    obj.timer++;
    if (obj.timer >= 64) obj.energy = 0;
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
            // &47a2-&47a4: yellow slime -> coronium boulder.
            if (tt == ObjectType::YELLOW_SLIME) {
                target.type = ObjectType::CORONIUM_BOULDER;
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

// &4AD6 update_fireball. Stationary fire: palette cycle, random flip,
// one upward PARTICLE_FIREBALL/frame (angle=0xc0) for the ember effect.
void update_fireball(Object& obj, UpdateContext& ctx) {
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 8, ctx.damage_events);

    if (ctx.every_four_frames) {
        obj.palette = (obj.palette & 0x70) | (ctx.rng.next() & 0x07);
    }

    // &4b0b-&4b11: random horizontal/vertical flip every frame — keeps
    // the flames looking chaotic.
    uint8_t r = ctx.rng.next();
    obj.flags = (obj.flags & ~(ObjectFlags::FLIP_HORIZONTAL |
                               ObjectFlags::FLIP_VERTICAL)) |
                (r & (ObjectFlags::FLIP_HORIZONTAL |
                      ObjectFlags::FLIP_VERTICAL));

    // &4b1d-&4b23: emit one PARTICLE_FIREBALL each frame. The 6502 sets
    // angle=&c0 (straight up) so the ember rises out of the flame.
    if (ctx.particles) {
        ctx.particles->emit(ParticleType::FIREBALL, 1, obj, ctx.rng);
    }
}

// &4B26: Moving fireball - fire that moves
void update_moving_fireball(Object& obj, UpdateContext& ctx) {
    update_fireball(obj, ctx);
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
        ctx.particles->emit(ParticleType::EXPLOSION, 10, obj, ctx.rng);

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
