#include "behaviours/collectable.h"
#include "behaviours/projectile.h"   // for explode_object_with_duration
#include "objects/object_data.h"
#include "particles/particle_system.h"
#include "rendering/sprite_atlas.h"
#include "audio/audio.h"
#include "core/types.h"
#include "world/water.h"
#include <algorithm>
#include <cstdlib>

namespace Behaviors {

// &4B88 update_collectable_object. `energy` bit 7 = undisturbed pin
// (ASL/LSR at &4ba1 clears it on touch; &4ba5 BPL skips rest while pinned).
void update_collectable(Object& obj, UpdateContext& ctx) {
    // Port deviation: skip 6502 auto-collect for general collectables — it
    // breaks the S/R pocket model. Whistles + keys keep auto-collect since
    // the 6502 treats them as permanent player state, not held primaries.
    bool held_by_player =
        ctx.held_object_slot == static_cast<uint8_t>(ctx.this_slot);
    if (held_by_player) {
        // &4b93-&4b96 collect chime, played for any auto-collected
        // pickup (whistles, keys). One sound shared across the three
        // branches below — no need for distinct chimes per type.
        static constexpr uint8_t kSoundCollect[4] = { 0x72, 0xa5, 0x7b, 0x85 };

        if (obj.type == ObjectType::WHISTLE_ONE && ctx.whistle_one_collected) {
            *ctx.whistle_one_collected = true;
            obj.flags |= ObjectFlags::PENDING_REMOVAL;
            Audio::play(Audio::CH_ANY, kSoundCollect);
            return;
        }
        if (obj.type == ObjectType::WHISTLE_TWO && ctx.whistle_two_collected) {
            *ctx.whistle_two_collected = true;
            obj.flags |= ObjectFlags::PENDING_REMOVAL;
            Audio::play(Audio::CH_ANY, kSoundCollect);
            return;
        }
        // &4b8e-&4b91 DEC &07b5,X for X = type. The disassembly tags
        // &080e..&0818 as one logical band: weapons (0..5) + the three
        // immunity flags. Same auto-stamp pattern — sets the flag,
        // plays the collect chime, removes the held object.
        uint8_t t = static_cast<uint8_t>(obj.type);
        uint8_t booster = static_cast<uint8_t>(ObjectType::JETPACK_BOOSTER);
        if (t >= booster && t <= booster + 5 && ctx.player_weapons_collected) {
            ctx.player_weapons_collected[t - booster] = 0x80;
            obj.flags |= ObjectFlags::PENDING_REMOVAL;
            Audio::play(Audio::CH_ANY, kSoundCollect);
            return;
        }
        if (obj.type == ObjectType::FIRE_IMMUNITY_DEVICE &&
            ctx.fire_immunity_collected) {
            *ctx.fire_immunity_collected = true;
            obj.flags |= ObjectFlags::PENDING_REMOVAL;
            Audio::play(Audio::CH_ANY, kSoundCollect);
            return;
        }
        if (obj.type == ObjectType::MUSHROOM_IMMUNITY_PILL &&
            ctx.mushroom_immunity_collected) {
            *ctx.mushroom_immunity_collected = true;
            obj.flags |= ObjectFlags::PENDING_REMOVAL;
            Audio::play(Audio::CH_ANY, kSoundCollect);
            return;
        }
        if (obj.type == ObjectType::RADIATION_IMMUNITY_PILL &&
            ctx.radiation_immunity_collected) {
            *ctx.radiation_immunity_collected = true;
            obj.flags |= ObjectFlags::PENDING_REMOVAL;
            Audio::play(Audio::CH_ANY, kSoundCollect);
            return;
        }
        // &0806 player_keys_collected: keys auto-collect into a bitmask
        // (skipping the pocket) and update_door's &4c9e hook reads it to
        // gate RCD-triggered LOCKED toggles.
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
                Audio::play(Audio::CH_ANY, kSoundCollect);
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

// &4158 update_inactive_grenade. Latch state=1 while held, then promote
// to ACTIVE_GRENADE on drop so the countdown starts.
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

    // &416b-&416d: was held before, now dropped -> promote to
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

// &4360 update_power_pod. Pulses 2-in-16, decays to 0, then step-12 fireball.
// Port deviation: re-introduce the &4b9d disturbed pin so world-placed pods
// stay dormant until touched (6502 ticks them down regardless, which would
// destroy untouched tertiary pods after ~5s).
void update_power_pod(Object& obj, UpdateContext& ctx) {
    // Port deviation (mirrors &4b9d consider_disturbing_object): any
    // non-self touch clears bit 7 of energy. Pin velocity to 0 and
    // bail out while still pinned — no lifespan tick, no flash, no
    // pulse sound.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        obj.energy &= 0x7f;
    }
    if (obj.energy & 0x80) {
        obj.velocity_x = 0;
        obj.velocity_y = 0;
        return;
    }

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
    uint8_t idx = static_cast<uint8_t>(obj.type);
    uint8_t base_palette = object_types_palette_and_pickup[idx] & 0x7f;
    obj.palette = carry_clear ? static_cast<uint8_t>(base_palette ^ 0x30)
                              : base_palette;
    // &436a-&436f: only play the pulse sound during the 2-in-16
    // damaged-palette window.
    if (carry_clear) {
        static constexpr uint8_t kSoundPowerPodPulse[4] = { 0x05, 0xf2, 0xff, 0xc5 };
        Audio::play_at(Audio::CH_ANY, kSoundPowerPodPulse, obj.x.whole, obj.y.whole);
    }
}

// &4374: Destinator - key item for completing the game
void update_destinator(Object& obj, UpdateContext& ctx) {
    update_collectable(obj, ctx);
    // Flash more vigorously
    if (ctx.every_four_frames) {
        obj.palette ^= 0x04;
    }

    // &4389-&4397: 1-frame-in-32 pulsing chirp. The 6502 gates with
    // (frame_counter & 0x1f) == 0x01; we approximate by reusing
    // every_four_frames AND a 1-in-8 random roll for a similar cadence.
    // Port-only roll — routed through cosmetic_rng so the sound cadence
    // doesn't perturb game_rng's 6502-aligned sequence.
    if (ctx.every_four_frames && (ctx.cosmetic_rng.next() & 0x07) == 0) {
        static constexpr uint8_t kSoundDestinatorPulse[4] = {
            0x33, 0x03, 0x85, 0x12 };
        Audio::play_at(Audio::CH_ANY, kSoundDestinatorPulse,
                       obj.x.whole, obj.y.whole);
    }
}

// Port of &43a7 update_empty_flask:
//   &43a7 LDA #&4d ; OBJECT_FULL_FLASK
//   &43a9 BIT &1f ; this_object_in_water  ; positive if submerged
//   &43ab BPL &43e4 to_change_object_type ; convert to FULL_FLASK
//   &43ad RTS
// Skip the inert-body call — our main loop handles gravity.
void update_empty_flask(Object& obj, UpdateContext& ctx) {
    // Must use per-column waterline (this_object_in_water at &1f), not
    // NPC::is_underwater's SURFACE_Y shortcut — that flags any flask with
    // y > 0x4e as submerged and transmutes them on spawn.
    if (Water::is_underwater(ctx.landscape, obj.x.whole, obj.y.whole)) {
        // &43e4 change_object_type also refreshes sprite + palette.
        obj.type    = ObjectType::FULL_FLASK;
        obj.sprite  = object_types_sprite[
            static_cast<uint8_t>(ObjectType::FULL_FLASK)];
        obj.palette = object_types_palette_and_pickup[
            static_cast<uint8_t>(ObjectType::FULL_FLASK)] & 0x7f;
    }
}

// &43AE update_full_flask. Empties on hard contact (object impact with
// max|v|>=0x0a, or pre-collision magnitude>=0x14 against a tile), running
// a 16-frame splash that emits upward particles and douses fireballs.
void update_full_flask(Object& obj, UpdateContext& ctx) {
    bool start_emptying = false;

    // &43b0-&43b5: touching something, and still moving fast enough
    // (max(|vx|,|vy|) >= 0x0a) -> disturbed.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        uint8_t max_vel = static_cast<uint8_t>(
            std::max(std::abs(static_cast<int>(obj.velocity_x)),
                     std::abs(static_cast<int>(obj.velocity_y))));
        if (max_vel >= 0x0a) start_emptying = true;
    }

    // &43b7-&43bb hit-tile-hard. Must read pre_collision_magnitude (raw
    // pre-revert velocity); post-revert velocity fires on gentle landings.
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

    // &43d3-&43db emit PARTICLE_FLASK upward (angle=&c0). Count reduced
    // 8->2/frame: our 256-slot pool doesn't evict like the 6502's 32-slot
    // one, so 2×16 matches the 6502's pool-capped peak of ~32 live.
    if (ctx.particles) {
        ctx.particles->emit(ParticleType::FLASK, 2, obj, ctx.cosmetic_rng,
                            /*angle=*/0xc0);
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

// Port of &4351 update_remote_control_device:
//   &4351 JSR &0bbf check_if_object_fired
//   &4354 BNE &43a6 ; leave              ; not firing this frame
//   &4356 JSR &13fa play_sound (data 57 07 c1 d3)
//   &435d JMP &312b create_aim_particle
// Player-fire sets player_object_fired to the held slot
// (apply_player_input); doors/transporters/cannon read the same flag
// via check_if_object_hit_by_remote_control (&0bc5).
void update_control_device(Object& obj, UpdateContext& ctx) {
    update_collectable(obj, ctx);

    // check_if_object_fired (&0bbf) — this_object == player_object_fired?
    if (ctx.player_object_fired != static_cast<uint8_t>(ctx.this_slot)) {
        return;
    }

    // &4356-&4359: play_sound — RCD firing pew. Bytes follow the JSR.
    static constexpr uint8_t kSoundRCDFire[4] = { 0x57, 0x07, 0xc1, 0xd3 };
    Audio::play(Audio::CH_ANY, kSoundRCDFire);

    // &435d create_aim_particle (&312b). Must use emit_directed: plain
    // emit() inherits the held RCD's ~0 velocity, leaving AIM particles
    // stuck at the player's hands instead of streaming along aim angle.
    if (ctx.particles) {
        ctx.mgr.log_diag("RCD fire: aim_with_flip=0x%02x obj.sprite=%u "
                         "obj.flags=0x%02x obj.vy=%d obj.y=%u.%u\n",
                         (unsigned)ctx.player_aim_angle, (unsigned)obj.sprite,
                         (unsigned)obj.flags, (int)obj.velocity_y,
                         (unsigned)obj.y.whole, (unsigned)obj.y.fraction);
        ctx.particles->emit_directed(
            ParticleType::AIM, ctx.player_aim_angle, obj, ctx.cosmetic_rng);
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

            // &41e5 flash_background — 11 frames of sky strobe.
            if (ctx.background_flash_cooldown) {
                *ctx.background_flash_cooldown = 0x0b;
            }

            // &41e8 JMP &40db explode_object_with_duration_A — in-place
            // mutation of THIS slot to OBJECT_EXPLOSION with the
            // computed duration. Old port spawned a separate EXPLOSION
            // primary via create_object_centered AND set obj.energy=0
            // (which step 12 then mutated again) — two explosions at
            // one spot, and the second used step 12's default
            // (init_e>>5)+3 duration, losing the weight-based size.
            static constexpr uint8_t kSoundExplosion[4] = { 0x17, 0x03, 0x11, 0x04 };
            Audio::play_at(Audio::CH_PRIORITY, kSoundExplosion,
                           obj.x.whole, obj.y.whole);
            Behaviors::explode_object_with_duration(obj, duration);
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
        // Per-column waterline (radiation blocked by water). SURFACE_Y
        // shortcut would suppress damage in lower-world air pockets.
        // &41f5 BIT &0818: radiation pill collected → skip damage.
        bool immune = ctx.radiation_immunity_collected &&
                      *ctx.radiation_immunity_collected;
        bool underwater = Water::is_underwater(ctx.landscape, obj.x.whole, obj.y.whole);

        if (!immune && !underwater) {
            // Deal 8 radiation damage to player
            NPC::damage_player_if_touching(obj, ctx.mgr.player(), 8,
                                           ctx.damage_events, &ctx);
        }
    }

    // Random palette: radiation glow effect (&4203-&4207)
    obj.palette = (ctx.rng.next() >> 1); // LSR clears top bit for background plotting
}

// &41CA: Coronium boulder
void update_coronium_boulder(Object& obj, UpdateContext& ctx) {
    coronium_common(obj, ctx);
}

// Port of &41c2 update_coronium_crystal (4 inst entry, falls through to
// shared coronium body):
//   &41c2 LDA #&0a                    ; explosion duration 10
//   &41c4 INC &12 ; this_object_timer
//   &41c6 INC &12 ; this_object_timer  ; +2 / frame
//   &41c8 BMI &41e8 to_explode_object_with_duration_A
// Crystal explodes ~64 frames after spawn (timer overflows into bit 7).
void update_coronium_crystal(Object& obj, UpdateContext& ctx) {
    // Lifespan countdown: timer increases by 2, explodes at overflow (&41c4-&41c8)
    obj.timer += 2;
    if (obj.timer & 0x80) {
        // &41c8 BMI &41e8 JMP &40db — in-place mutation with duration 10.
        // Same fix as the boulder chain: don't double-spawn via
        // create_object_centered + step 12.
        static constexpr uint8_t kSoundExplosion[4] = { 0x17, 0x03, 0x11, 0x04 };
        Audio::play_at(Audio::CH_PRIORITY, kSoundExplosion,
                       obj.x.whole, obj.y.whole);
        Behaviors::explode_object_with_duration(obj, 10);
        return;
    }

    // Then shared coronium behavior (touching, radiation, glow)
    coronium_common(obj, ctx);
}

// &4216 update_alien_weapon. The 6502 dispatch at &03b9/&0432 routes type
// 0x47 directly to &4216, NOT through update_collectable — routing through
// it makes the held-touch bit-7 clear race the energy regen, hitting
// energy==0 and triggering step-12 self-destruct.
void update_alien_weapon(Object& obj, UpdateContext& ctx) {
    // &4216 increase_energy_by_one_if_not_zero — slow regen toward 0xff
    // when the weapon has any charge left. Clamps at 0xff so a freshly-
    // spawned weapon doesn't roll over (port of the INC/BEQ/DEC chain at
    // &2549-&254d).
    if (obj.energy != 0 && obj.energy < 0xff) obj.energy++;

    // &4219-&421c check_if_object_fired. The held-and-fired contract is
    // wired via apply_player_input setting player_object_fired = held
    // slot when SPACE is pressed; nothing else writes the flag.
    if (ctx.player_object_fired != static_cast<uint8_t>(ctx.this_slot)) {
        return;
    }

    // &421e-&4225 spawn PLASMA_BALL with vx=0x40 (negated via FLIP_HORIZONTAL
    // inherited from holding player), vy=0. Bullet's own update tracks the
    // player via lock-on.
    int gslot = ctx.mgr.create_object_at(
        ObjectType::PLASMA_BALL, /*min_free_slots=*/1, obj);
    if (gslot < 0) return;
    Object& ball = ctx.mgr.object(gslot);
    int8_t vx = 0x40;
    if (obj.is_flipped_h()) vx = static_cast<int8_t>(-vx);
    ball.velocity_x = vx;
    ball.velocity_y = 0;
    NPC::offset_child_from_parent(ball, obj);

    // &4227 JMP play_low_beep at &14ad. Sound parameter bytes from
    // &14b0: 5d 04 ff 05 — same beep used by the plasma gun and the
    // "object absorbed" cue.
    static constexpr uint8_t kSoundLowBeep[4] = { 0x5d, 0x04, 0xff, 0x05 };
    Audio::play(Audio::CH_ANY, kSoundLowBeep);
}

// Port of &439c update_giant_block:
//   &439c LDA &20 ; this_object_waterline
//   &439e CMP #&c0                  ; 3/4 submerged?
//   &43a0 BCC &43a6 ; leave
//   &43a2 DEC &42 ; acceleration_y  ; float upwards (twice -> accel -2)
//   &43a4 DEC &42
//   &43a6 RTS
// Weight 6 -> apply_water_effects gives zero buoyancy DECs; the giant
// block relies entirely on this routine for its float. Decrement vy by 2
// directly: physics adds gravity (+1) after our routine, giving net -1
// upward (matches `accel_y = -2 + gravity_bit = -1`).
void update_giant_block(Object& obj, UpdateContext& ctx) {
    // &20 this_object_waterline: how far the bottom of the AABB sits
    // below the waterline, in y-fraction units (0 = surface or above,
    // 0xff = fully submerged). Matches the diff math at apply_water_
    // effects.
    int sprite_h_units = (obj.sprite <= 0x80 && sprite_atlas[obj.sprite].h > 0)
        ? (sprite_atlas[obj.sprite].h - 1) * 8 : 0;
    int max_y_abs = static_cast<int>(obj.y.whole) * 256 +
                    static_cast<int>(obj.y.fraction) + sprite_h_units;
    int waterline_abs =
        static_cast<int>(Water::get_waterline_y(obj.x.whole)) * 256;
    int diff = max_y_abs - waterline_abs;
    uint8_t amount_under = (diff <= 0)        ? 0
                          : (diff >= 0x100)   ? 0xff
                                              : static_cast<uint8_t>(diff);

    // &439e CMP #&c0 / BCC leave — at least 3/4 of the block submerged.
    if (amount_under < 0xc0) return;

    // &43a2-&43a4 DEC accel_y twice. apply_acceleration runs immediately
    // after with gravity_bit = 1, so the net delta is vy -= 1.
    int nv = static_cast<int>(obj.velocity_y) - 2;
    if (nv < -128) nv = -128;
    obj.velocity_y = static_cast<int8_t>(nv);
}

// Port of &43ad update_inert:
//   &43ad RTS
// Pure physics objects (piano, boulder, invisible inert) — gravity,
// collision, velocity handled by the main physics loop.
void update_inert(Object& obj, UpdateContext& ctx) {
    // No active behavior.
}

} // namespace Behaviors
