#include "game/game.h"
#include "objects/physics.h"
#include "objects/collision.h"
#include "rendering/sprite_atlas.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
#include "world/wind.h"
#include "world/water.h"
#include <array>

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

    // Port of &2fb8 check_for_tile_collisions_on_top_and_bottom_edges_tile_loop
    // section sweep: probe every 32-fraction x-section across the player's
    // AABB width, not just one point. For partial-solid tiles like
    // STONE_SLOPE_78 (door substitute, solid only in left quarter), a
    // single probe at player.x.fraction misses the surface when the
    // player's left edge is past the solid band; probing the whole width
    // catches any section the AABB overlaps.
    //
    // Ground-like (coll_fv=0) obstruction is "y_frac > threshold", so the
    // LOWEST section threshold produces the most grounded outcome.
    // Ceiling-like (coll_fv=1) is "y_frac <= threshold", so the HIGHEST.
    auto effective_threshold = [&](uint8_t ttype, bool fh, bool fv,
                                    uint8_t x_start, bool ceiling_like)->uint8_t {
        int end = static_cast<int>(x_start) + sprite_w_frac;
        uint8_t best = tile_threshold_at_x(ttype, fh, fv, x_start);
        // Step by 0x20 (one section). Include the right edge explicitly.
        for (int px = (static_cast<int>(x_start) | 0x1f) + 1; px <= end; px += 0x20) {
            uint8_t t = tile_threshold_at_x(ttype, fh, fv,
                                              static_cast<uint8_t>(px & 0xff));
            if (ceiling_like) { if (t > best) best = t; }
            else              { if (t < best) best = t; }
        }
        return best;
    };

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

        ResolvedTile cres = resolve_tile_with_tertiary(landscape_,
                                                       player.x.whole,
                                                       player.y.whole);
        uint8_t ctile = Collision::substitute_door_for_obstruction(
            cres.tile_and_flip, cres.data_offset,
            reinterpret_cast<const std::array<Object, 16>&>(
                object_mgr_.object(0)),
            object_mgr_.tertiary_data_byte(cres.data_offset));
        uint8_t ctype = ctile & TileFlip::TYPE_MASK;
        bool cfh = (ctile & TileFlip::HORIZONTAL) != 0;
        bool cfv = (ctile & TileFlip::VERTICAL) != 0;
        bool blocked = false;
        if (Collision::is_tile_type_solid(ctype)) {
            bool ccoll_fv = cfv ^ tile_obstruction_v_flip_bit(ctype);
            uint8_t cthresh = effective_threshold(
                ctype, cfh, cfv, player.x.fraction, ccoll_fv);
            blocked = ccoll_fv
                ? (player.y.fraction <= cthresh)
                : (player.y.fraction >  cthresh);
        }
        // Object AABB backstop — port of &2a64 check_for_collisions +
        // &2bb6 apply_collision_to_objects_velocities. Tile obstruction
        // alone (STONE_SLOPE_78 pattern) only covers the left quarter of
        // a door tile; the pixel-precise AABB catches the rest of the
        // door sprite where the pattern says "empty" but the player is
        // physically overlapping the door primary.
        if (!blocked && Collision::overlaps_solid_object(
                player, 0,
                reinterpret_cast<const std::array<Object, 16>&>(
                    object_mgr_.object(0)))) {
            blocked = true;
        }
        if (blocked) {
            player.x = old_x;
            player.velocity_x = 0;
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

        // If the sprite's top ends up inside the obstruction region of
        // its current tile, that's an "overshoot" — rewind. The
        // obstruction sits above or below the threshold depending on the
        // effective collision flip: landscape flip_v XOR the &04ab bit.
        // Door tiles swap to STONE_SLOPE_78 / SPACE before this check so
        // a closed door blocks and an open one doesn't, matching the
        // 6502's door_tiles_table substitution at obstruction time.
        ResolvedTile yres = resolve_tile_with_tertiary(landscape_,
                                                       player.x.whole,
                                                       player.y.whole);
        uint8_t ctile = Collision::substitute_door_for_obstruction(
            yres.tile_and_flip, yres.data_offset,
            reinterpret_cast<const std::array<Object, 16>&>(
                object_mgr_.object(0)),
            object_mgr_.tertiary_data_byte(yres.data_offset));
        uint8_t ctype = ctile & TileFlip::TYPE_MASK;
        bool cfh = (ctile & TileFlip::HORIZONTAL) != 0;
        bool cfv = (ctile & TileFlip::VERTICAL) != 0;
        bool y_blocked = false;
        if (Collision::is_tile_type_solid(ctype)) {
            bool ccoll_fv = cfv ^ tile_obstruction_v_flip_bit(ctype);
            uint8_t cthresh = effective_threshold(
                ctype, cfh, cfv, player.x.fraction, ccoll_fv);
            y_blocked = ccoll_fv
                ? (player.y.fraction <= cthresh)   // ceiling-like: top solid
                : (player.y.fraction >  cthresh);  // ground-like: bottom solid
        }
        // Object-AABB backstop — catches the rest of the door sprite where
        // the STONE_SLOPE_78 pattern says "empty". Same mechanism as the
        // X-axis revert above.
        if (!y_blocked && Collision::overlaps_solid_object(
                player, 0,
                reinterpret_cast<const std::array<Object, 16>&>(
                    object_mgr_.object(0)))) {
            y_blocked = true;
        }
        bool object_supported = false;
        if (y_blocked) {
            player.y = old_y;
            if (player.velocity_y > 0) {
                player.flags |= ObjectFlags::SUPPORTED;
                // When the Y revert was due to AABB overlap with a
                // weight-7 static (and not tile obstruction), the
                // tile-based grounded check below will read SPACE under
                // the player and clear SUPPORTED. Preserve it so the
                // flag survives to the friction / animation code.
                object_supported = true;
            }
            player.velocity_y = 0;
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
            reinterpret_cast<const std::array<Object, 16>&>(
                object_mgr_.object(0)),
            object_mgr_.tertiary_data_byte(fres.data_offset));
        uint8_t ftype = ftile & TileFlip::TYPE_MASK;
        bool ffh = (ftile & TileFlip::HORIZONTAL) != 0;
        bool ffv = (ftile & TileFlip::VERTICAL) != 0;

        bool grounded = false;
        if (Collision::is_tile_type_solid(ftype)) {
            bool fcoll_fv = ffv ^ tile_obstruction_v_flip_bit(ftype);
            uint8_t fthresh = effective_threshold(
                ftype, ffh, ffv, player.x.fraction, fcoll_fv);
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

    // Apply water effects
    Water::apply_water_effects(player, player.weight());

    // Object-object collision for player
    auto obj_coll = Collision::check_object_collision(
        player, 0,
        reinterpret_cast<const std::array<Object, 16>&>(object_mgr_.object(0)));
    if (obj_coll.collided) {
        player.touching = static_cast<uint8_t>(obj_coll.other_slot);
    } else {
        player.touching = 0x80;
    }

    // Update camera
    camera_.follow_player(player.x.whole, player.y.whole);
}
