#include "behaviours/creature.h"
#include "behaviours/mood.h"
#include "behaviours/path.h"
#include "audio/audio.h"
#include "world/water.h"
#include "world/tertiary.h"
#include "core/types.h"
#include <cstdio>
#include <cstdlib>

namespace Behaviors {

// &4A11: Player update - handled mostly in game.cpp, this covers supplementary logic
void update_player(Object& obj, UpdateContext& ctx) {
    // No-op stub for the dispatch table — player runs in Game::update_player().
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
                    NPC::offset_child_from_parent(bolt, obj);
                    obj.timer = 8; // Chattering animation timer
                }
                break;
            }
        }
    }

    // Chattering animation (timer counts down when active)
    if (obj.timer > 0 && !(obj.timer & 0x80)) {
        obj.timer--;
        // &4925-&492b: produce the chatter call. 6502 stores the
        // computed pitch into envelopes_table + &cf via self-modifying
        // code; we use the default block. Only fires periodically when
        // chatter is animating (~1/4 of those frames).
        if ((ctx.rng.next() & 0x03) == 0) {
            obj.palette = 0x4b; // cyB when chattering
            static constexpr uint8_t kSoundChatter[4] = { 0x33, 0xf3, 0xcd, 0x82 };
            Audio::play_at(Audio::CH_ANY, kSoundChatter,
                           obj.x.whole, obj.y.whole);
        }
    }

    // &4933-&494a produce power pod toward whistle-two source if 16-tile
    // LOS clear (check_for_obstruction_between_objects_80). LOS is door-
    // aware; without it Chatter fires through walls.
    if (ctx.whistle_two_activator < GameConstants::PRIMARY_OBJECT_SLOTS &&
        NPC::has_line_of_sight(obj, ctx.whistle_two_activator,
                                /*max_tiles=*/16, ctx)) {
        const Object& source = ctx.mgr.object(ctx.whistle_two_activator);
        int8_t sdx = static_cast<int8_t>(source.x.whole - obj.x.whole);
        int8_t sdy = static_cast<int8_t>(source.y.whole - obj.y.whole);
        // Fire a power pod toward the whistle source.
        int slot = NPC::fire_projectile(obj, ObjectType::POWER_POD, ctx);
        if (slot >= 0) {
            Object& pod = ctx.mgr.object(slot);
            pod.velocity_x = (sdx > 0) ? 0x10 : -0x10;
            pod.velocity_y = (sdy > 0) ? 0x10 : -0x10;
            NPC::offset_child_from_parent(pod, obj);
            obj.energy = 0; // Deactivate chatter after producing power pod
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

// Port of &46f0 update_crew_member:
//   &46f0 JSR &2492 play_scream_if_damaged
//   &46f3 JSR &3a6d update_walking_state
//   &46f6 JSR &254e increase_energy_by_one_if_not_zero
//   &46f9 LDA #&07                    ; 1-in-32 flip chance
//   &46fb JSR &257a consider_flipping_object_to_match_velocity_x_A
//   &46fe TAY                         ; A = x flip
//   &46ff LDA #&c0 ; upright
//   &4701 JMP &38d0 set_spacesuit_sprite_and_palette
// Wanders, can be rescued; port currently uses our reduced Mood
// system instead of the 6502's walking-state chain.
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
        // &42be-&42c1 squeal sound. Fluffy's complaint when upset or
        // when something nasty wanders near.
        static constexpr uint8_t kSoundFluffySqueal[4] = { 0xb0, 0x24, 0xb6, 0xe2 };
        Audio::play_at(Audio::CH_ANY, kSoundFluffySqueal, obj.x.whole, obj.y.whole);
    } else if (mood != NPCMood::MINUS_TWO &&
               (ctx.rng.next() & 0x7f) == 0) {
        // &42d4-&42d7 purr. Plays when fluffy is in non-negative mood;
        // 1-in-128 chance per frame keeps it ambient rather than
        // continuous.
        static constexpr uint8_t kSoundFluffyPurr[4] = { 0xc7, 0x81, 0xc1, 0xf3 };
        Audio::play_at(Audio::CH_ANY, kSoundFluffyPurr, obj.x.whole, obj.y.whole);
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

    // Approximate "held by player" without slot access: adjacent + matching
    // velocity. 6502 reads player_object_held == this_object directly.
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

// Per-type imp tables, indexed by (type − RED_MAGENTA_IMP): projectile
// &319d, minimum_energy &31a2, food &317f, gift &31a7.
static constexpr uint8_t imp_projectile_type[5] = {
    0x34, // red/magenta   -> BLUE_MUSHROOM_BALL
    0x17, // red/yellow    -> RED_BULLET
    0x58, // blue/cyan     -> CORONIUM_CRYSTAL
    0x33, // cyan/yellow   -> RED_MUSHROOM_BALL
    0x33, // red/cyan      -> RED_MUSHROOM_BALL
};
static constexpr uint8_t imp_minimum_energy[5] = {
    0x0a, 0x50, 0x46, 0x14, 0x13,
};
static constexpr uint8_t imp_food_type[5] = {
    0x11, // red/magenta   -> WASP
    0x2f, // red/yellow    -> WHITE_YELLOW_BIRD
    0x10, // blue/cyan     -> PIRANHA
    0x34, // cyan/yellow   -> BLUE_MUSHROOM_BALL
    0x30, // red/cyan      -> RED_MAGENTA_BIRD
};
static constexpr uint8_t imp_gift_type[5] = {
    0x4b, // red/magenta   -> POWER_POD
    0x12, // red/yellow    -> ACTIVE_GRENADE
    0x47, // blue/cyan     -> ALIEN_WEAPON
    0x47, // cyan/yellow   -> ALIEN_WEAPON
    0x12, // red/cyan      -> ACTIVE_GRENADE
};

// NPC_WAS_FED bit of objects_state (&11 in zp). Set by feed-detection
// below, read by the at-home gift-drop branch, cleared after dropping.
constexpr uint8_t kNPC_WAS_FED = 0x10;

// &44EF update_imp (all 5 variants). objects_state packs mood (bits 7-6),
// NPC_CLIMBING (bit 5), NPC_WAS_FED (bit 4), grounded-frame counter (bits 3-0).
// Walking/climbing/jumping physics not yet ported.
void update_imp(Object& obj, UpdateContext& ctx) {
    // &44ef-&44f7: newly-created imps start in MINUS_TWO (aggressive).
    if (obj.flags & ObjectFlags::NEWLY_CREATED) {
        obj.state = NPCMood::MINUS_TWO;
    }

    // &44f9-&4504: speed from mood (excited 0x28, neutral 0x10).
    uint8_t mood = Mood::get_mood(obj);
    int8_t speed = (mood != NPCMood::ZERO) ? 0x28 : 0x10;

    // &4506-&450b: convert object type into NPC stimuli index
    // (type − OBJECT_RED_MAGENTA_IMP). 0..4 indexes all five variants.
    uint8_t tidx = static_cast<uint8_t>(obj.type) -
                   static_cast<uint8_t>(ObjectType::RED_MAGENTA_IMP);
    if (tidx >= 5) tidx = 0;

    // &27c9/&2814/&4552 food absorption + WAS_FED latch. Hoisted above
    // the at-home block so an imp eating in its pipe drops same frame,
    // matching the 6502's post-stimuli was-fed promotion.
    bool was_fed_before = (obj.state & kNPC_WAS_FED) != 0;
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& touched = ctx.mgr.object(obj.touching);
        if (touched.is_active() &&
            static_cast<uint8_t>(touched.type) == imp_food_type[tidx]) {
            touched.flags |= ObjectFlags::PENDING_REMOVAL;
            obj.state |= kNPC_WAS_FED;
            if (!was_fed_before) {
                ctx.mgr.log_diag(
                    "imp p%d tidx=%u FED by type=0x%02x @%u,%u",
                    ctx.this_slot, tidx,
                    static_cast<unsigned>(touched.type),
                    obj.x.whole, obj.y.whole);
            }
        }
    }

    // &450c-&453f at-home gift drop. Runs on any PIPE tile, drops gift
    // when fed+counter>0, ALWAYS despawns at end — without &453f the imp
    // camps the pipe and re-runs the block forever.
    {
        // Resolve via tertiary so a bush-in-pipe (GREENERY_WITH_OBJECT_FROM_
        // TYPE redirected to PIPE) reads as PIPE — same as the 6502's &1715
        // get_tile_and_check_for_tertiary_objects.
        ResolvedTile res =
            resolve_tile_with_tertiary(ctx.landscape, obj.x.whole, obj.y.whole);
        uint8_t home_tile = res.tile_and_flip;
        uint8_t home_type = home_tile & TileFlip::TYPE_MASK;
        // Log every frame an imp with WAS_FED is alive so we can see
        // what tile/state it's actually in. Quiet when not fed, since
        // unfed wandering imps would flood the log.
        if (obj.state & kNPC_WAS_FED) {
            uint8_t gifts = ctx.imp_gifts_remaining
                ? ctx.imp_gifts_remaining[tidx] : 0xff;
            ctx.mgr.log_diag(
                "imp p%d tidx=%u FED-tick @%u,%u xf=0x%02x yf=0x%02x"
                " raw=0x%02x tile=0x%02x type=0x%02x sup=%d touching=0x%02x"
                " gifts=%u",
                ctx.this_slot, tidx,
                obj.x.whole, obj.y.whole,
                obj.x.fraction, obj.y.fraction,
                res.raw_tile_type,
                home_tile, home_type,
                obj.is_supported() ? 1 : 0,
                obj.touching,
                static_cast<unsigned>(gifts));
        }
        if (home_type == static_cast<uint8_t>(TileType::PIPE)) {
            // &4517-&451a: halve walking_speed twice on the pipe so the
            // imp settles instead of skidding off-centre.
            speed = static_cast<int8_t>(speed >> 2);

            // &451b-&4525 port deviation: 6502 also gates on
            // |centre_x_fraction|>=0x68. We home to bush.x (frac 0x40 per
            // &3e39 override) not pipe-centre, so the band never trips —
            // accept earlier despawn as the cost of any drop firing.

            // &4527-&4529 tile_collision_y_flags bit 7 (landed-this-frame).
            // Approximate with is_supported() — close enough since the imp
            // settles within the gift counter's first window.
            bool landed = obj.is_supported();
            ctx.mgr.log_diag(
                "imp p%d AT-PIPE @%u,%u landed=%d fed=%d gifts=%u",
                ctx.this_slot, obj.x.whole, obj.y.whole,
                landed ? 1 : 0,
                (obj.state & kNPC_WAS_FED) ? 1 : 0,
                ctx.imp_gifts_remaining
                    ? static_cast<unsigned>(ctx.imp_gifts_remaining[tidx])
                    : 0xffu);

            if (landed) {
                // &452b-&4531: gift-spawn is fed-gated and counter-gated.
                // 6502 DECs first then BMI; we check >0 then DEC, same
                // arithmetic outcome (initial N -> N drops).
                if ((obj.state & kNPC_WAS_FED) &&
                    ctx.imp_gifts_remaining &&
                    ctx.imp_gifts_remaining[tidx] > 0) {
                    ctx.imp_gifts_remaining[tidx]--;
                    int gslot = ctx.mgr.create_object_at(
                        static_cast<ObjectType>(imp_gift_type[tidx]),
                        /*min_free_slots=*/1, obj);
                    ctx.mgr.log_diag(
                        "imp p%d DROP gift_type=0x%02x gslot=%d remaining=%u",
                        ctx.this_slot,
                        static_cast<unsigned>(imp_gift_type[tidx]),
                        gslot,
                        static_cast<unsigned>(ctx.imp_gifts_remaining[tidx]));
                    if (gslot >= 0) {
                        Object& gift = ctx.mgr.object(gslot);
                        // &4533-&4537: gift vx = gift_type, negated by
                        // invert_if_negative when imp faces left so the
                        // gift flies the same way the imp is facing.
                        int8_t gift_vx = static_cast<int8_t>(imp_gift_type[tidx]);
                        if (obj.is_flipped_h()) gift_vx = static_cast<int8_t>(-gift_vx);
                        gift.velocity_x = gift_vx;
                        gift.velocity_y = -0x38;
                        // Disarm update_collectable's undisturbed pin —
                        // POWER_POD/ALIEN_WEAPON inherit energy bit 7
                        // and would otherwise zero our launch velocity.
                        gift.energy &= 0x7f;
                        NPC::offset_child_from_parent(gift, obj);
                        static constexpr uint8_t kSoundSqueal[4] =
                            { 0x37, 0xf1, 0x4d, 0xe0 };
                        Audio::play_at(Audio::CH_PRIORITY, kSoundSqueal,
                                       obj.x.whole, obj.y.whole);
                        if (ctx.floating_labels) {
                            FloatingLabel f;
                            f.world_x = gift.x.whole;
                            f.world_y = gift.y.whole;
                            f.x_frac  = gift.x.fraction;
                            f.y_frac  = gift.y.fraction;
                            std::snprintf(f.text, sizeof(f.text), "GIFT!");
                            f.rgb = 0x33ccff;
                            f.ttl = 90;
                            ctx.floating_labels->push_back(f);
                        }
                    }
                }
                // &453f set_object_as_far_away. Imps are KEEP_AS_TERTIARY
                // so return_to_tertiary re-arms the spawn gate. PENDING_
                // REMOVAL routes the slot through step 14's continue,
                // preventing step 15 from drifting y.whole off zero.
                ctx.mgr.log_diag(
                    "imp p%d DESPAWN at pipe @%u,%u",
                    ctx.this_slot, obj.x.whole, obj.y.whole);
                ctx.mgr.return_to_tertiary(ctx.this_slot);
                obj.flags |= ObjectFlags::PENDING_REMOVAL;
                return;
            }
        }
    }

    // &4542-&4548: minimum energy per type. Reached only via the
    // not_home branch in the 6502, so kept after the at-home block.
    NPC::enforce_minimum_energy(obj, imp_minimum_energy[tidx]);

    // &4548: check_for_npc_stimuli — updates the mood based on
    // environmental factors (time, damage, eating, …). Our Mood::
    // update_mood covers the same purpose.
    Mood::update_mood(obj, ctx);

    // &4554-&4558: WAS_FED locks the imp into MINUS_TWO mood. The 6502
    // re-applies this every frame so the eating-stimulus mood delta from
    // check_for_npc_stimuli can't push a fed imp out of MINUS_TWO before
    // it reaches its pipe.
    if (obj.state & kNPC_WAS_FED) {
        Mood::set_mood(obj, NPCMood::MINUS_TWO);
        mood = NPCMood::MINUS_TWO;
    }

    // &455a-&455f update_walking_npc (&3b08). walking_speed is a TARGET
    // velocity reached via capped acceleration. Target slot + AVOID flag
    // come from this_object_target_object_and_flags (stamped by the mood
    // pass at &27d5-&2802).
    if (mood == NPCMood::MINUS_TWO || mood == NPCMood::MINUS_ONE) {
        uint8_t target_slot = obj.target_and_flags & TargetFlags::OBJECT_MASK;
        bool avoid = (obj.target_and_flags & TargetFlags::AVOID) != 0;
        const Object& target =
            (target_slot < GameConstants::PRIMARY_OBJECT_SLOTS &&
             ctx.mgr.object(target_slot).is_active())
                ? ctx.mgr.object(target_slot)
                : ctx.mgr.player();
        uint8_t target_x = target.x.whole;
        uint8_t target_y = target.y.whole;

        int8_t dx = static_cast<int8_t>(target_x - obj.x.whole);
        // AVOID inverts the desired direction without touching the target —
        // mirrors the 6502 walker reading the flag at the bottom of
        // &3b08, where the sign of the seek delta is flipped.
        if (avoid) dx = static_cast<int8_t>(-dx);
        int8_t target_vx = (dx > 0) ? speed
                         : (dx < 0) ? static_cast<int8_t>(-speed)
                         : 0;
        constexpr int kAccel = 2;
        if (obj.velocity_x < target_vx) {
            int nv = obj.velocity_x + kAccel;
            if (nv > target_vx) nv = target_vx;
            obj.velocity_x = static_cast<int8_t>(nv);
        } else if (obj.velocity_x > target_vx) {
            int nv = obj.velocity_x - kAccel;
            if (nv < target_vx) nv = target_vx;
            obj.velocity_x = static_cast<int8_t>(nv);
        }

        // &3ae1/&3a54/&3a59 reduced jump port. Without per-axis collision
        // probes, approximate "blocked" via target-above-by-2 or velocity
        // sign-flip (bounce_reflect after wall hit). Gated to ≤1/4 frames
        // so jumps don't stack and the imp can rest between hops.
        int8_t dy = static_cast<int8_t>(target_y - obj.y.whole);
        // Only chase a target above us when we're actually approaching
        // it. Under AVOID the target IS the threat, so jumping toward
        // it would defeat the avoid behaviour the mood pass set up.
        bool target_above = !avoid && (dy <= -2);
        bool wall_bounced =
            (target_vx > 0 && obj.velocity_x < 0) ||
            (target_vx < 0 && obj.velocity_x > 0);
        if ((obj.state & kNPC_WAS_FED) && obj.is_supported() &&
            ctx.every_four_frames && (target_above || wall_bounced) &&
            (ctx.rng.next() & 0x07) == 0) {
            // 6502 set_npc_jumping subtracts ~0x0a from acceleration_y;
            // our physics doesn't separate accel from velocity, so apply
            // the impulse directly. -0x18 matches the frogman jump and
            // clears a typical 2-tile ledge.
            obj.velocity_y = -0x18;
            obj.flags &= ~ObjectFlags::SUPPORTED;
        }
    } else if (ctx.every_sixteen_frames) {
        // &31da-ish wander: jitter velocity_x every 16 frames.
        obj.velocity_x = static_cast<int8_t>((ctx.rng.next() & 0x0f) - 7);
    }

    // &4562-&4578 contact damage 5. The &4564 CPY &0e gate means a fed
    // imp brushing past the player doesn't hurt them — mirror by skipping
    // when WAS_FED is latched.
    if (obj.touching == 0 && !(obj.state & kNPC_WAS_FED)) {
        NPC::damage_player_if_touching(obj, ctx.mgr.player(), 5, ctx.damage_events);
    }

    // &45c7-&45d3 find_a_target_and_fire (A=8 -> fires when rng<=4, ~2%/frame).
    // Range-gated to 16 tiles per axis via &335a CMP #&06; skip when already
    // touching the player so a hugged imp doesn't shoot itself.
    bool not_at_target = (obj.touching != 0);
    const Object& player_for_range = ctx.mgr.player();
    int8_t rdx = static_cast<int8_t>(obj.x.whole - player_for_range.x.whole);
    int8_t rdy = static_cast<int8_t>(obj.y.whole - player_for_range.y.whole);
    bool in_range = std::abs(static_cast<int>(rdx)) < 16 &&
                    std::abs(static_cast<int>(rdy)) < 16;
    if (not_at_target && in_range && ctx.rng.next() < 5) {
        uint8_t proj_type = imp_projectile_type[tidx];
        if (proj_type < static_cast<uint8_t>(ObjectType::COUNT)) {
            int slot = NPC::fire_projectile(
                obj, static_cast<ObjectType>(proj_type), ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                const Object& player = ctx.mgr.player();
                int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
                int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
                int adx = std::abs(static_cast<int>(dx));
                int ady = std::abs(static_cast<int>(dy));
                int total = adx + ady;
                if (total == 0) total = 1;
                constexpr int kMag = 0x30;
                int vx = kMag * adx / total;
                int vy = kMag * ady / total;
                b.velocity_x = static_cast<int8_t>(dx >= 0 ? vx : -vx);
                b.velocity_y = static_cast<int8_t>(dy >= 0 ? vy : -vy);
                // Clear collectable undisturbed pin: CORONIUM_CRYSTAL
                // (type 0x4a..0x64) inherits energy bit 7 from
                // init_object_from_type; step 15 would zero our velocity.
                b.energy &= 0x7f;
                NPC::offset_child_from_parent(b, obj);

                // &460f imp_sound_parameters. The 6502 patches byte 3
                // (pitch) at runtime per imp variant via STA at &4609;
                // here we use the default-table values verbatim. Mostly
                // heard as a wide pitch range across the imp colours.
                static constexpr uint8_t kSoundImp[4] = { 0x9c, 0x05, 0xa6, 0xa5 };
                Audio::play_at(Audio::CH_ANY, kSoundImp, obj.x.whole, obj.y.whole);
            }
        }
    }

    // Face direction of movement.
    NPC::face_movement_direction(obj);

    // &45d6-&45e6: sprite from velocity magnitude mod 0x0c, divide by 8,
    // shift right once more to collapse into 0..2. Base = SPRITE_IMP_
    // WALKING_ONE (0x64). If velocity_x is zero, use the walking-one
    // frame directly (no animation).
    if (obj.velocity_x == 0) {
        NPC::change_object_sprite_to_base_plus_A(obj, 0);
    } else {
        // /8 scaling (X=2) approximated by helper's default /16; off/2
        // still lands in 0..2 — full scaling TODO.
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
        NPC::damage_player_if_touching(obj, ctx.mgr.player(), damage, ctx.damage_events);
    } else {
        if (ctx.every_sixteen_frames) {
            obj.velocity_x = (ctx.rng.next() & 0x07) - 3;
        }
    }

    NPC::face_movement_direction(obj);
    NPC::enforce_minimum_energy(obj, 0x7f);
}

// Port of &4463 update_red_frogman:
//   &4463 LDX #&09 ; npc stimuli type
//   &4465 JSR &27c9 check_for_npc_stimuli
//   &4468 LDA #&33 ; OBJECT_RED_MUSHROOM_BALL
//   &446a TAY
//   &446b JSR &3c0c avoid_object_type_Y
//   &446e JSR &3d26 consider_updating_npc_path
//   &4471 LDY #&64                    ; min energy 100
//   &4473 BNE &448a set_frogman_minimum_energy (always)
// Red frogman does NOT deal touch damage in the original; only the
// green/invisible variants do (they enter at &4477).
void update_red_frogman(Object& obj, UpdateContext& ctx) {
    frogman_common(obj, ctx, 0);
    NPC::enforce_minimum_energy(obj, 0x64);
}

// Port of &4477 update_green_frogman / cyan frogman:
//   &4477 LDX &3b ; this_object_touching
//   &4479 BNE &4488 ; not_touching_player
//   &447b JSR &4005 add_to_player_mushroom_timer (X=0 → red mushroom)
//   &447e LDA #&07
//   &4480 STA &12 ; this_object_timer (jump cooldown 7 frames)
//   &4482 ASL A                       ; A = 14 damage
//   &4483 LDY #&00 ; OBJECT_SLOT_PLAYER
//   &4485 JSR &24a6 damage_object
//   &4488 LDY #&5a                    ; min energy 90
//         (falls through to set_frogman_minimum_energy)
void update_green_frogman(Object& obj, UpdateContext& ctx) {
    frogman_common(obj, ctx, 14);
    NPC::enforce_minimum_energy(obj, 0x5a); // min 90
}

// Port of &4475 update_invisible_frogman (one instruction, falls
// through into update_green_frogman):
//   &4475 LSR &2b ; this_object_visibility   ; clear top bit → invisible
// TODO: wire invisibility once Object has a visibility field.
void update_invisible_frogman(Object& obj, UpdateContext& ctx) {
    update_green_frogman(obj, ctx);
}

// Port of &47c9 update_red_slime. Stationary, harmful on contact, drips
// RED_DROP particles. Sprite cycles 3-2-1-0-0-1-2-3 over 16 frames via
// abs((frame16 / 2) - 4). Drop initial offset at &47e2: x_frac=0x30/0x90
// per slime's h-flip, y_frac=0x40, vy=4.
void update_red_slime(Object& obj, UpdateContext& ctx) {
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 3, ctx.damage_events);
    NPC::enforce_minimum_energy(obj, 0x7f);

    uint8_t frame16 = ctx.frame_counter & 0x0f;
    int sprite_offset;
    if (frame16 == 0) {
        // 1-in-2 chance per 16 frames of dripping a RED_DROP. The
        // 6502 reads bit 7 of rng_state+1 directly via BIT &da; we
        // advance the rng to get a similar coin-flip.
        if (ctx.rng.next() & 0x80) {
            int slot = ctx.mgr.create_object_at(
                ObjectType::RED_DROP, /*min_free_slots=*/4, obj);
            if (slot > 0) {
                Object& drop = ctx.mgr.object(slot);
                bool flip_h =
                    (obj.flags & ObjectFlags::FLIP_HORIZONTAL) != 0;
                drop.x.fraction = flip_h ? 0x90 : 0x30;
                drop.y.fraction = 0x40;
                drop.velocity_y = 4;
            }
        }
        sprite_offset = 3;
    } else {
        // Cycle 3-2-1-0-0-1-2-3 across the 16-frame window.
        int a = (frame16 >> 1) - 4;
        if (a < 0) a = ~a;          // 6502 EOR #&ff
        sprite_offset = a;
    }

    // Must use &32aa subtract_width_from_position (X-only), not full
    // &3292: ceiling-mounted slime with y_frac=0 would underflow y.whole
    // and park itself + spawned RED_DROPs inside the solid ceiling.
    NPC::change_object_sprite_x_only(obj, static_cast<uint8_t>(sprite_offset));
}

// &422A: Green slime - walks slowly
void update_green_slime(Object& obj, UpdateContext& ctx) {
    // Slow walking movement
    if (ctx.every_sixteen_frames) {
        obj.velocity_x = (ctx.rng.next() & 0x03) - 1;
    }
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 2, ctx.damage_events);
    NPC::face_movement_direction(obj);
    NPC::enforce_minimum_energy(obj, 0x3f);
    // If absorbs coronium crystal, becomes yellow slime
}

// &4266: Yellow slime - can be picked up
void update_yellow_slime(Object& obj, UpdateContext& ctx) {
    // Yellow slime is heavier, doesn't move much
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 1, ctx.damage_events);
}

// &4761 update_big_fish — swims, eats piranhas. 
void update_big_fish(Object& obj, UpdateContext& ctx) {
    // &4761-&4765: eat any piranha we're touching.
    NPC::consider_absorbing_object_touched(obj, ObjectType::PIRANHA, ctx.mgr);

    // &4766-&476a: minimum energy 25 (was 0x3f in the old placeholder).
    NPC::enforce_minimum_energy(obj, 0x19);

    // &476b-&476f: out-of-water gate. 
    if (!Water::is_underwater(ctx.landscape, obj.x.whole, obj.y.whole)) {
        return;
    }

    // &4771-&4776: consider_finding_target(PIRANHA, range=PIRANHA). 
    NPC::consider_finding_target(obj, ObjectType::PIRANHA, ctx);

    // &4777: consider_updating_npc_path — LOS sweep + tx/ty pick.
    NPC::update_target_directness(obj, ctx);
    NPC::update_npc_path(obj, ctx);

    // &477a-&4785: magnitude 0x10, doubled to 0x20 when DIRECTNESS_TWO
    // ("can see or has seen piranha") is set; max acceleration 2. 
    uint8_t magnitude = (obj.target_and_flags & TargetFlags::DIRECTNESS_TWO)
                        ? 0x20 : 0x10;
    NPC::move_towards_target_with_probability(obj, ctx, magnitude, 2, 0xff);

    // &4786: consider_flipping_object_to_match_velocity_x — 1-in-4 flip.
    NPC::consider_face_movement_direction(obj, ctx.rng);
}

// Port of &420a update_worm (thin shim into &4e5e update_worm_or_maggot):
//   &420a LDA #&86                    ; OBJECT_RED_FROGMAN | &80 = "also target player"
//   &420c LDY #&07 ; OBJECT_GREEN_FROGMAN   ; avoid type
//   &420e LDX #&00                    ; 0 damage
//   &4210 JSR &4e5e update_worm_or_maggot
//   &4213 JMP &3c11 avoid_target      ; worms avoid green frogmen + player
// Port currently uses a simplified "move toward player" instead of the
// 6502 burrow path; the type/avoid/damage signature is preserved.
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
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 3, ctx.damage_events);
    NPC::face_movement_direction(obj);

    // &4ea1-&4eb3 squeal pair: two pitches layered into one warble.
    // play_at's 16-tile cutoff matches the 6502's distance gate.
    if ((ctx.rng.next() & 0x0f) == 0) {
        static constexpr uint8_t kSoundWormA[4] = { 0x33, 0xf3, 0x09, 0xb4 };
        static constexpr uint8_t kSoundWormB[4] = { 0x33, 0xf3, 0x07, 0xb5 };
        Audio::play_at(Audio::CH_ANY, kSoundWormA, obj.x.whole, obj.y.whole);
        Audio::play_at(Audio::CH_ANY, kSoundWormB, obj.x.whole, obj.y.whole);
    }

    // Random chance to despawn
    if (ctx.rng.next() == 0) {
        obj.energy = 0;
    }
}

// Port of &4e52 update_maggot (thin shim into &4e5e update_worm_or_maggot):
//   &4e52 LDA &15 ; this_object_energy
//   &4e54 AND #&7f                    ; clear MSB
//   &4e56 STA &15
//   &4e58 LDA #&82 ; OBJECT_CREW_MEMBER | &80   ; target+avoid pair
//   &4e5a LDY #&2f ; OBJECT_WHITE_YELLOW_BIRD
//   &4e5c LDX #&14                    ; 20 damage
//         (falls through to &4e5e update_worm_or_maggot)
// Same worm/maggot common body, different damage + target/avoid types.
void update_maggot(Object& obj, UpdateContext& ctx) {
    update_worm(obj, ctx);
}

// &4F21 update_piranha_or_wasp. Single routine; is_wasp branch flips
// gravity, default target (LARGE_HIVE vs SMALL_HIVE), and element gate.
// Aggressiveness stored in obj.state.
void update_piranha_or_wasp(Object& obj, UpdateContext& ctx) {
    const bool is_wasp = (obj.type == ObjectType::WASP);

    // &4f2b-&4f33 gravity tweak: piranhas +4 (sink), wasps -1
    // (cancel_gravity vs the main loop's +1).
    if (is_wasp) {
        NPC::cancel_gravity(obj);
    } else {
        if (obj.velocity_y < 127 - 4) obj.velocity_y += 4;
    }

    // &4f33-&4f42 1-in-2 retarget. Must update target_and_flags (not
    // velocity) so the hive's ±0x20 emerge momentum blends gradually
    // into the seek vector instead of stalling on tick one. Home-hive
    // fallback: leave target_and_flags as set by update_hive.
    if (ctx.rng.next() & 0x40) {
        bool target_player = obj.state >= (ctx.rng.next());
        if (target_player) {
            // Low 5 bits = target slot (0=player); preserve directness/
            // AVOID in top 3 (path-planning state).
            obj.target_and_flags = static_cast<uint8_t>(
                (obj.target_and_flags & 0xe0) | 0);
        }
    }

    // &4f45-&4f5e: 1/256 chance to play the passive sound; if
    // aggressiveness beats the roll and the creature is touching the
    // player, damage 24 and always play the attack sound.
    uint8_t roll = ctx.rng.next();
    bool damaging = (roll < obj.state) && (obj.touching == 0);
    if (damaging) {
        NPC::damage_player_if_touching(obj, ctx.mgr.player(), 24, ctx.damage_events);
        // &4f57-&4f5a: piranha / wasp attack sting sound (always plays
        // when damage was inflicted).
        static constexpr uint8_t kSoundPiranhaWasp[4] = { 0x33, 0xf3, 0x4f, 0x35 };
        Audio::play_at(Audio::CH_ANY, kSoundPiranhaWasp, obj.x.whole, obj.y.whole);
    }

    // &4f5e-&4f65: sprite frame from velocity magnitude mod 0x0c, shift
    // right twice -> 0..2. change_object_sprite_to_base_plus_A looks up
    // object_types_sprite[type] (WASP_ONE or PIRANHA_ONE) and adds the
    // frame, giving the three animation sprites for each creature.
    uint8_t off = NPC::update_sprite_offset_using_velocities(obj, 0x0c);
    off >>= 2;
    NPC::change_object_sprite_to_base_plus_A(obj, off);

    // &4f68: face movement direction (flip_object_to_match_velocity_x).
    NPC::face_movement_direction(obj);

    // &4f6b-&4f73 out-of-element bail. Must use per-column waterline:
    // SURFACE_Y shortcut wrongly flags lower-world air pockets as water,
    // stranding wasps that spawn there.
    bool in_water =
        Water::is_underwater(ctx.landscape, obj.x.whole, obj.y.whole);
    bool out_of_element = is_wasp ? in_water : !in_water;
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
// &4694-&4697: only RED_MAGENTA_BIRD has a non-zero minimum (0x1e = 30) — it
// regenerates when wounded so a single hit doesn't finish it.
static constexpr uint8_t birds_energy_table[4]  = { 0, 0, 0x1e, 0    };

// Common body of all bird types. Handles the 6502's update_bird path at
// &4631 onwards plus the earlier hooks for whistling / invisible birds.
// The wrapper functions below just implement the little preamble that
// distinguishes red-magenta and invisible birds.
static void update_bird_common(Object& obj, UpdateContext& ctx) {
    uint8_t tidx = static_cast<uint8_t>(obj.type) -
                   static_cast<uint8_t>(ObjectType::GREEN_YELLOW_BIRD);
    if (tidx >= 4) tidx = 0;

    // &4631-&463f: 1-in-64 chance each frame of playing the bird's call.
    // The 4 parameter bytes are the volume / frequency envelope block
    // that follows the 6502's JSR play_sound at &4638.
    if ((ctx.rng.next() & 0x3f) == 0) {
        static constexpr uint8_t kSoundBirdCall[4] = { 0x57, 0x07, 0x43, 0xf6 };
        Audio::play_at(Audio::CH_ANY, kSoundBirdCall, obj.x.whole, obj.y.whole);
    }

    // &4641-&464a: if touching the player, apply damage based on type.
    if (obj.touching == 0) {
        NPC::damage_player_if_touching(obj, ctx.mgr.player(),
                                       birds_damage_table[tidx],
                                       ctx.damage_events);
    }

    // &464a-&4652: clamp minimum energy. All four birds use 0, so this
    // is effectively a no-op today — kept for faithfulness.
    NPC::enforce_minimum_energy(obj, birds_energy_table[tidx]);

    // &4654-&4659 visibility on damage. WAS_DAMAGED flag approximates the
    // 6502's &253c check; invisible birds read obj.state to reveal.
    if (obj.flags & ObjectFlags::WAS_DAMAGED) {
        obj.state = 0x80;   // non-zero -> visible
    }

    // &465b-&4668 sprite cycle: |v|%0x14 >> 2, with 4->2 so the 4-frame
    // wing cycle dips through the middle pose on fast movement.
    {
        uint8_t off = NPC::update_sprite_offset_using_velocities(obj, 0x14);
        off >>= 2;
        if (off == 4) off = 2;
        NPC::change_object_sprite_to_base_plus_A(obj, off);
    }

    // &466b-&4670: eat any wasp we're touching, then aim at the next one.
    //   LDA #&11 ; OBJECT_WASP / JSR &3be1 ; consider_absorbing_object_touched
    //   LDA #&11 / LDY #&00    / JSR &3bf8 ; consider_finding_target
    NPC::consider_absorbing_object_touched(obj, ObjectType::WASP, ctx.mgr);
    NPC::consider_finding_target(obj, ObjectType::WASP, ctx);

    // &467a-&4686: NPC path update, then move towards the current target
    // with magnitude 0x40, max-accel 8, 1-in-4 probability (0x40 / 256).
    NPC::move_towards_target_with_probability(obj, ctx, 0x40, 8, 0x40);

    // &4686 `DEC this_object_acceleration_y` - cancel gravity (again,
    // because the post-path code may have re-introduced it). We use the
    // same cancel_gravity helper on velocity_y.
    NPC::cancel_gravity(obj);

    // &4688: if in any water, dampen both velocities twice (divide by 4).
    // Per-column waterline — SURFACE_Y shortcut would dampen birds flying
    // through the lower world's air pockets as if they were submerged.
    if (Water::is_underwater(ctx.landscape, obj.x.whole, obj.y.whole)) {
        NPC::dampen_velocities_twice(obj);
    }

    // Face the direction we're moving (flip_object_to_match_velocity_x
    // is implicit in face_movement_direction).
    NPC::face_movement_direction(obj);
}

// Port of &4631 update_bird (common entry for green/yellow + white/yellow):
//   &4631 JSR &2587 rnd
//   &4634 AND #&3f                    ; 1-in-64 bird call
//   &4636 BNE &463f ; skip_sound
//   &4638 JSR &13fa play_sound (data 57 07 43 f6)
//   &463f LDX &41 ; this_object_type  ; falls through to per-type body
// The two visible variants only differ in the damage/energy lookup
// that the common body picks up off the X-indexed table.
void update_bird(Object& obj, UpdateContext& ctx) {
    update_bird_common(obj, ctx);
}

// Port of &4621 update_red_magenta_bird:
//   &4621 LDA &da ; rnd_state+1
//   &4623 BNE &463f ; skip_sound       ; 1-in-256 chance
//   &4625 JSR &2c9e play_whistle_two_sound   ; deactivates Chatter
//   &4628 JMP &463f ; skip_sound
//         (falls into update_bird common body at &463f via the JMP)
// The full whistle-two side effect (setting whistle_two_activating_
// object so Chatter deactivates) is a separate TODO; the audible
// whistle goes through here.
void update_red_magenta_bird(Object& obj, UpdateContext& ctx) {
    uint8_t r = ctx.rng.next();
    if (r == 0) {
        // &2c9e-&2ca1 play_whistle_two_sound. The wider effect (setting
        // whistle_two_activating_object so Chatter deactivates) is a
        // separate TODO; the audible whistle goes through here.
        static constexpr uint8_t kSoundWhistleTwo[4] = { 0xb0, 0x24, 0xb6, 0xe2 };
        Audio::play_at(Audio::CH_ANY, kSoundWhistleTwo, obj.x.whole, obj.y.whole);
    }
    update_bird_common(obj, ctx);
}

// Port of &462b update_invisible_bird:
//   &462b LDA &11 ; this_object_state  ; nonzero = recently damaged
//   &462d BNE &4631 update_bird        ; skip clearing visibility
//   &462f LSR &2b ; this_object_visibility   ; clear MSB → invisible
//         (falls through to &4631 update_bird common body)
// 6502 stores visibility at &2b; we use bit 7 of obj.palette since
// that's the plot path that honours invisibility in our port.
void update_invisible_bird(Object& obj, UpdateContext& ctx) {
    // &462b-&462f: if bird hasn't taken damage recently, clear the top
    // bit of this_object_visibility to make it invisible again.
    if (obj.state == 0) {
        obj.palette &= 0x7f;    // invisible this frame
    }
    update_bird_common(obj, ctx);
}

// &4170 update_gargoyle. Data byte bits 6-0 select one of 5 sub-types
// (bit 7 = spawn-gate, masked). Tables below are byte-for-byte from
// &418b-&419e; create_projectile (&33ab) negates vx via x_flip so the
// projectile mirrors when the gargoyle faces left.
void update_gargoyle(Object& obj, UpdateContext& ctx) {
    static constexpr uint8_t kFreqMask[5] = { 0x0f, 0x07, 0x07, 0x07, 0x03 };
    static constexpr int8_t  kVx[5] = {
        static_cast<int8_t>(0x11),
        static_cast<int8_t>(0x7f),
        static_cast<int8_t>(0x7f),
        static_cast<int8_t>(0x7f),
        static_cast<int8_t>(0x01),
    };
    static constexpr int8_t  kVy[5] = {
        static_cast<int8_t>(0xc0),
        static_cast<int8_t>(0x0c),
        static_cast<int8_t>(0x04),
        static_cast<int8_t>(0xf9),
        static_cast<int8_t>(0x9a),
    };
    static constexpr ObjectType kProj[5] = {
        ObjectType::LIGHTNING,
        ObjectType::PLASMA_BALL,
        ObjectType::PLASMA_BALL,
        ObjectType::PLASMA_BALL,
        ObjectType::LIGHTNING,
    };

    // Strip the spawn-gate bit (bit 7) and clamp to table range. The
    // 6502 indexes blindly with no bounds check; clamping is a port
    // safety since corrupted data shouldn't read past the tables.
    uint8_t type = static_cast<uint8_t>(obj.tertiary_data_offset & 0x7f);
    if (type >= 5) type = 0;

    // &4170-&4175: fire when (freq_mask & frame_counter) == 0.
    if ((kFreqMask[type] & ctx.frame_counter) == 0) {
        int8_t vx = kVx[type];
        int8_t vy = kVy[type];
        // &33ab-&33b0 invert_if_negative flips vx when x_flip is set —
        // i.e. fire to the left when the gargoyle faces left.
        if (obj.flags & ObjectFlags::FLIP_HORIZONTAL) {
            vx = static_cast<int8_t>(-vx);
        }
        int slot = NPC::fire_projectile(obj, kProj[type], ctx);
        if (slot >= 0) {
            Object& proj = ctx.mgr.object(slot);
            proj.velocity_x = vx;
            proj.velocity_y = vy;
            NPC::offset_child_from_parent(proj, obj);
        }
    }

    // &4186-&4188: gain_energy_Y_and_flash_if_damaged with min 0x5a (90).
    // Inline the 6502's regen ladder: every 4 frames, if energy < 0xc0
    // increment by 1; then floor to min_energy. Skipping the
    // damaged-palette flash for now — it's purely visual.
    if (ctx.every_four_frames && obj.energy != 0 && obj.energy < 0xc0) {
        obj.energy++;
    }
    NPC::enforce_minimum_energy(obj, 0x5a);
}

// &4704 update_triax — boss with multiple teleport-away conditions.
// Faithful port; despawn paths cover: destinator absorbed, no-LOS,
// low energy, very-low energy, player-west-not-flooding, plus a
// 1-in-256 random kick. obj.ty=0 + TELEPORTING flag drives the
// 32-frame teleport animation that ends with the object at y=0 (=
// inactive / despawned by the next demotion pass).
static void triax_teleport_away(Object& obj) {
    // &475c-&489e set_this_object_ty_and_set_teleporting(A=0).
    obj.ty = 0;
    obj.flags |= ObjectFlags::TELEPORTING;
    obj.timer = 0x20;
}

void update_triax(Object& obj, UpdateContext& ctx) {
    // &4704-&4710 absorb destinator -> re-arm tertiary &9d so it
    // respawns at (0x64, 0xd6) in the lab, then teleport away.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& target = ctx.mgr.object(obj.touching);
        if (target.type == ObjectType::DESTINATOR) {
            target.flags |= ObjectFlags::PENDING_REMOVAL;
            ctx.mgr.set_tertiary_data_byte(0x9d, 0x80);
            triax_teleport_away(obj);
            return;
        }
    }

    Mood::update_mood(obj, ctx);
    NPC::update_npc_path(obj, ctx);  // 16-frame LOS raycast -> directness

    // &4712-&4718 no-LOS give-up: 1-in-256 frames when directness < 2.
    uint8_t lvl = NPC::directness_level(obj);
    if (lvl < 2) {
        if (ctx.rng.next() == 0) { triax_teleport_away(obj); return; }
    } else {
        // &471a-&4724: energy < 0x40 -> 1-in-64 teleport ((rnd & 0xfc) == 0).
        if (obj.energy < 0x40 && (ctx.rng.next() & 0xfc) == 0) {
            triax_teleport_away(obj); return;
        }
    }

    // &4726-&4730: 1-in-32 grenade else icer. consider_firing_at_player_
    // and_move_triax grants +2 energy per update, then flows into
    // move_hovering_npc (&486e) -> thrust_towards_target (&487a) which
    // does DEC acceleration_y at &4883 — Triax is a flying enemy.
    NPC::cancel_gravity(obj);
    const Object& player = ctx.mgr.player();
    int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
    int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
    if (obj.energy < 0xfe) obj.energy = static_cast<uint8_t>(obj.energy + 2);
    // 6502 fire-rate gate &276c-&2773: (energy/8 + 2) >= rnd.
    if (lvl >= 2) {
        uint8_t threshold =
            static_cast<uint8_t>((obj.energy >> 3) + 2);
        if (ctx.rng.next() < threshold) {
            bool grenade = (ctx.rng.next() & 0xf8) == 0;  // 1/32
            ObjectType proj = grenade ? ObjectType::ACTIVE_GRENADE
                                      : ObjectType::ICER_BULLET;
            NPC::seek_player(obj, player, 8);
            int8_t vx, vy;
            if (NPC::fire_at_target(obj, player, ctx.rng, vx, vy)) {
                int slot = NPC::fire_projectile(obj, proj, ctx);
                if (slot >= 0) {
                    Object& bullet = ctx.mgr.object(slot);
                    bullet.velocity_x = vx;
                    bullet.velocity_y = vy;
                    bullet.timer = 48;
                    NPC::offset_child_from_parent(bullet, obj);
                }
            }
        }
    }

    // &4733-&473d: energy < 5 -> 1-in-4 teleport (faithful: bit 0 of OR'd
    // randoms covers it; we use a simpler ((rnd & 0xc0) == 0) gate).
    if (obj.energy < 5 && (ctx.rng.next() & 0xc0) == 0) {
        triax_teleport_away(obj); return;
    }

    // &473f-&474d: when player is west of 0x80 AND not flooding,
    // 1-in-4 random teleport. Skipped during flooding endgame so
    // Triax can chase the player across the rising water.
    bool flooding = ctx.flooding_state && (*ctx.flooding_state & 0x80);
    bool player_west = player.x.whole < 0x80;
    if (player_west && !flooding && (ctx.rng.next() & 0x03) == 0) {
        triax_teleport_away(obj); return;
    }

    // &474f-&4752: always 1-in-256 random teleport regardless.
    if (ctx.rng.next() == 0) { triax_teleport_away(obj); return; }

    NPC::face_movement_direction(obj);

    // &4754 update_crew_member chain — walking animation. Cycle through
    // SPACESUIT walking sprites 0x04..0x07; standing snaps to 0x04.
    int abs_vx = obj.velocity_x < 0 ? -obj.velocity_x : obj.velocity_x;
    if (abs_vx > 0) {
        uint8_t inc = static_cast<uint8_t>(1 + (abs_vx >> 4));
        obj.timer = static_cast<uint8_t>(obj.timer + inc);
        NPC::animate_walking(obj, 0x04, obj.timer);
    } else {
        obj.sprite = 0x04;
    }
}

} // namespace Behaviors
