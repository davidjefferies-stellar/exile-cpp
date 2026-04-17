#include "ai/behaviors/creature.h"
#include "ai/mood.h"
#include "core/types.h"
#include <cstdlib>

namespace Behaviors {

// &4A11: Player update - handled mostly in game.cpp, this covers supplementary logic
void update_player(Object& obj, UpdateContext& ctx) {
    // Player-specific updates beyond movement (already in game.cpp):
    // - Aiming angle updates
    // - Blaster cooldown
    // - Energy bell sounds
    // Most player logic is in Game::update_player(), this is a no-op stub
    // for the dispatch table since the player is processed separately.
}

// Common chatter logic shared by active and inactive (port of &48a7-&48c0)
static void chatter_common(Object& obj, UpdateContext& ctx) {
    // Respond to whistle one: activate chatter
    if (ctx.whistle_one_active) {
        obj.timer |= 0x80;  // Set activation flag
        Mood::set_mood(obj, NPCMood::MINUS_TWO);
    }

    // NPC stimuli (type 7)
    Mood::update_mood(obj, ctx);

    // If fed coronium crystal (stimulus flag), increase energy reserve
    // (Simplified: feeding happens through the touching/collision system)
}

// &48D7: Active chatter - follows player, fires lightning, responds to whistles
void update_active_chatter(Object& obj, UpdateContext& ctx) {
    chatter_common(obj, ctx);
    NPC::cancel_gravity(obj);

    // Chatter can't be destroyed but deactivates at zero energy
    if (obj.energy == 0) {
        obj.type = ObjectType::INACTIVE_CHATTER;
        return;
    }

    const Object& player = ctx.mgr.player();
    int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
    int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);

    // Follow player at a distance (~2-3 tiles)
    if (std::abs(dx) > 3) {
        obj.velocity_x = (dx > 0) ? 4 : -4;
    } else if (std::abs(dx) < 2) {
        obj.velocity_x = (dx > 0) ? -2 : 2;
    }
    if (std::abs(dy) > 3) {
        obj.velocity_y = (dy > 0) ? 4 : -4;
    }

    // 1 in 8 chance of flipping to match velocity
    if ((ctx.rng.next() & 0x1f) == 0) {
        NPC::face_movement_direction(obj);
    }

    // Every 8 frames: consider firing lightning at enemies (&48ea)
    if (ctx.every_eight_frames) {
        // Search for flying enemies / turrets to target
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& target = ctx.mgr.object(i);
            if (!target.is_active()) continue;
            uint8_t t = static_cast<uint8_t>(target.type);
            // Target turrets and flying enemies
            bool is_target = (t >= 0x1f && t <= 0x20) || // Turrets
                             (t >= 0x1c && t <= 0x1e) || // Rolling robots
                             t == 0x21;                   // Hovering robot
            if (!is_target) continue;

            int8_t tdx = static_cast<int8_t>(target.x.whole - obj.x.whole);
            int8_t tdy = static_cast<int8_t>(target.y.whole - obj.y.whole);
            if (std::abs(tdx) < 8 && std::abs(tdy) < 4) {
                // Fire lightning toward target
                int slot = NPC::fire_projectile(obj, ObjectType::LIGHTNING, ctx);
                if (slot >= 0) {
                    Object& bolt = ctx.mgr.object(slot);
                    bolt.velocity_x = (tdx > 0) ? 0x20 : -0x20;
                    bolt.velocity_y = tdy;
                    obj.timer = 8; // Chattering animation timer
                }
                break;
            }
        }
    }

    // Chattering animation (timer counts down when active)
    if (obj.timer > 0 && !(obj.timer & 0x80)) {
        obj.timer--;
        // Chattering palette change
        if ((ctx.rng.next() & 0x03) == 0) {
            obj.palette = 0x4b; // cyB when chattering
        }
    }

    // Whistle two response: produce power pod if Chatter can see the source (&4933)
    if (ctx.whistle_two_activator < GameConstants::PRIMARY_OBJECT_SLOTS) {
        const Object& source = ctx.mgr.object(ctx.whistle_two_activator);
        int8_t sdx = static_cast<int8_t>(source.x.whole - obj.x.whole);
        int8_t sdy = static_cast<int8_t>(source.y.whole - obj.y.whole);
        if (std::abs(sdx) < 16 && std::abs(sdy) < 16) {
            // Fire a power pod toward the whistle source
            int slot = NPC::fire_projectile(obj, ObjectType::POWER_POD, ctx);
            if (slot >= 0) {
                Object& pod = ctx.mgr.object(slot);
                pod.velocity_x = (sdx > 0) ? 0x10 : -0x10;
                pod.velocity_y = (sdy > 0) ? 0x10 : -0x10;
                obj.energy = 0; // Deactivate chatter after producing power pod
            }
        }
    }
}

// &48C1: Inactive chatter - activates when whistle one is played
void update_inactive_chatter(Object& obj, UpdateContext& ctx) {
    chatter_common(obj, ctx);

    // Activate only when whistle one sets the activation flag
    if (!(obj.timer & 0x80)) return;

    // Check energy reserve
    if (obj.energy == 0) {
        // No energy: can't activate. Reset flag.
        obj.timer &= 0x7f;
        return;
    }

    // Activate: change to ACTIVE_CHATTER
    obj.type = ObjectType::ACTIVE_CHATTER;
}

// &46F0: Crew member - wanders, can be rescued
void update_crew_member(Object& obj, UpdateContext& ctx) {
    Mood::update_mood(obj, ctx);
    uint8_t mood = Mood::get_mood(obj);

    if (mood == NPCMood::ZERO || mood == NPCMood::PLUS_ONE) {
        // Neutral/friendly: follow player slowly
        NPC::seek_player(obj, ctx.mgr.player(), 2);
    } else {
        // Hostile: wander randomly
        if (ctx.every_sixteen_frames) {
            obj.velocity_x = (ctx.rng.next() & 0x07) - 3;
        }
    }
    NPC::face_movement_direction(obj);
    NPC::enforce_minimum_energy(obj, 0x3f);
}

// &4288: Fluffy - small companion creature
// Squeals when damaged or when enemies are near. Purrs when happy.
// Walks around using NPC pathfinding. Won't move when held by player.
void update_fluffy(Object& obj, UpdateContext& ctx) {
    NPC::enforce_minimum_energy(obj, 0x29); // Min energy 41

    // NPC stimuli check (type 6 = responds to imps/fireballs)
    Mood::update_mood(obj, ctx);

    uint8_t mood = Mood::get_mood(obj);
    bool is_squealing = false;

    // Squeal if damaged (energy dropped sharply)
    if (obj.state & 0x08) { // Recently damaged flag
        is_squealing = true;
        obj.state &= ~0x08;
    }

    // Squeal if mood is MINUS_TWO (very scared)
    if (mood == NPCMood::MINUS_TWO) {
        is_squealing = true;
    }

    // Every 8 frames, check for nearby enemies (imps)
    if ((ctx.frame_counter & 0x0b) == 0) {
        // Search for nearby imps or flying enemies
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& other = ctx.mgr.object(i);
            if (!other.is_active()) continue;
            // Check if it's an imp type (0x29-0x2d)
            uint8_t t = static_cast<uint8_t>(other.type);
            if (t >= 0x29 && t <= 0x2d) {
                int8_t dx = static_cast<int8_t>(other.x.whole - obj.x.whole);
                int8_t dy = static_cast<int8_t>(other.y.whole - obj.y.whole);
                uint8_t dist = static_cast<uint8_t>(
                    std::max(std::abs(dx), std::abs(dy)));
                // Squeal more likely when enemy is closer
                if (dist < ctx.rng.next()) {
                    is_squealing = true;
                }
                break;
            }
        }
    }

    if (is_squealing) {
        obj.timer |= 0x80; // Set active flag (top bit of timer)
    }

    // When not in negative mood, don't seek targets (target self)
    if (mood == NPCMood::ZERO || mood == NPCMood::PLUS_ONE) {
        // Neutral/happy: don't chase anything
    }

    // Animate: when active, randomly flip horizontally or vertically
    uint8_t axis = ctx.rng.next() & 0x02; // 0 = X flip, 2 = Y flip
    if (obj.timer & 0x80) {
        // Active: flip randomly
        if (axis == 0) {
            obj.flags ^= ObjectFlags::FLIP_HORIZONTAL;
        } else {
            obj.flags ^= ObjectFlags::FLIP_VERTICAL;
        }

        // Purr when active and not squealing (happy or neutral mood)
        if (!is_squealing && (mood == NPCMood::ZERO || mood == NPCMood::PLUS_ONE)) {
            // Purring (sound would play here)
        }
    }

    // Reduce activity: timer bit 7 decays based on mood
    // More active when happy or scared, less when neutral
    uint8_t mood_val = obj.state & NPCMood::MASK;
    if (mood_val == NPCMood::ZERO) {
        // Neutral: rarely active
        if (ctx.rng.next() < 0x20) obj.timer &= 0x7f;
    }

    // Don't move if held by player
    // (Check: is this object the held object? We can approximate by checking velocity sync)
    // The original checks player_object_held == this_object
    // We don't have direct access to held_slot here, but if velocity matches player exactly
    // and position is adjacent, it's likely held. Skip movement in that case.
    // A cleaner approach: check if our slot matches some held flag. For now, check if
    // we're at the same position as the player (held objects are always adjacent).
    const Object& player = ctx.mgr.player();
    int8_t pdx = static_cast<int8_t>(obj.x.whole - player.x.whole);
    int8_t pdy = static_cast<int8_t>(obj.y.whole - player.y.whole);
    bool likely_held = (std::abs(pdx) <= 1 && std::abs(pdy) <= 1 &&
                        obj.velocity_x == player.velocity_x &&
                        obj.velocity_y == player.velocity_y);

    if (!likely_held && (obj.timer & 0x80)) {
        // Walk using NPC walking type 2, speed 0x28
        // Simplified: move toward target or wander
        if (mood == NPCMood::MINUS_ONE || mood == NPCMood::MINUS_TWO) {
            // Scared: run away from threats (toward player for safety)
            NPC::seek_player(obj, player, 0x10);
        } else {
            // Wander randomly
            if (ctx.every_sixteen_frames) {
                obj.velocity_x = (ctx.rng.next() & 0x0f) - 7;
            }
        }
    }

    NPC::face_movement_direction(obj);
}

// &44EF: Imp update (all 5 types)
void update_imp(Object& obj, UpdateContext& ctx) {
    Mood::update_mood(obj, ctx);
    uint8_t mood = Mood::get_mood(obj);

    // Walking speed depends on mood
    int8_t speed = (mood == NPCMood::ZERO) ? 0x10 : 0x08;

    // Seek player or wander based on mood
    if (mood == NPCMood::MINUS_TWO || mood == NPCMood::MINUS_ONE) {
        NPC::seek_player(obj, ctx.mgr.player(), speed);
        // Damage player on contact
        NPC::damage_player_if_touching(obj, ctx.mgr.player(), 5);
    } else {
        // Neutral/positive: wander
        if (ctx.every_sixteen_frames) {
            obj.velocity_x = ((ctx.rng.next() & 0x0f) - 7);
        }
    }

    NPC::face_movement_direction(obj);
    NPC::animate_walking(obj, 0x64, ctx.frame_counter); // IMP_WALKING_ONE base sprite
}

// Common frogman behavior
static void frogman_common(Object& obj, UpdateContext& ctx, uint8_t damage) {
    Mood::update_mood(obj, ctx);
    uint8_t mood = Mood::get_mood(obj);

    // Frogmen jump periodically
    if (obj.is_supported() && ctx.every_sixteen_frames) {
        if (ctx.rng.next() < 0x40) {
            obj.velocity_y = -0x18; // Jump
        }
    }

    if (mood == NPCMood::MINUS_TWO) {
        NPC::seek_player(obj, ctx.mgr.player(), 6);
        NPC::damage_player_if_touching(obj, ctx.mgr.player(), damage);
    } else {
        if (ctx.every_sixteen_frames) {
            obj.velocity_x = (ctx.rng.next() & 0x07) - 3;
        }
    }

    NPC::face_movement_direction(obj);
    NPC::enforce_minimum_energy(obj, 0x7f);
}

// &4463: Red frogman — avoids mushroom balls, min energy 100 (&64),
// does NOT deal touch damage in the original. Only green/invisible do.
void update_red_frogman(Object& obj, UpdateContext& ctx) {
    frogman_common(obj, ctx, 0);
    NPC::enforce_minimum_energy(obj, 0x64);
}

// &4477: Green/cyan frogman — 14 touch damage (&447e LDA#&07; ASL → 14).
// Also adds to player mushroom timer (&447b JSR add_to_player_mushroom_timer).
void update_green_frogman(Object& obj, UpdateContext& ctx) {
    frogman_common(obj, ctx, 14);
    NPC::enforce_minimum_energy(obj, 0x5a); // min 90
}

// &4475: Invisible frogman — clears visibility bit (LSR &2b in the original,
// not yet represented in our Object struct) then falls through to the
// green frogman behavior (same 14 damage + mushroom timer).
// TODO: wire invisibility once Object has a visibility field.
void update_invisible_frogman(Object& obj, UpdateContext& ctx) {
    update_green_frogman(obj, ctx);
}

// &47C9: Red slime - stationary, harms on contact
void update_red_slime(Object& obj, UpdateContext& ctx) {
    // Red slime is mostly static but damages on contact
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 3);
    NPC::enforce_minimum_energy(obj, 0x7f);
}

// &422A: Green slime - walks slowly
void update_green_slime(Object& obj, UpdateContext& ctx) {
    // Slow walking movement
    if (ctx.every_sixteen_frames) {
        obj.velocity_x = (ctx.rng.next() & 0x03) - 1;
    }
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 2);
    NPC::face_movement_direction(obj);
    NPC::enforce_minimum_energy(obj, 0x3f);
    // If absorbs coronium crystal, becomes yellow slime
}

// &4266: Yellow slime - can be picked up
void update_yellow_slime(Object& obj, UpdateContext& ctx) {
    // Yellow slime is heavier, doesn't move much
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 1);
}

// &4761: Big fish - swims, eats piranhas
void update_big_fish(Object& obj, UpdateContext& ctx) {
    NPC::cancel_gravity(obj);
    if (!NPC::is_underwater(obj)) {
        // Fish out of water: no movement
        return;
    }

    // Swim toward piranhas or wander
    if (ctx.every_eight_frames) {
        obj.velocity_x += (ctx.rng.next() & 0x07) - 3;
        obj.velocity_y += (ctx.rng.next() & 0x03) - 1;
    }

    NPC::face_movement_direction(obj);
    NPC::enforce_minimum_energy(obj, 0x3f);

    // Absorb piranhas on contact
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& other = ctx.mgr.object(obj.touching);
        if (other.type == ObjectType::PIRANHA) {
            ctx.mgr.remove_object(obj.touching);
        }
    }
}

// &420A: Worm - burrows through earth
void update_worm(Object& obj, UpdateContext& ctx) {
    // Worms move toward player underground
    if (ctx.every_eight_frames) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
        if (dx > 0) obj.velocity_x = 2;
        else if (dx < 0) obj.velocity_x = -2;
        obj.velocity_y = (dy > 0) ? 2 : -2;
    }
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 3);
    NPC::face_movement_direction(obj);

    // Random chance to despawn
    if (ctx.rng.next() == 0) {
        obj.energy = 0;
    }
}

// &4E52: Maggot - similar to worm
void update_maggot(Object& obj, UpdateContext& ctx) {
    update_worm(obj, ctx);
}

// &4F21: Piranha or wasp - flying/swimming predator
void update_piranha_or_wasp(Object& obj, UpdateContext& ctx) {
    bool is_wasp = (obj.type == ObjectType::WASP);

    if (is_wasp) {
        NPC::cancel_gravity(obj);
    }

    // Check element compatibility
    bool in_element = is_wasp ? !NPC::is_underwater(obj) : NPC::is_underwater(obj);
    if (!in_element) return; // Don't move if out of element

    // Random jitter every 8 frames
    if (ctx.every_eight_frames) {
        obj.velocity_x += (ctx.rng.next() & 0x07) - 3;
        obj.velocity_y += (ctx.rng.next() & 0x03) - 1;
    }

    // Aggressiveness: sometimes target player
    if (ctx.every_sixteen_frames && (ctx.rng.next() & 0x01)) {
        NPC::seek_player(obj, ctx.mgr.player(), 4);
    }

    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 24);
    NPC::face_movement_direction(obj);
    NPC::enforce_minimum_energy(obj, 0x0a);
}

// &4631: Bird (green/yellow and white/yellow variants)
void update_bird(Object& obj, UpdateContext& ctx) {
    NPC::cancel_gravity(obj);

    // Flying movement with random jitter
    if (ctx.every_eight_frames) {
        obj.velocity_x += (ctx.rng.next() & 0x03) - 1;
        obj.velocity_y += (ctx.rng.next() & 0x03) - 1;
    }

    // Green birds: no damage. White birds: 3 damage.
    uint8_t damage = (obj.type == ObjectType::WHITE_YELLOW_BIRD) ? 3 : 0;
    if (damage > 0) NPC::damage_player_if_touching(obj, ctx.mgr.player(), damage);

    NPC::face_movement_direction(obj);

    // 4-frame wing animation
    uint8_t frame = (ctx.frame_counter >> 2) & 0x03;
    obj.sprite = 0x59 + frame; // BIRD_ONE base sprite
}

// &4621: Red/magenta bird - aggressive
void update_red_magenta_bird(Object& obj, UpdateContext& ctx) {
    NPC::cancel_gravity(obj);

    // Red/magenta birds can play whistle two (port of &4625)
    // Every 64 frames, if near player and mood allows
    if (ctx.every_sixty_four_frames && (ctx.rng.next() & 0x03) == 0) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
        if (std::abs(dx) < 8 && std::abs(dy) < 8) {
            // This bird plays whistle two, which can activate Chatter's power pod
            // In the original, this sets whistle_two_activating_object to this bird's slot
            // We'd need the slot index here; for now, signal via a known slot value
        }
    }

    // More aggressive: actively seeks player
    if (ctx.every_eight_frames) {
        NPC::seek_player(obj, ctx.mgr.player(), 3);
    }

    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 64);
    NPC::face_movement_direction(obj);
    NPC::enforce_minimum_energy(obj, 0x1e);

    uint8_t frame = (ctx.frame_counter >> 2) & 0x03;
    obj.sprite = 0x59 + frame;
}

// &462B: Invisible bird - invisible until damaged
void update_invisible_bird(Object& obj, UpdateContext& ctx) {
    NPC::cancel_gravity(obj);

    if (ctx.every_eight_frames) {
        obj.velocity_x += (ctx.rng.next() & 0x03) - 1;
        obj.velocity_y += (ctx.rng.next() & 0x03) - 1;
    }

    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 20);
    NPC::face_movement_direction(obj);

    uint8_t frame = (ctx.frame_counter >> 2) & 0x03;
    obj.sprite = 0x59 + frame;
}

// &4170: Gargoyle - stationary, spits fireballs
void update_gargoyle(Object& obj, UpdateContext& ctx) {
    // Gargoyles don't move, just fire periodically
    if (ctx.every_thirty_two_frames) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        if (std::abs(dx) < 16) {
            int slot = NPC::fire_projectile(obj, ObjectType::FIREBALL, ctx);
            if (slot >= 0) {
                Object& fireball = ctx.mgr.object(slot);
                fireball.velocity_x = (dx > 0) ? 8 : -8;
                fireball.velocity_y = -4;
            }
        }
    }
}

// &4704: Triax - the boss, teleports and attacks
void update_triax(Object& obj, UpdateContext& ctx) {
    Mood::update_mood(obj, ctx);

    // Triax is always aggressive
    const Object& player = ctx.mgr.player();

    // Teleport if far from player
    int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
    int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);

    if (std::abs(dx) > 20 || std::abs(dy) > 20) {
        // Teleport near player
        if (ctx.rng.next() < 0x10) {
            obj.x.whole = player.x.whole + (ctx.rng.next() & 0x0f) - 8;
            obj.y.whole = player.y.whole + (ctx.rng.next() & 0x0f) - 8;
            obj.velocity_x = 0;
            obj.velocity_y = 0;
        }
    }

    // Attack: fire icer bullets toward player
    if (ctx.every_eight_frames) {
        NPC::seek_player(obj, ctx.mgr.player(), 8);
        if (ctx.rng.next() < 0x40) {
            int slot = NPC::fire_projectile(obj, ObjectType::ICER_BULLET, ctx);
            if (slot >= 0) {
                Object& bullet = ctx.mgr.object(slot);
                bullet.velocity_x = (dx > 0) ? 0x20 : -0x20;
                bullet.velocity_y = (dy > 0) ? 0x10 : -0x10;
            }
        }
    }

    NPC::face_movement_direction(obj);
    NPC::enforce_minimum_energy(obj, 0xfd);
}

} // namespace Behaviors
