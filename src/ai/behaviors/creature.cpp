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

// Per-type tables from &319d-&31a6, indexed by (type − RED_MAGENTA_IMP).
//  - projectile: what imp fires at the player when angered.
//  - minimum_energy: applied every frame via enforce_minimum_energy.
//  - gift: what a fed imp deposits at its pipe (TODO: requires at-home
//          detection which we approximate below).
static constexpr uint8_t imp_projectile_type[5] = {
    0x34, // red/magenta   → BLUE_MUSHROOM_BALL
    0x17, // red/yellow    → RED_BULLET
    0x58, // blue/cyan     → CORONIUM_CRYSTAL
    0x33, // cyan/yellow   → RED_MUSHROOM_BALL
    0x33, // red/cyan      → RED_MUSHROOM_BALL
};
static constexpr uint8_t imp_minimum_energy[5] = {
    0x0a, 0x50, 0x46, 0x14, 0x13,
};

// &44EF: Imp update (all 5 types). Port of update_imp.
//
// objects_state packs:
//   bits 7-6: NPC mood (MINUS_TWO, MINUS_ONE, ZERO, PLUS_ONE)
//   bit  5  : NPC_CLIMBING
//   bit  4  : NPC_WAS_FED
//   bits 3-0: frames since last standing on a walkable surface
//
// We don't port the full walking / climbing / jumping physics yet; this
// version covers the 6502's observable behaviour:
//   - newly-spawned imps start in MINUS_TWO (angry) mood.
//   - walking speed depends on mood (0x28 when excited, 0x10 when neutral).
//   - minimum energy and projectile type look up per-variant from tables.
//   - damage the player on contact, 5 points (&4573 LDA #&05).
//   - ~3-in-128 chance per frame of firing the variant's projectile.
//   - sprite from velocity magnitude (SPRITE_IMP_WALKING_ONE + 0..2).
void update_imp(Object& obj, UpdateContext& ctx) {
    // &44ef-&44f7: newly-created imps get MINUS_TWO mood so they start
    // aggressive. Clear NEWLY_CREATED after handling so we only run this
    // once; the main loop also clears it in step 18, but doing it here
    // too is harmless and mirrors the 6502's one-shot semantic.
    if (obj.flags & ObjectFlags::NEWLY_CREATED) {
        obj.state = NPCMood::MINUS_TWO;
    }

    // &44f9-&4504: speed from mood. "ASL A; EOR state; BMI not_zero_mood":
    // mood field is bits 7-6, ASL shifts bit 6 into 7; EOR with the
    // original state flips bit 7 back based on bit 7 alone. Result is
    // "is non-zero mood?" → bit 7 set. In our enum ZERO=0x00 and any
    // other value has at least one of bits 7-6 set, so a simple test
    // against NPCMood::MASK suffices.
    uint8_t mood = Mood::get_mood(obj);
    int8_t speed = (mood != NPCMood::ZERO) ? 0x28 : 0x10;

    // &4506-&450b: convert object type into NPC stimuli index
    // (type − OBJECT_RED_MAGENTA_IMP). 0..4 indexes all five variants.
    uint8_t tidx = static_cast<uint8_t>(obj.type) -
                   static_cast<uint8_t>(ObjectType::RED_MAGENTA_IMP);
    if (tidx >= 5) tidx = 0;

    // &4542-&4548: minimum energy per type.
    NPC::enforce_minimum_energy(obj, imp_minimum_energy[tidx]);

    // &4548: check_for_npc_stimuli — updates the mood based on
    // environmental factors (time, damage, eating, …). Our Mood::
    // update_mood covers the same purpose.
    Mood::update_mood(obj, ctx);

    // &455a-&455f: NPC path update + walking physics. We don't have
    // those helpers, so approximate: if the imp has an angry mood, walk
    // towards the player; neutral mood wanders.
    if (mood == NPCMood::MINUS_TWO || mood == NPCMood::MINUS_ONE) {
        NPC::seek_player(obj, ctx.mgr.player(), speed);
    } else if (ctx.every_sixteen_frames) {
        // &31da-ish wander: jitter velocity_x every 16 frames.
        obj.velocity_x = static_cast<int8_t>((ctx.rng.next() & 0x0f) - 7);
    }

    // &4562-&4578: if touching target (player or whatever we're
    // chasing) and it's pick-upable, deal 5 damage and latch onto its
    // velocity. We simplify to "damage player on contact".
    if (obj.touching == 0) {
        NPC::damage_player_if_touching(obj, ctx.mgr.player(), 5);
    }

    // &45c7-&45d3: 3-in-128 chance to fire at the player when not
    // currently touching the target. We just roll against 6 / 256 (~= 3/
    // 128) and fire the variant's projectile.
    bool not_at_target = (obj.touching != 0);
    if (not_at_target && ctx.rng.next() < 6) {
        uint8_t proj_type = imp_projectile_type[tidx];
        if (proj_type < static_cast<uint8_t>(ObjectType::COUNT)) {
            int slot = NPC::fire_projectile(
                obj, static_cast<ObjectType>(proj_type), ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                const Object& player = ctx.mgr.player();
                int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
                int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
                b.velocity_x = (dx > 0) ?  0x18 : -0x18;
                b.velocity_y = (dy > 0) ?  0x10 : -0x10;
            }
        }
    }

    // &4568-&456a: "skip attacking target if MINUS_TWO state bit 7 is
    // set" — effectively "too angry to pick up a gift". We don't model
    // gifts yet (TODO: port &452e-&453c imp-at-pipe gift drop).

    // Face direction of movement.
    NPC::face_movement_direction(obj);

    // &45d6-&45e6: sprite from velocity magnitude mod 0x0c, divide by 8,
    // shift right once more to collapse into 0..2. Base = SPRITE_IMP_
    // WALKING_ONE (0x64). If velocity_x is zero, use the walking-one
    // frame directly (no animation).
    if (obj.velocity_x == 0) {
        NPC::change_object_sprite_to_base_plus_A(obj, 0);
    } else {
        // update_sprite_offset_using_scaled_velocities divides by 8 (X=2)
        // rather than the default 16 (X=3) that our helper uses; the
        // resulting off/2 still lands in the 0..2 range after the two
        // LSRs that follow. Approximation is close enough for animation
        // cadence; full /8 scaling is TODO.
        uint8_t off = NPC::update_sprite_offset_using_velocities(obj, 0x0c);
        off >>= 2;
        if (off > 2) off = 2;
        NPC::change_object_sprite_to_base_plus_A(obj, off);
    }

    // &45f0-&460f: play imp sound (scream if just damaged, random-
    // pitched call otherwise). Sound playback is TODO.
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

// &4F21: Piranha or wasp - flying/swimming predator. Port of
// update_piranha_or_wasp. Both share the same routine in the 6502, with
// a single `is_wasp` branch flipping gravity (sinks vs floats), the
// default target (LARGE_HIVE vs SMALL_HIVE) and the water/air element
// gate. aggressiveness is stored in obj.state.
void update_piranha_or_wasp(Object& obj, UpdateContext& ctx) {
    const bool is_wasp = (obj.type == ObjectType::WASP);

    // &4f2b-&4f33: piranhas get a +4 sinking acceleration, wasps get −1
    // (countering the +1 gravity applied by the main loop). We model that
    // here by adjusting velocity_y directly, matching cancel_gravity and
    // adding +4 for piranhas.
    if (is_wasp) {
        NPC::cancel_gravity(obj);
    } else {
        if (obj.velocity_y < 127 - 4) obj.velocity_y += 4;
    }

    // &4f33-&4f42: 1-in-2 chance every frame of considering a new target.
    // When aggressiveness (obj.state) >= rnd, aim at the player directly;
    // otherwise fall back on the species' default nest type. Our helper
    // takes over the "consider_finding_target" / "consider_updating_npc_
    // path" chain by simply applying velocity via seek.
    if (ctx.rng.next() & 0x40) {
        const Object& player = ctx.mgr.player();
        bool target_player = obj.state >= (ctx.rng.next());
        if (target_player) {
            NPC::seek_player(obj, player, 4);
        }
        // Home-hive targeting isn't wired up yet; skip the fallback.
    }

    // &4f45-&4f5e: 1/256 chance to play the passive sound; if
    // aggressiveness beats the roll and the creature is touching the
    // player, damage 24 and always play the attack sound.
    uint8_t roll = ctx.rng.next();
    bool damaging = (roll < obj.state) && (obj.touching == 0);
    if (damaging) {
        NPC::damage_player_if_touching(obj, ctx.mgr.player(), 24);
    }
    // Sound playback is TODO.

    // &4f5e-&4f65: sprite frame from velocity magnitude mod 0x0c, shift
    // right twice → 0..2. change_object_sprite_to_base_plus_A looks up
    // object_types_sprite[type] (WASP_ONE or PIRANHA_ONE) and adds the
    // frame, giving the three animation sprites for each creature.
    uint8_t off = NPC::update_sprite_offset_using_velocities(obj, 0x0c);
    off >>= 2;
    NPC::change_object_sprite_to_base_plus_A(obj, off);

    // &4f68: face movement direction (flip_object_to_match_velocity_x).
    NPC::face_movement_direction(obj);

    // &4f6b-&4f73: if not colliding with tiles top/bottom AND out of
    // element (piranha above water OR wasp below water), leave — they
    // don't move outside their medium.
    bool out_of_element = is_wasp ? NPC::is_underwater(obj)
                                  : !NPC::is_underwater(obj);
    if (!obj.tile_collision && out_of_element) return;

    // &4f75-&4f7e: move towards current target with magnitude 0x30, max
    // accel 0x18, probability 5-in-32 (0x28).
    NPC::move_towards_target_with_probability(obj, ctx, 0x30, 0x18, 0x28);

    // &4f7e-&4f90: every 8 frames, jitter a single acceleration axis by
    // a signed byte in [−0x10, +0x0f].
    if (ctx.every_eight_frames) {
        int8_t jitter = static_cast<int8_t>((ctx.rng.next() & 0x1f) - 0x10);
        bool pick_y = (ctx.rng.next() & 0x02) != 0;
        if (pick_y) {
            int v = int(obj.velocity_y) + jitter;
            if (v >  127) v =  127;
            if (v < -128) v = -128;
            obj.velocity_y = static_cast<int8_t>(v);
        } else {
            int v = int(obj.velocity_x) + jitter;
            if (v >  127) v =  127;
            if (v < -128) v = -128;
            obj.velocity_x = static_cast<int8_t>(v);
        }
    }

    // &4f92-&4f98: regain 1 energy per frame while below 10, so wasps
    // and piranhas can't softly starve.
    if (obj.energy < 0x0a && obj.energy > 0) obj.energy++;
}

// Per-type damage / minimum-energy tables from &4690 and &4694. Indexed
// by (type − OBJECT_GREEN_YELLOW_BIRD), covering green, white, red,
// invisible.
static constexpr uint8_t birds_damage_table[4]  = { 0, 3, 0x40, 0x14 };
static constexpr uint8_t birds_energy_table[4]  = { 0, 0,    0,    0 };

// Common body of all bird types. Handles the 6502's update_bird path at
// &4631 onwards plus the earlier hooks for whistling / invisible birds.
// The wrapper functions below just implement the little preamble that
// distinguishes red-magenta and invisible birds.
static void update_bird_common(Object& obj, UpdateContext& ctx) {
    uint8_t tidx = static_cast<uint8_t>(obj.type) -
                   static_cast<uint8_t>(ObjectType::GREEN_YELLOW_BIRD);
    if (tidx >= 4) tidx = 0;

    // &4631-&463f: 1-in-64 chance each frame of playing the bird's call
    // sound. Sound playback is TODO; just roll the rng to keep the rng
    // state in step with the original.
    if ((ctx.rng.next() & 0x3f) == 0) {
        // play_sound(57 07 43 f6) — not yet wired.
    }

    // &4641-&464a: if touching the player, apply damage based on type.
    if (obj.touching == 0) {
        NPC::damage_player_if_touching(obj, ctx.mgr.player(),
                                       birds_damage_table[tidx]);
    }

    // &464a-&4652: clamp minimum energy. All four birds use 0, so this
    // is effectively a no-op today — kept for faithfulness.
    NPC::enforce_minimum_energy(obj, birds_energy_table[tidx]);

    // &4654-&4659: if the bird has just taken >=8 damage, set its
    // visibility bit (obj.state). Invisible birds read that flag to
    // temporarily reveal themselves. We approximate "was_damaged" with
    // the WAS_DAMAGED flag the main loop sets via check_if_object_was_
    // damaged (&253c).
    if (obj.flags & ObjectFlags::WAS_DAMAGED) {
        obj.state = 0x80;   // non-zero → visible
    }

    // &465b-&4668: sprite frame from velocity magnitude mod 0x14, shifted
    // right twice → 0..4. Value 4 collapses to 2 (BIRD_THREE), so we get
    // a 4-frame wing cycle that dips through the middle pose on fast
    // movement. change_object_sprite_to_base_plus_A indexes from
    // object_types_sprite[type] (SPRITE_BIRD_ONE = 0x59 for all birds).
    {
        uint8_t off = NPC::update_sprite_offset_using_velocities(obj, 0x14);
        off >>= 2;
        if (off == 4) off = 2;
        NPC::change_object_sprite_to_base_plus_A(obj, off);
    }

    // &466b-&4672: eat any wasp we're touching, then consider a new
    // target among wasps. consider_absorbing_object_touched and
    // consider_finding_target are complex routines; a pragmatic stand-in
    // is: if currently touching a wasp slot, zero its energy (pseudo-eat)
    // and aim at it next. Full port is TODO.
    // TODO: port &3be1 consider_absorbing_object_touched and &3bf8
    //       consider_finding_target.

    // &467a-&4686: NPC path update, then move towards the current target
    // with magnitude 0x40, max-accel 8, 1-in-4 probability (0x40 / 256).
    NPC::move_towards_target_with_probability(obj, ctx, 0x40, 8, 0x40);

    // &4686 `DEC this_object_acceleration_y` - cancel gravity (again,
    // because the post-path code may have re-introduced it). We use the
    // same cancel_gravity helper on velocity_y.
    NPC::cancel_gravity(obj);

    // &4688: if in any water, dampen both velocities twice (divide by 4).
    if (NPC::is_underwater(obj)) {
        NPC::dampen_velocities_twice(obj);
    }

    // Face the direction we're moving (flip_object_to_match_velocity_x
    // is implicit in face_movement_direction).
    NPC::face_movement_direction(obj);
}

// &4631: Green/yellow and white/yellow birds. Plain wrapper around
// update_bird_common — their only distinguishing behaviour is the
// damage/energy table lookup.
void update_bird(Object& obj, UpdateContext& ctx) {
    update_bird_common(obj, ctx);
}

// &4621: Red/magenta bird. 1-in-256 chance per frame of playing
// whistle-two, which deactivates Chatter. We don't have whistle sound
// playback yet; the logic signals intent via a flag on obj.state so the
// Chatter update can react later.
void update_red_magenta_bird(Object& obj, UpdateContext& ctx) {
    uint8_t r = ctx.rng.next();
    if (r == 0) {
        // TODO: play_whistle_two_sound — sets whistle_two_activator to
        // this bird's slot, deactivating Chatter.
    }
    update_bird_common(obj, ctx);
}

// &462B: Invisible bird. Stays invisible until it's been damaged; that's
// tracked by obj.state (non-zero = recently damaged = visible). We
// clear obj.visibility (stored as bit 7 of obj.palette in our port's
// convention? actually in the 6502 it's &2b this_object_visibility; we
// approximate with bit 7 of obj.palette since that's how the 6502 routes
// invisibility into plotting).
void update_invisible_bird(Object& obj, UpdateContext& ctx) {
    // &462b-&462f: if bird hasn't taken damage recently, clear the top
    // bit of this_object_visibility to make it invisible again.
    if (obj.state == 0) {
        obj.palette &= 0x7f;    // invisible this frame
    }
    update_bird_common(obj, ctx);
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
