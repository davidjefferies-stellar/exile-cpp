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

} // namespace NPC
