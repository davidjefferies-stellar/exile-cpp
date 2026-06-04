#include "game/game.h"
#include "behaviours/environment.h"
#include "behaviours/npc_helpers.h"
#include "objects/physics.h"
#include "objects/collision.h"
#include "objects/tile_collision.h"
#include "objects/object_data.h"
#include "objects/object_manager.h"
#include "audio/audio.h"
#include "rendering/sprite_atlas.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
#include "world/obstruction.h"
#include "world/wind.h"
#include "world/water.h"
#include <array>

// &3bad check_if_slope_is_too_sleep_for_npc with player walking-type
// index 0 — max angle 0x32 (~70°). Uses 6502 two's complement abs so
// 0x80 maps to itself (definitely too steep).
static bool slope_too_steep_for_player(uint8_t tile_angle) {
    uint8_t abs_a = (tile_angle & 0x80)
        ? static_cast<uint8_t>(static_cast<uint8_t>(~tile_angle) + 1)
        : tile_angle;
    return abs_a >= 0x32;
}

// Sweep one tile's sections, return most-grounded threshold across the
// AABB width. Picks LOWEST for ground-like, HIGHEST for ceiling-like —
// needed for partial-solid tiles (STONE_SLOPE_78) where a single probe
// at player.x.fraction misses the solid band. Port of &2fb8's loop.
static uint8_t single_tile_effective_threshold(uint8_t tile_type, bool flip_h,
                                                 bool flip_v, uint8_t x_start,
                                                 int sprite_w_frac,
                                                 bool ceiling_like) {
    int end = static_cast<int>(x_start) + sprite_w_frac;
    uint8_t best = tile_threshold_at_x(tile_type, flip_h, flip_v, x_start);
    for (int px = (static_cast<int>(x_start) | 0x1f) + 1; px <= end; px += 0x20) {
        uint8_t t = tile_threshold_at_x(tile_type, flip_h, flip_v,
                                         static_cast<uint8_t>(px & 0xff));
        if (ceiling_like) { if (t > best) best = t; }
        else              { if (t < best) best = t; }
    }
    return best;
}

// Probe supporting tile at player centre x; use directly when obstructing
// so snap follows the slope surface. Fall back to MIN/MAX-over-width for
// partial-solid tiles (STONE_SLOPE_78) where the centre doesn't sit over
// the solid band.
static uint8_t slope_tracking_threshold(uint8_t tile_type, bool flip_h,
                                         bool flip_v, uint8_t x_at,
                                         int sprite_w_frac,
                                         bool ceiling_like) {
    uint8_t at_x = tile_threshold_at_x(tile_type, flip_h, flip_v, x_at);
    if (!ceiling_like) {
        // Ground-like: thresh=0xff means "no obstruction at this x".
        if (at_x < 0xff) return at_x;
    } else {
        // Ceiling-like: thresh=0x00 means "no obstruction at this x".
        if (at_x > 0x00) return at_x;
    }
    return single_tile_effective_threshold(tile_type, flip_h, flip_v,
                                            x_at, sprite_w_frac, ceiling_like);
}

// Probe a single tile cell with door substitution. Used by both the
// row-span and column-span helpers below.
static bool probe_point_with_door_subst(
    const Landscape& landscape, ObjectManager& mgr,
    uint8_t tile_x, uint8_t tile_y, uint8_t x_frac, uint8_t y_frac)
{
    ResolvedTile r = resolve_tile_with_tertiary(landscape, tile_x, tile_y);
    uint8_t tile = Collision::substitute_door_for_obstruction(
        r.tile_and_flip, r.data_offset,
        reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(mgr.object(0)),
        mgr.tertiary_data_byte(r.data_offset));
    uint8_t type = tile & TileFlip::TYPE_MASK;
    if (!Collision::is_tile_type_solid(type)) return false;
    return Collision::tile_and_flip_obstructs_point(tile, x_frac, y_frac);
}

// Horizontal blocking via per-row old/new diff: block only when the move
// introduces a NEW obstruction at that row. Pre-existing overlaps
// (head-in-ceiling, feet-on-floor) are skipped or the player freezes.
// Port-analogue of &2e8a depth + &306c direction-vector resolution.
static bool column_move_blocked(
    const Landscape& landscape, ObjectManager& mgr,
    uint8_t old_tx, uint8_t old_xf,
    uint8_t new_tx, uint8_t new_xf,
    uint8_t y_whole, uint8_t y_frac, int sprite_h_frac)
{
    int top_abs = static_cast<int>(y_whole) * 256 + static_cast<int>(y_frac);
    int bot_abs = top_abs + sprite_h_frac;
    // Step-up tolerance: probe only the TOP HALF of the sprite. Lower-
    // half obstructions are treated as slopes/steps (Y-snap will lift the
    // player). Walls must extend into the upper half to block X motion.
    // Port shortcut for &306c's depth-ratio slope-vs-wall classification.
    int gate_bot = top_abs + (bot_abs - top_abs) / 2;
    if (gate_bot < top_abs) gate_bot = top_abs;
    for (int sy = top_abs; sy <= gate_bot; sy += 0x20) {
        int clamp_sy   = sy > gate_bot ? gate_bot : sy;
        uint8_t ty     = static_cast<uint8_t>((clamp_sy >> 8) & 0xff);
        uint8_t ty_frac = static_cast<uint8_t>(clamp_sy & 0xff);
        bool old_solid = probe_point_with_door_subst(
            landscape, mgr, old_tx, ty, old_xf, ty_frac);
        bool new_solid = probe_point_with_door_subst(
            landscape, mgr, new_tx, ty, new_xf, ty_frac);
        if (new_solid && !old_solid) return true;
    }
    // Catch the gate-bottom row if the step skipped past it.
    uint8_t ty     = static_cast<uint8_t>((gate_bot >> 8) & 0xff);
    uint8_t ty_frac = static_cast<uint8_t>(gate_bot & 0xff);
    bool old_solid = probe_point_with_door_subst(
        landscape, mgr, old_tx, ty, old_xf, ty_frac);
    bool new_solid = probe_point_with_door_subst(
        landscape, mgr, new_tx, ty, new_xf, ty_frac);
    return new_solid && !old_solid;
}

// Port of &2fb8 check_for_tile_collisions_on_top_and_bottom_edges_tile_
// loop. Walks each 32-frac x-section across the AABB, crossing tiles at
// &2fed; first obstructed section wins. Gates VERTICAL motion — caller
// passes head y_frac for up-probe, feet y_frac for down-probe.
static bool player_aabb_obstructed(
    const Landscape& landscape, ObjectManager& mgr,
    uint8_t tile_y, uint8_t x_whole, uint8_t x_frac, int sprite_w_frac,
    uint8_t y_frac)
{
    // 16-bit absolute X of the AABB's left and right edges.
    int left  = static_cast<int>(x_whole) * 256 + static_cast<int>(x_frac);
    int right = left + sprite_w_frac;

    // Iterate section starts: the section containing `left`, then every
    // 0x20-fraction boundary up to (but not past) `right`.
    int first_section = left & ~0x1f;
    for (int sx = first_section; sx <= right; sx += 0x20) {
        uint8_t tile_x_s  = static_cast<uint8_t>((sx >> 8) & 0xff);
        uint8_t x_frac_s  = static_cast<uint8_t>(sx & 0xff);

        // Resolve THIS section's tile (crossing boundaries as needed) and
        // run the same door-substitute dance the main block does.
        ResolvedTile r = resolve_tile_with_tertiary(landscape, tile_x_s, tile_y);
        uint8_t tile = Collision::substitute_door_for_obstruction(
            r.tile_and_flip, r.data_offset,
            reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(mgr.object(0)),
            mgr.tertiary_data_byte(r.data_offset));
        uint8_t type = tile & TileFlip::TYPE_MASK;
        if (!Collision::is_tile_type_solid(type)) continue;

        if (Collision::point_in_tile_solid(landscape, tile_x_s, tile_y,
                                           x_frac_s, y_frac)) {
            // is_tile_type_solid passed but point_in_tile_solid needs the
            // raw tile byte (post-substitute) so call the tile_and_flip
            // overload if available; otherwise fall back via a manual
            // obstruction lookup.
            return true;
        }
        // point_in_tile_solid reads the RAW tile from the landscape, not
        // our door-substituted `tile`. For METAL_DOOR / STONE_DOOR the raw
        // tile is non-solid (they're classified as passable tile types),
        // so the substitute STONE_SLOPE_78 obstruction is missed unless
        // we probe the substituted byte directly.
        bool fh = (tile & TileFlip::HORIZONTAL) != 0;
        bool fv = (tile & TileFlip::VERTICAL)   != 0;
        uint8_t thresh = tile_threshold_at_x(type, fh, fv, x_frac_s);
        bool coll_fv = fv ^ tile_obstruction_v_flip_bit(type);
        bool obstructed = coll_fv ? (y_frac <= thresh)
                                  : (y_frac >  thresh);
        if (obstructed) return true;
    }
    return false;
}

// &1aea-&1af5: while the player holds an object its weight comes from
// player_weights_when_holding_objects_table (&19ac), indexed by the held
// object's weight; otherwise it's the player's own weight (3). The whole
// frame reads this single value (&38), so buoyancy and mass-ratio
// collisions all see the heavier player — a held big fish (weight 5)
// zeroes buoyancy so the player sinks and walks the seabed.
static uint8_t player_effective_weight(const Object& player,
                                       const Object* held) {
    if (!held) return player.weight();
    static constexpr uint8_t kHoldWeights[7] = {2, 2, 3, 4, 4, 5, 6};
    uint8_t w = held->weight();
    return kHoldWeights[w > 6 ? 6 : w];
}

// AABB scan for a lighter active primary overlapping the player. Used
// by both axes' &2bb6 heavier-hits-lighter branch. Returns the slot, or
// -1 if none. Skips held<->player (&2afd-&2b0e), intangible (&0354 bit
// 7), and zero-weight types.
static int find_lighter_overlap(const Object& player, ObjectManager& mgr,
                                uint8_t held_slot, uint8_t pw_weight,
                                int sprite_w_frac, int sprite_h_frac) {
    int px = player.x.whole * 256 + player.x.fraction;
    int py = player.y.whole * 256 + player.y.fraction;
    for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        if (i == held_slot) continue;
        const Object& other = mgr.object(i);
        if (!other.is_active()) continue;
        uint8_t idx = static_cast<uint8_t>(other.type);
        if (idx >= static_cast<uint8_t>(ObjectType::COUNT)) continue;
        uint8_t tflags = object_types_flags[idx];
        if (tflags & ObjectTypeFlags::INTANGIBLE) continue;
        uint8_t ow = tflags & ObjectTypeFlags::WEIGHT_MASK;
        if (ow == 0 || ow >= pw_weight) continue;
        // Port deviation: skip just-fired bullets sitting inside the
        // player AABB. The 6502 fires after &1b54 collision so they
        // never appear here; we fire before physics, so without the
        // skip apply_mass_ratio_velocity drags pistol vx 0x40->0x38.
        if (other.flags & ObjectFlags::NEWLY_CREATED) continue;
        int8_t tdx = static_cast<int8_t>(player.x.whole - other.x.whole);
        int8_t tdy = static_cast<int8_t>(player.y.whole - other.y.whole);
        if (std::abs(tdx) > 2 || std::abs(tdy) > 2) continue;
        int ox = other.x.whole * 256 + other.x.fraction;
        int oy = other.y.whole * 256 + other.y.fraction;
        int ow_ = (other.sprite <= 0x80)
                  ? (sprite_atlas[other.sprite].w > 0
                     ? (sprite_atlas[other.sprite].w - 1) * 16 : 0) : 0;
        int oh_ = (other.sprite <= 0x80)
                  ? (sprite_atlas[other.sprite].h > 0
                     ? (sprite_atlas[other.sprite].h - 1) * 8 : 0) : 0;
        if (ox + ow_ <= px || px + sprite_w_frac <= ox) continue;
        if (oy + oh_ <= py || py + sprite_h_frac <= oy) continue;
        return i;
    }
    return -1;
}

// Physics / integration half of the player update. Takes the frame's
// acceleration vector produced by apply_player_input and runs the chain:
// wind -> acceleration -> axis-separated integration with solid-tile revert
// -> water effects -> object-object touching -> camera follow. Deliberately
// does not touch inputs or actions — that's apply_player_input's job.
void Game::integrate_player_motion(Object& player,
                                   int8_t accel_x, int8_t accel_y) {
    // Snapshot the pre-motion y so the splash check below knows whether
    // the integrator just dropped the player across the waterline. Mirrors
    // the per-object splash detection in object_update.cpp.
    Fixed8_8 pre_motion_y = player.y;

    // &1aea-&1af5: effective player weight for this frame, raised by any
    // held object. Drives buoyancy (a held big fish sinks the player) and
    // the mass-ratio collisions below.
    const Object* held_obj = ((held_object_slot_ & 0x80) == 0)
        ? &object_mgr_.object(held_object_slot_) : nullptr;
    uint8_t pw = player_effective_weight(player, held_obj);

    // Apply wind (surface only)
    Wind::apply_surface_wind(player);

    // Apply physics
    Physics::apply_acceleration(player, accel_x, accel_y, every_sixteen_frames_);

    // Ground friction via &3235 calculate_seven_eighths:
    //   new_vx = vx - sign(vx) * (|vx| + 7) / 8
    // +7 round guarantees |vx| strictly decreases so tails reach 0.
    if ((player.flags & ObjectFlags::SUPPORTED) && accel_x == 0 &&
        player.velocity_x != 0) {
        int v = player.velocity_x;
        int abs_v = v < 0 ? -v : v;
        int eighth = (abs_v + 7) / 8;
        player.velocity_x = static_cast<int8_t>(v < 0 ? v + eighth : v - eighth);
    }

    // Height in sub-tile units matching the 6502's (rows-1)*8 convention
    // (same as collision.cpp::sprite_height_units and
    // sprites_height_and_vertical_flip_table at &5e89). Used by both the
    // X-motion feet-row probe and the Y-motion ground clamp.
    int sprite_h = (player.sprite <= 0x80)
                   ? sprite_atlas[player.sprite].h : 22;
    int sprite_h_frac = (sprite_h > 0 ? sprite_h - 1 : 0) * 8;
    int sprite_w = (player.sprite <= 0x80)
                   ? sprite_atlas[player.sprite].w : 5;
    int sprite_w_frac = (sprite_w > 0 ? sprite_w - 1 : 0) * 16;

    // Tile collision — &2f8c-&30df via TileCollision::resolve (walks AABB
    // edges -> depth-vector -> push + velocity reflect). Object-object
    // collision is a separate axis-aware pass below (&2a64 + &2bb6).
    Fixed8_8 old_x = player.x;
    Fixed8_8 old_y = player.y;
    player.x.add_velocity(player.velocity_x);
    player.y.add_velocity(player.velocity_y);

    // &2a64 check_for_collisions fires BEFORE tile collision (it jumps
    // to &2ee8 via the &2a61 tail call when done). Static objects skip
    // their own collision pass (&1b50), so the player's stamp is the
    // only path that puts the player into switch.touching / door.touching
    // — and it has to happen on the pre-revert position, or the tile
    // bounce will have pushed the player out of AABB overlap by the
    // time we get here.
    {
        auto obj_coll_pre = Collision::check_object_collision(
            player, 0,
            reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(object_mgr_.object(0)));
        if (obj_coll_pre.collided) {
            player.touching = static_cast<uint8_t>(obj_coll_pre.other_slot);
            Object& other = object_mgr_.object(obj_coll_pre.other_slot);
            other.touching = 0;
        } else {
            player.touching = 0x80;
        }
    }

    int8_t pre_resolve_vx = player.velocity_x;
    int8_t pre_resolve_vy = player.velocity_y;
    Fixed8_8 pre_resolve_x = player.x;
    Fixed8_8 pre_resolve_y = player.y;

    TileCollision::Result tcr = TileCollision::resolve(
        player, old_x.whole, old_x.fraction, old_y.whole, old_y.fraction,
        landscape_, object_mgr_, static_cast<int>(held_object_slot_));
    player.tile_collision = tcr.top_or_bottom_collision;

    bool object_supported = false;

    // Object-object — &2a64 check_for_collisions + &2bb6 mass-ratio
    // transfer. Port deviation: also revert position when blocked by a
    // strictly heavier collider (the velocity ratio alone doesn't pin
    // the lighter side in our port). Held primary excluded.
    {
        auto& all = reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
            object_mgr_.object(0));
        // Find the heavier primary that ACTUALLY overlaps the player. The
        // old code took overlaps_solid_object as a yes/no, then grabbed the
        // first heavier object by slot — usually NOT the overlapping one —
        // so push_out_of_overlap got the wrong blocker, found no overlap,
        // skipped the position separation, and let the velocity transfer
        // run on degenerate geometry (launching the player off it).
        int blocker = Collision::overlapping_solid_slot(
            player, 0, all, static_cast<int>(held_object_slot_));
        // Skip a just-spawned heavier object (e.g. the player's own plasma
        // bullet) for one frame, matching find_lighter_overlap.
        if (blocker >= 0 && (all[blocker].flags & ObjectFlags::NEWLY_CREATED))
            blocker = -1;
        if (blocker >= 0) {
                // &2b80-&2b91: push player position out of overlap along
                // the smallest-overlap axis and kick velocity by ±2 in
                // that direction. Replaces the old "revert to old_x/old_y"
                // approach — that was a no-op when the blocker moved into
                // the player (closing door) since old_x already overlapped.
                Object& other = object_mgr_.object(blocker);
                int push_axis = Collision::push_out_of_overlap(player, other);

                // &2b9e mass-ratio velocity transfer. Pass the HEAVIER
                // object as "this": the &2bee transfer adds the lighter
                // object's share in the direction that *opposes* the
                // closing velocity only when the heavier side is "this".
                // Run with the player as "this" it does the reverse and
                // launches the player (and cannon) along their motion.
                // &2bcd doubles only the smallest-overlap (push-out) axis.
                {
                    auto t = Collision::apply_mass_ratio_velocity(
                        other.velocity_x, player.velocity_x,
                        other.weight(), pw, push_axis == 0);
                    other.velocity_x  = t.this_v;
                    player.velocity_x = t.other_v;
                }
                {
                    auto t = Collision::apply_mass_ratio_velocity(
                        other.velocity_y, player.velocity_y,
                        other.weight(), pw, push_axis == 1);
                    other.velocity_y  = t.this_v;
                    player.velocity_y = t.other_v;
                }
                if (player.velocity_y >= 0) object_supported = true;
            } else {
            // Lighter-overlap push: kick lighter primaries the player
            // walks/falls into. Same as &2bb6's heavier-pushes-lighter
            // half — no position revert here, just a velocity transfer.
            int pushee = find_lighter_overlap(player, object_mgr_,
                                               held_object_slot_, pw,
                                               sprite_w_frac, sprite_h_frac);
            // (verbose plr-push diagnostics removed — flask issue fixed)
            if (pushee >= 0) {
                Object& other = object_mgr_.object(pushee);
                {
                    auto t = Collision::apply_mass_ratio_velocity(
                        player.velocity_x, other.velocity_x,
                        pw, other.weight(), false);
                    player.velocity_x = t.this_v;
                    other.velocity_x  = t.other_v;
                }
                {
                    auto t = Collision::apply_mass_ratio_velocity(
                        player.velocity_y, other.velocity_y,
                        pw, other.weight(), false);
                    player.velocity_y = t.this_v;
                    other.velocity_y  = t.other_v;
                }
            }
        }
    }

    // SUPPORTED — port of &1b86-&1b96: set when (!top && bottom). Port
    // deviation: at our 8 frac/pixel the &308a default -8 frac gap clears
    // SUPPORTED next frame and causes a 1px bounce — latch through "no
    // collision but vy>=0" via a probe so physics's vy-clamp stays valid.
    bool any_bottom_collision = tcr.landed_on_bottom || object_supported;
    bool any_top_collision    = tcr.top_or_bottom_collision && !tcr.landed_on_bottom;
    bool was_supported = (player.flags & ObjectFlags::SUPPORTED) != 0;
    player.flags &= ~ObjectFlags::SUPPORTED;
    if (!any_top_collision && any_bottom_collision) {
        player.flags |= ObjectFlags::SUPPORTED;
    } else if (was_supported && player.velocity_y >= 0 && !any_top_collision) {
        // No fresh bottom collision but were supported last frame and
        // not rising. Probe ~24 frac (3 pixels) below the feet — covers
        // a single bounce gap without extending so far that walking off
        // a ledge keeps SUPPORTED set. Gate strictly on vy >= 0 so the
        // first frames of jetpack thrust (vy = -1, -2, ...) aren't
        // mistaken for a bounce gap and the upward velocity zeroed.
        int feet_abs_y = static_cast<int>(player.y.whole) * 256 +
                         static_cast<int>(player.y.fraction) + sprite_h_frac;
        bool found_support = false;
        for (int probe_dy = 8; probe_dy <= 24 && !found_support; probe_dy += 8) {
            int probe_abs_y = feet_abs_y + probe_dy;
            uint8_t probe_ty = static_cast<uint8_t>((probe_abs_y >> 8) & 0xff);
            uint8_t probe_yf = static_cast<uint8_t>(probe_abs_y & 0xff);
            ResolvedTile pres = resolve_tile_with_tertiary(
                landscape_, player.x.whole, probe_ty);
            uint8_t ptile = Collision::substitute_door_for_obstruction(
                pres.tile_and_flip, pres.data_offset,
                reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                    object_mgr_.object(0)),
                object_mgr_.tertiary_data_byte(pres.data_offset));
            uint8_t ptype = ptile & TileFlip::TYPE_MASK;
            bool pfh = (ptile & TileFlip::HORIZONTAL) != 0;
            bool pfv = (ptile & TileFlip::VERTICAL)   != 0;
            if (Collision::is_tile_type_solid(ptype) &&
                Obstruction::is_obstructed(
                    get_obstruction_pattern_index(ptype, pfh, pfv),
                    player.x.fraction, probe_yf,
                    get_tile_y_offset(ptype, pfv),
                    pfv ^ tile_obstruction_v_flip_bit(ptype))) {
                found_support = true;
            }
        }
        if (found_support) {
            player.flags |= ObjectFlags::SUPPORTED;
            // Kill any residual upward drift — the probe says we're on
            // the floor conceptually, so a stale negative vy from a
            // recent bounce shouldn't keep lifting us frame after frame.
            if (player.velocity_y < 0) player.velocity_y = 0;
        }
    }

    // Landing damping — &37e6-&37f5: when an airborne player (state low
    // nibble >= 0x0a) lands with accel_y == 0, vy passes through
    // calculate_seven_eighths thrice (vy *= ~0.67). Without it the player
    // bounces 6+ times. Uses pre-update counter (6502 order at &38b9).
    {
        uint8_t old_counter = player.state & 0x0f;
        bool jumping_or_flying = (old_counter >= 0x0a);
        if (accel_y == 0 && jumping_or_flying && any_bottom_collision) {
            for (int i = 0; i < 3; i++) {
                int v = player.velocity_y;
                int abs_v = v < 0 ? -v : v;
                int eighth = (abs_v + 7) / 8;
                player.velocity_y = static_cast<int8_t>(
                    v < 0 ? v + eighth : v - eighth);
            }
        }
    }

    // &3a6d update_walking_state. Reset counter to 0 only when player
    // is upright AND any Y-axis collision (tile top/bottom or object
    // bottom) AND slope walkable; otherwise increment (cap 0x0f).
    // Looser "SUPPORTED-only" gate leaked walking across airborne frames.
    bool upright = player_angle_ >= 0xb0 && player_angle_ <= 0xce;
    bool any_y_collision = tcr.top_or_bottom_collision || object_supported;
    bool walkable_slope = !slope_too_steep_for_player(player_tile_collision_angle_);
    uint8_t counter = player.state & 0x0f;
    if (upright && any_y_collision && walkable_slope) {
        counter = 0;
    } else if (counter < 0x0f) {
        counter++;
    }
    player.state = static_cast<uint8_t>((player.state & 0xf0) | counter);

    // Slope angle for the walking branch in apply_player_input.
    // Sample the supporting tile's threshold left and right of the
    // player's centre and feed the delta to angle_from_deltas — the
    // same conversion the 6502 applies at &306c after building an
    // obstruction-depth vector. Skip while airborne so the last
    // grounded value persists into a jump.
    if (tcr.landed_on_bottom) {
        int feet_abs_y = static_cast<int>(player.y.whole) * 256 +
                         static_cast<int>(player.y.fraction) + sprite_h_frac;
        uint8_t feet_ty = static_cast<uint8_t>((feet_abs_y >> 8) & 0xff);
        ResolvedTile sres = resolve_tile_with_tertiary(
            landscape_, player.x.whole, feet_ty);
        uint8_t stile = Collision::substitute_door_for_obstruction(
            sres.tile_and_flip, sres.data_offset,
            reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                object_mgr_.object(0)),
            object_mgr_.tertiary_data_byte(sres.data_offset));
        uint8_t stype = stile & TileFlip::TYPE_MASK;
        bool sfh = (stile & TileFlip::HORIZONTAL) != 0;
        bool sfv = (stile & TileFlip::VERTICAL)   != 0;
        bool scoll_fv = sfv ^ tile_obstruction_v_flip_bit(stype);
        if (!scoll_fv) {
            constexpr int SAMPLE_HALF_DX = 0x10;
            uint8_t left_x  = static_cast<uint8_t>(player.x.fraction - SAMPLE_HALF_DX);
            uint8_t right_x = static_cast<uint8_t>(player.x.fraction + SAMPLE_HALF_DX);
            uint8_t left_t  = tile_threshold_at_x(stype, sfh, sfv, left_x);
            uint8_t right_t = tile_threshold_at_x(stype, sfh, sfv, right_x);
            int dthresh = static_cast<int>(right_t) - static_cast<int>(left_t);
            if (dthresh >  127) dthresh =  127;
            if (dthresh < -128) dthresh = -128;
            player_tile_collision_angle_ = NPC::angle_from_deltas(
                static_cast<int8_t>(SAMPLE_HALF_DX * 2),
                static_cast<int8_t>(dthresh));
        } else {
            player_tile_collision_angle_ = 0;
        }
    }

    // Port of &3fea-&4002 update_mushroom_tile collision branch: pick
    // red/blue from flip_v, add to player_mushroom_timers[], emit one
    // PARTICLE_STAR_OR_MUSHROOM. Probe every tile row the AABB overlaps —
    // red mushrooms sit on the feet tile, head-only would miss them.
    {
        int head_abs = static_cast<int>(player.y.whole) * 256 +
                       static_cast<int>(player.y.fraction);
        int feet_abs = head_abs + sprite_h_frac;
        uint8_t head_tile_y = static_cast<uint8_t>((head_abs >> 8) & 0xff);
        uint8_t feet_tile_y = static_cast<uint8_t>((feet_abs >> 8) & 0xff);
        for (int ty = head_tile_y;
             ty != static_cast<uint8_t>(feet_tile_y + 1);
             ty = static_cast<uint8_t>(ty + 1)) {
            uint8_t tile = landscape_.get_tile(player.x.whole,
                                               static_cast<uint8_t>(ty));
            uint8_t type = tile & TileFlip::TYPE_MASK;
            if (type != static_cast<uint8_t>(TileType::MUSHROOMS)) continue;
            bool is_blue = (tile & TileFlip::VERTICAL) != 0;
            int which    = is_blue ? 1 : 0;
            // &4005 add_to_player_mushroom_timer: +0x3f per frame of
            // contact, capped at 0xff. Immunity-pill / immobility-timer
            // gates (&400f-&4019) aren't tracked in the port yet.
            int sum = static_cast<int>(player_mushroom_timers_[which]) + 0x3f;
            if (sum > 0xff) sum = 0xff;
            player_mushroom_timers_[which] = static_cast<uint8_t>(sum);
            // &4000-&4002 emit STAR_OR_MUSHROOM via &3f7f. The 6502
            // pre-shifts the base by -0x40 in both x and y so the
            // (y_rand=0xff) jitter spreads around the head/above-head
            // rows instead of head/below — without it the spray sits one
            // tile too low.
            Object src = player;
            int yf = int(src.y.fraction) - 0x40;
            if (yf < 0) { yf += 256; src.y.whole--; }
            src.y.fraction = static_cast<uint8_t>(yf);
            int xf = int(src.x.fraction) - 0x40;
            if (xf < 0) { xf += 256; src.x.whole--; }
            src.x.fraction = static_cast<uint8_t>(xf);
            particles_.emit(ParticleType::STAR_OR_MUSHROOM, 1,
                            src, cosmetic_rng_);
            // &3ff9-&3ffc: mushroom contact sound — soft poof on top of
            // the spore puff.
            static constexpr uint8_t kSoundMushroomPoof[4] = { 0x33, 0xf3, 0x1d, 0x03 };
            Audio::play(Audio::CH_ANY, kSoundMushroomPoof);
            break;
        }
    }

    // &3ef2 update_invisible_switch_tile — collision branch. Fires
    // INVISIBLE_SWITCH effects for each tile row the AABB overlaps; door
    // pairs (e.g. entry &89) only get their OPENING bit set this way.
    {
        int head_abs = static_cast<int>(player.y.whole) * 256 +
                       static_cast<int>(player.y.fraction);
        int feet_abs = head_abs + sprite_h_frac;
        uint8_t head_tile_y = static_cast<uint8_t>((head_abs >> 8) & 0xff);
        uint8_t feet_tile_y = static_cast<uint8_t>((feet_abs >> 8) & 0xff);
        for (int ty = head_tile_y;
             ty != static_cast<uint8_t>(feet_tile_y + 1);
             ty = static_cast<uint8_t>(ty + 1)) {
            Behaviors::trigger_invisible_switch_at(
                player, player.x.whole, static_cast<uint8_t>(ty),
                object_mgr_, landscape_);
        }
    }

    // Water splash — port of &2f69-&2f82 add_water_particles_for_splash.
    // If the player just crossed the waterline this frame moving downward,
    // emit one PARTICLE_WATER at the crossing point with angle &c0 (head
    // up). The per-object loop does the same for primaries; the player
    // takes its own integration path so we mirror the check here.
    {
        uint8_t wy = Water::get_waterline_y(player.x.whole);
        bool was_above = pre_motion_y.whole < wy;
        bool now_at    = player.y.whole >= wy;
        if (was_above && now_at && player.velocity_y > 0) {
            particles_.emit_directed(ParticleType::WATER, 0xc0, player, cosmetic_rng_);
        }
    }

    // Apply water effects. Return value is the 6502's &2f69
    // finished_applying_buoyancy emit-particle decision: true when the
    // object is partially submerged AND moving downward / stationary,
    // i.e. swimming at the surface. Emit one PARTICLE_WATER upward.
    if (Water::apply_water_effects(landscape_, player, pw,
                                    every_four_frames_)) {
        particles_.emit_directed(ParticleType::WATER, 0xc0, player,
                                 cosmetic_rng_);
    }

    // Tile-based wind / water-current — same dispatch as the per-object
    // loop. Without this the player feels surface wind but not the local
    // gusts inside windy caverns or the river current in Triax's lab.
    Wind::apply_tile_environment(player, landscape_, object_mgr_,
                                 frame_counter_, rng_, cosmetic_rng_,
                                 particles_);

    // Object-object touching was stamped before tile collision above —
    // matches the 6502's &2a64 → &2ee8 ordering. Don't re-stamp here, or
    // a tile bounce that pushed the player out of overlap would wipe
    // the touching field that update_switch / update_door needs.

    // Update camera
    camera_.follow_player(player.x.whole, player.y.whole);
}
