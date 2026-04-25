#include "behaviours/collectable.h"
#include "objects/object_data.h"
#include "particles/particle_system.h"
#include "core/types.h"
#include "world/water.h"
#include <algorithm>
#include <cstdlib>

namespace Behaviors {

// &4B88: Generic collectable item (keys, weapons, equipment, etc.)
// Port of update_collectable_object.
//
// `energy` is overloaded as a "disturbed" flag — bit 7 SET means
// undisturbed (still pinned to spawn). The 6502 uses ASL/LSR at &4ba1
// to clear that bit when something touches the object, then BIT/BPL at
// &4ba5 to skip the rest of the routine if disturbed. Our previous
// version had this inverted: it SET bit 7 on touch, leaving the pin
// behaviour permanently active and letting collectables drift under
// gravity / tile collision (then demote to secondary, then promote, and
// so on — over time the secondary pool fills with one entry per tile
// the player has wandered through).
//
// Faithful behaviour:
//   - If the player is currently holding this object, mark it collected
//     and remove it (set_object_for_removal &2529 → PENDING_REMOVAL).
//   - If something other than the player is touching us, clear bit 7
//     of energy ("disturbed").
//   - If still undisturbed, zero our velocity and snap back to the
//     previous frame's position — the collectable is glued to its tile.
//   - If disturbed, fall through and let the main physics step take
//     over (gravity, water, tile collision).
void update_collectable(Object& obj, UpdateContext& ctx) {
    // The 6502's update_collectable_object at &4b88 auto-collects (sets
    // PENDING_REMOVAL + decrements player_collected[type]) whenever the
    // item is in the player's held slot. That path doesn't map cleanly
    // onto our S/R pocket model — retrieving an item from a pocket
    // drops it straight into the held slot, so an auto-collect fires
    // next frame and the item vanishes the moment R is pressed.
    //
    // Port approach: skip the auto-collect for general collectables so
    // the S/R pocket mechanism stays coherent. Whistles are the one
    // exception — the 6502 treats them as permanent player state
    // (player_whistle_*_collected at &0816/&0817) rather than a primary
    // you keep in hand, so auto-collect matches the original here:
    // touching the whistle flips the "have whistle" flag and the primary
    // disappears, leaving Y/U keys enabled for the rest of the run.
    bool held_by_player =
        ctx.held_object_slot == static_cast<uint8_t>(ctx.this_slot);
    if (held_by_player) {
        if (obj.type == ObjectType::WHISTLE_ONE && ctx.whistle_one_collected) {
            *ctx.whistle_one_collected = true;
            obj.flags |= ObjectFlags::PENDING_REMOVAL;
            return;
        }
        if (obj.type == ObjectType::WHISTLE_TWO && ctx.whistle_two_collected) {
            *ctx.whistle_two_collected = true;
            obj.flags |= ObjectFlags::PENDING_REMOVAL;
            return;
        }
        // Keys behave like whistles: auto-collect into
        // player_keys_collected (port of &0806) and consume the primary.
        // The door-unlock path (update_door's &4c9e hook) later reads
        // the bitmask to decide whether the RCD can toggle a matching
        // door's LOCKED flag. Keys skip the pocket stack entirely — this
        // matches the 6502, where player_collected[key_type] is stamped
        // directly at pickup rather than occupying an inventory slot.
        if (ctx.player_keys_collected) {
            int key_index = -1;
            switch (obj.type) {
                case ObjectType::CYAN_YELLOW_GREEN_KEY: key_index = 0; break;
                case ObjectType::RED_YELLOW_GREEN_KEY:  key_index = 1; break;
                case ObjectType::GREEN_YELLOW_RED_KEY:  key_index = 2; break;
                case ObjectType::YELLOW_WHITE_RED_KEY:  key_index = 3; break;
                case ObjectType::RED_MAGENTA_RED_KEY:   key_index = 4; break;
                case ObjectType::BLUE_CYAN_GREEN_KEY:   key_index = 5; break;
                default: break;
            }
            if (key_index >= 0) {
                ctx.player_keys_collected[key_index] = 0x80;
                obj.flags |= ObjectFlags::PENDING_REMOVAL;
                return;
            }
        }
    }

    // &4b9d-&4ba3: any non-self touch clears bit 7 of energy (the
    // "undisturbed" pin). The 6502 does ASL/LSR rather than AND #&7f,
    // but the visible effect is the same.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        obj.energy &= 0x7f;
    }

    // &4ba5-&4ba7: if undisturbed, pin position by zeroing velocity.
    // Net effect is the object stays put on its tile until touched.
    bool undisturbed = (obj.energy & 0x80) != 0;
    if (undisturbed) {
        obj.velocity_x = 0;
        obj.velocity_y = 0;
    }
}

// &4158 update_inactive_grenade. Port of:
//   JSR consider_disturbing_object  (&4b9d — same pin/disturb bookkeeping
//                                    that update_collectable runs)
//   JSR check_if_object_fired       (&0bbf — returns Z=1 if the object
//                                    was just launched via a turret etc.;
//                                    unhandled in our port yet)
//   if A == player_object_held → state = 1 (has-been-held)
//   else if state != 0         → change_object_type to ACTIVE_GRENADE
//
// Net effect: the first time the player picks up an inactive grenade,
// obj.state flips to 1. The moment they drop it (or R-retrieve then
// swap), the next update sees "not held AND state != 0" and promotes
// the primary to ACTIVE_GRENADE, whose update routine handles the
// ticking-down countdown and eventual explosion.
void update_inactive_grenade(Object& obj, UpdateContext& ctx) {
    // &4158 consider_disturbing_object: pin-while-undisturbed behaviour.
    update_collectable(obj, ctx);

    // &4160-&4164: "is the player holding this particular object?".
    // held_object_slot == this_slot means the player's hands are on it.
    // Latch state = 1 so a future drop knows the grenade was handled.
    bool held_by_player =
        ctx.held_object_slot == static_cast<uint8_t>(ctx.this_slot);
    if (held_by_player) {
        obj.state = 1;
        return;
    }

    // &4167-&4169: not currently held. If never held (state still 0),
    // the grenade is just sitting around — leave it alone so the player
    // can pick up and pocket it safely.
    if (obj.state == 0) return;

    // &416b-&416d: was held before, now dropped → promote to
    // ACTIVE_GRENADE. change_object_type refreshes sprite + palette
    // from the per-type tables. update_active_grenade takes over from
    // here (countdown + explosion).
    obj.type    = ObjectType::ACTIVE_GRENADE;
    obj.sprite  = object_types_sprite[
        static_cast<uint8_t>(ObjectType::ACTIVE_GRENADE)];
    obj.palette = object_types_palette_and_pickup[
        static_cast<uint8_t>(ObjectType::ACTIVE_GRENADE)] & 0x7f;
    // Carry any "I was just thrown" velocity over unchanged; the active
    // grenade's update will start its fuse timer on the next frame.
}

// &4360: Power pod — limited lifespan; pulses visibly and audibly twice
// every 16 frames (when frame_counter_sixteen < 2). When its energy
// finally hits zero the main loop's step-12 explosion path takes over
// (6502 explosion type for OBJECT_POWER_POD is "turn into fireball",
// from the &0491 &85 entry at line 619 of the disassembly).
//
// Port of &4360-&4373:
//   JSR reduce_energy_by_one
//   LDA frame_counter_sixteen; CMP #&02
//   JSR use_damaged_palette_if_carry_clear ; damaged palette when fc16 < 2,
//                                          ; default palette restored
//                                          ; otherwise. ALWAYS writes the
//                                          ; palette — the "flash" is a
//                                          ; 2-in-16 inverted colour-3.
//   BCS leave                                ; skip sound otherwise
//   play pulsing sound
//
// Deliberately does NOT call update_collectable — the 6502 skips
// consider_disturbing_object for power pods (they're free-floating /
// thrown projectiles, not pinned tile objects).
void update_power_pod(Object& obj, UpdateContext& ctx) {
    // &4360 reduce_energy_by_one. Energy==0 hits the main loop's
    // explosion branch after this routine returns.
    if (obj.energy > 0) obj.energy--;
    if (obj.energy == 0) return;

    // &4363-&4367: fc16 = this_object_frame_counter_sixteen; compare
    // against 2; feed carry into use_damaged_palette_if_carry_clear.
    // Our global frame_counter is close enough for a synced flash.
    uint8_t fc16 = ctx.frame_counter & 0x0f;
    bool carry_clear = (fc16 < 2);

    // &4ddf use_damaged_palette_if_carry_clear: always reset palette to
    // the object type's default, then XOR #&30 when carry was clear.
    // This is important — the 6502 restores the palette each frame, so
    // our previous "XOR on toggle" implementation was wrong: it let
    // adjacent flash frames cancel and left the pod permanently tinted
    // once fc16 crossed the threshold.
    uint8_t idx = static_cast<uint8_t>(obj.type);
    uint8_t base_palette = object_types_palette_and_pickup[idx] & 0x7f;
    obj.palette = carry_clear ? static_cast<uint8_t>(base_palette ^ 0x30)
                              : base_palette;
    // TODO: play pulsing sound (&436c) when audio is ported.
    (void)ctx;
}

// &4374: Destinator - key item for completing the game
void update_destinator(Object& obj, UpdateContext& ctx) {
    update_collectable(obj, ctx);
    // Flash more vigorously
    if (ctx.every_four_frames) {
        obj.palette ^= 0x04;
    }
}

// &43A7 update_empty_flask. Tiny routine: if the flask is below the
// waterline (`this_object_in_water` bit 7 set), change type to
// FULL_FLASK. Otherwise fall through as an inert collectable — the
// 6502 explicitly reuses update_inert_object semantics here (no
// motion, no pickup detection beyond the standard collectable path).
// We skip the inert-body call; our main update loop handles gravity
// and tile collision for EMPTY_FLASK naturally.
void update_empty_flask(Object& obj, UpdateContext& ctx) {
    (void)ctx;
    // Must check against the actual per-column waterline, not
    // NPC::is_underwater's SURFACE_Y (0x4e) shortcut — that's the
    // upper-world ceiling, and flagging any flask with y > 0x4e as
    // submerged makes empty flasks transmute to full the instant they
    // spawn anywhere on the playfield. 6502 reads `this_object_in_water`
    // at &1f, computed per-column from the waterline table.
    if (Water::is_underwater(ctx.landscape, obj.x.whole, obj.y.whole)) {
        // &43e4 change_object_type: the 6502 also refreshes sprite +
        // palette from the per-type tables, which we mirror so the
        // flask instantly reflects its full-state colour.
        obj.type    = ObjectType::FULL_FLASK;
        obj.sprite  = object_types_sprite[
            static_cast<uint8_t>(ObjectType::FULL_FLASK)];
        obj.palette = object_types_palette_and_pickup[
            static_cast<uint8_t>(ObjectType::FULL_FLASK)] & 0x7f;
    }
}

// &43AE update_full_flask. Port of the full routine.
//
// Trigger-to-empty rules (&43ae-&43bb):
//   - Touching another object AND larger of |vx|,|vy| >= 0x0a — the
//     flask hit something hard → start emptying.
//   - OR `this_object_pre_collision_velocity_magnitude` >= 0x14 — the
//     flask hit a tile hard. We don't track that byte yet, so
//     approximate with the current-velocity check (the update_fn runs
//     before physics-step integration, so velocity here is the
//     would-be velocity for this frame; good enough).
//
// While emptying (timer != 0):
//   - If touching a FIREBALL, set_object_for_removal on the fireball
//     (&43d0) — splashing water douses fire.
//   - Emit 8 PARTICLE_FLASK with angle=0xc0 (upward). The 6502 adds
//     them all via add_particles; our helper takes a count directly.
//   - Decrement timer. On reaching 0, change_object_type back to
//     EMPTY_FLASK (&43e2-&43e4).
void update_full_flask(Object& obj, UpdateContext& ctx) {
    bool start_emptying = false;

    // &43b0-&43b5: touching something, and still moving fast enough
    // (max(|vx|,|vy|) >= 0x0a) → disturbed.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        uint8_t max_vel = static_cast<uint8_t>(
            std::max(std::abs(static_cast<int>(obj.velocity_x)),
                     std::abs(static_cast<int>(obj.velocity_y))));
        if (max_vel >= 0x0a) start_emptying = true;
    }

    // &43b7-&43bb: hit a tile hard.
    //   LDA this_object_pre_collision_velocity_magnitude ; &1d
    //   CMP #&14 ; BCC skip_starting_timer
    // `pre_collision_magnitude` is captured by the physics revert in
    // object_update before the bounce/damp pass — the raw velocity at the
    // moment of impact. Post-revert velocity isn't usable here: a modest
    // fall lands at ~0x10, bounces to ~0x0c via bounce_reflect, and the
    // previous 0x08 fallback fired on every landing — water got knocked
    // out of the flask even when it was gently placed on a ledge.
    if (obj.pre_collision_magnitude >= 0x14) {
        start_emptying = true;
    }

    // &43bd-&43bf: arm 16-frame emptying countdown. Re-arming while
    // already counting down is harmless (the 6502 STAs unconditionally).
    if (start_emptying) obj.timer = 0x10;

    // &43c1-&43c3: if not emptying, leave — rest of routine is skipped.
    if (obj.timer == 0) return;

    // &43c5-&43d0: if touching a fireball, extinguish it. The 6502
    // calls set_object_for_removal (&2516) which sets PENDING_REMOVAL
    // for the reaper; we do the same so the fireball disappears next
    // frame through the normal flow.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& touched = ctx.mgr.object(obj.touching);
        if (touched.type == ObjectType::FIREBALL ||
            touched.type == ObjectType::MOVING_FIREBALL) {
            touched.flags |= ObjectFlags::PENDING_REMOVAL;
        }
    }

    // &43d3-&43db: emit 8 PARTICLE_FLASK moving upward. add_particles
    // handles random spread per type; our particle emitter inherits
    // the flask's velocity as base, which is close to the 6502's
    // angle=0xc0 fixed-upward behaviour once FLASK type's y-rand
    // (&025e) is applied.
    if (ctx.particles) {
        ctx.particles->emit(ParticleType::FLASK, 8, obj, ctx.rng);
    }

    // &43de-&43e4: countdown, then become empty.
    obj.timer--;
    if (obj.timer == 0) {
        obj.type    = ObjectType::EMPTY_FLASK;
        obj.sprite  = object_types_sprite[
            static_cast<uint8_t>(ObjectType::EMPTY_FLASK)];
        obj.palette = object_types_palette_and_pickup[
            static_cast<uint8_t>(ObjectType::EMPTY_FLASK)] & 0x7f;
    }
}

// &4351 update_remote_control_device.
//
// The 6502 boils this down to four lines:
//   JSR check_if_object_fired  (&0bbf; Z=1 iff player_object_fired == this)
//   BNE leave
//   play_sound                 (audio not ported)
//   JMP create_aim_particle    (&312b — emit one PARTICLE_AIM at the
//                                       object's aiming angle)
// When the player presses Fire while holding the RCD, `player_object_
// fired` is set to the held slot (see apply_player_input) — that's the
// signal this routine uses to know "I was just activated".
//
// Doors, transporters and the cannon (via &4c9e / &4dc8 /
// check_if_object_hit_by_remote_control at &0bc5) all read the same
// flag to detect "player aimed the RCD at me this frame".
void update_control_device(Object& obj, UpdateContext& ctx) {
    update_collectable(obj, ctx);

    // check_if_object_fired (&0bbf) — this_object == player_object_fired?
    if (ctx.player_object_fired != static_cast<uint8_t>(ctx.this_slot)) {
        return;
    }

    // &435d JMP create_aim_particle: one PARTICLE_AIM per firing frame.
    // The 6502 also flips horizontally to "put aim particles on same
    // side as player's face" (&312e) — our particle emitter inherits
    // the object's flip state via emit so it falls out naturally.
    if (ctx.particles) {
        ctx.particles->emit(ParticleType::AIM, 1, obj, ctx.rng);
    }
}

// Shared coronium behavior (port of &41ca-&4209)
// Both boulders and crystals cause chain-reaction explosions on contact,
// radiation damage to the player, and glow with random palettes.
static void coronium_common(Object& obj, UpdateContext& ctx) {
    // Check if touching another coronium object -> chain explosion (&41ca-&41e8)
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS && obj.touching != 0) {
        Object& other = ctx.mgr.object(obj.touching);
        if (other.type == ObjectType::CORONIUM_BOULDER ||
            other.type == ObjectType::CORONIUM_CRYSTAL) {
            // Remove the other coronium object
            uint8_t other_weight = other.weight();
            ctx.mgr.remove_object(obj.touching);

            // Explosion duration = (this_weight + other_weight) * 2 + 3
            // Boulder weight=5, crystal weight=2
            uint8_t duration = (obj.weight() + other_weight) * 2 + 3;

            // Create explosion at this location
            int slot = ctx.mgr.create_object_at(ObjectType::EXPLOSION, 0, obj);
            if (slot >= 0) {
                ctx.mgr.object(slot).tertiary_data_offset = duration;
            }

            // This object is also consumed
            obj.energy = 0;
            return;
        }
    }

    // Radiation damage to player (&41eb-&4203)
    // If touching player directly, always damage.
    // If player is holding this object, 1 in 4 chance per frame.
    bool touching_player = (obj.touching == 0);
    bool player_holding = false;

    // Approximate held-check: player at adjacent position with same velocity
    const Object& player = ctx.mgr.player();
    int8_t dx = static_cast<int8_t>(obj.x.whole - player.x.whole);
    int8_t dy = static_cast<int8_t>(obj.y.whole - player.y.whole);
    if (std::abs(dx) <= 1 && std::abs(dy) <= 1 &&
        obj.velocity_x == player.velocity_x) {
        player_holding = true;
    }

    if (touching_player || (player_holding && (ctx.rng.next() & 0xc0) == 0)) {
        // Check radiation immunity (player_radiation_immunity_pill_collected)
        // and whether coronium is underwater (radiation blocked by water).
        // Per-column waterline — the SURFACE_Y shortcut would flag the
        // whole lower world as "submerged" and silently suppress all
        // coronium damage even in air pockets.
        bool immune = false; // Would check player inventory
        bool underwater = Water::is_underwater(ctx.landscape, obj.x.whole, obj.y.whole);

        if (!immune && !underwater) {
            // Deal 8 radiation damage to player
            NPC::damage_player_if_touching(obj, ctx.mgr.player(), 8);
        }
    }

    // Random palette: radiation glow effect (&4203-&4207)
    obj.palette = (ctx.rng.next() >> 1); // LSR clears top bit for background plotting
}

// &41CA: Coronium boulder
void update_coronium_boulder(Object& obj, UpdateContext& ctx) {
    coronium_common(obj, ctx);
}

// &41C2: Coronium crystal - also has a lifespan timer
// Timer increments by 2 each frame; explodes when it overflows (128 frames)
void update_coronium_crystal(Object& obj, UpdateContext& ctx) {
    // Lifespan countdown: timer increases by 2, explodes at overflow (&41c4-&41c8)
    obj.timer += 2;
    if (obj.timer & 0x80) {
        // Timer overflowed: explode with duration 10
        int slot = ctx.mgr.create_object_at(ObjectType::EXPLOSION, 0, obj);
        if (slot >= 0) {
            ctx.mgr.object(slot).tertiary_data_offset = 10;
        }
        obj.energy = 0;
        return;
    }

    // Then shared coronium behavior (touching, radiation, glow)
    coronium_common(obj, ctx);
}

// &4216: Alien weapon - special weapon pickup
void update_alien_weapon(Object& obj, UpdateContext& ctx) {
    update_collectable(obj, ctx);
}

// &439C: Giant block - heavy physics object
void update_giant_block(Object& obj, UpdateContext& ctx) {
    // Giant blocks just follow physics, no active behavior
}

// &43AD: Inert physics object (piano, boulder, invisible inert)
void update_inert(Object& obj, UpdateContext& ctx) {
    // Pure physics objects - no active behavior.
    // Gravity, collision, and velocity are handled by the main physics loop.
}

} // namespace Behaviors
