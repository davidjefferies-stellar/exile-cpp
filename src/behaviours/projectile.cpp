#include "behaviours/projectile.h"
#include "behaviours/mood.h"
#include "audio/audio.h"
#include "core/types.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "rendering/sprite_atlas.h"
#include "world/water.h"
#include "particles/particle_system.h"
#include <algorithm>
#include <cstdlib>

namespace Behaviors {

// Port of &4dd4 rotate_colour_from_A. Drives palette through a 4-entry
// cycle based on the high bits of A. Returns the low bits (0-3) so callers
// can gate periodic events (sound, etc.) on them.
static uint8_t rotate_colour_from_A(Object& obj, uint8_t a) {
    static constexpr uint8_t PALETTE_TABLE[4] = { 0x52, 0x63, 0x35, 0x21 };
    uint8_t idx = (a >> 2) & 0x03;
    obj.palette = PALETTE_TABLE[idx];
    return idx;
}

// Port of explode_object_with_duration_A at &40db-&40ed.
//
// The 6502 mutates the current primary IN-PLACE into an EXPLOSION:
//   STA this_object_tertiary_data_offset  ; duration counter
//   LDA #&44 (OBJECT_EXPLOSION); STA this_object_type
//   LDA #&ce; STA explosion_timer          ; -50 = active for 50 frames
//
// The slot keeps its position, its velocity, its (unused from here)
// state, and every other field. update_explosion (ported to
// projectile.cpp) then ticks the duration counter down each frame,
// damages nearby objects, emits 2 PARTICLE_EXPLOSION per frame, and
// eventually flags itself PENDING_REMOVAL.
//
// We intentionally DON'T go through "set energy=0 → step-12 spawns a
// new explosion slot" in object_update.cpp, because that path allocates
// a separate primary and leaves the original one to be removed — a
// faithful port needs the mutation to happen in the original's slot so
// chain reactions and target bookkeeping line up. Sound playback (&13f8)
// is a TODO.
static void explode_object_with_duration(Object& obj, uint8_t duration) {
    // Remember the centre of the object BEFORE changing types. Our
    // renderer anchors sprites by their top-left in world coords, so if
    // we only swap type/sprite without touching x/y a grenade-sized
    // source (SPRITE_BALL, ~4x4) turns into an explosion-sized one
    // (SPRITE_EXPLOSION, much larger) rooted at the same top-left,
    // which drags the explosion down-and-right of where the fuse
    // visibly burned. The 6502's tile-granular display masks this; our
    // sub-tile fractions make it obvious.
    int old_w_frac = 0, old_h_frac = 0;
    if (obj.sprite <= 0x7c) {
        const SpriteAtlasEntry& e = sprite_atlas[obj.sprite];
        old_w_frac = (e.w > 0 ? (e.w - 1) : 0) * 16;
        old_h_frac = (e.h > 0 ? (e.h - 1) : 0) * 8;
    }

    obj.tertiary_data_offset = duration;
    obj.type    = ObjectType::EXPLOSION;
    obj.sprite  = object_types_sprite[
        static_cast<uint8_t>(ObjectType::EXPLOSION)];
    obj.palette = object_types_palette_and_pickup[
        static_cast<uint8_t>(ObjectType::EXPLOSION)] & 0x7f;
    obj.energy  = get_initial_energy(
        static_cast<uint8_t>(ObjectType::EXPLOSION));
    obj.timer   = 0;

    // Recenter on the old sprite's centre. Shift = (old_size − new_size)
    // / 2, applied to the position fraction + carry into whole.
    int new_w_frac = 0, new_h_frac = 0;
    if (obj.sprite <= 0x7c) {
        const SpriteAtlasEntry& e = sprite_atlas[obj.sprite];
        new_w_frac = (e.w > 0 ? (e.w - 1) : 0) * 16;
        new_h_frac = (e.h > 0 ? (e.h - 1) : 0) * 8;
    }
    auto shift_axis = [](uint8_t& whole, uint8_t& frac, int delta) {
        int sum = int(whole) * 0x100 + int(frac) + delta;
        sum &= 0xffff;
        whole = static_cast<uint8_t>((sum >> 8) & 0xff);
        frac  = static_cast<uint8_t>(sum & 0xff);
    };
    shift_axis(obj.x.whole, obj.x.fraction, (old_w_frac - new_w_frac) / 2);
    shift_axis(obj.y.whole, obj.y.fraction, (old_h_frac - new_h_frac) / 2);

    // &40e8: global explosion_timer is used to drive the screen flash;
    // we don't have that hooked up yet (TODO), so skip it here.
}

// Port of &4005 add_to_player_mushroom_timer. Adds 0x3f (+optional 1 from
// entry carry) to timers[which] unless it would overflow; does not yet
// honor the mushroom-immunity-pill gate or player_immobility_timers.
// Call with which=0 for red mushrooms, which=1 for blue.
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

// Port of &441b / &46bf bullet main body. Explodes if:
//   - touching a damageable object (damages it first)
//   - hit a solid tile (tile_collision was flagged during last physics step)
//   - lifespan timer hit zero
// Otherwise just face the direction of travel.
static void common_bullet_update(Object& obj, UpdateContext& ctx, uint8_t damage) {
    // &1faf: touching a damageable target?
    int tgt = bullet_touching_damageable(obj, ctx.mgr);
    if (tgt >= 0) {
        Object& target = ctx.mgr.object(tgt);
        if (target.energy > damage) target.energy -= damage;
        else                        target.energy = 0;
        obj.energy = 0; // explode_bullet
        return;
    }

    // &4434: reduce_energy_by_one (lifespan).
    if (obj.timer > 0) obj.timer--;
    if (obj.timer == 0) {
        obj.energy = 0;
        return;
    }

    // &4439: tile collision → explode. The 6502's SBC #&14 / distance check
    // isn't ported yet; we just explode on any solid-tile collision.
    if (obj.tile_collision) {
        obj.energy = 0;
        return;
    }

    // &4447-&4460: orient bullet to its current velocity.
    uint8_t angle = calculate_angle_from_velocities(obj.velocity_x, obj.velocity_y);
    orient_bullet_to_angle(obj, angle);
}

// &42F7 update_active_grenade. Port of the full detonation chain.
//
//   JSR check_if_object_fired (&0bbf) — if the player just threw this
//     exact slot, un-activate: timer=0, type = INACTIVE_GRENADE. This
//     lets the player cancel an accidental arm by re-pocketing the
//     grenade before the fuse burns down. TODO: requires
//     player_object_fired tracking; not yet wired.
//
//   Destroyed-mid-air path (&4305-&430d): if energy==0 (something
//     damaged the grenade to death), explode_object_with_duration 10 —
//     a short, weak explosion.
//
//   Fuse-expiry path (&430d-&4311): when timer hits 0x60 (96 frames),
//     explode_object_with_duration 16 — the full grenade bang.
//
//   Otherwise (&4316-&4325): increment timer, rotate palette through the
//     4-entry table at &4dd4, and play the "tick" sound once per 16
//     frames when the palette index wraps to 0.
//
// We mutate the primary in-place on explosion so chain reactions and
// bystander damage land on the original slot (see
// explode_object_with_duration above).
void update_active_grenade(Object& obj, UpdateContext& ctx) {
    // &4305-&4309: destroyed → quick explosion (duration 10).
    if (obj.energy == 0) {
        explode_object_with_duration(obj, 0x0a);
        // Immediately seed the explosion with its first burst of
        // particles so the transition frame isn't silent.
        if (ctx.particles) {
            ctx.particles->emit(ParticleType::EXPLOSION, 8, obj, ctx.rng);
        }
        return;
    }

    // &430d-&4311: fuse expired → full explosion (duration 16).
    if (obj.timer >= 0x60) {
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

// Port of the trail emission at &46d9-&46e9 inside update_bullet_with_
// particle_trail. The 6502 flips the bullet's angle by &80 (EOR) so the
// particle leaves the rear of the bullet, overrides the PARTICLE_
// PROJECTILE_TRAIL colour from a per-bullet table, and adds one
// particle. Our particle code doesn't yet honour per-emit colour
// overrides; we still emit the trail so the bullet has a visible
// streak, just with the type's default colour.
//
// Colour table from &46ec:
//   OBJECT_ICER_BULLET     (&13) → green or yellow (random 1 of 2)
//   OBJECT_TRACER_BULLET   (&14) → blue or magenta
//   OBJECT_CANNONBALL      (&15) → cycle all colours
//   OBJECT_BLUE_DEATH_BALL (&16) → cycle all colours
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

// &4614: Tracer bullet - follows target, regenerates energy
void update_tracer_bullet(Object& obj, UpdateContext& ctx) {
    common_bullet_update(obj, ctx, 15);
    // Tracer homes toward player
    const Object& player = ctx.mgr.player();
    int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
    int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
    if (dx > 0 && obj.velocity_x < 0x20) obj.velocity_x++;
    if (dx < 0 && obj.velocity_x > -0x20) obj.velocity_x--;
    if (dy > 0 && obj.velocity_y < 0x20) obj.velocity_y++;
    if (dy < 0 && obj.velocity_y > -0x20) obj.velocity_y--;
    // Tracer never expires (energy restored)
    if (obj.energy < 0x10) obj.energy++;
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
        if (target.energy > 170) target.energy -= 170;
        else                     target.energy = 0;
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

// &434A: Red bullet - medium damage
void update_red_bullet(Object& obj, UpdateContext& ctx) {
    common_bullet_update(obj, ctx, 30);
    emit_projectile_trail(obj, ctx);
}

// &441B: Pistol bullet - standard
void update_pistol_bullet(Object& obj, UpdateContext& ctx) {
    common_bullet_update(obj, ctx, 10);
    emit_projectile_trail(obj, ctx);
}

// &4A88: Plasma ball — on object contact, turn it into a short fireball.
// Otherwise, reduce energy each frame; while underwater, 1-in-4 random
// removal; when out of energy, explode. Falls under gravity like any
// weight-4 projectile — the 6502 doesn't touch acceleration_y in this
// routine, so the main loop's +1-per-frame gravity stands.
// TODO: particle emission (add_plasma_particles &4aa7), fireball conversion
// on object-type match (&4a8d-&4a90 needs collides_with_plasma_ball).
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
    // Needs the actual waterline, not npc_helpers::is_underwater — that
    // one compares against SURFACE_Y (upper-world ceiling, 0x4f) and
    // flags any y >= 0x4f as "underwater", which is the whole playfield.
    // Previous code here killed the plasma ball 3-in-4 frames on land.
    // Also: 6502 uses OR then BPL (kill when both high bits clear = 1/4);
    // previous code used AND and then !(r & 0x80), giving 3/4 kill rate.
    if (Water::is_underwater(ctx.landscape, obj.x.whole, obj.y.whole)) {
        uint8_t r = ctx.rng.next() | ctx.rng.next();
        if (!(r & 0x80)) {
            obj.energy = 0;
            return;
        }
    }

    // &4a9a: reduce_energy_by_one. If it reaches 0 → remove (explode).
    if (obj.energy > 0) obj.energy--;
    if (obj.energy == 0) {
        // &4ac8 remove_plasma_ball_or_fireball adds a burst of particles.
        if (ctx.particles) ctx.particles->emit(ParticleType::PLASMA, 4, obj, ctx.rng);
        return;
    }

    // &4a9f+: trail particles while alive.
    if (ctx.particles) ctx.particles->emit(ParticleType::PLASMA, 1, obj, ctx.rng);
}

// &4101-&4154: Lightning size state machine.
//   state  = signed size, range [-4, +4]. Positive = growing, negative =
//            shrinking (then removed at 0).
//   timer  = counts down from 0 into negatives; at -25 turns growth into
//            shrink (&412b CMP #&e7).
// Each frame: damages touching non-chatter object for 80; if collided or
// already shrinking, flip sign of size (or snap to -2 if we'd hit 0);
// grow/shrink by 1; clamp to [-4,+4]; set sprite from |size|; flip v each
// frame, flip h every 2; cancel gravity; carry previous velocity forward.
void update_lightning(Object& obj, UpdateContext& ctx) {
    auto s8 = [](uint8_t u) { return static_cast<int8_t>(u); };

    bool touching = (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS);
    bool damaged_something = false;

    // &4101-&4110: damage the touching object (except ACTIVE_CHATTER).
    if (touching) {
        Object& target = ctx.mgr.object(obj.touching);
        if (target.type != ObjectType::ACTIVE_CHATTER) {
            if (target.energy > 80) target.energy -= 80;
            else                    target.energy = 0;
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
            // &4130 BEQ → remove lightning at zero size.
            obj.energy = 0;
            return;
        }
    }

    // &4133-&4138: keep_within_range Y=4 → clamp to [-4, +4].
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

// &4698: Mushroom balls (red and blue) — on contact with a fireball convert
// to a coronium crystal. Otherwise reduce lifespan each frame; when touched
// (or at end of life) there is a 1-in-2 chance of exploding. The explosion
// adds 0x3f to the player's red or blue mushroom timer (selected by the
// parity of the palette byte) then removes the ball.
void update_red_mushroom_ball(Object& obj, UpdateContext& ctx) {
    // &4698-&46a3: touching a fireball → convert to coronium crystal.
    bool touching = (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS);
    if (touching) {
        Object& touched = ctx.mgr.object(obj.touching);
        if (touched.type == ObjectType::FIREBALL) {
            obj.type  = ObjectType::CORONIUM_CRYSTAL;
            obj.timer = 0;
            return;
        }
    }

    // &46a6-&46a9: not touching → reduce energy; return if still alive.
    if (!touching) {
        if (obj.energy > 0) obj.energy--;
        if (obj.energy != 0) return;
    }

    // &46ab-&46ad: 1-in-2 chance of exploding now.
    if (ctx.rng.next() & 0x80) return;

    // &46af-&46b2: palette LSR → low bit selects red (0) or blue (1) timer.
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

// &4791: Invisible debris - just floats
void update_invisible_debris(Object& obj, UpdateContext& ctx) {
    obj.timer++;
    if (obj.timer >= 64) obj.energy = 0;
}

// &4799: Red drop - falls
void update_red_drop(Object& obj, UpdateContext& ctx) {
    obj.timer++;
    if (obj.timer >= 128) obj.energy = 0;
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 5);
}

// &4AD6: Fireball — stationary fire. Port of update_fireball.
//
// The 6502 also animates the palette by cycling through a small table,
// flips the sprite randomly each frame, and — importantly — emits one
// PARTICLE_FIREBALL per frame with angle=0xc0 (upward). Without the
// particles the fire looks flat; our palette cycle was already in
// place but the rising-ember effect was missing.
void update_fireball(Object& obj, UpdateContext& ctx) {
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 8);

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
    // Duration stored in tertiary_data_offset. Matches &4fca-&4fce: if
    // the counter has hit zero the explosion is finished — flag for
    // removal (set_object_for_removal &2516 ORA #&20) and return. The
    // main update loop's PENDING_REMOVAL step reaps the slot next frame.
    if (obj.tertiary_data_offset == 0) {
        obj.flags |= ObjectFlags::PENDING_REMOVAL;
        return;
    }
    obj.tertiary_data_offset--;
    // Explosions are INTANGIBLE, so the main loop's physics gate already
    // skips the gravity tick for them — no explicit cancel_gravity needed.

    // Random palette for visual effect
    obj.palette = ctx.rng.next() & 0x0f;

    // Emit explosion particles each frame while burning.
    if (ctx.particles)
        ctx.particles->emit(ParticleType::EXPLOSION, 2, obj, ctx.rng);

    // Port of &4fd8-&4fe2 + accelerate_all_objects (&343a-&34b0).
    //
    //   LDA duration; CMP #&08; ROR &28    — damages targets when
    //     duration >= 8 (long explosions only). Cleared again at the
    //     end of accelerate_all_objects (&34a7 LSR), so it's effectively
    //     a per-frame flag re-evaluated from current duration.
    //   LDA duration; ASL; ASL; STA &35    — acceleration_power =
    //     duration * 4. 1 duration → 4 sub-tile units = 0.5 tile radius.
    //
    // accelerate_all_objects then iterates every slot (skipping self)
    // and computes, for each target:
    //   weight_factor = weight * 2 + 8          (heavier = less pushable)
    //   remaining     = power − weight_factor − distance
    //   if remaining < 0            → skip (too far / too heavy / static)
    //   if damages && remaining >=4 → damage_object(remaining * 2)
    //   if weight == 7              → skip push (static)
    //   push magnitude = remaining / 2, direction = source → target
    //   add push to target.velocity_x / velocity_y
    //
    // distance is measured in sub-tile units (1 tile = 8), using max of
    // the two axes (Chebyshev) which matches the 6502's LOS raycast.
    uint8_t duration = obj.tertiary_data_offset;
    uint8_t power    = static_cast<uint8_t>(duration << 2);
    bool    damages  = duration >= 8;

    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        Object& other = ctx.mgr.object(i);
        if (!other.is_active()) continue;
        if (&other == &obj) continue;    // don't push the explosion itself

        // Fractional position deltas. Using only `.whole` zeros the
        // direction vector when both objects share a tile (the most
        // impactful near-ground-zero case), so damage would land but
        // the target wouldn't move. Sub-tile precision matches the
        // 6502's calculate_angle_from_vector (&22d4) which operates
        // on the full 16-bit positions.
        int other_fx = int(other.x.whole) * 256 + int(other.x.fraction);
        int other_fy = int(other.y.whole) * 256 + int(other.y.fraction);
        int obj_fx   = int(obj.x.whole)   * 256 + int(obj.x.fraction);
        int obj_fy   = int(obj.y.whole)   * 256 + int(obj.y.fraction);
        int dfx = other_fx - obj_fx;
        int dfy = other_fy - obj_fy;

        // Tile-granular distance still drives the power drop-off so
        // radius behaviour matches the 6502's 8-units-per-tile scale.
        int adx_tiles = std::abs(dfx) / 256;
        int ady_tiles = std::abs(dfy) / 256;
        int dist_units = std::max(adx_tiles, ady_tiles) * 8;

        uint8_t weight        = other.weight();
        bool    static_target = weight >= 7;
        int     weight_factor = weight * 2 + 8;

        int remaining = int(power) - weight_factor - dist_units;
        if (remaining <= 0) continue;

        // Damage — acceleration * 2 (port of &3485 ASL A / &3486 JSR
        // damage_object). Cap at object's current energy.
        if (damages && remaining >= 4) {
            uint8_t hurt = static_cast<uint8_t>(std::min(255, remaining * 2));
            other.energy = (other.energy > hurt) ? (other.energy - hurt) : 0;
        }

        // Statics feel damage but don't get accelerated.
        if (static_target) continue;

        // Push magnitude = remaining / 2, in the direction from the
        // explosion to the target. Normalise by the Chebyshev length
        // of the sub-tile delta so one axis always gets the full push
        // and the other is proportional. When both deltas are exactly
        // zero (perfect overlap), fall back to a purely upward kick —
        // matches the feel of the 6502 where zero-vector targets end
        // up with their acceleration_sign EOR'd angle defaulting
        // "up" for the blast wave.
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
}

} // namespace Behaviors
