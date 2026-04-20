#include "ai/npc_helpers.h"
#include "objects/object_data.h"
#include "core/types.h"
#include <cstdlib>

namespace NPC {

void cancel_gravity(Object& obj) {
    // Counteract the +1 gravity applied by physics each frame
    if (obj.velocity_y > 0) obj.velocity_y--;
}

void move_toward(Object& obj, uint8_t target_x, uint8_t target_y, int8_t speed) {
    int8_t dx = static_cast<int8_t>(target_x - obj.x.whole);
    int8_t dy = static_cast<int8_t>(target_y - obj.y.whole);

    if (dx > 0) obj.velocity_x = speed;
    else if (dx < 0) obj.velocity_x = -speed;

    if (dy > 0) obj.velocity_y = speed;
    else if (dy < 0) obj.velocity_y = -speed;
}

void set_sprite_from_velocity(Object& obj, uint8_t base_sprite, int num_frames) {
    // Use velocity direction to pick animation frame
    int frame = 0;
    if (obj.velocity_x != 0 || obj.velocity_y != 0) {
        frame = (std::abs(obj.velocity_x) + std::abs(obj.velocity_y)) & (num_frames - 1);
    }
    obj.sprite = base_sprite + frame;
}

void animate_walking(Object& obj, uint8_t base_sprite, uint8_t frame_counter) {
    uint8_t frame = (frame_counter >> 2) & 0x03;
    obj.sprite = base_sprite + frame;
}

bool is_underwater(const Object& obj) {
    return obj.y.whole >= GameConstants::SURFACE_Y + 1;
}

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

void enforce_minimum_energy(Object& obj, uint8_t min_energy) {
    if (obj.energy < min_energy) {
        obj.energy = min_energy;
    }
}

void seek_player(Object& obj, const Object& player, int8_t speed) {
    move_toward(obj, player.x.whole, player.y.whole, speed);
}

void flee_player(Object& obj, const Object& player, int8_t speed) {
    int8_t dx = static_cast<int8_t>(obj.x.whole - player.x.whole);
    int8_t dy = static_cast<int8_t>(obj.y.whole - player.y.whole);

    if (dx >= 0) obj.velocity_x = speed;
    else obj.velocity_x = -speed;

    if (dy >= 0) obj.velocity_y = speed;
    else obj.velocity_y = -speed;
}

void face_movement_direction(Object& obj) {
    if (obj.velocity_x < 0) {
        obj.flags |= ObjectFlags::FLIP_HORIZONTAL;
    } else if (obj.velocity_x > 0) {
        obj.flags &= ~ObjectFlags::FLIP_HORIZONTAL;
    }
}

int fire_projectile(Object& obj, ObjectType bullet_type, UpdateContext& ctx) {
    return ctx.mgr.create_object_at(bullet_type, 4, obj);
}

uint8_t update_sprite_offset_using_velocities(Object& obj, uint8_t modulus) {
    // &2555-&256c. "max of |vx|, |vy|" then LSR 4 times (divide by 16),
    // add 1, add the existing timer, mod `modulus`. Faster-moving objects
    // tick through their frames faster.
    uint8_t ax = static_cast<uint8_t>(std::abs(obj.velocity_x));
    uint8_t ay = static_cast<uint8_t>(std::abs(obj.velocity_y));
    uint8_t m  = (ax > ay) ? ax : ay;
    m = static_cast<uint8_t>(m >> 4);
    uint16_t sum = static_cast<uint16_t>(obj.timer) + 1 + m;
    if (modulus == 0) modulus = 1;
    obj.timer = static_cast<uint8_t>(sum % modulus);
    return obj.timer;
}

void change_object_sprite_to_base_plus_A(Object& obj, uint8_t offset) {
    uint8_t tidx = static_cast<uint8_t>(obj.type);
    if (tidx >= static_cast<uint8_t>(ObjectType::COUNT)) return;
    obj.sprite = static_cast<uint8_t>(object_types_sprite[tidx] + offset);
}

void dampen_velocities_twice(Object& obj) {
    // &321f: two consecutive arithmetic shifts right per axis. Signed
    // halving preserves direction; two halves divides by 4.
    for (int pass = 0; pass < 2; pass++) {
        obj.velocity_x = static_cast<int8_t>(obj.velocity_x >> 1);
        obj.velocity_y = static_cast<int8_t>(obj.velocity_y >> 1);
    }
}

void move_towards_target_with_probability(Object& obj, UpdateContext& ctx,
                                          uint8_t magnitude,
                                          uint8_t max_accel,
                                          uint8_t prob_threshold) {
    // &31da reduced port. Original uses rnd_state + 1 vs threshold as the
    // probability gate; we honour that by rolling a single rng byte and
    // comparing. If the roll passes, accelerate toward the current target
    // (defaulting to player if the target slot is invalid) by `magnitude`
    // scaled to obj.velocity, clamped to `max_accel` per axis.
    uint8_t roll = ctx.rng.next();
    if (roll > prob_threshold) return;

    // Resolve target slot — low 5 bits of target_and_flags, zero = player.
    uint8_t slot = obj.target_and_flags & 0x1f;
    const Object& target = (slot < GameConstants::PRIMARY_OBJECT_SLOTS &&
                            ctx.mgr.object(slot).is_active())
                           ? ctx.mgr.object(slot)
                           : ctx.mgr.player();
    (void)magnitude; // magnitude governs vector length in the full routine;
                     // in this reduced port we lean on max_accel instead.

    int8_t tdx = static_cast<int8_t>(target.x.whole - obj.x.whole);
    int8_t tdy = static_cast<int8_t>(target.y.whole - obj.y.whole);

    auto nudge = [&](int8_t& v, int8_t d) {
        int step = (d > 0) ? int(max_accel) / 4 :
                   (d < 0) ? -int(max_accel) / 4 : 0;
        int nv = int(v) + step;
        if (nv >  int(max_accel)) nv =  int(max_accel);
        if (nv < -int(max_accel)) nv = -int(max_accel);
        v = static_cast<int8_t>(nv);
    };
    nudge(obj.velocity_x, tdx);
    nudge(obj.velocity_y, tdy);
}

} // namespace NPC
