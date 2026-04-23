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
                // Probe tiles at both the object's top (obj.y.whole) AND the
                // tile the bottom edge enters — matches the 6502's use of
                // this_object_maximum_y (obj.y + height) at &2a48. A short
                // object (32-unit grenade, bullet etc.) can have its top
                // stuck in an empty tile while its bottom plunges into a
                // solid tile below; checking only the top lets it sink
                // visibly into the floor before overflow kicks in.
                int obj_h_units = (obj.sprite <= 0x7c)
                    ? (sprite_atlas[obj.sprite].h > 0
                        ? (sprite_atlas[obj.sprite].h - 1) * 8 : 0)
                    : 0;

                // Per-section obstruction probe. `is_tile_solid` classifies a
                // whole tile by type which is too coarse for spaceship-wall
                // tiles (and slopes) where the solid region is only a
                // sub-section of the tile. NPCs spawned inside such a tile's
                // passable region — Triax at (&99, &3b) in the ship interior
                // being the classic case — would otherwise be flagged as
                // embedded in solid geometry and every gravity step would
                // revert, pinning them in place. `point_in_tile_solid`
                // evaluates the tile's obstruction pattern at the exact
                // (x_frac, y_frac) of the sample, matching the 6502's
                // &2fce LDA (&7c),Y + CMP y_fraction check at per-section
                // resolution.
                // Probe with door substitution. The raw METAL_DOOR /
                // STONE_DOOR tile types are marked non-solid so unresolved
                // callers don't phase-block around them; we resolve them
                // here to their live closed/open substitute (STONE_SLOPE_78
                // or SPACE) so thrown grenades and other primaries collide
                // with closed doors instead of dropping through. Player
                // motion uses the same substitute_door_for_obstruction
                // pattern in player_motion.cpp.
                auto probe_tile = [&](uint8_t ttx, uint8_t tty,
                                      uint8_t tty_frac)->bool {
                    ResolvedTile res =
                        resolve_tile_with_tertiary(landscape_, ttx, tty);
                    uint8_t subst = Collision::substitute_door_for_obstruction(
                        res.tile_and_flip, res.data_offset,
                        reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                            object_mgr_.object(0)),
                        object_mgr_.tertiary_data_byte(res.data_offset));
                    return Collision::tile_and_flip_obstructs_point(
                        subst, obj.x.fraction, tty_frac);
                };
                auto any_tile_solid = [&](uint8_t tx, uint8_t ty,
                                          uint8_t ty_frac)->bool {
                    // Top-edge probe: the sample point is within the
                    // current tile at (x_frac, ty_frac).
                    if (probe_tile(tx, ty, ty_frac)) return true;
                    // Bottom-edge probe: the object's bottom may be in a
                    // different tile below. Translate into that tile's
                    // local y_frac and check there too.
                    int bottom_abs = static_cast<int>(ty) * 256 +
                                     static_cast<int>(ty_frac) + obj_h_units;
                    uint8_t bottom_tile_y =
                        static_cast<uint8_t>((bottom_abs >> 8) & 0xff);
                    uint8_t bottom_y_frac =
                        static_cast<uint8_t>(bottom_abs & 0xff);
                    if (bottom_tile_y != ty &&
                        probe_tile(tx, bottom_tile_y, bottom_y_frac)) {
                        return true;
                    }
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
                {
                    Fixed8_8 old_x = obj.x;
                    obj.x.add_velocity(obj.velocity_x);
                    bool blocked =
                        any_tile_solid(obj.x.whole, obj.y.whole, obj.y.fraction) ||
                        Collision::overlaps_solid_object(obj, slot, all_primaries);
                    if (blocked) {
                        obj.x = old_x;
                        obj.velocity_x = bounce_reflect(obj.velocity_x);
                        obj.velocity_y = damp_seven_eighths(obj.velocity_y);
                    }
                }
                {
                    Fixed8_8 old_y = obj.y;
                    obj.y.add_velocity(obj.velocity_y);
                    bool blocked =
                        any_tile_solid(obj.x.whole, obj.y.whole, obj.y.fraction) ||
                        Collision::overlaps_solid_object(obj, slot, all_primaries);
                    if (blocked) {
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
                Water::apply_water_effects(obj, obj.weight());

                if (Collision::is_tile_solid(landscape_, obj.x.whole,
                                             static_cast<uint8_t>(obj.y.whole + 1))) {
                    obj.flags |= ObjectFlags::SUPPORTED;
                } else {
                    obj.flags &= ~ObjectFlags::SUPPORTED;
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
