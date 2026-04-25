#include "game/game.h"
#include "objects/physics.h"
#include "objects/collision.h"
#include "objects/object_manager.h"
#include "rendering/sprite_atlas.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
#include "world/wind.h"
#include "world/water.h"
#include <array>

// Single-tile section sweep: returns the most-grounded (or most-ceiling'd)
// threshold across the AABB width WITHIN one already-resolved tile. Used
// by the feet-row grounding check at the bottom of integrate_player_motion,
// where we need a specific landing Y to snap to, not just a yes/no block.
// For partial-solid tiles like STONE_SLOPE_78 (door substitute, solid only
// in the left quarter) a single probe at player.x.fraction misses the
// surface when the player's left edge is past the solid band; this helper
// picks the LOWEST threshold (most grounded) for ground-like tiles or the
// HIGHEST (most ceiling'd) for v-flipped tiles. Port of &2fb8's per-
// section loop, constrained to one tile's width.
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

// Horizontal-motion blocking with per-row old/new diff. Walks every
// 0x20-fraction row down the sprite height and asks "is this row
// obstructed at the new column but NOT at the old column?" — i.e. did
// the move introduce a NEW wall overlap at this row.
//
// Pre-existing obstructions (head already in a ceiling, feet already
// touching floor, sprite straddling a section that's always solid) are
// skipped because the move didn't create them and blocking on them
// freezes the player forever. This is the key to the 6502-like
// behaviour: left/right motion is gated solely by walls that would
// actually be *encountered* by the edge, not by vertical overlaps that
// top/bottom obstructions should handle separately.
//
// Port analogue: the 6502 computes obstruction DEPTHS per-edge (&2e8a)
// and resolves them via a direction vector at &306c. Our diff-based
// check captures the same "block only on new obstruction in this axis"
// spirit without the full angle-vector math.
static bool column_move_blocked(
    const Landscape& landscape, ObjectManager& mgr,
    uint8_t old_tx, uint8_t old_xf,
    uint8_t new_tx, uint8_t new_xf,
    uint8_t y_whole, uint8_t y_frac, int sprite_h_frac)
{
    int top_abs = static_cast<int>(y_whole) * 256 + static_cast<int>(y_frac);
    int bot_abs = top_abs + sprite_h_frac;
    for (int sy = top_abs; sy <= bot_abs; sy += 0x20) {
        int clamp_sy   = sy > bot_abs ? bot_abs : sy;
        uint8_t ty     = static_cast<uint8_t>((clamp_sy >> 8) & 0xff);
        uint8_t ty_frac = static_cast<uint8_t>(clamp_sy & 0xff);
        bool old_solid = probe_point_with_door_subst(
            landscape, mgr, old_tx, ty, old_xf, ty_frac);
        bool new_solid = probe_point_with_door_subst(
            landscape, mgr, new_tx, ty, new_xf, ty_frac);
        if (new_solid && !old_solid) return true;
    }
    // Catch the very bottom row if the step skipped past it.
    uint8_t ty     = static_cast<uint8_t>((bot_abs >> 8) & 0xff);
    uint8_t ty_frac = static_cast<uint8_t>(bot_abs & 0xff);
    bool old_solid = probe_point_with_door_subst(
        landscape, mgr, old_tx, ty, old_xf, ty_frac);
    bool new_solid = probe_point_with_door_subst(
        landscape, mgr, new_tx, ty, new_xf, ty_frac);
    return new_solid && !old_solid;
}

// Port of &2fb8 check_for_tile_collisions_on_top_and_bottom_edges_tile_loop.
//
// Walks every 32-fraction x-section the player's AABB overlaps, crossing
// to the neighbouring tile (via &2fed INC tile_x / &2fef
// set_obstruction_data_variables_for_bottom_tile) when a section rolls
// past x_frac 0x08. For each section it asks "is the point (section_x,
// player_y_frac) inside the resolved tile's obstruction pattern?" and
// returns true on the first hit.
//
// Used to gate VERTICAL motion. This is the 6502's top_obstruction /
// bottom_obstruction analogue: the caller passes the y_frac of whichever
// edge it cares about — player.y.fraction for upward motion (head-row
// probe → catches ceilings), feet_y_frac for downward motion (feet-row
// probe → catches floors).
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

// Physics / integration half of the player update. Takes the frame's
// acceleration vector produced by apply_player_input and runs the chain:
// wind → acceleration → axis-separated integration with solid-tile revert
// → water effects → object-object touching → camera follow. Deliberately
// does not touch inputs or actions — that's apply_player_input's job.
void Game::integrate_player_motion(Object& player,
                                   int8_t accel_x, int8_t accel_y) {
    // Apply wind (surface only)
    Wind::apply_surface_wind(player);

    // Apply physics
    Physics::apply_acceleration(player, accel_x, accel_y, every_sixteen_frames_);

    // Ground friction — while supported with no horizontal input, damp
    // velocity_x toward 0 via the 6502's calculate_seven_eighths
    // (&3235): new_vx = vx - sign(vx) * (|vx| + 7) / 8. The +7 round
    // guarantees |vx| strictly decreases, so small tails fall to 0
    // instead of lingering. Emulates the effect of the walking code
    // pulling velocity toward the input target (which is 0 here).
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
    int sprite_h = (player.sprite <= 0x7c)
                   ? sprite_atlas[player.sprite].h : 22;
    int sprite_h_frac = (sprite_h > 0 ? sprite_h - 1 : 0) * 8;
    int sprite_w = (player.sprite <= 0x7c)
                   ? sprite_atlas[player.sprite].w : 5;
    int sprite_w_frac = (sprite_w > 0 ? sprite_w - 1 : 0) * 16;

    // AABB-spanning obstruction probe lives in player_aabb_obstructed at
    // TU scope above (per CLAUDE.md: no lambdas). Walks sections across
    // tile boundaries so moving right into a wall is caught, and false
    // blocks on left-edge tiles that spill into the next column are
    // avoided.

    // X movement — obstruction-aware. Resolve the tile at the player's
    // head row; METAL_DOOR / STONE_DOOR tiles get swapped for
    // STONE_SLOPE_78 (closed) or SPACE (open) via
    // substitute_door_for_obstruction (port of &3ebd-&3ec2
    // door_tiles_table). Faithful to the 6502: blocking is tile-based,
    // no object-AABB blocking, and we probe only the head-row tile —
    // the 6502's level layout relies on solid obstacles like doors
    // spanning the row the player's sprite occupies (stacked door tiles
    // here, or vertical doors elsewhere), so the head probe catches
    // them. A ground-like substitute (STONE_SLOPE_78, thresh=0) is
    // non-obstructing at y_frac=0 on purpose: that's the "surface" the
    // player stands on when jumping onto a door from above.
    {
        Fixed8_8 old_x = player.x;
        player.x.add_velocity(player.velocity_x);

        // Port of the 6502's left/right_obstruction model (&2fa4 / &3033
        // check_for_top_and_bottom_tile_collisions with X = leading section):
        // blocking horizontal motion depends on whether the LEADING
        // vertical edge of the AABB — the right column when moving right,
        // the left column when moving left — crosses into solid geometry
        // across the sprite's height. Static probes (velocity_x == 0)
        // don't block; walking away from the wall shouldn't either.
        //
        // This replaces the previous head-row-only probe, which
        // conflated "head is in a ceiling" with "left/right edge hit a
        // wall" and made any head-in-ceiling state freeze sideways
        // motion. The 6502 avoids that by keeping top/bottom and
        // left/right obstructions on independent axes.
        bool blocked = false;
        if (player.velocity_x != 0) {
            // Leading vertical edge at OLD and NEW positions: right edge
            // when moving right (+ sprite_w_frac), left edge otherwise.
            int old_lead = static_cast<int>(old_x.whole) * 256 +
                           static_cast<int>(old_x.fraction) +
                           (player.velocity_x > 0 ? sprite_w_frac : 0);
            int new_lead = static_cast<int>(player.x.whole) * 256 +
                           static_cast<int>(player.x.fraction) +
                           (player.velocity_x > 0 ? sprite_w_frac : 0);
            uint8_t old_tx = static_cast<uint8_t>((old_lead >> 8) & 0xff);
            uint8_t old_xf = static_cast<uint8_t>(old_lead & 0xff);
            uint8_t new_tx = static_cast<uint8_t>((new_lead >> 8) & 0xff);
            uint8_t new_xf = static_cast<uint8_t>(new_lead & 0xff);
            blocked = column_move_blocked(
                landscape_, object_mgr_,
                old_tx, old_xf, new_tx, new_xf,
                player.y.whole, player.y.fraction, sprite_h_frac);
        }
        // Object AABB backstop — port of &2a64 check_for_collisions +
        // &2bb6 apply_collision_to_objects_velocities. Tile obstruction
        // alone (STONE_SLOPE_78 pattern) only covers the left quarter of
        // a door tile; the pixel-precise AABB catches the rest of the
        // door sprite where the pattern says "empty" but the player is
        // physically overlapping the door primary.
        //
        // On an object-AABB block we also apply the 6502's mass-ratio
        // velocity transfer (calculate_transfer_velocities at &2bee)
        // instead of simply zeroing velocity. Hitting a heavier object
        // bounces the player back; hitting an equal-weight object just
        // halts; hitting a lighter one would push it but we already
        // short-circuit above since a lighter collider doesn't trigger
        // overlaps_solid_object's block.
        int obj_blocker = -1;
        if (!blocked) {
            auto& all = reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                object_mgr_.object(0));
            if (Collision::overlaps_solid_object(player, 0, all)) {
                // Find the specific blocker for the transfer math.
                for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
                    const Object& other = all[i];
                    if (!other.is_active()) continue;
                    uint8_t ow = other.weight();
                    if (ow <= player.weight()) continue;
                    // Cheap AABB check mirroring overlaps_solid_object.
                    int px = player.x.whole * 256 + player.x.fraction;
                    int py = player.y.whole * 256 + player.y.fraction;
                    int ox = other.x.whole * 256 + other.x.fraction;
                    int oy = other.y.whole * 256 + other.y.fraction;
                    int pw = (player.sprite <= 0x7c)
                        ? (sprite_atlas[player.sprite].w > 0
                            ? (sprite_atlas[player.sprite].w - 1) * 16 : 0) : 0;
                    int ph = sprite_h_frac;
                    int ow_ = (other.sprite <= 0x7c)
                        ? (sprite_atlas[other.sprite].w > 0
                            ? (sprite_atlas[other.sprite].w - 1) * 16 : 0) : 0;
                    int oh_ = (other.sprite <= 0x7c)
                        ? (sprite_atlas[other.sprite].h > 0
                            ? (sprite_atlas[other.sprite].h - 1) * 8 : 0) : 0;
                    if (ox + ow_ > px && px + pw > ox &&
                        oy + oh_ > py && py + ph > oy) {
                        obj_blocker = i;
                        break;
                    }
                }
                blocked = true;
            }
        }
        if (blocked) {
            player.x = old_x;
            if (obj_blocker >= 0) {
                Object& other = object_mgr_.object(obj_blocker);
                bool hit_from_right = player.velocity_x > 0;
                auto t = Collision::apply_mass_ratio_velocity(
                    player.velocity_x, other.velocity_x,
                    player.weight(), other.weight(),
                    hit_from_right);
                player.velocity_x = t.this_v;
                other.velocity_x  = t.other_v;
            } else {
                player.velocity_x = 0;    // tile block — hard stop
            }
        }
    }

    // Y movement — obstruction-aware. Ground surface within a tile is at
    // tiles_obstruction_y_offsets[type]'s upper nibble * 16, rounded up
    // with ORA #&0f (&245f-&246f in the disassembly). For EARTH, STONE,
    // etc., that surface is partway down the tile, so the player must be
    // able to enter the tile as long as his sprite-top stays above the
    // obstruction line.
    {
        Fixed8_8 old_y = player.y;
        player.y.add_velocity(player.velocity_y);

        // Port of the 6502's top/bottom_obstruction model (&2fb8-&300d).
        // Blocking vertical motion depends on whether the LEADING
        // horizontal edge of the AABB — the bottom row when moving down,
        // the top row when moving up — crosses into solid geometry
        // across the sprite's width. Static probes (velocity_y == 0)
        // don't block.
        //
        // Door tiles are substituted to STONE_SLOPE_78 (closed) / SPACE
        // (open) inside player_aabb_obstructed's tile-by-tile walk so
        // closed doors block and open doors don't, matching the 6502's
        // door_tiles_table swap at obstruction time.
        bool y_blocked = false;
        if (player.velocity_y != 0) {
            int lead_abs = static_cast<int>(player.y.whole) * 256 +
                           static_cast<int>(player.y.fraction) +
                           (player.velocity_y > 0 ? sprite_h_frac : 0);
            uint8_t lead_ty = static_cast<uint8_t>((lead_abs >> 8) & 0xff);
            uint8_t lead_yf = static_cast<uint8_t>(lead_abs & 0xff);
            y_blocked = player_aabb_obstructed(
                landscape_, object_mgr_,
                lead_ty, player.x.whole, player.x.fraction,
                sprite_w_frac, lead_yf);
        }
        // Object-AABB backstop — same mechanism as the X-axis revert
        // above, with the same mass-ratio velocity transfer on block.
        int y_obj_blocker = -1;
        if (!y_blocked) {
            auto& all = reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                object_mgr_.object(0));
            if (Collision::overlaps_solid_object(player, 0, all)) {
                for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
                    const Object& other = all[i];
                    if (!other.is_active()) continue;
                    uint8_t ow = other.weight();
                    if (ow <= player.weight()) continue;
                    int px = player.x.whole * 256 + player.x.fraction;
                    int py = player.y.whole * 256 + player.y.fraction;
                    int ox = other.x.whole * 256 + other.x.fraction;
                    int oy = other.y.whole * 256 + other.y.fraction;
                    int pw = (player.sprite <= 0x7c)
                        ? (sprite_atlas[player.sprite].w > 0
                            ? (sprite_atlas[player.sprite].w - 1) * 16 : 0) : 0;
                    int ph = sprite_h_frac;
                    int ow_ = (other.sprite <= 0x7c)
                        ? (sprite_atlas[other.sprite].w > 0
                            ? (sprite_atlas[other.sprite].w - 1) * 16 : 0) : 0;
                    int oh_ = (other.sprite <= 0x7c)
                        ? (sprite_atlas[other.sprite].h > 0
                            ? (sprite_atlas[other.sprite].h - 1) * 8 : 0) : 0;
                    if (ox + ow_ > px && px + pw > ox &&
                        oy + oh_ > py && py + ph > oy) {
                        y_obj_blocker = i;
                        break;
                    }
                }
                y_blocked = true;
            }
        }
        bool object_supported = false;
        if (y_blocked) {
            player.y = old_y;
            if (player.velocity_y > 0) {
                player.flags |= ObjectFlags::SUPPORTED;
                // When the Y revert was due to AABB overlap with a
                // heavier static, the tile-based grounded check below
                // will read SPACE under the player and clear SUPPORTED.
                // Preserve it so the flag survives to the friction /
                // animation code.
                object_supported = true;
            }
            if (y_obj_blocker >= 0) {
                Object& other = object_mgr_.object(y_obj_blocker);
                bool hit_from_below = player.velocity_y < 0; // moving up into other
                auto t = Collision::apply_mass_ratio_velocity(
                    player.velocity_y, other.velocity_y,
                    player.weight(), other.weight(),
                    hit_from_below);
                player.velocity_y = t.this_v;
                other.velocity_y  = t.other_v;
            } else {
                player.velocity_y = 0;
            }
        }

        // Ground clamp + support check. The player only stands on tiles
        // whose effective collision flip is 0 (ground at the bottom of the
        // cell). Tiles with collision flip 1 are ceilings/overhangs — they
        // block upward motion via the top-obstruction revert above but
        // don't support the player from below.
        //
        // Door-tile substitution applies here too: a closed door below
        // the feet reads as STONE_SLOPE_78 so the player stands on it,
        // while an open door reads as SPACE so the player falls through
        // — faithful to the 6502, which has no object-AABB support and
        // drives everything off tile obstruction.
        int feet_abs = static_cast<int>(player.y.whole) * 256 +
                       static_cast<int>(player.y.fraction) + sprite_h_frac;
        uint8_t feet_tile_y = static_cast<uint8_t>((feet_abs >> 8) & 0xff);
        uint8_t feet_frac   = static_cast<uint8_t>(feet_abs & 0xff);
        ResolvedTile fres = resolve_tile_with_tertiary(landscape_,
                                                       player.x.whole,
                                                       feet_tile_y);
        uint8_t ftile = Collision::substitute_door_for_obstruction(
            fres.tile_and_flip, fres.data_offset,
            reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                object_mgr_.object(0)),
            object_mgr_.tertiary_data_byte(fres.data_offset));
        uint8_t ftype = ftile & TileFlip::TYPE_MASK;
        bool ffh = (ftile & TileFlip::HORIZONTAL) != 0;
        bool ffv = (ftile & TileFlip::VERTICAL) != 0;

        bool grounded = false;
        if (Collision::is_tile_type_solid(ftype)) {
            bool fcoll_fv = ffv ^ tile_obstruction_v_flip_bit(ftype);
            uint8_t fthresh = single_tile_effective_threshold(
                ftype, ffh, ffv, player.x.fraction,
                sprite_w_frac, fcoll_fv);
            bool feet_in_obstr = fcoll_fv
                ? (feet_frac <= fthresh)
                : (feet_frac >= fthresh);
            if (feet_in_obstr) {
                grounded = true;
                uint8_t land_y = fcoll_fv ? 0 : fthresh;
                if (player.velocity_y >= 0) {
                    int target_top = static_cast<int>(feet_tile_y) * 256 +
                                     static_cast<int>(land_y) - sprite_h_frac;
                    player.y.whole    = static_cast<uint8_t>((target_top >> 8) & 0xff);
                    player.y.fraction = static_cast<uint8_t>(target_top & 0xff);
                    player.velocity_y = 0;
                }
            }
        }
        if (grounded || object_supported) player.flags |=  ObjectFlags::SUPPORTED;
        else                              player.flags &= ~ObjectFlags::SUPPORTED;
    }

    // Port of &3fea-&4002 update_mushroom_tile's collision branch. When
    // the player overlaps a MUSHROOMS tile, the 6502:
    //   * picks red or blue based on tile flip_v (&40 bit at &09),
    //   * adds to player_mushroom_timers[red=0 / blue=1] (&4005),
    //   * emits one PARTICLE_STAR_OR_MUSHROOM (&4000-&4002).
    //
    // The "event" branch that spawns mushroom ball primaries (&3fde-&3fe9)
    // is gated on TILE_PROCESSING_FLAG_EVENTS — a separate code path our
    // port hasn't wired; hooking it up is the update_events job.
    //
    // Probe every tile row the player's AABB overlaps, not just the head
    // tile: red mushrooms sit on the floor (the player's feet tile, which
    // is player.y.whole + 1 when the player is standing) and blue
    // mushrooms on the ceiling (player.y.whole when jumping up). Checking
    // only player.y.whole would silently miss red mushrooms entirely — the
    // head tile is the tile ABOVE the floor.
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
            // &4000-&4002 emit one STAR_OR_MUSHROOM particle. The 6502
            // emits from `this_object` (the player), which combined with
            // our use_vcentre flag (0x08 in the STAR_OR_MUSHROOM table row)
            // puts the particle at the player's vertical centre — a tile
            // above a floor mushroom, a tile below a ceiling mushroom.
            // Emit from the mushroom tile instead so the spores visibly
            // puff out where the mushrooms actually are. Use emit_at to
            // skip the object-centred base offset entirely.
            particles_.emit_at(ParticleType::STAR_OR_MUSHROOM,
                               player.x.whole, static_cast<uint8_t>(ty),
                               rng_);
            break;
        }
    }

    // Apply water effects
    Water::apply_water_effects(landscape_, player, player.weight());

    // Object-object collision for player
    auto obj_coll = Collision::check_object_collision(
        player, 0,
        reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(object_mgr_.object(0)));
    if (obj_coll.collided) {
        player.touching = static_cast<uint8_t>(obj_coll.other_slot);
    } else {
        player.touching = 0x80;
    }

    // Update camera
    camera_.follow_player(player.x.whole, player.y.whole);
}
