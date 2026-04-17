#include "ai/behaviors/robot.h"
#include "ai/mood.h"
#include "core/types.h"
#include <cstdlib>

namespace Behaviors {

// &4ED8: Turret (green/white and cyan/red)
void update_turret(Object& obj, UpdateContext& ctx) {
    // Turrets are stationary: only fire at targets
    if (obj.energy < 0x80) {
        NPC::enforce_minimum_energy(obj, 0x46);
        return; // Not enough energy to fire
    }

    // Fire at player if in range
    if (ctx.every_eight_frames) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
        uint8_t dist = static_cast<uint8_t>(std::max(std::abs(dx), std::abs(dy)));

        if (dist < 16) {
            // Determine bullet type from tertiary data
            ObjectType bullet = ObjectType::PISTOL_BULLET;
            if (obj.tertiary_data_offset > 0) {
                // Different turrets fire different projectiles
                uint8_t data = obj.state;
                if ((data & 0x0e) == 0x0e) bullet = ObjectType::RED_BULLET;
                else if ((data & 0x06) == 0x06) bullet = ObjectType::ICER_BULLET;
            }

            int slot = NPC::fire_projectile(obj, bullet, ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                b.velocity_x = (dx > 0) ? 0x18 : -0x18;
                b.velocity_y = (dy > 0) ? 0x10 : -0x10;
                b.timer = 64; // Bullet lifespan
            }
        }
    }
}

// &4EDE: Rolling robot (magenta and red variants)
void update_rolling_robot(Object& obj, UpdateContext& ctx) {
    // Minimum energy varies by type
    uint8_t min_energy = 0x14; // Magenta
    if (obj.type == ObjectType::RED_ROLLING_ROBOT) min_energy = 0x46;
    NPC::enforce_minimum_energy(obj, min_energy);

    // Only move if energy >= 0x80
    if (obj.energy < 0x80) return;

    // Roll along ground: maintain horizontal velocity
    if (obj.is_supported()) {
        if (obj.velocity_x == 0) {
            // Start rolling in a direction
            obj.velocity_x = (ctx.rng.next() & 0x01) ? 4 : -4;
        }
    }

    // Reverse direction on wall collision
    // (collision detection will zero velocity_x on wall hit,
    //  next frame we detect zero and reverse)
    if (obj.velocity_x == 0 && obj.is_supported()) {
        obj.velocity_x = obj.is_flipped_h() ? 4 : -4;
    }

    NPC::face_movement_direction(obj);

    // Fire at player
    if (ctx.every_sixteen_frames && obj.energy >= 0x80) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        if (std::abs(dx) < 12) {
            ObjectType bullet = ObjectType::PISTOL_BULLET;
            if (obj.type == ObjectType::RED_ROLLING_ROBOT) bullet = ObjectType::ICER_BULLET;
            int slot = NPC::fire_projectile(obj, bullet, ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                b.velocity_x = (dx > 0) ? 0x18 : -0x18;
                b.velocity_y = -4;
                b.timer = 48;
            }
        }
    }
}

// &4EE2: Blue rolling robot - more aggressive, uses NPC walking
void update_blue_rolling_robot(Object& obj, UpdateContext& ctx) {
    NPC::enforce_minimum_energy(obj, 0x46);

    Mood::update_mood(obj, ctx);

    // Blue rolling robot actively seeks player
    if (obj.energy >= 0x80) {
        NPC::seek_player(obj, ctx.mgr.player(), 4);
    }

    NPC::face_movement_direction(obj);

    // Fire tracer bullets
    if (ctx.every_sixteen_frames && obj.energy >= 0x80) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        if (std::abs(dx) < 16) {
            int slot = NPC::fire_projectile(obj, ObjectType::TRACER_BULLET, ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                b.velocity_x = (dx > 0) ? 0x18 : -0x18;
                b.velocity_y = 0;
                b.timer = 96;
            }
        }
    }
}

// &4804: Hovering robot - flies, patrols, fires
void update_hovering_robot(Object& obj, UpdateContext& ctx) {
    NPC::cancel_gravity(obj);
    NPC::enforce_minimum_energy(obj, 0x81);

    // Patrol: hover near player
    if (ctx.every_eight_frames) {
        NPC::seek_player(obj, ctx.mgr.player(), 3);
    }

    // Random vertical jitter
    if (ctx.every_four_frames) {
        obj.velocity_y += (ctx.rng.next() & 0x03) - 1;
    }

    NPC::face_movement_direction(obj);

    // Fire at player
    if (ctx.every_sixteen_frames && obj.energy >= 0x80) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        if (std::abs(dx) < 12) {
            int slot = NPC::fire_projectile(obj, ObjectType::RED_BULLET, ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                b.velocity_x = (dx > 0) ? 0x20 : -0x20;
                b.velocity_y = 0;
                b.timer = 48;
            }
        }
    }
}

// &481F: Clawed robot (4 variants)
void update_clawed_robot(Object& obj, UpdateContext& ctx) {
    // Min energy depends on variant
    uint8_t min_energy;
    switch (obj.type) {
        case ObjectType::MAGENTA_CLAWED_ROBOT: min_energy = 0x46; break;
        case ObjectType::CYAN_CLAWED_ROBOT:    min_energy = 0x5a; break;
        case ObjectType::GREEN_CLAWED_ROBOT:   min_energy = 0x80; break;
        case ObjectType::RED_CLAWED_ROBOT:     min_energy = 0x82; break;
        default: min_energy = 0x46; break;
    }

    // Gain 2 energy per update
    if (obj.energy < 0xff - 1) obj.energy += 2;
    NPC::enforce_minimum_energy(obj, min_energy);

    // Teleport away if low energy or can't reach player
    if (obj.energy < 0x8c) {
        if (ctx.rng.next() == 0) {
            // Teleport to bottom of world
            obj.y.whole = 0xfe;
            obj.velocity_x = 0;
            obj.velocity_y = 0;
            return;
        }
    }

    // Move toward player aggressively
    NPC::seek_player(obj, ctx.mgr.player(), 6);
    NPC::face_movement_direction(obj);

    // Attack: fire icer bullets
    if (ctx.every_eight_frames) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
        if (std::abs(dx) < 10 && std::abs(dy) < 10) {
            int slot = NPC::fire_projectile(obj, ObjectType::ICER_BULLET, ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                b.velocity_x = (dx > 0) ? 0x20 : -0x20;
                b.velocity_y = (dy > 0) ? 0x10 : -0x10;
                b.timer = 48;
            }
        }
    }

    // Melee damage on contact
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 15);
}

// &43E7: Hovering ball - floats, fires
void update_hovering_ball(Object& obj, UpdateContext& ctx) {
    NPC::cancel_gravity(obj);

    // Float with random drift
    if (ctx.every_eight_frames) {
        obj.velocity_x += (ctx.rng.next() & 0x03) - 1;
        obj.velocity_y += (ctx.rng.next() & 0x03) - 1;
    }

    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 10);
}

// &43EB: Invisible hovering ball
void update_invisible_hovering_ball(Object& obj, UpdateContext& ctx) {
    update_hovering_ball(obj, ctx);
}

} // namespace Behaviors
