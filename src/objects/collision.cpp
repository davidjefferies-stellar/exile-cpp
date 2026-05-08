#include "objects/collision.h"
#include "objects/object_data.h"
#include "world/tile_data.h"
#include "world/obstruction.h"
#include "rendering/sprite_atlas.h"
#include "core/types.h"
#include <cstdlib>

namespace Collision {

bool is_tile_type_solid(uint8_t type) {
    // Only genuinely non-obstructing tile types short-circuit to false.
    // Everything else defers to the pattern check (tile_threshold_at_x +
    // is_obstructed), which correctly produces "no obstruction" for
    // patterns that are naturally empty. Marking something like SWITCH
    // or NEST as non-solid here would bypass the pattern and let the
    // player walk straight through a tile that has a real obstruction
    // band (switch has `tiles_obstruction_y_offsets[0x08] = 0xb0` — a
    // thin surface at the top of the tile the player can stand on).
    //
    // METAL_DOOR / STONE_DOOR are listed so that callers that haven't
    // gone through substitute_door_for_obstruction still treat the raw
    // door tile as passable. Player motion always substitutes first so
    // it never sees the raw door types here.
    switch (static_cast<TileType>(type & TileFlip::TYPE_MASK)) {
        case TileType::SPACE:
        case TileType::VARIABLE_WIND:
        case TileType::CONSTANT_WIND:
        case TileType::WATER:
        case TileType::INVISIBLE_SWITCH:
        case TileType::SPACE_WITH_OBJECT_FROM_DATA:
        case TileType::SPACE_WITH_OBJECT_FROM_TYPE:
        case TileType::METAL_DOOR:
        case TileType::STONE_DOOR:
            return false;
        default:
            return true;
    }
}

bool is_tile_solid(const Landscape& landscape, uint8_t tile_x, uint8_t tile_y) {
    uint8_t tile = landscape.get_tile(tile_x, tile_y);
    return is_tile_type_solid(tile & TileFlip::TYPE_MASK);
}

// Check a test point (x_frac, y_frac) within a tile for obstruction.
// Pattern index + y-offset use the landscape's raw flip_v (&247c/&2462);
// the above/below test uses the effective collision flip — landscape
// flip_v XOR the &04ab bit-7 per-type override (&2477).
static bool is_point_obstructed(uint8_t tile_type, bool flip_h, bool flip_v,
                                 uint8_t x_frac, uint8_t y_frac) {
    int pattern_idx = get_obstruction_pattern_index(tile_type, flip_h, flip_v);
    uint8_t y_offset = get_tile_y_offset(tile_type, flip_v);
    bool coll_fv = flip_v ^ tile_obstruction_v_flip_bit(tile_type);
    return Obstruction::is_obstructed(pattern_idx, x_frac, y_frac,
                                      y_offset, coll_fv);
}

// Probe a point in the tile containing (tile_x, tile_y). The tile_y adjustment
// (and the tile-passable early-out) is handled by the caller; this just looks
// up the tile and its pattern and runs the obstruction test.
bool point_in_tile_solid(const Landscape& landscape,
                          uint8_t tile_x, uint8_t tile_y,
                          uint8_t x_frac, uint8_t y_frac) {
    if (!is_tile_solid(landscape, tile_x, tile_y)) return false;
    uint8_t tile = landscape.get_tile(tile_x, tile_y);
    uint8_t type = tile & TileFlip::TYPE_MASK;
    bool fh = (tile & TileFlip::HORIZONTAL) != 0;
    bool fv = (tile & TileFlip::VERTICAL) != 0;
    return is_point_obstructed(type, fh, fv, x_frac, y_frac);
}

bool tile_and_flip_obstructs_point(uint8_t tile_and_flip,
                                    uint8_t x_frac, uint8_t y_frac) {
    uint8_t type = tile_and_flip & TileFlip::TYPE_MASK;
    if (!is_tile_type_solid(type)) return false;
    bool fh = (tile_and_flip & TileFlip::HORIZONTAL) != 0;
    bool fv = (tile_and_flip & TileFlip::VERTICAL) != 0;
    return is_point_obstructed(type, fh, fv, x_frac, y_frac);
}

// File-local alias kept to avoid touching the handful of call sites below
// that spell it `tile_obstructs_point`.
static bool tile_obstructs_point(const Landscape& landscape,
                                 uint8_t tile_x, uint8_t tile_y,
                                 uint8_t x_frac, uint8_t y_frac) {
    return point_in_tile_solid(landscape, tile_x, tile_y, x_frac, y_frac);
}

// Returns true if the object's point position (x, y) is inside solid geometry.
// Used by the simple undo-on-overlap resolution in update_player.
TileCollisionResult check_tile_collision(const Landscape& landscape, const Object& obj) {
    TileCollisionResult result;

    uint8_t ox = obj.x.whole;
    uint8_t oy = obj.y.whole;
    uint8_t ox_frac = obj.x.fraction;
    uint8_t oy_frac = obj.y.fraction;

    // --- Inside current tile: is the point itself obstructed? ---
    if (tile_obstructs_point(landscape, ox, oy, ox_frac, oy_frac)) {
        result.any = true;
    }

    // --- Below: foot at (ox, oy+1), top-of-tile sample. ---
    // Use a slightly-inside y so surfaces (threshold==0 for fully-solid tiles)
    // register as obstructed.
    if (tile_obstructs_point(landscape, ox,
                             static_cast<uint8_t>(oy + 1),
                             ox_frac, 0x04)) {
        result.bottom = true;
        result.any = true;
        result.push_y = -1;
    }

    // --- Above: sample bottom of tile above at ox_frac. ---
    if (tile_obstructs_point(landscape, ox,
                             static_cast<uint8_t>(oy - 1),
                             ox_frac, 0xfc)) {
        result.top = true;
        result.any = true;
        result.push_y = 1;
    }

    // --- Right: sample left edge of tile to right, at object's y_frac. ---
    if (tile_obstructs_point(landscape,
                             static_cast<uint8_t>(ox + 1), oy,
                             0x04, oy_frac)) {
        result.right = true;
        result.any = true;
        result.push_x = -1;
    }

    // --- Left: sample right edge of tile to left. ---
    if (tile_obstructs_point(landscape,
                             static_cast<uint8_t>(ox - 1), oy,
                             0xfc, oy_frac)) {
        result.left = true;
        result.any = true;
        result.push_x = 1;
    }

    if (result.any) {
        // Coarse angle from push direction; used only by some behaviors.
        if      (result.push_x > 0 && result.push_y == 0) result.angle = 0x00;
        else if (result.push_x > 0 && result.push_y > 0)  result.angle = 0x20;
        else if (result.push_y > 0)                       result.angle = 0x40;
        else if (result.push_x < 0 && result.push_y > 0)  result.angle = 0x60;
        else if (result.push_x < 0)                       result.angle = 0x80;
        else if (result.push_x < 0 && result.push_y < 0)  result.angle = 0xA0;
        else if (result.push_y < 0)                       result.angle = 0xC0;
        else                                              result.angle = 0xE0;
    }
    return result;
}

// Returns the object's extent in 16-bit position units (whole*256 + fraction).
// Matches the 6502's this_object_width / this_object_height — the raw table
// bytes from sprites_width_and_horizontal_flip_table (atlas: (w-1)*16) and
// sprites_height_and_vertical_flip_table (atlas: (h-1)*8), which live in the
// x.fraction / y.fraction space directly.
static int sprite_width_units(uint8_t sprite) {
    if (sprite > 0x80) return 0;
    return (sprite_atlas[sprite].w > 0 ? sprite_atlas[sprite].w - 1 : 0) * 16;
}
static int sprite_height_units(uint8_t sprite) {
    if (sprite > 0x80) return 0;
    return (sprite_atlas[sprite].h > 0 ? sprite_atlas[sprite].h - 1 : 0) * 8;
}

// Object-object collision: port of &2a64 check_for_collisions.
// Broad phase: within +/- 2 tiles in both x and y.
// Narrow phase: pixel-precise AABB overlap using sprite widths/heights.
// This matches the 6502 — a small bullet next to a large player does NOT
// register as touching unless their rectangles actually overlap.
ObjectCollisionResult check_object_collision(
    const Object& obj, int slot,
    const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>& all_objects) {

    ObjectCollisionResult result;

    int this_x  = obj.x.whole * 256 + obj.x.fraction;
    int this_y  = obj.y.whole * 256 + obj.y.fraction;
    int this_w  = sprite_width_units(obj.sprite);
    int this_h  = sprite_height_units(obj.sprite);

    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        if (i == slot) continue;
        const Object& other = all_objects[i];
        if (!other.is_active()) continue;

        // Intangibility gate (&2b2d)
        uint8_t idx = static_cast<uint8_t>(other.type);
        if (idx < static_cast<uint8_t>(ObjectType::COUNT)) {
            if (object_types_flags[idx] & ObjectTypeFlags::INTANGIBLE) continue;
        }

        // Broad phase in whole-tile units (&2a7e-&2a90): x within +/-2 tiles,
        // y within +/-2 tiles of this object.
        int8_t tdx = static_cast<int8_t>(obj.x.whole - other.x.whole);
        int8_t tdy = static_cast<int8_t>(obj.y.whole - other.y.whole);
        if (std::abs(tdx) > 2 || std::abs(tdy) > 2) continue;

        // Pixel-precise AABB overlap in 16-bit position units.
        int other_x = other.x.whole * 256 + other.x.fraction;
        int other_y = other.y.whole * 256 + other.y.fraction;
        int other_w = sprite_width_units(other.sprite);
        int other_h = sprite_height_units(other.sprite);

        // No overlap if either object is entirely left of / above the other.
        // The 6502 treats "exactly touching" as no overlap (&2ae9 BEQ skip),
        // hence the strict < comparisons.
        if (other_x + other_w <= this_x)  continue;
        if (this_x  + this_w  <= other_x) continue;
        if (other_y + other_h <= this_y)  continue;
        if (this_y  + this_h  <= other_y) continue;

        result.collided = true;
        result.other_slot = i;
        int dx = this_x - other_x;
        int dy = this_y - other_y;
        result.push_x = (dx > 0) ? 1 : (dx < 0) ? -1 : 0;
        result.push_y = (dy > 0) ? 1 : (dy < 0) ? -1 : 0;
        return result; // First collision wins, matching the 6502.
    }

    return result;
}

bool overlaps_solid_object(const Object& obj, int self_slot,
                            const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>& all_objects,
                            int skip_slot) {
    int this_x = obj.x.whole * 256 + obj.x.fraction;
    int this_y = obj.y.whole * 256 + obj.y.fraction;
    int this_w = sprite_width_units(obj.sprite);
    int this_h = sprite_height_units(obj.sprite);
    uint8_t self_weight = obj.weight();

    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
        if (i == self_slot) continue;
        // Held-object exclusion (port of &2afd-&2b0e). The carried primary's
        // AABB overlaps the player's whenever HeldObject::update_position
        // pins it flush; without this skip, a heavier held (coronium
        // boulder, weight 5) blocks the player's own X motion.
        if (i == skip_slot) continue;
        const Object& other = all_objects[i];
        if (!other.is_active()) continue;

        // The 6502's apply_collision_to_objects_velocities at &2bb6 uses
        // a mass-ratio model: velocity is transferred between colliders
        // scaled by 2^(other_weight − self_weight). When the collider is
        // much heavier, the self-side ratio underflows to zero and the
        // lighter object simply fails to move into the heavier one —
        // the hard block the user sees walking into a wall / door /
        // cannon. We emulate that with a position-revert whenever the
        // other side is strictly heavier than us (any nonzero weight
        // difference already gives ≥2× transfer → effectively blocking
        // on the lighter side).
        //
        // INTANGIBLE (flag 0x80) objects never block — explosions,
        // lightning, transporter beams, fireballs. Opening/open doors
        // are also short-circuited below.
        uint8_t tidx = static_cast<uint8_t>(other.type);
        uint8_t tflags = (tidx < static_cast<uint8_t>(ObjectType::COUNT))
                         ? object_types_flags[tidx] : 0;
        uint8_t other_weight = tflags & ObjectTypeFlags::WEIGHT_MASK;
        if (other_weight <= self_weight) continue;   // lighter or equal → no block
        if (tflags & ObjectTypeFlags::INTANGIBLE) continue;

        // Doors: OPENING set means the sprite has slid far enough out of
        // the way that the passage is functionally clear. update_door
        // ticks obj.x.fraction between 0x10 (open) and 0x0f+carry
        // (closed), so the door sprite physically moves; but the AABB
        // check here is an extra safeguard that short-circuits for
        // opening/open doors so the player can walk through.
        if (tidx >= 0x3c && tidx <= 0x3f) {
            if (other.tertiary_data_offset & 0x02) continue; // opening/open
        }

        int8_t tdx = static_cast<int8_t>(obj.x.whole - other.x.whole);
        int8_t tdy = static_cast<int8_t>(obj.y.whole - other.y.whole);
        if (std::abs(tdx) > 2 || std::abs(tdy) > 2) continue;

        int other_x = other.x.whole * 256 + other.x.fraction;
        int other_y = other.y.whole * 256 + other.y.fraction;
        int other_w = sprite_width_units(other.sprite);
        int other_h = sprite_height_units(other.sprite);

        if (other_x + other_w <= this_x)  continue;
        if (this_x  + this_w  <= other_x) continue;
        if (other_y + other_h <= this_y)  continue;
        if (this_y  + this_h  <= other_y) continue;
        return true;
    }
    return false;
}

// Sign-preserving signed halve: floor toward -infinity. Matches the
// 6502's CMP #&80 / ROR A idiom (&2bca, &2c05) that brings the prior
// sign back into bit 7 after the shift.
static int asr1_floor(int x) {
    if (x >= 0) return x / 2;
    return -((-x + 1) / 2);
}

// Apply one side of the transfer (port of &2bc6-&2bed). The transfer is
// halved and, if the smallest overlap is in this axis, the original is
// added back to give a 1.5× kick (the &2bcf ADC &a2,X "doubling" path).
// Result is clamped to signed 8-bit per &327f prevent_overflow.
static int process_one_side(int transfer, bool double_it, int start_v) {
    int processed = asr1_floor(transfer);
    if (double_it) processed += transfer;
    int sum = start_v + processed;
    if (sum >  127) sum =  127;
    if (sum < -128) sum = -128;
    return sum;
}

// Port of &2bee calculate_transfer_velocities + &2bc6 apply_collision_
// to_object_velocity. Computes lesser/greater transfer values from the
// half-velocity-difference and weight ratio, then routes them to the
// two sides per the BMI / DEX-LDX swap at &2bd6-&2bde.
//
// Heavier side (this_heavier): adds `greater` (negative-signed when
//   this_v > other_v) to itself; adds `lesser` (positive) to other.
// Lighter side: adds -lesser to itself; adds -greater to other (the
//   `invert_if_positive` + index swap at &2bd8-&2bde inverts both).
//
// `smallest_overlap_in_this_axis` mirrors the per-axis bit pattern in
// `collision_velocity_direction_flags_table` (&29dc): both transfers on
// the impact axis are doubled, the other axis isn't.
VelocityTransfer apply_mass_ratio_velocity(
        int8_t this_v_in, int8_t other_v_in,
        uint8_t this_weight, uint8_t other_weight,
        bool smallest_overlap_in_this_axis) {

    int half_diff = asr1_floor(int(this_v_in) - int(other_v_in));

    int wdiff = int(this_weight) - int(other_weight);
    int wdiff_abs = wdiff < 0 ? -wdiff : wdiff;
    int shifts = wdiff_abs == 0 ? 1 : wdiff_abs;
    int lesser = half_diff;
    for (int i = 0; i < shifts; i++) lesser = asr1_floor(lesser);
    // &2c09-&2c0b CMP #&80 / ADC #0: round up by 1 if negative.
    if (lesser < 0) lesser += 1;
    int greater = lesser - half_diff;

    int this_xfer  = (wdiff > 0) ?  greater : -lesser;
    int other_xfer = (wdiff > 0) ?  lesser  : -greater;

    int this_out  = process_one_side(this_xfer,  smallest_overlap_in_this_axis, this_v_in);
    int other_out = process_one_side(other_xfer, smallest_overlap_in_this_axis, other_v_in);

    VelocityTransfer out;
    out.this_v  = static_cast<int8_t>(this_out);
    out.other_v = static_cast<int8_t>(other_out);
    return out;
}

uint8_t substitute_door_for_obstruction(
        uint8_t tile_and_flip, int data_offset,
        const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>& all_objects,
        uint8_t tertiary_byte_fallback) {
    uint8_t type = tile_and_flip & TileFlip::TYPE_MASK;
    if (type != static_cast<uint8_t>(TileType::METAL_DOOR) &&
        type != static_cast<uint8_t>(TileType::STONE_DOOR)) {
        return tile_and_flip;
    }

    // Read the live door data byte. If a primary owns this tertiary
    // slot, that's authoritative (update_door mutates it each frame).
    // Otherwise fall back to the tertiary store (with the spawn gate
    // stripped — bit 7 is for spawning, not door state).
    uint8_t data = static_cast<uint8_t>(tertiary_byte_fallback & 0x7f);
    if (data_offset > 0) {
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
            const Object& obj = all_objects[i];
            if (obj.is_active() &&
                obj.tertiary_slot == static_cast<uint16_t>(data_offset)) {
                data = obj.tertiary_data_offset;
                break;
            }
        }
    }

    // &3e91-&3e94 door_tiles_table: per door orientation × open state.
    //   index 0 (h closed): TILE_SPACESHIP_WALL_HORIZONTAL_QUARTER (0x17)
    //   index 1 (h open):   TILE_SPACE (0x19)
    //   index 2 (v closed): TILE_STONE_SLOPE_SEVENTY_EIGHT (0x2a)
    //   index 3 (v open):   TILE_SPACE (0x19)
    //
    // The 6502 picks the orientation from the tile's flip bits at
    // &3ea1-&3ea7: orientation = fh XOR fv (0 = horizontal door,
    // 1 = vertical door). Both-set or neither-set means horizontal —
    // and that's load-bearing for collision: STONE_SLOPE_78 is
    // solid only in its left quarter, so substituting it for a CLOSED
    // HORIZONTAL door (a 1-tile-wide solid slab the player walks ON)
    // makes the player sink into the door past the slope's solid
    // region. Horizontal doors must use the SPACESHIP_WALL_HORIZONTAL_
    // QUARTER tile, whose obstruction is a uniform thin top slab.
    bool opening = (data & 0x02) != 0;
    bool fh = (tile_and_flip & TileFlip::HORIZONTAL) != 0;
    bool fv = (tile_and_flip & TileFlip::VERTICAL)   != 0;
    bool vertical = (fh != fv);
    if (opening) {
        return static_cast<uint8_t>(TileType::SPACE);
    }
    return vertical
        ? static_cast<uint8_t>(TileType::STONE_SLOPE_78)
        : static_cast<uint8_t>(TileType::SPACESHIP_WALL_HORIZONTAL_QUARTER);
}

} // namespace Collision
