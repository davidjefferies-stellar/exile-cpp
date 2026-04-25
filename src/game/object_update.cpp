#include "game/game.h"
#include "objects/physics.h"
#include "objects/collision.h"
#include "objects/object_data.h"
#include "objects/held_object.h"
#include "behaviours/behavior_dispatch.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
#include "world/wind.h"
#include "world/water.h"
#include "rendering/sprite_atlas.h"
#include <array>

// Port of &30e9-&30f5 apply_tile_collision_to_position_and_velocity's
// bounce math, applied per-axis (our integration is axis-separated
// whereas the 6502 uses a single vector).
//
//   magnitude = min(|v|, 0x20)           ; &30e7 cap
//   magnitude = max(magnitude - 2, 0)    ; &30e9 lose 2
//   magnitude = magnitude * 7 / 8        ; &30ef damp 7/8
//   new_v     = -sign(old_v) * magnitude ; reflect
//
// Settles within 3-4 bounces because each pass loses ~25% of magnitude
// (sub-2 gets clamped, 7/8 takes the rest).
static int8_t bounce_reflect(int8_t v_in) {
    int mag = (v_in < 0) ? -v_in : v_in;
    if (mag > 0x20) mag = 0x20;
    mag = (mag > 2) ? (mag - 2) : 0;
    mag = mag * 7 / 8;
    return static_cast<int8_t>(v_in > 0 ? -mag : mag);
}

// Companion to bounce_reflect for the *other* axis on a collision. The
// 6502's &30ef calculate_seven_eighths damps the *combined* vector
// magnitude and then splits it between vx and vy via the reflected angle.
// Our axis-separated integration would otherwise leave the non-colliding
// axis at full velocity, so a thrown grenade would slide forever along
// the floor after the first bounce. Apply the 7/8 damping-without-reflect
// on the other axis to approximate that magnitude coupling.
// Port of &3235 calculate_seven_eighths.
static int8_t damp_seven_eighths(int8_t v_in) {
    int mag = (v_in < 0) ? -v_in : v_in;
    mag = mag * 7 / 8;
    return static_cast<int8_t>(v_in > 0 ? mag : -mag);
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

        // Step 3: Handle held objects
        if (slot == held_object_slot_) {
            HeldObject::update_position(obj, player);
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
                }
                obj.timer--;
                continue; // Skip physics while teleporting
            }
        }

        // Step 10: Call type-specific update routine
        auto update_fn = AI::get_update_func(obj.type);
        if (update_fn) {
            UpdateContext uctx{object_mgr_, landscape_, rng_, frame_counter_,
                              every_four_frames_, every_eight_frames_,
                              every_sixteen_frames_, every_thirty_two_frames_,
                              every_sixty_four_frames_,
                              whistle_one_active_, whistle_two_activator_,
                              &whistle_one_collected_, &whistle_two_collected_,
                              player_mushroom_timers_,
                              player_keys_collected_,
                              &particles_,
                              held_object_slot_, player_object_fired_, slot};
            update_fn(obj, uctx);
        }

        // Step 11: Handle held object dropping
        if (slot == held_object_slot_) {
            if (HeldObject::should_drop(obj, player)) {
                HeldObject::drop(obj, object_mgr_.player(), held_object_slot_);
            }
        }

        // Step 12: Handle explosions
        if (obj.energy == 0) {
            object_mgr_.create_object_at(ObjectType::EXPLOSION, 0, obj);
            object_mgr_.remove_object(slot);
            if (slot == held_object_slot_) held_object_slot_ = 0x80;
            continue;
        }

        // Step 14: Reap PENDING_REMOVAL objects.
        //
        // The 6502's set_object_for_removal at &2516 just sets the flag;
        // the main loop later zeroes the slot. It does NOT bounce the
        // object back to tertiary — PENDING_REMOVAL means "this thing is
        // GONE" (collected, exploded, despawned) and reviving it would
        // undo whatever effect set the flag. Calling return_to_tertiary
        // here used to re-arm bit 7 of the data byte (since the else
        // branch unconditionally ORs 0x80) so collected items respawned
        // a few frames later.
        if (obj.flags & ObjectFlags::PENDING_REMOVAL) {
            object_mgr_.remove_object(slot);
            if (slot == held_object_slot_) held_object_slot_ = 0x80;
            continue;
        }

        // Step 9: Apply wind (only above surface)
        Wind::apply_surface_wind(obj);

        // Wind particle emission — port of &3f73 add_wind_particle_
        // using_velocities. One PARTICLE_WIND per frame with probability
        // `magnitude / 0x7f`: stronger wind → visibly more drift trails.
        // The 6502 emits through the current object's fraction field so
        // wind particles appear around the object being pushed; emit
        // helper does the same through the object source.
        {
            uint8_t mag = Wind::surface_wind_magnitude(obj);
            if (mag > 0 && (rng_.next() & 0x7f) < mag) {
                particles_.emit(ParticleType::WIND, 1, obj, rng_);
            }
        }

        // Step 15: Apply physics (gravity + velocity)
        if (slot != held_object_slot_) {
            // Per-type physics gate:
            //   weight 7           -> fully static; pin position, zero velocity.
            //   INTANGIBLE (0x80)  -> keeps its velocity but skips gravity
            //                         (explosions, lightning, transporter beams,
            //                         moving fireballs, invisible inert — these
            //                         are "not physical" in the 6502's sense).
            //   otherwise          -> gravity + wind + velocity integration as normal.
            uint8_t tidx = static_cast<uint8_t>(obj.type);
            uint8_t tflags = (tidx < static_cast<uint8_t>(ObjectType::COUNT))
                             ? object_types_flags[tidx] : 0;
            bool fully_static = obj.weight() >= 7;
            bool gravity_exempt = (tflags & ObjectTypeFlags::INTANGIBLE) != 0;

            if (fully_static) {
                obj.velocity_x = 0;
                obj.velocity_y = 0;
                // Static objects still need to notice when something is
                // touching them — switches fire on touching==player, doors
                // self-close on touching!=none, etc. The 6502's &2a64
                // check_for_collisions runs for every object, regardless of
                // weight; skipping it for weight-7 statics meant the switch
                // never saw the player even with overlapping AABBs.
                auto obj_coll = Collision::check_object_collision(
                    obj, slot,
                    reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(object_mgr_.object(0)));
                if (obj_coll.collided) {
                    obj.touching = static_cast<uint8_t>(obj_coll.other_slot);
                } else {
                    obj.touching = 0x80;
                }
            } else {
                Physics::apply_acceleration(obj, 0, 0, every_sixteen_frames_);
                if (gravity_exempt && obj.velocity_y > 0) {
                    // Undo the gravity +1 that apply_acceleration's y-axis
                    // hard-codes. Leaves any real upward/downward velocity
                    // the object gave itself intact.
                    obj.velocity_y--;
                }

                // Axis-separated integrate + undo-on-overlap. The 6502 sets
                // &1b (tile_top_or_bottom_collision) when such an undo happens;
                // we record it on obj.tile_collision for the next frame's
                // updater (bullets use it to explode on impact).
                //
                // Sprite AABB in 16-bit fraction-unit space. The 6502
                // stores (pixels-1)*16 in its width table and (rows-1)*8
                // in its height table (see &5e89 / the port at
                // tertiary_spawn.cpp:186); derive the same here so the
                // probe matches what the renderer actually draws.
                int obj_h_units = (obj.sprite <= 0x7c)
                    ? (sprite_atlas[obj.sprite].h > 0
                        ? (sprite_atlas[obj.sprite].h - 1) * 8 : 0)
                    : 0;
                int obj_w_units = (obj.sprite <= 0x7c)
                    ? (sprite_atlas[obj.sprite].w > 0
                        ? (sprite_atlas[obj.sprite].w - 1) * 16 : 0)
                    : 0;

                // Probe a single (tile_x, tile_y) cell at an explicit
                // sub-tile (x_frac, y_frac). Door tiles are swapped for
                // their live closed/open substitute so primaries collide
                // with closed doors like the player does, and the pattern-
                // based `tile_and_flip_obstructs_point` handles slopes
                // and spaceship-wall tiles where only a sub-section is
                // solid — same per-section resolution as the 6502's
                // &2fce LDA (&7c),Y / CMP y_fraction check.
                auto probe_tile = [&](uint8_t ttx, uint8_t tty,
                                      uint8_t ttx_frac,
                                      uint8_t tty_frac)->bool {
                    ResolvedTile res =
                        resolve_tile_with_tertiary(landscape_, ttx, tty);
                    uint8_t subst = Collision::substitute_door_for_obstruction(
                        res.tile_and_flip, res.data_offset,
                        reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                            object_mgr_.object(0)),
                        object_mgr_.tertiary_data_byte(res.data_offset));
                    return Collision::tile_and_flip_obstructs_point(
                        subst, ttx_frac, tty_frac);
                };

                // Sample all four corners of the sprite's AABB. The
                // previous implementation skipped right/bottom corners
                // when they landed in the SAME tile as top-left, which
                // misses slopes and partial-tile patterns: the corners
                // have different (x_frac, y_frac) even inside one tile,
                // and the obstruction threshold is a function of
                // x_section / y_frac, so "same tile" doesn't mean "same
                // obstruction answer". A red rolling robot (152-frac
                // tall) fits entirely inside a single tile row; without
                // the bottom probes the robot would slip through slope
                // tiles because only the top of the sprite (y_frac ~0,
                // above the slope surface) got tested.
                //
                // The probes are idempotent when corners happen to
                // collapse onto the same point (sprite width or height
                // zero), so always running all four is harmless.
                auto any_tile_solid = [&](uint8_t tx, uint8_t tx_frac,
                                          uint8_t ty, uint8_t ty_frac)->bool {
                    int right_abs  = static_cast<int>(tx) * 256 +
                                     static_cast<int>(tx_frac) + obj_w_units;
                    uint8_t r_tx   = static_cast<uint8_t>((right_abs >> 8) & 0xff);
                    uint8_t r_frac = static_cast<uint8_t>(right_abs & 0xff);
                    int bot_abs    = static_cast<int>(ty) * 256 +
                                     static_cast<int>(ty_frac) + obj_h_units;
                    uint8_t b_ty   = static_cast<uint8_t>((bot_abs >> 8) & 0xff);
                    uint8_t b_frac = static_cast<uint8_t>(bot_abs & 0xff);

                    if (probe_tile(tx,   ty,   tx_frac, ty_frac)) return true;
                    if (probe_tile(r_tx, ty,   r_frac,  ty_frac)) return true;
                    if (probe_tile(tx,   b_ty, tx_frac, b_frac))  return true;
                    if (probe_tile(r_tx, b_ty, r_frac,  b_frac))  return true;
                    return false;
                };

                // 6502 &1b `tile_top_or_bottom_collision` only flags
                // top/bottom (Y-axis) collisions — side (X-axis) hits go
                // into `left_obstruction` / `right_obstruction` which
                // bullets and piranha/wasp behaviour don't read. Mirror
                // that here: tile_collision is set *only* on the Y revert.
                //
                // The tile-pattern probe isn't enough on its own around
                // closed doors: STONE_SLOPE_78 (the door substitute) is
                // only solid in the tile's left quarter, while the door
                // sprite spans ~half the tile. For the remaining band we
                // fall back to overlaps_solid_object — same mechanism
                // player_motion.cpp uses. Without this a grenade dropped
                // near tile-centre passes through a closed door.
                auto& all_primaries =
                    reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                        object_mgr_.object(0));
                obj.tile_collision = false;
                obj.pre_collision_magnitude = 0;
                {
                    Fixed8_8 old_x = obj.x;
                    obj.x.add_velocity(obj.velocity_x);
                    bool blocked =
                        any_tile_solid(obj.x.whole, obj.x.fraction,
                                       obj.y.whole, obj.y.fraction) ||
                        Collision::overlaps_solid_object(obj, slot, all_primaries);
                    if (blocked) {
                        // Port of &30b7: capture the max-axis velocity
                        // BEFORE the reflect/damp so update_full_flask and
                        // friends can tell a hard collision from a scrape.
                        uint8_t pre = static_cast<uint8_t>(std::max(
                            std::abs(static_cast<int>(obj.velocity_x)),
                            std::abs(static_cast<int>(obj.velocity_y))));
                        obj.pre_collision_magnitude = pre;
                        obj.x = old_x;
                        obj.velocity_x = bounce_reflect(obj.velocity_x);
                        obj.velocity_y = damp_seven_eighths(obj.velocity_y);
                    }
                }
                {
                    Fixed8_8 old_y = obj.y;
                    obj.y.add_velocity(obj.velocity_y);
                    bool blocked =
                        any_tile_solid(obj.x.whole, obj.x.fraction,
                                       obj.y.whole, obj.y.fraction) ||
                        Collision::overlaps_solid_object(obj, slot, all_primaries);
                    if (blocked) {
                        uint8_t pre = static_cast<uint8_t>(std::max(
                            std::abs(static_cast<int>(obj.velocity_x)),
                            std::abs(static_cast<int>(obj.velocity_y))));
                        if (pre > obj.pre_collision_magnitude) {
                            obj.pre_collision_magnitude = pre;
                        }
                        obj.y = old_y;
                        if (obj.velocity_y > 0) obj.flags |= ObjectFlags::SUPPORTED;
                        obj.velocity_y = bounce_reflect(obj.velocity_y);
                        obj.velocity_x = damp_seven_eighths(obj.velocity_x);
                        obj.tile_collision = true;
                    }

                    // Water splash — port of &2f69-&2f82 add_water_
                    // particles_for_splash. If the object just crossed
                    // the waterline this frame moving downward, emit one
                    // PARTICLE_WATER at the crossing point. Angle is &c0
                    // in the original (straight up); our emit helper
                    // picks up the object's velocity as the particle's
                    // starting vector, which gets us rising droplets
                    // close enough to the 6502 effect.
                    uint8_t wy = Water::get_waterline_y(obj.x.whole);
                    bool was_above = old_y.whole < wy;
                    bool now_at    = obj.y.whole >= wy;
                    if (was_above && now_at && obj.velocity_y > 0) {
                        particles_.emit(ParticleType::WATER, 1, obj, rng_);
                    }
                }

                // Apply water effects (buoyancy + damping)
                Water::apply_water_effects(landscape_, obj, obj.weight());

                // SUPPORTED re-evaluation: probe the tile(s) one frac unit
                // below the sprite's actual bottom edge, across both the
                // left and right columns the sprite occupies. Previously
                // this was `is_tile_solid(x, y+1)` which (a) used the
                // coarse whole-tile-type check instead of the obstruction
                // pattern, (b) sampled at `y+1` which lands INSIDE a
                // multi-tile sprite rather than below it, and (c) only
                // tested the x-origin column. All three meant robots
                // with straddle-wide sprites or 2-tile heights had
                // SUPPORTED cleared each frame — the rolling-robot
                // "only roll when supported" branch then stalled.
                {
                    int foot_abs = static_cast<int>(obj.y.whole) * 256 +
                                   static_cast<int>(obj.y.fraction) +
                                   obj_h_units + 1;
                    uint8_t foot_ty   = static_cast<uint8_t>((foot_abs >> 8) & 0xff);
                    uint8_t foot_frac = static_cast<uint8_t>(foot_abs & 0xff);
                    int right_abs  = static_cast<int>(obj.x.whole) * 256 +
                                     static_cast<int>(obj.x.fraction) +
                                     obj_w_units;
                    uint8_t r_tx   = static_cast<uint8_t>((right_abs >> 8) & 0xff);
                    uint8_t r_frac = static_cast<uint8_t>(right_abs & 0xff);

                    // Always probe both columns — even when the sprite
                    // fits in one tile column (r_tx == obj.x.whole), the
                    // two probes are at different x_fracs within that
                    // tile, and slopes/partial-tile patterns give
                    // different obstruction answers. Same reasoning as
                    // the 4-corner AABB probe above.
                    bool supported =
                        probe_tile(obj.x.whole, foot_ty,
                                   obj.x.fraction, foot_frac) ||
                        probe_tile(r_tx, foot_ty, r_frac, foot_frac);
                    if (supported) obj.flags |=  ObjectFlags::SUPPORTED;
                    else           obj.flags &= ~ObjectFlags::SUPPORTED;
                }

                // Object-object collision: set touching field
                auto obj_coll = Collision::check_object_collision(
                    obj, slot,
                    reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(object_mgr_.object(0)));
                if (obj_coll.collided) {
                    obj.touching = static_cast<uint8_t>(obj_coll.other_slot);
                } else {
                    obj.touching = 0x80; // Not touching anything
                }
            }
        }

        // Step 18: Clear creation flags
        obj.flags &= ~ObjectFlags::NOT_PLOTTED;
        obj.flags &= ~ObjectFlags::NEWLY_CREATED;
    }
}
