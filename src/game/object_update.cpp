#include "game/game.h"
#include "objects/physics.h"
#include "objects/collision.h"
#include "objects/tile_collision.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "objects/held_object.h"
#include "behaviours/behavior_dispatch.h"
#include "behaviours/environment.h"
#include "behaviours/npc_helpers.h"
#include "behaviours/projectile.h"
#include "audio/audio.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
#include "world/wind.h"
#include "world/water.h"
#include "rendering/sprite_atlas.h"
#include <array>

// Port of &30e9-&30f5 — bounce damp:
//   &30e9 SBC #&02                ; subtract 2 from velocity magnitude
//   &30eb BCS &30ef               ; underflowed? clamp to 0
//   &30ed LDA #&00
//   &30ef JSR &3235 calculate_seven_eighths
//   &30f2 JSR &2357 calculate_vector_from_magnitude_and_angle
//   &30f5 STA &45 ; velocity_y    ; (caller also writes vector_x to &43)
// Applied per-axis here because our integration is axis-separated
// whereas the 6502 uses a single combined-magnitude vector. Cap 0x20,
// lose 2, damp 7/8, reflect — settles within 3-4 bounces.
static int8_t bounce_reflect(int8_t v_in) {
    int mag = (v_in < 0) ? -v_in : v_in;
    if (mag > 0x20) mag = 0x20;
    mag = (mag > 2) ? (mag - 2) : 0;
    mag = mag * 7 / 8;
    return static_cast<int8_t>(v_in > 0 ? -mag : mag);
}

// Port of &3235 calculate_seven_eighths — damp without reflect on the
// non-colliding axis to approximate the 6502's combined-magnitude split
// that our axis-separated integration would otherwise miss.
static int8_t damp_seven_eighths(int8_t v_in) {
    int mag = (v_in < 0) ? -v_in : v_in;
    mag = mag * 7 / 8;
    return static_cast<int8_t>(v_in > 0 ? mag : -mag);
}

// Re-centre a 16-bit Fixed8_8 axis by `delta` fraction-units, wrapping
// at 0xffff like the 6502's 8-bit ADC chain. Used by step 12 to keep
// the explosion sprite centred on the projectile it replaces.
static void shift_axis(uint8_t& whole, uint8_t& frac, int delta) {
    int sum = int(whole) * 0x100 + int(frac) + delta;
    sum &= 0xffff;
    whole = static_cast<uint8_t>((sum >> 8) & 0xff);
    frac  = static_cast<uint8_t>(sum & 0xff);
}

// AABB-corner solid probe for the held primary's penetration check
// at step 3 — axis-separated revert tests post-move flush vs. old
// held position to detect a wall slap.
static bool held_aabb_solid(const Landscape& landscape,
                             Fixed8_8 hx, Fixed8_8 hy, int hw, int hh) {
    int right_abs = int(hx.whole) * 256 + int(hx.fraction) + hw;
    int bot_abs   = int(hy.whole) * 256 + int(hy.fraction) + hh;
    uint8_t rtx = uint8_t((right_abs >> 8) & 0xff);
    uint8_t rxf = uint8_t(right_abs & 0xff);
    uint8_t bty = uint8_t((bot_abs   >> 8) & 0xff);
    uint8_t byf = uint8_t(bot_abs   & 0xff);
    return
        Collision::point_in_tile_solid(landscape,
            hx.whole, hy.whole, hx.fraction, hy.fraction) ||
        Collision::point_in_tile_solid(landscape,
            rtx, hy.whole, rxf, hy.fraction) ||
        Collision::point_in_tile_solid(landscape,
            hx.whole, bty, hx.fraction, byf) ||
        Collision::point_in_tile_solid(landscape,
            rtx, bty, rxf, byf);
}

// Sprite AABB width in 1/256-tile fraction units (port of the 6502's
// (w-1)*16 entry in sprites_width_and_horizontal_flip_table at &5e0c).
static int spr_w_units(uint8_t sprite_id) {
    if (sprite_id > 0x80) return 0;
    int w = sprite_atlas[sprite_id].w;
    return (w > 0 ? (w - 1) : 0) * 16;
}

// Sprite AABB height in fraction units (port of (h-1)*8 from &5e89).
static int spr_h_units(uint8_t sprite_id) {
    if (sprite_id > 0x80) return 0;
    int h = sprite_atlas[sprite_id].h;
    return (h > 0 ? (h - 1) : 0) * 8;
}

// ±2 velocity kick along the smallest-overlap axis (port of &2b80-&2b8b).
// Clamps to signed 8-bit per &327f prevent_overflow.
static void nudge_velocity(int8_t& v, int sign) {
    int nv = int(v) + 2 * sign;
    if (nv >  127) nv =  127;
    if (nv < -128) nv = -128;
    v = static_cast<int8_t>(nv);
}

// Port of &2b51-&2bb0 object-object overlap response: smallest-edge
// push-out + ±2 nudge + &2bb6 mass-ratio transfer + symmetric touching
// stamp (&2b1d-&2b27). Caller filters intangibles and newly-created
// bullets (they self-handle in common_bullet_update).
static void resolve_obj_overlap_response(Object& obj, int slot,
                                         Object& other, int other_slot) {
    int this_l = obj.x.whole * 256 + obj.x.fraction;
    int this_t = obj.y.whole * 256 + obj.y.fraction;
    int this_r = this_l + spr_w_units(obj.sprite);
    int this_b = this_t + spr_h_units(obj.sprite);
    int other_l = other.x.whole * 256 + other.x.fraction;
    int other_t = other.y.whole * 256 + other.y.fraction;
    int other_r = other_l + spr_w_units(other.sprite);
    int other_b = other_t + spr_h_units(other.sprite);

    // pen[] edges: 0 right (push -x), 1 left (+x),
    // 2 bot (push -y), 3 top (+y).
    int pen[4];
    pen[0] = this_r  - other_l;
    pen[1] = other_r - this_l;
    pen[2] = this_b  - other_t;
    pen[3] = other_b - this_t;
    int smallest = 0;
    for (int e = 1; e < 4; e++)
        if (pen[e] < pen[smallest]) smallest = e;
    bool axis_y = (smallest >= 2);
    int sign = (smallest == 0 || smallest == 2) ? -1 : +1;
    int depth = pen[smallest];

    // &2b8d-&2b94 push this out by overlap.
    if (!axis_y) {
        int nx = this_l + sign * depth;
        obj.x.whole    = static_cast<uint8_t>((nx >> 8) & 0xff);
        obj.x.fraction = static_cast<uint8_t>(nx & 0xff);
    } else {
        int ny = this_t + sign * depth;
        obj.y.whole    = static_cast<uint8_t>((ny >> 8) & 0xff);
        obj.y.fraction = static_cast<uint8_t>(ny & 0xff);
    }

    // &2b80-&2b8b ±2 velocity nudge along smallest-overlap axis.
    if (!axis_y) nudge_velocity(obj.velocity_x, sign);
    else         nudge_velocity(obj.velocity_y, sign);

    // &2bb6 per-axis mass-ratio transfer.
    {
        auto t = Collision::apply_mass_ratio_velocity(
            obj.velocity_x, other.velocity_x,
            obj.weight(), other.weight(), !axis_y);
        obj.velocity_x = t.this_v;
        other.velocity_x = t.other_v;
    }
    {
        auto t = Collision::apply_mass_ratio_velocity(
            obj.velocity_y, other.velocity_y,
            obj.weight(), other.weight(), axis_y);
        obj.velocity_y = t.this_v;
        other.velocity_y = t.other_v;
    }

    // &2b1d-&2b27 symmetric touching stamp.
    obj.touching   = static_cast<uint8_t>(other_slot);
    other.touching = static_cast<uint8_t>(slot);

    // &29e5 object_collision_y_flags bit 7 — set when this object was
    // pushed up (its bottom edge hit the other's top). Mirror to both
    // objects' transient bottom_collision so &19 = &18 | &29e5 in
    // check_demotion sees object-supported stationary objects.
    if (axis_y && sign == -1) obj.bottom_collision   = true;
    if (axis_y && sign == +1) other.bottom_collision = true;
}

// Full 18-step update loop - port of &1a0b-&1e18
void Game::update_objects() {
    const Object& player = object_mgr_.player();

    // Secondary object promotion
    object_mgr_.promote_selective(rng_);

    // Main loop over slots 1-15
    for (int slot = 1; slot < GameConstants::PRIMARY_OBJECT_SLOTS; slot++) {
        Object& obj = object_mgr_.object(slot);
        if (!obj.is_active()) continue;

        // &1a27-&1a33 prelude: snapshot current position into prev_x/y so
        // per-type updaters can revert via set_position_from_previous.
        obj.prev_x = obj.x;
        obj.prev_y = obj.y;
        // &1ae1 STX &2b with X=0xff: visibility defaults to visible
        // every frame; invisible_bird / invisible_frogman clear it later.
        obj.visible = true;

        // Step 3: &1afd-&1b54 — snap held primary flush to player's
        // facing side, then run the shared check_for_collisions. Drift
        // (held_expected_* vs post-resolve) feeds &1ca9 drop test.
        if (slot == held_object_slot_) {
            Fixed8_8 prev_held_x = obj.x;
            Fixed8_8 prev_held_y = obj.y;
            HeldObject::update_position(obj, player);
            held_expected_x_ = obj.x;
            held_expected_y_ = obj.y;
            // Held primaries skip step 15's per-frame reset; clear here
            // or a pre-pickup impact magnitude pins update_full_flask's
            // >= 0x14 trigger and bleeds water indefinitely.
            obj.pre_collision_magnitude = 0;
            obj.bottom_collision = false;
            TileCollision::Result tcr = TileCollision::resolve(
                obj, prev_held_x.whole, prev_held_x.fraction,
                prev_held_y.whole, prev_held_y.fraction,
                landscape_, object_mgr_, /*skip_slot=*/-1);
            obj.tile_collision = tcr.top_or_bottom_collision;
            if (tcr.landed_on_bottom) obj.bottom_collision = true;
        }

        // Step 7: Check demotion
        if (object_mgr_.check_demotion(slot, frame_counter_)) {
            if (slot == held_object_slot_) {
                held_object_slot_ = 0x80;
            }
            continue;
        }

        // Step 8: Handle teleporting (port of &1bfd-&1c44)
        if (obj.flags & ObjectFlags::TELEPORTING) {
            if (obj.timer == 0) {
                // Finished teleporting: clear flag
                obj.flags &= ~ObjectFlags::TELEPORTING;
                if (obj.energy < 0xff) obj.energy++;
            } else {
                if (obj.timer == 0x11) {
                    // Brief removal at midpoint (object disappears)
                }
                if (obj.timer == 0x10) {
                    // Change position to teleport destination
                    obj.x.whole = obj.tx;
                    obj.y.whole = obj.ty;
                    obj.x.fraction = 0x80; // Center in tile
                    obj.y.fraction = 0x80;
                    obj.velocity_x = 0;
                    obj.velocity_y = 0;
                    // &1c32-&1c35 play sound for object changing position
                    // in teleport. play_at so distance attenuates for
                    // off-screen primaries that teleport (clawed robots,
                    // hovering balls returning to nest).
                    static constexpr uint8_t kSoundTeleportArrive[4] = {
                        0x33, 0xf3, 0x63, 0xf3 };
                    Audio::play_at(Audio::CH_ANY, kSoundTeleportArrive,
                                   obj.x.whole, obj.y.whole);
                }
                obj.timer--;
                continue; // Skip physics while teleporting
            }
        }

        // Step 9b: Port of &1b54 check_for_collisions — refresh touching
        // before per-type dispatch so update routines (notably &4704
        // update_triax) see this frame's overlap. We run for all weights
        // unlike the 6502 (&1b50 skips statics); symmetric so it matches.
        {
            auto early_coll = Collision::check_object_collision(
                obj, slot, object_mgr_.primary_array());
            if (early_coll.collided) {
                obj.touching = static_cast<uint8_t>(early_coll.other_slot);
                Object& other = object_mgr_.object(early_coll.other_slot);
                other.touching = static_cast<uint8_t>(slot);
            } else if (obj.weight() < 7) {
                obj.touching = 0x80;
            }
        }

        // Step 10: Call type-specific update routine
        auto update_fn = AI::get_update_func(obj.type);
        if (update_fn) {
            UpdateContext uctx{object_mgr_, landscape_, rng_, cosmetic_rng_,
                              frame_counter_,
                              every_four_frames_, every_eight_frames_,
                              every_sixteen_frames_, every_thirty_two_frames_,
                              every_sixty_four_frames_,
                              whistle_one_active_, &whistle_two_activator_,
                              &whistle_one_collected_, &whistle_two_collected_,
                              &chatter_energy_reserve_,
                              player_weapons_collected_,
                              weapon_energy_,
                              &player_immobility_movement_,
                              &player_immobility_thrust_,
                              &fire_immunity_collected_,
                              &radiation_immunity_collected_,
                              &mushroom_immunity_collected_,
                              player_mushroom_timers_,
                              player_keys_collected_,
                              &particles_,
                              held_object_slot_, player_object_fired_,
                              // &311d player_aiming_angle_with_flip:
                              // mirror via (0x80 - s) when facing left,
                              // matching Weapon::get_firing_velocity so
                              // AIM particles and bullets agree.
                              static_cast<uint8_t>(player.is_flipped_h()
                                  ? (0x80 - static_cast<int8_t>(player_aim_angle_))
                                  : static_cast<int8_t>(player_aim_angle_)),
                              slot,
                              imp_gifts_remaining_,
                              &background_flash_cooldown_,
                              renderer_ && renderer_->damage_overlay_enabled()
                                  ? &damage_events_ : nullptr,
                              &explosion_timer_,
                              &flooding_state_,
                              &floating_labels_,
                              sucking_nest_damages_player_};
            update_fn(obj, uctx);
        }

        // Step 11: &1ca9-&1cc4 consider_dropping_held_object. Drop if
        // signed drift between held_expected_* and post-resolve obj.x/y
        // leaves the [-0x30, +0x30) frac window on either axis.
        if (slot == held_object_slot_) {
            int drift_x =
                (static_cast<int>(held_expected_x_.whole) * 256 +
                 static_cast<int>(held_expected_x_.fraction)) -
                (static_cast<int>(obj.x.whole) * 256 +
                 static_cast<int>(obj.x.fraction));
            int drift_y =
                (static_cast<int>(held_expected_y_.whole) * 256 +
                 static_cast<int>(held_expected_y_.fraction)) -
                (static_cast<int>(obj.y.whole) * 256 +
                 static_cast<int>(obj.y.fraction));
            if (drift_x >= 0x30 || drift_x < -0x30 ||
                drift_y >= 0x30 || drift_y < -0x30) {
                HeldObject::drop(obj, object_mgr_.player(),
                                 held_object_slot_);
            }
        }

        // Step 12: Port of &1ce3-&1cf3 — energy=0 mutates slot IN-PLACE
        // into EXPLOSION via &40db. Duration = (initial_energy/32)+3.
        // Must stay in-place: tertiary-backed primaries (doors) keep
        // obj.tertiary_slot so dedup blocks immediate respawn.
        if (obj.energy == 0 && obj.type != ObjectType::EXPLOSION &&
            !object_type_is_indestructible(static_cast<uint8_t>(obj.type))) {
            // Diag: imps reaching energy==0 (step 12 explosion mutation).
            uint8_t tidx_e = static_cast<uint8_t>(obj.type);
            if (tidx_e >= 0x29 && tidx_e <= 0x2d) {
                object_mgr_.log_diag(
                    "imp p%d ENERGY_ZERO -> explosion @%u,%u flags=0x%02x "
                    "touching=0x%02x",
                    slot, obj.x.whole, obj.y.whole, obj.flags, obj.touching);
            }
            // &1ce3-&1cf3 dispatch top-two-bits = 00 (indestructible) is
            // a no-op for flasks/keys/pickups/EXPLOSION itself. Without
            // the guard, grenade radius mutates flasks; without the
            // EXPLOSION guard, step 12 re-fires on its own mutated slot.
            uint8_t init_e = get_initial_energy(static_cast<uint8_t>(obj.type));
            uint8_t duration = static_cast<uint8_t>((init_e >> 5) + 3);

            // Port-only deviation: swap small projectile sprites
            // (&12..&1b OBJECT_RANGE_PROJECTILES) for SPRITE_FIREBALL
            // so explosions read on our 40-tile viewport. Larger sprites
            // keep their original — palette flicker matches 6502.
            uint8_t t = static_cast<uint8_t>(obj.type);
            bool is_projectile = (t >= 0x12 && t <= 0x1b);
            if (is_projectile) {
                int old_w_frac = 0, old_h_frac = 0;
                if (obj.sprite <= 0x80) {
                    const SpriteAtlasEntry& e = sprite_atlas[obj.sprite];
                    old_w_frac = (e.w > 0 ? (e.w - 1) : 0) * 16;
                    old_h_frac = (e.h > 0 ? (e.h - 1) : 0) * 8;
                }
                obj.sprite = object_types_sprite[
                    static_cast<uint8_t>(ObjectType::EXPLOSION)];
                obj.palette = object_types_palette_and_pickup[
                    static_cast<uint8_t>(ObjectType::EXPLOSION)] & 0x7f;
                int new_w_frac = 0, new_h_frac = 0;
                if (obj.sprite <= 0x80) {
                    const SpriteAtlasEntry& e = sprite_atlas[obj.sprite];
                    new_w_frac = (e.w > 0 ? (e.w - 1) : 0) * 16;
                    new_h_frac = (e.h > 0 ? (e.h - 1) : 0) * 8;
                }
                shift_axis(obj.x.whole, obj.x.fraction, (old_w_frac - new_w_frac) / 2);
                shift_axis(obj.y.whole, obj.y.fraction, (old_h_frac - new_h_frac) / 2);
            }

            Behaviors::explode_object_with_duration(obj, duration);
            // &1ce3-&1cf3 dispatch top 2 bits of object_types_update_
            // routine_addresses_high: 0x40 = loud squeal + squeal +
            // explosion (&40be), 0xc0 = squeal + explosion (&40c5),
            // 0x80 = fireball-no-sound (&40bb, not split out yet).
            uint8_t exp_flag = object_types_update_routine_addresses_high[t] & 0xc0;
            if (exp_flag == 0x40) {
                static constexpr uint8_t kSoundLoudSqueal[4] = { 0x57, 0x07, 0x43, 0xf6 };
                Audio::play_at(Audio::CH_ANY, kSoundLoudSqueal,
                               obj.x.whole, obj.y.whole);
            }
            if (exp_flag == 0x40 || exp_flag == 0xc0) {
                static constexpr uint8_t kSoundSqueal[4] = { 0x33, 0x03, 0x2d, 0x84 };
                Audio::play_at(Audio::CH_ANY, kSoundSqueal,
                               obj.x.whole, obj.y.whole);
            }
            static constexpr uint8_t kSoundExplosion[4] = { 0x17, 0x03, 0x11, 0x04 };
            Audio::play_at(Audio::CH_PRIORITY, kSoundExplosion,
                           obj.x.whole, obj.y.whole);
            // The slot is now an EXPLOSION primary — let it run through
            // the normal update_explosion path on subsequent frames so
            // the duration counts down, particles fire, and the radius
            // damage applies.
            if (slot == held_object_slot_) held_object_slot_ = 0x80;
            continue;
        }

        // Step 14: Reap PENDING_REMOVAL (port of &2516). Don't bounce
        // back to tertiary — that would re-arm bit 7 and respawn
        // collected items a few frames later.
        if (obj.flags & ObjectFlags::PENDING_REMOVAL) {
            uint8_t tidx_r = static_cast<uint8_t>(obj.type);
            if (tidx_r >= 0x29 && tidx_r <= 0x2d) {
                object_mgr_.log_diag(
                    "imp p%d PENDING_REMOVAL reap type=0x%02x @%u,%u "
                    "flags=0x%02x energy=0x%02x",
                    slot, tidx_r, obj.x.whole, obj.y.whole,
                    obj.flags, obj.energy);
            }
            object_mgr_.remove_object(slot);
            if (slot == held_object_slot_) held_object_slot_ = 0x80;
            continue;
        }

        // Step 9: Apply wind (only above surface)
        Wind::apply_surface_wind(obj);

        // Port of &3f73 add_wind_particle_using_velocities:
        //   &3f73 JSR &22d4 calculate_angle_from_vector
        //   &3f76 LDA &da ; rnd_state+1
        //   &3f78 LSR A
        //   &3f79 CMP &b7 ; magnitude
        //   &3f7b BCS &3fb6 ; leave   ; stronger wind → more particles
        //   &3f7d LDY #&6e ; PARTICLE_WIND
        //   (falls through to &3f7f set_new_particles_position_from_this_object)
        // One PARTICLE_WIND per frame with probability magnitude/0x7f,
        // emitted in the wind's direction.
        {
            int8_t wvx = 0, wvy = 0;
            Wind::surface_wind_vector(obj, wvx, wvy);
            uint8_t mag = Wind::surface_wind_magnitude(obj);
            // Gate roll stays on game_rng — the 6502 makes the same
            // 1-byte roll at &3f73 so keeping it here preserves alignment.
            if (mag > 0 && (rng_.next() & 0x7f) < mag) {
                uint8_t angle = NPC::angle_from_deltas(wvx, wvy);
                particles_.emit_directed(ParticleType::WIND, angle, obj, cosmetic_rng_);
            }
        }

        // Step 15: Apply physics (gravity + velocity)
        if (slot != held_object_slot_) {
            // Per-type physics gate: weight 7 = fully static; energy
            // bit 7 on types >= 0x4a = undisturbed-pin (6502 omits
            // add_A_to_position; we need an explicit skip or gravity
            // drifts pinned collectables). INTANGIBLE skips gravity.
            uint8_t tidx = static_cast<uint8_t>(obj.type);
            uint8_t tflags = (tidx < static_cast<uint8_t>(ObjectType::COUNT))
                             ? object_types_flags[tidx] : 0;
            bool fully_static = obj.weight() >= 7;
            // 6502 placeholders pin position via set_position_from_
            // previous_position each frame they stay; the per-type
            // update fully owns motion, so the +1 gravity our shared
            // physics step adds is a port artefact. Exempt them.
            bool gravity_exempt = (tflags & ObjectTypeFlags::INTANGIBLE) != 0 ||
                                  obj.type == ObjectType::PLACEHOLDER;
            // Energy-bit-7 pin: pure collectables (keys + equipment)
            // that route through update_collectable, which clears bit 7
            // on touch. POWER_POD (0x4b) has its own update routine but
            // clears bit 7 on touch too (see update_power_pod), so it
            // needs the pin while undisturbed.
            uint8_t ti = static_cast<uint8_t>(obj.type);
            bool uses_update_collectable =
                ti == 0x4b ||
                (ti >= 0x51 && ti <= 0x54) ||
                (ti >= 0x56 && ti <= 0x57) ||
                (ti >= 0x59 && ti <= 0x63);
            bool pin_undisturbed = (obj.energy & 0x80) != 0 &&
                                    uses_update_collectable;
            // &4ba9 update_bush pins unconditionally. Bushes are
            // weight 6 (fall outside fully_static) — explicit pin
            // here, or wasp/projectile residual velocity drifts them.
            bool pin_bush = obj.type == ObjectType::BUSH;

            if (fully_static || pin_undisturbed || pin_bush) {
                obj.velocity_x = 0;
                obj.velocity_y = 0;
                // Statics need touching for switch fire / door close;
                // 6502 &2a64 check_for_collisions runs regardless of
                // weight, so we do too.
                auto obj_coll = Collision::check_object_collision(
                    obj, slot, object_mgr_.primary_array());
                // Stamp on overlap only; cross-frame staleness is
                // handled by the &1dd6-&1dda OR #&80 at slot-loop end.
                if (obj_coll.collided) {
                    obj.touching = static_cast<uint8_t>(obj_coll.other_slot);
                    object_mgr_.object(obj_coll.other_slot).touching =
                        static_cast<uint8_t>(slot);
                }
            } else {
                Physics::apply_acceleration(obj, 0, 0, every_sixteen_frames_);
                if (gravity_exempt && obj.velocity_y > 0) {
                    // Undo the gravity +1 that apply_acceleration's y-axis
                    // hard-codes. Leaves any real upward/downward velocity
                    // the object gave itself intact.
                    obj.velocity_y--;
                }

                // Tile collision — TileCollision::resolve, port of
                // &2f8c-&30df. Object-object overlap is handled below
                // by the &2bb6 mass-ratio transfer.
                Fixed8_8 ou_old_x = obj.x;
                Fixed8_8 ou_old_y = obj.y;
                obj.x.add_velocity(obj.velocity_x);
                obj.y.add_velocity(obj.velocity_y);

                // SUPPORTED re-set from tcr.landed_on_bottom below;
                // 6502 derives it from tile_collision_y_flags bit 7.
                obj.flags &= ~ObjectFlags::SUPPORTED;
                obj.bottom_collision = false;

                TileCollision::Result tcr = TileCollision::resolve(
                    obj, ou_old_x.whole, ou_old_x.fraction,
                    ou_old_y.whole, ou_old_y.fraction,
                    landscape_, object_mgr_,
                    /*skip_slot=*/-1);
                obj.tile_collision = tcr.top_or_bottom_collision;
                if (tcr.landed_on_bottom) {
                    obj.flags |= ObjectFlags::SUPPORTED;
                    obj.bottom_collision = true;
                }

                // Port of &2b51-&2bb0 — split detection keeps the
                // weight filter for door/turret/cannon; bullets (&2afd)
                // and projectiles 0x13..0x19 self-handle.
                {
                    uint8_t self_idx = static_cast<uint8_t>(obj.type);
                    bool is_projectile = (self_idx >= 0x13 && self_idx <= 0x19);
                    auto& all_primaries = object_mgr_.primary_array();
                    bool eligible =
                        !is_projectile &&
                        !(obj.flags & ObjectFlags::NEWLY_CREATED) &&
                        !(tflags & ObjectTypeFlags::INTANGIBLE);

                    int blocker = Collision::overlapping_solid_slot(
                        obj, slot, all_primaries);
                    if (eligible && blocker >= 0) {
                        // Heavier-blocker: same &2b97 response as lighter
                        // — push out + nudge + transfer for every overlap.
                        Object& other = object_mgr_.object(blocker);
                        resolve_obj_overlap_response(obj, slot, other, blocker);
                    } else if (eligible) {
                        // Lighter target: overlapping_solid_slot only
                        // matches HEAVIER, so grenade vs flask would
                        // fly through without this explicit branch.
                        auto coll = Collision::check_object_collision(
                            obj, slot, all_primaries);
                        if (coll.collided) {
                            Object& other = object_mgr_.object(coll.other_slot);
                            uint8_t oi = static_cast<uint8_t>(other.type);
                            uint8_t of = (oi < static_cast<uint8_t>(ObjectType::COUNT))
                                         ? object_types_flags[oi] : 0;
                            if (!(of & ObjectTypeFlags::INTANGIBLE)) {
                                resolve_obj_overlap_response(
                                    obj, slot, other, coll.other_slot);
                            }
                        }
                    }
                }

                // Water splash — port of &2f69-&2f82. Emit one upward
                // PARTICLE_WATER if the object just dropped across the
                // waterline this frame.
                {
                    uint8_t wy = Water::get_waterline_y(obj.x.whole);
                    bool was_above = ou_old_y.whole < wy;
                    bool now_at    = obj.y.whole >= wy;
                    if (was_above && now_at && obj.velocity_y > 0) {
                        particles_.emit_directed(ParticleType::WATER,
                                                 0xc0, obj, cosmetic_rng_);
                    }
                }
                // Apply water effects (buoyancy + damping)
                Water::apply_water_effects(landscape_, obj, obj.weight(),
                                            every_four_frames_);

                // Tile-based wind / water-current (port of &3f18 / &3f41 /
                // &3fa3). Pushes the object toward the tile's wind/flow
                // vector. Runs after the global surface wind so a windy
                // cavern's local force can override the surface drift.
                Wind::apply_tile_environment(obj, landscape_, object_mgr_,
                                             frame_counter_, rng_, cosmetic_rng_,
                                             particles_);

                // SUPPORTED is driven by TileCollision::resolve's
                // landed_on_bottom flag (see above).

                // Symmetric overlap stamp (&2b14-&2b27) — late re-check
                // after physics so velocity-driven moves register this
                // frame; end-of-update OR #&80 clears staleness.
                auto obj_coll = Collision::check_object_collision(
                    obj, slot, object_mgr_.primary_array());
                if (obj_coll.collided) {
                    obj.touching = static_cast<uint8_t>(obj_coll.other_slot);
                    object_mgr_.object(obj_coll.other_slot).touching =
                        static_cast<uint8_t>(slot);
                } else {
                    obj.touching = 0x80;
                }
            }
        }

        // &3ef2 update_invisible_switch_tile collision branch — scans
        // primaries for type-filtered switches the player can't trigger
        // (e.g. flask-only at &7f,&77 idx &ae).
        {
            uint8_t sprite_id = obj.sprite;
            uint8_t h_frac = 0;
            if (sprite_id <= 0x80) {
                int h = sprite_atlas[sprite_id].h;
                h_frac = static_cast<uint8_t>((h > 0 ? (h - 1) : 0) * 8);
            }
            int head_abs = static_cast<int>(obj.y.whole) * 256 +
                           static_cast<int>(obj.y.fraction);
            int feet_abs = head_abs + h_frac;
            uint8_t head_tile_y = static_cast<uint8_t>((head_abs >> 8) & 0xff);
            uint8_t feet_tile_y = static_cast<uint8_t>((feet_abs >> 8) & 0xff);
            for (int ty = head_tile_y;
                 ty != static_cast<uint8_t>(feet_tile_y + 1);
                 ty = static_cast<uint8_t>(ty + 1)) {
                Behaviors::trigger_invisible_switch_at(
                    obj, obj.x.whole, static_cast<uint8_t>(ty),
                    object_mgr_, landscape_);
            }
        }

        // &1dd6-&1dda end-of-update touching clear — next frame starts
        // "not touching" unless a later slot's symmetric stamp
        // (&2b22-&2b24) re-sets it. Without this, static doors stay
        // pinned and close at &4cea's clamp speed instead of the table.
        obj.touching |= 0x80;

        // Step 18: Clear creation flags
        obj.flags &= ~ObjectFlags::NOT_PLOTTED;
        obj.flags &= ~ObjectFlags::NEWLY_CREATED;
    }
}
