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

// AABB scan for a lighter active primary overlapping the player. Used
// by both axes' &2bb6 heavier-hits-lighter branch. Returns the slot, or
// -1 if none. Skips held<->player (&2afd-&2b0e), intangible (&0354 bit
// 7), and zero-weight types.
static int find_lighter_overlap(const Object& player, ObjectManager& mgr,
                                uint8_t held_slot,
                                int sprite_w_frac, int sprite_h_frac) {
    int px = player.x.whole * 256 + player.x.fraction;
    int py = player.y.whole * 256 + player.y.fraction;
    uint8_t pw_weight = player.weight();
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
// wind → acceleration → axis-separated integration with solid-tile revert
// → water effects → object-object touching → camera follow. Deliberately
// does not touch inputs or actions — that's apply_player_input's job.
void Game::integrate_player_motion(Object& player,
                                   int8_t accel_x, int8_t accel_y) {
    // Snapshot the pre-motion y so the splash check below knows whether
    // the integrator just dropped the player across the waterline. Mirrors
    // the per-object splash detection in object_update.cpp.
    Fixed8_8 pre_motion_y = player.y;

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
    // edges → depth-vector → push + velocity reflect). Object-object
    // collision is a separate axis-aware pass below (&2a64 + &2bb6).
    Fixed8_8 old_x = player.x;
    Fixed8_8 old_y = player.y;
    player.x.add_velocity(player.velocity_x);
    player.y.add_velocity(player.velocity_y);

    int8_t pre_resolve_vx = player.velocity_x;
    int8_t pre_resolve_vy = player.velocity_y;
    Fixed8_8 pre_resolve_x = player.x;
    Fixed8_8 pre_resolve_y = player.y;

    TileCollision::Result tcr = TileCollision::resolve(
        player, old_x.whole, old_x.fraction, old_y.whole, old_y.fraction,
        landscape_, object_mgr_, static_cast<int>(held_object_slot_));
    player.tile_collision = tcr.top_or_bottom_collision;

    // Per-frame player-physics trace. Lines look like:
    //   plr 1234 pos=(99,3b,80,a4) v=(+18,+01) sup=1 land=1 col=1
    //   resolve: pre=(99,3b,80,a3) v=(+18,+01) post=(99,3b,80,a4) v=(+18,+00)
    // Toggle off by commenting the block out — the file rolls each launch.
    if (debug_log_.is_open()) {
        bool was_supp = (player.flags & ObjectFlags::SUPPORTED) != 0;
        char line[200];
        std::snprintf(line, sizeof(line),
            "plr %u pos=(%02x,%02x,%02x,%02x) v=(%+d,%+d) sup=%d land=%d "
            "col=%d sur=%d  pre=(%02x,%02x,%02x,%02x) prev=(%+d,%+d) old=(%02x,%02x,%02x,%02x)\n",
            static_cast<unsigned>(frame_counter_),
            player.x.whole, player.x.fraction, player.y.whole, player.y.fraction,
            static_cast<int>(player.velocity_x), static_cast<int>(player.velocity_y),
            was_supp ? 1 : 0, tcr.landed_on_bottom ? 1 : 0,
            tcr.collided ? 1 : 0, tcr.surrounded ? 1 : 0,
            pre_resolve_x.whole, pre_resolve_x.fraction,
            pre_resolve_y.whole, pre_resolve_y.fraction,
            static_cast<int>(pre_resolve_vx), static_cast<int>(pre_resolve_vy),
            old_x.whole, old_x.fraction, old_y.whole, old_y.fraction);
        debug_log_ << line;
        debug_log_.flush();
    }

    bool object_supported = false;

    // Object-object — &2a64 check_for_collisions + &2bb6 mass-ratio
    // transfer. Port deviation: also revert position when blocked by a
    // strictly heavier collider (the velocity ratio alone doesn't pin
    // the lighter side in our port). Held primary excluded.
    {
        auto& all = reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
            object_mgr_.object(0));
        if (Collision::overlaps_solid_object(player, 0, all,
                                             static_cast<int>(held_object_slot_))) {
            int blocker = -1;
            for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
                if (i == static_cast<int>(held_object_slot_)) continue;
                const Object& other = all[i];
                if (!other.is_active()) continue;
                if (other.weight() <= player.weight()) continue;
                blocker = i;
                break;
            }
            if (blocker >= 0) {
                // Revert position back to old; transfer velocity via
                // mass ratio. Both axes revert because the 6502's
                // mass-transfer is unified, not per-axis.
                player.x = old_x;
                player.y = old_y;
                Object& other = object_mgr_.object(blocker);
                {
                    auto t = Collision::apply_mass_ratio_velocity(
                        player.velocity_x, other.velocity_x,
                        player.weight(), other.weight(), true);
                    player.velocity_x = t.this_v;
                    other.velocity_x  = t.other_v;
                }
                {
                    auto t = Collision::apply_mass_ratio_velocity(
                        player.velocity_y, other.velocity_y,
                        player.weight(), other.weight(), true);
                    player.velocity_y = t.this_v;
                    other.velocity_y  = t.other_v;
                }
                if (player.velocity_y >= 0) object_supported = true;
            }
        } else {
            // Lighter-overlap push: kick lighter primaries the player
            // walks/falls into. Same as &2bb6's heavier-pushes-lighter
            // half — no position revert here, just a velocity transfer.
            int pushee = find_lighter_overlap(player, object_mgr_,
                                               held_object_slot_,
                                               sprite_w_frac, sprite_h_frac);
            if (pushee >= 0) {
                Object& other = object_mgr_.object(pushee);
                {
                    auto t = Collision::apply_mass_ratio_velocity(
                        player.velocity_x, other.velocity_x,
                        player.weight(), other.weight(), false);
                    player.velocity_x = t.this_v;
                    other.velocity_x  = t.other_v;
                }
                {
                    auto t = Collision::apply_mass_ratio_velocity(
                        player.velocity_y, other.velocity_y,
                        player.weight(), other.weight(), false);
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
        // not rising. Probe ~1px below the feet with door substitution;
        // empty → ledge walk-off, release SUPPORTED.
        int feet_abs_y = static_cast<int>(player.y.whole) * 256 +
                         static_cast<int>(player.y.fraction) + sprite_h_frac;
        // Probe 8 frac below the feet — well within the bounce gap
        // (-8 frac default at &308a) but small enough that a real
        // ledge fall reads as empty space.
        int probe_abs_y = feet_abs_y + 8;
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
            player.flags |= ObjectFlags::SUPPORTED;
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

    // Frames-since-walkable counter — port of update_walking_state at
    // &3a4c-&3a8a. Stored in the low nibble of player.state. Reset to 0
    // while SUPPORTED (so walking remains active across latched frames),
    // otherwise incremented (cap at 0x0f). The walking branch in
    // apply_player_input gates strictly on counter == 0 (port of &3b10
    // BNE leave); check_if_player_or_npc_jumping (&3b8c) considers the
    // player jumping when counter ≥ 0x0a.
    bool effectively_supported = (player.flags & ObjectFlags::SUPPORTED) != 0;
    uint8_t counter = player.state & 0x0f;
    if (effectively_supported) {
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

    // ==== Legacy collision blocks below are dead code, retained briefly
    // ==== for diff readability. To be removed in a follow-up commit.
#if 0
    {
        Fixed8_8 old_x = player.x;
        player.x.add_velocity(player.velocity_x);

        // Port of &2fa4 / &3033 left/right_obstruction: block only on the
        // LEADING vertical edge. Static probes (velocity_x == 0) don't
        // block. Keeps top/bottom and left/right obstructions on
        // independent axes per the 6502.
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
        // Object AABB backstop — &2a64 + &2bb6. Tile obstruction misses
        // the right 3/4 of door tiles (STONE_SLOPE_78 pattern only covers
        // the left quarter); pixel-AABB catches the rest. Heavier hit →
        // mass-ratio velocity transfer (&2bee), not naive vx=0.
        int obj_blocker = -1;
        if (!blocked) {
            auto& all = reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                object_mgr_.object(0));
            if (Collision::overlaps_solid_object(player, 0, all,
                                                 static_cast<int>(held_object_slot_))) {
                // Find the specific blocker for the transfer math.
                for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
                    if (i == static_cast<int>(held_object_slot_)) continue;
                    const Object& other = all[i];
                    if (!other.is_active()) continue;
                    uint8_t ow = other.weight();
                    if (ow <= player.weight()) continue;
                    // Cheap AABB check mirroring overlaps_solid_object.
                    int px = player.x.whole * 256 + player.x.fraction;
                    int py = player.y.whole * 256 + player.y.fraction;
                    int ox = other.x.whole * 256 + other.x.fraction;
                    int oy = other.y.whole * 256 + other.y.fraction;
                    int pw = (player.sprite <= 0x80)
                        ? (sprite_atlas[player.sprite].w > 0
                            ? (sprite_atlas[player.sprite].w - 1) * 16 : 0) : 0;
                    int ph = sprite_h_frac;
                    int ow_ = (other.sprite <= 0x80)
                        ? (sprite_atlas[other.sprite].w > 0
                            ? (sprite_atlas[other.sprite].w - 1) * 16 : 0) : 0;
                    int oh_ = (other.sprite <= 0x80)
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
                bool smallest_in_x = player.velocity_x != 0;
                auto t = Collision::apply_mass_ratio_velocity(
                    player.velocity_x, other.velocity_x,
                    player.weight(), other.weight(),
                    smallest_in_x);
                player.velocity_x = t.this_v;
                other.velocity_x  = t.other_v;
            } else {
                player.velocity_x = 0;    // tile block — hard stop
            }
        } else {
            // Heavier-hits-lighter half of &2bb6. Block path above only
            // fires on HEAVIER overlap; without this the player walks
            // through flasks/RCD. Skip held<->player (&2afd-&2b0e).
            int pushee = find_lighter_overlap(player, object_mgr_,
                                               held_object_slot_,
                                               sprite_w_frac, sprite_h_frac);
            if (pushee >= 0) {
                player.x = old_x;
                Object& other = object_mgr_.object(pushee);
                bool smallest_in_x = player.velocity_x != 0;
                auto t = Collision::apply_mass_ratio_velocity(
                    player.velocity_x, other.velocity_x,
                    player.weight(), other.weight(),
                    smallest_in_x);
                player.velocity_x = t.this_v;
                other.velocity_x  = t.other_v;
            }
        }
    }

    // Y movement — obstruction-aware. Ground surface within a tile sits
    // at tiles_obstruction_y_offsets[type] upper-nibble * 16, ORA #&0f
    // (&245f-&246f). Sprite-top above the obstruction line = passable.
    {
        Fixed8_8 old_y = player.y;
        player.y.add_velocity(player.velocity_y);

        // &2fb8-&300d top/bottom_obstruction: block on the LEADING
        // horizontal edge only; static probes (vy==0) don't block. Door
        // substitution applies inside player_aabb_obstructed.
        bool y_blocked = false;
        bool y_blocked_by_tile = false;
        uint8_t lead_ty_for_snap = 0;
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
            if (y_blocked) {
                y_blocked_by_tile = true;
                lead_ty_for_snap = lead_ty;
            }
        }
        // Object-AABB backstop — same mechanism as the X-axis revert
        // above, with the same mass-ratio velocity transfer on block.
        int y_obj_blocker = -1;
        if (!y_blocked) {
            auto& all = reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                object_mgr_.object(0));
            if (Collision::overlaps_solid_object(player, 0, all,
                                                 static_cast<int>(held_object_slot_))) {
                for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
                    if (i == static_cast<int>(held_object_slot_)) continue;
                    const Object& other = all[i];
                    if (!other.is_active()) continue;
                    uint8_t ow = other.weight();
                    if (ow <= player.weight()) continue;
                    int px = player.x.whole * 256 + player.x.fraction;
                    int py = player.y.whole * 256 + player.y.fraction;
                    int ox = other.x.whole * 256 + other.x.fraction;
                    int oy = other.y.whole * 256 + other.y.fraction;
                    int pw = (player.sprite <= 0x80)
                        ? (sprite_atlas[player.sprite].w > 0
                            ? (sprite_atlas[player.sprite].w - 1) * 16 : 0) : 0;
                    int ph = sprite_h_frac;
                    int ow_ = (other.sprite <= 0x80)
                        ? (sprite_atlas[other.sprite].w > 0
                            ? (sprite_atlas[other.sprite].w - 1) * 16 : 0) : 0;
                    int oh_ = (other.sprite <= 0x80)
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
            // Downward tile block: SNAP feet to the surface (don't revert
            // to old_y, which leaves the player ~8-16 frac above the
            // floor and makes SUPPORTED toggle every ~5 frames). 6502 at
            // &308a pushes out by -2 frac giving the same flush landing.
            if (y_blocked_by_tile && player.velocity_y > 0 && y_obj_blocker < 0) {
                ResolvedTile lres = resolve_tile_with_tertiary(
                    landscape_, player.x.whole, lead_ty_for_snap);
                uint8_t ltile = Collision::substitute_door_for_obstruction(
                    lres.tile_and_flip, lres.data_offset,
                    reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                        object_mgr_.object(0)),
                    object_mgr_.tertiary_data_byte(lres.data_offset));
                uint8_t ltype = ltile & TileFlip::TYPE_MASK;
                if (Collision::is_tile_type_solid(ltype)) {
                    bool lfh = (ltile & TileFlip::HORIZONTAL) != 0;
                    bool lfv = (ltile & TileFlip::VERTICAL)   != 0;
                    bool lcoll_fv = lfv ^ tile_obstruction_v_flip_bit(ltype);
                    uint8_t lthresh = slope_tracking_threshold(
                        ltype, lfh, lfv, player.x.fraction,
                        sprite_w_frac, lcoll_fv);
                    uint8_t snap_y = lcoll_fv ? 0 : lthresh;
                    int target_feet_abs = static_cast<int>(lead_ty_for_snap) * 256 +
                                          static_cast<int>(snap_y);
                    int target_top_abs  = target_feet_abs - sprite_h_frac;
                    int current_top_abs = static_cast<int>(old_y.whole) * 256 +
                                          static_cast<int>(old_y.fraction);
                    // Reject upward snaps: STONE_SLOPE_78 (closed vertical
                    // door) has threshold 0 in the left quarter and would
                    // teleport a side-approaching player onto the door's
                    // "ceiling". Fall through to old_y revert.
                    if (target_top_abs < current_top_abs) {
                        player.y = old_y;
                        player.flags |= ObjectFlags::SUPPORTED;
                        object_supported = true;
                    } else {
                        player.y.whole    = static_cast<uint8_t>((target_top_abs >> 8) & 0xff);
                        player.y.fraction = static_cast<uint8_t>(target_top_abs & 0xff);
                        player.flags |= ObjectFlags::SUPPORTED;
                        object_supported = true;
                    }
                } else {
                    // Section that obstructed wasn't a fully-solid tile
                    // type at the simple lookup; fall back to plain
                    // revert (old behaviour).
                    player.y = old_y;
                    player.flags |= ObjectFlags::SUPPORTED;
                    object_supported = true;
                }
                player.velocity_y = 0;
            } else {
                player.y = old_y;
                if (player.velocity_y > 0) {
                    player.flags |= ObjectFlags::SUPPORTED;
                    // AABB-overlap Y revert: latch SUPPORTED so the
                    // tile-based grounded check below (which reads SPACE
                    // under the player) doesn't clear it.
                    object_supported = true;
                }
                if (y_obj_blocker >= 0) {
                    Object& other = object_mgr_.object(y_obj_blocker);
                    bool smallest_in_y = player.velocity_y != 0;
                    auto t = Collision::apply_mass_ratio_velocity(
                        player.velocity_y, other.velocity_y,
                        player.weight(), other.weight(),
                        smallest_in_y);
                    player.velocity_y = t.this_v;
                    other.velocity_y  = t.other_v;
                } else {
                    player.velocity_y = 0;
                }
            }
        } else {
            // Heavier-hits-lighter half of &2bb6 — Y axis. Mirrors the
            // X-axis branch above so the player lands on a flask instead
            // of falling through it, and a flask under the player gets
            // a downward kick.
            int pushee = find_lighter_overlap(player, object_mgr_,
                                               held_object_slot_,
                                               sprite_w_frac, sprite_h_frac);
            if (pushee >= 0) {
                player.y = old_y;
                Object& other = object_mgr_.object(pushee);
                bool smallest_in_y = player.velocity_y != 0;
                auto t = Collision::apply_mass_ratio_velocity(
                    player.velocity_y, other.velocity_y,
                    player.weight(), other.weight(),
                    smallest_in_y);
                player.velocity_y = t.this_v;
                other.velocity_y  = t.other_v;
                if (player.velocity_y >= 0) {
                    player.flags |= ObjectFlags::SUPPORTED;
                    object_supported = true;
                }
            }
        }

        // Ground clamp + support. Stand only on collision-flip-0 tiles;
        // flip-1 tiles (ceilings/overhangs) block from above but don't
        // support. Door substitution: closed → STONE_SLOPE_78 stands on,
        // open → SPACE falls through. Drives off tile obstruction only.
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
        uint8_t snap_ty = feet_tile_y;
        uint8_t snap_y  = 0;
        if (Collision::is_tile_type_solid(ftype)) {
            bool fcoll_fv = ffv ^ tile_obstruction_v_flip_bit(ftype);
            // Loose check uses MIN over sprite width — catches grounded
            // even when the player's centre x is over a non-obstructing
            // patch and only an edge section is on the floor (door
            // substitutes, slope edges).
            uint8_t fthresh_min = single_tile_effective_threshold(
                ftype, ffh, ffv, player.x.fraction,
                sprite_w_frac, fcoll_fv);
            // Snap target uses thresh AT player's centre — needed for
            // slopes so the snap follows the slope surface as the player
            // walks across it. MIN would pin the player to the highest
            // point under their sprite and break uphill walking.
            uint8_t fthresh_at = slope_tracking_threshold(
                ftype, ffh, ffv, player.x.fraction,
                sprite_w_frac, fcoll_fv);
            bool feet_in_obstr = fcoll_fv
                ? (feet_frac <= fthresh_min)
                : (feet_frac >= fthresh_min);
            // Reject ceiling-like floor patterns (thresh<0x40): closed-
            // vertical-door substitute STONE_SLOPE_78 has thresh 0 in its
            // left quarter; snapping there puts the player in the tile
            // above. Let per-frame Y-collision handle these as walls.
            bool ceiling_pattern_floor = !fcoll_fv && fthresh_at < 0x40;
            if (feet_in_obstr && !ceiling_pattern_floor) {
                grounded = true;
                snap_ty = feet_tile_y;
                snap_y  = fcoll_fv ? 0 : fthresh_at;
            }
        }

        // Sprite-height fallback (port deviation): standing→walking sprite
        // swaps shrink height by ~40 frac and lift feet above feet_tile_y.
        // Probe feet_tile_y+1 within a 1/4-tile tolerance so the grounded
        // probe survives the swap. 6502 just gets lucky with flat tiles.
        if (!grounded) {
            uint8_t below_ty = static_cast<uint8_t>(feet_tile_y + 1);
            ResolvedTile bres = resolve_tile_with_tertiary(
                landscape_, player.x.whole, below_ty);
            uint8_t btile = Collision::substitute_door_for_obstruction(
                bres.tile_and_flip, bres.data_offset,
                reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                    object_mgr_.object(0)),
                object_mgr_.tertiary_data_byte(bres.data_offset));
            uint8_t btype = btile & TileFlip::TYPE_MASK;
            if (Collision::is_tile_type_solid(btype)) {
                bool bfh = (btile & TileFlip::HORIZONTAL) != 0;
                bool bfv = (btile & TileFlip::VERTICAL)   != 0;
                bool bcoll_fv = bfv ^ tile_obstruction_v_flip_bit(btype);
                // MIN gives the highest surface in below_ty across the
                // sprite — used for the gap test so any solid section
                // under the player counts as "near the floor".
                uint8_t bthresh_min = single_tile_effective_threshold(
                    btype, bfh, bfv, player.x.fraction,
                    sprite_w_frac, bcoll_fv);
                // Snap target uses thresh-at-x for slope tracking.
                uint8_t bthresh_at = slope_tracking_threshold(
                    btype, bfh, bfv, player.x.fraction,
                    sprite_w_frac, bcoll_fv);
                uint8_t min_surface_y = bcoll_fv ? 0 : bthresh_min;
                uint8_t at_surface_y  = bcoll_fv ? 0 : bthresh_at;
                int feet_abs_full     = static_cast<int>(feet_tile_y) * 256 +
                                        static_cast<int>(feet_frac);
                int min_surface_abs   = static_cast<int>(below_ty) * 256 +
                                        static_cast<int>(min_surface_y);
                int gap = min_surface_abs - feet_abs_full;
                // Same ceiling-pattern guard as the primary probe: a
                // sub-tile-fraction floor surface near the top of the
                // tile (door substitute STONE_SLOPE_78 etc.) is a wall,
                // not a stand-on surface.
                bool ceiling_pattern = !bcoll_fv && bthresh_at < 0x40;
                if (gap >= 0 && gap <= 0x40 && !ceiling_pattern) {
                    grounded = true;
                    snap_ty = below_ty;
                    snap_y  = at_surface_y;
                }
            }
        }

        if (grounded && player.velocity_y >= 0) {
            int target_top = static_cast<int>(snap_ty) * 256 +
                             static_cast<int>(snap_y) - sprite_h_frac;
            int current_top = static_cast<int>(player.y.whole) * 256 +
                              static_cast<int>(player.y.fraction);
            int upward = current_top - target_top;
            // Cap upward snap to ~0x40 frac. Larger jumps mean the
            // "surface" is the top of a partial-solid tile (STONE_SLOPE_78
            // as door substitute, thresh 0 in the left quarter) and would
            // teleport onto the door's ceiling. 6502 avoids via &306c.
            if (upward <= 0x40) {
                player.y.whole    = static_cast<uint8_t>((target_top >> 8) & 0xff);
                player.y.fraction = static_cast<uint8_t>(target_top & 0xff);
                player.velocity_y = 0;
            }
        }

        // Refresh tile_collision_angle (&1c) from supporting tile slope:
        // sample threshold left/right of player.x.fraction, delta →
        // angle_from_deltas. Same conversion as &306c. Skip while airborne
        // so the last grounded value persists into jumps (6502 behaviour).
        if (grounded) {
            ResolvedTile sres = resolve_tile_with_tertiary(
                landscape_, player.x.whole, snap_ty);
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
                // cfv=0: surface is at y_frac=thresh. Slope = d(thresh)/d(x).
                constexpr int SAMPLE_HALF_DX = 0x10;  // 16 frac, 1/16 of a tile.
                uint8_t left_x  = static_cast<uint8_t>(
                    player.x.fraction - SAMPLE_HALF_DX);
                uint8_t right_x = static_cast<uint8_t>(
                    player.x.fraction + SAMPLE_HALF_DX);
                uint8_t left_t  = tile_threshold_at_x(stype, sfh, sfv, left_x);
                uint8_t right_t = tile_threshold_at_x(stype, sfh, sfv, right_x);
                int dthresh = static_cast<int>(right_t) - static_cast<int>(left_t);
                if (dthresh >  127) dthresh =  127;
                if (dthresh < -128) dthresh = -128;
                player_tile_collision_angle_ = NPC::angle_from_deltas(
                    static_cast<int8_t>(SAMPLE_HALF_DX * 2),
                    static_cast<int8_t>(dthresh));
            } else {
                // cfv=1 (full-solid floor / ceiling): surface at top of
                // tile, no slope.
                player_tile_collision_angle_ = 0;
            }
        }
        if (grounded || object_supported) player.flags |=  ObjectFlags::SUPPORTED;
        else                              player.flags &= ~ObjectFlags::SUPPORTED;
    }
#endif

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
            // &4000-&4002 emit STAR_OR_MUSHROOM. 6502 emits from the
            // player's fixed-point position (&3f7f), not the mushroom
            // tile — uses (whole+frac) + jitter. We skip the -0x40
            // sub-tile offset at &3f83.
            particles_.emit(ParticleType::STAR_OR_MUSHROOM, 1,
                            player, rng_);
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
            particles_.emit_directed(ParticleType::WATER, 0xc0, player, rng_);
        }
    }

    // Apply water effects
    Water::apply_water_effects(landscape_, player, player.weight(),
                                every_four_frames_);

    // Tile-based wind / water-current — same dispatch as the per-object
    // loop. Without this the player feels surface wind but not the local
    // gusts inside windy caverns or the river current in Triax's lab.
    Wind::apply_tile_environment(player, landscape_, object_mgr_,
                                 frame_counter_, rng_, particles_);

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
