#include "behaviours/creature.h"
#include "behaviours/mood.h"
#include "behaviours/path.h"
#include "audio/audio.h"
#include "objects/object_data.h"
#include "particles/particle_system.h"
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
    // &48ad-&48af STA &12 / STA &11: whistle one writes A=0x80 to both
    // timer and state, clearing low bits — not OR / set_mood, which
    // preserved them. A mid-chatter retrigger must reset cleanly.
    if (ctx.whistle_one_active) {
        obj.timer = 0x80;
        obj.state = NPCMood::MINUS_TWO;
    }

    // &48b1-&48b6: NPC stimuli (type 7) + path update.
    Mood::update_mood(obj, ctx);

    // &48b9-&48bd: feeding. 6502 reads bit 0 of the stimuli byte (food
    // absorbed) and INCs chatter_energy_reserve. Mood::update_mood owns
    // the stimuli byte and doesn't surface it, so re-do the touch test
    // here — the PENDING_REMOVAL stamp is idempotent.
    if (ctx.chatter_energy_reserve &&
        obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& touched = ctx.mgr.object(obj.touching);
        if (touched.is_active() &&
            touched.type == ObjectType::CORONIUM_CRYSTAL) {
            touched.flags |= ObjectFlags::PENDING_REMOVAL;
            (*ctx.chatter_energy_reserve)++;
        }
    }
}

// Port of &3286 change_object_type: rewrites type, palette, sprite from
// the type tables in one shot. Position recentre at &329e-&32b3 is
// skipped — both Chatter variants share sprite 0x14 so the centre is
// already correct.
static void change_object_type(Object& obj, ObjectType new_type) {
    uint8_t idx = static_cast<uint8_t>(new_type);
    obj.type    = new_type;
    obj.palette = object_types_palette_and_pickup[idx] & 0x7f;
    obj.sprite  = object_types_sprite[idx];
}

// Port of &3547 flash_if_damaged with min_energy=0 (the LDY #&00 path
// used by Chatter at &48da). No regen — strobes base_palette ^ &30 for
// 2-in-8 frames when energy < &80, otherwise stamps base palette.
static void flash_if_damaged(Object& obj, UpdateContext& ctx) {
    uint8_t base = object_types_palette_and_pickup[
        static_cast<uint8_t>(obj.type)] & 0x7f;
    bool low_energy = obj.energy < 0x80;
    bool damaged_phase = (ctx.frame_counter & 0x07) < 0x02;
    obj.palette = (low_energy && damaged_phase)
                      ? static_cast<uint8_t>(base ^ 0x30)
                      : base;
}

// &48D7 update_active_chatter. Faithful port of the 6502:
//   chatter_common -> flash_if_damaged -> energy=0 deactivate -> 1-in-32
//   flip -> every 8 frames fire lightning at a CYAN_RED_TURRET within
//   ~14 deg of horizontal -> chattering timer / sound -> whistle-two
//   power pod -> set target=player (if untargeted) -> thrust_towards_target.
void update_active_chatter(Object& obj, UpdateContext& ctx) {
    chatter_common(obj, ctx);

    // &48da-&48dc flash_if_damaged (min energy 0, Chatter is indestructible
    // so the floor never matters — only the 2-in-8 strobe does).
    flash_if_damaged(obj, ctx);

    // &48df-&48e2: damage zeroed energy -> change_object_type back to
    // INACTIVE_CHATTER (refreshes palette/sprite to the dormant tables).
    if (obj.energy == 0) {
        change_object_type(obj, ObjectType::INACTIVE_CHATTER);
        return;
    }

    // &48e3-&48e5 consider_flipping_object_to_match_velocity_x_A with
    // A=&1f -> 1-in-32 chance of flipping. AND mask matches any bit set
    // in rnd; only zero (1/32) triggers the flip.
    if ((ctx.rng.next() & 0x1f) == 0) {
        NPC::face_movement_direction(obj);
    }

    // &48e8-&490d every_eight_frames: find_object A=&20 / Y=&86 — match
    // CYAN_RED_TURRET as primary OR any FLYING_ENEMIES type (range index 6
    // at &29ee + 6, covering &22..&31: clawed robots, Triax, maggot,
    // gargoyle, imps, birds). The randomised pick / probability gate from
    // &3c2a is collapsed to "first matching primary" here.
    if (ctx.every_eight_frames) {
        int best = -1;
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& cand = ctx.mgr.object(i);
            if (!cand.is_active()) continue;
            uint8_t t = static_cast<uint8_t>(cand.type);
            bool is_turret = (t == 0x20);
            bool is_flying = (t >= 0x22 && t <= 0x31);
            if (!is_turret && !is_flying) continue;
            best = i;
            break;
        }
        if (best >= 0) {
            const Object& tgt = ctx.mgr.object(best);
            int8_t tdx = static_cast<int8_t>(tgt.x.whole - obj.x.whole);
            int8_t tdy = static_cast<int8_t>(tgt.y.whole - obj.y.whole);
            uint8_t angle = NPC::angle_from_deltas(tdx, tdy);

            // &48f7-&48fd: ADC #&40 / EOR flags / BMI skip — fire only when
            // the rotated angle's high bit matches the chatter's flip flag.
            // ADC also sets carry when angle >= &c0, which the SBC below
            // inherits (the difference between subtracting &0a vs &0b).
            uint8_t rotated = static_cast<uint8_t>(angle + 0x40);
            bool backward =
                ((rotated ^ obj.flags) & 0x80) != 0;
            bool carry_in = (angle >= 0xc0);

            // &48ff-&4907: AND &7f / SBC &0a / CMP &6c / BCC skip. The SBC
            // uses the carry from the ADC above, so we subtract &0a with
            // carry set, &0b with carry clear. Underflow to a high A makes
            // the CMP carry set too -> fire. Net: fires when masked is in
            // [0x00..0x0a] (near "right") OR [0x77..0x7f] (near "left").
            uint8_t masked   = static_cast<uint8_t>(angle & 0x7f);
            uint8_t sub_val  = carry_in ? 0x0a : 0x0b;
            uint8_t after_sb = static_cast<uint8_t>(masked - sub_val);
            bool horizontal  = (after_sb >= 0x6c);

            if (!backward && horizontal) {
                // &4909 STA &12: chattering timer carries the SBC result so
                // the pitch envelope downstream uses the firing angle.
                obj.timer = after_sb;
                int slot = NPC::fire_projectile(obj, ObjectType::LIGHTNING, ctx);
                if (slot >= 0) {
                    Object& bolt = ctx.mgr.object(slot);
                    // &33a5-&33a9 create_lightning: x velocity = 0x28
                    // signed by x_flip, y velocity = 0.
                    bool facing_left = (obj.flags & ObjectFlags::FLIP_HORIZONTAL) != 0;
                    bolt.velocity_x = facing_left ? -0x28 : 0x28;
                    bolt.velocity_y = 0;
                    NPC::offset_child_from_parent(bolt, obj);
                }
            }
        }
    }

    // &490e-&4932 chattering: BEQ skip on timer == 0, else DEC timer; 1-in-4
    // (rnd >= &c0) plays the chatter sound and stamps palette = cyB. The
    // 6502 doesn't mask off bit 7 here — when the lightning-fire SBC
    // underflows for "near right" angles, timer holds 0xf5+ and ticks down
    // through 0x80 the same as any other start value.
    if (obj.timer != 0) {
        obj.timer--;
        if (ctx.rng.next() >= 0xc0) {
            // &491a-&4925 pitch computation: ((rnd>>2) ^ mood) + 0x40 ^ 0xc0
            // >> 1, stored into envelopes_table + &cf so the chatter sound's
            // second-stage freq delta varies per call. Without this every
            // chatter plays at the identical pitch and sounds wrong.
            uint8_t r     = ctx.rng.next();
            uint8_t mood  = obj.state;
            uint8_t pitch = static_cast<uint8_t>(
                ((((r >> 2) ^ mood) + 0x40) ^ 0xc0) >> 1);
            Audio::set_envelope_byte(0xcf, pitch);

            static constexpr uint8_t kSoundChatter[4] = { 0x33, 0xf3, 0xcd, 0x82 };
            Audio::play_at(Audio::CH_ANY, kSoundChatter,
                           obj.x.whole, obj.y.whole);
            obj.palette = 0x4b;
        }
    }

    // &4933-&494b whistle-two power pod. fire toward the activator if LOS
    // clear; carry-clear (the 6502's pod-created path) zeroes energy and
    // deactivates Chatter on the next frame.
    uint8_t w2 = ctx.whistle_two_activator ? *ctx.whistle_two_activator : 0xff;
    if (w2 < GameConstants::PRIMARY_OBJECT_SLOTS &&
        NPC::has_line_of_sight(obj, w2, /*max_tiles=*/16, ctx)) {
        const Object& source = ctx.mgr.object(w2);
        int8_t sdx = static_cast<int8_t>(source.x.whole - obj.x.whole);
        int8_t sdy = static_cast<int8_t>(source.y.whole - obj.y.whole);
        int slot = NPC::fire_projectile(obj, ObjectType::POWER_POD, ctx);
        if (slot >= 0) {
            Object& pod = ctx.mgr.object(slot);
            // &493d-&4941 fire_at_target_with_velocity A=&40 (firing
            // velocity * 4 = &10). 16-bit math for trajectory skipped.
            pod.velocity_x = (sdx > 0) ?  0x10 : -0x10;
            pod.velocity_y = (sdy > 0) ?  0x10 : -0x10;
            NPC::offset_child_from_parent(pod, obj);
            obj.energy = 0;
        }
    }

    // &494c-&4953: ORA touching, target_object; BNE skip; STA target_and_
    // flags. Fires only when both already point at slot 0 (player). Net
    // effect is clearing any flags Mood::update_mood may have stamped
    // (AVOID etc.) when Chatter is already on the player.
    uint8_t target_slot = obj.target_and_flags & TargetFlags::OBJECT_MASK;
    if (obj.touching == 0 && target_slot == 0) {
        obj.target_and_flags = 0;
    }

    // &4954 JMP thrust_towards_target (&487a):
    //   LDA #&1c magnitude / LDY #&04 max_accel / LDX #&80 prob (1-in-2)
    //   JSR move_towards_target_with_probability
    //   DEC accel_y (cancel gravity)
    //   JSR consider_hovering_over_ground   ; nudges accel_y up near ground
    //   JMP add_jetpack_thrust_particles    ; emits when accel != 0
    NPC::move_towards_target_with_probability(obj, ctx,
                                              /*magnitude=*/0x1c,
                                              /*max_accel=*/0x04,
                                              /*prob_threshold=*/0x80);
    NPC::cancel_gravity(obj);

    // &1f3d add_jetpack_thrust_particles: the 6502 reads accel_x|accel_y
    // which is non-zero whenever cancel_gravity or hover ran. Our port
    // doesn't keep a persistent accel field, so use "any active thrust
    // this frame" — Chatter is always thrusting while active.
    if (ctx.particles) {
        ctx.particles->emit(ParticleType::JETPACK, 1, obj, ctx.cosmetic_rng);
    }
}

// &48C1 update_inactive_chatter. Activates only when whistle-one set the
// timer bit 7 AND chatter_energy_reserve has been built up by feeding
// coronium crystals (each whistle activation drains one reserve unit).
void update_inactive_chatter(Object& obj, UpdateContext& ctx) {
    chatter_common(obj, ctx);

    // &48c4-&48c6: timer bit 7 is the whistle-one latch; ignore otherwise.
    if (!(obj.timer & 0x80)) return;

    // &48c8 STA &15: refill energy on each activation attempt.
    obj.energy = 0x80;

    // &48ca-&48cd: DEC reserve; BMI -> &48bd INC reserve and leave (no
    // activation). Without a reserve, no amount of whistling activates
    // Chatter — the player must feed a CORONIUM_CRYSTAL first.
    uint8_t* reserve = ctx.chatter_energy_reserve;
    if (!reserve || *reserve == 0) {
        return;
    }
    (*reserve)--;

    // &48cf JMP change_object_type — type, palette, and sprite together,
    // otherwise the active Chatter inherits the inactive cyan-blue palette
    // and looks identical to the dormant state.
    change_object_type(obj, ObjectType::ACTIVE_CHATTER);
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

    // Snapshot before Mood::update_mood — mood.cpp:212 consumes
    // WAS_DAMAGED as a one-shot stimulus, but the 6502 reads it again
    // at &4295 check_if_object_was_damaged for the squeal trigger.
    bool was_damaged = (obj.flags & ObjectFlags::WAS_DAMAGED) != 0;

    // NPC stimuli check (type 6 = responds to imps/fireballs)
    Mood::update_mood(obj, ctx);

    uint8_t mood = Mood::get_mood(obj);
    bool is_squealing = false;

    // &4295-&4298 check_if_object_was_damaged. WAS_DAMAGED is bit 3 of
    // flags (NOT state).
    if (was_damaged) {
        is_squealing = true;
    }

    // Squeal if mood is MINUS_TWO (very scared)
    if (mood == NPCMood::MINUS_TWO) {
        is_squealing = true;
    }

    // &42a8-&42ac every-8-frames enemy scan. 6502 uses &06 this_object_
    // frame_counter (slot*0x11+global) so multiple fluffies don't scan
    // on the same frames.
    uint8_t fluffy_counter = static_cast<uint8_t>(
        ctx.this_slot * 0x11 + ctx.frame_counter);
    if ((fluffy_counter & 0x0b) == 0) {
        // &42ae-&42b2 find_object_ignoring_obstructions in
        // OBJECT_RANGE_FLYING_ENEMIES (0x22..0x31): clawed robots,
        // Triax, maggot, gargoyle, all imps, birds.
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& other = ctx.mgr.object(i);
            if (!other.is_active()) continue;
            uint8_t t = static_cast<uint8_t>(other.type);
            if (t >= 0x22 && t <= 0x31) {
                int8_t dx = static_cast<int8_t>(other.x.whole - obj.x.whole);
                int8_t dy = static_cast<int8_t>(other.y.whole - obj.y.whole);
                uint8_t dist = static_cast<uint8_t>(
                    std::max(std::abs(dx), std::abs(dy)));
                // &42ba CMP &da rnd_state+1 (peek). Squeal probability
                // rises as the enemy gets closer; using rng.next here
                // would burn a fresh byte and drift the rng stream.
                if (dist < ctx.rng.peek(1)) {
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
        // &42f0-&42f4 JMP update_walking_npc_and_check_for_obstacles_with_
        // speed_A with X=2 (walking type), A=&28 (speed 40). Walking type
        // 2 is shared with imps: max_accel 0x10, weight 1, turn_prob 0xc8,
        // jump_prob 0x08 (~3% per frame).
        uint8_t target_slot = obj.target_and_flags & TargetFlags::OBJECT_MASK;
        bool avoid = (obj.target_and_flags & TargetFlags::AVOID) != 0;
        const Object& target =
            (target_slot < GameConstants::PRIMARY_OBJECT_SLOTS &&
             ctx.mgr.object(target_slot).is_active())
                ? ctx.mgr.object(target_slot)
                : player;

        int8_t dx = static_cast<int8_t>(target.x.whole - obj.x.whole);
        if (avoid) dx = static_cast<int8_t>(-dx);
        constexpr int8_t kSpeed = 0x28;
        int8_t target_vx = (dx > 0) ? kSpeed
                         : (dx < 0) ? static_cast<int8_t>(-kSpeed)
                         : 0;
        constexpr int kMaxAccel = 0x10;
        int diff = int(target_vx) - int(obj.velocity_x);
        int step = (diff >  kMaxAccel) ?  kMaxAccel
                 : (diff < -kMaxAccel) ? -kMaxAccel
                 : diff;
        obj.velocity_x = static_cast<int8_t>(int(obj.velocity_x) + step);

        // &3afc-&3b04 jump probability gate: rnd < jump_prob_table[2]
        // (= 0x08) every frame. consider_setting_npc_jumping requires the
        // NPC to be on a walkable surface — we use SUPPORTED as the
        // analog. Impulse -0x18 matches the imp port (frogman-sized hop;
        // 6502's &3a65 SBC #&0a + walking accel composes to similar).
        if (obj.is_supported() && ctx.rng.next() < 0x08) {
            obj.velocity_y = -0x18;
            obj.flags &= ~ObjectFlags::SUPPORTED;
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
    // Port-only has_left_home latch (false = still on spawn pipe, true =
    // has stepped off a PIPE tile). Lives in its own Object field — the
    // sprite cycle below (update_sprite_offset_using_velocities at &2557)
    // writes obj.timer = (timer+1+speed) % 0x0c every frame, so storing
    // the latch in timer corrupted it (despawn fires when timer==1).
    if (obj.flags & ObjectFlags::NEWLY_CREATED) {
        obj.state = NPCMood::MINUS_TWO;
        obj.has_left_home = false;
    }

    // &44f9-&4504: speed from mood (excited 0x28, neutral 0x10).
    uint8_t mood = Mood::get_mood(obj);
    int8_t speed = (mood != NPCMood::ZERO) ? 0x28 : 0x10;

    // &4506-&450b: convert object type into NPC stimuli index
    // (type − OBJECT_RED_MAGENTA_IMP). 0..4 indexes all five variants.
    uint8_t tidx = static_cast<uint8_t>(obj.type) -
                   static_cast<uint8_t>(ObjectType::RED_MAGENTA_IMP);
    if (tidx >= 5) tidx = 0;

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
        // Latch the "has left home" marker the first frame we see a
        // non-PIPE tile under the imp. Without this, a freshly-spawned
        // imp whose physics step set SUPPORTED on frame 1 would trip
        // the at-home despawn before it ever walks out of the pipe.
        if (home_type != static_cast<uint8_t>(TileType::PIPE)) {
            obj.has_left_home = true;
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
            // Approximate with is_supported() AND not NEWLY_CREATED — the
            // 6502 flag fires only on the airborne→ground transition, so
            // an imp spawned centred on a PIPE tile (which physics reports
            // as supported on frame 1) wouldn't trigger it. Without the
            // newly-created guard every nest-spawned imp despawns on the
            // same frame it appears.
            bool landed = obj.is_supported() &&
                          !(obj.flags & ObjectFlags::NEWLY_CREATED);

            // &452b-&453f: home-despawn fires for BOTH fed and unfed imps
            // in the 6502 (the unfed BEQ at &452c skips the gift spawn but
            // still falls through to &453f set_object_as_far_away). Gate
            // on has_left_home so a freshly-spawned imp doesn't vanish
            // before walking out of the pipe.
            if (landed && obj.has_left_home) {
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
                ctx.mgr.return_to_tertiary(ctx.this_slot);
                obj.flags |= ObjectFlags::PENDING_REMOVAL;
                return;
            }
        }
    }

    // &4542-&4548: minimum energy per type. Reached only via the
    // not_home branch in the 6502, so kept after the at-home block.
    NPC::enforce_minimum_energy(obj, imp_minimum_energy[tidx]);

    // &4548 check_for_npc_stimuli food absorption (not-home branch only,
    // per the BNE &4542 at &4515). An imp can NOT eat and drop a gift on
    // the same frame — the at-home branch returns before reaching here.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& touched = ctx.mgr.object(obj.touching);
        if (touched.is_active() &&
            static_cast<uint8_t>(touched.type) == imp_food_type[tidx]) {
            touched.flags |= ObjectFlags::PENDING_REMOVAL;
            obj.state |= kNPC_WAS_FED;
        }
    }

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
        NPC::damage_player_if_touching(obj, ctx.mgr.player(), 5, ctx.damage_events, &ctx);
    }

    // &45c7-&45d3 find_a_target_and_fire_at_it_with_likelihood_A_divided_by_four(A=8).
    // Threshold = (8>>2)+2 = 4 -> ~5/256 random gate per frame. Then the 6502
    // pipeline gates on (a) LOS to player via find_object's obstruction check
    // (&3cb2-&3cbd) and (b) distance < 16 tiles + facing-direction match in
    // fire_at_target's vector math (&335a / &27a3). Without (a) and (b) blue/cyan
    // imps in a pipe fired coronium crystals through walls.
    bool not_at_target = (obj.touching != 0);
    if (not_at_target && ctx.rng.next() < 5) {
        const Object& player = ctx.mgr.player();
        // (a) LOS to player. has_line_of_sight_randomized mirrors find_object's
        // ((rnd & 0x4f) ^ 0xff) cap when nearest_object_distance is unset, so
        // the cap lands in 0xb0..0xff — effectively "any tile obstruction
        // blocks". Same helper update_clawed_robot uses for the player aim.
        bool los = NPC::has_line_of_sight_randomized(
            obj, /*target_slot=*/0, ctx);

        // (b1) distance < 16 tiles per axis. Mirrors &335a relative_tiles_log
        // >= 6 -> leave; calculate_firing_vector_from_distance bails out
        // before any projectile is spawned when this fails.
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
        int adx = std::abs(static_cast<int>(dx));
        int ady = std::abs(static_cast<int>(dy));
        bool in_range = (adx < 16) && (ady < 16);

        // (b2) facing-direction match. &27a3 EOR x_flip / BPL leave_with_
        // carry_clear: if the firing vector x sign mismatches the imp's
        // x_flip, the projectile would fly backwards — skip and let the
        // walker turn the imp first.
        bool facing_left = obj.is_flipped_h();
        bool target_left = (dx < 0);
        bool forwards = (facing_left == target_left);

        uint8_t proj_type = imp_projectile_type[tidx];
        if (los && in_range && forwards &&
            proj_type < static_cast<uint8_t>(ObjectType::COUNT)) {
            int slot = NPC::fire_projectile(
                obj, static_cast<ObjectType>(proj_type), ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                int total = adx + ady;
                if (total == 0) total = 1;
                constexpr int kMag = 0x30;
                int vx = kMag * adx / total;
                int vy = kMag * ady / total;
                b.velocity_x = static_cast<int8_t>(dx >= 0 ? vx : -vx);
                b.velocity_y = static_cast<int8_t>(dy >= 0 ? vy : -vy);
                // CORONIUM_CRYSTAL inherits the collectable undisturbed pin
                // (bit 7 of energy) from init_object_from_type; clear it so
                // step 15 doesn't zero our launch velocity.
                b.energy &= 0x7f;
                // RED_BULLET uses common_bullet_update's timer countdown
                // (port-only — 6502 reads energy from the range table).
                // Other imp projectiles drive their own timers.
                if (proj_type == static_cast<uint8_t>(ObjectType::RED_BULLET)) {
                    b.timer = 0x40;
                }
                NPC::offset_child_from_parent(b, obj);
            }
        }
    }

    // Face direction of movement.
    NPC::face_movement_direction(obj);

    // &45d6-&45e6: sprite from |velocity|/8 mod 0x0c, then LSR/LSR to
    // collapse to 0..2 across SPRITE_IMP_WALKING_ONE..THREE (base 0x64).
    // Still imp at velocity_x == 0 uses the walking-one frame directly.
    if (obj.velocity_x == 0) {
        NPC::change_object_sprite_to_base_plus_A(obj, 0);
    } else {
        uint8_t off = NPC::update_sprite_offset_using_velocities(
            obj, 0x0c, /*divide_shift=*/3);
        off >>= 2;
        NPC::change_object_sprite_to_base_plus_A(obj, off);
    }

    // &45f0-&460f: scream at fixed pitch 0xa5 when WAS_DAMAGED (>=8 in
    // a single hit), otherwise once every 16 frames roll a 50% call with
    // state-mixed random pitch. imp_sound_parameters bytes 0..2 are
    // fixed; byte 3 is the patched pitch.
    uint8_t pitch = 0;
    bool play = false;
    if (obj.flags & ObjectFlags::WAS_DAMAGED) {
        pitch = 0xa5;
        play = true;
    } else if ((ctx.frame_counter & 0x0f) == 0) {
        uint8_t r = ctx.rng.next();
        // 6502 LSR/ROR: bit 7 of result = bit 0 of r → 50% gate.
        if (r & 0x01) {
            uint8_t a = static_cast<uint8_t>((r >> 2) | 0x80);
            a ^= obj.state;
            a &= 0xe0;
            a >>= 1;
            a |= 0x05;
            pitch = a;
            play = true;
        }
    }
    if (play) {
        const uint8_t imp_sound[4] = { 0x9c, 0x05, 0xa6, pitch };
        Audio::play_at(Audio::CH_ANY, imp_sound, obj.x.whole, obj.y.whole);
    }
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
        NPC::damage_player_if_touching(obj, ctx.mgr.player(), damage, ctx.damage_events, &ctx);
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
// Bit 7 of palette is our visibility flag (matches update_invisible_bird).
void update_invisible_frogman(Object& obj, UpdateContext& ctx) {
    obj.visible = false;  // &4475 LSR &2b unconditional
    update_green_frogman(obj, ctx);
}

// Port of &47c9 update_red_slime. Stationary, harmful on contact, drips
// RED_DROP particles. Sprite cycles 3-2-1-0-0-1-2-3 over 16 frames via
// abs((frame16 / 2) - 4). Drop initial offset at &47e2: x_frac=0x30/0x90
// per slime's h-flip, y_frac=0x40, vy=4.
void update_red_slime(Object& obj, UpdateContext& ctx) {
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 3, ctx.damage_events, &ctx);
    NPC::enforce_minimum_energy(obj, 0x7f);

    uint8_t frame16 = ctx.frame_counter & 0x0f;
    int sprite_offset;
    if (frame16 == 0) {
        // 1-in-2 chance per 16 frames of dripping a RED_DROP. 6502 reads
        // bit 7 of rnd_state+1 directly via BIT &da (peek, no advance).
        if (ctx.rng.peek(1) & 0x80) {
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
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 2, ctx.damage_events, &ctx);
    NPC::face_movement_direction(obj);
    NPC::enforce_minimum_energy(obj, 0x3f);
    // If absorbs coronium crystal, becomes yellow slime
}

// &4266: Yellow slime - can be picked up
void update_yellow_slime(Object& obj, UpdateContext& ctx) {
    // Yellow slime is heavier, doesn't move much
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 1, ctx.damage_events, &ctx);
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

// &4e5e update_worm_or_maggot common body. Damage is loaded by the caller
// (X=&00 for worm at &420e, X=&14 for maggot at &4e5c) so the shared
// routine doesn't hardcode it. Port currently uses a simplified "move
// toward player" instead of the 6502 burrow path; the type/avoid/damage
// signature is preserved.
static void update_worm_or_maggot(Object& obj, UpdateContext& ctx,
                                  uint8_t damage) {
    if (ctx.every_eight_frames) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
        if (dx > 0) obj.velocity_x = 2;
        else if (dx < 0) obj.velocity_x = -2;
        obj.velocity_y = (dy > 0) ? 2 : -2;
    }
    if (damage > 0) {
        NPC::damage_player_if_touching(obj, ctx.mgr.player(), damage,
                                       ctx.damage_events, &ctx);
    }
    NPC::face_movement_direction(obj);

    // &4e96-&4eb3 squeal pair: skip unless target has been seen, and
    // scale frequency by distance — threshold = (0x0f ^ dist), play when
    // threshold >= rnd. Effectively silent past ~10 tiles even though
    // play_at allows 16; matches the 6502 fall-off.
    if (obj.target_and_flags & TargetFlags::DIRECTNESS_TWO) {
        const Object& player = ctx.mgr.player();
        int8_t ddx = static_cast<int8_t>(obj.x.whole - player.x.whole);
        int8_t ddy = static_cast<int8_t>(obj.y.whole - player.y.whole);
        uint8_t adx = ddx < 0 ? -ddx : ddx;
        uint8_t ady = ddy < 0 ? -ddy : ddy;
        uint8_t dist = adx > ady ? adx : ady;
        if (dist < 0x0f) {
            uint8_t threshold = static_cast<uint8_t>(dist ^ 0x0f);
            if (threshold >= ctx.rng.next()) {
                static constexpr uint8_t kSoundWormA[4] = { 0x33, 0xf3, 0x09, 0xb4 };
                static constexpr uint8_t kSoundWormB[4] = { 0x33, 0xf3, 0x07, 0xb5 };
                Audio::play_at(Audio::CH_ANY, kSoundWormA, obj.x.whole, obj.y.whole);
                Audio::play_at(Audio::CH_ANY, kSoundWormB, obj.x.whole, obj.y.whole);
            }
        }
    }

    // Random chance to despawn
    if (ctx.rng.next() == 0) {
        obj.energy = 0;
    }
}

// &420a update_worm: damage=0 (worms don't hurt on touch, they burrow).
void update_worm(Object& obj, UpdateContext& ctx) {
    update_worm_or_maggot(obj, ctx, 0);
}

// &4e52 update_maggot: damage=20 (X=&14 at &4e5c).
void update_maggot(Object& obj, UpdateContext& ctx) {
    update_worm_or_maggot(obj, ctx, 0x14);
}

// &4F21 update_piranha_or_wasp. Single routine; is_wasp branch flips
// gravity, default target (LARGE_HIVE vs SMALL_HIVE), and element gate.
// Aggressiveness stored in obj.state.
void update_piranha_or_wasp(Object& obj, UpdateContext& ctx) {
    const bool is_wasp = (obj.type == ObjectType::WASP);

    // &4f2b-&4f33: piranha STA accel_y=4, falls through to &4f31 DEC
    // accel_y -> net +3 (overwrites gravity, not added to it). Wasp
    // skips STA, goes straight to DEC -> -1 (cancels &1f01 +1 gravity).
    if (is_wasp) {
        NPC::cancel_gravity(obj);
    } else {
        NPC::cancel_gravity(obj);
        if (obj.velocity_y < 127 - 3) obj.velocity_y += 3;
    }

    // &4f33-&4f3b 1-in-2 retarget. BIT &db / BVS skip means "retarget
    // when bit 6 of rnd_state+2 is CLEAR"; CPX &da / BCS find_hive means
    // "target player when state < rnd_state+1". Both are peeks. Home-
    // hive fallback: leave target_and_flags as set by update_hive.
    if (!(ctx.rng.peek(2) & 0x40)) {
        bool target_player = obj.state < ctx.rng.peek(1);
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
        NPC::damage_player_if_touching(obj, ctx.mgr.player(), 24, ctx.damage_events, &ctx);
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
                                       ctx.damage_events, &ctx);
    }

    // &464a-&4652: clamp minimum energy. All four birds use 0, so this
    // is effectively a no-op today — kept for faithfulness.
    NPC::enforce_minimum_energy(obj, birds_energy_table[tidx]);

    // &4654-&4659 visibility on damage. WAS_DAMAGED flag approximates the
    // 6502's &253c check; invisible birds read obj.state to reveal.
    if (obj.flags & ObjectFlags::WAS_DAMAGED) {
        obj.state = 0x80;   // non-zero -> visible
    }

    // &4659 BNE skips the sprite + path block when state != 0 (just-
    // damaged or invisible bird that just revealed). Gate on it.
    if (obj.state == 0) {
        // &465b-&4668 sprite cycle: |v|%0x14 >> 2, 4 -> 2 so fast
        // movement still dips through the middle pose.
        uint8_t off = NPC::update_sprite_offset_using_velocities(obj, 0x14);
        off >>= 2;
        if (off == 4) off = 2;
        NPC::change_object_sprite_to_base_plus_A(obj, off);

        // &466b-&4670: eat any wasp touching, then aim at the next one.
        NPC::consider_absorbing_object_touched(obj, ObjectType::WASP, ctx.mgr);
        NPC::consider_finding_target(obj, ObjectType::WASP, ctx);

        // &467a-&4686: NPC path + move toward target (mag 0x40, max
        // accel 8, 1-in-4 probability).
        NPC::move_towards_target_with_probability(obj, ctx, 0x40, 8, 0x40);

        // &4686 DEC accel_y cancels gravity (post-path re-applied it).
        NPC::cancel_gravity(obj);
    }

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
// play_whistle_two_sound at &2c9e plays the high-note block, stamps
// &29d8 with this_object, then falls through to &2cb4 for the shared
// low-note block — the Chatter LOS check at &4933 reads &29d8.
void update_red_magenta_bird(Object& obj, UpdateContext& ctx) {
    // &4621 LDA &da rnd_state+1 (peek, no advance).
    if (ctx.rng.peek(1) == 0) {
        static constexpr uint8_t kSoundWhistleHigh[4] = { 0xb0, 0x24, 0xb6, 0xe2 };
        static constexpr uint8_t kSoundWhistleLow[4]  = { 0xb0, 0x24, 0xb6, 0xb3 };
        Audio::play_at(Audio::CH_ANY, kSoundWhistleHigh, obj.x.whole, obj.y.whole);
        Audio::play_at(Audio::CH_ANY, kSoundWhistleLow,  obj.x.whole, obj.y.whole);
        // &2ca5-&2ca7 STA &29d8 with this_object. Chatter's &4933 path
        // reads this slot and tracks LOS toward it to drop a power pod.
        if (ctx.whistle_two_activator) {
            *ctx.whistle_two_activator = static_cast<uint8_t>(ctx.this_slot);
        }
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
    // &462b-&462f: state == 0 means not recently damaged; clear visibility
    // (default true from &1ae1 reset) so the renderer skips the draw.
    if (obj.state == 0) obj.visible = false;
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

    // &4170-&4175: fire when (freq_mask & this_object_frame_counter) == 0.
    // 6502 uses &06 (slot*0x11 + global) — phases per-slot so multiple
    // gargoyles don't fire in lockstep.
    uint8_t per_obj_counter = static_cast<uint8_t>(
        ctx.this_slot * 0x11 + ctx.frame_counter);
    if ((kFreqMask[type] & per_obj_counter) == 0) {
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
            // &33b4 JSR &331d folds the firer's vx into vector_x and
            // clamps the magnitude (cap is 0x50 or |firer.vx|+0x20).
            // Without this, vx=0x7f bullets fly too flat — the angle to
            // vy=0x0c is ~5° instead of the 6502's ~8.5°.
            proj.velocity_x = NPC::apply_firing_vx_clamp(vx, obj.velocity_x);
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
    // Triax intro diagnostics: position, velocity, destinator pos + AABB
    // overlap, to see whether Triax can ever physically reach the
    // destinator or is blocked by a solid tile above it.
    int triax_x_abs = (int)obj.x.whole * 256 + (int)obj.x.fraction;
    int triax_y_abs = (int)obj.y.whole * 256 + (int)obj.y.fraction;
    int triax_right = triax_x_abs + 64;   // SPACESUIT_VERTICAL (w-1)*16
    int triax_bot   = triax_y_abs + 168;  // SPACESUIT_VERTICAL (h-1)*8
    int dest_slot = -1, dest_x_abs = 0, dest_y_abs = 0;
    int dest_right = 0, dest_bot = 0;
    for (int s = 1; s < GameConstants::PRIMARY_OBJECT_SLOTS; ++s) {
        Object& o = ctx.mgr.object(s);
        if (o.type == ObjectType::DESTINATOR && o.is_active()) {
            dest_slot = s;
            dest_x_abs = (int)o.x.whole * 256 + (int)o.x.fraction;
            dest_y_abs = (int)o.y.whole * 256 + (int)o.y.fraction;
            dest_right = dest_x_abs + 112; // CONSOLE (w-1)*16
            dest_bot   = dest_y_abs + 88;  // CONSOLE (h-1)*8
            break;
        }
    }
    ctx.mgr.log_diag(
        "TRIAX f=%u pos=(%02x.%02x,%02x.%02x) v=(%d,%d) touch=%02x "
        "e=%02x fl=%02x tm=%02x bot=0x%04x right=0x%04x",
        ctx.frame_counter,
        obj.x.whole, obj.x.fraction, obj.y.whole, obj.y.fraction,
        (int)obj.velocity_x, (int)obj.velocity_y, obj.touching,
        obj.energy, obj.flags, obj.timer, triax_bot, triax_right);
    if (dest_slot >= 0) {
        bool xov = (triax_right > dest_x_abs) && (triax_x_abs < dest_right);
        bool yov = (triax_bot   > dest_y_abs) && (triax_y_abs < dest_bot);
        ctx.mgr.log_diag(
            "  DEST p%d at=0x%04x,0x%04x bot=0x%04x right=0x%04x "
            "y_gap=%d xov=%d yov=%d",
            dest_slot, dest_x_abs, dest_y_abs, dest_bot, dest_right,
            dest_y_abs - triax_bot, (int)xov, (int)yov);
    } else {
        ctx.mgr.log_diag("  DEST not in primary table");
    }

    // &4704-&4710 absorb destinator -> re-arm tertiary &9d so it
    // respawns at (0x64, 0xd6) in the lab, then teleport away.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& target = ctx.mgr.object(obj.touching);
        ctx.mgr.log_diag("TRIAX touching slot=%02x type=%02x",
                         obj.touching, (uint8_t)target.type);
        if (target.type == ObjectType::DESTINATOR) {
            target.flags |= ObjectFlags::PENDING_REMOVAL;
            ctx.mgr.set_tertiary_data_byte(0x9d, 0x80);
            triax_teleport_away(obj);
            ctx.mgr.log_diag("TRIAX absorbed destinator -> teleport away");
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
            ctx.mgr.log_diag("TRIAX pre-seek v=(%d,%d)",
                             (int)obj.velocity_x, (int)obj.velocity_y);
            NPC::seek_player(obj, player, 8);
            ctx.mgr.log_diag("TRIAX post-seek v=(%d,%d) (seek_player "
                             "OVERWRITES velocity — vs 6502 accel-based)",
                             (int)obj.velocity_x, (int)obj.velocity_y);
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
