#include "ai/behaviors/projectile.h"
#include "ai/mood.h"
#include "core/types.h"
#include "world/water.h"
#include "particles/particle_system.h"
#include <algorithm>

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

// Common bullet behavior: decrement energy (lifespan), face direction of travel
static void common_bullet_update(Object& obj, UpdateContext& ctx, uint8_t damage) {
    // Lifespan countdown
    if (obj.timer > 0) obj.timer--;
    if (obj.timer == 0) {
        obj.energy = 0; // Trigger explosion
        return;
    }

    // Face direction of travel
    NPC::face_movement_direction(obj);

    // Damage on contact
    if (obj.touching == 0) { // Touching player
        NPC::damage_player_if_touching(obj, ctx.mgr.player(), damage);
        obj.energy = 0; // Explode on contact
    }
}

// &42F7: Active grenade — 96-frame fuse; palette rotates through 4 colours;
// sound plays once per 16-frame cycle when the palette index wraps to 0.
// TODO(&42f7): check_if_object_fired → revert to INACTIVE_GRENADE if the
// player just threw this slot. Requires player_object_fired tracking.
void update_active_grenade(Object& obj, UpdateContext& ctx) {
    // &4305-&4309: if energy already 0, explode (duration 10). The main
    // loop treats energy==0 as "remove me", so we just return.
    if (obj.energy == 0) return;

    // &430b-&4311: after 96 frames, explode with duration 16.
    if (obj.timer >= 0x60) {
        obj.energy = 0;
        return;
    }

    // &4316: advance timer.
    obj.timer++;

    // &4318: animate palette; low bits are 0 once per 16 frames.
    uint8_t idx = rotate_colour_from_A(obj, obj.timer);

    // &431b-&4325: play grenade sound when idx == 0 (once per 16 frames).
    // TODO: wire sound when the audio system is ported.
    (void)idx;
}

// &46BF: Icer bullet - freezes on contact, 2 frame explosion
void update_icer_bullet(Object& obj, UpdateContext& ctx) {
    common_bullet_update(obj, ctx, 20);
    // Icer has longer range - reset timer if still moving
    if (obj.velocity_x != 0 || obj.velocity_y != 0) {
        if (obj.timer < 2) obj.timer = 2;
    }
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
}

// &434A: Red bullet - medium damage
void update_red_bullet(Object& obj, UpdateContext& ctx) {
    common_bullet_update(obj, ctx, 30);
}

// &441B: Pistol bullet - standard
void update_pistol_bullet(Object& obj, UpdateContext& ctx) {
    common_bullet_update(obj, ctx, 10);
}

// &4A88: Plasma ball — on object contact, turn it into a short fireball.
// Otherwise, reduce energy each frame; while underwater, 1-in-4 random
// removal; when out of energy, explode. Gravity cancelled (see &4a92+).
// TODO: particle emission (add_plasma_particles &4aa7), fireball conversion
// on object-type match (&4a8d-&4a90 needs collides_with_plasma_ball).
void update_plasma_ball(Object& obj, UpdateContext& ctx) {
    NPC::cancel_gravity(obj);

    // &4a88: if touching another object, turn that object into a duration-13
    // fireball (the plasma ball "becomes" the fireball in the same slot).
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS && obj.touching != 0) {
        obj.type   = ObjectType::FIREBALL;
        obj.timer  = 0x0d;
        obj.energy = 0x0d;
        obj.state  = 0; // zero target-object — "from exploding object"
        return;
    }

    // &4a92-&4a98: 1-in-4 random removal while underwater.
    if (NPC::is_underwater(obj)) {
        uint8_t r = ctx.rng.next() & ctx.rng.next(); // emulates AND of rnd_state & rnd_state+3
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

// &4AD6: Fireball - stationary fire
void update_fireball(Object& obj, UpdateContext& ctx) {
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 8);
    // Animate palette
    if (ctx.every_four_frames) {
        obj.palette = (obj.palette & 0x70) | (ctx.rng.next() & 0x07);
    }
}

// &4B26: Moving fireball - fire that moves
void update_moving_fireball(Object& obj, UpdateContext& ctx) {
    update_fireball(obj, ctx);
    NPC::cancel_gravity(obj);
}

// &4F9C: Explosion - expanding damage area
void update_explosion(Object& obj, UpdateContext& ctx) {
    // Duration stored in tertiary_data_offset
    if (obj.tertiary_data_offset > 0) {
        obj.tertiary_data_offset--;
    } else {
        obj.energy = 0; // Explosion finished
        return;
    }

    // Random palette for visual effect
    obj.palette = ctx.rng.next() & 0x0f;

    // Emit explosion particles each frame while burning.
    if (ctx.particles)
        ctx.particles->emit(ParticleType::EXPLOSION, 2, obj, ctx.rng);

    // Damage nearby objects (explosion radius based on remaining duration)
    uint8_t power = obj.tertiary_data_offset * 4;
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS && obj.touching != 0) {
        Object& target = ctx.mgr.object(obj.touching);
        if (target.energy > power) {
            target.energy -= power;
        } else {
            target.energy = 0;
        }
    }

    // Push nearby objects away
    for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        Object& other = ctx.mgr.object(i);
        if (!other.is_active()) continue;
        int8_t dx = static_cast<int8_t>(other.x.whole - obj.x.whole);
        int8_t dy = static_cast<int8_t>(other.y.whole - obj.y.whole);
        uint8_t dist = (std::abs(dx) > std::abs(dy))
                       ? static_cast<uint8_t>(std::abs(dx))
                       : static_cast<uint8_t>(std::abs(dy));
        if (dist < 4) {
            int8_t push = static_cast<int8_t>(power >> 3);
            if (dx > 0) other.velocity_x += push;
            else if (dx < 0) other.velocity_x -= push;
            if (dy > 0) other.velocity_y += push;
            else if (dy < 0) other.velocity_y -= push;
        }
    }
}

} // namespace Behaviors
