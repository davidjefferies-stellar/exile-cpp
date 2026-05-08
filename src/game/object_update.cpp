#include "game/game.h"
#include "objects/physics.h"
#include "objects/collision.h"
#include "objects/tile_collision.h"
#include "objects/object_data.h"
#include "objects/held_object.h"
#include "behaviours/behavior_dispatch.h"
#include "behaviours/npc_helpers.h"
#include "audio/audio.h"
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

// AABB-corner solid probe for the held primary's penetration check at
// step 3. Returns true if any of the four corners (top-left, top-right,
// bottom-left, bottom-right) sits inside a solid tile. Used by the
// axis-separated revert to test "if I left this axis at the new flush
// value but kept the other axis at the old held position, does the
// sprite still penetrate a wall?".
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

// Full 18-step update loop - port of &1a0b-&1e18
void Game::update_objects() {
    const Object& player = object_mgr_.player();

    // Secondary object promotion
    object_mgr_.promote_selective(rng_);

    // Main loop over slots 1-15
    for (int slot = 1; slot < GameConstants::PRIMARY_OBJECT_SLOTS; slot++) {
        Object& obj = object_mgr_.object(slot);
        if (!obj.is_active()) continue;

        // Step 3: Handle held objects.
        //
        // 6502 &1afd-&1b54: position the held primary flush to the
        // player's facing side (objects_x = player_x + offset), then
        // fall through to JSR check_for_collisions at &1b54 — exactly
        // the same routine every other object runs. If the flush
        // position penetrates solid geometry, that routine reverts
        // position so the held bumps the wall and stays there while the
        // player moves on. Drift accumulates, and consider_dropping_
        // held_object (&1cab) eventually fires HeldObject::should_drop
        // to drop it.
        //
        // Our port previously skipped tile collision for held primaries
        // (step 15 has `if (slot != held_object_slot_)`), so the held
        // visually clipped through walls. Re-add the revert here without
        // touching step 15: snapshot the position before update_position,
        // run a tile-overlap test on the four AABB corners, and if any
        // sit inside a solid tile, restore the snapshot. The motion
        // delta over a single frame is small, so a corner-only probe
        // catches penetrations the same way the 6502's section sweep
        // would.
        if (slot == held_object_slot_) {
            Fixed8_8 old_held_x = obj.x;
            Fixed8_8 old_held_y = obj.y;
            HeldObject::update_position(obj, player);
            Fixed8_8 new_held_x = obj.x;
            Fixed8_8 new_held_y = obj.y;
            int hw = (obj.sprite <= 0x80)
                ? (sprite_atlas[obj.sprite].w > 0
                    ? (sprite_atlas[obj.sprite].w - 1) * 16 : 0) : 0;
            int hh = (obj.sprite <= 0x80)
                ? (sprite_atlas[obj.sprite].h > 0
                    ? (sprite_atlas[obj.sprite].h - 1) * 8 : 0) : 0;
            // Axis-separated revert. The 6502's check_for_collisions reverts
            // x and y independently: a horizontal wall doesn't pin the
            // held vertically, and vice versa. Reverting both together
            // (the previous version) made the held stick at its old y
            // while the player jumped, leaving a tall gap until a facing
            // flip refreshed update_position to a non-penetrating
            // position on the other side.
            //
            // Try Y first (with old x), then X (with whichever y we
            // settled on). Each axis only reverts if the move along
            // THAT axis caused the overlap.
            obj.x = old_held_x;
            obj.y = new_held_y;
            if (held_aabb_solid(landscape_, obj.x, obj.y, hw, hh)) {
                obj.y = old_held_y;
            }
            obj.x = new_held_x;
            if (held_aabb_solid(landscape_, obj.x, obj.y, hw, hh)) {
                obj.x = old_held_x;
            }
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

        // Step 9b: Refresh `touching` BEFORE the type-specific update reads it.
        // The 6502 calls check_for_collisions at &1b54 — ahead of the per-type
        // dispatch — so update routines (most importantly &4704 update_triax,
        // which absorbs the destinator on its very first frame) see a touching
        // field that reflects the current overlap. Our flow used to detect
        // collisions only AFTER the type update, leaving frame-1 update_triax
        // looking at the initial `touching = 0xff`. The destinator absorb-and-
        // teleport beat never fired, leaving Triax pinned inside the ceiling
        // tile (the spawn point at (&99, &3b) overlaps the ship-roof tile, and
        // gravity's integrate-and-revert can't unstick him from a position
        // that's already inside solid geometry).
        {
            auto early_coll = Collision::check_object_collision(
                obj, slot,
                reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                    object_mgr_.object(0)));
            obj.touching = early_coll.collided
                ? static_cast<uint8_t>(early_coll.other_slot)
                : 0x80;
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
                              held_object_slot_, player_object_fired_, slot,
                              imp_gifts_remaining_,
                              &background_flash_cooldown_,
                              renderer_ && renderer_->damage_overlay_enabled()
                                  ? &damage_events_ : nullptr,
                              &explosion_timer_,
                              &flooding_state_,
                              &floating_labels_};
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
            object_mgr_.create_object_centered(ObjectType::EXPLOSION, 0, obj);
            // &40db-&40de play_sound_on_channel_zero (priority): the
            // generic "object exploded" boom. Wired here, the central
            // energy-hits-zero hook, so every type of explosion picks
            // it up — bullets, grenades, robots, NPCs.
            static constexpr uint8_t kSoundExplosion[4] = { 0x17, 0x03, 0x11, 0x04 };
            Audio::play_at(Audio::CH_PRIORITY, kSoundExplosion,
                           obj.x.whole, obj.y.whole);
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
        // The 6502's flow at &3f73-&3f91 is: (1) calculate_angle_from_vector
        // turns the active wind (vector_x, vector_y) into &b5; (2) the
        // probability gate at &3f76-&3f7b checks that &b7 magnitude exceeds
        // a random byte; (3) add_particle reads the angle and the type's
        // spd_rand/spd_base to pick a base velocity in the wind direction.
        // emit_directed reproduces step 3, so wind particles now drift in
        // the actual wind direction rather than a random one.
        {
            int8_t wvx = 0, wvy = 0;
            Wind::surface_wind_vector(obj, wvx, wvy);
            uint8_t mag = Wind::surface_wind_magnitude(obj);
            if (mag > 0 && (rng_.next() & 0x7f) < mag) {
                uint8_t angle = NPC::angle_from_deltas(wvx, wvy);
                particles_.emit_directed(ParticleType::WIND, angle, obj, rng_);
            }
        }

        // Step 15: Apply physics (gravity + velocity)
        if (slot != held_object_slot_) {
            // Per-type physics gate:
            //   weight 7           -> fully static; pin position, zero velocity.
            //   "undisturbed pin"  -> energy bit 7 set on a type whose update
            //                         routine pins via consider_disturbing_object
            //                         (collectables, inactive grenade). The
            //                         6502 keeps these still by simply NOT
            //                         calling add_A_to_position from inside
            //                         the type's update — there's no global
            //                         "integrate velocity into position" step
            //                         like ours has. Without an equivalent
            //                         skip here, gravity adds +1 to vy each
            //                         frame and our generic integration drifts
            //                         the object downward even after the
            //                         type's pin zeroed velocity. Treat it
            //                         like fully_static.
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
            // "undisturbed" pin via energy bit 7 — see comment above.
            // Active for the types whose update_fn runs the consider_
            // disturbing_object pin (collectables 0x4a..0x64 and the
            // inactive grenade dispatch chain). The bit gets cleared on
            // touch in update_collectable, after which physics resumes.
            bool pin_undisturbed = (obj.energy & 0x80) != 0 &&
                                    static_cast<uint8_t>(obj.type) >= 0x4a;
            // BUSH pins unconditionally. &4ba9 update_bush is just JSR
            // set_this_object_velocities_to_zero + JMP set_this_object_
            // position_from_previous_position — the bush freezes in
            // place every frame regardless of touch. Bushes are weight 6
            // (flags 0xd6 & 0x07), so they fall outside fully_static; pin
            // them explicitly here or a wasp/projectile leaves residual
            // velocity that gravity then integrates.
            bool pin_bush = obj.type == ObjectType::BUSH;

            if (fully_static || pin_undisturbed || pin_bush) {
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

                // ====================================================
                // Tile collision — TileCollision::resolve port of the
                // 6502 &2f8c-&30df chain. Walks AABB edges, builds an
                // obstruction vector, pushes the object out perpendicular
                // to the surface, and reflects velocity at reduced angle.
                // Same module the player uses; covers slopes, walls,
                // floors, ceilings under one pipeline.
                //
                // Object-object overlap is checked AFTER as a hard revert
                // so primaries don't pass through each other. The 6502
                // handles object-object via mass-ratio velocity transfer
                // at &2bb6 — already partially modelled by step 9b's
                // touching probe and the player_motion equivalent; for
                // primaries we keep the simpler hard-block until we have
                // a faithful port of &2bb6.
                // ====================================================
                Fixed8_8 ou_old_x = obj.x;
                Fixed8_8 ou_old_y = obj.y;
                obj.x.add_velocity(obj.velocity_x);
                obj.y.add_velocity(obj.velocity_y);

                // Clear SUPPORTED before the resolve; it'll be re-set
                // below if landed_on_bottom comes back true. The 6502
                // doesn't have a SUPPORTED flag per se — it derives the
                // "grounded" state from tile_collision_y_flags bit 7
                // (set when the collision was "more to the bottom" of
                // the AABB, i.e. the object landed on something).
                obj.flags &= ~ObjectFlags::SUPPORTED;

                TileCollision::Result tcr = TileCollision::resolve(
                    obj, ou_old_x.whole, ou_old_x.fraction,
                    ou_old_y.whole, ou_old_y.fraction,
                    landscape_, object_mgr_,
                    /*skip_slot=*/-1);
                obj.tile_collision = tcr.top_or_bottom_collision;
                if (tcr.landed_on_bottom) {
                    obj.flags |= ObjectFlags::SUPPORTED;
                }

                // Object-object overlap revert. Newly-spawned bullets are
                // exempt because their initial position deliberately
                // overlaps the firer's AABB (port of &2afd-&2b0e).
                //
                // Projectile types (ICER_BULLET 0x13 .. PLASMA_BALL 0x19)
                // also skip the revert. The 6502's check_for_collisions
                // sets this_object_touching during overlap detection
                // (&2b18) and the per-bullet update consumes that on
                // the same beat — but our hard revert moves the bullet
                // BACK out of the overlap and step 9b then re-probes
                // touching from the post-revert position, finding no
                // overlap. Letting the bullet penetrate the AABB for
                // one frame lets step 9b on the NEXT frame catch the
                // overlap; common_bullet_update then sets energy=0,
                // and step 12 (which runs before step 15) explodes
                // the bullet that same frame. Net delay: 1 frame.
                {
                    uint8_t self_idx = static_cast<uint8_t>(obj.type);
                    bool is_projectile = (self_idx >= 0x13 && self_idx <= 0x19);
                    auto& all_primaries =
                        reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                            object_mgr_.object(0));
                    if (!is_projectile &&
                        Collision::overlaps_solid_object(obj, slot, all_primaries) &&
                        !(obj.flags & ObjectFlags::NEWLY_CREATED) &&
                        !(tflags & ObjectTypeFlags::INTANGIBLE)) {
                        obj.x = ou_old_x;
                        obj.y = ou_old_y;
                        obj.velocity_x = 0;
                        obj.velocity_y = 0;
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
                                                 0xc0, obj, rng_);
                    }
                }
#if 0
                // Sprite AABB in 16-bit fraction-unit space. The 6502
                // stores (pixels-1)*16 in its width table and (rows-1)*8
                // in its height table (see &5e89 / the port at
                // tertiary_spawn.cpp:186); derive the same here so the
                // probe matches what the renderer actually draws.
                int obj_h_units = (obj.sprite <= 0x80)
                    ? (sprite_atlas[obj.sprite].h > 0
                        ? (sprite_atlas[obj.sprite].h - 1) * 8 : 0)
                    : 0;
                int obj_w_units = (obj.sprite <= 0x80)
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

                // Per-axis "escape" relaxation — partial port of &306c
                // apply_tile_collision_to_position_and_velocity. Allow
                // the move only if the start position has its OPPOSITE
                // side blocked (motion in the escape direction).
                auto side_corners = [&](uint8_t tx, uint8_t tx_frac,
                                        uint8_t ty, uint8_t ty_frac,
                                        bool& top, bool& bot,
                                        bool& left, bool& right) {
                    int right_abs = static_cast<int>(tx) * 256 +
                                    static_cast<int>(tx_frac) + obj_w_units;
                    uint8_t r_tx   = static_cast<uint8_t>((right_abs >> 8) & 0xff);
                    uint8_t r_frac = static_cast<uint8_t>(right_abs & 0xff);
                    int bot_abs   = static_cast<int>(ty) * 256 +
                                    static_cast<int>(ty_frac) + obj_h_units;
                    uint8_t b_ty   = static_cast<uint8_t>((bot_abs >> 8) & 0xff);
                    uint8_t b_frac = static_cast<uint8_t>(bot_abs & 0xff);
                    bool tl = probe_tile(tx,   ty,   tx_frac, ty_frac);
                    bool tr = probe_tile(r_tx, ty,   r_frac,  ty_frac);
                    bool bl = probe_tile(tx,   b_ty, tx_frac, b_frac);
                    bool br = probe_tile(r_tx, b_ty, r_frac,  b_frac);
                    top   = tl || tr;
                    bot   = bl || br;
                    left  = tl || bl;
                    right = tr || br;
                };
                bool start_top = false, start_bot = false;
                bool start_left = false, start_right = false;
                side_corners(obj.x.whole, obj.x.fraction,
                             obj.y.whole, obj.y.fraction,
                             start_top, start_bot, start_left, start_right);

                // Object-object overlap at start relaxes for newly-spawned
                // bullets and for intangible objects (explosions, lightning,
                // transporter beams, fireballs). The relax is needed because
                // a bullet's spawn position can overlap its firer's AABB on
                // frame 1 — without an escape, the next frame's revert
                // would pin it inside the firer.
                //
                // Tightened: previously this fired for ANY established
                // primary, which let imps that ended up inside a closed
                // door's AABB (e.g. door closed on them, or tunnelling on
                // a prior frame) walk straight out the other side. Restrict
                // to NEWLY_CREATED + INTANGIBLE so an imp that somehow gets
                // inside a door is stuck there (acceptable; a stuck imp is
                // far less visible than one phasing through a locked door).
                bool start_obj_overlap = false;
                if ((obj.flags & ObjectFlags::NEWLY_CREATED) ||
                    (tflags & ObjectTypeFlags::INTANGIBLE)) {
                    start_obj_overlap =
                        Collision::overlaps_solid_object(obj, slot, all_primaries);
                }
                {
                    Fixed8_8 old_x = obj.x;
                    obj.x.add_velocity(obj.velocity_x);
                    bool tile_blocked =
                        any_tile_solid(obj.x.whole, obj.x.fraction,
                                       obj.y.whole, obj.y.fraction);
                    bool obj_blocked =
                        Collision::overlaps_solid_object(obj, slot, all_primaries);
                    // Object-overlap relax only escapes another object's AABB,
                    // never punches through tiles — otherwise a player pushing
                    // a resting collectable would walk it through walls/floor.
                    //
                    // Side-relax (clauses 2/3) is tile-pattern-only and was
                    // letting fed imps phase through closed locked doors:
                    // substitute_door_for_obstruction returns STONE_SLOPE_78
                    // (solid only in the left tile-quarter) so an imp whose
                    // left edge sat in the solid quarter and right edge in
                    // the empty band registered start_left && !start_right,
                    // and rightward motion got relaxed despite the door
                    // primary's full-tile AABB. Gate the side-relax on
                    // !obj_blocked so heavier-primary collisions (door,
                    // cannon, hive) outrank the tile-pattern escape; the
                    // bullet-escape clause 1 stays unconditional because
                    // bullets must leave their firer's AABB on frame 1.
                    bool relax_x = (start_obj_overlap && !tile_blocked) ||
                        (obj.velocity_x > 0 && start_left  && !start_right && !obj_blocked) ||
                        (obj.velocity_x < 0 && start_right && !start_left  && !obj_blocked);
                    if ((tile_blocked || obj_blocked) && !relax_x) {
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
                    bool tile_blocked =
                        any_tile_solid(obj.x.whole, obj.x.fraction,
                                       obj.y.whole, obj.y.fraction);
                    bool obj_blocked =
                        Collision::overlaps_solid_object(obj, slot, all_primaries);
                    // Same side-relax gating as the X-axis above: a heavier
                    // primary (closed horizontal door, etc.) must outrank
                    // the tile-pattern escape. Without !obj_blocked here, a
                    // jumping imp whose top edge clipped the door tile's
                    // sub-tile-thin SPACESHIP_WALL_HORIZONTAL_QUARTER pattern
                    // could escape upward through the door's primary AABB.
                    bool relax_y = (start_obj_overlap && !tile_blocked) ||
                        (obj.velocity_y > 0 && start_top && !start_bot && !obj_blocked) ||
                        (obj.velocity_y < 0 && start_bot && !start_top && !obj_blocked);
                    if ((tile_blocked || obj_blocked) && !relax_y) {
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
                    // PARTICLE_WATER at the crossing point with angle
                    // &c0 (straight up): emit_directed feeds the angle +
                    // the type's spd_rand/spd_base through &2357 so the
                    // droplet starts with vy ≈ -(spd_base..spd_base+spd_rand)
                    // and visibly leaps out of the water.
                    uint8_t wy = Water::get_waterline_y(obj.x.whole);
                    bool was_above = old_y.whole < wy;
                    bool now_at    = obj.y.whole >= wy;
                    if (was_above && now_at && obj.velocity_y > 0) {
                        particles_.emit_directed(ParticleType::WATER,
                                                 0xc0, obj, rng_);
                    }
                }
#endif // legacy axis-separated revert disabled

                // Apply water effects (buoyancy + damping)
                Water::apply_water_effects(landscape_, obj, obj.weight(),
                                            every_four_frames_);

                // Tile-based wind / water-current (port of &3f18 / &3f41 /
                // &3fa3). Pushes the object toward the tile's wind/flow
                // vector. Runs after the global surface wind so a windy
                // cavern's local force can override the surface drift.
                Wind::apply_tile_environment(obj, landscape_, object_mgr_,
                                             frame_counter_, rng_, particles_);

                // SUPPORTED is now driven by TileCollision::resolve's
                // landed_on_bottom flag (see above). The legacy probe
                // is kept under #if 0 for diff-readability and will be
                // deleted with the rest of the legacy block.
#if 0
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
                    bool supported =
                        probe_tile(obj.x.whole, foot_ty,
                                   obj.x.fraction, foot_frac) ||
                        probe_tile(r_tx, foot_ty, r_frac, foot_frac);
                    if (supported) obj.flags |=  ObjectFlags::SUPPORTED;
                    else           obj.flags &= ~ObjectFlags::SUPPORTED;
                }
#endif

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
